#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

// ADR-0093: a MIDI note or clip edit, a mute and a burst of edits made while
// playing switch at one block boundary. Each case plays, edits mid-note and
// plays on, then proves the edit seamless against an independent oracle: a
// fresh run of the edited song (or, for a removed note, of the song with
// that note ending at the switching block) must match sample for sample.
// That holds only if the new content is heard from the activation block, no
// block was held or silent and nothing clicked. The revision must become
// active with no restart.
using oscillate::test::require;
using oscillate::test::take;

constexpr std::size_t block_frames = 64;
constexpr std::size_t channels = 2;
// The song, in blocks. The first clip holds a note across the edit block
// and a later note; the second clip, after it, holds one more.
constexpr std::uint64_t first_clip_blocks = 240;
constexpr std::uint64_t held_end_block = 150;
constexpr std::uint64_t later_note_block = 180;
constexpr std::uint64_t second_clip_block = 250;
constexpr std::uint64_t second_clip_blocks = 60;
constexpr std::uint64_t note_blocks = 30;
constexpr std::uint64_t song_blocks = 400;
// Every edit lands at this block, inside the held note.
constexpr std::uint64_t edit_block = 100;
// Where edits put notes and clips, all after the edit block.
constexpr std::uint64_t added_note_block = 200;
constexpr std::uint64_t moved_note_block = 120;
constexpr std::uint64_t duplicate_block = 320;
constexpr std::uint64_t moved_clip_block = 330;
// Where a muted track has long been silent.
constexpr std::uint64_t muted_block = 199;
// The blocks each side of the switch that must all sound.
constexpr std::uint64_t around_blocks = 4;
// Bit-identical renders; the margin only absorbs float printing.
constexpr double match_tolerance = 1e-6;
// The switch's sample step may be at most this multiple of the largest
// step the steady note makes just before it (ADR-0038's transient bound).
constexpr double click_ratio = 2.0;
// A block quieter than this (mean square) is silent.
constexpr double silent_power = 1e-12;
constexpr std::uint8_t held_key = 60;
constexpr std::uint8_t added_key = 64;
constexpr std::uint8_t later_key = 67;
constexpr std::uint8_t second_key = 72;
constexpr std::uint8_t velocity = 100;
// Fresh ids for notes and clips an edit adds.
constexpr std::uint64_t edit_id_high = 0x6c697665;
constexpr std::uint64_t edit_clip_serial = 1'000;
// A mute or level change glides over this many frames, as in the app.
constexpr std::uint32_t gain_ramp_frames = 128;
constexpr std::uint64_t gain_ramp_blocks = gain_ramp_frames / block_frames;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-routed-live-edit") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

engine::RoutedMixerLimits limits() {
  engine::RoutedMixerLimits result{{1, block_frames},
                                   block_frames,
                                   6ULL * block_frames * sizeof(engine::StereoFrame),
                                   5ULL * (block_frames + 1) * sizeof(engine::StereoFrame)};
  result.gain_ramp_frames = gain_ramp_frames;
  return result;
}

template <typename Id> Id fresh(std::uint64_t serial) {
  return take(Id::from_bits({edit_id_high, serial}));
}

timeline::TickPosition tick(const project::Snapshot& snapshot, std::uint64_t block) {
  return take(snapshot.tempo.sample_to_tick({static_cast<std::int64_t>(block * block_frames)},
                                            snapshot.sample_rate));
}

project::MidiNote note(const project::Snapshot& snapshot, std::uint8_t key, std::uint64_t from,
                       std::uint64_t to) {
  return {take(project::NoteId::from_bits({0, key})), tick(snapshot, from), tick(snapshot, to), key,
          velocity};
}

const project::MidiClip& midi_clip(const project::Snapshot& snapshot, project::ClipId id) {
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find(track.midi_clips, id, &project::MidiClip::id);
    if (found != track.midi_clips.end())
      return *found;
  }
  throw std::runtime_error("no such MIDI clip");
}

struct SongFiles {
  Directory directory;
  fs::path root = directory.path() / "song";
};

struct SongContent {
  project::Snapshot saved;
  project::TrackId track;
  project::ClipId first;
  project::ClipId second;
  project::MidiNote held;
  project::MidiNote later;
};

// One Prism track with the two clips above.
SongContent write_song(const fs::path& root) {
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  const auto start = session->snapshot();
  const auto first = take(session->midi_clips().add_midi_clip_with_notes(
      track, {0}, tick(start, first_clip_blocks),
      {note(start, held_key, 0, held_end_block),
       note(start, later_key, later_note_block, later_note_block + note_blocks)}));
  const auto with_first = session->snapshot();
  const auto second = take(session->midi_clips().add_midi_clip_with_notes(
      track, tick(with_first, second_clip_block), tick(with_first, second_clip_blocks),
      {note(with_first, second_key, 0, note_blocks)}));
  REQUIRE_OK(session->saving().save(), "the song did not save");
  auto saved = session->snapshot();
  const auto& notes = midi_clip(saved, first).notes;
  const auto held = *std::ranges::find(notes, held_key, &project::MidiNote::key);
  const auto later = *std::ranges::find(notes, later_key, &project::MidiNote::key);
  return {std::move(saved), track, first, second, held, later};
}

struct Song : SongFiles, SongContent {
  Song() : SongContent(write_song(root)) {}

  // `clip` shifted to start at `block`.
  [[nodiscard]] project::Placement placed(const project::MidiClip& clip,
                                          std::uint64_t block) const {
    const auto& musical = std::get<project::MusicalInterval>(clip.placement);
    const auto start = tick(saved, block);
    return project::MusicalInterval{start, {start.value + musical.end.value - musical.start.value}};
  }
};

// A playing run: a model, its routed control and source, and every output
// sample (left channel) rendered so far.
struct Run {
  Gate gate;
  project::ProjectModel model;
  app::RoutedPlaybackControl control;
  std::unique_ptr<app::RoutedAudioSource> source;
  audio::TransportCursor transport;
  std::vector<float> left;

  Run(const fs::path& root, const project::Snapshot& song)
      : model(take(project::ProjectModel::open(song, gate))), control(model, limits()) {
    REQUIRE_OK(control.submit_current(), "the graph did not prepare");
    source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, block_frames));
    control.bind_live_source(*source);
  }
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;
  ~Run() { control.unbind_live_source(); }

  // Renders up to block `until`, observing between blocks as the app's
  // status poll does; no block may hold, fail or allocate.
  void play_to(std::uint64_t until) {
    const auto device = source->render_source();
    std::array<engine::StereoFrame, block_frames> stereo{};
    std::array<float, block_frames * channels> samples{};
    while (left.size() < until * block_frames) {
      counted_allocations = 0;
      counting_allocations = true;
      const auto rendered = audio::render_transport_block(device, transport, true, stereo, samples);
      counting_allocations = false;
      REQUIRE_EQ(rendered, audio::BlockResult::rendered, "a block was held or failed to render");
      REQUIRE_EQ(counted_allocations, 0, "a callback allocated");
      for (std::size_t frame = 0; frame < block_frames; ++frame)
        left.push_back(samples[frame * channels]);
      control.observe();
    }
  }

  // Applies `operations` as the playing project's next revision and offers it.
  std::uint64_t edit(const std::vector<project::Operation>& operations, std::string_view label) {
    auto applied = take(model.apply(operations));
    const auto begun = control.begin_edit(applied, label);
    REQUIRE_OK(begun, std::string(label) + " was refused while playing: " +
                          (begun ? std::string() : begun.error().message));
    REQUIRE_NE(control.status().application, app::PlaybackApplication::failed,
               std::string(label) + " was reported failed");
    return applied.snapshot->revision;
  }
};

// The song with `operations` applied, played from the start.
std::vector<float> reference(const Song& song, const std::vector<project::Operation>& operations) {
  Gate gate;
  auto model = take(project::ProjectModel::open(song.saved, gate));
  auto snapshot = operations.empty() ? model.snapshot() : take(model.apply(operations)).snapshot;
  Run run(song.root, *snapshot);
  run.play_to(song_blocks);
  return std::move(run.left);
}

double block_power(std::span<const float> samples, std::uint64_t block) {
  double power = 0.0;
  for (const float sample : samples.subspan(block * block_frames, block_frames))
    power += static_cast<double>(sample) * sample;
  return power / static_cast<double>(block_frames);
}

// The largest step between neighbouring samples of `window`.
double largest_step(std::span<const float> window) {
  double largest = 0.0;
  for (std::size_t index = 1; index < window.size(); ++index)
    largest = std::max(largest, std::abs(static_cast<double>(window[index]) - window[index - 1]));
  return largest;
}

// The switch at edit_block is continuous: every block around it (up to
// `after` blocks on) sounds and its first sample steps no further than the
// steady note does.
void require_continuous(std::span<const float> samples, std::string_view label,
                        std::uint64_t after = around_blocks) {
  for (auto block = edit_block - around_blocks; block < edit_block + after; ++block)
    REQUIRE_GT(block_power(samples, block), silent_power,
               std::string(label) + ": a silent block at the switch");
  const auto at = edit_block * block_frames;
  const auto steady = largest_step(
      samples.subspan(at - around_blocks * block_frames, around_blocks * block_frames));
  REQUIRE_LE(std::abs(static_cast<double>(samples[at]) - samples[at - 1]), click_ratio * steady,
             std::string(label) + ": the output jumps at the switch");
}

void require_matches(std::span<const float> live, std::span<const float> expected,
                     std::string_view label) {
  REQUIRE_EQ(live.size(), expected.size(), std::string(label) + ": runs differ in length");
  double largest = 0.0;
  std::size_t worst = 0;
  for (std::size_t index = 0; index < live.size(); ++index) {
    const auto difference = std::abs(static_cast<double>(live[index]) - expected[index]);
    if (difference > largest) {
      largest = difference;
      worst = index;
    }
  }
  REQUIRE_LE(largest, match_tolerance,
             std::string(label) + ": differs from the edited song at sample " +
                 std::to_string(worst));
}

// An edit made while playing, and what the song must then sound like from
// the start: the edit itself, or how it sounds from the switch on.
struct LiveEdit {
  std::string_view label;
  std::vector<project::Operation> operations;
  std::vector<project::Operation> expected;
};

// Plays to the edit block, applies the edit, plays on, and checks the run
// against a fresh render of the song with the expected operations applied.
void check_live_edit(const Song& song, const LiveEdit& live_edit) {
  const auto label = live_edit.label;
  Run run(song.root, song.saved);
  run.play_to(edit_block);
  const auto revision = run.edit(live_edit.operations, label);
  run.play_to(song_blocks);
  REQUIRE_EQ(run.control.status().active_graph_revision, revision,
             std::string(label) + " did not become active without a restart");
  REQUIRE_EQ(run.control.status().application, app::PlaybackApplication::active,
             std::string(label) + " did not become active without a restart");
  require_matches(run.left, reference(song, live_edit.expected), label);
  require_continuous(run.left, label);
}

project::MidiNote moved(const Song& song, project::MidiNote note, std::uint64_t block) {
  const auto start = tick(song.saved, block);
  note.end = {start.value + note.end.value - note.start.value};
  note.start = start;
  return note;
}

void note_add_is_heard_from_its_block() {
  Song song;
  const project::MidiNote added{fresh<project::NoteId>(1), tick(song.saved, added_note_block),
                                tick(song.saved, added_note_block + note_blocks), added_key,
                                velocity};
  const std::vector<project::Operation> edit{project::AddMidiNote{song.first, added}};
  check_live_edit(song, {"note add", edit, edit});
}

void note_move_plays_at_its_new_place() {
  Song song;
  const std::vector<project::Operation> edit{
      project::SetMidiNote{song.first, moved(song, song.later, moved_note_block)}};
  check_live_edit(song, {"note move", edit, edit});
}

void note_delete_releases_the_sounding_note() {
  Song song;
  // The deleted note sounds as if it had ended at the switch.
  auto ended = song.held;
  ended.end = tick(song.saved, edit_block);
  check_live_edit(song, {"note delete",
                         {project::RemoveMidiNote{song.first, song.held.id}},
                         {project::SetMidiNote{song.first, ended}}});
}

void clip_duplicate_plays_its_copy() {
  Song song;
  auto copy = midi_clip(song.saved, song.second);
  copy.id = fresh<project::ClipId>(edit_clip_serial);
  copy.placement = song.placed(copy, duplicate_block);
  for (std::size_t index = 0; index < copy.notes.size(); ++index)
    copy.notes[index].id = fresh<project::NoteId>(index + 1);
  const std::vector<project::Operation> edit{project::AddMidiClip{song.track, copy}};
  check_live_edit(song, {"clip duplicate", edit, edit});
}

void clip_move_plays_at_its_new_place() {
  Song song;
  const std::vector<project::Operation> edit{project::SetMidiClipPlacement{
      song.second, song.placed(midi_clip(song.saved, song.second), moved_clip_block)}};
  check_live_edit(song, {"clip move", edit, edit});
}

void clip_delete_releases_its_sounding_note() {
  Song song;
  // The held note ends at the switch; nothing later in the clip plays.
  auto ended = song.held;
  ended.end = tick(song.saved, edit_block);
  check_live_edit(song, {"clip delete",
                         {project::RemoveClip{song.first}},
                         {project::SetMidiNote{song.first, ended},
                          project::RemoveMidiNote{song.first, song.later.id}}});
}

void track_mute_fades_without_a_gap() {
  Song song;
  Run run(song.root, song.saved);
  run.play_to(edit_block);
  const auto revision = run.edit({project::SetTrackMuted{song.track, true}}, "track mute");
  run.play_to(muted_block + 1);
  REQUIRE_EQ(run.control.status().active_graph_revision, revision,
             "the mute did not become active without a restart");
  const auto unmuted = reference(song, {});
  // Untouched before the switch, then a fade to silence, never a cut.
  require_matches(std::span(run.left).first(edit_block * block_frames),
                  std::span(unmuted).first(edit_block * block_frames), "track mute");
  require_continuous(run.left, "track mute", gain_ramp_blocks);
  REQUIRE_LT(block_power(run.left, muted_block), silent_power, "the muted track still sounds");
}

void rapid_edits_coalesce_onto_the_newest_revision() {
  Song song;
  Run run(song.root, song.saved);
  run.play_to(edit_block);
  const project::MidiNote added{fresh<project::NoteId>(1), tick(song.saved, added_note_block),
                                tick(song.saved, added_note_block + note_blocks), added_key,
                                velocity};
  // Four edits before a block renders: each is accepted, none refused; the
  // mute and unmute wait for the note edits, then cancel out.
  (void)run.edit({project::AddMidiNote{song.first, added}}, "rapid note add");
  (void)run.edit({project::SetMidiNote{song.first, moved(song, added, moved_note_block)}},
                 "rapid note move");
  (void)run.edit({project::SetTrackMuted{song.track, true}}, "rapid mute");
  const auto newest = run.edit({project::SetTrackMuted{song.track, false}}, "rapid unmute");
  run.play_to(song_blocks);
  REQUIRE_EQ(run.control.status().active_graph_revision, newest,
             "the newest of the rapid edits did not become active");
  REQUIRE_EQ(run.control.status().application, app::PlaybackApplication::active,
             "the newest of the rapid edits did not become active");
  require_matches(
      run.left,
      reference(song, {project::AddMidiNote{song.first, moved(song, added, moved_note_block)}}),
      "rapid edits");
  require_continuous(run.left, "rapid edits");
}

void an_edit_waiting_behind_a_note_edit_applies_next() {
  Song song;
  Run run(song.root, song.saved);
  run.play_to(edit_block);
  (void)run.edit({project::RemoveMidiNote{song.first, song.later.id}}, "note delete");
  const auto muted = run.edit({project::SetTrackMuted{song.track, true}}, "mute after it");
  run.play_to(muted_block + 1);
  REQUIRE_EQ(run.control.status().active_graph_revision, muted,
             "the mute waiting behind the note edit did not apply");
  REQUIRE_LT(block_power(run.left, muted_block), silent_power, "the muted track still sounds");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "routed live edit contract passed",
      {{"note_add_is_heard_from_its_block", note_add_is_heard_from_its_block},
       {"note_move_plays_at_its_new_place", note_move_plays_at_its_new_place},
       {"note_delete_releases_the_sounding_note", note_delete_releases_the_sounding_note},
       {"clip_duplicate_plays_its_copy", clip_duplicate_plays_its_copy},
       {"clip_move_plays_at_its_new_place", clip_move_plays_at_its_new_place},
       {"clip_delete_releases_its_sounding_note", clip_delete_releases_its_sounding_note},
       {"track_mute_fades_without_a_gap", track_mute_fades_without_a_gap},
       {"rapid_edits_coalesce_onto_the_newest_revision",
        rapid_edits_coalesce_onto_the_newest_revision},
       {"an_edit_waiting_behind_a_note_edit_applies_next",
        an_edit_waiting_behind_a_note_edit_applies_next}});
}
