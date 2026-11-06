#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <oscillate/application/clip_playback.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {

namespace app = oscillate::application;
namespace engine = oscillate::engine;
namespace media = oscillate::media;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

template <typename Id> Id id(std::uint64_t value) {
  auto result = Id::from_bits({0, value});
  REQUIRE_OK(result, "fixture ID failed");
  return result.value();
}

project::Snapshot snapshot() {
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  REQUIRE_TRUE(tempo, "fixture timing map failed");
  REQUIRE_TRUE(meter, "fixture timing map failed");
  const auto clip = project::Clip{id<project::ClipId>(3),
                                  id<project::SourceId>(4),
                                  {{2}, {6}},
                                  project::ElapsedInterval{{10}, {14}},
                                  1};
  const project::SourceRecord source{id<project::SourceId>(4),
                                     project::SourceEncoding::wav,
                                     std::string(64, 'a'),
                                     60,
                                     48000,
                                     2,
                                     8};
  return {id<project::ProjectId>(1),
          1,
          48000,
          std::move(tempo).value(),
          std::move(meter).value(),
          {{id<project::TrackId>(2), {clip}}},
          {source}};
}

void push(media::PrefetchRing& ring, std::uint64_t generation, std::uint64_t first,
          std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    const auto position = first + index;
    require(ring.try_push(
                {ring.clip(),
                 generation,
                 position,
                 {static_cast<float>(position) / 10.0F, -static_cast<float>(position) / 20.0F}}),
            "fixture prefetch push failed");
  }
}

void near(engine::StereoFrame frame, float left, float right) {
  REQUIRE_LT(std::fabs(frame.left - left), 0.00001F, "stereo sample or placement mismatch");
  REQUIRE_LT(std::fabs(frame.right - right), 0.00001F, "stereo sample or placement mismatch");
}

std::unique_ptr<engine::RenderPlan> prepare(std::uint64_t revision,
                                            const engine::NodeRequest& request,
                                            const engine::RenderPlan* prior = nullptr) {
  auto result = engine::prepare_graph(revision, 4, std::span(&request, 1), prior);
  REQUIRE_OK(result, "clip graph preparation failed");
  return std::move(result).value();
}

void placement_offset_seek_and_live_edit() {
  auto state = snapshot();
  auto ring = std::make_shared<media::PrefetchRing>(state.tracks[0].clips[0].id, 16);
  auto readiness = std::make_shared<app::PlaybackReadiness>();
  push(*ring, 1, 2, 4);
  readiness->set_steady(true);
  auto request = app::make_clip_request(state, ring->clip(), ring, readiness, 17, 4);
  REQUIRE_OK(request, "valid clip request failed");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(prepare(1, request.value())), engine::PublishStatus::accepted,
             "initial clip graph publish failed");

  std::array<engine::StereoFrame, 4> before{};
  require(graph.render_block(before, {0, 1}), "pre-clip silence render failed");
  for (const auto& frame : before) {
    near(frame, 0, 0);
  }
  counted_allocations = 0;
  counting_allocations = true;
  const bool rendered = graph.render_block(before, {8, 1});
  counting_allocations = false;
  REQUIRE_TRUE(rendered, "clip callback allocated or failed");
  REQUIRE_EQ(counted_allocations, 0, "clip callback allocated or failed");
  near(before[0], 0, 0);
  near(before[1], 0, 0);
  near(before[2], 0.2F, -0.1F);
  near(before[3], 0.3F, -0.15F);

  state.revision = 2; // An unrelated project edit retains the source runtime.
  auto unchanged = app::make_clip_request(state, ring->clip(), ring, readiness, 17, 4);
  REQUIRE_TRUE(unchanged, "unrelated revision changed runtime compatibility");
  REQUIRE_EQ(unchanged.value().compatibility_key, request.value().compatibility_key,
             "unrelated revision changed runtime compatibility");
  auto next = prepare(2, unchanged.value(), graph.active_plan());
  REQUIRE_EQ(next->transitions().size(), 1, "live edit did not reuse clip runtime");
  REQUIRE_EQ(next->transitions()[0].kind, engine::TransitionKind::reused,
             "live edit did not reuse clip runtime");
  REQUIRE_EQ(graph.publish(std::move(next)), engine::PublishStatus::accepted,
             "live graph edit publish failed");
  std::array<engine::StereoFrame, 4> after{};
  require(graph.render_block(after, {12, 1}), "live edit render failed");
  near(after[0], 0.4F, -0.2F);
  near(after[1], 0.5F, -0.25F);
  near(after[2], 0, 0);
  near(after[3], 0, 0);
  REQUIRE_EQ(graph.active_revision(), 2, "live edit did not activate");
  (void)graph.collect_retired();

  readiness->set_steady(false);
  push(*ring, 2, 3, 2);
  std::array<engine::StereoFrame, 2> seek{};
  require(graph.render_block(seek, {11, 2}), "seek render failed");
  near(seek[0], 0.3F, -0.15F);
  near(seek[1], 0.4F, -0.2F);
  REQUIRE_EQ(ring->underrun_count(), 0, "seek priming counted as underrun");
}

void clip_fades_shape_live_samples_without_callback_allocation() {
  auto state = snapshot();
  state.tracks.front().clips.front().fades = {0.5, 0.5, project::FadeCurve::linear,
                                              project::FadeCurve::linear};
  auto ring = std::make_shared<media::PrefetchRing>(state.tracks.front().clips.front().id, 16);
  auto readiness = std::make_shared<app::PlaybackReadiness>();
  push(*ring, 1, 2, 4);
  readiness->set_steady(true);
  auto faded = app::make_clip_request(state, ring->clip(), ring, readiness, 22, 4);
  REQUIRE_OK(faded, "faded clip request failed");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(prepare(1, faded.value())), engine::PublishStatus::accepted,
             "faded clip graph publish failed");
  std::array<engine::StereoFrame, 4> output{};
  counted_allocations = 0;
  counting_allocations = true;
  const bool rendered = graph.render_block(output, {10, 1});
  counting_allocations = false;
  REQUIRE_TRUE(rendered, "faded clip callback allocated or failed");
  REQUIRE_EQ(counted_allocations, 0, "faded clip callback allocated or failed");
  near(output[0], 0.0F, 0.0F);
  near(output[1], 0.15F, -0.075F);
  near(output[2], 0.2F, -0.1F);
  near(output[3], 0.0F, 0.0F);
  state.tracks.front().clips.front().fades.out_curve = project::FadeCurve::smoothstep;
  auto changed = app::make_clip_request(state, ring->clip(), ring, readiness, 22, 4);
  REQUIRE_TRUE(changed, "fade curve change silently reused stale processor runtime");
  REQUIRE_NE(changed.value().compatibility_key, faded.value().compatibility_key,
             "fade curve change silently reused stale processor runtime");
}

void gaps_are_counted_only_after_priming() {
  auto state = snapshot();
  auto ring = std::make_shared<media::PrefetchRing>(state.tracks[0].clips[0].id, 8);
  auto readiness = std::make_shared<app::PlaybackReadiness>();
  auto request = app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4);
  REQUIRE_OK(request, "gap request failed");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(prepare(1, request.value())), engine::PublishStatus::accepted,
             "gap graph publish failed");
  std::array<engine::StereoFrame, 2> block{};
  require(graph.render_block(block, {10, 1}), "priming render failed");
  near(block[0], 0, 0);
  near(block[1], 0, 0);
  REQUIRE_EQ(ring->underrun_count(), 0, "priming gap counted");
  readiness->set_steady(true);
  require(graph.render_block(block, {12, 1}), "steady render failed");
  REQUIRE_EQ(ring->underrun_count(), 1, "steady gap not counted exactly once");
  const auto reported = ring->pop_underrun();
  REQUIRE_TRUE(reported, "direct clip graph lost the affected source interval");
  REQUIRE_EQ(reported->clip, ring->clip(), "direct clip graph lost the affected source interval");
  REQUIRE_EQ(reported->generation, 1, "direct clip graph lost the affected source interval");
  REQUIRE_EQ(reported->first_source_frame, 4,
             "direct clip graph lost the affected source interval");
  REQUIRE_EQ(reported->missing_frames, 2, "direct clip graph lost the affected source interval");
  near(block[0], 0, 0);
  near(block[1], 0, 0);
}

void geometry_changes_replace_runtime_and_ring_retirement_is_safe() {
  auto state = snapshot();
  auto old_ring = std::make_shared<media::PrefetchRing>(state.tracks[0].clips[0].id, 8);
  const std::weak_ptr<media::PrefetchRing> old_lifetime = old_ring;
  auto readiness = std::make_shared<app::PlaybackReadiness>();
  engine::GraphPublisher graph;
  {
    auto first = app::make_clip_request(state, old_ring->clip(), old_ring, readiness, 7, 4);
    REQUIRE_OK(first, "old clip request failed");
    REQUIRE_EQ(graph.publish(prepare(1, first.value())), engine::PublishStatus::accepted,
               "old graph publish failed");
  }
  std::array<engine::StereoFrame, 2> block{};
  require(graph.render_block(block, {10, 1}), "old graph render failed");
  old_ring.reset();
  require(!old_lifetime.expired(), "active graph did not retain ring");

  state.tracks[0].clips[0].placement = project::ElapsedInterval{{20}, {24}};
  auto new_ring = std::make_shared<media::PrefetchRing>(state.tracks[0].clips[0].id, 8);
  auto moved = app::make_clip_request(state, new_ring->clip(), new_ring, readiness, 7, 4);
  REQUIRE_OK(moved, "moved clip request failed");
  auto moved_plan = prepare(2, moved.value(), graph.active_plan());
  REQUIRE_EQ(moved_plan->transitions().size(), 1,
             "geometry/ring change silently reused stale runtime");
  REQUIRE_EQ(moved_plan->transitions()[0].kind, engine::TransitionKind::replaced,
             "geometry/ring change silently reused stale runtime");
  REQUIRE_TRUE(moved_plan->transitions()[0].audible_continuity_limited,
               "geometry/ring change silently reused stale runtime");
  REQUIRE_EQ(graph.publish(std::move(moved_plan)), engine::PublishStatus::accepted,
             "moved graph publish failed");
  require(!old_lifetime.expired(), "pending graph destroyed active ring");
  require(graph.render_block(block, {20, 1}), "moved graph render failed");
  require(!old_lifetime.expired(), "callback destroyed retired ring");
  REQUIRE_EQ(graph.collect_retired().reclaimed, 1, "retired graph was not reclaimed");
  require(old_lifetime.expired(), "control reclamation leaked retired ring");

  state.tracks[0].clips[0].source_range = {{3}, {7}};
  auto trimmed = app::make_clip_request(state, new_ring->clip(), new_ring, readiness, 7, 4);
  REQUIRE_TRUE(trimmed, "source trim did not invalidate runtime");
  REQUIRE_NE(trimmed.value().compatibility_key, moved.value().compatibility_key,
             "source trim did not invalidate runtime");
  state.tracks[0].clips[0].settings_generation = 2;
  auto settings = app::make_clip_request(state, new_ring->clip(), new_ring, readiness, 7, 4);
  REQUIRE_TRUE(settings, "clip settings did not invalidate runtime");
  REQUIRE_NE(settings.value().compatibility_key, trimmed.value().compatibility_key,
             "clip settings did not invalidate runtime");
}

void control_side_rejects_unrenderable_clips() {
  auto state = snapshot();
  auto ring = std::make_shared<media::PrefetchRing>(state.tracks[0].clips[0].id, 8);
  auto readiness = std::make_shared<app::PlaybackReadiness>();
  require(!app::make_clip_request(state, ring->clip(), {}, readiness, 1, 4),
          "missing ring accepted");
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 0, 4),
          "zero instance ID accepted");
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 1, 8193),
          "oversized callback capacity accepted");
  require(!app::make_clip_request(state, id<project::ClipId>(99), ring, readiness, 1, 4),
          "mismatched ring clip accepted");
  auto unknown_ring = std::make_shared<media::PrefetchRing>(id<project::ClipId>(99), 8);
  auto missing_clip =
      app::make_clip_request(state, unknown_ring->clip(), unknown_ring, readiness, 1, 4);
  REQUIRE_TRUE(!missing_clip, "missing clip was rendered");
  REQUIRE_EQ(missing_clip.error().code, oscillate::foundation::ErrorCode::not_found,
             "missing clip was rendered");
  state.sources.clear();
  auto unresolved = app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4);
  REQUIRE_TRUE(!unresolved, "unresolved source was rendered");
  REQUIRE_EQ(unresolved.error().code, oscillate::foundation::ErrorCode::unavailable,
             "unresolved source was rendered");
  state = snapshot();
  state.sources[0].sample_rate = oscillate::test::sample_rate_44k;
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4),
          "rate mismatch accepted");
  state = snapshot();
  state.tracks[0].clips[0].source_range.end = {9};
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4),
          "source range past EOF accepted");
  state = snapshot();
  state.tracks[0].clips[0].placement = project::ElapsedInterval{{10}, {15}};
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4),
          "time stretch was silently accepted");
  state = snapshot();
  state.tracks[0].clips[0].placement = project::MusicalInterval{{0}, {160'000}};
  auto musical = app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4);
  REQUIRE_OK(musical, "equivalent musical placement rejected");
  state.tracks[0].clips[0].placement = project::ElapsedInterval{{-1}, {3}};
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4),
          "negative clip placement accepted");
  state = snapshot();
  state.tracks[0].clips[0].processing.pitch_semitones = 5.0;
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4),
          "unrendered pitch intent played the source unchanged");
  state = snapshot();
  state.tracks[0].clips[0].processing.reviewed_warp_markers.push_back(
      {{4}, timeline::SamplePosition{1}});
  require(!app::make_clip_request(state, ring->clip(), ring, readiness, 1, 4),
          "same-duration internal warp intent played the source unchanged");
}

} // namespace

int main() {
  try {
    placement_offset_seek_and_live_edit();
    clip_fades_shape_live_samples_without_callback_allocation();
    gaps_are_counted_only_after_priming();
    geometry_changes_replace_runtime_and_ring_retirement_is_safe();
    control_side_rejects_unrenderable_clips();
    std::cout << "application clip playback tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
