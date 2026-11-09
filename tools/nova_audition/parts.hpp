#pragma once

// Notes and parts shared by the audition sets: C minor with the Hijaz B
// natural, the pads with their parts (the picked three also saved as
// "Pad - <preset>.mid"), and a pad placed on its own track.
#include "audition.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::tools::parts {

inline constexpr std::int64_t quarter = timeline::ticks_per_quarter;
inline constexpr int beats_per_bar = 4;

// C minor, with the B natural of Hijaz on G.
enum class Pitch : std::uint8_t { c = 0, d = 2, eb = 3, f = 5, g = 7, ab = 8, a = 9, bb = 10, b = 11 };
inline constexpr int keys_per_octave = 12;
inline constexpr int octave_of_key_zero = -1; // Key 60 is C4.
constexpr std::uint8_t key(Pitch pitch, int octave) {
  return static_cast<std::uint8_t>((octave - octave_of_key_zero) * keys_per_octave +
                                   static_cast<int>(pitch));
}

inline constexpr std::uint8_t soft = 72;
inline constexpr std::uint8_t medium = 84;
inline constexpr std::uint8_t firm = 100;

struct Note {
  double beat;
  double length;
  std::uint8_t key;
  std::uint8_t velocity;
};

// Sustained voicings that move with the lead's phrases but enter late or
// leave early, with gaps: an open fifth, then C minor, A♭, the G of the
// Hijaz, back to C, and C to close. The lead starts on beat 8.5.
inline std::vector<Note> held_part() {
  using P = Pitch;
  return {
      {0.0, 7.0, key(P::c, 3), medium},    {0.0, 7.0, key(P::g, 3), medium},
      {2.5, 4.5, key(P::eb, 4), soft},     {9.75, 7.25, key(P::c, 3), medium},
      {9.75, 7.25, key(P::g, 3), medium},  {11.5, 4.5, key(P::eb, 4), soft},
      {17.5, 6.5, key(P::ab, 2), medium},  {17.5, 6.5, key(P::eb, 3), medium},
      {18.25, 5.75, key(P::c, 4), soft},   {25.25, 5.0, key(P::g, 2), medium},
      {25.25, 5.0, key(P::d, 3), medium},  {26.5, 3.75, key(P::g, 3), soft},
      {31.0, 8.5, key(P::c, 3), medium},   {31.0, 8.5, key(P::g, 3), medium},
      {32.75, 6.75, key(P::eb, 4), soft},  {34.5, 3.0, key(P::f, 3), soft},
      {40.75, 7.25, key(P::c, 2), medium}, {40.75, 7.25, key(P::g, 2), medium},
      {41.5, 6.5, key(P::c, 3), soft},
  };
}

// The floor: the root an octave or two under the lead's low range, moving
// to A♭ and G with the harmony, with a fifth now and then.
inline std::vector<Note> low_part() {
  using P = Pitch;
  return {
      {0.0, 15.5, key(P::c, 3), medium},  {6.5, 8.0, key(P::g, 3), soft},
      {17.5, 7.0, key(P::ab, 2), medium}, {25.25, 5.25, key(P::g, 2), medium},
      {31.0, 8.5, key(P::c, 3), medium},  {33.5, 5.0, key(P::g, 3), soft},
      {40.75, 7.25, key(P::c, 3), medium},
  };
}

// A pad: its synth, its part, and the Expanse on its track.
struct Pad {
  std::string_view engine;
  std::string_view preset;
  std::string_view space; // Expanse preset.
  double space_mix;
  std::vector<Note> (*part)();
  int octaves = 0; // The part transposed by this many octaves.
};

inline constexpr std::string_view space_engine = "expanse";
inline constexpr std::string_view space_mix_id = "expanse.mix";
inline constexpr double wet = 0.45;
inline constexpr double cloud = 0.6;
// A flute plays nothing below C4, so a flute pad plays the part an octave up.
inline constexpr int flute_octaves = 1;
// A pad's track level, linear.
inline constexpr double pad_level = 0.5;

inline const Pad ney_breath{"nova", "Ney Breath", "Ambient Wash", wet, held_part};
inline const Pad two_flutes{"wren", "Two Flutes", "Endless Cloud", cloud, held_part,
                            flute_octaves};
inline const Pad warm_sub_bed{"nova", "Warm Sub Bed", "Dark Hall", wet, low_part};

struct PadTrack {
  project::TrackId track;
  project::InsertSlotId space;
  project::ClipId clip;
};

// A clip written whole takes fresh note ids, so its notes carry any.
inline project::NoteId placeholder_note() {
  return audition::take(project::NoteId::from_bits({0, 1}));
}

inline std::int64_t ticks(double beats) {
  return static_cast<std::int64_t>(beats * static_cast<double>(quarter));
}

// The pad on a new track at `pad_level`, in a clip from bar 1 lasting
// `clip_beats`; with `cut_beat`, notes end there and later ones are left out.
inline PadTrack add_pad(application::ProjectSession& session, const Pad& pad, double clip_beats,
                        std::optional<double> cut_beat = std::nullopt) {
  using audition::done;
  using audition::take;
  const auto track = take(session.instruments().add_native_instrument_track(
      pad.engine, audition::preset_index(audition::synth(pad.engine), pad.preset)));
  std::vector<project::MidiNote> notes;
  for (const auto& note : pad.part()) {
    const double end = cut_beat ? std::min(note.beat + note.length, *cut_beat)
                                : note.beat + note.length;
    if (end <= note.beat)
      continue;
    const auto shifted = static_cast<std::uint8_t>(note.key + pad.octaves * keys_per_octave);
    notes.push_back({placeholder_note(), {ticks(note.beat)}, {ticks(end)}, shifted,
                     note.velocity});
  }
  const auto clip = take(session.midi_clips().add_midi_clip_with_notes(
      track, {0}, {ticks(clip_beats)}, std::move(notes)));
  const auto& space = audition::effect(space_engine);
  const auto slot = take(session.effects().add_effect_engine(
      track, 0, space_engine, audition::preset_index(space, pad.space)));
  auto values = take(session.effects().native_effect_values(track, slot));
  const auto mix = native_dsp::find_parameter(space, space_mix_id);
  if (!mix)
    audition::fail("no parameter " + std::string(space_mix_id));
  values[*mix] = pad.space_mix;
  done(session.effects().set_native_effect_values(track, slot, values));
  done(session.tracks().set_track_level_pan(track, pad_level, 0.0));
  return {track, slot, clip};
}

// An instrument track at a factory preset and a linear level.
inline project::TrackId add_track(application::ProjectSession& session, std::string_view engine,
                                  std::string_view preset, double level) {
  const auto track = audition::take(session.instruments().add_native_instrument_track(
      engine, audition::preset_index(audition::synth(engine), preset)));
  // The session refuses a change that changes nothing.
  if (session.snapshot().tracks[*session.tracks().track_index(track)].level != level)
    audition::done(session.tracks().set_track_level_pan(track, level, 0.0));
  return track;
}

// Notes at beats from `start`, in a clip that holds them all and lasts at
// least `min_beats`.
inline project::ClipId add_clip(application::ProjectSession& session, project::TrackId track,
                                double start, const std::vector<Note>& notes, int octaves = 0,
                                double min_beats = 0.0) {
  double length = min_beats;
  std::vector<project::MidiNote> placed;
  for (const auto& note : notes) {
    length = std::max(length, note.beat + note.length);
    const auto shifted = static_cast<std::uint8_t>(note.key + octaves * keys_per_octave);
    placed.push_back({placeholder_note(), {ticks(note.beat)}, {ticks(note.beat + note.length)},
                      shifted, note.velocity});
  }
  return audition::take(session.midi_clips().add_midi_clip_with_notes(
      track, {ticks(start)}, {ticks(length)}, std::move(placed)));
}

// A built-in effect at the start of the track's chain.
inline project::InsertSlotId add_effect(application::ProjectSession& session,
                                        project::TrackId track, std::string_view engine,
                                        std::string_view preset) {
  return audition::take(session.effects().add_effect_engine(
      track, 0, engine, audition::preset_index(audition::effect(engine), preset)));
}

inline void set_effect_value(application::ProjectSession& session, project::TrackId track,
                             project::InsertSlotId slot, std::string_view engine,
                             std::string_view id, double value) {
  auto values = audition::take(session.effects().native_effect_values(track, slot));
  const auto index = native_dsp::find_parameter(audition::effect(engine), id);
  if (!index)
    audition::fail("no parameter " + std::string(id));
  values[*index] = value;
  audition::done(session.effects().set_native_effect_values(track, slot, values));
}

// The three picked pads in the order the user listed them (Two Flutes, Warm
// Sub Bed, Ney Breath), for their twelve bars and then from the top again
// for as long as `beats` runs on.
inline constexpr int pad_part_bars = 12;
inline void add_picked_pads(application::ProjectSession& session, double beats) {
  constexpr double part_beats = pad_part_bars * beats_per_bar;
  for (const auto* pad : {&two_flutes, &warm_sub_bed, &ney_breath}) {
    const auto placed = add_pad(session, *pad, std::min(beats, part_beats));
    const double rest = beats - part_beats;
    if (rest <= 0.0)
      continue;
    std::vector<Note> again;
    for (const auto& note : pad->part())
      if (note.beat < rest)
        again.push_back({note.beat, std::min(note.length, rest - note.beat), note.key,
                         note.velocity});
    add_clip(session, placed.track, part_beats, again, pad->octaves, rest);
  }
}

} // namespace oscillate::tools::parts
