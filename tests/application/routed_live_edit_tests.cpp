#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/application/routed_source_selection.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
namespace media = oscillate::media;
namespace synth = oscillate::native_dsp::synth;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

// ADR-0093: a MIDI note or clip edit, a mute and a burst of edits made while
// playing switch at one block boundary. ADR-0098: so do edits only a whole
// replacement source and graph carry (a track added or removed, a tempo or
// meter change, new media, a change of instrument, MIDI with other lanes). Each case plays, edits
// mid-note and plays on, then proves the edit seamless against an independent oracle: a fresh run
// of the edited song (or, for a removed note, of the song with that note ending at the switching
// block) must match sample for sample. That holds only if the new content is heard from the
// activation block, no block was held or silent and nothing clicked. The revision must become
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
// Frames each replacement source streams ahead of the callback.
constexpr std::size_t stream_frames = 65'536;
// How long a replacement source may take to prime its first block.
constexpr auto prime_timeout = std::chrono::seconds(10);
constexpr auto prime_poll = std::chrono::milliseconds(1);
// The tempo a live tempo change sets: slower, so the held note is still held.
constexpr double edited_bpm = 96.0;
constexpr std::uint8_t edited_meter_numerator = 7;
constexpr std::uint8_t edited_meter_denominator = 8;
// Blocks after a switch by which a removed track or replaced instrument has
// faded or rung out.
constexpr std::uint64_t settled_blocks = 40;
// The audio clip a media edit adds: where it starts and how long it plays.
constexpr std::uint64_t media_clip_block = 140;
constexpr std::uint64_t media_clip_blocks = 30;
constexpr double media_tone_hz = 330.0;
constexpr double media_tone_level = 0.25;
// The engine an instrument change selects.
constexpr std::string_view replacement_engine = "ember";
// The level a MIDI clip's lane glides between.
constexpr double lane_start_level = 1.0;
constexpr double lane_end_level = 0.5;
// Ids of the fixture's added track and source.
constexpr std::uint64_t edit_track_serial = 2'000;

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
// sample (left channel) rendered so far. It renders as the app does: the
// source selection hands each graph revision its source bank, and a staged
// replacement fills exactly on the block it tries to take over.
struct Run {
  fs::path root;
  Gate gate;
  project::ProjectModel model;
  app::RoutedPlaybackControl control;
  std::shared_ptr<app::RoutedAudioSource> source;
  std::unique_ptr<app::RoutedSourceSelection> selection;
  std::shared_ptr<app::RoutedAudioSource> candidate;
  std::uint64_t exact_revision = 0;
  audio::TransportCursor transport;
  std::vector<float> left;

  Run(fs::path song_root, const project::Snapshot& song)
      : root(std::move(song_root)), model(take(project::ProjectModel::open(song, gate))),
        control(model, limits()) {
    REQUIRE_OK(control.submit_current(), "the graph did not prepare");
    source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, stream_frames));
    control.bind_live_source(*source);
    selection = std::make_unique<app::RoutedSourceSelection>(
        app::RoutedSourceBinding{model.snapshot()->revision, source});
  }
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;
  ~Run() { control.unbind_live_source(); }

  static bool render(void* context, std::span<engine::StereoFrame> output,
                     engine::BlockContext block) noexcept {
    auto& run = *static_cast<Run*>(context);
    auto lease = run.selection->enter_callback();
    struct Fill {
      app::RoutedSourceSelection::CallbackLease& lease;
      std::uint64_t exact;
    } fill{lease, run.exact_revision};
    const std::span<const std::span<const engine::StereoFrame>> unused{};
    return run.control.render_block(
               unused, output, block,
               {&fill, nullptr,
                [](void* context, std::uint64_t revision, engine::BlockContext fill_block,
                   std::size_t frames,
                   std::span<const std::span<const engine::StereoFrame>>& selected) noexcept {
                  auto& state = *static_cast<Fill*>(context);
                  const bool exact = revision == state.exact;
                  auto* bank = static_cast<app::RoutedAudioSource*>(
                      exact ? state.lease.source_for(revision)
                            : state.lease.source_for_graph_revision(revision));
                  if (bank == nullptr)
                    return false;
                  return exact ? bank->fill_source_bank_exact(fill_block, frames, selected)
                               : bank->fill_source_bank(fill_block, frames, selected);
                }}) == engine::RoutedRenderStatus::rendered;
  }

  // After each block, as the app's status poll: an adopted replacement
  // becomes the playing source.
  void observe() {
    control.observe();
    if (candidate && control.status().active_graph_revision == exact_revision &&
        selection->promote_candidate(exact_revision)) {
      source = std::move(candidate);
      control.bind_live_source(*source);
      exact_revision = 0;
    }
  }

  // Renders up to block `until`, observing between blocks as the app's
  // status poll does; no block may hold, fail or allocate.
  void play_to(std::uint64_t until) {
    const audio::StereoRenderSource device{this, &Run::render, nullptr, nullptr};
    std::array<engine::StereoFrame, block_frames> stereo{};
    std::array<float, block_frames * channels> samples{};
    while (left.size() < until * block_frames) {
      await_streamed(*source);
      if (candidate)
        await_streamed(*candidate);
      counted_allocations = 0;
      counting_allocations = true;
      const auto rendered = audio::render_transport_block(device, transport, true, stereo, samples);
      counting_allocations = false;
      REQUIRE_EQ(rendered, audio::BlockResult::rendered, "a block was held or failed to render");
      REQUIRE_EQ(counted_allocations, 0, "a callback allocated");
      for (std::size_t frame = 0; frame < block_frames; ++frame)
        left.push_back(samples[frame * channels]);
      observe();
    }
  }

  // Waits until `bank` holds every clip frame of the next block, so a run
  // rendered faster than real time never outpaces its streaming worker.
  void await_streamed(app::RoutedAudioSource& bank) const {
    const auto position = transport.completed_snapshot();
    const auto deadline = std::chrono::steady_clock::now() + prime_timeout;
    while (bank.prime_handoff_window(position.generation, position.next_sample, block_frames) !=
           media::HandoffWindowStatus::ready) {
      require(std::chrono::steady_clock::now() < deadline, "a source did not stream ahead");
      std::this_thread::sleep_for(prime_poll);
    }
  }

  // Applies `operations` as an edit only a replacement source carries: it is
  // captured, its source prepared and primed for the next block, staged and
  // published as a live switch (ADR-0098).
  std::uint64_t source_edit(const std::vector<project::Operation>& operations,
                            std::string_view label) {
    auto applied = take(model.apply(operations));
    require(control.can_capture_source_edit(*applied.snapshot),
            std::string(label) + " is not offered as a source switch");
    auto captured = control.capture_source_edit(applied, label);
    REQUIRE_OK(captured, std::string(label) + " was refused while playing: " +
                             (captured ? std::string() : captured.error().message));
    const auto& capture = captured.value();
    const auto position = transport.completed_snapshot();
    candidate =
        take(app::RoutedAudioSource::prepare(root, capture, control, stream_frames, nullptr, {},
                                             position.next_sample, position.generation));
    const auto deadline = std::chrono::steady_clock::now() + prime_timeout;
    while (candidate->prime_handoff_window(position.generation, position.next_sample,
                                           block_frames) != media::HandoffWindowStatus::ready) {
      require(std::chrono::steady_clock::now() < deadline,
              std::string(label) + ": the replacement source did not prime");
      std::this_thread::sleep_for(prime_poll);
    }
    const auto revision = applied.snapshot->revision;
    require(selection->stage_candidate({revision, candidate}),
            std::string(label) + ": the replacement source was not staged");
    exact_revision = revision;
    const auto published = control.publish_captured_source(capture);
    REQUIRE_OK(published, std::string(label) + " did not publish: " +
                              (published ? std::string() : published.error().message));
    return revision;
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

// Master rebalance edits (ADR-0123) while playing: the gain, then the EQ,
// then the track's role all reach the running stage without a graph switch
// or a gap, each gliding from where it was; once its stage is neutral again
// the song plays exactly as it did without a rebalance.
void rebalance_edits_glide_without_a_gap() {
  constexpr double boost_db = 6.0;
  constexpr double boost = 2.0; // 6 dB, near enough for a level check
  constexpr double level_tolerance = 0.05;
  constexpr double low_db = 6.0;
  constexpr double mid_db = -4.0;
  constexpr double high_db = 3.0;
  constexpr std::uint64_t eq_block = edit_block + 20;
  constexpr std::uint64_t role_block = held_end_block;
  constexpr std::uint64_t glide_blocks = 16;
  Song song;
  const auto unchanged = reference(song, {});
  Run run(song.root, song.saved);
  run.play_to(edit_block);
  project::MasterRebalance rebalance;
  rebalance.enabled = true;
  auto& music = rebalance.roles[static_cast<std::size_t>(project::MixRole::music)];
  music.gain_db = boost_db;
  auto revision = run.edit({project::SetMasterRebalance{rebalance}}, "rebalance gain");
  run.play_to(eq_block);
  REQUIRE_EQ(run.control.status().active_graph_revision, revision,
             "the gain edit did not become active without a restart");
  require_continuous(run.left, "rebalance gain");
  const auto window = [](const std::vector<float>& samples, std::uint64_t from) {
    double power = 0.0;
    for (auto block = from; block < from + around_blocks; ++block)
      power += block_power(samples, block);
    return power;
  };
  const auto ratio = std::sqrt(window(run.left, eq_block - around_blocks) /
                               window(unchanged, eq_block - around_blocks));
  REQUIRE_NEAR(ratio, boost, level_tolerance * boost, "the role's gain is heard");

  music.low_db = low_db;
  music.mid_db = mid_db;
  music.high_db = high_db;
  revision = run.edit({project::SetMasterRebalance{rebalance}}, "rebalance EQ");
  run.play_to(role_block);
  REQUIRE_EQ(run.control.status().active_graph_revision, revision,
             "the EQ edit did not become active without a restart");
  for (auto block = eq_block; block < eq_block + around_blocks; ++block)
    REQUIRE_GT(block_power(run.left, block), silent_power, "rebalance EQ: a silent block");

  // The track leaves the rebalance: its stage glides back to neutral.
  revision = run.edit({project::SetTrackMixRole{song.track, project::MixRole::none}}, "role");
  run.play_to(song_blocks);
  REQUIRE_EQ(run.control.status().active_graph_revision, revision,
             "the role edit did not become active without a restart");
  const auto settled = (role_block + glide_blocks) * block_frames;
  require_matches(std::span(run.left).subspan(settled), std::span(unchanged).subspan(settled),
                  "a neutral stage plays the song unchanged");
}

// --- Edits a whole replacement carries (ADR-0098) ------------------------------

// The playing run over the whole song, with `edit` made at the edit block.
template <typename Edit> Run& play_with(Run& run, Edit&& edit) {
  run.play_to(edit_block);
  const auto revision = edit(run);
  run.play_to(song_blocks);
  REQUIRE_EQ(run.control.status().active_graph_revision, revision,
             "the edit did not become active without a restart");
  REQUIRE_EQ(run.control.status().application, app::PlaybackApplication::active,
             "the edit did not become active without a restart");
  return run;
}

// A source switch made at the edit block, checked against a fresh render of
// the song with `expected` applied.
void check_source_edit(const Song& song, const LiveEdit& live_edit) {
  Run run(song.root, song.saved);
  play_with(run, [&](Run& playing) {
    return playing.source_edit(live_edit.operations, live_edit.label);
  });
  require_matches(run.left, reference(song, live_edit.expected), live_edit.label);
  require_continuous(run.left, live_edit.label);
}

// A second synth track, added with a clip whose note starts after the edit.
std::vector<project::Operation> add_synth_track(const Song& song) {
  const auto track = fresh<project::TrackId>(edit_track_serial);
  const auto& prism = *std::get_if<project::NativeInstrumentPatch>(
      &std::ranges::find(song.saved.tracks, song.track, &project::Track::id)->instrument_source);
  auto clip = midi_clip(song.saved, song.second);
  clip.id = fresh<project::ClipId>(edit_clip_serial);
  clip.placement = song.placed(clip, added_note_block);
  for (std::size_t index = 0; index < clip.notes.size(); ++index)
    clip.notes[index].id = fresh<project::NoteId>(index + 1);
  return {project::AddTrack{track, project::TrackKind::instrument},
          project::SetNativeInstrumentPatch{track, prism}, project::AddMidiClip{track, clip}};
}

void track_add_switches_with_its_source() {
  Song song;
  const auto edit = add_synth_track(song);
  Run run(song.root, song.saved);
  play_with(run, [&](Run& playing) { return playing.source_edit(edit, "track add"); });
  // Exact until the new track's first note: a synth that starts at the
  // switch need not match one that has run from the start, note for note.
  const auto until = added_note_block * block_frames;
  const auto expected = reference(song, edit);
  require_matches(std::span(run.left).first(until), std::span(expected).first(until), "track add");
  require_continuous(run.left, "track add");
  REQUIRE_GT(block_power(run.left, added_note_block + 1), silent_power,
             "the added track does not play");
}

void track_remove_fades_out_then_switches() {
  Song song;
  Run run(song.root, song.saved);
  play_with(run, [&](Run& playing) {
    return playing.source_edit({project::RemoveTrack{song.track}}, "track remove");
  });
  // Untouched before the switch, then a fade to silence, never a cut.
  const auto unedited = reference(song, {});
  require_matches(std::span(run.left).first(edit_block * block_frames),
                  std::span(unedited).first(edit_block * block_frames), "track remove");
  require_continuous(run.left, "track remove", gain_ramp_blocks);
  for (auto block = edit_block + settled_blocks; block < song_blocks; ++block)
    REQUIRE_LT(block_power(run.left, block), silent_power, "the removed track still sounds");
}

void tempo_change_moves_what_follows() {
  Song song;
  const auto tempo = take(timeline::TempoMap::create(
      {{{0}, edited_bpm, timeline::TempoShape::step}}, song.saved.tempo.revision() + 1));
  const std::vector<project::Operation> edit{project::ReplaceTempoMap{tempo}};
  check_source_edit(song, {"tempo change", edit, edit});
}

// A tempo ramp drawn on the tempo lane (ADR-0112) switches like any tempo
// change: from the switch, everything plays along the ramp.
void tempo_ramp_moves_what_follows() {
  Song song;
  const auto opening = song.saved.tempo.events().front().bpm;
  const auto tempo = take(timeline::TempoMap::create(
      {{{0}, opening, timeline::TempoShape::ease_in_out},
       {tick(song.saved, song_blocks), edited_bpm, timeline::TempoShape::step}},
      song.saved.tempo.revision() + 1));
  const std::vector<project::Operation> edit{project::ReplaceTempoMap{tempo}};
  check_source_edit(song, {"tempo ramp", edit, edit});
}

void meter_change_switches_with_its_source() {
  Song song;
  const auto meter = take(timeline::MeterMap::create(
      {{{0}, edited_meter_numerator, edited_meter_denominator}}, song.saved.meter.revision() + 1));
  const std::vector<project::Operation> edit{project::ReplaceMeterMap{meter}};
  check_source_edit(song, {"meter change", edit, edit});
}

// A lane that glides the song track's level down over the first clip.
project::AutomationLane level_lane(const Song& song) {
  const project::AutomationTarget level =
      project::TrackControlTarget{song.track, project::TrackControlTarget::Key::channel_level};
  auto session = take(app::ProjectSession::open(song.root));
  return {fresh<project::AutomationLaneId>(1),
          level,
          take(session->automation().automation_domain(song.track, level)),
          project::AutomationTimebase::musical,
          {{fresh<project::AutomationPointId>(1), 0, lane_start_level},
           {fresh<project::AutomationPointId>(2), tick(song.saved, later_note_block).value,
            lane_end_level, project::AutomationShape::linear}}};
}

// A track whose level is automated fades out too: the mute fade runs on top
// of its lane.
void track_remove_fades_out_over_its_level_lane() {
  Song song;
  Gate gate;
  auto model = take(project::ProjectModel::open(song.saved, gate));
  const auto automated =
      *take(model.apply({project::AddAutomationLane{song.track, level_lane(song)}})).snapshot;
  Run run(song.root, automated);
  play_with(run, [&](Run& playing) {
    return playing.source_edit({project::RemoveTrack{song.track}}, "automated track remove");
  });
  require_continuous(run.left, "automated track remove", gain_ramp_blocks);
  for (auto block = edit_block + settled_blocks; block < song_blocks; ++block)
    REQUIRE_LT(block_power(run.left, block), silent_power, "the removed track still sounds");
}

void midi_edit_with_a_level_lane_switches_whole() {
  Song song;
  // A level lane on the first clip: the clip's notes and the track's level
  // switch together.
  const auto lane = level_lane(song);
  const project::MidiNote added{fresh<project::NoteId>(1), tick(song.saved, added_note_block),
                                tick(song.saved, added_note_block + note_blocks), added_key,
                                velocity};
  const std::vector<project::Operation> edit{project::AddAutomationLane{song.first, lane},
                                             project::AddMidiNote{song.first, added}};
  check_source_edit(song, {"MIDI edit with a level lane", edit, edit});
}

// A different built-in engine than the song's, needing no media.
const synth::SynthEngine& other_engine() {
  const auto* engine = synth::find_engine(replacement_engine);
  require(engine != nullptr && engine != &synth::default_engine(),
          "the replacement engine is not another built-in one");
  return *engine;
}

void instrument_change_rings_out_the_old_one() {
  Song song;
  const auto patch =
      take(oscillate::native_inserts::native_patch_for_preset(other_engine(), std::nullopt));
  Run run(song.root, song.saved);
  play_with(run, [&](Run& playing) {
    return playing.source_edit({project::SetNativeInstrumentPatch{song.track, patch}},
                               "instrument change");
  });
  const auto unedited = reference(song, {});
  require_matches(std::span(run.left).first(edit_block * block_frames),
                  std::span(unedited).first(edit_block * block_frames), "instrument change");
  // The held note releases into its tail; nothing cuts.
  require_continuous(run.left, "instrument change", 1);
  // The new engine plays the later note.
  REQUIRE_GT(block_power(run.left, later_note_block + 1), silent_power,
             "the new instrument does not play");
}

void instrument_removal_rings_out() {
  Song song;
  Run run(song.root, song.saved);
  play_with(run, [&](Run& playing) {
    return playing.source_edit({project::SetNativeInstrumentPatch{song.track, std::nullopt}},
                               "instrument removal");
  });
  // The old instrument releases its note at the switch and plays nothing
  // after: as if the note ended there and no other followed.
  auto ended = song.held;
  ended.end = tick(song.saved, edit_block);
  require_matches(run.left,
                  reference(song, {project::SetMidiNote{song.first, ended},
                                   project::RemoveMidiNote{song.first, song.later.id},
                                   project::RemoveClip{song.second}}),
                  "instrument removal");
  require_continuous(run.left, "instrument removal", 1);
}

// A managed WAV tone in the song's store and the audio track it was
// imported to, and the song as it plays: that track, without the tone.
struct MediaContent {
  project::Snapshot saved;
  project::TrackId audio;
  project::SourceRecord tone;
  project::Clip clip;
  project::Snapshot playing;
};

MediaContent import_tone(const SongFiles& files) {
  auto session = take(app::ProjectSession::open(files.root));
  const auto rate = session->snapshot().sample_rate;
  std::vector<float> samples(media_clip_blocks * block_frames);
  for (std::size_t frame = 0; frame < samples.size(); ++frame)
    samples[frame] =
        static_cast<float>(media_tone_level * std::sin(2.0 * std::numbers::pi * media_tone_hz *
                                                       static_cast<double>(frame) / rate));
  const auto wav = files.directory.path() / "tone.wav";
  oscillate::test::write_wav_float(wav, rate, oscillate::test::mono_channels, samples);
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "WAV fixture open failed");
  auto audio = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  REQUIRE_OK(session->saving().save(), "the media song did not save");
  auto saved = session->snapshot();
  auto clip = std::ranges::find(saved.tracks, audio, &project::Track::id)->clips.front();
  auto tone = *std::ranges::find(saved.sources, clip.source, &project::SourceRecord::id);
  const auto start = static_cast<std::int64_t>(media_clip_block * block_frames);
  clip.placement = project::ElapsedInterval{
      {start}, {start + static_cast<std::int64_t>(media_clip_blocks * block_frames)}};
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  auto playing =
      *take(model.apply({project::RemoveClip{clip.id}, project::RemoveSource{tone.id}})).snapshot;
  return {std::move(saved), audio, std::move(tone), std::move(clip), std::move(playing)};
}

struct MediaSong : Song, MediaContent {
  MediaSong() : MediaContent(import_tone(*this)) {}
};

void new_media_plays_from_its_switch() {
  MediaSong song;
  const std::vector<project::Operation> edit{project::AddSource{song.tone},
                                             project::AddClip{song.audio, song.clip}};
  Run run(song.root, song.playing);
  play_with(run, [&](Run& playing) { return playing.source_edit(edit, "new media"); });
  Gate gate;
  auto model = take(project::ProjectModel::open(song.playing, gate));
  Run fresh_run(song.root, *take(model.apply(edit)).snapshot);
  fresh_run.play_to(song_blocks);
  require_matches(run.left, fresh_run.left, "new media");
  require_continuous(run.left, "new media");
  REQUIRE_GT(block_power(run.left, media_clip_block + 1), silent_power,
             "the new media does not play");
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
        an_edit_waiting_behind_a_note_edit_applies_next},
       {"rebalance_edits_glide_without_a_gap", rebalance_edits_glide_without_a_gap},
       {"track_add_switches_with_its_source", track_add_switches_with_its_source},
       {"track_remove_fades_out_then_switches", track_remove_fades_out_then_switches},
       {"track_remove_fades_out_over_its_level_lane", track_remove_fades_out_over_its_level_lane},
       {"tempo_change_moves_what_follows", tempo_change_moves_what_follows},
       {"tempo_ramp_moves_what_follows", tempo_ramp_moves_what_follows},
       {"meter_change_switches_with_its_source", meter_change_switches_with_its_source},
       {"midi_edit_with_a_level_lane_switches_whole", midi_edit_with_a_level_lane_switches_whole},
       {"instrument_change_rings_out_the_old_one", instrument_change_rings_out_the_old_one},
       {"instrument_removal_rings_out", instrument_removal_rings_out},
       {"new_media_plays_from_its_switch", new_media_plays_from_its_switch}});
}
