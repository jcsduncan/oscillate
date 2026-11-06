#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace oscillate::native_dsp::synth::wren {

// Wren (ADR-0089): a sampled concert flute, the University of Iowa MIS flute
// (pp, mf and ff, each with and without vibrato), installed by
// tools/wren/install_iowa_flute.py and held in memory whole. Breath
// (wren_breath.hpp) decides each note's playing when it starts: velocity,
// vibrato that blooms on long notes, swells, slurs, t/k tonguing and grace
// notes. Mono slurs a note into the next with a crossfade past its attack;
// Poly 4 tongues every note. Each voice blends the plain take into the
// vibrato take, both looping while held. Tone: Air and Warmth shelves and
// Space, a small hall on the Expanse reverb. Without the library Wren is
// silent and says so. Defaults are the "Lyrical Air" preset.
[[nodiscard]] const SynthEngine& engine();

// Breath's settings as the kernel reads them when a note starts, from a
// patch (`values` in the engine's table order); values of another size give
// the defaults. The kernel uses this, so previews (the panel's lane) decide
// exactly as playback does.
[[nodiscard]] breath::Settings breath_settings(std::span<const double> values) noexcept;

// How the kernel plays Breath's decision for a note on `key` lasting
// `length_beats` (0 for a live note) with a patch (`values` in table order;
// values of another size play the decision as it is):
// - Poly 4 tongues (t) what Breath would slur;
// - the Staccato articulation holds a note breath::staccato_beats at most
//   (a live note exactly that);
// - the vibrato take's share at full bloom is Breath's times the Vibrato
//   control;
// - a grace sounds only when its key is one Wren plays.
// Previews (the panel's phrase lane) read this to show what playback does.
struct Played {
  breath::Tongue tongue = breath::Tongue::t;
  double length_beats = 0.0;
  double vibrato = 0.0;
  bool grace = false;
};
[[nodiscard]] Played played(std::span<const double> values, int key, double length_beats,
                            const breath::Decision& decision) noexcept;

// The sounding keys, B3 to D-flat 7. Keys outside are ignored.
inline constexpr int first_key = 59;
inline constexpr int last_key = 97;
inline constexpr std::size_t key_count = last_key - first_key + 1;
// The recorded dynamics: pp, mf and ff.
inline constexpr std::uint32_t layer_count = 3;
// Makeup before Level. The Iowa takes keep their recorded levels, made at
// one fixed input gain, and sit low: a typical ff note peaks near -18 dBFS,
// so the audition line peaked near -18 dBFS with RMS near -33. +12 dB puts
// it near -6 dBFS like the other instruments; the few loudest high takes
// (near 0 dBFS as recorded) meet the safety knee.
inline constexpr double headroom_db = 12.0;

// Telemetry layout (ADR-0037).
namespace telemetry {
inline constexpr std::size_t library = 0;        // library::State.
inline constexpr std::size_t notes = 1;          // Notes started since reset (wraps at 2^24).
inline constexpr std::size_t last_key = 2;       // The latest note's key; -1 before any.
inline constexpr std::size_t last_velocity = 3;  // As Breath played it.
inline constexpr std::size_t last_tongue = 4;    // breath::Tongue.
inline constexpr std::size_t last_staccato = 5;  // 1 when the latest note was staccato.
inline constexpr std::size_t last_grace = 6;     // 1 when the latest note had a grace.
inline constexpr std::size_t vibrato = 7;        // 1 when the latest note has vibrato.
inline constexpr std::size_t output_peak_db = 8; // dBFS this block, >= -120.
inline constexpr std::size_t size = 9;
} // namespace telemetry

// The sample library: one per process, shared by every Wren. All functions
// are control-thread only.
namespace library {

inline constexpr std::string_view name = "University of Iowa MIS flute";
inline constexpr std::string_view attribution =
    "University of Iowa Musical Instrument Samples (Lawrence Fritts)";
inline constexpr std::string_view install_command = "python3 -I tools/wren/install_iowa_flute.py";
// Overrides the directory (tests, or a library kept elsewhere).
inline constexpr std::string_view directory_variable = "OSCILLATE_WREN_LIBRARY";
inline constexpr std::string_view manifest_file = "wren.manifest";
inline constexpr std::string_view format_line = "oscillate wren library 1";
// The pack's rate and channels; Wren resamples to the session's.
inline constexpr std::uint32_t pack_rate = 48'000;
inline constexpr std::uint32_t pack_channels = 1;

enum class State : std::uint8_t { missing, invalid, ready };

struct Status {
  State state = State::missing;
  std::string message; // Why it is missing or invalid; empty when ready.
  std::filesystem::path directory;
  std::string name;
  std::string attribution;
  std::size_t samples = 0;
  std::uint64_t resident_bytes = 0;
};

// $OSCILLATE_WREN_LIBRARY when set, otherwise
// $XDG_DATA_HOME/oscillate/instruments/wren/iowa-flute (~/.local/share when
// XDG_DATA_HOME is unset), as the install script uses.
[[nodiscard]] std::filesystem::path default_directory();

// The library's state; while missing or invalid each call looks again when
// the directory or its manifest changed.
[[nodiscard]] Status status();

// Loads the library in `directory` and makes it the one new and idle Wrens
// play. A loaded library stays in memory until the process exits.
[[nodiscard]] Status load(const std::filesystem::path& directory);

} // namespace library

} // namespace oscillate::native_dsp::synth::wren
