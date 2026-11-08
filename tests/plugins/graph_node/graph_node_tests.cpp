#include "oscillate/engine/routed_publisher.hpp"
#include "oscillate/plugins/vst3_graph_node.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using namespace oscillate;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {

using oscillate::test::require;

timeline::TempoMap tempo(double bpm = 120.0) {
  auto created = timeline::TempoMap::create({{{0}, bpm, timeline::TempoShape::step}}, 1);
  REQUIRE_OK(created, "fixture tempo map failed");
  return std::move(created).value();
}

project::Vst3PluginInstance saved(const plugins::ScanResult& scan, float gain);

void effect_timing_comes_from_project_map(const plugins::ScanResult& scan) {
  auto node =
      plugins::prepare_vst3_graph_node(saved(scan, -97.0F), scan, 2, tempo(96.0), 48'000.0, 128);
  require(node.ready(), "timed effect fixture failed to prepare");
  auto runtime = node.request.create_runtime();
  REQUIRE_NE(runtime, nullptr, "timed effect runtime was not created");
  std::array<engine::StereoFrame, 64> frames{};
  frames.fill({1.0F, 1.0F});
  runtime->process(frames, {48'000, 1});
  // At 96 BPM, one elapsed second is 1.6 quarters: 0.96 + 0.16.
  require(std::fabs(frames[7].left - 1.12F) < 0.00001F &&
              std::fabs(frames[7].right - 1.12F) < 0.00001F,
          "effect plugin did not receive project tempo and quarter position");
  auto changed_bpm =
      plugins::prepare_vst3_graph_node(saved(scan, -97.0F), scan, 2, tempo(120.0), 48'000.0, 128);
  REQUIRE_TRUE(changed_bpm.ready(), "tempo change reused an effect runtime with stale timing");
  REQUIRE_NE(changed_bpm.request.compatibility_key, node.request.compatibility_key,
             "tempo change reused an effect runtime with stale timing");
  auto offline =
      plugins::prepare_vst3_graph_node(saved(scan, -97.0F), scan, 2, tempo(96.0), 48'000.0, 128,
                                       plugins::Vst3ProcessingMode::offline);
  REQUIRE_TRUE(offline.ready(), "offline mode reused a real-time effect runtime");
  REQUIRE_NE(offline.request.compatibility_key, node.request.compatibility_key,
             "offline mode reused a real-time effect runtime");
  auto changing = timeline::TempoMap::create(
      {{{0}, 96.0, timeline::TempoShape::step}, {{1'024'000}, 120.0, timeline::TempoShape::step}},
      2);
  REQUIRE_OK(changing, "changing tempo fixture is invalid");
  for (const std::uint32_t block_size : {64U, 128U}) {
    auto stepped = plugins::prepare_vst3_graph_node(saved(scan, -97.0F), scan, 2, changing.value(),
                                                    48'000.0, 128);
    if (!stepped.ready())
      throw std::runtime_error("stepped tempo effect did not prepare: " + stepped.diagnostic);
    auto stepped_runtime = stepped.request.create_runtime();
    std::vector<engine::StereoFrame> output(block_size, {1.0F, 1.0F});
    counted_allocations = 0;
    counting_allocations = true;
    stepped_runtime->process(output, {0, 1});
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "stepped tempo effect callback failed or allocated");
    REQUIRE_EQ(stepped.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
               "stepped tempo effect callback failed or allocated");
    REQUIRE_LT(std::fabs(output[38].left - 0.96F), 0.00001F,
               "effect did not switch BPM and quarter position at the sample-32 boundary");
    REQUIRE_LT(std::fabs(output[39].left - (1.2F + (32.0F / 30'000.0F) / 10.0F)), 0.00001F,
               "effect did not switch BPM and quarter position at the sample-32 boundary");
  }
  auto gradual = timeline::TempoMap::create(
      {{{0}, 96.0, timeline::TempoShape::linear_bpm},
       {{timeline::ticks_per_quarter}, 120.0, timeline::TempoShape::step}},
      3);
  REQUIRE_OK(gradual, "gradual tempo fixture is invalid");
  auto ramp = plugins::prepare_vst3_graph_node(saved(scan, -97.0F), scan, 2, gradual.value(),
                                               48'000.0, 128);
  require(ramp.ready(), "effect host rejected a prepared gradual tempo");
  auto ramp_runtime = ramp.request.create_runtime();
  frames.fill({1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  ramp_runtime->process(frames, {24'000, 1});
  counting_allocations = false;
  const auto ratio = std::exp(0.2);
  const auto expected_bpm = 96.0 * ratio;
  const auto expected_quarter = 4.0 * (ratio - 1.0);
  REQUIRE_EQ(counted_allocations, 0,
             "effect host did not forward ramp BPM and musical position at block start");
  REQUIRE_EQ(ramp.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "effect host did not forward ramp BPM and musical position at block start");
  REQUIRE_LT(std::fabs(frames[7].left -
                       static_cast<float>(expected_bpm / 100.0 + expected_quarter / 10.0)),
             0.00001F, "effect host did not forward ramp BPM and musical position at block start");
  auto rounded = timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::step}, {{1}, 90.0, timeline::TempoShape::step}}, 4);
  REQUIRE_OK(rounded, "same-sample tempo fixture is invalid");
  auto last_wins = plugins::prepare_vst3_graph_node(saved(scan, -97.0F), scan, 2, rounded.value(),
                                                    48'000.0, 128);
  require(last_wins.ready(), "same-sample tempo events did not prepare");
  auto rounded_runtime = last_wins.request.create_runtime();
  frames.fill({1.0F, 1.0F});
  rounded_runtime->process(frames, {0, 1});
  REQUIRE_EQ(last_wins.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "same-sample tempo events did not use the final BPM");
  REQUIRE_LT(std::fabs(frames[7].left - 0.9F), 0.00001F,
             "same-sample tempo events did not use the final BPM");
}

class ConstantSource final : public engine::ProcessorRuntime {
public:
  ConstantSource(float left = 1.0F, float right = -1.0F) : left_(left), right_(right) {}
  void process(std::span<engine::StereoFrame> frames, engine::BlockContext) noexcept override {
    for (auto& frame : frames)
      frame = {left_, right_};
  }

private:
  float left_;
  float right_;
};

project::Vst3PluginInstance saved(const plugins::ScanResult& scan, float gain) {
  const auto id = project::PluginInstanceId::from_bits({0, 1});
  REQUIRE_OK(id, "fixture ID failed");
  const auto& klass = scan.metadata.classes.front();
  project::Vst3PluginInstance result{.id = id.value(),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = "/unused/advisory.vst3",
                                     .binary_sha256 = scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  result.component_state.resize(sizeof(gain));
  std::memcpy(result.component_state.data(), &gain, sizeof(gain));
  return result;
}

engine::NodeRequest source(float left = 1.0F, float right = -1.0F) {
  return {.instance_id = 1, .compatibility_key = 1, .create_runtime = [left, right] {
            return std::make_unique<ConstantSource>(left, right);
          }};
}

std::unique_ptr<engine::RenderPlan> plan(std::uint64_t revision,
                                         const plugins::PreparedVst3GraphNode& node,
                                         const engine::RenderPlan* prior) {
  std::array<engine::NodeRequest, 2> requests{source(), node.request};
  auto built = engine::prepare_graph(revision, 128, requests, prior);
  REQUIRE_OK(built, "graph build failed");
  return std::move(built.value());
}

void state_survives_publication(const plugins::ScanResult& scan) {
  auto saved_plugin = saved(scan, -0.5F); // Seven-sample history fixture.
  saved_plugin.parameters = {{42, 0.5}};
  auto node = plugins::prepare_vst3_graph_node(saved_plugin, scan, 2, tempo(), 48000.0, 128);
  require(node.ready(), "verified plugin did not prepare");
  REQUIRE_EQ(node.latency_samples, 7, "plugin latency was not exposed to graph composition");
  REQUIRE_EQ(node.unresolved_parameter_ids, std::vector<std::uint32_t>{42},
             "unrestored parameter reference was not disclosed");
  REQUIRE_TRUE(!node.diagnostic.empty(), "unrestored parameter reference was not disclosed");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(plan(1, node, nullptr)), engine::PublishStatus::accepted,
             "initial graph was rejected");
  std::array<engine::StereoFrame, 64> frames{};
  require(graph.render_block(frames, {0, 1}), "initial graph did not render");
  REQUIRE_EQ(frames.front().left, 0.0F, "stateful fixture did not render its expected delay");
  REQUIRE_EQ(frames[6].left, 0.0F, "stateful fixture did not render its expected delay");
  REQUIRE_EQ(frames[7].left, 0.5F, "stateful fixture did not render its expected delay");
  REQUIRE_EQ(frames.back().right, -0.5F, "stereo fixture output was wrong");
  auto next = plan(2, node, graph.active_plan());
  REQUIRE_EQ(next->transitions().size(), 2, "compatible plugin runtime was replaced");
  REQUIRE_EQ(next->transitions()[1].kind, engine::TransitionKind::reused,
             "compatible plugin runtime was replaced");
  REQUIRE_TRUE(!next->transitions()[1].audible_continuity_limited,
               "compatible plugin runtime was replaced");
  REQUIRE_EQ(graph.publish(std::move(next)), engine::PublishStatus::accepted,
             "revised graph was rejected");
  counting_allocations = true;
  const bool rendered = graph.render_block(frames, {64, 1});
  counting_allocations = false;
  require(rendered, "revised graph did not render");
  REQUIRE_EQ(counted_allocations, 0, "graph/plugin callback allocated");
  REQUIRE_EQ(frames.front().left, 0.5F, "plugin history reset at graph publication");
  REQUIRE_EQ(frames.front().right, -0.5F, "plugin history reset at graph publication");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "successful callback was reported failed");
  REQUIRE_EQ(graph.collect_retired().reclaimed, 1, "old graph was not retired on control side");

  auto missing = scan;
  missing.status = plugins::ScanStatus::quarantined;
  missing.diagnostic = "fixture quarantined";
  auto unavailable =
      plugins::prepare_vst3_graph_node(saved_plugin, missing, 2, tempo(), 48000.0, 128);
  REQUIRE_TRUE(!unavailable.ready(), "missing or quarantined plugin produced a render node");
  REQUIRE_TRUE(!unavailable.diagnostic.empty(),
               "missing or quarantined plugin produced a render node");
  REQUIRE_EQ(unavailable.request.instance_id, 0,
             "missing or quarantined plugin produced a render node");
  REQUIRE_EQ(unavailable.latency_samples, 0,
             "missing or quarantined plugin produced a render node");
  REQUIRE_EQ(graph.active_revision(), 2, "failed bind changed active graph");
  REQUIRE_TRUE(graph.render_block(frames, {128, 1}), "failed bind disturbed valid playback");
  REQUIRE_EQ(frames.front().left, 0.5F, "failed bind disturbed valid playback");

  auto replacement =
      plugins::prepare_vst3_graph_node(saved(scan, 0.25F), scan, 2, tempo(), 48000.0, 128);
  require(replacement.ready(), "replacement failed preparation");
  auto replaced_plan = plan(3, replacement, graph.active_plan());
  REQUIRE_EQ(replaced_plan->transitions()[1].kind, engine::TransitionKind::replaced,
             "state change was incorrectly treated as history-preserving");
  REQUIRE_TRUE(replaced_plan->transitions()[1].audible_continuity_limited,
               "state change was incorrectly treated as history-preserving");
  REQUIRE_EQ(graph.publish(std::move(replaced_plan)), engine::PublishStatus::accepted,
             "replacement graph was rejected");
  require(graph.render_block(frames, {192, 1}), "replacement graph did not render");
  REQUIRE_GT(frames.front().left, 0.25F, "replacement did not use the bounded fade");
  REQUIRE_LT(frames.front().left, 0.5F, "replacement did not use the bounded fade");
  REQUIRE_EQ(frames.back().left, 0.25F, "replacement did not use the bounded fade");
  REQUIRE_TRUE(graph.render_block(frames, {256, 1}), "replacement plugin state was not restored");
  REQUIRE_EQ(frames.front().left, 0.25F, "replacement plugin state was not restored");
}

void process_failure_mutes_and_reports(const plugins::ScanResult& scan) {
  auto node = plugins::prepare_vst3_graph_node(saved(scan, 999.0F), scan, 2, tempo(), 48000.0, 128);
  require(node.ready(), "failure fixture did not prepare");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(plan(1, node, nullptr)), engine::PublishStatus::accepted,
             "failure fixture graph was rejected");
  std::array<engine::StereoFrame, 64> frames{};
  require(graph.render_block(frames, {0, 1}), "failure fixture graph could not render");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::process_failed,
             "plugin processing failure was not reported");
  for (const auto& frame : frames)
    require(frame.left == 0.0F && frame.right == 0.0F, "failed plugin emitted partial output");
  require(graph.render_block(frames, {64, 1}), "failed path stopped the graph");
  for (const auto& frame : frames)
    require(frame.left == 0.0F && frame.right == 0.0F, "failed path did not stay muted");
}

void routed_sidechain_matches_independent_bus_oracle(const plugins::ScanResult& sidechain_scan,
                                                     const plugins::ScanResult& no_aux_scan) {
  const auto saved_effect = saved(sidechain_scan, 1.0F);
  auto node = plugins::prepare_vst3_graph_node(saved_effect, sidechain_scan, 2, tempo(), 48'000.0,
                                               128, plugins::Vst3ProcessingMode::realtime, {}, 0,
                                               false, {}, true);
  REQUIRE_TRUE(node.ready(), "sidechain graph node did not prove its activated auxiliary input");
  REQUIRE_TRUE(node.routed_sidechain,
               "sidechain graph node did not prove its activated auxiliary input");
  auto runtime = node.request.create_runtime();
  auto* target = dynamic_cast<engine::SidechainProcessorRuntime*>(runtime.get());
  REQUIRE_NE(target, nullptr, "VST3 graph runtime did not expose the detector entry point");
  for (const std::size_t frames : {64U, 128U}) {
    std::array<engine::StereoFrame, 128> main{}, detector{};
    main.fill({0.25F, -0.5F});
    detector.fill({0.125F, -0.25F});
    counted_allocations = 0;
    counting_allocations = true;
    target->process_with_sidechain(std::span(main).first(frames), std::span(detector).first(frames),
                                   {0, 1});
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0,
               "sidechain graph output did not sum exact main and detector samples");
    REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
               "sidechain graph output did not sum exact main and detector samples");
    REQUIRE_TRUE(std::ranges::all_of(std::span(main).first(frames),
                                     [](const auto& sample) {
                                       return sample.left == 0.375F && sample.right == -0.75F;
                                     }),
                 "sidechain graph output did not sum exact main and detector samples");
  }
  auto unconnected =
      plugins::prepare_vst3_graph_node(saved_effect, sidechain_scan, 3, tempo(), 48'000.0, 128);
  REQUIRE_TRUE(unconnected.ready(),
               "sidechain topology reused an unconnected VST3 processor instance");
  REQUIRE_TRUE(!unconnected.routed_sidechain,
               "sidechain topology reused an unconnected VST3 processor instance");
  REQUIRE_NE(unconnected.request.compatibility_key, node.request.compatibility_key,
             "sidechain topology reused an unconnected VST3 processor instance");
  auto incompatible = plugins::prepare_vst3_graph_node(
      saved(no_aux_scan, 1.0F), no_aux_scan, 4, tempo(), 48'000.0, 128,
      plugins::Vst3ProcessingMode::realtime, {}, 0, false, {}, true);
  REQUIRE_TRUE(!incompatible.ready(),
               "missing VST3 auxiliary input was not refused before graph publication");
  REQUIRE_EQ(incompatible.binding_status, plugins::Vst3BindingStatus::load_failed,
             "missing VST3 auxiliary input was not refused before graph publication");
  REQUIRE_NE(incompatible.diagnostic.find("exactly one stereo auxiliary input"), std::string::npos,
             "missing VST3 auxiliary input was not refused before graph publication");
  REQUIRE_TRUE(!incompatible.request.create_runtime,
               "missing VST3 auxiliary input was not refused before graph publication");
  auto existing = plugins::prepare_vst3_graph_node(saved(no_aux_scan, 0.5F), no_aux_scan, 2,
                                                   tempo(), 48'000.0, 128);
  require(existing.ready(), "prior non-sidechain VST3 graph did not prepare");
  engine::GraphPublisher publisher;
  REQUIRE_EQ(publisher.publish(plan(1, existing, nullptr)), engine::PublishStatus::accepted,
             "prior VST3 graph was not published");
  std::array<engine::StereoFrame, 64> audio{};
  REQUIRE_TRUE(publisher.render_block(audio, {0, 1}),
               "prior VST3 graph did not render before failed detector preparation");
  REQUIRE_EQ(publisher.active_revision(), 1,
             "prior VST3 graph did not render before failed detector preparation");
  REQUIRE_EQ(audio.front().left, 0.5F,
             "prior VST3 graph did not render before failed detector preparation");
  REQUIRE_TRUE(!incompatible.ready(),
               "failed detector preparation disturbed the prior active VST3 graph");
  REQUIRE_EQ(publisher.active_revision(), 1,
             "failed detector preparation disturbed the prior active VST3 graph");
  REQUIRE_TRUE(publisher.render_block(audio, {64, 1}),
               "failed detector preparation disturbed the prior active VST3 graph");
  REQUIRE_EQ(audio.front().left, 0.5F,
             "failed detector preparation disturbed the prior active VST3 graph");
}

void invalid_runtime_contracts(const plugins::ScanResult& scan) {
  const auto instance = saved(scan, 0.5F);
  auto invalid_id = plugins::prepare_vst3_graph_node(instance, scan, 0, tempo(), 48000.0, 128);
  REQUIRE_TRUE(!invalid_id.ready(), "zero engine ID was accepted");
  REQUIRE_TRUE(!invalid_id.diagnostic.empty(), "zero engine ID was accepted");

  auto node = plugins::prepare_vst3_graph_node(instance, scan, 2, tempo(), 48000.0, 128);
  require(node.ready(), "runtime guard fixture did not prepare");
  auto runtime = node.request.create_runtime();
  REQUIRE_NE(runtime, nullptr, "prepared plugin factory was not single-use");
  REQUIRE_EQ(node.request.create_runtime(), nullptr, "prepared plugin factory was not single-use");
  std::array<engine::StereoFrame, 129> oversized{};
  oversized.fill({1.0F, 1.0F});
  runtime->process(oversized, {0, 1});
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::process_failed,
             "oversized callback was not bounded and muted");
  REQUIRE_EQ(oversized.front().left, 0.0F, "oversized callback was not bounded and muted");
  REQUIRE_EQ(oversized.back().right, 0.0F, "oversized callback was not bounded and muted");
}

void benchmark(const plugins::ScanResult& scan, std::size_t block_frames, bool ramp) {
  auto timing =
      ramp ? timeline::TempoMap::create(
                 {{{0}, 120.0, timeline::TempoShape::linear_bpm},
                  {{1000 * timeline::ticks_per_quarter}, 180.0, timeline::TempoShape::step}},
                 2)
           : timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  REQUIRE_OK(timing, "benchmark tempo map is invalid");
  auto node = plugins::prepare_vst3_graph_node(saved(scan, 0.5F), scan, 2,
                                               std::move(timing).value(), 48000.0, 128);
  require(node.ready(), "benchmark plugin did not prepare");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(plan(1, node, nullptr)), engine::PublishStatus::accepted,
             "benchmark graph did not publish");
  std::array<engine::StereoFrame, 128> storage{};
  auto frames = std::span(storage).first(block_frames);
  std::uint64_t position = 0;
  for (unsigned warmup = 0; warmup < 1000; ++warmup) {
    require(graph.render_block(frames, {position, 1}), "benchmark warmup failed");
    position += block_frames;
  }
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "benchmark plugin did not remain active after warmup");
  REQUIRE_EQ(frames.back().left, 0.5F, "benchmark plugin did not remain active after warmup");
  constexpr unsigned iterations = 50'000;
  counted_allocations = 0;
  counting_allocations = true;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    if (!graph.render_block(frames, {position, 1}))
      std::abort();
    position += block_frames;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "benchmark callback allocated");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "benchmark plugin stopped processing");
  REQUIRE_EQ(frames.back().left, 0.5F, "benchmark plugin stopped processing");
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / iterations;
  std::fprintf(stdout, "VST3 graph fixture %s %zu frames: %lld ns/block, %zu allocations\n",
               ramp ? "ramp" : "constant", block_frames, static_cast<long long>(nanoseconds),
               counted_allocations);
}

void automated_connected_effect(const plugins::ScanResult& scan) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  const auto project_id = project::ProjectId::from_bits({0, 50});
  const auto track_id = project::TrackId::from_bits({0, 51});
  const auto slot_id = project::InsertSlotId::from_bits({0, 52});
  const auto lane_id = project::AutomationLaneId::from_bits({0, 53});
  const auto first_id = project::AutomationPointId::from_bits({0, 54});
  const auto last_id = project::AutomationPointId::from_bits({0, 55});
  REQUIRE_TRUE(project_id, "automation fixture IDs failed");
  REQUIRE_TRUE(track_id, "automation fixture IDs failed");
  REQUIRE_TRUE(slot_id, "automation fixture IDs failed");
  REQUIRE_TRUE(lane_id, "automation fixture IDs failed");
  REQUIRE_TRUE(first_id, "automation fixture IDs failed");
  REQUIRE_TRUE(last_id, "automation fixture IDs failed");
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  REQUIRE_OK(meter, "automation fixture meter failed");
  project::Snapshot snapshot{project_id.value(), 1, 48'000, tempo(), meter.value(), {}};
  auto stepped_tempo = timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::step}, {{1'280'000}, 90.0, timeline::TempoShape::step}},
      2);
  REQUIRE_OK(stepped_tempo, "automation step-tempo fixture failed");
  snapshot.tempo = std::move(stepped_tempo).value();
  project::Track track{track_id.value(), {}, project::TrackKind::audio};
  track.effects.push_back({slot_id.value(), true, plugin});
  track.automation.push_back(
      {lane_id.value(),
       project::Vst3ParameterTarget{plugin.id, 101},
       {project::AutomationRepresentation::vst3_normalized, "gain", "", 0.0, 1.0, 0.25, false, 0},
       project::AutomationTimebase::elapsed,
       {{first_id.value(), 0, 0.25, project::AutomationShape::linear, {}},
        {last_id.value(), 63, 0.5, project::AutomationShape::hold, {}}},
       1});
  snapshot.tracks.push_back(std::move(track));
  auto prepared = automation_project::prepare_project_automation(snapshot, snapshot.revision);
  REQUIRE_TRUE(prepared, "VST3 automation did not prepare");
  REQUIRE_EQ(prepared.value().size(), 1, "VST3 automation did not prepare");
  auto node = plugins::prepare_vst3_graph_node(plugin, scan, 2, snapshot.tempo, 48'000.0, 128,
                                               plugins::Vst3ProcessingMode::realtime,
                                               std::move(prepared).value(), 1);
  require(node.ready(), "connected VST3 automation graph did not prepare");
  std::array<engine::NodeRequest, 2> requests{source(0.25F, -0.5F), node.request};
  auto built = engine::prepare_graph(1, 128, requests, nullptr);
  REQUIRE_OK(built, "connected automation graph did not prepare");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(std::move(built).value()), engine::PublishStatus::accepted,
             "connected automation graph did not publish");
  std::array<engine::StereoFrame, 64> frames{};
  frames.fill({0.25F, -0.5F});
  counted_allocations = 0;
  counting_allocations = true;
  require(graph.render_block(frames, {0, 1}), "connected automation graph did not render");
  counting_allocations = false;
  REQUIRE_TRUE(node.manual_events, "manual event or preview took precedence over saved automation");
  REQUIRE_EQ(node.manual_events->submit({.graph_revision = 1,
                                         .instance_id = 2,
                                         .compatibility_key = node.request.compatibility_key},
                                        2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::automated,
             "manual event or preview took precedence over saved automation");
  REQUIRE_EQ(node.manual_events->preview({.graph_revision = 1,
                                          .instance_id = 2,
                                          .compatibility_key = node.request.compatibility_key},
                                         {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::automated,
             "manual event or preview took precedence over saved automation");
  REQUIRE_EQ(counted_allocations, 0,
             "sample-offset VST3 automation did not reach first render block without allocation");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "sample-offset VST3 automation did not reach first render block without allocation");
  REQUIRE_LT(std::fabs(frames.front().left - 0.25F), 0.00001F,
             "sample-offset VST3 automation did not reach first render block without allocation");
  REQUIRE_LT(std::fabs(frames[31].left - (0.25F + 0.25F * 31.0F / 63.0F)), 0.00001F,
             "sample-offset VST3 automation did not reach first render block without allocation");
  REQUIRE_LT(std::fabs(frames[32].left - (0.25F + 0.25F * 32.0F / 63.0F)), 0.00001F,
             "sample-offset VST3 automation did not reach first render block without allocation");
  REQUIRE_LT(std::fabs(frames[33].left - (0.25F + 0.25F * 33.0F / 63.0F)), 0.00001F,
             "sample-offset VST3 automation did not reach first render block without allocation");
  REQUIRE_LT(std::fabs(frames.back().left - 0.5F), 0.00001F,
             "sample-offset VST3 automation did not reach first render block without allocation");
  REQUIRE_LT(std::fabs(frames.back().right + 1.0F), 0.00001F,
             "sample-offset VST3 automation did not reach first render block without allocation");
}

void controller_state_changes_compatibility_key(const plugins::ScanResult& scan) {
  auto plugin = saved(scan, 1.0F);
  auto original = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, 128);
  require(original.ready(), "controller-state baseline did not prepare");
  plugin.controller_state = std::vector<std::uint8_t>{42};
  auto changed = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, 128);
  REQUIRE_TRUE(changed.ready(), "controller-only state change reused an effect runtime");
  REQUIRE_NE(changed.request.compatibility_key, original.request.compatibility_key,
             "controller-only state change reused an effect runtime");
}

void preview_connected_effect(const plugins::ScanResult& scan, std::uint32_t frames_per_block) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  auto node = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, 128,
                                               plugins::Vst3ProcessingMode::realtime, {}, 0, true);
  REQUIRE_TRUE(node.ready(), "preview effect did not prepare");
  REQUIRE_TRUE(node.manual_events, "preview effect did not prepare");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(plan(1, node, nullptr)), engine::PublishStatus::accepted,
             "preview effect graph did not publish");
  std::vector<engine::StereoFrame> output(frames_per_block, {1.0F, 1.0F});
  require(graph.render_block(output, {0, 1}), "preview effect graph did not activate");
  const auto identity = node.manual_events->active_graph();
  auto& slot = *node.manual_events;
  const auto preview = [&](std::uint32_t id, double value) {
    return slot.preview(identity, {id, 0, value});
  };
  REQUIRE_EQ(preview(999, 0.5), plugins::Vst3ManualEventStatus::unavailable,
             "effect preview did not validate and coalesce drag values");
  REQUIRE_EQ(slot.preview({.graph_revision = identity.graph_revision,
                           .instance_id = identity.instance_id + 1,
                           .compatibility_key = identity.compatibility_key},
                          {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "effect preview did not validate and coalesce drag values");
  REQUIRE_EQ(preview(101, 0.25), plugins::Vst3ManualEventStatus::accepted,
             "effect preview did not validate and coalesce drag values");
  REQUIRE_EQ(preview(101, 0.5), plugins::Vst3ManualEventStatus::accepted,
             "effect preview did not validate and coalesce drag values");
  output.assign(frames_per_block, {1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  const bool preview_rendered = graph.render_block(output, {frames_per_block, 1});
  counting_allocations = false;
  REQUIRE_TRUE(preview_rendered,
               "effect drag preview was not audible without a saved receipt or allocated");
  REQUIRE_EQ(counted_allocations, 0,
             "effect drag preview was not audible without a saved receipt or allocated");
  REQUIRE_LT(std::fabs(output.front().left - 2.0F), 0.00001F,
             "effect drag preview was not audible without a saved receipt or allocated");
  REQUIRE_EQ(slot.receipt().phase, plugins::Vst3ManualEventPhase::empty,
             "effect drag preview was not audible without a saved receipt or allocated");
  REQUIRE_EQ(preview(101, 0.9), plugins::Vst3ManualEventStatus::accepted,
             "effect final gesture did not supersede pending preview");
  REQUIRE_EQ(slot.submit(identity, 2, {101, 0, 0.75}), plugins::Vst3ManualEventStatus::accepted,
             "effect final gesture did not supersede pending preview");
  output.assign(frames_per_block, {1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(output, {std::uint64_t{2} * frames_per_block, 1}),
               "effect final commit failed to dominate stale preview");
  REQUIRE_LT(std::fabs(output.front().left - 3.0F), 0.00001F,
             "effect final commit failed to dominate stale preview");
  REQUIRE_EQ(slot.receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "effect final commit failed to dominate stale preview");
  REQUIRE_EQ(slot.receipt().project_sample, 2 * frames_per_block,
             "effect final commit failed to dominate stale preview");
  REQUIRE_OK(slot.collect(), "effect final preview receipt did not collect");
  output.assign(frames_per_block, {1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(output, {std::uint64_t{3} * frames_per_block, 1}),
               "effect stale preview replayed after final commit");
  REQUIRE_LT(std::fabs(output.front().left - 3.0F), 0.00001F,
             "effect stale preview replayed after final commit");
  REQUIRE_EQ(preview(101, 0.6), plugins::Vst3ManualEventStatus::accepted,
             "effect old graph could not stage a preview for retirement test");
  auto replacement =
      plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(121.0), 48'000.0, 128,
                                       plugins::Vst3ProcessingMode::realtime, {}, 0, true);
  REQUIRE_TRUE(replacement.ready(), "effect replacement preview graph did not publish");
  REQUIRE_EQ(graph.publish(plan(2, replacement, graph.active_plan())),
             engine::PublishStatus::accepted, "effect replacement preview graph did not publish");
  output.assign(frames_per_block, {1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(output, {std::uint64_t{4} * frames_per_block, 1}),
               "retired effect graph still accepted a preview");
  REQUIRE_EQ(slot.active_graph().graph_revision, 0,
             "retired effect graph still accepted a preview");
  REQUIRE_EQ(preview(101, 0.8), plugins::Vst3ManualEventStatus::stale_instance,
             "retired effect graph still accepted a preview");
}

void queued_native_gesture_preserves_newer_preview(const plugins::ScanResult& scan,
                                                   std::uint32_t frames_per_block) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  auto node = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, 128,
                                               plugins::Vst3ProcessingMode::realtime, {}, 0, true);
  REQUIRE_TRUE(node.ready(), "ordered preview effect did not prepare");
  REQUIRE_TRUE(node.manual_events, "ordered preview effect did not prepare");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(plan(1, node, nullptr)), engine::PublishStatus::accepted,
             "ordered preview graph did not publish");
  std::vector<engine::StereoFrame> output(frames_per_block, {1.0F, 1.0F});
  require(graph.render_block(output, {0, 1}), "ordered preview graph did not activate");
  auto& slot = *node.manual_events;
  const auto identity = slot.active_graph();
  const auto submit = [&](std::uint64_t revision, double value, std::uint64_t sequence) {
    return slot.submit(identity, revision, {101, 0, value}, sequence);
  };
  const auto preview = [&](double value, std::uint64_t sequence) {
    return slot.preview(identity, {101, 0, value}, sequence);
  };
  REQUIRE_EQ(submit(2, 0.75, 1), plugins::Vst3ManualEventStatus::accepted,
             "new drag could not stage while older completion awaited a callback");
  REQUIRE_EQ(preview(0.9, 2), plugins::Vst3ManualEventStatus::accepted,
             "new drag could not stage while older completion awaited a callback");
  output.assign(frames_per_block, {1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  const auto first_rendered = graph.render_block(output, {frames_per_block, 1});
  counting_allocations = false;
  REQUIRE_TRUE(first_rendered, "first committed event did not render before newer preview");
  REQUIRE_EQ(counted_allocations, 0, "first committed event did not render before newer preview");
  REQUIRE_LT(std::fabs(output.front().left - 3.0F), 0.00001F,
             "first committed event did not render before newer preview");
  REQUIRE_OK(slot.collect(), "first committed event did not render before newer preview");

  REQUIRE_EQ(preview(0.25, 3), plugins::Vst3ManualEventStatus::accepted,
             "later held drag could not stage");
  output.assign(frames_per_block, {1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(output, {std::uint64_t{2} * frames_per_block, 1}),
               "queued older completion refused a previously heard held drag");
  REQUIRE_LT(std::fabs(output.front().left - 1.0F), 0.00001F,
             "queued older completion refused a previously heard held drag");
  REQUIRE_EQ(submit(3, 0.9, 2), plugins::Vst3ManualEventStatus::accepted,
             "queued older completion refused a previously heard held drag");
  output.assign(frames_per_block, {1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  const auto second_rendered = graph.render_block(output, {std::uint64_t{3} * frames_per_block, 1});
  counting_allocations = false;
  REQUIRE_TRUE(second_rendered, "queued committed event failed to render before held drag");
  REQUIRE_EQ(counted_allocations, 0, "queued committed event failed to render before held drag");
  REQUIRE_LT(std::fabs(output.front().left - 3.6F), 0.00001F,
             "queued committed event failed to render before held drag");
  REQUIRE_OK(slot.collect(), "queued committed event failed to render before held drag");
  output.assign(frames_per_block, {1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  const auto third_rendered = graph.render_block(output, {std::uint64_t{4} * frames_per_block, 1});
  counting_allocations = false;
  REQUIRE_TRUE(third_rendered, "newer held drag was erased by older completed gesture");
  REQUIRE_EQ(counted_allocations, 0, "newer held drag was erased by older completed gesture");
  REQUIRE_LT(std::fabs(output.front().left - 1.0F), 0.00001F,
             "newer held drag was erased by older completed gesture");

  REQUIRE_EQ(preview(0.4, 4), plugins::Vst3ManualEventStatus::accepted,
             "same-gesture completion failed to retire its transient preview");
  REQUIRE_EQ(submit(4, 0.5, 4), plugins::Vst3ManualEventStatus::accepted,
             "same-gesture completion failed to retire its transient preview");
  output.assign(frames_per_block, {1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(output, {std::uint64_t{5} * frames_per_block, 1}),
               "same-gesture committed value was not applied");
  REQUIRE_OK(slot.collect(), "same-gesture committed value was not applied");
  REQUIRE_LT(std::fabs(output.front().left - 2.0F), 0.00001F,
             "same-gesture committed value was not applied");
  output.assign(frames_per_block, {1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(output, {std::uint64_t{6} * frames_per_block, 1}),
               "same-gesture stale preview replayed after receipt");
  REQUIRE_LT(std::fabs(output.front().left - 2.0F), 0.00001F,
             "same-gesture stale preview replayed after receipt");
}

void preview_survives_exact_overlay_and_failed_older_receipt() {
  plugins::Vst3ManualEventSlot slot({.instance_id = 2, .compatibility_key = 7}, {101}, {}, true);
  slot.activate(1);
  REQUIRE_EQ(slot.submit({.graph_revision = 1, .instance_id = 2, .compatibility_key = 7}, 2,
                         {101, 0, 0.75}, 1),
             plugins::Vst3ManualEventStatus::accepted,
             "failed-receipt preview fixture did not stage");
  REQUIRE_EQ(slot.preview({.graph_revision = 1, .instance_id = 2, .compatibility_key = 7},
                          {101, 0, 0.25}, 2),
             plugins::Vst3ManualEventStatus::accepted,
             "failed-receipt preview fixture did not stage");
  REQUIRE_OK(slot.claim(), "failed-receipt preview fixture did not stage");
  slot.finish(false, -1);
  REQUIRE_OK(slot.collect(),
             "failed older receipt discarded a newer drag on the same live instance");
  REQUIRE_OK(slot.claim_preview(),
             "failed older receipt discarded a newer drag on the same live instance");
  REQUIRE_EQ(slot.preview({.graph_revision = 1, .instance_id = 2, .compatibility_key = 7},
                          {101, 0, 0.4}, 3),
             plugins::Vst3ManualEventStatus::accepted, "overlay preview fixture did not stage");
  slot.control_overlay_rebound(1, 2);
  const auto carried = slot.claim_preview();
  REQUIRE_TRUE(carried, "exact-host control overlay discarded a newer tagged preview");
  REQUIRE_EQ(carried->id, 101, "exact-host control overlay discarded a newer tagged preview");
  REQUIRE_LT(std::fabs(carried->normalized_value - 0.4), 0.00001,
             "exact-host control overlay discarded a newer tagged preview");
  REQUIRE_EQ(
      slot.preview({.graph_revision = 2, .instance_id = 2, .compatibility_key = 7}, {101, 0, 0.6}),
      plugins::Vst3ManualEventStatus::accepted, "untagged preview fixture did not stage");
  slot.control_overlay_rebound(2, 3);
  require(!slot.claim_preview(), "untagged preview crossed a graph overlay");
  REQUIRE_EQ(slot.preview({.graph_revision = 3, .instance_id = 2, .compatibility_key = 7},
                          {101, 0, 0.8}, 4),
             plugins::Vst3ManualEventStatus::accepted, "retirement preview fixture did not stage");
  slot.deactivate();
  require(!slot.claim_preview(), "retired plugin instance replayed a preview");

  plugins::Vst3ManualEventSlot interleaved({.instance_id = 3, .compatibility_key = 8}, {101, 202},
                                           {}, true);
  interleaved.activate(1);
  REQUIRE_EQ(interleaved.preview({.graph_revision = 1, .instance_id = 3, .compatibility_key = 8},
                                 {202, 0, 0.3}, 1),
             plugins::Vst3ManualEventStatus::accepted,
             "interleaved parameter fixture did not stage");
  REQUIRE_EQ(interleaved.submit({.graph_revision = 1, .instance_id = 3, .compatibility_key = 8}, 2,
                                {101, 0, 0.8}, 2),
             plugins::Vst3ManualEventStatus::accepted,
             "interleaved parameter fixture did not stage");
  REQUIRE_OK(interleaved.claim(), "interleaved parameter fixture did not stage");
  interleaved.finish(true, 64);
  const auto other_parameter = interleaved.claim_preview();
  REQUIRE_TRUE(other_parameter, "unrelated committed parameter erased another active drag");
  REQUIRE_EQ(other_parameter->id, 202, "unrelated committed parameter erased another active drag");
  REQUIRE_LT(std::fabs(other_parameter->normalized_value - 0.3), 0.00001,
             "unrelated committed parameter erased another active drag");
}

void size_scoped_manual_effect(const plugins::ScanResult& scan, std::uint32_t qualified_frames,
                               std::uint32_t actual_frames) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  auto node = plugins::prepare_vst3_graph_node(
      plugin, scan, 2, tempo(), 48'000.0, 128, plugins::Vst3ProcessingMode::realtime, {}, 0, true,
      {}, false, std::vector<plugins::Vst3QualifiedManualBlock>{{qualified_frames, {101}}});
  REQUIRE_TRUE(node.ready(), "size-scoped effect failed to prepare");
  REQUIRE_TRUE(node.manual_events, "size-scoped effect failed to prepare");
  auto runtime = node.request.create_runtime();
  runtime->graph_activated(1);
  auto& slot = *node.manual_events;
  const auto identity = slot.active_graph();
  std::vector<engine::StereoFrame> output(actual_frames, {1.0F, 1.0F});
  REQUIRE_EQ(slot.preview(identity, {101, 0, 0.5}), plugins::Vst3ManualEventStatus::accepted,
             "size-scoped preview failed control-side preflight");
  counted_allocations = 0;
  counting_allocations = true;
  runtime->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "size-scoped preview applied at an unqualified actual process size or allocated");
  REQUIRE_LT(std::fabs(output.front().left - (qualified_frames == actual_frames ? 2.0F : 1.0F)),
             0.00001F,
             "size-scoped preview applied at an unqualified actual process size or allocated");
  REQUIRE_EQ(slot.receipt().phase, plugins::Vst3ManualEventPhase::empty,
             "size-scoped preview applied at an unqualified actual process size or allocated");
  REQUIRE_EQ(slot.submit(identity, 2, {101, 0, 0.75}), plugins::Vst3ManualEventStatus::accepted,
             "size-scoped commit failed control-side preflight");
  output.assign(actual_frames, {1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  runtime->process(output, {actual_frames, 1});
  counting_allocations = false;
  const bool matched = qualified_frames == actual_frames;
  REQUIRE_EQ(counted_allocations, 0,
             "size-scoped commit changed audio or receipt at an unqualified process size");
  REQUIRE_LT(std::fabs(output.front().left - (matched ? 3.0F : 1.0F)), 0.00001F,
             "size-scoped commit changed audio or receipt at an unqualified process size");
  REQUIRE_EQ(
      slot.receipt().phase,
      (matched ? plugins::Vst3ManualEventPhase::applied : plugins::Vst3ManualEventPhase::failed),
      "size-scoped commit changed audio or receipt at an unqualified process size");
  REQUIRE_EQ(slot.receipt().revision, 2,
             "size-scoped commit changed audio or receipt at an unqualified process size");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "size-scoped commit changed audio or receipt at an unqualified process size");
  REQUIRE_OK(slot.collect(), "size-scoped committed receipt was not collectable");
}

void tempo_split_denies_unqualified_effect_segment(const plugins::ScanResult& scan) {
  auto changing = timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::step}, {{1'024'000}, 90.0, timeline::TempoShape::step}},
      2);
  REQUIRE_OK(changing, "size-scoped tempo fixture failed");
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  auto node = plugins::prepare_vst3_graph_node(
      plugin, scan, 2, changing.value(), 48'000.0, 128, plugins::Vst3ProcessingMode::realtime, {},
      0, true, {}, false, std::vector<plugins::Vst3QualifiedManualBlock>{{128, {101}}});
  require(node.ready(), "size-scoped tempo effect failed to prepare");
  auto runtime = node.request.create_runtime();
  runtime->graph_activated(1);
  auto& slot = *node.manual_events;
  const auto identity = slot.active_graph();
  REQUIRE_EQ(slot.submit(identity, 2, {101, 0, 0.75}), plugins::Vst3ManualEventStatus::accepted,
             "size-scoped tempo commit failed preflight");
  std::array<engine::StereoFrame, 128> output{};
  output.fill({1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  runtime->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "tempo split applied 128-frame-only control to a 32-frame host segment");
  REQUIRE_LT(std::fabs(output.front().left - 1.0F), 0.00001F,
             "tempo split applied 128-frame-only control to a 32-frame host segment");
  REQUIRE_LT(std::fabs(output.back().left - 1.0F), 0.00001F,
             "tempo split applied 128-frame-only control to a 32-frame host segment");
  REQUIRE_EQ(slot.receipt().phase, plugins::Vst3ManualEventPhase::failed,
             "tempo split applied 128-frame-only control to a 32-frame host segment");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "tempo split applied 128-frame-only control to a 32-frame host segment");
}

void manual_connected_effect(const plugins::ScanResult& scan) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  auto denied = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, 128);
  REQUIRE_TRUE(denied.ready(), "generic VST3 fixture did not prepare");
  REQUIRE_TRUE(denied.manual_events, "generic VST3 fixture did not prepare");
  engine::GraphPublisher denied_graph;
  REQUIRE_EQ(denied_graph.publish(plan(1, denied, nullptr)), engine::PublishStatus::accepted,
             "generic VST3 graph did not publish");
  std::array<engine::StereoFrame, 64> frames{};
  frames.fill({1.0F, 1.0F});
  require(denied_graph.render_block(frames, {0, 1}), "generic VST3 graph did not render");
  REQUIRE_EQ(denied.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "generic VST3 manual event or preview admitted without fixed-latency evidence");
  REQUIRE_EQ(denied.manual_events->submit({.graph_revision = 1,
                                           .instance_id = 2,
                                           .compatibility_key = denied.request.compatibility_key},
                                          2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::latency_unverified,
             "generic VST3 manual event or preview admitted without fixed-latency evidence");
  REQUIRE_EQ(denied.manual_events->preview({.graph_revision = 1,
                                            .instance_id = 2,
                                            .compatibility_key = denied.request.compatibility_key},
                                           {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::latency_unverified,
             "generic VST3 manual event or preview admitted without fixed-latency evidence");
  auto node = plugins::prepare_vst3_graph_node(
      plugin, scan, 2, tempo(), 48'000.0, 128, plugins::Vst3ProcessingMode::realtime, {}, 0,
      true); // ConnectedGain fixture declares a constant seven-sample latency.
  require(node.ready() && node.latency_samples == 7 && node.manual_events,
          "fixed-latency connected fixture failed to prepare");
  auto& mailbox = *node.manual_events;
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "unpublished runtime admitted an event");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(plan(1, node, nullptr)), engine::PublishStatus::accepted,
             "manual event graph did not publish");
  REQUIRE_EQ(mailbox.active_graph().graph_revision, 0,
             "prepared but unpublished graph admitted an event");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "prepared but unpublished graph admitted an event");
  frames.fill({1.0F, 1.0F});
  require(graph.render_block(frames, {0, 1}), "manual event graph did not render");
  const auto active = mailbox.active_graph();
  REQUIRE_EQ(active.graph_revision, graph.active_revision(),
             "published runtime did not expose exact graph and processor identity");
  REQUIRE_EQ(active.graph_revision, 1,
             "published runtime did not expose exact graph and processor identity");
  REQUIRE_EQ(active.instance_id, 2,
             "published runtime did not expose exact graph and processor identity");
  REQUIRE_EQ(active.compatibility_key, node.request.compatibility_key,
             "published runtime did not expose exact graph and processor identity");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "manual event fixture failed baseline render");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 3,
                             .compatibility_key = node.request.compatibility_key},
                            2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "manual event admission did not enforce instance, metadata and capacity");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key + 1},
                            2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "manual event admission did not enforce instance, metadata and capacity");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 2,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "manual event admission did not enforce instance, metadata and capacity");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            2, {999, 0, 0.75}),
             plugins::Vst3ManualEventStatus::unavailable,
             "manual event admission did not enforce instance, metadata and capacity");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            2, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::accepted,
             "manual event admission did not enforce instance, metadata and capacity");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            3, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::busy,
             "manual event admission did not enforce instance, metadata and capacity");
  frames.fill({1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  require(graph.render_block(frames, {64, 1}), "manual event graph failed second block");
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "manual event was not applied and acknowledged at the first audible block");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "manual event was not applied and acknowledged at the first audible block");
  REQUIRE_EQ(mailbox.receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "manual event was not applied and acknowledged at the first audible block");
  REQUIRE_EQ(mailbox.receipt().revision, 2,
             "manual event was not applied and acknowledged at the first audible block");
  REQUIRE_EQ(mailbox.receipt().project_sample, 64,
             "manual event was not applied and acknowledged at the first audible block");
  REQUIRE_LT(std::fabs(frames[0].left - 3.0F), 0.00001F,
             "manual event was not applied and acknowledged at the first audible block");
  const auto receipt = mailbox.collect();
  REQUIRE_TRUE(receipt, "manual receipt did not release the bounded slot");
  REQUIRE_EQ(receipt->revision, 2, "manual receipt did not release the bounded slot");
  REQUIRE_EQ(mailbox.receipt().phase, plugins::Vst3ManualEventPhase::empty,
             "manual receipt did not release the bounded slot");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            2, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::invalid_revision,
             "collected slot accepted an older saved revision");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            1, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::invalid_revision,
             "collected slot accepted an older saved revision");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            3, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::accepted,
             "second manual revision was not admitted after receipt collection");
  std::array<engine::StereoFrame, 128> larger{};
  larger.fill({1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  require(graph.render_block(larger, {128, 1}), "manual event graph failed larger block");
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "128-frame manual event missed the boundary or allocated");
  REQUIRE_EQ(mailbox.receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "128-frame manual event missed the boundary or allocated");
  REQUIRE_EQ(mailbox.receipt().revision, 3,
             "128-frame manual event missed the boundary or allocated");
  REQUIRE_EQ(mailbox.receipt().project_sample, 128,
             "128-frame manual event missed the boundary or allocated");
  REQUIRE_LT(std::fabs(larger[0].left - 2.0F), 0.00001F,
             "128-frame manual event missed the boundary or allocated");
  REQUIRE_OK(mailbox.collect(), "second manual receipt was unavailable");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 2,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            4, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "wrong graph revision admitted a manual event");

  // A held plan has no authority before the callback publishes it. A reused
  // runtime changes graph identity at the switch and drops an old-plan event.
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            4, {101, 0, 0.6}),
             plugins::Vst3ManualEventStatus::accepted,
             "held-switch fixture could not queue old graph event");
  auto reused = plan(2, node, graph.active_plan());
  REQUIRE_EQ(reused->transitions()[1].kind, engine::TransitionKind::reused,
             "compatible graph did not hold a reused runtime");
  REQUIRE_EQ(graph.publish(std::move(reused)), engine::PublishStatus::accepted,
             "compatible graph did not hold a reused runtime");
  REQUIRE_EQ(mailbox.active_graph().graph_revision, 1,
             "held graph admitted a future-revision event");
  REQUIRE_EQ(graph.active_revision(), 1, "held graph admitted a future-revision event");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 2,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            5, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "held graph admitted a future-revision event");
  frames.fill({1.0F, 1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  const bool switched = graph.render_block(frames, {256, 1});
  counting_allocations = false;
  REQUIRE_TRUE(switched, "reused runtime applied a stale event or lost its active identity");
  REQUIRE_EQ(counted_allocations, 0,
             "reused runtime applied a stale event or lost its active identity");
  REQUIRE_EQ(graph.active_revision(), 2,
             "reused runtime applied a stale event or lost its active identity");
  REQUIRE_EQ(mailbox.active_graph().graph_revision, 2,
             "reused runtime applied a stale event or lost its active identity");
  REQUIRE_EQ(mailbox.receipt().phase, plugins::Vst3ManualEventPhase::failed,
             "reused runtime applied a stale event or lost its active identity");
  REQUIRE_LT(std::fabs(frames.front().left - 2.0F), 0.00001F,
             "reused runtime applied a stale event or lost its active identity");
  REQUIRE_OK(mailbox.collect(), "stale held-switch event or retired graph was not collected");
  REQUIRE_EQ(graph.collect_retired().reclaimed, 1,
             "stale held-switch event or retired graph was not collected");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 1,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            5, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "reused runtime did not admit only its new active graph revision");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 2,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            5, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::accepted,
             "reused runtime did not admit only its new active graph revision");
  frames.fill({1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(frames, {320, 1}),
               "new-revision event did not apply on the reused runtime");
  REQUIRE_EQ(mailbox.receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "new-revision event did not apply on the reused runtime");
  REQUIRE_OK(mailbox.collect(), "new-revision receipt did not collect");

  auto replacement =
      plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(121.0), 48'000.0, 128,
                                       plugins::Vst3ProcessingMode::realtime, {}, 0, true);
  REQUIRE_TRUE(replacement.ready(), "replacement fixture did not change compatibility identity");
  REQUIRE_NE(replacement.request.compatibility_key, node.request.compatibility_key,
             "replacement fixture did not change compatibility identity");
  REQUIRE_EQ(graph.publish(plan(3, replacement, graph.active_plan())),
             engine::PublishStatus::accepted,
             "prepared replacement runtime became active before publication");
  REQUIRE_EQ(replacement.manual_events->active_graph().graph_revision, 0,
             "prepared replacement runtime became active before publication");
  frames.fill({1.0F, 1.0F});
  REQUIRE_TRUE(graph.render_block(frames, {384, 1}),
               "superseded runtime remained an admission authority while awaiting reclamation");
  REQUIRE_EQ(graph.active_revision(), 3,
             "superseded runtime remained an admission authority while awaiting reclamation");
  REQUIRE_EQ(mailbox.active_graph().graph_revision, 0,
             "superseded runtime remained an admission authority while awaiting reclamation");
  REQUIRE_EQ(replacement.manual_events->active_graph().graph_revision, 3,
             "superseded runtime remained an admission authority while awaiting reclamation");
  REQUIRE_EQ(mailbox.submit({.graph_revision = 2,
                             .instance_id = 2,
                             .compatibility_key = node.request.compatibility_key},
                            6, {101, 0, 0.8}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "superseded runtime remained an admission authority while awaiting reclamation");
  REQUIRE_EQ(graph.collect_retired().reclaimed, 1,
             "reclaiming old graph reactivated the superseded runtime");
  REQUIRE_EQ(mailbox.active_graph().graph_revision, 0,
             "reclaiming old graph reactivated the superseded runtime");
}

void routed_manual_graph_identity(const plugins::ScanResult& scan) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  auto node = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, 128,
                                               plugins::Vst3ProcessingMode::realtime, {}, 0, true);
  REQUIRE_TRUE(node.ready(), "routed VST3 fixture did not prepare");
  REQUIRE_TRUE(node.manual_events, "routed VST3 fixture did not prepare");
  auto owner = std::shared_ptr<engine::ProcessorRuntime>(node.request.create_runtime());
  REQUIRE_NE(owner, nullptr, "routed VST3 runtime did not construct");
  std::array<engine::RoutedNode, 2> nodes{{{{1, 0}, {}}, {{2, 7}, owner}}};
  const std::array<engine::RoutingEdge, 1> edges{{{11, 1, 2}}};
  const engine::RoutedMixerLimits limits{
      {2, 64}, 64, 3ULL * 64 * sizeof(engine::StereoFrame), 65ULL * sizeof(engine::StereoFrame)};
  engine::RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, limits),
             "routed prepared runtime claimed graph authority");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 0,
             "routed prepared runtime claimed graph authority");
  std::array<engine::StereoFrame, 64> input{}, output{};
  input.fill({1.0F, 1.0F});
  const std::array<std::span<const engine::StereoFrame>, 2> inputs{input, {}};
  REQUIRE_EQ(publisher.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "initial routed publication did not activate the VST3 slot");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 1,
             "initial routed publication did not activate the VST3 slot");
  REQUIRE_TRUE(publisher.request_hold(), "routed graph failed to hold the block boundary");
  REQUIRE_EQ(publisher.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::holding,
             "routed graph failed to hold the block boundary");
  REQUIRE_OK(publisher.prepare_and_publish(2, nodes, edges, limits),
             "held routed graph admitted an unpublished revision");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 1,
             "held routed graph admitted an unpublished revision");
  REQUIRE_EQ(node.manual_events->submit({.graph_revision = 2,
                                         .instance_id = 2,
                                         .compatibility_key = node.request.compatibility_key},
                                        2, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "held routed graph admitted an unpublished revision");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = publisher.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "routed graph switch did not update the reused VST3 slot without allocation");
  REQUIRE_EQ(counted_allocations, 0,
             "routed graph switch did not update the reused VST3 slot without allocation");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "routed graph switch did not update the reused VST3 slot without allocation");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 2,
             "routed graph switch did not update the reused VST3 slot without allocation");
  REQUIRE_EQ(publisher.collect_retired(), 1, "routed old graph did not retire");
  REQUIRE_EQ(node.manual_events->submit({.graph_revision = 2,
                                         .instance_id = 2,
                                         .compatibility_key = node.request.compatibility_key},
                                        2, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::accepted,
             "active routed VST3 runtime refused a verified event");
  counted_allocations = 0;
  counting_allocations = true;
  const auto applied = publisher.render_block(inputs, output, {128, 1});
  counting_allocations = false;
  const auto receipt = node.manual_events->receipt();
  REQUIRE_EQ(applied, engine::RoutedRenderStatus::rendered,
             "routed manual event did not apply and acknowledge at a block boundary");
  REQUIRE_EQ(counted_allocations, 0,
             "routed manual event did not apply and acknowledge at a block boundary");
  REQUIRE_EQ(receipt.phase, plugins::Vst3ManualEventPhase::applied,
             "routed manual event did not apply and acknowledge at a block boundary");
  REQUIRE_EQ(receipt.revision, 2,
             "routed manual event did not apply and acknowledge at a block boundary");
  REQUIRE_EQ(receipt.project_sample, 128,
             "routed manual event did not apply and acknowledge at a block boundary");
  REQUIRE_OK(node.manual_events->collect(),
             "routed manual event did not apply and acknowledge at a block boundary");

  auto replacement =
      plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(121.0), 48'000.0, 128,
                                       plugins::Vst3ProcessingMode::realtime, {}, 0, true);
  auto replacement_owner =
      std::shared_ptr<engine::ProcessorRuntime>(replacement.request.create_runtime());
  REQUIRE_TRUE(replacement.ready(), "routed replacement VST3 runtime did not prepare");
  REQUIRE_NE(replacement_owner, nullptr, "routed replacement VST3 runtime did not prepare");
  REQUIRE_TRUE(publisher.request_hold(), "routed replacement did not hold");
  REQUIRE_EQ(publisher.render_block(inputs, output, {192, 1}), engine::RoutedRenderStatus::holding,
             "routed replacement did not hold");
  nodes[1].processor = std::move(replacement_owner);
  nodes[1].replacement = engine::RoutedTailReplacement{owner, 1};
  REQUIRE_OK(publisher.prepare_and_publish(3, nodes, edges, limits),
             "routed replacement became active before callback switch");
  REQUIRE_EQ(replacement.manual_events->active_graph().graph_revision, 0,
             "routed replacement became active before callback switch");
  REQUIRE_EQ(publisher.render_block(inputs, output, {192, 1}), engine::RoutedRenderStatus::rendered,
             "routed retired VST3 runtime admitted an event while its tail drained");
  REQUIRE_EQ(publisher.active_revision(), 3,
             "routed retired VST3 runtime admitted an event while its tail drained");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 0,
             "routed retired VST3 runtime admitted an event while its tail drained");
  REQUIRE_EQ(replacement.manual_events->active_graph().graph_revision, 3,
             "routed retired VST3 runtime admitted an event while its tail drained");
  REQUIRE_EQ(node.manual_events->submit({.graph_revision = 2,
                                         .instance_id = 2,
                                         .compatibility_key = node.request.compatibility_key},
                                        3, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "routed retired VST3 runtime admitted an event while its tail drained");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "reclaiming routed graph revived the retired VST3 slot");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 0,
             "reclaiming routed graph revived the retired VST3 slot");
}

void routed_control_overlay_rebinds_manual_slot(const plugins::ScanResult& scan,
                                                std::size_t frames) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  plugin.missing = false;
  auto node = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, 128,
                                               plugins::Vst3ProcessingMode::realtime, {}, 0, true);
  REQUIRE_TRUE(node.ready(), "overlay VST3 fixture did not prepare");
  REQUIRE_TRUE(node.manual_events, "overlay VST3 fixture did not prepare");
  auto owner = std::shared_ptr<engine::ProcessorRuntime>(node.request.create_runtime());
  REQUIRE_NE(owner, nullptr, "overlay VST3 runtime did not construct");
  const std::array<engine::RoutedNode, 2> nodes{{{{1, 0}, owner}, {{2, 0}, {}}}};
  const std::array<engine::RoutingEdge, 1> edges{{{11, 1, 2}}};
  const engine::RoutedMixerLimits limits{
      {2, 64}, 128, 3ULL * 128 * sizeof(engine::StereoFrame), 129ULL * sizeof(engine::StereoFrame)};
  engine::RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, limits),
             "overlay VST3 route did not prepare");
  std::array<engine::StereoFrame, 128> input{}, output{};
  input.fill({1.0F, 1.0F});
  const std::array<std::span<const engine::StereoFrame>, 2> inputs{std::span(input).first(frames),
                                                                   {}};
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(publisher.render_block(inputs, block, {0, 1}), engine::RoutedRenderStatus::rendered,
             "overlay VST3 baseline did not activate its manual slot");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 1,
             "overlay VST3 baseline did not activate its manual slot");
  const std::array<engine::RoutedNodeControl, 2> controls{
      {{1, false, {1.0F, 1.0F}}, {2, false, {1.0F, 1.0F}}}};
  REQUIRE_EQ(node.manual_events->submit({.graph_revision = 1,
                                         .instance_id = 2,
                                         .compatibility_key = node.request.compatibility_key},
                                        2, {101, 0, 0.5}),
             plugins::Vst3ManualEventStatus::accepted,
             "control overlay crossed an unacknowledged VST3 manual event");
  REQUIRE_TRUE(!publisher.prepare_control_overlay(2, controls),
               "control overlay crossed an unacknowledged VST3 manual event");
  REQUIRE_TRUE(!publisher.has_pending(),
               "control overlay crossed an unacknowledged VST3 manual event");
  REQUIRE_EQ(publisher.render_block(inputs, block, {frames, 1}),
             engine::RoutedRenderStatus::rendered,
             "control overlay admitted an uncollected VST3 receipt");
  REQUIRE_EQ(node.manual_events->receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "control overlay admitted an uncollected VST3 receipt");
  REQUIRE_TRUE(!publisher.prepare_control_overlay(2, controls),
               "control overlay admitted an uncollected VST3 receipt");
  REQUIRE_OK(node.manual_events->collect(), "control overlay admitted an uncollected VST3 receipt");
  REQUIRE_EQ(node.manual_events->preview({.graph_revision = 1,
                                          .instance_id = 2,
                                          .compatibility_key = node.request.compatibility_key},
                                         {101, 0, 0.9}),
             plugins::Vst3ManualEventStatus::accepted,
             "quiescent VST3 manual slot could not stage a revision-only overlay");
  REQUIRE_OK(publisher.prepare_control_overlay(2, controls),
             "quiescent VST3 manual slot could not stage a revision-only overlay");
  REQUIRE_TRUE(!publisher.request_hold(),
               "quiescent VST3 manual slot could not stage a revision-only overlay");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = publisher.render_block(inputs, block, {2 * frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "VST3 control overlay lost host state, applied stale preview or silenced a block");
  REQUIRE_EQ(counted_allocations, 0,
             "VST3 control overlay lost host state, applied stale preview or silenced a block");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "VST3 control overlay lost host state, applied stale preview or silenced a block");
  REQUIRE_EQ(node.manual_events->active_graph().graph_revision, 2,
             "VST3 control overlay lost host state, applied stale preview or silenced a block");
  REQUIRE_LT(std::fabs(output.front().left - 2.0F), 0.00001F,
             "VST3 control overlay lost host state, applied stale preview or silenced a block");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "VST3 control overlay lost host state, applied stale preview or silenced a block");
  REQUIRE_EQ(node.manual_events->submit({.graph_revision = 1,
                                         .instance_id = 2,
                                         .compatibility_key = node.request.compatibility_key},
                                        3, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "VST3 overlay kept old manual authority or refused the new revision");
  REQUIRE_EQ(node.manual_events->submit({.graph_revision = 2,
                                         .instance_id = 2,
                                         .compatibility_key = node.request.compatibility_key},
                                        3, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::accepted,
             "VST3 overlay kept old manual authority or refused the new revision");
  counted_allocations = 0;
  counting_allocations = true;
  const auto applied = publisher.render_block(inputs, block, {3 * frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(applied, engine::RoutedRenderStatus::rendered,
             "rebound VST3 manual event missed the first block or changed timing");
  REQUIRE_EQ(counted_allocations, 0,
             "rebound VST3 manual event missed the first block or changed timing");
  REQUIRE_EQ(node.manual_events->receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "rebound VST3 manual event missed the first block or changed timing");
  REQUIRE_EQ(node.manual_events->receipt().revision, 3,
             "rebound VST3 manual event missed the first block or changed timing");
  REQUIRE_EQ(node.manual_events->receipt().project_sample, static_cast<std::int64_t>(3 * frames),
             "rebound VST3 manual event missed the first block or changed timing");
  REQUIRE_LT(std::fabs(output.front().left - 3.0F), 0.00001F,
             "rebound VST3 manual event missed the first block or changed timing");
}

constexpr std::int64_t lane_span = 1'000'000;

double lane_start(std::uint32_t parameter) { return static_cast<double>(parameter) / 256.0; }

template <typename Id> Id fixture_id(std::uint64_t bits) {
  const auto created = Id::from_bits({0, bits});
  REQUIRE_OK(created, "lane fixture ID failed");
  return created.value();
}

// One linear lane per parameter 2..lanes+1 that changes on every sample, so
// each lane uses its full per-frame share of the effect event budget.
std::vector<automation_project::PreparedTargetAutomation>
lane_automation(const project::Vst3PluginInstance& plugin, std::uint32_t lanes) {
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  REQUIRE_OK(meter, "lane fixture meter failed");
  project::Snapshot snapshot{
      fixture_id<project::ProjectId>(60), 1, 48'000, tempo(), meter.value(), {}};
  project::Track track{fixture_id<project::TrackId>(61), {}, project::TrackKind::audio};
  track.effects.push_back({fixture_id<project::InsertSlotId>(62), true, plugin});
  for (std::uint32_t parameter = 2; parameter < 2 + lanes; ++parameter) {
    track.automation.push_back(
        {fixture_id<project::AutomationLaneId>(100 + parameter),
         project::Vst3ParameterTarget{plugin.id, parameter},
         {project::AutomationRepresentation::vst3_normalized, "lane", "", 0.0, 1.0, 0.5, false, 0},
         project::AutomationTimebase::elapsed,
         {{fixture_id<project::AutomationPointId>(1'000 + 2 * parameter),
           0,
           lane_start(parameter),
           project::AutomationShape::linear,
           {}},
          {fixture_id<project::AutomationPointId>(1'001 + 2 * parameter),
           lane_span,
           lane_start(parameter) + 0.5,
           project::AutomationShape::hold,
           {}}},
         1});
  }
  snapshot.tracks.push_back(std::move(track));
  auto prepared = automation_project::prepare_project_automation(snapshot, snapshot.revision);
  REQUIRE_TRUE(prepared, "lane fixture automation did not prepare");
  REQUIRE_EQ(prepared.value().size(), lanes, "lane fixture automation did not prepare");
  return std::move(prepared).value();
}

struct LoggedPoint {
  std::uint32_t id{0};
  std::int32_t offset{0};
  double value{0.0};
};

struct FixtureLog {
  std::size_t process_calls{0};
  std::vector<LoggedPoint> points; // Every point of the latest process call.
};

FixtureLog fixture_log(const plugins::ScanResult& scan) {
  void* module = dlopen(scan.binary.canonical_binary.c_str(), RTLD_NOW | RTLD_NOLOAD);
  REQUIRE_NE(module, nullptr, "lane effect fixture module was not resident");
  using Calls = std::size_t (*)();
  using Point = bool (*)(std::size_t, std::uint32_t*, std::int32_t*, double*);
  auto calls = reinterpret_cast<Calls>(dlsym(module, "OscillateFixtureProcessCalls"));
  auto count = reinterpret_cast<Calls>(dlsym(module, "OscillateFixtureParameterPointCount"));
  auto point = reinterpret_cast<Point>(dlsym(module, "OscillateFixtureParameterPoint"));
  REQUIRE_NE(calls, nullptr, "lane effect point log is unavailable");
  REQUIRE_NE(count, nullptr, "lane effect point log is unavailable");
  REQUIRE_NE(point, nullptr, "lane effect point log is unavailable");
  FixtureLog log{calls(), std::vector<LoggedPoint>(count())};
  for (std::size_t index = 0; index < log.points.size(); ++index)
    require(
        point(index, &log.points[index].id, &log.points[index].offset, &log.points[index].value),
        "lane effect point log entry is unreadable");
  REQUIRE_EQ(dlclose(module), 0, "lane effect point log could not close");
  return log;
}

// Independent oracle: one process call per block; parameter queues arrive in
// ID order, so the manual gain (ID 1) precedes the lanes, and every lane has
// one point per frame at its exact offset with the closed-form ramp value.
void expect_lane_points(const FixtureLog& log, std::size_t prior_calls, std::uint32_t lanes,
                        std::uint32_t frames, std::int64_t position,
                        std::optional<plugins::Vst3ParameterEvent> manual) {
  REQUIRE_EQ(log.process_calls, prior_calls + 1, "effect block was not one plugin process call");
  const std::size_t manual_count = manual ? 1 : 0;
  REQUIRE_EQ(log.points.size(), static_cast<std::size_t>(lanes) * frames + manual_count,
             "effect did not receive every automation point within the event budget");
  if (manual)
    require(log.points[0].id == manual->id && log.points[0].offset == 0 &&
                log.points[0].value == manual->normalized_value,
            "manual effect event lost its sample-zero position beside full automation");
  for (std::uint32_t lane = 0; lane < lanes; ++lane) {
    const auto parameter = 2 + lane;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      const auto& point =
          log.points[manual_count + static_cast<std::size_t>(lane) * frames + frame];
      const double expected = lane_start(parameter) + 0.5 * static_cast<double>(position + frame) /
                                                          static_cast<double>(lane_span);
      REQUIRE_EQ(point.id, parameter,
                 "effect lane point has the wrong parameter, sample offset or value");
      REQUIRE_EQ(point.offset, static_cast<std::int32_t>(frame),
                 "effect lane point has the wrong parameter, sample offset or value");
      REQUIRE_LT(std::fabs(point.value - expected), tolerance::double_accumulation,
                 "effect lane point has the wrong parameter, sample offset or value");
    }
  }
}

void lane_effect_capacity(const plugins::ScanResult& scan, std::uint32_t frames,
                          std::uint32_t lanes) {
  using plugins::Vst3ManualEventStatus;
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.missing = false;
  auto node = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, frames,
                                               plugins::Vst3ProcessingMode::realtime,
                                               lane_automation(plugin, lanes), 7, true);
  if (!node.ready())
    throw std::runtime_error("multi-lane effect did not prepare: " + node.diagnostic);
  REQUIRE_NE(node.manual_events, nullptr, "multi-lane effect has no manual event slot");
  std::array<engine::NodeRequest, 2> requests{source(0.5F, -1.0F), node.request};
  auto built = engine::prepare_graph(1, frames, requests, nullptr);
  REQUIRE_OK(built, "multi-lane effect graph did not prepare");
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(std::move(built).value()), engine::PublishStatus::accepted,
             "multi-lane effect graph did not publish");
  std::vector<engine::StereoFrame> output(frames);
  const auto before_first = fixture_log(scan).process_calls;
  counted_allocations = 0;
  counting_allocations = true;
  const bool first = graph.render_block(output, {0, 1});
  counting_allocations = false;
  REQUIRE_TRUE(first, "multi-lane effect callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "multi-lane effect callback failed or allocated");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "multi-lane effect callback failed or allocated");
  expect_lane_points(fixture_log(scan), before_first, lanes, frames, 0, std::nullopt);
  for (const auto& frame : output)
    require(frame.left == 0.25F && frame.right == -0.5F,
            "multi-lane effect did not keep its default gain");

  auto& mailbox = *node.manual_events;
  const auto identity = mailbox.active_graph();
  REQUIRE_EQ(mailbox.submit(identity, 2, {2, 0, 0.75}), Vst3ManualEventStatus::automated,
             "manual effect event overrode an automated lane");
  const plugins::Vst3ParameterEvent manual{1, 0, 0.75};
  REQUIRE_EQ(mailbox.submit(identity, 2, manual), Vst3ManualEventStatus::accepted,
             "fully automated effect refused an eligible manual event");
  const auto before_second = fixture_log(scan).process_calls;
  counted_allocations = 0;
  counting_allocations = true;
  const bool second = graph.render_block(output, {frames, 1});
  counting_allocations = false;
  REQUIRE_TRUE(second, "multi-lane effect manual callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "multi-lane effect manual callback failed or allocated");
  REQUIRE_EQ(node.runtime_status->load(), plugins::Vst3GraphRuntimeStatus::ready,
             "multi-lane effect manual callback failed or allocated");
  expect_lane_points(fixture_log(scan), before_second, lanes, frames, frames, manual);
  for (const auto& frame : output)
    require(frame.left == 0.375F && frame.right == -0.75F,
            "manual effect gain was not audible from sample zero");
  const auto receipt = mailbox.collect();
  REQUIRE_TRUE(receipt, "manual event beside full effect automation lacked an applied receipt");
  REQUIRE_EQ(receipt->phase, plugins::Vst3ManualEventPhase::applied,
             "manual event beside full effect automation lacked an applied receipt");
  REQUIRE_EQ(receipt->revision, 2,
             "manual event beside full effect automation lacked an applied receipt");
  REQUIRE_EQ(receipt->project_sample, frames,
             "manual event beside full effect automation lacked an applied receipt");
}

void lane_effect_overflow(const plugins::ScanResult& scan, std::uint32_t frames) {
  auto plugin = saved(scan, 1.0F);
  plugin.component_state.clear();
  plugin.missing = false;
  // One lane more than (ceiling - manual slot) / frames; the fixture exposes every target.
  const auto lanes = (plugins::max_vst3_parameter_events - 1U) / frames + 1U;
  auto rejected = plugins::prepare_vst3_graph_node(plugin, scan, 2, tempo(), 48'000.0, frames,
                                                   plugins::Vst3ProcessingMode::realtime,
                                                   lane_automation(plugin, lanes), 7, true);
  REQUIRE_TRUE(!rejected.ready(),
               "effect automation beyond the event budget was not rejected explicitly");
  REQUIRE_EQ(rejected.binding_status, plugins::Vst3BindingStatus::automation_unavailable,
             "effect automation beyond the event budget was not rejected explicitly");
  REQUIRE_TRUE(!rejected.diagnostic.empty(),
               "effect automation beyond the event budget was not rejected explicitly");
  REQUIRE_EQ(rejected.request.instance_id, 0,
             "effect automation beyond the event budget was not rejected explicitly");
  REQUIRE_TRUE(!rejected.request.create_runtime,
               "effect automation beyond the event budget was not rejected explicitly");
  REQUIRE_TRUE(!rejected.manual_events,
               "effect automation beyond the event budget was not rejected explicitly");
  REQUIRE_TRUE(!rejected.runtime_status,
               "effect automation beyond the event budget was not rejected explicitly");
}
} // namespace

int main() {
  try {
    const auto bundle = std::filesystem::canonical(VST3_GAIN_BUNDLE);
    plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
    const auto scanned = scanner.scan(bundle);
    REQUIRE_TRUE(scanned.ok(), "fixture scan failed");
    REQUIRE_EQ(scanned.metadata.classes.size(), 1, "fixture scan failed");
    state_survives_publication(scanned);
    effect_timing_comes_from_project_map(scanned);
    process_failure_mutes_and_reports(scanned);
    invalid_runtime_contracts(scanned);
    const auto connected = scanner.scan(std::filesystem::canonical(VST3_CONNECTED_BUNDLE));
    require(connected.ok(), "connected automation fixture scan failed");
    controller_state_changes_compatibility_key(connected);
    automated_connected_effect(connected);
    preview_connected_effect(connected, 64);
    preview_connected_effect(connected, 128);
    queued_native_gesture_preserves_newer_preview(connected, 64);
    queued_native_gesture_preserves_newer_preview(connected, 128);
    preview_survives_exact_overlay_and_failed_older_receipt();
    size_scoped_manual_effect(connected, 64, 64);
    size_scoped_manual_effect(connected, 64, 128);
    size_scoped_manual_effect(connected, 128, 64);
    size_scoped_manual_effect(connected, 128, 128);
    tempo_split_denies_unqualified_effect_segment(connected);
    manual_connected_effect(connected);
    routed_manual_graph_identity(connected);
    routed_control_overlay_rebinds_manual_slot(connected, 64);
    routed_control_overlay_rebinds_manual_slot(connected, 128);
    const auto sidechain = scanner.scan(std::filesystem::canonical(VST3_SIDECHAIN_BUNDLE));
    require(sidechain.ok(), "sidechain fixture scan failed");
    routed_sidechain_matches_independent_bus_oracle(sidechain, connected);
    const auto lanes = scanner.scan(std::filesystem::canonical(VST3_LANE_EFFECT_BUNDLE));
    if (!lanes.ok() || lanes.metadata.classes.size() != 1)
      throw std::runtime_error("lane effect fixture scan failed: " + lanes.diagnostic);
    for (const std::uint32_t frames : {64U, 128U}) {
      lane_effect_capacity(lanes, frames, 8);
      lane_effect_capacity(lanes, frames, (plugins::max_vst3_parameter_events - 1U) / frames);
      lane_effect_overflow(lanes, frames);
    }
    if (std::getenv("OSCILLATE_BENCHMARK_PLUGIN_GRAPH")) {
      benchmark(scanned, 64, false);
      benchmark(scanned, 128, false);
      benchmark(scanned, 64, true);
      benchmark(scanned, 128, true);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "graph node test: %s\n", error.what());
    return 1;
  }
}
