// Marker algebra for the Quick warp editor, applied through ProjectSession:
// each edit is one validated undo step that saves and reopens.
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-warp-edit") {}
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

fs::path make_wav(const fs::path& directory, std::uint32_t frames,
                  std::uint32_t sample_rate = oscillate::test::sample_rate_48k) {
  const auto path = directory / "source.wav";
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, sample_rate);
  put32(bytes, sample_rate * 2);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 2);
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    put16(bytes, static_cast<std::uint16_t>(frame % 4'000 == 10 ? 20'000 : 100));
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

struct Song {
  explicit Song(std::uint32_t source_rate = oscillate::test::sample_rate_48k)
      : source_rate(source_rate) {}
  std::uint32_t source_rate;
  Directory directory;
  fs::path root = directory.path() / "song";
  std::unique_ptr<app::ProjectSession> session = take(app::ProjectSession::create(root));
  project::ClipId clip = import_clip();
  [[nodiscard]] project::ClipId import_clip() {
    const auto wav = make_wav(directory.path(), source_rate, source_rate);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "fixture import failed");
    REQUIRE_OK(session->imports().import_wav_audio(fd), "fixture import failed");
    (void)::close(fd);
    return session->snapshot().tracks.front().clips.front().id;
  }
  void apply(const project::ClipProcessing& processing) {
    REQUIRE_OK(session->audio_clips().set_audio_clip_processing(clip, processing),
               "processing edit was rejected");
  }
  [[nodiscard]] app::ClipWarpLayout layout() const {
    return take(app::clip_warp_layout(session->snapshot(), clip));
  }
};

void markers_add_move_and_remove_as_single_undo_steps() {
  Song song;
  const auto empty = song.layout();
  REQUIRE_TRUE(empty.markers.empty(), "fresh clip layout is wrong");
  REQUIRE_EQ(empty.output_frames, 48'000, "fresh clip layout is wrong");
  REQUIRE_TRUE(!empty.musical, "fresh clip layout is wrong");

  const auto revision = song.session->snapshot().revision;
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, 12'000)));
  auto layout = song.layout();
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1,
             "added marker did not pin the source frame playing at its position");
  REQUIRE_EQ(layout.markers.size(), 1,
             "added marker did not pin the source frame playing at its position");
  REQUIRE_EQ(layout.markers[0].output_frame, 12'000,
             "added marker did not pin the source frame playing at its position");
  REQUIRE_EQ(layout.markers[0].source_frame, 12'000,
             "added marker did not pin the source frame playing at its position");
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, 30'000)));
  REQUIRE_TRUE(!app::add_warp_marker(song.session->snapshot(), song.clip, 12'000),
               "duplicate or endpoint marker was accepted");
  REQUIRE_TRUE(!app::add_warp_marker(song.session->snapshot(), song.clip, 0),
               "duplicate or endpoint marker was accepted");
  REQUIRE_TRUE(!app::add_warp_marker(song.session->snapshot(), song.clip, 48'000),
               "duplicate or endpoint marker was accepted");

  // Moving the first marker stretches only its neighbouring segments and
  // keeps the second marker exactly where it was.
  song.apply(take(app::move_warp_marker(song.session->snapshot(), song.clip,
                                        {.index = 0, .output_frame = 18'000})));
  layout = song.layout();
  REQUIRE_EQ(layout.markers.size(), 2, "moving a marker changed its source frame or a neighbour");
  REQUIRE_EQ(layout.markers[0].output_frame, 18'000,
             "moving a marker changed its source frame or a neighbour");
  REQUIRE_EQ(layout.markers[0].source_frame, 12'000,
             "moving a marker changed its source frame or a neighbour");
  REQUIRE_EQ(layout.markers[1].output_frame, 30'000,
             "moving a marker changed its source frame or a neighbour");
  REQUIRE_EQ(layout.markers[1].source_frame, 30'000,
             "moving a marker changed its source frame or a neighbour");
  REQUIRE_TRUE(!app::move_warp_marker(song.session->snapshot(), song.clip,
                                      {.index = 0, .output_frame = 30'000}),
               "a marker move past a neighbour, the clip end or a missing index was accepted");
  REQUIRE_TRUE(!app::move_warp_marker(song.session->snapshot(), song.clip,
                                      {.index = 1, .output_frame = 17'000}),
               "a marker move past a neighbour, the clip end or a missing index was accepted");
  REQUIRE_TRUE(!app::move_warp_marker(song.session->snapshot(), song.clip,
                                      {.index = 1, .output_frame = 48'000}),
               "a marker move past a neighbour, the clip end or a missing index was accepted");
  REQUIRE_TRUE(!app::move_warp_marker(song.session->snapshot(), song.clip,
                                      {.index = 5, .output_frame = 20'000}),
               "a marker move past a neighbour, the clip end or a missing index was accepted");

  // One undo reverts exactly the move.
  require(song.session->history().undo().has_value() &&
              song.layout().markers[0].output_frame == 12'000,
          "undo did not revert exactly one marker move");
  REQUIRE_OK(song.session->history().redo(), "redo did not restore the marker move");
  REQUIRE_EQ(song.layout().markers[0].output_frame, 18'000, "redo did not restore the marker move");
  song.apply(take(app::remove_warp_marker(song.session->snapshot(), song.clip, 1)));
  layout = song.layout();
  REQUIRE_EQ(layout.markers.size(), 1, "removing a marker changed the other marker");
  REQUIRE_EQ(layout.markers[0].source_frame, 12'000, "removing a marker changed the other marker");

  REQUIRE_OK(song.session->saving().save(), "save failed");
  const auto saved = song.session->snapshot().tracks.front().clips.front().processing;
  song.session.reset();
  auto reopened = take(app::ProjectSession::open(song.root));
  REQUIRE_EQ(reopened->snapshot().tracks.front().clips.front().processing, saved,
             "edited markers did not reopen");
}

void reviewed_suggestions_apply_atomically_without_moving_audio() {
  Song song;
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, 20'000)));
  const auto before = song.session->snapshot().revision;
  // 4,010 and 8,010 are new; 20,005 is too close to the existing marker;
  // 0 and 50,000 are outside the trimmed range.
  const std::array<std::uint64_t, 5> suggested{8'010, 4'010, 20'005, 0, 50'000};
  song.apply(take(app::apply_warp_suggestions(song.session->snapshot(), song.clip, suggested, 64)));
  const auto layout = song.layout();
  REQUIRE_EQ(song.session->snapshot().revision, before + 1,
             "suggestions were not applied as one revision");
  REQUIRE_EQ(layout.markers.size(), 3, "suggestions were not applied as one revision");
  for (const auto& marker : layout.markers)
    REQUIRE_EQ(marker.output_frame, marker.source_frame,
               "an applied suggestion moved audio instead of pinning it in place");
  require(!app::apply_warp_suggestions(song.session->snapshot(), song.clip,
                                       std::array<std::uint64_t, 1>{20'001}, 64),
          "a suggestion set that adds nothing was applied");
  REQUIRE_OK(song.session->history().undo(), "one undo did not remove every applied suggestion");
  REQUIRE_EQ(song.layout().markers.size(), 1, "one undo did not remove every applied suggestion");
}

void suggestions_colliding_after_rate_conversion_are_skipped() {
  constexpr std::uint32_t converted_source_rate = oscillate::test::sample_rate_96k;
  Song song(converted_source_rate);
  const auto& snapshot = song.session->snapshot();
  const auto& clip = snapshot.tracks.front().clips.front();
  const auto& source = snapshot.sources.front();
  auto plan = take(oscillate::processing::prepare_clip_frame_plan(clip, source, snapshot.tempo,
                                                                  snapshot.sample_rate));
  std::uint64_t first = 0;
  for (std::uint64_t frame = 10'000; frame < 11'000; ++frame) {
    const auto a = plan.warp.output_at_source(frame);
    const auto b = plan.warp.output_at_source(frame + 1);
    if (a && b && *a == *b) {
      first = frame;
      break;
    }
  }
  REQUIRE_NE(first, 0, "rate-converted fixture has no colliding output frames");
  const std::array<std::uint64_t, 2> suggested{first, first + 1};
  song.apply(take(app::apply_warp_suggestions(snapshot, song.clip, suggested)));
  REQUIRE_EQ(
      song.session->snapshot().tracks.front().clips.front().processing.reviewed_warp_markers.size(),
      1, "suggestions colliding at one output frame were applied together");
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

// A clip on a musical placement stores tick offsets; added and moved markers
// resolve back to (within a frame of) the requested output positions.
void musical_markers_use_ticks_and_round_trip() {
  Song song;
  auto state = song.session->snapshot();
  const auto quarter = timeline::ticks_per_quarter;
  // Two quarter notes at 120 BPM are 48,000 frames: no stretch.
  state.tracks.front().clips.front().placement = project::MusicalInterval{{0}, {2 * quarter}};
  Gate gate;
  auto model = take(project::ProjectModel::open(state, gate));
  const auto layout = take(app::clip_warp_layout(*model.snapshot(), song.clip));
  REQUIRE_TRUE(layout.musical, "musical fixture layout is wrong");
  REQUIRE_EQ(layout.output_frames, 48'000, "musical fixture layout is wrong");
  auto edited = take(app::add_warp_marker(*model.snapshot(), song.clip, 16'000));
  require(std::holds_alternative<timeline::TickPosition>(
              edited.reviewed_warp_markers.front().target_offset),
          "a musical clip marker was not stored in ticks");
  (void)take(model.apply({project::SetClipProcessing{song.clip, edited}}));
  auto moved = take(
      app::move_warp_marker(*model.snapshot(), song.clip, {.index = 0, .output_frame = 24'000}));
  (void)take(model.apply({project::SetClipProcessing{song.clip, moved}}));
  const auto after = take(app::clip_warp_layout(*model.snapshot(), song.clip));
  const auto delta = after.markers.front().output_frame > 24'000
                         ? after.markers.front().output_frame - 24'000
                         : 24'000 - after.markers.front().output_frame;
  REQUIRE_EQ(after.markers.size(), 1, "musical marker did not land at the requested output frame");
  REQUIRE_LE(delta, 1, "musical marker did not land at the requested output frame");
  REQUIRE_EQ(after.markers.front().source_frame, 16'000,
             "musical marker did not land at the requested output frame");
}

// --- Split, slip and trim of processed clips -----------------------------------

// The absolute source frame each clip on the first track plays at project
// sample `sample`, through the authoritative clip frame plan.
std::optional<std::int64_t> heard_source(const project::Snapshot& state, std::int64_t sample) {
  for (const auto& clip : state.tracks.front().clips) {
    const auto source = std::ranges::find(state.sources, clip.source, &project::SourceRecord::id);
    auto plan = take(oscillate::processing::prepare_clip_frame_plan(clip, *source, state.tempo,
                                                                    state.sample_rate));
    const auto first = static_cast<std::int64_t>(plan.project_start_frame);
    if (sample < first || sample >= static_cast<std::int64_t>(plan.project_end_frame()))
      continue;
    const auto local = plan.warp.source_at_output(static_cast<std::uint64_t>(sample - first));
    return static_cast<std::int64_t>(plan.source_start_frame + *local);
  }
  return std::nullopt;
}

// Every project sample in [first, last) plays the same source frame before
// and after, within `tolerance` frames inside [loose_first, loose_last) and
// exactly elsewhere.
void require_same_audio(const project::Snapshot& before, const project::Snapshot& after,
                        std::int64_t first, std::int64_t last, std::int64_t loose_first,
                        std::int64_t loose_last, std::string_view what) {
  for (auto sample = first; sample < last; ++sample) {
    const auto old_frame = heard_source(before, sample);
    const auto new_frame = heard_source(after, sample);
    require(old_frame && new_frame, std::string(what) + ": a sample lost its audio");
    const auto tolerance = sample >= loose_first && sample < loose_last ? 1 : 0;
    REQUIRE_NEAR(*old_frame, *new_frame, tolerance,
                 std::string(what) + ": audio moved at project sample " + std::to_string(sample));
  }
}

// Markers at outputs 18,000 (source 12,000) and 30,000 (source 30,000).
void add_two_markers(Song& song) {
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, 12'000)));
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, 30'000)));
  song.apply(take(app::move_warp_marker(song.session->snapshot(), song.clip,
                                        {.index = 0, .output_frame = 18'000})));
}

std::vector<project::Clip> clips(const app::ProjectSession& session) {
  return session.snapshot().tracks.front().clips;
}

void split_keeps_markers_with_their_audio() {
  Song song;
  add_two_markers(song);
  song.apply([&] {
    auto processing = song.session->snapshot().tracks.front().clips.front().processing;
    processing.warp.mode = project::WarpMode::beats;
    processing.pitch_semitones = 3.0;
    return processing;
  }());
  REQUIRE_TRUE(song.session->automation().set_audio_clip_channel_level_point_at_fraction(
                   song.clip, {0.0}, 0.5),
               "processed split clip automation setup failed");
  REQUIRE_TRUE(song.session->automation().set_audio_clip_channel_level_point_at_fraction(
                   song.clip, {0.75}, 1.5),
               "processed split clip automation setup failed");
  const auto before = song.session->snapshot();
  const auto original = clips(*song.session);
  // Between the markers: the cut's source frame is the map's, rounded.
  auto right = take(song.session->audio_clips().split_audio_clip_at_sample(song.clip, {24'000}));
  const auto after = song.session->snapshot();
  REQUIRE_EQ(after.revision, before.revision + 1, "split was not one revision");
  const auto halves = clips(*song.session);
  REQUIRE_EQ(halves.size(), 2, "split did not make two clips");
  REQUIRE_EQ(halves[1].id, right, "split did not make two clips");
  REQUIRE_EQ(halves[0].processing.reviewed_warp_markers.size(), 1,
             "split did not keep each marker with its audio");
  REQUIRE_EQ(halves[1].processing.reviewed_warp_markers.size(), 1,
             "split did not keep each marker with its audio");
  REQUIRE_EQ(halves[0].processing.reviewed_warp_markers[0].source_frame.value, 12'000,
             "split did not keep each marker with its audio");
  REQUIRE_EQ(halves[1].processing.reviewed_warp_markers[0].source_frame.value, 30'000,
             "split did not keep each marker with its audio");
  REQUIRE_EQ(std::get<timeline::SamplePosition>(
                 halves[1].processing.reviewed_warp_markers[0].target_offset)
                 .value,
             6'000, "split did not keep each marker with its audio");
  REQUIRE_EQ(halves[0].processing.warp, halves[1].processing.warp,
             "split halves lost settings or do not meet in the source");
  REQUIRE_EQ(halves[1].processing.pitch_semitones, 3.0,
             "split halves lost settings or do not meet in the source");
  REQUIRE_EQ(halves[0].source_range.end, halves[1].source_range.start,
             "split halves lost settings or do not meet in the source");
  require(halves[0].automation.size() == 1 && halves[1].automation.size() == 1 &&
              halves[0].automation.front().id != halves[1].automation.front().id &&
              halves[1].automation.front().points.front().coordinate == 0 &&
              // Two thirds of the way from 0.5 to 1.5, blended in decibels.
              std::abs(halves[1].automation.front().points.front().value -
                       0.5 * std::pow(3.0, 2.0 / 3.0)) <
                  oscillate::test::tolerance::double_rounding,
          "processed split did not preserve clip-local automation");
  require_same_audio(before, after, 0, 48'000, 18'000, 30'000, "split between markers");
  REQUIRE_TRUE(song.session->history().undo(), "split did not undo in one step");
  REQUIRE_EQ(clips(*song.session), original, "split did not undo in one step");
  REQUIRE_TRUE(song.session->history().redo(), "split redo/save failed");
  REQUIRE_OK(song.session->saving().save(), "split redo/save failed");
  const auto saved = clips(*song.session);
  song.session.reset();
  song.session = take(app::ProjectSession::open(song.root));
  REQUIRE_EQ(clips(*song.session), saved, "split clips did not reopen");

  // Exactly at a marker: the marker becomes both halves' shared edge, exact.
  Song exact;
  add_two_markers(exact);
  const auto exact_before = exact.session->snapshot();
  (void)take(exact.session->audio_clips().split_audio_clip_at_sample(exact.clip, {18'000}));
  const auto exact_halves = clips(*exact.session);
  REQUIRE_EQ(exact_halves[0].source_range.end.value, 12'000,
             "split at a marker did not use it as the edge");
  REQUIRE_TRUE(exact_halves[0].processing.reviewed_warp_markers.empty(),
               "split at a marker did not use it as the edge");
  REQUIRE_EQ(exact_halves[1].processing.reviewed_warp_markers.size(), 1,
             "split at a marker did not use it as the edge");
  require_same_audio(exact_before, exact.session->snapshot(), 0, 48'000, 0, 0, "split at a marker");
}

void trim_keeps_marker_timing_and_never_drops_a_marker() {
  Song song;
  add_two_markers(song);
  const auto before = song.session->snapshot();
  const auto original = clips(*song.session);
  using Edge = app::AudioClipEdge;
  // Past a reviewed marker: rejected, nothing changes (FS-05).
  require(!song.session->audio_clips().trim_audio_clip(song.clip, Edge::left, {20'000}) &&
              !song.session->audio_clips().trim_audio_clip(song.clip, Edge::left, {18'000}) &&
              !song.session->audio_clips().trim_audio_clip(song.clip, Edge::right, {25'000}) &&
              song.session->snapshot().revision == before.revision,
          "a trim that would drop a reviewed marker was accepted");
  // Before the first marker: it is rebased and everything left plays as before.
  REQUIRE_OK(song.session->audio_clips().trim_audio_clip(song.clip, Edge::left, {15'000}),
             "left trim before a marker was rejected");
  auto trimmed = clips(*song.session).front();
  REQUIRE_EQ(trimmed.processing.reviewed_warp_markers.size(), 2,
             "left trim did not rebase the markers");
  REQUIRE_EQ(
      std::get<timeline::SamplePosition>(trimmed.processing.reviewed_warp_markers[0].target_offset)
          .value,
      3'000, "left trim did not rebase the markers");
  REQUIRE_EQ(std::get<project::ElapsedInterval>(trimmed.placement).start.value, 15'000,
             "left trim did not rebase the markers");
  require_same_audio(before, song.session->snapshot(), 15'000, 48'000, 15'000, 18'000, "left trim");
  // Extending again follows the first segment's slope; nothing inside moves.
  const auto narrowed = song.session->snapshot();
  REQUIRE_OK(song.session->audio_clips().trim_audio_clip(song.clip, Edge::left, {5'000}),
             "left trim extension was rejected");
  require_same_audio(narrowed, song.session->snapshot(), 15'000, 48'000, 15'000, 18'000,
                     "left trim extension");
  // The first segment plays 12,000 source frames over 18,000 outputs.
  const auto start = clips(*song.session).front().source_range.start.value;
  REQUIRE_NEAR(start, 5'000 * 12'000 / 18'000, 1,
               "left trim extension did not follow the edge segment");
  // The right edge cannot reach past the source.
  const auto revision = song.session->snapshot().revision;
  REQUIRE_TRUE(!song.session->audio_clips().trim_audio_clip(song.clip, Edge::right, {60'000}),
               "a trim past the source end was accepted");
  REQUIRE_EQ(song.session->snapshot().revision, revision,
             "a trim past the source end was accepted");
  REQUIRE_OK(song.session->audio_clips().trim_audio_clip(song.clip, Edge::right, {40'000}),
             "right trim was rejected");
  REQUIRE_TRUE(song.session->history().undo(), "trims did not undo one step each");
  REQUIRE_TRUE(song.session->history().undo(), "trims did not undo one step each");
  REQUIRE_TRUE(song.session->history().undo(), "trims did not undo one step each");
  REQUIRE_EQ(clips(*song.session), original, "trims did not undo one step each");
}

void slip_moves_audio_with_its_markers() {
  Song song;
  using Edge = app::AudioClipEdge;
  REQUIRE_OK(song.session->audio_clips().trim_audio_clip(song.clip, Edge::left, {8'000}),
             "slip fixture trim failed");
  REQUIRE_OK(song.session->audio_clips().trim_audio_clip(song.clip, Edge::right, {36'000}),
             "slip fixture trim failed");
  // Marker at project 16,000 (output 8,000), moved to project 17,000.
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, 8'000)));
  song.apply(take(app::move_warp_marker(song.session->snapshot(), song.clip,
                                        {.index = 0, .output_frame = 9'000})));
  const auto before = song.session->snapshot();
  const auto original = clips(*song.session).front();
  REQUIRE_OK(song.session->audio_clips().slip_audio_clip(song.clip, 2'000), "slip was rejected");
  const auto slipped = clips(*song.session).front();
  REQUIRE_EQ(slipped.placement, original.placement,
             "slip moved the clip or detached the marker from its audio");
  REQUIRE_EQ(slipped.processing.reviewed_warp_markers.size(), 1,
             "slip moved the clip or detached the marker from its audio");
  REQUIRE_EQ(slipped.processing.reviewed_warp_markers[0].source_frame.value, 16'000,
             "slip moved the clip or detached the marker from its audio");
  // The first segment plays 8,000 source frames over 9,000 outputs, so a
  // 2,000-frame slip moves the audio 2,250 project samples earlier.
  const auto offset =
      std::get<timeline::SamplePosition>(slipped.processing.reviewed_warp_markers[0].target_offset)
          .value;
  REQUIRE_EQ(offset, 9'000 - 2'250,
             "slip did not shift the audio by the timeline span of its delta");
  REQUIRE_NEAR(slipped.source_range.start.value, 10'000, 1,
               "slip did not shift the audio by the timeline span of its delta");
  for (std::int64_t sample = 8'000; sample + 2'250 < 36'000; sample += 7)
    REQUIRE_NEAR(*heard_source(song.session->snapshot(), sample),
                 *heard_source(before, sample + 2'250), 1,
                 "slipped audio is not the same audio shifted");
  // A slip that would push the marker out of the clip, or the window past
  // the source, rejects without changing anything.
  const auto revision = song.session->snapshot().revision;
  REQUIRE_TRUE(!song.session->audio_clips().slip_audio_clip(song.clip, 6'100),
               "a slip that drops a marker or leaves the source was accepted");
  REQUIRE_TRUE(!song.session->audio_clips().slip_audio_clip(song.clip, 1'000'000),
               "a slip that drops a marker or leaves the source was accepted");
  REQUIRE_EQ(song.session->snapshot().revision, revision,
             "a slip that drops a marker or leaves the source was accepted");
  REQUIRE_TRUE(song.session->history().undo(), "slip did not undo in one step");
  REQUIRE_EQ(clips(*song.session).front(), original, "slip did not undo in one step");
}

void musical_split_and_trim_rebase_ticks() {
  Song song;
  auto state = song.session->snapshot();
  const auto quarter = timeline::ticks_per_quarter;
  // Two quarter notes at 120 BPM, one marker on the first beat moved later.
  state.tracks.front().clips.front().placement = project::MusicalInterval{{0}, {2 * quarter}};
  Gate gate;
  auto model = take(project::ProjectModel::open(state, gate));
  auto marked = take(app::add_warp_marker(*model.snapshot(), song.clip, 24'000));
  (void)take(model.apply({project::SetClipProcessing{song.clip, marked}}));
  auto moved = take(
      app::move_warp_marker(*model.snapshot(), song.clip, {.index = 0, .output_frame = 30'000}));
  (void)take(model.apply({project::SetClipProcessing{song.clip, moved}}));
  const auto before = *model.snapshot();
  const auto marker_tick =
      std::get<timeline::TickPosition>(
          before.tracks.front().clips.front().processing.reviewed_warp_markers[0].target_offset)
          .value;
  // Split on the beat (before the marker): the right half's marker offset is
  // rebased by exactly one quarter note.
  auto left = take(app::rewindow_processed_clip(before, song.clip, 0, quarter, false));
  auto right = take(app::rewindow_processed_clip(before, song.clip, quarter, 2 * quarter, false));
  REQUIRE_TRUE(left.processing.reviewed_warp_markers.empty(),
               "musical split did not rebase marker ticks");
  REQUIRE_EQ(left.removed_markers, 1, "musical split did not rebase marker ticks");
  REQUIRE_EQ(right.processing.reviewed_warp_markers.size(), 1,
             "musical split did not rebase marker ticks");
  REQUIRE_EQ(
      std::get<timeline::TickPosition>(right.processing.reviewed_warp_markers[0].target_offset)
          .value,
      marker_tick - quarter, "musical split did not rebase marker ticks");
  REQUIRE_EQ(std::get<project::MusicalInterval>(right.placement).start.value, quarter,
             "musical split did not rebase marker ticks");
  REQUIRE_EQ(left.source_range.end, right.source_range.start,
             "musical split halves do not meet in the source");
  auto copy = before.tracks.front().clips.front();
  copy.id = take(project::ClipId::from_bits({0, 9'000}));
  copy.placement = right.placement;
  copy.source_range = right.source_range;
  copy.processing = right.processing;
  (void)take(model.apply({project::SetClipPlacement{song.clip, left.placement},
                          project::SetClipSourceRange{song.clip, left.source_range},
                          project::SetClipProcessing{song.clip, left.processing},
                          project::AddClip{before.tracks.front().id, copy}}));
  require_same_audio(before, *model.snapshot(), 0, 48'000, 0, 30'000, "musical split");
}

// The fixture has a click every 4,000 frames (at 10, 4,010, ...): detected
// suggestions sit on them, inside the clip, and change nothing until applied.
// Durable onset review: analysis of the whole verified source is stored per
// source as generated suggestions; applying one makes a clip marker,
// dismissing one is a correction that reanalysis keeps. Both survive reopen.
void onset_review_is_durable_and_keeps_corrections() {
  Song song;
  const auto source = song.session->snapshot().sources.front();
  const auto revision = song.session->snapshot().revision;
  auto onsets = take(app::analyse_source_onsets(song.root, song.session->snapshot(), source.id));
  REQUIRE_GE(onsets.frames.size(), 10, "onset analysis was missing or changed the project");
  REQUIRE_TRUE(!onsets.truncated, "onset analysis was missing or changed the project");
  REQUIRE_EQ(song.session->snapshot().revision, revision,
             "onset analysis was missing or changed the project");
  for (const auto frame : onsets.frames) {
    const auto phase = (frame + 4'000 - 10) % 4'000;
    require(phase <= 2 || phase >= 3'998, "an onset is not on a fixture click");
  }
  std::atomic<bool> cancelled{true};
  require(!app::analyse_source_onsets(song.root, song.session->snapshot(), source.id, &cancelled),
          "cancelled onset analysis returned results");
  // A result captured before an intervening edit is rejected.
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, 2'000)));
  require(!song.session->source_analysis().accept_source_onset_analysis(source, revision,
                                                                        onsets.frames, false),
          "a stale onset analysis was stored");
  REQUIRE_OK(song.session->history().undo(), "fixture undo failed");
  require(song.session->source_analysis()
              .accept_source_onset_analysis(source, song.session->snapshot().revision,
                                            onsets.frames, onsets.truncated)
              .has_value(),
          "onset analysis was not stored");
  auto suggested = app::clip_onset_suggestions(song.session->snapshot(), song.clip, 64);
  REQUIRE_EQ(suggested, onsets.frames, "stored onsets were not offered as suggestions");
  // Apply the first suggestion; dismiss the second.
  song.apply(take(app::apply_warp_suggestions(song.session->snapshot(), song.clip,
                                              std::array<std::uint64_t, 1>{suggested[0]}, 64)));
  REQUIRE_OK(song.session->source_analysis().set_source_dismissed_onsets(source.id, {suggested[1]}),
             "dismissal was rejected");
  auto remaining = app::clip_onset_suggestions(song.session->snapshot(), song.clip, 64);
  REQUIRE_EQ(remaining.size(), suggested.size() - 2,
             "an applied or dismissed onset was still suggested");
  REQUIRE_EQ(std::ranges::find(remaining, suggested[0]), remaining.end(),
             "an applied or dismissed onset was still suggested");
  REQUIRE_EQ(std::ranges::find(remaining, suggested[1]), remaining.end(),
             "an applied or dismissed onset was still suggested");
  // Reanalysis replaces generated onsets only: the dismissal and the marker hold.
  const auto again =
      take(app::analyse_source_onsets(song.root, song.session->snapshot(), source.id));
  require(song.session->source_analysis()
              .accept_source_onset_analysis(source, song.session->snapshot().revision, again.frames,
                                            again.truncated)
              .has_value(),
          "reanalysis was not stored");
  REQUIRE_EQ(app::clip_onset_suggestions(song.session->snapshot(), song.clip, 64), remaining,
             "reanalysis restored a dismissed onset or touched a marker");
  REQUIRE_EQ(clips(*song.session).front().processing.reviewed_warp_markers.size(), 1,
             "reanalysis restored a dismissed onset or touched a marker");
  REQUIRE_OK(song.session->saving().save(), "onset review save failed");
  const auto saved = song.session->snapshot().source_tempo_reviews;
  song.session.reset();
  song.session = take(app::ProjectSession::open(song.root));
  REQUIRE_EQ(song.session->snapshot().source_tempo_reviews, saved, "onset review did not reopen");
  REQUIRE_EQ(app::clip_onset_suggestions(song.session->snapshot(), song.clip, 64), remaining,
             "onset review did not reopen");
}

// A source tempo review at 100 BPM: beats every 14,400 frames.
void review_tempo(Song& song, double bpm, std::vector<std::uint64_t> beats) {
  const auto& source = song.session->snapshot().sources.front();
  oscillate::analysis::TempoEstimate estimate;
  estimate.identity = {take(app::tempo_cache_key(source)).hex(), 1};
  estimate.sample_rate = source.sample_rate;
  estimate.frames = source.frames;
  estimate.bpm = bpm;
  estimate.confidence = 0.9;
  estimate.suggested_beat_frames = std::move(beats);
  require(song.session->source_analysis()
              .accept_source_tempo_analysis(source.id, song.session->snapshot().revision, estimate)
              .has_value(),
          "tempo review fixture was rejected");
}

void auto_warp_puts_reviewed_beats_on_the_grid() {
  Song song;
  constexpr auto quarter = timeline::ticks_per_quarter;
  require(!app::propose_auto_warp(song.session->snapshot(), song.clip),
          "auto-warp accepted an elapsed clip");
  REQUIRE_OK(song.session->audio_clips().toggle_audio_clip_timebase(song.clip),
             "musical timebase toggle failed");
  require(!app::propose_auto_warp(song.session->snapshot(), song.clip),
          "auto-warp worked without reviewed beats");
  review_tempo(song, 100.0, {7'000, 21'400, 35'800});
  auto proposal = take(app::propose_auto_warp(song.session->snapshot(), song.clip));
  // The first beat plays near tick 0.29 quarters: the nearest grid beat is the
  // clip start, so it lands on the next one; each later beat one quarter on.
  require(proposal.beat_markers == 3 && proposal.skipped_beats == 0 && proposal.source_bpm == 100.0,
          "auto-warp did not place every beat");
  const auto& markers = proposal.processing.reviewed_warp_markers;
  for (std::size_t index = 0; index < 3; ++index)
    REQUIRE_EQ(std::get<timeline::TickPosition>(markers[index].target_offset).value,
               static_cast<std::int64_t>(index + 1) * quarter,
               "an auto-warp beat is off the quarter grid");
  // The tail (12,200 frames after the last beat) keeps the last beat interval.
  const auto end = std::get<project::MusicalInterval>(proposal.placement).end.value;
  REQUIRE_NEAR(end, (3 * quarter + 12'200 * quarter / 14'400), 1,
               "auto-warp clip end does not follow the last beat interval");
  const auto revision = song.session->snapshot().revision;
  REQUIRE_TRUE(song.session->warp()
                   .apply_audio_clip_warp(song.clip, proposal.settings_generation,
                                          proposal.placement, proposal.processing)
                   .has_value(),
               "auto-warp was not one edit");
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1, "auto-warp was not one edit");
  const auto layout = song.layout();
  REQUIRE_EQ(layout.markers.size(), 3, "auto-warped beats do not play on the project grid");
  REQUIRE_EQ(layout.markers[0].output_frame, 24'000,
             "auto-warped beats do not play on the project grid");
  REQUIRE_EQ(layout.markers[1].output_frame, 48'000,
             "auto-warped beats do not play on the project grid");
  REQUIRE_EQ(layout.markers[2].output_frame, 72'000,
             "auto-warped beats do not play on the project grid");
  REQUIRE_EQ(layout.markers[0].source_frame, 7'000,
             "auto-warped beats do not play on the project grid");
  require(!song.session->warp().apply_audio_clip_warp(song.clip, proposal.settings_generation,
                                                      proposal.placement, proposal.processing),
          "a stale auto-warp proposal was applied");
  REQUIRE_OK(song.session->history().undo(), "auto-warp did not undo in one step");
  REQUIRE_TRUE(song.layout().markers.empty(), "auto-warp did not undo in one step");

  // A reviewed marker is a fixed anchor: the beat that would cross it is
  // skipped and the marker itself is untouched.
  auto processing = clips(*song.session).front().processing;
  processing.reviewed_warp_markers.push_back({{28'000}, timeline::TickPosition{quarter * 3 / 2}});
  song.apply(processing);
  proposal = take(app::propose_auto_warp(song.session->snapshot(), song.clip));
  REQUIRE_EQ(proposal.beat_markers, 2, "auto-warp moved a reviewed marker or crossed it");
  REQUIRE_EQ(proposal.skipped_beats, 1, "auto-warp moved a reviewed marker or crossed it");
  REQUIRE_NE(std::ranges::find(proposal.processing.reviewed_warp_markers,
                               processing.reviewed_warp_markers.front()),
             proposal.processing.reviewed_warp_markers.end(),
             "auto-warp moved a reviewed marker or crossed it");
}

void tempo_conform_stretches_to_the_project_tempo() {
  Song song;
  REQUIRE_OK(song.session->audio_clips().toggle_audio_clip_timebase(song.clip),
             "musical timebase toggle failed");
  require(!app::propose_tempo_conform(song.session->snapshot(), song.clip),
          "conform worked without a reviewed tempo");
  review_tempo(song, 100.0, {7'000, 21'400, 35'800});
  // One second at 100 BPM is 5/3 beats; at the project's 120 BPM that plays
  // in 40,000 frames.
  auto proposal = take(app::propose_tempo_conform(song.session->snapshot(), song.clip));
  REQUIRE_EQ(proposal.processing, clips(*song.session).front().processing,
             "conform changed processing without markers");
  REQUIRE_TRUE(song.session->warp()
                   .apply_audio_clip_warp(song.clip, proposal.settings_generation,
                                          proposal.placement, proposal.processing)
                   .has_value(),
               "conformed clip does not play at the project tempo");
  REQUIRE_EQ(song.layout().output_frames, 40'000,
             "conformed clip does not play at the project tempo");
  // A user BPM correction wins over the detection.
  require(song.session->source_analysis()
              .set_source_tempo_correction(song.session->snapshot().sources.front().id, 120.0)
              .has_value(),
          "tempo correction failed");
  proposal = take(app::propose_tempo_conform(song.session->snapshot(), song.clip));
  REQUIRE_EQ(proposal.source_bpm, 120.0, "conform ignored the corrected BPM");
  REQUIRE_EQ(std::get<project::MusicalInterval>(proposal.placement).end.value,
             2 * timeline::ticks_per_quarter, "conform ignored the corrected BPM");
}

// The clip strip (ADR-0053): Follow, a typed BPM and the downbeat flag, each
// one undo step, with the downbeat landing on a bar.
void clip_strip_follows_corrects_and_sets_the_downbeat() {
  Song song;
  constexpr auto quarter = timeline::ticks_per_quarter;
  constexpr auto bar = 4 * quarter;
  require(!song.session->warp().set_audio_clip_follow(song.clip, true),
          "Follow worked before the source had a tempo");
  review_tempo(song, 100.0, {7'000, 35'800});
  const auto original = clips(*song.session).front().placement;

  // Follow: musical, one second at 100 BPM is 5/3 beats, and the downbeat
  // (source frame 7,000, near the project start) lands on a bar. On the first
  // bar the clip would start before zero, so it moves to the next bar instead:
  // following never adds bars before bar 1.
  auto revision = song.session->snapshot().revision;
  REQUIRE_OK(song.session->warp().set_audio_clip_follow(song.clip, true),
             "Follow was not one edit");
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1, "Follow was not one edit");
  auto placement = std::get<project::MusicalInterval>(clips(*song.session).front().placement);
  REQUIRE_LE(std::abs(placement.end.value - placement.start.value - 5 * quarter / 3), 1,
             "Follow did not conform the clip to its BPM");
  REQUIRE_NEAR(take(app::clip_downbeat_tick(song.session->snapshot(), song.clip)), bar, 1,
               "Follow did not put the downbeat on a bar");
  REQUIRE_EQ(song.session->snapshot().lead_bars, 0,
             "Follow added bars before bar 1 rather than moving the clip to the next bar");
  REQUIRE_GE(placement.start.value, 0,
             "Follow added bars before bar 1 rather than moving the clip to the next bar");

  // A typed BPM rescales the grid around the downbeat, which stays put.
  revision = song.session->snapshot().revision;
  REQUIRE_OK(song.session->warp().set_audio_clip_source_bpm(song.clip, 120.0),
             "a typed BPM was not one edit");
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1, "a typed BPM was not one edit");
  placement = std::get<project::MusicalInterval>(clips(*song.session).front().placement);
  REQUIRE_LE(std::abs(placement.end.value - placement.start.value - 2 * quarter), 1,
             "a typed BPM moved the downbeat or missed the tempo");
  REQUIRE_NEAR(take(app::clip_downbeat_tick(song.session->snapshot(), song.clip)), bar, 2,
               "a typed BPM moved the downbeat or missed the tempo");
  REQUIRE_EQ(song.session->snapshot().source_tempo_reviews.front().corrected_bpm, 120.0,
             "the typed BPM was not saved as the source's correction");

  // The downbeat flag: a new downbeat lands on the nearest bar.
  REQUIRE_OK(song.session->warp().set_audio_clip_downbeat(song.clip, 31'000),
             "setting the downbeat failed");
  REQUIRE_EQ(song.session->snapshot().source_tempo_reviews.front().downbeat_frame, 31'000,
             "the downbeat was not saved");
  const auto downbeat = take(app::clip_downbeat_tick(song.session->snapshot(), song.clip));
  REQUIRE_NEAR(downbeat, (downbeat + bar / 2) / bar * bar, 2, "the new downbeat is not on a bar");

  // Own tempo: real time, unstretched, from where it starts.
  const auto start = take(song.session->snapshot().tempo.tick_to_sample(
      std::get<project::MusicalInterval>(clips(*song.session).front().placement).start,
      song.session->snapshot().sample_rate));
  REQUIRE_OK(song.session->warp().set_audio_clip_follow(song.clip, false), "Own tempo failed");
  const auto elapsed = std::get<project::ElapsedInterval>(clips(*song.session).front().placement);
  REQUIRE_EQ(elapsed.start.value, start.value,
             "Own tempo did not play unstretched from the clip start");
  REQUIRE_EQ(elapsed.end.value - elapsed.start.value, 48'000,
             "Own tempo did not play unstretched from the clip start");

  for (int step = 0; step < 4; ++step)
    REQUIRE_OK(song.session->history().undo(), "a clip strip edit did not undo");
  REQUIRE_EQ(clips(*song.session).front().placement, original,
             "the clip strip edits did not undo one step each");
  REQUIRE_TRUE(!song.session->snapshot().source_tempo_reviews.front().corrected_bpm,
               "the clip strip edits did not undo one step each");
  REQUIRE_TRUE(!song.session->snapshot().source_tempo_reviews.front().downbeat_frame,
               "the clip strip edits did not undo one step each");
}

// A loop is cut on its bar lines: one second at 240 BPM is exactly one bar,
// so its first frame is the downbeat even when the beat tracker locked onto
// the off-beats (a top loop's hats). Following leaves it where it was dropped.
void a_whole_bar_loop_follows_from_its_first_frame() {
  Song song;
  review_tempo(song, 240.0, {6'000, 18'000, 30'000, 42'000});
  REQUIRE_EQ(
      app::source_beat_grid(song.session->snapshot(), song.session->snapshot().sources.front().id)
          ->downbeat,
      0, "a whole-bar loop's downbeat was not its first frame");
  REQUIRE_OK(song.session->warp().set_audio_clip_follow(song.clip, true), "Follow failed");
  const auto placement = std::get<project::MusicalInterval>(clips(*song.session).front().placement);
  REQUIRE_EQ(placement.start.value, 0, "a followed loop moved from where it was dropped");
  REQUIRE_EQ(song.session->snapshot().lead_bars, 0,
             "a followed loop moved from where it was dropped");
}

// Bars before bar 1: everything on the timeline moves later by the same
// time, so it plays as before (a tempo ramp included), in one undo step.
void lead_bars_move_the_whole_timeline() {
  Song song;
  constexpr auto quarter = timeline::ticks_per_quarter;
  constexpr auto bar = 4 * quarter;
  REQUIRE_OK(song.session->song().set_tempo_event({8 * bar}, 140.0, timeline::TempoShape::step),
             "tempo ramp fixture failed");
  REQUIRE_OK(song.session->song().set_tempo_event({0}, 120.0, timeline::TempoShape::linear_bpm),
             "tempo ramp fixture failed");
  const auto before = song.session->snapshot();
  const auto start = std::get<project::ElapsedInterval>(clips(*song.session).front().placement);
  const auto ramp_end = take(before.tempo.tick_to_sample({8 * bar}, before.sample_rate)).value;
  REQUIRE_OK(song.session->song().insert_lead_bars(2), "lead bars were not one edit");
  REQUIRE_EQ(song.session->snapshot().revision, before.revision + 1, "lead bars were not one edit");
  const auto& after = song.session->snapshot();
  const auto moved = std::get<project::ElapsedInterval>(clips(*song.session).front().placement);
  // Two bars at the opening 120 BPM are four seconds: 192,000 frames.
  require(after.lead_bars == 2 && moved.start.value == start.start.value + 192'000 &&
              moved.end.value == start.end.value + 192'000,
          "an elapsed clip did not move by the lead bars' time");
  REQUIRE_EQ(take(after.tempo.tick_to_sample({10 * bar}, after.sample_rate)).value,
             ramp_end + 192'000, "the tempo ramp did not keep its timing after the lead bars");
  REQUIRE_OK(song.session->history().undo(), "lead bars did not undo in one step");
  REQUIRE_EQ(song.session->snapshot().lead_bars, 0, "lead bars did not undo in one step");
  REQUIRE_EQ(clips(*song.session).front().placement, project::Placement{start},
             "lead bars did not undo in one step");
}

void transpose_splits_into_semitones_and_cents() {
  const auto check = [](double semitones, int whole, int cents) {
    const auto parts = app::split_transpose(semitones);
    require(parts.semitones == whole && parts.cents == cents,
            "transpose split is wrong for " + std::to_string(semitones));
    REQUIRE_LT(std::abs(app::join_transpose(whole, cents) * 100.0 - (whole * 100 + cents)),
               oscillate::test::tolerance::double_accumulation, "transpose join is wrong");
  };
  check(0.0, 0, 0);
  check(7.04, 7, 4);
  check(-7.04, -7, -4);
  check(0.5, 0, 50);
  check(-0.5, 0, -50);
  check(2.51, 3, -49);
  check(-2.51, -3, 49);
  check(48.0, 48, 0);
  check(-48.0, -48, 0);
  REQUIRE_EQ(app::source_frame_from_view(48'000, 44'100, 48'000), 44'100,
             "view-to-source frame conversion is wrong");
  REQUIRE_EQ(app::source_frame_from_view(12'345, 48'000, 48'000), 12'345,
             "view-to-source frame conversion is wrong");
}

} // namespace

// The rate-conversion case needs worker source-rate conversion
// (OSCILLATE_BUILD_SOURCE_RESAMPLER); it runs as its own CTest case and
// reports a skip in builds without it.
constexpr std::string_view rate_conversion_case = "rate-conversion";

// --- Marker commands and tempo ramps on the arrangement (ADR-0112) ------------

// Fixture positions in project frames of the one-second, 48 kHz clip.
constexpr std::uint64_t first_marker = 12'000;
constexpr std::uint64_t second_marker = 30'000;
constexpr std::uint64_t clip_frames = 48'000;
// A tempo read back from frame positions, to a hundredth of a BPM.
constexpr double bpm_tolerance = 0.01;

[[nodiscard]] bool same_tempo(const std::vector<timeline::TempoEvent>& left,
                              const std::vector<timeline::TempoEvent>& right) {
  return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
    return a.tick == b.tick && a.bpm == b.bpm && a.shape_to_next == b.shape_to_next;
  });
}

void slide_moves_the_audio_under_a_marker_that_stays() {
  Song song;
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, first_marker)));
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, second_marker)));
  // The audio playing 2,000 frames earlier now plays at the first marker.
  constexpr std::uint64_t slid_from = 10'000;
  const auto revision = song.session->snapshot().revision;
  song.apply(take(app::slide_warp_marker(song.session->snapshot(), song.clip,
                                         {.index = 0, .output_frame = slid_from})));
  auto layout = song.layout();
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1, "a slide was not one edit");
  REQUIRE_EQ(layout.markers[0].output_frame, first_marker, "the slid marker left its place");
  REQUIRE_EQ(layout.markers[0].source_frame, slid_from, "the audio did not slide under the marker");
  REQUIRE_EQ(layout.markers[1].source_frame, second_marker, "a slide moved the next marker");
  // Never past a neighbour or a clip end.
  REQUIRE_FALSE(app::slide_warp_marker(song.session->snapshot(), song.clip,
                                       {.index = 0, .output_frame = second_marker + 1}),
                "a slide past the next marker was accepted");
  REQUIRE_FALSE(
      app::slide_warp_marker(song.session->snapshot(), song.clip, {.index = 0, .output_frame = 0}),
      "a slide onto the clip start was accepted");
  REQUIRE_FALSE(app::slide_warp_marker(song.session->snapshot(), song.clip,
                                       {.index = 2, .output_frame = slid_from}),
                "a slide of a missing marker was accepted");
  REQUIRE_OK(song.session->history().undo(), "the slide did not undo");
  REQUIRE_EQ(song.layout().markers[0].source_frame, first_marker, "the slide did not undo");
}

void straight_from_here_keeps_the_segment_tempo_to_the_end() {
  Song song;
  constexpr std::uint64_t third_marker = 40'000;
  for (const auto at : {first_marker, second_marker, third_marker})
    song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, at)));
  // The first segment now plays its 12,000 source frames over 18,000.
  constexpr std::uint64_t stretched_to = 18'000;
  song.apply(take(app::move_warp_marker(song.session->snapshot(), song.clip,
                                        {.index = 0, .output_frame = stretched_to})));

  // Remove all after: the marker stays, the later ones go, one edit.
  auto revision = song.session->snapshot().revision;
  song.apply(take(app::remove_warp_markers_after(song.session->snapshot(), song.clip, 0)));
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1, "Remove all after was not one edit");
  REQUIRE_EQ(song.layout().markers.size(), 1, "Remove all after kept a later marker");
  REQUIRE_FALSE(app::remove_warp_markers_after(song.session->snapshot(), song.clip, 0),
                "Remove all after on the last marker made an edit");
  REQUIRE_OK(song.session->history().undo(), "Remove all after did not undo");
  REQUIRE_EQ(song.layout().markers.size(), 3, "Remove all after did not undo");

  // Straight from here: the 1.5x stretch carries on from the marker, so the
  // remaining 36,000 source frames take 54,000 and the clip grows to 72,000.
  constexpr std::uint64_t straightened_end = stretched_to + (clip_frames - first_marker) * 3 / 2;
  const auto proposal =
      take(app::straighten_after_warp_marker(song.session->snapshot(), song.clip, 0));
  revision = song.session->snapshot().revision;
  REQUIRE_OK(song.session->warp().apply_audio_clip_warp(song.clip, proposal.settings_generation,
                                                        proposal.placement, proposal.processing),
             "Straight from here was refused");
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1,
             "Straight from here was not one edit");
  const auto layout = song.layout();
  REQUIRE_EQ(layout.markers.size(), 1, "Straight from here kept a later marker");
  REQUIRE_EQ(layout.output_frames, straightened_end, "the tempo did not carry on to the clip end");
  REQUIRE_EQ(layout.markers[0].output_frame, stretched_to, "Straight from here moved the marker");
  REQUIRE_OK(song.session->history().undo(), "Straight from here did not undo");
  REQUIRE_EQ(song.layout().output_frames, clip_frames, "Straight from here did not undo");
  REQUIRE_EQ(song.layout().markers.size(), 3, "Straight from here did not undo");
}

void a_segment_sets_the_track_tempo() {
  Song song;
  constexpr double project_bpm = 120.0;
  // Musical at 120 BPM: the second is two quarter notes.
  REQUIRE_OK(song.session->audio_clips().toggle_audio_clip_timebase(song.clip),
             "musical timebase toggle failed");
  review_tempo(song, project_bpm, {});
  // Half a second of source (24,000 frames) over the clip's last two thirds
  // (32,000 frames, 4/3 quarter notes at 120): 4/3 quarters per half second.
  constexpr std::uint64_t marker_source = 24'000;
  constexpr std::uint64_t marker_output = 16'000;
  constexpr double segment_bpm = 160.0;
  song.apply(take(app::add_warp_marker(song.session->snapshot(), song.clip, marker_source)));
  song.apply(take(app::move_warp_marker(song.session->snapshot(), song.clip,
                                        {.index = 0, .output_frame = marker_output})));
  const auto bpm = take(app::warp_segment_source_bpm(song.session->snapshot(), song.clip, 0));
  REQUIRE_NEAR(bpm, segment_bpm, bpm_tolerance, "the segment's own tempo is wrong");
  REQUIRE_FALSE(app::warp_segment_source_bpm(song.session->snapshot(), song.clip, 1),
                "a missing marker's segment had a tempo");
  const auto revision = song.session->snapshot().revision;
  REQUIRE_OK(song.session->warp().set_audio_clip_source_bpm(song.clip, bpm),
             "Set tempo from this segment was refused");
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1,
             "Set tempo from this segment was not one edit");
  REQUIRE_NEAR(*song.session->snapshot().source_tempo_reviews.front().corrected_bpm, segment_bpm,
               bpm_tolerance, "the track tempo is not the segment's");
  REQUIRE_OK(song.session->history().undo(), "Set tempo from this segment did not undo");
  REQUIRE_FALSE(song.session->snapshot().source_tempo_reviews.front().corrected_bpm.has_value(),
                "Set tempo from this segment did not undo");
}

// Seconds into a linear BPM ramp after `quarters` of its `length` quarter
// notes, from `first` to `last` BPM (the integral of 60 / bpm).
double ramp_seconds(double quarters, double length, double first, double last) {
  const auto delta = last - first;
  return 60.0 * length / delta * std::log1p(delta * quarters / (length * first));
}

void a_drawn_ramp_is_one_step_and_clips_follow_it() {
  Song song;
  constexpr auto quarter = timeline::ticks_per_quarter;
  constexpr double opening_bpm = 120.0;
  constexpr double ramp_end_bpm = 90.0;
  constexpr double other_bpm = 100.0;
  constexpr std::int64_t ramp_from = 4 * quarter;
  constexpr std::int64_t ramp_to = 12 * quarter;
  constexpr std::int64_t inside_ramp = 8 * quarter;
  auto& tempo = song.session->song();
  REQUIRE_OK(tempo.set_tempo_event({inside_ramp}, other_bpm, timeline::TempoShape::step),
             "fixture tempo event was refused");
  const auto before = song.session->snapshot().tempo.events();

  // A musical clip from the ramp's start with a marker two beats in.
  constexpr std::int64_t marker_tick = 2 * quarter;
  constexpr std::uint64_t marker_source = 24'000;
  auto processing = song.session->snapshot().tracks.front().clips.front().processing;
  processing.reviewed_warp_markers.push_back(
      {{marker_source}, timeline::TickPosition{marker_tick}});
  REQUIRE_OK(song.session->warp().apply_audio_clip_warp(
                 song.clip,
                 song.session->snapshot().tracks.front().clips.front().settings_generation,
                 project::MusicalInterval{{ramp_from}, {ramp_from + 4 * quarter}}, processing),
             "fixture musical clip was refused");

  const auto revision = song.session->snapshot().revision;
  REQUIRE_OK(
      tempo.set_tempo_ramp({ramp_from}, {ramp_to}, ramp_end_bpm, timeline::TempoShape::linear_bpm),
      "a drawn ramp was refused");
  REQUIRE_EQ(song.session->snapshot().revision, revision + 1, "a drawn ramp was not one edit");
  const auto& events = song.session->snapshot().tempo.events();
  REQUIRE_EQ(events.size(), 3, "the ramp kept a point inside it");
  REQUIRE_EQ(events[1].tick.value, ramp_from, "the ramp does not start where it was drawn");
  REQUIRE_EQ(events[1].bpm, opening_bpm, "the ramp did not start from the tempo playing there");
  REQUIRE_EQ(events[1].shape_to_next, timeline::TempoShape::linear_bpm, "the ramp lost its shape");
  REQUIRE_EQ(events[2].tick.value, ramp_to, "the ramp does not end where it was drawn");
  REQUIRE_EQ(events[2].bpm, ramp_end_bpm, "the ramp does not reach its end tempo");
  REQUIRE_EQ(events[2].shape_to_next, timeline::TempoShape::step, "the ramp's end does not hold");

  // The marker plays where the ramp puts its beat: two seconds of 120 BPM,
  // then two quarter notes into an eight-quarter 120 -> 90 ramp.
  const auto ramp_length = static_cast<double>(ramp_to - ramp_from) / quarter;
  const auto expected = (static_cast<double>(ramp_from) / quarter * 60.0 / opening_bpm +
                         ramp_seconds(static_cast<double>(marker_tick) / quarter, ramp_length,
                                      opening_bpm, ramp_end_bpm)) *
                        oscillate::test::sample_rate_48k;
  const auto layout = song.layout();
  REQUIRE_NEAR(static_cast<double>(layout.project_start + layout.markers.front().output_frame),
               expected, 1.0, "the warp marker did not follow the ramp");

  REQUIRE_OK(song.session->history().undo(), "the ramp did not undo");
  REQUIRE_TRUE(same_tempo(song.session->snapshot().tempo.events(), before),
               "the ramp did not undo exactly");

  // Dragging a point is one edit; the first point stays at the start.
  REQUIRE_OK(tempo.move_tempo_event({inside_ramp}, {ramp_to}, ramp_end_bpm),
             "moving a tempo point was refused");
  REQUIRE_EQ(song.session->snapshot().tempo.events().back().tick.value, ramp_to,
             "the tempo point did not move");
  REQUIRE_FALSE(tempo.move_tempo_event({0}, {ramp_from}, opening_bpm),
                "the opening tempo point left the start");
  REQUIRE_OK(song.session->history().undo(), "moving a tempo point did not undo");
  REQUIRE_TRUE(same_tempo(song.session->snapshot().tempo.events(), before),
               "moving a tempo point did not undo");

  // Removing the end of a ramp leaves the point before it holding.
  REQUIRE_OK(
      tempo.set_tempo_ramp({ramp_from}, {ramp_to}, ramp_end_bpm, timeline::TempoShape::ease_in_out),
      "an S-curve ramp was refused");
  REQUIRE_OK(tempo.remove_tempo_event({ramp_to}), "removing a ramp's end was refused");
  REQUIRE_EQ(song.session->snapshot().tempo.events().back().shape_to_next,
             timeline::TempoShape::step, "the last tempo point still ramps to nothing");
}

// Two eight-bar tracks at 128 BPM; the second starts two bars before the
// first ends, so they overlap for two bars.
void ramp_to_this_tracks_tempo_spans_the_transition() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  constexpr double track_bpm = 128.0;
  constexpr auto quarter = timeline::ticks_per_quarter;
  constexpr auto bar = 4 * quarter;
  constexpr std::int64_t bars_per_track = 8;
  constexpr std::int64_t incoming_bar = 6;
  // Eight bars at 128 BPM, to the frame.
  constexpr auto track_frames =
      static_cast<std::uint32_t>(bars_per_track * 4 * 60 * rate / track_bpm);
  Directory directory;
  auto session = take(app::ProjectSession::create(directory.path() / "song"));
  const auto wav = make_wav(directory.path(), track_frames, rate);
  for (int track = 0; track < 2; ++track) {
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "fixture import failed");
    REQUIRE_OK(session->imports().import_wav_audio(fd), "fixture import failed");
    (void)::close(fd);
  }
  const auto outgoing = session->snapshot().tracks[0].clips.front().id;
  const auto incoming = session->snapshot().tracks[1].clips.front().id;
  REQUIRE_FALSE(session->song().ramp_to_clip_tempo(incoming),
                "a ramp was planned before the track had a tempo");
  for (const auto& source : session->snapshot().sources) {
    oscillate::analysis::TempoEstimate estimate;
    estimate.identity = {take(app::tempo_cache_key(source)).hex(), 1};
    estimate.sample_rate = source.sample_rate;
    estimate.frames = source.frames;
    estimate.bpm = track_bpm;
    estimate.confidence = 0.9;
    REQUIRE_OK(session->source_analysis().accept_source_tempo_analysis(
                   source.id, session->snapshot().revision, estimate),
               "tempo review fixture was rejected");
  }
  const auto place = [&](project::ClipId clip, std::int64_t first_bar) {
    const auto* found = &session->snapshot().tracks[0].clips.front();
    for (const auto& track : session->snapshot().tracks)
      for (const auto& candidate : track.clips)
        if (candidate.id == clip)
          found = &candidate;
    REQUIRE_OK(
        session->warp().apply_audio_clip_warp(
            clip, found->settings_generation,
            project::MusicalInterval{{first_bar * bar}, {(first_bar + bars_per_track) * bar}},
            found->processing),
        "fixture placement was refused");
  };
  place(outgoing, 0);
  place(incoming, bars_per_track);
  REQUIRE_FALSE(session->song().ramp_to_clip_tempo(incoming),
                "a ramp was planned without a transition");
  place(incoming, incoming_bar);

  const auto planned = take(app::plan_transition_ramp(session->snapshot(), incoming));
  REQUIRE_EQ(planned.from.value, incoming_bar * bar, "the ramp does not start with the transition");
  REQUIRE_EQ(planned.to.value, bars_per_track * bar, "the ramp does not end with the transition");
  REQUIRE_EQ(planned.bpm, track_bpm, "the ramp does not end at the track's tempo");
  const auto before = session->snapshot().tempo.events();
  const auto revision = session->snapshot().revision;
  REQUIRE_OK(session->song().ramp_to_clip_tempo(incoming), "Ramp to this track's tempo failed");
  REQUIRE_EQ(session->snapshot().revision, revision + 1,
             "Ramp to this track's tempo was not one edit");
  const auto& events = session->snapshot().tempo.events();
  REQUIRE_EQ(events.size(), 3, "the transition ramp is not one ramp");
  REQUIRE_EQ(events[1].shape_to_next, app::transition_ramp_shape,
             "the transition ramp has the wrong shape");
  REQUIRE_EQ(events.back().bpm, track_bpm, "the mix does not land at the track's tempo");
  REQUIRE_OK(session->history().undo(), "Ramp to this track's tempo did not undo");
  REQUIRE_TRUE(same_tempo(session->snapshot().tempo.events(), before),
               "Ramp to this track's tempo did not undo exactly");
}

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string_view(argv[1]) == rate_conversion_case) {
      if (!OSCILLATE_HAVE_SOURCE_RESAMPLER)
        return OSCILLATE_TEST_SKIP_RETURN_CODE;
      suggestions_colliding_after_rate_conversion_are_skipped();
      return 0;
    }
    markers_add_move_and_remove_as_single_undo_steps();
    reviewed_suggestions_apply_atomically_without_moving_audio();
    musical_markers_use_ticks_and_round_trip();
    onset_review_is_durable_and_keeps_corrections();
    auto_warp_puts_reviewed_beats_on_the_grid();
    tempo_conform_stretches_to_the_project_tempo();
    clip_strip_follows_corrects_and_sets_the_downbeat();
    a_whole_bar_loop_follows_from_its_first_frame();
    lead_bars_move_the_whole_timeline();
    transpose_splits_into_semitones_and_cents();
    split_keeps_markers_with_their_audio();
    trim_keeps_marker_timing_and_never_drops_a_marker();
    slip_moves_audio_with_its_markers();
    musical_split_and_trim_rebase_ticks();
    slide_moves_the_audio_under_a_marker_that_stays();
    straight_from_here_keeps_the_segment_tempo_to_the_end();
    a_segment_sets_the_track_tempo();
    a_drawn_ramp_is_one_step_and_clips_follow_it();
    ramp_to_this_tracks_tempo_spans_the_transition();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
