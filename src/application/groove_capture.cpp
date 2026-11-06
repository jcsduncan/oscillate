#include "oscillate/application/groove_capture.hpp"

#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/timeline/timing.hpp"
#if OSCILLATE_HAVE_CODEC
#include "oscillate/codec/encoded_reader.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <unistd.h>
#include <utility>

namespace oscillate::application::groove_capture {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using timeline::ticks_per_quarter;

// --- Band filters -----------------------------------------------------------

// A Butterworth section's Q; two in cascade fall at 24 dB an octave.
constexpr double butterworth_q = std::numbers::sqrt2 / 2.0;
constexpr std::size_t sections_per_edge = 2;
// No cutoff above this share of the rate (hats at a low rate stay valid).
constexpr double max_cutoff_share = 0.45;

struct Biquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  double z1 = 0.0, z2 = 0.0;
  double run(double x) noexcept {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

enum class Edge : std::uint8_t { lowpass, highpass };

// RBJ cookbook low- and high-pass.
Biquad section(Edge edge, double hz, double rate) {
  const double cutoff = std::min(hz, rate * max_cutoff_share);
  const double w = 2.0 * std::numbers::pi * cutoff / rate;
  const double alpha = std::sin(w) / (2.0 * butterworth_q);
  const double cosw = std::cos(w);
  const double a0 = 1.0 + alpha;
  Biquad q;
  if (edge == Edge::lowpass) {
    q.b0 = (1.0 - cosw) / 2.0 / a0;
    q.b1 = (1.0 - cosw) / a0;
    q.b2 = q.b0;
  } else {
    q.b0 = (1.0 + cosw) / 2.0 / a0;
    q.b1 = -(1.0 + cosw) / a0;
    q.b2 = q.b0;
  }
  q.a1 = -2.0 * cosw / a0;
  q.a2 = (1.0 - alpha) / a0;
  return q;
}

std::vector<Biquad> band_filter(Band band, double rate) {
  std::vector<Biquad> chain;
  const auto add = [&](Edge edge, double hz) {
    for (std::size_t i = 0; i < sections_per_edge; ++i)
      chain.push_back(section(edge, hz, rate));
  };
  switch (band) {
  case Band::all:
    add(Edge::highpass, rumble_cutoff_hz);
    break;
  case Band::kick:
    add(Edge::lowpass, kick_cutoff_hz);
    break;
  case Band::perc:
    add(Edge::highpass, perc_low_hz);
    add(Edge::lowpass, perc_high_hz);
    break;
  case Band::hats:
    add(Edge::highpass, hats_cutoff_hz);
    break;
  }
  return chain;
}

// --- Onsets -----------------------------------------------------------------

// The envelope follower: near-instant attack, a slow release so a hit's
// tail does not read as new hits.
constexpr double attack_seconds = 0.0005;
constexpr double release_seconds = 0.03;
// The detector looks at the envelope once a millisecond.
constexpr double hop_seconds = 0.001;
// A hit is a rise of rise_db over rise_lag_seconds...
constexpr double rise_lag_seconds = 0.005;
constexpr double rise_db = 6.0;
// ...no closer than min_gap_seconds to the last (a 32nd at 250 BPM)...
constexpr double min_gap_seconds = 0.03;
// ...whose peak over peak_window_seconds is within strength_floor_db of the
// band's loudest.
constexpr double peak_window_seconds = 0.02;
constexpr double strength_floor_db = 30.0;
// The envelope's floor for the rise, under the loudest.
constexpr double silence_db = 80.0;
// The hit starts where the band first climbs this share of the way from
// the valley before it to its peak.
constexpr double start_fraction = 0.5;

constexpr double milliseconds_per_second = 1000.0;

double db_to_gain(double db) noexcept { return std::pow(10.0, db / 20.0); }

std::size_t hops_for(double seconds, std::size_t hop, std::uint32_t rate) {
  return std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(seconds * rate / static_cast<double>(hop))));
}

// --- Grid -------------------------------------------------------------------

constexpr std::size_t min_grid_hits = 8;
constexpr double coarse_bpm_step = 0.05;
constexpr double fine_bpm_step = 0.001;
// The slower of two tempos an octave apart wins when it fits this well.
constexpr double octave_tie_share = 0.98;

struct Resultant {
  double length = 0.0; // 0..1
  double angle = 0.0;  // radians
};

Resultant resultant(std::span<const Onset> beats, double period) {
  double c = 0.0;
  double s = 0.0;
  double total = 0.0;
  for (const auto& beat : beats) {
    const double phase = 2.0 * std::numbers::pi * beat.frame / period;
    c += beat.strength * std::cos(phase);
    s += beat.strength * std::sin(phase);
    total += beat.strength;
  }
  if (total <= 0.0)
    return {};
  return {std::hypot(c, s) / total, std::atan2(s, c)};
}

double period_frames(double bpm, std::uint32_t rate) { return 60.0 * rate / bpm; }

// --- Measurement ------------------------------------------------------------

constexpr std::size_t min_hits = 4;
// Confidence: at least one hit a beat over the bars...
constexpr double hits_per_beat_for_confidence = 1.0;
// ...and each step's hits within this spread (a share of a step) of their mean.
constexpr double spread_tolerance = 0.15;
// Swing: 50 % is straight, each step of late on the pair's second adds half.
constexpr double straight_percent = 50.0;
constexpr double percent_per_step = 50.0;
// describe(): what counts as straight, on the grid, and loose.
constexpr double straight_swing_margin = 1.5;
constexpr double on_grid_ms = 1.0;
constexpr double loose_confidence = 0.35;

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  const auto q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
std::int64_t positive_mod(std::int64_t a, std::int64_t b) noexcept {
  const auto r = a % b;
  return r < 0 ? r + b : r;
}

foundation::Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}

Result<void> validate(const Request& request) {
  if (request.bars < min_bars || request.bars > max_bars)
    return Result<void>::failure(invalid("Measure between 4 and 32 bars"));
  if (request.cycle_steps != 16 && request.cycle_steps != 32)
    return Result<void>::failure(invalid("A groove from a record has 16 or 32 steps"));
  if (request.steps_per_quarter != 2 && request.steps_per_quarter != 4 &&
      request.steps_per_quarter != 8)
    return Result<void>::failure(invalid("Choose an 8th, 16th or 32nd grid"));
  if (request.bpm && (!std::isfinite(*request.bpm) || *request.bpm <= 0.0))
    return Result<void>::failure(invalid("The tempo must be above zero"));
  return Result<void>::success();
}

std::vector<float> peaks(std::span<const float> mono, double first, double last) {
  std::vector<float> out(bar_peak_points, 0.0F);
  if (last <= first || mono.empty())
    return out;
  const double per_point = (last - first) / static_cast<double>(bar_peak_points);
  float loudest = 0.0F;
  for (std::size_t p = 0; p < bar_peak_points; ++p) {
    const auto a =
        static_cast<std::int64_t>(std::floor(first + per_point * static_cast<double>(p)));
    const auto b =
        static_cast<std::int64_t>(std::ceil(first + per_point * static_cast<double>(p + 1)));
    for (auto i = std::max<std::int64_t>(a, 0);
         i < std::min<std::int64_t>(b, static_cast<std::int64_t>(mono.size())); ++i)
      out[p] = std::max(out[p], std::abs(mono[static_cast<std::size_t>(i)]));
    loudest = std::max(loudest, out[p]);
  }
  if (loudest > 0.0F)
    for (auto& v : out)
      v /= loudest;
  return out;
}

// --- Decoding ---------------------------------------------------------------

constexpr std::size_t read_block_frames = 4096;
// Audio read before the first place looked at, so a hit right at it is seen
// rising.
constexpr double lead_seconds = 0.05;

template <typename Frame, typename Reader>
Result<std::vector<float>> read_mono(Reader& reader, std::uint64_t first, std::uint64_t last,
                                     const std::function<bool()>& cancelled) {
  std::vector<float> mono;
  if (last <= first)
    return Result<std::vector<float>>::success(std::move(mono));
  mono.reserve(static_cast<std::size_t>(last - first));
  std::array<Frame, read_block_frames> block{};
  auto at = first;
  while (at < last) {
    if (cancelled && cancelled())
      return Result<std::vector<float>>::failure({ErrorCode::cancelled, "Cancelled"});
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), last - at));
    auto got = reader.read_at(at, std::span(block).first(count));
    if (!got)
      return Result<std::vector<float>>::failure(got.error());
    if (got.value() == 0)
      break;
    for (std::size_t i = 0; i < got.value(); ++i)
      mono.push_back(0.5F * (block[i].left + block[i].right));
    at += got.value();
  }
  return Result<std::vector<float>>::success(std::move(mono));
}

// --- Audition ---------------------------------------------------------------

constexpr double audition_kick_hz = 55.0;
constexpr double audition_kick_drop_hz = 90.0; // Pitch falls from 55 + 90 Hz.
constexpr double audition_kick_pitch_seconds = 0.03;
constexpr double audition_kick_seconds = 0.18;
constexpr double audition_kick_gain = 0.7;
constexpr double audition_hat_seconds = 0.035;
constexpr double audition_hat_gain = 0.22;
// The hats' noise: a seeded generator, so the A/B is the same every time.
constexpr std::uint64_t audition_seed = 0x9e3779b97f4a7c15ULL;

} // namespace

std::vector<Onset> detect_onsets(std::span<const float> mono, std::uint32_t rate, Band band) {
  std::vector<Onset> onsets;
  if (mono.empty() || rate == 0)
    return onsets;
  auto chain = band_filter(band, static_cast<double>(rate));
  const auto hop =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(hop_seconds * rate)));
  const double attack = std::exp(-1.0 / (attack_seconds * rate));
  const double release = std::exp(-1.0 / (release_seconds * rate));
  std::vector<float> filtered(mono.size());
  std::vector<float> envelope((mono.size() + hop - 1) / hop, 0.0F);
  double follow = 0.0;
  for (std::size_t n = 0; n < mono.size(); ++n) {
    double y = mono[n];
    for (auto& q : chain)
      y = q.run(y);
    filtered[n] = static_cast<float>(y);
    const double level = std::abs(y);
    const double k = level > follow ? attack : release;
    follow = k * follow + (1.0 - k) * level;
    auto& slot = envelope[n / hop];
    slot = std::max(slot, static_cast<float>(follow));
  }
  const float loudest = *std::ranges::max_element(envelope);
  if (loudest <= 0.0F)
    return onsets;
  const double weakest = loudest * db_to_gain(-strength_floor_db);
  const double quiet = loudest * db_to_gain(-silence_db);
  const auto lag = hops_for(rise_lag_seconds, hop, rate);
  const auto gap = hops_for(min_gap_seconds, hop, rate);
  const auto peak_hops = hops_for(peak_window_seconds, hop, rate);
  const double latency =
      band_latency_ms[static_cast<std::size_t>(band)] / milliseconds_per_second * rate;
  const auto hops = envelope.size();
  std::vector<double> rise(hops, 0.0);
  for (std::size_t k = lag; k < hops; ++k)
    rise[k] = 20.0 * std::log10((envelope[k] + quiet) / (envelope[k - lag] + quiet));
  std::optional<std::size_t> last;
  for (std::size_t k = lag; k < hops; ++k) {
    if (rise[k] < rise_db)
      continue;
    // The steepest rise near it, the first of equals.
    bool best = true;
    for (std::size_t j = k > gap ? k - gap : 0; j <= std::min(hops - 1, k + gap) && best; ++j)
      if ((j < k && rise[j] >= rise[k]) || (j > k && rise[j] > rise[k]))
        best = false;
    if (!best || (last && k - *last < gap))
      continue;
    double strength = 0.0;
    for (std::size_t j = k; j < std::min(hops, k + peak_hops + 1); ++j)
      strength = std::max(strength, static_cast<double>(envelope[j]));
    if (strength < weakest)
      continue;
    // The valley before the rise: the quietest hop over the rise's lag (the
    // last of equals). A noisy hit's envelope wobbles as it climbs, so
    // walking back to the first dip can stop inside the hit and place it late.
    // Then the first sample a share of the way from it to the peak.
    std::size_t valley = k - lag;
    for (auto j = k - lag; j <= k; ++j)
      if (envelope[j] <= envelope[valley])
        valley = j;
    const double base = envelope[valley];
    const auto first = valley * hop;
    const auto end = std::min(mono.size(), (k + peak_hops + 1) * hop);
    double peak = 0.0;
    for (auto n = first; n < end; ++n)
      peak = std::max(peak, static_cast<double>(std::abs(filtered[n])));
    const double threshold = base + start_fraction * (peak - base);
    double start = static_cast<double>(k * hop);
    for (auto n = first; n < end; ++n) {
      const double level = std::abs(filtered[n]);
      if (level >= threshold) {
        // Between the sample before and this one.
        const double before = n > 0 ? std::abs(filtered[n - 1]) : 0.0;
        const double share =
            level > before ? std::clamp((threshold - before) / (level - before), 0.0, 1.0) : 1.0;
        start = static_cast<double>(n) - 1.0 + share;
        if (n == 0)
          start = 0.0;
        break;
      }
    }
    onsets.push_back({start - latency, strength});
    last = k;
  }
  return onsets;
}

Result<Grid> fit_grid(std::span<const Onset> beats, std::uint32_t rate, std::optional<double> bpm,
                      double from_frame) {
  if (rate == 0)
    return Result<Grid>::failure(invalid("No sample rate"));
  if (beats.size() < min_grid_hits)
    return Result<Grid>::failure(
        {ErrorCode::not_found, "Too few hits to find the beat: choose a part with drums"});
  double tempo = 0.0;
  if (bpm) {
    tempo = *bpm;
  } else {
    double best = -1.0;
    for (double t = min_fit_bpm; t <= max_fit_bpm + fine_bpm_step; t += coarse_bpm_step) {
      const double score = resultant(beats, period_frames(t, rate)).length;
      if (score > best) {
        best = score;
        tempo = t;
      }
    }
    const double half = tempo / 2.0;
    if (half >= min_fit_bpm &&
        resultant(beats, period_frames(half, rate)).length >= octave_tie_share * best)
      tempo = half;
    // Refine around the coarse choice.
    double refined = tempo;
    double refined_score = -1.0;
    for (double t = tempo - coarse_bpm_step; t <= tempo + coarse_bpm_step; t += fine_bpm_step) {
      const double score = resultant(beats, period_frames(t, rate)).length;
      if (score > refined_score) {
        refined_score = score;
        refined = t;
      }
    }
    tempo = refined;
  }
  const double period = period_frames(tempo, rate);
  const auto fitted = resultant(beats, period);
  double offset = fitted.angle / (2.0 * std::numbers::pi) * period;
  if (offset < 0.0)
    offset += period;
  // The beat of the bar whose hits are strongest is the one.
  constexpr auto bar_beats = static_cast<std::int64_t>(beats_per_bar);
  std::array<double, beats_per_bar> weight{};
  for (const auto& beat : beats) {
    const auto q = static_cast<std::int64_t>(std::llround((beat.frame - offset) / period));
    weight[static_cast<std::size_t>(positive_mod(q, bar_beats))] += beat.strength;
  }
  const auto one =
      static_cast<std::int64_t>(std::distance(weight.begin(), std::ranges::max_element(weight)));
  // The first such beat at or after from_frame (half a beat of slack).
  auto q = static_cast<std::int64_t>(std::ceil((from_frame - offset) / period - 0.5));
  while (positive_mod(q, bar_beats) != one)
    ++q;
  return Result<Grid>::success({tempo, offset + static_cast<double>(q) * period, fitted.length});
}

std::optional<std::int64_t> downbeat_shift(std::span<const GrooveHit> hits,
                                           std::uint32_t steps_per_quarter) {
  if (steps_per_quarter == 0)
    return std::nullopt;
  const auto step = ticks_per_quarter / steps_per_quarter;
  for (const auto every : {static_cast<std::int64_t>(steps_per_quarter * beats_per_bar),
                           static_cast<std::int64_t>(steps_per_quarter)}) {
    long double sum = 0.0L;
    std::size_t count = 0;
    for (const auto& hit : hits) {
      const auto index = floor_div(hit.tick + step / 2, step);
      if (positive_mod(index, every) == 0) {
        sum += static_cast<long double>(hit.tick - index * step);
        ++count;
      }
    }
    if (count > 0)
      return static_cast<std::int64_t>(std::llround(sum / static_cast<long double>(count)));
  }
  return std::nullopt;
}

namespace {

// The reference every band is measured against: the kick's downbeat
// offset, else All's, else the band's own, else none. `all` is asked for
// only when the kick has no downbeat hits.
std::int64_t reference_shift(std::span<const GrooveHit> kick,
                             const std::function<std::vector<GrooveHit>()>& all,
                             std::span<const GrooveHit> band, std::uint32_t steps_per_quarter) {
  if (const auto shift = downbeat_shift(kick, steps_per_quarter))
    return *shift;
  if (const auto shift = downbeat_shift(all(), steps_per_quarter))
    return *shift;
  return downbeat_shift(band, steps_per_quarter).value_or(0);
}

} // namespace

Result<Measurement> measure(std::span<const GrooveHit> hits, const MeasureSettings& settings) {
  using R = Result<Measurement>;
  if (hits.size() < min_hits)
    return R::failure(invalid("A groove needs at least four hits"));
  Measurement out;
  out.groove = {settings.name, settings.steps_per_quarter,
                std::vector<persistence::GrooveStep>(settings.cycle_steps)};
  if (auto valid = persistence::validate_groove(out.groove); !valid)
    return R::failure(valid.error());
  out.step_ms = settings.step_ms;
  const auto step = ticks_per_quarter / settings.steps_per_quarter;
  const auto cycle = static_cast<std::int64_t>(settings.cycle_steps);
  std::vector<GrooveHit> sorted(hits.begin(), hits.end());
  std::ranges::stable_sort(sorted, {}, &GrooveHit::tick);
  const auto nearest = [&](std::int64_t tick) { return floor_div(tick + step / 2, step); };
  const auto shift = settings.shift;
  const auto slots = settings.cycle_steps;
  std::vector<double> sum(slots, 0.0);
  std::vector<double> square(slots, 0.0);
  std::vector<double> strength(slots, 0.0);
  std::vector<std::size_t> count(slots, 0);
  double total_strength = 0.0;
  for (const auto& hit : sorted) {
    const auto tick = hit.tick - shift;
    const auto index = nearest(tick);
    const auto slot = static_cast<std::size_t>(positive_mod(index, cycle));
    const double offset = static_cast<double>(tick - index * step) / static_cast<double>(step);
    sum[slot] += offset;
    square[slot] += offset * offset;
    strength[slot] += hit.strength;
    ++count[slot];
    total_strength += hit.strength;
    if (out.hits.size() < max_folded_hits)
      out.hits.push_back({static_cast<double>(slot) + offset, hit.strength});
  }
  out.hit_count = sorted.size();
  const double mean_strength = total_strength / static_cast<double>(sorted.size());
  out.steps.resize(slots);
  double spread_sum = 0.0;
  double swing_sum = 0.0;
  std::size_t swing_hits = 0;
  double offbeat_sum = 0.0;
  std::size_t offbeat_hits = 0;
  const auto spq = static_cast<std::size_t>(settings.steps_per_quarter);
  for (std::size_t slot = 0; slot < slots; ++slot) {
    if (count[slot] == 0)
      continue;
    auto& feel = out.steps[slot];
    const auto n = static_cast<double>(count[slot]);
    const double mean = sum[slot] / n;
    feel.hits = count[slot];
    feel.timing = std::clamp(mean, -persistence::max_groove_timing, persistence::max_groove_timing);
    feel.spread = std::sqrt(std::max(0.0, square[slot] / n - mean * mean));
    feel.timing_ms = feel.timing * settings.step_ms;
    feel.spread_ms = feel.spread * settings.step_ms;
    feel.velocity = mean_strength > 0.0
                        ? std::clamp(strength[slot] / n / mean_strength, min_step_velocity,
                                     persistence::max_groove_velocity)
                        : 1.0;
    out.groove.steps[slot] = {feel.timing, feel.velocity};
    spread_sum += feel.spread * n;
    if (slot % 2 == 1) {
      swing_sum += feel.timing * n;
      swing_hits += count[slot];
    }
    if (spq % 2 == 0 && slot % spq == spq / 2) {
      offbeat_sum += feel.timing * n;
      offbeat_hits += count[slot];
    }
    if (feel.velocity >= accent_ratio)
      ++out.accents;
  }
  const double expected = settings.bars > 0 ? static_cast<double>(settings.bars * beats_per_bar) *
                                                  hits_per_beat_for_confidence
                                            : 0.0;
  const double coverage =
      expected > 0.0 ? std::min(1.0, static_cast<double>(out.hit_count) / expected) : 1.0;
  const double mean_spread = spread_sum / static_cast<double>(out.hit_count);
  const double tightness = std::clamp(1.0 - mean_spread / spread_tolerance, 0.0, 1.0);
  out.confidence = coverage * tightness;
  if (swing_hits > 0)
    out.swing_percent =
        straight_percent + percent_per_step * swing_sum / static_cast<double>(swing_hits);
  if (offbeat_hits > 0)
    out.offbeat_ms = offbeat_sum / static_cast<double>(offbeat_hits) * settings.step_ms;
  return R::success(std::move(out));
}

std::string describe(const Measurement& m) {
  const auto spq = m.groove.steps_per_quarter;
  const char* grid = spq == 2 ? "8ths" : spq == 8 ? "32nds" : "16ths";
  std::string text;
  if (m.confidence < loose_confidence)
    text = "Loose, take it with a pinch of salt: ";
  char buffer[160];
  if (std::abs(m.swing_percent - straight_percent) < straight_swing_margin)
    std::snprintf(buffer, sizeof buffer, "Straight %s", grid);
  else if (m.swing_percent > straight_percent)
    std::snprintf(buffer, sizeof buffer, "Swung %s at %.0f %%", grid, m.swing_percent);
  else
    std::snprintf(buffer, sizeof buffer, "Rushed %s at %.0f %%", grid, m.swing_percent);
  text += buffer;
  if (std::abs(m.offbeat_ms) < on_grid_ms)
    text += ", offbeats on the grid";
  else {
    std::snprintf(buffer, sizeof buffer, ", offbeats %.0f ms %s", std::abs(m.offbeat_ms),
                  m.offbeat_ms > 0.0 ? "late" : "early");
    text += buffer;
  }
  if (m.accents == 0)
    text += ", no accents";
  else if (m.accents == 1)
    text += ", 1 accent";
  else
    text += ", " + std::to_string(m.accents) + " accents";
  text += ".";
  return text;
}

Result<Capture> capture_audio(std::span<const float> mono, std::uint32_t rate,
                              const Request& request) {
  using R = Result<Capture>;
  if (auto valid = validate(request); !valid)
    return R::failure(valid.error());
  if (rate == 0 || mono.empty())
    return R::failure(invalid("The record is empty"));
  auto kicks = detect_onsets(mono, rate, Band::kick);
  if (kicks.size() < min_grid_hits)
    kicks = detect_onsets(mono, rate, Band::all);
  auto grid = fit_grid(kicks, rate, request.bpm, request.start_seconds * rate);
  if (!grid)
    return R::failure(grid.error());
  const auto onsets = request.band == Band::kick ? kicks : detect_onsets(mono, rate, request.band);
  const double quarter = period_frames(grid.value().bpm, rate);
  const double bar = quarter * beats_per_bar;
  const double downbeat = grid.value().downbeat_frame;
  const auto fit = static_cast<std::size_t>(
      std::max(0.0, std::floor((static_cast<double>(mono.size()) - downbeat) / bar)));
  const auto bars = std::min(request.bars, fit);
  if (bars == 0)
    return R::failure(invalid("Too little of the record is left for a bar at this tempo"));
  const double step = quarter / request.steps_per_quarter;
  const double per_tick = quarter / static_cast<double>(ticks_per_quarter);
  const double end = downbeat + bar * static_cast<double>(bars);
  // A band's hits in the window, in ticks from the downbeat.
  const auto in_window = [&](std::span<const Onset> found) {
    std::vector<GrooveHit> placed;
    for (const auto& onset : found)
      if (onset.frame >= downbeat - step / 2.0 && onset.frame < end - step / 2.0)
        placed.push_back(
            {static_cast<std::int64_t>(std::llround((onset.frame - downbeat) / per_tick)),
             onset.strength});
    return placed;
  };
  const auto hits = in_window(onsets);
  const auto kick_hits = in_window(kicks);
  const auto shift = reference_shift(
      kick_hits, [&] { return in_window(detect_onsets(mono, rate, Band::all)); }, hits,
      request.steps_per_quarter);
  auto measured = measure(hits, {request.name, request.steps_per_quarter, request.cycle_steps, bars,
                                 step / rate * 1000.0, shift});
  if (!measured)
    return R::failure(measured.error());
  Capture out;
  out.measurement = std::move(measured.value());
  out.bpm = grid.value().bpm;
  out.grid_fit = grid.value().fit;
  out.bar_peaks = peaks(mono, downbeat, downbeat + bar);
  for (const auto& onset : onsets)
    if (onset.frame >= downbeat && onset.frame < downbeat + bar)
      out.bar_onsets.push_back((onset.frame - downbeat) / bar);
  return R::success(std::move(out));
}

Result<Capture> capture_file(const std::filesystem::path& file, const Request& request,
                             const std::function<bool()>& cancelled) {
  using R = Result<Capture>;
  if (auto valid = validate(request); !valid)
    return R::failure(valid.error());
  std::uint32_t rate = 0;
  std::vector<float> mono;
  double lead = 0.0;
  const auto window = [&](std::uint32_t r, std::uint64_t frames) {
    const double start = std::max(0.0, request.start_seconds);
    lead = std::min(start, lead_seconds);
    const auto first = static_cast<std::uint64_t>(std::llround((start - lead) * r));
    const auto last = std::min<std::uint64_t>(
        frames, first + static_cast<std::uint64_t>(std::llround((max_capture_seconds + lead) * r)));
    return std::pair{first, last};
  };
#if OSCILLATE_HAVE_CODEC
  auto reader = codec::EncodedReader::open(file);
  if (!reader)
    return R::failure(reader.error());
  rate = reader.value().info().sample_rate;
  const auto [first, last] = window(rate, reader.value().info().frames);
  auto read = read_mono<codec::StereoFrame>(reader.value(), first, last, cancelled);
#else
  auto reader = media::WavReader::open(file);
  if (!reader)
    return R::failure(reader.error());
  rate = reader.value().sample_rate();
  const auto [first, last] = window(rate, reader.value().frame_count());
  auto read = read_mono<media::StereoFrame>(reader.value(), first, last, cancelled);
#endif
  if (!read)
    return R::failure(read.error());
  mono = std::move(read.value());
  auto from_lead = request;
  from_lead.start_seconds = lead;
  return capture_audio(mono, rate, from_lead);
}

Result<Capture> capture_clip(const persistence::ProjectStore& store,
                             const project::Snapshot& snapshot, project::ClipId id,
                             const Request& request, const std::function<bool()>& cancelled) {
  using R = Result<Capture>;
  if (auto valid = validate(request); !valid)
    return R::failure(valid.error());
  const project::Clip* clip = nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find(track.clips, id, &project::Clip::id);
    if (found != track.clips.end())
      clip = &*found;
  }
  if (clip == nullptr)
    return R::failure({ErrorCode::not_found, "Audio clip not found"});
  const auto source = std::ranges::find(snapshot.sources, clip->source, &project::SourceRecord::id);
  if (source == snapshot.sources.end())
    return R::failure({ErrorCode::not_found, "The clip's audio is missing"});
  auto plan =
      processing::prepare_clip_frame_plan(*clip, *source, snapshot.tempo, snapshot.sample_rate);
  if (!plan)
    return R::failure(plan.error());
  const auto& p = plan.value();
  const auto& tempo = snapshot.tempo;
  const auto project_rate = snapshot.sample_rate;
  const auto to_tick = [&](double frame) -> std::optional<std::int64_t> {
    auto tick =
        tempo.sample_to_tick({static_cast<std::int64_t>(std::llround(frame))}, project_rate);
    return tick ? std::optional(tick.value().value) : std::nullopt;
  };
  const auto to_frame = [&](std::int64_t tick) -> std::optional<std::int64_t> {
    auto frame = tempo.tick_to_sample({tick}, project_rate);
    return frame ? std::optional(frame.value().value) : std::nullopt;
  };
  const auto start = to_tick(static_cast<double>(p.project_start_frame));
  const auto finish = to_tick(static_cast<double>(p.project_end_frame()));
  if (!start || !finish || *finish <= *start)
    return R::failure(invalid("The clip is too short"));
  const auto bar_ticks = ticks_per_quarter * static_cast<std::int64_t>(beats_per_bar);
  // From the first bar line in the clip, else its start.
  auto window_start = -floor_div(-*start, bar_ticks) * bar_ticks;
  if (window_start + bar_ticks > *finish)
    window_start = *start;
  const auto window_end =
      std::min(*finish, window_start + bar_ticks * static_cast<std::int64_t>(request.bars));
  const auto bars =
      static_cast<std::size_t>(std::max<std::int64_t>(1, (window_end - window_start) / bar_ticks));
  // The window's frames in the source.
  const auto source_at = [&](std::int64_t tick) -> std::optional<std::uint64_t> {
    const auto frame = to_frame(tick);
    if (!frame || *frame < static_cast<std::int64_t>(p.project_start_frame))
      return std::nullopt;
    const auto output = static_cast<std::uint64_t>(*frame) - p.project_start_frame;
    const auto rel = p.warp.source_at_output(std::min(output, p.warp.output_frames()));
    return rel ? std::optional(p.source_start_frame + *rel) : std::nullopt;
  };
  const auto first_source = source_at(window_start);
  const auto last_source = source_at(window_end);
  if (!first_source || !last_source || *last_source <= *first_source)
    return R::failure(invalid("The clip's audio does not cover a bar"));
  const auto source_rate = source->sample_rate;
  const auto pad = static_cast<std::uint64_t>(std::llround(lead_seconds * source_rate));
  const auto read_first = *first_source > pad ? *first_source - pad : 0;
  const auto read_last = *last_source + pad;
  auto descriptor = store.open_verified_source(*source);
  if (!descriptor)
    return R::failure(descriptor.error());
  auto reader = media::AudioReader::open_fd(descriptor.value(), *source);
  (void)::close(descriptor.value());
  if (!reader)
    return R::failure(reader.error());
  auto read = read_mono<media::StereoFrame>(
      reader.value(), read_first, std::min<std::uint64_t>(read_last, reader.value().frame_count()),
      cancelled);
  if (!read)
    return R::failure(read.error());
  const auto& mono = read.value();
  // A band's hits through the clip's warp onto the song's timeline.
  const auto step_ticks = ticks_per_quarter / request.steps_per_quarter;
  const auto on_timeline = [&](Band band) {
    std::vector<GrooveHit> placed;
    for (const auto& onset : detect_onsets(mono, source_rate, band)) {
      const auto at = static_cast<double>(read_first) + onset.frame;
      if (at < static_cast<double>(p.source_start_frame))
        continue;
      const auto rel = static_cast<std::uint64_t>(std::llround(at)) - p.source_start_frame;
      const auto output = p.warp.output_at_source(rel);
      if (!output)
        continue;
      const auto tick = to_tick(static_cast<double>(p.project_start_frame + *output));
      if (!tick || *tick < window_start - step_ticks / 2 || *tick >= window_end - step_ticks / 2)
        continue;
      placed.push_back({*tick, onset.strength});
    }
    return placed;
  };
  const auto hits = on_timeline(request.band);
  const auto kick_hits = request.band == Band::kick ? hits : on_timeline(Band::kick);
  const auto shift = reference_shift(
      kick_hits, [&] { return request.band == Band::all ? hits : on_timeline(Band::all); }, hits,
      request.steps_per_quarter);
  // A step's length in ms at the window's tempo.
  const auto beat_start = to_frame(window_start);
  const auto beat_end = to_frame(window_start + ticks_per_quarter);
  if (!beat_start || !beat_end || *beat_end <= *beat_start)
    return R::failure(invalid("The song's tempo cannot be read here"));
  const double quarter_frames = static_cast<double>(*beat_end - *beat_start);
  const double step_ms =
      quarter_frames / request.steps_per_quarter / static_cast<double>(project_rate) * 1000.0;
  auto measured = measure(
      hits, {request.name, request.steps_per_quarter, request.cycle_steps, bars, step_ms, shift});
  if (!measured)
    return R::failure(measured.error());
  Capture out;
  out.measurement = std::move(measured.value());
  out.bpm = 60.0 * static_cast<double>(project_rate) / quarter_frames;
  out.grid_fit = 1.0;
  // The first measured bar, in the read audio's frames.
  const auto bar_first = source_at(window_start);
  const auto bar_last = source_at(std::min(window_end, window_start + bar_ticks));
  if (bar_first && bar_last)
    out.bar_peaks = peaks(mono, static_cast<double>(*bar_first - read_first),
                          static_cast<double>(*bar_last - read_first));
  for (const auto& hit : hits)
    if (hit.tick >= window_start && hit.tick < window_start + bar_ticks)
      out.bar_onsets.push_back(static_cast<double>(hit.tick - window_start) /
                               static_cast<double>(bar_ticks));
  return R::success(std::move(out));
}

std::vector<float> audition(const persistence::GrooveTemplate& groove, double bpm,
                            std::uint32_t rate, std::size_t bars, bool grooved) {
  if (rate == 0 || bpm <= 0.0 || groove.steps.empty() || groove.steps_per_quarter == 0)
    return {};
  const double quarter = period_frames(bpm, rate);
  const double step = quarter / groove.steps_per_quarter;
  const auto total =
      static_cast<std::size_t>(std::llround(quarter * static_cast<double>(beats_per_bar * bars)));
  std::vector<float> out(total * 2, 0.0F);
  const auto add = [&](double at, double seconds, double gain, auto&& voice) {
    const auto first = static_cast<std::int64_t>(std::llround(at));
    const auto length = static_cast<std::int64_t>(std::llround(seconds * rate));
    for (std::int64_t i = 0; i < length; ++i) {
      const auto n = first + i;
      if (n < 0 || n >= static_cast<std::int64_t>(total))
        continue;
      const double t = static_cast<double>(i) / rate;
      const auto sample = static_cast<float>(gain * voice(t, seconds));
      out[static_cast<std::size_t>(n) * 2] += sample;
      out[static_cast<std::size_t>(n) * 2 + 1] += sample;
    }
  };
  // The kick: a falling sine on every beat, straight (the groove's anchor).
  for (std::size_t beat = 0; beat < beats_per_bar * bars; ++beat) {
    double phase = 0.0;
    double last_t = 0.0;
    add(quarter * static_cast<double>(beat), audition_kick_seconds, audition_kick_gain,
        [&](double t, double length) {
          const double hz =
              audition_kick_hz + audition_kick_drop_hz * std::exp(-t / audition_kick_pitch_seconds);
          phase += 2.0 * std::numbers::pi * hz * (t - last_t);
          last_t = t;
          return std::sin(phase) * (1.0 - t / length);
        });
  }
  // The hats: seeded noise, differenced twice for a bright tilt.
  std::uint64_t state = audition_seed;
  const auto noise = [&] {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return static_cast<double>(state >> 11U) / static_cast<double>(1ULL << 53U) * 2.0 - 1.0;
  };
  const auto steps = static_cast<std::size_t>(
      std::llround(static_cast<double>(beats_per_bar * bars * groove.steps_per_quarter)));
  for (std::size_t s = 0; s < steps; ++s) {
    const auto& feel = groove.steps[s % groove.steps.size()];
    const double at = step * (static_cast<double>(s) + (grooved ? feel.timing : 0.0));
    const double level = grooved ? feel.velocity : 1.0;
    double previous = 0.0;
    double earlier = 0.0;
    add(at, audition_hat_seconds, audition_hat_gain * level, [&](double t, double length) {
      const double x = noise();
      const double y = x - 2.0 * previous + earlier;
      earlier = previous;
      previous = x;
      return 0.25 * y * (1.0 - t / length) * (1.0 - t / length);
    });
  }
  for (auto& v : out)
    v = std::clamp(v, -1.0F, 1.0F);
  return out;
}

} // namespace oscillate::application::groove_capture
