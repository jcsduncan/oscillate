#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

// ADR-0045: a Standard MIDI File becomes tracks, clips, pedal lanes and
// optionally tempo and meter — with exact ticks — as one undo step.
namespace {
using namespace oscillate;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using application::MidiImportOptions;
constexpr std::int64_t tpq = timeline::ticks_per_quarter;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-midi-import") {}
};

void vlq(Bytes& out, std::uint32_t value) {
  std::uint8_t buffer[4];
  int count = 0;
  buffer[count++] = static_cast<std::uint8_t>(value & 0x7FU);
  while ((value >>= 7U) != 0)
    buffer[count++] = static_cast<std::uint8_t>((value & 0x7FU) | 0x80U);
  while (count > 0)
    out.push_back(buffer[--count]);
}

void u32(Bytes& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

struct TrackBytes {
  Bytes body;
  TrackBytes& at(std::uint32_t delta, std::initializer_list<std::uint8_t> event) {
    vlq(body, delta);
    body.insert(body.end(), event.begin(), event.end());
    return *this;
  }
  TrackBytes& tempo(std::uint32_t delta, std::uint32_t microseconds) {
    return at(delta, {0xFF, 0x51, 0x03, static_cast<std::uint8_t>(microseconds >> 16U),
                      static_cast<std::uint8_t>(microseconds >> 8U),
                      static_cast<std::uint8_t>(microseconds)});
  }
  TrackBytes& meter(std::uint32_t delta, std::uint8_t numerator, std::uint8_t power) {
    return at(delta, {0xFF, 0x58, 0x04, numerator, power, 24, 8});
  }
  TrackBytes& end(std::uint32_t delta = 0) { return at(delta, {0xFF, 0x2F, 0x00}); }
};

Bytes smf(std::uint16_t format, std::uint16_t division, std::initializer_list<TrackBytes> tracks) {
  Bytes out{'M', 'T', 'h', 'd'};
  u32(out, 6);
  for (const auto value : {format, static_cast<std::uint16_t>(tracks.size()), division}) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
  }
  for (const auto& track : tracks) {
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    u32(out, static_cast<std::uint32_t>(track.body.size()));
    out.insert(out.end(), track.body.begin(), track.body.end());
  }
  return out;
}

const project::Track& track(const application::ProjectSession& session, project::TrackId id) {
  const auto& tracks = session.snapshot().tracks;
  return *std::ranges::find(tracks, id, &project::Track::id);
}

void tracks_tempo_meter_and_exact_ticks() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  TrackBytes conductor;
  conductor.tempo(0, 600'000).meter(0, 3, 2).end(); // 100 BPM, 3/4.
  TrackBytes lead;
  lead.at(0, {0xFF, 0x03, 0x04, 'L', 'e', 'a', 'd'})
      .at(0, {0x90, 60, 100})
      .at(240, {0x80, 60, 0}) // An eighth note at 480 PPQ.
      .at(0, {0x90, 64, 90})
      .at(1440, {0x80, 64, 0}); // Crosses into bar two of 3/4.
  lead.end();
  TrackBytes drums;
  drums.at(0, {0x99, 36, 120}).at(120, {0x89, 36, 0}).end();
  const auto bytes = smf(1, 480, {conductor, lead, drums});
  const auto before = session->snapshot().revision;
  const auto result = take(session->imports().import_midi_bytes(bytes));
  REQUIRE_EQ(session->snapshot().revision, before + 1, "the import is one revision");
  REQUIRE_EQ(result.tracks.size(), 2, "two tracks with one clip each and three notes");
  REQUIRE_EQ(result.clips, 2, "two tracks with one clip each and three notes");
  REQUIRE_EQ(result.notes, 3, "two tracks with one clip each and three notes");
  REQUIRE_TRUE(result.tempo_imported, "tempo and meter imported");
  REQUIRE_TRUE(result.meter_imported, "tempo and meter imported");
  const auto& snapshot = session->snapshot();
  REQUIRE_EQ(snapshot.tempo.events().size(), 1, "the file's tempo replaces the project's");
  REQUIRE_EQ(snapshot.tempo.events()[0].bpm, 100.0, "the file's tempo replaces the project's");
  REQUIRE_EQ(snapshot.meter.events().size(), 1, "the file's meter replaces the project's");
  REQUIRE_EQ(snapshot.meter.events()[0].numerator, 3, "the file's meter replaces the project's");
  REQUIRE_EQ(snapshot.meter.events()[0].denominator, 4, "the file's meter replaces the project's");
  const auto& synth = track(*session, result.tracks[0]);
  const auto& drum = track(*session, result.tracks[1]);
  REQUIRE_EQ(std::get<project::NativeInstrumentPatch>(synth.instrument_source).engine,
             native_dsp::synth::default_engine().descriptor->id,
             "melodic channels play the default synth");
  REQUIRE_EQ(std::get<project::NativeInstrumentPatch>(drum.instrument_source).engine, "pulse",
             "channel 10 plays the drum synth");
  const auto& clip = synth.midi_clips.front();
  const auto& placed = std::get<project::MusicalInterval>(clip.placement);
  REQUIRE_EQ(placed.start.value, 0, "the clip covers whole 3/4 bars");
  REQUIRE_EQ(clip.pattern_length.value, std::int64_t{2} * 3 * tpq,
             "the clip covers whole 3/4 bars");
  REQUIRE_EQ(clip.notes.size(), 2, "note ticks rescale exactly");
  REQUIRE_EQ(clip.notes[0].start.value, 0, "note ticks rescale exactly");
  REQUIRE_EQ(clip.notes[0].end.value, tpq / 2, "note ticks rescale exactly");
  REQUIRE_EQ(clip.notes[0].velocity, 100, "note ticks rescale exactly");
  REQUIRE_EQ(clip.notes[1].start.value, tpq / 2, "note ticks rescale exactly");
  REQUIRE_EQ(clip.notes[1].end.value, tpq / 2 + 3 * tpq, "note ticks rescale exactly");
  REQUIRE_EQ(drum.midi_clips.front().notes.front().end.value, tpq / 4, "drum note length");

  REQUIRE_OK(session->history().undo(), "undo");
  REQUIRE_TRUE(session->snapshot().tracks.empty(), "one undo removes the whole import");
  REQUIRE_EQ(session->snapshot().tempo.events()[0].bpm, 120.0, "one undo removes the whole import");
}

void odd_divisions_do_not_drift() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  TrackBytes notes;
  // 7 PPQ does not divide the project's ticks per quarter.
  for (int i = 0; i < 700; ++i)
    notes.at(0, {0x90, 60, 64}).at(1, {0x80, 60, 0});
  notes.end();
  const auto result = take(session->imports().import_midi_bytes(
      smf(0, 7, {notes}), {MidiImportOptions::Target::new_tracks, std::nullopt, false, 0}));
  const auto& clip = track(*session, result.tracks[0]).midi_clips.front();
  REQUIRE_EQ(clip.notes.size(), 700, "every note");
  // Note i starts at file tick i: each converts from its absolute position,
  // rounded to nearest, so a hundred quarters later nothing has accumulated.
  for (std::int64_t i = 0; i < 700; ++i) {
    REQUIRE_EQ(clip.notes[static_cast<std::size_t>(i)].start.value, (i * tpq * 2 + 7) / 14,
               "file tick " + std::to_string(i) + " converts exactly");
  }
  REQUIRE_EQ(clip.notes[7].start.value, tpq, "whole quarters land exactly on the grid");
  REQUIRE_EQ(clip.notes[693].start.value, 99 * tpq, "whole quarters land exactly on the grid");
}

void pedals_become_lanes_and_chase_across_clips() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  TrackBytes notes;
  notes
      .at(0, {0xB0, 64, 127}) // Sustain down at 0.
      .at(0, {0x90, 60, 80})
      .at(96, {0x80, 60, 0})
      .at(96, {0xB0, 64, 64})  // Half pedal.
      .at(96, {0xB0, 67, 127}) // Soft.
      .at(96, {0xB0, 64, 0})   // Up.
      .at(0, {0xB0, 7, 100})   // Volume: not a pedal.
      .end();
  const auto result = take(session->imports().import_midi_bytes(smf(0, 96, {notes})));
  const auto& clip = track(*session, result.tracks[0]).midi_clips.front();
  const auto sustain =
      std::ranges::find(clip.pedals, project::MidiPedal::sustain, &project::MidiPedalLane::pedal);
  const auto soft =
      std::ranges::find(clip.pedals, project::MidiPedal::soft, &project::MidiPedalLane::pedal);
  REQUIRE_EQ(clip.pedals.size(), 2, "sustain and soft lanes; volume is not a pedal");
  REQUIRE_NE(sustain, clip.pedals.end(), "sustain and soft lanes; volume is not a pedal");
  REQUIRE_NE(soft, clip.pedals.end(), "sustain and soft lanes; volume is not a pedal");
  REQUIRE_EQ(sustain->points.size(), 3, "sustain keeps half-pedal values");
  REQUIRE_EQ(sustain->points[0].value, 1.0, "sustain keeps half-pedal values");
  REQUIRE_EQ(sustain->points[1].position.value, 2 * tpq, "sustain keeps half-pedal values");
  REQUIRE_EQ(sustain->points[1].value, 64.0 / 127.0, "sustain keeps half-pedal values");
  REQUIRE_EQ(sustain->points[2].value, 0.0, "sustain keeps half-pedal values");
  REQUIRE_EQ(result.pedal_points, 4, "pedal points are counted");

  // A dense file splits into several clips; the held pedal is chased into
  // each one.
  Directory dense_directory;
  auto dense = take(application::ProjectSession::create(dense_directory.path() / "song"));
  TrackBytes busy;
  busy.at(0, {0xB0, 64, 127});
  for (int i = 0; i < 5'000; ++i)
    busy.at(0, {0x90, static_cast<std::uint8_t>(40 + i % 40), 70})
        .at(24, {0x80, static_cast<std::uint8_t>(40 + i % 40), 0});
  busy.end();
  const auto split = take(dense->imports().import_midi_bytes(smf(0, 96, {busy})));
  const auto& clips = track(*dense, split.tracks[0]).midi_clips;
  REQUIRE_EQ(clips.size(), 2, "5000 notes split into two clips");
  REQUIRE_EQ(split.notes, 5'000, "5000 notes split into two clips");
  REQUIRE_EQ(clips[0].notes.size(), project::max_midi_notes_per_clip, "the first clip is full");
  const auto& second = std::get<project::MusicalInterval>(clips[1].placement);
  REQUIRE_EQ(second.start.value % (4 * tpq), 0, "clips start on bars and never overlap");
  REQUIRE_GE(second.start, std::get<project::MusicalInterval>(clips[0].placement).end,
             "clips start on bars and never overlap");
  REQUIRE_EQ(clips[1].pedals.size(), 1, "a held pedal is chased into the next clip");
  REQUIRE_EQ(clips[1].pedals[0].points.front().position.value, 0,
             "a held pedal is chased into the next clip");
  REQUIRE_EQ(clips[1].pedals[0].points.front().value, 1.0,
             "a held pedal is chased into the next clip");
}

void into_a_track_at_a_bar_without_tempo() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto target = take(session->instruments().add_synth_track());
  TrackBytes notes;
  notes.tempo(0, 250'000).at(0, {0x90, 60, 80}).at(480, {0x80, 60, 0}).end();
  const auto result = take(session->imports().import_midi_bytes(
      smf(0, 480, {notes}), {MidiImportOptions::Target::track, target, false, 2}));
  REQUIRE_EQ(result.tracks.size(), 1, "into the chosen track, project tempo kept");
  REQUIRE_EQ(result.tracks[0], target, "into the chosen track, project tempo kept");
  REQUIRE_TRUE(!result.tempo_imported, "into the chosen track, project tempo kept");
  REQUIRE_EQ(session->snapshot().tempo.events()[0].bpm, 120.0, "tempo unchanged");
  const auto& clip = track(*session, target).midi_clips.front();
  REQUIRE_EQ(std::get<project::MusicalInterval>(clip.placement).start.value,
             std::int64_t{2} * 4 * tpq, "the file starts at bar three");
  REQUIRE_EQ(clip.notes.front().start.value, 0, "the file starts at bar three");
  REQUIRE_EQ(clip.notes.front().end.value, tpq, "the file starts at bar three");
  // Importing over the same bars collides with the clip already there.
  require(!session->imports().import_midi_bytes(
              smf(0, 480, {notes}), {MidiImportOptions::Target::track, target, false, 2}),
          "overlapping clips are refused");
  const auto audio = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  require(!session->imports().import_midi_bytes(
              smf(0, 480, {notes}), {MidiImportOptions::Target::track, audio, false, 0}),
          "a bus cannot take MIDI");
}

void smpte_files_follow_elapsed_time() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  TrackBytes notes;
  // 25 fps x 40 ticks: 1000 ticks per second; at 120 BPM a second is two
  // quarter notes.
  notes.at(0, {0x90, 60, 80}).at(1'000, {0x80, 60, 0}).end();
  const auto result = take(session->imports().import_midi_bytes(smf(0, 0xE728, {notes})));
  REQUIRE_TRUE(!result.tempo_imported, "SMPTE files keep the tempo map");
  REQUIRE_TRUE(!result.notices.empty(), "SMPTE files keep the tempo map");
  const auto& note = track(*session, result.tracks[0]).midi_clips.front().notes.front();
  REQUIRE_EQ(note.end.value, 2 * tpq, "one second is two quarters at 120 BPM");
}

void files_from_descriptors_and_failures() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  TrackBytes notes;
  notes.at(0, {0x90, 60, 80}).at(96, {0x80, 60, 0}).at(0, {0x80, 61, 0}).end();
  const auto path = directory.path() / "song.mid";
  {
    const auto bytes = smf(0, 96, {notes});
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture opens");
  const auto result = session->imports().import_midi_file(fd);
  ::close(fd);
  REQUIRE_OK(result, "a file descriptor imports");
  REQUIRE_EQ(result.value().notes, 1, "a file descriptor imports");
  require(std::ranges::any_of(result.value().notices,
                              [](const std::string& notice) {
                                return notice.find("note-off") != std::string::npos;
                              }),
          "repairs are reported");
  const auto revision = session->snapshot().revision;
  TrackBytes empty;
  empty.end();
  require(!session->imports().import_midi_bytes(smf(0, 96, {empty})),
          "a file with no notes is refused");
  require(!session->imports().import_midi_bytes(Bytes{1, 2, 3}), "garbage is refused");
  REQUIRE_EQ(session->snapshot().revision, revision, "failures change nothing");
}

} // namespace

int main() {
  try {
    tracks_tempo_meter_and_exact_ticks();
    odd_divisions_do_not_drift();
    pedals_become_lanes_and_chase_across_clips();
    into_a_track_at_a_bar_without_tempo();
    smpte_files_follow_elapsed_time();
    files_from_descriptors_and_failures();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "MIDI import contract passed\n";
  return 0;
}
