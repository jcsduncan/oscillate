#include "oscillate/application/vst3/track_insert_provider.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace oscillate;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t low, std::uint64_t high = 0) {
  return take(Id::from_bits({high, low}));
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

struct TemporaryProject {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("oscillate-serial-vst3-" + std::to_string(getpid()));
  TemporaryProject() { std::filesystem::remove_all(path); }
  ~TemporaryProject() { std::filesystem::remove_all(path); }
};

project::ProjectModel model(Gate& gate) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000, std::move(tempo),
                                            std::move(meter), gate));
}

project::Vst3PluginInstance saved(const plugins::ScanResult& scan, float gain) {
  const auto& klass = scan.metadata.classes.front();
  project::Vst3PluginInstance result{.id = id<project::PluginInstanceId>(10),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = "/tmp/Advisory.vst3",
                                     .binary_sha256 = scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  result.component_state.resize(sizeof(gain));
  std::memcpy(result.component_state.data(), &gain, sizeof(gain));
  return result;
}

void source(std::span<std::span<const engine::StereoFrame>> inputs,
            std::span<const routing::ProjectRouteSource> layout, project::TrackId track,
            std::span<const engine::StereoFrame> frames) {
  for (const auto& item : layout) {
    if (item.track == track) {
      inputs[item.topological_index] = frames;
      return;
    }
  }
  throw std::runtime_error("source track absent from route layout");
}

void near(engine::StereoFrame frame, float expected) {
  if (std::fabs(frame.left - expected) >= 0.00001F || std::fabs(frame.right + expected) >= 0.00001F)
    throw std::runtime_error("VST3 route sample left=" + std::to_string(frame.left) + " right=" +
                             std::to_string(frame.right) + " expected=" + std::to_string(expected));
}

void verified_effect_only_processes_own_track(const plugins::ScanResult& scanned,
                                              std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2);
  const auto dry = id<project::TrackId>(3);
  const auto returns = id<project::TrackId>(4);
  const auto send = id<project::SendId>(5);
  const auto effect = saved(scanned, -0.5F);
  (void)take(state.apply({project::AddTrack{wet}, project::AddTrack{dry},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::InsertVst3Plugin{wet, 0, effect}}));
  auto limits = engine::RoutedMixerLimits{{1, 64},
                                          128,
                                          5ULL * 128 * sizeof(engine::StereoFrame),
                                          5ULL * 65 * sizeof(engine::StereoFrame)};
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(effect.id, scanned);
  application::RoutedPlaybackControl control(state, limits, &provider);
  REQUIRE_OK(control.submit_current(), "verified VST3 route did not prepare");
  std::array<engine::StereoFrame, 128> wet_signal{}, dry_signal{}, output{};
  wet_signal[0] = {1.0F, -1.0F};
  wet_signal[block_frames - 1] = {1.0F, -1.0F};
  dry_signal[0] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  source(inputs, control.source_layout(), wet, std::span(wet_signal).first(block_frames));
  source(inputs, control.source_layout(), dry, std::span(dry_signal).first(block_frames));
  counting_allocations = true;
  const auto first = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, engine::RoutedRenderStatus::rendered, "verified VST3 route did not render");
  REQUIRE_EQ(counted_allocations, 0, "verified VST3 route did not render");
  near(output[0], 0.0F);
  near(output[6], 0.0F);
  near(output[7], 1.5F); // Effect and independent dry route align at seven frames.
  require(!control.processor_fault(), "healthy VST3 effect was reported failed");
  control.observe();

  auto revised = take(state.apply({project::AddSendRoute{wet, {send, returns, 0.5}}}));
  // A route edit around an unchanged VST3 insert switches live: no hold.
  require(control.begin_edit(revised, "add send").has_value(),
          "verified VST3 runtime did not survive live route preparation");
  wet_signal.fill({});
  dry_signal.fill({});
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered, "verified VST3 route did not switch");
  REQUIRE_EQ(counted_allocations, 0, "verified VST3 route did not switch");
  near(output[0], 0.0F);
  near(output[6], 0.75F); // The live route edit retains the wet tail and new send gain.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "VST3 topology edit did not activate");
  REQUIRE_EQ(state.snapshot()->tracks[0].effects.size(), 1,
             "route edit changed saved insert ID, position or component state");
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(state.snapshot()->tracks[0].effects.front().processor),
      effect, "route edit changed saved insert ID, position or component state");

  auto changed = take(state.apply({project::SetVst3ComponentState{effect.id, {1, 2, 3, 4}}}));
  // A changed state loads into a new instance, which switches in live
  // (ADR-0050): no hold, and the saved state is the audible one.
  require(control.begin_edit(changed, "plugin state").has_value() &&
              control.render_block(inputs, std::span(output).first(block_frames),
                                   {block_frames * 2, 1}) == engine::RoutedRenderStatus::rendered,
          "changed VST3 state held or silenced a block");
  control.observe();
  REQUIRE_EQ(control.status().model_revision, changed.snapshot->revision,
             "changed VST3 state did not switch live to a new instance");
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "changed VST3 state did not switch live to a new instance");
  REQUIRE_EQ(control.status().application, application::PlaybackApplication::active,
             "changed VST3 state did not switch live to a new instance");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(state.snapshot()->tracks[0].effects[0].processor)
                 .component_state,
             std::vector<std::uint8_t>({1, 2, 3, 4}),
             "changed VST3 state did not switch live to a new instance");
}

// A VST3 effect whose state changes its latency is replaced live, and the
// parallel dry path's delay compensation moves with it (ADR-0005, #102): no
// block holds, fails or allocates, the revision activates without a restart,
// and once the switch has settled both paths are aligned at the new latency.
void latency_change_replaces_the_plugin_live(const plugins::ScanResult& scanned,
                                             std::size_t block_frames) {
  // The fixture's latency at its ordinary gain, and at its reserved
  // 96-frame state, where it plays at half gain.
  constexpr std::size_t short_latency = 7;
  constexpr std::size_t long_latency = 96;
  constexpr float long_state = -96.0F;
  constexpr float short_gain = 0.5F;
  constexpr float long_gain = 0.5F;
  constexpr std::uint32_t compensation_frames = 128;
  constexpr std::uint64_t settle_blocks = 8;
  constexpr std::size_t routes = 8;
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2);
  const auto dry = id<project::TrackId>(3);
  const auto effect = saved(scanned, -short_gain);
  (void)take(state.apply(
      {project::AddTrack{wet}, project::AddTrack{dry}, project::InsertVst3Plugin{wet, 0, effect}}));
  const engine::RoutedMixerLimits limits{
      .latency = {1, compensation_frames},
      .max_frames = block_frames,
      .max_buffer_bytes = routes * block_frames * sizeof(engine::StereoFrame),
      .max_history_bytes = routes * (compensation_frames + 1) * sizeof(engine::StereoFrame)};
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(effect.id, scanned);
  application::RoutedPlaybackControl control(state, limits, &provider);
  REQUIRE_OK(control.submit_current(), "the latent VST3 route did not prepare");
  std::vector<engine::StereoFrame> wet_signal(block_frames), dry_signal(block_frames),
      output(block_frames);
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  source(inputs, control.source_layout(), wet, wet_signal);
  source(inputs, control.source_layout(), dry, dry_signal);
  const auto play = [&](std::uint64_t block, bool impulse) {
    wet_signal.assign(block_frames, {});
    dry_signal.assign(block_frames, {});
    if (impulse) {
      wet_signal[0] = {1.0F, -1.0F};
      dry_signal[0] = {1.0F, -1.0F};
    }
    counted_allocations = 0;
    counting_allocations = true;
    const auto rendered = control.render_block(inputs, output, {block * block_frames, 1});
    counting_allocations = false;
    REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered, "a block held or failed");
    REQUIRE_EQ(counted_allocations, 0, "a block allocated");
    control.observe();
  };
  play(0, true);
  near(output[short_latency], short_gain + 1.0F);

  auto latent = effect;
  std::memcpy(latent.component_state.data(), &long_state, sizeof(long_state));
  const auto changed =
      take(state.apply({project::SetVst3ComponentState{effect.id, latent.component_state}}));
  REQUIRE_OK(control.begin_edit(changed, "latency change"),
             "a latency-changing VST3 state was refused while playing");
  for (std::uint64_t block = 1; block < settle_blocks; ++block)
    play(block, false);
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "the latency change did not become active without a restart");
  REQUIRE_EQ(control.status().application, application::PlaybackApplication::active,
             "the latency change did not become active without a restart");
  // Both paths meet again, at the new latency only.
  std::vector<engine::StereoFrame> heard;
  for (auto block = settle_blocks; heard.size() <= long_latency; ++block) {
    play(block, block == settle_blocks);
    heard.insert(heard.end(), output.begin(), output.end());
  }
  for (std::size_t frame = 0; frame <= long_latency; ++frame)
    near(heard[frame], frame == long_latency ? long_gain + 1.0F : 0.0F);
  require(!control.processor_fault(), "the replaced VST3 effect faulted");
}

void manual_effect_binding_tracks_callback_graph(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(80);
  const auto returns = id<project::TrackId>(81);
  auto effect = saved(scanned, 0.5F);
  effect.missing = false;
  (void)take(state.apply({project::AddTrack{wet},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::InsertVst3Plugin{wet, 0, effect}}));
  const auto slot_id = state.snapshot()->tracks.front().effects.front().id;
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(effect.id, scanned);
  std::shared_ptr<plugins::Vst3ManualEventSlot> retained;
  {
    application::RoutedPlaybackControl control(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
    REQUIRE_OK(control.submit_current(), "manual binding fixture did not prepare");
    const auto first_revision = state.snapshot()->revision;
    require(!control.active_vst3_effect_manual_events(wet, slot_id, effect.id, first_revision),
            "prepared graph leaked a manual event binding before callback acceptance");
    std::array<engine::StereoFrame, 64> input{}, output{};
    input.fill({0.25F, -0.25F});
    std::array<std::span<const engine::StereoFrame>, 3> inputs{};
    source(inputs, control.source_layout(), wet, input);
    REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
               "manual binding fixture did not activate");
    control.observe();
    auto binding =
        control.active_vst3_effect_manual_events(wet, slot_id, effect.id, first_revision);
    REQUIRE_TRUE(binding, "active saved effect did not expose its own callback-accepted slot");
    REQUIRE_TRUE(binding->slot,
                 "active saved effect did not expose its own callback-accepted slot");
    REQUIRE_EQ(binding->identity.graph_revision, first_revision,
               "active saved effect did not expose its own callback-accepted slot");
    REQUIRE_EQ(binding->identity.instance_id, provider.assigned_instance_id(effect.id),
               "active saved effect did not expose its own callback-accepted slot");
    REQUIRE_NE(binding->identity.compatibility_key, 0,
               "active saved effect did not expose its own callback-accepted slot");
    retained = binding->slot;
    REQUIRE_TRUE(
        !control.active_vst3_effect_manual_events(returns, slot_id, effect.id, first_revision),
        "manual binding accepted a mismatched owner, slot, plugin, or graph revision");
    REQUIRE_TRUE(!control.active_vst3_effect_manual_events(wet, id<project::InsertSlotId>(82),
                                                           effect.id, first_revision),
                 "manual binding accepted a mismatched owner, slot, plugin, or graph revision");
    REQUIRE_TRUE(!control.active_vst3_effect_manual_events(
                     wet, slot_id, id<project::PluginInstanceId>(83), first_revision),
                 "manual binding accepted a mismatched owner, slot, plugin, or graph revision");
    REQUIRE_TRUE(
        !control.active_vst3_effect_manual_events(wet, slot_id, effect.id, first_revision + 1),
        "manual binding accepted a mismatched owner, slot, plugin, or graph revision");

    const auto revised =
        take(state.apply({project::AddSendRoute{wet, {id<project::SendId>(84), returns, 0.5}}}));
    // The route edit switches live; until the callback adopts it, neither
    // revision exposes a manual binding.
    require(
        control.begin_edit(revised, "manual binding route") &&
            !control.active_vst3_effect_manual_events(wet, slot_id, effect.id, first_revision) &&
            !control.active_vst3_effect_manual_events(wet, slot_id, effect.id,
                                                      revised.snapshot->revision),
        "held or prepared successor graph exposed a manual event binding");
    REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::rendered,
               "manual binding successor graph did not activate");
    control.observe();
    auto successor = control.active_vst3_effect_manual_events(wet, slot_id, effect.id,
                                                              revised.snapshot->revision);
    REQUIRE_TRUE(successor,
                 "topology revision lost processor slot state or retained stale graph identity");
    REQUIRE_EQ(successor->slot, retained,
               "topology revision lost processor slot state or retained stale graph identity");
    REQUIRE_EQ(successor->identity.instance_id, binding->identity.instance_id,
               "topology revision lost processor slot state or retained stale graph identity");
    REQUIRE_TRUE(!control.active_vst3_effect_manual_events(wet, slot_id, effect.id, first_revision),
                 "topology revision lost processor slot state or retained stale graph identity");

    const auto changed =
        take(state.apply({project::SetVst3ComponentState{effect.id, {1, 2, 3, 4}}}));
    // The changed state runs in a new instance with its own manual slot; the
    // old revision's slot is gone.
    require(control.begin_edit(changed, "changed state") &&
                control.render_block(inputs, output, {128, 1}) ==
                    engine::RoutedRenderStatus::rendered,
            "a VST3 state change held or silenced a block");
    control.observe();
    const auto replaced = control.active_vst3_effect_manual_events(wet, slot_id, effect.id,
                                                                   changed.snapshot->revision);
    REQUIRE_TRUE(!control.active_vst3_effect_manual_events(wet, slot_id, effect.id,
                                                           revised.snapshot->revision),
                 "a VST3 state change kept the old instance's manual slot");
    REQUIRE_TRUE(replaced, "a VST3 state change kept the old instance's manual slot");
    REQUIRE_NE(replaced->identity.instance_id, binding->identity.instance_id,
               "a VST3 state change kept the old instance's manual slot");
  }
  REQUIRE_TRUE(retained, "retired playback owner left a live manual event slot");
  REQUIRE_EQ(retained->active_graph().graph_revision, 0,
             "retired playback owner left a live manual event slot");
}

void channel_control_overlay_rebinds_routed_vst3_manual_slot(const plugins::ScanResult& scanned,
                                                             std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(85);
  auto effect = saved(scanned, 0.5F);
  effect.missing = false;
  (void)take(state.apply({project::AddTrack{wet}, project::InsertVst3Plugin{wet, 0, effect}}));
  const auto slot_id = state.snapshot()->tracks.front().effects.front().id;
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(effect.id, scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "VST3 channel overlay fixture did not prepare");
  const auto original_revision = state.snapshot()->revision;
  std::array<engine::StereoFrame, 128> input{}, output{};
  input.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  source(inputs, control.source_layout(), wet, std::span(input).first(block_frames));
  const auto first_status =
      control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  REQUIRE_EQ(first_status, engine::RoutedRenderStatus::rendered,
             "VST3 channel overlay fixture did not activate: " +
                 std::to_string(static_cast<int>(first_status)));
  control.observe();
  const auto before =
      control.active_vst3_effect_manual_events(wet, slot_id, effect.id, original_revision);
  REQUIRE_TRUE(before, "VST3 channel overlay fixture lacked its manual slot");
  REQUIRE_TRUE(before->slot, "VST3 channel overlay fixture lacked its manual slot");
  const auto changed = take(state.apply({project::SetTrackMuted{wet, true}}));
  REQUIRE_OK(control.begin_edit(changed, "mute VST3 track"),
             "VST3 channel overlay refused the control edit");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "VST3 channel overlay held or allocated in the callback");
  REQUIRE_EQ(counted_allocations, 0, "VST3 channel overlay held or allocated in the callback");
  control.observe();
  const auto after =
      control.active_vst3_effect_manual_events(wet, slot_id, effect.id, changed.snapshot->revision);
  REQUIRE_TRUE(after, "VST3 channel overlay reset its processor or kept the stale manual identity");
  REQUIRE_EQ(after->slot, before->slot,
             "VST3 channel overlay reset its processor or kept the stale manual identity");
  REQUIRE_EQ(after->identity.instance_id, before->identity.instance_id,
             "VST3 channel overlay reset its processor or kept the stale manual identity");
  REQUIRE_TRUE(
      !control.active_vst3_effect_manual_events(wet, slot_id, effect.id, original_revision),
      "VST3 channel overlay reset its processor or kept the stale manual identity");
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "VST3 channel overlay reset its processor or kept the stale manual identity");
}

void missing_scan_and_process_failure(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2);
  const auto dry = id<project::TrackId>(3);
  auto effect = saved(scanned, 999.0F);
  effect.missing = false;
  (void)take(state.apply(
      {project::AddTrack{wet}, project::AddTrack{dry}, project::InsertVst3Plugin{wet, 0, effect}}));
  plugins::Vst3TrackInsertProvider provider;
  application::RoutedPlaybackControl missing(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_TRUE(!missing.submit_current(),
               "missing scan receipt discarded placeholder or activated an unverified effect");
  REQUIRE_EQ(missing.status().application, application::PlaybackApplication::failed,
             "missing scan receipt discarded placeholder or activated an unverified effect");
  REQUIRE_EQ(missing.status().active_graph_revision, 0,
             "missing scan receipt discarded placeholder or activated an unverified effect");
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(state.snapshot()->tracks[0].effects[0].processor).id,
      effect.id, "missing scan receipt discarded placeholder or activated an unverified effect");

  auto wrong_binary = scanned;
  wrong_binary.binary.sha256[0] = wrong_binary.binary.sha256[0] == 'a' ? 'b' : 'a';
  provider.set_scan_receipt(effect.id, wrong_binary);
  application::RoutedPlaybackControl mismatch(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_TRUE(!mismatch.submit_current(),
               "changed binary scan was accepted for saved plugin identity");
  REQUIRE_EQ(mismatch.status().application, application::PlaybackApplication::failed,
             "changed binary scan was accepted for saved plugin identity");
  REQUIRE_EQ(mismatch.status().active_graph_revision, 0,
             "changed binary scan was accepted for saved plugin identity");

  provider.set_scan_receipt(effect.id, scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "failure fixture did not prepare");
  std::array<engine::StereoFrame, 64> wet_signal{}, dry_signal{}, output{};
  wet_signal.fill({1.0F, -1.0F});
  dry_signal[0] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  source(inputs, control.source_layout(), wet, wet_signal);
  source(inputs, control.source_layout(), dry, dry_signal);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "faulting VST3 route did not render its isolated fallback");
  near(output[0], 0.0F);
  near(output[7], 1.0F); // Fault mutes wet track; dry track remains audible.
  control.observe();
  REQUIRE_TRUE(control.processor_fault(),
               "runtime process fault was not separately reported from graph revision");
  REQUIRE_EQ(control.status().application, application::PlaybackApplication::active,
             "runtime process fault was not separately reported from graph revision");
  require(!control.active_vst3_effect_manual_events(
              wet, state.snapshot()->tracks.front().effects.front().id, effect.id,
              state.snapshot()->revision),
          "faulted VST3 processor exposed a manual event slot");
}

void provider_assigns_fresh_high_half_ids(const plugins::ScanResult& scanned) {
  constexpr auto first_engine_id = std::uint64_t{1} << 63;
  Gate gate;
  auto state = model(gate);
  plugins::Vst3TrackInsertProvider provider;
  auto first = saved(scanned, 0.5F);
  auto second = saved(scanned, -0.5F);
  auto failing = saved(scanned, 0.25F);
  auto fourth = saved(scanned, 0.75F);
  first.id = id<project::PluginInstanceId>(10, 1);
  second.id = id<project::PluginInstanceId>(10, 2);
  failing.id = id<project::PluginInstanceId>(10, 3);
  fourth.id = id<project::PluginInstanceId>(10, 4);
  REQUIRE_TRUE(!provider.assigned_instance_id(first.id),
               "unprepared saved effects already held graph identities");
  REQUIRE_TRUE(!provider.assigned_instance_id(second.id),
               "unprepared saved effects already held graph identities");
  provider.set_scan_receipt(first.id, scanned);
  auto first_runtime = take(provider.prepare(*state.snapshot(), first, 128));
  REQUIRE_TRUE(first_runtime.processor,
               "first full saved plugin ID was not assigned the first high-half graph ID");
  REQUIRE_EQ(provider.assigned_instance_id(first.id), first_engine_id,
             "first full saved plugin ID was not assigned the first high-half graph ID");

  auto bad_scan = scanned;
  bad_scan.binary.sha256[0] = bad_scan.binary.sha256[0] == 'a' ? 'b' : 'a';
  provider.set_scan_receipt(failing.id, bad_scan);
  auto rejected = provider.prepare(*state.snapshot(), failing, 128);
  REQUIRE_TRUE(!rejected, "failed verification consumed or changed a graph identity");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unavailable,
             "failed verification consumed or changed a graph identity");
  REQUIRE_TRUE(!provider.assigned_instance_id(failing.id),
               "failed verification consumed or changed a graph identity");
  REQUIRE_EQ(provider.assigned_instance_id(first.id), first_engine_id,
             "failed verification consumed or changed a graph identity");

  provider.set_scan_receipt(second.id, scanned);
  auto second_runtime = take(provider.prepare(*state.snapshot(), second, 128));
  REQUIRE_TRUE(second_runtime.processor,
               "different full plugin IDs sharing low bits collided or shared runtime ownership");
  REQUIRE_EQ(provider.assigned_instance_id(second.id), first_engine_id + 1,
             "different full plugin IDs sharing low bits collided or shared runtime ownership");
  REQUIRE_NE(first_runtime.processor, second_runtime.processor,
             "different full plugin IDs sharing low bits collided or shared runtime ownership");
  // Every preparation is a new instance (ADR-0050): a changed saved state
  // runs beside the old one under its own, newest graph identity.
  auto changed = saved(scanned, 0.25F);
  changed.id = first.id;
  auto changed_runtime = take(provider.prepare(*state.snapshot(), changed, 128));
  REQUIRE_TRUE(changed_runtime.processor,
               "re-preparing changed saved settings reused or disturbed a graph identity");
  REQUIRE_NE(changed_runtime.processor, first_runtime.processor,
             "re-preparing changed saved settings reused or disturbed a graph identity");
  REQUIRE_EQ(provider.assigned_instance_id(first.id), first_engine_id + 2,
             "re-preparing changed saved settings reused or disturbed a graph identity");
  REQUIRE_EQ(provider.assigned_instance_id(second.id), first_engine_id + 1,
             "re-preparing changed saved settings reused or disturbed a graph identity");

  provider.set_scan_receipt(failing.id, scanned);
  auto third_runtime = take(provider.prepare(*state.snapshot(), failing, 128));
  REQUIRE_TRUE(third_runtime.processor,
               "third saved plugin ID did not receive an independent high-half graph ID");
  REQUIRE_EQ(provider.assigned_instance_id(failing.id), first_engine_id + 3,
             "third saved plugin ID did not receive an independent high-half graph ID");
  REQUIRE_NE(third_runtime.processor, first_runtime.processor,
             "third saved plugin ID did not receive an independent high-half graph ID");
  REQUIRE_NE(third_runtime.processor, second_runtime.processor,
             "third saved plugin ID did not receive an independent high-half graph ID");
  // No per-session instance budget remains.
  provider.set_scan_receipt(fourth.id, scanned);
  auto fourth_runtime = take(provider.prepare(*state.snapshot(), fourth, 128));
  REQUIRE_TRUE(fourth_runtime.processor,
               "fourth effect was refused or disturbed prior assignments");
  REQUIRE_EQ(provider.assigned_instance_id(fourth.id), first_engine_id + 4,
             "fourth effect was refused or disturbed prior assignments");
  REQUIRE_EQ(provider.assigned_instance_id(first.id), first_engine_id + 2,
             "fourth effect was refused or disturbed prior assignments");
  REQUIRE_EQ(provider.assigned_instance_id(second.id), first_engine_id + 1,
             "fourth effect was refused or disturbed prior assignments");

  std::array<engine::StereoFrame, 64> first_audio{}, second_audio{}, third_audio{}, fourth_audio{};
  first_audio.fill({1.0F, -1.0F});
  second_audio.fill({1.0F, -1.0F});
  third_audio.fill({1.0F, -1.0F});
  fourth_audio.fill({1.0F, -1.0F});
  counted_allocations = 0;
  counting_allocations = true;
  first_runtime.processor->process(first_audio, {0, 1});
  second_runtime.processor->process(second_audio, {0, 1});
  third_runtime.processor->process(third_audio, {0, 1});
  fourth_runtime.processor->process(fourth_audio, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "four independently prepared effects allocated or failed in the callback");
  REQUIRE_TRUE(!first_runtime.process_failed(),
               "four independently prepared effects allocated or failed in the callback");
  REQUIRE_TRUE(!second_runtime.process_failed(),
               "four independently prepared effects allocated or failed in the callback");
  REQUIRE_TRUE(!third_runtime.process_failed(),
               "four independently prepared effects allocated or failed in the callback");
  REQUIRE_TRUE(!fourth_runtime.process_failed(),
               "four independently prepared effects allocated or failed in the callback");
}

void two_verified_effects_render_separate_tracks(const plugins::ScanResult& scanned,
                                                 std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto first_track = id<project::TrackId>(2);
  const auto second_track = id<project::TrackId>(3);
  auto first = saved(scanned, 0.5F);
  auto second = saved(scanned, 0.25F);
  first.id = id<project::PluginInstanceId>(10, 1);
  second.id = id<project::PluginInstanceId>(10, 2);
  (void)take(state.apply({project::AddTrack{first_track}, project::AddTrack{second_track},
                          project::InsertVst3Plugin{first_track, 0, first},
                          project::InsertVst3Plugin{second_track, 0, second}}));
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(first.id, scanned);
  provider.set_scan_receipt(second.id, scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(),
             "two verified saved effects did not receive independent live graph identities");
  REQUIRE_NE(provider.assigned_instance_id(first.id), provider.assigned_instance_id(second.id),
             "two verified saved effects did not receive independent live graph identities");
  std::array<engine::StereoFrame, 128> first_signal{}, second_signal{}, output{};
  first_signal[0] = {1.0F, -1.0F};
  second_signal[0] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  source(inputs, control.source_layout(), first_track, std::span(first_signal).first(block_frames));
  source(inputs, control.source_layout(), second_track,
         std::span(second_signal).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "two verified effects did not process independently without callback allocation");
  REQUIRE_EQ(counted_allocations, 0,
             "two verified effects did not process independently without callback allocation");
  REQUIRE_TRUE(!control.processor_fault(first_track),
               "two verified effects did not process independently without callback allocation");
  REQUIRE_TRUE(!control.processor_fault(second_track),
               "two verified effects did not process independently without callback allocation");
  near(output[0], 0.0F);
  near(output[7], 0.75F); // Independent seven-frame effects align.
}

void two_verified_effects_reopen_and_render_in_serial_order(const plugins::ScanResult& scanned,
                                                            std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2);
  const auto dry = id<project::TrackId>(3);
  auto first = saved(scanned, 0.5F);
  auto second = saved(scanned, 0.25F);
  first.id = id<project::PluginInstanceId>(10, 1);
  second.id = id<project::PluginInstanceId>(10, 2);
  first.missing = false;
  second.missing = false;
  (void)take(state.apply({project::AddTrack{wet}, project::AddTrack{dry},
                          project::InsertVst3Plugin{wet, 0, first},
                          project::InsertVst3Plugin{wet, 1, second}}));
  TemporaryProject folder;
  {
    auto writer = take(persistence::ProjectStore::open_writer(folder.path, true));
    REQUIRE_EQ(take(writer.save(*state.snapshot(), persistence::SaveKind::explicit_save)).status,
               persistence::SaveStatus::committed, "two-effect project save did not commit");
  }
  auto reader = take(persistence::ProjectStore::open_reader(folder.path));
  auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.tracks[0].effects, state.snapshot()->tracks[0].effects,
             "save/reopen lost serial effect order, slot IDs or opaque state");
  REQUIRE_NE(loaded.snapshot.tracks[0].effects[0].id, loaded.snapshot.tracks[0].effects[1].id,
             "save/reopen lost serial effect order, slot IDs or opaque state");
  auto reopened = take(project::ProjectModel::open(std::move(loaded.snapshot), gate));
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(first.id, scanned);
  provider.set_scan_receipt(second.id, scanned);
  application::RoutedPlaybackControl control(reopened, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(),
             "reopened serial slots did not bind as distinct verified processors");
  REQUIRE_NE(provider.assigned_instance_id(first.id), provider.assigned_instance_id(second.id),
             "reopened serial slots did not bind as distinct verified processors");
  const auto first_slot = reopened.snapshot()->tracks[0].effects[0].id;
  const auto second_slot = reopened.snapshot()->tracks[0].effects[1].id;
  std::array<engine::StereoFrame, 128> wet_signal{}, dry_signal{}, output{};
  wet_signal[0] = {1.0F, -1.0F};
  dry_signal[0] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  source(inputs, control.source_layout(), wet, std::span(wet_signal).first(block_frames));
  source(inputs, control.source_layout(), dry, std::span(dry_signal).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "reopened serial VST3 path allocated, failed or lost verification");
  REQUIRE_EQ(counted_allocations, 0,
             "reopened serial VST3 path allocated, failed or lost verification");
  REQUIRE_TRUE(!control.processor_fault(),
               "reopened serial VST3 path allocated, failed or lost verification");
  const auto first_binding = control.active_vst3_effect_manual_events(
      wet, first_slot, first.id, reopened.snapshot()->revision);
  const auto second_binding = control.active_vst3_effect_manual_events(
      wet, second_slot, second.id, reopened.snapshot()->revision);
  REQUIRE_TRUE(first_binding,
               "serial effect slots aliased or accepted the other saved plugin identity");
  REQUIRE_TRUE(second_binding,
               "serial effect slots aliased or accepted the other saved plugin identity");
  REQUIRE_NE(first_binding->slot, second_binding->slot,
             "serial effect slots aliased or accepted the other saved plugin identity");
  REQUIRE_NE(first_binding->identity.instance_id, second_binding->identity.instance_id,
             "serial effect slots aliased or accepted the other saved plugin identity");
  REQUIRE_TRUE(!control.active_vst3_effect_manual_events(wet, first_slot, second.id,
                                                         reopened.snapshot()->revision),
               "serial effect slots aliased or accepted the other saved plugin identity");
  near(output[0], 0.0F);
  near(output[13], 0.0F);
  near(output[14], 1.125F); // Two serial gains and compensated dry path align.
}

void three_verified_effects_keep_order_and_history(const plugins::ScanResult& scanned,
                                                   std::size_t block_frames) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(22);
  const auto dry = id<project::TrackId>(23);
  const auto returns = id<project::TrackId>(24);
  auto first = saved(scanned, 0.5F);
  auto second = saved(scanned, 0.25F);
  auto third = saved(scanned, 0.75F);
  first.id = id<project::PluginInstanceId>(30);
  second.id = id<project::PluginInstanceId>(31);
  third.id = id<project::PluginInstanceId>(32);
  (void)take(state.apply({project::AddTrack{wet}, project::AddTrack{dry},
                          project::AddTrack{returns, project::TrackKind::return_bus},
                          project::InsertVst3Plugin{wet, 0, first},
                          project::InsertVst3Plugin{wet, 1, second},
                          project::InsertVst3Plugin{wet, 2, third}}));
  TemporaryProject folder;
  {
    auto writer = take(persistence::ProjectStore::open_writer(folder.path, true));
    REQUIRE_EQ(take(writer.save(*state.snapshot(), persistence::SaveKind::explicit_save)).status,
               persistence::SaveStatus::committed, "three-stage project did not save");
  }
  auto reader = take(persistence::ProjectStore::open_reader(folder.path));
  auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.tracks.front().effects, state.snapshot()->tracks.front().effects,
             "three saved processor identities, states or order changed on reopen");
  REQUIRE_EQ(loaded.snapshot.tracks.front().effects.size(), 3,
             "three saved processor identities, states or order changed on reopen");
  auto reopened = take(project::ProjectModel::open(std::move(loaded.snapshot), gate));
  plugins::Vst3TrackInsertProvider provider;
  for (const auto plugin : {first.id, second.id, third.id})
    provider.set_scan_receipt(plugin, scanned);
  application::RoutedPlaybackControl control(reopened, {{1, 64}, 128, 8'192, 8'192}, &provider);
  REQUIRE_OK(control.submit_current(),
             "three-stage saved path did not bind separate runtime owners");
  REQUIRE_NE(provider.assigned_instance_id(first.id), provider.assigned_instance_id(second.id),
             "three-stage saved path did not bind separate runtime owners");
  REQUIRE_NE(provider.assigned_instance_id(second.id), provider.assigned_instance_id(third.id),
             "three-stage saved path did not bind separate runtime owners");
  std::array<engine::StereoFrame, 128> wet_signal{}, dry_signal{}, output{};
  wet_signal[0] = {1.0F, -1.0F};
  wet_signal[block_frames - 1] = {1.0F, -1.0F};
  dry_signal[0] = {1.0F, -1.0F};
  std::array<std::span<const engine::StereoFrame>, 4> inputs{};
  source(inputs, control.source_layout(), wet, std::span(wet_signal).first(block_frames));
  source(inputs, control.source_layout(), dry, std::span(dry_signal).first(block_frames));
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "three-stage callback allocated, failed or omitted a runtime");
  REQUIRE_EQ(counted_allocations, 0, "three-stage callback allocated, failed or omitted a runtime");
  REQUIRE_TRUE(!control.processor_fault(),
               "three-stage callback allocated, failed or omitted a runtime");
  near(output[20], 0.0F);
  near(output[21], 1.09375F); // Three seven-frame gains and PDC-aligned dry impulse.

  const auto changed =
      take(reopened.apply({project::AddSendRoute{wet, {id<project::SendId>(25), returns, 0.5}}}));
  REQUIRE_OK(control.begin_edit(changed, "add return send"),
             "three-stage routing revision did not publish live");
  wet_signal.fill({});
  dry_signal.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched =
      control.render_block(inputs, std::span(output).first(block_frames), {block_frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "three-stage live revision lost runtime history or allocated");
  REQUIRE_EQ(counted_allocations, 0, "three-stage live revision lost runtime history or allocated");
  REQUIRE_TRUE(!control.processor_fault(),
               "three-stage live revision lost runtime history or allocated");
  near(output[20],
       0.140625F); // The final wet impulse drains through the retained stages and new send.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "three-stage graph revision did not become audible");
}

void automated_effect_renders_and_rejects_live_replacement(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2);
  const auto returns = id<project::TrackId>(3);
  auto effect = saved(scanned, 1.0F);
  effect.component_state.clear();
  effect.parameters = {{101, 0.25}};
  const project::AutomationLane lane{
      id<project::AutomationLaneId>(20),
      project::Vst3ParameterTarget{effect.id, 101},
      {project::AutomationRepresentation::vst3_normalized, "gain", "", 0.0, 1.0, 0.25, false, 0},
      project::AutomationTimebase::elapsed,
      {{id<project::AutomationPointId>(21), 0, 0.25, project::AutomationShape::hold, {}},
       {id<project::AutomationPointId>(22), 1, 0.5, project::AutomationShape::hold, {}}},
      1};
  (void)take(state.apply(
      {project::AddTrack{wet}, project::AddTrack{returns, project::TrackKind::return_bus},
       project::InsertVst3Plugin{wet, 0, effect}, project::AddAutomationLane{wet, lane}}));
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(effect.id, scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "automated VST3 route did not prepare");
  std::array<engine::StereoFrame, 64> wet_signal{}, output{};
  wet_signal[0] = {0.25F, -0.25F};
  wet_signal[1] = {0.125F, -0.125F};
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  source(inputs, control.source_layout(), wet, wet_signal);
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  require(rendered == engine::RoutedRenderStatus::rendered && counted_allocations == 0 &&
              !control.processor_fault(),
          "automated VST3 route failed or allocated in callback: " +
              std::to_string(static_cast<int>(rendered)) +
              ", allocations=" + std::to_string(counted_allocations) +
              ", fault=" + std::to_string(control.processor_fault().has_value()));
  near(output[0], 0.25F);
  near(output[1], 0.25F);
  control.observe();
  const auto rerouted =
      take(state.apply({project::AddSendRoute{wet, {id<project::SendId>(4), returns, 0.5}}}));
  REQUIRE_OK(control.begin_edit(rerouted, "add send"),
             "routing-only edit failed to reuse automated VST3 processor");
  REQUIRE_EQ(control.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::rendered,
             "routing-only edit failed to reuse automated VST3 processor");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, rerouted.snapshot->revision,
             "automated VST3 route edit did not activate");
  auto changed_lane = lane;
  changed_lane.settings_generation = 2;
  changed_lane.points.back().value = 0.25;
  const auto changed = take(state.apply({project::ReplaceAutomationLane{lane.id, changed_lane}}));
  // Until a VST3 runtime can take new lanes live, the edit is refused at
  // once, with no hold, and the old graph plays on.
  require(!control.begin_edit(changed, "VST3 automation") &&
              control.render_block(inputs, output, {128, 1}) ==
                  engine::RoutedRenderStatus::rendered &&
              control.status().model_revision == changed.snapshot->revision &&
              control.status().active_graph_revision < changed.snapshot->revision &&
              control.status().application == application::PlaybackApplication::failed,
          "active VST3 automation edit was silently treated as audible");
}

void benchmark(const plugins::ScanResult& scanned, std::size_t block_frames, std::size_t stages) {
  Gate gate;
  auto state = model(gate);
  const auto wet = id<project::TrackId>(2);
  const auto dry = id<project::TrackId>(3);
  auto effect = saved(scanned, 0.5F);
  effect.id = id<project::PluginInstanceId>(10, 1);
  (void)take(state.apply(
      {project::AddTrack{wet}, project::AddTrack{dry}, project::InsertVst3Plugin{wet, 0, effect}}));
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(effect.id, scanned);
  if (stages >= 2) {
    auto second = saved(scanned, 0.25F);
    second.id = id<project::PluginInstanceId>(10, 2);
    (void)take(state.apply({project::InsertVst3Plugin{wet, 1, second}}));
    provider.set_scan_receipt(second.id, scanned);
  }
  if (stages >= 3) {
    auto third = saved(scanned, 0.75F);
    third.id = id<project::PluginInstanceId>(10, 3);
    (void)take(state.apply({project::InsertVst3Plugin{wet, 2, third}}));
    provider.set_scan_receipt(third.id, scanned);
  }
  application::RoutedPlaybackControl control(state, {{1, 64}, 128, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "benchmark route could not prepare");
  std::array<engine::StereoFrame, 128> wet_signal{}, dry_signal{}, output{};
  wet_signal.fill({0.25F, -0.25F});
  dry_signal.fill({0.25F, -0.25F});
  std::array<std::span<const engine::StereoFrame>, 3> inputs{};
  source(inputs, control.source_layout(), wet, std::span(wet_signal).first(block_frames));
  source(inputs, control.source_layout(), dry, std::span(dry_signal).first(block_frames));
  auto frames = std::span(output).first(block_frames);
  std::uint64_t position = 0;
  for (unsigned warmup = 0; warmup < 1'000; ++warmup) {
    REQUIRE_EQ(control.render_block(inputs, frames, {position, 1}),
               engine::RoutedRenderStatus::rendered, "benchmark warmup did not render");
    position += block_frames;
  }
  constexpr unsigned iterations = 20'000;
  counted_allocations = 0;
  counting_allocations = true;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    if (control.render_block(inputs, frames, {position, 1}) != engine::RoutedRenderStatus::rendered)
      std::abort();
    position += block_frames;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "benchmark VST3 callback allocated");
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / iterations;
  std::fprintf(stdout,
               "Routed VST3 fixture serial-%zu %zu frames: %lld ns/block, %zu allocations\n",
               stages, block_frames, static_cast<long long>(nanoseconds), counted_allocations);
}

void changed_state_prepares_a_fresh_instance(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = model(gate);
  const auto track = id<project::TrackId>(901);
  const auto effect = saved(scanned, 0.5F);
  (void)take(state.apply({project::AddTrack{track}, project::InsertVst3Plugin{track, 0, effect}}));
  plugins::Vst3TrackInsertProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(effect.id, scanned);
  auto initial = take(provider.prepare(*state.snapshot(), effect, 128));
  REQUIRE_EQ(initial.finite_tail_frames, std::optional<std::uint32_t>{0},
             "initial plugin lost its tail declaration or singleton fixture qualification");
  REQUIRE_TRUE(initial.fixed_latency_qualified,
               "initial plugin lost its tail declaration or singleton fixture qualification");
  auto changed = saved(scanned, 0.25F);
  (void)take(state.apply({project::SetVst3ComponentState{effect.id, changed.component_state}}));
  const auto& candidate = *state.snapshot();
  changed =
      std::get<project::Vst3PluginInstance>(candidate.tracks.front().effects.front().processor);
  auto replacement = take(provider.prepare(candidate, changed, 128));
  initial.manual_events->activate(1);
  replacement.manual_events->activate(2);
  REQUIRE_EQ(replacement.finite_tail_frames, std::optional<std::uint32_t>{0},
             "changed state reused the old instance identity");
  REQUIRE_TRUE(replacement.fixed_latency_qualified,
               "changed state reused the old instance identity");
  REQUIRE_NE(replacement.manual_events->active_graph().instance_id,
             initial.manual_events->active_graph().instance_id,
             "changed state reused the old instance identity");
  REQUIRE_EQ(provider.assigned_instance_id(effect.id),
             replacement.manual_events->active_graph().instance_id,
             "changed state reused the old instance identity");
  std::array<engine::StereoFrame, 16> old_audio{}, new_audio{};
  old_audio[0] = {1.0F, -1.0F};
  new_audio[0] = old_audio[0];
  initial.processor->process(old_audio, {0, 1});
  replacement.processor->process(new_audio, {0, 1});
  near(old_audio[7], 0.5F);
  near(new_audio[7], 0.25F);
}

void fixed_tail_evidence_is_exact_and_state_bound(const plugins::ScanResult& scanned) {
  auto effect = saved(scanned, 0.5F);
  effect.missing = false;
  plugins::Vst3FixedTailQualification evidence{.binary_sha256 = scanned.binary.sha256,
                                               .binary_byte_size = scanned.binary.byte_size,
                                               .class_id = effect.class_id,
                                               .vendor = effect.vendor,
                                               .version = effect.version,
                                               .sample_rate = oscillate::test::sample_rate_48k,
                                               .declared_tail_frames = 0,
                                               .component_state = effect.component_state,
                                               .parameters = effect.parameters,
                                               .tested_64_frames = true,
                                               .tested_128_frames = true};
  for (const auto block_frames : {64U, 128U}) {
    Gate gate;
    auto state = model(gate);
    const auto track = id<project::TrackId>(1200 + block_frames);
    (void)take(
        state.apply({project::AddTrack{track}, project::InsertVst3Plugin{track, 0, effect}}));
    plugins::Vst3TrackInsertProvider provider;
    provider.set_scan_receipt(effect.id, scanned);
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "tail declaration alone qualified a VST3 binary");
    provider.set_fixed_tail_qualification(effect.id, evidence);
    auto prepared = take(provider.prepare(*state.snapshot(), effect, block_frames));
    REQUIRE_TRUE(prepared.fixed_tail_qualified,
                 "exact binary/state tail evidence did not qualify its declared bound");
    REQUIRE_EQ(prepared.finite_tail_frames, 0,
               "exact binary/state tail evidence did not qualify its declared bound");
    REQUIRE_EQ(prepared.latency_frames, 7,
               "exact binary/state tail evidence did not qualify its declared bound");
    require(!prepared.fixed_latency_qualified, "tail evidence alone implied fixed-latency proof");
    provider.set_manual_qualification(effect.id, true, {});
    auto unscoped = take(provider.prepare(*state.snapshot(), effect, block_frames));
    REQUIRE_TRUE(unscoped.fixed_latency_qualified,
                 "unscoped manual-control admission retained exact-state tail proof");
    REQUIRE_TRUE(!unscoped.fixed_tail_qualified,
                 "unscoped manual-control admission retained exact-state tail proof");
    provider.set_size_scoped_manual_qualification(effect.id, true, {{64, {}}, {128, {}}});
    auto dual = take(provider.prepare(*state.snapshot(), effect, block_frames));
    dual.manual_events->activate(1);
    const auto identity = dual.manual_events->active_graph();
    REQUIRE_TRUE(dual.fixed_latency_qualified,
                 "exact dual receipt admitted a live control or lost fixed tail");
    REQUIRE_TRUE(dual.fixed_tail_qualified,
                 "exact dual receipt admitted a live control or lost fixed tail");
    REQUIRE_EQ(dual.finite_tail_frames, 0,
               "exact dual receipt admitted a live control or lost fixed tail");
    REQUIRE_EQ(dual.manual_events->admission(identity, 2, {101, 0, 0.75}),
               plugins::Vst3ManualEventStatus::unavailable,
               "exact dual receipt admitted a live control or lost fixed tail");
    provider.set_size_scoped_manual_qualification(effect.id, true, {{64, {}}});
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "one callback-size receipt qualified a dual invariant");
    provider.set_size_scoped_manual_qualification(effect.id, true, {{64, {}}, {128, {}}});

    // The synthetic delayed gain is a memoryless effect after its seven-frame
    // latency. The impulse, zero-input drain and a fresh restored instance
    // agree at both admitted block sizes.
    std::array<engine::StereoFrame, 128> frames{};
    frames[0] = {1.0F, -1.0F};
    prepared.processor->process(std::span(frames).first(block_frames), {0, 1});
    near(frames[7], 0.5F);
    require(std::ranges::all_of(
                std::span(frames).first(block_frames).subspan(8),
                [](const auto& frame) { return frame.left == 0.0F && frame.right == 0.0F; }),
            "qualified fixture emitted audio beyond latency plus declared tail");
    frames.fill({});
    prepared.processor->process(std::span(frames).first(block_frames), {block_frames, 1});
    require(std::ranges::all_of(
                std::span(frames).first(block_frames),
                [](const auto& frame) { return frame.left == 0.0F && frame.right == 0.0F; }),
            "qualified fixture emitted a late tail on silence");
    TemporaryProject folder;
    {
      auto writer = take(persistence::ProjectStore::open_writer(folder.path, true));
      REQUIRE_EQ(take(writer.save(*state.snapshot(), persistence::SaveKind::explicit_save)).status,
                 persistence::SaveStatus::committed, "fixed-tail state save did not commit");
    }
    auto reader = take(persistence::ProjectStore::open_reader(folder.path));
    auto loaded = take(reader.load_current());
    const auto persisted = std::get<project::Vst3PluginInstance>(
        loaded.snapshot.tracks.front().effects.front().processor);
    REQUIRE_EQ(persisted, effect, "fixed-tail plugin identity or state changed on reopen");
    auto reopened_model = take(project::ProjectModel::open(std::move(loaded.snapshot), gate));
    plugins::Vst3TrackInsertProvider reopened;
    reopened.set_scan_receipt(effect.id, scanned);
    reopened.set_fixed_tail_qualification(effect.id, evidence);
    auto restored = take(reopened.prepare(*reopened_model.snapshot(), persisted, block_frames));
    require(restored.fixed_tail_qualified,
            "exact state restore lost its session-supplied tail evidence");
    auto wrong_controller = evidence;
    wrong_controller.controller_state = std::vector<std::uint8_t>{};
    provider.set_fixed_tail_qualification(effect.id, wrong_controller);
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "controller-only state mismatch retained effect tail qualification");
    provider.set_fixed_tail_qualification(effect.id, evidence);
    frames.fill({});
    frames[0] = {1.0F, -1.0F};
    restored.processor->process(std::span(frames).first(block_frames), {0, 1});
    near(frames[7], 0.5F);

    auto changed = effect;
    changed.component_state[0] ^= 1;
    require(!take(provider.prepare(*state.snapshot(), changed, block_frames)).fixed_tail_qualified,
            "changed saved component state retained tail qualification");
    auto updated_evidence = evidence;
    updated_evidence.component_state = changed.component_state;
    provider.set_fixed_tail_qualification(effect.id, updated_evidence);
    auto updated = take(provider.prepare(*state.snapshot(), changed, block_frames));
    require(updated.fixed_tail_qualified,
            "new exact-state evidence did not qualify the changed saved state");
    frames.fill({});
    frames[0] = {1.0F, -1.0F};
    updated.processor->process(std::span(frames).first(block_frames), {0, 1});
    REQUIRE_TRUE(std::isfinite(frames[7].left),
                 "state variant did not produce bounded finite output");
    REQUIRE_GT(frames[7].left, 0.0F, "state variant did not produce bounded finite output");
    REQUIRE_LT(frames[7].left, 1.0F, "state variant did not produce bounded finite output");
    provider.set_fixed_tail_qualification(effect.id, evidence);
    auto wrong = evidence;
    wrong.binary_sha256[0] = wrong.binary_sha256[0] == 'a' ? 'b' : 'a';
    provider.set_fixed_tail_qualification(effect.id, wrong);
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "changed binary identity retained tail qualification");
    wrong = evidence;
    wrong.declared_tail_frames = 1;
    provider.set_fixed_tail_qualification(effect.id, wrong);
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "unmatched declared tail retained qualification");
    wrong = evidence;
    wrong.tested_128_frames = false;
    provider.set_fixed_tail_qualification(effect.id, wrong);
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "incomplete block-size evidence retained tail qualification");
    wrong = evidence;
    wrong.parameters.push_back({101, 0.5});
    provider.set_fixed_tail_qualification(effect.id, wrong);
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "changed saved parameter record retained tail qualification");
    provider.set_fixed_tail_qualification(effect.id, evidence);
    provider.set_manual_qualification(effect.id, false, {101});
    require(!take(provider.prepare(*state.snapshot(), effect, block_frames)).fixed_tail_qualified,
            "live manual parameter capability retained fixed-tail qualification");
  }
}

void parameter_qualification_is_per_saved_effect(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = model(gate);
  const auto first_track = id<project::TrackId>(951);
  const auto second_track = id<project::TrackId>(952);
  auto first = saved(scanned, 1.0F);
  first.id = id<project::PluginInstanceId>(953);
  first.component_state.clear();
  first.parameters = {{101, 0.25}};
  first.missing = false;
  auto second = first;
  second.id = id<project::PluginInstanceId>(954);
  (void)take(state.apply({project::AddTrack{first_track}, project::AddTrack{second_track},
                          project::InsertVst3Plugin{first_track, 0, first},
                          project::InsertVst3Plugin{second_track, 0, second}}));
  plugins::Vst3TrackInsertProvider provider;
  provider.set_scan_receipt(first.id, scanned);
  provider.set_scan_receipt(second.id, scanned);
  provider.set_manual_qualification(first.id, false, {101, 101, 999});
  auto qualified = take(provider.prepare(*state.snapshot(), first, 128));
  auto other = take(provider.prepare(*state.snapshot(), second, 128));
  REQUIRE_TRUE(qualified.fixed_latency_qualified,
               "effect qualification leaked to a second saved instance");
  REQUIRE_TRUE(!other.fixed_latency_qualified,
               "effect qualification leaked to a second saved instance");
  REQUIRE_TRUE(qualified.manual_events, "effect qualification leaked to a second saved instance");
  REQUIRE_TRUE(other.manual_events, "effect qualification leaked to a second saved instance");
  qualified.manual_events->activate(1);
  other.manual_events->activate(1);
  const auto first_identity = qualified.manual_events->active_graph();
  const auto second_identity = other.manual_events->active_graph();
  REQUIRE_EQ(
      qualified.manual_events->admission({.graph_revision = 1,
                                          .instance_id = first_identity.instance_id,
                                          .compatibility_key = first_identity.compatibility_key},
                                         2, {101, 0, 0.75}),
      plugins::Vst3ManualEventStatus::accepted,
      "exact effect control was not isolated to its saved instance and verified metadata");
  REQUIRE_EQ(
      qualified.manual_events->admission({.graph_revision = 1,
                                          .instance_id = first_identity.instance_id,
                                          .compatibility_key = first_identity.compatibility_key},
                                         2, {999, 0, 0.75}),
      plugins::Vst3ManualEventStatus::unavailable,
      "exact effect control was not isolated to its saved instance and verified metadata");
  REQUIRE_EQ(
      other.manual_events->admission({.graph_revision = 1,
                                      .instance_id = second_identity.instance_id,
                                      .compatibility_key = second_identity.compatibility_key},
                                     2, {101, 0, 0.75}),
      plugins::Vst3ManualEventStatus::latency_unverified,
      "exact effect control was not isolated to its saved instance and verified metadata");

  provider.set_size_scoped_manual_qualification(first.id, true, {{64, {101, 101, 999}}});
  auto scoped = take(provider.prepare(*state.snapshot(), first, 128));
  REQUIRE_TRUE(scoped.manual_events,
               "provider failed exact process-block and loaded-control intersection");
  REQUIRE_TRUE(scoped.manual_events->qualified_for_block(101, 64),
               "provider failed exact process-block and loaded-control intersection");
  REQUIRE_TRUE(!scoped.manual_events->qualified_for_block(101, 128),
               "provider failed exact process-block and loaded-control intersection");
  REQUIRE_TRUE(!scoped.manual_events->qualified_for_block(999, 64),
               "provider failed exact process-block and loaded-control intersection");

  // Unscoped whole-binary fixture evidence may only admit a singleton. A
  // second saved effect never borrows it, even when the same binary is used.
  plugins::Vst3TrackInsertProvider global_fixture(plugins::Vst3ProcessingMode::realtime, true);
  global_fixture.set_scan_receipt(first.id, scanned);
  global_fixture.set_scan_receipt(second.id, scanned);
  REQUIRE_TRUE(!take(global_fixture.prepare(*state.snapshot(), first, 128)).fixed_latency_qualified,
               "constructor fixture admission leaked across saved effect instances");
  REQUIRE_TRUE(
      !take(global_fixture.prepare(*state.snapshot(), second, 128)).fixed_latency_qualified,
      "constructor fixture admission leaked across saved effect instances");

  global_fixture.set_manual_qualification(first.id, true, {101});
  auto explicitly_qualified = take(global_fixture.prepare(*state.snapshot(), first, 128));
  auto unrecorded = take(global_fixture.prepare(*state.snapshot(), second, 128));
  REQUIRE_TRUE(explicitly_qualified.fixed_latency_qualified,
               "unrecorded effect inherited another effect's fixed-latency evidence");
  REQUIRE_TRUE(!unrecorded.fixed_latency_qualified,
               "unrecorded effect inherited another effect's fixed-latency evidence");
  explicitly_qualified.manual_events->activate(2);
  unrecorded.manual_events->activate(2);
  const auto explicit_identity = explicitly_qualified.manual_events->active_graph();
  const auto unrecorded_identity = unrecorded.manual_events->active_graph();
  REQUIRE_EQ(explicitly_qualified.manual_events->admission(
                 {.graph_revision = 2,
                  .instance_id = explicit_identity.instance_id,
                  .compatibility_key = explicit_identity.compatibility_key},
                 3, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::accepted,
             "per-effect manual event admission did not follow exact saved evidence");
  REQUIRE_EQ(unrecorded.manual_events->admission(
                 {.graph_revision = 2,
                  .instance_id = unrecorded_identity.instance_id,
                  .compatibility_key = unrecorded_identity.compatibility_key},
                 3, {101, 0, 0.75}),
             plugins::Vst3ManualEventStatus::latency_unverified,
             "per-effect manual event admission did not follow exact saved evidence");
  global_fixture.set_manual_qualification(second.id, false);
  require(!take(global_fixture.prepare(*state.snapshot(), second, 128)).fixed_latency_qualified,
          "explicitly unqualified effect gained manual admission");
}

void provider_binds_the_saved_vst3_detector_bus(const plugins::ScanResult& sidechain_scan,
                                                const plugins::ScanResult& no_aux_scan) {
  const auto exercise = [](const plugins::ScanResult& scan, bool available) {
    Gate gate;
    auto state = model(gate);
    const auto source_track = id<project::TrackId>(1'401);
    const auto target_track = id<project::TrackId>(1'402);
    const auto effect = saved(scan, 1.0F);
    (void)take(state.apply({project::AddTrack{source_track}, project::AddTrack{target_track},
                            project::InsertVst3Plugin{target_track, 0, effect}}));
    const auto slot = state.snapshot()->tracks[1].effects.front().id;
    const auto committed = take(state.apply({project::AddSendRoute{
        source_track,
        {id<project::SendId>(1'403), target_track, 1.0, project::SendPurpose::sidechain, slot}}}));
    plugins::Vst3TrackInsertProvider provider;
    provider.set_scan_receipt(effect.id, scan);
    auto prepared = provider.prepare(*committed.snapshot, effect, 128);
    if (!available) {
      REQUIRE_TRUE(!prepared,
                   "missing auxiliary input did not fail before changing saved detector intent");
      REQUIRE_EQ(committed.snapshot->tracks[0].sends.front().target_insert, slot,
                 "missing auxiliary input did not fail before changing saved detector intent");
      return;
    }
    REQUIRE_OK(prepared, "provider failed to bind an exact saved VST3 detector send");
    auto* detector =
        dynamic_cast<engine::SidechainProcessorRuntime*>(prepared.value().processor.get());
    REQUIRE_NE(detector, nullptr, "prepared VST3 effect omitted its detector runtime");
    for (const auto frames : {64U, 128U}) {
      std::array<engine::StereoFrame, 128> audio{}, key{};
      audio.fill({0.25F, -0.5F});
      key.fill({0.125F, -0.25F});
      counted_allocations = 0;
      counting_allocations = true;
      detector->process_with_sidechain(std::span(audio).first(frames), std::span(key).first(frames),
                                       {0, 1});
      counting_allocations = false;
      REQUIRE_EQ(counted_allocations, 0,
                 "provider's activated VST3 detector returned wrong audio or allocated");
      REQUIRE_TRUE(std::ranges::all_of(std::span(audio).first(frames),
                                       [](const auto& frame) {
                                         return frame.left == 0.375F && frame.right == -0.75F;
                                       }),
                   "provider's activated VST3 detector returned wrong audio or allocated");
    }
  };
  exercise(sidechain_scan, true);
  exercise(no_aux_scan, false);
}

} // namespace

int main() {
  try {
    const auto bundle = std::filesystem::canonical(VST3_GAIN_BUNDLE);
    plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
    const auto scanned = scanner.scan(bundle);
    REQUIRE_TRUE(scanned.ok(), "gain fixture scan failed");
    REQUIRE_EQ(scanned.metadata.classes.size(), 1, "gain fixture scan failed");
    verified_effect_only_processes_own_track(scanned, 64);
    verified_effect_only_processes_own_track(scanned, 128);
    latency_change_replaces_the_plugin_live(scanned, 64);
    latency_change_replaces_the_plugin_live(scanned, 128);
    manual_effect_binding_tracks_callback_graph(scanned);
    channel_control_overlay_rebinds_routed_vst3_manual_slot(scanned, 64);
    channel_control_overlay_rebinds_routed_vst3_manual_slot(scanned, 128);
    missing_scan_and_process_failure(scanned);
    provider_assigns_fresh_high_half_ids(scanned);
    two_verified_effects_render_separate_tracks(scanned, 64);
    two_verified_effects_render_separate_tracks(scanned, 128);
    two_verified_effects_reopen_and_render_in_serial_order(scanned, 64);
    two_verified_effects_reopen_and_render_in_serial_order(scanned, 128);
    three_verified_effects_keep_order_and_history(scanned, 64);
    three_verified_effects_keep_order_and_history(scanned, 128);
    const auto connected = scanner.scan(std::filesystem::canonical(VST3_CONNECTED_BUNDLE));
    require(connected.ok(), "connected routed automation fixture did not scan");
    automated_effect_renders_and_rejects_live_replacement(connected);
    changed_state_prepares_a_fresh_instance(scanned);
    fixed_tail_evidence_is_exact_and_state_bound(scanned);
    parameter_qualification_is_per_saved_effect(connected);
    const auto sidechain = scanner.scan(std::filesystem::canonical(VST3_SIDECHAIN_BUNDLE));
    require(sidechain.ok(), "routed VST3 sidechain fixture did not scan");
    provider_binds_the_saved_vst3_detector_bus(sidechain, connected);
    if (std::getenv("OSCILLATE_BENCHMARK_ROUTED_VST3")) {
      for (const auto stages : {1U, 2U, 3U}) {
        benchmark(scanned, 64, stages);
        benchmark(scanned, 128, stages);
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "routed VST3 insert: %s\n", error.what());
    return 1;
  }
}
