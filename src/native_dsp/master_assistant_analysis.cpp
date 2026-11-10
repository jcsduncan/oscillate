#include "dsp/bs1770.hpp"
#include "dsp/crossover.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/master_assistant.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

// The master assistant's measurements (ADR-0124): one pass over a stereo
// programme, then the readings from what that pass kept per 100 ms step.
namespace oscillate::native_dsp::assistant {
namespace {

namespace bs1770 = dsp::bs1770;
using Tree = dsp::CrossoverTree<regions - 1>;

constexpr std::array<double, regions - 1> splits{low_split_hz, high_split_hz};
constexpr std::size_t left_channel = 0;
constexpr std::size_t right_channel = 1;
constexpr std::size_t channels = 2;
// A power under this is silence (about -300 dB).
constexpr double tiny_power = 1e-30;
constexpr double half = 0.5;

[[nodiscard]] double db_of_power(double power) noexcept {
  return dsp::power_to_db(power, floor_db, tiny_power);
}
[[nodiscard]] double db_of_gain(double gain) noexcept {
  return dsp::gain_to_db(gain, floor_db, tiny_power);
}

// One region's running measurements.
struct RegionState {
  double step_power = 0.0;
  float step_peak = 0.0F;
  double mid_power = 0.0;
  double side_power = 0.0;
  // Onsets.
  std::size_t hop_frames = 1;
  std::uint64_t spacing_hops = 1;
  double loudest_hop = 0.0;
  double hop_energy = 0.0;
  std::size_t hop_fill = 0;
  std::array<double, onset_history_hops> history{};
  std::size_t history_count = 0;
  std::uint64_t hops = 0;
  std::optional<std::uint64_t> last_onset;
  std::size_t onsets = 0;
  std::vector<float> step_powers;
  std::vector<float> step_peaks;
};

// The steps that count for a crest or a loud level, and that loud level
// (loud_percentile of their powers, dB).
struct Gated {
  std::vector<std::size_t> steps;
  double loud_db = floor_db;
};

// The steps within crest_gate_db of the loudest few.
Gated gate_steps(std::span<const float> powers) {
  Gated gated;
  if (powers.empty())
    return gated;
  std::vector<double> sorted(powers.begin(), powers.end());
  std::ranges::sort(sorted, std::greater<>());
  const auto top = std::max<std::size_t>(
      1, static_cast<std::size_t>(crest_reference_share * static_cast<double>(sorted.size())));
  const double reference =
      std::accumulate(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(top), 0.0) /
      static_cast<double>(top);
  if (reference <= tiny_power)
    return gated;
  const double gate = reference * std::pow(10.0, -crest_gate_db / 10.0);
  std::vector<double> loud;
  for (std::size_t i = 0; i < powers.size(); ++i) {
    if (powers[i] >= gate && powers[i] > tiny_power) {
      gated.steps.push_back(i);
      loud.push_back(db_of_power(powers[i]));
    }
  }
  if (!loud.empty())
    gated.loud_db = tonal::percentile(loud, loud_percentile);
  return gated;
}

// The mean crest (dB) of the gated steps: each step's peak over its RMS.
double mean_crest(std::span<const float> powers, std::span<const float> peaks, const Gated& gated) {
  if (gated.steps.empty())
    return 0.0;
  double sum = 0.0;
  for (const auto i : gated.steps)
    sum += db_of_gain(peaks[i]) - db_of_power(powers[i]);
  return sum / static_cast<double>(gated.steps.size());
}

double standard_deviation(std::span<const double> values) {
  if (values.size() < 2)
    return 0.0;
  const double mean =
      std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double sum = 0.0;
  for (const double v : values)
    sum += (v - mean) * (v - mean);
  return std::sqrt(sum / static_cast<double>(values.size()));
}

[[nodiscard]] bool judged(std::size_t band) noexcept {
  const double hz = tonal::band_hz(band);
  return hz >= tonal_judged_low_hz * std::pow(2.0, -1.0 / (2.0 * tonal::bands_per_octave)) &&
         hz <= tonal_judged_high_hz * std::pow(2.0, 1.0 / (2.0 * tonal::bands_per_octave));
}

} // namespace

double Analysis::seconds() const noexcept {
  return sample_rate == 0 ? 0.0 : static_cast<double>(frames) / static_cast<double>(sample_rate);
}

struct Analyser::State {
  explicit State(std::uint32_t sample_rate)
      : rate(sample_rate), meter(static_cast<double>(sample_rate)) {
    const double hz = static_cast<double>(sample_rate);
    step_frames =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(hz * step_seconds)));
    static_assert(onset_hop_seconds.size() == regions);
    for (std::size_t r = 0; r < regions; ++r) {
      region[r].hop_frames = std::max<std::size_t>(
          1, static_cast<std::size_t>(std::lround(hz * onset_hop_seconds[r])));
      region[r].spacing_hops =
          static_cast<std::uint64_t>(std::lround(onset_spacing_seconds / onset_hop_seconds[r]));
    }
    std::array<dsp::Lr4Design, regions - 1> designs{};
    for (std::size_t k = 0; k < splits.size(); ++k)
      designs[k] = dsp::Lr4Design::at(splits[k], hz);
    mid_tree.set(designs, designs.size());
    side_tree.set(designs, designs.size());
    for (auto& k : weighting)
      k.prepare(hz);
  }

  void frame(double left, double right) {
    last_left = left;
    last_right = right;
    const double kl = weighting[left_channel].process(left);
    const double kr = weighting[right_channel].process(right);
    step_k += kl * kl + kr * kr;
    step_square += half * (left * left + right * right);
    const auto peak = static_cast<float>(std::max(std::fabs(left), std::fabs(right)));
    step_peak = std::max(step_peak, peak);
    sample_peak = std::max(sample_peak, static_cast<double>(peak));
    true_peak = std::max({true_peak, std::fabs(peaks[left_channel].push(left)),
                          std::fabs(peaks[right_channel].push(right))});

    const double mid = half * (left + right);
    const double side = half * (left - right);
    meter.push(mid, side);
    std::array<double, regions> mid_band{};
    std::array<double, regions> side_band{};
    mid_tree.process(mid, mid_band);
    side_tree.process(side, side_band);
    for (std::size_t r = 0; r < regions; ++r) {
      auto& region = this->region[r];
      const double m2 = mid_band[r] * mid_band[r];
      region.step_power += m2;
      region.step_peak = std::max(region.step_peak, static_cast<float>(std::fabs(mid_band[r])));
      region.mid_power += m2;
      region.side_power += side_band[r] * side_band[r];
      region.hop_energy += m2;
      if (++region.hop_fill == region.hop_frames)
        close_hop(region);
    }
    if (++step_fill == step_frames)
      close_step();
  }

  void close_hop(RegionState& region) {
    const double power = region.hop_energy / static_cast<double>(region.hop_frames);
    region.loudest_hop = std::max(region.loudest_hop, power);
    if (region.history_count == onset_history_hops) {
      const double mean = std::accumulate(region.history.begin(), region.history.end(), 0.0) /
                          static_cast<double>(onset_history_hops);
      const bool spaced =
          !region.last_onset || region.hops - *region.last_onset >= region.spacing_hops;
      const double level = db_of_power(power);
      if (spaced && level > onset_floor_dbfs &&
          level > db_of_power(region.loudest_hop) - onset_range_db &&
          level > db_of_power(mean) + onset_rise_db) {
        ++region.onsets;
        region.last_onset = region.hops;
      }
    }
    region.history[region.hops % onset_history_hops] = power;
    region.history_count = std::min(region.history_count + 1, onset_history_hops);
    ++region.hops;
    region.hop_energy = 0.0;
    region.hop_fill = 0;
  }

  void close_step() {
    const auto n = static_cast<double>(step_frames);
    step_k_power.push_back(static_cast<float>(step_k / n));
    step_powers.push_back(static_cast<float>(step_square / n));
    step_peaks.push_back(step_peak);
    for (auto& r : region) {
      r.step_powers.push_back(static_cast<float>(r.step_power / n));
      r.step_peaks.push_back(r.step_peak);
      r.step_power = 0.0;
      r.step_peak = 0.0F;
    }
    step_k = 0.0;
    step_square = 0.0;
    step_peak = 0.0F;
    step_fill = 0;
  }

  std::uint32_t rate;
  std::size_t step_frames = 1;
  std::uint64_t frames = 0;
  std::array<bs1770::KWeighting, channels> weighting{};
  std::array<bs1770::TruePeak, channels> peaks{};
  Tree mid_tree;
  Tree side_tree;
  tonal::BandMeter meter;
  std::array<RegionState, regions> region{};
  double step_k = 0.0;
  double step_square = 0.0;
  float step_peak = 0.0F;
  std::size_t step_fill = 0;
  double sample_peak = 0.0;
  double true_peak = 0.0;
  double last_left = 0.0;
  double last_right = 0.0;
  std::vector<float> step_k_power;
  std::vector<float> step_powers;
  std::vector<float> step_peaks;
};

Analyser::Analyser(std::uint32_t sample_rate) : state_(std::make_unique<State>(sample_rate)) {}
Analyser::Analyser(Analyser&&) noexcept = default;
Analyser& Analyser::operator=(Analyser&&) noexcept = default;
Analyser::~Analyser() = default;

void Analyser::push(std::span<const float> left, std::span<const float> right) {
  auto& s = *state_;
  const auto n = std::min(left.size(), right.size());
  for (std::size_t i = 0; i < n; ++i)
    s.frame(left[i], right[i]);
  s.frames += n;
}

Analysis Analyser::finish() && {
  auto& s = *state_;
  // The true-peak interpolators answer late: drain them by holding the
  // last frame, as if the programme went on. A programme cut from a song
  // does go on, and a step to silence would read as a peak it never has.
  for (std::size_t i = 0; i < bs1770::TruePeak::radius + 1; ++i)
    s.true_peak = std::max({s.true_peak, std::fabs(s.peaks[left_channel].push(s.last_left)),
                            std::fabs(s.peaks[right_channel].push(s.last_right))});
  Analysis out;
  out.sample_rate = s.rate;
  out.frames = s.frames;
  out.sample_peak_dbfs = db_of_gain(s.sample_peak);
  out.true_peak_dbtp = db_of_gain(s.true_peak);
  out.step_power = s.step_k_power;

  // Loudness: 400 ms blocks every step, gated (BS.1770); the loudest
  // momentary and short-term windows.
  const auto& k = s.step_k_power;
  const auto window_mean = [&](std::size_t last, std::size_t length) {
    double sum = 0.0;
    for (std::size_t i = last + 1 - length; i <= last; ++i)
      sum += k[i];
    return sum / static_cast<double>(length);
  };
  if (k.size() >= bs1770::momentary_steps) {
    bs1770::Gate gate(k.size());
    for (std::size_t last = bs1770::momentary_steps - 1; last < k.size(); ++last) {
      const double power = window_mean(last, bs1770::momentary_steps);
      (void)gate.add(power);
      if (power > 0.0)
        out.max_momentary_lufs = std::max(out.max_momentary_lufs, bs1770::lkfs(power));
      if (last + 1 >= bs1770::short_term_steps) {
        const double term = window_mean(last, bs1770::short_term_steps);
        if (term > 0.0)
          out.max_short_term_lufs = std::max(out.max_short_term_lufs, bs1770::lkfs(term));
      }
    }
    out.integrated_lufs = gate.integrated().integrated_lkfs;
  }

  const auto wide = gate_steps(s.step_powers);
  out.crest_db = mean_crest(s.step_powers, s.step_peaks, wide);
  out.loud_db = wide.loud_db;
  double mid_total = 0.0;
  for (const auto& r : s.region)
    mid_total += r.mid_power;
  for (std::size_t r = 0; r < regions; ++r) {
    auto& region = s.region[r];
    auto& measure = out.region[r];
    const auto gated = gate_steps(region.step_powers);
    measure.crest_db = mean_crest(region.step_powers, region.step_peaks, gated);
    measure.loud_db = gated.loud_db;
    const double loud_seconds = static_cast<double>(gated.steps.size()) * step_seconds;
    measure.onsets_per_second =
        loud_seconds > 0.0 ? static_cast<double>(region.onsets) / loud_seconds : 0.0;
    measure.width_db = region.mid_power > tiny_power
                           ? db_of_power(region.side_power) - db_of_power(region.mid_power)
                           : floor_db;
    measure.share_db =
        mid_total > tiny_power ? db_of_power(region.mid_power / mid_total) : floor_db;
  }

  // The tonal curve and the width per band, from the band meter.
  out.fine_power = s.meter.mean_a();
  const auto mid_levels = tonal::grid_levels(s.meter.mean_a());
  const auto side_levels = tonal::grid_levels(s.meter.mean_b());
  out.tonal = tonal::normalised(mid_levels);
  for (std::size_t b = 0; b < tonal::bands; ++b)
    out.width_db[b] = std::max(floor_db, side_levels[b] - mid_levels[b]);
  double low_mid = 0.0;
  double low_side = 0.0;
  for (std::size_t f = 0; f < tonal::fine_bands; ++f) {
    if (tonal::fine_high_hz(f) > mono_bass_hz)
      break;
    low_mid += s.meter.mean_a()[f];
    low_side += s.meter.mean_b()[f];
  }
  out.low_side_db = low_mid > tiny_power ? db_of_power(low_side) - db_of_power(low_mid) : floor_db;

  // Tonal spread: the regions' balance per segment, over the loud segments.
  const auto segment_steps = static_cast<std::size_t>(std::lround(segment_seconds / step_seconds));
  struct Segment {
    double loudness = 0.0;
    std::array<double, regions> power{};
  };
  std::vector<Segment> segments;
  for (std::size_t first = 0; first + segment_steps <= k.size(); first += segment_steps) {
    Segment segment;
    for (std::size_t i = first; i < first + segment_steps; ++i) {
      segment.loudness += k[i];
      for (std::size_t r = 0; r < regions; ++r)
        segment.power[r] += s.region[r].step_powers[i];
    }
    segments.push_back(segment);
  }
  if (!segments.empty()) {
    const double loudest = std::ranges::max_element(segments, {}, &Segment::loudness)->loudness;
    std::vector<double> low_balance;
    std::vector<double> top_balance;
    for (const auto& segment : segments) {
      if (segment.loudness <= tiny_power ||
          db_of_power(segment.loudness) < db_of_power(loudest) - spread_gate_lu)
        continue;
      low_balance.push_back(db_of_power(segment.power[0]) - db_of_power(segment.power[1]));
      top_balance.push_back(db_of_power(segment.power[2]) - db_of_power(segment.power[1]));
    }
    out.tonal_spread_db =
        std::max(standard_deviation(low_balance), standard_deviation(top_balance));
  }
  return out;
}

Analysis analyse(std::span<const float> left, std::span<const float> right,
                 std::uint32_t sample_rate) {
  Analyser analyser(sample_rate);
  analyser.push(left, right);
  return std::move(analyser).finish();
}

// ---- Region meter --------------------------------------------------------------------

struct RegionMeter::State {
  explicit State(std::uint32_t rate) {
    std::array<dsp::Lr4Design, regions - 1> designs{};
    for (std::size_t k = 0; k < splits.size(); ++k)
      designs[k] = dsp::Lr4Design::at(splits[k], static_cast<double>(rate));
    tree.set(designs, designs.size());
  }
  Tree tree;
  RoleRegions power{};
};

RegionMeter::RegionMeter(std::uint32_t sample_rate)
    : state_(std::make_unique<State>(sample_rate)) {}
RegionMeter::RegionMeter(RegionMeter&&) noexcept = default;
RegionMeter& RegionMeter::operator=(RegionMeter&&) noexcept = default;
RegionMeter::~RegionMeter() = default;

void RegionMeter::push(std::span<const float> mono) {
  std::array<double, regions> band{};
  for (const float x : mono) {
    state_->tree.process(x, band);
    for (std::size_t r = 0; r < regions; ++r)
      state_->power[r] += band[r] * band[r];
  }
}

RoleRegions RegionMeter::powers() const noexcept { return state_->power; }

// ---- Tonal distance ------------------------------------------------------------------

tonal::Curve tonal_difference(const tonal::Curve& levels, const tonal::Curve& target) noexcept {
  tonal::Curve diff{};
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    if (!judged(b))
      continue;
    diff[b] = levels[b] - target[b];
    sum += diff[b];
    ++count;
  }
  const double mean = count == 0 ? 0.0 : sum / static_cast<double>(count);
  for (std::size_t b = 0; b < tonal::bands; ++b)
    diff[b] = judged(b) ? diff[b] - mean : 0.0;
  return diff;
}

TonalDistance tonal_distance(const tonal::Curve& levels, const tonal::Curve& target) noexcept {
  const auto diff = tonal_difference(levels, target);
  TonalDistance out;
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    if (!judged(b))
      continue;
    sum += diff[b] * diff[b];
    ++count;
    if (std::fabs(diff[b]) > std::fabs(out.worst_db)) {
      out.worst_db = diff[b];
      out.worst_band = b;
    }
  }
  out.rms_db = count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
  return out;
}

Window loudest_window(const Analysis& analysis, double seconds) noexcept {
  const auto& k = analysis.step_power;
  const auto step_frames =
      analysis.sample_rate == 0
          ? std::uint64_t{0}
          : static_cast<std::uint64_t>(std::lround(analysis.sample_rate * step_seconds));
  const auto steps = static_cast<std::size_t>(std::lround(seconds / step_seconds));
  if (steps == 0 || k.size() <= steps || step_frames == 0)
    return {0, analysis.frames};
  double sum = 0.0;
  for (std::size_t i = 0; i < steps; ++i)
    sum += k[i];
  double best = sum;
  std::size_t first = 0;
  for (std::size_t i = steps; i < k.size(); ++i) {
    sum += k[i] - k[i - steps];
    if (sum > best) {
      best = sum;
      first = i + 1 - steps;
    }
  }
  return {first * step_frames, steps * step_frames};
}

// ---- References ----------------------------------------------------------------------

Reference reference_from(const tonal::Target& target) {
  Reference out;
  out.name = target.name;
  out.target = target;
  return out;
}

foundation::Result<Reference> reference_from(std::string name, std::span<const Analysis> tracks) {
  using R = foundation::Result<Reference>;
  std::vector<tonal::Curve> curves;
  Traits traits{};
  traits.low_side_db = 0.0;
  std::size_t used = 0;
  for (const auto& track : tracks) {
    if (!track.integrated_lufs)
      continue;
    curves.push_back(track.tonal);
    for (std::size_t r = 0; r < regions; ++r)
      traits.width_db[r] += track.region[r].width_db;
    traits.crest_db += track.crest_db;
    traits.low_side_db += track.low_side_db;
    ++used;
  }
  if (used == 0)
    return R::failure(
        {foundation::ErrorCode::invalid_argument, "No reference has measurable loudness"});
  const auto n = static_cast<double>(used);
  for (auto& w : traits.width_db)
    w /= n;
  traits.crest_db /= n;
  traits.low_side_db /= n;
  auto target = tonal::build_target("reference", name, curves);
  if (!target)
    return R::failure(target.error());
  Reference out;
  out.name = std::move(name);
  out.target = std::move(target).value();
  out.traits = traits;
  return R::success(std::move(out));
}

// ---- Outcome -------------------------------------------------------------------------

Outcome outcome(const Analysis& analysis, const Reference& reference) noexcept {
  return {analysis.lufs(), analysis.true_peak_dbtp,
          tonal_distance(analysis.tonal, reference.target.median).rms_db, analysis.crest_db};
}

namespace {

std::string fixed(double value, int digits) {
  std::array<char, 32> text{};
  (void)std::snprintf(text.data(), text.size(), "%.*f", digits, value);
  return text.data();
}

} // namespace

std::vector<std::string> outcome_lines(const Outcome& before, const Outcome& after,
                                       const Goals& goals) {
  const double lufs = goals.lufs;
  const double ceiling = goals.ceiling_dbtp;
  const bool loud_ok = std::fabs(after.lufs - lufs) <= loudness_tolerance_lu;
  const bool peak_ok = after.true_peak_dbtp <= ceiling;
  return {
      "Loudness " + fixed(after.lufs, 1) + " LUFS against " + fixed(lufs, 1) + " (±" +
          fixed(loudness_tolerance_lu, 1) + "): " + (loud_ok ? "on target" : "off target") +
          "; was " + fixed(before.lufs, 1) + ".",
      "True peak " + fixed(after.true_peak_dbtp, 2) + " dBTP against a " + fixed(ceiling, 1) +
          " dBTP ceiling: " + (peak_ok ? "under it" : "over it") + "; was " +
          fixed(before.true_peak_dbtp, 2) + ".",
      "Tonal distance from the reference " + fixed(after.tonal_rms_db, 2) + " dB RMS; was " +
          fixed(before.tonal_rms_db, 2) + ".",
      "Crest factor " + fixed(after.crest_db, 1) + " dB; was " + fixed(before.crest_db, 1) + ".",
  };
}

bool meets(const Outcome& outcome, const Goals& goals) noexcept {
  return std::fabs(outcome.lufs - goals.lufs) <= loudness_tolerance_lu &&
         outcome.true_peak_dbtp <= goals.ceiling_dbtp;
}

double matched_gain_db(double lufs, double other_lufs) noexcept {
  return std::min(lufs, other_lufs) - lufs;
}

} // namespace oscillate::native_dsp::assistant
