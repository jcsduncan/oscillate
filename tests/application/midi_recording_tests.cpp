// MIDI recording (ADR-0086): where a key press lands, the metronome's
// beats, the count-in, and a take written into the song.

#include "oscillate/application/midi_recording.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace application = oscillate::application;
namespace project = oscillate::project;
namespace fs = std::filesystem;
using oscillate::timeline::ticks_per_quarter;

constexpr std::int64_t quarter = ticks_per_quarter;
constexpr std::int64_t eighth = ticks_per_quarter / 2;
constexpr std::int64_t bar = ticks_per_quarter * 4;
constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
// The default song: 120 BPM in 4/4, so a beat is half a second.
constexpr std::uint64_t beat_frames = rate / 2;
constexpr std::int64_t second_ns = 1'000'000'000;

using oscillate::test::require;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-recording") {}
};

// The sample heard at a key press is the device's last heard point moved on
// by the time since, wrapped where the transport loops.
void presses_land_where_they_were_heard() {
  const oscillate::audio_io::TransportCursor::AudiblePoint point{48'000, 10 * second_ns};
  REQUIRE_EQ(application::heard_sample(point, 10 * second_ns + second_ns / 2, rate, std::nullopt),
             72'000, "half a second after the point is 24000 frames later");
  REQUIRE_EQ(application::heard_sample(point, 10 * second_ns - second_ns / 4, rate, std::nullopt),
             36'000, "a press before the point is heard lands before it");
  REQUIRE_EQ(application::heard_sample({100, second_ns}, 0, rate, std::nullopt), 0,
             "never before the song's start");
  // A loop over [0, 60000): 50000 + 24000 runs 14000 past its end.
  const application::LoopSamples loop{0, 60'000};
  REQUIRE_EQ(application::heard_sample({50'000, second_ns}, second_ns + second_ns / 2, rate, loop),
             14'000, "past the loop's end the press wraps to its start");
  REQUIRE_EQ(application::heard_sample({20'000, second_ns}, second_ns, rate, loop), 20'000,
             "inside the loop nothing wraps");
}

// A new song with one synth track.
struct Song {
  Directory directory;
  std::unique_ptr<application::ProjectSession> session;
  std::optional<project::TrackId> track;
  Song() {
    auto opened = application::ProjectSession::create(directory.path() / "song");
    REQUIRE_OK(opened, "project creation failed");
    session = std::move(opened.value());
    auto added = session->instruments().add_synth_track();
    REQUIRE_OK(added, "synth track failed");
    track = added.value();
  }
};

// The metronome clicks each beat, the bar's first accented; a count-in is
// one bar of beats at the tempo where the take starts.
void clicks_follow_the_song() {
  Song song;
  auto& session = song.session;
  const auto& state = session->snapshot();
  const auto beats = application::click_schedule(state, 8 * beat_frames);
  REQUIRE_EQ(beats.size(), 8, "two bars of 4/4 are eight beats");
  for (std::size_t index = 0; index < beats.size(); ++index) {
    REQUIRE_EQ(beats[index].sample, index * beat_frames, "a beat every half second");
    REQUIRE_EQ(beats[index].accent, (index % 4 == 0), "each bar's first beat is accented");
  }
  const auto count_in = application::count_in_at(state, 3 * beat_frames);
  REQUIRE_TRUE(count_in, "a count-in is a bar of four half-second beats");
  REQUIRE_EQ(count_in->beats, 4, "a count-in is a bar of four half-second beats");
  REQUIRE_EQ(count_in->beat_frames, beat_frames, "a count-in is a bar of four half-second beats");
}

const project::Track& track_of(const application::ProjectSession& session, project::TrackId id) {
  for (const auto& track : session.snapshot().tracks)
    if (track.id == id)
      return track;
  throw std::runtime_error("track missing");
}

application::RecordedNote played(std::int64_t start, std::int64_t length, std::uint8_t key) {
  return {{start}, {start + length}, key, 90};
}

// In empty space a take makes its own clip at the first note's bar and
// grows it bar by bar; the whole take is one undo step.
void a_take_makes_and_grows_its_clip() {
  Song song;
  auto& session = song.session;
  const auto track = *song.track;
  constexpr std::uint64_t gesture = 77;
  const std::vector first{played(bar + quarter, eighth, 60)};
  auto made = session->notes().record_notes(track, first, std::nullopt, gesture);
  REQUIRE_TRUE(made, "the first note makes a clip");
  REQUIRE_TRUE(made.value(), "the first note makes a clip");
  const auto take = *made.value();
  {
    const auto& clips = track_of(*session, track).midi_clips;
    REQUIRE_EQ(clips.size(), 1, "one clip");
    const auto& span = std::get<project::MusicalInterval>(clips.front().placement);
    REQUIRE_EQ(span.start.value, bar, "the clip covers the bar the note began in");
    REQUIRE_EQ(span.end.value, 2 * bar, "the clip covers the bar the note began in");
    REQUIRE_EQ(clips.front().notes.size(), 1, "the note sits where it was played, in clip ticks");
    REQUIRE_EQ(clips.front().notes.front().start.value, quarter,
               "the note sits where it was played, in clip ticks");
    REQUIRE_EQ(clips.front().notes.front().end.value, quarter + eighth,
               "the note sits where it was played, in clip ticks");
    REQUIRE_EQ(clips.front().notes.front().velocity, 90,
               "the note sits where it was played, in clip ticks");
  }
  // A note two bars on, held over a bar line: the clip grows to hold it.
  const std::vector later{played(3 * bar + 3 * quarter, quarter + eighth, 64)};
  auto grown = session->notes().record_notes(track, later, take, gesture);
  REQUIRE_TRUE(grown, "the take keeps its clip");
  REQUIRE_EQ(grown.value(), take, "the take keeps its clip");
  {
    const auto& clip = track_of(*session, track).midi_clips.front();
    const auto& span = std::get<project::MusicalInterval>(clip.placement);
    REQUIRE_EQ(span.end.value, 5 * bar, "the clip grows to the bar after the note");
    REQUIRE_EQ(clip.pattern_length.value, 4 * bar, "the clip grows to the bar after the note");
    REQUIRE_EQ(clip.notes.size(), 2, "both notes are in it");
  }
  REQUIRE_OK(session->history().undo(), "one undo removes the whole take");
  REQUIRE_TRUE(track_of(*session, track).midi_clips.empty(), "one undo removes the whole take");
}

// Notes landing in a clip join it; a looping clip folds them into its
// pattern, so each pass layers on the last; a repeat of a note is dropped.
void a_take_overdubs_a_looping_clip() {
  Song song;
  auto& session = song.session;
  const auto track = *song.track;
  REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "clip failed");
  const auto clip = track_of(*session, track).midi_clips.front().id;
  REQUIRE_OK(session->midi_clips().set_midi_clip_loop(clip, true), "a one-bar loop over four bars");
  REQUIRE_OK(session->midi_clips().resize_midi_clip(clip, {4 * bar}),
             "a one-bar loop over four bars");
  constexpr std::uint64_t gesture = 5;
  const std::vector pass{played(eighth, eighth, 60), played(2 * bar + quarter, eighth, 62),
                         played(3 * bar + eighth, eighth, 60),
                         played(3 * bar + 7 * eighth, quarter, 67)};
  auto recorded = session->notes().record_notes(track, pass, std::nullopt, gesture);
  REQUIRE_TRUE(recorded, "no clip of its own is needed");
  REQUIRE_TRUE(!recorded.value(), "no clip of its own is needed");
  const auto& notes = track_of(*session, track).midi_clips.front().notes;
  REQUIRE_EQ(notes.size(), 3, "the repeated note on the next pass is dropped");
  const auto has = [&](std::int64_t start, std::int64_t end, std::uint8_t key) {
    for (const auto& note : notes)
      if (note.start.value == start && note.end.value == end && note.key == key)
        return true;
    return false;
  };
  REQUIRE_TRUE(has(eighth, quarter, 60), "passes fold into the one-bar pattern");
  REQUIRE_TRUE(has(quarter, quarter + eighth, 62), "passes fold into the one-bar pattern");
  require(has(7 * eighth, bar, 67), "a note over the pattern's end stops there");
  REQUIRE_EQ(track_of(*session, track).midi_clips.size(), 1, "no new clip");
}

// The take's clip never grows into the next clip on the track.
void a_take_stops_at_the_next_clip() {
  Song song;
  auto& session = song.session;
  const auto track = *song.track;
  REQUIRE_OK(session->midi_clips().add_midi_clip(track, {2 * bar}), "clip failed");
  const std::vector held{played(quarter, 6 * quarter, 60)};
  auto made = session->notes().record_notes(track, held, std::nullopt, 9);
  REQUIRE_TRUE(made, "a clip is made before the other");
  REQUIRE_TRUE(made.value(), "a clip is made before the other");
  for (const auto& clip : track_of(*session, track).midi_clips) {
    if (clip.id != *made.value())
      continue;
    const auto& span = std::get<project::MusicalInterval>(clip.placement);
    REQUIRE_EQ(span.start.value, 0, "it ends where the next clip starts");
    REQUIRE_EQ(span.end.value, 2 * bar, "it ends where the next clip starts");
    REQUIRE_EQ(clip.notes.front().end.value, 6 * quarter + quarter, "the note fits inside it");
  }
}

} // namespace

int main() {
  try {
    presses_land_where_they_were_heard();
    clicks_follow_the_song();
    a_take_makes_and_grows_its_clip();
    a_take_overdubs_a_looping_clip();
    a_take_stops_at_the_next_clip();
  } catch (const std::exception& error) {
    std::cerr << "midi recording: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
