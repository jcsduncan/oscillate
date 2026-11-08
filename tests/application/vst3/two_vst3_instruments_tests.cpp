#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/manual_effect_edit.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace fs = std::filesystem;
namespace plugins = oscillate::plugins;
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

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-two-vst3") {}
};

project::Vst3PluginInstance saved(const plugins::ScanResult& scan, project::PluginInstanceId plugin,
                                  float level) {
  const auto& klass = scan.metadata.classes.front();
  project::Vst3PluginInstance result{.id = plugin,
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = "/advisory/TimedInstrument.vst3",
                                     .binary_sha256 = scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  result.component_state.resize(sizeof(level));
  std::memcpy(result.component_state.data(), &level, sizeof(level));
  return result;
}

project::MidiClip clip(std::uint64_t clip_id, std::uint64_t note_id, std::int64_t start_tick) {
  return {id<project::ClipId>(clip_id),
          project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
          {timeline::ticks_per_quarter},
          false,
          {{id<project::NoteId>(note_id), {start_tick}, {100'240'000}, 60, 127}},
          1};
}

project::ProjectModel make_model(Gate& gate, const project::Vst3PluginInstance& first,
                                 const project::Vst3PluginInstance& second) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  auto state = take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000,
                                                  std::move(tempo), std::move(meter), gate));
  (void)take(
      state.apply({project::AddTrack{id<project::TrackId>(2), project::TrackKind::instrument},
                   project::AddTrack{id<project::TrackId>(3), project::TrackKind::instrument},
                   project::AddTrack{id<project::TrackId>(4), project::TrackKind::return_bus},
                   project::AddMidiClip{id<project::TrackId>(2), clip(12, 22, 200'000)},
                   project::AddMidiClip{id<project::TrackId>(3), clip(13, 23, 400'000)},
                   project::InsertVst3Plugin{id<project::TrackId>(2), 0, first},
                   project::InsertVst3Plugin{id<project::TrackId>(3), 0, second}}));
  return state;
}

class Adoption final : public app::Vst3ManualLiveAdoption {
public:
  Adoption(project::ProjectModel& model, app::RoutedPlaybackControl& control)
      : model_(model), control_(control) {}
  foundation::Result<void> preflight(project::PluginInstanceId plugin,
                                     plugins::Vst3ActiveGraphIdentity identity) override {
    control_.observe();
    return control_.preflight_vst3_manual_edit(plugin, identity);
  }
  foundation::Result<void> begin(const project::Snapshot& saved, project::PluginInstanceId plugin,
                                 plugins::Vst3ActiveGraphIdentity identity) override {
    auto adopted = model_.adopt_authoritative_snapshot(saved);
    if (!adopted)
      return foundation::Result<void>::failure(adopted.error());
    return control_.begin_vst3_manual_edit(adopted.value(), "instrument parameter", plugin,
                                           identity);
  }
  foundation::Result<void> settle(plugins::Vst3ManualEventReceipt receipt) override {
    return control_.settle_vst3_manual_edit(receipt);
  }

private:
  project::ProjectModel& model_;
  app::RoutedPlaybackControl& control_;
};

class CapturingProvider final : public app::TrackInsertProvider {
public:
  explicit CapturingProvider(bool qualified = false)
      : provider_(plugins::Vst3ProcessingMode::realtime, qualified) {}
  void set_scan_receipt(project::PluginInstanceId plugin, plugins::ScanResult scan) {
    provider_.set_scan_receipt(plugin, std::move(scan));
  }
  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t max_frames) override {
    auto result = provider_.prepare(snapshot, saved, max_frames);
    if (result)
      owners_.insert_or_assign(saved.id, result.value().processor);
    return result;
  }
  [[nodiscard]] std::shared_ptr<engine::ProcessorRuntime>
  owner(project::PluginInstanceId plugin) const {
    const auto found = owners_.find(plugin);
    return found == owners_.end() ? nullptr : found->second;
  }
  [[nodiscard]] std::shared_ptr<plugins::Vst3ManualEventSlot>
  manual_events(project::TrackId track, project::PluginInstanceId plugin) const noexcept {
    return provider_.manual_events(track, plugin);
  }
  [[nodiscard]] bool stop_after_join() noexcept { return provider_.stop_after_join(); }

private:
  plugins::Vst3InstrumentTrackProvider provider_;
  std::map<project::PluginInstanceId, std::shared_ptr<engine::ProcessorRuntime>> owners_;
};

// Tests the source bank's own admission after two exact graph receipts have
// been accepted, independently of any plugin host.
class SourceAdmissionFixture final : public app::TrackInsertProvider {
public:
  class SilentSource final : public engine::ProcessorRuntime {
  public:
    void process(std::span<engine::StereoFrame>, engine::BlockContext) noexcept override {}
  };
  foundation::Result<app::PreparedTrackInsert>
  prepare(const project::Snapshot&, const project::Vst3PluginInstance&, std::uint32_t) override {
    app::PreparedTrackInsert prepared{};
    prepared.processor = std::make_shared<SilentSource>();
    prepared.process_failed = []() noexcept { return false; };
    prepared.instrument_source = true;
    return foundation::Result<app::PreparedTrackInsert>::success(std::move(prepared));
  }
};

void near(engine::StereoFrame frame, float expected) {
  require(std::fabs(frame.left - expected) < 0.00001F &&
              std::fabs(frame.right + expected) < 0.00001F,
          "two-source routed output differs from independent fixture sum: expected " +
              std::to_string(expected) + ", got " + std::to_string(frame.left) + "/" +
              std::to_string(frame.right));
}

void two_source_live_route(const plugins::ScanResult& scanned, std::size_t frames) {
  Gate gate;
  const auto first_id = id<project::PluginInstanceId>(5);
  const auto second_id = id<project::PluginInstanceId>(6);
  auto state = make_model(gate, saved(scanned, first_id, 0.5F), saved(scanned, second_id, 0.25F));
  Directory directory;
  auto writer = take(oscillate::persistence::ProjectStore::open_writer(directory.path(), true));
  REQUIRE_EQ(
      take(writer.save(*state.snapshot(), oscillate::persistence::SaveKind::explicit_save)).status,
      oscillate::persistence::SaveStatus::committed, "two-source project did not save");
  auto reader = take(oscillate::persistence::ProjectStore::open_reader(directory.path()));
  auto reopened = take(reader.load_current());
  REQUIRE_EQ(reopened.snapshot.tracks[0].instrument_source,
             state.snapshot()->tracks[0].instrument_source,
             "two saved instrument identities or MIDI schedules did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks[1].instrument_source,
             state.snapshot()->tracks[1].instrument_source,
             "two saved instrument identities or MIDI schedules did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks[0].midi_clips, state.snapshot()->tracks[0].midi_clips,
             "two saved instrument identities or MIDI schedules did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks[1].midi_clips, state.snapshot()->tracks[1].midi_clips,
             "two saved instrument identities or MIDI schedules did not reopen");
  auto model = take(project::ProjectModel::open(std::move(reopened.snapshot), gate));
  CapturingProvider provider;
  provider.set_scan_receipt(first_id, scanned);
  provider.set_scan_receipt(second_id, scanned);
  app::RoutedPlaybackControl control(model, {{1, 64}, frames, 1 << 20, 1 << 20}, &provider);
  REQUIRE_TRUE(control.submit_current(),
               "two live VST3 sources were not bound to independent route nodes");
  REQUIRE_TRUE(control.has_bound_instrument_track(id<project::TrackId>(2)),
               "two live VST3 sources were not bound to independent route nodes");
  REQUIRE_TRUE(control.has_bound_instrument_track(id<project::TrackId>(3)),
               "two live VST3 sources were not bound to independent route nodes");
  REQUIRE_TRUE(!control.has_bound_instrument_track(id<project::TrackId>(4)),
               "two live VST3 sources were not bound to independent route nodes");
  REQUIRE_TRUE(provider.owner(first_id),
               "two live VST3 sources were not bound to independent route nodes");
  REQUIRE_TRUE(provider.owner(second_id),
               "two live VST3 sources were not bound to independent route nodes");
  REQUIRE_NE(provider.owner(first_id), provider.owner(second_id),
             "two live VST3 sources were not bound to independent route nodes");
  auto source = app::RoutedAudioSource::prepare(directory.path(), *model.snapshot(), control, 128);
  REQUIRE_OK(source, "exactly bound two-source MIDI project did not prepare audio bank");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  counted_allocations = 0;
  counting_allocations = true;
  const auto first = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, engine::RoutedRenderStatus::rendered,
             "two-source first callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "two-source first callback failed or allocated");
  REQUIRE_TRUE(!control.processor_fault(), "two-source first callback failed or allocated");
  near(output[4], 0.0F);
  near(output[5], 0.5F);
  near(output[9], 0.5F);
  near(output[10], 0.75F);
  control.observe();
  require(!control.prepared_route_tail_receipt(model.snapshot()->revision, 128),
          "two unqualified VST3 sources were admitted to real-time export");
  const auto active = control.status().active_graph_revision;
  const auto revised =
      take(model.apply({project::SetTrackLevelPan{id<project::TrackId>(2), 0.5, 0.0}}));
  // A level edit is a qualified mixer control (ADR-0029): it applies live
  // through the channel overlay, gliding over one block, with no held graph.
  REQUIRE_OK(control.begin_edit(revised, "first source level"),
             "two-source live level edit was not admitted");
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed = control.render_block(inputs, output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "two-source live level edit held, allocated or lost a processor");
  REQUIRE_EQ(counted_allocations, 0,
             "two-source live level edit held, allocated or lost a processor");
  REQUIRE_TRUE(!control.processor_fault(),
               "two-source live level edit held, allocated or lost a processor");
  REQUIRE_GT(output.front().left, output.back().left,
             "two-source level edit did not glide down within the block");
  near(output.back(), 0.5F); // First source 0.5 * track level 0.5, second source 0.25.
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "two-source graph edit was not acknowledged");
  REQUIRE_GT(control.status().active_graph_revision, active,
             "two-source graph edit was not acknowledged");
  // A third unbound MIDI track cannot borrow either instrument's receipt.
  const auto unbound = id<project::TrackId>(7);
  const auto third = take(model.apply({project::AddTrack{unbound, project::TrackKind::instrument},
                                       project::AddMidiClip{unbound, clip(17, 27, 200'000)}}));
  Gate third_gate;
  auto third_model = take(project::ProjectModel::open(*third.snapshot, third_gate));
  SourceAdmissionFixture source_fixture;
  app::RoutedPlaybackControl source_gate(third_model, {{1, 64}, frames, 1 << 20, 1 << 20},
                                         &source_fixture);
  REQUIRE_OK(source_gate.submit_current(),
             "third unbound MIDI track borrowed another source's graph or source-bank binding");
  REQUIRE_TRUE(source_gate.has_bound_instrument_track(id<project::TrackId>(2)),
               "third unbound MIDI track borrowed another source's graph or source-bank binding");
  REQUIRE_TRUE(source_gate.has_bound_instrument_track(id<project::TrackId>(3)),
               "third unbound MIDI track borrowed another source's graph or source-bank binding");
  REQUIRE_TRUE(!source_gate.has_bound_instrument_track(unbound),
               "third unbound MIDI track borrowed another source's graph or source-bank binding");
  REQUIRE_TRUE(
      !app::RoutedAudioSource::prepare(directory.path(), *third_model.snapshot(), source_gate, 128),
      "third unbound MIDI track borrowed another source's graph or source-bank binding");
  // The older active graph remains honest when a source schedule changes.
  require(!control.begin_edit(third, "unbound MIDI source") &&
              control.render_block(inputs, output, {2 * frames, 1}) ==
                  engine::RoutedRenderStatus::rendered &&
              control.status().active_graph_revision == revised.snapshot->revision &&
              control.status().application == app::PlaybackApplication::failed,
          "unsupported live source layout claimed to be audible");
  require(provider.stop_after_join(), "two instrument hosts did not receive stopped blocks");
}

void two_source_fault_isolated(const plugins::ScanResult& scanned, std::size_t frames) {
  Gate gate;
  const auto first_id = id<project::PluginInstanceId>(5);
  const auto second_id = id<project::PluginInstanceId>(6);
  auto state = make_model(gate, saved(scanned, first_id, 0.5F), saved(scanned, second_id, 0.25F));
  CapturingProvider provider;
  provider.set_scan_receipt(first_id, scanned);
  provider.set_scan_receipt(second_id, scanned);
  app::RoutedPlaybackControl control(state, {{1, 64}, frames, 1 << 20, 1 << 20}, &provider);
  REQUIRE_OK(control.submit_current(), "two-source fault fixture did not prepare");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames), invalid(frames);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "two-source fault fixture did not render first block");
  provider.owner(first_id)->process(invalid, {frames, 0}); // Fault only the first exact host.
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "one source fault contaminated the healthy source or allocated");
  REQUIRE_EQ(counted_allocations, 0,
             "one source fault contaminated the healthy source or allocated");
  REQUIRE_TRUE(control.processor_fault(id<project::TrackId>(2)),
               "one source fault contaminated the healthy source or allocated");
  REQUIRE_TRUE(!control.processor_fault(id<project::TrackId>(3)),
               "one source fault contaminated the healthy source or allocated");
  near(output[0], 0.25F);
  const auto healthy_slot = provider.manual_events(id<project::TrackId>(3), second_id);
  REQUIRE_TRUE(healthy_slot, "healthy source lost its active graph before stop");
  REQUIRE_NE(healthy_slot->active_graph().graph_revision, 0,
             "healthy source lost its active graph before stop");
  REQUIRE_TRUE(!provider.stop_after_join(),
               "stop-all failed to deactivate the healthy host after its peer faulted");
  REQUIRE_EQ(healthy_slot->active_graph().graph_revision, 0,
             "stop-all failed to deactivate the healthy host after its peer faulted");
}

void two_source_manual_edits(const plugins::ScanResult& scanned, std::size_t frames) {
  const auto first_track = id<project::TrackId>(2);
  const auto second_track = id<project::TrackId>(3);
  const auto first_id = id<project::PluginInstanceId>(5);
  const auto second_id = id<project::PluginInstanceId>(6);
  auto first_saved = saved(scanned, first_id, 0.5F);
  auto second_saved = saved(scanned, second_id, 0.25F);
  for (auto* instrument : {&first_saved, &second_saved}) {
    instrument->bundle_path = scanned.binary.canonical_bundle.string();
    instrument->missing = false;
  }
  first_saved.parameters = {{1, 0.5}};
  second_saved.parameters = {{1, 0.25}};
  Gate gate;
  auto initial = make_model(gate, first_saved, second_saved);
  Directory directory;
  {
    auto writer = take(oscillate::persistence::ProjectStore::open_writer(directory.path(), true));
    REQUIRE_EQ(
        take(writer.save(*initial.snapshot(), oscillate::persistence::SaveKind::explicit_save))
            .status,
        oscillate::persistence::SaveStatus::committed, "two-source manual project did not save");
  }
  auto session = take(app::ProjectSession::open(directory.path()));
  auto mirror = take(project::ProjectModel::open(session->snapshot(), gate));
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(first_id, scanned);
  provider.set_scan_receipt(second_id, scanned);
  app::RoutedPlaybackControl control(mirror, {{1, 64}, frames, 1 << 20, 1 << 20}, &provider);
  Adoption adoption(mirror, control);
  REQUIRE_OK(control.submit_current(), "two-source manual graph did not prepare");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  const auto render = [&](std::size_t block) {
    counted_allocations = 0;
    counting_allocations = true;
    const auto status = control.render_block(inputs, output, {block * frames, 1});
    counting_allocations = false;
    REQUIRE_EQ(status, engine::RoutedRenderStatus::rendered,
               "two-source manual callback failed, faulted or allocated");
    REQUIRE_EQ(counted_allocations, 0, "two-source manual callback failed, faulted or allocated");
    REQUIRE_TRUE(!control.processor_fault(),
                 "two-source manual callback failed, faulted or allocated");
  };
  render(0);
  near(output[10], 0.75F);
  control.observe();
  const auto topology = control.active_topology_revision();
  const auto first_binding =
      control.active_vst3_instrument_manual_events(first_track, first_id, topology);
  const auto second_binding =
      control.active_vst3_instrument_manual_events(second_track, second_id, topology);
  REQUIRE_TRUE(first_binding,
               "manual slots did not retain distinct saved track and plugin identity");
  REQUIRE_TRUE(second_binding,
               "manual slots did not retain distinct saved track and plugin identity");
  REQUIRE_NE(first_binding->slot, second_binding->slot,
             "manual slots did not retain distinct saved track and plugin identity");
  REQUIRE_NE(first_binding->identity.instance_id, second_binding->identity.instance_id,
             "manual slots did not retain distinct saved track and plugin identity");
  REQUIRE_TRUE(!control.active_vst3_instrument_manual_events(first_track, second_id, topology),
               "manual slots did not retain distinct saved track and plugin identity");
  REQUIRE_TRUE(!control.active_vst3_instrument_manual_events(second_track, first_id, topology),
               "manual slots did not retain distinct saved track and plugin identity");

  const auto plugin_on = [&](project::TrackId track) {
    const auto& tracks = session->snapshot().tracks;
    const auto owner = std::ranges::find(tracks, track, &project::Track::id);
    REQUIRE_NE(owner, tracks.end(), "manual edit owner vanished");
    return std::get<project::Vst3PluginInstance>(owner->instrument_source);
  };
  const auto commit = [&](project::TrackId track, app::Vst3ManualInstrumentEdit& edit, float value,
                          plugins::Vst3ActiveGraphIdentity identity) {
    const auto original = plugin_on(track);
    auto state = original.component_state;
    std::memcpy(state.data(), &value, sizeof(value));
    return edit.commit(session->snapshot().revision, track, original, std::move(state),
                       {{1, value}}, {1, 0, value}, identity);
  };
  const auto opened = session->snapshot().revision;
  app::Vst3ManualInstrumentEdit crossed(*session, first_binding->slot, &adoption);
  REQUIRE_TRUE(!commit(second_track, crossed, 0.4F, first_binding->identity),
               "cross-wired source slot saved or applied another track's edit");
  REQUIRE_EQ(session->snapshot().revision, opened,
             "cross-wired source slot saved or applied another track's edit");
  REQUIRE_EQ(control.status().active_graph_revision, opened,
             "cross-wired source slot saved or applied another track's edit");

  const auto second_before = plugin_on(second_track);
  app::Vst3ManualInstrumentEdit first_edit(*session, first_binding->slot, &adoption);
  const auto pending_first = take(commit(first_track, first_edit, 0.6F, first_binding->identity));
  REQUIRE_EQ(pending_first.phase, app::Vst3ManualEditPhase::pending,
             "first source edit lost pending revision truth or mutated second source");
  REQUIRE_EQ(pending_first.saved_revision, opened + 1,
             "first source edit lost pending revision truth or mutated second source");
  REQUIRE_EQ(control.status().active_graph_revision, opened,
             "first source edit lost pending revision truth or mutated second source");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "first source edit lost pending revision truth or mutated second source");
  REQUIRE_EQ(plugin_on(second_track), second_before,
             "first source edit lost pending revision truth or mutated second source");
  REQUIRE_TRUE(!control.active_vst3_instrument_manual_events(second_track, second_id, topology),
               "first source edit lost pending revision truth or mutated second source");
  render(1);
  near(output[0], 0.85F);
  const auto applied_first = first_edit.observe();
  control.observe();
  REQUIRE_EQ(applied_first.phase, app::Vst3ManualEditPhase::applied,
             "first source receipt did not preserve second source or its topology");
  REQUIRE_EQ(applied_first.live_revision, opened + 1,
             "first source receipt did not preserve second source or its topology");
  REQUIRE_EQ(applied_first.applied_sample, static_cast<std::int64_t>(frames),
             "first source receipt did not preserve second source or its topology");
  REQUIRE_EQ(control.status().active_graph_revision, opened + 1,
             "first source receipt did not preserve second source or its topology");
  REQUIRE_EQ(control.active_topology_revision(), topology,
             "first source receipt did not preserve second source or its topology");
  REQUIRE_EQ(plugin_on(second_track), second_before,
             "first source receipt did not preserve second source or its topology");
  const auto second_after_first =
      control.active_vst3_instrument_manual_events(second_track, second_id, topology);
  REQUIRE_TRUE(second_after_first, "second source slot changed after first source's live edit");
  REQUIRE_EQ(second_after_first->slot, second_binding->slot,
             "second source slot changed after first source's live edit");
  REQUIRE_EQ(second_after_first->identity.instance_id, second_binding->identity.instance_id,
             "second source slot changed after first source's live edit");

  app::Vst3ManualInstrumentEdit second_edit(*session, second_after_first->slot, &adoption);
  const auto first_before_second = plugin_on(first_track);
  const auto pending_second =
      take(commit(second_track, second_edit, 0.4F, second_after_first->identity));
  REQUIRE_EQ(pending_second.phase, app::Vst3ManualEditPhase::pending,
             "second source edit changed the first source or claimed early activation");
  REQUIRE_EQ(plugin_on(first_track), first_before_second,
             "second source edit changed the first source or claimed early activation");
  REQUIRE_EQ(control.status().active_graph_revision, opened + 1,
             "second source edit changed the first source or claimed early activation");
  render(2);
  near(output[0], 1.0F);
  const auto applied_second = second_edit.observe();
  control.observe();
  REQUIRE_EQ(applied_second.phase, app::Vst3ManualEditPhase::applied,
             "second source receipt did not preserve first source and revision truth");
  REQUIRE_EQ(applied_second.live_revision, opened + 2,
             "second source receipt did not preserve first source and revision truth");
  REQUIRE_EQ(applied_second.applied_sample, static_cast<std::int64_t>(2 * frames),
             "second source receipt did not preserve first source and revision truth");
  REQUIRE_EQ(control.status().active_graph_revision, opened + 2,
             "second source receipt did not preserve first source and revision truth");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "second source receipt did not preserve first source and revision truth");
  REQUIRE_EQ(control.active_topology_revision(), topology,
             "second source receipt did not preserve first source and revision truth");
  REQUIRE_EQ(plugin_on(first_track), first_before_second,
             "second source receipt did not preserve first source and revision truth");
  REQUIRE_OK(session->saving().save(), "two-source manual edits did not save");
  const auto first_final = plugin_on(first_track);
  const auto second_final = plugin_on(second_track);
  session.reset();
  auto reopened = take(app::ProjectSession::open(directory.path()));
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(reopened->snapshot().tracks[0].instrument_source),
      first_final, "two live instrument edits did not reopen with independent saved state");
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(reopened->snapshot().tracks[1].instrument_source),
      second_final, "two live instrument edits did not reopen with independent saved state");
  require(provider.stop_after_join(), "two-source manual hosts did not stop");
  Gate reopened_gate;
  auto reopened_model = take(project::ProjectModel::open(reopened->snapshot(), reopened_gate));
  plugins::Vst3InstrumentTrackProvider reopened_provider(plugins::Vst3ProcessingMode::realtime,
                                                         true);
  reopened_provider.set_scan_receipt(first_id, scanned);
  reopened_provider.set_scan_receipt(second_id, scanned);
  app::RoutedPlaybackControl reopened_control(reopened_model, {{1, 64}, frames, 1 << 20, 1 << 20},
                                              &reopened_provider);
  REQUIRE_OK(reopened_control.submit_current(),
             "reopened two-source state did not prepare its own graph");
  std::vector<std::span<const engine::StereoFrame>> reopened_inputs(
      reopened_control.topological_order().size());
  std::vector<engine::StereoFrame> reopened_output(frames);
  counted_allocations = 0;
  counting_allocations = true;
  const auto reopened_status =
      reopened_control.render_block(reopened_inputs, reopened_output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(reopened_status, engine::RoutedRenderStatus::rendered,
             "reopened two-source callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "reopened two-source callback failed or allocated");
  REQUIRE_TRUE(!reopened_control.processor_fault(),
               "reopened two-source callback failed or allocated");
  near(reopened_output[10], 1.0F);
  require(reopened_provider.stop_after_join(), "reopened two-source hosts did not stop");
}
} // namespace

int main() {
  try {
    plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
    const auto scanned = scanner.scan(fs::path(VST3_INSTRUMENT_BUNDLE));
    REQUIRE_TRUE(scanned.ok(), "two-source synthetic VST3 scan failed");
    REQUIRE_EQ(scanned.metadata.classes.size(), 1, "two-source synthetic VST3 scan failed");
    for (const std::size_t frames : {64U, 128U}) {
      two_source_live_route(scanned, frames);
      two_source_fault_isolated(scanned, frames);
      two_source_manual_edits(scanned, frames);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
