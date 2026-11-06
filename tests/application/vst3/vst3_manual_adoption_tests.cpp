#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/manual_effect_edit.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
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
namespace fs = std::filesystem;
using application::Vst3ManualEditPhase;

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

// The playback mirror and routed control of one run. It adopts the session's
// saved revision only through the manual-edit contract.
class Adoption final : public application::Vst3ManualLiveAdoption {
public:
  Adoption(project::ProjectModel& mirror, application::RoutedPlaybackControl& control)
      : mirror_(mirror), control_(control) {}
  foundation::Result<void> preflight(project::PluginInstanceId plugin,
                                     plugins::Vst3ActiveGraphIdentity expected) override {
    control_.observe();
    return control_.preflight_vst3_manual_edit(plugin, expected);
  }
  foundation::Result<void> begin(const project::Snapshot& saved, project::PluginInstanceId plugin,
                                 plugins::Vst3ActiveGraphIdentity expected) override {
    auto adopted = mirror_.adopt_authoritative_snapshot(saved);
    if (!adopted)
      return foundation::Result<void>::failure(adopted.error());
    return control_.begin_vst3_manual_edit(adopted.value(), "manual VST3 parameter", plugin,
                                           expected);
  }
  [[nodiscard]] foundation::Result<void> settle(plugins::Vst3ManualEventReceipt receipt) override {
    ++settles;
    auto result = control_.settle_vst3_manual_edit(receipt);
    last_settled = result.has_value();
    return result;
  }
  int settles = 0;
  bool last_settled = false;

private:
  project::ProjectModel& mirror_;
  application::RoutedPlaybackControl& control_;
};

enum class Kind : std::uint8_t { effect, instrument };

struct Options {
  Kind kind = Kind::effect;
  std::uint32_t frames = 64;
  bool qualified = true;
  bool automated = false;
  bool fail_saves = false;
};

// A saved project, its session, a playback mirror and an active routed graph
// for one qualified synthetic plugin, prepared for 128 frames like Quick and
// rendered in device blocks of the selected size. ConnectedGain multiplies by 4 x value
// after seven frames; TimedInstrument outputs its Level while a note sounds.
struct Rig {
  Options options;
  Gate gate;
  fs::path root;
  bool fail_save = false;
  std::unique_ptr<application::ProjectSession> session;
  std::optional<project::ProjectModel> mirror;
  std::unique_ptr<plugins::Vst3TrackInsertProvider> effects;
  std::unique_ptr<plugins::Vst3InstrumentTrackProvider> instruments;
  std::unique_ptr<application::RoutedPlaybackControl> control;
  std::unique_ptr<Adoption> adoption;
  project::TrackId track = id<project::TrackId>(2);
  project::InsertSlotId slot = id<project::InsertSlotId>(1);
  project::PluginInstanceId plugin = id<project::PluginInstanceId>(10);
  std::uint32_t parameter = 101;
  std::vector<engine::StereoFrame> input;
  std::vector<std::span<const engine::StereoFrame>> inputs;
  std::vector<engine::StereoFrame> output;

  Rig(const plugins::ScanResult& scanned, Options selected) : options(selected) {
    root = fs::temp_directory_path() /
           ("oscillate-manual-adoption-" + std::to_string(::getpid()) + "-" +
            std::to_string(static_cast<int>(options.kind)) + "-" + std::to_string(options.frames));
    fs::remove_all(root);
    write_project(scanned);
    session = take(application::ProjectSession::open(root, [this](persistence::SaveStage stage) {
      return fail_save && stage == persistence::SaveStage::manifest_written;
    }));
    if (options.automated) {
      const auto lane = session->parameter_automation().set_vst3_parameter_automation_point(
          track, plugin, parameter, 0, 0.4);
      REQUIRE_OK(lane,
                 "automation lane setup failed: " + (lane ? std::string{} : lane.error().message));
    }
    mirror.emplace(take(project::ProjectModel::open(session->snapshot(), gate)));
    application::TrackInsertProvider* provider = nullptr;
    if (options.kind == Kind::effect) {
      effects = std::make_unique<plugins::Vst3TrackInsertProvider>(
          plugins::Vst3ProcessingMode::realtime, options.qualified);
      effects->set_scan_receipt(plugin, scanned);
      provider = effects.get();
    } else {
      instruments = std::make_unique<plugins::Vst3InstrumentTrackProvider>(
          plugins::Vst3ProcessingMode::realtime, options.qualified);
      instruments->set_scan_receipt(plugin, scanned);
      provider = instruments.get();
    }
    control = std::make_unique<application::RoutedPlaybackControl>(
        *mirror, engine::RoutedMixerLimits{{1, 64}, 128, 4'096, 4'096}, provider);
    adoption = std::make_unique<Adoption>(*mirror, *control);
    const auto prepared = control->submit_current();
    REQUIRE_OK(prepared, "manual adoption graph did not prepare: " +
                             (prepared ? std::string{} : prepared.error().message));
    input.assign(options.frames, {0.25F, -0.25F});
    inputs.assign(control->topological_order().size(), {});
    for (const auto& item : control->source_layout()) {
      if (item.track == track)
        inputs[item.topological_index] = input;
    }
    output.assign(options.frames, {});
    require(render(0), "manual adoption graph did not activate");
    control->observe();
    fail_save = options.fail_saves;
  }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;
  ~Rig() {
    if (instruments)
      (void)instruments->stop_after_join();
    adoption.reset();
    control.reset();
    session.reset();
    fs::remove_all(root);
  }

  void write_project(const plugins::ScanResult& scanned) {
    auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
    auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
    auto state = take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000,
                                                    std::move(tempo), std::move(meter), gate));
    const auto klass = std::ranges::find_if(scanned.metadata.classes, [](const auto& candidate) {
      return candidate.category == "Audio Module Class";
    });
    REQUIRE_NE(klass, scanned.metadata.classes.end(), "fixture audio class missing");
    project::Vst3PluginInstance saved{.id = plugin,
                                      .class_id = klass->id,
                                      .vendor = klass->vendor,
                                      .name = klass->name,
                                      .version = klass->version,
                                      .bundle_path = scanned.binary.canonical_bundle.string(),
                                      .binary_sha256 = scanned.binary.sha256,
                                      .component_state = {},
                                      .parameters = {},
                                      .missing = false};
    if (options.kind == Kind::effect) {
      saved.parameters = {{parameter, 0.25}};
      (void)take(
          state.apply({project::AddTrack{track}, project::InsertVst3Plugin{track, 0, saved}}));
      slot = state.snapshot()->tracks.front().effects.front().id;
    } else {
      parameter = 1;
      const float level = 0.5F;
      saved.component_state.resize(sizeof(level));
      std::memcpy(saved.component_state.data(), &level, sizeof(level));
      saved.parameters = {{parameter, 0.5}};
      // The note starts at sample 5 and sounds for 12,000 samples, through
      // every block rendered here.
      const project::MidiClip clip{
          id<project::ClipId>(4),
          project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
          {timeline::ticks_per_quarter},
          false,
          {{id<project::NoteId>(6), {200'000}, {timeline::ticks_per_quarter / 2}, 60, 127}},
          1};
      (void)take(state.apply({project::AddTrack{track, project::TrackKind::instrument},
                              project::AddMidiClip{track, clip},
                              project::InsertVst3Plugin{track, 0, saved}}));
    }
    auto writer = take(persistence::ProjectStore::open_writer(root, true));
    REQUIRE_EQ(take(writer.save(*state.snapshot(), persistence::SaveKind::explicit_save)).status,
               persistence::SaveStatus::committed, "manual adoption project save did not commit");
  }

  [[nodiscard]] bool render(std::uint64_t block) {
    const engine::BlockContext context{block * options.frames, 1};
    counting_allocations = true;
    const auto status = control->render_block(inputs, output, context);
    counting_allocations = false;
    return status == engine::RoutedRenderStatus::rendered;
  }

  [[nodiscard]] const project::Vst3PluginInstance& saved_plugin() const {
    const auto& owner = session->snapshot().tracks.front();
    return options.kind == Kind::effect
               ? std::get<project::Vst3PluginInstance>(owner.effects.front().processor)
               : std::get<project::Vst3PluginInstance>(owner.instrument_source);
  }

  [[nodiscard]] std::optional<
      std::pair<std::shared_ptr<plugins::Vst3ManualEventSlot>, plugins::Vst3ActiveGraphIdentity>>
  binding() const {
    const auto topology = control->active_topology_revision();
    if (options.kind == Kind::effect) {
      if (auto found = control->active_vst3_effect_manual_events(track, slot, plugin, topology))
        return std::pair{found->slot, found->identity};
    } else if (auto found =
                   control->active_vst3_instrument_manual_events(track, plugin, topology)) {
      return std::pair{found->slot, found->identity};
    }
    return std::nullopt;
  }

  // One transaction per gesture, as a UI owner keeps it until its receipt.
  struct Edit {
    std::optional<application::Vst3ManualEffectEdit> effect;
    std::optional<application::Vst3ManualInstrumentEdit> instrument;
    application::Vst3ManualEditStatus observe() {
      return effect ? effect->observe() : instrument->observe();
    }
  };

  [[nodiscard]] Edit edit(std::shared_ptr<plugins::Vst3ManualEventSlot> manual) {
    Edit result;
    if (options.kind == Kind::effect)
      result.effect.emplace(*session, std::move(manual), adoption.get());
    else
      result.instrument.emplace(*session, std::move(manual), adoption.get());
    return result;
  }

  foundation::Result<application::Vst3ManualEditStatus>
  commit(Edit& gesture, double value, plugins::Vst3ActiveGraphIdentity identity) {
    const auto original = saved_plugin();
    auto references = original.parameters;
    std::erase_if(references, [&](const auto& item) { return item.id == parameter; });
    references.push_back({parameter, value});
    std::ranges::sort(references, {}, &project::Vst3ParameterReference::id);
    const plugins::Vst3ParameterEvent event{parameter, 0, value};
    const auto revision = session->snapshot().revision;
    if (gesture.effect)
      return gesture.effect->commit(revision, track, slot, original, original.component_state,
                                    std::move(references), event, identity);
    return gesture.instrument->commit(revision, track, original, original.component_state,
                                      std::move(references), event, identity);
  }

  // The audible sample for a plugin value once latency has filled.
  [[nodiscard]] float expected(double value) const {
    return options.kind == Kind::effect ? 0.25F * static_cast<float>(value * 4.0)
                                        : static_cast<float>(value);
  }
  [[nodiscard]] float baseline() const {
    return expected(options.kind == Kind::effect ? 0.25 : 0.5);
  }
  [[nodiscard]] bool audible(float value) const {
    return std::fabs(output.front().left - value) < 0.00001F &&
           std::fabs(output.back().left - value) < 0.00001F;
  }
};

void two_successive_edits(const plugins::ScanResult& scanned, Kind kind, std::uint32_t frames) {
  Rig rig(scanned, {.kind = kind, .frames = frames});
  const auto opened = rig.session->snapshot().revision;
  const auto topology = rig.control->active_topology_revision();
  REQUIRE_EQ(rig.control->status().active_graph_revision, opened,
             "qualified fixture did not play its saved value");
  REQUIRE_TRUE((kind == Kind::instrument || rig.audible(rig.baseline())),
               "qualified fixture did not play its saved value");
  auto first_binding = rig.binding();
  REQUIRE_OK(first_binding, "qualified active plugin exposed no manual slot");
  const auto [manual, identity] = *first_binding;

  auto first = rig.edit(manual);
  auto pending = take(rig.commit(first, 0.6, identity));
  REQUIRE_EQ(pending.phase, Vst3ManualEditPhase::pending,
             "first manual edit was not pending after save");
  REQUIRE_EQ(pending.saved_revision, opened + 1, "first manual edit was not pending after save");
  REQUIRE_EQ(pending.live_revision, 0, "first manual edit was not pending after save");
  const auto held = rig.control->status();
  REQUIRE_EQ(held.model_revision, opened + 1,
             "pending manual revision was reported live or admitted a second gesture");
  REQUIRE_EQ(held.active_graph_revision, opened,
             "pending manual revision was reported live or admitted a second gesture");
  REQUIRE_EQ(held.application, application::PlaybackApplication::pending,
             "pending manual revision was reported live or admitted a second gesture");
  REQUIRE_TRUE(rig.control->vst3_manual_edit_pending(),
               "pending manual revision was reported live or admitted a second gesture");
  REQUIRE_TRUE(!rig.binding(),
               "pending manual revision was reported live or admitted a second gesture");
  REQUIRE_TRUE(!rig.commit(first, 0.65, identity),
               "pending manual revision was reported live or admitted a second gesture");
  REQUIRE_EQ(rig.session->snapshot().revision, opened + 1,
             "pending manual revision was reported live or admitted a second gesture");

  counted_allocations = 0;
  REQUIRE_TRUE(rig.render(1), "manual event block failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "manual event block failed or allocated");
  REQUIRE_TRUE(!rig.control->processor_fault(), "manual event block failed or allocated");
  require(rig.audible(rig.expected(0.6)), "first manual value was not audible at the boundary");
  const auto applied = first.observe();
  rig.control->observe();
  const auto live = rig.control->status();
  REQUIRE_EQ(applied.phase, Vst3ManualEditPhase::applied,
             "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_EQ(applied.live_revision, opened + 1,
             "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_EQ(applied.applied_sample, static_cast<std::int64_t>(frames),
             "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_EQ(rig.adoption->settles, 1,
             "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_TRUE(rig.adoption->last_settled,
               "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_EQ(live.model_revision, opened + 1,
             "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_EQ(live.active_graph_revision, opened + 1,
             "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_EQ(live.application, application::PlaybackApplication::active,
             "applied receipt did not advance the audible revision without a topology change");
  REQUIRE_EQ(rig.control->active_topology_revision(), topology,
             "applied receipt did not advance the audible revision without a topology change");

  auto second_binding = rig.binding();
  REQUIRE_TRUE(second_binding, "adopted revision lost the retained runtime slot identity");
  REQUIRE_EQ(second_binding->first, manual,
             "adopted revision lost the retained runtime slot identity");
  REQUIRE_EQ(second_binding->second.graph_revision, topology,
             "adopted revision lost the retained runtime slot identity");
  REQUIRE_EQ(second_binding->second.instance_id, identity.instance_id,
             "adopted revision lost the retained runtime slot identity");
  REQUIRE_EQ(second_binding->second.compatibility_key, identity.compatibility_key,
             "adopted revision lost the retained runtime slot identity");
  auto second = rig.edit(manual);
  REQUIRE_EQ(take(rig.commit(second, 0.75, second_binding->second)).phase,
             Vst3ManualEditPhase::pending,
             "second manual edit was not admitted from the matched state");
  counted_allocations = 0;
  REQUIRE_TRUE(rig.render(2), "second manual value was not audible or allocated");
  REQUIRE_EQ(counted_allocations, 0, "second manual value was not audible or allocated");
  REQUIRE_TRUE(rig.audible(rig.expected(0.75)), "second manual value was not audible or allocated");
  const auto second_applied = second.observe();
  rig.control->observe();
  REQUIRE_EQ(second_applied.phase, Vst3ManualEditPhase::applied,
             "second receipt did not identify its exact revision and sample");
  REQUIRE_EQ(second_applied.live_revision, opened + 2,
             "second receipt did not identify its exact revision and sample");
  REQUIRE_EQ(second_applied.applied_sample, static_cast<std::int64_t>(2 * frames),
             "second receipt did not identify its exact revision and sample");
  REQUIRE_EQ(rig.control->status().active_graph_revision, opened + 2,
             "second receipt did not identify its exact revision and sample");
  REQUIRE_EQ(rig.adoption->settles, 2,
             "second receipt did not identify its exact revision and sample");
  REQUIRE_TRUE(rig.render(3), "retained runtime did not keep the second value");
  REQUIRE_TRUE(rig.audible(rig.expected(0.75)), "retained runtime did not keep the second value");

  // Undo saves the earlier value without an event. An effect's earlier state
  // loads into a new instance that switches in live (ADR-0050); an
  // instrument's must wait for a voice handoff, so the audible revision stays
  // on the acknowledged edit rather than imply the undo is live.
  REQUIRE_OK(rig.session->history().undo(), "manual edit undo failed");
  const auto mirrored = take(rig.mirror->adopt_authoritative_snapshot(rig.session->snapshot()));
  const auto holding = [&] {
    const engine::BlockContext context{4 * static_cast<std::uint64_t>(frames), 1};
    return rig.control->render_block(rig.inputs, rig.output, context) ==
           engine::RoutedRenderStatus::holding;
  };
  const auto undo_edit = rig.control->begin_edit(mirrored, "undo");
  if (kind == Kind::effect) {
    REQUIRE_OK(undo_edit, "undo state change did not switch live");
    REQUIRE_TRUE(rig.render(4), "undo state change did not switch live");
    rig.control->observe();
    REQUIRE_EQ(rig.control->status().active_graph_revision, mirrored.snapshot->revision,
               "undo state change did not become the audible revision");
    REQUIRE_EQ(rig.control->status().application, application::PlaybackApplication::active,
               "undo state change did not become the audible revision");
    REQUIRE_OK(rig.session->saving().save(), "undone manual intent did not save");
    return;
  }
  require(!undo_edit || (holding() && !rig.control->publish_held()),
          "undo state change published over the active plugin");
  require(rig.control->status().active_graph_revision == opened + 2 &&
              rig.control->status().application == application::PlaybackApplication::failed &&
              rig.render(4) && rig.audible(rig.expected(0.75)) && !rig.binding(),
          "undo was implied live or replaced the acknowledged audible value: active=" +
              std::to_string(rig.control->status().active_graph_revision) +
              " expected=" + std::to_string(opened + 2) + " application=" +
              std::to_string(static_cast<int>(rig.control->status().application)) +
              " fault=" + std::to_string(rig.control->processor_fault().has_value()) +
              " out=" + std::to_string(rig.output.front().left) + "/" +
              std::to_string(rig.output.back().left) +
              " binding=" + std::to_string(rig.binding().has_value()));
  const auto intent = rig.saved_plugin();
  REQUIRE_OK(rig.session->saving().save(), "undone manual intent did not save");
  rig.session.reset();
  auto reopened = take(application::ProjectSession::open(rig.root));
  const auto& owner = reopened->snapshot().tracks.front();
  const auto& restored =
      kind == Kind::effect ? std::get<project::Vst3PluginInstance>(owner.effects.front().processor)
                           : std::get<project::Vst3PluginInstance>(owner.instrument_source);
  REQUIRE_EQ(restored, intent, "saved manual intent did not reopen");
}

void rapid_drag_over_pending_receipt(const plugins::ScanResult& scanned, Kind kind,
                                     std::uint32_t frames) {
  Rig rig(scanned, {.kind = kind, .frames = frames});
  const auto opened = rig.session->snapshot().revision;
  const auto binding = rig.binding();
  REQUIRE_OK(binding, "rapid preview fixture exposed no active slot");
  const auto [slot, identity] = *binding;
  auto first = rig.edit(slot);
  REQUIRE_EQ(take(rig.commit(first, 0.6, identity)).phase, Vst3ManualEditPhase::pending,
             "rapid preview fixture did not save the first gesture");
  const auto insert =
      kind == Kind::effect ? std::optional<project::InsertSlotId>{rig.slot} : std::nullopt;
  const auto pending =
      rig.control->pending_vst3_manual_preview_events(rig.track, insert, rig.plugin);
  REQUIRE_TRUE(pending, "pending preview did not retain the exact active plugin identity");
  REQUIRE_EQ(pending->slot, slot,
             "pending preview did not retain the exact active plugin identity");
  REQUIRE_EQ(pending->identity.graph_revision, identity.graph_revision,
             "pending preview did not retain the exact active plugin identity");
  REQUIRE_TRUE(!rig.control->pending_vst3_manual_preview_events(rig.track, insert,
                                                                id<project::PluginInstanceId>(11)),
               "pending preview did not retain the exact active plugin identity");
  REQUIRE_TRUE(!rig.control->pending_vst3_manual_preview_events(
                   rig.track,
                   kind == Kind::effect
                       ? std::optional<project::InsertSlotId>{id<project::InsertSlotId>(999)}
                       : std::optional<project::InsertSlotId>{rig.slot},
                   rig.plugin),
               "pending preview did not retain the exact active plugin identity");
  const auto preview = [&](std::uint32_t parameter, double value,
                           plugins::Vst3ActiveGraphIdentity graph) {
    return pending->slot->preview(graph.graph_revision, graph.instance_id, graph.compatibility_key,
                                  {parameter, 0, value});
  };
  REQUIRE_EQ(preview(rig.parameter, 0.7, identity), plugins::Vst3ManualEventStatus::accepted,
             "pending preview failed latest-value or parameter qualification");
  REQUIRE_EQ(preview(rig.parameter, 0.8, identity), plugins::Vst3ManualEventStatus::accepted,
             "pending preview failed latest-value or parameter qualification");
  REQUIRE_EQ(preview(rig.parameter + 900, 0.9, identity),
             plugins::Vst3ManualEventStatus::unavailable,
             "pending preview failed latest-value or parameter qualification");
  auto stale = identity;
  ++stale.instance_id;
  REQUIRE_EQ(preview(rig.parameter, 0.9, stale), plugins::Vst3ManualEventStatus::stale_instance,
             "stale preview was admitted or pending model revision was reported live");
  REQUIRE_EQ(rig.control->status().model_revision, opened + 1,
             "stale preview was admitted or pending model revision was reported live");
  REQUIRE_EQ(rig.control->status().active_graph_revision, opened,
             "stale preview was admitted or pending model revision was reported live");

  counted_allocations = 0;
  REQUIRE_TRUE(rig.render(1), "pending committed value did not precede a later drag preview");
  REQUIRE_EQ(counted_allocations, 0,
             "pending committed value did not precede a later drag preview");
  REQUIRE_TRUE(rig.audible(rig.expected(0.6)),
               "pending committed value did not precede a later drag preview");
  REQUIRE_EQ(first.observe().phase, Vst3ManualEditPhase::applied,
             "first committed value lost its exact receipt");
  rig.control->observe();
  REQUIRE_EQ(rig.control->status().active_graph_revision, opened + 1,
             "transient preview advanced the audible saved revision");
  counted_allocations = 0;
  REQUIRE_TRUE(rig.render(2),
               "latest drag value was not audible in the next callback without allocation");
  REQUIRE_EQ(counted_allocations, 0,
             "latest drag value was not audible in the next callback without allocation");
  REQUIRE_TRUE(rig.audible(rig.expected(0.8)),
               "latest drag value was not audible in the next callback without allocation");
  auto second = rig.edit(slot);
  REQUIRE_EQ(take(rig.commit(second, 0.8, identity)).phase, Vst3ManualEditPhase::pending,
             "later gesture did not save after the first exact receipt");
  counted_allocations = 0;
  REQUIRE_TRUE(rig.render(3), "later committed gesture was not audible or allocated");
  REQUIRE_EQ(counted_allocations, 0, "later committed gesture was not audible or allocated");
  REQUIRE_TRUE(rig.audible(rig.expected(0.8)),
               "later committed gesture was not audible or allocated");
  REQUIRE_EQ(second.observe().phase, Vst3ManualEditPhase::applied,
             "later committed gesture was not audible or allocated");
  rig.control->observe();
  REQUIRE_EQ(rig.control->status().active_graph_revision, opened + 2,
             "second gesture did not settle the exact saved revision");

  Rig failed(scanned, {.kind = kind, .frames = frames});
  const auto failed_binding = failed.binding();
  REQUIRE_OK(failed_binding, "failed preview fixture exposed no slot");
  auto failed_edit = failed.edit(failed_binding->first);
  REQUIRE_EQ(take(failed.commit(failed_edit, 0.6, failed_binding->second)).phase,
             Vst3ManualEditPhase::pending, "failed preview fixture did not save");
  const auto failed_pending =
      failed.control->pending_vst3_manual_preview_events(failed.track, insert, failed.plugin);
  REQUIRE_TRUE(failed_pending, "failed preview fixture did not stage a later drag");
  REQUIRE_EQ(failed_pending->slot->preview(
                 failed_pending->identity.graph_revision, failed_pending->identity.instance_id,
                 failed_pending->identity.compatibility_key, {failed.parameter, 0, 0.8}),
             plugins::Vst3ManualEventStatus::accepted,
             "failed preview fixture did not stage a later drag");
  REQUIRE_OK(failed_binding->first->claim(), "failed pending event was not claimable");
  failed_binding->first->finish(false, -1);
  REQUIRE_TRUE(
      !failed.control->pending_vst3_manual_preview_events(failed.track, insert, failed.plugin),
      "failed commit left preview access or an applied receipt");
  REQUIRE_EQ(failed_edit.observe().phase, Vst3ManualEditPhase::failed,
             "failed commit left preview access or an applied receipt");
  failed.control->observe();
  counted_allocations = 0;
  REQUIRE_TRUE(failed.render(1),
               "failed committed event leaked the transient preview into audible audio");
  REQUIRE_EQ(counted_allocations, 0,
             "failed committed event leaked the transient preview into audible audio");
  REQUIRE_TRUE(failed.audible(failed.baseline()),
               "failed committed event leaked the transient preview into audible audio");
  REQUIRE_EQ(failed.control->status().active_graph_revision,
             failed.session->snapshot().revision - 1,
             "failed committed event leaked the transient preview into audible audio");
}

void fail_closed_paths(const plugins::ScanResult& scanned, Kind kind) {
  {
    Rig rig(scanned, {.kind = kind, .qualified = false});
    const auto opened = rig.session->snapshot().revision;
    if (kind == Kind::effect) {
      // The effect slot exists but its closed latency gate refuses admission.
      auto binding = rig.binding();
      REQUIRE_OK(binding, "unqualified effect exposed no slot to refuse");
      auto gesture = rig.edit(binding->first);
      const auto refused = rig.commit(gesture, 0.6, binding->second);
      REQUIRE_TRUE(!refused, "unqualified effect saved or admitted a live edit");
      REQUIRE_NE(refused.error().message.find("latency"), std::string::npos,
                 "unqualified effect saved or admitted a live edit");
      REQUIRE_EQ(rig.session->snapshot().revision, opened,
                 "unqualified effect saved or admitted a live edit");
    } else {
      require(!rig.binding(), "unqualified instrument exposed a manual slot");
    }
  }
  {
    Rig rig(scanned, {.kind = kind, .automated = true});
    const auto opened = rig.session->snapshot().revision;
    auto binding = rig.binding();
    REQUIRE_OK(binding, "automated fixture exposed no slot");
    auto gesture = rig.edit(binding->first);
    const auto refused = rig.commit(gesture, 0.6, binding->second);
    REQUIRE_TRUE(!refused, "automated parameter accepted a manual live edit");
    REQUIRE_NE(refused.error().message.find("automation"), std::string::npos,
               "automated parameter accepted a manual live edit");
    REQUIRE_EQ(rig.session->snapshot().revision, opened,
               "automated parameter accepted a manual live edit");
    REQUIRE_EQ(rig.control->status().active_graph_revision, opened,
               "automated parameter accepted a manual live edit");
  }
  {
    Rig rig(scanned, {.kind = kind});
    const auto opened = rig.session->snapshot().revision;
    auto binding = rig.binding();
    REQUIRE_OK(binding, "stale fixture exposed no slot");
    auto stale = binding->second;
    ++stale.instance_id;
    auto gesture = rig.edit(binding->first);
    REQUIRE_TRUE(!rig.commit(gesture, 0.6, stale), "stale instance identity saved a live edit");
    REQUIRE_EQ(rig.session->snapshot().revision, opened,
               "stale instance identity saved a live edit");
    // A plugin fault during the event block fails the receipt. The prior
    // audible revision and runtime remain, saved intent is kept, and no later
    // gesture can claim the newer saved state from the mismatched plugin.
    REQUIRE_EQ(take(rig.commit(gesture, 0.6, binding->second)).phase, Vst3ManualEditPhase::pending,
               "fault fixture edit was not pending");
    REQUIRE_OK(binding->first->claim(), "fault fixture event was not claimable");
    binding->first->finish(false, -1);
    const auto failed = gesture.observe();
    rig.control->observe();
    require(failed.phase == Vst3ManualEditPhase::failed && failed.saved_revision == opened + 1 &&
                rig.control->status().active_graph_revision == opened &&
                rig.control->status().application == application::PlaybackApplication::failed &&
                rig.saved_plugin().parameters.back().normalized_value == 0.6 && rig.render(1) &&
                rig.audible(rig.baseline()) && !rig.binding(),
            "failed receipt was implied live or lost saved intent: phase=" +
                std::to_string(static_cast<int>(failed.phase)) +
                " active=" + std::to_string(rig.control->status().active_graph_revision) +
                " opened=" + std::to_string(opened) +
                " app=" + std::to_string(static_cast<int>(rig.control->status().application)) +
                " out=" + std::to_string(rig.output.front().left) +
                " binding=" + std::to_string(rig.binding().has_value()) +
                " kind=" + std::to_string(static_cast<int>(kind)));
    auto later = rig.edit(binding->first);
    REQUIRE_TRUE(!rig.commit(later, 0.75, binding->second),
                 "later gesture proceeded from an unmatched audible state");
    REQUIRE_EQ(rig.session->snapshot().revision, opened + 1,
               "later gesture proceeded from an unmatched audible state");
  }
  {
    Rig rig(scanned, {.kind = kind, .fail_saves = true});
    const auto opened = rig.session->snapshot().revision;
    auto binding = rig.binding();
    REQUIRE_OK(binding, "save-error fixture exposed no slot");
    auto gesture = rig.edit(binding->first);
    const auto refused = rig.commit(gesture, 0.6, binding->second);
    const auto status = gesture.observe();
    REQUIRE_TRUE(!refused, "unsaved manual edit reached the live plugin");
    REQUIRE_EQ(status.phase, Vst3ManualEditPhase::model_not_saved,
               "unsaved manual edit reached the live plugin");
    REQUIRE_EQ(rig.adoption->settles, 0, "unsaved manual edit reached the live plugin");
    REQUIRE_TRUE(!rig.control->vst3_manual_edit_pending(),
                 "unsaved manual edit reached the live plugin");
    REQUIRE_EQ(rig.control->status().active_graph_revision, opened,
               "unsaved manual edit reached the live plugin");
    REQUIRE_TRUE(rig.render(1), "unsaved manual edit reached the live plugin");
    REQUIRE_TRUE(rig.audible(rig.baseline()), "unsaved manual edit reached the live plugin");
  }
}

} // namespace

int main() {
  try {
    plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
    const auto effect = scanner.scan(VST3_CONNECTED_BUNDLE);
    const auto instrument = scanner.scan(VST3_INSTRUMENT_BUNDLE);
    REQUIRE_TRUE(effect.ok(), "manual adoption fixtures did not scan");
    REQUIRE_TRUE(instrument.ok(), "manual adoption fixtures did not scan");
    for (const auto frames : {64U, 128U}) {
      two_successive_edits(effect, Kind::effect, frames);
      two_successive_edits(instrument, Kind::instrument, frames);
      rapid_drag_over_pending_receipt(effect, Kind::effect, frames);
      rapid_drag_over_pending_receipt(instrument, Kind::instrument, frames);
    }
    fail_closed_paths(effect, Kind::effect);
    fail_closed_paths(instrument, Kind::instrument);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
