#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace inserts = oscillate::native_inserts;
namespace fs = std::filesystem;

// ADR-0043: a built-in effect's panel values, automation lanes and master
// slot edit live — no graph rebuild, no hold, no callback allocation — and are
// reported audible only once the stage acknowledges the exact revision. A
// sidechained effect edits live too.
using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-routed-effect") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

engine::RoutedMixerLimits limits(std::size_t max_frames = 64) {
  engine::RoutedMixerLimits result{{1, 64},
                                   max_frames,
                                   7ULL * max_frames * sizeof(engine::StereoFrame),
                                   5ULL * 65 * sizeof(engine::StereoFrame)};
  result.node_meters = true;
  return result;
}

struct Live {
  Gate gate;
  project::ProjectModel model;
  app::RoutedPlaybackControl control;
  std::unique_ptr<app::RoutedAudioSource> source;
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 64> stereo{};
  std::array<float, 128> samples{};

  Live(const fs::path& root, const project::Snapshot& saved)
      : model(take(project::ProjectModel::open(saved, gate))), control(model, limits()) {
    REQUIRE_OK(control.submit_current(), "graph did not prepare");
    source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
    control.bind_live_source(*source);
  }
  ~Live() { control.unbind_live_source(); }

  double render(int blocks) {
    const auto device = source->render_source();
    double power = 0.0;
    counting_allocations = true;
    for (int block = 0; block < blocks; ++block) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
                 audio::BlockResult::rendered, "block failed to render");
      for (const float sample : samples)
        power += static_cast<double>(sample) * sample;
    }
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "callbacks never allocate");
    return power;
  }

  // Applies `commands`, offers them to playback and proves they went live.
  void edit_live(const std::vector<project::Operation>& commands, std::string_view label) {
    auto applied = take(model.apply(commands));
    const auto edited = control.begin_edit(applied, label);
    REQUIRE_OK(edited, std::string(label) + " was not accepted live: " +
                           (edited ? std::string() : edited.error().message));
    require(!control.held_boundary().has_value(), std::string(label) + " held the graph");
    REQUIRE_GT(render(4), 0.0, std::string(label) + ": sound continues through the edit");
    control.observe();
    REQUIRE_EQ(control.status().active_graph_revision, applied.snapshot->revision,
               std::string(label) + " is audible once its stage acknowledges it");
  }
};

project::NativeEffectPatch with_value(const project::NativeEffectPatch& patch,
                                      std::string_view parameter, double value) {
  auto resolved = take(inserts::resolve_native_effect(patch));
  resolved.values[take(synth::find_parameter(*resolved.engine->descriptor, parameter))] = value;
  return take(inserts::make_native_effect_patch(*resolved.engine, resolved.values, patch.document));
}

// A held Prism note on track one; `effect` on its chain (and on the master).
project::Snapshot effect_project(const fs::path& root, std::string_view effect, bool master,
                                 bool sidechained = false) {
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip was not added");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session->notes().add_note(clip, {0}, 60), "note was not added");
  if (master) {
    (void)take(session->effects().add_effect_engine(project::MasterSignalPath{}, 0, effect));
  } else if (sidechained) {
    // The effect sits on an audio bus fed by the synth; a second synth keys it.
    const auto bus = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
    REQUIRE_OK(session->routing().set_main_output(track, bus), "synth routed to the bus");
    const auto slot = take(session->effects().add_effect_engine(bus, 0, effect));
    const auto key = take(session->instruments().add_synth_track());
    REQUIRE_OK(session->midi_clips().add_midi_clip(key, {0}), "key clip");
    const auto key_clip = session->snapshot().tracks.back().midi_clips.front().id;
    REQUIRE_OK(session->notes().add_note(key_clip, {0}, 36), "key note");
    (void)take(session->routing().add_sidechain(key, bus, slot, 1.0));
  } else {
    (void)take(session->effects().add_effect_engine(track, 0, effect));
  }
  REQUIRE_OK(session->saving().save(), "project did not save");
  return session->snapshot();
}

void track_effect_values_edit_live() {
  Directory directory;
  const auto root = directory.path() / "song";
  Live live(root, effect_project(root, "contour", false));
  REQUIRE_GT(live.render(40), 1e-3, "the filtered note is audible");
  live.control.observe();
  const auto& track = live.model.snapshot()->tracks.front();
  const auto& slot = track.effects.front();
  const auto& patch = std::get<project::NativeEffectPatch>(slot.processor);
  live.edit_live({project::SetNativeEffectSettings{track.id, slot.id,
                                                   with_value(patch, "contour.cutoff", 400.0)}},
                 "track effect cutoff");
  // A second edit supersedes on the same stage.
  const auto& again = live.model.snapshot()->tracks.front();
  live.edit_live({project::SetNativeEffectSettings{again.id, again.effects.front().id,
                                                   with_value(std::get<project::NativeEffectPatch>(
                                                                  again.effects.front().processor),
                                                              "contour.cutoff", 2'000.0)}},
                 "second track effect cutoff");
}

// ADR-0058: a redrawn Pump shape reaches the running stage without a
// rebuild, like a knob.
void effect_documents_edit_live() {
  Directory directory;
  const auto root = directory.path() / "song";
  Live live(root, effect_project(root, "pump", false));
  REQUIRE_GT(live.render(40), 1e-3, "the pumped note is audible");
  live.control.observe();
  const auto& track = live.model.snapshot()->tracks.front();
  const auto& slot = track.effects.front();
  auto patch = std::get<project::NativeEffectPatch>(slot.processor);
  REQUIRE_EQ(patch.document.curves.size(), 1, "Pump starts with its shape");
  patch.document.curves.front().points = {{0.0, 0.0}, {0.18, 0.0}, {0.2, 1.0}, {1.0, 1.0}};
  live.edit_live({project::SetNativeEffectSettings{track.id, slot.id, patch}}, "pump shape");
}

// A linked group's shape edit changes Pumps on two chains in one revision;
// every stage takes it live, and a newer group edit supersedes an
// unacknowledged one.
void linked_effect_groups_edit_live() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    std::vector<project::InsertSlotId> pumps;
    for (int i = 0; i < 2; ++i) {
      const auto track = take(session->instruments().add_synth_track());
      REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
      const auto clip = session->snapshot().tracks.back().midi_clips.front().id;
      REQUIRE_OK(session->notes().add_note(clip, {0}, static_cast<std::uint8_t>(60 + i)), "note");
      pumps.push_back(take(session->effects().add_effect_engine(track, 0, "pump")));
    }
    REQUIRE_OK(session->effects().link_native_effects(pumps), "Pumps linked");
    REQUIRE_OK(session->saving().save(), "project did not save");
    return session->snapshot();
  }();
  Live live(root, saved);
  REQUIRE_GT(live.render(40), 1e-3, "the pumped notes are audible");
  live.control.observe();
  const auto redraw = [&](double hold) {
    std::vector<project::Operation> commands;
    for (const auto& track : live.model.snapshot()->tracks) {
      auto patch = std::get<project::NativeEffectPatch>(track.effects.front().processor);
      patch.document.curves.front().points = {
          {0.0, 0.0}, {hold, 0.0}, {hold + 0.02, 1.0}, {1.0, 1.0}};
      ++patch.generation;
      commands.push_back(
          project::SetNativeEffectSettings{track.id, track.effects.front().id, patch});
    }
    return commands;
  };
  live.edit_live(redraw(0.18), "linked pump shape");
  for (const auto& track : live.model.snapshot()->tracks)
    REQUIRE_EQ(std::get<project::NativeEffectPatch>(track.effects.front().processor)
                   .document.curves.front()
                   .points[1]
                   .x,
               0.18, "both Pumps hold the new shape");
  // Two drags in a row: the second supersedes the first before it lands.
  auto first = take(live.model.apply(redraw(0.25)));
  REQUIRE_OK(live.control.begin_edit(first, "drag"), "the first drag goes live");
  auto second = take(live.model.apply(redraw(0.3)));
  REQUIRE_OK(live.control.begin_edit(second, "drag"), "the second drag supersedes it");
  require(!live.control.held_boundary().has_value(), "no drag holds the graph");
  REQUIRE_GT(live.render(4), 0.0, "sound continues through the drags");
  live.control.observe();
  REQUIRE_EQ(live.control.status().active_graph_revision, second.snapshot->revision,
             "the newest group edit is audible once every stage has it");
}

void effect_automation_edits_live() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    const auto track = take(session->instruments().add_synth_track());
    REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
    const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
    REQUIRE_OK(session->notes().add_note(clip, {0}, 60), "note");
    const auto slot = take(session->effects().add_effect_engine(track, 0, "contour"));
    (void)take(session->parameter_automation().set_effect_automation_point(
        track, slot, "contour.cutoff", 0, 300.0));
    REQUIRE_OK(session->saving().save(), "saved");
    return session->snapshot();
  }();
  Live live(root, saved);
  (void)live.render(20);
  live.control.observe();
  auto lane = live.model.snapshot()->tracks.front().automation.front();
  lane.points.front().value = 5'000.0;
  ++lane.settings_generation;
  live.edit_live({project::ReplaceAutomationLane{lane.id, lane}}, "effect lane point");
}

void sidechained_effect_edits_live() {
  Directory directory;
  const auto root = directory.path() / "song";
  Live live(root, effect_project(root, "pocket", false, true));
  REQUIRE_GT(live.render(40), 1e-3, "the keyed bus is audible");
  live.control.observe();
  const auto bus = std::ranges::find_if(live.model.snapshot()->tracks, [](const auto& item) {
    return item.kind == project::TrackKind::group_bus;
  });
  const auto& slot = bus->effects.front();
  const auto& patch = std::get<project::NativeEffectPatch>(slot.processor);
  const auto& table = *effects::find_engine("pocket")->descriptor;
  const auto* depth = &table.parameters.front();
  for (const auto& parameter : table.parameters) {
    if (parameter.automatable && !synth::is_discrete(parameter) &&
        parameter.timing == synth::ParameterTiming::live) {
      depth = &parameter;
      break;
    }
  }
  live.edit_live(
      {project::SetNativeEffectSettings{
          bus->id, slot.id, with_value(patch, depth->id, synth::from_normalised(*depth, 0.8))}},
      "sidechained effect value");
}

void master_effect_values_edit_live() {
  Directory directory;
  const auto root = directory.path() / "song";
  Live live(root, effect_project(root, "vista", true));
  REQUIRE_GT(live.render(40), 1e-3, "the master is audible");
  live.control.observe();
  const auto& slot = live.model.snapshot()->master_effects.front();
  const auto& patch = std::get<project::NativeEffectPatch>(slot.processor);
  const auto& table = *effects::find_engine("vista")->descriptor;
  const synth::ParameterDescriptor* control = nullptr;
  for (const auto& parameter : table.parameters) {
    if (parameter.automatable && !synth::is_discrete(parameter) &&
        parameter.timing == synth::ParameterTiming::live) {
      control = &parameter;
      break;
    }
  }
  REQUIRE_NE(control, nullptr, "Vista has a live control");
  live.edit_live({project::SetNativeEffectSettings{
                     project::MasterSignalPath{}, slot.id,
                     with_value(patch, control->id, synth::from_normalised(*control, 0.7))}},
                 "master effect value");
}

} // namespace

// The common mix move: a compressor or Pocket on the bass synth itself,
// keyed by the kick synth, with no bus between them.
void instrument_track_effect_takes_a_key(std::string_view effect, bool solo_bass = false) {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    const auto bass = take(session->instruments().add_synth_track());
    REQUIRE_OK(session->midi_clips().add_midi_clip(bass, {0}), "bass clip");
    const auto bass_clip = session->snapshot().tracks.front().midi_clips.front().id;
    REQUIRE_OK(session->notes().add_note(bass_clip, {0}, 36), "bass note");
    const auto slot = take(session->effects().add_effect_engine(bass, 0, effect));
    const auto kick = take(session->instruments().add_synth_track());
    REQUIRE_OK(session->midi_clips().add_midi_clip(kick, {0}), "kick clip");
    const auto kick_clip = session->snapshot().tracks.back().midi_clips.front().id;
    REQUIRE_OK(session->notes().add_note(kick_clip, {0}, 36), "kick note");
    REQUIRE_OK(session->routing().set_sidechain_source(bass, slot, kick, 1.0), "kick keys it");
    if (solo_bass)
      REQUIRE_OK(session->tracks().set_track_soloed(bass, true), "bass soloed");
    REQUIRE_OK(session->saving().save(), "project did not save");
    return session->snapshot();
  }();
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  const auto submitted = control.submit_current();
  REQUIRE_OK(submitted, std::string(effect) +
                            " keyed on an instrument track does not "
                            "play: " +
                            (submitted ? std::string() : submitted.error().message));
  Live live(root, saved);
  REQUIRE_GT(live.render(40), 1e-3, "the keyed bass is audible");
  const auto& bass = live.model.snapshot()->tracks.front();
  const auto key = live.control.active_effect_key_peak(bass.id, bass.effects.front().id);
  require(key && *key > 1e-3F, std::string(effect) + " receives the kick as its key");
  if (solo_bass) {
    // Soloing the bass silences the kick, but the kick still keys it.
    live.render(200);
    live.control.observe();
    const auto kick = live.control.active_track_meter(live.model.snapshot()->tracks.back().id);
    const auto heard = live.control.active_track_meter(bass.id);
    REQUIRE_TRUE(kick, "a soloed bass keeps its key's source audible");
    REQUIRE_EQ(kick->left_peak, 0.0F, "a soloed bass keeps its key's source audible");
    REQUIRE_EQ(kick->right_peak, 0.0F, "a soloed bass keeps its key's source audible");
    REQUIRE_TRUE(heard, "the soloed bass is silent");
    REQUIRE_GT(heard->left_peak, 1e-3F, "the soloed bass is silent");
  }
}

// A keyed effect can be switched off (its key waits, saved) and deleted
// (its key goes with it, in one undo step).
void keyed_effect_switches_off_and_deletes() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto bass = take(session->instruments().add_synth_track());
  const auto slot = take(session->effects().add_effect_engine(bass, 0, "pocket"));
  const auto kick = take(session->instruments().add_synth_track());
  REQUIRE_OK(session->routing().set_sidechain_source(bass, slot, kick, 1.0), "kick keys Pocket");
  REQUIRE_OK(session->effects().set_effect_insert_enabled(bass, slot, false),
             "a keyed effect cannot be switched off");
  REQUIRE_EQ(session->snapshot().tracks.back().sends.size(), 1, "switching off lost the key");
  {
    Gate gate;
    auto model = take(project::ProjectModel::open(session->snapshot(), gate));
    app::RoutedPlaybackControl control(model, limits());
    const auto submitted = control.submit_current();
    REQUIRE_OK(submitted, "a switched-off keyed effect does not play: " +
                              (submitted ? std::string() : submitted.error().message));
  }
  REQUIRE_OK(session->effects().set_effect_insert_enabled(bass, slot, true), "switched back on");
  REQUIRE_OK(session->effects().remove_effect_insert(bass, slot),
             "a keyed effect cannot be deleted");
  REQUIRE_TRUE(session->snapshot().tracks.front().effects.empty(),
               "deleting the effect left its key behind");
  REQUIRE_TRUE(session->snapshot().tracks.back().sends.empty(),
               "deleting the effect left its key behind");
  REQUIRE_OK(session->history().undo(), "undo did not restore the effect and its key together");
  REQUIRE_EQ(session->snapshot().tracks.front().effects.size(), 1,
             "undo did not restore the effect and its key together");
  REQUIRE_EQ(session->snapshot().tracks.back().sends.size(), 1,
             "undo did not restore the effect and its key together");
}

int main() {
  try {
    keyed_effect_switches_off_and_deletes();
    instrument_track_effect_takes_a_key("press");
    instrument_track_effect_takes_a_key("pocket");
    instrument_track_effect_takes_a_key("press", true);
    track_effect_values_edit_live();
    effect_documents_edit_live();
    linked_effect_groups_edit_live();
    instrument_track_effect_takes_a_key("pump");
    effect_automation_edits_live();
    sidechained_effect_edits_live();
    master_effect_values_edit_live();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "routed effect live-edit contract passed\n";
  return 0;
}
