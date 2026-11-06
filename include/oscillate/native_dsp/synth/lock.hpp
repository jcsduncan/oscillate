#pragma once

#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

// Kick & Bass Lock (ADR-0072): an instrument that follows another track's
// notes, read ahead from the project so nothing waits on audio. Anvil reads
// the bass notes to tune each hit's tail; Nova reads the kick hits, with
// each hit's exact amplitude, to duck and fold its low end.
namespace oscillate::native_dsp::synth {

// The pair: the kick engine follows the bass engine's notes and the bass
// follows the kick's hits. Each side's Lock switch is a choice parameter,
// off at lock_choice_off. Nova's is [Off, On], on at lock_choice_on. Anvil's
// is [Off, Bass note, Keep tone]: any choice but Off locks it (the pair
// forms, so Nova ducks and monos its lows under the hits), and only Bass note
// also retunes each hit's tail to the bass. Keep tone was appended so saved
// values keep their meaning. The panels, the session's one-click Lock and
// the tests name them through these.
inline constexpr std::string_view lock_kick_engine = "anvil";
inline constexpr std::string_view lock_bass_engine = "nova";
inline constexpr std::string_view lock_kick_parameter = "anvil.lock";
inline constexpr std::string_view lock_bass_parameter = "nova.lock.on";
inline constexpr double lock_choice_off = 0.0;
inline constexpr double lock_choice_on = 1.0;
// Anvil's locked choices: the tail follows the bass note, or the drawn tone
// is kept. The one-click Lock (and Mix Check) choose Keep tone.
inline constexpr double lock_kick_bass_note = 1.0;
inline constexpr double lock_kick_keep_tone = 2.0;

// A followed hit's amplitude: peak-normalised, one value every
// lock_envelope_frames frames from the hit's start.
inline constexpr std::uint32_t lock_envelope_frames = 16;
inline constexpr std::uint32_t no_lock_envelope = std::numeric_limits<std::uint32_t>::max();

// One note of a followed track, in project frames.
struct LockNote {
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
  std::uint32_t envelope = no_lock_envelope; // Into LockState::envelopes.
};

// Immutable once offered; read by the kernel on the callback.
struct LockState {
  std::vector<LockNote> notes; // By start, then key.
  std::vector<std::vector<float>> envelopes;
};

} // namespace oscillate::native_dsp::synth
