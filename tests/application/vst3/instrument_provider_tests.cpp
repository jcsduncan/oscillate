#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/routing/project_route_plan.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
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

namespace tolerance = oscillate::test::tolerance;

using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using oscillate::test::require;
using oscillate::test::take;
template <typename Id> Id id(std::uint64_t low) { return take(Id::from_bits({0, low})); }

project::Vst3PluginInstance& saved_instrument(project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.instrument_source);
}

const project::Vst3PluginInstance& saved_instrument(const project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.instrument_source);
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

class IncompleteProvider final : public application::TrackInsertProvider {
public:
  foundation::Result<application::PreparedTrackInsert>
  prepare(const project::Snapshot&, const project::Vst3PluginInstance&, std::uint32_t) override {
    return foundation::Result<application::PreparedTrackInsert>::success({});
  }
};

class WrongKindProvider final : public application::TrackInsertProvider {
public:
  class SilentProcessor final : public engine::ProcessorRuntime {
  public:
    void process(std::span<engine::StereoFrame>, engine::BlockContext) noexcept override {}
  };
  foundation::Result<application::PreparedTrackInsert>
  prepare(const project::Snapshot&, const project::Vst3PluginInstance&, std::uint32_t) override {
    application::PreparedTrackInsert prepared{};
    prepared.processor = std::make_shared<SilentProcessor>();
    prepared.process_failed = []() noexcept { return false; };
    return foundation::Result<application::PreparedTrackInsert>::success(std::move(prepared));
  }
};

struct TemporaryProject {
  fs::path path =
      fs::temp_directory_path() / ("oscillate-vst3-instrument-" + std::to_string(getpid()));
  TemporaryProject() { fs::remove_all(path); }
  ~TemporaryProject() { fs::remove_all(path); }
};

project::Vst3PluginInstance saved(const plugins::ScanResult& scan) {
  const auto& klass = scan.metadata.classes.front();
  project::Vst3PluginInstance result{.id = id<project::PluginInstanceId>(5),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = "/advisory/TimedInstrument.vst3",
                                     .binary_sha256 = scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  const float level = 0.5F;
  result.component_state.resize(sizeof(level));
  std::memcpy(result.component_state.data(), &level, sizeof(level));
  return result;
}

project::ProjectModel make_model(Gate& gate, const project::Vst3PluginInstance& plugin) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  auto state = take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000,
                                                  std::move(tempo), std::move(meter), gate));
  const project::MidiClip clip{id<project::ClipId>(4),
                               project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
                               {timeline::ticks_per_quarter},
                               false,
                               {{id<project::NoteId>(6), {200'000}, {10'240'000}, 60, 127}},
                               1};
  (void)take(
      state.apply({project::AddTrack{id<project::TrackId>(2), project::TrackKind::instrument},
                   project::AddTrack{id<project::TrackId>(3), project::TrackKind::return_bus},
                   project::AddMidiClip{id<project::TrackId>(2), clip},
                   project::InsertVst3Plugin{id<project::TrackId>(2), 0, plugin}}));
  return state;
}

project::ProjectModel reopen(project::ProjectModel& state, Gate& gate) {
  TemporaryProject folder;
  auto writer = take(persistence::ProjectStore::open_writer(folder.path, true));
  REQUIRE_EQ(take(writer.save(*state.snapshot(), persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "instrument project save did not commit");
  auto reader = take(persistence::ProjectStore::open_reader(folder.path));
  auto loaded = take(reader.load_current());
  REQUIRE_EQ(saved_instrument(loaded.snapshot.tracks[0]),
             saved_instrument(state.snapshot()->tracks[0]),
             "saved instrument identity, opaque state or MIDI did not reopen");
  REQUIRE_EQ(loaded.snapshot.tracks[0].midi_clips, state.snapshot()->tracks[0].midi_clips,
             "saved instrument identity, opaque state or MIDI did not reopen");
  return take(project::ProjectModel::open(std::move(loaded.snapshot), gate));
}

void near(engine::StereoFrame frame, float expected) {
  REQUIRE_LT(std::fabs(frame.left - expected), 0.00001F,
             "VST3 instrument routed audio differs from note timing oracle");
  REQUIRE_LT(std::fabs(frame.right + expected), 0.00001F,
             "VST3 instrument routed audio differs from note timing oracle");
}

void expect_fixture_transport(const plugins::ScanResult& scanned, std::int64_t expected_sample,
                              double expected_quarter, double expected_bpm) {
  void* module = dlopen(scanned.binary.canonical_binary.c_str(), RTLD_NOW | RTLD_NOLOAD);
  REQUIRE_NE(module, nullptr, "instrument fixture module was not resident");
  using Reader = bool (*)(std::int64_t*, double*, double*, bool*);
  auto reader = reinterpret_cast<Reader>(dlsym(module, "OscillateFixtureTransport"));
  REQUIRE_NE(reader, nullptr, "instrument fixture transport probe is unavailable");
  std::int64_t sample = -1;
  double quarter = -1.0;
  double bpm = -1.0;
  bool playing = false;
  REQUIRE_TRUE(reader(&sample, &quarter, &bpm, &playing),
               "instrument received incorrect transport across a tempo boundary");
  REQUIRE_EQ(sample, expected_sample,
             "instrument received incorrect transport across a tempo boundary");
  REQUIRE_LT(std::fabs(quarter - expected_quarter), 1e-8,
             "instrument received incorrect transport across a tempo boundary");
  REQUIRE_LT(std::fabs(bpm - expected_bpm), 1e-8,
             "instrument received incorrect transport across a tempo boundary");
  REQUIRE_TRUE(playing, "instrument received incorrect transport across a tempo boundary");
  REQUIRE_EQ(dlclose(module), 0, "instrument fixture transport probe could not close");
}

void stepped_tempo_within_block(const plugins::ScanResult& scanned, std::uint32_t frames) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto snapshot = *state.snapshot();
  // At 48 kHz and 120 BPM this tick is exactly sample 32, inside either block size.
  snapshot.tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::step}, {{1'280'000}, 90.0, timeline::TempoShape::step}},
      2));
  snapshot.tracks[0].automation.push_back(
      {id<project::AutomationLaneId>(70),
       project::Vst3ParameterTarget{id<project::PluginInstanceId>(5), 1},
       {project::AutomationRepresentation::vst3_normalized, "level", "", 0.0, 1.0, 0.5, false, 0},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(71), 30, 0.5, project::AutomationShape::linear},
        {id<project::AutomationPointId>(72), 34, 1.0}},
       1});
  auto prepared_snapshot = std::make_shared<project::Snapshot>(std::move(snapshot));
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(
      provider.prepare(*prepared_snapshot, saved_instrument(prepared_snapshot->tracks[0]), frames));
  std::vector<engine::StereoFrame> output(frames);
  counted_allocations = 0;
  counting_allocations = true;
  source.processor->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "stepped tempo instrument callback failed or allocated");
  REQUIRE_TRUE(!source.process_failed(), "stepped tempo instrument callback failed or allocated");
  near(output[4], 0.0F);
  near(output[5], 0.5F);
  near(output[31], 0.625F);
  near(output[32], 0.75F);
  near(output[34], 1.0F);
  expect_fixture_transport(scanned, 32, 32.0 / 24'000.0, 90.0);
  source.processor->process(output, {frames, 1});
  require(!source.process_failed(), "instrument faulted after the tempo transition");
  expect_fixture_transport(scanned, frames,
                           32.0 / 24'000.0 + (frames - 32.0) * 90.0 / (60.0 * 48'000.0), 90.0);
  require(provider.stop_after_join(), "stepped tempo instrument did not stop");
}

void gradual_tempo_at_block_start(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto snapshot = *state.snapshot();
  snapshot.tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::linear_bpm},
       {{4 * timeline::ticks_per_quarter}, 180.0, timeline::TempoShape::step}},
      2));
  auto prepared_snapshot = std::make_shared<project::Snapshot>(std::move(snapshot));
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(
      provider.prepare(*prepared_snapshot, saved_instrument(prepared_snapshot->tracks[0]), 64));
  std::array<engine::StereoFrame, 64> output{};
  counted_allocations = 0;
  counting_allocations = true;
  source.processor->process(output, {24'000, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "gradual tempo instrument callback failed or allocated");
  REQUIRE_TRUE(!source.process_failed(), "gradual tempo instrument callback failed or allocated");
  const auto ratio = std::exp(0.125);
  expect_fixture_transport(scanned, 24'000, 8.0 * (ratio - 1.0), 120.0 * ratio);
  require(provider.stop_after_join(), "gradual tempo instrument did not stop");
}

void live_routed_instrument(const plugins::ScanResult& scanned, std::uint32_t frames) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto reopened = reopen(state, gate);
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  application::RoutedPlaybackControl control(reopened, {{1, 64}, frames, 4'096, 4'096}, &provider);
  if (auto submitted = control.submit_current(); !submitted)
    throw std::runtime_error("verified instrument route failed: " + submitted.error().message);
  REQUIRE_EQ(control.bound_instrument_track(), id<project::TrackId>(2),
             "single routed VST3 instrument lost its bound source track");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  counting_allocations = true;
  const auto first = control.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, engine::RoutedRenderStatus::rendered,
             "instrument first routed callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "instrument first routed callback failed or allocated");
  near(output[0], 0.0F);
  near(output[4], 0.0F);
  near(output[5], 0.5F);
  near(output[frames - 1], 0.5F);
  require(!control.processor_fault(), "healthy instrument reported a process failure");
  control.observe();

  const auto send = id<project::SendId>(7);
  auto revised = take(reopened.apply(
      {project::AddSendRoute{id<project::TrackId>(2), {send, id<project::TrackId>(3), 0.5}}}));
  // A route edit around an unchanged VST3 instrument switches live: no hold.
  require(control.begin_edit(revised, "route instrument send").has_value(),
          "live instrument route edit did not publish");
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "routed instrument changed graph with a callback allocation");
  REQUIRE_EQ(counted_allocations, 0, "routed instrument changed graph with a callback allocation");
  near(output[0], 0.75F);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, revised.snapshot->revision,
             "instrument route graph revision did not activate");
  REQUIRE_EQ(control.render_block(inputs, output, {0, 2}), engine::RoutedRenderStatus::rendered,
             "instrument seek render failed");
  near(output[0], 0.0F);
  near(output[4], 0.0F);
  near(output[5], 0.75F);
  require(provider.stop_after_join(), "instrument stop did not send a stopped block");
  const auto resume_at = frames;
  REQUIRE_EQ(control.render_block(inputs, output, {resume_at, 3}),
             engine::RoutedRenderStatus::rendered, "instrument post-stop render failed");
  near(output[0], 0.0F);
  require(!control.processor_fault(), "normal stop faulted the instrument");
}

// A MIDI edit on a VST3 instrument switches at the next block (ADR-0101):
// the held note the edit moves later is released at once and starts again at
// its new place, with no held or silent block beyond what the edit plays and
// no callback allocation. The revision is active without a restart.
void live_midi_edit_moves_the_held_note(const plugins::ScanResult& scanned, std::uint32_t frames) {
  // Where the moved note starts again, in the block after the edit.
  constexpr std::uint32_t restart_offset = 16;
  constexpr std::int64_t ticks_per_sample = timeline::ticks_per_quarter / 24'000;
  constexpr float held_level = 0.5F;
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto reopened = reopen(state, gate);
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  application::RoutedPlaybackControl control(reopened, {{1, 64}, frames, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "verified instrument route failed");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "instrument first routed callback failed");
  near(output[frames - 1], held_level);
  control.observe();

  auto moved = state.snapshot()->tracks[0].midi_clips.front().notes.front();
  const auto length = moved.end.value - moved.start.value;
  moved.start = {static_cast<std::int64_t>(frames + restart_offset) * ticks_per_sample};
  moved.end = {moved.start.value + length};
  const auto changed = take(reopened.apply({project::SetMidiNote{id<project::ClipId>(4), moved}}));
  REQUIRE_OK(control.begin_edit(changed, "move note"),
             "a VST3 MIDI edit was refused while playing");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = control.render_block(inputs, output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered, "the MIDI edit's block failed");
  REQUIRE_EQ(counted_allocations, 0, "the MIDI edit's block allocated");
  for (std::uint32_t frame = 0; frame < restart_offset; ++frame)
    near(output[frame], 0.0F);
  for (std::uint32_t frame = restart_offset; frame < frames; ++frame)
    near(output[frame], held_level);
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "the MIDI edit did not become active without a restart");
  REQUIRE_EQ(control.status().application, application::PlaybackApplication::active,
             "the MIDI edit did not become active without a restart");
  require(!control.processor_fault(), "the MIDI edit faulted the instrument");
}

// A VST3 instrument removed while playing switches with a replacement
// source (ADR-0098): the old instrument drains beside the new graph and
// releases its held note on the switching block, then plays nothing more
// (ADR-0101).
void live_instrument_removal_releases_its_note(const plugins::ScanResult& scanned,
                                               std::uint32_t frames) {
  constexpr float held_level = 0.5F;
  // The fixture's release: its level falls by an eighth a frame.
  constexpr std::uint32_t tail_frames = 8;
  // Blocks rendered after the removal.
  constexpr std::uint64_t drained_blocks = 4;
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set the instrument tail fixture");
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto reopened = reopen(state, gate);
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  application::RoutedPlaybackControl control(reopened, {{1, 64}, frames, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "verified instrument route failed");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "instrument first routed callback failed");
  near(output[frames - 1], held_level);
  control.observe();

  const auto removed =
      take(reopened.apply({project::RemoveVst3Plugin{id<project::PluginInstanceId>(5)}}));
  require(control.can_capture_source_edit(*removed.snapshot),
          "a VST3 instrument removal is not offered as a source switch");
  const auto capture = take(control.capture_source_edit(removed, "remove instrument"));
  REQUIRE_OK(control.publish_captured_source(capture), "the instrument removal did not publish");
  for (std::uint64_t block = 1; block <= drained_blocks; ++block) {
    counted_allocations = 0;
    counting_allocations = true;
    const auto rendered = control.render_block(inputs, output, {block * frames, 1});
    counting_allocations = false;
    REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered, "a block after the removal failed");
    REQUIRE_EQ(counted_allocations, 0, "a block after the removal allocated");
    // The switching block releases the note: its tail, then silence.
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      const auto remaining = block == 1 && frame < tail_frames ? tail_frames - frame : 0U;
      near(output[frame],
           held_level * static_cast<float>(remaining) / static_cast<float>(tail_frames));
    }
    control.observe();
  }
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear the instrument tail fixture");
  REQUIRE_EQ(control.status().active_graph_revision, removed.snapshot->revision,
             "the instrument removal did not become active without a restart");
  require(!control.processor_fault(), "the removed instrument faulted");
}

void two_prepared_instruments_keep_independent_owners(const plugins::ScanResult& scanned,
                                                      std::uint32_t frames) {
  Gate gate;
  auto first = saved(scanned);
  auto second = first;
  second.id = id<project::PluginInstanceId>(15);
  const float second_level = 0.25F;
  std::memcpy(second.component_state.data(), &second_level, sizeof(second_level));
  auto state = make_model(gate, first);
  auto second_clip = state.snapshot()->tracks[0].midi_clips.front();
  second_clip.id = id<project::ClipId>(14);
  second_clip.notes.front().id = id<project::NoteId>(16);
  second_clip.notes.front().start = {400'000}; // Ten samples at 48 kHz, 120 BPM.
  const auto second_track = id<project::TrackId>(12);
  // Placed last, after the fixture's bus, so its row stays fixed.
  (void)take(state.apply({project::AddTrack{second_track, project::TrackKind::instrument, 2},
                          project::AddMidiClip{second_track, second_clip},
                          project::InsertVst3Plugin{second_track, 0, second}}));
  TemporaryProject folder;
  auto writer = take(persistence::ProjectStore::open_writer(folder.path, true));
  REQUIRE_EQ(take(writer.save(*state.snapshot(), persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "two-instrument save did not commit");
  auto reader = take(persistence::ProjectStore::open_reader(folder.path));
  auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.tracks.size(), 3,
             "two instrument source identities or MIDI did not reopen");
  REQUIRE_EQ(saved_instrument(loaded.snapshot.tracks[0]), first,
             "two instrument source identities or MIDI did not reopen");
  REQUIRE_EQ(saved_instrument(loaded.snapshot.tracks[2]), second,
             "two instrument source identities or MIDI did not reopen");
  REQUIRE_EQ(loaded.snapshot.tracks[2].midi_clips.front(), second_clip,
             "two instrument source identities or MIDI did not reopen");
  auto snapshot = std::make_shared<project::Snapshot>(std::move(loaded.snapshot));
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(first.id, scanned);
  provider.set_scan_receipt(second.id, scanned);
  auto first_source = take(provider.prepare(*snapshot, first, frames));
  auto second_source = take(provider.prepare(*snapshot, second, frames));
  const auto first_track = id<project::TrackId>(2);
  REQUIRE_TRUE(first_source.processor,
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(second_source.processor,
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_NE(first_source.processor, second_source.processor,
             "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(first_source.instrument_source,
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(second_source.instrument_source,
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(!provider.manual_events(),
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(provider.manual_events(first_track, first.id),
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(provider.manual_events(second_track, second.id),
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_NE(provider.manual_events(first_track, first.id),
             provider.manual_events(second_track, second.id),
             "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(!provider.manual_events(first_track, second.id),
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(!provider.manual_events(second_track, first.id),
               "two source preparations aliased a host, manual slot or saved identity");
  REQUIRE_TRUE(!provider.manual_events(first_track, id<project::PluginInstanceId>(99)),
               "two source preparations aliased a host, manual slot or saved identity");
  auto wrong_second = second;
  wrong_second.name = "DifferentSavedClass";
  REQUIRE_TRUE(!provider.prepare(*snapshot, wrong_second, frames),
               "changed source identity or cross-track export receipt was accepted");
  REQUIRE_TRUE(!provider.prepared_export_end(first_track, second.id),
               "changed source identity or cross-track export receipt was accepted");
  REQUIRE_TRUE(!provider.prepared_export_end(second_track, first.id),
               "changed source identity or cross-track export receipt was accepted");
  std::vector<engine::StereoFrame> first_output(frames), second_output(frames);
  counted_allocations = 0;
  counting_allocations = true;
  first_source.processor->process(first_output, {0, 1});
  second_source.processor->process(second_output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "two prepared instrument callbacks failed or allocated");
  REQUIRE_TRUE(!first_source.process_failed(),
               "two prepared instrument callbacks failed or allocated");
  REQUIRE_TRUE(!second_source.process_failed(),
               "two prepared instrument callbacks failed or allocated");
  near(first_output[4], 0.0F);
  near(first_output[5], 0.5F);
  near(second_output[9], 0.0F);
  near(second_output[10], 0.25F);
  near({first_output[10].left + second_output[10].left,
        first_output[10].right + second_output[10].right},
       0.75F);
  // Manual admission belongs to the saved instance. A qualified first host
  // must not make the second host editable merely because both use a fixture.
  plugins::Vst3InstrumentTrackProvider isolated;
  isolated.set_scan_receipt(first.id, scanned);
  isolated.set_scan_receipt(second.id, scanned);
  isolated.set_manual_qualification(first.id, true, {1});
  isolated.set_manual_qualification(second.id, false);
  const auto qualified_first = take(isolated.prepare(*snapshot, first, frames));
  const auto unqualified_second = take(isolated.prepare(*snapshot, second, frames));
  REQUIRE_TRUE(qualified_first.manual_events,
               "manual qualification leaked from one VST3 instrument source to another");
  REQUIRE_TRUE(!unqualified_second.manual_events,
               "manual qualification leaked from one VST3 instrument source to another");
  REQUIRE_NE(qualified_first.processor, unqualified_second.processor,
             "manual qualification leaked from one VST3 instrument source to another");
  REQUIRE_TRUE(!qualified_first.fixed_tail_qualified,
               "manual qualification leaked from one VST3 instrument source to another");
  REQUIRE_TRUE(!unqualified_second.fixed_tail_qualified,
               "manual qualification leaked from one VST3 instrument source to another");
  // One failed callback must retire only that runtime's manual state and
  // leave the other fixture's MIDI schedule and host able to render.
  first_source.processor->process(first_output, {frames, 0});
  REQUIRE_TRUE(first_source.process_failed(), "source fault escaped its exact instance");
  REQUIRE_TRUE(!second_source.process_failed(), "source fault escaped its exact instance");
  REQUIRE_TRUE(!provider.stop_after_join(first_track, first.id),
               "source fault escaped its exact instance");
  REQUIRE_TRUE(!provider.stop_after_join(first_track, second.id),
               "source fault escaped its exact instance");
  counted_allocations = 0;
  counting_allocations = true;
  second_source.processor->process(second_output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "healthy source was lost with a faulty peer or ambiguous export was admitted");
  REQUIRE_TRUE(!second_source.process_failed(),
               "healthy source was lost with a faulty peer or ambiguous export was admitted");
  REQUIRE_TRUE(provider.stop_after_join(second_track, second.id),
               "healthy source was lost with a faulty peer or ambiguous export was admitted");
  REQUIRE_TRUE(!provider.stop_after_join(),
               "healthy source was lost with a faulty peer or ambiguous export was admitted");
  REQUIRE_TRUE(!provider.prepared_export_end(),
               "healthy source was lost with a faulty peer or ambiguous export was admitted");
  REQUIRE_TRUE(!provider.prepare_export_end(70),
               "healthy source was lost with a faulty peer or ambiguous export was admitted");
}

void paired_instrument_effects_have_any_shape(const plugins::ScanResult& scanned,
                                              std::uint32_t frames) {
  Gate gate;
  const auto first_track = id<project::TrackId>(2);
  const auto second_track = id<project::TrackId>(52);
  auto first = saved(scanned);
  auto second = first;
  second.id = id<project::PluginInstanceId>(53);
  auto first_effect = first;
  first_effect.id = id<project::PluginInstanceId>(54);
  auto second_effect = first;
  second_effect.id = id<project::PluginInstanceId>(55);
  auto state = make_model(gate, first);
  // Placed last, after the fixture's bus, so its row stays fixed.
  (void)take(state.apply({project::AddTrack{second_track, project::TrackKind::instrument, 2},
                          project::InsertVst3Plugin{second_track, 0, second},
                          project::InsertVst3Plugin{first_track, 0, first_effect},
                          project::InsertVst3Plugin{second_track, 0, second_effect}}));
  auto snapshot = state.snapshot();
  REQUIRE_EQ(snapshot->tracks[0].effects.size(), 1,
             "paired instrument effect fixture did not retain two source chains");
  REQUIRE_EQ(snapshot->tracks[2].effects.size(), 1,
             "paired instrument effect fixture did not retain two source chains");
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(first.id, scanned);
  provider.set_scan_receipt(second.id, scanned);
  auto prepared_first = take(provider.prepare(*snapshot, first, frames));
  auto prepared_second = take(provider.prepare(*snapshot, second, frames));
  REQUIRE_TRUE(prepared_first.instrument_source,
               "two one-effect instrument sources did not prepare independently");
  REQUIRE_TRUE(prepared_second.instrument_source,
               "two one-effect instrument sources did not prepare independently");
  REQUIRE_NE(prepared_first.processor, prepared_second.processor,
             "two one-effect instrument sources did not prepare independently");
  REQUIRE_TRUE(provider.stop_after_join(first_track, first.id),
               "two one-effect instrument sources did not prepare independently");
  REQUIRE_TRUE(provider.stop_after_join(second_track, second.id),
               "two one-effect instrument sources did not prepare independently");

  // Any chain shape is admitted (ADR-0050): the instrument prepares whatever
  // its own and its neighbours' chains hold.
  const auto admitted = [&](project::Snapshot candidate) {
    plugins::Vst3InstrumentTrackProvider probe;
    probe.set_scan_receipt(first.id, scanned);
    return probe.prepare(candidate, first, frames).has_value() &&
           probe.stop_after_join(first_track, first.id);
  };
  auto asymmetric = *snapshot;
  asymmetric.tracks[2].effects.clear();
  require(admitted(std::move(asymmetric)), "two instruments with only one effect were refused");
  auto extra_path = *snapshot;
  extra_path.tracks[1].effects.push_back(
      {id<project::InsertSlotId>(56), true, project::NativeEffectPatch{"expanse"}});
  require(admitted(std::move(extra_path)),
          "separate native effect beside two instrument chains was refused");
  auto second_effect_on_first = *snapshot;
  second_effect_on_first.tracks[0].effects.push_back(
      {id<project::InsertSlotId>(57), true, second_effect});
  second_effect_on_first.tracks[2].effects.clear();
  require(admitted(std::move(second_effect_on_first)),
          "two effects on only one source were refused");
}

void planned_loop_keeps_instrument_tail(const plugins::ScanResult& scanned, std::uint32_t frames) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, frames, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "loop instrument route did not publish");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot enable loop tail fixture");
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "instrument did not render before loop wrap");
  near(output[frames - 1], 0.5F);
  counting_allocations = true;
  const auto wrapped = control.render_block(inputs, output, {0, 2, 1, true});
  counting_allocations = false;
  REQUIRE_EQ(wrapped, engine::RoutedRenderStatus::rendered,
             "planned loop wrap failed or allocated in the callback");
  REQUIRE_EQ(counted_allocations, 0, "planned loop wrap failed or allocated in the callback");
  near(output[0], 0.5F);
  near(output[4], 0.25F);
  near(output[5], 0.5F);
  require(!control.processor_fault(), "planned loop wrap faulted the instrument");
  require(provider.stop_after_join(), "loop instrument stop did not release notes");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0, "cannot clear loop tail fixture");
}

plugins::Vst3InstrumentTailQualification tail_evidence(const plugins::ScanResult& scanned,
                                                       const project::Vst3PluginInstance& saved) {
  return {.binary_sha256 = scanned.binary.sha256,
          .binary_byte_size = scanned.binary.byte_size,
          .class_id = saved.class_id,
          .vendor = saved.vendor,
          .name = saved.name,
          .version = saved.version,
          .sample_rate = oscillate::test::sample_rate_48k,
          .declared_tail_frames = 8,
          .component_state = saved.component_state,
          .parameters = saved.parameters,
          .note_off_release_tested_64_frames = true,
          .note_off_release_tested_128_frames = true};
}

void fixed_instrument_tail_evidence(const plugins::ScanResult& scanned, std::uint32_t frames) {
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot enable finite instrument release fixture");
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  const auto& plugin = saved_instrument(state.snapshot()->tracks[0]);
  auto evidence = tail_evidence(scanned, plugin);
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(plugin.id, scanned);
  require(!take(provider.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "instrument tail declaration alone qualified the release");
  provider.set_tail_qualification(plugin.id, evidence);
  auto prepared = take(provider.prepare(*state.snapshot(), plugin, frames));
  REQUIRE_TRUE(prepared.instrument_source,
               "exact note-off release evidence did not qualify the instrument tail");
  REQUIRE_TRUE(prepared.fixed_tail_qualified,
               "exact note-off release evidence did not qualify the instrument tail");
  REQUIRE_EQ(prepared.finite_tail_frames, 8,
             "exact note-off release evidence did not qualify the instrument tail");
  REQUIRE_EQ(prepared.latency_frames, 0,
             "exact note-off release evidence did not qualify the instrument tail");
  std::vector<engine::StereoFrame> block(frames);
  for (std::uint64_t position = 0; position <= 320; position += frames) {
    counted_allocations = 0;
    counting_allocations = true;
    prepared.processor->process(block, {position, 1});
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "qualified instrument callback failed or allocated");
    REQUIRE_TRUE(!prepared.process_failed(), "qualified instrument callback failed or allocated");
    if (position == 256) {
      REQUIRE_LT(std::fabs(block[0].left - 0.5F), tolerance::float_round_trip_f,
                 "note-off release exceeded its eight-frame declaration");
      REQUIRE_LT(std::fabs(block[0].right + 0.5F), tolerance::float_round_trip_f,
                 "note-off release exceeded its eight-frame declaration");
      REQUIRE_LT(std::fabs(block[7].left - 0.0625F), tolerance::float_round_trip_f,
                 "note-off release exceeded its eight-frame declaration");
      REQUIRE_LT(std::fabs(block[7].right + 0.0625F), tolerance::float_round_trip_f,
                 "note-off release exceeded its eight-frame declaration");
      REQUIRE_EQ(block[8].left, 0.0F, "note-off release exceeded its eight-frame declaration");
      REQUIRE_EQ(block[8].right, 0.0F, "note-off release exceeded its eight-frame declaration");
    }
  }
  REQUIRE_EQ(block.back().left, 0.0F, "instrument emitted a late tail after note-off drain");
  REQUIRE_EQ(block.back().right, 0.0F, "instrument emitted a late tail after note-off drain");
  require(provider.stop_after_join(), "qualified instrument did not stop");

  auto reopened = reopen(state, gate);
  plugins::Vst3InstrumentTrackProvider restored;
  restored.set_scan_receipt(plugin.id, scanned);
  restored.set_tail_qualification(plugin.id, evidence);
  require(take(restored.prepare(*reopened.snapshot(),
                                saved_instrument(reopened.snapshot()->tracks[0]), frames))
              .fixed_tail_qualified,
          "exact saved instrument state lost session-supplied tail evidence after reopen");
  auto changed_snapshot = std::make_shared<project::Snapshot>(*state.snapshot());
  auto& changed = saved_instrument(changed_snapshot->tracks[0]);
  changed.component_state[0] ^= 1;
  plugins::Vst3InstrumentTrackProvider changed_provider;
  changed_provider.set_scan_receipt(plugin.id, scanned);
  changed_provider.set_tail_qualification(plugin.id, evidence);
  require(!take(changed_provider.prepare(*changed_snapshot, changed, frames)).fixed_tail_qualified,
          "changed instrument state retained tail evidence");
  auto wrong_controller = evidence;
  wrong_controller.controller_state = std::vector<std::uint8_t>{};
  provider.set_tail_qualification(plugin.id, wrong_controller);
  require(!take(provider.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "controller-only state mismatch retained instrument tail qualification");
  provider.set_tail_qualification(plugin.id, evidence);
  auto wrong = evidence;
  wrong.binary_sha256[0] = wrong.binary_sha256[0] == '0' ? '1' : '0';
  provider.set_tail_qualification(plugin.id, wrong);
  require(!take(provider.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "changed binary identity retained instrument tail evidence");
  wrong = evidence;
  wrong.declared_tail_frames = 0;
  provider.set_tail_qualification(plugin.id, wrong);
  require(!take(provider.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "changed tail declaration retained instrument evidence");
  wrong = evidence;
  wrong.note_off_release_tested_128_frames = false;
  provider.set_tail_qualification(plugin.id, wrong);
  require(!take(provider.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "incomplete block-size evidence qualified instrument release");
  wrong = evidence;
  wrong.sample_rate = oscillate::test::sample_rate_44k;
  provider.set_tail_qualification(plugin.id, wrong);
  require(!take(provider.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "mismatched sample-rate evidence qualified instrument release");
  provider.set_tail_qualification(plugin.id, evidence);
  require(!take(provider.prepare(*state.snapshot(), plugin, 96)).fixed_tail_qualified,
          "untested callback size qualified instrument release");
  auto automated_snapshot = std::make_shared<project::Snapshot>(*state.snapshot());
  automated_snapshot->tracks[0].automation.push_back(
      {id<project::AutomationLaneId>(80),
       project::Vst3ParameterTarget{plugin.id, 1},
       {project::AutomationRepresentation::vst3_normalized, "level", "", 0.0, 1.0, 0.5, false, 0},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(81), 0, 0.5}},
       1});
  plugins::Vst3InstrumentTrackProvider automated;
  automated.set_scan_receipt(plugin.id, scanned);
  automated.set_tail_qualification(plugin.id, evidence);
  require(!take(automated.prepare(*automated_snapshot,
                                  saved_instrument(automated_snapshot->tracks[0]), frames))
               .fixed_tail_qualified,
          "saved automation retained fixed instrument-tail qualification");
  plugins::Vst3InstrumentTrackProvider manual(plugins::Vst3ProcessingMode::realtime, true);
  manual.set_scan_receipt(plugin.id, scanned);
  manual.set_tail_qualification(plugin.id, evidence);
  require(!take(manual.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "admitted manual editing retained fixed instrument-tail qualification");
  plugins::Vst3InstrumentTrackProvider offline(plugins::Vst3ProcessingMode::offline);
  offline.set_scan_receipt(plugin.id, scanned);
  offline.set_tail_qualification(plugin.id, evidence);
  require(!take(offline.prepare(*state.snapshot(), plugin, frames)).fixed_tail_qualified,
          "real-time release evidence qualified a different host mode");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "infinite", 1), 0,
             "cannot enable unknown instrument tail fixture");
  auto unknown = take(provider.prepare(*state.snapshot(), plugin, frames));
  REQUIRE_TRUE(!unknown.finite_tail_frames,
               "unknown instrument tail was treated as finite release evidence");
  REQUIRE_TRUE(!unknown.fixed_tail_qualified,
               "unknown instrument tail was treated as finite release evidence");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear finite instrument release fixture");
}

void prepared_export_end(const plugins::ScanResult& scanned, std::uint32_t frames) {
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot enable finite instrument release fixture");
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto snapshot = std::make_shared<project::Snapshot>(*state.snapshot());
  // Notes at and after the chosen source end must never enter the host,
  // even while the first note's qualified release is rendered.
  snapshot->tracks[0].midi_clips[0].notes.push_back(
      {id<project::NoteId>(7), {2'800'000}, {4'000'000}, 60, 127});
  snapshot->tracks[0].midi_clips[0].notes.push_back(
      {id<project::NoteId>(8), {4'800'000}, {6'400'000}, 60, 127});
  const auto& plugin = saved_instrument(snapshot->tracks[0]);
  plugins::Vst3InstrumentTrackProvider unqualified;
  unqualified.set_scan_receipt(plugin.id, scanned);
  auto unqualified_source = take(unqualified.prepare(*snapshot, plugin, frames));
  require(!unqualified_source.fixed_tail_qualified,
          "fixture without release evidence unexpectedly qualified its tail");
  REQUIRE_TRUE(!unqualified.prepare_export_end(70),
               "tail declaration alone admitted an export MIDI cutoff");
  REQUIRE_TRUE(!unqualified.prepared_export_end(),
               "tail declaration alone admitted an export MIDI cutoff");
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(plugin.id, scanned);
  provider.set_tail_qualification(plugin.id, tail_evidence(scanned, plugin));
  auto prepared = take(provider.prepare(*snapshot, plugin, frames));
  REQUIRE_TRUE(prepared.fixed_tail_qualified,
               "export end failed exact, one-shot or release-horizon admission");
  REQUIRE_TRUE(!provider.prepare_export_end(0),
               "export end failed exact, one-shot or release-horizon admission");
  REQUIRE_TRUE(!provider.prepare_export_end((std::uint64_t{1} << 40U) - 7),
               "export end failed exact, one-shot or release-horizon admission");
  REQUIRE_EQ(take(provider.prepare_export_end(70)), 70,
             "export end failed exact, one-shot or release-horizon admission");
  REQUIRE_EQ(provider.prepared_export_end(), 70,
             "export end failed exact, one-shot or release-horizon admission");
  REQUIRE_TRUE(!provider.prepare_export_end(71),
               "export end failed exact, one-shot or release-horizon admission");
  std::vector<engine::StereoFrame> rendered(256);
  for (std::size_t position = 0; position < rendered.size(); position += frames) {
    counted_allocations = 0;
    counting_allocations = true;
    prepared.processor->process(std::span(rendered).subspan(position, frames), {position, 1});
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "prepared VST3 export callback allocated or failed");
    REQUIRE_TRUE(!prepared.process_failed(), "prepared VST3 export callback allocated or failed");
  }
  REQUIRE_EQ(rendered[5].left, 0.5F, "exact note-off release or post-end note suppression failed");
  REQUIRE_EQ(rendered[69].left, 0.5F, "exact note-off release or post-end note suppression failed");
  REQUIRE_EQ(rendered[70].left, 0.5F, "exact note-off release or post-end note suppression failed");
  REQUIRE_EQ(rendered[77].left, 0.0625F,
             "exact note-off release or post-end note suppression failed");
  REQUIRE_EQ(rendered[78].left, 0.0F, "exact note-off release or post-end note suppression failed");
  REQUIRE_EQ(rendered[120].left, 0.0F,
             "exact note-off release or post-end note suppression failed");
  REQUIRE_EQ(rendered[150].left, 0.0F,
             "exact note-off release or post-end note suppression failed");
  REQUIRE_EQ(provider.prepared_export_end(), 70,
             "running instrument accepted a new export end or lost its receipt");
  REQUIRE_TRUE(!provider.prepare_export_end(200),
               "running instrument accepted a new export end or lost its receipt");
  REQUIRE_TRUE(provider.stop_after_join(),
               "running instrument accepted a new export end or lost its receipt");

  plugins::Vst3InstrumentTrackProvider started;
  started.set_scan_receipt(plugin.id, scanned);
  started.set_tail_qualification(plugin.id, tail_evidence(scanned, plugin));
  auto started_source = take(started.prepare(*snapshot, plugin, frames));
  std::vector<engine::StereoFrame> first(frames);
  started_source.processor->process(first, {0, 1});
  REQUIRE_TRUE(!started.prepare_export_end(70),
               "instrument accepted cutoff after its first callback");
  REQUIRE_TRUE(!started.prepared_export_end(),
               "instrument accepted cutoff after its first callback");
  require(started.stop_after_join(), "started instrument did not stop");

  auto automated_snapshot = std::make_shared<project::Snapshot>(*snapshot);
  automated_snapshot->tracks[0].automation.push_back(
      {id<project::AutomationLaneId>(80),
       project::Vst3ParameterTarget{plugin.id, 1},
       {project::AutomationRepresentation::vst3_normalized, "level", "", 0.0, 1.0, 0.5, false, 0},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(81), 0, 0.5}},
       1});
  plugins::Vst3InstrumentTrackProvider automated;
  automated.set_scan_receipt(plugin.id, scanned);
  automated.set_tail_qualification(plugin.id, tail_evidence(scanned, plugin));
  auto automated_source = take(automated.prepare(
      *automated_snapshot, saved_instrument(automated_snapshot->tracks[0]), frames));
  REQUIRE_TRUE(!automated_source.fixed_tail_qualified,
               "saved automation admitted a fixed export cutoff");
  REQUIRE_TRUE(!automated.prepare_export_end(70),
               "saved automation admitted a fixed export cutoff");
  plugins::Vst3InstrumentTrackProvider manual(plugins::Vst3ProcessingMode::realtime, true);
  manual.set_scan_receipt(plugin.id, scanned);
  manual.set_tail_qualification(plugin.id, tail_evidence(scanned, plugin));
  auto manual_source = take(manual.prepare(*snapshot, plugin, frames));
  REQUIRE_TRUE(!manual_source.fixed_tail_qualified,
               "admitted manual mutability retained export cutoff qualification");
  REQUIRE_TRUE(!manual.prepare_export_end(70),
               "admitted manual mutability retained export cutoff qualification");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "infinite", 1), 0,
             "cannot enable unknown instrument tail fixture");
  plugins::Vst3InstrumentTrackProvider unknown;
  unknown.set_scan_receipt(plugin.id, scanned);
  unknown.set_tail_qualification(plugin.id, tail_evidence(scanned, plugin));
  auto unknown_source = take(unknown.prepare(*snapshot, plugin, frames));
  REQUIRE_TRUE(!unknown_source.finite_tail_frames,
               "unknown instrument tail admitted an export cutoff");
  REQUIRE_TRUE(!unknown.prepare_export_end(70),
               "unknown instrument tail admitted an export cutoff");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear finite instrument release fixture");
}

void export_end_event_capacity(const plugins::ScanResult& scanned) {
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot enable finite instrument release fixture");
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto snapshot = std::make_shared<project::Snapshot>(*state.snapshot());
  auto& clip = snapshot->tracks[0].midi_clips[0];
  clip.placement = project::MusicalInterval{{0}, {4 * timeline::ticks_per_quarter}};
  clip.pattern_length = {4 * timeline::ticks_per_quarter};
  clip.notes.clear();
  // One fewer than the most notes that may sound at once are held at the
  // cut; a short note ending, then another starting and ending, within one
  // block before it add three events, so the cut's block holds 1026.
  constexpr std::int64_t ticks_per_sample = 40'000;
  constexpr std::int64_t cut_sample = 20'000;
  constexpr std::int64_t first_short_start = 19'800;
  constexpr std::int64_t first_short_end = 19'900;
  constexpr std::int64_t second_short_start = 19'950;
  constexpr std::int64_t second_short_end = 19'990;
  std::uint64_t index = 0;
  for (; index + 1 < engine::max_concurrent_midi_notes; ++index) {
    const auto start = static_cast<std::int64_t>(5 + index * 16) * ticks_per_sample;
    const auto end = static_cast<std::int64_t>(32'000 + index * 16) * ticks_per_sample;
    clip.notes.push_back({id<project::NoteId>(1'000 + index), {start}, {end}, 60, 127});
  }
  for (const auto& [start, end] : {std::pair{first_short_start, first_short_end},
                                   std::pair{second_short_start, second_short_end}})
    clip.notes.push_back({id<project::NoteId>(1'000 + index++),
                          {start * ticks_per_sample},
                          {end * ticks_per_sample},
                          60,
                          127});
  const auto& plugin = saved_instrument(snapshot->tracks[0]);
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(plugin.id, scanned);
  provider.set_tail_qualification(plugin.id, tail_evidence(scanned, plugin));
  auto prepared = take(provider.prepare(*snapshot, plugin, 128));
  const auto rejected = provider.prepare_export_end(cut_sample);
  REQUIRE_TRUE(prepared.fixed_tail_qualified,
               "cutoff did not reject more than 1024 events in its block transactionally");
  REQUIRE_TRUE(!rejected,
               "cutoff did not reject more than 1024 events in its block transactionally");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::resource_exhausted,
             "cutoff did not reject more than 1024 events in its block transactionally");
  REQUIRE_TRUE(!provider.prepared_export_end(),
               "cutoff did not reject more than 1024 events in its block transactionally");
  REQUIRE_EQ(take(provider.prepare_export_end(1'000)), 1'000,
             "failed cutoff changed the private MIDI schedule or consumed the fresh runtime");
  REQUIRE_EQ(provider.prepared_export_end(), 1'000,
             "failed cutoff changed the private MIDI schedule or consumed the fresh runtime");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear finite instrument release fixture");
}

void rejection_and_fault(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  routing::ProjectRouteIdRegistry registry;
  require(!registry.compile(*state.snapshot(), 64),
          "unbound saved instrument was admitted into the route plan");
  const routing::BoundStage effect_token{.track = id<project::TrackId>(2),
                                         .slot = id<project::InsertSlotId>(5),
                                         .processor = routing::StageProcessor::vst3,
                                         .plugin = id<project::PluginInstanceId>(5)};
  require(!registry.compile(*state.snapshot(), 64, std::span(&effect_token, 1)),
          "effect token was accepted for a saved instrument source");
  const routing::BoundStage instrument_token{.track = id<project::TrackId>(2),
                                             .processor = routing::StageProcessor::vst3,
                                             .plugin = id<project::PluginInstanceId>(5)};
  REQUIRE_OK(registry.compile(*state.snapshot(), 64, std::span(&instrument_token, 1)),
             "exact instrument token was refused for its saved instrument source");
  auto mismatched = *state.snapshot();
  mismatched.tracks[0].kind = project::TrackKind::audio;
  mismatched.tracks[0].midi_clips.clear();
  require(!registry.compile(mismatched, 64, std::span(&instrument_token, 1)),
          "instrument token was accepted for a saved audio effect track");
  IncompleteProvider incomplete;
  application::RoutedPlaybackControl without_runtime(state, {{1, 64}, 64, 4'096, 4'096},
                                                     &incomplete);
  REQUIRE_TRUE(!without_runtime.submit_current(),
               "incomplete instrument provider produced an active route");
  REQUIRE_EQ(without_runtime.status().active_graph_revision, 0,
             "incomplete instrument provider produced an active route");
  WrongKindProvider wrong_kind;
  application::RoutedPlaybackControl wrong_source(state, {{1, 64}, 64, 4'096, 4'096}, &wrong_kind);
  REQUIRE_TRUE(!wrong_source.submit_current(),
               "effect-only provider was accepted as a saved instrument source");
  REQUIRE_EQ(wrong_source.status().active_graph_revision, 0,
             "effect-only provider was accepted as a saved instrument source");
  plugins::Vst3InstrumentTrackProvider provider;
  application::RoutedPlaybackControl unverified(state, {{1, 64}, 64, 4'096, 4'096}, &provider);
  REQUIRE_TRUE(!unverified.submit_current(), "unscanned instrument entered the graph");
  REQUIRE_EQ(unverified.status().active_graph_revision, 0,
             "unscanned instrument entered the graph");
  auto wrong = scanned;
  wrong.binary.sha256[0] = wrong.binary.sha256[0] == 'a' ? 'b' : 'a';
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), wrong);
  require(!unverified.submit_current(), "changed instrument binary receipt was accepted");
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  REQUIRE_OK(unverified.submit_current(), "recovered instrument scan was rejected");
  std::vector<std::span<const engine::StereoFrame>> inputs(unverified.topological_order().size());
  std::array<engine::StereoFrame, 64> output{};
  REQUIRE_EQ(setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable instrument process failure fixture");
  counting_allocations = true;
  const auto failed = unverified.render_block(inputs, output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
             "cannot disable instrument process failure fixture");
  REQUIRE_EQ(failed, engine::RoutedRenderStatus::rendered,
             "instrument process failure did not mute and report the route");
  REQUIRE_EQ(counted_allocations, 0,
             "instrument process failure did not mute and report the route");
  REQUIRE_EQ(output[5].left, 0.0F, "instrument process failure did not mute and report the route");
  REQUIRE_OK(unverified.processor_fault(),
             "instrument process failure did not mute and report the route");
  REQUIRE_EQ(unverified.render_block(inputs, output, {64, 1}), engine::RoutedRenderStatus::rendered,
             "faulted instrument executed again instead of staying muted");
  REQUIRE_EQ(output[0].left, 0.0F, "faulted instrument executed again instead of staying muted");

  auto altered = *state.snapshot();
  plugins::Vst3InstrumentTrackProvider invalid_format;
  invalid_format.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  REQUIRE_TRUE(!invalid_format.prepare(*state.snapshot(), saved_instrument(altered.tracks[0]), 0),
               "unsupported instrument processing format was accepted");
  REQUIRE_TRUE(!invalid_format.prepare(*state.snapshot(), saved_instrument(altered.tracks[0]), 256),
               "unsupported instrument processing format was accepted");
  plugins::Vst3InstrumentTrackProvider invalid_callback;
  invalid_callback.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto prepared =
      take(invalid_callback.prepare(*state.snapshot(), saved_instrument(altered.tracks[0]), 64));
  std::array<engine::StereoFrame, 64> invalid_frames{};
  invalid_frames.fill({7.0F, 7.0F});
  counting_allocations = true;
  prepared.processor->process(invalid_frames, {0, 0});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "invalid instrument callback was not bounded, muted and reported");
  REQUIRE_TRUE(prepared.process_failed(),
               "invalid instrument callback was not bounded, muted and reported");
  REQUIRE_EQ(invalid_frames[0].left, 0.0F,
             "invalid instrument callback was not bounded, muted and reported");
  saved_instrument(altered.tracks[0]).parameters.push_back({1, 0.25});
  plugins::Vst3InstrumentTrackProvider restored;
  restored.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto restored_state = restored.prepare(altered, saved_instrument(altered.tracks[0]), 64);
  REQUIRE_OK(restored_state,
             "opaque instrument state was rejected because a saved parameter reference is missing");
  altered = *state.snapshot();
  saved_instrument(altered.tracks[0]).component_state.clear();
  saved_instrument(altered.tracks[0]).parameters.push_back({2, 0.25});
  plugins::Vst3InstrumentTrackProvider unsupported;
  unsupported.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  require(!unsupported.prepare(altered, saved_instrument(altered.tracks[0]), 64),
          "unapplied static instrument parameter value was accepted");
  altered = *state.snapshot();
  altered.tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::linear_bpm},
       {{timeline::ticks_per_quarter}, 130.0, timeline::TempoShape::step}},
      2));
  plugins::Vst3InstrumentTrackProvider changing;
  changing.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  REQUIRE_OK(changing.prepare(altered, saved_instrument(altered.tracks[0]), 64),
             "instrument rejected a prepared gradual tempo map");
  altered = *state.snapshot();
  saved_instrument(altered.tracks[0]).name = "ChangedInstrument";
  plugins::Vst3InstrumentTrackProvider wrong_class;
  wrong_class.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  require(!wrong_class.prepare(altered, saved_instrument(altered.tracks[0]), 64),
          "changed VST3 class identity was accepted");
  altered = *state.snapshot();
  saved_instrument(altered.tracks[0]).component_state.resize(3);
  plugins::Vst3InstrumentTrackProvider bad_state;
  bad_state.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  require(!bad_state.prepare(altered, saved_instrument(altered.tracks[0]), 64),
          "malformed saved instrument state was applied");
  altered = *state.snapshot();
  altered.tracks[0].midi_clips[0].notes[0].key = 128;
  plugins::Vst3InstrumentTrackProvider bad_midi;
  bad_midi.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  require(!bad_midi.prepare(altered, saved_instrument(altered.tracks[0]), 64),
          "malformed saved MIDI reached a VST3 instrument");
  auto missing_binary = scanned;
  missing_binary.binary.canonical_bundle = "/missing/TimedInstrument.vst3";
  plugins::Vst3InstrumentTrackProvider missing;
  missing.set_scan_receipt(id<project::PluginInstanceId>(5), missing_binary);
  require(!missing.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks[0]), 64),
          "missing scanned instrument binary was accepted");
}

void instrument_parameter_delivery(const plugins::ScanResult& scanned, std::uint32_t frames) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  auto static_snapshot = *state.snapshot();
  saved_instrument(static_snapshot.tracks[0]).component_state.clear();
  saved_instrument(static_snapshot.tracks[0]).parameters.push_back({1, 0.25});
  plugins::Vst3InstrumentTrackProvider static_provider;
  static_provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto static_source = take(static_provider.prepare(
      static_snapshot, saved_instrument(static_snapshot.tracks[0]), frames));
  std::vector<engine::StereoFrame> output(frames);
  counting_allocations = true;
  static_source.processor->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "static instrument value failed or allocated during playback");
  REQUIRE_TRUE(!static_source.process_failed(),
               "static instrument value failed or allocated during playback");
  near(output[5], 0.25F);

  const project::AutomationLane lane{
      id<project::AutomationLaneId>(70),
      project::Vst3ParameterTarget{id<project::PluginInstanceId>(5), 1},
      {project::AutomationRepresentation::vst3_normalized, "level", "", 0.0, 1.0, 0.5, false, 0},
      project::AutomationTimebase::elapsed,
      {{id<project::AutomationPointId>(71), 0, 0.25, project::AutomationShape::linear},
       {id<project::AutomationPointId>(72), 16, 0.75}},
      1};
  (void)take(state.apply({project::AddAutomationLane{id<project::TrackId>(2), lane}}));
  plugins::Vst3InstrumentTrackProvider automated;
  automated.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(
      automated.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks[0]), frames));
  counting_allocations = true;
  source.processor->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "automated instrument callback failed or allocated");
  REQUIRE_TRUE(!source.process_failed(), "automated instrument callback failed or allocated");
  near(output[5], 0.40625F);
  near(output[16], 0.75F);
  plugins::Vst3InstrumentTrackProvider offline(plugins::Vst3ProcessingMode::offline);
  offline.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto offline_source = take(
      offline.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks[0]), frames));
  offline_source.processor->process(output, {0, 1});
  require(!offline_source.process_failed(), "offline instrument automation failed");
  near(output[5], 0.40625F);
  near(output[16], 0.75F);
}

// Any neighbouring chain shape leaves the instrument preparable (ADR-0050).
void separate_native_chain_preflight(const plugins::ScanResult& scanned) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  const auto admitted = [&](const project::Snapshot& candidate) {
    plugins::Vst3InstrumentTrackProvider provider;
    provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
    auto prepared = provider.prepare(candidate, saved_instrument(candidate.tracks[0]), 64);
    return prepared.has_value() && prepared.value().instrument_source && provider.stop_after_join();
  };
  for (const auto kind :
       {project::TrackKind::audio, project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    auto snapshot = *state.snapshot();
    auto& path = snapshot.tracks[1];
    path.kind = kind;
    path.effects = {{id<project::InsertSlotId>(20), true, project::NativeEffectPatch{"shape"}},
                    {id<project::InsertSlotId>(21), true, project::NativeEffectPatch{"press"}}};
    require(admitted(snapshot),
            "separate two-stage native path prevented verified MIDI source preparation");
    auto ninth = snapshot;
    for (std::uint64_t slot = 22; slot <= 28; ++slot)
      ninth.tracks[1].effects.push_back(
          {id<project::InsertSlotId>(slot), true, project::NativeEffectPatch{"expanse"}});
    require(admitted(ninth), "nine native stages on another path blocked the instrument");
    auto mixed = snapshot;
    auto second_plugin = saved(scanned);
    second_plugin.id = id<project::PluginInstanceId>(6);
    mixed.tracks[1].effects[1].processor = second_plugin;
    require(admitted(mixed), "a mixed native/VST3 path blocked the instrument");
    auto on_instrument = snapshot;
    on_instrument.tracks[0].effects.push_back(
        {id<project::InsertSlotId>(23), true, project::NativeEffectPatch{"expanse"}});
    require(admitted(on_instrument),
            "native chains on the instrument and a separate path were refused");
  }
}

void callback_baseline(const plugins::ScanResult& scanned, std::uint32_t frames) {
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, frames, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "benchmark route preparation failed");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  constexpr std::uint64_t blocks = 10'000;
  counted_allocations = 0;
  bool rendered = true;
  counting_allocations = true;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < blocks; ++index) {
    rendered &= control.render_block(inputs, output, {index * frames, 1}) ==
                engine::RoutedRenderStatus::rendered;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  counting_allocations = false;
  REQUIRE_TRUE(rendered, "routed instrument benchmark callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "routed instrument benchmark callback failed or allocated");
  REQUIRE_TRUE(!control.processor_fault(),
               "routed instrument benchmark callback failed or allocated");
  std::fprintf(
      stdout, "VST3 routed instrument %u frames: %lld ns/block, %zu allocations\n", frames,
      static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() /
                             static_cast<std::int64_t>(blocks)),
      counted_allocations);
  require(provider.stop_after_join(), "benchmark instrument did not stop");
}

void manual_instrument_event(const plugins::ScanResult& scanned, std::uint32_t frames) {
  using plugins::Vst3ManualEventPhase;
  using plugins::Vst3ManualEventStatus;
  Gate gate;
  auto instrument = saved(scanned);
  instrument.missing = false;
  auto state = make_model(gate, instrument);
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, frames, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "manual instrument graph did not prepare");
  auto slot = provider.manual_events();
  const auto track = id<project::TrackId>(2);
  const auto plugin = id<project::PluginInstanceId>(5);
  const auto first_revision = state.snapshot()->revision;
  REQUIRE_TRUE(slot, "prepared instrument admitted a manual event before its first callback");
  REQUIRE_EQ(slot->active_graph().graph_revision, 0,
             "prepared instrument admitted a manual event before its first callback");
  require(!control.active_vst3_instrument_manual_events(track, plugin, first_revision),
          "prepared instrument leaked a manual slot before callback acceptance");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<engine::StereoFrame> output(frames);
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "manual instrument first callback failed");
  const auto identity = slot->active_graph();
  REQUIRE_EQ(identity.graph_revision, state.snapshot()->revision,
             "instrument slot did not activate with graph identity");
  REQUIRE_NE(identity.instance_id, 0, "instrument slot did not activate with graph identity");
  REQUIRE_NE(identity.compatibility_key, 0, "instrument slot did not activate with graph identity");
  const auto binding = control.active_vst3_instrument_manual_events(track, plugin, first_revision);
  REQUIRE_TRUE(binding, "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_EQ(binding->slot, slot,
             "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_EQ(binding->identity.graph_revision, first_revision,
             "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_EQ(binding->identity.instance_id, identity.instance_id,
             "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_EQ(binding->identity.compatibility_key, identity.compatibility_key,
             "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_TRUE(!control.active_vst3_instrument_manual_events(id<project::TrackId>(3), plugin,
                                                             first_revision),
               "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_TRUE(!control.active_vst3_instrument_manual_events(
                   track, id<project::PluginInstanceId>(8), first_revision),
               "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_TRUE(!control.active_vst3_instrument_manual_events(track, plugin, first_revision + 1),
               "instrument lookup accepted an unbound owner, plugin, or graph revision");
  REQUIRE_EQ(slot->submit({.graph_revision = identity.graph_revision,
                           .instance_id = identity.instance_id + 1,
                           .compatibility_key = identity.compatibility_key},
                          2, {1, 0, 0.75}),
             Vst3ManualEventStatus::stale_instance,
             "instrument manual admission failed identity or bounded capacity checks");
  REQUIRE_EQ(slot->submit({.graph_revision = identity.graph_revision,
                           .instance_id = identity.instance_id,
                           .compatibility_key = identity.compatibility_key + 1},
                          2, {1, 0, 0.75}),
             Vst3ManualEventStatus::stale_instance,
             "instrument manual admission failed identity or bounded capacity checks");
  REQUIRE_EQ(slot->submit({.graph_revision = identity.graph_revision + 1,
                           .instance_id = identity.instance_id,
                           .compatibility_key = identity.compatibility_key},
                          2, {1, 0, 0.75}),
             Vst3ManualEventStatus::stale_instance,
             "instrument manual admission failed identity or bounded capacity checks");
  REQUIRE_EQ(slot->submit(identity, 2, {1, 0, 0.75}), Vst3ManualEventStatus::accepted,
             "instrument manual admission failed identity or bounded capacity checks");
  REQUIRE_EQ(slot->submit(identity, 3, {1, 0, 0.8}), Vst3ManualEventStatus::busy,
             "instrument manual admission failed identity or bounded capacity checks");
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered = control.render_block(inputs, output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
             "instrument manual callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "instrument manual callback failed or allocated");
  REQUIRE_TRUE(!control.processor_fault(), "instrument manual callback failed or allocated");
  near(output.front(), 0.75F);
  const auto receipt = slot->collect();
  REQUIRE_TRUE(receipt, "instrument manual event lacked first-block sample/revision receipt");
  REQUIRE_EQ(receipt->phase, Vst3ManualEventPhase::applied,
             "instrument manual event lacked first-block sample/revision receipt");
  REQUIRE_EQ(receipt->revision, 2,
             "instrument manual event lacked first-block sample/revision receipt");
  REQUIRE_EQ(receipt->project_sample, frames,
             "instrument manual event lacked first-block sample/revision receipt");
  REQUIRE_EQ(slot->submit(identity, 2, {1, 0, 0.5}), Vst3ManualEventStatus::invalid_revision,
             "instrument manual slot reused an acknowledged revision");
  auto revised = take(state.apply({project::AddSendRoute{
      id<project::TrackId>(2), {id<project::SendId>(7), id<project::TrackId>(3), 0.5}}}));
  // The route edit switches live; until the callback adopts it, neither
  // revision exposes a manual binding.
  require(
      control.begin_edit(revised, "add instrument send").has_value() &&
          !control.active_vst3_instrument_manual_events(track, plugin, first_revision) &&
          !control.active_vst3_instrument_manual_events(track, plugin, revised.snapshot->revision),
      "instrument topology edit did not publish");
  REQUIRE_EQ(slot->submit(identity, 3, {1, 0, 0.5}), Vst3ManualEventStatus::accepted,
             "old graph stopped admitting before accepted callback switch");
  REQUIRE_EQ(control.render_block(inputs, output, {2 * frames, 1}),
             engine::RoutedRenderStatus::rendered,
             "reused instrument did not switch manual identity at callback boundary");
  REQUIRE_EQ(slot->active_graph().graph_revision, revised.snapshot->revision,
             "reused instrument did not switch manual identity at callback boundary");
  const auto successor =
      control.active_vst3_instrument_manual_events(track, plugin, revised.snapshot->revision);
  REQUIRE_TRUE(successor,
               "topology edit lost the instrument slot or retained its old graph identity");
  REQUIRE_EQ(successor->slot, slot,
             "topology edit lost the instrument slot or retained its old graph identity");
  REQUIRE_EQ(successor->identity.instance_id, identity.instance_id,
             "topology edit lost the instrument slot or retained its old graph identity");
  REQUIRE_TRUE(!control.active_vst3_instrument_manual_events(track, plugin, first_revision),
               "topology edit lost the instrument slot or retained its old graph identity");
  const auto failed = slot->collect();
  REQUIRE_TRUE(failed, "instrument accepted a pending event from its retired graph revision");
  REQUIRE_EQ(failed->phase, Vst3ManualEventPhase::failed,
             "instrument accepted a pending event from its retired graph revision");
  REQUIRE_EQ(failed->revision, 3,
             "instrument accepted a pending event from its retired graph revision");
  REQUIRE_EQ(slot->submit(identity, 4, {1, 0, 0.5}), Vst3ManualEventStatus::stale_instance,
             "instrument accepted a pending event from its retired graph revision");
  auto changed = take(state.apply({project::SetVst3ComponentState{plugin, {1, 2, 3}}}));
  REQUIRE_GT(changed.snapshot->revision, revised.snapshot->revision,
             "saved instrument state advanced but stale live slot remained available");
  REQUIRE_TRUE(
      !control.active_vst3_instrument_manual_events(track, plugin, revised.snapshot->revision),
      "saved instrument state advanced but stale live slot remained available");
  REQUIRE_TRUE(provider.stop_after_join(), "stopped instrument kept its manual event slot active");
  REQUIRE_EQ(slot->active_graph().graph_revision, 0,
             "stopped instrument kept its manual event slot active");
  require(!control.active_vst3_instrument_manual_events(track, plugin, revised.snapshot->revision),
          "retired instrument remained available for manual editing");
}

void instrument_control_overlay_rebinds_manual_slot(const plugins::ScanResult& scanned,
                                                    std::uint32_t frames) {
  Gate gate;
  auto instrument = saved(scanned);
  instrument.missing = false;
  auto state = make_model(gate, instrument);
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(provider.prepare(*state.snapshot(), instrument, frames));
  REQUIRE_TRUE(source.processor, "instrument overlay fixture lacked its prepared runtime and slot");
  REQUIRE_TRUE(source.manual_events,
               "instrument overlay fixture lacked its prepared runtime and slot");
  const std::array<engine::RoutedNode, 2> nodes{{{{1, 0}, source.processor}, {{2, 0}, {}}}};
  const std::array<engine::RoutingEdge, 1> edges{{{11, 1, 2}}};
  const engine::RoutedMixerLimits limits{{2, 64},
                                         frames,
                                         3ULL * frames * sizeof(engine::StereoFrame),
                                         (frames + 1ULL) * sizeof(engine::StereoFrame)};
  engine::RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, limits),
             "instrument overlay route did not prepare");
  std::array<engine::StereoFrame, 128> output{};
  const std::array<std::span<const engine::StereoFrame>, 2> inputs{};
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(publisher.render_block(inputs, block, {0, 1}), engine::RoutedRenderStatus::rendered,
             "instrument overlay baseline did not activate");
  REQUIRE_EQ(source.manual_events->active_graph().graph_revision, 1,
             "instrument overlay baseline did not activate");
  REQUIRE_TRUE(!source.process_failed(), "instrument overlay baseline did not activate");
  const auto identity = source.manual_events->active_graph();
  const std::array<engine::RoutedNodeControl, 2> controls{
      {{1, false, {1.0F, 1.0F}}, {2, false, {1.0F, 1.0F}}}};
  REQUIRE_OK(publisher.prepare_control_overlay(2, controls),
             "instrument revision-only overlay required a rebuild");
  counted_allocations = 0;
  counting_allocations = true;
  const auto switched = publisher.render_block(inputs, block, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, engine::RoutedRenderStatus::rendered,
             "instrument overlay reset the host, silenced a block or missed its identity switch");
  REQUIRE_EQ(counted_allocations, 0,
             "instrument overlay reset the host, silenced a block or missed its identity switch");
  REQUIRE_TRUE(!source.process_failed(),
               "instrument overlay reset the host, silenced a block or missed its identity switch");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "instrument overlay reset the host, silenced a block or missed its identity switch");
  REQUIRE_EQ(source.manual_events->active_graph().graph_revision, 2,
             "instrument overlay reset the host, silenced a block or missed its identity switch");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "instrument overlay reset the host, silenced a block or missed its identity switch");
  REQUIRE_EQ(source.manual_events->submit({.graph_revision = 1,
                                           .instance_id = identity.instance_id,
                                           .compatibility_key = identity.compatibility_key},
                                          3, {1, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "instrument overlay retained old manual authority or refused the new revision");
  REQUIRE_EQ(source.manual_events->submit({.graph_revision = 2,
                                           .instance_id = identity.instance_id,
                                           .compatibility_key = identity.compatibility_key},
                                          3, {1, 0, 0.75}),
             plugins::Vst3ManualEventStatus::accepted,
             "instrument overlay retained old manual authority or refused the new revision");
  counted_allocations = 0;
  counting_allocations = true;
  const auto applied = publisher.render_block(inputs, block, {2 * frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(applied, engine::RoutedRenderStatus::rendered,
             "rebound instrument parameter missed its first audible block");
  REQUIRE_EQ(counted_allocations, 0, "rebound instrument parameter missed its first audible block");
  REQUIRE_EQ(source.manual_events->receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "rebound instrument parameter missed its first audible block");
  REQUIRE_EQ(source.manual_events->receipt().revision, 3,
             "rebound instrument parameter missed its first audible block");
  REQUIRE_EQ(source.manual_events->receipt().project_sample, 2 * frames,
             "rebound instrument parameter missed its first audible block");
  require(provider.stop_after_join(), "instrument overlay fixture did not stop after callback");
}

void preview_instrument_event(const plugins::ScanResult& scanned, std::uint32_t frames) {
  Gate gate;
  auto instrument = saved(scanned);
  instrument.missing = false;
  auto state = make_model(gate, instrument);
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true, {1});
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(provider.prepare(*state.snapshot(),
                                      saved_instrument(state.snapshot()->tracks.front()), frames));
  auto slot = provider.manual_events();
  REQUIRE_TRUE(slot, "preview instrument did not prepare");
  REQUIRE_TRUE(source.processor, "preview instrument did not prepare");
  source.processor->graph_activated(1);
  const auto identity = slot->active_graph();
  auto preview = [&](std::uint32_t id, double value) {
    return slot->preview(identity, {id, 0, value});
  };
  REQUIRE_EQ(preview(2, 0.8), plugins::Vst3ManualEventStatus::unavailable,
             "instrument drag preview did not validate and coalesce");
  REQUIRE_EQ(preview(1, 0.25), plugins::Vst3ManualEventStatus::accepted,
             "instrument drag preview did not validate and coalesce");
  REQUIRE_EQ(preview(1, 0.65), plugins::Vst3ManualEventStatus::accepted,
             "instrument drag preview did not validate and coalesce");
  std::vector<engine::StereoFrame> output(frames);
  counted_allocations = 0;
  counting_allocations = true;
  source.processor->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "instrument preview callback allocated or published a saved receipt");
  REQUIRE_TRUE(!source.process_failed(),
               "instrument preview callback allocated or published a saved receipt");
  REQUIRE_EQ(slot->receipt().phase, plugins::Vst3ManualEventPhase::empty,
             "instrument preview callback allocated or published a saved receipt");
  near(output[5], 0.65F);
  REQUIRE_EQ(preview(1, 0.9), plugins::Vst3ManualEventStatus::accepted,
             "instrument final gesture did not supersede pending preview");
  REQUIRE_EQ(slot->submit(identity, 2, {1, 0, 0.75}), plugins::Vst3ManualEventStatus::accepted,
             "instrument final gesture did not supersede pending preview");
  source.processor->process(output, {frames, 1});
  near(output.front(), 0.75F);
  REQUIRE_EQ(slot->receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "instrument final commit failed to dominate stale preview");
  REQUIRE_EQ(slot->receipt().revision, 2,
             "instrument final commit failed to dominate stale preview");
  REQUIRE_EQ(slot->receipt().project_sample, frames,
             "instrument final commit failed to dominate stale preview");
  REQUIRE_OK(slot->collect(), "instrument final commit failed to dominate stale preview");
  source.processor->process(output, {2 * frames, 1});
  REQUIRE_TRUE(!source.process_failed(), "instrument stale preview replayed after final commit");
  REQUIRE_EQ(slot->receipt().phase, plugins::Vst3ManualEventPhase::empty,
             "instrument stale preview replayed after final commit");
  REQUIRE_EQ(preview(1, 0.3), plugins::Vst3ManualEventStatus::accepted,
             "instrument could not stage retirement preview");
  source.processor->graph_deactivated(1);
  source.processor->graph_activated(2);
  source.processor->process(output, {3 * frames, 1});
  REQUIRE_TRUE(!source.process_failed(), "instrument stale preview replayed after graph switch");
  REQUIRE_TRUE(!slot->claim_preview(), "instrument stale preview replayed after graph switch");
  REQUIRE_EQ(slot->preview({.graph_revision = 1,
                            .instance_id = identity.instance_id,
                            .compatibility_key = identity.compatibility_key},
                           {1, 0, 0.4}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "old instrument graph accepted a preview after switch");
  require(provider.stop_after_join(), "preview instrument did not stop");
}

void size_scoped_instrument_event(const plugins::ScanResult& scanned) {
  Gate gate;
  auto instrument = saved(scanned);
  instrument.missing = false;
  auto state = make_model(gate, instrument);
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  provider.set_size_scoped_manual_qualification(id<project::PluginInstanceId>(5), true,
                                                {{64, {1}}});
  auto source = take(
      provider.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks.front()), 128));
  auto slot = provider.manual_events();
  REQUIRE_TRUE(slot, "size-scoped instrument failed preparation");
  REQUIRE_TRUE(source.processor, "size-scoped instrument failed preparation");
  source.processor->graph_activated(1);
  const auto identity = slot->active_graph();
  REQUIRE_EQ(slot->preview(identity, {1, 0, 0.65}), plugins::Vst3ManualEventStatus::accepted,
             "size-scoped instrument preview failed preflight");
  std::array<engine::StereoFrame, 128> output{};
  counted_allocations = 0;
  counting_allocations = true;
  source.processor->process(output, {0, 1});
  counting_allocations = false;
  near(output[5], 0.5F);
  REQUIRE_EQ(counted_allocations, 0,
             "128-frame instrument callback applied 64-only preview or allocated");
  REQUIRE_TRUE(!source.process_failed(),
               "128-frame instrument callback applied 64-only preview or allocated");
  REQUIRE_EQ(slot->receipt().phase, plugins::Vst3ManualEventPhase::empty,
             "128-frame instrument callback applied 64-only preview or allocated");
  REQUIRE_EQ(slot->submit(identity, 2, {1, 0, 0.75}), plugins::Vst3ManualEventStatus::accepted,
             "size-scoped instrument commit failed preflight");
  counted_allocations = 0;
  counting_allocations = true;
  source.processor->process(output, {128, 1});
  counting_allocations = false;
  near(output.front(), 0.5F);
  REQUIRE_EQ(counted_allocations, 0,
             "128-frame instrument callback applied 64-only commit or lost failed receipt");
  REQUIRE_TRUE(!source.process_failed(),
               "128-frame instrument callback applied 64-only commit or lost failed receipt");
  REQUIRE_EQ(slot->receipt().phase, plugins::Vst3ManualEventPhase::failed,
             "128-frame instrument callback applied 64-only commit or lost failed receipt");
  REQUIRE_EQ(slot->receipt().revision, 2,
             "128-frame instrument callback applied 64-only commit or lost failed receipt");
  REQUIRE_OK(slot->collect(),
             "128-frame instrument callback applied 64-only commit or lost failed receipt");
  REQUIRE_EQ(slot->submit(identity, 3, {1, 0, 0.75}), plugins::Vst3ManualEventStatus::accepted,
             "64-frame retry failed preflight");
  source.processor->process(std::span(output).first(64), {256, 1});
  REQUIRE_TRUE(!source.process_failed(),
               "64-frame instrument retry did not reach qualified host call");
  REQUIRE_EQ(slot->receipt().phase, plugins::Vst3ManualEventPhase::applied,
             "64-frame instrument retry did not reach qualified host call");
  REQUIRE_EQ(slot->receipt().project_sample, 256,
             "64-frame instrument retry did not reach qualified host call");
}

void manual_instrument_gates(const plugins::ScanResult& scanned) {
  Gate gate;
  auto instrument = saved(scanned);
  instrument.missing = false;
  auto state = make_model(gate, instrument);
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  application::RoutedPlaybackControl control(state, {{1, 64}, 64, 4'096, 4'096}, &provider);
  REQUIRE_OK(control.submit_current(), "latency-gated instrument did not prepare");
  auto slot = provider.manual_events();
  REQUIRE_TRUE(slot, "unpublished source accepted a manual event");
  REQUIRE_EQ(slot->submit({.graph_revision = 1, .instance_id = 1, .compatibility_key = 1}, 2,
                          {1, 0, 0.75}),
             plugins::Vst3ManualEventStatus::stale_instance,
             "unpublished source accepted a manual event");
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::array<engine::StereoFrame, 64> output{};
  REQUIRE_EQ(control.render_block(inputs, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "latency-gated instrument first render failed");
  const auto identity = slot->active_graph();
  require(!control.active_vst3_instrument_manual_events(
              id<project::TrackId>(2), id<project::PluginInstanceId>(5), identity.graph_revision),
          "unqualified instrument exposed a routed manual binding");
  REQUIRE_EQ(slot->submit(identity, 2, {1, 0, 0.75}),
             plugins::Vst3ManualEventStatus::latency_unverified,
             "generic instrument admitted a manual value without fixed latency evidence");

  plugins::Vst3InstrumentTrackProvider scoped(plugins::Vst3ProcessingMode::realtime, true, {2});
  scoped.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto scoped_source = take(
      scoped.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks.front()), 64));
  scoped_source.processor->graph_activated(1);
  const auto scoped_identity = scoped.manual_events()->active_graph();
  REQUIRE_EQ(scoped.manual_events()->admission(scoped_identity, 2, {1, 0, 0.75}),
             plugins::Vst3ManualEventStatus::unavailable,
             "parameter-scoped qualification admitted an unqualified control");
  plugins::Vst3InstrumentTrackProvider exact(plugins::Vst3ProcessingMode::realtime, true, {1});
  exact.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto exact_source = take(
      exact.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks.front()), 64));
  exact_source.processor->graph_activated(1);
  const auto exact_identity = exact.manual_events()->active_graph();
  REQUIRE_EQ(exact.manual_events()->admission(exact_identity, 2, {1, 0, 0.75}),
             plugins::Vst3ManualEventStatus::accepted,
             "qualified continuous instrument control was refused");

  plugins::Vst3InstrumentTrackProvider sized;
  sized.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  sized.set_size_scoped_manual_qualification(id<project::PluginInstanceId>(5), true,
                                             {{64, {1, 1, 999}}});
  auto sized_source = take(
      sized.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks.front()), 128));
  REQUIRE_TRUE(sized_source.manual_events,
               "instrument provider lost exact process-size and metadata intersection");
  REQUIRE_TRUE(sized_source.manual_events->qualified_for_block(1, 64),
               "instrument provider lost exact process-size and metadata intersection");
  REQUIRE_TRUE(!sized_source.manual_events->qualified_for_block(1, 128),
               "instrument provider lost exact process-size and metadata intersection");
  REQUIRE_TRUE(!sized_source.manual_events->qualified_for_block(999, 64),
               "instrument provider lost exact process-size and metadata intersection");

  auto automated = *state.snapshot();
  automated.tracks.front().automation.push_back(
      {id<project::AutomationLaneId>(70),
       project::Vst3ParameterTarget{id<project::PluginInstanceId>(5), 1},
       {project::AutomationRepresentation::vst3_normalized, "level", "", 0.0, 1.0, 0.5, false, 0},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(71), 0, 0.5}},
       1});
  auto snapshot = std::make_shared<project::Snapshot>(std::move(automated));
  plugins::Vst3InstrumentTrackProvider automated_provider(plugins::Vst3ProcessingMode::realtime,
                                                          true);
  automated_provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto prepared =
      take(automated_provider.prepare(*snapshot, saved_instrument(snapshot->tracks.front()), 64));
  prepared.processor->graph_activated(1);
  const auto automated_identity = automated_provider.manual_events()->active_graph();
  REQUIRE_EQ(automated_provider.manual_events()->submit(automated_identity, 2, {1, 0, 0.75}),
             plugins::Vst3ManualEventStatus::automated,
             "instrument manual value overrode an automated parameter");
}

constexpr std::int64_t lane_span = 1'000'000;

double lane_start(std::uint32_t parameter) { return static_cast<double>(parameter) / 256.0; }

// A linear ramp that changes on every frame, so each lane fills its full
// per-frame share of the event budget.
project::AutomationLane continuous_lane(std::uint32_t parameter) {
  return {id<project::AutomationLaneId>(100 + parameter),
          project::Vst3ParameterTarget{id<project::PluginInstanceId>(5), parameter},
          {project::AutomationRepresentation::vst3_normalized, "lane", "", 0.0, 1.0, 0.5, false, 0},
          project::AutomationTimebase::elapsed,
          {{id<project::AutomationPointId>(1'000 + 2 * parameter), 0, lane_start(parameter),
            project::AutomationShape::linear},
           {id<project::AutomationPointId>(1'001 + 2 * parameter), lane_span,
            lane_start(parameter) + 0.5}},
          1};
}

std::shared_ptr<project::Snapshot> lane_snapshot(const project::ProjectModel& state,
                                                 std::uint32_t lanes) {
  auto snapshot = *state.snapshot();
  for (std::uint32_t parameter = 2; parameter < 2 + lanes; ++parameter)
    snapshot.tracks[0].automation.push_back(continuous_lane(parameter));
  return std::make_shared<project::Snapshot>(std::move(snapshot));
}

struct LoggedPoint {
  std::uint32_t id{0};
  std::int32_t offset{0};
  double value{0.0};
};

std::vector<LoggedPoint> delivered_points(const plugins::ScanResult& scanned) {
  void* module = dlopen(scanned.binary.canonical_binary.c_str(), RTLD_NOW | RTLD_NOLOAD);
  REQUIRE_NE(module, nullptr, "lane instrument fixture module was not resident");
  using Count = std::size_t (*)();
  using Point = bool (*)(std::size_t, std::uint32_t*, std::int32_t*, double*);
  auto count = reinterpret_cast<Count>(dlsym(module, "OscillateFixtureParameterPointCount"));
  auto point = reinterpret_cast<Point>(dlsym(module, "OscillateFixtureParameterPoint"));
  REQUIRE_NE(count, nullptr, "lane instrument point log is unavailable");
  REQUIRE_NE(point, nullptr, "lane instrument point log is unavailable");
  std::vector<LoggedPoint> points(count());
  for (std::size_t index = 0; index < points.size(); ++index)
    require(point(index, &points[index].id, &points[index].offset, &points[index].value),
            "lane instrument point log entry is unreadable");
  REQUIRE_EQ(dlclose(module), 0, "lane instrument point log could not close");
  return points;
}

// Independent oracle: every lane delivers one point per frame at its exact
// sample offset with the linear-ramp value, after an optional manual point.
void expect_lane_points(const plugins::ScanResult& scanned, std::uint32_t lanes,
                        std::uint32_t frames, std::int64_t position,
                        std::optional<plugins::Vst3ParameterEvent> manual) {
  const auto points = delivered_points(scanned);
  const std::size_t manual_count = manual ? 1 : 0;
  REQUIRE_EQ(points.size(), static_cast<std::size_t>(lanes) * frames + manual_count,
             "instrument did not receive every automation point within the event budget");
  if (manual)
    require(points[0].id == manual->id && points[0].offset == 0 &&
                points[0].value == manual->normalized_value,
            "manual instrument event lost its sample-zero position beside full automation");
  for (std::uint32_t lane = 0; lane < lanes; ++lane) {
    const auto parameter = 2 + lane;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      const auto& point = points[manual_count + static_cast<std::size_t>(lane) * frames + frame];
      const double expected = lane_start(parameter) + 0.5 * static_cast<double>(position + frame) /
                                                          static_cast<double>(lane_span);
      REQUIRE_EQ(point.id, parameter,
                 "automation lane point has the wrong parameter, sample offset or value");
      REQUIRE_EQ(point.offset, static_cast<std::int32_t>(frame),
                 "automation lane point has the wrong parameter, sample offset or value");
      REQUIRE_LT(std::fabs(point.value - expected), tolerance::double_accumulation,
                 "automation lane point has the wrong parameter, sample offset or value");
    }
  }
}

// ADR-0035: a sustain pedal reaches the instrument as the parameter its
// IMidiMapping assigns to CC64 (here a hidden one), at the pedal's sample
// offset, and is sent again after a seek so it stays down.
void pedal_reaches_mapped_controller(const plugins::ScanResult& scanned, std::uint32_t frames) {
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_PEDAL", "1", 1), 0, "pedal mode set");
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  (void)take(state.apply(
      {project::SetMidiPedalLane{id<project::ClipId>(4),
                                 project::MidiPedal::sustain,
                                 {{{0}, 1.0}, {{timeline::ticks_per_quarter / 2}, 0.0}}}}));
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(
      provider.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks[0]), frames));
  std::vector<engine::StereoFrame> output(frames);
  counting_allocations = true;
  source.processor->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "a pedalled instrument block failed or allocated");
  REQUIRE_TRUE(!source.process_failed(), "a pedalled instrument block failed or allocated");
  auto points = delivered_points(scanned);
  REQUIRE_EQ(points.size(), 1, "the sustain pedal reached the mapped CC64 parameter at its offset");
  REQUIRE_EQ(points[0].id, 900,
             "the sustain pedal reached the mapped CC64 parameter at its offset");
  REQUIRE_EQ(points[0].offset, 0,
             "the sustain pedal reached the mapped CC64 parameter at its offset");
  REQUIRE_EQ(points[0].value, 1.0,
             "the sustain pedal reached the mapped CC64 parameter at its offset");
  source.processor->process(output, {frames, 1});
  require(delivered_points(scanned).empty(), "a held pedal is not resent within a generation");
  source.processor->process(output, {4 * frames, 2});
  points = delivered_points(scanned);
  REQUIRE_EQ(points.size(), 1, "a seek resends the held pedal");
  REQUIRE_EQ(points[0].id, 900, "a seek resends the held pedal");
  REQUIRE_EQ(points[0].offset, 0, "a seek resends the held pedal");
  REQUIRE_EQ(points[0].value, 1.0, "a seek resends the held pedal");
  source.processor->process(output, {12'000 - frames / 2, 2});
  points = delivered_points(scanned);
  REQUIRE_EQ(points.size(), 1, "the lift reaches the plugin at its sample");
  REQUIRE_EQ(points[0].value, 0.0, "the lift reaches the plugin at its sample");
  REQUIRE_EQ(points[0].offset, static_cast<std::int32_t>(frames / 2),
             "the lift reaches the plugin at its sample");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_PEDAL"), 0, "pedal mode cleared");
}

// A manual edit to the parameter a pedal is mapped to, in the block where
// the pedal moves at the same offset, sends one value there (the manual
// edit's, applied last), not two the host would refuse.
void manual_edit_meets_pedal_at_one_offset(const plugins::ScanResult& scanned,
                                           std::uint32_t frames) {
  constexpr std::uint32_t level_parameter = 1;
  constexpr double pedal_down = 1.0;
  constexpr double manual_value = 0.25;
  constexpr std::uint64_t manual_revision = 1;
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_PEDAL", "visible", 1), 0, "pedal mode set");
  Gate gate;
  auto state = make_model(gate, saved(scanned));
  (void)take(state.apply({project::SetMidiPedalLane{
      id<project::ClipId>(4), project::MidiPedal::sustain, {{{0}, pedal_down}}}}));
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(
      provider.prepare(*state.snapshot(), saved_instrument(state.snapshot()->tracks[0]), frames));
  REQUIRE_TRUE(source.manual_events, "the pedalled instrument admits manual edits");
  const auto graph_revision = state.snapshot()->revision;
  source.processor->graph_activated(graph_revision);
  const auto identity = source.manual_events->active_graph();
  REQUIRE_EQ(source.manual_events->submit({.graph_revision = graph_revision,
                                           .instance_id = identity.instance_id,
                                           .compatibility_key = identity.compatibility_key},
                                          manual_revision, {level_parameter, 0, manual_value}),
             plugins::Vst3ManualEventStatus::accepted, "the manual edit is admitted");
  std::vector<engine::StereoFrame> output(frames);
  counting_allocations = true;
  counted_allocations = 0;
  source.processor->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the colliding block allocated");
  REQUIRE_TRUE(!source.process_failed(),
               "a manual edit and a pedal at one offset failed the block");
  const auto points = delivered_points(scanned);
  REQUIRE_EQ(points.size(), 1, "one value reaches the parameter at the shared offset");
  REQUIRE_EQ(points[0].id, level_parameter, "one value reaches the parameter at the shared offset");
  REQUIRE_EQ(points[0].offset, 0, "one value reaches the parameter at the shared offset");
  REQUIRE_EQ(points[0].value, manual_value, "the manual edit, applied last, wins");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_PEDAL"), 0, "pedal mode cleared");
}

void automation_event_budget_contract() {
  using plugins::vst3_automation_event_budget;
  REQUIRE_EQ(vst3_automation_event_budget(8, 128), 1'025U,
             "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
  REQUIRE_EQ(vst3_automation_event_budget(31, 128), 3'969U,
             "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
  REQUIRE_TRUE(!vst3_automation_event_budget(32, 128),
               "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
  REQUIRE_EQ(vst3_automation_event_budget(63, 64), 4'033U,
             "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
  REQUIRE_TRUE(!vst3_automation_event_budget(64, 64),
               "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
  REQUIRE_EQ(vst3_automation_event_budget(0, 128), 1U,
             "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
  REQUIRE_TRUE(!vst3_automation_event_budget(0, 0),
               "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
  REQUIRE_TRUE(!vst3_automation_event_budget(std::size_t{1} << 40U, 1),
               "VST3 automation budget does not reserve lanes-by-frames plus one manual slot");
}

void lane_capacity(const plugins::ScanResult& scanned, std::uint32_t frames, std::uint32_t lanes) {
  using plugins::Vst3ManualEventPhase;
  using plugins::Vst3ManualEventStatus;
  Gate gate;
  auto instrument = saved(scanned);
  instrument.missing = false;
  auto state = make_model(gate, instrument);
  const auto snapshot = lane_snapshot(state, lanes);
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  auto source = take(provider.prepare(*snapshot, saved_instrument(snapshot->tracks[0]), frames));
  source.processor->graph_activated(1);
  auto slot = provider.manual_events();
  const auto identity = slot->active_graph();
  const plugins::Vst3ParameterEvent manual{1, 0, 0.75};
  REQUIRE_EQ(slot->submit(identity, 2, manual), Vst3ManualEventStatus::accepted,
             "fully automated instrument refused an eligible manual event");
  std::vector<engine::StereoFrame> output(frames);
  counted_allocations = 0;
  counting_allocations = true;
  source.processor->process(output, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "multi-lane instrument callback failed or allocated");
  REQUIRE_TRUE(!source.process_failed(), "multi-lane instrument callback failed or allocated");
  expect_lane_points(scanned, lanes, frames, 0, manual);
  near(output[frames - 1], 0.75F);
  const auto receipt = slot->collect();
  REQUIRE_TRUE(receipt, "manual event beside full automation lacked an applied receipt");
  REQUIRE_EQ(receipt->phase, Vst3ManualEventPhase::applied,
             "manual event beside full automation lacked an applied receipt");
  REQUIRE_EQ(receipt->revision, 2, "manual event beside full automation lacked an applied receipt");
  REQUIRE_EQ(receipt->project_sample, 0,
             "manual event beside full automation lacked an applied receipt");
  counted_allocations = 0;
  counting_allocations = true;
  source.processor->process(output, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "multi-lane instrument second callback failed or allocated");
  REQUIRE_TRUE(!source.process_failed(),
               "multi-lane instrument second callback failed or allocated");
  expect_lane_points(scanned, lanes, frames, frames, std::nullopt);
  require(provider.stop_after_join(), "multi-lane instrument did not stop");
}

void lane_overflow(const plugins::ScanResult& scanned, std::uint32_t frames) {
  Gate gate;
  auto instrument = saved(scanned);
  instrument.missing = false;
  auto state = make_model(gate, instrument);
  const auto lanes = (plugins::max_vst3_parameter_events - 1U) / frames + 1U;
  const auto snapshot = lane_snapshot(state, lanes);
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true);
  provider.set_scan_receipt(id<project::PluginInstanceId>(5), scanned);
  const auto rejected = provider.prepare(*snapshot, saved_instrument(snapshot->tracks[0]), frames);
  REQUIRE_TRUE(!rejected,
               "instrument automation beyond the event budget was not rejected explicitly");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::resource_exhausted,
             "instrument automation beyond the event budget was not rejected explicitly");
  REQUIRE_TRUE(!provider.manual_events(),
               "instrument automation beyond the event budget was not rejected explicitly");
}
} // namespace

int main() {
  try {
    plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
    auto scanned = scanner.scan(fs::path(VST3_INSTRUMENT_BUNDLE));
    if (!scanned.ok() || scanned.metadata.classes.size() != 1)
      throw std::runtime_error("instrument fixture scan failed: " + scanned.diagnostic);
    live_routed_instrument(scanned, 64);
    live_routed_instrument(scanned, 128);
    live_midi_edit_moves_the_held_note(scanned, 64);
    live_midi_edit_moves_the_held_note(scanned, 128);
    live_instrument_removal_releases_its_note(scanned, 64);
    live_instrument_removal_releases_its_note(scanned, 128);
    two_prepared_instruments_keep_independent_owners(scanned, 64);
    two_prepared_instruments_keep_independent_owners(scanned, 128);
    paired_instrument_effects_have_any_shape(scanned, 64);
    paired_instrument_effects_have_any_shape(scanned, 128);
    planned_loop_keeps_instrument_tail(scanned, 64);
    planned_loop_keeps_instrument_tail(scanned, 128);
    fixed_instrument_tail_evidence(scanned, 64);
    fixed_instrument_tail_evidence(scanned, 128);
    prepared_export_end(scanned, 64);
    prepared_export_end(scanned, 128);
    export_end_event_capacity(scanned);
    stepped_tempo_within_block(scanned, 64);
    stepped_tempo_within_block(scanned, 128);
    gradual_tempo_at_block_start(scanned);
    rejection_and_fault(scanned);
    instrument_parameter_delivery(scanned, 64);
    instrument_parameter_delivery(scanned, 128);
    preview_instrument_event(scanned, 64);
    preview_instrument_event(scanned, 128);
    size_scoped_instrument_event(scanned);
    manual_instrument_event(scanned, 64);
    manual_instrument_event(scanned, 128);
    instrument_control_overlay_rebinds_manual_slot(scanned, 64);
    instrument_control_overlay_rebinds_manual_slot(scanned, 128);
    manual_instrument_gates(scanned);
    separate_native_chain_preflight(scanned);
    callback_baseline(scanned, 64);
    callback_baseline(scanned, 128);
    automation_event_budget_contract();
    pedal_reaches_mapped_controller(scanned, 64);
    pedal_reaches_mapped_controller(scanned, 128);
    manual_edit_meets_pedal_at_one_offset(scanned, 64);
    manual_edit_meets_pedal_at_one_offset(scanned, 128);
    auto lanes = scanner.scan(fs::path(VST3_LANE_INSTRUMENT_BUNDLE));
    if (!lanes.ok() || lanes.metadata.classes.size() != 1)
      throw std::runtime_error("lane instrument fixture scan failed: " + lanes.diagnostic);
    for (const std::uint32_t frames : {64U, 128U}) {
      lane_capacity(lanes, frames, 8);
      lane_capacity(lanes, frames, (plugins::max_vst3_parameter_events - 1U) / frames);
      lane_overflow(lanes, frames);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
