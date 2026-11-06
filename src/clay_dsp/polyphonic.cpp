#include "detect.hpp"
#include "note_sets.hpp"

#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include "spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

// Polyphonic analysis: two STFT resolutions with reassigned peaks; per hop,
// iterative estimation and cancellation (Klapuri 2006) seeds a joint choice
// of the note set that best explains the frame (NoteSets); each segment
// between onsets takes its notes by a vote over its settled hops, and a note
// carries on while its partial series still covers energy. Partials shared
// between notes are split by what each note's neighbouring partials lead one
// to expect, and tails far under the notes they follow are dropped.
namespace oscillate::clay_dsp::detail {
namespace {

// The short window; the long one is twice it and serves low pitches.
constexpr double short_window_seconds = 4'096.0 / 48'000.0;
constexpr double long_pitch_below_hz = 200.0;
// Candidate pitches and harmonic summation.
constexpr double candidate_cents = 20.0;
constexpr std::size_t salience_harmonics = 20;
constexpr double weight_alpha_hz = 27.0;
constexpr double weight_beta_hz = 320.0;
// A peak counts as harmonic h of a pitch within this many cents (or this
// share of a bin, whichever is wider).
constexpr double match_cents = 35.0;
constexpr double match_bins = 0.6;
// A note stops being looked for when the best remaining salience falls under
// this share of the frame's first.
constexpr double stop_ratio = 0.3;
// A cancelled partial is taken out whole unless it stands this far above
// its neighbours (then another note shares it, and only the neighbours'
// level is taken).
constexpr double shared_excess = 1.5;
// A pitch's fundamental must reach this share of its strongest partial's
// (whitened, square-root) weight: -40 dB in amplitude, so weak piano bass
// fundamentals still count.
constexpr double fundamental_floor = 0.1;
// The f0 of an estimate is refined from its first harmonics.
constexpr std::size_t refine_harmonics = 8;
// Pitches within this many cents are one note.
constexpr double track_cents = 50.0;
// The joint choice reads the long window's peaks below this and the short
// window's above.
constexpr double merged_below_hz = 500.0;
// A segment's notes are voted over this long once it has settled: a note
// chosen in this share of those hops sounds. A note stops when its series
// covers under end_ratio of its best for quiet_seconds.
constexpr double vote_seconds = 0.1;
constexpr double vote_share = 0.5;
constexpr double end_ratio = 0.003;
constexpr double quiet_seconds = 0.03;
// A note entering without a transient must cover this share of the frame
// that no sounding note does.
constexpr double entry_share = 0.05;

// A note whose loudest point is this far under a note sounding with it, or
// that stopped this recently before it, is a tail (dampers, resonance).
constexpr double tail_db = -26.0;
constexpr double tail_window_seconds = 0.5;
// Partials per note.
constexpr std::size_t max_partials = 30;
constexpr std::size_t poll_hops = 128;

struct Spectra {
  FrameAnalyser short_window;
  FrameAnalyser long_window;
  std::vector<Peak> short_peaks;
  std::vector<Peak> long_peaks;
  double short_bin_hz = 0.0;
  double long_bin_hz = 0.0;

  explicit Spectra(std::uint32_t rate)
      : short_window(fft_size_at_least(short_window_seconds * rate)),
        long_window(2 * short_window.size()),
        short_bin_hz(static_cast<double>(rate) / static_cast<double>(short_window.size())),
        long_bin_hz(static_cast<double>(rate) / static_cast<double>(long_window.size())) {}

  void analyse(std::span<const float> mono, std::int64_t centre) {
    short_window.analyse(mono, centre);
    long_window.analyse(mono, centre);
    for (auto* pair : {&short_window, &long_window}) {
      const auto magnitude = pair->magnitude();
      const double loudest = *std::max_element(magnitude.begin(), magnitude.end());
      pair->peaks(loudest * db_to_ratio(peak_floor_db),
                  pair == &short_window ? short_peaks : long_peaks);
    }
  }

  [[nodiscard]] bool low(double f0) const noexcept { return f0 < long_pitch_below_hz; }
  [[nodiscard]] double bin_hz_for(double f0) const noexcept {
    return low(f0) ? long_bin_hz : short_bin_hz;
  }
};

// A peak in Hz with its remaining (whitened) weight.
struct Weighted {
  double hz = 0.0;
  double weight = 0.0;
  double residual = 0.0;
};

// The index of the peak with the largest remaining weight within the
// tolerance of `hz`, or npos.
constexpr auto npos = std::numeric_limits<std::size_t>::max();
[[nodiscard]] std::size_t match(const std::vector<Weighted>& peaks, double hz,
                                double tolerance_hz) {
  const auto lo = std::ranges::lower_bound(peaks, hz - tolerance_hz, {}, &Weighted::hz);
  std::size_t best = npos;
  for (auto it = lo; it != peaks.end() && it->hz <= hz + tolerance_hz; ++it) {
    const auto index = static_cast<std::size_t>(it - peaks.begin());
    if (it->residual > 0.0 && (best == npos || it->residual > peaks[best].residual))
      best = index;
  }
  return best;
}

// The index of the peak nearest `hz` within the tolerance, or npos. A
// note's partials take the nearest peak, never a louder neighbour that
// belongs to another note.
[[nodiscard]] std::size_t nearest(const std::vector<Weighted>& peaks, double hz,
                                  double tolerance_hz) {
  const auto lo = std::ranges::lower_bound(peaks, hz - tolerance_hz, {}, &Weighted::hz);
  std::size_t best = npos;
  for (auto it = lo; it != peaks.end() && it->hz <= hz + tolerance_hz; ++it) {
    const auto index = static_cast<std::size_t>(it - peaks.begin());
    if (best == npos || std::abs(it->hz - hz) < std::abs(peaks[best].hz - hz))
      best = index;
  }
  return best;
}

[[nodiscard]] double tolerance(double hz, double bin_hz) noexcept {
  return std::max(hz * (std::exp2(match_cents / cents_per_octave) - 1.0), match_bins * bin_hz);
}

struct Estimate {
  double f0 = 0.0;
  double salience = 0.0;
};

class Estimator {
public:
  Estimator(const AnalysisOptions& options, std::uint32_t rate) : rate_(rate) {
    const double octaves = std::log2(options.maximum_hz / options.minimum_hz);
    const auto count = static_cast<std::size_t>(octaves * cents_per_octave / candidate_cents) + 1;
    for (std::size_t c = 0; c < count; ++c)
      candidates_.push_back(options.minimum_hz *
                            std::exp2(static_cast<double>(c) * candidate_cents / cents_per_octave));
    max_polyphony_ = options.max_polyphony;
  }

  void estimate(Spectra& spectra, std::vector<Estimate>& out) {
    out.clear();
    load(spectra.short_peaks, spectra.short_bin_hz, short_);
    load(spectra.long_peaks, spectra.long_bin_hz, long_);
    double first = 0.0;
    for (std::size_t voice = 0; voice < max_polyphony_; ++voice) {
      double best = 0.0;
      double best_f0 = 0.0;
      for (const double f0 : candidates_) {
        // One estimate per pitch per hop.
        const bool taken = std::ranges::any_of(out, [&](const Estimate& e) {
          return std::abs(cents_per_octave * std::log2(f0 / e.f0)) < track_cents;
        });
        if (taken)
          continue;
        const double s = salience(f0, spectra);
        if (s > best) {
          best = s;
          best_f0 = f0;
        }
      }
      if (best <= 0.0 || (voice > 0 && best < stop_ratio * first))
        break;
      if (voice == 0)
        first = best;
      const double f0 = refine(best_f0, spectra);
      out.push_back({f0, best});
      cancel(f0, spectra.short_bin_hz, short_);
      cancel(f0, spectra.long_bin_hz, long_);
    }
  }

private:
  // Peaks are whitened by a square root so that loud low partials do not
  // drown the rest.
  static void load(const std::vector<Peak>& peaks, double bin_hz, std::vector<Weighted>& out) {
    out.clear();
    for (const auto& p : peaks) {
      const double w = std::sqrt(p.amplitude);
      out.push_back({p.bin * bin_hz, w, w});
    }
  }

  [[nodiscard]] std::vector<Weighted>& list(double f0) noexcept {
    return f0 < long_pitch_below_hz ? long_ : short_;
  }

  [[nodiscard]] double salience(double f0, Spectra& spectra) {
    auto& peaks = list(f0);
    const double bin_hz = spectra.bin_hz_for(f0);
    const double ceiling = partial_ceiling * rate_;
    // A pitch needs its own fundamental: without one, a sub-octave or
    // sub-fifth of two notes (A2 under A3 and E4) gathers their partials and,
    // once picked, cancels them.
    const auto fundamental = nearest(peaks, f0, tolerance(f0, bin_hz));
    if (fundamental == npos)
      return 0.0;
    double sum = 0.0;
    double strongest = 0.0;
    for (std::size_t h = 1; h <= salience_harmonics; ++h) {
      const double hz = static_cast<double>(h) * f0;
      if (hz >= ceiling)
        break;
      const auto i = match(peaks, hz, tolerance(hz, bin_hz));
      if (i == npos)
        continue;
      strongest = std::max(strongest, peaks[i].weight);
      sum += (f0 + weight_alpha_hz) / (hz + weight_beta_hz) * peaks[i].residual;
    }
    if (peaks[fundamental].weight < fundamental_floor * strongest)
      return 0.0;
    return sum;
  }

private:
  [[nodiscard]] double refine(double f0, Spectra& spectra) {
    auto& peaks = list(f0);
    const double bin_hz = spectra.bin_hz_for(f0);
    double weighted = 0.0;
    double total = 0.0;
    for (std::size_t h = 1; h <= refine_harmonics; ++h) {
      const double hz = static_cast<double>(h) * f0;
      const auto i = match(peaks, hz, tolerance(hz, bin_hz));
      if (i == npos)
        continue;
      weighted += peaks[i].hz / static_cast<double>(h) * peaks[i].residual;
      total += peaks[i].residual;
    }
    return total > 0.0 ? weighted / total : f0;
  }

  void cancel(double f0, double bin_hz, std::vector<Weighted>& peaks) const {
    const double ceiling = partial_ceiling * rate_;
    std::vector<std::size_t> matched;
    for (std::size_t h = 1; static_cast<double>(h) * f0 < ceiling; ++h)
      matched.push_back(match(peaks, static_cast<double>(h) * f0,
                              tolerance(static_cast<double>(h) * f0, bin_hz)));
    const auto level = [&](std::size_t h) {
      return h < matched.size() && matched[h] != npos ? peaks[matched[h]].residual : 0.0;
    };
    // Neighbours are judged as they were before any note was cancelled, so a
    // neighbour another note already took does not make this partial look
    // shared.
    const auto original = [&](std::size_t h) {
      return h < matched.size() && matched[h] != npos ? peaks[matched[h]].weight : 0.0;
    };
    std::vector<double> take(matched.size(), 0.0);
    for (std::size_t h = 0; h < matched.size(); ++h) {
      if (matched[h] == npos)
        continue;
      double neighbours = 0.0;
      int count = 0;
      if (h > 0) {
        neighbours += original(h - 1);
        ++count;
      }
      if (h + 1 < matched.size()) {
        neighbours += original(h + 1);
        ++count;
      }
      const double around = count > 0 ? neighbours / count : original(h);
      const double own = level(h);
      take[h] = original(h) <= shared_excess * around ? own : std::min(own, around);
    }
    for (std::size_t h = 0; h < matched.size(); ++h) {
      if (matched[h] != npos)
        peaks[matched[h]].residual = std::max(0.0, peaks[matched[h]].residual - take[h]);
    }
  }

  std::uint32_t rate_;
  std::vector<double> candidates_;
  std::size_t max_polyphony_ = 1;
  std::vector<Weighted> short_;
  std::vector<Weighted> long_;
};

// Per hop, which notes claim each peak as which harmonic.
struct Claim {
  std::size_t note = 0;
  std::size_t harmonic = 0; // 0-based
  bool low = false;         // in the long window's peaks
  std::size_t peak = npos;
};

// Assigns every note its partials, splitting a partial two notes share by
// what each note's own neighbouring partials lead one to expect.
void assign_partials(std::vector<Note>& notes, std::span<const float> mono, std::uint32_t rate,
                     std::int64_t hop, Spectra& spectra, const Context& context, bool& stopped) {
  const double ceiling = partial_ceiling * rate;
  const double phase_step = two_pi * static_cast<double>(hop) / rate;
  std::int64_t first = std::numeric_limits<std::int64_t>::max();
  std::int64_t last = 0;
  for (auto& note : notes) {
    double lowest = std::numeric_limits<double>::infinity();
    for (const float f : note.pitch)
      lowest = f > 0.0F ? std::min(lowest, static_cast<double>(f)) : lowest;
    note.partial_count =
        std::isfinite(lowest)
            ? std::clamp<std::size_t>(static_cast<std::size_t>(ceiling / lowest), 1, max_partials)
            : 1;
    note.partials.assign(note.hops() * note.partial_count, PartialFrame{});
    note.amplitude.assign(note.hops(), 0.0F);
    first = std::min(first, note.first_hop);
    last = std::max(last, note.first_hop + static_cast<std::int64_t>(note.hops()));
  }
  std::vector<Claim> claims;
  std::vector<Weighted> short_list;
  std::vector<Weighted> long_list;
  for (auto k = first; k < last; ++k) {
    if (static_cast<std::size_t>(k - first) % poll_hops == 0 && context.stopped()) {
      stopped = true;
      return;
    }
    spectra.analyse(mono, k * hop);
    short_list.clear();
    long_list.clear();
    for (const auto& p : spectra.short_peaks)
      short_list.push_back({p.bin * spectra.short_bin_hz, p.amplitude, p.amplitude});
    for (const auto& p : spectra.long_peaks)
      long_list.push_back({p.bin * spectra.long_bin_hz, p.amplitude, p.amplitude});
    claims.clear();
    for (std::size_t n = 0; n < notes.size(); ++n) {
      auto& note = notes[n];
      if (k < note.first_hop || k >= note.first_hop + static_cast<std::int64_t>(note.hops()))
        continue;
      const auto index = static_cast<std::size_t>(k - note.first_hop);
      const double f0 = note.pitch[index];
      const bool low = spectra.low(f0);
      const auto& peaks = low ? long_list : short_list;
      const double bin_hz = low ? spectra.long_bin_hz : spectra.short_bin_hz;
      for (std::size_t h = 0; h < note.partial_count; ++h) {
        const double hz = static_cast<double>(h + 1) * f0;
        auto& p = note.partials[index * note.partial_count + h];
        const auto previous =
            index > 0 ? note.partials[(index - 1) * note.partial_count + h] : PartialFrame{};
        p.frequency = static_cast<float>(hz);
        p.phase = static_cast<float>(std::remainder(previous.phase + phase_step * hz, two_pi));
        if (hz >= ceiling)
          continue;
        const auto i = nearest(peaks, hz, tolerance(hz, bin_hz));
        if (i != npos)
          claims.push_back({n, h, low, i});
      }
    }
    // Claims on one partial: the same frequency, whichever window's peak
    // each note read it from (a low note reads the long window, a high one
    // the short, and both can claim one partial).
    const auto claim_hz = [&](const Claim& c) {
      return (c.low ? long_list : short_list)[c.peak].hz;
    };
    // Reassigned peaks of one sinusoid agree to well within a bin in either
    // window, so claims are on one partial only when that close; the wider
    // harmonic tolerance would make neighbouring partials look shared.
    const auto same_partial = [&](const Claim& a, const Claim& b) {
      return std::abs(claim_hz(a) - claim_hz(b)) <= match_bins * spectra.short_bin_hz;
    };
    std::vector<int> owners(claims.size(), 0);
    for (std::size_t i = 0; i < claims.size(); ++i) {
      for (std::size_t j = 0; j < claims.size(); ++j) {
        if (claims[j].note != claims[i].note && same_partial(claims[i], claims[j]))
          ++owners[i];
      }
    }
    const auto own_level = [&](std::size_t n, std::size_t h) -> double {
      for (std::size_t i = 0; i < claims.size(); ++i) {
        const auto& c = claims[i];
        if (c.note == n && c.harmonic == h && owners[i] == 0)
          return (c.low ? long_list : short_list)[c.peak].weight;
      }
      return -1.0;
    };
    // Expected level of each shared claim from the note's unshared
    // neighbouring harmonics.
    std::vector<double> expected(claims.size(), 1.0);
    for (std::size_t i = 0; i < claims.size(); ++i) {
      const auto& c = claims[i];
      if (owners[i] == 0)
        continue;
      double sum = 0.0;
      int count = 0;
      for (const auto h : {c.harmonic > 0 ? c.harmonic - 1 : npos, c.harmonic + 1}) {
        if (h == npos)
          continue;
        const double level = own_level(c.note, h);
        if (level >= 0.0) {
          sum += level;
          ++count;
        }
      }
      expected[i] = count > 0 ? sum / count : 0.0;
    }
    for (std::size_t i = 0; i < claims.size(); ++i) {
      const auto& c = claims[i];
      const auto& peaks = c.low ? long_list : short_list;
      const auto& source = c.low ? spectra.long_peaks : spectra.short_peaks;
      double share = 1.0;
      if (owners[i] > 0) {
        double sum = expected[i];
        for (std::size_t j = 0; j < claims.size(); ++j) {
          if (claims[j].note != c.note && same_partial(c, claims[j]))
            sum += expected[j];
        }
        // With nothing to go on, the claimants share it equally.
        share = sum > 0.0 ? expected[i] / sum : 1.0 / static_cast<double>(owners[i] + 1);
      }
      auto& note = notes[c.note];
      const auto index = static_cast<std::size_t>(k - note.first_hop);
      auto& p = note.partials[index * note.partial_count + c.harmonic];
      p.frequency = static_cast<float>(peaks[c.peak].hz);
      p.amplitude = static_cast<float>(peaks[c.peak].weight * share);
      p.phase = static_cast<float>(source[c.peak].phase);
      note.amplitude[index] += p.amplitude;
    }
  }
}

// Notes far quieter than a note sounding with them or just before them: the
// dampers' fall, sympathetic strings, a release's tail.
std::vector<bool> tails(const std::vector<Note>& notes, std::int64_t window) {
  std::vector<float> loudest(notes.size(), 0.0F);
  for (std::size_t n = 0; n < notes.size(); ++n)
    for (const float a : notes[n].amplitude)
      loudest[n] = std::max(loudest[n], a);
  std::vector<bool> tail(notes.size(), false);
  for (std::size_t x = 0; x < notes.size(); ++x) {
    for (std::size_t y = 0; y < notes.size(); ++y) {
      const bool near = notes[y].start < notes[x].end && notes[x].start < notes[y].end + window;
      if (x != y && near && static_cast<double>(loudest[x]) < db_to_ratio(tail_db) * loudest[y])
        tail[x] = true;
    }
  }
  return tail;
}

} // namespace

void envelope_from_partials(Note& note) {
  const auto hops = note.hops();
  const auto rows = (hops + envelope_stride - 1) / envelope_stride;
  note.envelope.assign(rows * envelope_bands, 0.0F);
  std::vector<std::pair<double, double>> points;
  for (std::size_t r = 0; r < rows; ++r) {
    const auto k = r * envelope_stride;
    points.clear();
    for (std::size_t h = 0; h < note.partial_count; ++h) {
      const auto& p = note.partials[k * note.partial_count + h];
      if (p.amplitude > 0.0F && p.frequency > 0.0F)
        points.emplace_back(envelope_band_of(p.frequency),
                            std::log(static_cast<double>(p.amplitude)));
    }
    std::ranges::sort(points);
    for (std::size_t b = 0; b < envelope_bands; ++b) {
      double value = 0.0;
      const auto band = static_cast<double>(b);
      if (points.empty()) {
        value = 0.0;
      } else if (band <= points.front().first) {
        value = points.front().second;
      } else if (band >= points.back().first) {
        value = points.back().second;
      } else {
        const auto above =
            std::ranges::lower_bound(points, band, {}, &std::pair<double, double>::first);
        const auto below = above - 1;
        const double t = (band - below->first) / std::max(1e-9, above->first - below->first);
        value = (1.0 - t) * below->second + t * above->second;
      }
      note.envelope[r * envelope_bands + b] = static_cast<float>(value);
    }
  }
}

bool detect_polyphonic(const Context& context, std::int64_t hop, std::vector<Note>& notes) {
  const auto rate = context.sample_rate;
  const auto mono = context.mono;
  const auto length = static_cast<std::int64_t>(mono.size());
  const auto hop_count = static_cast<std::size_t>(length / hop + 1);
  Spectra spectra(rate);
  Estimator estimator(*context.options, rate);
  NoteSets sets(rate, context.options->minimum_hz, context.options->maximum_hz);

  // Every hop: its peaks (the long window's below merged_below_hz, the short
  // one's above) and the note set that best explains it on its own.
  std::vector<std::vector<SpectralPeak>> frames(hop_count);
  std::vector<std::vector<NoteChoice>> picked(hop_count);
  std::vector<Estimate> estimates;
  std::vector<double> seeds;
  for (std::size_t k = 0; k < hop_count; ++k) {
    if (k % poll_hops == 0 && context.stopped())
      return false;
    spectra.analyse(mono, static_cast<std::int64_t>(k) * hop);
    estimator.estimate(spectra, estimates);
    auto& merged = frames[k];
    for (const auto& p : spectra.long_peaks) {
      if (p.bin * spectra.long_bin_hz < merged_below_hz)
        merged.push_back({p.bin * spectra.long_bin_hz, p.amplitude});
    }
    for (const auto& p : spectra.short_peaks) {
      if (p.bin * spectra.short_bin_hz >= merged_below_hz)
        merged.push_back({p.bin * spectra.short_bin_hz, p.amplitude});
    }
    seeds.clear();
    for (const auto& e : estimates)
      seeds.push_back(e.f0);
    sets.choose(merged, seeds, {}, picked[k]);
  }

  // Segments run from onset to onset. Each one's notes are chosen once, by a
  // vote over its settled hops (after the long window has passed the
  // attack), so the attack's smear and one hop's doubt cannot add or break
  // notes.
  std::vector<std::int64_t> onsets{0};
  warp_dsp::MemorySourceView view(mono, mono);
  warp_dsp::analysis::TransientOptions onset_options;
  onset_options.sample_rate = rate;
  if (auto found = warp_dsp::analysis::detect_transients(view, onset_options, context.cancelled)) {
    for (const auto& t : found.value())
      onsets.push_back(static_cast<std::int64_t>(t.frame));
  } else if (context.stopped()) {
    return false;
  }
  std::ranges::sort(onsets);
  onsets.erase(std::unique(onsets.begin(), onsets.end(),
                           [&](std::int64_t a, std::int64_t b) { return b / hop == a / hop; }),
               onsets.end());
  // Settled: the long window has left the attack and its thump.
  const auto settle = static_cast<std::size_t>(
      std::ceil(static_cast<double>(spectra.long_window.size()) / static_cast<double>(hop)));
  const auto votes =
      static_cast<std::size_t>(std::max(1.0, vote_seconds * rate / static_cast<double>(hop)));
  const auto quiet_limit =
      static_cast<std::size_t>(std::max(1.0, quiet_seconds * rate / static_cast<double>(hop)));
  const auto same = [](double a, double b) {
    return std::abs(cents_per_octave * std::log2(a / b)) < track_cents;
  };

  struct Sounding {
    double pitch = 0.0;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::size_t first_hop = 0;
    std::vector<float> f0;
    double best = 0.0;
    std::size_t quiet = 0;
    std::size_t heard = 0; // hops it was heard in
  };
  std::vector<Sounding> active;
  std::vector<Sounding> finished;
  const auto finish = [&](std::size_t i, std::int64_t end) {
    auto note = std::move(active[i]);
    note.end = end;
    finished.push_back(std::move(note));
    active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
  };
  const auto begin = [&](double pitch, std::int64_t start, std::size_t first_hop) {
    Sounding note;
    note.pitch = pitch;
    note.start = start;
    note.first_hop = first_hop;
    active.push_back(std::move(note));
  };
  struct Cluster {
    std::vector<double> pitches;
    std::size_t first = 0;
  };
  std::vector<Cluster> entering;
  for (std::size_t s = 0; s < onsets.size(); ++s) {
    const auto onset = onsets[s];
    const auto from = static_cast<std::size_t>(onset / hop);
    const auto to =
        s + 1 < onsets.size() ? static_cast<std::size_t>(onsets[s + 1] / hop) : hop_count;
    if (from >= to)
      continue;
    // The vote.
    auto a = std::min(from + settle, to - 1);
    const auto b = std::min(a + votes, to);
    std::vector<Cluster> clusters;
    for (auto k = a; k < b; ++k) {
      for (const auto& c : picked[k]) {
        auto it = std::ranges::find_if(
            clusters, [&](const Cluster& x) { return same(x.pitches.front(), c.f0); });
        if (it == clusters.end()) {
          clusters.push_back({{c.f0}, k});
        } else {
          it->pitches.push_back(c.f0);
        }
      }
    }
    std::vector<double> voted;
    for (auto& c : clusters) {
      if (static_cast<double>(c.pitches.size()) >= vote_share * static_cast<double>(b - a)) {
        auto middle = c.pitches.begin() + static_cast<std::ptrdiff_t>(c.pitches.size() / 2);
        std::nth_element(c.pitches.begin(), middle, c.pitches.end());
        voted.push_back(*middle);
      }
    }
    // Notes not voted for stop at the onset; new ones start there.
    for (std::size_t i = active.size(); i-- > 0;) {
      if (std::ranges::none_of(voted, [&](double v) { return same(v, active[i].pitch); }))
        finish(i, onset);
    }
    for (const double v : voted) {
      if (std::ranges::none_of(active, [&](const Sounding& n) { return same(n.pitch, v); }))
        begin(v, onset, from);
    }
    entering.clear();
    // Through the segment: each note carries on while its partial series
    // still covers energy; a note that enters without a transient (a soft
    // entry) starts once it has been the frame's choice long enough.
    for (auto k = from; k < to; ++k) {
      for (std::size_t i = active.size(); i-- > 0;) {
        auto& note = active[i];
        const auto m = sets.measure(frames[k], note.pitch);
        const bool heard = m.covered > end_ratio * note.best && m.covered > 0.0;
        note.f0.resize(k - note.first_hop + 1, static_cast<float>(note.pitch));
        if (heard) {
          note.f0.back() = static_cast<float>(same(m.f0, note.pitch) ? m.f0 : note.pitch);
          note.best = std::max(note.best, m.covered);
          ++note.heard;
          note.quiet = 0;
        } else if (++note.quiet >= quiet_limit) {
          note.f0.resize(note.f0.size() - note.quiet);
          finish(i, static_cast<std::int64_t>(k + 1 - note.quiet) * hop);
        }
      }
      if (k < a)
        continue;
      for (const auto& c : picked[k]) {
        if (std::ranges::any_of(active, [&](const Sounding& n) { return same(n.pitch, c.f0); }))
          continue;
        auto it = std::ranges::find_if(
            entering, [&](const Cluster& x) { return same(x.pitches.front(), c.f0); });
        if (it == entering.end()) {
          entering.push_back({{c.f0}, k});
          continue;
        }
        // An entry must add energy the sounding notes do not explain (a
        // subharmonic of them adds none).
        std::vector<double> sounding;
        for (const auto& n : active)
          sounding.push_back(n.pitch);
        if (sets.fresh(frames[k], c.f0, sounding) < entry_share)
          continue;
        it->pitches.push_back(c.f0);
        if (it->pitches.size() >= votes) {
          begin(it->pitches.front(), static_cast<std::int64_t>(it->first) * hop, it->first);
          auto& note = active.back();
          note.f0.assign(k - it->first, static_cast<float>(note.pitch));
          entering.erase(it);
        }
      }
      // An entry must be chosen hop after hop.
      std::erase_if(entering, [&](const Cluster& x) { return x.first + x.pitches.size() <= k; });
    }
  }
  while (!active.empty())
    finish(active.size() - 1, length);

  const auto minimum = static_cast<std::int64_t>(context.options->minimum_note_seconds * rate);
  for (auto& s : finished) {
    s.end = std::min(s.end, length);
    if (s.end - s.start < minimum || s.f0.empty() || s.heard == 0)
      continue;
    Note note;
    note.kind = NoteKind::sinusoidal;
    note.first_hop = static_cast<std::int64_t>(s.first_hop);
    note.start = std::clamp<std::int64_t>(s.start, 0, length);
    note.end = s.end;
    note.pitch = std::move(s.f0);
    notes.push_back(std::move(note));
  }
  bool stopped = false;
  assign_partials(notes, mono, rate, hop, spectra, context, stopped);
  if (stopped)
    return false;
  // Tails: the dampers' fall and the strings that ring on are far quieter
  // than the notes they follow.
  const auto quiet = tails(notes, static_cast<std::int64_t>(tail_window_seconds * rate));
  if (std::ranges::find(quiet, true) != quiet.end()) {
    std::vector<Note> kept;
    for (std::size_t n = 0; n < notes.size(); ++n) {
      if (!quiet[n]) {
        kept.push_back(std::move(notes[n]));
      }
    }
    notes = std::move(kept);
    assign_partials(notes, mono, rate, hop, spectra, context, stopped);
    if (stopped)
      return false;
  }
  for (auto& note : notes)
    envelope_from_partials(note);
  std::ranges::sort(notes, {}, &Note::start);
  return true;
}

} // namespace oscillate::clay_dsp::detail
