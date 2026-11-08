#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/export/realtime_tail_plan.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;
template <typename T>
void require(const foundation::Result<T>& result,
             std::string_view message = "prepared route tail test failed") {
  require(static_cast<bool>(result), message);
}

using oscillate::test::take;
template <typename Id> Id id(std::uint64_t low) { return take(Id::from_bits({0, low})); }

class Gate final : public project::TransportGate {
  class Lease final : public project::StoppedTransportLease {};

public:
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

project::ProjectModel model(Gate& gate) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000, std::move(tempo),
                                            std::move(meter), gate));
}

project::Vst3PluginInstance saved_effect() {
  return {id<project::PluginInstanceId>(10),
          std::string(32, 'a'),
          "Fixture",
          "Tail",
          "1.0",
          "/unopened/Tail.vst3",
          std::string(64, 'b'),
          {},
          {},
          false};
}

class NoopEffect final : public engine::ProcessorRuntime {
public:
  void process(std::span<engine::StereoFrame>, engine::BlockContext) noexcept override {}
};

class Provider final : public app::TrackInsertProvider {
public:
  foundation::Result<app::PreparedTrackInsert>
  prepare(const project::Snapshot&, const project::Vst3PluginInstance&, std::uint32_t) override {
    if (!bind)
      return foundation::Result<app::PreparedTrackInsert>::failure(
          {foundation::ErrorCode::unavailable, "Synthetic plugin was not bound"});
    app::PreparedTrackInsert result{};
    result.processor = std::make_shared<NoopEffect>();
    result.process_failed = [fault = fault]() noexcept {
      return fault->load(std::memory_order_acquire);
    };
    result.finite_tail_frames = tail;
    result.fixed_tail_qualified = qualified;
    result.instrument_source = instrument;
    if (instrument)
      result.prepared_export_end = [this] { return export_end; };
    return foundation::Result<app::PreparedTrackInsert>::success(std::move(result));
  }
  std::optional<std::uint32_t> tail = 8;
  std::optional<std::uint64_t> export_end;
  bool instrument = false;
  bool qualified = true;
  bool bind = true;
  std::shared_ptr<std::atomic<bool>> fault = std::make_shared<std::atomic<bool>>(false);
};

engine::RoutedMixerLimits limits() {
  return {{1, 64},
          64,
          8ULL * 64 * sizeof(engine::StereoFrame),
          8ULL * 65 * sizeof(engine::StereoFrame)};
}

void prepared_effect_tail_follows_group_and_send_routes() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(2);
  const auto group = id<project::TrackId>(3);
  const auto returns = id<project::TrackId>(4);
  const auto send = id<project::SendId>(5);
  (void)take(state.apply({project::AddTrack{audio},
                          project::AddTrack{group, project::TrackKind::group_bus},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::SetTrackMainOutput{audio, group},
                          project::AddSendRoute{audio, {send, returns, 0.5}},
                          project::InsertVst3Plugin{audio, 0, saved_effect()}}));
  Provider provider;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  const auto revision = state.snapshot()->revision;
  require(!control.prepared_route_tail_receipt(revision));
  require(control.submit_current());
  auto receipt = take(control.prepared_route_tail_receipt(revision));
  require(receipt.revision == revision && receipt.route.topological_order.size() == 4 &&
          receipt.route.edges.size() == 4 && receipt.nodes.size() == 4);
  const auto source = control.source_layout().front().node;
  const auto source_tail =
      std::ranges::find(receipt.nodes, source, &app::PreparedRouteTailReceipt::NodeTail::node);
  require(source_tail != receipt.nodes.end() && source_tail->frames == 8);
  std::vector<oscillate::exporting::RealtimeNodeTail> tails;
  tails.reserve(receipt.nodes.size());
  for (const auto& node : receipt.nodes)
    tails.push_back({node.node, node.frames});
  auto planned = oscillate::exporting::plan_realtime_tail_frames(receipt.route, receipt.master,
                                                                 tails, 480'000);
  require(planned && planned.value() == 8);
  require(!control.prepared_route_tail_receipt(revision + 1));

  std::array<engine::StereoFrame, 64> samples{}, output{};
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  inputs[control.source_layout().front().topological_index] = samples;
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered);
  control.observe();
  require(control.prepared_route_tail_receipt(revision));
  (void)take(state.apply({project::SetSendGain{send, 0.25}}));
  require(!control.prepared_route_tail_receipt(revision));
}

void unqualified_or_unknown_plugin_tail_fails_closed() {
  for (int case_index = 0; case_index < 4; ++case_index) {
    Gate gate;
    auto state = model(gate);
    const auto audio = id<project::TrackId>(2);
    (void)take(state.apply(
        {project::AddTrack{audio}, project::InsertVst3Plugin{audio, 0, saved_effect()}}));
    Provider provider;
    if (case_index == 0)
      provider.qualified = false;
    if (case_index == 1)
      provider.tail = std::nullopt;
    if (case_index == 3)
      provider.bind = false;
    app::RoutedPlaybackControl control(state, limits(), &provider);
    if (case_index == 3) {
      require(!control.submit_current());
      require(!control.prepared_route_tail_receipt(state.snapshot()->revision));
      continue;
    }
    require(control.submit_current());
    if (case_index == 2)
      provider.fault->store(true, std::memory_order_release);
    require(!control.prepared_route_tail_receipt(state.snapshot()->revision));
  }
}

void instrument_tail_requires_exact_prepared_midi_cut() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(22);
  (void)take(state.apply({project::AddTrack{track, project::TrackKind::instrument},
                          project::InsertVst3Plugin{track, 0, saved_effect()}}));
  Provider provider;
  provider.instrument = true;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  require(control.submit_current(), "instrument graph did not prepare");
  const auto revision = state.snapshot()->revision;
  require(!control.prepared_route_tail_receipt(revision, 256),
          "instrument tail declaration alone passed without a MIDI cut");
  provider.export_end = 256;
  REQUIRE_TRUE(!control.prepared_route_tail_receipt(revision, 257),
               "instrument tail admitted a different or absent MIDI cutoff");
  REQUIRE_TRUE(!control.prepared_route_tail_receipt(revision),
               "instrument tail admitted a different or absent MIDI cutoff");
  const auto receipt = take(control.prepared_route_tail_receipt(revision, 256));
  const auto source = control.source_layout().front().node;
  const auto tail =
      std::ranges::find(receipt.nodes, source, &app::PreparedRouteTailReceipt::NodeTail::node);
  REQUIRE_NE(tail, receipt.nodes.end(),
             "exact prepared instrument release was not included in the route tail");
  REQUIRE_EQ(tail->frames, 8,
             "exact prepared instrument release was not included in the route tail");
  provider.fault->store(true, std::memory_order_release);
  require(!control.prepared_route_tail_receipt(revision, 256),
          "failed prepared instrument was admitted for real-time export");
}

void native_prepared_tails_match_export_drain_policy() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(2);
  // Shape with one low-pass band on the track; Expanse then Ceiling on the master.
  const auto& shape = *oscillate::native_dsp::effects::find_engine("shape");
  auto values = oscillate::native_dsp::default_values(*shape.descriptor);
  const auto set = [&](std::string_view parameter, double value) {
    values[*oscillate::native_dsp::find_parameter(*shape.descriptor, parameter)] = value;
  };
  set("shape.on_1", 1.0);
  set("shape.kind_1", static_cast<double>(oscillate::native_dsp::EqBandKind::low_pass));
  set("shape.freq_1", 1'000.0);
  set("shape.q_1", 0.707);
  const auto preset = [](std::string_view engine) {
    return take(oscillate::native_inserts::native_effect_patch_for_preset(
        *oscillate::native_dsp::effects::find_engine(engine), std::nullopt));
  };
  (void)take(state.apply(
      {project::AddTrack{audio},
       project::AddNativeEffect{
           audio, 0, take(oscillate::native_inserts::make_native_effect_patch(shape, values))},
       project::AddNativeEffect{project::MasterSignalPath{}, 0, preset("expanse")},
       project::AddNativeEffect{project::MasterSignalPath{}, 1, preset("ceiling")}}));
  app::RoutedPlaybackControl control(state, limits());
  require(control.submit_current());
  auto receipt = take(control.prepared_route_tail_receipt(state.snapshot()->revision));
  const auto source = control.source_layout().front().node;
  const auto source_tail =
      std::ranges::find(receipt.nodes, source, &app::PreparedRouteTailReceipt::NodeTail::node);
  const auto master_tail = std::ranges::find(receipt.nodes, receipt.master,
                                             &app::PreparedRouteTailReceipt::NodeTail::node);
  const std::array<oscillate::native_dsp::EqBandSettings, 1> bands{
      {{oscillate::native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}};
  const auto eq = take(oscillate::native_dsp::prepare_eq(bands, 48'000));
  const auto eq_tail = take(oscillate::native_dsp::estimate_eq_tail_frames(eq));
  require(source_tail != receipt.nodes.end() && source_tail->frames == eq_tail);
  // The reverb rings on after the mix; the receipt carries its bounded tail.
  require(master_tail != receipt.nodes.end() && master_tail->frames.value_or(0) > 0);
  REQUIRE_GT(control.master_output_latency_frames(), 0); // Output latency is separate.
  std::vector<oscillate::exporting::RealtimeNodeTail> tails;
  tails.reserve(receipt.nodes.size());
  for (const auto& node : receipt.nodes)
    tails.push_back({node.node, node.frames});
  const auto planned = oscillate::exporting::plan_realtime_tail_frames(
      receipt.route, receipt.master, tails, 480'000);
  require(planned && planned.value() == eq_tail + master_tail->frames.value_or(0));
}

void bypassed_master_vst3_is_not_a_live_stage() {
  Gate gate;
  auto base = model(gate).snapshot();
  auto saved = *base;
  saved.master_effects.emplace_back(id<project::InsertSlotId>(11), false, saved_effect());
  auto state = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(state, limits());
  require(control.submit_current(), "bypassed VST3 master blocked graph preparation");
  auto receipt = take(control.prepared_route_tail_receipt(state.snapshot()->revision));
  const auto master = std::ranges::find(receipt.nodes, receipt.master,
                                        &app::PreparedRouteTailReceipt::NodeTail::node);
  REQUIRE_NE(master, receipt.nodes.end(), "bypassed VST3 master contributed a prepared tail");
  REQUIRE_EQ(master->frames, 0, "bypassed VST3 master contributed a prepared tail");

  saved.master_effects.front().enabled = true;
  auto enabled = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl rejected(enabled, limits());
  require(!rejected.submit_current(), "enabled VST3 master unexpectedly prepared");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main() {
  prepared_effect_tail_follows_group_and_send_routes();
  unqualified_or_unknown_plugin_tail_fails_closed();
  instrument_tail_requires_exact_prepared_midi_cut();
  native_prepared_tails_match_export_drain_policy();
  bypassed_master_vst3_is_not_a_live_stage();
}
