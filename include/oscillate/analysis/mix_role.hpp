#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/project/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

// Which part of a mix a recording plays (ADR-0123): drums, bass, vocals or
// other music, heard from its audio for the master rebalance. Worker or
// control thread only; deterministic for the same samples.
namespace oscillate::analysis {

struct HeardFeatures {
  // Mean spectral centroid of the sounding frames, Hz.
  double centroid_hz = 0.0;
  // Share of the sounding energy under low_band_hz.
  double low_share = 0.0;
  // Note and hit onsets per second of sounding audio.
  double onsets_per_second = 0.0;
  // Share of sounding frames with one clear pitch.
  double voiced_share = 0.0;
  // Median pitch of the voiced frames, Hz (0 with none).
  double median_pitch_hz = 0.0;
  // Mean distance of the voiced frames' pitch from the nearest equal-tempered
  // semitone, cents: a voice slides and bends, an instrument sits on notes.
  double off_note_cents = 0.0;
  // Median level of the sounding frames under the loudest frame within
  // decay_window_seconds either side, dB: hits fall away between them,
  // held notes do not.
  double decay_db = 0.0;
  // Seconds of sounding audio measured.
  double sounding_seconds = 0.0;
};

struct HeardRole {
  project::MixRole role = project::MixRole::music;
  HeardFeatures features;
};

// The analysis frame and the measurements' bounds, named so tests derive
// from them.
inline constexpr std::size_t hear_frame = 2048;
inline constexpr std::size_t hear_hop = 1024;
inline constexpr double low_band_hz = 150.0;
// Frames this far under the loudest are silence.
inline constexpr double hear_silence_db = -50.0;
inline constexpr double pitch_low_hz = 40.0;
inline constexpr double pitch_high_hz = 1'000.0;
// A frame is voiced when its normalised difference dips under this.
inline constexpr double voiced_threshold = 0.2;
// Under this much sounding audio, nothing can be told: music.
inline constexpr double hear_min_seconds = 2.0;

// The rules, in order.
inline constexpr double decay_window_seconds = 0.25;
// Drums: many hits that fall away between them.
inline constexpr double drum_onsets_per_second = 3.0;
inline constexpr double drum_max_decay_db = -10.0;
// Bass: pitched, low and mostly under low_band_hz.
inline constexpr double bass_min_voiced = 0.3;
inline constexpr double bass_max_pitch_hz = 160.0;
inline constexpr double bass_min_low_share = 0.5;
// Vocals: pitched in the voice's range, off the semitones.
inline constexpr double vocal_min_voiced = 0.4;
inline constexpr double vocal_min_pitch_hz = 90.0;
inline constexpr double vocal_max_pitch_hz = 1'000.0;
inline constexpr double vocal_min_off_note_cents = 12.0;

// The features of mono audio at `sample_rate`, and the role they say.
[[nodiscard]] HeardFeatures hear_features(std::span<const float> mono, std::uint32_t sample_rate);
[[nodiscard]] project::MixRole role_from_features(const HeardFeatures& features) noexcept;
[[nodiscard]] HeardRole hear_role(std::span<const float> mono, std::uint32_t sample_rate);

// Listens to at most hear_excerpts excerpts of hear_excerpt_seconds spread
// over a source (all of a shorter one), folded to mono.
inline constexpr std::size_t hear_excerpts = 8;
inline constexpr double hear_excerpt_seconds = 6.0;
[[nodiscard]] foundation::Result<HeardRole> hear_role(media::AudioReader& reader,
                                                      const std::function<bool()>& cancelled = {});

} // namespace oscillate::analysis
