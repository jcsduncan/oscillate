#pragma once

#include "dsp/bs1770.hpp"
#include "edge_dsp.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Ceiling's DSP (ADR-0117): a true-peak-aware lookahead gain stage with a
// fast peak envelope that releases to a slow loudness envelope, onset room
// for transients, an oversampled soft clipper that rounds what the gain
// stage lets past, and the true-peak guard. Everything runs under a ceiling
// of 1: the kernel scales its signal by the ceiling around the clipper and
// guard.
namespace oscillate::native_dsp::effects::ceiling_dsp {

// ---- Building blocks -----------------------------------------------------------------------

// The least of the last `length` values pushed: a monotonic queue.
class MinHold {
public:
  // Control only.
  void prepare(std::size_t length);
  void reset() noexcept;
  [[nodiscard]] double push(double value) noexcept;

private:
  struct Entry {
    std::uint64_t frame = 0;
    double value = 0.0;
  };
  std::vector<Entry> queue_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::uint64_t frame_ = 0;
  std::size_t length_ = 1;
};

// The mean of the last `length` values pushed, re-summed exactly once per
// lap so rounding cannot drift.
class Boxcar {
public:
  // Control only.
  void prepare(std::size_t length);
  void reset(double value) noexcept;
  [[nodiscard]] double push(double value) noexcept;

private:
  std::vector<double> values_;
  std::size_t position_ = 0;
  double sum_ = 0.0;
};

// A fixed delay of whole frames for one stereo stream.
class FrameDelay {
public:
  // Control only.
  void prepare(std::size_t frames);
  void reset() noexcept;
  // Takes a frame, returns the one `frames` earlier.
  [[nodiscard]] std::array<double, 2> push(std::array<double, 2> frame) noexcept;

private:
  std::vector<std::array<double, 2>> line_;
  std::size_t position_ = 0;
};

// One channel's 8x intersample peak estimate: seven fractional phases of a
// 33-tap Kaiser-windowed sinc, each normalised to unity at DC. Like
// bs1770::TruePeak it answers `radius` samples late with the largest of the
// points between the samples `radius` and `radius + 1` back, but reads
// closer to the continuous peak near Nyquist, so steady high tones stay out
// of the clipper's knee. Allocation-free.
class FinePeak {
public:
  static constexpr std::size_t radius = native_dsp::dsp::bs1770::TruePeak::radius;
  static constexpr std::size_t taps = 2 * radius + 1;
  static constexpr std::size_t factor = 8;

  FinePeak() noexcept;
  void reset() noexcept;
  [[nodiscard]] double push(double x) noexcept;

private:
  std::array<std::array<double, taps>, factor - 1> kernels_{};
  std::array<double, 2 * taps> history_{};
  std::size_t write_ = 0;
};

// ---- The gain stage ------------------------------------------------------------------------

// What the controls ask of the gain stage, prepared on control.
struct Shape {
  // Where sustained material sits (natural-log gain, <= 0): under the
  // clipper's knee, so the knee only ever bends what the rooms let past.
  double sustain = 0.0;
  // The onset room at full Transients, in natural-log gain, and the
  // clipper's room (Clip), likewise; both >= 0.
  double onset_room = 0.0;
  double clip_room = 0.0;
  double transients = 0.0;
  // Per-frame release coefficients: the fast envelope's toward the slow
  // one, and the slow envelope's attack and release.
  double fast_release = 0.0;
  double slow_attack = 0.0;
  double slow_release = 0.0;
  // How much of the slow envelope the fast one releases to (Character).
  double character = 0.0;
  // Stereo link of the slow (sustain) and fast (transient) envelopes.
  double link = 1.0;
  double transient_link = 1.0;
};

// Per frame, from each channel's 8x peak (FinePeak): the gain (natural log, <= 0)
// to apply to the frame `delay()` earlier, such that its interpolated peak
// sits at or under the ceiling times its room. The room is the sustain
// level plus the clip room plus, while an onset is detected, the onset room
// times Transients.
class GainStage {
public:
  // Control only: `attack` frames of lookahead and averaging, `hold` frames
  // held after a peak before the release starts.
  void prepare(std::size_t attack, std::size_t hold, double sample_rate);
  void reset() noexcept;
  void set(const Shape& shape) noexcept { shape_ = shape; }
  // Takes the newest frame; returns the log gain for each channel of the
  // frame delay() earlier.
  [[nodiscard]] std::array<double, 2> process(double left, double right) noexcept;
  [[nodiscard]] std::size_t delay() const noexcept { return delay_; }
  // The onset detector's reading for the newest frame, 0 to 1.
  [[nodiscard]] double onset() const noexcept { return onset_; }

  static constexpr std::size_t peak_radius = FinePeak::radius;

private:
  [[nodiscard]] double detect_onset(double left, double right) noexcept;

  Shape shape_{};
  std::array<FinePeak, 2> peaks_{};
  // Each channel's magnitude over the last radius + 1 frames, newest first.
  std::array<edge_dsp::Ring, 2> magnitudes_{};
  std::array<MinHold, 2> holds_{};
  std::array<double, 2> slow_{};
  std::array<double, 2> fast_{};
  std::array<Boxcar, 2> first_{};
  std::array<Boxcar, 2> second_{};
  // The onset detector: fast- and slow-attack followers of the power.
  double fast_power_ = 0.0;
  double slow_power_ = 0.0;
  double fast_keep_ = 0.0;
  double slow_keep_ = 0.0;
  double release_keep_ = 0.0;
  double onset_ = 0.0;
  std::size_t delay_ = 0;
};

// ---- The clipper ---------------------------------------------------------------------------

// The oversampled soft clipper at a ceiling of 1: Edge's curve and
// half-band oversampler (ADR-0108) at clip_oversampling, linear phase.
// With no room to clip it is a plain delay of the same length, so the signal
// passes exactly and the oversampler sleeps; it wakes when the room opens,
// fills its filters while the delay still plays, then cross-fades to the
// oversampled path (and back when the room closes).
class Clipper {
public:
  // Control only.
  void prepare(double sample_rate);
  void reset() noexcept;
  // The knee (Edge's, as a fraction of the ceiling; 0 is a flat top), and
  // whether there is room to clip at all. On `jump` (before the first
  // block) the path is taken at once.
  void set(double knee, bool active, bool jump) noexcept;
  // In place, one frame: returns the most the curve took off (linear, >= 1
  // as a ratio of input to output magnitude at the top rate).
  double process(double& left, double& right) noexcept;
  [[nodiscard]] std::uint32_t latency() const noexcept;

private:
  const edge_dsp::Design* design_ = nullptr;
  std::array<edge_dsp::Resampler, 2> resamplers_{};
  std::array<edge_dsp::Antialiased, 2> antialiased_{};
  edge_dsp::Curve curve_{};
  FrameDelay delay_;
  bool active_ = false;
  bool running_ = false;
  // Frames the woken oversampler still needs before its output is valid.
  std::uint32_t warming_ = 0;
  // 0: the delay; 1: the oversampled path.
  double mix_ = 0.0;
  double fade_step_ = 1.0;
};

// The oversampling index (edge_table::Oversampling) the clipper runs at.
inline constexpr std::size_t clip_oversampling = 3; // 8x, with antiderivative antialiasing

} // namespace oscillate::native_dsp::effects::ceiling_dsp
