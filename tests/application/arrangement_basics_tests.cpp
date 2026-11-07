#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

// The redesign's arrangement basics in the session: names, colours and
// markers; default reverb and delay returns with silent sends on every
// track; the clipboard (copy, cut, paste, Ctrl+D duplicate) for clips,
// notes and automation points; and track duplication. Every edit is one
// undo step.
namespace {
using namespace oscillate;
namespace fs = std::filesystem;
using project::TrackKind;

using oscillate::test::require;

using oscillate::test::take;

void accepted(foundation::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-arrangement") {}
};

constexpr std::int64_t bar = 4 * timeline::ticks_per_quarter;

const project::Track& track(const application::ProjectSession& session, project::TrackId id) {
  const auto& tracks = session.snapshot().tracks;
  const auto found = std::ranges::find(tracks, id, &project::Track::id);
  REQUIRE_NE(found, tracks.end(), "track exists");
  return *found;
}

std::size_t audible_sends_to(const project::Track& from, project::TrackId to) {
  return static_cast<std::size_t>(std::ranges::count_if(from.sends, [&](const auto& send) {
    return send.destination == to && send.purpose == project::SendPurpose::audible;
  }));
}

void new_projects_get_reverb_and_delay_returns() {
  Directory directory;
  auto session = take(
      application::ProjectSession::create(directory.path() / "song", {.default_returns = true}));
  const auto& tracks = session->snapshot().tracks;
  REQUIRE_EQ(tracks.size(), 2, "two named return tracks");
  REQUIRE_EQ(tracks[0].kind, TrackKind::return_bus, "two named return tracks");
  REQUIRE_EQ(tracks[1].kind, TrackKind::return_bus, "two named return tracks");
  REQUIRE_EQ(tracks[0].name, "A · Reverb", "two named return tracks");
  REQUIRE_EQ(tracks[1].name, "B · Delay", "two named return tracks");
  const auto* reverb = std::get_if<project::NativeEffectPatch>(&tracks[0].effects.at(0).processor);
  const auto* delay = std::get_if<project::NativeEffectPatch>(&tracks[1].effects.at(0).processor);
  REQUIRE_TRUE(reverb, "Expanse on the reverb return, Relay on the delay return");
  REQUIRE_EQ(reverb->engine, "expanse", "Expanse on the reverb return, Relay on the delay return");
  REQUIRE_TRUE(delay, "Expanse on the reverb return, Relay on the delay return");
  REQUIRE_EQ(delay->engine, "relay", "Expanse on the reverb return, Relay on the delay return");
  const auto wet = [](const project::NativeEffectPatch& patch, std::string_view id) {
    return std::ranges::any_of(patch.parameters,
                               [&](const auto& p) { return p.id == id && p.value == 1.0; });
  };
  REQUIRE_TRUE(wet(*reverb, "expanse.mix"), "returns run fully wet");
  REQUIRE_TRUE(wet(*delay, "relay.mix"), "returns run fully wet");

  // Sends exist only once their knob is turned up: a new track has none,
  // and the first move creates the send to that return.
  const auto returns = std::vector{tracks[0].id, tracks[1].id};
  const auto synth = take(session->instruments().add_synth_track());
  require(track(*session, synth).sends.empty(), "a new track carries no silent sends");
  accepted(session->routing().set_send_level(synth, returns[0], 0.5));
  REQUIRE_EQ(audible_sends_to(track(*session, synth), returns[0]), 1,
             "set_send_level creates the send at its level");
  REQUIRE_TRUE(std::ranges::any_of(track(*session, synth).sends,
                                   [&](const auto& send) {
                                     return send.destination == returns[0] && send.gain == 0.5;
                                   }),
               "set_send_level creates the send at its level");
  accepted(session->routing().set_send_level(synth, returns[0], 0.25));
  REQUIRE_EQ(audible_sends_to(track(*session, synth), returns[0]), 1,
             "a second move adjusts the same send");
  accepted(session->history().undo());
  accepted(session->history().undo());
  require(track(*session, synth).sends.empty(), "undo removes the created send");
}

void existing_projects_are_not_seeded() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  require(session->snapshot().tracks.empty(), "the default create starts empty");
  const auto synth = take(session->instruments().add_synth_track());
  require(track(*session, synth).sends.empty(), "a plain project has no returns or sends");
}

void names_colours_and_markers() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto synth = take(session->instruments().add_synth_track());
  accepted(session->tracks().set_track_name(synth, "Lead"));
  accepted(session->tracks().set_track_color(synth, 0xC4'9B'FFU));
  REQUIRE_EQ(track(*session, synth).name, "Lead", "name and colour are saved");
  REQUIRE_EQ(track(*session, synth).color, 0xC4'9B'FFU, "name and colour are saved");
  require(!session->tracks().set_track_color(synth, 0x1'00'00'00U), "colours are 24-bit");
  accepted(session->history().undo());
  require(!track(*session, synth).color, "undo restores the palette colour");

  const auto drop = take(session->song().add_marker({8 * bar}, "Drop"));
  const auto intro = take(session->song().add_marker({0}, "Intro"));
  REQUIRE_EQ(take(session->song().add_marker({0}, "Start")), intro, "one marker per position");
  const auto& markers = session->snapshot().markers;
  REQUIRE_EQ(markers.size(), 2, "markers are kept in position order");
  REQUIRE_EQ(markers[0].name, "Start", "markers are kept in position order");
  REQUIRE_EQ(markers[1].id, drop, "markers are kept in position order");
  REQUIRE_EQ(take(session->song().next_marker({1})).id, drop, "next and previous marker queries");
  REQUIRE_EQ(take(session->song().previous_marker({8 * bar})).id, intro,
             "next and previous marker queries");
  require(!session->song().next_marker({8 * bar}), "no marker after the last one");
  accepted(session->song().move_marker(intro, {16 * bar}));
  REQUIRE_EQ(session->snapshot().markers.back().id, intro, "moving re-sorts the markers");
  accepted(session->song().remove_marker(drop));
  REQUIRE_EQ(session->snapshot().markers.size(), 1, "markers can be removed");
}

void clips_copy_paste_and_duplicate() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto upper = take(session->instruments().add_synth_track());
  const auto lower = take(session->instruments().add_synth_track());
  const auto target = take(session->instruments().add_synth_track());
  const auto target_lower = take(session->instruments().add_synth_track());
  accepted(session->midi_clips().add_midi_clip(upper, {0}));
  accepted(session->midi_clips().add_midi_clip(lower, {bar}));
  const auto first = track(*session, upper).midi_clips.front().id;
  const auto second = track(*session, lower).midi_clips.front().id;
  accepted(session->notes().add_note(first, {0}, 60));
  const std::vector clips{first, second};

  // Paste keeps relative timing and track offsets.
  accepted(session->clipboard().copy_clips(clips));
  const auto pasted = take(session->clipboard().paste_clips(target, {4 * bar}));
  REQUIRE_EQ(pasted.size(), 2, "both clips pasted");
  const auto start = [&](project::TrackId id) {
    return std::get<project::MusicalInterval>(track(*session, id).midi_clips.front().placement)
        .start.value;
  };
  REQUIRE_EQ(start(target), 4 * bar, "the second clip lands one bar later, one track lower");
  REQUIRE_EQ(start(target_lower), 5 * bar, "the second clip lands one bar later, one track lower");
  REQUIRE_NE(track(*session, target).midi_clips.front().id, first,
             "pasted clips and notes get fresh identities");
  REQUIRE_NE(track(*session, target).midi_clips.front().notes.front().id,
             track(*session, upper).midi_clips.front().notes.front().id,
             "pasted clips and notes get fresh identities");
  accepted(session->history().undo());
  REQUIRE_TRUE(track(*session, target).midi_clips.empty(), "a paste is one undo step");
  REQUIRE_TRUE(track(*session, target_lower).midi_clips.empty(), "a paste is one undo step");

  // Ctrl+D: copies right after the selection's end, on the same tracks.
  const auto duplicated = take(session->clipboard().duplicate_clips(clips));
  REQUIRE_EQ(duplicated.size(), 2, "duplicate adds one copy per clip");
  REQUIRE_EQ(track(*session, upper).midi_clips.size(), 2, "duplicate adds one copy per clip");
  REQUIRE_EQ(track(*session, lower).midi_clips.size(), 2, "duplicate adds one copy per clip");
  const auto& upper_copy = track(*session, upper).midi_clips.back();
  REQUIRE_EQ(std::get<project::MusicalInterval>(upper_copy.placement).start.value, 2 * bar,
             "duplicates start where the selection ends");
  require(session->clipboard().clipboard_has_clips(),
          "duplicating keeps the copied clips on the clipboard");

  // Cut removes, paste restores elsewhere; both undoable.
  accepted(session->clipboard().cut_clips(std::vector{second}));
  REQUIRE_EQ(track(*session, lower).midi_clips.size(), 1, "cut removes the clip");
  (void)take(session->clipboard().paste_clips(target, {0}));
  REQUIRE_EQ(track(*session, target).midi_clips.size(), 1, "cut clips paste");
}

// A selected group moves, merges and deletes together, one undo step each.
void clips_move_merge_and_delete_together() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto upper = take(session->instruments().add_synth_track());
  const auto lower = take(session->instruments().add_synth_track());
  accepted(session->midi_clips().add_midi_clip(upper, {0}));
  accepted(session->midi_clips().add_midi_clip(upper, {2 * bar}));
  accepted(session->midi_clips().add_midi_clip(lower, {bar}));
  const auto first = track(*session, upper).midi_clips.front().id;
  const auto second = track(*session, upper).midi_clips.back().id;
  const auto third = track(*session, lower).midi_clips.front().id;
  accepted(session->notes().add_note(first, {0}, 60));
  accepted(session->notes().add_note(second, {timeline::ticks_per_quarter}, 64));
  const auto start_of = [&](project::TrackId owner, project::ClipId id) {
    const auto& clips = track(*session, owner).midi_clips;
    const auto found = std::ranges::find(clips, id, &project::MidiClip::id);
    REQUIRE_NE(found, clips.end(), "the clip is on the expected track");
    return std::get<project::MusicalInterval>(found->placement).start.value;
  };

  // Moved a bar later, the spacing kept.
  const std::vector group{first, third};
  accepted(session->clipboard().move_clips(group, bar, 0));
  REQUIRE_EQ(start_of(upper, first), bar, "the group moves together");
  REQUIRE_EQ(start_of(lower, third), 2 * bar, "the group moves together");
  accepted(session->history().undo());
  REQUIRE_EQ(start_of(upper, first), 0, "a group move is one undo step");
  REQUIRE_EQ(start_of(lower, third), bar, "a group move is one undo step");

  // Down a row: the upper clip onto the lower track, the lower one refused
  // as a whole (there is no row below it).
  require(!session->clipboard().move_clips(group, 0, 1), "moving off the track list is refused");
  REQUIRE_EQ(start_of(upper, first), 0, "a refused group move changes nothing");
  REQUIRE_EQ(start_of(lower, third), bar, "a refused group move changes nothing");
  accepted(session->clipboard().move_clips(std::vector{first, second}, 0, 1));
  REQUIRE_TRUE(track(*session, upper).midi_clips.empty(), "clips move onto another track together");
  REQUIRE_EQ(track(*session, lower).midi_clips.size(), 3, "clips move onto another track together");
  accepted(session->history().undo());
  require(!session->clipboard().move_clips(group, -2 * bar, 0), "clips cannot move before bar 1");

  // Merged: one clip from the first's start to the last's end, notes kept
  // where they sounded.
  require(!session->midi_clips().merge_midi_clips(group), "merging across tracks is refused");
  const auto merged = take(session->midi_clips().merge_midi_clips(std::vector{first, second}));
  const auto& upper_clips = track(*session, upper).midi_clips;
  REQUIRE_EQ(upper_clips.size(), 1, "two clips become one");
  REQUIRE_EQ(upper_clips.front().id, merged, "two clips become one");
  const auto& interval = std::get<project::MusicalInterval>(upper_clips.front().placement);
  REQUIRE_EQ(interval.start.value, 0, "the merge spans both clips");
  REQUIRE_EQ(interval.end.value, 3 * bar, "the merge spans both clips");
  REQUIRE_EQ(upper_clips.front().notes.size(), 2, "notes keep their place in the song");
  REQUIRE_EQ(upper_clips.front().notes.back().start.value, 2 * bar + timeline::ticks_per_quarter,
             "notes keep their place in the song");
  accepted(session->history().undo());
  REQUIRE_EQ(track(*session, upper).midi_clips.size(), 2, "a merge is one undo step");

  // Deleted together.
  accepted(session->clipboard().remove_clips(group));
  REQUIRE_EQ(track(*session, upper).midi_clips.size(), 1, "the group is deleted");
  REQUIRE_TRUE(track(*session, lower).midi_clips.empty(), "the group is deleted");
  accepted(session->history().undo());
  REQUIRE_EQ(track(*session, lower).midi_clips.size(), 1, "a group delete is one undo step");
}

void notes_copy_paste() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto synth = take(session->instruments().add_synth_track());
  accepted(session->midi_clips().add_midi_clip(synth, {0}));
  const auto clip = track(*session, synth).midi_clips.front().id;
  accepted(session->notes().add_note(clip, {0}, 60));
  accepted(session->notes().add_note(clip, {timeline::ticks_per_quarter}, 64));
  std::vector<project::NoteId> notes;
  std::int64_t selection_end = 0;
  for (const auto& note : track(*session, synth).midi_clips.front().notes) {
    notes.push_back(note.id);
    selection_end = std::max(selection_end, note.end.value);
  }
  const auto duplicated = take(session->clipboard().duplicate_notes(clip, notes));
  const auto& after = track(*session, synth).midi_clips.front().notes;
  REQUIRE_EQ(duplicated.size(), 2, "duplicate notes adds two");
  REQUIRE_EQ(after.size(), 4, "duplicate notes adds two");
  require(std::ranges::any_of(after,
                              [&](const auto& note) {
                                return note.key == 60 && note.start.value == selection_end;
                              }),
          "duplicated notes follow the selection's end");
  accepted(session->clipboard().copy_notes(clip, std::vector{notes.front()}));
  (void)take(session->clipboard().paste_notes(clip, {3 * timeline::ticks_per_quarter + 120}));
  REQUIRE_EQ(track(*session, synth).midi_clips.front().notes.size(), 5, "notes paste");
}
// Ctrl+D on a track: the copy lands right below it as one undo step, with
// fresh identities everywhere and its lanes bound to its own slot and send.
void tracks_duplicate_below_themselves() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto synth = take(session->instruments().add_synth_track());
  const auto below = take(session->instruments().add_synth_track());
  const auto reverb = take(session->tracks().add_bus_track(TrackKind::return_bus));
  const auto slot = take(session->effects().add_effect_engine(synth, 0, "contour"));
  (void)take(session->parameter_automation().set_effect_automation_point(
      synth, slot, "contour.cutoff", 0, 200.0));
  (void)take(session->parameter_automation().set_channel_level_automation_point(synth, 0, 0.5));
  accepted(session->routing().set_send_level(synth, reverb, 0.5));
  const auto send = track(*session, synth).sends.front().id;
  (void)take(session->parameter_automation().set_send_gain_automation_point(send, 0, 0.25));
  accepted(session->midi_clips().add_midi_clip(synth, {0}));
  const auto clip = track(*session, synth).midi_clips.front().id;
  accepted(session->notes().add_note(clip, {0}, 60));
  accepted(session->tracks().set_track_name(synth, "Lead"));
  accepted(session->tracks().set_track_level_pan(synth, 0.5, -0.25));

  const auto copy = take(session->tracks().duplicate_track(synth));
  {
    const auto& tracks = session->snapshot().tracks;
    REQUIRE_EQ(tracks.size(), 4, "the copy lands directly below the original");
    REQUIRE_EQ(tracks[0].id, synth, "the copy lands directly below the original");
    REQUIRE_EQ(tracks[1].id, copy, "the copy lands directly below the original");
    REQUIRE_EQ(tracks[2].id, below, "the copy lands directly below the original");
    const auto& original = track(*session, synth);
    const auto& duplicate = track(*session, copy);
    REQUIRE_EQ(duplicate.kind, original.kind,
               "the copy keeps the instrument, name and mixer settings");
    REQUIRE_EQ(duplicate.instrument_source, original.instrument_source,
               "the copy keeps the instrument, name and mixer settings");
    REQUIRE_EQ(duplicate.name, "Lead", "the copy keeps the instrument, name and mixer settings");
    REQUIRE_EQ(duplicate.level, 0.5, "the copy keeps the instrument, name and mixer settings");
    REQUIRE_EQ(duplicate.pan, -0.25, "the copy keeps the instrument, name and mixer settings");
    REQUIRE_EQ(duplicate.effects.size(), 1, "the effect is copied into a fresh slot");
    REQUIRE_NE(duplicate.effects[0].id, slot, "the effect is copied into a fresh slot");
    REQUIRE_EQ(duplicate.effects[0].processor, original.effects[0].processor,
               "the effect is copied into a fresh slot");
    REQUIRE_EQ(duplicate.sends.size(), 1, "the send is copied under a fresh identity");
    REQUIRE_NE(duplicate.sends[0].id, send, "the send is copied under a fresh identity");
    REQUIRE_EQ(duplicate.sends[0].destination, reverb, "the send is copied under a fresh identity");
    REQUIRE_EQ(duplicate.sends[0].gain, 0.5, "the send is copied under a fresh identity");
    const auto& copied_clip = duplicate.midi_clips.at(0);
    REQUIRE_EQ(duplicate.midi_clips.size(), 1, "clips and notes are copied under fresh identities");
    REQUIRE_NE(copied_clip.id, clip, "clips and notes are copied under fresh identities");
    REQUIRE_EQ(copied_clip.notes.size(), 1, "clips and notes are copied under fresh identities");
    REQUIRE_NE(copied_clip.notes[0].id, original.midi_clips[0].notes[0].id,
               "clips and notes are copied under fresh identities");
    REQUIRE_EQ(copied_clip.notes[0].key, 60, "clips and notes are copied under fresh identities");
    REQUIRE_EQ(duplicate.automation.size(), original.automation.size(),
               "every track lane is copied");
    REQUIRE_EQ(duplicate.automation.size(), 3, "every track lane is copied");
    for (const auto& lane : duplicate.automation) {
      for (const auto& source : original.automation) {
        REQUIRE_NE(lane.id, source.id, "copied lanes get fresh identities");
        for (const auto& point : lane.points) {
          for (const auto& source_point : source.points)
            REQUIRE_NE(point.id, source_point.id, "copied points get fresh identities");
        }
      }
      if (const auto* effect = std::get_if<project::NativeEffectParameterTarget>(&lane.target))
        REQUIRE_EQ(effect->slot, duplicate.effects[0].id, "effect lanes follow the copy's slot");
      else if (const auto* gain = std::get_if<project::SendGainTarget>(&lane.target))
        REQUIRE_EQ(gain->send, duplicate.sends[0].id, "send lanes follow the copy's send");
      else if (const auto* control = std::get_if<project::TrackControlTarget>(&lane.target))
        REQUIRE_EQ(control->track, copy, "channel lanes follow the copy");
      else
        require(false, "no other lane was copied");
    }
  }

  accepted(session->history().undo());
  const auto& tracks = session->snapshot().tracks;
  REQUIRE_EQ(tracks.size(), 3, "one undo removes the copy");
  REQUIRE_EQ(tracks[0].id, synth, "one undo removes the copy");
  REQUIRE_EQ(tracks[1].id, below, "one undo removes the copy");
  const auto missing = session->tracks().duplicate_track(copy);
  REQUIRE_TRUE(!missing, "an unknown track cannot be duplicated");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::not_found,
             "an unknown track cannot be duplicated");
  const auto bus = session->tracks().duplicate_track(reverb);
  REQUIRE_TRUE(!bus, "buses are not duplicated");
  REQUIRE_EQ(bus.error().code, foundation::ErrorCode::unsupported, "buses are not duplicated");
}

} // namespace

int main() {
  try {
    new_projects_get_reverb_and_delay_returns();
    existing_projects_are_not_seeded();
    names_colours_and_markers();
    clips_copy_paste_and_duplicate();
    clips_move_merge_and_delete_together();
    notes_copy_paste();
    tracks_duplicate_below_themselves();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "arrangement basics contract passed\n";
  return 0;
}
