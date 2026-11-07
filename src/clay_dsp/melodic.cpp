#include "detect.hpp"
#include "spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <vector>

// Melodic analysis: probabilistic YIN (Mauch & Dixon 2014) with a Viterbi
// path over pitch and voicing, notes cut where the voicing stops, the
// smoothed pitch moves to a new level or the level jumps, then partials and
// the envelope through the partials' levels per hop.
namespace oscillate::clay_dsp::detail {
namespace {

// YIN integrates over this long; a frame is twice it.
constexpr double yin_window_seconds = 1'024.0 / 48'000.0;
// pYIN's threshold prior: Beta(2, 11.33), mean 0.15, over 100 thresholds.
constexpr int threshold_count = 100;
constexpr double beta_alpha = 2.0;
constexpr double beta_beta = 11.33;
// The pitch states and how far the pitch may move per hop.
constexpr double bin_cents = 20.0;
constexpr int max_jump_bins = 13;
// Probability of keeping the voicing from one hop to the next.
constexpr double voicing_stay = 0.99;
// A candidate snaps the path to its exact pitch within this many bins.
constexpr double snap_bins = 1.5;
// Frames this far under the loudest are silence.
constexpr double silence_db = -60.0;
// Unvoiced gaps this short inside a note are bridged.
constexpr double bridge_seconds = 0.015;
// A note changes when its smoothed pitch leaves its level by this much for
// this long; the smoothing is a moving average this long.
constexpr double split_cents = 70.0;
constexpr double split_hold_seconds = 0.06;
constexpr double split_smooth_seconds = 0.2;
// A re-articulation: the level rises by this much from a dip.
constexpr double reattack_db = 9.0;
constexpr double reattack_seconds = 0.05;
// Edges are refined on RMS blocks this long: a note starts where the level
// first reaches its median minus start_db, and ends where it last holds
// above its median minus end_db.
constexpr double block_seconds = 0.001;
constexpr double edge_reference_seconds = 0.1;
constexpr double start_db = -12.0;
constexpr double end_db = -24.0;
// Partials: windows hold at least this many periods of the lowest pitch;
// a partial is looked for within this share of f0 around h * f0.
constexpr double partial_periods = 4.0;
constexpr std::size_t max_partial_window = 8'192;
constexpr std::size_t max_partials = 48;
constexpr double partial_search = 0.4;
// Cancellation is polled every this many hops.
constexpr std::size_t poll_hops = 256;
constexpr double tiny = 1e-12;

struct Candidate {
  double frequency = 0.0;
  double probability = 0.0;
};

struct YinFrame {
  std::vector<Candidate> candidates;
  double energy_db = -std::numeric_limits<double>::infinity();
};

class Yin {
public:
  Yin(std::uint32_t sample_rate, const AnalysisOptions& options)
      : rate_(sample_rate),
        window_(static_cast<std::size_t>(std::round(yin_window_seconds * sample_rate))),
        size_(fft_size_at_least(3.0 * static_cast<double>(window_))), fft_(size_), a_(size_),
        b_(size_), spectrum_a_(size_), spectrum_b_(size_), product_(size_), d_(window_),
        cmndf_(window_), squares_(2 * window_ + 1) {
    min_lag_ = std::max<std::size_t>(2, static_cast<std::size_t>(sample_rate / options.maximum_hz));
    max_lag_ =
        std::min(window_ - 2, static_cast<std::size_t>(sample_rate / options.minimum_hz) + 1);
    double total = 0.0;
    for (int i = 0; i < threshold_count; ++i) {
      const double s = (i + 1.0) / threshold_count;
      thresholds_.push_back(s);
      const double weight =
          std::pow(s, beta_alpha - 1.0) * std::pow(std::max(0.0, 1.0 - s), beta_beta - 1.0);
      prior_.push_back(weight);
      total += weight;
    }
    for (auto& p : prior_)
      p /= total;
  }

  [[nodiscard]] std::size_t window() const noexcept { return window_; }

  void analyse(std::span<const float> signal, std::int64_t centre, YinFrame& out) {
    out.candidates.clear();
    // The difference function integrates over [first, first + window) and
    // reads up to a lag beyond it, so centring the integration window on
    // `centre` makes the pitch belong to that moment.
    const auto first = centre - static_cast<std::int64_t>(window_ / 2);
    const auto length = static_cast<std::int64_t>(signal.size());
    const auto read = [&](std::size_t j) -> float {
      const auto at = first + static_cast<std::int64_t>(j);
      return at >= 0 && at < length ? signal[static_cast<std::size_t>(at)] : 0.0F;
    };
    std::fill_n(a_.data(), size_, 0.0F);
    std::fill_n(b_.data(), size_, 0.0F);
    squares_[0] = 0.0;
    for (std::size_t j = 0; j < 2 * window_; ++j) {
      const float x = read(j);
      b_[j] = x;
      if (j < window_)
        a_[j] = x;
      squares_[j + 1] = squares_[j] + static_cast<double>(x) * x;
    }
    const double e0 = squares_[window_];
    out.energy_db = 10.0 * std::log10(std::max(tiny, e0 / static_cast<double>(window_)));
    if (e0 <= tiny)
      return;
    // r(tau) = sum a[j] b[j + tau] = IFFT(conj(A) B).
    fft_.forward(a_.data(), spectrum_a_.data());
    fft_.forward(b_.data(), spectrum_b_.data());
    product_[0] = spectrum_a_[0] * spectrum_b_[0];
    product_[1] = spectrum_a_[1] * spectrum_b_[1];
    for (std::size_t k = 1; k < size_ / 2; ++k) {
      const float ar = spectrum_a_[2 * k];
      const float ai = spectrum_a_[2 * k + 1];
      const float br = spectrum_b_[2 * k];
      const float bi = spectrum_b_[2 * k + 1];
      product_[2 * k] = ar * br + ai * bi;
      product_[2 * k + 1] = ar * bi - ai * br;
    }
    fft_.inverse(product_.data(), a_.data());
    const double scale = 1.0 / static_cast<double>(size_);
    double running = 0.0;
    cmndf_[0] = 1.0;
    for (std::size_t tau = 1; tau <= max_lag_; ++tau) {
      const double e_tau = squares_[tau + window_] - squares_[tau];
      d_[tau] = std::max(0.0, e0 + e_tau - 2.0 * a_[tau] * scale);
      running += d_[tau];
      cmndf_[tau] = running > tiny ? d_[tau] * static_cast<double>(tau) / running : 1.0;
    }
    // Local minima of the CMNDF, refined by a parabola.
    struct Minimum {
      double lag;
      double value;
    };
    std::vector<Minimum> minima;
    for (std::size_t tau = min_lag_; tau + 1 <= max_lag_ - 1; ++tau) {
      const double y0 = cmndf_[tau - 1];
      const double y1 = cmndf_[tau];
      const double y2 = cmndf_[tau + 1];
      if (y1 < y0 && y1 <= y2) {
        const double denominator = y0 - 2.0 * y1 + y2;
        const double shift =
            denominator > tiny ? std::clamp(0.5 * (y0 - y2) / denominator, -0.5, 0.5) : 0.0;
        minima.push_back({static_cast<double>(tau) + shift, y1 - 0.25 * (y0 - y2) * shift});
      }
    }
    if (minima.empty())
      return;
    std::vector<double> mass(minima.size(), 0.0);
    for (std::size_t i = 0; i < thresholds_.size(); ++i) {
      for (std::size_t m = 0; m < minima.size(); ++m) {
        if (minima[m].value < thresholds_[i]) {
          mass[m] += prior_[i];
          break;
        }
      }
    }
    for (std::size_t m = 0; m < minima.size(); ++m) {
      if (mass[m] > 0.0)
        out.candidates.push_back({static_cast<double>(rate_) / minima[m].lag, mass[m]});
    }
  }

private:
  std::uint32_t rate_;
  std::size_t window_;
  std::size_t size_;
  warp_dsp::detail::RealFft fft_;
  warp_dsp::detail::AlignedBuffer a_;
  warp_dsp::detail::AlignedBuffer b_;
  warp_dsp::detail::AlignedBuffer spectrum_a_;
  warp_dsp::detail::AlignedBuffer spectrum_b_;
  warp_dsp::detail::AlignedBuffer product_;
  std::vector<double> d_;
  std::vector<double> cmndf_;
  std::vector<double> squares_;
  std::size_t min_lag_ = 2;
  std::size_t max_lag_ = 2;
  std::vector<double> thresholds_;
  std::vector<double> prior_;
};

struct PathPoint {
  bool voiced = false;
  double frequency = 0.0;
};

// Viterbi over (voiced, unvoiced) x pitch bins.
[[nodiscard]] std::vector<PathPoint> viterbi(const std::vector<YinFrame>& frames,
                                             const AnalysisOptions& options, double peak_db) {
  const double min_hz = options.minimum_hz;
  const double max_hz = options.maximum_hz;
  const auto bins =
      static_cast<int>(std::ceil(cents_per_octave * std::log2(max_hz / min_hz) / bin_cents)) + 1;
  // Each pitch bin voiced, then each unvoiced.
  const auto state_index = [bins](int voicing, int bin) {
    return static_cast<std::size_t>(voicing) * static_cast<std::size_t>(bins) +
           static_cast<std::size_t>(bin);
  };
  const auto states = state_index(2, 0);
  const auto bin_of = [&](double hz) {
    return cents_per_octave * std::log2(hz / min_hz) / bin_cents;
  };
  // Triangular pitch transition, normalised.
  std::vector<double> jump(static_cast<std::size_t>(max_jump_bins + 1));
  double norm = 0.0;
  for (int d = -max_jump_bins; d <= max_jump_bins; ++d)
    norm += max_jump_bins + 1 - std::abs(d);
  for (int d = 0; d <= max_jump_bins; ++d)
    jump[static_cast<std::size_t>(d)] = std::log((max_jump_bins + 1 - d) / norm);
  const double stay = std::log(voicing_stay);
  const double change = std::log(1.0 - voicing_stay);
  const auto count = frames.size();
  std::vector<std::uint16_t> back(count * states, 0);
  std::vector<double> score(states, -std::log(static_cast<double>(states)));
  std::vector<double> next(states);
  std::vector<double> observed(states);
  for (std::size_t t = 0; t < count; ++t) {
    std::ranges::fill(observed, 0.0);
    double total = 0.0;
    const bool silent = frames[t].energy_db < peak_db + silence_db;
    if (!silent) {
      for (const auto& c : frames[t].candidates) {
        const auto b = static_cast<int>(std::lround(bin_of(c.frequency)));
        if (b < 0 || b >= bins)
          continue;
        observed[static_cast<std::size_t>(b)] += c.probability;
        total += c.probability;
      }
    }
    const double unvoiced = std::max(0.0, 1.0 - total) / bins;
    for (int b = 0; b < bins; ++b) {
      observed[static_cast<std::size_t>(b)] =
          std::log(observed[static_cast<std::size_t>(b)] + tiny);
      observed[state_index(1, b)] = std::log(unvoiced + tiny);
    }
    if (t == 0) {
      for (std::size_t s = 0; s < states; ++s)
        score[s] += observed[s];
      continue;
    }
    for (int b = 0; b < bins; ++b) {
      for (int voicing = 0; voicing < 2; ++voicing) {
        double best = -std::numeric_limits<double>::infinity();
        std::uint16_t from = 0;
        for (int p = std::max(0, b - max_jump_bins); p <= std::min(bins - 1, b + max_jump_bins);
             ++p) {
          const double j = jump[static_cast<std::size_t>(std::abs(b - p))];
          for (int previous = 0; previous < 2; ++previous) {
            const auto s = state_index(previous, p);
            const double value = score[s] + j + (previous == voicing ? stay : change);
            if (value > best) {
              best = value;
              from = static_cast<std::uint16_t>(s);
            }
          }
        }
        const auto target = state_index(voicing, b);
        next[target] = best + observed[target];
        back[t * states + target] = from;
      }
    }
    const double top = *std::ranges::max_element(next);
    for (std::size_t s = 0; s < states; ++s)
      score[s] = next[s] - top;
  }
  std::vector<PathPoint> path(count);
  if (count == 0)
    return path;
  auto state =
      static_cast<std::size_t>(std::distance(score.begin(), std::ranges::max_element(score)));
  for (std::size_t t = count; t-- > 0;) {
    const auto b = static_cast<int>(state) % bins;
    const bool voiced = static_cast<int>(state) < bins;
    double frequency = min_hz * std::exp2(b * bin_cents / cents_per_octave);
    double nearest = snap_bins;
    for (const auto& c : frames[t].candidates) {
      const double distance = std::abs(bin_of(c.frequency) - b);
      if (distance < nearest) {
        nearest = distance;
        frequency = c.frequency;
      }
    }
    path[t] = {voiced, frequency};
    if (t > 0)
      state = back[t * states + state];
  }
  return path;
}

struct Run {
  std::size_t first = 0; // hops
  std::size_t last = 0;  // inclusive
  std::int64_t start = 0;
  std::int64_t end = 0;
  bool refine_start = true;
  bool refine_end = true;
};

[[nodiscard]] double median(std::vector<double> values) {
  if (values.empty())
    return 0.0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

// Splits a voiced run where its pitch settles at a new level, or where the
// level rises from a dip.
void split_run(const Run& run, const std::vector<PathPoint>& path,
               const std::vector<YinFrame>& frames, std::int64_t hop, std::uint32_t rate,
               std::vector<Run>& out) {
  const auto count = run.last - run.first + 1;
  std::vector<double> cents(count);
  double last_valid = 0.0;
  bool any = false;
  for (std::size_t i = 0; i < count; ++i) {
    const double f = path[run.first + i].frequency;
    if (path[run.first + i].voiced && f > 0.0) {
      last_valid = hz_to_cents(f);
      any = true;
    }
    cents[i] = last_valid;
  }
  if (!any) {
    out.push_back(run);
    return;
  }
  // Fill the leading gap with the first valid value.
  for (std::size_t i = 0; i < count; ++i) {
    if (path[run.first + i].voiced && path[run.first + i].frequency > 0.0) {
      for (std::size_t j = 0; j < i; ++j)
        cents[j] = cents[i];
      break;
    }
  }
  const double per_hop = static_cast<double>(rate) / static_cast<double>(hop);
  const auto half = static_cast<std::size_t>(std::max(1.0, split_smooth_seconds * per_hop / 2.0));
  const auto hold = static_cast<std::size_t>(std::max(1.0, split_hold_seconds * per_hop));
  std::vector<double> prefix(count + 1, 0.0);
  for (std::size_t i = 0; i < count; ++i)
    prefix[i + 1] = prefix[i] + cents[i];
  std::vector<double> smooth(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto lo = i >= half ? i - half : 0;
    const auto hi = std::min(count, i + half + 1);
    smooth[i] = (prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo);
  }
  std::vector<std::size_t> cuts;
  const auto level_over = [&](std::size_t from, std::size_t to) {
    std::vector<double> values(cents.begin() + static_cast<std::ptrdiff_t>(from),
                               cents.begin() + static_cast<std::ptrdiff_t>(std::min(count, to)));
    return median(std::move(values));
  };
  double anchor = level_over(0, hold);
  std::size_t pending = count;
  // After a cut the smoothed pitch is still gliding; wait until it reaches
  // the new level before looking for the next change.
  bool settling = false;
  for (std::size_t i = 0; i < count; ++i) {
    if (std::abs(smooth[i] - anchor) <= split_cents) {
      pending = count;
      settling = false;
      continue;
    }
    if (settling)
      continue;
    if (pending == count)
      pending = i;
    if (i - pending + 1 < hold)
      continue;
    // The smoothed pitch starts to move up to half a window before the raw
    // pitch does, so the new level is read once that window has passed, and
    // the cut (where the smoothed pitch crosses halfway) lies within a
    // window of where it started to move.
    const auto settled = std::min(pending + half, count - 1);
    const double level = level_over(settled, settled + hold);
    if (std::abs(level - anchor) <= split_cents) {
      pending = count;
      continue;
    }
    const double middle = 0.5 * (anchor + level);
    const double direction = level > anchor ? 1.0 : -1.0;
    auto cut = pending;
    const auto from = pending >= hold ? pending - hold : 0;
    const auto until = std::min(count - 1, pending + 2 * half);
    for (std::size_t j = from; j <= until; ++j) {
      if ((smooth[j] - middle) * direction >= 0.0) {
        cut = j;
        break;
      }
    }
    if (cut > 0 && (cuts.empty() || cut > cuts.back()))
      cuts.push_back(cut);
    anchor = level;
    pending = count;
    settling = true;
  }
  // Re-articulations: a rise of reattack_db from a dip.
  const auto reach = static_cast<std::size_t>(std::max(1.0, reattack_seconds * per_hop));
  for (std::size_t i = reach; i < count; ++i) {
    std::size_t dip = i - reach;
    for (std::size_t j = i - reach; j < i; ++j) {
      if (frames[run.first + j].energy_db < frames[run.first + dip].energy_db)
        dip = j;
    }
    const double before_peak = frames[run.first + (dip >= reach ? dip - reach : 0)].energy_db;
    const double low = frames[run.first + dip].energy_db;
    if (dip > 0 && frames[run.first + i].energy_db - low >= reattack_db &&
        before_peak - low >= reattack_db) {
      if (std::ranges::find(cuts, dip) == cuts.end())
        cuts.push_back(dip);
      i += reach;
    }
  }
  std::ranges::sort(cuts);
  std::size_t from = 0;
  for (std::size_t c = 0; c <= cuts.size(); ++c) {
    const auto to = c < cuts.size() ? cuts[c] : count;
    if (to <= from)
      continue;
    Run part;
    part.first = run.first + from;
    part.last = run.first + to - 1;
    part.start = from == 0 ? run.start : static_cast<std::int64_t>(part.first) * hop;
    part.end = to == count ? run.end : static_cast<std::int64_t>(run.first + to) * hop;
    part.refine_start = from == 0 && run.refine_start;
    part.refine_end = to == count && run.refine_end;
    out.push_back(part);
    from = to;
  }
}

class EdgeFinder {
public:
  EdgeFinder(std::span<const float> mono, std::uint32_t rate)
      : block_(std::max<std::int64_t>(
            1, static_cast<std::int64_t>(std::lround(block_seconds * rate)))),
        rate_(rate) {
    const auto blocks = (static_cast<std::int64_t>(mono.size()) + block_ - 1) / block_;
    rms_.resize(static_cast<std::size_t>(blocks), 0.0);
    for (std::size_t i = 0; i < mono.size(); ++i)
      rms_[i / static_cast<std::size_t>(block_)] += static_cast<double>(mono[i]) * mono[i];
    for (auto& r : rms_)
      r = std::sqrt(r / static_cast<double>(block_));
  }

  [[nodiscard]] std::int64_t start(std::int64_t guess, std::int64_t search) const {
    const double reference = reference_level(guess, guess + reference_frames());
    if (reference <= tiny)
      return guess;
    const double threshold = reference * db_to_ratio(start_db);
    for (auto b = block_of(guess - search); b <= block_of(guess + search); ++b) {
      if (level(b) >= threshold)
        return b * block_;
    }
    return guess;
  }

  [[nodiscard]] std::int64_t end(std::int64_t guess, std::int64_t search) const {
    const double reference = reference_level(guess - reference_frames(), guess);
    if (reference <= tiny)
      return guess;
    const double threshold = reference * db_to_ratio(end_db);
    for (auto b = block_of(guess + search); b >= block_of(guess - search); --b) {
      if (level(b) >= threshold)
        return (b + 1) * block_;
    }
    return guess;
  }

private:
  [[nodiscard]] std::int64_t reference_frames() const noexcept {
    return static_cast<std::int64_t>(edge_reference_seconds * rate_);
  }
  [[nodiscard]] std::int64_t block_of(std::int64_t frame) const noexcept {
    return std::clamp<std::int64_t>(frame / block_, 0, static_cast<std::int64_t>(rms_.size()) - 1);
  }
  [[nodiscard]] double level(std::int64_t block) const noexcept {
    return rms_[static_cast<std::size_t>(block)];
  }
  [[nodiscard]] double reference_level(std::int64_t from, std::int64_t to) const {
    std::vector<double> values;
    for (auto b = block_of(from); b <= block_of(to); ++b)
      values.push_back(level(b));
    return median(std::move(values));
  }

  std::int64_t block_;
  std::uint32_t rate_;
  std::vector<double> rms_;
};

// Partials and envelope for every hop of a note.
void track_partials(Note& note, std::span<const float> mono, std::uint32_t rate, std::int64_t hop,
                    std::map<std::size_t, std::unique_ptr<FrameAnalyser>>& analysers) {
  const auto hops = note.hops();
  double lowest = std::numeric_limits<double>::infinity();
  for (const float f : note.pitch) {
    if (f > 0.0F)
      lowest = std::min(lowest, static_cast<double>(f));
  }
  if (!std::isfinite(lowest))
    return;
  const auto size =
      std::min(max_partial_window, fft_size_at_least(partial_periods * rate / lowest));
  auto& analyser = analysers[size];
  if (!analyser)
    analyser = std::make_unique<FrameAnalyser>(size);
  const double bin_hz = static_cast<double>(rate) / static_cast<double>(size);
  const auto ceiling = partial_ceiling * rate;
  note.partial_count =
      std::clamp<std::size_t>(static_cast<std::size_t>(ceiling / lowest), 1, max_partials);
  note.partials.assign(hops * note.partial_count, PartialFrame{});
  note.amplitude.assign(hops, 0.0F);
  const double phase_step = two_pi * static_cast<double>(hop) / rate;
  for (std::size_t k = 0; k < hops; ++k) {
    const double f0 = note.pitch[k];
    analyser->analyse(mono, (note.first_hop + static_cast<std::int64_t>(k)) * hop);
    const auto magnitude = analyser->magnitude();
    const double loudest = *std::ranges::max_element(magnitude);
    const double floor = loudest * db_to_ratio(peak_floor_db);
    double total = 0.0;
    for (std::size_t h = 0; h < note.partial_count; ++h) {
      auto& p = note.partials[k * note.partial_count + h];
      const double target = static_cast<double>(h + 1) * f0;
      const auto previous =
          k > 0 ? note.partials[(k - 1) * note.partial_count + h] : PartialFrame{};
      p.frequency = static_cast<float>(target);
      p.phase = static_cast<float>(std::remainder(previous.phase + phase_step * target, two_pi));
      if (f0 <= 0.0 || target >= ceiling)
        continue;
      const auto lo = static_cast<std::size_t>(
          std::max(1.0, std::floor((target - partial_search * f0) / bin_hz)));
      const auto hi =
          std::min(magnitude.size() - 2,
                   static_cast<std::size_t>(std::ceil((target + partial_search * f0) / bin_hz)));
      std::size_t best = 0;
      for (auto b = lo; b <= hi; ++b) {
        if (magnitude[b] >= floor && magnitude[b] > magnitude[b - 1] &&
            magnitude[b] >= magnitude[b + 1] && (best == 0 || magnitude[b] > magnitude[best]))
          best = b;
      }
      if (best == 0)
        continue;
      const auto peak = analyser->peak(best);
      p.frequency = static_cast<float>(peak.bin * bin_hz);
      p.amplitude = static_cast<float>(peak.amplitude);
      p.phase = static_cast<float>(peak.phase);
      total += peak.amplitude;
    }
    note.amplitude[k] = static_cast<float>(total);
  }
}

} // namespace

bool detect_melodic(const Context& context, std::int64_t hop, std::vector<Note>& notes) {
  const auto& options = *context.options;
  const auto rate = context.sample_rate;
  const auto mono = context.mono;
  const auto length = static_cast<std::int64_t>(mono.size());
  const auto hop_count = static_cast<std::size_t>(length / hop + 1);
  Yin yin(rate, options);
  std::vector<YinFrame> frames(hop_count);
  double peak_db = -std::numeric_limits<double>::infinity();
  for (std::size_t k = 0; k < hop_count; ++k) {
    if (k % poll_hops == 0 && context.stopped())
      return false;
    yin.analyse(mono, static_cast<std::int64_t>(k) * hop, frames[k]);
    peak_db = std::max(peak_db, frames[k].energy_db);
  }
  const auto path = viterbi(frames, options, peak_db);
  // Voiced runs, bridging short gaps.
  const auto bridge = static_cast<std::size_t>(bridge_seconds * rate / static_cast<double>(hop));
  std::vector<Run> runs;
  std::size_t k = 0;
  while (k < hop_count) {
    if (!path[k].voiced) {
      ++k;
      continue;
    }
    Run run;
    run.first = k;
    std::size_t last = k;
    std::size_t j = k + 1;
    while (j < hop_count) {
      if (path[j].voiced) {
        last = j;
        ++j;
        continue;
      }
      std::size_t gap_end = j;
      while (gap_end < hop_count && !path[gap_end].voiced)
        ++gap_end;
      if (gap_end < hop_count && gap_end - j <= bridge) {
        j = gap_end;
        continue;
      }
      break;
    }
    run.last = last;
    run.start = static_cast<std::int64_t>(run.first) * hop;
    run.end = static_cast<std::int64_t>(run.last + 1) * hop;
    runs.push_back(run);
    k = last + 1;
  }
  std::vector<Run> parts;
  for (const auto& run : runs)
    split_run(run, path, frames, hop, rate, parts);
  const EdgeFinder edges(mono, rate);
  const auto search = static_cast<std::int64_t>(yin.window()) + hop;
  const auto minimum = static_cast<std::int64_t>(options.minimum_note_seconds * rate);
  std::map<std::size_t, std::unique_ptr<FrameAnalyser>> analysers;
  for (const auto& part : parts) {
    if (context.stopped())
      return false;
    Note note;
    note.kind = NoteKind::sinusoidal;
    note.start = part.refine_start ? edges.start(part.start, search) : part.start;
    note.end = part.refine_end ? edges.end(part.end, search) : part.end;
    note.start = std::clamp<std::int64_t>(note.start, 0, length);
    note.end = std::clamp<std::int64_t>(note.end, note.start, length);
    if (note.end - note.start < minimum)
      continue;
    note.first_hop = note.start / hop;
    const auto last_hop = (note.end + hop - 1) / hop;
    const auto count = static_cast<std::size_t>(last_hop - note.first_hop + 1);
    note.pitch.assign(count, 0.0F);
    // Pitch from the path, held across unvoiced hops and the edges.
    float held = 0.0F;
    for (std::size_t i = 0; i < count; ++i) {
      const auto at = static_cast<std::size_t>(note.first_hop) + i;
      if (at >= part.first && at <= part.last && at < hop_count && path[at].voiced)
        held = static_cast<float>(path[at].frequency);
      note.pitch[i] = held;
    }
    const auto first_voiced = std::ranges::find_if(note.pitch, [](float f) { return f > 0.0F; });
    if (first_voiced == note.pitch.end())
      continue;
    std::fill(note.pitch.begin(), first_voiced, *first_voiced);
    track_partials(note, mono, rate, hop, analysers);
    // The envelope runs through the partials' own levels: on a sung vowel
    // this lands within 1 dB of the resonances between harmonics, where the
    // true envelope (iterated cepstrum) misses by 3 dB or more.
    envelope_from_partials(note);
    notes.push_back(std::move(note));
  }
  return true;
}

} // namespace oscillate::clay_dsp::detail
