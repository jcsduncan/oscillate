#include "oscillate/application/project_session.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>
#include <variant>
#include <vector>

// A Ctrl-drag copies clips to the drop (UQ-14): copies of the clips land
// moved by the drag, keeping their spacing and row offsets, on the same or
// another compatible track; the originals stay; the copies are independent;
// and a copy that cannot land makes no edit at all. Each copy is one undo
// step.
namespace {
using namespace oscillate;
using oscillate::test::require;
using oscillate::test::take;

constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * beat;
constexpr std::int64_t second_clip_start = 2 * bar;
constexpr std::int64_t copy_offset = 4 * bar;
constexpr std::uint8_t original_key = 60;
constexpr std::uint8_t copy_key = 67;
// Half a second of a quiet level: a short audio clip.
constexpr std::size_t source_frames = oscillate::test::sample_rate_48k / 2;
constexpr std::int16_t source_level = 4096;

void accepted(foundation::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-clip-copy") {}
};

const project::Track& track(const application::ProjectSession& session, project::TrackId id) {
  const auto& tracks = session.snapshot().tracks;
  const auto found = std::ranges::find(tracks, id, &project::Track::id);
  REQUIRE_NE(found, tracks.end(), "track exists");
  return *found;
}

std::int64_t start_tick(const application::ProjectSession& session,
                        const project::Placement& placement) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement))
    return musical->start.value;
  const auto& snapshot = session.snapshot();
  return take(snapshot.tempo.sample_to_tick(std::get<project::ElapsedInterval>(placement).start,
                                            snapshot.sample_rate))
      .value;
}

const project::MidiClip& midi_clip(const application::ProjectSession& session, project::ClipId id) {
  for (const auto& item : session.snapshot().tracks) {
    const auto found = std::ranges::find(item.midi_clips, id, &project::MidiClip::id);
    if (found != item.midi_clips.end())
      return *found;
  }
  throw std::runtime_error("MIDI clip missing");
}

std::size_t clip_count(const application::ProjectSession& session) {
  std::size_t count = 0;
  for (const auto& item : session.snapshot().tracks)
    count += item.clips.size() + item.midi_clips.size();
  return count;
}

void midi_copy_is_independent_and_one_undo_step() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto synth = take(session->instruments().add_synth_track());
  accepted(session->midi_clips().add_midi_clip(synth, {0}));
  const auto original = track(*session, synth).midi_clips.front().id;
  accepted(session->notes().add_note(original, {0}, original_key));
  const auto before = midi_clip(*session, original);

  const auto revision = session->snapshot().revision;
  const auto copies = take(session->clipboard().copy_clips_by(std::vector{original}, bar, 0));
  REQUIRE_EQ(session->snapshot().revision, revision + 1, "a copy is one edit");
  REQUIRE_EQ(copies.size(), 1, "one clip copied");
  const auto copy = copies.front();
  REQUIRE_NE(copy, original, "the copy is a new clip");
  REQUIRE_TRUE(midi_clip(*session, original) == before, "the original stays as it was");
  const auto& copied = midi_clip(*session, copy);
  REQUIRE_EQ(start_tick(*session, copied.placement), bar, "the copy lands at the drop");
  REQUIRE_EQ(copied.notes.size(), before.notes.size(), "the copy has the original's notes");
  REQUIRE_NE(copied.notes.front().id, before.notes.front().id, "the copy's notes are new notes");

  // Editing the copy leaves the original alone.
  accepted(session->notes().add_note(copy, {beat}, copy_key));
  REQUIRE_TRUE(midi_clip(*session, original) == before, "editing the copy changed the original");

  accepted(session->history().undo());
  accepted(session->history().undo());
  REQUIRE_EQ(track(*session, synth).midi_clips.size(), 1, "one undo takes the copy back");
  REQUIRE_TRUE(midi_clip(*session, original) == before, "undo leaves the original");
}

void selection_copies_keep_their_spacing_and_rows() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto upper = take(session->instruments().add_synth_track());
  const auto lower = take(session->instruments().add_synth_track());
  const auto below = take(session->instruments().add_synth_track());
  accepted(session->midi_clips().add_midi_clip(upper, {0}));
  accepted(session->midi_clips().add_midi_clip(lower, {second_clip_start}));
  const std::vector clips{track(*session, upper).midi_clips.front().id,
                          track(*session, lower).midi_clips.front().id};

  const auto revision = session->snapshot().revision;
  const auto copies = take(session->clipboard().copy_clips_by(clips, copy_offset, 1));
  REQUIRE_EQ(session->snapshot().revision, revision + 1, "a group copy is one edit");
  REQUIRE_EQ(copies.size(), clips.size(), "every selected clip copied");
  REQUIRE_EQ(track(*session, upper).midi_clips.size(), 1, "the originals stay");
  REQUIRE_EQ(track(*session, lower).midi_clips.size(), 2, "the upper copy lands a row down");
  REQUIRE_EQ(track(*session, below).midi_clips.size(), 1, "the lower copy lands a row down");
  REQUIRE_EQ(start_tick(*session, track(*session, lower).midi_clips.back().placement), copy_offset,
             "the upper clip's copy moves by the drag");
  REQUIRE_EQ(start_tick(*session, track(*session, below).midi_clips.front().placement),
             second_clip_start + copy_offset, "the copies keep their spacing");
}

void audio_copies_to_another_audio_track() {
  Directory directory;
  const auto folder = directory.path();
  auto session = take(application::ProjectSession::create(folder / "song"));
  const auto source = folder / "source.wav";
  const std::vector<std::int16_t> samples(source_frames, source_level);
  oscillate::test::write_wav_pcm16(source, oscillate::test::sample_rate_48k,
                                   oscillate::test::mono_channels, samples);
  for (int imported = 0; imported < 2; ++imported) {
    const int descriptor = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    require(descriptor >= 0, "WAV fixture open failed");
    const auto result = session->imports().import_wav_audio(descriptor);
    (void)::close(descriptor);
    require(result.has_value(), "audio import failed");
  }
  const auto& tracks = session->snapshot().tracks;
  REQUIRE_EQ(tracks.size(), 2, "two audio tracks");
  const auto from = tracks[0].id;
  const auto to = tracks[1].id;
  const auto clip = tracks[0].clips.front();
  const auto copies = take(session->clipboard().copy_clips_by(std::vector{clip.id}, bar, 1));
  REQUIRE_EQ(track(*session, from).clips.size(), 1, "the original stays");
  REQUIRE_EQ(track(*session, to).clips.size(), 2, "the copy lands on the next audio track");
  const auto& copy = track(*session, to).clips.back();
  REQUIRE_EQ(copy.id, copies.front(), "the copy is the new clip");
  REQUIRE_EQ(copy.source, clip.source, "the copy plays the same source");
  REQUIRE_TRUE(copy.source_range == clip.source_range, "the copy plays the same audio");
  REQUIRE_EQ(start_tick(*session, copy.placement), start_tick(*session, clip.placement) + bar,
             "the copy lands at the drop");
}

void refused_copies_make_no_edit() {
  Directory directory;
  const auto folder = directory.path();
  auto session = take(application::ProjectSession::create(folder / "song"));
  const auto synth = take(session->instruments().add_synth_track());
  const auto source = folder / "source.wav";
  const std::vector<std::int16_t> samples(source_frames, source_level);
  oscillate::test::write_wav_pcm16(source, oscillate::test::sample_rate_48k,
                                   oscillate::test::mono_channels, samples);
  const int descriptor = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "WAV fixture open failed");
  const auto imported = session->imports().import_wav_audio(descriptor);
  (void)::close(descriptor);
  require(imported.has_value(), "audio import failed");
  accepted(session->midi_clips().add_midi_clip(synth, {0}));
  const std::vector clip{track(*session, synth).midi_clips.front().id};
  REQUIRE_EQ(session->snapshot().tracks.front().id, synth, "the instrument track is on top");

  const auto revision = session->snapshot().revision;
  const auto clips = clip_count(*session);
  REQUIRE_FALSE(session->clipboard().copy_clips_by(clip, bar, 1),
                "a MIDI copy onto an audio track is refused");
  REQUIRE_FALSE(session->clipboard().copy_clips_by(clip, bar, -1),
                "a copy above the first track is refused");
  REQUIRE_FALSE(session->clipboard().copy_clips_by(clip, -bar, 0),
                "a copy before bar 1 is refused");
  REQUIRE_FALSE(session->clipboard().copy_clips_by(clip, beat, 0),
                "a copy overlapping a clip is refused");
  REQUIRE_EQ(session->snapshot().revision, revision, "refused copies make no edit");
  REQUIRE_EQ(clip_count(*session), clips, "refused copies add no clip");
}

} // namespace

int main() {
  try {
    midi_copy_is_independent_and_one_undo_step();
    selection_copies_keep_their_spacing_and_rows();
    audio_copies_to_another_audio_track();
    refused_copies_make_no_edit();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "clip copy-drag contract passed\n";
  return 0;
}
