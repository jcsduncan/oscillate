#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Constellation (ADR-0077): every preset of an engine rendered offline
// playing one short phrase and measured, so a browser can place it by how it
// sounds. Measuring is a pure function of a render; rendering is
// deterministic; the cache keeps measurements by a hash of what was
// rendered, so only new or edited sounds are rendered again. Control or
// worker threads only.
namespace oscillate::application::sound_map {

// Bump when the phrase or the measurements change: every cached
// measurement is then redone.
inline constexpr std::uint32_t analysis_version = 1;

// The test phrase: a held note (a bass) or an octave dyad (everything else),
// held, released, and the tail listened to.
inline constexpr std::uint32_t render_rate = 48'000;
inline constexpr double held_seconds = 1.0;
inline constexpr double render_seconds = 2.5;
inline constexpr std::uint32_t render_block_frames = 128;
inline constexpr std::uint8_t bass_key = 36;                    // C2
inline constexpr std::array<std::uint8_t, 2> dyad_keys{48, 60}; // C3, C4
inline constexpr std::uint8_t phrase_velocity = 100;
inline constexpr std::string_view bass_category = "Bass";

// Which phrase a sound plays: its preset category decides.
enum class Register : std::uint8_t { bass, other };
[[nodiscard]] Register register_for(std::string_view category) noexcept;

// What a render sounds like, each 0..1.
struct Features {
  // The log-frequency spectral centroid of the mid signal: 0 at 20 Hz, 1 at
  // 20 kHz.
  double brightness = 0.0;
  // Short to long: how slowly it starts and how long it stays audible,
  // through its decay and release.
  double length = 0.0;
  // Spectral flux of the mid signal over the held part: 0 static.
  double movement = 0.0;
  // Side energy over mid plus side: 0 mono, 0.5 uncorrelated.
  double width = 0.0;
  // False when the render stays under the silence floor; the rest is 0.
  bool audible = false;
  bool operator==(const Features&) const = default;
};

// The measurements' constants, named so tests derive from them.
inline constexpr double envelope_hop_seconds = 0.010;
inline constexpr double silence_rms = 1e-5;              // -100 dBFS
inline constexpr double attack_fraction = 0.9;           // of the peak envelope
inline constexpr double audible_floor = 0.031'622'776'6; // -30 dB under the peak
inline constexpr double attack_share = 0.35;             // of length; the rest is the tail
inline constexpr double centroid_low_hz = 20.0;
inline constexpr double centroid_high_hz = 20'000.0;
inline constexpr double spectrum_floor = 1e-4; // -80 dB under the frame's peak bin
inline constexpr std::size_t movement_bands = 24;
inline constexpr double movement_low_hz = 50.0;
inline constexpr double movement_high_hz = 16'000.0;
inline constexpr double movement_frame_floor = 1e-2; // -40 dB of the loudest frame's energy

// Pure: measures a render made at `sample_rate` whose notes were released at
// `held_frames`. Same frames, same features, bit for bit.
[[nodiscard]] Features measure(std::span<const engine::StereoFrame> frames,
                               std::uint32_t sample_rate, std::size_t held_frames);

// A sound as an engine plays it: dense values in table order and its
// document.
struct Sound {
  std::vector<double> values;
  project::NativeSynthDocument document;
  bool operator==(const Sound&) const = default;
};

// Factory preset `preset`, resolved as a track loading it would be.
[[nodiscard]] foundation::Result<Sound> factory_sound(const native_dsp::synth::SynthEngine& engine,
                                                      std::size_t preset);
// A user preset file's text (ADR-0058) for this engine.
[[nodiscard]] foundation::Result<Sound>
sound_from_preset_text(const native_dsp::synth::SynthEngine& engine, std::string_view text);

// Returns true to abandon a render (checked between blocks).
using StopRequested = std::function<bool()>;

// Renders the phrase offline. Fails for an invalid sound, a document with
// imported media (there is no project to read it from) or a stop.
[[nodiscard]] foundation::Result<std::vector<engine::StereoFrame>>
render(const native_dsp::synth::SynthEngine& engine, const Sound& sound, Register phrase,
       const StopRequested& stop = {});
// render, then measure.
[[nodiscard]] foundation::Result<Features> analyse(const native_dsp::synth::SynthEngine& engine,
                                                   const Sound& sound, Register phrase,
                                                   const StopRequested& stop = {});

// The cache key: the engine and its version, the canonical saved patch, the
// phrase and analysis_version. Fails when the sound is not valid for the
// engine.
[[nodiscard]] foundation::Result<std::uint64_t>
sound_key(const native_dsp::synth::SynthEngine& engine, const Sound& sound, Register phrase);
// 64-bit FNV-1a, for keys.
[[nodiscard]] std::uint64_t fnv1a(std::string_view bytes,
                                  std::uint64_t basis = 0xcbf2'9ce4'8422'2325ULL) noexcept;

// Where a sound sits in the sky, each 0..1: x brightness, y length, and its
// movement and width, each against the library.
struct Placement {
  double x = 0.0;
  double y = 0.0;
  double movement = 0.0;
  double width = 0.0;
};

// The library's spread, by robust percentiles of its audible sounds, so one
// outlier does not crowd every other star into a corner.
inline constexpr double low_percentile = 0.05;
inline constexpr double high_percentile = 0.95;
class Sky {
public:
  Sky() = default;
  explicit Sky(std::span<const Features> library);
  // Clamped into 0..1; an axis with no spread places at its middle.
  [[nodiscard]] Placement place(const Features& features) const noexcept;

private:
  struct Range {
    double low = 0.0;
    double high = 1.0;
  };
  Range brightness_, length_, movement_, width_;
};

// Measurements by key in one JSON file. Unreadable, oversized or
// other-version files read as empty: a cache is never the truth.
inline constexpr std::size_t max_cache_bytes = std::size_t{4} * 1024U * 1024U;
class FeatureCache {
public:
  FeatureCache() = default;
  [[nodiscard]] static FeatureCache load(const std::filesystem::path& file);
  [[nodiscard]] std::optional<Features> find(std::uint64_t key) const;
  void insert(std::uint64_t key, const Features& features);
  // Keeps only these keys (what the library holds now).
  void retain(std::span<const std::uint64_t> keys);
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  // Writes through a temporary file and a rename, creating the directory.
  [[nodiscard]] foundation::Result<void> save() const;
  [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
  std::filesystem::path file_;
  std::map<std::uint64_t, Features> entries_;
};

// Analyses several sounds on up to `threads` threads. `done` runs once per
// sound as each finishes (from any of those threads; it must be thread
// safe) with its index and result. Returns when all are done or stopped.
struct Job {
  Sound sound;
  Register phrase = Register::other;
};
using JobDone = std::function<void(std::size_t index, const foundation::Result<Features>& result)>;
inline constexpr std::size_t max_analysis_threads = 4;
// Half the machine's cores, at least one and at most max_analysis_threads,
// so the desktop and any playback keep theirs.
[[nodiscard]] std::size_t analysis_threads() noexcept;
void analyse_all(const native_dsp::synth::SynthEngine& engine, std::span<const Job> jobs,
                 std::size_t threads, const JobDone& done, const StopRequested& stop = {});

} // namespace oscillate::application::sound_map
