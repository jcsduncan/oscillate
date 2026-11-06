#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Nova's wavetables and noises (ADR-0057, spec 08 §3 and §12): band-limited
// tables built on control from user audio or generated in-house, and the
// built-in noise library. Everything here is immutable once built; the
// callback only reads.
namespace oscillate::native_dsp::synth::nova {

// A table frame is one cycle of table_frame_size samples at the top level.
inline constexpr std::size_t table_frame_size = 2'048;
inline constexpr std::size_t max_table_frames = 256;
// Level 0 keeps every harmonic a 2048-sample frame can hold (1..1024); each
// further level halves the top harmonic, down to the fundamental alone.
inline constexpr std::size_t table_levels = 11;
// Samples stored before and after every frame of every level, wrapped from
// the other end of the cycle, so a 4-point read at any phase never wraps.
inline constexpr std::size_t table_guard = 2;

// How a reader moves between neighbouring frames.
enum class FrameInterpolation : std::uint8_t {
  crossfade, // Linear crossfade of the two frames' samples.
  spectral,  // Per-harmonic magnitude and phase morph, precomputed.
};

// One band limit of every frame. Frame f starts at
// data[f * stride + table_guard], holds `length` samples (a power of two,
// at most table_frame_size and at least 256) and has table_guard wrapped
// samples on each side.
struct TableLevel {
  std::uint32_t length = 0;
  std::uint32_t harmonics = 0; // Highest harmonic kept.
  std::size_t stride = 0;      // length + 2 * table_guard.
  std::vector<float> data;
};

// A band-limited wavetable. A frame's level-0 cycle has no DC and peaks at
// 1.0 across the whole table (one gain for all frames, so morphs keep their
// relative loudness).
class Wavetable {
public:
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::size_t frames() const noexcept { return frames_; }
  [[nodiscard]] const TableLevel& level(std::size_t index) const noexcept { return levels_[index]; }
  // The level for a read advancing `cycles_per_sample` (the playback
  // frequency over the sample rate): the richest level whose top harmonic
  // stays below 0.45 of the sample rate. Callback safe.
  [[nodiscard]] std::size_t level_for(double cycles_per_sample) const noexcept;
  // A frame of a level, starting at its first real sample (the guard
  // samples sit at [-table_guard, 0) and [length, length + table_guard)).
  [[nodiscard]] const float* frame(std::size_t level, std::size_t index) const noexcept {
    const auto& l = levels_[level];
    return l.data.data() + index * l.stride + table_guard;
  }
  // Spectral variant (FrameInterpolation::spectral): the same table with
  // max_table_frames frames, morphed per harmonic between the source frames.
  // Null for tables of one frame (they need no morph).
  [[nodiscard]] const Wavetable* spectral() const noexcept { return spectral_.get(); }

  // Builders only.
  Wavetable(std::string name, std::size_t frames, std::vector<TableLevel> levels,
            std::unique_ptr<const Wavetable> spectral)
      : name_(std::move(name)), frames_(frames), levels_(std::move(levels)),
        spectral_(std::move(spectral)) {}

private:
  std::string name_;
  std::size_t frames_ = 0;
  std::vector<TableLevel> levels_; // table_levels entries.
  std::unique_ptr<const Wavetable> spectral_;
};

// Builds a table from mono audio cut into frames of `frame_length` samples
// (0: the whole audio is one frame). Each frame is resampled to
// table_frame_size through its spectrum, then DC is removed, the table is
// normalised and its levels are band-limited by zeroing harmonics. Audio
// that does not fill a whole frame is dropped; more than max_table_frames
// frames are thinned evenly to max_table_frames. Control only; may take a
// few hundred milliseconds for a full table.
[[nodiscard]] foundation::Result<std::shared_ptr<const Wavetable>>
build_wavetable(std::span<const float> mono, std::size_t frame_length, std::string name);

// Builds a table from frames given as harmonic spectra: frame f's harmonic
// h (1-based, up to 1024) has magnitude magnitudes[f][h - 1] and phase
// phases[f][h - 1] in radians, so a frame is sum m sin(2 pi h x + phase):
// phase 0 is a sine starting at zero. Used by the in-house generators.
[[nodiscard]] foundation::Result<std::shared_ptr<const Wavetable>>
build_wavetable_from_spectra(std::span<const std::vector<float>> magnitudes,
                             std::span<const std::vector<float>> phases, std::string name);

// The factory tables, generated in-house (no third-party content): at least
// the contract's names (basic_shapes, glass_formant, reese_saw, vowel_morph,
// fm_bell, grit_fold, spectral_pad, analog_pulse) and more, each with up to
// max_table_frames frames that morph musically. Names are stable ids.
[[nodiscard]] std::span<const std::string_view> builtin_table_names() noexcept;
// Built on first use and cached for the process; thread-safe; null for an
// unknown name. Control only (the first call builds).
[[nodiscard]] std::shared_ptr<const Wavetable> builtin_table(std::string_view name);

// A noise or texture loop for the noise oscillator: stereo, at its own rate,
// looping seamlessly from the last frame back to the first.
struct NoiseSample {
  std::string name;
  std::uint32_t sample_rate = 48'000;
  std::vector<engine::StereoFrame> frames;
};
// Factory noises, generated in-house: at least white, pink, brown,
// analog_hiss, vinyl and breath.
[[nodiscard]] std::span<const std::string_view> builtin_noise_names() noexcept;
[[nodiscard]] std::shared_ptr<const NoiseSample> builtin_noise(std::string_view name);
// A user noise from decoded media: a short crossfade makes its loop seamless.
[[nodiscard]] foundation::Result<std::shared_ptr<const NoiseSample>>
build_noise(const SynthMediaAsset& asset, std::string name);

} // namespace oscillate::native_dsp::synth::nova
