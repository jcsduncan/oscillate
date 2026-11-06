#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::native_dsp::synth::concert {

// Concert (ADR-0071): a sampled grand piano. It plays one library, the
// Salamander Grand Piano (a Yamaha C5, CC-BY 3.0), installed on the user's
// machine by tools/concert/install_salamander.py. The first frames of every
// sample stay in memory and the rest streams from disk off the callback.
// Velocity crossfades between sixteen layers; the dampers, half-pedal,
// sostenuto and una corda are the instrument's own (ADR-0035). Without the
// library Concert is silent and says so. Defaults are the "Concert Grand"
// preset.
[[nodiscard]] const SynthEngine& engine();

// The 88 keys, A0 to C8. Keys outside are ignored, as on the instrument.
inline constexpr int first_key = 21;
inline constexpr int last_key = 108;
inline constexpr std::size_t key_count = 88;
// Output trim before Level: a velocity-100 four-note chord of the library
// peaks near -6 dBFS, leaving the safety knee for real overloads.
inline constexpr double headroom_db = -9.0;

// Telemetry layout (ADR-0037): the pedals; each key's ringing energy (log10
// of its mean square relative to a mezzo-forte note, floored at
// energy_floor) and its damper contact (0 lifted .. 1 resting), from A0 up;
// then the library's state (library::State) and the number of streamed
// blocks that arrived late since the kernel started.
namespace telemetry {
inline constexpr std::size_t sustain = 0;
inline constexpr std::size_t sostenuto = 1;
inline constexpr std::size_t soft = 2;
inline constexpr std::size_t first_energy = 3;
inline constexpr std::size_t first_damper = first_energy + key_count;
inline constexpr std::size_t library = first_damper + key_count;
inline constexpr std::size_t underruns = library + 1;
inline constexpr std::size_t size = underruns + 1;
inline constexpr float energy_floor = -12.0F;
} // namespace telemetry

// The sample library: one per process, shared by every Concert. All
// functions are control-thread only.
namespace library {

inline constexpr std::string_view name = "Salamander Grand Piano V3";
inline constexpr std::string_view instrument = "Yamaha C5";
inline constexpr std::string_view attribution =
    "Salamander Grand Piano by Alexander Holm, CC-BY 3.0";
inline constexpr std::string_view install_command =
    "python3 -I tools/concert/install_salamander.py";
// Overrides the directory (tests, or a library kept elsewhere).
inline constexpr std::string_view directory_variable = "OSCILLATE_CONCERT_LIBRARY";
inline constexpr std::string_view manifest_file = "concert.manifest";
inline constexpr std::string_view format_line = "oscillate concert library 1";
// The pack's rate; Concert resamples to the session's.
inline constexpr std::uint32_t pack_rate = 48'000;
// Frames of each note sample kept in memory; the rest streams. Release,
// resonance and pedal samples are short and stay in memory whole.
inline constexpr std::uint32_t resident_frames = 16'384;
// A note sample serves keys at most this far from its root (the Salamander
// samples every minor third, so one semitone either side).
inline constexpr int max_repitch_semitones = 2;

enum class State : std::uint8_t { missing, invalid, ready };

struct Status {
  State state = State::missing;
  // Why it is missing or invalid, for the panel; empty when ready.
  std::string message;
  std::filesystem::path directory;
  // From the library's manifest once ready.
  std::string name;
  std::string attribution;
  std::size_t samples = 0;
  std::uint64_t resident_bytes = 0;
  // Each key's sample length at a mezzo-forte layer, in seconds, A0 up
  // (key_count values; empty unless ready).
  std::vector<double> ring_seconds;
  int last_damped_key = 0;
};

// $OSCILLATE_CONCERT_LIBRARY when set, otherwise
// $XDG_DATA_HOME/oscillate/instruments/concert/salamander-grand-v3
// (~/.local/share when XDG_DATA_HOME is unset), as the install script uses.
[[nodiscard]] std::filesystem::path default_directory();

// The library's state. The first call loads the default directory; while it
// is missing or invalid, each call looks again, so an install made while the
// app runs is picked up the next time a panel asks or a Concert starts.
[[nodiscard]] Status status();

// Loads the library in `directory` and makes it the one new and idle
// Concerts play. A library that loaded stays in memory until the process
// exits, because a sounding kernel may still read it.
[[nodiscard]] Status load(const std::filesystem::path& directory);

} // namespace library

} // namespace oscillate::native_dsp::synth::concert
