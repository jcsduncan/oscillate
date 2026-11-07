#include "note_sets.hpp"

#include "detect.hpp"

#include "oscillate/clay_dsp/take.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace oscillate::clay_dsp::detail {
namespace {

// Peaks this far under the frame's loudest are noise.
constexpr double noise_floor_db = -60.0;
// A partial is matched within this many cents of its place on the series
// (or this many Hz, for low partials).
constexpr double fit_cents = 25.0;
constexpr double min_fit_hz = 3.0;
constexpr std::size_t max_harmonics = 30;
// The series' stretch, s in f_h = h f0 (1 + s h^2), stays within this; it is
// fitted from partials from this harmonic up.
constexpr double max_stretch = 5e-4;
constexpr std::size_t stretch_from = 3;
// The pitch is refined from the partials up to this harmonic.
constexpr std::size_t refine_up_to = 6;
// The first and last partials, with one neighbour each, are expected at this
// share of it: piano and bass fundamentals are often far under the second.
// A fundamental with no peak at all is expected at the second partial's
// level: a real note sounds some fundamental, a subharmonic made of higher
// notes' partials none.
constexpr double edge_share = 0.25;
constexpr double absent_share = 0.5;
// Costs, as shares of the frame's energy: one per note, less for a note
// that sounded last frame; missing partials weigh this much.
constexpr double new_note_cost = 0.04;
constexpr double held_note_cost = 0.01;
constexpr double missing_weight = 1.0;
// Subharmonics of the picks are tried as candidates.
constexpr std::array<double, 2> subharmonics{2.0, 3.0};
// Candidates within this many cents are one; a note within this many of a
// last-frame note is that note.
constexpr double same_cents = 30.0;
constexpr double held_cents = 50.0;
constexpr int max_moves = 32;
// A note absorbs the notes whose peaks' energy it covers this much of.
constexpr double absorb_share = 0.8;

[[nodiscard]] double cents_between(double a, double b) noexcept {
  return std::abs(cents_per_octave * std::log2(a / b));
}

} // namespace

NoteSets::NoteSets(std::uint32_t rate, const AnalysisOptions& options)
    : rate_(rate), minimum_hz_(options.minimum_hz), maximum_hz_(options.maximum_hz) {}

void NoteSets::evaluate(double f0, Candidate& out) const {
  const double ceiling = partial_ceiling * rate_;
  for (int pass = 0; pass < 2; ++pass) {
    out = Candidate{};
    out.f0 = f0;
    std::array<double, max_harmonics + 1> level{};
    std::array<std::size_t, max_harmonics + 1> peak{};
    double stretch = 0.0;
    double stretch_sum = 0.0;
    int stretch_count = 0;
    std::size_t top = 0;
    for (std::size_t h = 1; h <= max_harmonics; ++h) {
      const double n = static_cast<double>(h);
      const double predicted = n * f0 * (1.0 + stretch * n * n);
      if (predicted >= ceiling)
        break;
      top = h;
      const double tolerance =
          std::max(min_fit_hz, predicted * (std::exp2(fit_cents / cents_per_octave) - 1.0));
      const auto lo = std::ranges::lower_bound(hz_, predicted - tolerance);
      std::size_t best = max_peaks;
      for (auto it = lo; it != hz_.end() && *it <= predicted + tolerance; ++it) {
        const auto i = static_cast<std::size_t>(it - hz_.begin());
        if (best == max_peaks || std::abs(*it - predicted) < std::abs(hz_[best] - predicted))
          best = i;
      }
      peak[h] = best;
      if (best == max_peaks)
        continue;
      level[h] = weight_[best];
      if ((out.cover[best / 64] & (std::uint64_t{1} << (best % 64))) == 0) {
        out.cover[best / 64] |= std::uint64_t{1} << (best % 64);
        out.covered += weight_[best] * weight_[best];
      }
      if (h >= stretch_from) {
        stretch_sum += (hz_[best] / (n * f0) - 1.0) / (n * n);
        ++stretch_count;
        stretch = std::clamp(stretch_sum / stretch_count, 0.0, max_stretch);
      }
    }
    // Missing partials: each partial is expected at the mean of its
    // neighbours' levels (a quarter of the one neighbour at either end, since
    // weak fundamentals are common). A subharmonic, whose every other
    // partial is empty, pays for each gap.
    for (std::size_t h = 1; h <= top; ++h) {
      const double expected = h == 1 ? (peak[1] == max_peaks ? absent_share : edge_share) * level[2]
                              : h == top ? edge_share * level[h - 1]
                                         : 0.5 * (level[h - 1] + level[h + 1]);
      const double gap = std::max(0.0, expected - level[h]);
      out.missing += missing_weight * gap * gap;
    }
    if (pass == 1)
      break;
    // Refine the pitch from the low partials, then evaluate again.
    double sum = 0.0;
    double total = 0.0;
    for (std::size_t h = 1; h <= std::min(top, refine_up_to); ++h) {
      if (peak[h] == max_peaks)
        continue;
      const double n = static_cast<double>(h);
      sum += level[h] * hz_[peak[h]] / (n * (1.0 + stretch * n * n));
      total += level[h];
    }
    if (total <= 0.0 || cents_between(sum / total, f0) > same_cents)
      break;
    f0 = sum / total;
  }
}

double NoteSets::overlap_share(const Candidate& inner, const Candidate& outer) const {
  double inside = 0.0;
  for (std::size_t i = 0; i < hz_.size(); ++i) {
    const auto bit = std::uint64_t{1} << (i % 64);
    if ((inner.cover[i / 64] & bit) != 0 && (outer.cover[i / 64] & bit) != 0)
      inside += weight_[i] * weight_[i];
  }
  return inner.covered > 0.0 ? inside / inner.covered : 0.0;
}

double NoteSets::score(std::span<const std::size_t> set) const {
  Cover cover{};
  double penalty = 0.0;
  for (const auto c : set) {
    for (std::size_t w = 0; w < cover.size(); ++w)
      cover[w] |= candidates_[c].cover[w];
    penalty += candidates_[c].missing + candidates_[c].cost;
  }
  double uncovered = 0.0;
  for (std::size_t i = 0; i < hz_.size(); ++i) {
    if ((cover[i / 64] & (std::uint64_t{1} << (i % 64))) == 0)
      uncovered += weight_[i] * weight_[i];
  }
  return uncovered + penalty;
}

bool NoteSets::load(std::span<const SpectralPeak> peaks) {
  hz_.clear();
  weight_.clear();
  energy_ = 0.0;
  double loudest = 0.0;
  for (const auto& p : peaks)
    loudest = std::max(loudest, p.amplitude);
  if (loudest <= 0.0)
    return false;
  // The loudest peaks, kept in frequency order. Weights are square roots of
  // amplitude, so loud low partials do not drown the rest.
  std::vector<SpectralPeak> kept;
  for (const auto& p : peaks) {
    if (p.amplitude >= loudest * db_to_ratio(noise_floor_db))
      kept.push_back(p);
  }
  if (kept.size() > max_peaks) {
    std::ranges::nth_element(kept, kept.begin() + static_cast<std::ptrdiff_t>(max_peaks),
                             std::greater{}, &SpectralPeak::amplitude);
    kept.resize(max_peaks);
    std::ranges::sort(kept, {}, &SpectralPeak::hz);
  }
  for (const auto& p : kept) {
    hz_.push_back(p.hz);
    weight_.push_back(std::sqrt(p.amplitude));
    energy_ += p.amplitude;
  }
  return true;
}

NoteChoice NoteSets::measure(std::span<const SpectralPeak> peaks, double f0) {
  if (!load(peaks))
    return {f0, 0.0};
  Candidate c;
  evaluate(f0, c);
  return {c.f0, c.covered / std::max(energy_, 1e-30), c.covered};
}

double NoteSets::fresh(std::span<const SpectralPeak> peaks, double f0,
                       std::span<const double> sounding) {
  if (!load(peaks))
    return 0.0;
  Candidate note;
  evaluate(f0, note);
  Cover others{};
  for (const double pitch : sounding) {
    Candidate other;
    evaluate(pitch, other);
    for (std::size_t w = 0; w < others.size(); ++w)
      others[w] |= other.cover[w];
  }
  double added = 0.0;
  for (std::size_t i = 0; i < hz_.size(); ++i) {
    const auto bit = std::uint64_t{1} << (i % 64);
    if ((note.cover[i / 64] & bit) != 0 && (others[i / 64] & bit) == 0)
      added += weight_[i] * weight_[i];
  }
  return added / std::max(energy_, 1e-30);
}

void NoteSets::choose(std::span<const SpectralPeak> peaks, Hints hints,
                      std::vector<NoteChoice>& out) {
  const auto seeds = hints.seeds;
  const auto previous = hints.previous;
  out.clear();
  if (!load(peaks))
    return;
  // Candidates: the picks, their subharmonics and last frame's notes.
  candidates_.clear();
  const auto add = [&](double f0) {
    if (f0 < minimum_hz_ || f0 > maximum_hz_)
      return;
    for (const auto& c : candidates_) {
      if (cents_between(c.f0, f0) < same_cents)
        return;
    }
    Candidate c;
    evaluate(f0, c);
    if (c.covered <= 0.0)
      return;
    const bool held = std::ranges::any_of(
        previous, [&](double p) { return cents_between(p, c.f0) < held_cents; });
    c.cost = (held ? held_note_cost : new_note_cost) * energy_;
    candidates_.push_back(c);
  };
  for (const double f0 : seeds)
    add(f0);
  const auto picks = candidates_.size();
  for (const double f0 : previous)
    add(f0);
  for (std::size_t i = 0; i < picks; ++i)
    for (const double d : subharmonics)
      add(candidates_[i].f0 / d);
  // Local search from the picks.
  std::vector<std::size_t> set;
  set.reserve(picks);
  for (std::size_t i = 0; i < picks; ++i)
    set.push_back(i);
  double current = score(set);
  std::vector<std::size_t> trial;
  for (int move = 0; move < max_moves; ++move) {
    double best = current;
    std::vector<std::size_t> best_set;
    const auto consider = [&]() {
      const double s = score(trial);
      if (s < best - 1e-12 * energy_) {
        best = s;
        best_set = trial;
      }
    };
    for (std::size_t i = 0; i < set.size(); ++i) {
      trial = set;
      trial.erase(trial.begin() + static_cast<std::ptrdiff_t>(i));
      consider();
    }
    for (std::size_t c = 0; c < candidates_.size(); ++c) {
      if (std::ranges::find(set, c) != set.end())
        continue;
      trial = set;
      trial.push_back(c);
      consider();
      // Absorb: one note in place of every note whose peaks it mostly covers
      // (a weak-fundamental G3 for the D5 and G4 made of its partials).
      trial.clear();
      for (const auto m : set) {
        if (overlap_share(candidates_[m], candidates_[c]) < absorb_share)
          trial.push_back(m);
      }
      if (trial.size() + 1 < set.size()) {
        trial.push_back(c);
        consider();
      }
      for (std::size_t i = 0; i < set.size(); ++i) {
        trial = set;
        trial[i] = c;
        consider();
      }
    }
    if (best_set.empty() && best >= current)
      break;
    set = std::move(best_set);
    current = best;
  }
  for (const auto c : set)
    out.push_back({candidates_[c].f0, candidates_[c].covered / std::max(energy_, 1e-30),
                   candidates_[c].covered});
  std::ranges::sort(out, std::greater{}, &NoteChoice::share);
}

} // namespace oscillate::clay_dsp::detail
