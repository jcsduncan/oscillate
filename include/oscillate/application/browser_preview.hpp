#pragma once

#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Pure rules behind the browser's previews and the arrangement's smart
// drops (ADR-0059): what a preset preview plays, how a sample's tempo is
// read, where the next beat falls, which key a dropped clip moves to, and
// whether a track looks like a kick. No Qt, no I/O, no project edits.
namespace oscillate::application {

// ---- Preset previews -------------------------------------------------------

// One note of a preview phrase, in frames from the phrase's start.
struct PreviewNote {
  std::uint64_t start_frame = 0;
  std::uint64_t length_frames = 0;
  std::uint8_t key = 60;
  std::uint8_t velocity = 100;
  bool operator==(const PreviewNote&) const = default;
};

enum class PreviewStyle : std::uint8_t { drums, percussion, bass, line, chord, texture };

// The phrase that suits a sound: a beat for Pulse, a top loop for Knock's
// percussion, a riff for a bass, a
// short line for leads, plucks and mallets, a voiced chord for pads, keys
// and pianos, and a long chord for textures. Category and preset name are
// the preset's own words; matching ignores case.
struct PresetWords {
  std::string_view engine;
  std::string_view category;
  std::string_view preset_name;
};
[[nodiscard]] PreviewStyle preview_style(const PresetWords& sound) noexcept;

// The phrase in time at `bpm` (clamped to 60..180, a preview's useful range),
// at most two bars long. Notes are sorted by start; every key is in 0..127.
// Drums use Pulse's pads from `drum_first_key` (kick, kick, snare, rim, clap,
// three toms, closed hat, open hat, ...); percussion plays a sixteenth-note
// top loop over Knock's eight pads from `drum_first_key`.
[[nodiscard]] std::vector<PreviewNote> preview_phrase(PreviewStyle style, double bpm,
                                                      std::uint32_t sample_rate,
                                                      std::uint8_t drum_first_key = 36);

// ---- Sample previews -------------------------------------------------------

// A tempo written in a sample's file name: "Loop 124 BPM", "drums_128bpm",
// "Bass-Am-90", "120_Kick". The number nearest a "bpm" token wins; without
// one, a lone 60..200 number set off by separators. Key-like tokens ("Am",
// "F#") and numbers with letters attached ("808", "v2") are not tempos.
[[nodiscard]] std::optional<double> tempo_from_file_name(std::string_view stem) noexcept;

// A loop's tempo from its length. Loops are cut to whole bars at a whole
// (or half) BPM, so of the tempos that fit 4, 8, 16, 32 or 64 beats into the
// length, those within 0.1 % of a half-BPM step in 70..180 qualify; the one
// nearest `preferred` wins. One-shots (under one second) have none.
[[nodiscard]] std::optional<double>
tempo_from_loop_length(std::uint64_t frames, std::uint32_t sample_rate, double preferred) noexcept;

// The project sample of the first quarter-note beat at or after `now`, by the
// project's tempo map (a beat within one sample of `now` is `now`).
[[nodiscard]] std::optional<std::int64_t>
next_beat_sample(const timeline::TempoMap& tempo, std::int64_t now, std::uint32_t sample_rate);

// ---- Smart drops -----------------------------------------------------------

// Transposing a clip into the song's key: the clip's key (from its source's
// review, plus the clip's current pitch) and the nearest shift (-6..+5
// semitones; ties go down) onto the song's key, or onto its relative key
// when the modes differ (A minor material in C major stays put).
struct KeyMatch {
  project::MusicalKey from;
  project::MusicalKey to;
  int semitones = 0;
  bool operator==(const KeyMatch&) const = default;
};
// None when already in key, when the source key is unknown or less
// confident than `min_confidence` (a correction is always trusted), or
// when the shifted pitch would leave the clip's pitch range.
[[nodiscard]] std::optional<KeyMatch> key_match(const project::SourceKeyReview& review,
                                                double current_pitch_semitones,
                                                project::MusicalKey song,
                                                double min_confidence = 0.5) noexcept;

// ---- Kick suggestion -------------------------------------------------------

// True when a track looks like a kick: its name or one of its clips' names
// says so ("Kick", "BD", "Bass Drum", "kck"), so does the name of the sound
// it plays (`sound_name`), or a Pulse kit plays nothing but its kick pads.
// Buses never do.
[[nodiscard]] bool looks_like_kick(const project::Track& track,
                                   std::string_view sound_name = {}) noexcept;

// True when the track already keys at least one sidechain.
[[nodiscard]] bool keys_a_sidechain(const project::Track& track) noexcept;

} // namespace oscillate::application
