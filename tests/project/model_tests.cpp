#include "oscillate/project/insert_projection.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <bit>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace oscillate;
using namespace oscillate::project;
using foundation::ErrorCode;
using foundation::Result;
using timeline::MeterEvent;
using timeline::MeterMap;
using timeline::TempoEvent;
using timeline::TempoMap;
using timeline::TempoShape;

static_assert(!std::is_copy_constructible_v<ProjectModel>);
static_assert(std::is_move_constructible_v<ProjectModel>);
static_assert(!std::is_copy_constructible_v<AppliedRevision>);

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t low) { return take(Id::from_bits({0, low})); }

TempoMap tempo(double bpm = 120.0, std::uint64_t revision = 1) {
  return take(TempoMap::create({TempoEvent{{0}, bpm, TempoShape::step}}, revision));
}

MeterMap meter(std::uint8_t numerator = 4, std::uint64_t revision = 1) {
  return take(MeterMap::create({MeterEvent{{0}, numerator, 4}}, revision));
}

class FakeGate final : public TransportGate {
public:
  struct Lease final : StoppedTransportLease {
    explicit Lease(FakeGate& gate) : gate_(gate) { ++gate_.live; }
    ~Lease() override { --gate_.live; }
    FakeGate& gate_;
  };
  Result<std::unique_ptr<StoppedTransportLease>> acquire_stopped() override {
    ++acquisitions;
    if (!allow || live != 0) {
      return Result<std::unique_ptr<StoppedTransportLease>>::failure(
          {ErrorCode::unavailable, "transport is running"});
    }
    if (return_null) {
      return Result<std::unique_ptr<StoppedTransportLease>>::success(nullptr);
    }
    return Result<std::unique_ptr<StoppedTransportLease>>::success(std::make_unique<Lease>(*this));
  }
  bool allow = true;
  bool return_null = false;
  int live = 0;
  int acquisitions = 0;
  bool attempt_start() {
    if (live != 0) {
      return false;
    }
    allow = false;
    return true;
  }
  void stop() { allow = true; }
};

ProjectModel model(FakeGate& gate, std::uint32_t rate = oscillate::test::sample_rate_48k) {
  return take(ProjectModel::create(id<ProjectId>(1), rate, tempo(), meter(), gate));
}

Clip clip(std::uint64_t clip_number, Placement placement, std::uint64_t source_number = 1) {
  return {id<ClipId>(clip_number), id<SourceId>(source_number), SourceRange{{0}, {100'000}},
          placement, 1};
}

SourceRecord source(std::uint64_t number, std::uint64_t frames = 100'000) {
  return {
      id<SourceId>(number), SourceEncoding::wav, std::string(64, 'a'), 400'044, 48'000, 2, frames};
}

void setup_track(ProjectModel& project, TrackId track) {
  REQUIRE_OK(project.apply({AddTrack{track}}));
}

void track_insertion_is_ordered_and_atomic() {
  FakeGate gate;
  auto project = model(gate);
  const auto first = id<TrackId>(1);
  const auto inserted = id<TrackId>(2);
  REQUIRE_OK(project.apply({AddTrack{first}}));
  const auto before = project.snapshot();
  REQUIRE_TRUE(!project.apply({AddTrack{inserted, TrackKind::instrument, 2}}),
               "out-of-range track insertion changed the project");
  REQUIRE_EQ(project.snapshot(), before, "out-of-range track insertion changed the project");
  auto added = take(project.apply({AddTrack{inserted, TrackKind::instrument, 0}}));
  REQUIRE_EQ(added.snapshot->tracks.size(), 2,
             "top insertion did not put the new track before the existing track");
  REQUIRE_EQ(added.snapshot->tracks[0].id, inserted,
             "top insertion did not put the new track before the existing track");
  REQUIRE_EQ(added.snapshot->tracks[1].id, first,
             "top insertion did not put the new track before the existing track");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks.size(), 1,
             "one undo did not restore the original order");
  REQUIRE_EQ(project.snapshot()->tracks[0].id, first,
             "one undo did not restore the original order");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].id, inserted,
             "redo did not restore the inserted order");
}

void track_move_preserves_identity_and_is_undoable() {
  FakeGate gate;
  auto project = model(gate);
  const auto first = id<TrackId>(41);
  const auto second = id<TrackId>(42);
  const auto third = id<TrackId>(43);
  REQUIRE_OK(project.apply({AddTrack{first}, AddTrack{second}, AddTrack{third}}));
  REQUIRE_OK(project.apply({SetTrackMuted{first, true}}));
  const auto before = project.snapshot();
  REQUIRE_TRUE(!project.apply({MoveTrack{first, 3}}), "invalid track moves changed the project");
  REQUIRE_TRUE(!project.apply({MoveTrack{first, 0}}), "invalid track moves changed the project");
  REQUIRE_TRUE(!project.apply({MoveTrack{id<TrackId>(99), 1}}),
               "invalid track moves changed the project");
  REQUIRE_EQ(project.snapshot(), before, "invalid track moves changed the project");
  const auto moved = take(project.apply({MoveTrack{first, 2}}));
  REQUIRE_EQ(moved.snapshot->tracks[0].id, second, "track move lost order or track state");
  REQUIRE_EQ(moved.snapshot->tracks[1].id, third, "track move lost order or track state");
  REQUIRE_EQ(moved.snapshot->tracks[2].id, first, "track move lost order or track state");
  REQUIRE_TRUE(moved.snapshot->tracks[2].muted, "track move lost order or track state");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks, before->tracks,
             "one undo did not restore track order");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[2].id, first,
             "redo did not restore track order");
  const auto moved_back = take(project.apply({MoveTrack{first, 0}}));
  REQUIRE_EQ(moved_back.snapshot->tracks[0].id, first,
             "moving a track upward did not restore the expected order");
  REQUIRE_EQ(moved_back.snapshot->tracks[1].id, second,
             "moving a track upward did not restore the expected order");
  REQUIRE_EQ(moved_back.snapshot->tracks[2].id, third,
             "moving a track upward did not restore the expected order");
  // Undo and redo of a mixer control are routing changes, as the edits are.
  REQUIRE_OK(project.apply({SetTrackLevelPan{second, 0.5, 0.25}}));
  REQUIRE_TRUE(take(project.undo()).changes.routing, "undoing a level change skipped routing");
  REQUIRE_TRUE(take(project.redo()).changes.routing, "redoing a level change skipped routing");
  REQUIRE_OK(project.apply({SetTrackMuted{second, true}}));
  REQUIRE_TRUE(take(project.undo()).changes.routing, "undoing a mute skipped routing");
}

void revisions_and_atomic_compounds() {
  FakeGate gate;
  auto project = model(gate);
  auto original = project.snapshot();
  REQUIRE_EQ(original->revision, 1, "wrong initial snapshot");
  REQUIRE_TRUE(original->tracks.empty(), "wrong initial snapshot");
  auto track = id<TrackId>(10);
  auto left = clip(20, MusicalInterval{{0}, {timeline::ticks_per_quarter}});
  auto right = clip(21, ElapsedInterval{{24'000}, {48'000}});
  auto applied =
      take(project.apply({AddTrack{track}, AddClip{track, left}, AddClip{track, right}}));
  REQUIRE_EQ(applied.snapshot->revision, 2, "compound did not publish once");
  REQUIRE_EQ(applied.snapshot->tracks[0].clips.size(), 2, "compound did not publish once");
  REQUIRE_EQ(applied.changes.tracks.size(), 1, "change set missed affected IDs");
  REQUIRE_EQ(applied.changes.clips.size(), 2, "change set missed affected IDs");
  REQUIRE_TRUE(original->tracks.empty(), "retained snapshot changed");
  REQUIRE_EQ(original->revision, 1, "retained snapshot changed");
  REQUIRE_EQ(project.undo_depth(), 1, "compound created multiple undo steps");

  auto before = project.snapshot();
  auto failed = project.apply(
      {RemoveClip{left.id}, AddClip{track, clip(22, ElapsedInterval{{23'999}, {24'001}})}});
  REQUIRE_TRUE(!failed, "overlapping compound accepted");
  REQUIRE_EQ(failed.error().code, ErrorCode::conflict, "overlapping compound accepted");
  REQUIRE_NE(failed.error().message.find(track.to_string()), std::string::npos,
             "conflict did not name stable IDs");
  REQUIRE_NE(failed.error().message.find(right.id.to_string()), std::string::npos,
             "conflict did not name stable IDs");
  REQUIRE_EQ(project.snapshot(), before, "failed compound changed published state/history");
  REQUIRE_EQ(project.undo_depth(), 1, "failed compound changed published state/history");
  require(!project.apply({}).has_value(), "empty command accepted");
  require(!project.apply({AddTrack{track}}).has_value(), "duplicate track accepted");
  require(!project.apply({RemoveTrack{id<TrackId>(99)}}).has_value(), "missing track removed");
  require(!project.apply({AddClip{id<TrackId>(99), left}}).has_value(), "missing parent accepted");
  require(!project.apply({AddClip{track, left}}).has_value(), "duplicate clip accepted");
  require(!project.apply({RemoveClip{id<ClipId>(99)}}).has_value(), "missing clip removed");
  require(!project.apply({SetClipPlacement{id<ClipId>(99), left.placement}}).has_value(),
          "missing clip moved");
  REQUIRE_EQ(project.snapshot(), before, "rejected command published revision");
}

void rendered_intervals_and_map_revalidation() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  require(!project.apply({AddClip{track, clip(1, MusicalInterval{{0}, {1}})}}),
          "positive tick span that rounds to zero samples accepted");
  REQUIRE_OK(project.apply({AddClip{track, clip(1, MusicalInterval{{0}, {40'000}})}}));
  auto overlap = project.apply({AddClip{track, clip(2, ElapsedInterval{{0}, {1}})}});
  REQUIRE_TRUE(!overlap, "mixed anchors overlapping one sample accepted");
  REQUIRE_EQ(overlap.error().code, ErrorCode::conflict,
             "mixed anchors overlapping one sample accepted");
  REQUIRE_NE(overlap.error().message.find(id<ClipId>(1).to_string()), std::string::npos,
             "mixed overlap omitted clip IDs");
  REQUIRE_NE(overlap.error().message.find(id<ClipId>(2).to_string()), std::string::npos,
             "mixed overlap omitted clip IDs");
  require(!project.apply({AddClip{track, clip(3, MusicalInterval{{0}, {0}})}}),
          "zero musical span accepted");
  require(!project.apply({AddClip{track, clip(3, ElapsedInterval{{-1}, {1}})}}),
          "negative elapsed anchor accepted");
  require(!project.apply({AddClip{track, clip(3, ElapsedInterval{{1}, {1}})}}),
          "zero elapsed span accepted");
  REQUIRE_OK(project.apply({AddClip{track, clip(2, ElapsedInterval{{1}, {20}})}}));
  auto previous = project.snapshot();
  auto changed = project.apply({ReplaceTempoMap{tempo(60, 2)}});
  REQUIRE_TRUE(!changed, "tempo edit did not revalidate mixed-anchor overlap");
  REQUIRE_EQ(changed.error().code, ErrorCode::conflict,
             "tempo edit did not revalidate mixed-anchor overlap");
  REQUIRE_EQ(project.snapshot(), previous, "rejected tempo map published");
  auto moved = project.apply({SetClipPlacement{id<ClipId>(2), ElapsedInterval{{0}, {1}}}});
  REQUIRE_TRUE(!moved, "timebase/placement overlap accepted");
  REQUIRE_EQ(project.snapshot(), previous, "timebase/placement overlap accepted");
  auto meter_change = take(project.apply({ReplaceMeterMap{meter(3, 2)}}));
  REQUIRE_TRUE(meter_change.changes.timing, "meter map not updated");
  REQUIRE_EQ(meter_change.snapshot->meter.events()[0].numerator, 3, "meter map not updated");
  REQUIRE_EQ(meter_change.changes.tracks.size(), 1,
             "timing change set missed affected track or clips");
  REQUIRE_EQ(meter_change.changes.clips.size(), 2,
             "timing change set missed affected track or clips");
  REQUIRE_OK(project.undo());
  REQUIRE_EQ(project.snapshot()->meter.events()[0].numerator, 4, "meter undo failed");
}

void gestures_undo_and_redo() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  REQUIRE_OK(project.apply({AddClip{track, clip(1, ElapsedInterval{{0}, {10}})}}));
  const auto baseline_depth = project.undo_depth();
  REQUIRE_OK(project.apply({SetClipPlacement{id<ClipId>(1), ElapsedInterval{{10}, {20}}}}, 7));
  const auto accepted_before_failure = project.snapshot();
  REQUIRE_TRUE(!project.apply({SetClipPlacement{id<ClipId>(1), ElapsedInterval{{20}, {20}}}}, 7),
               "rejected gesture step changed snapshot");
  REQUIRE_EQ(project.snapshot(), accepted_before_failure, "rejected gesture step changed snapshot");
  REQUIRE_OK(project.apply({SetClipPlacement{id<ClipId>(1), ElapsedInterval{{20}, {30}}}}, 7));
  REQUIRE_EQ(project.undo_depth(), baseline_depth + 1,
             "rejected gesture step broke successful gesture coalescing");
  REQUIRE_EQ(project.snapshot()->tracks[0].clips[0].settings_generation, 3,
             "placement generation not advanced");
  auto revision = project.snapshot()->revision;
  auto undone = take(project.undo());
  REQUIRE_EQ(undone.snapshot->revision, revision + 1,
             "gesture undo did not restore exact before state");
  REQUIRE_EQ(std::get<ElapsedInterval>(undone.snapshot->tracks[0].clips[0].placement).start.value,
             0, "gesture undo did not restore exact before state");
  REQUIRE_EQ(undone.snapshot->tracks[0].clips[0].settings_generation, 1,
             "gesture undo did not restore exact before state");
  auto redone = take(project.redo());
  REQUIRE_EQ(redone.snapshot->revision, revision + 2,
             "gesture redo did not restore exact after state");
  REQUIRE_EQ(std::get<ElapsedInterval>(redone.snapshot->tracks[0].clips[0].placement).start.value,
             20, "gesture redo did not restore exact after state");
  REQUIRE_EQ(redone.snapshot->tracks[0].clips[0].settings_generation, 3,
             "gesture redo did not restore exact after state");
  REQUIRE_OK(project.undo());
  REQUIRE_OK(project.apply({AddTrack{id<TrackId>(11)}}));
  REQUIRE_EQ(project.redo_depth(), 0, "new command did not invalidate redo");
  REQUIRE_TRUE(!project.redo(), "new command did not invalidate redo");
  require(!project.apply({AddTrack{id<TrackId>(12)}}, 0), "zero gesture accepted");
  REQUIRE_OK(project.apply({SetClipPlacement{id<ClipId>(1), ElapsedInterval{{10}, {20}}}}, 7));
  const auto depth_before_navigation = project.undo_depth();
  REQUIRE_OK(project.undo());
  REQUIRE_OK(project.redo());
  REQUIRE_OK(project.apply({SetClipPlacement{id<ClipId>(1), ElapsedInterval{{20}, {30}}}}, 7));
  REQUIRE_EQ(project.undo_depth(), depth_before_navigation + 1,
             "gesture ID reused after history navigation coalesced old edit");
}

void unchanged_edits_preserve_redo_history() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio = id<TrackId>(10);
  const auto group = id<TrackId>(11);
  const auto item = clip(12, ElapsedInterval{{0}, {100}});
  REQUIRE_OK(project.apply(
      {AddTrack{audio}, AddTrack{group, TrackKind::group_bus}, AddClip{audio, item}}));
  REQUIRE_OK(project.apply({SetTrackMainOutput{audio, group}}));
  REQUIRE_OK(project.undo());
  const auto unchanged = project.snapshot();
  const auto undo_depth = project.undo_depth();
  const auto redo_depth = project.redo_depth();
  REQUIRE_TRUE(!project.apply({SetTrackMainOutput{audio, std::nullopt}}),
               "unchanged command advanced revision or cleared redo history");
  REQUIRE_TRUE(!project.apply({SetClipPlacement{item.id, item.placement}}),
               "unchanged command advanced revision or cleared redo history");
  REQUIRE_TRUE(!project.apply({SetClipSourceRange{item.id, item.source_range}}),
               "unchanged command advanced revision or cleared redo history");
  REQUIRE_TRUE(!project.apply({SetSampleRate{48'000}}),
               "unchanged command advanced revision or cleared redo history");
  REQUIRE_EQ(project.snapshot(), unchanged,
             "unchanged command advanced revision or cleared redo history");
  REQUIRE_EQ(project.undo_depth(), undo_depth,
             "unchanged command advanced revision or cleared redo history");
  REQUIRE_EQ(project.redo_depth(), redo_depth,
             "unchanged command advanced revision or cleared redo history");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].main_output, group,
             "unchanged command made redo unusable");
}

void rate_gate_conversion_and_exact_undo() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  REQUIRE_OK(project.apply({AddClip{track, clip(1, ElapsedInterval{{80}, {240}})}}));
  gate.allow = false;
  auto before = project.snapshot();
  auto stopped = project.apply({SetSampleRate{44'100}});
  REQUIRE_TRUE(!stopped, "rate changed while transport running");
  REQUIRE_EQ(stopped.error().code, ErrorCode::unavailable, "rate changed while transport running");
  REQUIRE_EQ(project.snapshot(), before, "rate changed while transport running");
  gate.stop();
  auto converted = take(project.apply({SetSampleRate{44'100}}));
  REQUIRE_EQ(gate.live, 1, "rate scaling did not use exact rational ties-to-even conversion");
  REQUIRE_EQ(gate.acquisitions, 2,
             "rate scaling did not use exact rational ties-to-even conversion");
  REQUIRE_TRUE(converted.stopped_transport,
               "rate scaling did not use exact rational ties-to-even conversion");
  REQUIRE_EQ(
      std::get<ElapsedInterval>(converted.snapshot->tracks[0].clips[0].placement).start.value, 74,
      "rate scaling did not use exact rational ties-to-even conversion");
  REQUIRE_EQ(std::get<ElapsedInterval>(converted.snapshot->tracks[0].clips[0].placement).end.value,
             220, "rate scaling did not use exact rational ties-to-even conversion");
  require(!gate.attempt_start(), "transport started while result held stopped lease");
  REQUIRE_TRUE(!project.undo(), "rate undo acquired a second exclusive stopped lease");
  REQUIRE_EQ(project.snapshot(), converted.snapshot,
             "rate undo acquired a second exclusive stopped lease");
  converted.stopped_transport.reset();
  REQUIRE_EQ(gate.live, 0, "transport could not start after released rate-edit lease");
  REQUIRE_TRUE(gate.attempt_start(), "transport could not start after released rate-edit lease");
  REQUIRE_TRUE(!project.undo(), "rate undo bypassed running transport gate");
  REQUIRE_EQ(project.snapshot(), converted.snapshot, "rate undo bypassed running transport gate");
  gate.stop();
  auto undone = take(project.undo());
  REQUIRE_TRUE(undone.stopped_transport, "rate undo did not retain stopped lease for caller");
  REQUIRE_EQ(gate.live, 1, "rate undo did not retain stopped lease for caller");
  REQUIRE_TRUE(!gate.attempt_start(), "rate undo did not retain stopped lease for caller");
  REQUIRE_EQ(undone.snapshot->sample_rate, 48'000,
             "rate undo reverse-scaled instead of restoring exact old coordinates");
  REQUIRE_EQ(std::get<ElapsedInterval>(undone.snapshot->tracks[0].clips[0].placement).start.value,
             80, "rate undo reverse-scaled instead of restoring exact old coordinates");
  REQUIRE_EQ(std::get<ElapsedInterval>(undone.snapshot->tracks[0].clips[0].placement).end.value,
             240, "rate undo reverse-scaled instead of restoring exact old coordinates");
  undone.stopped_transport.reset();
  require(gate.attempt_start(), "transport did not start after undo lease release");
  gate.stop();
  auto redone = take(project.redo());
  REQUIRE_TRUE(redone.stopped_transport, "rate redo did not retain stopped lease for caller");
  REQUIRE_EQ(gate.live, 1, "rate redo did not retain stopped lease for caller");
  REQUIRE_TRUE(!gate.attempt_start(), "rate redo did not retain stopped lease for caller");
  REQUIRE_EQ(project.snapshot()->sample_rate, 44'100, "rate redo failed");
  redone.stopped_transport.reset();
  require(gate.attempt_start(), "transport did not start after redo lease release");
  gate.stop();
  require(!project.apply({SetSampleRate{192'000}}), "unsupported rate accepted");
  gate.return_null = true;
  auto null_lease = project.apply({SetSampleRate{48'000}});
  REQUIRE_TRUE(!null_lease, "null stopped lease accepted");
  REQUIRE_EQ(null_lease.error().code, ErrorCode::internal, "null stopped lease accepted");
}

void removals_undo() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  auto item = clip(1, ElapsedInterval{{0}, {100}}, 5);
  REQUIRE_OK(project.apply({AddClip{track, item}}));
  REQUIRE_OK(project.apply({RemoveClip{item.id}}));
  require(project.snapshot()->tracks[0].clips.empty(), "clip removal failed");
  REQUIRE_OK(project.undo());
  REQUIRE_EQ(project.snapshot()->tracks[0].clips.size(), 1, "clip removal undo failed");
  REQUIRE_OK(project.apply({RemoveTrack{track}}));
  require(project.snapshot()->tracks.empty(), "track removal failed");
  REQUIRE_OK(project.undo());
  REQUIRE_EQ(project.snapshot()->tracks.size(), 1, "track removal undo failed");
}

void invalid_generation_and_overflow() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  require(
      !project.apply({AddClip{track, Clip{id<ClipId>(1), id<SourceId>(1), SourceRange{{0}, {100}},
                                          ElapsedInterval{{0}, {1}}, 0}}}),
      "zero settings generation accepted");
  REQUIRE_OK(project.apply({AddClip{track, Clip{id<ClipId>(1), id<SourceId>(1),
                                                SourceRange{{0}, {100}}, ElapsedInterval{{0}, {1}},
                                                std::numeric_limits<std::uint64_t>::max()}}}));
  auto before = project.snapshot();
  REQUIRE_TRUE(!project.apply({SetClipPlacement{id<ClipId>(1), ElapsedInterval{{1}, {2}}}}),
               "settings generation overflow changed state");
  REQUIRE_EQ(project.snapshot(), before, "settings generation overflow changed state");
  REQUIRE_TRUE(
      !project.apply({SetSampleRate{44'100}, AddClip{track, clip(2, ElapsedInterval{{0}, {1}})}}),
      "invalid compound rate edit partially committed");
  REQUIRE_EQ(project.snapshot(), before, "invalid compound rate edit partially committed");
}

void decoded_snapshot_validation_and_limits() {
  FakeGate gate;
  require(!ProjectModel::create(id<ProjectId>(1), 192'000, tempo(), meter(), gate),
          "unsupported initial project rate accepted");
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  REQUIRE_OK(project.apply({AddClip{track, clip(1, ElapsedInterval{{0}, {1}})}}));
  const auto valid = *project.snapshot();
  auto opened = take(ProjectModel::open(valid, gate));
  REQUIRE_EQ(opened.snapshot()->revision, valid.revision,
             "decoded snapshot did not reopen without session history");
  REQUIRE_EQ(opened.undo_depth(), 0, "decoded snapshot did not reopen without session history");
  auto invalid = valid;
  invalid.revision = 0;
  require(!ProjectModel::open(invalid, gate), "zero revision accepted");
  invalid = valid;
  invalid.sample_rate = 0;
  require(!ProjectModel::open(invalid, gate), "invalid decoded rate accepted");
  invalid = valid;
  invalid.tracks.push_back(invalid.tracks[0]);
  require(!ProjectModel::open(invalid, gate), "duplicate decoded track accepted");
  invalid = valid;
  invalid.tracks.push_back({id<TrackId>(11), {invalid.tracks[0].clips[0]}});
  require(!ProjectModel::open(invalid, gate), "duplicate decoded clip accepted");
  invalid = valid;
  invalid.tracks[0].clips[0].settings_generation = 0;
  require(!ProjectModel::open(invalid, gate), "zero decoded settings generation accepted");
  invalid = valid;
  invalid.tracks[0].clips[0].placement = MusicalInterval{
      {std::numeric_limits<std::int64_t>::max() - 1}, {std::numeric_limits<std::int64_t>::max()}};
  require(!ProjectModel::open(invalid, gate), "overflowing musical anchor accepted");
  invalid = valid;
  invalid.revision = std::numeric_limits<std::uint64_t>::max();
  auto exhausted = take(ProjectModel::open(invalid, gate));
  REQUIRE_TRUE(!exhausted.apply({AddTrack{id<TrackId>(12)}}), "revision wrapped at UINT64_MAX");
  REQUIRE_EQ(exhausted.snapshot()->revision, invalid.revision, "revision wrapped at UINT64_MAX");
  REQUIRE_TRUE(!exhausted.restore_snapshot(valid), "saved snapshot restore wrapped at UINT64_MAX");
  REQUIRE_EQ(exhausted.snapshot()->revision, invalid.revision,
             "saved snapshot restore wrapped at UINT64_MAX");
}

void saved_snapshot_restore_is_atomic_and_undoable() {
  FakeGate gate;
  auto project = model(gate);
  const auto first = id<TrackId>(31);
  const auto second = id<TrackId>(32);
  REQUIRE_OK(project.apply({AddTrack{first}}));
  auto saved = *project.snapshot();
  REQUIRE_OK(project.apply({AddTrack{second}}));
  const auto before = project.snapshot();
  const auto undo_before = project.undo_depth();

  auto wrong_project = saved;
  wrong_project.id = id<ProjectId>(99);
  REQUIRE_TRUE(!project.restore_snapshot(wrong_project),
               "foreign saved project changed the model or history");
  REQUIRE_EQ(project.snapshot(), before, "foreign saved project changed the model or history");
  REQUIRE_EQ(project.undo_depth(), undo_before,
             "foreign saved project changed the model or history");
  auto invalid = saved;
  invalid.tracks.push_back(invalid.tracks.front());
  REQUIRE_TRUE(!project.restore_snapshot(invalid), "invalid saved snapshot partially restored");
  REQUIRE_EQ(project.snapshot(), before, "invalid saved snapshot partially restored");
  REQUIRE_EQ(project.undo_depth(), undo_before, "invalid saved snapshot partially restored");
  saved.revision = 0;
  REQUIRE_TRUE(!project.restore_snapshot(saved), "zero saved revision was accepted");
  REQUIRE_EQ(project.snapshot(), before, "zero saved revision was accepted");
  saved.revision = 2;

  auto restored = take(project.restore_snapshot(saved));
  REQUIRE_EQ(restored.snapshot->revision, before->revision + 1,
             "saved snapshot restore did not publish one complete monotonic revision");
  REQUIRE_EQ(restored.snapshot->tracks.size(), 1,
             "saved snapshot restore did not publish one complete monotonic revision");
  REQUIRE_EQ(restored.snapshot->tracks.front().id, first,
             "saved snapshot restore did not publish one complete monotonic revision");
  REQUIRE_EQ(restored.changes.tracks.size(), 2,
             "saved snapshot restore did not publish one complete monotonic revision");
  REQUIRE_EQ(project.undo_depth(), undo_before + 1,
             "saved snapshot restore did not publish one complete monotonic revision");
  REQUIRE_EQ(project.redo_depth(), 0,
             "saved snapshot restore did not publish one complete monotonic revision");
  REQUIRE_TRUE(!restored.stopped_transport,
               "saved snapshot restore did not publish one complete monotonic revision");
  auto undone = take(project.undo());
  REQUIRE_EQ(undone.snapshot->revision, restored.snapshot->revision + 1,
             "restore undo did not recover the displaced project state");
  REQUIRE_EQ(undone.snapshot->tracks.size(), 2,
             "restore undo did not recover the displaced project state");
  REQUIRE_EQ(undone.snapshot->tracks.back().id, second,
             "restore undo did not recover the displaced project state");
  auto redone = take(project.redo());
  REQUIRE_EQ(redone.snapshot->revision, undone.snapshot->revision + 1,
             "restore redo did not recover the selected saved state");
  REQUIRE_EQ(redone.snapshot->tracks.size(), 1,
             "restore redo did not recover the selected saved state");
  REQUIRE_OK(project.undo());
  REQUIRE_OK(project.restore_snapshot(saved));
  REQUIRE_EQ(project.redo_depth(), 0, "new restore did not clear superseded redo history");
  REQUIRE_TRUE(!project.redo(), "new restore did not clear superseded redo history");
}

void saved_snapshot_restore_respects_rate_gate() {
  FakeGate gate;
  auto project = model(gate);
  auto saved = *project.snapshot();
  saved.sample_rate = oscillate::test::sample_rate_44k;
  gate.allow = false;
  const auto before = project.snapshot();
  REQUIRE_TRUE(!project.restore_snapshot(saved),
               "running transport accepted rate-changing restore");
  REQUIRE_EQ(project.snapshot(), before, "running transport accepted rate-changing restore");
  REQUIRE_EQ(project.undo_depth(), 0, "running transport accepted rate-changing restore");
  gate.allow = true;
  gate.return_null = true;
  REQUIRE_TRUE(!project.restore_snapshot(saved),
               "empty stopped lease accepted rate-changing restore");
  REQUIRE_EQ(project.snapshot(), before, "empty stopped lease accepted rate-changing restore");
  gate.return_null = false;
  auto restored = take(project.restore_snapshot(saved));
  REQUIRE_TRUE(restored.stopped_transport,
               "rate-changing restore did not retain its stopped lease");
  REQUIRE_TRUE(restored.changes.sample_rate,
               "rate-changing restore did not retain its stopped lease");
  REQUIRE_EQ(restored.snapshot->sample_rate, 44'100,
             "rate-changing restore did not retain its stopped lease");
  REQUIRE_EQ(gate.live, 1, "rate-changing restore did not retain its stopped lease");
  REQUIRE_TRUE(!project.undo(), "rate-changing restore undo bypassed the held transport gate");
  REQUIRE_EQ(project.snapshot(), restored.snapshot,
             "rate-changing restore undo bypassed the held transport gate");
  restored.stopped_transport.reset();
  auto undone = take(project.undo());
  REQUIRE_TRUE(undone.stopped_transport,
               "rate-changing restore undo lost the original rate or lease");
  REQUIRE_EQ(undone.snapshot->sample_rate, 48'000,
             "rate-changing restore undo lost the original rate or lease");
  REQUIRE_EQ(undone.snapshot->revision, restored.snapshot->revision + 1,
             "rate-changing restore undo lost the original rate or lease");
}

void rate_failure_boundaries() {
  FakeGate gate;
  auto project = model(gate, 44'100);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  REQUIRE_OK(project.apply(
      {AddClip{track, clip(1, ElapsedInterval{{std::numeric_limits<std::int64_t>::max() - 1},
                                              {std::numeric_limits<std::int64_t>::max()}})}}));
  const auto before = project.snapshot();
  REQUIRE_TRUE(!project.apply({SetSampleRate{96'000}}),
               "overflowing elapsed rate conversion changed project");
  REQUIRE_EQ(project.snapshot(), before, "overflowing elapsed rate conversion changed project");

  auto small = model(gate, 96'000);
  setup_track(small, track);
  REQUIRE_OK(small.apply({AddClip{track, clip(1, ElapsedInterval{{0}, {1}})}}));
  auto tiny_before = small.snapshot();
  REQUIRE_TRUE(!small.apply({SetSampleRate{44'100}}),
               "rate conversion that rounds clip to zero accepted");
  REQUIRE_EQ(small.snapshot(), tiny_before, "rate conversion that rounds clip to zero accepted");

  auto large_start = model(gate, 44'100);
  setup_track(large_start, track);
  const auto max = std::numeric_limits<std::int64_t>::max();
  REQUIRE_OK(large_start.apply({AddClip{track, clip(1, ElapsedInterval{{max - 10}, {max - 9}})}}));
  require(!large_start.apply({SetSampleRate{96'000}}), "start overflow accepted");
}

void source_trim_is_independent_of_arrangement() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  REQUIRE_OK(project.apply({AddClip{track, clip(1, ElapsedInterval{{0}, {100}})}}));
  const auto before = project.snapshot();
  REQUIRE_TRUE(!project.apply({SetClipSourceRange{id<ClipId>(1), SourceRange{{-1}, {10}}}}),
               "negative source frame accepted");
  REQUIRE_EQ(project.snapshot(), before, "negative source frame accepted");
  REQUIRE_TRUE(!project.apply({SetClipSourceRange{id<ClipId>(1), SourceRange{{10}, {10}}}}),
               "zero source range accepted");
  REQUIRE_EQ(project.snapshot(), before, "zero source range accepted");
  require(!project.apply({SetClipSourceRange{id<ClipId>(2), SourceRange{{1}, {10}}}}),
          "missing source trim target accepted");
  const auto trimmed =
      take(project.apply({SetClipSourceRange{id<ClipId>(1), SourceRange{{10}, {90}}}}));
  const auto& edited = trimmed.snapshot->tracks[0].clips[0];
  REQUIRE_EQ(edited.source_range.start.value, 10,
             "source trim changed arrangement or failed to advance generation");
  REQUIRE_EQ(edited.source_range.end.value, 90,
             "source trim changed arrangement or failed to advance generation");
  REQUIRE_EQ(edited.settings_generation, 2,
             "source trim changed arrangement or failed to advance generation");
  REQUIRE_EQ(std::get<ElapsedInterval>(edited.placement).end.value, 100,
             "source trim changed arrangement or failed to advance generation");
  auto reopened = take(ProjectModel::open(*trimmed.snapshot, gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0].clips[0].source_range, (SourceRange{{10}, {90}}),
             "decoded snapshot lost source trim");
  REQUIRE_OK(project.apply({SetSampleRate{44'100}}));
  REQUIRE_EQ(project.snapshot()->tracks[0].clips[0].source_range, (SourceRange{{10}, {90}}),
             "project rate rescaled source frames");
  REQUIRE_OK(project.undo());
  REQUIRE_OK(project.undo());
  REQUIRE_EQ(project.snapshot()->tracks[0].clips[0].source_range, (SourceRange{{0}, {100'000}}),
             "source trim undo failed");
  auto invalid = *before;
  invalid.tracks[0].clips[0].source_range = {{9}, {8}};
  require(!ProjectModel::open(invalid, gate), "invalid decoded source range accepted");
}

void timing_change_set_reports_event_edits() {
  FakeGate gate;
  auto project = model(gate);
  const auto first_tempo = take(project.apply({ReplaceTempoMap{tempo(130, 1)}}));
  require(first_tempo.changes.timing, "tempo edit did not mark timing dirty");
  const auto undo_tempo = take(project.undo());
  require(undo_tempo.changes.timing, "tempo undo with same map revision missed timing change");
  const auto first_meter = take(project.apply({ReplaceMeterMap{meter(3, 1)}}));
  require(first_meter.changes.timing, "meter edit did not mark timing dirty");
  const auto undo_meter = take(project.undo());
  require(undo_meter.changes.timing, "meter undo with same map revision missed timing change");
  auto two_events =
      take(TempoMap::create({TempoEvent{{0}, 120, TempoShape::step},
                             TempoEvent{{timeline::ticks_per_quarter}, 140, TempoShape::step}},
                            1));
  REQUIRE_OK(project.apply({ReplaceTempoMap{std::move(two_events)}}));
  require(take(project.undo()).changes.timing,
          "tempo event count change was omitted from undo change set");
}

void large_decoded_manifest_ids_are_validated() {
  constexpr std::uint64_t max_fixture_clips = 100'000;
  FakeGate gate;
  Snapshot decoded = *model(gate).snapshot();
  const auto track = id<TrackId>(10);
  decoded.tracks.push_back({track, {}});
  auto& clips = decoded.tracks.front().clips;
  clips.reserve(max_fixture_clips);
  for (std::uint64_t number = 1; number <= max_fixture_clips; ++number) {
    const auto start = static_cast<std::int64_t>(number * 2);
    clips.push_back(clip(number, ElapsedInterval{{start}, {start + 1}}));
  }
  auto opened = take(ProjectModel::open(decoded, gate));
  REQUIRE_EQ(opened.snapshot()->tracks.front().clips.size(), max_fixture_clips,
             "large valid decoded clip set rejected");

  decoded.tracks.push_back({id<TrackId>(11), {clip(1, ElapsedInterval{{0}, {1}})}});
  auto duplicate = ProjectModel::open(std::move(decoded), gate);
  REQUIRE_TRUE(!duplicate, "late duplicate clip ID was not rejected with its stable ID");
  REQUIRE_EQ(duplicate.error().code, ErrorCode::conflict,
             "late duplicate clip ID was not rejected with its stable ID");
  REQUIRE_NE(duplicate.error().message.find(id<ClipId>(1).to_string()), std::string::npos,
             "late duplicate clip ID was not rejected with its stable ID");
}

void source_records_and_unresolved_clips() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(20);
  const auto item = clip(30, ElapsedInterval{{0}, {24'000}}, 7);
  REQUIRE_OK(project.apply({AddTrack{track}, AddClip{track, item}}));
  REQUIRE_TRUE(project.snapshot()->sources.empty(), "unresolved source clip was rejected");
  REQUIRE_EQ(project.snapshot()->tracks[0].clips[0], item, "unresolved source clip was rejected");

  auto before = project.snapshot();
  auto failed = project.apply({AddSource{source(7, 99'999)}});
  REQUIRE_TRUE(!failed, "source shorter than existing clip was accepted");
  REQUIRE_EQ(failed.error().code, ErrorCode::invalid_argument,
             "source shorter than existing clip was accepted");
  REQUIRE_EQ(project.snapshot(), before, "source shorter than existing clip was accepted");
  auto added = take(project.apply({AddSource{source(7)}}));
  REQUIRE_EQ(added.changes.sources, std::vector<SourceId>{id<SourceId>(7)},
             "source addition did not invalidate referencing clip");
  REQUIRE_EQ(added.changes.clips, std::vector<ClipId>{item.id},
             "source addition did not invalidate referencing clip");
  REQUIRE_EQ(added.changes.tracks, std::vector<TrackId>{track},
             "source addition did not invalidate referencing clip");
  REQUIRE_TRUE(!project.apply({AddSource{source(7)}}), "duplicate source changed state");
  REQUIRE_EQ(project.snapshot(), added.snapshot, "duplicate source changed state");
  auto removed = take(project.apply({RemoveSource{id<SourceId>(7)}}));
  REQUIRE_TRUE(removed.snapshot->sources.empty(),
               "removing source deleted clip or failed to report dependency");
  REQUIRE_EQ(removed.snapshot->tracks[0].clips[0], item,
             "removing source deleted clip or failed to report dependency");
  REQUIRE_EQ(removed.changes.clips, std::vector<ClipId>{item.id},
             "removing source deleted clip or failed to report dependency");
  require(!project.apply({RemoveSource{id<SourceId>(7)}}), "missing source removal accepted");
  REQUIRE_EQ(take(project.undo()).snapshot->sources.size(), 1, "source removal undo failed");
  require(take(project.redo()).snapshot->sources.empty(), "source removal redo failed");
  auto reopened = take(ProjectModel::open(*project.snapshot(), gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0].clips[0], item, "reopen lost unresolved clip");
  REQUIRE_TRUE(reopened.snapshot()->sources.empty(), "reopen lost unresolved clip");
}

void external_source_relink_is_atomic_and_undoable() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(20);
  const auto item = clip(30, ElapsedInterval{{0}, {24'000}}, 7);
  auto linked = source(7);
  linked.external_link = ExternalSourceLink{"/tmp/original.wav"};
  REQUIRE_OK(project.apply({AddTrack{track}, AddSource{linked}, AddClip{track, item}}));
  const auto before = project.snapshot();
  const RelinkSource command{before->revision, linked.id, "/tmp/original.wav", "/tmp/moved.wav"};
  REQUIRE_TRUE(!project.apply({RelinkSource{before->revision - 1, linked.id, "/tmp/original.wav",
                                            "/tmp/moved.wav"}}),
               "stale revision changed external source link");
  REQUIRE_EQ(project.snapshot(), before, "stale revision changed external source link");
  REQUIRE_TRUE(!project.apply(
                   {RelinkSource{before->revision, linked.id, "/tmp/other.wav", "/tmp/moved.wav"}}),
               "stale expected path changed external source link");
  REQUIRE_EQ(project.snapshot(), before, "stale expected path changed external source link");
  REQUIRE_TRUE(!project.apply({RelinkSource{before->revision, linked.id, "/tmp/original.wav",
                                            "/tmp/../moved.wav"}}),
               "invalid replacement path changed project");
  REQUIRE_EQ(project.snapshot(), before, "invalid replacement path changed project");
  const auto applied = take(project.apply({command}));
  auto expected = linked;
  expected.external_link.value().last_known_path = "/tmp/moved.wav";
  REQUIRE_EQ(applied.snapshot->sources, std::vector<SourceRecord>{expected},
             "relink changed encoded identity, clip references or revision semantics");
  REQUIRE_EQ(applied.snapshot->tracks.front().clips.front(), item,
             "relink changed encoded identity, clip references or revision semantics");
  REQUIRE_EQ(applied.changes.sources, std::vector<SourceId>{linked.id},
             "relink changed encoded identity, clip references or revision semantics");
  REQUIRE_EQ(applied.changes.clips, std::vector<ClipId>{item.id},
             "relink changed encoded identity, clip references or revision semantics");
  REQUIRE_EQ(applied.changes.tracks, std::vector<TrackId>{track},
             "relink changed encoded identity, clip references or revision semantics");
  REQUIRE_EQ(project.undo_depth(), 2,
             "relink changed encoded identity, clip references or revision semantics");
  REQUIRE_TRUE(!project.apply({command}), "stale relink command was accepted twice");
  REQUIRE_EQ(project.snapshot(), applied.snapshot, "stale relink command was accepted twice");
  REQUIRE_EQ(take(project.undo()).snapshot->sources.front(), linked,
             "relink undo did not restore old path");
  REQUIRE_EQ(take(project.redo()).snapshot->sources.front(), expected,
             "relink redo did not restore new path");

  auto managed = source(8);
  REQUIRE_OK(project.apply({AddSource{managed}}));
  const auto unchanged = project.snapshot();
  REQUIRE_TRUE(
      !project.apply({RelinkSource{unchanged->revision, managed.id, "", "/tmp/moved.wav"}}),
      "managed source accepted external relink");
  REQUIRE_EQ(project.snapshot(), unchanged, "managed source accepted external relink");
  REQUIRE_TRUE(
      !project.apply({RelinkSource{unchanged->revision, id<SourceId>(99), "", "/tmp/moved.wav"}}),
      "missing source accepted external relink");
  REQUIRE_EQ(project.snapshot(), unchanged, "missing source accepted external relink");
}

void source_compounds_gestures_and_validation() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(8);
  const auto item = clip(9, ElapsedInterval{{0}, {100}}, 4);
  const auto media = source(4);
  auto first = take(project.apply({AddTrack{track}, AddSource{media}}, 99));
  REQUIRE_EQ(first.snapshot->sources.size(), 1, "source compound not published");
  REQUIRE_OK(project.apply({AddClip{track, item}}, 99));
  REQUIRE_EQ(project.undo_depth(), 1, "same-gesture source and clip did not coalesce");
  REQUIRE_TRUE(take(project.undo()).snapshot->sources.empty(),
               "source+clip gesture undo was incomplete");
  REQUIRE_TRUE(project.snapshot()->tracks.empty(), "source+clip gesture undo was incomplete");
  REQUIRE_EQ(take(project.redo()).snapshot->sources[0], media,
             "source+clip gesture redo was incomplete");
  REQUIRE_EQ(project.snapshot()->tracks[0].clips[0], item,
             "source+clip gesture redo was incomplete");
  auto original = project.snapshot();
  REQUIRE_TRUE(!project.apply({RemoveSource{media.id}, AddSource{media}, AddSource{media}}),
               "failed duplicate source compound was not atomic");
  REQUIRE_EQ(project.snapshot(), original, "failed duplicate source compound was not atomic");

  auto bad = *original;
  bad.sources[0].sha256[0] = 'A';
  require(!ProjectModel::open(bad, gate), "uppercase SHA-256 accepted");
  bad = *original;
  bad.sources[0].sha256.pop_back();
  require(!ProjectModel::open(bad, gate), "short SHA-256 accepted");
  bad = *original;
  bad.sources[0].encoding = std::bit_cast<SourceEncoding>(std::uint8_t{99});
  require(!ProjectModel::open(bad, gate), "unknown source encoding accepted");
  for (const auto encoding : {SourceEncoding::aiff, SourceEncoding::flac, SourceEncoding::mp3}) {
    auto encoded = *original;
    encoded.sources[0].encoding = encoding;
    encoded.sources[0].byte_length = 100;
    REQUIRE_OK(ProjectModel::open(encoded, gate), "known source encoding rejected");
    encoded.sources[0].byte_length = 0;
    require(!ProjectModel::open(encoded, gate), "empty encoded media accepted");
  }
  bad = *original;
  bad.sources[0].byte_length = 43;
  require(!ProjectModel::open(bad, gate), "short WAV media accepted");
  bad = *original;
  bad.sources[0].channels = 0;
  require(!ProjectModel::open(bad, gate), "zero source channels accepted");
  bad = *original;
  bad.sources[0].sample_rate = 0;
  require(!ProjectModel::open(bad, gate), "zero source rate accepted");
  bad = *original;
  bad.sources[0].byte_length = 0;
  require(!ProjectModel::open(bad, gate), "empty encoded media accepted");
  bad = *original;
  bad.sources[0].frames = 0;
  require(!ProjectModel::open(bad, gate), "zero source frames accepted");
  bad = *original;
  bad.sources.push_back(bad.sources[0]);
  require(!ProjectModel::open(bad, gate), "duplicate source in decoded snapshot accepted");
}

void source_count_has_bounded_validation() {
  FakeGate gate;
  auto project = model(gate);
  auto decoded = *project.snapshot();
  decoded.sources.reserve(max_source_records + 1);
  for (std::size_t i = 0; i < max_source_records; ++i) {
    decoded.sources.push_back(source(i + 1));
  }
  auto opened = take(ProjectModel::open(decoded, gate));
  REQUIRE_EQ(opened.snapshot()->sources.size(), max_source_records,
             "maximum source manifest failed to reopen");
  decoded.sources.push_back(source(max_source_records + 1));
  auto too_many = ProjectModel::open(std::move(decoded), gate);
  REQUIRE_TRUE(!too_many, "oversized source manifest accepted");
  REQUIRE_EQ(too_many.error().code, ErrorCode::resource_exhausted,
             "oversized source manifest accepted");
  auto fail = opened.apply({AddSource{source(max_source_records + 2)}});
  REQUIRE_TRUE(!fail, "source command exceeded fixed record cap");
  REQUIRE_EQ(fail.error().code, ErrorCode::resource_exhausted,
             "source command exceeded fixed record cap");
}

template <typename Native> Native* native_effect(Snapshot& snapshot) {
  for (auto& slot : snapshot.master_effects) {
    if (auto* native = std::get_if<Native>(&slot.processor))
      return native;
  }
  return nullptr;
}
template <typename Native> const Native* native_effect(const Snapshot& snapshot) {
  for (const auto& slot : snapshot.master_effects) {
    if (const auto* native = std::get_if<Native>(&slot.processor))
      return native;
  }
  return nullptr;
}
const Vst3PluginInstance& effect_plugin(const Track& track, std::size_t index) {
  return std::get<Vst3PluginInstance>(track.effects.at(index).processor);
}
Vst3PluginInstance& effect_plugin(Track& track, std::size_t index) {
  return std::get<Vst3PluginInstance>(track.effects.at(index).processor);
}
std::vector<Vst3PluginInstance> effect_plugins(const Track& track) {
  std::vector<Vst3PluginInstance> plugins;
  plugins.reserve(track.effects.size());
  for (const auto& effect : track.effects) {
    plugins.push_back(std::get<Vst3PluginInstance>(effect.processor));
  }
  return plugins;
}
EffectInsert effect_slot(std::uint64_t number, Vst3PluginInstance value) {
  return {id<InsertSlotId>(number), true, std::move(value)};
}

void native_insert_order_and_retired_slot_identity() {
  FakeGate gate;
  auto project = model(gate);
  const SignalPathId master{MasterSignalPath{}};
  REQUIRE_OK(project.apply({AddNativeEffect{master, 0, NativeEffectPatch{"press"}}}));
  auto first = project.snapshot()->master_effects.front().id;
  REQUIRE_OK(project.apply({AddNativeEffect{master, 0, NativeEffectPatch{"shape"}}}));
  const auto ordered = project.snapshot();
  REQUIRE_EQ(std::get<NativeEffectPatch>(ordered->master_effects[0].processor).engine, "shape",
             "native insertion lost its position or existing slot identity");
  REQUIRE_EQ(std::get<NativeEffectPatch>(ordered->master_effects[1].processor).engine, "press",
             "native insertion lost its position or existing slot identity");
  REQUIRE_EQ(ordered->master_effects[1].id, first,
             "native insertion lost its position or existing slot identity");
  const auto eq_id = ordered->master_effects[0].id;
  REQUIRE_OK(project.apply({RemoveEffectInsert{master, eq_id}}));
  REQUIRE_OK(project.apply({AddNativeEffect{master, 0, NativeEffectPatch{"shape"}}}));
  const auto readded = project.snapshot()->master_effects[0].id;
  REQUIRE_NE(readded, eq_id, "remove and readd reused retired native slot identity");
  REQUIRE_GT(project.snapshot()->next_insert_slot_serial, ordered->next_insert_slot_serial,
             "remove and readd reused retired native slot identity");
  REQUIRE_OK(project.undo());
  REQUIRE_OK(project.apply({AddNativeEffect{master, 1, NativeEffectPatch{"expanse"}}}));
  REQUIRE_NE(project.snapshot()->master_effects.back().id, readded,
             "undo branch reused a retired slot identity");
  REQUIRE_GT(project.snapshot()->next_insert_slot_serial, ordered->next_insert_slot_serial,
             "undo branch reused a retired slot identity");
}

Vst3PluginInstance plugin(std::uint64_t number) {
  return {id<PluginInstanceId>(number),
          std::string(32, 'a'),
          "Fixture Vendor",
          "Gain",
          "1.0",
          "/tmp/Gain.vst3",
          std::string(64, 'b'),
          {0, 1, 254, 255},
          {{7, 0.5}},
          true};
}

void vst3_placeholders_commands_and_bounds() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(10);
  setup_track(project, track);
  auto first = plugin(1);
  auto second = plugin(2);
  first.controller_state = std::vector<std::uint8_t>{};
  second.controller_state = std::vector<std::uint8_t>{4, 5};
  auto inserted = take(project.apply({InsertVst3Plugin{track, 0, first}}));
  REQUIRE_EQ(inserted.changes.plugins, std::vector{first.id},
             "VST3 insert did not preserve placeholder state");
  REQUIRE_EQ(effect_plugins(inserted.snapshot->tracks[0]), std::vector{first},
             "VST3 insert did not preserve placeholder state");
  REQUIRE_OK(project.apply({InsertVst3Plugin{track, 1, second}}));
  auto moved = take(project.apply({MoveVst3Plugin{track, first.id, 1}}));
  REQUIRE_EQ(effect_plugins(moved.snapshot->tracks[0]),
             (std::vector<Vst3PluginInstance>{second, first}), "VST3 chain reorder failed");
  auto edited = take(project.apply(
      {SetVst3ComponentState{first.id, {9, 8}},
       SetVst3ControllerState{first.id, std::vector<std::uint8_t>{1, 2}},
       SetVst3ParameterReferences{first.id, {{7, 0.75}}}, SetVst3Missing{first.id, false}}));
  REQUIRE_EQ(effect_plugin(edited.snapshot->tracks[0], 1).component_state,
             (std::vector<std::uint8_t>{9, 8}), "VST3 compound edit lost state");
  REQUIRE_EQ(effect_plugin(edited.snapshot->tracks[0], 1).controller_state,
             (std::optional<std::vector<std::uint8_t>>{{1, 2}}), "VST3 compound edit lost state");
  REQUIRE_TRUE(!effect_plugin(edited.snapshot->tracks[0], 1).missing,
               "VST3 compound edit lost state");
  const auto old_slot = edited.snapshot->tracks[0].effects[1].id;
  auto missing = take(project.apply({SetVst3Missing{first.id, true}}));
  const auto before_relink = missing.snapshot;
  const auto old_path = effect_plugin(before_relink->tracks[0], 1).bundle_path;
  const auto old_hash = effect_plugin(before_relink->tracks[0], 1).binary_sha256;
  REQUIRE_TRUE(!project.apply({RelinkVst3Plugin{first.id, old_path, std::string(64, 'f'),
                                                "/opt/relocated/Gain.vst3"}}),
               "mismatched VST3 relink identity changed the model");
  REQUIRE_TRUE(
      !project.apply({RelinkVst3Plugin{first.id, old_path, "", "/opt/relocated/Gain.vst3"}}),
      "mismatched VST3 relink identity changed the model");
  REQUIRE_TRUE(!project.apply({RelinkVst3Plugin{first.id, old_path, old_hash, ""}}),
               "mismatched VST3 relink identity changed the model");
  REQUIRE_TRUE(
      !project.apply({RelinkVst3Plugin{first.id, old_path, old_hash, "relative/Gain.vst3"}}),
      "mismatched VST3 relink identity changed the model");
  REQUIRE_TRUE(!project.apply(
                   {RelinkVst3Plugin{first.id, old_path, old_hash, "/opt/../relocated/Gain.vst3"}}),
               "mismatched VST3 relink identity changed the model");
  REQUIRE_EQ(project.snapshot(), before_relink,
             "mismatched VST3 relink identity changed the model");
  auto relinked = take(
      project.apply({RelinkVst3Plugin{first.id, old_path, old_hash, "/opt/relocated/Gain.vst3"}}));
  const auto& recovered = effect_plugin(relinked.snapshot->tracks[0], 1);
  REQUIRE_EQ(relinked.snapshot->tracks[0].effects[1].id, old_slot,
             "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_EQ(recovered.id, first.id, "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_EQ(recovered.bundle_path, "/opt/relocated/Gain.vst3",
             "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_TRUE(!recovered.missing, "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_EQ(recovered.component_state, (std::vector<std::uint8_t>{9, 8}),
             "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_EQ(recovered.controller_state, (std::optional<std::vector<std::uint8_t>>{{1, 2}}),
             "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_EQ(recovered.parameters, (std::vector<Vst3ParameterReference>{{7, 0.75}}),
             "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].effects[1].processor,
             before_relink->tracks[0].effects[1].processor,
             "VST3 relink lost slot/state or was not one undo step");
  REQUIRE_OK(project.redo());
  REQUIRE_OK(project.undo());
  REQUIRE_OK(project.undo());
  const auto before_rejection = project.snapshot();
  const auto history = project.undo_depth();
  REQUIRE_TRUE(!project.apply({SetVst3ParameterReferences{first.id, {{7, 0.2}, {7, 0.8}}}}),
               "duplicate VST3 parameter mutated project");
  REQUIRE_EQ(project.snapshot(), before_rejection, "duplicate VST3 parameter mutated project");
  REQUIRE_EQ(project.undo_depth(), history, "duplicate VST3 parameter mutated project");
  require(!project.apply({SetVst3ComponentState{
              first.id, std::vector<std::uint8_t>(max_vst3_state_bytes + 1)}}),
          "oversized VST3 state accepted");
  REQUIRE_TRUE(!project.apply({SetVst3ControllerState{
                   first.id, std::vector<std::uint8_t>(max_vst3_state_bytes + 1)}}),
               "oversized controller state mutated project or history");
  REQUIRE_EQ(project.snapshot(), before_rejection,
             "oversized controller state mutated project or history");
  REQUIRE_EQ(project.undo_depth(), history,
             "oversized controller state mutated project or history");
  auto malformed = plugin(3);
  malformed.bundle_path = "../Gain.vst3";
  REQUIRE_TRUE(!project.apply({InsertVst3Plugin{track, 2, malformed}}),
               "malformed VST3 identity mutated project");
  REQUIRE_EQ(project.snapshot(), before_rejection, "malformed VST3 identity mutated project");
  malformed = plugin(3);
  malformed.parameters = {{7, std::numeric_limits<double>::quiet_NaN()}};
  require(!project.apply({InsertVst3Plugin{track, 2, malformed}}),
          "nonfinite VST3 parameter accepted");
  malformed = plugin(3);
  malformed.binary_sha256[0] = 'A';
  require(!project.apply({InsertVst3Plugin{track, 2, malformed}}),
          "noncanonical VST3 binary digest accepted");
  malformed = plugin(3);
  malformed.parameters.resize(max_vst3_parameter_references + 1, {0, 0.5});
  require(!project.apply({InsertVst3Plugin{track, 2, malformed}}),
          "oversized VST3 parameter set accepted");
  REQUIRE_TRUE(!project.apply({InsertVst3Plugin{id<TrackId>(99), 0, plugin(3)}}),
               "VST3 insert accepted missing track, bad index or duplicate instance");
  REQUIRE_TRUE(!project.apply({InsertVst3Plugin{track, 3, plugin(3)}}),
               "VST3 insert accepted missing track, bad index or duplicate instance");
  REQUIRE_TRUE(!project.apply({InsertVst3Plugin{track, 2, first}}),
               "VST3 insert accepted missing track, bad index or duplicate instance");
  REQUIRE_TRUE(!project.apply({RemoveVst3Plugin{id<PluginInstanceId>(99)}}),
               "VST3 edit accepted missing instance or invalid move");
  REQUIRE_TRUE(!project.apply({MoveVst3Plugin{track, first.id, 2}}),
               "VST3 edit accepted missing instance or invalid move");
  REQUIRE_TRUE(!project.apply({MoveVst3Plugin{id<TrackId>(99), first.id, 0}}),
               "VST3 edit accepted missing instance or invalid move");
  const auto check_vendor = [&](std::string vendor, bool expected) {
    auto candidate = *project.snapshot();
    effect_plugin(candidate.tracks[0], 0).vendor = std::move(vendor);
    REQUIRE_EQ(ProjectModel::open(std::move(candidate), gate).has_value(), expected,
               "VST3 UTF-8 metadata validation mismatch");
  };
  check_vendor("\xc3\xa9", true);
  check_vendor("\xe2\x82\xac", true);
  check_vendor("\xf0\x9f\x8e\xb5", true);
  check_vendor("", false);
  check_vendor(std::string(129, 'a'), false);
  check_vendor("bad\nname", false);
  check_vendor("\xc0\x80", false);
  check_vendor("\xe2", false);
  check_vendor("\xe2\x28\xa1", false);
  check_vendor("\xe2\x82\x20", false);
  malformed = plugin(3);
  malformed.bundle_path = "/tmp/../Gain.vst3";
  require(!project.apply({InsertVst3Plugin{track, 2, malformed}}), "VST3 path traversal accepted");
  malformed.bundle_path = "/tmp//Gain.vst3";
  require(!project.apply({InsertVst3Plugin{track, 2, malformed}}),
          "VST3 empty path segment accepted");
  auto undone = take(project.undo());
  REQUIRE_EQ(effect_plugin(undone.snapshot->tracks[0], 1), first,
             "VST3 compound undo lost placeholder bytes");
  REQUIRE_EQ(undone.changes.plugins.size(), 2, "VST3 compound undo lost placeholder bytes");
  REQUIRE_EQ(effect_plugin(take(project.redo()).snapshot->tracks[0], 1).component_state,
             (std::vector<std::uint8_t>{9, 8}), "VST3 redo lost component state");
  REQUIRE_OK(project.apply({RemoveVst3Plugin{second.id}}));
  REQUIRE_EQ(effect_plugin(take(project.undo()).snapshot->tracks[0], 0), second,
             "VST3 removal undo lost chain position");
  auto removed_track = take(project.apply({RemoveTrack{track}}));
  REQUIRE_EQ(removed_track.changes.plugins.size(), 2,
             "VST3 track removal omitted changed plugin identities");
  REQUIRE_TRUE(removed_track.snapshot->tracks.empty(),
               "VST3 track removal omitted changed plugin identities");
  REQUIRE_OK(project.undo());
  auto invalid_open = *project.snapshot();
  invalid_open.tracks[0].effects.push_back(
      effect_slot(999, effect_plugin(invalid_open.tracks[0], 0)));
  require(!ProjectModel::open(std::move(invalid_open), gate),
          "duplicate VST3 instance ID reopened");
  invalid_open = *project.snapshot();
  effect_plugin(invalid_open.tracks[0], 0).component_state.resize(max_vst3_state_bytes);
  effect_plugin(invalid_open.tracks[0], 1).component_state.resize(max_vst3_state_bytes);
  invalid_open.tracks[0].effects.push_back(effect_slot(997, plugin(3)));
  effect_plugin(invalid_open.tracks[0], 2).component_state.resize(max_vst3_state_bytes);
  require(!ProjectModel::open(std::move(invalid_open), gate),
          "aggregate VST3 state budget exceeded on reopen");
  invalid_open = *project.snapshot();
  invalid_open.tracks[0].effects.clear();
  for (std::size_t i = 0; i <= max_vst3_plugins_per_track; ++i) {
    invalid_open.tracks[0].effects.push_back(effect_slot(1000 + i, plugin(i + 1)));
  }
  require(!ProjectModel::open(std::move(invalid_open), gate),
          "VST3 per-track instance count exceeded on reopen");
  auto budget_model = model(gate);
  setup_track(budget_model, track);
  REQUIRE_OK(budget_model.apply({InsertVst3Plugin{track, 0, plugin(1)},
                                 InsertVst3Plugin{track, 1, plugin(2)},
                                 InsertVst3Plugin{track, 2, plugin(3)}}));
  REQUIRE_OK(budget_model.apply(
      {SetVst3ComponentState{id<PluginInstanceId>(1),
                             std::vector<std::uint8_t>(max_vst3_state_bytes)},
       SetVst3ComponentState{id<PluginInstanceId>(2),
                             std::vector<std::uint8_t>(max_vst3_state_bytes - 4)}}));
  const auto at_budget = budget_model.snapshot();
  REQUIRE_TRUE(!budget_model.apply(
                   {SetVst3ComponentState{id<PluginInstanceId>(3), std::vector<std::uint8_t>(5)}}),
               "aggregate VST3 state replacement crossed budget or published rejection");
  REQUIRE_EQ(budget_model.snapshot(), at_budget,
             "aggregate VST3 state replacement crossed budget or published rejection");
  REQUIRE_TRUE(!budget_model.apply(
                   {SetVst3ControllerState{id<PluginInstanceId>(3), std::vector<std::uint8_t>(5)}}),
               "controller chunk escaped the shared VST3 state budget");
  REQUIRE_EQ(budget_model.snapshot(), at_budget,
             "controller chunk escaped the shared VST3 state budget");
  auto extra = plugin(4);
  extra.component_state = {1};
  REQUIRE_TRUE(!budget_model.apply({InsertVst3Plugin{track, 3, extra}}),
               "aggregate VST3 state insertion crossed budget or published rejection");
  REQUIRE_EQ(budget_model.snapshot(), at_budget,
             "aggregate VST3 state insertion crossed budget or published rejection");
}

void routing_intent_commands_and_rollback() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio = id<TrackId>(1);
  const auto group = id<TrackId>(2);
  const auto returns = id<TrackId>(3);
  REQUIRE_OK(project.apply({AddTrack{audio}, AddTrack{group, TrackKind::group_bus},
                            AddTrack{returns, TrackKind::return_bus}}));
  const SendRoute send{id<SendId>(10), returns, 0.5};
  auto routed = take(project.apply({SetTrackMainOutput{audio, group}, AddSendRoute{audio, send}}));
  REQUIRE_TRUE(routed.changes.routing, "routing command did not publish one revision");
  REQUIRE_EQ(routed.snapshot->tracks[0].main_output, group,
             "routing command did not publish one revision");
  REQUIRE_EQ(routed.snapshot->tracks[0].sends, std::vector<SendRoute>{send},
             "routing command did not publish one revision");
  const auto prior = project.snapshot();
  const auto undo_depth = project.undo_depth();
  REQUIRE_TRUE(!project.apply({SetTrackMainOutput{audio, returns}}),
               "invalid main destination published a candidate");
  REQUIRE_EQ(project.snapshot(), prior, "invalid main destination published a candidate");
  REQUIRE_EQ(project.undo_depth(), undo_depth, "invalid main destination published a candidate");
  // Sending the group to the return is valid; routing the return back to the
  // group then creates a cycle.
  REQUIRE_OK(project.apply({AddSendRoute{group, {id<SendId>(12), returns, 1.0}}}));
  const auto before_cycle = project.snapshot();
  const auto depth_before_cycle = project.undo_depth();
  REQUIRE_TRUE(!project.apply({SetTrackMainOutput{returns, group}}),
               "cycle rejection published a candidate or consumed undo");
  REQUIRE_EQ(project.snapshot(), before_cycle,
             "cycle rejection published a candidate or consumed undo");
  REQUIRE_EQ(project.undo_depth(), depth_before_cycle,
             "cycle rejection published a candidate or consumed undo");
  REQUIRE_TRUE(!project.apply({AddSendRoute{audio, {id<SendId>(10), returns, 0.25}}}),
               "invalid send identity, gain or endpoint changed project");
  REQUIRE_TRUE(!project.apply({SetSendGain{id<SendId>(10), 5.0}}),
               "invalid send identity, gain or endpoint changed project");
  REQUIRE_TRUE(!project.apply({AddSendRoute{audio, {id<SendId>(13), id<TrackId>(99), 1.0}}}),
               "invalid send identity, gain or endpoint changed project");
  REQUIRE_EQ(project.snapshot(), before_cycle,
             "invalid send identity, gain or endpoint changed project");
  auto changed = take(project.apply({SetSendGain{id<SendId>(10), 0.25}}));
  REQUIRE_TRUE(changed.changes.routing, "send gain edit missing");
  REQUIRE_EQ(changed.snapshot->tracks[0].sends[0].gain, 0.25, "send gain edit missing");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].sends[0].gain, 0.5,
             "routing gain undo/redo lost value");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].sends[0].gain, 0.25,
             "routing gain undo/redo lost value");
  // Removing a bus takes the routes into it: the audio track now plays to
  // the master, and one undo brings the bus and its route back.
  auto removed = take(project.apply({RemoveTrack{group}}));
  REQUIRE_TRUE(removed.changes.routing, "removing a bus did not send its inputs to the master");
  REQUIRE_EQ(removed.snapshot->tracks.size(), 2,
             "removing a bus did not send its inputs to the master");
  REQUIRE_TRUE(!removed.snapshot->tracks[0].main_output,
               "removing a bus did not send its inputs to the master");
  const auto restored = take(project.undo());
  REQUIRE_EQ(restored.snapshot->tracks.size(), 3, "bus removal did not undo atomically");
  REQUIRE_EQ(restored.snapshot->tracks[0].main_output, group,
             "bus removal did not undo atomically");
  auto unsent = take(project.apply({RemoveTrack{returns}}));
  REQUIRE_TRUE(unsent.snapshot->tracks[0].sends.empty(),
               "removing a return bus did not remove the sends to it");
  REQUIRE_TRUE(unsent.snapshot->tracks[1].sends.empty(),
               "removing a return bus did not remove the sends to it");
  REQUIRE_OK(project.undo());
  auto malformed = *prior;
  malformed.tracks[1].clips.push_back(clip(100, ElapsedInterval{{0}, {100}}));
  require(!ProjectModel::open(std::move(malformed), gate), "bus with source clip reopened");
  REQUIRE_GT(undo_depth, 0, "routing history setup failed");
}

void sidechain_send_targets_one_built_in_effect() {
  FakeGate gate;
  auto state = model(gate);
  const auto source = id<TrackId>(71);
  const auto destination = id<TrackId>(72);
  REQUIRE_OK(state.apply({AddTrack{source}, AddTrack{destination}}));
  const auto inserted =
      take(state.apply({AddNativeEffect{destination, 0, NativeEffectPatch{"press"}}}));
  const auto compressor = inserted.snapshot->tracks[1].effects.front().id;
  const SendRoute sidechain{id<SendId>(73), destination, 0.5, SendPurpose::sidechain, compressor};
  const auto routed = take(state.apply({AddSendRoute{source, sidechain}}));
  REQUIRE_TRUE(routed.changes.routing, "sidechain send did not retain its effect slot and gain");
  REQUIRE_EQ(routed.snapshot->tracks[0].sends.front(), sidechain,
             "sidechain send did not retain its effect slot and gain");
  const auto before = state.snapshot();
  REQUIRE_TRUE(!state.apply({AddSendRoute{source,
                                          {id<SendId>(74), destination, 1.0, SendPurpose::sidechain,
                                           id<InsertSlotId>(999)}}}),
               "invalid sidechain target or audible-send target changed the project");
  REQUIRE_TRUE(!state.apply({AddSendRoute{
                   source, {id<SendId>(75), destination, 1.0, SendPurpose::audible, compressor}}}),
               "invalid sidechain target or audible-send target changed the project");
  REQUIRE_EQ(state.snapshot(), before,
             "invalid sidechain target or audible-send target changed the project");
  REQUIRE_TRUE(take(state.undo()).snapshot->tracks[0].sends.empty(),
               "sidechain route did not survive undo and redo");
  REQUIRE_EQ(take(state.redo()).snapshot->tracks[0].sends.front(), sidechain,
             "sidechain route did not survive undo and redo");
  // A switched-off target keeps its key, inactive until it is on again.
  const auto bypassed = take(state.apply({SetEffectInsertEnabled{destination, compressor, false}}));
  REQUIRE_EQ(bypassed.snapshot->tracks[0].sends.front(), sidechain,
             "switching off a sidechain target lost its key");
  REQUIRE_OK(state.undo());
  // The key's gain is automated; removing what it keys takes both.
  const AutomationDomain domain{AutomationRepresentation::physical,
                                "mixer.send_gain",
                                "linear",
                                0.0,
                                4.0,
                                1.0,
                                false,
                                0,
                                AutomationScale::decibel};
  const AutomationLane gain_lane{
      id<AutomationLaneId>(76),    SendGainTarget{sidechain.id},           domain,
      AutomationTimebase::elapsed, {{id<AutomationPointId>(77), 0, 0.25}}, 1};
  REQUIRE_OK(state.apply({AddAutomationLane{source, gain_lane}}));
  const auto keyed = take(state.apply({RemoveEffectInsert{destination, compressor}}));
  REQUIRE_TRUE(keyed.snapshot->tracks[0].sends.empty(),
               "removing a keyed effect left its key's send or gain automation behind");
  REQUIRE_TRUE(keyed.snapshot->tracks[0].automation.empty(),
               "removing a keyed effect left its key's send or gain automation behind");
  REQUIRE_TRUE(keyed.changes.routing,
               "removing a keyed effect left its key's send or gain automation behind");
  REQUIRE_EQ(take(state.undo()).snapshot->tracks[0].sends.front(), sidechain,
             "undo did not restore the key with its effect");
  // Removing a keyed VST3 effect takes its key with it too.
  const auto keyer = id<TrackId>(80);
  const auto plugin_host = id<TrackId>(81);
  const auto keyed_plugin = plugin(78);
  REQUIRE_OK(state.apply({AddTrack{keyer}, AddTrack{plugin_host}}));
  const auto with_plugin = take(state.apply({InsertVst3Plugin{plugin_host, 0, keyed_plugin}}));
  const auto plugin_slot = with_plugin.snapshot->tracks[3].effects.back().id;
  const SendRoute plugin_key{id<SendId>(79), plugin_host, 1.0, SendPurpose::sidechain, plugin_slot};
  REQUIRE_OK(state.apply({AddSendRoute{keyer, plugin_key}}));
  const auto unplugged = take(state.apply({RemoveVst3Plugin{keyed_plugin.id}}));
  REQUIRE_TRUE(unplugged.snapshot->tracks[2].sends.empty(),
               "removing a keyed VST3 effect left its key's send behind");
  REQUIRE_EQ(unplugged.snapshot->tracks[0].sends, std::vector{sidechain},
             "removing a keyed VST3 effect took another effect's key");
  for (int step = 0; step < 4; ++step) {
    REQUIRE_OK(state.undo());
  }
  // Deleting the keyed track (a synth ducked by a kick) takes the key too.
  const auto deleted = take(state.apply({RemoveTrack{destination}}));
  REQUIRE_EQ(deleted.snapshot->tracks.size(), 1,
             "deleting a keyed track left the key's send behind");
  REQUIRE_TRUE(deleted.snapshot->tracks[0].sends.empty(),
               "deleting a keyed track left the key's send behind");
  REQUIRE_TRUE(deleted.snapshot->tracks[0].automation.empty(),
               "deleting a keyed track left the key's send behind");
  const auto back = take(state.undo());
  REQUIRE_EQ(back.snapshot->tracks.size(), 2, "undo did not restore the keyed track and its key");
  REQUIRE_EQ(back.snapshot->tracks[0].sends.front(), sidechain,
             "undo did not restore the keyed track and its key");
}

void sidechain_send_can_name_a_saved_vst3_effect() {
  FakeGate gate;
  auto state = model(gate);
  const auto source = id<TrackId>(76);
  const auto destination = id<TrackId>(77);
  REQUIRE_OK(state.apply({AddTrack{source}, AddTrack{destination}}));
  const auto effect = plugin(78);
  const auto inserted = take(state.apply({InsertVst3Plugin{destination, 0, effect}}));
  const auto slot = inserted.snapshot->tracks[1].effects.front().id;
  const SendRoute sidechain{id<SendId>(79), destination, 0.5, SendPurpose::sidechain, slot};
  REQUIRE_EQ(take(state.apply({AddSendRoute{source, sidechain}})).snapshot->tracks[0].sends.front(),
             sidechain, "saved VST3 detector send did not retain its exact effect slot");
  REQUIRE_EQ(take(state.apply({SetEffectInsertEnabled{destination, slot, false}}))
                 .snapshot->tracks[0]
                 .sends.front(),
             sidechain, "switching off a VST3 detector target lost its key");
  REQUIRE_OK(state.undo());
  REQUIRE_TRUE(take(state.undo()).snapshot->tracks[0].sends.empty(),
               "VST3 detector send lost its saved identity across undo/redo");
  REQUIRE_EQ(take(state.redo()).snapshot->tracks[0].sends.front(), sidechain,
             "VST3 detector send lost its saved identity across undo/redo");
}

AutomationLane vst_lane(std::uint64_t lane_number, PluginInstanceId plugin_id,
                        std::uint64_t point_number) {
  return {id<AutomationLaneId>(lane_number),
          Vst3ParameterTarget{plugin_id, 7},
          {AutomationRepresentation::vst3_normalized, "vst3:param:7", "", 0.0, 1.0, 0.5, false, 0},
          AutomationTimebase::musical,
          {{id<AutomationPointId>(point_number), 0, 0.2, AutomationShape::linear},
           {id<AutomationPointId>(point_number + 1), timeline::ticks_per_quarter, 0.8}},
          1};
}

void sample_rate_rescales_elapsed_automation_atomically() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio_track = id<TrackId>(100);
  const auto instrument_track = id<TrackId>(101);
  const auto audio = clip(102, ElapsedInterval{{48'000}, {96'000}});
  const MidiClip midi{id<ClipId>(103),
                      ElapsedInterval{{48'000}, {96'000}},
                      {timeline::ticks_per_quarter},
                      false,
                      {}};
  const auto effect = plugin(104);
  REQUIRE_OK(project.apply(
      {AddTrack{audio_track}, AddTrack{instrument_track, TrackKind::instrument},
       AddClip{audio_track, audio}, AddMidiClip{instrument_track, midi},
       InsertVst3Plugin{audio_track, 0, effect},
       SetNativeInstrumentPatch{instrument_track, NativeInstrumentPatch{"prism", 1, {}}}}));
  auto track_lane = vst_lane(106, effect.id, 107);
  track_lane.timebase = AutomationTimebase::elapsed;
  track_lane.points[0].coordinate = 80;
  track_lane.points[1].coordinate = 240;
  auto clip_lane = track_lane;
  clip_lane.id = id<AutomationLaneId>(109);
  clip_lane.points[0].id = id<AutomationPointId>(110);
  clip_lane.points[1].id = id<AutomationPointId>(111);
  AutomationLane midi_lane{
      id<AutomationLaneId>(112),
      TrackControlTarget{instrument_track, TrackControlTarget::Key::channel_level},
      {AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0, 1.0,
       false, 0, AutomationScale::decibel},
      AutomationTimebase::elapsed,
      {{id<AutomationPointId>(113), 80, 0.5}},
      1};
  REQUIRE_OK(project.apply({AddAutomationLane{audio_track, track_lane},
                            AddAutomationLane{audio.id, clip_lane},
                            AddAutomationLane{midi.id, midi_lane}}));
  const auto before = project.snapshot();
  auto changed = take(project.apply({SetSampleRate{44'100}}));
  REQUIRE_TRUE(changed.changes.sample_rate,
               "rate change omitted elapsed automation from change set");
  REQUIRE_TRUE(changed.changes.automation,
               "rate change omitted elapsed automation from change set");
  REQUIRE_EQ(changed.changes.automation_lanes.size(), 3,
             "rate change omitted elapsed automation from change set");
  REQUIRE_EQ(changed.changes.clips.size(), 2,
             "rate change omitted elapsed automation from change set");
  REQUIRE_EQ(changed.changes.tracks.size(), 2,
             "rate change omitted elapsed automation from change set");
  const auto& audio_state = changed.snapshot->tracks[0];
  const auto& midi_state = changed.snapshot->tracks[1];
  REQUIRE_EQ(audio_state.automation[0].points[0].coordinate, 74,
             "elapsed track or clip envelope did not retain seconds after rate change");
  REQUIRE_EQ(audio_state.automation[0].points[1].coordinate, 220,
             "elapsed track or clip envelope did not retain seconds after rate change");
  REQUIRE_EQ(audio_state.automation[0].settings_generation, 2,
             "elapsed track or clip envelope did not retain seconds after rate change");
  REQUIRE_EQ(audio_state.clips[0].automation[0].points[0].coordinate, 74,
             "elapsed track or clip envelope did not retain seconds after rate change");
  REQUIRE_EQ(audio_state.clips[0].automation[0].settings_generation, 2,
             "elapsed track or clip envelope did not retain seconds after rate change");
  REQUIRE_EQ(midi_state.midi_clips[0].automation[0].points[0].coordinate, 74,
             "elapsed track or clip envelope did not retain seconds after rate change");
  REQUIRE_EQ(midi_state.midi_clips[0].automation[0].settings_generation, 2,
             "elapsed track or clip envelope did not retain seconds after rate change");
  changed.stopped_transport.reset();
  auto undone = take(project.undo());
  REQUIRE_EQ(undone.snapshot->tracks, before->tracks,
             "rate undo did not restore exact automation coordinates and generations");
  REQUIRE_EQ(undone.snapshot->sample_rate, 48'000,
             "rate undo did not restore exact automation coordinates and generations");
}

void automation_commands_validate_and_preserve_placeholders() {
  FakeGate gate;
  auto project = model(gate);
  require(!requires_automation_rendering(*project.snapshot()),
          "empty project falsely requires automation renderer");
  const auto track = id<TrackId>(10);
  const auto audio = clip(20, MusicalInterval{{0}, {timeline::ticks_per_quarter * 2}});
  const auto missing = plugin(30);
  REQUIRE_OK(
      project.apply({AddTrack{track}, AddClip{track, audio}, InsertVst3Plugin{track, 0, missing}}));
  auto track_lane = vst_lane(40, missing.id, 50);
  auto clip_lane = vst_lane(41, missing.id, 60);
  clip_lane.points[1].coordinate = timeline::ticks_per_quarter * 4;
  auto added = take(project.apply(
      {AddAutomationLane{track, track_lane}, AddAutomationLane{audio.id, clip_lane}}));
  REQUIRE_TRUE(added.changes.automation,
               "track/clip automation compound lost missing-plugin references");
  REQUIRE_EQ(added.changes.automation_lanes.size(), 2,
             "track/clip automation compound lost missing-plugin references");
  REQUIRE_EQ(added.snapshot->tracks[0].automation, std::vector{track_lane},
             "track/clip automation compound lost missing-plugin references");
  REQUIRE_EQ(added.snapshot->tracks[0].clips[0].automation, std::vector{clip_lane},
             "track/clip automation compound lost missing-plugin references");
  auto clip_only = *added.snapshot;
  clip_only.tracks[0].automation.clear();
  require(requires_automation_rendering(clip_only),
          "clip envelope was invisible to automation renderer guard");
  auto before = project.snapshot();
  auto history = project.undo_depth();
  auto duplicate = vst_lane(42, missing.id, 70);
  REQUIRE_TRUE(!project.apply({AddAutomationLane{track, duplicate}}),
               "duplicate target or missing owner changed state");
  REQUIRE_TRUE(!project.apply({AddAutomationLane{id<ClipId>(99), duplicate}}),
               "duplicate target or missing owner changed state");
  REQUIRE_EQ(project.snapshot(), before, "duplicate target or missing owner changed state");
  REQUIRE_EQ(project.undo_depth(), history, "duplicate target or missing owner changed state");
  duplicate.id = track_lane.id;
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}),
          "duplicate project lane ID accepted");
  duplicate.id = id<AutomationLaneId>(42);
  duplicate.points[0].id = track_lane.points[0].id;
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}),
          "duplicate project point ID accepted");
  duplicate = vst_lane(42, id<PluginInstanceId>(99), 70);
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}),
          "orphan automation plugin accepted");
  duplicate = vst_lane(42, missing.id, 70);
  duplicate.points[0].value = std::numeric_limits<double>::quiet_NaN();
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}),
          "nonfinite automation value accepted");
  duplicate = vst_lane(42, missing.id, 70);
  duplicate.points[0].handles.x1 = 0.9;
  duplicate.points[0].handles.x2 = 0.2;
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}),
          "nonmonotone automation handles accepted");
  duplicate = vst_lane(42, missing.id, 70);
  duplicate.points.resize(max_automation_points_per_lane + 1, duplicate.points[0]);
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}),
          "oversized automation lane accepted");
  duplicate = vst_lane(42, missing.id, 70);
  duplicate.points.clear();
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}), "empty clip envelope accepted");
  duplicate = vst_lane(42, missing.id, 70);
  std::get<Vst3ParameterTarget>(duplicate.target).parameter_id = 8;
  duplicate.domain.semantic_key = "vst3:param:8";
  duplicate.domain.discrete = true;
  duplicate.domain.step_count = 2;
  duplicate.points[0].to_next = AutomationShape::hold;
  duplicate.points[1].to_next = AutomationShape::hold;
  require(!project.apply({AddAutomationLane{audio.id, duplicate}}),
          "off-step discrete automation value accepted");
  duplicate.points[0].value = 0.0;
  duplicate.points[1].value = 1.0;
  REQUIRE_EQ(take(project.apply({AddAutomationLane{audio.id, duplicate}}))
                 .snapshot->tracks[0]
                 .clips[0]
                 .automation.size(),
             2, "valid normalized discrete automation rejected");
  REQUIRE_OK(project.apply({RemoveAutomationLane{duplicate.id}}));
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].clips[0].automation.size(), 2,
             "direct automation lane removal did not undo");
  REQUIRE_OK(project.redo());
  const auto history_before_gesture = project.undo_depth();

  auto edited = track_lane;
  edited.settings_generation = 2;
  edited.points[0].value = 0.4;
  REQUIRE_OK(project.apply({ReplaceAutomationLane{track_lane.id, edited}}, 700));
  edited.settings_generation = 3;
  edited.points[0].value = 0.6;
  REQUIRE_OK(project.apply({ReplaceAutomationLane{track_lane.id, edited}}, 700));
  REQUIRE_EQ(project.undo_depth(), history_before_gesture + 1,
             "gesture undo/redo did not restore exact automation lane");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].automation[0], track_lane,
             "gesture undo/redo did not restore exact automation lane");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].automation[0], edited,
             "gesture undo/redo did not restore exact automation lane");
  require(!project.apply({ReplaceAutomationLane{track_lane.id, track_lane}}),
          "stale automation generation accepted");
  auto moved = take(project.apply(
      {SetClipPlacement{audio.id, MusicalInterval{{timeline::ticks_per_quarter * 3},
                                                  {timeline::ticks_per_quarter * 5}}}}));
  REQUIRE_EQ(moved.snapshot->tracks[0].clips[0].automation, std::vector{clip_lane},
             "clip move changed local envelope coordinates");
  auto removed = take(project.apply({RemoveVst3Plugin{missing.id}}));
  REQUIRE_EQ(removed.changes.automation_lanes.size(), 2,
             "explicit plugin removal retained orphan automation");
  REQUIRE_TRUE(removed.snapshot->tracks[0].automation.empty(),
               "explicit plugin removal retained orphan automation");
  REQUIRE_TRUE(removed.snapshot->tracks[0].clips[0].automation.empty(),
               "explicit plugin removal retained orphan automation");
  REQUIRE_TRUE(!requires_automation_rendering(*removed.snapshot),
               "explicit plugin removal retained orphan automation");
  auto restored = take(project.undo());
  REQUIRE_EQ(restored.snapshot->tracks[0].clips[0].automation, std::vector{clip_lane},
             "plugin removal undo lost clip automation");
  REQUIRE_TRUE(requires_automation_rendering(*restored.snapshot),
               "plugin removal undo lost clip automation");

  auto invalid = *project.snapshot();
  invalid.tracks[0].automation[0].domain.maximum = 2.0;
  require(!ProjectModel::open(std::move(invalid), gate),
          "VST3 automation domain outside normalized range reopened");
  invalid = *project.snapshot();
  invalid.tracks[0].automation[0].points[1].coordinate = 0;
  require(!ProjectModel::open(std::move(invalid), gate),
          "duplicate automation coordinate reopened");
  invalid = *project.snapshot();
  invalid.tracks[0].automation[0].points[0].to_next = static_cast<AutomationShape>(255);
  require(!ProjectModel::open(std::move(invalid), gate),
          "unknown automation interpolation reopened");
  invalid = *project.snapshot();
  invalid.tracks[0].automation[0].timebase = static_cast<AutomationTimebase>(255);
  require(!ProjectModel::open(std::move(invalid), gate), "unknown automation timebase reopened");
  invalid = *project.snapshot();
  invalid.tracks[0].automation[0].domain.default_value = 2.0;
  require(!ProjectModel::open(std::move(invalid), gate),
          "out-of-domain automation default reopened");
  invalid = *project.snapshot();
  invalid.tracks[0].automation[0].settings_generation = 0;
  require(!ProjectModel::open(std::move(invalid), gate), "zero automation generation reopened");

  auto instrument = model(gate);
  const auto instrument_track = id<TrackId>(80);
  REQUIRE_OK(instrument.apply(
      {AddTrack{instrument_track, TrackKind::instrument},
       SetNativeInstrumentPatch{instrument_track, NativeInstrumentPatch{"prism", 1, {}}}}));
  AutomationLane level{id<AutomationLaneId>(82),
                       TrackControlTarget{instrument_track, TrackControlTarget::Key::channel_level},
                       {AutomationRepresentation::physical, "mixer.channel_level", "linear_gain",
                        0.0, 4.0, 1.0, false, 0, AutomationScale::decibel},
                       AutomationTimebase::elapsed,
                       {{id<AutomationPointId>(83), 0, 0.5}},
                       1};
  REQUIRE_EQ(take(instrument.apply({AddAutomationLane{instrument_track, level}}))
                 .snapshot->tracks[0]
                 .automation,
             std::vector{level}, "instrument track level automation was rejected");
  const MidiClip notes{id<ClipId>(84),
                       MusicalInterval{{0}, {timeline::ticks_per_quarter * 2}},
                       {timeline::ticks_per_quarter},
                       false,
                       {}};
  REQUIRE_OK(instrument.apply({AddMidiClip{instrument_track, notes}}));
  auto midi_envelope = level;
  midi_envelope.id = id<AutomationLaneId>(85);
  midi_envelope.points[0].id = id<AutomationPointId>(86);
  auto with_midi_envelope = take(instrument.apply({AddAutomationLane{notes.id, midi_envelope}}));
  REQUIRE_EQ(with_midi_envelope.snapshot->tracks[0].midi_clips[0].automation,
             std::vector{midi_envelope}, "MIDI clip-local automation was not retained");
  auto midi_only = *with_midi_envelope.snapshot;
  midi_only.tracks[0].automation.clear();
  require(requires_automation_rendering(midi_only),
          "MIDI clip envelope was invisible to automation renderer guard");
  REQUIRE_EQ(take(instrument.apply({SetMidiClipPlacement{
                      notes.id, MusicalInterval{{timeline::ticks_per_quarter * 3},
                                                {timeline::ticks_per_quarter * 5}}}}))
                 .snapshot->tracks[0]
                 .midi_clips[0]
                 .automation,
             std::vector{midi_envelope}, "MIDI clip move rewrote local envelope");
}

void adding_clip_with_automation_reports_new_lane_ids() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio_track = id<TrackId>(8'100);
  const auto midi_track = id<TrackId>(8'101);
  REQUIRE_OK(project.apply({AddTrack{audio_track}, AddTrack{midi_track, TrackKind::instrument}}));
  auto audio = clip(8'102, MusicalInterval{{0}, {timeline::ticks_per_quarter}});
  auto midi = MidiClip{id<ClipId>(8'103),
                       MusicalInterval{{0}, {timeline::ticks_per_quarter}},
                       {timeline::ticks_per_quarter},
                       false,
                       {}};
  auto lane_for = [](TrackId track, AutomationLaneId lane, AutomationPointId point) {
    return AutomationLane{lane,
                          TrackControlTarget{track, TrackControlTarget::Key::channel_level},
                          {AutomationRepresentation::physical, "mixer.channel_level", "linear_gain",
                           0.0, 4.0, 1.0, false, 0, AutomationScale::decibel},
                          AutomationTimebase::musical,
                          {{point, 0, 0.5, AutomationShape::linear}},
                          1};
  };
  audio.automation.push_back(
      lane_for(audio_track, id<AutomationLaneId>(8'104), id<AutomationPointId>(8'105)));
  midi.automation.push_back(
      lane_for(midi_track, id<AutomationLaneId>(8'106), id<AutomationPointId>(8'107)));
  auto added = take(project.apply({AddClip{audio_track, audio}, AddMidiClip{midi_track, midi}}));
  REQUIRE_TRUE(added.changes.automation,
               "clip creation omitted owned automation from its change set");
  REQUIRE_EQ(added.changes.automation_lanes.size(), 2,
             "clip creation omitted owned automation from its change set");
  REQUIRE_NE(std::ranges::find(added.changes.automation_lanes, audio.automation.front().id),
             added.changes.automation_lanes.end(),
             "clip creation omitted owned automation from its change set");
  REQUIRE_NE(std::ranges::find(added.changes.automation_lanes, midi.automation.front().id),
             added.changes.automation_lanes.end(),
             "clip creation omitted owned automation from its change set");
}

void send_gain_automation_has_stable_route_binding() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio = id<TrackId>(310);
  const auto returned = id<TrackId>(311);
  const auto send = id<SendId>(312);
  const auto audio_clip = clip(313, ElapsedInterval{{0}, {100}});
  REQUIRE_OK(
      project.apply({AddTrack{audio}, AddTrack{returned, TrackKind::return_bus},
                     AddClip{audio, audio_clip}, AddSendRoute{audio, {send, returned, 0.5}}}));
  const AutomationDomain domain{AutomationRepresentation::physical,
                                "mixer.send_gain",
                                "linear",
                                0.0,
                                4.0,
                                1.0,
                                false,
                                0,
                                AutomationScale::decibel};
  const AutomationLane track_lane{id<AutomationLaneId>(314),
                                  SendGainTarget{send},
                                  domain,
                                  AutomationTimebase::elapsed,
                                  {{id<AutomationPointId>(315), 0, 0.25}},
                                  1};
  const AutomationLane clip_lane{id<AutomationLaneId>(316),
                                 SendGainTarget{send},
                                 domain,
                                 AutomationTimebase::elapsed,
                                 {{id<AutomationPointId>(317), 0, 0.75}},
                                 1};
  auto added = take(project.apply(
      {AddAutomationLane{audio, track_lane}, AddAutomationLane{audio_clip.id, clip_lane}}));
  REQUIRE_TRUE(added.changes.automation,
               "send gain track/clip lanes were not persisted atomically");
  REQUIRE_EQ(added.snapshot->tracks[0].automation, std::vector{track_lane},
             "send gain track/clip lanes were not persisted atomically");
  REQUIRE_EQ(added.snapshot->tracks[0].clips[0].automation, std::vector{clip_lane},
             "send gain track/clip lanes were not persisted atomically");
  auto orphan = track_lane;
  orphan.id = id<AutomationLaneId>(318);
  orphan.target = SendGainTarget{id<SendId>(999)};
  orphan.points[0].id = id<AutomationPointId>(319);
  REQUIRE_TRUE(!project.apply({AddAutomationLane{audio, orphan}}),
               "orphan send automation was accepted or its route removed silently");
  REQUIRE_TRUE(!project.apply({RemoveSendRoute{send}}),
               "orphan send automation was accepted or its route removed silently");
  auto bad_domain = track_lane;
  bad_domain.id = id<AutomationLaneId>(320);
  bad_domain.domain.semantic_key = "mixer.other";
  bad_domain.points[0].id = id<AutomationPointId>(321);
  require(!project.apply({ReplaceAutomationLane{track_lane.id, bad_domain}}),
          "send gain target accepted incompatible descriptor metadata");
  REQUIRE_TRUE(take(project.undo()).snapshot->tracks[0].automation.empty(),
               "send gain lane undo/redo lost stable route identity");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].automation, std::vector{track_lane},
             "send gain lane undo/redo lost stable route identity");
}

void insert_projection_preserves_order_and_signal_paths() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio = id<TrackId>(10);
  const auto group = id<TrackId>(11);
  const auto returns = id<TrackId>(12);
  const auto instrument = id<TrackId>(13);
  REQUIRE_OK(project.apply({AddTrack{audio}, AddTrack{group, TrackKind::group_bus},
                            AddTrack{returns, TrackKind::return_bus},
                            AddTrack{instrument, TrackKind::instrument, 3}}));
  auto state = *project.snapshot();
  auto first = plugin(100);
  auto second = plugin(101);
  second.missing = false;
  state.tracks[0].effects = {effect_slot(1000, first), effect_slot(1001, second)};
  state.tracks[0].effects[1].enabled = false;
  state.tracks[3].instrument_source = plugin(102);
  state.master_effects = {{id<InsertSlotId>(2000), true, NativeEffectPatch{"shape", 1, {}, 3}},
                          {id<InsertSlotId>(2001), true, NativeEffectPatch{"press", 1, {}, 4}},
                          {id<InsertSlotId>(2002), true, NativeEffectPatch{"expanse", 1, {}, 5}},
                          {id<InsertSlotId>(2003), true, NativeEffectPatch{"ceiling", 1, {}, 6}}};
  REQUIRE_OK(ProjectModel::open(state, gate), "projection fixture is not schema-valid");
  const auto projected = take(project_insert_chains(state));
  REQUIRE_EQ(projected.project, state.id,
             "projection dropped path identity or missing-plugin status");
  REQUIRE_EQ(projected.revision, state.revision,
             "projection dropped path identity or missing-plugin status");
  REQUIRE_EQ(projected.paths.size(), 5,
             "projection dropped path identity or missing-plugin status");
  REQUIRE_TRUE(projected.has_missing_plugins,
               "projection dropped path identity or missing-plugin status");
  REQUIRE_EQ(projected.paths[0].path, SignalPathId{audio},
             "effect order, bypass or placeholder status changed");
  REQUIRE_EQ(projected.paths[0].track_kind, TrackKind::audio,
             "effect order, bypass or placeholder status changed");
  REQUIRE_EQ(projected.paths[0].effects.size(), 2,
             "effect order, bypass or placeholder status changed");
  REQUIRE_EQ(projected.paths[0].effects[0].slot, id<InsertSlotId>(1000),
             "effect order, bypass or placeholder status changed");
  REQUIRE_EQ(projected.paths[0].effects[1].slot, id<InsertSlotId>(1001),
             "effect order, bypass or placeholder status changed");
  REQUIRE_EQ(projected.paths[0].effects[0].identity,
             (std::variant<PluginInstanceId, NativeInsertKind>{first.id}),
             "effect order, bypass or placeholder status changed");
  REQUIRE_EQ(projected.paths[0].effects[1].identity,
             (std::variant<PluginInstanceId, NativeInsertKind>{second.id}),
             "effect order, bypass or placeholder status changed");
  REQUIRE_TRUE(projected.paths[0].effects[0].enabled,
               "effect order, bypass or placeholder status changed");
  REQUIRE_TRUE(!projected.paths[0].effects[1].enabled,
               "effect order, bypass or placeholder status changed");
  REQUIRE_TRUE(projected.paths[0].effects[0].missing_plugin,
               "effect order, bypass or placeholder status changed");
  REQUIRE_TRUE(!projected.paths[0].effects[1].missing_plugin,
               "effect order, bypass or placeholder status changed");
  REQUIRE_EQ(projected.paths[1].path, SignalPathId{group}, "group or return signal path missing");
  REQUIRE_TRUE(projected.paths[1].effects.empty(), "group or return signal path missing");
  REQUIRE_EQ(projected.paths[2].path, SignalPathId{returns}, "group or return signal path missing");
  REQUIRE_TRUE(projected.paths[2].effects.empty(), "group or return signal path missing");
  REQUIRE_EQ(projected.paths[3].path, SignalPathId{instrument},
             "instrument source was placed in effect chain");
  REQUIRE_TRUE(projected.paths[3].effects.empty(), "instrument source was placed in effect chain");
  REQUIRE_EQ(std::get<ProjectedVst3InstrumentSource>(projected.paths[3].source).plugin,
             std::get<Vst3PluginInstance>(state.tracks[3].instrument_source).id,
             "instrument source was placed in effect chain");
  const auto& master = projected.paths[4];
  REQUIRE_EQ(master.path, SignalPathId{MasterSignalPath{}},
             "master effect order or generation changed");
  REQUIRE_TRUE(!master.track_kind, "master effect order or generation changed");
  REQUIRE_EQ(master.effects.size(), 4, "master effect order or generation changed");
  REQUIRE_EQ(master.effects[0].identity,
             (std::variant<PluginInstanceId, NativeInsertKind>{NativeInsertKind::effect}),
             "master effect order or generation changed");
  REQUIRE_EQ(master.effects[0].native_engine, "shape", "master effect order or generation changed");
  REQUIRE_EQ(master.effects[3].native_engine, "ceiling",
             "master effect order or generation changed");
  REQUIRE_EQ(master.effects[0].native_settings_generation, 3,
             "master effect order or generation changed");
  REQUIRE_EQ(master.effects[3].native_settings_generation, 6,
             "master effect order or generation changed");
  std::swap(state.tracks[0].effects[0], state.tracks[0].effects[1]);
  state.id = id<ProjectId>(2);
  const auto reordered = take(project_insert_chains(state));
  REQUIRE_EQ(reordered.paths[0].effects[0].slot, id<InsertSlotId>(1001),
             "slot identity changed on reorder or project identity edit");
  REQUIRE_EQ(reordered.paths[0].effects[1].slot, id<InsertSlotId>(1000),
             "slot identity changed on reorder or project identity edit");
}

void insert_projection_types_native_and_empty_sources() {
  FakeGate gate;
  auto project = model(gate);
  const auto synth = id<TrackId>(21);
  const auto empty = id<TrackId>(22);
  REQUIRE_OK(
      project.apply({AddTrack{synth, TrackKind::instrument}, AddTrack{empty, TrackKind::instrument},
                     SetNativeInstrumentPatch{synth, NativeInstrumentPatch{"crate", 1, {}}}}));
  const auto projected = take(project_insert_chains(*project.snapshot()));
  REQUIRE_EQ(projected.paths.size(), 3,
             "native or empty instrument source was not typed separately");
  REQUIRE_TRUE(projected.paths[0].effects.empty(),
               "native or empty instrument source was not typed separately");
  REQUIRE_EQ(std::get<ProjectedNativeInstrumentSource>(projected.paths[0].source).engine, "crate",
             "native or empty instrument source was not typed separately");
  REQUIRE_TRUE(std::holds_alternative<std::monostate>(projected.paths[1].source),
               "native or empty instrument source was not typed separately");
}

void insert_projection_rejects_invalid_snapshot() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio = id<TrackId>(30);
  const auto group = id<TrackId>(31);
  REQUIRE_OK(project.apply({AddTrack{audio}, AddTrack{group, TrackKind::group_bus}}));
  auto state = *project.snapshot();
  state.tracks[0].effects = {effect_slot(3000, plugin(301)), effect_slot(3001, plugin(301))};
  require(!project_insert_chains(state), "duplicate plugin identity was projected");
  state.tracks[0].effects.pop_back();
  state.tracks[1].id = audio;
  require(!project_insert_chains(state), "duplicate signal path was projected");
  state.tracks[1].id = group;
  state.tracks[0].effects.push_back(effect_slot(3000, plugin(302)));
  require(!project_insert_chains(state), "duplicate slot identity was projected");
  state.tracks[0].effects.clear();
  state.master_effects = {{id<InsertSlotId>(4000), true, NativeEffectPatch{}}};
  require(!project_insert_chains(state), "invalid native insert was projected");
  state.master_effects.clear();
  state.revision = 0;
  require(!project_insert_chains(state), "zero revision was projected");
}

void native_insert_commands_cover_all_signal_paths_and_history() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio = id<TrackId>(501);
  const auto instrument = id<TrackId>(502);
  const auto group = id<TrackId>(503);
  const auto returns = id<TrackId>(504);
  REQUIRE_OK(project.apply({AddTrack{audio}, AddTrack{instrument, TrackKind::instrument},
                            AddTrack{group, TrackKind::group_bus},
                            AddTrack{returns, TrackKind::return_bus}}));
  const std::vector<SignalPathId> paths{audio, instrument, group, returns, MasterSignalPath{}};
  for (std::size_t index = 0; index < paths.size(); ++index) {
    const auto& path = paths[index];
    NativeEffectProcessor processor = NativeEffectPatch{"ceiling"};
    if (index == 1) {
      processor = NativeEffectPatch{"shape", 1, {{"shape.gain_1", 3.0}}};
    } else if (index == 2) {
      processor = NativeEffectPatch{"expanse"};
    } else if (index == 3) {
      processor = NativeEffectPatch{"press"};
    }
    auto applied = take(project.apply({AddNativeEffect{path, 0, processor}}));
    require((std::holds_alternative<MasterSignalPath>(path)
                 ? applied.snapshot->master_effects.size() == 1
                 : applied.changes.tracks.size() == 1),
            "native effect did not reach selected signal path");
  }
  const auto before = project.snapshot();
  auto invalid = NativeEffectPatch{}; // no engine
  require(
      !project.apply({AddNativeEffect{audio, 1, invalid}}) &&
          !project.apply({AddNativeEffect{id<TrackId>(999), 0, NativeEffectPatch{"ceiling"}}}) &&
          !project.apply({AddNativeEffect{audio, 2, NativeEffectPatch{"ceiling"}}}) &&
          project.snapshot() == before,
      "invalid processor, path or insertion position published state");
  auto added = take(project.apply({AddNativeEffect{audio, 0, NativeEffectPatch{"press"}}}));
  const auto first = added.snapshot->tracks[0].effects[0].id;
  const auto second = added.snapshot->tracks[0].effects[1].id;
  REQUIRE_NE(first, second, "new insert ID was reused after earlier allocation");
  REQUIRE_GT(first.bits().low, second.bits().low,
             "new insert ID was reused after earlier allocation");
  auto moved = take(project.apply({MoveEffectInsert{audio, second, 0}}));
  REQUIRE_EQ(moved.snapshot->tracks[0].effects[0].id, second,
             "move did not use destination index after removal or preserve IDs");
  REQUIRE_EQ(moved.snapshot->tracks[0].effects[1].id, first,
             "move did not use destination index after removal or preserve IDs");
  auto bypassed = take(project.apply({SetEffectInsertEnabled{audio, first, false}}));
  REQUIRE_TRUE(!bypassed.snapshot->tracks[0].effects[1].enabled, "bypass lost insert identity");
  REQUIRE_EQ(bypassed.snapshot->tracks[0].effects[1].id, first, "bypass lost insert identity");
  const auto depth = project.undo_depth();
  const auto stable = project.snapshot();
  REQUIRE_TRUE(!project.apply({MoveEffectInsert{audio, second, 0}}),
               "no-op, invalid index or cross-path edit mutated project/history");
  REQUIRE_TRUE(!project.apply({SetEffectInsertEnabled{audio, first, false}}),
               "no-op, invalid index or cross-path edit mutated project/history");
  REQUIRE_TRUE(!project.apply({MoveEffectInsert{audio, second, 2}}),
               "no-op, invalid index or cross-path edit mutated project/history");
  REQUIRE_TRUE(!project.apply({SetEffectInsertEnabled{group, first, false}}),
               "no-op, invalid index or cross-path edit mutated project/history");
  REQUIRE_TRUE(!project.apply({RemoveEffectInsert{MasterSignalPath{}, first}}),
               "no-op, invalid index or cross-path edit mutated project/history");
  REQUIRE_EQ(project.undo_depth(), depth,
             "no-op, invalid index or cross-path edit mutated project/history");
  REQUIRE_EQ(project.snapshot(), stable,
             "no-op, invalid index or cross-path edit mutated project/history");
  REQUIRE_TRUE(!project.apply(
                   {SetEffectInsertEnabled{audio, first, true}, MoveEffectInsert{group, first, 0}}),
               "failed compound effect edit published a partial bypass change");
  REQUIRE_EQ(project.snapshot(), stable,
             "failed compound effect edit published a partial bypass change");
  REQUIRE_EQ(project.undo_depth(), depth,
             "failed compound effect edit published a partial bypass change");
  auto reopened = take(ProjectModel::open(*stable, gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0].effects, stable->tracks[0].effects,
             "edited insert order, bypass or allocator was not reopen-valid");
  REQUIRE_EQ(reopened.snapshot()->next_insert_slot_serial, stable->next_insert_slot_serial,
             "edited insert order, bypass or allocator was not reopen-valid");
  REQUIRE_TRUE(take(project.undo()).snapshot->tracks[0].effects[1].enabled,
               "insert move/bypass undo or redo changed identity or order");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].effects[0].id, first,
             "insert move/bypass undo or redo changed identity or order");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].effects[0].id, second,
             "insert move/bypass undo or redo changed identity or order");
  REQUIRE_TRUE(!take(project.redo()).snapshot->tracks[0].effects[1].enabled,
               "insert move/bypass undo or redo changed identity or order");
  const auto removed = take(project.apply({RemoveEffectInsert{audio, first}}));
  REQUIRE_EQ(removed.snapshot->tracks[0].effects.size(), 1,
             "explicit native effect removal changed neighbour identity");
  REQUIRE_EQ(removed.snapshot->tracks[0].effects[0].id, second,
             "explicit native effect removal changed neighbour identity");
  const auto retired_serial = removed.snapshot->next_insert_slot_serial;
  REQUIRE_OK(project.undo());
  REQUIRE_OK(project.apply({AddNativeEffect{audio, 0, NativeEffectPatch{"ceiling"}}}));
  REQUIRE_NE(project.snapshot()->tracks[0].effects[0].id, first,
             "divergent undo branch reused a retired insert identity");
  REQUIRE_GT(project.snapshot()->next_insert_slot_serial, retired_serial,
             "divergent undo branch reused a retired insert identity");
  auto full = *project.snapshot();
  while (full.tracks[2].effects.size() < max_effect_slots_per_path) {
    full.tracks[2].effects.push_back({id<InsertSlotId>(10'000 + full.tracks[2].effects.size()),
                                      true, NativeEffectPatch{"ceiling"}});
  }
  auto capped = take(ProjectModel::open(full, gate));
  const auto at_limit = capped.snapshot();
  REQUIRE_TRUE(!capped.apply({AddNativeEffect{group, max_effect_slots_per_path,
                                              NativeEffectPatch{"ceiling"}}}),
               "native insert exceeded per-path slot limit");
  REQUIRE_EQ(capped.snapshot(), at_limit, "native insert exceeded per-path slot limit");
  auto exhausted = *stable;
  exhausted.next_insert_slot_serial = std::numeric_limits<std::uint64_t>::max();
  auto no_ids = take(ProjectModel::open(exhausted, gate));
  require(!no_ids.apply({AddNativeEffect{group, 0, NativeEffectPatch{"ceiling"}}}),
          "exhausted durable insert allocator reused a slot ID");
  auto with_master_vst3 = *stable;
  const auto master_plugin = plugin(520);
  const auto master_slot = id<InsertSlotId>(521);
  with_master_vst3.master_effects.push_back({master_slot, true, master_plugin});
  auto master_project = take(ProjectModel::open(with_master_vst3, gate));
  auto master_removed =
      take(master_project.apply({RemoveEffectInsert{MasterSignalPath{}, master_slot}}));
  REQUIRE_EQ(master_removed.snapshot->master_effects, stable->master_effects,
             "master VST3 slot removal did not retain its undo identity");
  REQUIRE_EQ(master_removed.changes.plugins, std::vector{master_plugin.id},
             "master VST3 slot removal did not retain its undo identity");
  REQUIRE_EQ(take(master_project.undo()).snapshot->master_effects, with_master_vst3.master_effects,
             "master VST3 slot removal did not retain its undo identity");
  // The plugin commands reach a master effect too.
  const std::vector<std::uint8_t> master_state{7, 6};
  auto saved = take(master_project.apply({SetVst3ComponentState{master_plugin.id, master_state}}));
  REQUIRE_TRUE(saved.changes.master_effects, "master VST3 state edit was not a master change");
  REQUIRE_EQ(
      std::get<Vst3PluginInstance>(saved.snapshot->master_effects.back().processor).component_state,
      master_state, "master VST3 state edit was lost");
  auto plugin_removed = take(master_project.apply({RemoveVst3Plugin{master_plugin.id}}));
  REQUIRE_EQ(plugin_removed.snapshot->master_effects, stable->master_effects,
             "removing a master VST3 plugin left its slot");
}

void native_insert_settings_preserve_slot_and_history() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(701);
  REQUIRE_OK(project.apply({AddTrack{track}}));
  const NativeEffectPatch original_eq{"shape", 1, {{"shape.gain_1", 0.5}}, 3};
  REQUIRE_OK(project.apply({AddNativeEffect{track, 0, original_eq},
                            AddNativeEffect{track, 1, NativeEffectPatch{"press"}},
                            AddNativeEffect{track, 2, NativeEffectPatch{"expanse"}},
                            AddNativeEffect{track, 3, NativeEffectPatch{"ceiling"}}}));
  const auto first = project.snapshot();
  const auto slots = first->tracks[0].effects;
  const auto eq_id = slots[0].id;
  auto updated_eq = original_eq;
  updated_eq.parameters[0].value = 6.0;
  updated_eq.generation = 99; // Input generation must not control the saved clock.
  const auto prior_depth = project.undo_depth();
  const auto changed = take(project.apply({SetNativeEffectSettings{track, eq_id, updated_eq}}));
  const auto& saved_eq =
      std::get<NativeEffectPatch>(changed.snapshot->tracks[0].effects[0].processor);
  REQUIRE_EQ(project.undo_depth(), prior_depth + 1,
             "settings replacement changed slot identity, history or settings generation");
  REQUIRE_EQ(changed.changes.tracks, std::vector{track},
             "settings replacement changed slot identity, history or settings generation");
  REQUIRE_EQ(changed.snapshot->tracks[0].effects[0].id, eq_id,
             "settings replacement changed slot identity, history or settings generation");
  REQUIRE_TRUE(changed.snapshot->tracks[0].effects[0].enabled,
               "settings replacement changed slot identity, history or settings generation");
  REQUIRE_EQ(saved_eq.generation, 4,
             "settings replacement changed slot identity, history or settings generation");
  REQUIRE_EQ(saved_eq.parameters, updated_eq.parameters,
             "settings replacement changed slot identity, history or settings generation");
  const auto stable = project.snapshot();
  auto reject = [&](const SetNativeEffectSettings& command) {
    REQUIRE_TRUE(!project.apply({command}),
                 "invalid native settings edit published state or history");
    REQUIRE_EQ(project.snapshot(), stable,
               "invalid native settings edit published state or history");
    REQUIRE_EQ(project.undo_depth(), prior_depth + 1,
               "invalid native settings edit published state or history");
  };
  reject({track, eq_id, updated_eq});
  auto bad_eq = updated_eq;
  bad_eq.parameters[0].value = std::numeric_limits<double>::quiet_NaN();
  reject({track, eq_id, bad_eq});
  reject({track, eq_id, NativeEffectPatch{"ceiling"}});
  reject({id<TrackId>(999), eq_id, updated_eq});
  reject({MasterSignalPath{}, eq_id, updated_eq});
  reject({track, id<InsertSlotId>(999), updated_eq});
  auto undone = take(project.undo());
  REQUIRE_EQ(undone.snapshot->tracks[0].effects, slots,
             "native settings undo/redo did not restore exact chain");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].effects, stable->tracks[0].effects,
             "native settings undo/redo did not restore exact chain");
  const auto reopened = take(ProjectModel::open(*project.snapshot(), gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0].effects, stable->tracks[0].effects,
             "native settings did not survive project reopen");

  const std::vector<NativeEffectProcessor> replacements{
      NativeEffectPatch{"shape", 1, {{"shape.gain_1", -2.0}}, 1},
      NativeEffectPatch{"press", 1, {{"press.ratio", 3.0}}, 99},
      NativeEffectPatch{"expanse", 1, {{"expanse.mix", 0.5}}, 99},
      NativeEffectPatch{"ceiling", 1, {{"ceiling.ceiling", -2.0}}, 99}};
  for (std::size_t index = 0; index < replacements.size(); ++index) {
    const auto before = project.snapshot();
    auto applied =
        take(project.apply({SetNativeEffectSettings{track, slots[index].id, replacements[index]}}));
    REQUIRE_EQ(applied.snapshot->tracks[0].effects[index].id, slots[index].id,
               "native replacement failed to update the requested slot");
    REQUIRE_NE(applied.snapshot->tracks[0].effects[index].processor,
               before->tracks[0].effects[index].processor,
               "native replacement failed to update the requested slot");
  }
  const auto replaced_snapshot = project.snapshot();
  const auto& replaced_chain = replaced_snapshot->tracks[0].effects;
  const auto generation = [&](std::size_t index) {
    return std::get<NativeEffectPatch>(replaced_chain[index].processor).generation;
  };
  REQUIRE_EQ(generation(0), 5,
             "native settings generation followed caller data instead of saved state");
  REQUIRE_EQ(generation(1), 2,
             "native settings generation followed caller data instead of saved state");
  REQUIRE_EQ(generation(2), 2,
             "native settings generation followed caller data instead of saved state");
  REQUIRE_EQ(generation(3), 2,
             "native settings generation followed caller data instead of saved state");
  const auto before_compound = project.snapshot();
  const auto before_compound_depth = project.undo_depth();
  auto invalid_compressor = std::get<NativeEffectPatch>(replacements[1]);
  invalid_compressor.parameters[0].value = std::numeric_limits<double>::infinity();
  REQUIRE_TRUE(!project.apply({SetNativeEffectSettings{track, slots[0].id, updated_eq},
                               SetNativeEffectSettings{track, slots[1].id, invalid_compressor}}),
               "failed compound settings edit published a partial change");
  REQUIRE_EQ(project.snapshot(), before_compound,
             "failed compound settings edit published a partial change");
  REQUIRE_EQ(project.undo_depth(), before_compound_depth,
             "failed compound settings edit published a partial change");

  auto exhausted = *project.snapshot();
  std::get<NativeEffectPatch>(exhausted.tracks[0].effects[2].processor).generation =
      std::numeric_limits<std::uint64_t>::max();
  auto at_limit = take(ProjectModel::open(exhausted, gate));
  const auto at_limit_snapshot = at_limit.snapshot();
  REQUIRE_TRUE(
      !at_limit.apply({SetNativeEffectSettings{track, slots[2].id, NativeEffectPatch{"expanse"}}}),
      "generation overflow changed project state");
  REQUIRE_EQ(at_limit.snapshot(), at_limit_snapshot, "generation overflow changed project state");
  REQUIRE_EQ(at_limit.undo_depth(), 0, "generation overflow changed project state");

  auto with_plugin = *project.snapshot();
  const auto plugin_slot = id<InsertSlotId>(704);
  with_plugin.tracks[0].effects.push_back(effect_slot(704, plugin(705)));
  auto plugin_project = take(ProjectModel::open(with_plugin, gate));
  const auto plugin_snapshot = plugin_project.snapshot();
  REQUIRE_TRUE(!plugin_project.apply(
                   {SetNativeEffectSettings{track, plugin_slot, NativeEffectPatch{"shape"}}}),
               "native settings edit replaced a VST3 plugin slot");
  REQUIRE_EQ(plugin_project.snapshot(), plugin_snapshot,
             "native settings edit replaced a VST3 plugin slot");
  REQUIRE_EQ(plugin_project.undo_depth(), 0, "native settings edit replaced a VST3 plugin slot");
}

void convolution_insert_preserves_identity_and_undo() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(990);
  auto media = source(991, 48'000);
  REQUIRE_OK(project.apply({AddTrack{track}, AddSource{media}}));
  const ManagedAudioReference reference{media.id,          media.sha256,   media.byte_length,
                                        media.sample_rate, media.channels, media.frames};
  auto added =
      take(project.apply({AddNativeEffect{track, 0, NativeConvolution{reference, 0.3, 1}}}));
  const auto slot = added.snapshot->tracks[0].effects[0].id;
  REQUIRE_TRUE(has_convolution_insert(*added.snapshot),
               "saved convolution insert lost its managed IR identity");
  REQUIRE_EQ(std::get<NativeConvolution>(added.snapshot->tracks[0].effects[0].processor).ir,
             reference, "saved convolution insert lost its managed IR identity");
  const auto stable = project.snapshot();
  auto bad = reference;
  bad.sha256[0] = 'b';
  REQUIRE_TRUE(
      !project.apply({SetNativeEffectSettings{track, slot, NativeConvolution{bad, 0.4, 1}}}),
      "invalid relink or source deletion changed a referenced convolution IR");
  REQUIRE_TRUE(!project.apply({RemoveSource{media.id}}),
               "invalid relink or source deletion changed a referenced convolution IR");
  REQUIRE_EQ(project.snapshot(), stable,
             "invalid relink or source deletion changed a referenced convolution IR");
  auto changed = take(
      project.apply({SetNativeEffectSettings{track, slot, NativeConvolution{reference, 0.7, 1}}}));
  REQUIRE_EQ(
      std::get<NativeConvolution>(changed.snapshot->tracks[0].effects[0].processor).generation, 2,
      "convolution wet edit did not advance generation");
  REQUIRE_EQ(std::get<NativeConvolution>(changed.snapshot->tracks[0].effects[0].processor).wet, 0.7,
             "convolution wet edit did not advance generation");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].effects, stable->tracks[0].effects,
             "convolution undo or redo lost the IR, slot or generation");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].effects, changed.snapshot->tracks[0].effects,
             "convolution undo or redo lost the IR, slot or generation");
  REQUIRE_OK(project.apply({SetEffectInsertEnabled{track, slot, false}}));
  require(has_convolution_insert(*project.snapshot()),
          "bypassed convolution was lost from unsupported-render detection");
}

void effect_removal_cleans_only_attached_vst3_automation_atomically() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(601);
  const auto source = plugin(602);
  const auto other = plugin(603);
  const auto audio = clip(604, MusicalInterval{{0}, {timeline::ticks_per_quarter * 2}});
  REQUIRE_OK(
      project.apply({AddTrack{track}, AddClip{track, audio}, InsertVst3Plugin{track, 0, source},
                     InsertVst3Plugin{track, 1, other}}));
  const auto target = project.snapshot()->tracks[0].effects[0].id;
  const auto survivor = project.snapshot()->tracks[0].effects[1].id;
  const auto lane = vst_lane(605, source.id, 606);
  const auto clip_lane = vst_lane(607, source.id, 608);
  const auto keep = vst_lane(609, other.id, 610);
  REQUIRE_OK(project.apply({AddAutomationLane{track, lane}, AddAutomationLane{audio.id, clip_lane},
                            AddAutomationLane{track, keep}}));
  auto bypassed = take(project.apply({SetEffectInsertEnabled{track, target, false}}));
  REQUIRE_TRUE(!bypassed.snapshot->tracks[0].effects[0].enabled,
               "bypassing VST3 effect discarded its automation references");
  REQUIRE_EQ(bypassed.snapshot->tracks[0].automation.size(), 2,
             "bypassing VST3 effect discarded its automation references");
  REQUIRE_EQ(bypassed.snapshot->tracks[0].clips[0].automation.size(), 1,
             "bypassing VST3 effect discarded its automation references");
  const auto before = project.snapshot();
  auto removed = take(project.apply({RemoveEffectInsert{track, target}}));
  REQUIRE_EQ(removed.snapshot->tracks[0].effects.size(), 1,
             "effect removal did not atomically remove only its automation");
  REQUIRE_EQ(removed.snapshot->tracks[0].effects[0].id, survivor,
             "effect removal did not atomically remove only its automation");
  REQUIRE_EQ(removed.snapshot->tracks[0].automation, std::vector{keep},
             "effect removal did not atomically remove only its automation");
  REQUIRE_TRUE(removed.snapshot->tracks[0].clips[0].automation.empty(),
               "effect removal did not atomically remove only its automation");
  REQUIRE_TRUE(removed.changes.automation,
               "effect removal did not atomically remove only its automation");
  REQUIRE_EQ(removed.changes.automation_lanes.size(), 2,
             "effect removal did not atomically remove only its automation");
  auto reopened = take(ProjectModel::open(*removed.snapshot, gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0].automation, std::vector{keep},
             "removed effect left orphan automation in reopen-valid state");
  auto restored = take(project.undo());
  REQUIRE_EQ(restored.snapshot->tracks[0], before->tracks[0],
             "removal undo/redo did not restore plugin and attached lanes together");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0], removed.snapshot->tracks[0],
             "removal undo/redo did not restore plugin and attached lanes together");

  auto instrument = model(gate);
  const auto instrument_track = id<TrackId>(620);
  const auto notes = MidiClip{id<ClipId>(621),
                              MusicalInterval{{0}, {timeline::ticks_per_quarter * 2}},
                              {timeline::ticks_per_quarter},
                              false,
                              {}};
  const auto instrument_source = plugin(622);
  const auto instrument_effect = plugin(623);
  REQUIRE_OK(instrument.apply({AddTrack{instrument_track, TrackKind::instrument},
                               AddMidiClip{instrument_track, notes},
                               InsertVst3Plugin{instrument_track, 0, instrument_source},
                               InsertVst3Plugin{instrument_track, 0, instrument_effect}}));
  const auto midi_lane = vst_lane(624, instrument_effect.id, 625);
  REQUIRE_OK(instrument.apply({AddAutomationLane{notes.id, midi_lane}}));
  const auto effect_id = instrument.snapshot()->tracks[0].effects[0].id;
  auto cleared = take(instrument.apply({RemoveEffectInsert{instrument_track, effect_id}}));
  REQUIRE_TRUE(cleared.snapshot->tracks[0].midi_clips[0].automation.empty(),
               "MIDI clip automation or separate instrument source changed on effect removal");
  REQUIRE_EQ(std::get<Vst3PluginInstance>(cleared.snapshot->tracks[0].instrument_source).id,
             instrument_source.id,
             "MIDI clip automation or separate instrument source changed on effect removal");
  REQUIRE_EQ(take(instrument.undo()).snapshot->tracks[0].midi_clips[0].automation,
             std::vector{midi_lane},
             "MIDI clip automation or separate instrument source changed on effect removal");
}

void mute_and_delete_tracks_are_undoable() {
  FakeGate gate;
  auto project = model(gate);
  const auto audio = id<TrackId>(2);
  const auto group = id<TrackId>(3);
  REQUIRE_OK(project.apply({AddTrack{audio}}));
  require(!requires_routed_rendering(*project.snapshot()),
          "plain audio track unexpectedly required routed rendering");
  auto muted = take(project.apply({SetTrackMuted{audio, true}}));
  REQUIRE_TRUE(muted.snapshot->tracks[0].muted, "mute did not publish one routing edit");
  REQUIRE_TRUE(muted.changes.routing, "mute did not publish one routing edit");
  REQUIRE_EQ(muted.changes.tracks, std::vector{audio}, "mute did not publish one routing edit");
  REQUIRE_TRUE(requires_routed_rendering(*muted.snapshot), "mute did not publish one routing edit");
  REQUIRE_TRUE(!project.apply({SetTrackMuted{audio, true}}),
               "unchanged or missing track mute was accepted");
  REQUIRE_TRUE(!project.apply({SetTrackMuted{id<TrackId>(99), true}}),
               "unchanged or missing track mute was accepted");
  REQUIRE_TRUE(!take(project.undo()).snapshot->tracks[0].muted, "mute undo/redo lost state");
  REQUIRE_TRUE(take(project.redo()).snapshot->tracks[0].muted, "mute undo/redo lost state");
  auto solo = take(project.apply({SetTrackSoloed{audio, true}}));
  REQUIRE_TRUE(solo.snapshot->tracks[0].soloed, "solo did not publish a routed revision");
  REQUIRE_TRUE(solo.changes.routing, "solo did not publish a routed revision");
  REQUIRE_TRUE(requires_routed_rendering(*solo.snapshot), "solo did not publish a routed revision");
  REQUIRE_TRUE(!project.apply({SetTrackSoloed{audio, true}}),
               "unchanged or missing solo was accepted");
  REQUIRE_TRUE(!project.apply({SetTrackSoloed{id<TrackId>(99), true}}),
               "unchanged or missing solo was accepted");
  REQUIRE_TRUE(!take(project.undo()).snapshot->tracks[0].soloed, "solo undo/redo lost state");
  REQUIRE_TRUE(take(project.redo()).snapshot->tracks[0].soloed, "solo undo/redo lost state");
  auto channel = take(project.apply({SetTrackLevelPan{audio, 0.5, 0.25}}));
  REQUIRE_EQ(channel.snapshot->tracks[0].level, 0.5,
             "channel level/pan did not publish one routing edit");
  REQUIRE_EQ(channel.snapshot->tracks[0].pan, 0.25,
             "channel level/pan did not publish one routing edit");
  REQUIRE_TRUE(channel.changes.routing, "channel level/pan did not publish one routing edit");
  for (const auto [level, pan] : {std::pair{5.0, 0.0}, std::pair{1.0, -1.1},
                                  std::pair{std::numeric_limits<double>::quiet_NaN(), 0.0}})
    require(!project.apply({SetTrackLevelPan{audio, level, pan}}),
            "invalid channel setting was accepted");
  REQUIRE_TRUE(!project.apply({SetTrackLevelPan{audio, 0.5, 0.25}}),
               "unchanged or missing channel edit was accepted");
  REQUIRE_TRUE(!project.apply({SetTrackLevelPan{id<TrackId>(99), 1.0, 0.0}}),
               "unchanged or missing channel edit was accepted");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].level, 1.0,
             "channel level/pan undo/redo lost state");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].pan, 0.25,
             "channel level/pan undo/redo lost state");
  auto master = take(project.apply({SetMasterLevel{0.75}}));
  REQUIRE_EQ(master.snapshot->master_level, 0.75,
             "master output level did not publish one routing edit");
  REQUIRE_TRUE(master.changes.routing, "master output level did not publish one routing edit");
  for (const double level : {-0.01, 4.1, std::numeric_limits<double>::quiet_NaN()})
    require(!project.apply({SetMasterLevel{level}}), "invalid master level was accepted");
  require(!project.apply({SetMasterLevel{0.75}}), "unchanged master level was accepted");
  REQUIRE_EQ(take(project.undo()).snapshot->master_level, 1.0, "master level undo/redo lost state");
  REQUIRE_EQ(take(project.redo()).snapshot->master_level, 0.75,
             "master level undo/redo lost state");
  REQUIRE_OK(project.apply({AddTrack{group, TrackKind::group_bus}}));
  auto removed = take(project.apply({RemoveTrack{audio}}));
  REQUIRE_EQ(removed.snapshot->tracks.size(), 1, "track deletion did not publish one routing edit");
  REQUIRE_TRUE(removed.changes.routing, "track deletion did not publish one routing edit");
  REQUIRE_TRUE(take(project.undo()).snapshot->tracks[0].muted,
               "track deletion undo did not restore channel controls");
  REQUIRE_TRUE(project.snapshot()->tracks[0].soloed,
               "track deletion undo did not restore channel controls");
  REQUIRE_EQ(project.snapshot()->tracks[0].level, 0.5,
             "track deletion undo did not restore channel controls");
}

void clip_processing_is_durable_undoable_and_validated() {
  FakeGate gate;
  auto project = model(gate);
  const auto track = id<TrackId>(1);
  const auto clip_id = id<ClipId>(2);
  setup_track(project, track);
  REQUIRE_OK(project.apply(
      {AddClip{track, clip(2, MusicalInterval{{0}, {2 * timeline::ticks_per_quarter}})}}));
  const ClipProcessing reviewed{
      7.0,
      false,
      {{{25'000}, timeline::TickPosition{timeline::ticks_per_quarter / 2}},
       {{50'000}, timeline::TickPosition{timeline::ticks_per_quarter}}},
      {}};
  const auto before = project.snapshot();
  auto applied = take(project.apply({SetClipProcessing{clip_id, reviewed}}));
  REQUIRE_EQ(applied.snapshot->tracks[0].clips[0].processing, reviewed,
             "clip processing did not publish one generation");
  REQUIRE_EQ(applied.snapshot->tracks[0].clips[0].settings_generation, 2,
             "clip processing did not publish one generation");
  REQUIRE_EQ(applied.changes.clips, std::vector{clip_id},
             "clip processing did not publish one generation");
  REQUIRE_TRUE(!project.apply({SetClipProcessing{clip_id, reviewed}}),
               "unchanged or missing clip processing was accepted");
  REQUIRE_TRUE(!project.apply({SetClipProcessing{id<ClipId>(99), reviewed}}),
               "unchanged or missing clip processing was accepted");
  auto invalid = reviewed;
  invalid.pitch_semitones = 48.01;
  require(!project.apply({SetClipProcessing{clip_id, invalid}}), "out-of-range pitch accepted");
  for (const auto& corrupt : std::vector<std::function<void(ClipWarpSettings&)>>{
           // Deliberately corrupt in-memory enums: validation must reject them.
           [](ClipWarpSettings& warp) {
             // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
             warp.mode = static_cast<WarpMode>(6);
           },
           [](ClipWarpSettings& warp) {
             // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
             warp.beats.loop = static_cast<WarpBeatsLoop>(3);
           },
           [](ClipWarpSettings& warp) { warp.beats.envelope = 100.5; },
           [](ClipWarpSettings& warp) { warp.tones.grain_ms = 4.9; },
           [](ClipWarpSettings& warp) { warp.texture.grain_ms = 501.0; },
           [](ClipWarpSettings& warp) { warp.texture.flux = -0.1; },
           [](ClipWarpSettings& warp) { warp.complex_pro.formants = 100.1; },
           [](ClipWarpSettings& warp) { warp.complex_pro.envelope = 7.0; },
           [](ClipWarpSettings& warp) {
             warp.texture.flux = std::numeric_limits<double>::infinity();
           }}) {
    invalid = reviewed;
    corrupt(invalid.warp);
    require(!project.apply({SetClipProcessing{clip_id, invalid}}),
            "invalid warp mode setting accepted");
  }
  invalid = reviewed;
  invalid.pitch_semitones = std::numeric_limits<double>::quiet_NaN();
  require(!project.apply({SetClipProcessing{clip_id, invalid}}), "nonfinite pitch accepted");
  invalid = reviewed;
  invalid.reviewed_warp_markers[1].source_frame = {24'000};
  require(!project.apply({SetClipProcessing{clip_id, invalid}}),
          "out-of-order source marker accepted");
  invalid = reviewed;
  invalid.reviewed_warp_markers[1].target_offset =
      timeline::TickPosition{timeline::ticks_per_quarter / 2};
  require(!project.apply({SetClipProcessing{clip_id, invalid}}),
          "colliding target marker accepted");
  invalid = reviewed;
  invalid.reviewed_warp_markers[0].target_offset = timeline::SamplePosition{24'000};
  require(!project.apply({SetClipProcessing{clip_id, invalid}}), "wrong marker timebase accepted");
  REQUIRE_TRUE(!project.apply({SetClipSourceRange{clip_id, {{0}, {50'000}}}}),
               "trim or placement silently dropped a reviewed marker");
  REQUIRE_TRUE(!project.apply({SetClipPlacement{
                   clip_id, MusicalInterval{{0}, {timeline::ticks_per_quarter}}}}),
               "trim or placement silently dropped a reviewed marker");
  REQUIRE_EQ(project.snapshot()->tracks[0].clips[0].processing, reviewed,
             "invalid edit changed reviewed markers");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].clips[0].processing,
             before->tracks[0].clips[0].processing,
             "clip processing undo/redo lost reviewed intent");
  REQUIRE_EQ(take(project.redo()).snapshot->tracks[0].clips[0].processing, reviewed,
             "clip processing undo/redo lost reviewed intent");

  // A mode choice and its remembered parameters are one generation and one
  // undo step; other modes' parameters survive a switch.
  auto chosen = reviewed;
  chosen.pitch_semitones = -48.0;
  chosen.warp.mode = WarpMode::texture;
  chosen.warp.mode_overridden = true;
  chosen.warp.texture = {250.0, 40.0};
  chosen.warp.complex_pro = {60.0, 64.0};
  auto mode_edit = take(project.apply({SetClipProcessing{clip_id, chosen}}));
  REQUIRE_EQ(mode_edit.snapshot->tracks[0].clips[0].processing, chosen,
             "warp mode edit did not publish one generation");
  REQUIRE_EQ(mode_edit.snapshot->tracks[0].clips[0].settings_generation, 3,
             "warp mode edit did not publish one generation");
  auto switched = chosen;
  switched.warp.mode = WarpMode::complex_pro;
  REQUIRE_OK(project.apply({SetClipProcessing{clip_id, switched}}));
  REQUIRE_EQ(project.snapshot()->tracks[0].clips[0].processing.warp.texture, chosen.warp.texture,
             "warp mode switch lost remembered parameters or undo order");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].clips[0].processing, chosen,
             "warp mode switch lost remembered parameters or undo order");
  REQUIRE_EQ(take(project.undo()).snapshot->tracks[0].clips[0].processing, reviewed,
             "warp mode switch lost remembered parameters or undo order");

  ClipProcessing varispeed;
  varispeed.pitch_semitones = 5.0;
  varispeed.warp.mode = WarpMode::repitch;
  REQUIRE_TRUE(!varispeed.requires_rendering(),
               "Re-Pitch transpose was treated as active pitch processing");
  REQUIRE_TRUE(reviewed.requires_rendering(),
               "Re-Pitch transpose was treated as active pitch processing");
  varispeed.reviewed_warp_markers = reviewed.reviewed_warp_markers;
  require(varispeed.requires_rendering(), "Re-Pitch markers did not require rendering");

  auto elapsed = model(gate);
  setup_track(elapsed, track);
  REQUIRE_OK(elapsed.apply({AddClip{track, clip(3, ElapsedInterval{{0}, {48'000}})}}));
  ClipProcessing elapsed_review;
  elapsed_review.reviewed_warp_markers.push_back({{25'000}, timeline::SamplePosition{12'000}});
  REQUIRE_OK(elapsed.apply({SetClipProcessing{id<ClipId>(3), elapsed_review}}));
  auto changed_rate = take(elapsed.apply({SetSampleRate{96'000}}));
  const auto& scaled = changed_rate.snapshot->tracks[0].clips[0];
  REQUIRE_EQ(
      std::get<timeline::SamplePosition>(scaled.processing.reviewed_warp_markers[0].target_offset)
          .value,
      24'000, "elapsed marker and placement did not scale together");
  REQUIRE_EQ(std::get<ElapsedInterval>(scaled.placement).end.value, 96'000,
             "elapsed marker and placement did not scale together");
  changed_rate.stopped_transport.reset();
  REQUIRE_EQ(std::get<timeline::SamplePosition>(take(elapsed.undo())
                                                    .snapshot->tracks[0]
                                                    .clips[0]
                                                    .processing.reviewed_warp_markers[0]
                                                    .target_offset)
                 .value,
             12'000, "rate undo did not restore exact marker offset");
}

} // namespace

int main() {
  try {
    track_insertion_is_ordered_and_atomic();
    track_move_preserves_identity_and_is_undoable();
    revisions_and_atomic_compounds();
    rendered_intervals_and_map_revalidation();
    gestures_undo_and_redo();
    unchanged_edits_preserve_redo_history();
    rate_gate_conversion_and_exact_undo();
    removals_undo();
    invalid_generation_and_overflow();
    decoded_snapshot_validation_and_limits();
    saved_snapshot_restore_is_atomic_and_undoable();
    saved_snapshot_restore_respects_rate_gate();
    rate_failure_boundaries();
    source_trim_is_independent_of_arrangement();
    timing_change_set_reports_event_edits();
    large_decoded_manifest_ids_are_validated();
    source_records_and_unresolved_clips();
    external_source_relink_is_atomic_and_undoable();
    source_compounds_gestures_and_validation();
    source_count_has_bounded_validation();
    native_insert_order_and_retired_slot_identity();
    vst3_placeholders_commands_and_bounds();
    routing_intent_commands_and_rollback();
    sidechain_send_targets_one_built_in_effect();
    sidechain_send_can_name_a_saved_vst3_effect();
    automation_commands_validate_and_preserve_placeholders();
    adding_clip_with_automation_reports_new_lane_ids();
    sample_rate_rescales_elapsed_automation_atomically();
    send_gain_automation_has_stable_route_binding();
    insert_projection_preserves_order_and_signal_paths();
    insert_projection_types_native_and_empty_sources();
    insert_projection_rejects_invalid_snapshot();
    native_insert_commands_cover_all_signal_paths_and_history();
    native_insert_settings_preserve_slot_and_history();
    convolution_insert_preserves_identity_and_undo();
    effect_removal_cleans_only_attached_vst3_automation_atomically();
    mute_and_delete_tracks_are_undoable();
    clip_processing_is_durable_undoable_and_validated();
  } catch (const std::exception& error) {
    std::cerr << "project contract: " << error.what() << '\n';
    return 1;
  }
  std::cout << "project contract passed\n";
}
