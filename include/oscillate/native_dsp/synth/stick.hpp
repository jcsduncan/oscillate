#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace oscillate::native_dsp::synth::stick {

// Stick (ADR-0114, ADR-0121): a sampled acoustic drum kit, every hit
// recorded on every mic. Two kits (CC BY 4.0) installed by
// tools/stick/install_kits.py: Naked Drums (Wilkinson Audio, in kinwie's
// SFZ conversion; the default) and DRS Studio (DrumGizmo's DRSKit 2.1, the
// one with brushes). Each hit's start stays in memory and the rest streams
// from disk.
//
// Notes follow the General MIDI drum map. Velocity picks a hit by its
// recorded power, with seeded round robins among its neighbours; the
// hi-hat pedal (stick.perf.pedal) opens the hat between its closed and open
// recordings; the mic mixer sets each mic's level, mute and solo, and Bleed
// off keeps each hit to its own close mics, the overheads and the room.
// Without its kit Stick is silent and says so. Defaults are the "Natural"
// preset.
[[nodiscard]] const SynthEngine& engine();

// ---- Kits ---------------------------------------------------------------------------------

enum class Kit : std::uint8_t { naked, drs };
inline constexpr std::size_t kit_count = 2;

// ---- Pieces and their articulations ----------------------------------------------------------

// The kit's pieces, in table order. A kit has some of them.
enum class Piece : std::uint8_t {
  kick,
  snare,
  hihat,
  tom1, // The highest tom.
  tom2,
  tom3,
  tom4,
  tom5,
  crash1,
  crash2,
  ride,
  splash,
  china
};
inline constexpr std::size_t piece_count = 13;
inline constexpr std::size_t max_toms = 5;
// A bit per Piece.
[[nodiscard]] constexpr std::uint32_t piece_bit(Piece piece) noexcept {
  return 1U << static_cast<unsigned>(piece);
}

// How a piece was struck. The hi-hat's strokes are recorded closed, half
// open or open, and its pedal on its own.
enum class Articulation : std::uint8_t {
  hit,       // The plain stroke: centre, bow or edge.
  alt,       // A second drum or stroke played as this piece (Naked's second
             // snare, DRS Studio's kick with its beater bouncing).
  sidestick, // Snare: the stick laid across the rim.
  bell,      // Ride: the bell.
  closed,    // Hi-hat strokes, closed to open.
  half,
  open,
  pedal // Hi-hat: the foot closing it.
};
inline constexpr std::size_t articulation_count = 8;

// What struck it: sticks, or brushes where the kit recorded them (the
// kick's beater and the hi-hat's pedal are the same either way).
enum class Striker : std::uint8_t { stick, brush };
inline constexpr std::size_t striker_count = 2;

// The hi-hat's openness each stroke was recorded at.
[[nodiscard]] std::optional<double> openness_of(Articulation articulation) noexcept;
// Whether a piece was recorded with an articulation (any kit).
[[nodiscard]] bool plays(Piece piece, Articulation articulation) noexcept;

// ---- The General MIDI map ---------------------------------------------------------------------

inline constexpr int first_key = 35; // B1 (C4 is 60), the second kick.
inline constexpr int last_key = 59;  // B3, the second ride (the ride).
// What a key plays: the hi-hat's F#2 strokes at the pedal's openness.
struct Mapped {
  Piece piece = Piece::kick;
  Articulation articulation = Articulation::hit;
  bool at_pedal = false;
};
// The piece and articulation a key plays on a kit with `toms` toms
// (nullopt for a key outside the map). Toms spread over the six tom keys,
// high to low.
[[nodiscard]] std::optional<Mapped> map_key(int key, std::size_t toms) noexcept;

// ---- Mics --------------------------------------------------------------------------------------

// The mixer's strips: a kit's mics grouped by what they hear.
enum class MicGroup : std::uint8_t {
  kick_in,
  kick_out,
  kick_sub,
  snare_top,
  snare_bottom,
  toms,
  hihat,
  ride,
  overheads,
  room,
  far_room,
  mid_side,
  china_oh
};
inline constexpr std::size_t mic_group_count = 13;
// Close mics hear their pieces; the overheads and rooms hear the kit.
[[nodiscard]] constexpr bool close_group(MicGroup group) noexcept {
  return group < MicGroup::overheads;
}

// A kit's channels (Naked Drums has 16, DRS Studio 13).
inline constexpr std::uint32_t max_mics = 16;

// The mic mixes on the kit selector, applied over the mixer's faders.
enum class Mix : std::uint8_t { dry, studio, room };
inline constexpr std::size_t mix_count = 3;

// ---- Ranges ----------------------------------------------------------------------------------

inline constexpr double max_tune_semitones = 12.0;
// Decay: 100 % lets a hit ring as recorded; less adds a decay reaching
// -60 dB at that share of the recording.
inline constexpr double min_decay = 0.05;
// A velocity curve of -1 (soft: light touches play louder) to 1 (hard).
inline constexpr double max_curve = 1.0;

// ---- Telemetry (ADR-0037) ----------------------------------------------------------------------

namespace telemetry {
inline constexpr std::size_t library = 0;        // library::State of the kit chosen.
inline constexpr std::size_t kit = 1;            // The kit chosen (Kit).
inline constexpr std::size_t hits = 2;           // Hits started since reset (wraps at 2^24).
inline constexpr std::size_t voices = 3;         // Voices sounding.
inline constexpr std::size_t output_peak_db = 4; // dBFS this block, >= -120.
inline constexpr std::size_t pedal = 5;          // The hi-hat's openness as played, 0..1.
inline constexpr std::size_t last_piece = 6;     // The latest hit's Piece, -1 before any.
inline constexpr std::size_t underruns = 7;      // Streamed frames heard late (wraps at 2^24).
inline constexpr std::size_t piece_level = 8;    // Per Piece: its loudest sounding hit, 0..1.
inline constexpr std::size_t mic_db = piece_level + piece_count; // Per MicGroup: peak dBFS.
inline constexpr std::size_t size = mic_db + mic_group_count;
} // namespace telemetry

// ---- The kits' libraries
// ---------------------------------------------------------------------------

// One library per kit, shared by every Stick. All functions are
// control-thread only.
namespace library {

inline constexpr std::string_view install_command = "python3 -I tools/stick/install_kits.py";
// Overrides the folder holding the kits (tests, or kits kept elsewhere).
inline constexpr std::string_view directory_variable = "OSCILLATE_STICK_LIBRARY";
inline constexpr std::string_view manifest_file = "stick.manifest";
inline constexpr std::string_view format_line = "oscillate stick library 1";
// Every kit is installed at this rate; Stick resamples to the session's.
inline constexpr std::uint32_t pack_rate = 48'000;
// CC BY 4.0 asks for credit and a link to the licence.
inline constexpr std::string_view licence_url = "https://creativecommons.org/licenses/by/4.0/";

// What the panel says of a kit before it is installed.
struct KitFacts {
  std::string_view name;         // On the kit selector.
  std::string_view description;  // Under it.
  std::string_view folder;       // Under the library folder.
  std::string_view install_flag; // install_kits.py --kit <flag>
  std::string_view attribution;
  std::uint32_t pieces = 0; // A piece_bit per piece it has.
  std::size_t toms = 0;
};
inline constexpr std::uint32_t common_pieces = piece_bit(Piece::kick) | piece_bit(Piece::snare) |
                                               piece_bit(Piece::hihat) | piece_bit(Piece::tom1) |
                                               piece_bit(Piece::tom2) | piece_bit(Piece::crash1) |
                                               piece_bit(Piece::crash2) | piece_bit(Piece::ride);
inline constexpr std::array<KitFacts, kit_count> kits{{
    {"Naked Drums", "Yamaha studio kit · 16 mics", "naked-drums", "naked",
     "Naked Drums by Wilkinson Audio, SFZ by kinwie, CC BY 4.0",
     common_pieces | piece_bit(Piece::tom3) | piece_bit(Piece::tom4) | piece_bit(Piece::tom5) |
         piece_bit(Piece::splash) | piece_bit(Piece::china),
     max_toms},
    {"DRS Studio", "Maple kit · 13 mics · brushes", "drs-studio", "drs",
     "DRSKit 2.1 by Jes Eiler (DRSDrums) and the DrumGizmo team, CC BY 4.0",
     common_pieces | piece_bit(Piece::tom3), 3},
}};
[[nodiscard]] constexpr const KitFacts& facts(Kit kit) noexcept {
  return kits[static_cast<std::size_t>(kit)];
}

// `loading` while a background read runs.
enum class State : std::uint8_t { missing, invalid, ready, loading };

struct Status {
  State state = State::missing;
  std::string message; // Why it is missing or invalid; empty when ready.
  std::filesystem::path directory;
  std::string name;
  std::string attribution;
  std::size_t samples = 0; // Hits.
  std::uint64_t resident_bytes = 0;
  std::uint64_t pack_bytes = 0;
  std::uint32_t mics = 0;
};

// What a ready kit has, for the panel: its mics and pieces.
struct Contents {
  std::array<std::uint32_t, mic_group_count> mics{}; // Channels per group.
  std::array<std::uint32_t, piece_count> hits{};     // Hits per piece.
  // Per piece, a bit per Articulation it was recorded with.
  std::array<std::uint32_t, piece_count> articulations{};
  // Per piece, whether it was recorded with brushes.
  std::array<bool, piece_count> brushes{};
  std::size_t toms = 0;
};

// $OSCILLATE_STICK_LIBRARY when set, otherwise
// $XDG_DATA_HOME/oscillate/instruments/stick (~/.local/share when
// XDG_DATA_HOME is unset), as the install script uses; each kit in its
// folder below.
[[nodiscard]] std::filesystem::path default_directory();
[[nodiscard]] std::filesystem::path kit_directory(Kit kit);

// The kit's state. The first look while it is not loaded starts reading it
// in the background and reports `loading` until done; while missing or
// invalid each later call looks again when its folder or manifest changed.
[[nodiscard]] Status status(Kit kit);
// What a ready kit holds (empty otherwise).
[[nodiscard]] Contents contents(Kit kit);

// Loads the kit in `directory` now and makes it the one Stick plays for
// `kit`. A loaded kit stays in memory until the process exits.
[[nodiscard]] Status load(Kit kit, const std::filesystem::path& directory);

// Waits for every background read status() or fetching started (offline
// renders, before their first block).
void wait();

} // namespace library

} // namespace oscillate::native_dsp::synth::stick
