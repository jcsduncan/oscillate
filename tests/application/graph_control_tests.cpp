#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <oscillate/application/graph_control.hpp>
#include <oscillate/native_dsp/effects/effect.hpp>
#include <oscillate/native_inserts/effect_binding.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
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
namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace dsp = oscillate::native_dsp;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

project::ProjectModel make_model(Gate& gate) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000, std::move(tempo),
                                            std::move(meter), gate));
}

project::Clip make_clip() {
  return {id<project::ClipId>(3),
          id<project::SourceId>(4),
          {{2}, {6}},
          project::ElapsedInterval{{10}, {14}},
          1};
}

project::SourceRecord make_source() {
  return {id<project::SourceId>(4),
          project::SourceEncoding::wav,
          std::string(64, 'a'),
          60,
          48'000,
          2,
          8};
}

app::ClipFeed feed(std::shared_ptr<media::PrefetchRing> ring,
                   std::shared_ptr<app::PlaybackReadiness> readiness) {
  return {ring->clip(), std::move(ring), std::move(readiness), 17};
}

void push(media::PrefetchRing& ring, std::uint64_t generation, std::uint64_t first,
          std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    const auto sample = first + index;
    require(
        ring.try_push({ring.clip(),
                       generation,
                       sample,
                       {static_cast<float>(sample) / 10.0F, -static_cast<float>(sample) / 20.0F}}),
        "fixture ring push failed");
  }
}

void near(engine::StereoFrame frame, float left, float right) {
  REQUIRE_LT(std::fabs(frame.left - left), 0.00001F, "old graph did not remain audible");
  REQUIRE_LT(std::fabs(frame.right - right), 0.00001F, "old graph did not remain audible");
}

void bootstrap_retry_failure_and_undo() {
  Gate gate;
  auto model = make_model(gate);
  auto ring = std::make_shared<media::PrefetchRing>(id<project::ClipId>(3), 16);
  auto readiness = std::make_shared<app::PlaybackReadiness>();
  readiness->set_steady(true);
  push(*ring, 1, 2, 4);
  auto initial = take(
      model.apply({project::AddSource{make_source()}, project::AddTrack{id<project::TrackId>(2)},
                   project::AddClip{id<project::TrackId>(2), make_clip()}}));
  REQUIRE_EQ(initial.snapshot->revision, 2, "fixture command was not atomic");

  engine::GraphPublisher graph;
  app::GraphControl control(model, graph, 4);
  require(!control.require_export_ready(2), "export accepted an unapplied opened model");
  auto missing = control.submit_current(std::nullopt);
  const auto bootstrap_status = control.status();
  REQUIRE_TRUE(!missing, "bootstrap failure did not retain accepted model revision");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "bootstrap failure did not retain accepted model revision");
  REQUIRE_EQ(control.status().active_graph_revision, 0,
             "bootstrap failure did not retain accepted model revision");
  REQUIRE_EQ(bootstrap_status.change.value_or(app::ChangeDescriptor{}).label, "open project",
             "bootstrap failure did not retain accepted model revision");
  require(!control.require_export_ready(2), "export accepted a failed bootstrap graph");
  REQUIRE_OK(control.retry(feed(ring, readiness)), "bootstrap retry did not publish same revision");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "publication was shown as audible before callback activation");
  std::array<engine::StereoFrame, 2> block{};
  require(graph.render_block(block, {10, 1}), "bootstrap render failed");
  near(block[0], 0.2F, -0.1F);
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "bootstrap activation or export guard failed");
  REQUIRE_TRUE(control.require_export_ready(2), "bootstrap activation or export guard failed");

  auto moved = take(model.apply(
      {project::SetClipPlacement{id<project::ClipId>(3), project::ElapsedInterval{{20}, {24}}}}));
  auto failed = control.submit(moved, std::nullopt, "move-clip");
  const auto failed_status = control.status();
  const auto failed_change = failed_status.change.value_or(app::ChangeDescriptor{});
  REQUIRE_TRUE(!failed, "accepted edit failure was not visible");
  REQUIRE_EQ(control.status().model_revision, 3, "accepted edit failure was not visible");
  REQUIRE_EQ(control.status().active_graph_revision, 2, "accepted edit failure was not visible");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "accepted edit failure was not visible");
  REQUIRE_OK(control.status().failure, "accepted edit failure was not visible");
  REQUIRE_EQ(failed_change.label, "move-clip", "accepted edit failure was not visible");
  REQUIRE_EQ(failed_change.affected.clips.size(), 1, "accepted edit failure was not visible");
  REQUIRE_EQ(failed_change.affected.clips[0], id<project::ClipId>(3),
             "accepted edit failure was not visible");
  require(!control.require_export_ready(3), "export silently used previous graph");
  require(graph.render_block(block, {12, 1}), "old graph stopped on failed edit");
  near(block[0], 0.4F, -0.2F);
  near(block[1], 0.5F, -0.25F);
  REQUIRE_EQ(graph.active_revision(), 2, "failed edit replaced old graph");

  REQUIRE_OK(control.retry(feed(ring, readiness)), "failed edit retry did not publish");
  const auto retry_change = control.status().change.value_or(app::ChangeDescriptor{});
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "retry was shown as active before callback boundary");
  REQUIRE_EQ(retry_change.label, "move-clip", "retry was shown as active before callback boundary");
  REQUIRE_EQ(retry_change.affected.clips.size(), 1,
             "retry was shown as active before callback boundary");
  REQUIRE_EQ(retry_change.affected.clips[0], id<project::ClipId>(3),
             "retry was shown as active before callback boundary");
  require(graph.render_block(block, {0, 1}), "retry activation render failed");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, 3, "retry did not activate current edit");
  REQUIRE_TRUE(control.require_export_ready(3), "retry did not activate current edit");

  push(*ring, 2, 2, 4);
  auto failed_again = take(model.apply(
      {project::SetClipPlacement{id<project::ClipId>(3), project::ElapsedInterval{{30}, {34}}}}));
  require(!control.submit(failed_again, std::nullopt, "move-clip"),
          "missing feed unexpectedly applied");
  require(graph.render_block(block, {20, 2}), "retry graph stopped on later failed edit");
  near(block[0], 0.2F, -0.1F);
  near(block[1], 0.3F, -0.15F);

  auto restored = take(control.undo_and_submit(feed(ring, readiness)));
  const auto undo_change = control.status().change.value_or(app::ChangeDescriptor{});
  REQUIRE_EQ(restored.snapshot->revision, 5, "undo did not publish a new pending revision");
  REQUIRE_EQ(control.status().model_revision, 5, "undo did not publish a new pending revision");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "undo did not publish a new pending revision");
  REQUIRE_EQ(undo_change.label, "undo", "undo did not publish a new pending revision");
  REQUIRE_TRUE(!undo_change.affected.clips.empty(), "undo did not publish a new pending revision");
  require(graph.render_block(block, {0, 1}), "undo activation render failed");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, 5, "undo did not recover active graph");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "undo did not recover active graph");
  REQUIRE_TRUE(control.require_export_ready(5), "undo did not recover active graph");
}

void superseded_result_is_ignored() {
  Gate gate;
  auto model = make_model(gate);
  engine::GraphPublisher graph;
  app::GraphControl control(model, graph, 4);
  REQUIRE_OK(control.submit_current(std::nullopt), "initial empty graph publication failed");
  std::array<engine::StereoFrame, 1> block{};
  require(graph.render_block(block, {0, 1}), "empty graph render failed");
  control.observe();

  auto old = take(model.apply({project::AddTrack{id<project::TrackId>(2)}}));
  auto current = take(model.apply({project::AddTrack{id<project::TrackId>(3)}}));
  REQUIRE_OK(control.submit(current, std::nullopt, "second-add-track"),
             "current revision publication failed");
  const auto pending = control.status();
  auto stale = control.submit(old, std::nullopt, "stale-add-track");
  const auto after_stale = control.status();
  REQUIRE_TRUE(!stale, "superseded result changed current status");
  REQUIRE_EQ(stale.error().code, foundation::ErrorCode::conflict,
             "superseded result changed current status");
  REQUIRE_EQ(after_stale.model_revision, pending.model_revision,
             "superseded result changed current status");
  REQUIRE_EQ(after_stale.application, app::PlaybackApplication::pending,
             "superseded result changed current status");
  REQUIRE_TRUE(!after_stale.failure, "superseded result changed current status");
  REQUIRE_EQ(after_stale.change.value_or(app::ChangeDescriptor{}).label, "second-add-track",
             "superseded result changed current status");
  require(graph.render_block(block, {1, 1}), "current graph render failed");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, current.snapshot->revision,
             "stale result displaced active revision");
  REQUIRE_TRUE(control.require_export_ready(current.snapshot->revision),
               "stale result displaced active revision");
  require(!control.retry(std::nullopt), "retry accepted an active graph");
}

void preparation_failures_and_supersession() {
  Gate gate;
  auto model = make_model(gate);
  engine::GraphPublisher graph;
  app::GraphControl control(model, graph, 0);
  auto invalid_block = control.submit_current(std::nullopt);
  REQUIRE_TRUE(!invalid_block, "graph preparation accepted zero capacity");
  REQUIRE_EQ(invalid_block.error().code, foundation::ErrorCode::invalid_argument,
             "graph preparation accepted zero capacity");
  require(!control.undo_and_submit(std::nullopt), "undo accepted an empty history");
  auto added = take(model.apply({project::AddTrack{id<project::TrackId>(2)}}));
  require(!control.retry(std::nullopt), "retry used a stale failed model revision");
  REQUIRE_EQ(control.status().model_revision, 1, "stale retry altered failure status");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "stale retry altered failure status");
  require(!control.submit_current(std::nullopt), "zero-capacity graph unexpectedly published");
  REQUIRE_EQ(control.status().model_revision, added.snapshot->revision,
             "submit_current did not pick up accepted revision");

  engine::GraphPublisher other_graph;
  app::GraphControl other(model, other_graph, 4);
  REQUIRE_OK(other.submit_current(std::nullopt), "valid empty graph failed");
  auto duplicate = other.submit_current(std::nullopt);
  REQUIRE_TRUE(!duplicate, "duplicate pending publication was not diagnosed");
  REQUIRE_EQ(duplicate.error().code, foundation::ErrorCode::conflict,
             "duplicate pending publication was not diagnosed");
  REQUIRE_EQ(other.status().application, app::PlaybackApplication::failed,
             "duplicate pending publication was not diagnosed");
  std::array<engine::StereoFrame, 1> block{};
  require(other_graph.render_block(block, {0, 1}), "pending graph did not activate");
  other.observe();
  REQUIRE_EQ(other.status().application, app::PlaybackApplication::active,
             "actual activation did not clear stale publication error");

  auto source = make_source();
  auto clip = make_clip();
  auto second = make_clip();
  second.id = id<project::ClipId>(5);
  second.placement = project::ElapsedInterval{{20}, {24}};
  REQUIRE_OK(
      model.apply({project::AddSource{source}, project::AddClip{id<project::TrackId>(2), clip},
                   project::AddClip{id<project::TrackId>(2), second}}));
  auto multi = other.submit_current(std::nullopt);
  REQUIRE_TRUE(!multi, "graph silently omitted unprepared clips");
  REQUIRE_EQ(multi.error().code, foundation::ErrorCode::unavailable,
             "graph silently omitted unprepared clips");
  REQUIRE_EQ(other.status().active_graph_revision, added.snapshot->revision,
             "graph silently omitted unprepared clips");

  auto single = take(model.undo());
  require(single.snapshot->tracks[0].clips.empty(), "fixture undo failed");
  REQUIRE_OK(
      model.apply({project::AddSource{source}, project::AddClip{id<project::TrackId>(2), clip}}));
  auto ring = std::make_shared<media::PrefetchRing>(id<project::ClipId>(99), 8);
  auto readiness = std::make_shared<app::PlaybackReadiness>();
  auto wrong_feed = other.submit_current(feed(ring, readiness));
  REQUIRE_TRUE(!wrong_feed, "wrong clip feed was accepted");
  REQUIRE_EQ(wrong_feed.error().code, foundation::ErrorCode::not_found,
             "wrong clip feed was accepted");
}

void multiple_audio_clips_mix_and_publish_atomically() {
  Gate gate;
  auto model = make_model(gate);
  const auto source = make_source();
  auto first = make_clip();
  auto later = first;
  later.id = id<project::ClipId>(5);
  later.placement = project::ElapsedInterval{{14}, {18}};
  auto simultaneous = first;
  simultaneous.id = id<project::ClipId>(6);
  simultaneous.placement = project::ElapsedInterval{{11}, {15}};
  REQUIRE_OK(model.apply({project::AddSource{source}, project::AddTrack{id<project::TrackId>(2)},
                          project::AddTrack{id<project::TrackId>(7)},
                          project::AddClip{id<project::TrackId>(2), first},
                          project::AddClip{id<project::TrackId>(2), later},
                          project::AddClip{id<project::TrackId>(7), simultaneous}}));
  const std::array ids{first.id, later.id, simultaneous.id};
  std::array<std::shared_ptr<media::PrefetchRing>, 3> rings{};
  std::array<std::shared_ptr<app::PlaybackReadiness>, 3> readiness{};
  std::vector<app::ClipFeed> feeds;
  feeds.reserve(ids.size());
  for (std::size_t index = 0; index < ids.size(); ++index) {
    rings[index] = std::make_shared<media::PrefetchRing>(ids[index], 16);
    readiness[index] = std::make_shared<app::PlaybackReadiness>();
    readiness[index]->set_steady(true);
    push(*rings[index], 1, 2, 4);
    feeds.push_back({ids[index], rings[index], readiness[index], index + 1});
  }
  engine::GraphPublisher graph;
  app::GraphControl control(model, graph, 4);
  REQUIRE_TRUE(!control.submit_current(std::span(feeds).first(2)),
               "partial clip feed list was accepted");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "partial clip feed list was accepted");
  auto duplicate = feeds;
  duplicate[2] = feeds[0];
  REQUIRE_TRUE(!control.retry(duplicate), "duplicate clip feed was accepted");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "duplicate clip feed was accepted");
  REQUIRE_OK(control.retry(feeds), "complete multi-clip graph failed");
  std::array<engine::StereoFrame, 4> output{};
  counted_allocations = 0;
  counting_allocations = true;
  const bool rendered = graph.render_block(output, {10, 1});
  counting_allocations = false;
  REQUIRE_TRUE(rendered, "multi-clip callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "multi-clip callback failed or allocated");
  near(output[0], 0.2F, -0.1F);
  near(output[1], 0.5F, -0.25F);
  near(output[2], 0.7F, -0.35F);
  near(output[3], 0.9F, -0.45F);
  require(graph.render_block(output, {14, 1}), "sequential render failed");
  near(output[0], 0.7F, -0.35F);
  near(output[1], 0.3F, -0.15F);
  near(output[2], 0.4F, -0.2F);
  near(output[3], 0.5F, -0.25F);
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "multi-clip graph activation was not visible");
  REQUIRE_TRUE(control.require_export_ready(model.snapshot()->revision),
               "multi-clip graph activation was not visible");
  auto edited = take(model.apply({project::AddTrack{id<project::TrackId>(8)}}));
  REQUIRE_OK(control.submit(edited, feeds, "add-track"),
             "unrelated edit did not prepare a multi-clip graph");
  const auto* pending = graph.active_plan();
  REQUIRE_NE(pending, nullptr, "active graph vanished");
  require(graph.render_block(output, {30, 1}), "edited graph did not render");
  REQUIRE_EQ(graph.active_plan()->transitions().size(), 3,
             "unrelated edit replaced a live clip runtime");
  REQUIRE_TRUE(std::ranges::all_of(graph.active_plan()->transitions(),
                                   [](const auto& transition) {
                                     return transition.kind == engine::TransitionKind::reused;
                                   }),
               "unrelated edit replaced a live clip runtime");
  control.observe();
  for (auto& ring : rings) {
    push(*ring, 2, 2, 4);
  }
  require(graph.render_block(output, {11, 2}), "seek multi-clip render failed");
  near(output[0], 0.5F, -0.25F);
  near(output[1], 0.7F, -0.35F);
  require(std::ranges::all_of(rings, [](const auto& ring) { return ring->underrun_count() == 0; }),
          "seek generated a false clip underrun");
}

void direct_graph_rejects_saved_routes_and_plugin_inserts() {
  Gate gate;
  auto routed_model = make_model(gate);
  const auto audio = id<project::TrackId>(60);
  const auto bus = id<project::TrackId>(61);
  auto routed = take(routed_model.apply({project::AddTrack{audio},
                                         project::AddTrack{bus, project::TrackKind::group_bus},
                                         project::SetTrackMainOutput{audio, bus}}));
  engine::GraphPublisher routed_graph;
  app::GraphControl routed_control(routed_model, routed_graph, 64);
  auto route_failure = routed_control.submit_current(std::nullopt);
  REQUIRE_TRUE(!route_failure, "serial live graph silently accepted saved routing");
  REQUIRE_EQ(route_failure.error().code, foundation::ErrorCode::unsupported,
             "serial live graph silently accepted saved routing");
  REQUIRE_EQ(routed_control.status().application, app::PlaybackApplication::failed,
             "serial live graph silently accepted saved routing");
  REQUIRE_EQ(routed_control.status().model_revision, routed.snapshot->revision,
             "serial live graph silently accepted saved routing");
  REQUIRE_EQ(routed_graph.active_revision(), 0,
             "serial live graph silently accepted saved routing");

  auto plugin_model = make_model(gate);
  const auto track = id<project::TrackId>(62);
  const project::Vst3PluginInstance placeholder{id<project::PluginInstanceId>(63),
                                                std::string(32, 'a'),
                                                "Fixture Vendor",
                                                "Gain",
                                                "1.0",
                                                "/tmp/Gain.vst3",
                                                std::string(64, 'b'),
                                                {},
                                                {},
                                                true};
  auto inserted = take(plugin_model.apply(
      {project::AddTrack{track}, project::InsertVst3Plugin{track, 0, placeholder}}));
  engine::GraphPublisher plugin_graph;
  app::GraphControl plugin_control(plugin_model, plugin_graph, 64);
  auto plugin_failure = plugin_control.submit_current(std::nullopt);
  REQUIRE_TRUE(!plugin_failure, "serial live graph silently accepted a saved VST3 insert");
  REQUIRE_EQ(plugin_failure.error().code, foundation::ErrorCode::unsupported,
             "serial live graph silently accepted a saved VST3 insert");
  REQUIRE_EQ(plugin_control.status().application, app::PlaybackApplication::failed,
             "serial live graph silently accepted a saved VST3 insert");
  REQUIRE_EQ(plugin_control.status().model_revision, inserted.snapshot->revision,
             "serial live graph silently accepted a saved VST3 insert");
  REQUIRE_EQ(plugin_graph.active_revision(), 0,
             "serial live graph silently accepted a saved VST3 insert");

  auto synth_model = make_model(gate);
  const auto instrument = id<project::TrackId>(63);
  auto synth =
      take(synth_model.apply({project::AddTrack{instrument, project::TrackKind::instrument},
                              project::SetNativeInstrumentPatch{
                                  instrument, project::NativeInstrumentPatch{"prism", 1, {}}}}));
  engine::GraphPublisher synth_graph;
  app::GraphControl synth_control(synth_model, synth_graph, 64);
  auto synth_failure = synth_control.submit_current(std::nullopt);
  REQUIRE_TRUE(!synth_failure, "direct live graph silently accepted a built-in synth");
  REQUIRE_EQ(synth_failure.error().code, foundation::ErrorCode::unsupported,
             "direct live graph silently accepted a built-in synth");
  REQUIRE_NE(synth_failure.error().message.find("instruments"), std::string::npos,
             "direct live graph silently accepted a built-in synth");
  REQUIRE_EQ(synth_control.status().application, app::PlaybackApplication::failed,
             "direct live graph silently accepted a built-in synth");
  REQUIRE_EQ(synth_control.status().model_revision, synth.snapshot->revision,
             "direct live graph silently accepted a built-in synth");
  REQUIRE_EQ(synth_graph.active_revision(), 0,
             "direct live graph silently accepted a built-in synth");
}

// The direct graph has no master effects: a saved one is refused so the
// bridge renders the project on the routed graph instead.
project::Operation master_effect() {
  return project::AddNativeEffect{
      project::MasterSignalPath{}, 0,
      take(oscillate::native_inserts::native_effect_patch_for_preset(
          *oscillate::native_dsp::effects::find_engine("ceiling"), std::nullopt))};
}

void direct_graph_rejects_saved_master_effects() {
  Gate gate;
  auto model = make_model(gate);
  auto applied = take(model.apply({master_effect()}));
  engine::GraphPublisher graph;
  app::GraphControl control(model, graph, 64);
  auto failed = control.submit_current(std::nullopt);
  REQUIRE_TRUE(!failed, "live graph silently omitted a saved master effect");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::unsupported,
             "live graph silently omitted a saved master effect");
  REQUIRE_NE(failed.error().message.find("master effects"), std::string::npos,
             "live graph silently omitted a saved master effect");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "live graph silently omitted a saved master effect");
  REQUIRE_EQ(control.status().model_revision, applied.snapshot->revision,
             "live graph silently omitted a saved master effect");
  REQUIRE_EQ(graph.active_revision(), 0, "live graph silently omitted a saved master effect");

  auto active_model = make_model(gate);
  engine::GraphPublisher active_graph;
  app::GraphControl active_control(active_model, active_graph, 64);
  REQUIRE_OK(active_control.submit_current(std::nullopt), "baseline live graph did not prepare");
  std::array<engine::StereoFrame, 1> block{};
  require(active_graph.render_block(block, {0, 1}), "baseline live graph did not activate");
  active_control.observe();
  const auto old_revision = active_graph.active_revision();
  auto changed = take(active_model.apply({master_effect()}));
  auto rejected = active_control.submit(changed, std::nullopt, "master effect");
  REQUIRE_TRUE(!rejected,
               "failed master effect edit masked model/graph divergence or stopped old audio");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unsupported,
             "failed master effect edit masked model/graph divergence or stopped old audio");
  REQUIRE_EQ(active_control.status().model_revision, changed.snapshot->revision,
             "failed master effect edit masked model/graph divergence or stopped old audio");
  REQUIRE_EQ(active_control.status().active_graph_revision, old_revision,
             "failed master effect edit masked model/graph divergence or stopped old audio");
  REQUIRE_EQ(active_control.status().application, app::PlaybackApplication::failed,
             "failed master effect edit masked model/graph divergence or stopped old audio");
  REQUIRE_TRUE(active_graph.render_block(block, {1, 1}),
               "failed master effect edit masked model/graph divergence or stopped old audio");
}

// An instrument track without a source is silent; the direct graph still
// plays the project's audio.
void empty_instrument_track_is_silent() {
  Gate gate;
  auto model = make_model(gate);
  const auto track = id<project::TrackId>(70);
  auto added = take(model.apply({project::AddTrack{track, project::TrackKind::instrument}}));
  engine::GraphPublisher graph;
  app::GraphControl control(model, graph, 64);
  REQUIRE_OK(control.submit_current(std::nullopt),
             "an empty instrument track blocked the direct graph");
  std::array<engine::StereoFrame, 1> block{};
  require(graph.render_block(block, {0, 1}), "an empty instrument track's graph did not render");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, added.snapshot->revision,
             "an empty instrument track's revision did not activate");
}

} // namespace

int main() {
  try {
    bootstrap_retry_failure_and_undo();
    superseded_result_is_ignored();
    preparation_failures_and_supersession();
    multiple_audio_clips_mix_and_publish_atomically();
    direct_graph_rejects_saved_routes_and_plugin_inserts();
    direct_graph_rejects_saved_master_effects();
    empty_instrument_track_is_silent();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
