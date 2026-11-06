#pragma once

#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace oscillate::native_dsp::synth::fret {

// Fret (ADR-0088): a sampled fingered bass, Black and Blue Basses
// ("darkblack", CC0), installed by tools/fret/install_black_and_blue.py.
// Every semitone is sampled with its velocity layers and round robins, plus
// ghost, staccato, behind-the-bridge and string-release samples. Feel
// (fret_feel.hpp) chooses each note's velocity and articulation when it
// starts; slides glide a held note to the next. The first frames of every
// note stay in memory and the rest streams from disk off the callback.
// Without the library Fret is silent and says so. Defaults are the "Funk
// Pocket" preset.
[[nodiscard]] const SynthEngine& engine();

// How a patch (`values` in the engine's table order) plays a note: Feel's
// settings as the kernel reads them when the note starts, and the
// articulation it then plays given Feel's own choice (Articulation forces
// one, Auto keeps Feel's). The kernel uses these, so previews such as the
// panel's groove lane decide exactly as playback does. Values of another
// size give the defaults and keep `decided`.
[[nodiscard]] feel::Settings feel_settings(std::span<const double> values) noexcept;
[[nodiscard]] feel::Articulation played_articulation(std::span<const double> values,
                                                     feel::Articulation decided) noexcept;

// The sounding keys, B0 to E4. Keys outside are ignored.
inline constexpr int first_key = 23;
inline constexpr int last_key = 64;
inline constexpr std::size_t key_count = last_key - first_key + 1;
// Output trim before Level: the loudest pluck of the library peaks near
// -6 dBFS, leaving the tone stage and the safety knee room.
inline constexpr double headroom_db = -6.0;

// Telemetry layout (ADR-0037).
namespace telemetry {
inline constexpr std::size_t library = 0;           // library::State.
inline constexpr std::size_t underruns = 1;         // Streamed blocks that arrived late.
inline constexpr std::size_t notes = 2;             // Notes started since reset (wraps at 2^24).
inline constexpr std::size_t last_key = 3;          // The latest note's key; -1 before any.
inline constexpr std::size_t last_velocity = 4;     // As Feel played it.
inline constexpr std::size_t last_articulation = 5; // fret::feel::Articulation.
inline constexpr std::size_t last_pushed = 6;       // 1 when the latest note was pushed.
inline constexpr std::size_t output_peak_db = 7;    // dBFS this block, >= -120.
inline constexpr std::size_t size = 8;
} // namespace telemetry

// The sample library: one per process, shared by every Fret. All functions
// are control-thread only.
namespace library {

inline constexpr std::string_view name = "Black and Blue Basses (darkblack, fingered)";
inline constexpr std::string_view attribution =
    "Black and Blue Basses by Karoryfer Samples, CC0 1.0";
inline constexpr std::string_view install_command =
    "python3 -I tools/fret/install_black_and_blue.py";
// Overrides the directory (tests, or a library kept elsewhere).
inline constexpr std::string_view directory_variable = "OSCILLATE_FRET_LIBRARY";
inline constexpr std::string_view manifest_file = "fret.manifest";
inline constexpr std::string_view format_line = "oscillate fret library 1";
// The pack's rate and channels; Fret resamples to the session's.
inline constexpr std::uint32_t pack_rate = 48'000;
inline constexpr std::uint32_t pack_channels = 1;
// Frames of each note sample kept in memory; the rest streams. Release
// samples are short and stay in memory whole.
inline constexpr std::uint32_t resident_frames = 16'384;

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

// $OSCILLATE_FRET_LIBRARY when set, otherwise
// $XDG_DATA_HOME/oscillate/instruments/fret/black-and-blue-darkblack
// (~/.local/share when XDG_DATA_HOME is unset), as the install script uses.
[[nodiscard]] std::filesystem::path default_directory();

// The library's state; while missing or invalid each call looks again
// when the directory or its manifest changed.
[[nodiscard]] Status status();

// Loads the library in `directory` and makes it the one new and idle Frets
// play. A loaded library stays in memory until the process exits.
[[nodiscard]] Status load(const std::filesystem::path& directory);

} // namespace library

} // namespace oscillate::native_dsp::synth::fret
