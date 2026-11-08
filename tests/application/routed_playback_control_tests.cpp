#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <new>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace native_dsp = oscillate::native_dsp;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t low) { return take(Id::from_bits({0, low})); }

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
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

engine::RoutedMixerLimits limits() {
  return {{1, 64},
          64,
          6ULL * 64 * sizeof(engine::StereoFrame),
          5ULL * 65 * sizeof(engine::StereoFrame)};
}

void set_source(std::span<std::span<const engine::StereoFrame>> inputs,
                std::span<const oscillate::routing::ProjectRouteSource> layout,
                project::TrackId track, std::span<const engine::StereoFrame> audio) {
  for (const auto& source : layout) {
    if (source.track == track) {
      inputs[source.topological_index] = audio;
      return;
    }
  }
  throw std::runtime_error("source track is absent from routed layout");
}

void near(engine::StereoFrame sample, float expected) {
  if (std::fabs(sample.left - expected) >= 0.00001F ||
      std::fabs(sample.right + expected) >= 0.00001F)
    throw std::runtime_error("routed gain expected " + std::to_string(expected) + " observed " +
                             std::to_string(sample.left));
}

// Built-in effect fixtures (ADR-0047). Shape, Press and Ceiling run the
// native_dsp EQ, compressor and limiter bit for bit, so those runtimes stay
// the oracles here.
project::NativeEffectPatch effect_patch(std::string_view engine_id,
                                        std::span<const std::pair<std::string, double>> values) {
  const auto* engine = native_dsp::effects::find_engine(engine_id);
  if (engine == nullptr)
    throw std::runtime_error("built-in effect is not registered");
  auto dense = native_dsp::synth::default_values(*engine->descriptor);
  for (const auto& [parameter, value] : values) {
    const auto index = native_dsp::synth::find_parameter(*engine->descriptor, parameter);
    if (!index)
      throw std::runtime_error("unknown built-in effect parameter " + parameter);
    dense[*index] = value;
  }
  return take(oscillate::native_inserts::make_native_effect_patch(*engine, dense));
}

project::NativeEffectPatch shape_patch(std::span<const native_dsp::EqBandSettings> bands) {
  std::vector<std::pair<std::string, double>> values;
  for (std::size_t index = 0; index < bands.size(); ++index) {
    const auto band = std::to_string(index + 1);
    values.push_back({"shape.on_" + band, 1.0});
    values.push_back({"shape.kind_" + band, static_cast<double>(bands[index].kind)});
    values.push_back({"shape.freq_" + band, bands[index].frequency_hz});
    values.push_back({"shape.q_" + band, bands[index].shape});
    values.push_back({"shape.gain_" + band, bands[index].gain_db});
  }
  return effect_patch("shape", values);
}

project::NativeEffectPatch shape_patch(native_dsp::EqBandSettings band) {
  return shape_patch(std::span(&band, 1));
}

project::NativeEffectPatch press_patch(const native_dsp::CompressorSettings& settings) {
  std::vector<std::pair<std::string, double>> values{
      {"press.threshold", settings.threshold_db}, {"press.ratio", settings.ratio},
      {"press.knee", settings.knee_db},           {"press.attack", settings.attack_ms},
      {"press.release", settings.release_ms},     {"press.makeup", settings.makeup_db}};
  if (settings.sidechain_highpass_hz != 0.0) {
    values.push_back({"press.detector_hp", 1.0});
    values.push_back({"press.detector_freq", settings.sidechain_highpass_hz});
  }
  return effect_patch("press", values);
}

project::NativeEffectPatch ceiling_patch(const native_dsp::LimiterSettings& settings) {
  const std::array<std::pair<std::string, double>, 3> values{
      {{"ceiling.ceiling", settings.ceiling_db},
       {"ceiling.lookahead", settings.lookahead_ms},
       {"ceiling.release", settings.release_ms}}};
  return effect_patch("ceiling", values);
}

constexpr native_dsp::EqBandSettings high_pass_100{native_dsp::EqBandKind::high_pass, 100.0, 0.707,
                                                   0.0};
constexpr native_dsp::EqBandSettings low_pass_1k{native_dsp::EqBandKind::low_pass, 1'000.0, 0.707,
                                                 0.0};

void master_level_uses_routed_output_after_graph_publication() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(1'901);
  const auto initial = take(state.apply({project::AddTrack{audio}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "master-level graph did not prepare");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "initial master block did not render");
  near(output[0], 0.25F);
  control.observe();
  const auto revised = take(state.apply({project::SetMasterLevel{0.5}}));
  REQUIRE_OK(control.begin_edit(revised, "master level"),
             "master level edit did not remain pending until the next callback");
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "master level edit did not remain pending until the next callback");
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "master level callback held, failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "master level callback held, failed or allocated");
  REQUIRE_GT(output[0].left, 0.125F, "master level did not ramp from the prior output");
  REQUIRE_LT(output[0].left, 0.25F, "master level did not ramp from the prior output");
  near(output.back(), 0.125F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "active graph revision did not report the master level change");
}

void live_mute_solo_and_inaudible_solo_revision_do_not_hold() {
  Gate gate;
  auto state = model(gate);
  const auto first = id<project::TrackId>(1'911);
  const auto second = id<project::TrackId>(1'912);
  (void)take(state.apply({project::AddTrack{first}, project::AddTrack{second}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "live solo fixture did not prepare");
  std::array<engine::StereoFrame, 64> first_signal{}, second_signal{}, output{};
  first_signal.fill({0.25F, -0.25F});
  second_signal.fill({0.5F, -0.5F});
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), first, first_signal);
  set_source(inputs, control.source_layout(), second, second_signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "live solo fixture did not render its first block");
  near(output.back(), 0.75F);
  control.observe();
  const auto mute = take(state.apply({project::SetTrackMuted{first, true}}));
  REQUIRE_OK(control.begin_edit(mute, "mute first track"), "live mute held the output");
  REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::rendered,
             "live mute held the output");
  near(output.back(), 0.5F);
  control.observe();
  const auto solo = take(state.apply({project::SetTrackSoloed{second, true}}));
  REQUIRE_OK(control.begin_edit(solo, "solo second track"),
             "unchanged effective mute projection held the output");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::rendered,
             "unchanged effective mute projection held the output");
  near(output.back(), 0.5F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, solo.snapshot->revision,
             "inaudible solo edit did not publish its saved revision");
}

// A live switch whose candidate can never take over (its source bank never
// fills) is withdrawn after a bounded number of passed blocks. The old graph
// plays throughout, the edit's revision is reported failed, and the next
// edit publishes and switches.
void unadoptable_live_switch_is_withdrawn_and_next_edit_publishes() {
  constexpr std::size_t block = 64;
  constexpr float level = 0.25F;
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(1'921);
  const auto initial = take(state.apply({project::AddTrack{track}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "withdrawal fixture did not prepare");
  std::array<engine::StereoFrame, block> signal{}, output{};
  signal.fill({level, -level});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, signal);
  struct Bank {
    std::span<const std::span<const engine::StereoFrame>> inputs;
    std::uint64_t refused = 0;
  } bank{inputs};
  const engine::RoutedSourceFill fill{
      &bank, nullptr,
      [](void* context, std::uint64_t revision, engine::BlockContext, std::size_t,
         std::span<const std::span<const engine::StereoFrame>>& selected) noexcept {
        auto& self = *static_cast<Bank*>(context);
        if (revision == self.refused)
          return false;
        selected = self.inputs;
        return true;
      }};
  std::uint64_t position = 0;
  const auto render = [&] {
    const auto status = control.render_block(inputs, output, {position, 1}, fill);
    position += block;
    control.observe();
    return status;
  };
  REQUIRE_EQ(render(), engine::RoutedRenderStatus::rendered, "the first graph did not render");
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "the first graph did not activate");

  const auto stuck = take(state.apply({project::AddNativeEffect{track, 0, press_patch({})}}));
  bank.refused = stuck.snapshot->revision;
  REQUIRE_OK(control.begin_edit(stuck, "add Press"), "the live switch was not published");
  REQUIRE_TRUE(!control.held_boundary(), "the edit did not take the live path");
  for (std::uint64_t passed = 0; passed < app::live_candidate_passed_block_limit; ++passed) {
    REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
               "the candidate was settled before its bounded wait ran out");
    REQUIRE_EQ(render(), engine::RoutedRenderStatus::rendered,
               "the old graph stopped while the candidate waited");
    near(output.back(), level);
  }
  // The callback answers the withdrawal at its next block.
  REQUIRE_EQ(render(), engine::RoutedRenderStatus::rendered,
             "the withdrawal interrupted the old graph");
  near(output.back(), level);
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "the withdrawn candidate's revision was not reported failed");
  REQUIRE_TRUE(control.status().failure.has_value(),
               "the withdrawn candidate's revision carries no error");
  REQUIRE_EQ(control.status().model_revision, stuck.snapshot->revision,
             "the failure was reported for another revision");
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "withdrawing the candidate changed the audible revision");
  REQUIRE_TRUE(!control.graph_handoff_in_progress(), "the withdrawn candidate still blocks edits");

  const auto next = take(state.apply({project::AddNativeEffect{track, 1, press_patch({})}}));
  REQUIRE_OK(control.begin_edit(next, "add a second Press"),
             "the edit after the withdrawal was refused");
  REQUIRE_EQ(render(), engine::RoutedRenderStatus::rendered,
             "the edit after the withdrawal held or silenced a block");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "the edit after the withdrawal did not switch");
  REQUIRE_EQ(control.status().active_graph_revision, next.snapshot->revision,
             "the edit after the withdrawal did not switch");
}

void live_channel_automation_uses_sample_values_before_routing() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(1'902);
  const project::AutomationLane lane{
      id<project::AutomationLaneId>(1'903),
      project::TrackControlTarget{audio, project::TrackControlTarget::Key::channel_level},
      {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0,
       0.8, false, 0, project::AutomationScale::decibel},
      project::AutomationTimebase::elapsed,
      {{id<project::AutomationPointId>(1'904), 0, 0.5, project::AutomationShape::linear},
       {id<project::AutomationPointId>(1'905), 2, 1.0}},
      1};
  (void)take(state.apply({project::AddTrack{audio}, project::SetTrackLevelPan{audio, 0.8, 0.25},
                          project::AddAutomationLane{audio, lane}}));
  app::RoutedPlaybackControl control(state, limits());
  const auto started = control.submit_current();
  REQUIRE_OK(started, started ? "" : started.error().message);
  std::array<engine::StereoFrame, 4> signal{}, output{};
  signal.fill({1.0F, -1.0F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "live channel automation lost sample timing, balance or callback allocation bound");
  REQUIRE_EQ(counted_allocations, 0,
             "live channel automation lost sample timing, balance or callback allocation bound");
  REQUIRE_LT(std::abs(output[0].left - 0.375F), tolerance::float_round_trip_f,
             "live channel automation lost sample timing, balance or callback allocation bound");
  REQUIRE_LT(std::abs(output[0].right + 0.5F), tolerance::float_round_trip_f,
             "live channel automation lost sample timing, balance or callback allocation bound");
  REQUIRE_LT(std::abs(output[2].left - 0.75F), tolerance::float_round_trip_f,
             "live channel automation lost sample timing, balance or callback allocation bound");
  REQUIRE_LT(std::abs(output[2].right + 1.0F), tolerance::float_round_trip_f,
             "live channel automation lost sample timing, balance or callback allocation bound");
}

void live_route_gain_edit_and_failed_model_divergence() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(2);
  const auto second = id<project::TrackId>(3);
  const auto group = id<project::TrackId>(4);
  const auto returns = id<project::TrackId>(5);
  const auto send = id<project::SendId>(6);
  auto initial = take(state.apply({project::AddTrack{audio}, project::AddTrack{second},
                                   project::AddTrack{group, project::TrackKind::group_bus},
                                   project::AddTrack{returns, project::TrackKind::return_bus},
                                   project::SetTrackMainOutput{audio, group},
                                   project::AddSendRoute{audio, {send, returns, 0.25}}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "initial saved route was not prepared as pending");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "initial saved route was not prepared as pending");
  REQUIRE_EQ(control.topological_order().size(), 5,
             "routed source layout does not include all saved audio tracks");
  REQUIRE_EQ(control.source_layout().size(), 2,
             "routed source layout does not include all saved audio tracks");

  std::array<engine::StereoFrame, 4> one{}, half{}, output{};
  one.fill({1.0F, -1.0F});
  half.fill({0.5F, -0.5F});
  std::array<std::span<const engine::StereoFrame>, 5> inputs{};
  set_source(inputs, control.source_layout(), audio, one);
  set_source(inputs, control.source_layout(), second, half);
  counting_allocations = true;
  const auto first = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, engine::RoutedRenderStatus::rendered,
             "first live routed block did not render allocation-free");
  REQUIRE_EQ(counted_allocations, 0, "first live routed block did not render allocation-free");
  near(output[0], 1.75F);
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "routed graph activation was not observed");
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "routed graph activation was not observed");

  auto changed = take(state.apply({project::SetSendGain{send, 0.5}}));
  REQUIRE_OK(control.begin_edit(changed, "send gain"),
             "gain edit did not start pending callback handoff");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "gain edit did not start pending callback handoff");
  REQUIRE_TRUE(!control.held_boundary(), "gain edit did not start pending callback handoff");
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {4, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "live send gain inserted silence, allocated or missed its ramp");
  REQUIRE_EQ(counted_allocations, 0,
             "live send gain inserted silence, allocated or missed its ramp");
  REQUIRE_TRUE(!control.held_boundary(),
               "live send gain inserted silence, allocated or missed its ramp");
  REQUIRE_LT(std::abs(output[0].left - 1.75390625F), tolerance::float_round_trip_f,
             "live send gain inserted silence, allocated or missed its ramp");
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "gain edit did not become the active revision");
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "gain edit did not become the active revision");

  // One edit changing two built-in chains switches both live (ADR-0050),
  // with no hold, while the old graph plays until the switching block.
  auto both = take(state.apply({project::AddNativeEffect{audio, 0, press_patch({})},
                                project::AddNativeEffect{second, 0, press_patch({})}}));
  REQUIRE_OK(control.begin_edit(both, "two chains"), "an edit changing two chains was refused");
  REQUIRE_EQ(control.render_block(inputs, output, {8, 1}), engine::RoutedRenderStatus::rendered,
             "the two-chain switch held or silenced a block");
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "the two-chain edit did not become the active revision");
  REQUIRE_EQ(control.status().active_graph_revision, both.snapshot->revision,
             "the two-chain edit did not become the active revision");
}

void live_send_gain_uses_existing_route_without_hold(std::size_t frames) {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(7'401);
  const auto returns = id<project::TrackId>(7'402);
  const auto second_return = id<project::TrackId>(7'404);
  const auto send = id<project::SendId>(7'403);
  const auto initial = take(state.apply(
      {project::AddTrack{audio}, project::AddTrack{returns, project::TrackKind::return_bus},
       project::AddTrack{second_return, project::TrackKind::return_bus},
       project::AddSendRoute{audio, {send, returns, 0.25}}}));
  auto budget = limits();
  budget.max_frames = frames;
  budget.max_buffer_bytes = 6ULL * frames * sizeof(engine::StereoFrame);
  app::RoutedPlaybackControl control(state, budget);
  REQUIRE_OK(control.submit_current(), "live send route failed to prepare");
  std::array<engine::StereoFrame, 128> signal{}, output{};
  signal.fill({1.0F, -1.0F});
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), audio, std::span(signal).first(frames));
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(control.render_block(inputs, block, {0, 1}), engine::RoutedRenderStatus::rendered,
             "live send route baseline did not include the return");
  REQUIRE_LT(std::abs(block.back().left - 1.25F), tolerance::float_round_trip_f,
             "live send route baseline did not include the return");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "live send route baseline was not acknowledged");
  const auto revised = take(state.apply({project::SetSendGain{send, 0.5}}));
  REQUIRE_TRUE(control.begin_edit(revised, "send gain"),
               "live send gain was not admitted without a hold");
  REQUIRE_TRUE(!control.held_boundary(), "live send gain was not admitted without a hold");
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, block, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "live send gain lost output or ramped to the wrong target");
  REQUIRE_EQ(counted_allocations, 0, "live send gain lost output or ramped to the wrong target");
  REQUIRE_GT(output.front().left, 1.25F,
             "live send gain lost output or ramped to the wrong target");
  REQUIRE_LT(output.front().left, 1.26F,
             "live send gain lost output or ramped to the wrong target");
  REQUIRE_LT(std::abs(block.back().left - 1.5F), tolerance::float_round_trip_f,
             "live send gain lost output or ramped to the wrong target");
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "live send gain did not acknowledge the audible revision");
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "live send gain did not acknowledge the audible revision");
  const auto changed_endpoint = take(state.apply(
      {project::RemoveSendRoute{send}, project::AddSendRoute{audio, {send, second_return, 0.5}}}));
  // A send keeping its ID but changing destination cannot continue its
  // route: the edit is refused while the old graph keeps playing, unheld.
  (void)control.begin_edit(changed_endpoint, "replace send target");
  REQUIRE_EQ(control.render_block(inputs, block, {2 * frames, 1}),
             engine::RoutedRenderStatus::rendered,
             "changed send endpoint bypassed the guarded route publication");
  REQUIRE_TRUE(!control.held_boundary(),
               "changed send endpoint bypassed the guarded route publication");
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "changed send endpoint bypassed the guarded route publication");
}

void automated_send_gain_refuses_live_overlay() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(7'411);
  const auto returns = id<project::TrackId>(7'412);
  const auto send = id<project::SendId>(7'413);
  const auto lane_id = id<project::AutomationLaneId>(7'414);
  project::AutomationLane lane{lane_id,
                               project::SendGainTarget{send},
                               {project::AutomationRepresentation::physical, "mixer.send_gain",
                                "linear_gain", 0.0, 4.0, 0.25, false, 0,
                                project::AutomationScale::decibel},
                               project::AutomationTimebase::elapsed,
                               {{id<project::AutomationPointId>(7'415), 0, 0.25}},
                               1};
  REQUIRE_OK(state.apply({project::AddTrack{audio},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::AddSendRoute{audio, {send, returns, 0.25}},
                          project::AddAutomationLane{audio, lane}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "automated send fixture did not prepare");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  signal.fill({1.0F, -1.0F});
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "automated send fixture did not activate");
  control.observe();
  const auto revised = take(state.apply({project::SetSendGain{send, 0.5}}));
  // The static gain overlay would drop the lane: the edit instead rebuilds the
  // graph, switching live with its automation intact.
  require(control.begin_edit(revised, "automated send gain").has_value() &&
              !control.held_boundary() &&
              control.render_block(inputs, output, {64, 1}) == engine::RoutedRenderStatus::rendered,
          "automated send gain incorrectly used the static live overlay");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "automated send gain did not switch live");
}

void live_send_automation_survives_graph_handoff() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(81);
  const auto returns = id<project::TrackId>(82);
  const auto send = id<project::SendId>(83);
  const auto lane_id = id<project::AutomationLaneId>(84);
  project::AutomationLane lane{
      lane_id,
      project::SendGainTarget{send},
      {project::AutomationRepresentation::physical, "mixer.send_gain", "linear_gain", 0.0, 4.0,
       0.25, false, 0, project::AutomationScale::decibel},
      project::AutomationTimebase::elapsed,
      {{id<project::AutomationPointId>(85), 0, 0.5, project::AutomationShape::linear},
       {id<project::AutomationPointId>(86), 4, 0.75}},
      1};
  (void)take(state.apply({project::AddTrack{audio},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::AddSendRoute{audio, {send, returns, 0.25}},
                          project::AddAutomationLane{audio, lane}}));
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "send automation graph failed to prepare");
  std::array<engine::StereoFrame, 4> one{}, output{};
  one.fill({1.0F, -1.0F});
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), audio, one);
  counting_allocations = true;
  auto rendered = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "initial send automation callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "initial send automation callback failed or allocated");
  // The direct path plus the send, which blends in decibels (schema 1.39):
  // geometrically from 0.5 to 0.75 over four samples.
  near(output[0], 1.5F);
  near(output[1], static_cast<float>(1.0 + 0.5 * std::pow(1.5, 0.25)));
  near(output[3], static_cast<float>(1.0 + 0.5 * std::pow(1.5, 0.75)));
  lane.points[1].value = 1.0;
  ++lane.settings_generation;
  const auto edit = take(state.apply({project::ReplaceAutomationLane{lane_id, lane}}));
  REQUIRE_OK(control.begin_edit(edit, "send automation"),
             "send automation graph revision did not hold and publish");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::holding,
             "send automation graph revision did not hold and publish");
  REQUIRE_OK(control.publish_held(), "send automation graph revision did not hold and publish");
  counting_allocations = true;
  rendered = control.render_block(inputs, output, {4, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "revised send automation callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "revised send automation callback failed or allocated");
  near(output[0], 2.0F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, edit.snapshot->revision,
             "send automation model revision was not rendered");
}

void initial_master_chain_keeps_history_through_route_edit() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(2);
  const auto returns = id<project::TrackId>(3);
  const auto send = id<project::SendId>(4);
  (void)take(state.apply(
      {project::AddTrack{audio}, project::AddTrack{returns, project::TrackKind::return_bus},
       project::AddSendRoute{audio, {send, returns, 0.25}},
       project::AddNativeEffect{project::MasterSignalPath{}, 0, shape_patch(low_pass_1k)},
       project::AddNativeEffect{project::MasterSignalPath{}, 1,
                                press_patch(native_dsp::CompressorSettings{})}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "initial master chain route did not prepare");
  std::array<engine::StereoFrame, 4> impulse{}, silence{}, output{};
  impulse[0] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), audio, impulse);

  const std::array<oscillate::native_dsp::EqBandSettings, 1> bands{
      {{oscillate::native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}};
  oscillate::native_dsp::EqRuntime reference(
      take(oscillate::native_dsp::prepare_eq(bands, 48'000)));
  oscillate::native_dsp::CompressorRuntime compressor(
      take(oscillate::native_dsp::prepare_compressor(oscillate::native_dsp::CompressorSettings{},
                                                     48'000)));
  std::array<engine::StereoFrame, 4> expected{};
  expected[0] = {1.25F, -1.25F};
  reference.process(expected, {0, 1});
  compressor.process(expected, {0, 1});
  counting_allocations = true;
  const auto first = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, engine::RoutedRenderStatus::rendered,
             "initial routed master effect allocated or failed");
  REQUIRE_EQ(counted_allocations, 0, "initial routed master effect allocated or failed");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    require(std::fabs(output[frame].left - expected[frame].left) < 0.000001F &&
                std::fabs(output[frame].right - expected[frame].right) < 0.000001F,
            "initial routed master chain disagrees with native reference");
  control.observe();

  auto changed = take(state.apply({project::SetSendGain{send, 0.5}}));
  REQUIRE_OK(control.begin_edit(changed, "send gain"),
             "route edit with active master chain was rejected");
  set_source(inputs, control.source_layout(), audio, silence);
  expected.fill({});
  reference.process(expected, {4, 1});
  compressor.process(expected, {4, 1});
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {4, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "live send change interrupted the stateful master chain");
  REQUIRE_EQ(counted_allocations, 0, "live send change interrupted the stateful master chain");
  REQUIRE_TRUE(!control.held_boundary(), "live send change interrupted the stateful master chain");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    require(std::fabs(output[frame].left - expected[frame].left) < 0.000001F &&
                std::fabs(output[frame].right - expected[frame].right) < 0.000001F,
            "routing edit reset live master chain history");
}

void source_layout_change_is_rejected_without_misrouting() {
  Gate gate;
  auto state = model(gate);
  const auto a = id<project::TrackId>(2);
  const auto bus = id<project::TrackId>(3);
  (void)take(
      state.apply({project::AddTrack{a}, project::AddTrack{bus, project::TrackKind::group_bus}}));
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "layout fixture did not prepare");
  std::array<engine::StereoFrame, 4> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), a, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "layout fixture did not activate");
  control.observe();
  auto changed = take(state.apply({project::AddTrack{id<project::TrackId>(4)}}));
  REQUIRE_OK(control.begin_edit(changed, "add audio track"), "layout edit was not held");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::holding,
             "layout edit was not held");
  auto failed = control.publish_held();
  REQUIRE_TRUE(!failed, "new source layout was silently applied to old ordered spans");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::conflict,
             "new source layout was silently applied to old ordered spans");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "new source layout was silently applied to old ordered spans");
  REQUIRE_EQ(control.source_layout().size(), 1,
             "new source layout was silently applied to old ordered spans");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::rendered,
             "old layout did not resume after rejected source change");
  near(output[0], 0.25F);
}

class SevenFrameGain final : public engine::ProcessorRuntime {
public:
  explicit SevenFrameGain(bool nonlinear = false) : nonlinear_(nonlinear) {}
  void process(std::span<engine::StereoFrame> frames, engine::BlockContext) noexcept override {
    for (auto& frame : frames) {
      const auto input = frame;
      frame = history_[cursor_];
      history_[cursor_] = nonlinear_ ? engine::StereoFrame{input.left * std::fabs(input.left),
                                                           input.right * std::fabs(input.right)}
                                     : engine::StereoFrame{input.left * 0.5F, input.right * 0.5F};
      cursor_ = (cursor_ + 1) % history_.size();
    }
  }

private:
  std::array<engine::StereoFrame, 7> history_{};
  std::size_t cursor_ = 0;
  bool nonlinear_ = false;
};

class BoundInsertFixture final : public app::TrackInsertProvider {
public:
  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t max_frames) override {
    ++preparations;
    if (snapshot.sample_rate != 48'000 || max_frames != expected_max_frames)
      return foundation::Result<app::PreparedTrackInsert>::failure(
          {foundation::ErrorCode::invalid_argument, "unexpected processing format"});
    // A missing plugin has no scan receipt, as with the real host.
    if (unavailable || saved.missing)
      return foundation::Result<app::PreparedTrackInsert>::failure(
          {foundation::ErrorCode::unavailable, "verified scan receipt is missing"});
    auto runtime = std::make_shared<SevenFrameGain>(nonlinear);
    owner = runtime;
    return foundation::Result<app::PreparedTrackInsert>::success(
        {std::move(runtime), 7,
         [fault = fault]() noexcept { return fault->load(std::memory_order_acquire); }});
  }

  std::uint32_t expected_max_frames = 64;
  std::size_t preparations = 0;
  bool unavailable = false;
  bool nonlinear = false;
  std::weak_ptr<SevenFrameGain> owner;
  std::shared_ptr<std::atomic<bool>> fault = std::make_shared<std::atomic<bool>>(false);
};

// Halves its input and adds a constant hum, which it keeps emitting on
// silent input, so a removed instance's ring-out stays audible.
class ImmediateGain final : public engine::ProcessorRuntime {
public:
  explicit ImmediateGain(float hum) : hum_(hum) {}
  void process(std::span<engine::StereoFrame> audio, engine::BlockContext) noexcept override {
    for (auto& frame : audio) {
      frame.left = frame.left * 0.5F + hum_;
      frame.right = frame.right * 0.5F - hum_;
    }
  }

private:
  float hum_;
};

class LiveAdditionProvider final : public app::TrackInsertProvider {
public:
  foundation::Result<app::PreparedTrackInsert>
  prepare(const project::Snapshot&, const project::Vst3PluginInstance&, std::uint32_t) override {
    ++preparations;
    if (unavailable)
      return foundation::Result<app::PreparedTrackInsert>::failure(
          {foundation::ErrorCode::unavailable, "synthetic verified binding unavailable"});
    auto runtime = std::make_shared<ImmediateGain>(hum);
    owner = runtime;
    app::PreparedTrackInsert result;
    result.processor = std::move(runtime);
    result.latency_frames = latency;
    result.process_failed = [fault = fault]() noexcept {
      return fault->load(std::memory_order_acquire);
    };
    result.finite_tail_frames = finite_tail;
    return foundation::Result<app::PreparedTrackInsert>::success(std::move(result));
  }

  std::size_t preparations = 0;
  bool unavailable = false;
  float hum = 0.0F;
  // Empty: the plugin cannot bound its tail.
  std::optional<std::uint32_t> finite_tail = 0;
  std::uint32_t latency = 0;
  std::weak_ptr<ImmediateGain> owner;
  std::shared_ptr<std::atomic<bool>> fault = std::make_shared<std::atomic<bool>>(false);
};

class DelayGain final : public engine::ProcessorRuntime {
public:
  struct Settings {
    std::size_t delay;
    float gain;
  };

  DelayGain(Settings settings, std::shared_ptr<std::atomic<bool>> fault)
      : delay_(settings.delay), gain_(settings.gain), fault_(std::move(fault)) {}

  void process(std::span<engine::StereoFrame> stereo, engine::BlockContext) noexcept override {
    if (fault_->load(std::memory_order_acquire)) {
      std::ranges::fill(stereo, engine::StereoFrame{});
      return;
    }
    for (auto& frame : stereo) {
      const auto input = frame;
      frame = history_[cursor_];
      history_[cursor_] = {input.left * gain_, input.right * gain_};
      cursor_ = (cursor_ + 1) % delay_;
    }
  }

private:
  std::array<engine::StereoFrame, 11> history_{};
  std::size_t delay_;
  float gain_;
  std::shared_ptr<std::atomic<bool>> fault_;
  std::size_t cursor_ = 0;
};

class TwoBoundEffects final : public app::TrackInsertProvider {
public:
  TwoBoundEffects(project::PluginInstanceId first, project::PluginInstanceId second)
      : first_(first), second_(second) {}

  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t max_frames) override {
    using Result = foundation::Result<app::PreparedTrackInsert>;
    if (snapshot.sample_rate != 48'000 || (max_frames != 64 && max_frames != 128))
      return Result::failure({foundation::ErrorCode::invalid_argument, "unexpected block format"});
    const auto first = saved.id == first_;
    if (!first && saved.id != second_)
      return Result::failure({foundation::ErrorCode::not_found, "unknown saved plugin identity"});
    ++preparations[first ? 0 : 1];
    if (!first && reject_second)
      return Result::failure({foundation::ErrorCode::unavailable, "second receipt unavailable"});
    auto runtime = std::make_shared<DelayGain>(
        DelayGain::Settings{first ? 7U : 11U, first ? 0.5F : 0.25F}, faults[first ? 0 : 1]);
    owners[first ? 0 : 1] = runtime;
    return Result::success(
        {std::move(runtime), first ? 7U : 11U, [fault = faults[first ? 0 : 1]]() noexcept {
           return fault->load(std::memory_order_acquire);
         }});
  }

  bool reject_second = false;
  std::array<std::size_t, 2> preparations{};
  std::array<std::weak_ptr<engine::ProcessorRuntime>, 2> owners{};
  std::array<std::shared_ptr<std::atomic<bool>>, 2> faults{
      std::make_shared<std::atomic<bool>>(false), std::make_shared<std::atomic<bool>>(false)};

private:
  project::PluginInstanceId first_;
  project::PluginInstanceId second_;
};

class SerialAffineDelay final : public engine::ProcessorRuntime {
public:
  struct Settings {
    std::size_t delay;
    float gain;
    float offset;
  };

  explicit SerialAffineDelay(Settings settings)
      : delay_(settings.delay), gain_(settings.gain), offset_(settings.offset) {}

  void process(std::span<engine::StereoFrame> stereo, engine::BlockContext) noexcept override {
    for (auto& frame : stereo) {
      const auto input = frame;
      frame = history_[cursor_];
      if (input.left != 0.0F || input.right != 0.0F)
        history_[cursor_] = {input.left * gain_ + offset_, input.right * gain_ - offset_};
      else
        history_[cursor_] = {};
      cursor_ = (cursor_ + 1) % delay_;
    }
    ++blocks_;
  }

  [[nodiscard]] std::size_t blocks() const noexcept { return blocks_; }

private:
  std::array<engine::StereoFrame, 11> history_{};
  std::size_t delay_;
  float gain_;
  float offset_;
  std::size_t cursor_ = 0;
  std::size_t blocks_ = 0;
};

class SerialAffineProvider final : public app::TrackInsertProvider {
public:
  SerialAffineProvider(project::PluginInstanceId first, project::PluginInstanceId second)
      : first_(first), second_(second) {}

  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t max_frames) override {
    using Result = foundation::Result<app::PreparedTrackInsert>;
    if (snapshot.sample_rate != 48'000 || (max_frames != 64 && max_frames != 128))
      return Result::failure({foundation::ErrorCode::invalid_argument, "unexpected block format"});
    const bool first = saved.id == first_;
    if (!first && saved.id != second_)
      return Result::failure({foundation::ErrorCode::not_found, "unknown saved effect"});
    const auto index = first ? 0U : 1U;
    ++preparations[index];
    if (reject_second && !first)
      return Result::failure({foundation::ErrorCode::unavailable, "second binding unavailable"});
    auto runtime = std::make_shared<SerialAffineDelay>(
        SerialAffineDelay::Settings{first ? 7U : 11U, first ? 2.0F : 3.0F, first ? 1.0F : 4.0F});
    owners[index] = runtime;
    return Result::success({std::move(runtime), first ? 7U : 11U, [fault = faults[index]] {
                              return fault->load(std::memory_order_acquire);
                            }});
  }

  bool reject_second = false;
  std::array<std::size_t, 2> preparations{};
  std::array<std::weak_ptr<SerialAffineDelay>, 2> owners{};
  std::array<std::shared_ptr<std::atomic<bool>>, 2> faults{
      std::make_shared<std::atomic<bool>>(false), std::make_shared<std::atomic<bool>>(false)};

private:
  project::PluginInstanceId first_;
  project::PluginInstanceId second_;
};

project::Vst3PluginInstance saved_fixture_insert() {
  return {id<project::PluginInstanceId>(10),
          std::string(32, 'a'),
          "Fixture Vendor",
          "Gain",
          "1.0",
          "/tmp/Gain.vst3",
          std::string(64, 'b'),
          {0, 1, 2, 3},
          {},
          false};
}

void failed_two_effect_publication_keeps_owners_uncommitted() {
  Gate gate;
  auto state = model(gate);
  const auto first_track = id<project::TrackId>(2);
  const auto second_track = id<project::TrackId>(3);
  auto first = saved_fixture_insert();
  auto second = first;
  second.id = id<project::PluginInstanceId>(11);
  (void)take(state.apply({project::AddTrack{first_track}, project::AddTrack{second_track},
                          project::InsertVst3Plugin{first_track, 0, first},
                          project::InsertVst3Plugin{second_track, 0, second}}));
  auto bounded = limits();
  bounded.max_buffer_bytes = 1;
  TwoBoundEffects provider(first.id, second.id);
  app::RoutedPlaybackControl control(state, bounded, &provider);
  const auto failed = control.submit_current();
  REQUIRE_TRUE(!failed, "failed two-effect graph publication committed partial runtime ownership");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{1, 1}),
             "failed two-effect graph publication committed partial runtime ownership");
  REQUIRE_TRUE(provider.owners[0].expired(),
               "failed two-effect graph publication committed partial runtime ownership");
  REQUIRE_TRUE(provider.owners[1].expired(),
               "failed two-effect graph publication committed partial runtime ownership");
  REQUIRE_TRUE(control.source_layout().empty(),
               "failed two-effect graph publication committed partial runtime ownership");
  REQUIRE_EQ(control.status().active_graph_revision, 0,
             "failed two-effect graph publication committed partial runtime ownership");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "failed two-effect graph publication committed partial runtime ownership");
}

// Without a VST3 host no chain can prepare a VST3 stage; the saved insert
// fails and the prior graph keeps playing.
void live_insert_without_vst3_host_keeps_prior_graph() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(2);
  (void)take(state.apply({project::AddTrack{track}}));
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "empty live route was not prepared");
  std::array<engine::StereoFrame, 4> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "empty live route was not activated");
  control.observe();
  const auto old_revision = control.status().active_graph_revision;
  const auto changed =
      take(state.apply({project::InsertVst3Plugin{track, 0, saved_fixture_insert()}}));
  REQUIRE_OK(control.begin_edit(changed, "insert effect"),
             "VST3 insert without a host activated or discarded prior audio");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::holding,
             "VST3 insert without a host activated or discarded prior audio");
  REQUIRE_TRUE(!control.publish_held(),
               "VST3 insert without a host activated or discarded prior audio");
  REQUIRE_EQ(control.status().active_graph_revision, old_revision,
             "VST3 insert without a host activated or discarded prior audio");
  REQUIRE_EQ(control.status().model_revision, changed.snapshot->revision,
             "VST3 insert without a host activated or discarded prior audio");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "VST3 insert without a host activated or discarded prior audio");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::rendered,
             "VST3 insert without a host activated or discarded prior audio");
  near(output[0], 0.25F);
}

class PulseInstrument final : public engine::ProcessorRuntime {
public:
  void process(std::span<engine::StereoFrame> frames, engine::BlockContext) noexcept override {
    if (!frames.empty())
      frames.front() = {1.0F, -1.0F};
    ++blocks;
  }
  std::size_t blocks = 0;
};

class MixedRoleProvider final : public app::TrackInsertProvider {
public:
  struct Roles {
    project::PluginInstanceId instrument;
    project::PluginInstanceId effect;
  };
  explicit MixedRoleProvider(Roles roles)
      : instrument_id(roles.instrument), effect_id(roles.effect) {}

  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t frames) override {
    using Result = foundation::Result<app::PreparedTrackInsert>;
    if (snapshot.sample_rate != 48'000 || (frames != 64 && frames != 128))
      return Result::failure({foundation::ErrorCode::invalid_argument, "wrong block format"});
    if (saved.id == instrument_id) {
      ++instrument_preparations;
      auto runtime = std::make_shared<PulseInstrument>();
      instrument_owner = runtime;
      return Result::success({std::move(runtime), 0, [] { return false; }, true});
    }
    if (saved.id != effect_id)
      return Result::failure({foundation::ErrorCode::not_found, "unknown processor"});
    ++effect_preparations;
    if (reject_effect)
      return Result::failure({foundation::ErrorCode::unavailable, "effect receipt unavailable"});
    auto runtime = std::make_shared<DelayGain>(DelayGain::Settings{7, 0.5F}, effect_fault);
    effect_owner = runtime;
    return Result::success({std::move(runtime), 7, [fault = effect_fault] {
                              return fault->load(std::memory_order_acquire);
                            }});
  }

  project::PluginInstanceId instrument_id;
  project::PluginInstanceId effect_id;
  bool reject_effect = false;
  std::size_t instrument_preparations = 0;
  std::size_t effect_preparations = 0;
  std::weak_ptr<PulseInstrument> instrument_owner;
  std::weak_ptr<DelayGain> effect_owner;
  std::shared_ptr<std::atomic<bool>> effect_fault = std::make_shared<std::atomic<bool>>(false);
};

class TwoInstrumentEffects final : public app::TrackInsertProvider {
public:
  TwoInstrumentEffects(project::PluginInstanceId source, project::PluginInstanceId first,
                       project::PluginInstanceId second)
      : ids_{source, first, second} {}

  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t frames) override {
    using Result = foundation::Result<app::PreparedTrackInsert>;
    if (snapshot.sample_rate != 48'000 || (frames != 64 && frames != 128))
      return Result::failure({foundation::ErrorCode::invalid_argument, "wrong block format"});
    if (saved.id == ids_[0]) {
      ++preparations[0];
      auto runtime = std::make_shared<PulseInstrument>();
      source = runtime;
      return Result::success({std::move(runtime), 0, [] { return false; }, true});
    }
    const auto stage = saved.id == ids_[1] ? 1U : saved.id == ids_[2] ? 2U : 3U;
    if (stage == 3)
      return Result::failure({foundation::ErrorCode::not_found, "unknown processor"});
    ++preparations[stage];
    auto runtime = std::make_shared<SerialAffineDelay>(SerialAffineDelay::Settings{
        stage == 1 ? 7U : 11U, stage == 1 ? 2.0F : 3.0F, stage == 1 ? 1.0F : 4.0F});
    effects[stage - 1] = runtime;
    return Result::success({std::move(runtime), stage == 1 ? 7U : 11U,
                            [fault = faults[stage - 1]] { return fault->load(); }});
  }

  std::array<std::size_t, 3> preparations{};
  std::weak_ptr<PulseInstrument> source;
  std::array<std::weak_ptr<SerialAffineDelay>, 2> effects{};
  std::array<std::shared_ptr<std::atomic<bool>>, 2> faults{
      std::make_shared<std::atomic<bool>>(false), std::make_shared<std::atomic<bool>>(false)};

private:
  std::array<project::PluginInstanceId, 3> ids_;
};

void instrument_two_serial_effects_keep_order_and_history(std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto instrument = id<project::TrackId>(2'401);
  const auto audio = id<project::TrackId>(2'402);
  const auto returns = id<project::TrackId>(2'403);
  const auto send = id<project::SendId>(2'404);
  auto source = saved_fixture_insert();
  source.id = id<project::PluginInstanceId>(2'405);
  auto first = source;
  first.id = id<project::PluginInstanceId>(2'406);
  auto second = source;
  second.id = id<project::PluginInstanceId>(2'407);
  const auto initial = take(state.apply(
      {project::AddTrack{instrument, project::TrackKind::instrument}, project::AddTrack{audio},
       project::AddTrack{returns, project::TrackKind::return_bus},
       project::AddSendRoute{audio, {send, returns, 0.0}},
       project::InsertVst3Plugin{instrument, 0, source},
       project::InsertVst3Plugin{instrument, 0, first},
       project::InsertVst3Plugin{instrument, 1, second}}));
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes *= block_frames / 64;
  bounded.max_history_bytes *= block_frames / 64;
  TwoInstrumentEffects provider(source.id, first.id, second.id);
  app::RoutedPlaybackControl control(state, bounded, &provider);
  control.set_live_switching(false); // covers held switching
  require(control.submit_current() && control.master_output_latency_frames() == 18 &&
              provider.preparations == std::array<std::size_t, 3>{1, 1, 1},
          "instrument and two effects did not publish one compensated serial path");
  const auto source_owner = provider.source.lock();
  const auto first_owner = provider.effects[0].lock();
  const auto second_owner = provider.effects[1].lock();
  REQUIRE_TRUE(source_owner, "instrument serial processor ownership was incomplete");
  REQUIRE_TRUE(first_owner, "instrument serial processor ownership was incomplete");
  REQUIRE_TRUE(second_owner, "instrument serial processor ownership was incomplete");
  std::array<engine::StereoFrame, 128> dry{}, silence{}, output{};
  dry[0] = {0.25F, -0.25F};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), instrument, std::span(silence).first(block_frames));
  set_source(inputs, control.source_layout(), audio, std::span(dry).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto status = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(status, engine::RoutedRenderStatus::rendered,
             "instrument serial callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "instrument serial callback failed or allocated");
  near(output[18], 13.25F); // Dry 0.25 plus (1 * 2 + 1) * 3 + 4.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "instrument serial graph revision did not activate");
  const auto revised = take(state.apply({project::SetSendGain{send, 0.25}}));
  REQUIRE_TRUE(control.begin_edit(revised, "send gain"),
               "instrument serial path could not publish held route edit");
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1}),
             engine::RoutedRenderStatus::holding,
             "instrument serial path could not publish held route edit");
  REQUIRE_TRUE(control.publish_held(), "instrument serial path could not publish held route edit");
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "held instrument route edit reset a processor or allocated");
  REQUIRE_EQ(counted_allocations, 0, "held instrument route edit reset a processor or allocated");
  REQUIRE_EQ(provider.source.lock(), source_owner,
             "held instrument route edit reset a processor or allocated");
  REQUIRE_EQ(provider.effects[0].lock(), first_owner,
             "held instrument route edit reset a processor or allocated");
  REQUIRE_EQ(provider.effects[1].lock(), second_owner,
             "held instrument route edit reset a processor or allocated");
  REQUIRE_EQ(source_owner->blocks, 2, "held instrument route edit reset a processor or allocated");
}

class TwoSourceEffects final : public app::TrackInsertProvider {
public:
  explicit TwoSourceEffects(std::array<project::PluginInstanceId, 4> ids) : ids_(ids) {}

  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t frames) override {
    using Result = foundation::Result<app::PreparedTrackInsert>;
    if (snapshot.sample_rate != 48'000 || (frames != 64 && frames != 128))
      return Result::failure({foundation::ErrorCode::invalid_argument, "wrong block format"});
    const auto found = std::ranges::find(ids_, saved.id);
    if (found == ids_.end())
      return Result::failure({foundation::ErrorCode::not_found, "unknown processor"});
    const auto index = static_cast<std::size_t>(found - ids_.begin());
    ++preparations[index];
    if (reject_second_effect && index == 3)
      return Result::failure({foundation::ErrorCode::unavailable, "second effect unavailable"});
    if (index < 2) {
      auto runtime = std::make_shared<PulseInstrument>();
      sources[index] = runtime;
      app::PreparedTrackInsert prepared{};
      prepared.processor = std::move(runtime);
      prepared.process_failed = [] { return false; };
      prepared.instrument_source = true;
      return Result::success(std::move(prepared));
    }
    auto runtime = std::make_shared<DelayGain>(
        DelayGain::Settings{index == 2 ? 7U : 11U, index == 2 ? 0.5F : 0.25F}, faults[index - 2]);
    effects[index - 2] = runtime;
    app::PreparedTrackInsert prepared{};
    prepared.processor = std::move(runtime);
    prepared.latency_frames = index == 2 ? 7U : 11U;
    prepared.process_failed = [fault = faults[index - 2]] {
      return fault->load(std::memory_order_acquire);
    };
    return Result::success(std::move(prepared));
  }

  bool reject_second_effect = false;
  std::array<std::size_t, 4> preparations{};
  std::array<std::weak_ptr<PulseInstrument>, 2> sources{};
  std::array<std::weak_ptr<DelayGain>, 2> effects{};
  std::array<std::shared_ptr<std::atomic<bool>>, 2> faults{
      std::make_shared<std::atomic<bool>>(false), std::make_shared<std::atomic<bool>>(false)};

private:
  std::array<project::PluginInstanceId, 4> ids_;
};

void two_instrument_effect_paths_keep_independent_state(std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto first_track = id<project::TrackId>(2'451);
  const auto second_track = id<project::TrackId>(2'452);
  const auto bus = id<project::TrackId>(2'453);
  const auto send = id<project::SendId>(2'454);
  auto first_source = saved_fixture_insert();
  first_source.id = id<project::PluginInstanceId>(2'455);
  auto second_source = first_source;
  second_source.id = id<project::PluginInstanceId>(2'456);
  auto first_effect = first_source;
  first_effect.id = id<project::PluginInstanceId>(2'457);
  auto second_effect = first_source;
  second_effect.id = id<project::PluginInstanceId>(2'458);
  const auto initial =
      take(state.apply({project::AddTrack{first_track, project::TrackKind::instrument},
                        project::AddTrack{second_track, project::TrackKind::instrument},
                        project::AddTrack{bus, project::TrackKind::return_bus},
                        project::AddSendRoute{first_track, {send, bus, 0.0}},
                        project::InsertVst3Plugin{first_track, 0, first_source},
                        project::InsertVst3Plugin{first_track, 0, first_effect},
                        project::InsertVst3Plugin{second_track, 0, second_source},
                        project::InsertVst3Plugin{second_track, 0, second_effect}}));
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes *= block_frames / 64;
  bounded.max_history_bytes *= block_frames / 64;
  TwoSourceEffects refused({first_source.id, second_source.id, first_effect.id, second_effect.id});
  refused.reject_second_effect = true;
  app::RoutedPlaybackControl rejected(state, bounded, &refused);
  rejected.set_live_switching(false); // covers held switching
  require(!rejected.submit_current() &&
              refused.preparations == std::array<std::size_t, 4>{1, 1, 1, 1} &&
              refused.sources[0].expired() && refused.sources[1].expired() &&
              refused.effects[0].expired() && refused.effects[1].expired() &&
              rejected.status().active_graph_revision == 0,
          "failed second effect preparation retained or published half a paired graph");
  TwoSourceEffects provider({first_source.id, second_source.id, first_effect.id, second_effect.id});
  app::RoutedPlaybackControl control(state, bounded, &provider);
  control.set_live_switching(false); // covers held switching
  require(control.submit_current() && control.master_output_latency_frames() == 11 &&
              provider.preparations == std::array<std::size_t, 4>{1, 1, 1, 1},
          "two source/effect paths did not prepare independently with PDC");
  const auto first_source_owner = provider.sources[0].lock();
  const auto second_source_owner = provider.sources[1].lock();
  const auto first_effect_owner = provider.effects[0].lock();
  const auto second_effect_owner = provider.effects[1].lock();
  REQUIRE_TRUE(first_source_owner, "paired source/effect paths shared or lost runtime ownership");
  REQUIRE_TRUE(second_source_owner, "paired source/effect paths shared or lost runtime ownership");
  REQUIRE_TRUE(first_effect_owner, "paired source/effect paths shared or lost runtime ownership");
  REQUIRE_TRUE(second_effect_owner, "paired source/effect paths shared or lost runtime ownership");
  REQUIRE_NE(first_source_owner, second_source_owner,
             "paired source/effect paths shared or lost runtime ownership");
  REQUIRE_NE(first_effect_owner, second_effect_owner,
             "paired source/effect paths shared or lost runtime ownership");
  std::array<engine::StereoFrame, 128> silence{}, output{};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), first_track, std::span(silence).first(block_frames));
  set_source(inputs, control.source_layout(), second_track, std::span(silence).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "paired source/effect callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "paired source/effect callback failed or allocated");
  REQUIRE_TRUE(!control.processor_fault(), "paired source/effect callback failed or allocated");
  near(output[10], 0.0F);
  near(output[11], 0.75F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "paired source/effect graph revision was not acknowledged");
  const auto revised = take(state.apply({project::SetSendGain{send, 0.25}}));
  REQUIRE_TRUE(control.begin_edit(revised, "paired route gain"),
               "paired source/effect paths could not publish a held routing edit");
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1}),
             engine::RoutedRenderStatus::holding,
             "paired source/effect paths could not publish a held routing edit");
  REQUIRE_TRUE(control.publish_held(),
               "paired source/effect paths could not publish a held routing edit");
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  REQUIRE_EQ(counted_allocations, 0,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  REQUIRE_EQ(provider.sources[0].lock(), first_source_owner,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  REQUIRE_EQ(provider.sources[1].lock(), second_source_owner,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  REQUIRE_EQ(provider.effects[0].lock(), first_effect_owner,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  REQUIRE_EQ(provider.effects[1].lock(), second_effect_owner,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  REQUIRE_EQ(first_source_owner->blocks, 2,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  REQUIRE_EQ(second_source_owner->blocks, 2,
             "held paired-route edit rebuilt a processor, lost history or allocated");
  near(output[11], 0.875F); // First path sends another 0.5 * 0.25 after its effect.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "held paired-route edit did not activate the saved revision");
  provider.faults[1]->store(true, std::memory_order_release);
  counted_allocations = 0;
  counting_allocations = true;
  const auto faulted =
      control.render_block(inputs, std::span(output).first(block_frames), {2 * block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(faulted, engine::RoutedRenderStatus::rendered,
             "second effect fault escaped its owning path or allocated");
  REQUIRE_EQ(counted_allocations, 0, "second effect fault escaped its owning path or allocated");
  REQUIRE_TRUE(control.processor_fault(second_track),
               "second effect fault escaped its owning path or allocated");
  REQUIRE_TRUE(!control.processor_fault(first_track),
               "second effect fault escaped its owning path or allocated");
  near(output[11], 0.625F);
  provider.faults[1]->store(false, std::memory_order_release);
  const auto third = take(state.apply(
      {project::AddTrack{id<project::TrackId>(2'459), project::TrackKind::instrument}}));
  REQUIRE_TRUE(control.begin_edit(third, "third path"),
               "third instrument path displaced active paired graph");
  REQUIRE_EQ(
      control.render_block(inputs, std::span(output).first(block_frames), {3 * block_frames, 1}),
      engine::RoutedRenderStatus::holding, "third instrument path displaced active paired graph");
  REQUIRE_TRUE(!control.publish_held(), "third instrument path displaced active paired graph");
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "third instrument path displaced active paired graph");
}

void mixed_instrument_and_effect_preserves_both_nodes(bool effect_first) {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(effect_first ? 2 : 3);
  const auto instrument = id<project::TrackId>(effect_first ? 3 : 2);
  const auto returns = id<project::TrackId>(4);
  const auto send = id<project::SendId>(5);
  auto first = saved_fixture_insert();
  auto second = first;
  second.id = id<project::PluginInstanceId>(11);
  std::vector<project::Operation> operations;
  if (effect_first)
    operations.emplace_back(project::AddTrack{audio});
  operations.emplace_back(project::AddTrack{instrument, project::TrackKind::instrument});
  if (!effect_first)
    operations.emplace_back(project::AddTrack{audio});
  operations.emplace_back(project::AddTrack{returns, project::TrackKind::return_bus});
  operations.emplace_back(project::AddSendRoute{audio, {send, returns, 0.0}});
  operations.emplace_back(project::InsertVst3Plugin{audio, 0, first});
  operations.emplace_back(project::InsertVst3Plugin{instrument, 0, second});
  const auto original = take(state.apply(operations));
  MixedRoleProvider provider({.instrument = second.id, .effect = first.id});
  provider.reject_effect = true;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  control.set_live_switching(false); // covers held switching
  const auto failed = control.submit_current();
  REQUIRE_TRUE(!failed,
               "failed mixed preparation retained an instrument or published a partial graph");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::unavailable,
             "failed mixed preparation retained an instrument or published a partial graph");
  REQUIRE_EQ(control.status().active_graph_revision, 0,
             "failed mixed preparation retained an instrument or published a partial graph");
  REQUIRE_TRUE(provider.instrument_owner.expired(),
               "failed mixed preparation retained an instrument or published a partial graph");
  REQUIRE_TRUE(provider.effect_owner.expired(),
               "failed mixed preparation retained an instrument or published a partial graph");
  provider.reject_effect = false;
  REQUIRE_TRUE(control.retry(),
               "mixed instrument/effect route did not recover or bind the MIDI source");
  REQUIRE_EQ(control.bound_instrument_track(), instrument,
             "mixed instrument/effect route did not recover or bind the MIDI source");
  REQUIRE_EQ(control.source_layout().size(), 2,
             "mixed instrument/effect route did not recover or bind the MIDI source");
  const auto instrument_owner = provider.instrument_owner.lock();
  const auto effect_owner = provider.effect_owner.lock();
  REQUIRE_TRUE(instrument_owner, "mixed processor owners were not retained");
  REQUIRE_TRUE(effect_owner, "mixed processor owners were not retained");
  std::array<engine::StereoFrame, 64> signal{}, silence{}, output{};
  signal[0] = {0.25F, -0.25F};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  set_source(inputs, control.source_layout(), instrument, silence);
  counted_allocations = 0;
  counting_allocations = true;
  const auto first_render = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first_render, engine::RoutedRenderStatus::rendered,
             "mixed callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "mixed callback failed or allocated");
  near(output[0], 0.0F);
  near(output[7], 1.125F); // PDC aligns the instrument pulse to the seven-frame effect.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, original.snapshot->revision,
             "mixed route did not activate the saved project revision");

  const auto revised = take(state.apply({project::SetSendGain{send, 0.25}}));
  REQUIRE_TRUE(control.begin_edit(revised, "route edit"),
               "mixed route could not publish a held routing revision");
  REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::holding,
             "mixed route could not publish a held routing revision");
  REQUIRE_TRUE(control.publish_held(), "mixed route could not publish a held routing revision");
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "held mixed route reset a processor or allocated on callback");
  REQUIRE_EQ(counted_allocations, 0, "held mixed route reset a processor or allocated on callback");
  REQUIRE_EQ(provider.instrument_owner.lock(), instrument_owner,
             "held mixed route reset a processor or allocated on callback");
  REQUIRE_EQ(provider.effect_owner.lock(), effect_owner,
             "held mixed route reset a processor or allocated on callback");
  REQUIRE_EQ(provider.instrument_preparations, (effect_first ? 1U : 2U),
             "held mixed route reset a processor or allocated on callback");
  REQUIRE_EQ(provider.effect_preparations, 2,
             "held mixed route reset a processor or allocated on callback");
  REQUIRE_EQ(instrument_owner->blocks, 2,
             "held mixed route reset a processor or allocated on callback");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "held mixed route revision did not activate");

  provider.effect_fault->store(true, std::memory_order_release);
  REQUIRE_TRUE(control.processor_fault(audio),
               "effect fault was not isolated from the bound MIDI instrument");
  REQUIRE_TRUE(!control.processor_fault(instrument),
               "effect fault was not isolated from the bound MIDI instrument");
  REQUIRE_TRUE(control.processor_fault(),
               "effect fault was not isolated from the bound MIDI instrument");
  provider.effect_fault->store(false, std::memory_order_release);
  // A new effect state prepares a new instance; the instrument keeps running.
  const auto changed = take(state.apply({project::SetVst3ComponentState{first.id, {9, 8}}}));
  REQUIRE_TRUE(control.begin_edit(changed, "effect state"),
               "held effect state change did not publish");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::holding,
             "held effect state change did not publish");
  REQUIRE_TRUE(control.publish_held(), "held effect state change did not publish");
  counted_allocations = 0;
  counting_allocations = true;
  const auto replaced = control.render_block(inputs, output, {128, 1});
  counting_allocations = false;
  control.observe();
  REQUIRE_EQ(replaced, engine::RoutedRenderStatus::rendered,
             "effect state change kept the old instance or restarted the instrument");
  REQUIRE_EQ(counted_allocations, 0,
             "effect state change kept the old instance or restarted the instrument");
  REQUIRE_EQ(provider.effect_preparations, 3,
             "effect state change kept the old instance or restarted the instrument");
  REQUIRE_NE(provider.effect_owner.lock(), effect_owner,
             "effect state change kept the old instance or restarted the instrument");
  REQUIRE_EQ(provider.instrument_owner.lock(), instrument_owner,
             "effect state change kept the old instance or restarted the instrument");
  REQUIRE_EQ(provider.instrument_preparations, (effect_first ? 1U : 2U),
             "effect state change kept the old instance or restarted the instrument");
  REQUIRE_EQ(instrument_owner->blocks, 3,
             "effect state change kept the old instance or restarted the instrument");
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "effect state change kept the old instance or restarted the instrument");
}

void synth_track_native_effect_processes_source_without_allocations() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(1'515);
  (void)take(state.apply(
      {project::AddTrack{track, project::TrackKind::instrument},
       project::SetNativeInstrumentPatch{track, project::NativeInstrumentPatch{"prism", 1, {}}}}));
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  const auto submitted = control.submit_current();
  REQUIRE_OK(submitted, submitted ? "" : submitted.error().message);
  const std::array<native_dsp::EqBandSettings, 1> settings{
      {{native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}};
  native_dsp::EqRuntime oracle(take(native_dsp::prepare_eq(settings, 48'000)));
  std::array<engine::StereoFrame, 64> signal{}, expected{}, output{};
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  signal[0] = {1.0F, -1.0F};
  expected = signal;
  set_source(inputs, control.source_layout(), track, signal);
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "synth track native effect failed or allocated in callback");
  REQUIRE_EQ(counted_allocations, 0, "synth track native effect failed or allocated in callback");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], expected[frame].left);
  control.observe();
  const auto first =
      take(state.apply({project::AddNativeEffect{track, 0, shape_patch(low_pass_1k)}}));
  REQUIRE_TRUE(control.begin_edit(first, "add first synth EQ"),
               "synth first native-stage addition did not publish");
  REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::holding,
             "synth first native-stage addition did not publish");
  REQUIRE_TRUE(control.publish_held(), "synth first native-stage addition did not publish");
  signal.fill({});
  signal.front() = {0.5F, -0.5F};
  expected = signal;
  oracle.process(expected, {64, 1});
  counted_allocations = 0;
  counting_allocations = true;
  const auto after_first = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(after_first, engine::RoutedRenderStatus::rendered,
             "synth first native-stage callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "synth first native-stage callback failed or allocated");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], expected[frame].left);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, first.snapshot->revision,
             "synth first native-stage addition left its saved revision inactive");
  const native_dsp::CompressorSettings unity{0.0, 4.0, 0.0, 0.1, 100.0, 0.0};
  const auto added = take(state.apply({project::AddNativeEffect{track, 1, press_patch(unity)}}));
  REQUIRE_TRUE(control.begin_edit(added, "add synth compressor"),
               "synth final-stage addition did not publish");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::holding,
             "synth final-stage addition did not publish");
  REQUIRE_TRUE(control.publish_held(), "synth final-stage addition did not publish");
  signal.fill({});
  signal.front() = {0.5F, -0.5F};
  expected = signal;
  oracle.process(expected, {128, 1});
  counted_allocations = 0;
  counting_allocations = true;
  const auto after_add = control.render_block(inputs, output, {128, 1});
  counting_allocations = false;
  REQUIRE_EQ(after_add, engine::RoutedRenderStatus::rendered,
             "synth final-stage callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "synth final-stage callback failed or allocated");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], expected[frame].left);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, added.snapshot->revision,
             "synth final-stage addition left its saved revision inactive");
  const auto final_slot = added.snapshot->tracks.front().effects.back().id;
  const auto removed_final = take(state.apply({project::RemoveEffectInsert{track, final_slot}}));
  REQUIRE_TRUE(control.begin_edit(removed_final, "remove synth final compressor"),
               "synth final-stage removal did not publish");
  REQUIRE_EQ(control.render_block(inputs, output, {192, 1}), engine::RoutedRenderStatus::holding,
             "synth final-stage removal did not publish");
  REQUIRE_TRUE(control.publish_held(), "synth final-stage removal did not publish");
  expected = signal;
  oracle.process(expected, {192, 1});
  counted_allocations = 0;
  counting_allocations = true;
  const auto after_final_remove = control.render_block(inputs, output, {192, 1});
  counting_allocations = false;
  REQUIRE_EQ(after_final_remove, engine::RoutedRenderStatus::rendered,
             "synth final-stage removal callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "synth final-stage removal callback failed or allocated");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], expected[frame].left);
  control.observe();
  const auto first_slot = removed_final.snapshot->tracks.front().effects.front().id;
  const auto removed_first = take(state.apply({project::RemoveEffectInsert{track, first_slot}}));
  REQUIRE_TRUE(control.begin_edit(removed_first, "remove first synth EQ"),
               "synth first-stage removal did not publish");
  REQUIRE_EQ(control.render_block(inputs, output, {256, 1}), engine::RoutedRenderStatus::holding,
             "synth first-stage removal did not publish");
  REQUIRE_TRUE(control.publish_held(), "synth first-stage removal did not publish");
  expected.fill({});
  oracle.process(expected, {256, 1});
  for (std::size_t frame = 0; frame < expected.size(); ++frame) {
    expected[frame].left += signal[frame].left;
    expected[frame].right += signal[frame].right;
  }
  counted_allocations = 0;
  counting_allocations = true;
  const auto after_first_remove = control.render_block(inputs, output, {256, 1});
  counting_allocations = false;
  REQUIRE_EQ(after_first_remove, engine::RoutedRenderStatus::rendered,
             "synth first-stage removal callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "synth first-stage removal callback failed or allocated");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], expected[frame].left);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, removed_first.snapshot->revision,
             "synth first-stage removal left its saved revision inactive");
}

void two_verified_effects_align_and_survive_revisions(std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto first_track = id<project::TrackId>(2);
  const auto second_track = id<project::TrackId>(3);
  const auto returns = id<project::TrackId>(4);
  const auto send = id<project::SendId>(5);
  auto first = saved_fixture_insert();
  auto second = first;
  second.id = id<project::PluginInstanceId>(11);
  (void)take(state.apply({project::AddTrack{first_track}, project::AddTrack{second_track},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::InsertVst3Plugin{first_track, 0, first},
                          project::InsertVst3Plugin{second_track, 0, second},
                          project::AddSendRoute{first_track, {send, returns, 0.0}}}));
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes = 8ULL * block_frames * sizeof(engine::StereoFrame);
  bounded.max_history_bytes = 4ULL * 65 * sizeof(engine::StereoFrame);
  TwoBoundEffects provider(first.id, second.id);
  provider.reject_second = true;
  app::RoutedPlaybackControl control(state, bounded, &provider);
  control.set_live_switching(false); // covers held switching
  const auto failed_initial = control.submit_current();
  REQUIRE_TRUE(!failed_initial,
               "failed second preparation published a graph or retained a first owner");
  REQUIRE_NE(failed_initial.error().message.find(second_track.to_string()), std::string::npos,
             "failed second preparation published a graph or retained a first owner");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{1, 1}),
             "failed second preparation published a graph or retained a first owner");
  REQUIRE_TRUE(provider.owners[0].expired(),
               "failed second preparation published a graph or retained a first owner");
  REQUIRE_TRUE(!control.processor_fault(),
               "failed second preparation published a graph or retained a first owner");
  REQUIRE_EQ(control.status().active_graph_revision, 0,
             "failed second preparation published a graph or retained a first owner");
  provider.reject_second = false;
  REQUIRE_OK(control.retry(), "two saved effect bindings did not retry independently");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{2, 2}),
             "two saved effect bindings did not retry independently");
  REQUIRE_EQ(control.source_layout().size(), 2,
             "two saved effect bindings did not retry independently");
  const auto first_owner = provider.owners[0].lock();
  const auto second_owner = provider.owners[1].lock();
  REQUIRE_TRUE(first_owner, "two saved effects did not retain distinct runtime owners");
  REQUIRE_TRUE(second_owner, "two saved effects did not retain distinct runtime owners");
  REQUIRE_NE(first_owner, second_owner, "two saved effects did not retain distinct runtime owners");

  std::array<engine::StereoFrame, 128> first_signal{}, second_signal{}, output{};
  first_signal[0] = {1.0F, -1.0F};
  second_signal[0] = {1.0F, -1.0F};
  first_signal[block_frames - 1] = {1.0F, -1.0F};
  second_signal[block_frames - 1] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), first_track,
             std::span(first_signal).first(block_frames));
  set_source(inputs, control.source_layout(), second_track,
             std::span(second_signal).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto initial = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(initial, engine::RoutedRenderStatus::rendered,
             "two-effect callback allocated or failed to render");
  REQUIRE_EQ(counted_allocations, 0, "two-effect callback allocated or failed to render");
  near(output[10], 0.0F);
  near(output[11], 0.75F); // 7-frame 0.5 gain plus 11-frame 0.25 gain, aligned by PDC.
  require(!control.processor_fault(first_track) && !control.processor_fault(second_track),
          "healthy effects reported per-track faults");
  control.observe();

  const auto revised = take(state.apply({project::SetSendGain{send, 0.5}}));
  REQUIRE_OK(control.begin_edit(revised, "send gain"),
             "two-effect held graph update did not publish");
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1}),
             engine::RoutedRenderStatus::holding, "two-effect held graph update did not publish");
  REQUIRE_OK(control.publish_held(), "two-effect held graph update did not publish");
  first_signal.fill({});
  second_signal.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "held graph edit recreated an effect or allocated on callback");
  REQUIRE_EQ(counted_allocations, 0,
             "held graph edit recreated an effect or allocated on callback");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{2, 2}),
             "held graph edit recreated an effect or allocated on callback");
  REQUIRE_EQ(provider.owners[0].lock(), first_owner,
             "held graph edit recreated an effect or allocated on callback");
  REQUIRE_EQ(provider.owners[1].lock(), second_owner,
             "held graph edit recreated an effect or allocated on callback");
  near(output[10], 1.0F); // Prior-block tails retain both runtimes across publication.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "revised two-effect graph did not activate");

  // A new state prepares a new second instance; the first keeps running.
  const auto changed = take(state.apply({project::SetVst3ComponentState{second.id, {9, 8, 7, 6}}}));
  REQUIRE_OK(control.begin_edit(changed, "second effect state"),
             "changed second state did not publish at the held boundary");
  REQUIRE_EQ(
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames * 2, 1}),
      engine::RoutedRenderStatus::holding,
      "changed second state did not publish at the held boundary");
  REQUIRE_OK(control.publish_held(), "changed second state did not publish at the held boundary");
  first_signal[0] = {1.0F, -1.0F};
  second_signal[0] = {1.0F, -1.0F};
  counted_allocations = 0;
  counting_allocations = true;
  const auto replaced =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames * 2, 1});
  counting_allocations = false;
  REQUIRE_EQ(replaced, engine::RoutedRenderStatus::rendered,
             "changed second state reused its old instance, replaced the first or allocated");
  REQUIRE_EQ(counted_allocations, 0,
             "changed second state reused its old instance, replaced the first or allocated");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{2, 3}),
             "changed second state reused its old instance, replaced the first or allocated");
  REQUIRE_EQ(provider.owners[0].lock(), first_owner,
             "changed second state reused its old instance, replaced the first or allocated");
  REQUIRE_TRUE(provider.owners[1].lock(),
               "changed second state reused its old instance, replaced the first or allocated");
  REQUIRE_NE(provider.owners[1].lock(), second_owner,
             "changed second state reused its old instance, replaced the first or allocated");
  near(output[11], 1.0F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "changed second state did not become active");

  // Removing the first effect while playing switches live: the track plays
  // dry and its compensation grows by the removed seven frames.
  control.set_live_switching(true);
  first_signal.fill({});
  second_signal.fill({});
  const auto removed = take(state.apply({project::RemoveVst3Plugin{first.id}}));
  REQUIRE_OK(control.begin_edit(removed, "remove first effect"),
             "live effect removal requested a hold");
  REQUIRE_TRUE(!control.held_boundary(), "live effect removal requested a hold");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched_out =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames * 3, 1});
  counting_allocations = false;
  control.observe();
  REQUIRE_EQ(switched_out, engine::RoutedRenderStatus::rendered,
             "live effect removal held, allocated or left the effect active");
  REQUIRE_EQ(counted_allocations, 0,
             "live effect removal held, allocated or left the effect active");
  REQUIRE_EQ(control.status().active_graph_revision, removed.snapshot->revision,
             "live effect removal held, allocated or left the effect active");
  REQUIRE_EQ(control.master_output_latency_frames(), 11,
             "live effect removal held, allocated or left the effect active");
  REQUIRE_TRUE(!control.processor_fault(first_track),
               "live effect removal held, allocated or left the effect active");
  // Past the compensation crossfade, the dry first path aligns with the
  // eleven-frame second effect.
  std::uint64_t position = block_frames * 4;
  for (; position < block_frames * 4 + 512; position += block_frames)
    REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {position, 1}),
               engine::RoutedRenderStatus::rendered, "post-removal block did not render");
  first_signal[0] = {1.0F, -1.0F};
  second_signal[0] = {1.0F, -1.0F};
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {position, 1}),
             engine::RoutedRenderStatus::rendered, "post-removal impulse block did not render");
  near(output[10], 0.0F);
  near(output[11], 1.75F); // Dry 1.0 and its 0.5 send, plus the second effect's 0.25.
  provider.faults[1]->store(true, std::memory_order_release);
  REQUIRE_TRUE(!control.processor_fault(first_track),
               "a single processor failure was not reported by track and summary");
  REQUIRE_TRUE(control.processor_fault(second_track),
               "a single processor failure was not reported by track and summary");
  REQUIRE_TRUE(control.processor_fault(),
               "a single processor failure was not reported by track and summary");
  counted_allocations = 0;
  counting_allocations = true;
  const auto faulted = control.render_block(inputs, std::span(output).first(block_frames),
                                            {position + block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(faulted, engine::RoutedRenderStatus::rendered,
             "per-track plugin fault blocked unrelated audio or allocated");
  REQUIRE_EQ(counted_allocations, 0, "per-track plugin fault blocked unrelated audio or allocated");
  near(output[11], 1.5F); // Only the dry first path and its send remain.
}

void serial_effects_preserve_order_state_and_failed_graph(std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2);
  const auto dry = id<project::TrackId>(3);
  const auto returns = id<project::TrackId>(4);
  const auto send = id<project::SendId>(5);
  auto first = saved_fixture_insert();
  auto second = first;
  second.id = id<project::PluginInstanceId>(11);
  const auto initial = take(state.apply({project::AddTrack{wet}, project::AddTrack{dry},
                                         project::AddTrack{returns, project::TrackKind::return_bus},
                                         project::InsertVst3Plugin{wet, 0, first},
                                         project::InsertVst3Plugin{wet, 1, second},
                                         project::AddSendRoute{wet, {send, returns, 0.0}}}));
  const auto first_slot = initial.snapshot->tracks[0].effects[0].id;
  const auto second_slot = initial.snapshot->tracks[0].effects[1].id;
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes = 8ULL * block_frames * sizeof(engine::StereoFrame);
  bounded.max_history_bytes = 5ULL * 65 * sizeof(engine::StereoFrame);
  SerialAffineProvider provider(first.id, second.id);
  provider.reject_second = true;
  app::RoutedPlaybackControl control(state, bounded, &provider);
  control.set_live_switching(false); // covers held switching
  require(!control.submit_current() && provider.preparations == std::array<std::size_t, 2>{1, 1} &&
              provider.owners[0].expired() && control.status().active_graph_revision == 0,
          "failed second serial binding leaked the first owner or published a partial chain");
  provider.reject_second = false;
  REQUIRE_OK(control.retry(), "two serial effects did not bind independently on retry");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{2, 2}),
             "two serial effects did not bind independently on retry");
  const auto first_owner = provider.owners[0].lock();
  const auto second_owner = provider.owners[1].lock();
  REQUIRE_TRUE(first_owner, "serial effects aliased their runtime owners");
  REQUIRE_TRUE(second_owner, "serial effects aliased their runtime owners");
  REQUIRE_NE(first_owner, second_owner, "serial effects aliased their runtime owners");

  std::array<engine::StereoFrame, 128> wet_signal{}, dry_signal{}, output{};
  wet_signal[0] = {0.25F, -0.25F};
  wet_signal[block_frames - 1] = {0.25F, -0.25F};
  dry_signal[0] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), wet, std::span(wet_signal).first(block_frames));
  set_source(inputs, control.source_layout(), dry, std::span(dry_signal).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "two-stage callback allocated or failed");
  REQUIRE_EQ(counted_allocations, 0, "two-stage callback allocated or failed");
  // A(x)=2x+1, B(x)=3x+4. B(A(.25))=8.5 while A(B(.25))=10.5.
  // Both stage latencies total 18 frames, aligning the direct dry impulse.
  near(output[18], 9.5F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "initial serial graph did not become active");

  const auto revised = take(state.apply({project::SetSendGain{send, 0.5}}));
  REQUIRE_OK(control.begin_edit(revised, "send gain"),
             "serial chain could not publish a routing-only edit at a held boundary");
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1}),
             engine::RoutedRenderStatus::holding,
             "serial chain could not publish a routing-only edit at a held boundary");
  REQUIRE_OK(control.publish_held(),
             "serial chain could not publish a routing-only edit at a held boundary");
  wet_signal.fill({});
  dry_signal.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "held route edit replaced a serial stage or allocated on callback");
  REQUIRE_EQ(counted_allocations, 0,
             "held route edit replaced a serial stage or allocated on callback");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{2, 2}),
             "held route edit replaced a serial stage or allocated on callback");
  REQUIRE_EQ(provider.owners[0].lock(), first_owner,
             "held route edit replaced a serial stage or allocated on callback");
  REQUIRE_EQ(provider.owners[1].lock(), second_owner,
             "held route edit replaced a serial stage or allocated on callback");
  REQUIRE_EQ(first_owner->blocks(), 2,
             "held route edit replaced a serial stage or allocated on callback");
  REQUIRE_EQ(second_owner->blocks(), 2,
             "held route edit replaced a serial stage or allocated on callback");
  near(output[17], 12.75F); // Prior block's final wet sample through main and 0.5 send.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "held serial graph did not become active");

  auto saved_reorder = *state.snapshot();
  std::ranges::swap(saved_reorder.tracks[0].effects[0], saved_reorder.tracks[0].effects[1]);
  const auto reordered = take(state.restore_snapshot(std::move(saved_reorder)));
  REQUIRE_EQ(reordered.snapshot->tracks[0].effects[0].id, second_slot,
             "saved reorder lost stable insert slot IDs");
  REQUIRE_EQ(reordered.snapshot->tracks[0].effects[1].id, first_slot,
             "saved reorder lost stable insert slot IDs");
  // Both instances are kept, reordered, with their running state.
  require(control.begin_edit(reordered, "reorder serial effects").has_value() &&
              control.render_block(inputs, std::span(output).first(block_frames),
                                   {2 * block_frames, 1}) == engine::RoutedRenderStatus::holding &&
              control.publish_held().has_value(),
          "serial reorder did not publish at the held boundary");
  wet_signal[0] = {0.25F, -0.25F};
  dry_signal[0] = {1.0F, -1.0F};
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed =
      control.render_block(inputs, std::span(output).first(block_frames), {2 * block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "serial reorder re-prepared a stage, lost its history or allocated");
  REQUIRE_EQ(counted_allocations, 0,
             "serial reorder re-prepared a stage, lost its history or allocated");
  REQUIRE_EQ(provider.preparations, (std::array<std::size_t, 2>{2, 2}),
             "serial reorder re-prepared a stage, lost its history or allocated");
  REQUIRE_EQ(provider.owners[0].lock(), first_owner,
             "serial reorder re-prepared a stage, lost its history or allocated");
  REQUIRE_EQ(provider.owners[1].lock(), second_owner,
             "serial reorder re-prepared a stage, lost its history or allocated");
  REQUIRE_EQ(first_owner->blocks(), 3,
             "serial reorder re-prepared a stage, lost its history or allocated");
  REQUIRE_EQ(second_owner->blocks(), 3,
             "serial reorder re-prepared a stage, lost its history or allocated");
  near(output[18], 16.75F); // A(B(x)) = 10.5, plus the 0.5 send and the dry path.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, reordered.snapshot->revision,
             "serial reorder did not become active");
  provider.faults[1]->store(true, std::memory_order_release);
  REQUIRE_TRUE(control.processor_fault(wet),
               "second serial stage fault was hidden by the healthy first stage");
  REQUIRE_TRUE(!control.processor_fault(dry),
               "second serial stage fault was hidden by the healthy first stage");
  REQUIRE_TRUE(control.processor_fault(),
               "second serial stage fault was hidden by the healthy first stage");
}

void verified_track_insert_and_parallel_pdc(std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto processed = id<project::TrackId>(2);
  const auto dry = id<project::TrackId>(3);
  const auto returns = id<project::TrackId>(4);
  const auto send = id<project::SendId>(5);
  const auto plugin = saved_fixture_insert();
  (void)take(state.apply({project::AddTrack{processed}, project::AddTrack{dry},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::InsertVst3Plugin{processed, 0, plugin}}));
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes = 5ULL * block_frames * sizeof(engine::StereoFrame);
  BoundInsertFixture provider;
  provider.expected_max_frames = static_cast<std::uint32_t>(block_frames);
  app::RoutedPlaybackControl control(state, bounded, &provider);
  control.set_live_switching(false); // covers held switching
  require(control.submit_current().has_value() && provider.preparations == 1 &&
              control.source_layout().size() == 2,
          "verified single insert did not prepare exactly once");

  std::array<engine::StereoFrame, 128> processed_signal{}, dry_signal{}, output{};
  processed_signal[0] = {1.0F, -1.0F};
  dry_signal[0] = {1.0F, -1.0F};
  processed_signal[block_frames - 1] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), processed,
             std::span(processed_signal).first(block_frames));
  set_source(inputs, control.source_layout(), dry, std::span(dry_signal).first(block_frames));
  counting_allocations = true;
  const auto first = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, engine::RoutedRenderStatus::rendered,
             "track insert callback allocated or did not render");
  REQUIRE_EQ(counted_allocations, 0, "track insert callback allocated or did not render");
  near(output[0], 0.0F);
  near(output[7], 1.5F); // One processed 0.5 impulse plus the dry 1.0 impulse.
  require(!control.processor_fault(), "healthy insert reported a process fault");
  control.observe();

  auto revised = take(state.apply({project::AddSendRoute{processed, {send, returns, 0.5}}}));
  REQUIRE_OK(control.begin_edit(revised, "add send"),
             "topology edit did not prepare with the same insert owner");
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1}),
             engine::RoutedRenderStatus::holding,
             "topology edit did not prepare with the same insert owner");
  REQUIRE_OK(control.publish_held(), "topology edit did not prepare with the same insert owner");
  processed_signal.fill({});
  dry_signal.fill({});
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "held topology edit recreated the plugin or allocated on callback");
  REQUIRE_EQ(counted_allocations, 0,
             "held topology edit recreated the plugin or allocated on callback");
  REQUIRE_EQ(provider.preparations, 1,
             "held topology edit recreated the plugin or allocated on callback");
  near(output[6], 0.75F); // Tail from the prior block, direct plus half-gain send.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "revised insert route was not active");

  auto changed = take(state.apply({project::SetVst3Missing{plugin.id, true}}));
  REQUIRE_OK(control.begin_edit(changed, "plugin missing"),
             "saved missing-plugin edit was not held");
  REQUIRE_EQ(
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames * 2, 1}),
      engine::RoutedRenderStatus::holding, "saved missing-plugin edit was not held");
  // The missing plugin is prepared again and refused; the old instance plays on.
  const auto failed = control.publish_held();
  REQUIRE_TRUE(!failed, "missing-plugin model edit was silently activated or discarded");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "missing-plugin model edit was silently activated or discarded");
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "missing-plugin model edit was silently activated or discarded");
  REQUIRE_EQ(control.status().model_revision, changed.snapshot->revision,
             "missing-plugin model edit was silently activated or discarded");
  REQUIRE_EQ(provider.preparations, 2,
             "missing-plugin model edit was silently activated or discarded");
  REQUIRE_EQ(
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames * 2, 1}),
      engine::RoutedRenderStatus::rendered,
      "old valid graph did not resume after insert edit failure");
  provider.fault->store(true, std::memory_order_release);
  REQUIRE_TRUE(control.processor_fault(),
               "runtime fault was not separate from saved/active revision diagnostics");
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "runtime fault was not separate from saved/active revision diagnostics");
}

void missing_insert_provider_does_not_publish() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(2);
  (void)take(state.apply(
      {project::AddTrack{track}, project::InsertVst3Plugin{track, 0, saved_fixture_insert()}}));
  BoundInsertFixture missing;
  missing.unavailable = true;
  app::RoutedPlaybackControl control(state, limits(), &missing);
  const auto submitted = control.submit_current();
  REQUIRE_TRUE(!submitted, "unavailable initial insert published or lost saved identity");
  REQUIRE_EQ(submitted.error().code, foundation::ErrorCode::unavailable,
             "unavailable initial insert published or lost saved identity");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "unavailable initial insert published or lost saved identity");
  REQUIRE_EQ(control.status().active_graph_revision, 0,
             "unavailable initial insert published or lost saved identity");
  REQUIRE_EQ(state.snapshot()->tracks[0].effects.size(), 1,
             "unavailable initial insert published or lost saved identity");
  auto invalid_limits = limits();
  invalid_limits.max_frames = 8193;
  app::RoutedPlaybackControl oversized(state, invalid_limits, &missing);
  const auto invalid = oversized.submit_current();
  REQUIRE_TRUE(!invalid, "invalid block limit reached the plugin provider before validation");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "invalid block limit reached the plugin provider before validation");
  REQUIRE_EQ(missing.preparations, 1,
             "invalid block limit reached the plugin provider before validation");
}

void pending_supersession_and_hold_validation() {
  Gate gate;
  auto state = model(gate);
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  auto first = take(state.apply({project::AddTrack{id<project::TrackId>(2)}}));
  REQUIRE_OK(control.submit_current(),
             "initial project refresh or duplicate publication guard failed");
  REQUIRE_EQ(control.status().model_revision, first.snapshot->revision,
             "initial project refresh or duplicate publication guard failed");
  REQUIRE_TRUE(!control.submit_current(),
               "initial project refresh or duplicate publication guard failed");
  auto second = take(state.apply({project::AddTrack{id<project::TrackId>(3)}}));
  // An edit made while the first graph is still pending waits for it.
  REQUIRE_OK(control.begin_edit(second, "second track"),
             "an edit during a pending publication was refused instead of queued");
  REQUIRE_EQ(control.status().model_revision, second.snapshot->revision,
             "the queued edit's revision was not accepted");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "the queued edit was not reported pending");
  REQUIRE_TRUE(!control.publish_held(), "a queued edit published before the first graph");
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  std::array<engine::StereoFrame, 4> output{};
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "older pending graph did not activate");
  control.observe(); // The first graph is active; the queued edit applies now.
  REQUIRE_EQ(control.status().active_graph_revision, first.snapshot->revision,
             "older pending graph did not activate");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::holding,
             "the queued source-layout edit did not request its switch");
  REQUIRE_TRUE(!control.publish_held().has_value(), "a source-layout edit was silently accepted");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "a source-layout edit was not reported failed");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::rendered,
             "the failed edit did not resume the original graph");
  REQUIRE_OK(control.retry(),
             "failed source-layout edit was not retryable or was silently accepted");
  REQUIRE_EQ(control.render_block(inputs, output, {8, 1}), engine::RoutedRenderStatus::holding,
             "failed source-layout edit was not retryable or was silently accepted");
  REQUIRE_TRUE(!control.publish_held().has_value(),
               "failed source-layout edit was not retryable or was silently accepted");
  REQUIRE_EQ(control.render_block(inputs, output, {8, 1}), engine::RoutedRenderStatus::rendered,
             "retry failure did not resume the original graph");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "retry failure did not resume the original graph");
}

void changed_model_during_hold_is_reported() {
  Gate gate;
  auto state = model(gate);
  (void)take(state.apply({project::AddTrack{id<project::TrackId>(2)}}));
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "hold fixture failed to publish");
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  std::array<engine::StereoFrame, 4> output{};
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "hold fixture did not activate");
  control.observe();
  auto first = take(state.apply({project::AddTrack{id<project::TrackId>(3)}}));
  REQUIRE_OK(control.begin_edit(first, "first add"), "unacknowledged callback hold was accepted");
  REQUIRE_TRUE(!control.publish_held(), "unacknowledged callback hold was accepted");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::holding,
             "callback did not acknowledge hold");
  auto later = take(state.apply({project::AddTrack{id<project::TrackId>(4)}}));
  auto failed = control.publish_held();
  REQUIRE_TRUE(!failed, "stale graph prep did not retain latest model and old active playback");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::conflict,
             "stale graph prep did not retain latest model and old active playback");
  REQUIRE_EQ(control.status().model_revision, later.snapshot->revision,
             "stale graph prep did not retain latest model and old active playback");
  REQUIRE_LT(control.status().active_graph_revision, later.snapshot->revision,
             "stale graph prep did not retain latest model and old active playback");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "stale graph prep did not retain latest model and old active playback");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::rendered,
             "stale graph prep did not retain latest model and old active playback");
}

void constrained_publication_preserves_failed_revision() {
  Gate gate;
  auto state = model(gate);
  auto tiny = limits();
  tiny.max_buffer_bytes = 1;
  app::RoutedPlaybackControl control(state, tiny);
  auto failed = control.submit_current();
  REQUIRE_TRUE(!failed, "resource failure discarded accepted project or was hidden on retry");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::resource_exhausted,
             "resource failure discarded accepted project or was hidden on retry");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "resource failure discarded accepted project or was hidden on retry");
  REQUIRE_TRUE(!control.retry().has_value(),
               "resource failure discarded accepted project or was hidden on retry");
}

void terminal_limiter_alignment_and_tail(std::size_t frames) {
  Gate gate;
  auto state = model(gate);
  const auto direct = id<project::TrackId>(91);
  const auto through_bus = id<project::TrackId>(92);
  const auto bus = id<project::TrackId>(93);
  const auto returns = id<project::TrackId>(94);
  const auto send = id<project::SendId>(95);
  (void)take(state.apply({project::AddTrack{direct}, project::AddTrack{through_bus},
                          project::AddTrack{bus, project::TrackKind::group_bus},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::SetTrackMainOutput{through_bus, bus},
                          project::AddSendRoute{direct, {send, returns, 0.25}},
                          project::AddNativeEffect{project::MasterSignalPath{}, 0,
                                                   ceiling_patch({0.0, 1.0, 100.0})}}));
  auto bounded = limits();
  bounded.max_frames = frames;
  bounded.max_buffer_bytes = 6ULL * frames * sizeof(engine::StereoFrame);
  app::RoutedPlaybackControl control(state, bounded);
  REQUIRE_OK(control.submit_current(),
             "routed terminal master did not report its exact 48-frame output latency");
  REQUIRE_EQ(control.master_output_latency_frames(), 48,
             "routed terminal master did not report its exact 48-frame output latency");
  REQUIRE_EQ(control.prepared_limiter_drain_frames(), 48,
             "routed terminal master did not report its exact 48-frame output latency");
  std::vector<engine::StereoFrame> impulse(frames), silence(frames), output(frames);
  const auto hit = frames - 1;
  impulse[hit] = {0.2F, -0.2F};
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  set_source(inputs, control.source_layout(), direct, impulse);
  set_source(inputs, control.source_layout(), through_bus, impulse);
  counting_allocations = true;
  const auto first = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, engine::RoutedRenderStatus::rendered,
             "initial limited block failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "initial limited block failed or allocated");
  for (const auto& frame : output)
    near(frame, 0.0F);
  control.observe();

  auto changed = take(state.apply({project::SetSendGain{send, 0.5}}));
  REQUIRE_OK(control.begin_edit(changed, "send gain"),
             "unrelated route edit did not begin with a live limiter");
  set_source(inputs, control.source_layout(), direct, silence);
  set_source(inputs, control.source_layout(), through_bus, silence);
  require(!control.held_boundary(), "terminal limiter send gain requested a hold");
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "limited live send switch failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "limited live send switch failed or allocated");
  for (std::size_t frame = 0; frame < frames; ++frame)
    near(output[frame], frame == 47 ? 0.45F : 0.0F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "route edit did not activate with the existing limiter");

  // A final project frame must be followed by silence-fed processing of the
  // exact lookahead tail; the device Stop path is not this drain operation.
  std::ranges::fill(impulse, engine::StereoFrame{});
  impulse[hit] = {0.2F, -0.2F};
  set_source(inputs, control.source_layout(), direct, impulse);
  REQUIRE_EQ(control.render_block(inputs, output, {2 * frames, 1}),
             engine::RoutedRenderStatus::rendered, "last source block did not render");
  set_source(inputs, control.source_layout(), direct, silence);
  REQUIRE_EQ(control.render_block(inputs, output, {3 * frames, 1}),
             engine::RoutedRenderStatus::rendered, "explicit silence-fed limiter drain failed");
  for (std::size_t frame = 0; frame < frames; ++frame)
    near(output[frame], frame == 47 ? 0.3F : 0.0F);
}

void terminal_output_latency_is_separate_from_edge_compensation() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(96);
  (void)take(state.apply(
      {project::AddTrack{audio}, project::AddNativeEffect{project::MasterSignalPath{}, 0,
                                                          ceiling_patch({0.0, 5.0, 100.0})}}));
  auto bounded = limits();
  bounded.latency.max_compensation_frames = 0;
  app::RoutedPlaybackControl control(state, bounded);
  REQUIRE_OK(control.submit_current(),
             "terminal 240-frame lookahead was incorrectly charged to zero edge PDC budget");
  REQUIRE_EQ(control.master_output_latency_frames(), 240,
             "terminal 240-frame lookahead was incorrectly charged to zero edge PDC budget");
  std::array<engine::StereoFrame, 64> impulse{}, silence{}, output{};
  impulse[0] = {0.25F, -0.25F};
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), audio, impulse);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "terminal long-lookahead graph did not activate");
  set_source(inputs, control.source_layout(), audio, silence);
  for (std::uint64_t block = 1; block < 4; ++block) {
    REQUIRE_EQ(control.render_block(inputs, output, {block * 64, 1}),
               engine::RoutedRenderStatus::rendered, "terminal long-lookahead continuation failed");
  }
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], frame == 48 ? 0.25F : 0.0F);
}

// Independent direct-form high-pass reference. This uses the published RBJ
// transfer function, rather than native_dsp's transposed runtime state.
class HighPassOracle {
public:
  HighPassOracle() {
    constexpr double frequency = 100.0;
    constexpr double q = 0.707;
    constexpr double sample_rate = oscillate::test::sample_rate_48k_hz;
    const double angle = 2.0 * std::numbers::pi * frequency / sample_rate;
    const double cosine = std::cos(angle);
    const double alpha = std::sin(angle) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 + cosine) / (2.0 * a0);
    b1_ = -(1.0 + cosine) / a0;
    b2_ = b0_;
    a1_ = -2.0 * cosine / a0;
    a2_ = (1.0 - alpha) / a0;
  }

  [[nodiscard]] float next(double input) noexcept {
    const double result = b0_ * input + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = input;
    y2_ = y1_;
    y1_ = result;
    return static_cast<float>(result);
  }

private:
  double b0_ = 0.0;
  double b1_ = 0.0;
  double b2_ = 0.0;
  double a1_ = 0.0;
  double a2_ = 0.0;
  double x1_ = 0.0;
  double x2_ = 0.0;
  double y1_ = 0.0;
  double y2_ = 0.0;
};

void separate_native_track_chains_preserve_independent_history() {
  Gate gate;
  auto state = model(gate);
  const auto first_track = id<project::TrackId>(1'491);
  const auto second_track = id<project::TrackId>(1'492);
  const auto high_pass = shape_patch(high_pass_100);
  const auto initial =
      take(state.apply({project::AddTrack{first_track}, project::AddTrack{second_track},
                        project::AddNativeEffect{first_track, 0, high_pass},
                        project::AddNativeEffect{second_track, 0, high_pass}}));
  auto bounded = limits();
  bounded.max_frames = 64;
  app::RoutedPlaybackControl control(state, bounded);
  control.set_live_switching(false); // covers held switching
  auto prepared = control.submit_current();
  if (!prepared)
    throw std::runtime_error("separate native track chains: " + prepared.error().message);
  REQUIRE_EQ(control.source_layout().size(), 2, "separate native track source layout is wrong");

  std::array<engine::StereoFrame, 64> first_signal{}, second_signal{}, output{};
  first_signal.back() = {1.0F, -1.0F};
  second_signal[62] = {0.5F, -0.5F};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), first_track, first_signal);
  set_source(inputs, control.source_layout(), second_track, second_signal);
  HighPassOracle first_oracle;
  HighPassOracle second_oracle;
  counted_allocations = 0;
  counting_allocations = true;
  const auto first_block = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first_block, engine::RoutedRenderStatus::rendered,
             "two native paths failed or allocated in the callback");
  REQUIRE_EQ(counted_allocations, 0, "two native paths failed or allocated in the callback");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame],
         first_oracle.next(frame == 63 ? 1.0 : 0.0) + second_oracle.next(frame == 62 ? 0.5 : 0.0));
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "native path graph did not activate");

  const auto revised = take(state.apply({project::SetTrackLevelPan{second_track, 0.5, 0.0}}));
  REQUIRE_OK(control.begin_edit(revised, "second native track level"),
             "second native track edit did not wait for a callback boundary");
  REQUIRE_EQ(control.status().active_graph_revision, initial.snapshot->revision,
             "second native track edit did not wait for a callback boundary");
  first_signal.fill({});
  second_signal.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "native control overlay held, failed or allocated in the callback");
  REQUIRE_EQ(counted_allocations, 0,
             "native control overlay held, failed or allocated in the callback");
  for (std::size_t frame = 0; frame < output.size(); ++frame) {
    const auto gain = 1.0F - 0.5F * static_cast<float>(frame + 1) / 64.0F;
    near(output[frame], first_oracle.next(0.0) + gain * second_oracle.next(0.0));
  }
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "native path edit was not marked active");

  const auto reordered = take(state.apply({project::MoveTrack{first_track, 1}}));
  REQUIRE_OK(control.begin_edit(reordered, "reorder native tracks"),
             "reordering native tracks lost their prepared processor bindings");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::holding,
             "reordering native tracks lost their prepared processor bindings");
  REQUIRE_OK(control.publish_held(),
             "reordering native tracks lost their prepared processor bindings");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::rendered,
             "reordered native tracks did not render");
  for (const auto& frame : output)
    near(frame, first_oracle.next(0.0) + 0.5F * second_oracle.next(0.0));
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, reordered.snapshot->revision,
             "reordered native tracks did not activate their graph revision");
}

void native_group_eq_processes_routed_sum_and_retains_state() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(1450);
  const auto group = id<project::TrackId>(1451);
  const auto returns = id<project::TrackId>(1452);
  const auto send = id<project::SendId>(1453);
  const auto initial = take(state.apply(
      {project::AddTrack{audio}, project::AddTrack{group, project::TrackKind::group_bus},
       project::AddTrack{returns, project::TrackKind::return_bus},
       project::SetTrackMainOutput{audio, group},
       project::AddSendRoute{audio, {send, returns, 0.25}},
       project::AddNativeEffect{group, 0, shape_patch(high_pass_100)}}));
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "saved group EQ did not publish");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  signal.back() = {1.0F, -1.0F};
  HighPassOracle oracle;
  counted_allocations = 0;
  counting_allocations = true;
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "group EQ callback failed");
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "group EQ callback allocated");
  for (std::size_t index = 0; index < output.size(); ++index)
    near(output[index], oracle.next(index == 63 ? 1.0 : 0.0) + (index == 63 ? 0.25F : 0.0F));
  control.observe();
  const auto revised = take(state.apply({project::SetSendGain{send, 0.5}}));
  signal.fill({});
  REQUIRE_OK(control.begin_edit(revised, "bus EQ send change"),
             "compatible group EQ send edit was not admitted");
  REQUIRE_TRUE(!control.held_boundary(), "compatible group EQ send edit was not admitted");
  REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::rendered,
             "group EQ did not survive live send publication");
  for (const auto frame : output)
    near(frame, oracle.next(0.0));
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "group EQ topology edit did not activate");
  const auto slot = revised.snapshot->tracks[1].effects.front().id;
  const auto bypass = take(state.apply({project::SetEffectInsertEnabled{group, slot, false}}));
  REQUIRE_OK(control.begin_edit(bypass, "bus EQ bypass"),
             "active group EQ bypass did not publish with a draining tail");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::holding,
             "active group EQ bypass did not publish with a draining tail");
  REQUIRE_OK(control.publish_held(), "active group EQ bypass did not publish with a draining tail");
  counted_allocations = 0;
  counting_allocations = true;
  const auto drained = control.render_block(inputs, output, {128, 1});
  counting_allocations = false;
  REQUIRE_EQ(drained, engine::RoutedRenderStatus::rendered,
             "group EQ tail drain failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "group EQ tail drain failed or allocated");
  for (const auto frame : output)
    near(frame, oracle.next(0.0));
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, bypass.snapshot->revision,
             "group EQ bypass did not activate or lost its saved state");
  REQUIRE_TRUE(!state.snapshot()->tracks[1].effects.front().enabled,
               "group EQ bypass did not activate or lost its saved state");
}

void native_return_eq_processes_send_after_gain() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(1460);
  const auto returns = id<project::TrackId>(1461);
  const auto send = id<project::SendId>(1462);
  (void)take(state.apply({project::AddTrack{audio},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::AddSendRoute{audio, {send, returns, 0.5}},
                          project::AddNativeEffect{returns, 0, shape_patch(high_pass_100)}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "saved return EQ did not publish");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  signal.back() = {1.0F, -1.0F};
  HighPassOracle oracle;
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "return EQ callback failed");
  for (std::size_t index = 0; index < output.size(); ++index)
    near(output[index], (index == 63 ? 1.0F : 0.0F) + oracle.next(index == 63 ? 0.5 : 0.0));
}

void mixed_native_vst3_keeps_order_and_state(std::size_t block_frames, bool native_first,
                                             bool compressor) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2200);
  const auto dry = id<project::TrackId>(2201);
  const auto returns = id<project::TrackId>(2202);
  const auto send = id<project::SendId>(2203);
  const auto plugin = saved_fixture_insert();
  const native_dsp::CompressorSettings compressor_settings{-24.0, 6.0, 0.0, 0.1, 100.0, 0.0};
  const project::NativeEffectProcessor native =
      compressor ? press_patch(compressor_settings) : shape_patch(low_pass_1k);
  (void)take(state.apply({project::AddTrack{wet}, project::AddTrack{dry},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::AddSendRoute{wet, {send, returns, 0.0}}}));
  if (native_first) {
    (void)take(state.apply({project::AddNativeEffect{wet, 0, native}}));
    (void)take(state.apply({project::InsertVst3Plugin{wet, 1, plugin}}));
  } else {
    (void)take(state.apply({project::InsertVst3Plugin{wet, 0, plugin}}));
    (void)take(state.apply({project::AddNativeEffect{wet, 1, native}}));
  }
  const auto native_slot = state.snapshot()->tracks[0].effects[native_first ? 0 : 1].id;
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes = 6ULL * block_frames * sizeof(engine::StereoFrame);
  BoundInsertFixture provider;
  provider.expected_max_frames = static_cast<std::uint32_t>(block_frames);
  provider.nonlinear = true;
  app::RoutedPlaybackControl control(state, bounded, &provider);
  control.set_live_switching(false); // covers held switching
  auto prepared = control.submit_current();
  if (!prepared)
    throw std::runtime_error("mixed prepare: " + prepared.error().message);
  require(provider.preparations == 1 && control.master_output_latency_frames() == 7,
          "mixed native/VST3 route did not prepare once with plugin PDC: " +
              std::to_string(provider.preparations) + "/" +
              std::to_string(control.master_output_latency_frames()));
  const std::array<native_dsp::EqBandSettings, 1> bands{
      {{native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}};
  std::optional<native_dsp::EqRuntime> eq_oracle;
  std::optional<native_dsp::CompressorRuntime> compressor_oracle;
  if (compressor) {
    compressor_oracle.emplace(take(native_dsp::prepare_compressor(
        {compressor_settings.threshold_db, compressor_settings.ratio, compressor_settings.knee_db,
         compressor_settings.attack_ms, compressor_settings.release_ms,
         compressor_settings.makeup_db},
        48'000)));
  } else {
    eq_oracle.emplace(take(native_dsp::prepare_eq(bands, 48'000)));
  }
  const auto process_native = [&](std::span<engine::StereoFrame> frames,
                                  engine::BlockContext context) {
    if (compressor)
      compressor_oracle->process(frames, context);
    else
      eq_oracle->process(frames, context);
  };
  std::array<engine::StereoFrame, 128> signal{}, dry_signal{}, output{}, first_expected{},
      second_expected{};
  signal[0] = {1.0F, -1.0F};
  signal[block_frames - 1] = {1.0F, -1.0F};
  dry_signal[0] = {1.0F, -1.0F};
  const auto nonlinear = [](float value) { return value * std::fabs(value); };
  if (native_first) {
    std::copy_n(signal.begin(), block_frames, first_expected.begin());
    process_native(std::span(first_expected).first(block_frames), {0, 1});
    process_native(std::span(second_expected).first(block_frames), {block_frames, 1});
    const auto eq_first = first_expected;
    const auto eq_second = second_expected;
    for (std::size_t frame = 0; frame < block_frames; ++frame) {
      first_expected[frame].left = frame >= 7 ? nonlinear(eq_first[frame - 7].left) : 0.0F;
      second_expected[frame].left = nonlinear(frame < 7 ? eq_first[block_frames - 7 + frame].left
                                                        : eq_second[frame - 7].left);
    }
  } else {
    for (std::size_t frame = 0; frame < block_frames; ++frame) {
      first_expected[frame].left = frame >= 7 ? nonlinear(signal[frame - 7].left) : 0.0F;
      second_expected[frame].left =
          frame < 7 ? nonlinear(signal[block_frames - 7 + frame].left) : 0.0F;
    }
    process_native(std::span(first_expected).first(block_frames), {0, 1});
    process_native(std::span(second_expected).first(block_frames), {block_frames, 1});
  }
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  set_source(inputs, control.source_layout(), wet, std::span(signal).first(block_frames));
  set_source(inputs, control.source_layout(), dry, std::span(dry_signal).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "mixed native/VST3 callback allocated or failed");
  REQUIRE_EQ(counted_allocations, 0, "mixed native/VST3 callback allocated or failed");
  for (std::size_t frame = 0; frame < block_frames; ++frame) {
    const float expected = frame == 7 ? 1.0F : 0.0F;
    REQUIRE_LT(std::fabs(output[frame].left - expected - first_expected[frame].left), 0.00001F,
               "mixed route ignored saved stage order or PDC");
    REQUIRE_TRUE(std::isfinite(output[frame].left), "mixed route ignored saved stage order or PDC");
  }
  control.observe();
  const auto revised = take(state.apply({project::SetSendGain{send, 0.5}}));
  REQUIRE_OK(control.begin_edit(revised, "mixed route send gain"),
             "mixed route could not publish a held routing edit");
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1}),
             engine::RoutedRenderStatus::holding,
             "mixed route could not publish a held routing edit");
  REQUIRE_OK(control.publish_held(), "mixed route could not publish a held routing edit");
  signal.fill({});
  dry_signal.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "mixed route held edit replaced its plugin or changed PDC");
  REQUIRE_EQ(counted_allocations, 0, "mixed route held edit replaced its plugin or changed PDC");
  REQUIRE_EQ(provider.preparations, 1, "mixed route held edit replaced its plugin or changed PDC");
  REQUIRE_EQ(control.master_output_latency_frames(), 7,
             "mixed route held edit replaced its plugin or changed PDC");
  for (std::size_t frame = 0; frame < block_frames; ++frame) {
    REQUIRE_LT(std::fabs(output[frame].left - second_expected[frame].left * 1.5F), 0.00001F,
               "mixed route lost native or plugin history across held publication");
  }
  control.observe();
  auto reversed = *state.snapshot();
  std::ranges::swap(reversed.tracks[0].effects[0], reversed.tracks[0].effects[1]);
  const auto changed = take(state.restore_snapshot(std::move(reversed)));
  // Reversing the chain keeps both running stages; the plugin is not prepared again.
  require(control.begin_edit(changed, "reverse mixed chain").has_value() &&
              control.render_block(inputs, std::span(output).first(block_frames),
                                   {2 * block_frames, 1}) == engine::RoutedRenderStatus::holding &&
              control.publish_held().has_value() &&
              changed.snapshot->tracks[0].effects[native_first ? 1 : 0].id == native_slot,
          "mixed reorder did not publish at the held boundary");
  counted_allocations = 0;
  counting_allocations = true;
  const auto reversed_block =
      control.render_block(inputs, std::span(output).first(block_frames), {2 * block_frames, 1});
  counting_allocations = false;
  control.observe();
  REQUIRE_EQ(reversed_block, engine::RoutedRenderStatus::rendered,
             "mixed reorder re-prepared the plugin, changed PDC or stayed inactive");
  REQUIRE_EQ(counted_allocations, 0,
             "mixed reorder re-prepared the plugin, changed PDC or stayed inactive");
  REQUIRE_EQ(provider.preparations, 1,
             "mixed reorder re-prepared the plugin, changed PDC or stayed inactive");
  REQUIRE_EQ(control.master_output_latency_frames(), 7,
             "mixed reorder re-prepared the plugin, changed PDC or stayed inactive");
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "mixed reorder re-prepared the plugin, changed PDC or stayed inactive");
  provider.fault->store(true, std::memory_order_release);
  REQUIRE_OK(control.processor_fault(wet), "mixed plugin fault was hidden by native stage");
}

// Removing an earlier stage while playing keeps the later stage's running
// processor, so its state (here a compressor's envelope) carries over.
void live_earlier_stage_removal_keeps_later_stage() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(2'310);
  const native_dsp::CompressorSettings settings{-30.0, 4.0, 0.0, 0.1, 100.0, 0.0};
  (void)take(state.apply({project::AddTrack{audio},
                          project::AddNativeEffect{audio, 0, shape_patch(high_pass_100)},
                          project::AddNativeEffect{audio, 1, press_patch(settings)}}));
  const auto earlier = state.snapshot()->tracks.front().effects.front().id;
  const auto compressor = state.snapshot()->tracks.front().effects.back().id;
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "earlier-stage fixture did not prepare");
  std::array<engine::StereoFrame, 64> input{}, output{};
  input.fill({0.5F, -0.5F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), audio, input);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "earlier-stage fixture did not render");
  control.observe();
  // Press holds its gain reduction for a second: a reused stage still shows
  // it after the switch, a fresh one would read zero.
  const auto reduction = [&] {
    std::vector<float> frame;
    return control.active_effect_telemetry(audio, compressor, frame) &&
                   frame.size() > native_dsp::effects::press::telemetry::reduction
               ? frame[native_dsp::effects::press::telemetry::reduction]
               : 0.0F;
  };
  REQUIRE_GT(reduction(), 0.0F, "the compressor did not engage before the edit");
  const auto removed = take(state.apply({project::RemoveEffectInsert{audio, earlier}}));
  REQUIRE_OK(control.begin_edit(removed, "remove earlier stage"),
             "earlier-stage removal did not reach a held boundary");
  REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::holding,
             "earlier-stage removal did not reach a held boundary");
  const auto published = control.publish_held();
  REQUIRE_OK(published, "earlier-stage removal did not publish: " +
                            (published ? std::string() : published.error().message));
  input.fill({});
  REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::rendered,
             "the rebuilt chain did not render");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, removed.snapshot->revision,
             "the remaining compressor was lost or reset by the removal");
  REQUIRE_GT(reduction(), 0.0F, "the remaining compressor was lost or reset by the removal");
}

void native_eq_compressor_chain_keeps_order_and_history(std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(1441);
  const auto returns = id<project::TrackId>(1442);
  const auto send = id<project::SendId>(1443);
  native_dsp::CompressorSettings settings;
  settings.threshold_db = -30.0;
  settings.ratio = 20.0;
  settings.knee_db = 0.0;
  settings.attack_ms = 0.1;
  settings.release_ms = 1.0;
  const auto initial = take(state.apply(
      {project::AddTrack{audio}, project::AddTrack{returns, project::TrackKind::return_bus},
       project::AddSendRoute{audio, {send, returns, 0.25}},
       project::AddNativeEffect{audio, 0, shape_patch(high_pass_100)},
       project::AddNativeEffect{audio, 1, press_patch(settings)}}));
  const auto compressor_slot = initial.snapshot->tracks[0].effects[1].id;
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes = 6ULL * block_frames * sizeof(engine::StereoFrame);
  app::RoutedPlaybackControl control(state, bounded);
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "ordered native route did not prepare");
  const std::array<native_dsp::EqBandSettings, 1> eq_settings{
      {{native_dsp::EqBandKind::high_pass, 100.0, 0.707, 0.0}}};
  native_dsp::EqRuntime eq(take(native_dsp::prepare_eq(eq_settings, 48'000)));
  native_dsp::CompressorRuntime compressor(take(
      native_dsp::prepare_compressor({settings.threshold_db, settings.ratio, settings.knee_db,
                                      settings.attack_ms, settings.release_ms, settings.makeup_db},
                                     48'000)));
  std::array<engine::StereoFrame, 128> signal{}, oracle{}, output{};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), audio, std::span(signal).first(block_frames));
  signal[block_frames - 1] = {1.0F, -1.0F};
  oracle = signal;
  eq.process(std::span(oracle).first(block_frames), {0, 1});
  compressor.process(std::span(oracle).first(block_frames), {0, 1});
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "ordered native route callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "ordered native route callback failed or allocated");
  for (std::size_t index = 0; index < block_frames; ++index)
    near(output[index], oracle[index].left * 1.25F);
  control.observe();
  const auto revised = take(state.apply({project::SetSendGain{send, 0.5}}));
  signal.fill({});
  REQUIRE_OK(control.begin_edit(revised, "native chain send gain"),
             "native chain send gain was not admitted live");
  REQUIRE_TRUE(!control.held_boundary(), "native chain send gain was not admitted live");
  oracle.fill({});
  eq.process(std::span(oracle).first(block_frames), {block_frames, 1});
  compressor.process(std::span(oracle).first(block_frames), {block_frames, 1});
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "native chain live callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "native chain live callback failed or allocated");
  for (std::size_t index = 0; index < block_frames; ++index) {
    const auto fraction = static_cast<float>(std::min<std::size_t>(index + 1, 64)) / 64.0F;
    near(output[index], oracle[index].left * (1.25F + 0.25F * fraction));
  }
  control.observe();
  const auto reordered = take(state.apply({project::MoveEffectInsert{audio, compressor_slot, 0}}));
  // A reorder switches live at a held boundary; both stages keep running.
  require(control.begin_edit(reordered, "native chain reorder").has_value() &&
              control.render_block(inputs, std::span(output).first(block_frames),
                                   {2 * block_frames, 1}) == engine::RoutedRenderStatus::holding,
          "native chain reorder did not reach a held boundary");
  const auto published = control.publish_held();
  REQUIRE_OK(published, "native chain reorder did not publish: " +
                            (published ? std::string() : published.error().message));
  REQUIRE_EQ(
      control.render_block(inputs, std::span(output).first(block_frames), {2 * block_frames, 1}),
      engine::RoutedRenderStatus::rendered, "reordered chain did not render");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, reordered.snapshot->revision,
             "native chain reorder misreported its revision");
}

void native_sidechain_send_matches_the_independent_compressor(std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto target = id<project::TrackId>(4'501);
  const auto detector = id<project::TrackId>(4'502);
  native_dsp::CompressorSettings settings;
  settings.threshold_db = -30.0;
  settings.ratio = 20.0;
  settings.knee_db = 0.0;
  settings.attack_ms = 0.1;
  REQUIRE_OK(state.apply({project::AddTrack{target}, project::AddTrack{detector},
                          project::AddNativeEffect{target, 0, press_patch(settings)}}));
  const auto slot = state.snapshot()->tracks.front().effects.front().id;
  REQUIRE_OK(state.apply({project::AddSendRoute{
      detector,
      {id<project::SendId>(4'503), target, 1.0, project::SendPurpose::sidechain, slot}}}));
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes = 6ULL * block_frames * sizeof(engine::StereoFrame);
  app::RoutedPlaybackControl control(state, bounded);
  REQUIRE_OK(control.submit_current(), "saved sidechain graph did not prepare");
  std::array<engine::StereoFrame, 128> main{}, key{}, expected{}, output{};
  main.fill({0.2F, -0.2F});
  key.fill({0.8F, 0.8F});
  expected = main;
  native_dsp::CompressorRuntime oracle(take(native_dsp::prepare_compressor(
      {settings.threshold_db, settings.ratio, settings.knee_db, settings.attack_ms,
       settings.release_ms, settings.makeup_db, settings.sidechain_highpass_hz},
      48'000)));
  oracle.process_with_sidechain(std::span(expected).first(block_frames),
                                std::span(key).first(block_frames), {0, 1});
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), target, std::span(main).first(block_frames));
  set_source(inputs, control.source_layout(), detector, std::span(key).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "saved sidechain callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "saved sidechain callback failed or allocated");
  for (std::size_t frame = 0; frame < block_frames; ++frame) {
    REQUIRE_LT(std::abs(output[frame].left - (expected[frame].left + key[frame].left)),
               tolerance::float_processing_f,
               "live sidechain output differs from the independent compressor plus audible key");
    REQUIRE_LT(std::abs(output[frame].right - (expected[frame].right + key[frame].right)),
               tolerance::float_processing_f,
               "live sidechain output differs from the independent compressor plus audible key");
  }
  control.observe();
  const auto revised = take(state.apply({project::SetSendGain{id<project::SendId>(4'503), 0.5}}));
  REQUIRE_OK(control.begin_edit(revised, "sidechain detector gain"),
             "sidechain gain edit was not admitted live");
  REQUIRE_TRUE(!control.held_boundary(), "sidechain gain edit was not admitted live");
  std::array<engine::StereoFrame, 128> half_key{};
  for (std::size_t frame = 0; frame < block_frames; ++frame) {
    const auto fraction = static_cast<float>(std::min<std::size_t>(frame + 1, 64)) / 64.0F;
    half_key[frame] = {0.8F - 0.4F * fraction, 0.8F - 0.4F * fraction};
  }
  expected = main;
  oracle.process_with_sidechain(std::span(expected).first(block_frames),
                                std::span(half_key).first(block_frames), {block_frames, 1});
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "sidechain live gain callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "sidechain live gain callback failed or allocated");
  for (std::size_t frame = 0; frame < block_frames; ++frame) {
    REQUIRE_LT(std::abs(output[frame].left - (expected[frame].left + key[frame].left)),
               tolerance::float_processing_f,
               "sidechain gain handoff reset compressor state or changed audible key level");
    REQUIRE_LT(std::abs(output[frame].right - (expected[frame].right + key[frame].right)),
               tolerance::float_processing_f,
               "sidechain gain handoff reset compressor state or changed audible key level");
  }
}

class StereoAuxFixture final : public engine::SidechainProcessorRuntime {
public:
  void process(std::span<engine::StereoFrame> audio, engine::BlockContext) noexcept override {
    for (auto& frame : audio) {
      frame.left *= 2.0F;
      frame.right *= 2.0F;
    }
  }
  void process_with_sidechain(std::span<engine::StereoFrame> audio,
                              std::span<const engine::StereoFrame> detector,
                              engine::BlockContext) noexcept override {
    for (std::size_t frame = 0; frame < audio.size(); ++frame) {
      audio[frame].left = audio[frame].left * 2.0F + detector[frame].left * 0.5F;
      audio[frame].right = audio[frame].right * 2.0F + detector[frame].right * 0.5F;
    }
  }
};

class StereoAuxProvider final : public app::TrackInsertProvider {
public:
  explicit StereoAuxProvider(bool connected) : connected_(connected) {}
  foundation::Result<app::PreparedTrackInsert>
  prepare(const project::Snapshot&, const project::Vst3PluginInstance&, std::uint32_t) override {
    app::PreparedTrackInsert prepared;
    prepared.processor = std::make_shared<StereoAuxFixture>();
    prepared.process_failed = [] { return false; };
    prepared.routed_sidechain = connected_;
    return foundation::Result<app::PreparedTrackInsert>::success(std::move(prepared));
  }

private:
  bool connected_;
};

void vst3_sidechain_uses_verified_detector_and_keeps_prior_graph(std::size_t block_frames) {
  const auto make_state = [](Gate& gate, bool with_send) {
    auto state = model(gate);
    const auto target = id<project::TrackId>(8'301);
    const auto detector = id<project::TrackId>(8'302);
    const auto effect = saved_fixture_insert();
    REQUIRE_OK(state.apply({project::AddTrack{target}, project::AddTrack{detector},
                            project::InsertVst3Plugin{target, 0, effect}}));
    const auto slot = state.snapshot()->tracks.front().effects.front().id;
    if (with_send)
      REQUIRE_OK(state.apply({project::AddSendRoute{
          detector,
          {id<project::SendId>(8'303), target, 1.0, project::SendPurpose::sidechain, slot}}}));
    return state;
  };
  const auto render = [block_frames](app::RoutedPlaybackControl& control, project::TrackId target,
                                     project::TrackId detector, std::uint64_t position) {
    std::array<engine::StereoFrame, 128> main{}, key{}, output{};
    main.fill({0.2F, -0.2F});
    key.fill({0.8F, -0.8F});
    std::array<std::span<const engine::StereoFrame>, 3> inputs{};
    set_source(inputs, control.source_layout(), target, std::span(main).first(block_frames));
    set_source(inputs, control.source_layout(), detector, std::span(key).first(block_frames));
    counted_allocations = 0;
    counting_allocations = true;
    const auto status =
        control.render_block(inputs, std::span(output).first(block_frames), {position, 1});
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "VST3 detector route allocated on the audio callback");
    return std::pair{status, output};
  };
  const auto target = id<project::TrackId>(8'301);
  const auto detector = id<project::TrackId>(8'302);
  auto bounded = limits();
  bounded.max_frames = block_frames;
  bounded.max_buffer_bytes = 6ULL * block_frames * sizeof(engine::StereoFrame);

  Gate live_gate;
  auto live_state = make_state(live_gate, true);
  StereoAuxProvider connected(true);
  app::RoutedPlaybackControl live(live_state, bounded, &connected);
  live.set_live_switching(false); // covers held switching
  REQUIRE_OK(live.submit_current(), "initial verified VST3 detector graph failed");
  auto [first_status, first] = render(live, target, detector, 0);
  REQUIRE_EQ(first_status, engine::RoutedRenderStatus::rendered,
             "verified VST3 detector graph did not render");
  for (const auto& frame : std::span(first).first(block_frames))
    near(frame, 1.6F); // 0.2*2 + 0.8*0.5 detector + 0.8 audible source.
  live.observe();
  const auto changed =
      take(live_state.apply({project::SetSendGain{id<project::SendId>(8'303), 0.5}}));
  REQUIRE_OK(live.begin_edit(changed, "VST3 detector send gain"),
             "verified detector route gain edit was rejected");
  auto [holding, _] = render(live, target, detector, block_frames);
  REQUIRE_EQ(holding, engine::RoutedRenderStatus::holding,
             "detector gain edit did not publish at a held boundary");
  REQUIRE_OK(live.publish_held(), "detector gain edit did not publish at a held boundary");
  auto [resumed, second] = render(live, target, detector, block_frames);
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "detector gain edit did not resume playback");
  for (const auto& frame : std::span(second).first(block_frames))
    near(frame, 1.4F);
  live.observe();
  REQUIRE_EQ(live.status().active_graph_revision, changed.snapshot->revision,
             "verified detector edit did not become active");

  Gate missing_gate;
  auto missing_state = make_state(missing_gate, true);
  StereoAuxProvider unqualified(false);
  app::RoutedPlaybackControl missing(missing_state, bounded, &unqualified);
  missing.set_live_switching(false); // covers held switching
  require(!missing.submit_current() &&
              missing_state.snapshot()->tracks[1].sends.front().target_insert ==
                  missing_state.snapshot()->tracks[0].effects.front().id,
          "unqualified detector silently activated or erased saved routing intent");

  Gate prior_gate;
  auto prior_state = make_state(prior_gate, false);
  app::RoutedPlaybackControl prior(prior_state, bounded, &unqualified);
  prior.set_live_switching(false); // covers held switching
  REQUIRE_OK(prior.submit_current(), "prior unconnected VST3 graph did not prepare");
  auto [prior_status, prior_audio] = render(prior, target, detector, 0);
  REQUIRE_EQ(prior_status, engine::RoutedRenderStatus::rendered,
             "prior unconnected VST3 graph did not render");
  for (const auto& frame : std::span(prior_audio).first(block_frames))
    near(frame, 1.2F);
  prior.observe();
  const auto active_revision = prior.status().active_graph_revision;
  const auto added = take(prior_state.apply({project::AddSendRoute{
      detector,
      {id<project::SendId>(8'303), target, 1.0, project::SendPurpose::sidechain,
       prior_state.snapshot()->tracks[0].effects.front().id}}}));
  REQUIRE_OK(prior.begin_edit(added, "connect VST3 detector"),
             "saved VST3 detector edit was lost before preflight");
  auto [pending, ignored] = render(prior, target, detector, block_frames);
  REQUIRE_EQ(pending, engine::RoutedRenderStatus::holding,
             "dynamic unqualified detector edit replaced the active graph or lost saved intent");
  REQUIRE_TRUE(!prior.publish_held(),
               "dynamic unqualified detector edit replaced the active graph or lost saved intent");
  REQUIRE_EQ(prior.status().application, app::PlaybackApplication::failed,
             "dynamic unqualified detector edit replaced the active graph or lost saved intent");
  REQUIRE_EQ(prior.status().model_revision, added.snapshot->revision,
             "dynamic unqualified detector edit replaced the active graph or lost saved intent");
  REQUIRE_EQ(prior.status().active_graph_revision, active_revision,
             "dynamic unqualified detector edit replaced the active graph or lost saved intent");
  REQUIRE_EQ(prior_state.snapshot()->tracks[1].sends.size(), 1,
             "dynamic unqualified detector edit replaced the active graph or lost saved intent");
  auto [unchanged, old_audio] = render(prior, target, detector, block_frames);
  REQUIRE_EQ(unchanged, engine::RoutedRenderStatus::rendered,
             "prior VST3 graph did not resume after detector refusal");
  for (const auto& frame : std::span(old_audio).first(block_frames))
    near(frame, 1.2F);
}

// A bypassed VST3 slot runs no plugin. Enabling it while playing prepares
// the plugin and switches live.
void saved_bypassed_vst3_effect_is_dry_and_enables_live() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(1450);
  const auto plugin = saved_fixture_insert();
  (void)take(state.apply({project::AddTrack{audio}, project::InsertVst3Plugin{audio, 0, plugin}}));
  const auto slot = state.snapshot()->tracks[0].effects[0].id;
  (void)take(state.apply({project::SetEffectInsertEnabled{audio, slot, false}}));
  BoundInsertFixture provider;
  provider.unavailable = true;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  REQUIRE_OK(control.submit_current(), "bypassed VST3 slot attempted to prepare a runtime");
  REQUIRE_EQ(provider.preparations, 0, "bypassed VST3 slot attempted to prepare a runtime");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "bypassed VST3 slot changed dry audio or allocated");
  REQUIRE_EQ(counted_allocations, 0, "bypassed VST3 slot changed dry audio or allocated");
  REQUIRE_TRUE(std::ranges::equal(output, signal,
                                  [](const auto& a, const auto& b) {
                                    return a.left == b.left && a.right == b.right;
                                  }),
               "bypassed VST3 slot changed dry audio or allocated");
  control.observe();
  provider.unavailable = false;
  const auto enabled = take(state.apply({project::SetEffectInsertEnabled{audio, slot, true}}));
  REQUIRE_OK(control.begin_edit(enabled, "enable saved VST3 effect"),
             "enabling a bypassed VST3 effect held, did not prepare it or lost its saved state");
  REQUIRE_TRUE(!control.held_boundary(),
               "enabling a bypassed VST3 effect held, did not prepare it or lost its saved state");
  REQUIRE_EQ(provider.preparations, 1,
             "enabling a bypassed VST3 effect held, did not prepare it or lost its saved state");
  REQUIRE_EQ(enabled.snapshot->tracks[0].effects[0].id, slot,
             "enabling a bypassed VST3 effect held, did not prepare it or lost its saved state");
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(enabled.snapshot->tracks[0].effects[0].processor),
      plugin, "enabling a bypassed VST3 effect held, did not prepare it or lost its saved state");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "enabling a VST3 effect while playing held or allocated");
  REQUIRE_EQ(counted_allocations, 0, "enabling a VST3 effect while playing held or allocated");
  // The new instance emits its own seven frames of latency first.
  for (std::size_t frame = 7; frame < output.size(); ++frame)
    near(output[frame], 0.125F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, enabled.snapshot->revision,
             "enabled VST3 effect did not become active with its latency");
  REQUIRE_EQ(control.master_output_latency_frames(), 7,
             "enabled VST3 effect did not become active with its latency");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::rendered,
             "enabled VST3 effect did not keep playing");
  for (const auto frame : output)
    near(frame, 0.125F);
}

// Adding a VST3 effect while playing prepares it while the old graph plays,
// then switches at the next block with no silent block and the same source.
void live_vst3_effect_addition_keeps_source_without_silence(std::size_t frames) {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(14'550 + frames);
  (void)take(state.apply({project::AddTrack{track}}));
  auto bounded = limits();
  bounded.max_frames = frames;
  bounded.max_buffer_bytes = 6ULL * frames * sizeof(engine::StereoFrame);
  LiveAdditionProvider provider;
  app::RoutedPlaybackControl control(state, bounded, &provider);
  REQUIRE_OK(control.submit_current(), "dry graph failed initial preparation");
  std::array<engine::StereoFrame, 128> signal{}, output{};
  std::ranges::fill(std::span(signal).first(frames), engine::StereoFrame{0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, std::span(signal).first(frames));
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(frames), {0, 1}),
             engine::RoutedRenderStatus::rendered, "dry graph did not render");
  control.observe();
  const auto old_revision = control.status().active_graph_revision;
  const auto applied =
      take(state.apply({project::InsertVst3Plugin{track, 0, saved_fixture_insert()}}));
  REQUIRE_OK(control.begin_edit(applied, "add VST3 effect"),
             "VST3 addition held, was not prepared or was claimed audible before its switch");
  REQUIRE_TRUE(!control.held_boundary(),
               "VST3 addition held, was not prepared or was claimed audible before its switch");
  REQUIRE_EQ(provider.preparations, 1,
             "VST3 addition held, was not prepared or was claimed audible before its switch");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "VST3 addition held, was not prepared or was claimed audible before its switch");
  REQUIRE_EQ(control.status().model_revision, applied.snapshot->revision,
             "VST3 addition held, was not prepared or was claimed audible before its switch");
  REQUIRE_EQ(control.status().active_graph_revision, old_revision,
             "VST3 addition held, was not prepared or was claimed audible before its switch");
  counted_allocations = 0;
  counting_allocations = true;
  const auto active = control.render_block(inputs, std::span(output).first(frames), {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(active, engine::RoutedRenderStatus::rendered,
             "VST3 addition held or allocated at its switching block");
  REQUIRE_EQ(counted_allocations, 0, "VST3 addition held or allocated at its switching block");
  for (std::size_t frame = 0; frame < frames; ++frame)
    near(output[frame], 0.125F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, applied.snapshot->revision,
             "new saved effect did not become active after the callback boundary");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "new saved effect did not become active after the callback boundary");
  provider.fault->store(true, std::memory_order_release);
  REQUIRE_OK(control.processor_fault(track),
             "newly inserted effect was omitted from active fault bookkeeping");
  provider.fault->store(false, std::memory_order_release);
  const auto level = take(state.apply({project::SetTrackLevelPan{track, 0.5, 0.0}}));
  REQUIRE_OK(control.begin_edit(level, "later track level"), "later level edit held the graph");
  REQUIRE_TRUE(!control.held_boundary(), "later level edit held the graph");
  for (std::uint64_t position = 2 * frames; position < 2 * frames + 512; position += frames)
    REQUIRE_EQ(control.render_block(inputs, std::span(output).first(frames), {position, 1}),
               engine::RoutedRenderStatus::rendered, "later level edit did not keep playing");
  near(output[frames - 1], 0.0625F);
  REQUIRE_EQ(provider.preparations, 1, "later level edit prepared the inserted plugin again");
}

// Removing a VST3 effect while playing switches live. The removed instance
// drains on silent input beside the new graph: a declared tail for its
// length, an unbounded one for four seconds, fading out over its last
// drain_fade_frames. The source plays on throughout.
void live_vst3_effect_removal_drains_old_instance(std::size_t frames, bool bounded_tail) {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(15'500 + frames + (bounded_tail ? 0 : 1000));
  (void)take(state.apply({project::AddTrack{track}}));
  auto bounded = limits();
  bounded.max_frames = frames;
  bounded.max_buffer_bytes = 6ULL * frames * sizeof(engine::StereoFrame);
  LiveAdditionProvider provider;
  provider.hum = 0.0625F;
  provider.finite_tail = bounded_tail ? std::optional<std::uint32_t>(100) : std::nullopt;
  app::RoutedPlaybackControl control(state, bounded, &provider);
  REQUIRE_OK(control.submit_current(), "removal fixture could not prepare");
  std::array<engine::StereoFrame, 128> signal{}, output{};
  std::ranges::fill(std::span(signal).first(frames), engine::StereoFrame{0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, std::span(signal).first(frames));
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(frames), {0, 1}),
             engine::RoutedRenderStatus::rendered, "removal fixture did not start");
  control.observe();
  const auto inserted =
      take(state.apply({project::InsertVst3Plugin{track, 0, saved_fixture_insert()}}));
  REQUIRE_OK(control.begin_edit(inserted, "add VST3 effect"), "VST3 effect did not switch in live");
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(frames), {frames, 1}),
             engine::RoutedRenderStatus::rendered, "VST3 effect did not switch in live");
  near(output[0], 0.1875F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, inserted.snapshot->revision,
             "inserted VST3 effect did not become active");
  const auto slot = state.snapshot()->tracks.front().effects.front().id;
  const auto removed = take(state.apply({project::RemoveEffectInsert{track, slot}}));
  REQUIRE_OK(control.begin_edit(removed, "remove VST3 effect"),
             "VST3 removal held or was claimed audible before its switch");
  REQUIRE_TRUE(!control.held_boundary(),
               "VST3 removal held or was claimed audible before its switch");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "VST3 removal held or was claimed audible before its switch");
  const std::uint64_t ring = bounded_tail ? 100 : 4ULL * 48'000;
  const auto expected = [&](std::uint64_t frame) {
    if (frame >= ring)
      return 0.25F;
    const auto left = ring - frame;
    const auto gain = !bounded_tail && left < engine::drain_fade_frames
                          ? static_cast<float>(left) / static_cast<float>(engine::drain_fade_frames)
                          : 1.0F;
    return 0.25F + 0.0625F * gain;
  };
  const auto blocks = (ring + frames) / frames + 1;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    counted_allocations = 0;
    counting_allocations = true;
    const auto status =
        control.render_block(inputs, std::span(output).first(frames), {(block + 2) * frames, 1});
    counting_allocations = false;
    REQUIRE_EQ(status, engine::RoutedRenderStatus::rendered,
               "VST3 removal held or allocated while the old instance drained");
    REQUIRE_EQ(counted_allocations, 0,
               "VST3 removal held or allocated while the old instance drained");
    for (std::size_t frame = 0; frame < frames; ++frame)
      near(output[frame], expected(block * frames + frame));
    if (block == 0) {
      control.observe();
      REQUIRE_EQ(control.status().active_graph_revision, removed.snapshot->revision,
                 "removed VST3 effect stayed active after its switch");
      REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
                 "removed VST3 effect stayed active after its switch");
      REQUIRE_TRUE(!control.processor_fault(track).has_value(),
                   "removed VST3 effect stayed active after its switch");
    }
  }
}

// A VST3 effect that fails to prepare while playing marks its saved revision
// failed and leaves the old graph audible; a retry once the plugin is
// available switches live.
void failed_live_vst3_preparation_keeps_old_graph() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(14'700);
  (void)take(state.apply({project::AddTrack{track}}));
  LiveAdditionProvider provider;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  REQUIRE_OK(control.submit_current(), "failure fixture could not prepare dry graph");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "failure fixture dry path did not render");
  control.observe();
  const auto old_revision = control.status().active_graph_revision;
  provider.unavailable = true;
  const auto applied =
      take(state.apply({project::InsertVst3Plugin{track, 0, saved_fixture_insert()}}));
  const auto refused = control.begin_edit(applied, "add unavailable VST3");
  REQUIRE_TRUE(!refused, "provider failure lost the saved edit, held or silently activated it");
  REQUIRE_TRUE(!control.held_boundary(),
               "provider failure lost the saved edit, held or silently activated it");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "provider failure lost the saved edit, held or silently activated it");
  REQUIRE_EQ(control.status().model_revision, applied.snapshot->revision,
             "provider failure lost the saved edit, held or silently activated it");
  REQUIRE_EQ(control.status().active_graph_revision, old_revision,
             "provider failure lost the saved edit, held or silently activated it");
  REQUIRE_OK(control.status().failure,
             "provider failure lost the saved edit, held or silently activated it");
  REQUIRE_NE(control.status().failure->message.find("synthetic verified binding unavailable"),
             std::string::npos,
             "provider failure lost the saved edit, held or silently activated it");
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "failed preparation blocked the old dry callback");
  REQUIRE_EQ(counted_allocations, 0, "failed preparation blocked the old dry callback");
  for (const auto frame : output)
    near(frame, 0.25F);
  provider.unavailable = false;
  REQUIRE_OK(control.retry(), "retried VST3 addition did not switch live");
  REQUIRE_TRUE(!control.held_boundary(), "retried VST3 addition did not switch live");
  REQUIRE_EQ(control.render_block(inputs, output, {128, 1}), engine::RoutedRenderStatus::rendered,
             "retried VST3 addition did not switch live");
  near(output[0], 0.125F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, applied.snapshot->revision,
             "retried VST3 addition did not become active");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "retried VST3 addition did not become active");
}

// Adding a latent VST3 effect while playing switches live. The dry path's
// compensation grows by the new latency and crossfades to its new reading;
// afterwards both paths align again.
void latent_vst3_addition_realigns_parallel_pdc(std::size_t frames) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(14'710);
  const auto dry = id<project::TrackId>(14'711);
  (void)take(state.apply({project::AddTrack{wet}, project::AddTrack{dry}}));
  auto bounded = limits();
  bounded.max_frames = frames;
  bounded.max_buffer_bytes = 6ULL * frames * sizeof(engine::StereoFrame);
  BoundInsertFixture provider;
  provider.expected_max_frames = static_cast<std::uint32_t>(frames);
  app::RoutedPlaybackControl control(state, bounded, &provider);
  REQUIRE_OK(control.submit_current(), "dry parallel graph failed to prepare");
  REQUIRE_EQ(control.master_output_latency_frames(), 0, "dry parallel graph failed to prepare");
  std::array<engine::StereoFrame, 128> wet_block{}, dry_block{}, output{};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), wet, std::span(wet_block).first(frames));
  set_source(inputs, control.source_layout(), dry, std::span(dry_block).first(frames));
  wet_block[0] = {1.0F, -1.0F};
  dry_block[0] = {0.25F, -0.25F};
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(frames), {0, 1}),
             engine::RoutedRenderStatus::rendered, "dry parallel graph did not render");
  near(output[0], 1.25F);
  control.observe();
  const auto added = take(state.apply({project::InsertVst3Plugin{wet, 0, saved_fixture_insert()}}));
  REQUIRE_OK(control.begin_edit(added, "add latent VST3"),
             "latent VST3 addition held or was not prepared");
  REQUIRE_TRUE(!control.held_boundary(), "latent VST3 addition held or was not prepared");
  REQUIRE_EQ(provider.preparations, 1, "latent VST3 addition held or was not prepared");
  wet_block.fill({});
  dry_block.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = control.render_block(inputs, std::span(output).first(frames), {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "latent VST3 addition held or allocated at its switching block");
  REQUIRE_EQ(counted_allocations, 0,
             "latent VST3 addition held or allocated at its switching block");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, added.snapshot->revision,
             "latent VST3 addition did not become active with its latency");
  REQUIRE_EQ(control.master_output_latency_frames(), 7,
             "latent VST3 addition did not become active with its latency");
  std::uint64_t position = 2 * frames;
  for (; position < 2 * frames + 512; position += frames)
    REQUIRE_EQ(control.render_block(inputs, std::span(output).first(frames), {position, 1}),
               engine::RoutedRenderStatus::rendered, "latent graph did not keep playing");
  wet_block[0] = {1.0F, -1.0F};
  dry_block[0] = {0.25F, -0.25F};
  REQUIRE_EQ(control.render_block(inputs, std::span(output).first(frames), {position, 1}),
             engine::RoutedRenderStatus::rendered, "latent graph impulse block did not render");
  for (std::size_t frame = 0; frame < frames; ++frame)
    near(output[frame], frame == 7 ? 0.75F : 0.0F);
}

// A new VST3 state while playing prepares a new instance, which replaces the
// running one at the next block. Its latency is unchanged, so the parallel
// path keeps its exact alignment.
void live_vst3_state_edit_prepares_new_instance() {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(14'715);
  const auto dry = id<project::TrackId>(14'716);
  const auto plugin = saved_fixture_insert();
  (void)take(state.apply(
      {project::AddTrack{wet}, project::AddTrack{dry}, project::InsertVst3Plugin{wet, 0, plugin}}));
  BoundInsertFixture provider;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  REQUIRE_OK(control.submit_current(), "latent VST3 did not prepare");
  std::array<engine::StereoFrame, 64> wet_block{}, dry_block{}, output{};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  wet_block[0] = {1.0F, -1.0F};
  dry_block[0] = {0.25F, -0.25F};
  set_source(inputs, control.source_layout(), wet, wet_block);
  set_source(inputs, control.source_layout(), dry, dry_block);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "latent VST3 first block failed");
  near(output[7], 0.75F);
  control.observe();
  const auto old_revision = control.status().active_graph_revision;
  const auto old_owner = provider.owner.lock();
  const auto changed = take(state.apply({project::SetVst3ComponentState{plugin.id, {9, 8, 7}}}));
  REQUIRE_OK(control.begin_edit(changed, "VST3 state"),
             "VST3 state edit held, was not prepared or was claimed audible early");
  REQUIRE_TRUE(!control.held_boundary(),
               "VST3 state edit held, was not prepared or was claimed audible early");
  REQUIRE_EQ(provider.preparations, 2,
             "VST3 state edit held, was not prepared or was claimed audible early");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "VST3 state edit held, was not prepared or was claimed audible early");
  REQUIRE_EQ(control.status().model_revision, changed.snapshot->revision,
             "VST3 state edit held, was not prepared or was claimed audible early");
  REQUIRE_EQ(control.status().active_graph_revision, old_revision,
             "VST3 state edit held, was not prepared or was claimed audible early");
  wet_block.fill({});
  dry_block.fill({});
  wet_block[2] = {1.0F, -1.0F};
  dry_block[2] = {0.25F, -0.25F};
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered, "VST3 state switch held or allocated");
  REQUIRE_EQ(counted_allocations, 0, "VST3 state switch held or allocated");
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], frame == 9 ? 0.75F : 0.0F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "VST3 state edit kept the old instance or changed PDC");
  REQUIRE_TRUE(provider.owner.lock(), "VST3 state edit kept the old instance or changed PDC");
  REQUIRE_NE(provider.owner.lock(), old_owner,
             "VST3 state edit kept the old instance or changed PDC");
  REQUIRE_EQ(control.master_output_latency_frames(), 7,
             "VST3 state edit kept the old instance or changed PDC");
  REQUIRE_TRUE(!control.processor_fault(), "VST3 state edit kept the old instance or changed PDC");
}

// Bypassing a VST3 effect while playing switches live to the dry path and
// retires the plugin instance.
void live_vst3_bypass_switches_to_dry_path() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(1460);
  (void)take(state.apply(
      {project::AddTrack{audio}, project::InsertVst3Plugin{audio, 0, saved_fixture_insert()}}));
  const auto slot = state.snapshot()->tracks[0].effects[0].id;
  BoundInsertFixture provider;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  REQUIRE_OK(control.submit_current(), "active VST3 fixture did not prepare");
  REQUIRE_EQ(provider.preparations, 1, "active VST3 fixture did not prepare");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), audio, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "active VST3 graph did not render");
  near(output[7], 0.125F);
  control.observe();
  const auto bypassed = take(state.apply({project::SetEffectInsertEnabled{audio, slot, false}}));
  REQUIRE_OK(control.begin_edit(bypassed, "bypass VST3 effect"), "VST3 bypass held the graph");
  REQUIRE_TRUE(!control.held_boundary(), "VST3 bypass held the graph");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "VST3 bypass held or allocated at its switching block");
  REQUIRE_EQ(counted_allocations, 0, "VST3 bypass held or allocated at its switching block");
  // The output latency drops by the plugin's 7 frames, so time cannot be
  // kept: the old instance rings out the 7 samples it still held (0.125)
  // over the start of the dry path, and nothing is skipped or silent.
  for (std::size_t frame = 0; frame < output.size(); ++frame)
    near(output[frame], frame < 7 ? 0.375F : 0.25F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, bypassed.snapshot->revision,
             "VST3 bypass did not become active or prepared a plugin");
  REQUIRE_EQ(control.master_output_latency_frames(), 0,
             "VST3 bypass did not become active or prepared a plugin");
  REQUIRE_EQ(provider.preparations, 1, "VST3 bypass did not become active or prepared a plugin");
  REQUIRE_TRUE(!control.processor_fault(),
               "VST3 bypass did not become active or prepared a plugin");
  REQUIRE_TRUE(!bypassed.snapshot->tracks[0].effects[0].enabled,
               "VST3 bypass did not become active or prepared a plugin");
}

// One edit adding a VST3 effect to each of two tracks switches both chains
// live.
void live_vst3_edit_spanning_two_tracks_switches_live() {
  Gate gate;
  auto state = model(gate);
  const auto first_track = id<project::TrackId>(14'720);
  const auto second_track = id<project::TrackId>(14'721);
  (void)take(state.apply({project::AddTrack{first_track}, project::AddTrack{second_track}}));
  LiveAdditionProvider provider;
  app::RoutedPlaybackControl control(state, limits(), &provider);
  REQUIRE_OK(control.submit_current(), "two-track fixture did not prepare");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  set_source(inputs, control.source_layout(), first_track, signal);
  set_source(inputs, control.source_layout(), second_track, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "two-track fixture did not render");
  near(output[0], 0.5F);
  control.observe();
  auto first = saved_fixture_insert();
  auto second = first;
  second.id = id<project::PluginInstanceId>(11);
  const auto applied = take(state.apply({project::InsertVst3Plugin{first_track, 0, first},
                                         project::InsertVst3Plugin{second_track, 0, second}}));
  REQUIRE_OK(control.begin_edit(applied, "add two VST3 effects"),
             "two-track VST3 edit held or did not prepare both effects");
  REQUIRE_TRUE(!control.held_boundary(),
               "two-track VST3 edit held or did not prepare both effects");
  REQUIRE_EQ(provider.preparations, 2, "two-track VST3 edit held or did not prepare both effects");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "two-track VST3 edit held or allocated at its switching block");
  REQUIRE_EQ(counted_allocations, 0,
             "two-track VST3 edit held or allocated at its switching block");
  for (const auto frame : output)
    near(frame, 0.25F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, applied.snapshot->revision,
             "two-track VST3 edit did not become active");
  REQUIRE_TRUE(!control.processor_fault(first_track), "two-track VST3 edit did not become active");
  REQUIRE_TRUE(!control.processor_fault(second_track), "two-track VST3 edit did not become active");
}

void captured_audio_edit_keeps_old_graph_until_explicit_hold() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(9'001);
  const auto clip = id<project::ClipId>(9'002);
  const auto source = id<project::SourceId>(9'003);
  const project::SourceRecord record{
      source, project::SourceEncoding::wav, std::string(64, 'a'), 400'044, 48'000, 2, 100'000};
  (void)take(state.apply(
      {project::AddSource{record}, project::AddTrack{track},
       project::AddClip{
           track, project::Clip{clip, source, {{0}, {64}}, project::ElapsedInterval{{0}, {64}}}}}));
  app::RoutedPlaybackControl control(state, limits());
  control.set_live_switching(false); // covers held switching
  REQUIRE_OK(control.submit_current(), "audio-only capture fixture did not prepare");
  std::array<engine::StereoFrame, 4> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "audio-only capture fixture did not activate");
  control.observe();
  const auto active_revision = control.status().active_graph_revision;
  auto moved =
      take(state.apply({project::SetClipPlacement{clip, project::ElapsedInterval{{8}, {72}}}}));
  auto captured = control.capture_source_edit(moved, "move audio clip");
  REQUIRE_OK(captured,
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(captured.value().snapshot, moved.snapshot,
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(captured.value().active_graph_revision, active_revision,
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(captured.value().max_frames, control.max_frames(),
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(captured.value().source_layout.size(), control.source_layout().size(),
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(captured.value().topological_order.size(), control.topological_order().size(),
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(control.status().model_revision, moved.snapshot->revision,
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(control.status().active_graph_revision, active_revision,
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_TRUE(!control.held_boundary(),
               "capture requested a hold, changed the active graph or lost its immutable inputs");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::rendered,
             "capture requested a hold, changed the active graph or lost its immutable inputs");
  near(output[0], 0.25F);
  REQUIRE_OK(control.request_captured_source_hold(captured.value()),
             "prepared source capture did not request a bounded held boundary");
  REQUIRE_EQ(control.render_block(inputs, output, {8, 1}), engine::RoutedRenderStatus::holding,
             "prepared source capture did not request a bounded held boundary");
  REQUIRE_OK(control.held_boundary(),
             "prepared source capture did not request a bounded held boundary");
  const auto blocked = control.publish_held();
  REQUIRE_TRUE(!blocked, "unwired source switching activated a graph or lost the old audible path");
  REQUIRE_EQ(blocked.error().code, foundation::ErrorCode::unsupported,
             "unwired source switching activated a graph or lost the old audible path");
  REQUIRE_EQ(control.render_block(inputs, output, {8, 1}), engine::RoutedRenderStatus::rendered,
             "unwired source switching activated a graph or lost the old audible path");
  REQUIRE_EQ(control.status().model_revision, moved.snapshot->revision,
             "unwired source switching activated a graph or lost the old audible path");
  REQUIRE_EQ(control.status().active_graph_revision, active_revision,
             "unwired source switching activated a graph or lost the old audible path");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "unwired source switching activated a graph or lost the old audible path");
}

void exact_split_is_source_only_but_arbitrary_add_is_not() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(9'031);
  const auto original = id<project::ClipId>(9'032);
  const auto right = id<project::ClipId>(9'033);
  const auto extra = id<project::ClipId>(9'034);
  const auto source = id<project::SourceId>(9'035);
  const project::SourceRecord record{
      source, project::SourceEncoding::wav, std::string(64, 'a'), 400'044, 48'000, 2, 100'000};
  (void)take(state.apply(
      {project::AddSource{record}, project::AddTrack{track},
       project::AddClip{
           track,
           project::Clip{original, source, {{0}, {256}}, project::ElapsedInterval{{0}, {256}}}}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "split admission graph did not prepare");
  std::array<engine::StereoFrame, 64> signal{}, output{};
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "split admission graph did not activate");
  control.observe();
  const auto split = take(state.apply(
      {project::SetClipPlacement{original, project::ElapsedInterval{{0}, {128}}},
       project::SetClipSourceRange{original, project::SourceRange{{0}, {128}}},
       project::AddClip{
           track,
           project::Clip{right, source, {{128}, {256}}, project::ElapsedInterval{{128}, {256}}}}}));
  auto captured = take(control.capture_source_edit(split, "split WAV clip"));
  REQUIRE_TRUE(captured.source_overlay_eligible,
               "exact managed-WAV partition was not eligible for source-only handoff");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "exact managed-WAV partition was not eligible for source-only handoff");
  const auto added = take(state.apply({project::AddClip{
      track, project::Clip{extra, source, {{0}, {128}}, project::ElapsedInterval{{256}, {384}}}}}));
  captured = take(control.capture_source_edit(added, "add another clip"));
  REQUIRE_TRUE(!captured.source_overlay_eligible,
               "arbitrary clip insertion entered exact-split source-only handoff");
  REQUIRE_LT(control.status().active_graph_revision, added.snapshot->revision,
             "arbitrary clip insertion entered exact-split source-only handoff");
}

void captured_audio_edit_failure_and_supersession_do_not_hold() {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(9'011);
  const auto clip = id<project::ClipId>(9'012);
  const auto source = id<project::SourceId>(9'013);
  const project::SourceRecord record{
      source, project::SourceEncoding::wav, std::string(64, 'a'), 400'044, 48'000, 2, 100'000};
  (void)take(state.apply(
      {project::AddSource{record}, project::AddTrack{track},
       project::AddClip{
           track, project::Clip{clip, source, {{0}, {64}}, project::ElapsedInterval{{0}, {64}}}}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "failure capture fixture did not prepare");
  std::array<engine::StereoFrame, 4> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  set_source(inputs, control.source_layout(), track, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "failure capture fixture did not activate");
  control.observe();
  const auto old_revision = control.status().active_graph_revision;
  auto first =
      take(state.apply({project::SetClipPlacement{clip, project::ElapsedInterval{{8}, {72}}}}));
  auto captured_first = take(control.capture_source_edit(first, "first move"));
  auto second =
      take(state.apply({project::SetClipSourceRange{clip, project::SourceRange{{8}, {72}}}}));
  auto captured_second = take(control.capture_source_edit(second, "trim clip"));
  REQUIRE_TRUE(!control.request_captured_source_hold(captured_first),
               "stale worker capture changed the current revision or held playback");
  REQUIRE_TRUE(!control.fail_captured_source_edit(
                   captured_first, {foundation::ErrorCode::io_failure, "obsolete worker failed"}),
               "stale worker capture changed the current revision or held playback");
  REQUIRE_EQ(control.status().model_revision, second.snapshot->revision,
             "stale worker capture changed the current revision or held playback");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "stale worker capture changed the current revision or held playback");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::rendered,
             "stale worker capture changed the current revision or held playback");
  auto failed = control.fail_captured_source_edit(
      captured_second, {foundation::ErrorCode::io_failure, "candidate media unavailable"});
  REQUIRE_TRUE(!failed, "failed source preparation requested a hold or hid the saved/active split");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::io_failure,
             "failed source preparation requested a hold or hid the saved/active split");
  REQUIRE_EQ(control.status().model_revision, second.snapshot->revision,
             "failed source preparation requested a hold or hid the saved/active split");
  REQUIRE_EQ(control.status().active_graph_revision, old_revision,
             "failed source preparation requested a hold or hid the saved/active split");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "failed source preparation requested a hold or hid the saved/active split");
  REQUIRE_TRUE(!control.request_captured_source_hold(captured_second),
               "failed source preparation requested a hold or hid the saved/active split");
  REQUIRE_EQ(control.render_block(inputs, output, {8, 1}), engine::RoutedRenderStatus::rendered,
             "failed source preparation requested a hold or hid the saved/active split");
}

void captured_audio_edit_rejects_synth_graph_before_hold() {
  Gate gate;
  auto state = model(gate);
  const auto audio = id<project::TrackId>(9'021);
  const auto instrument = id<project::TrackId>(9'022);
  const auto clip = id<project::ClipId>(9'023);
  const auto source = id<project::SourceId>(9'024);
  const project::SourceRecord record{
      source, project::SourceEncoding::wav, std::string(64, 'a'), 400'044, 48'000, 2, 100'000};
  (void)take(state.apply(
      {project::AddSource{record}, project::AddTrack{audio},
       project::AddClip{
           audio, project::Clip{clip, source, {{0}, {64}}, project::ElapsedInterval{{0}, {64}}}},
       project::AddTrack{instrument, project::TrackKind::instrument},
       project::SetNativeInstrumentPatch{instrument,
                                         project::NativeInstrumentPatch{"prism", 1, {}}}}));
  app::RoutedPlaybackControl control(state, limits());
  REQUIRE_OK(control.submit_current(), "synth exclusion fixture did not prepare");
  std::array<engine::StereoFrame, 4> signal{}, output{};
  signal.fill({0.25F, -0.25F});
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  set_source(inputs, control.source_layout(), audio, signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "synth exclusion fixture did not activate");
  control.observe();
  const auto active = control.status().active_graph_revision;
  auto moved =
      take(state.apply({project::SetClipPlacement{clip, project::ElapsedInterval{{8}, {72}}}}));
  auto refused = control.capture_source_edit(moved, "move with active synth");
  REQUIRE_TRUE(!refused, "audio edit capture reset or held an instrument-bearing graph");
  REQUIRE_EQ(refused.error().code, foundation::ErrorCode::unsupported,
             "audio edit capture reset or held an instrument-bearing graph");
  REQUIRE_EQ(control.status().model_revision, moved.snapshot->revision,
             "audio edit capture reset or held an instrument-bearing graph");
  REQUIRE_EQ(control.status().active_graph_revision, active,
             "audio edit capture reset or held an instrument-bearing graph");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "audio edit capture reset or held an instrument-bearing graph");
  REQUIRE_TRUE(!control.held_boundary(),
               "audio edit capture reset or held an instrument-bearing graph");
  REQUIRE_EQ(control.render_block(inputs, output, {4, 1}), engine::RoutedRenderStatus::rendered,
             "audio edit capture reset or held an instrument-bearing graph");
}
} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "routed playback control contract passed",
      {
          {"live_channel_automation_uses_sample_values_before_routing",
           live_channel_automation_uses_sample_values_before_routing},
          {"master_level_uses_routed_output_after_graph_publication",
           master_level_uses_routed_output_after_graph_publication},
          {"live_mute_solo_and_inaudible_solo_revision_do_not_hold",
           live_mute_solo_and_inaudible_solo_revision_do_not_hold},
          {"live_route_gain_edit_and_failed_model_divergence",
           live_route_gain_edit_and_failed_model_divergence},
          {"live_send_gain_uses_existing_route_without_hold",
           [] {
             live_send_gain_uses_existing_route_without_hold(64);
             live_send_gain_uses_existing_route_without_hold(128);
           }},
          {"automated_send_gain_refuses_live_overlay", automated_send_gain_refuses_live_overlay},
          {"live_send_automation_survives_graph_handoff",
           live_send_automation_survives_graph_handoff},
          {"initial_master_chain_keeps_history_through_route_edit",
           initial_master_chain_keeps_history_through_route_edit},
          {"source_layout_change_is_rejected_without_misrouting",
           source_layout_change_is_rejected_without_misrouting},
          {"verified_track_insert_and_parallel_pdc",
           [] {
             verified_track_insert_and_parallel_pdc(64);
             verified_track_insert_and_parallel_pdc(128);
           }},
          {"failed_two_effect_publication_keeps_owners_uncommitted",
           failed_two_effect_publication_keeps_owners_uncommitted},
          {"live_insert_without_vst3_host_keeps_prior_graph",
           live_insert_without_vst3_host_keeps_prior_graph},
          {"mixed_instrument_and_effect_preserves_both_nodes",
           [] {
             mixed_instrument_and_effect_preserves_both_nodes(true);
             mixed_instrument_and_effect_preserves_both_nodes(false);
           }},
          {"instrument_two_serial_effects_keep_order_and_history",
           [] {
             instrument_two_serial_effects_keep_order_and_history(64);
             instrument_two_serial_effects_keep_order_and_history(128);
           }},
          {"two_instrument_effect_paths_keep_independent_state",
           [] {
             two_instrument_effect_paths_keep_independent_state(64);
             two_instrument_effect_paths_keep_independent_state(128);
           }},
          {"synth_track_native_effect_processes_source_without_allocations",
           synth_track_native_effect_processes_source_without_allocations},
          {"two_verified_effects_align_and_survive_revisions",
           [] {
             two_verified_effects_align_and_survive_revisions(64);
             two_verified_effects_align_and_survive_revisions(128);
           }},
          {"serial_effects_preserve_order_state_and_failed_graph",
           [] {
             serial_effects_preserve_order_state_and_failed_graph(64);
             serial_effects_preserve_order_state_and_failed_graph(128);
           }},
          {"separate_native_track_chains_preserve_independent_history",
           separate_native_track_chains_preserve_independent_history},
          {"native_group_eq_processes_routed_sum_and_retains_state",
           native_group_eq_processes_routed_sum_and_retains_state},
          {"native_return_eq_processes_send_after_gain",
           native_return_eq_processes_send_after_gain},
          {"mixed_native_vst3_keeps_order_and_state",
           [] {
             mixed_native_vst3_keeps_order_and_state(64, false, false);
             mixed_native_vst3_keeps_order_and_state(128, false, false);
             mixed_native_vst3_keeps_order_and_state(64, true, false);
             mixed_native_vst3_keeps_order_and_state(128, true, false);
             mixed_native_vst3_keeps_order_and_state(64, false, true);
             mixed_native_vst3_keeps_order_and_state(128, false, true);
             mixed_native_vst3_keeps_order_and_state(64, true, true);
             mixed_native_vst3_keeps_order_and_state(128, true, true);
           }},
          {"live_earlier_stage_removal_keeps_later_stage",
           live_earlier_stage_removal_keeps_later_stage},
          {"native_eq_compressor_chain_keeps_order_and_history",
           [] {
             native_eq_compressor_chain_keeps_order_and_history(64);
             native_eq_compressor_chain_keeps_order_and_history(128);
           }},
          {"native_sidechain_send_matches_the_independent_compressor",
           [] {
             native_sidechain_send_matches_the_independent_compressor(64);
             native_sidechain_send_matches_the_independent_compressor(128);
           }},
          {"vst3_sidechain_uses_verified_detector_and_keeps_prior_graph",
           [] {
             vst3_sidechain_uses_verified_detector_and_keeps_prior_graph(64);
             vst3_sidechain_uses_verified_detector_and_keeps_prior_graph(128);
           }},
          {"saved_bypassed_vst3_effect_is_dry_and_enables_live",
           saved_bypassed_vst3_effect_is_dry_and_enables_live},
          {"live_vst3_effect_addition_keeps_source_without_silence",
           [] {
             live_vst3_effect_addition_keeps_source_without_silence(64);
             live_vst3_effect_addition_keeps_source_without_silence(128);
           }},
          {"live_vst3_effect_removal_drains_old_instance",
           [] {
             live_vst3_effect_removal_drains_old_instance(64, true);
             live_vst3_effect_removal_drains_old_instance(128, true);
             live_vst3_effect_removal_drains_old_instance(64, false);
             live_vst3_effect_removal_drains_old_instance(128, false);
           }},
          {"failed_live_vst3_preparation_keeps_old_graph",
           failed_live_vst3_preparation_keeps_old_graph},
          {"latent_vst3_addition_realigns_parallel_pdc",
           [] {
             latent_vst3_addition_realigns_parallel_pdc(64);
             latent_vst3_addition_realigns_parallel_pdc(128);
           }},
          {"live_vst3_state_edit_prepares_new_instance",
           live_vst3_state_edit_prepares_new_instance},
          {"live_vst3_bypass_switches_to_dry_path", live_vst3_bypass_switches_to_dry_path},
          {"live_vst3_edit_spanning_two_tracks_switches_live",
           live_vst3_edit_spanning_two_tracks_switches_live},
          {"missing_insert_provider_does_not_publish", missing_insert_provider_does_not_publish},
          {"pending_supersession_and_hold_validation", pending_supersession_and_hold_validation},
          {"unadoptable_live_switch_is_withdrawn_and_next_edit_publishes",
           unadoptable_live_switch_is_withdrawn_and_next_edit_publishes},
          {"changed_model_during_hold_is_reported", changed_model_during_hold_is_reported},
          {"constrained_publication_preserves_failed_revision",
           constrained_publication_preserves_failed_revision},
          {"terminal_limiter_alignment_and_tail",
           [] {
             terminal_limiter_alignment_and_tail(64);
             terminal_limiter_alignment_and_tail(128);
           }},
          {"terminal_output_latency_is_separate_from_edge_compensation",
           terminal_output_latency_is_separate_from_edge_compensation},
          {"captured_audio_edit_keeps_old_graph_until_explicit_hold",
           captured_audio_edit_keeps_old_graph_until_explicit_hold},
          {"exact_split_is_source_only_but_arbitrary_add_is_not",
           exact_split_is_source_only_but_arbitrary_add_is_not},
          {"captured_audio_edit_failure_and_supersession_do_not_hold",
           captured_audio_edit_failure_and_supersession_do_not_hold},
          {"captured_audio_edit_rejects_synth_graph_before_hold",
           captured_audio_edit_rejects_synth_graph_before_hold},
      });
}
