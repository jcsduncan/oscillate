#include "../native_dsp/bow_test_library.hpp"
#include "../native_dsp/concert_test_library.hpp"
#include "../native_dsp/fret_test_library.hpp"
#include "../native_dsp/wren_test_library.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

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
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-routed-synth") {}
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
  return {{1, 64},
          max_frames,
          6ULL * max_frames * sizeof(engine::StereoFrame),
          5ULL * 65 * sizeof(engine::StereoFrame)};
}

std::size_t index(std::string_view id) {
  return take(oscillate::native_dsp::find_parameter(*synth::default_engine().descriptor, id));
}

// A saved project with one Prism track playing a held C4 from the first beat.
project::Snapshot synth_project(const fs::path& root) {
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip was not added");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session->notes().add_note(clip, {0}, 60), "note was not added");
  REQUIRE_OK(session->saving().save(), "synth project did not save");
  return session->snapshot();
}

double block_power(const std::array<float, 128>& samples) {
  double power = 0.0;
  for (const float sample : samples)
    power += static_cast<double>(sample) * sample;
  return power;
}

void synth_track_plays_and_takes_live_edits_without_restart() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto saved = synth_project(root);
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "synth graph did not prepare");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
  control.bind_live_source(*source);
  const auto track = saved.tracks.front().id;
  REQUIRE_NE(source->synth_runtime(track), nullptr, "the routed source owns the synth runtime");

  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 64> stereo{};
  std::array<float, 128> samples{};
  const auto device = source->render_source();
  double sounded = 0.0;
  counted_allocations = 0;
  counting_allocations = true;
  for (int block = 0; block < 40; ++block) {
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "synth block failed to render");
    sounded += block_power(samples);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "routed synth callbacks never allocate");
  REQUIRE_GT(sounded, 1e-3, "the held note is audible");
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "the saved revision is active");

  // A panel edit to the playing synth is offered live, not by a restart.
  auto values = take(oscillate::native_inserts::resolve_native_instrument(
                         std::get<project::NativeInstrumentPatch>(
                             model.snapshot()->tracks.front().instrument_source)))
                    .values;
  values[index("prism.filter.cutoff")] = 300.0;
  values[index("prism.osc1.unison")] = 4; // Next-note: deferred, never a reset.
  auto patch = take(oscillate::native_inserts::make_native_patch(synth::default_engine(), values));
  auto edited = take(model.apply({project::SetNativeInstrumentPatch{track, patch}}));
  REQUIRE_OK(control.begin_edit(edited, "synth cutoff"),
             "synth edit was accepted by the live path");
  counting_allocations = true;
  double after = 0.0;
  for (int block = 0; block < 4; ++block) {
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "post-edit block failed to render");
    after += block_power(samples);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "live synth edits never allocate in the callback");
  REQUIRE_GT(after, 0.0, "the note keeps sounding through the edit");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, edited.snapshot->revision,
             "the edit is reported audible once the callback adopts it");
  REQUIRE_EQ(source->synth_runtime(track)->applied_revision(), edited.snapshot->revision,
             "the runtime acknowledged the exact revision");
  control.unbind_live_source();
}

void native_automation_edits_apply_without_restarting_voices() {
  for (const auto frames : {64U, 128U}) {
    Directory directory;
    const auto root = directory.path() / "song";
    const auto saved = synth_project(root);
    Gate gate;
    auto model = take(project::ProjectModel::open(saved, gate));
    app::RoutedPlaybackControl control(model, limits(frames));
    REQUIRE_OK(control.submit_current(), "synth automation graph did not prepare");
    auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, frames));
    control.bind_live_source(*source);
    Gate reference_gate;
    auto reference_model = take(project::ProjectModel::open(saved, reference_gate));
    app::RoutedPlaybackControl reference_control(reference_model, limits(frames));
    REQUIRE_OK(reference_control.submit_current(), "reference graph did not prepare");
    auto reference_source = take(app::RoutedAudioSource::prepare(root, *reference_model.snapshot(),
                                                                 reference_control, frames));
    reference_control.bind_live_source(*reference_source);
    audio::TransportCursor transport;
    audio::TransportCursor reference_transport;
    std::array<engine::StereoFrame, 128> stereo{};
    std::array<engine::StereoFrame, 128> reference_stereo{};
    std::array<float, 256> samples{};
    std::array<float, 256> reference_samples{};
    const auto output = std::span(samples).first(static_cast<std::size_t>(frames) * 2);
    const auto reference_output =
        std::span(reference_samples).first(static_cast<std::size_t>(frames) * 2);
    const auto device = source->render_source();
    const auto reference_device = reference_source->render_source();
    for (int block = 0; block < 16; ++block) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, output),
                 audio::BlockResult::rendered, "synth pre-edit blocks did not render");
      REQUIRE_EQ(audio::render_transport_block(reference_device, reference_transport, true,
                                               reference_stereo, reference_output),
                 audio::BlockResult::rendered, "synth pre-edit blocks did not render");
    }
    require(std::ranges::equal(output, reference_output),
            "reference and edited synth differed before automation");
    control.observe();
    reference_control.observe();
    const auto track = saved.tracks.front().id;
    auto* runtime = source->synth_runtime(track);
    REQUIRE_TRUE(runtime, "the note was not held before editing");
    REQUIRE_GT(runtime->observed_status().active_voices, 0, "the note was not held before editing");

    auto session = take(app::ProjectSession::open(root));
    const auto apply = [&](std::string_view label, bool expect_held_voice = true) {
      auto adopted = take(model.adopt_authoritative_snapshot(session->snapshot()));
      REQUIRE_OK(control.begin_edit(adopted, label),
                 "native automation edit did not enter the live mailbox");
      counted_allocations = 0;
      counting_allocations = true;
      const auto rendered = audio::render_transport_block(device, transport, true, stereo, output);
      counting_allocations = false;
      REQUIRE_EQ(rendered, audio::BlockResult::rendered,
                 "native automation callback failed or allocated");
      REQUIRE_EQ(counted_allocations, 0, "native automation callback failed or allocated");
      control.observe();
      require(control.status().active_graph_revision == session->snapshot().revision &&
                  source->synth_runtime(track) == runtime &&
                  (!expect_held_voice || runtime->observed_status().active_voices > 0),
              std::string(label) + " lost the runtime, revision, or sounding voice (active " +
                  std::to_string(control.status().active_graph_revision) + ", saved " +
                  std::to_string(session->snapshot().revision) + ", voices " +
                  std::to_string(runtime->observed_status().active_voices) + ", player " +
                  std::to_string(source->synth_player(track)->applied_plan_revision()) + ")");
    };
    const auto point = take(session->parameter_automation().set_native_automation_point(
        track, "prism.filter.cutoff", 0, 500.0));
    REQUIRE_OK(session->saving().save(), "live synth automation lane was not saved");
    apply("add synth automation lane");
    REQUIRE_EQ(audio::render_transport_block(reference_device, reference_transport, true,
                                             reference_stereo, reference_output),
               audio::BlockResult::rendered, "reference block after lane edit did not render");
    require(!std::ranges::equal(output, reference_output),
            "new synth automation lane made no audible difference");
    require(session->parameter_automation()
                .set_native_automation_value(track, "prism.filter.cutoff", point, 800.0)
                .has_value(),
            "live synth automation point value edit failed");
    apply("edit synth automation point");
    for (const double value : {1'000.0, 1'500.0}) {
      require(session->parameter_automation()
                  .set_native_automation_value(track, "prism.filter.cutoff", point, value)
                  .has_value(),
              "superseding synth automation value edit failed");
      auto adopted = take(model.adopt_authoritative_snapshot(session->snapshot()));
      REQUIRE_OK(control.begin_edit(adopted, "supersede synth automation"),
                 "pending synth automation offer could not be superseded");
    }
    counted_allocations = 0;
    counting_allocations = true;
    const auto superseded = audio::render_transport_block(device, transport, true, stereo, output);
    counting_allocations = false;
    control.observe();
    REQUIRE_EQ(superseded, audio::BlockResult::rendered,
               "latest synth automation offer did not apply without resetting voices");
    REQUIRE_EQ(counted_allocations, 0,
               "latest synth automation offer did not apply without resetting voices");
    REQUIRE_EQ(control.status().active_graph_revision, session->snapshot().revision,
               "latest synth automation offer did not apply without resetting voices");
    REQUIRE_GT(runtime->observed_status().active_voices, 0,
               "latest synth automation offer did not apply without resetting voices");
    require(session->automation()
                .toggle_automation_lane_timebase(
                    session->snapshot().tracks.front().automation.front().id)
                .has_value(),
            "live synth automation timebase edit failed");
    apply("change synth automation timebase");
    REQUIRE_OK(
        session->parameter_automation().remove_native_automation_lane(track, "prism.filter.cutoff"),
        "live synth automation lane removal failed");
    apply("remove synth automation lane");
    REQUIRE_OK(session->parameter_automation().set_native_automation_point(
                   track, "prism.filter.cutoff", 0, 500.0),
               "synth automation lane could not be recreated for transport checks");
    apply("recreate synth automation lane");
    require(transport.request_seek(0), "synth automation seek could not be queued");
    counted_allocations = 0;
    counting_allocations = true;
    const auto sought = audio::render_transport_block(device, transport, true, stereo, output);
    counting_allocations = false;
    REQUIRE_EQ(sought, audio::BlockResult::rendered,
               "synth automation seek did not evaluate at the requested sample");
    REQUIRE_EQ(counted_allocations, 0,
               "synth automation seek did not evaluate at the requested sample");
    require(transport.request_loop_wrap(0), "synth automation loop wrap could not be queued");
    counted_allocations = 0;
    counting_allocations = true;
    const auto wrapped = audio::render_transport_block(device, transport, true, stereo, output);
    counting_allocations = false;
    REQUIRE_EQ(wrapped, audio::BlockResult::rendered,
               "synth automation loop wrap lost its active revision");
    REQUIRE_EQ(counted_allocations, 0, "synth automation loop wrap lost its active revision");
    REQUIRE_EQ(control.status().active_graph_revision, session->snapshot().revision,
               "synth automation loop wrap lost its active revision");
    REQUIRE_OK(session->parameter_automation().set_native_automation_point(
                   track, "prism.filter.cutoff", 1, 600.0),
               "compound synth automation lane edit failed");
    auto values = take(session->instruments().native_instrument_values(track));
    values[index("prism.filter.cutoff")] = 400.0;
    REQUIRE_OK(session->instruments().set_native_instrument_values(track, values),
               "compound synth patch edit failed");
    auto compound = take(model.adopt_authoritative_snapshot(session->snapshot()));
    const auto refused = control.begin_edit(compound, "compound synth edit");
    REQUIRE_TRUE(!refused, "unsafe compound synth automation edit was not rejected clearly");
    REQUIRE_NE(refused.error().message.find("isolated lane edit"), std::string::npos,
               "unsafe compound synth automation edit was not rejected clearly");
    REQUIRE_LT(control.status().active_graph_revision, session->snapshot().revision,
               "unsafe compound synth automation edit was not rejected clearly");
    REQUIRE_EQ(source->synth_runtime(track), runtime,
               "unsafe compound synth automation edit was not rejected clearly");
    reference_control.unbind_live_source();
    control.unbind_live_source();
  }
}

void native_automation_mailbox_retries_latest_saved_revision() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto saved = synth_project(root);
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "automation mailbox graph did not prepare");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
  control.bind_live_source(*source);
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 64> stereo{};
  std::array<float, 128> samples{};
  const auto device = source->render_source();
  REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
             audio::BlockResult::rendered, "automation mailbox initial block did not render");
  control.observe();
  auto session = take(app::ProjectSession::open(root));
  for (int edit = 0; edit < 8; ++edit) {
    require(session->parameter_automation()
                .set_native_automation_point(saved.tracks.front().id, "prism.filter.cutoff", 0,
                                             500.0 + edit * 100.0)
                .has_value(),
            "automation mailbox edit failed");
    auto adopted = take(model.adopt_authoritative_snapshot(session->snapshot()));
    // Past the mailbox's bound the newest edit waits, never refused.
    REQUIRE_OK(control.begin_edit(adopted, "rapid synth lane edit"),
               "a rapid synth lane edit was refused instead of queued");
  }
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "the newest synth lane edit waits for the mailbox");
  REQUIRE_EQ(control.status().active_graph_revision, saved.revision,
             "an unacknowledged synth automation offer was reported active");
  counted_allocations = 0;
  counting_allocations = true;
  const auto first = audio::render_transport_block(device, transport, true, stereo, samples);
  counting_allocations = false;
  REQUIRE_EQ(first, audio::BlockResult::rendered,
             "full synth automation mailbox allocated in its callback");
  REQUIRE_EQ(counted_allocations, 0, "full synth automation mailbox allocated in its callback");
  control.observe(); // Acknowledges offer 7 and offers the waiting offer 8.
  counted_allocations = 0;
  counting_allocations = true;
  const auto second = audio::render_transport_block(device, transport, true, stereo, samples);
  counting_allocations = false;
  control.observe();
  REQUIRE_EQ(second, audio::BlockResult::rendered,
             "latest saved synth automation revision was not retried after acknowledgement");
  REQUIRE_EQ(counted_allocations, 0,
             "latest saved synth automation revision was not retried after acknowledgement");
  REQUIRE_EQ(control.status().active_graph_revision, session->snapshot().revision,
             "latest saved synth automation revision was not retried after acknowledgement");
  REQUIRE_EQ(source->synth_player(saved.tracks.front().id)->applied_plan_revision(),
             session->snapshot().revision,
             "latest saved synth automation revision was not retried after acknowledgement");
  control.unbind_live_source();
}

std::string automated_parameter(const synth::SynthEngine& engine) {
  const auto& table = *engine.descriptor;
  std::string cutoff = std::string(table.id) + ".filter.cutoff";
  if (oscillate::native_dsp::find_parameter(table, cutoff))
    return cutoff;
  for (const auto& parameter : table.parameters) {
    if (parameter.automatable && !oscillate::native_dsp::is_discrete(parameter) &&
        parameter.timing == oscillate::native_dsp::ParameterTiming::live)
      return std::string(parameter.id);
  }
  throw std::runtime_error("engine has no automatable parameter");
}

// Every registered engine plays through routed playback and accepts a live edit.
void every_engine_plays_live() {
  for (const auto* engine : synth::engines()) {
    Directory directory;
    const auto root = directory.path() / "song";
    project::Snapshot saved = [&] {
      auto session = take(app::ProjectSession::create(root));
      const auto track = take(session->instruments().add_synth_track());
      REQUIRE_OK(session->instruments().select_native_instrument(track, engine->descriptor->id),
                 "engine selected");
      REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip was not added");
      const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
      const auto key = bow_test::sounding_key(engine->descriptor->id, 36);
      REQUIRE_OK(session->notes().add_note(clip, {0}, static_cast<std::uint8_t>(key)),
                 "note saved");
      REQUIRE_OK(session->saving().save(), "note saved");
      return session->snapshot();
    }();
    Gate gate;
    auto model = take(project::ProjectModel::open(saved, gate));
    app::RoutedPlaybackControl control(model, limits());
    REQUIRE_OK(control.submit_current(), "graph prepared");
    auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
    control.bind_live_source(*source);
    audio::TransportCursor transport;
    std::array<engine::StereoFrame, 64> stereo{};
    std::array<float, 128> samples{};
    const auto device = source->render_source();
    double sounded = 0.0;
    for (int block = 0; block < 1'500 && sounded <= 1e-3; ++block) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
                 audio::BlockResult::rendered, "block rendered");
      sounded += block_power(samples);
    }
    REQUIRE_GT(sounded, 1e-3, std::string(engine->descriptor->id) + " is audible");
    control.observe();
    const auto track = saved.tracks.front().id;
    auto values = take(oscillate::native_inserts::resolve_native_instrument(
                           std::get<project::NativeInstrumentPatch>(
                               model.snapshot()->tracks.front().instrument_source)))
                      .values;
    const auto edited_id = automated_parameter(*engine);
    const auto edited_index =
        take(oscillate::native_dsp::find_parameter(*engine->descriptor, edited_id));
    values[edited_index] =
        oscillate::native_dsp::from_normalised(engine->descriptor->parameters[edited_index], 0.25);
    auto patch = take(oscillate::native_inserts::make_native_patch(*engine, values));
    auto edited = take(model.apply({project::SetNativeInstrumentPatch{track, patch}}));
    REQUIRE_OK(control.begin_edit(edited, "synth parameter"),
               "live edit accepted without a restart");
    for (int block = 0; block < 4; ++block) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
                 audio::BlockResult::rendered, "post-edit block rendered");
    }
    control.observe();
    REQUIRE_EQ(control.status().active_graph_revision, edited.snapshot->revision,
               std::string(engine->descriptor->id) + " acknowledges the live edit");
    control.unbind_live_source();
  }
}

void engine_switch_keeps_old_sound_and_reports_saved_state_until_source_replaced() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto saved = synth_project(root);
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "initial graph prepared");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
  control.bind_live_source(*source);
  const auto track = saved.tracks.front().id;
  auto* old_runtime = source->synth_runtime(track);
  REQUIRE_NE(old_runtime, nullptr, "initial synth runtime exists");
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 64> stereo{};
  std::array<float, 128> samples{};
  const auto device = source->render_source();
  for (int block = 0; block < 8; ++block)
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "old synth rendered before the switch");
  control.observe();
  const auto active_revision = control.status().active_graph_revision;

  const auto* ember = synth::find_engine("ember");
  REQUIRE_NE(ember, nullptr, "Ember is registered");
  const auto ember_patch =
      take(oscillate::native_inserts::native_patch_for_preset(*ember, std::nullopt));
  auto candidate = saved;
  candidate.revision = saved.revision + 1;
  candidate.tracks.front().instrument_source = ember_patch;
  require(control.can_capture_source_edit(candidate),
          "an engine change is eligible for a prepared source handoff");
  candidate.tracks.front().midi_clips.front().notes.front().velocity -= 1;
  require(control.can_capture_source_edit(candidate),
          "an engine change with a MIDI edit switches with its source too (ADR-0098)");
  const auto switched = take(model.apply({project::SetNativeInstrumentPatch{track, ember_patch}}));
  const auto offered = control.begin_edit(switched, "switch synth engine");
  REQUIRE_TRUE(!offered, "a source-only engine switch must report that it is unapplied");
  REQUIRE_NE(offered.error().message.find("saved but not audible"), std::string::npos,
             "a source-only engine switch must report that it is unapplied");
  const auto status = control.status();
  REQUIRE_EQ(status.application, app::PlaybackApplication::failed,
             "saved and audible revisions stay separate");
  REQUIRE_EQ(status.model_revision, switched.snapshot->revision,
             "saved and audible revisions stay separate");
  REQUIRE_EQ(status.active_graph_revision, active_revision,
             "saved and audible revisions stay separate");
  REQUIRE_OK(status.failure, "saved and audible revisions stay separate");
  REQUIRE_EQ(source->synth_runtime(track), old_runtime,
             "the old runtime remains bound to active playback");
  REQUIRE_EQ(old_runtime->engine().descriptor->id, synth::default_engine().descriptor->id,
             "the old runtime remains bound to active playback");
  counted_allocations = 0;
  counting_allocations = true;
  double old_sound = 0.0;
  for (int block = 0; block < 8; ++block) {
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "old synth continues without a transport stop");
    old_sound += block_power(samples);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "failed switch neither allocates nor silences the held note");
  REQUIRE_GT(old_sound, 0.0, "failed switch neither allocates nor silences the held note");
  REQUIRE_TRUE(!control.retry(), "retry cannot falsely publish the old source as the new engine");
  control.unbind_live_source();

  // A newly prepared source reads the saved choice after a deliberate restart.
  app::RoutedPlaybackControl restarted(model, limits());
  REQUIRE_OK(restarted.submit_current(), "restarted graph prepared");
  auto replacement = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), restarted, 64));
  REQUIRE_NE(replacement->synth_runtime(track), nullptr,
             "restart prepares the saved engine rather than reusing the old player");
  REQUIRE_EQ(replacement->synth_runtime(track)->engine().descriptor->id, "ember",
             "restart prepares the saved engine rather than reusing the old player");
}

void unknown_engine_fails_preparation_without_touching_the_patch() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto saved = synth_project(root);
  std::get<project::NativeInstrumentPatch>(saved.tracks.front().instrument_source).engine =
      "future-synth";
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "graph prepares without the source");
  auto prepared = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64);
  REQUIRE_TRUE(!prepared, "an unavailable engine is reported by name");
  REQUIRE_NE(prepared.error().message.find("future-synth"), std::string::npos,
             "an unavailable engine is reported by name");
  REQUIRE_EQ(
      std::get<project::NativeInstrumentPatch>(model.snapshot()->tracks.front().instrument_source)
          .engine,
      "future-synth", "the saved patch is kept for a newer build");
}

// Comp (ADR-0083, ADR-0084) on a synth track over a clip of chords held a
// bar each, every voice held a bar from the downbeat so the sound never
// stops, and every change pushed: it sees each next chord coming.
project::Snapshot comp_project(const fs::path& root) {
  constexpr auto bar = 4 * timeline::ticks_per_quarter;
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  std::vector<project::NativeInstrumentParameter> pads{{"comp.push_chance", 1.0}};
  for (const std::string_view lane : {"low", "mid", "top"}) {
    for (int step = 1; step <= 16; ++step)
      pads.push_back(
          {"comp." + std::string(lane) + "." + std::to_string(step), step == 1 ? 2.0 : 0.0});
  }
  for (int step = 1; step <= 16; ++step)
    pads.push_back({"comp.length." + std::to_string(step), 16.0});
  const auto slot = session->track_devices().add_note_effect(track, 0, "comp", pads);
  REQUIRE_OK(slot, "Comp was not added");
  std::vector<project::MidiNote> notes;
  std::uint64_t id = 1;
  const std::vector<std::vector<int>> chords{{45, 55, 60, 64}, {50, 57, 60, 65}};
  for (int repeat = 0; repeat < 4; ++repeat) {
    for (std::size_t index = 0; index < chords.size(); ++index) {
      const auto at = (std::int64_t{2} * repeat + static_cast<std::int64_t>(index)) * bar;
      for (const int key : chords[index])
        notes.push_back({take(project::NoteId::from_bits({0, id++})),
                         {at},
                         {at + bar},
                         static_cast<std::uint8_t>(key),
                         90});
    }
  }
  REQUIRE_OK(session->midi_clips().add_midi_clip_with_notes(track, {0}, {8 * bar}, notes),
             "the chords clip was not added");
  REQUIRE_OK(session->saving().save(), "Comp project did not save");
  return session->snapshot();
}

// Comp plays a clip live through the routed source, reading the notes ahead
// from the player's MIDI plan without allocating; an edit to Comp while
// playing goes to the running player in its automation plan: no rebuild, no
// held graph, no allocation on the callback and no break in the sound.
void comp_plays_ahead_and_takes_live_edits() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto saved = comp_project(root);
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "Comp graph did not prepare");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
  control.bind_live_source(*source);
  const auto& track = saved.tracks.front();
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 64> stereo{};
  std::array<float, 128> samples{};
  const auto device = source->render_source();
  double sounded = 0.0;
  counting_allocations = true;
  for (int block = 0; block < 40; ++block) {
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "Comp block failed to render");
    sounded += block_power(samples);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "Comp and its lookahead never allocate in the callback");
  REQUIRE_GT(sounded, 1e-3, "Comp plays the clip's chords");
  control.observe();
  auto parameters = track.note_effects.front().parameters;
  parameters.push_back({"comp.velocity", 80.0});
  auto edited = take(model.apply(
      {project::SetNoteEffectSettings{track.id, track.note_effects.front().id, parameters}}));
  REQUIRE_OK(control.begin_edit(edited, "comp"), "the Comp edit was accepted live");
  counting_allocations = true;
  for (int block = 0; block < 8; ++block) {
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "post-edit block failed to render");
    REQUIRE_GT(block_power(samples), 0.0, "the pad keeps sounding through the edit");
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "a Comp edit never allocates in the callback");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, edited.snapshot->revision,
             "the edit is audible once the player adopts it");
  REQUIRE_EQ(source->synth_player(track.id)->applied_plan_revision(), edited.snapshot->revision,
             "the player acknowledged the exact revision");
  control.unbind_live_source();
}

} // namespace

int main() {
  try {
    // Concert, Fret, Wren and Bow play their synthetic libraries, notes held whole in
    // memory, so live playback never waits on the disk.
    (void)concert_test::install(concert_test::resident_note_seconds);
    (void)fret_test::install(fret_test::resident_note_seconds);
    (void)wren_test::install();
    (void)bow_test::install();
    synth_track_plays_and_takes_live_edits_without_restart();
    native_automation_edits_apply_without_restarting_voices();
    native_automation_mailbox_retries_latest_saved_revision();
    every_engine_plays_live();
    engine_switch_keeps_old_sound_and_reports_saved_state_until_source_replaced();
    unknown_engine_fails_preparation_without_touching_the_patch();
    comp_plays_ahead_and_takes_live_edits();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "routed synth contract passed\n";
  return 0;
}
