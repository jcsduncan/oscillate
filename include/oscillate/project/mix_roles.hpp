#pragma once

#include "oscillate/project/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Master rebalance roles (ADR-0123): what part of the mix each track plays,
// and on which tracks' outputs each role's gain and EQ act. Pure functions of
// the saved snapshot, so live playback and export agree.
namespace oscillate::project {

// Why a track has its role, strongest first.
enum class RoleReason : std::uint8_t {
  chosen,  // The musician chose it on this track.
  group,   // It follows the group bus it plays into, which the musician set.
  device,  // Its built-in instrument plays only this part (Stick, Fret, ...).
  name,    // Its name, or its clips' names, say so.
  heard,   // Its audio was listened to (saved as Track::heard_role).
  notes,   // Its MIDI sits in the bass register.
  summed,  // A group bus: every track it sums plays this role.
  fallback // Nothing said otherwise: music, or none for a return or mixed bus.
};

struct TrackRole {
  TrackId track;
  MixRole role = MixRole::music;
  RoleReason reason = RoleReason::fallback;
  // The role whose stage acts on this track's output: an audio or
  // instrument track's role. Buses carry none: a bus's role only passes to
  // the tracks under it, so every sound passes its role's stage once, on its
  // own track, whatever buses and sends it reaches the master through.
  MixRole stage = MixRole::none;
};

// Every track's role, in snapshot.tracks order. A chosen role holds for the
// track and everything that plays into it unless they choose their own.
// Otherwise an audio or instrument track takes, in this order: its device's
// part, a part its name or its clips' names say, the part heard in its audio,
// bass when its notes sit low, else music. A group bus takes the role every
// track it sums shares (a track of none among them makes it mixed), else
// its name's, else none; a return bus none.
[[nodiscard]] std::vector<TrackRole> mix_roles(const Snapshot& snapshot);

// The built-in instruments that play one part only.
inline constexpr std::array<std::string_view, 4> drum_engines{"stick", "knock", "anvil", "pulse"};
inline constexpr std::array<std::string_view, 2> bass_engines{"fret", "torque"};

// The role a name says, from whole words (case-insensitive; letters and
// digits split words, so "Vox2" and "hi-hat" match): drums before vocals
// before bass, so "Bass Drum" is drums.
[[nodiscard]] std::optional<MixRole> role_from_name(std::string_view name);

// MIDI notes in the bass register: at least this many, with half of them
// below bass_median_key and nine in ten below bass_top_key.
inline constexpr std::size_t bass_min_notes = 4;
inline constexpr std::uint8_t bass_median_key = 48; // C3
inline constexpr std::uint8_t bass_top_key = 55;    // G3
inline constexpr double bass_top_share = 0.9;

} // namespace oscillate::project
