#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::clay_dsp::detail {

// A spectral peak: frequency and sinusoid amplitude.
struct SpectralPeak {
  double hz = 0.0;
  double amplitude = 0.0;
};

// A note chosen for a frame: its pitch and the share of the frame's
// (whitened) energy its partials cover.
struct NoteChoice {
  double f0 = 0.0;
  double share = 0.0;
  double covered = 0.0; // the covered energy itself
};

// Joint choice of the notes sounding in one frame: the set that best
// explains the frame's peaks, scored as
//   peaks no chosen note covers
//   + each note's missing partials (where its own spectral envelope, a
//     running median of its partial levels, expects energy and finds none)
//   + a cost per note (lower for a note that sounded in the last frame).
// Each candidate's partial levels are free, so a weak fundamental costs a
// little and a note explaining another hypothesis's peaks wins by needing
// fewer notes. Partials are matched along a stretched series
// f_h = h f0 (1 + s h^2), s fitted as the series is walked, so stiff strings
// fit. The set is found by local search (add, remove, swap) from the
// iterative estimator's picks, over those picks, their subharmonics and the
// last frame's notes.
class NoteSets {
public:
  NoteSets(std::uint32_t rate, double minimum_hz, double maximum_hz);

  // One pitch on its own: its refined pitch and the share of the frame's
  // energy its partial series covers (0 without a fundamental's series).
  [[nodiscard]] NoteChoice measure(std::span<const SpectralPeak> peaks, double f0);

  // The share of the frame's energy `f0`'s series covers that none of
  // `sounding`'s series do: what a new note would add.
  [[nodiscard]] double fresh(std::span<const SpectralPeak> peaks, double f0,
                             std::span<const double> sounding);

  // `peaks` sorted by frequency.
  void choose(std::span<const SpectralPeak> peaks, std::span<const double> seeds,
              std::span<const double> previous, std::vector<NoteChoice>& out);

private:
  static constexpr std::size_t max_peaks = 256;
  using Cover = std::array<std::uint64_t, max_peaks / 64>;

  struct Candidate {
    double f0 = 0.0;
    Cover cover{};
    double covered = 0.0; // energy of the peaks it covers
    double missing = 0.0;
    double cost = 0.0;
  };

  // Keeps the frame's peaks above the noise; false for a silent frame.
  bool load(std::span<const SpectralPeak> peaks);
  void evaluate(double f0, Candidate& out) const;
  [[nodiscard]] double score(std::span<const std::size_t> set) const;
  // The share of `inner`'s covered energy that `outer` covers too.
  [[nodiscard]] double overlap_share(const Candidate& inner, const Candidate& outer) const;

  std::uint32_t rate_;
  double minimum_hz_;
  double maximum_hz_;
  std::vector<double> hz_;
  std::vector<double> weight_;
  double energy_ = 0.0;
  std::vector<Candidate> candidates_;
};

} // namespace oscillate::clay_dsp::detail
