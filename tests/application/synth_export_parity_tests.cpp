#include "../native_dsp/bow_test_library.hpp"
#include "../native_dsp/concert_test_library.hpp"
#include "../native_dsp/fret_test_library.hpp"
#include "../native_dsp/wren_test_library.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace exporting = oscillate::exporting;
namespace synth = oscillate::native_dsp::synth;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-synth-export") {}
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

// Interleaved float32 samples from the exported WAV's data chunk.
using oscillate::test::read_float_wav;

// The parameter a platform test automates or edits live: the engine's
// filter cutoff when it has one, else its first continuous automatable one.
std::string automated_parameter(const synth::SynthEngine& engine) {
  const auto& table = *engine.descriptor;
  // An engine with several filters names them filter.1, filter.2, ...
  for (const char* name : {".filter.cutoff", ".filter.1.cutoff"}) {
    const std::string cutoff = std::string(table.id) + name;
    if (oscillate::native_dsp::find_parameter(table, cutoff)) {
      return cutoff;
    }
  }
  for (const auto& parameter : table.parameters) {
    if (parameter.automatable && !oscillate::native_dsp::is_discrete(parameter) &&
        parameter.timing == oscillate::native_dsp::ParameterTiming::live) {
      return std::string(parameter.id);
    }
  }
  throw std::runtime_error("engine has no automatable parameter");
}

// Every registered engine must export and play identically through the same
// shared session, playback and export layers.
using SteppedPoint = std::pair<std::string_view, double>;

std::vector<float> export_nulls_against_live_playback(const synth::SynthEngine& engine,
                                                      std::size_t preset,
                                                      std::span<const SteppedPoint> stepped = {},
                                                      bool pedals = false) {
  const std::string id(engine.descriptor->id);
  const std::string cutoff = automated_parameter(engine);
  std::cout << id << " (" << engine.descriptor->presets[preset].name << (pedals ? ", pedals" : "")
            << "): ";
  Directory directory;
  const auto root = directory.path() / "song";
  project::Snapshot saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    const auto track = take(session->instruments().add_synth_track());
    REQUIRE_OK(session->instruments().select_native_instrument(track, id, preset), "preset");
    REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
    const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
    REQUIRE_TRUE(session->notes()
                     .add_note(clip, {0}, static_cast<std::uint8_t>(bow_test::sounding_key(id, 36)))
                     .has_value(),
                 "overlapping notes");
    REQUIRE_TRUE(session->notes()
                     .add_note(clip, {timeline::ticks_per_quarter / 2},
                               static_cast<std::uint8_t>(bow_test::sounding_key(id, 43)))
                     .has_value(),
                 "overlapping notes");
    if (pedals) {
      // ADR-0035: the sustain pedal holds both notes for a quarter note,
      // three times their length, and half the soft pedal softens the second.
      require(session->pedals()
                      .paint_pedal_span(clip, project::MidiPedal::sustain, {0},
                                        {timeline::ticks_per_quarter}, 1.0)
                      .has_value() &&
                  session->pedals()
                      .paint_pedal_span(clip, project::MidiPedal::soft,
                                        {timeline::ticks_per_quarter / 4},
                                        {timeline::ticks_per_quarter}, 1.0)
                      .has_value(),
              "pedal lanes");
    }
    const auto& parameter =
        engine.descriptor
            ->parameters[*oscillate::native_dsp::find_parameter(*engine.descriptor, cutoff)];
    (void)take(session->parameter_automation().set_native_automation_point(
        track, cutoff, 0, oscillate::native_dsp::from_normalised(parameter, 0.3)));
    (void)take(session->parameter_automation().set_native_automation_point(
        track, cutoff, 1, oscillate::native_dsp::from_normalised(parameter, 0.8)));
    // Stepped lanes on discrete parameters (ADR-0034).
    for (const auto& [id, value] : stepped) {
      (void)take(session->parameter_automation().set_native_automation_point(track, id, 0, value));
    }
    REQUIRE_OK(session->saving().save(), "project saved");
    return session->snapshot();
  }();

  const auto output = directory.path() / "master.wav";
  {
    Gate gate;
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(saved, gate));
    REQUIRE_EQ(take(exporting::render_wav_master(saved, store, std::move(authority),
                                                 {output, exporting::WavFormat::float32})),
               exporting::PublishOutcome::durable, "master export published");
  }
  const auto stems = directory.path() / "stems";
  {
    Gate gate;
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(saved, gate));
    REQUIRE_EQ(take(exporting::render_wav_stems(saved, store, std::move(authority),
                                                {stems, exporting::WavFormat::float32})),
               exporting::PublishOutcome::durable, "stem export published");
  }
  const auto stem = stems / ("track-" + saved.tracks.front().id.to_string() + ".wav");
  require(fs::exists(stem), "the synth track has its own stem");
  const auto stem_samples = read_float_wav(stem);
  const auto exported = read_float_wav(output);
  REQUIRE_EQ(stem_samples, exported, "a lone synth's stem equals the master");
  const std::size_t frames = exported.size() / 2;
  // The second sixteenth-note ends at 3/8 s (120 BPM); the tail check below
  // proves the release is covered too.
  REQUIRE_GT(frames, 48'000 * 3 / 8, "export covers the notes and their release tail");

  double peak = 0.0;
  for (const float sample : exported)
    peak = std::max(peak, std::fabs(static_cast<double>(sample)));
  REQUIRE_GT(peak, 0.01, "the export is audible");
  for (const std::size_t block_frames : {64U, 128U}) {
    Gate gate;
    auto model = take(project::ProjectModel::open(saved, gate));
    app::RoutedPlaybackControl control(model, limits(block_frames));
    REQUIRE_OK(control.submit_current(), "live graph prepared");
    auto source =
        take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, block_frames));
    audio::TransportCursor transport;
    std::vector<engine::StereoFrame> stereo(block_frames);
    std::vector<float> samples(2 * block_frames);
    const auto device = source->render_source();
    std::vector<float> live;
    live.reserve(exported.size() + samples.size());
    while (live.size() < exported.size()) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
                 audio::BlockResult::rendered, "live block rendered");
      live.insert(live.end(), samples.begin(), samples.end());
    }
    double largest = 0.0;
    for (std::size_t i = 0; i < exported.size(); ++i)
      largest = std::max(largest, std::fabs(static_cast<double>(exported[i]) - live[i]));
    std::cout << block_frames << " frames: export vs live peak " << peak << ", largest difference "
              << largest << '\n';
    REQUIRE_LT(largest, 1e-6, "offline export nulls against live playback");
  }
  double tail = 0.0;
  for (std::size_t i = exported.size() - 256; i < exported.size(); ++i)
    tail = std::max(tail, std::fabs(static_cast<double>(exported[i])));
  REQUIRE_LT(tail, 1e-4, "the export ends after the release tail, not inside it");
  return exported;
}

// ADR-0048: every engine on its own track, rendered live on the callback
// alone and with audio workers, must produce the same bits.
void parallel_live_playback_is_bit_identical() {
  Directory directory;
  const auto root = directory.path() / "band";
  project::Snapshot saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    int pitch = 36;
    for (const auto* engine : synth::engines()) {
      const auto track = take(session->instruments().add_synth_track());
      require(session->instruments()
                  .select_native_instrument(track, engine->descriptor->id, 0)
                  .has_value(),
              "engine selected");
      REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
      const auto row = session->tracks().track_index(track);
      REQUIRE_OK(row, "track row");
      const auto clip = session->snapshot().tracks[*row].midi_clips.front().id;
      REQUIRE_OK(session->notes().add_note(clip, {0}, static_cast<std::uint8_t>(pitch)), "notes");
      REQUIRE_TRUE(session->notes()
                       .add_note(clip, {timeline::ticks_per_quarter / 2},
                                 static_cast<std::uint8_t>(pitch + 7))
                       .has_value(),
                   "notes");
      pitch += 5;
    }
    REQUIRE_OK(session->saving().save(), "project saved");
    return session->snapshot();
  }();
  constexpr std::size_t block_frames = 128;
  constexpr std::size_t total_frames = 48'000;
  const auto tracks = saved.tracks.size();
  const engine::RoutedMixerLimits band_limits{{1, 64},
                                              block_frames,
                                              (tracks + 4) * block_frames *
                                                  sizeof(engine::StereoFrame),
                                              (tracks + 4) * 65 * sizeof(engine::StereoFrame)};
  const auto render = [&](engine::AudioWorkerPool* workers) {
    Gate gate;
    auto model = take(project::ProjectModel::open(saved, gate));
    app::RoutedPlaybackControl control(model, band_limits);
    REQUIRE_OK(control.submit_current(), "live graph prepared");
    auto source =
        take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, block_frames));
    audio::TransportCursor transport;
    std::vector<engine::StereoFrame> stereo(block_frames);
    std::vector<float> samples(2 * block_frames);
    const auto device = source->render_source(workers);
    std::vector<float> live;
    live.reserve(2 * total_frames + samples.size());
    const auto started = std::chrono::steady_clock::now();
    while (live.size() < 2 * total_frames) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
                 audio::BlockResult::rendered, "live block rendered");
      live.insert(live.end(), samples.begin(), samples.end());
    }
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - started;
    std::cout << "one second of " << tracks << " engines with "
              << (workers == nullptr ? 0 : workers->attached_workers())
              << " workers: " << elapsed.count() << " ms\n";
    return live;
  };
  const auto alone = render(nullptr);
  double peak = 0.0;
  for (const float sample : alone)
    peak = std::max(peak, std::fabs(static_cast<double>(sample)));
  REQUIRE_GT(peak, 0.01, "the band is audible");
  for (const std::size_t count : {std::size_t{1}, std::size_t{3}, std::size_t{7}}) {
    engine::AudioWorkerPool pool(count);
    pool.start_owned_threads(count);
    const auto parallel = render(&pool);
    pool.stop_owned_threads();
    std::size_t differing = 0;
    for (std::size_t index = 0; index < alone.size(); ++index)
      differing += std::bit_cast<std::uint32_t>(alone[index]) !=
                   std::bit_cast<std::uint32_t>(parallel[index]);
    std::cout << tracks << " engines, " << count << " workers: " << differing
              << " differing samples\n";
    REQUIRE_EQ(differing, 0, "parallel live playback differs from the callback alone");
  }
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
    parallel_live_playback_is_bit_identical();
    for (const auto* engine : synth::engines()) {
      const auto plain = export_nulls_against_live_playback(*engine, 2);
      const auto pedalled = export_nulls_against_live_playback(*engine, 2, {}, true);
      REQUIRE_NE(pedalled, plain, "the pedals change the export");
    }
    // A built-in sequencer must also null: Torque's pattern runs while the
    // clip's notes are held.
    const auto* torque = synth::find_engine("torque");
    REQUIRE_NE(torque, nullptr, "Torque is registered");
    const auto presets = torque->descriptor->presets;
    const auto sequenced = std::ranges::find_if(
        presets, [](const auto& entry) { return entry.name == "Seq Acid Line"; });
    REQUIRE_NE(sequenced, presets.end(), "Torque has a sequencer preset");
    // Its pattern is also reshaped by stepped lanes: step 1's note and the
    // acid wave switch.
    const std::array<SteppedPoint, 2> stepped{
        {{"torque.seq.s1.note", 7.0}, {"torque.acid.wave", 1.0}}};
    (void)export_nulls_against_live_playback(
        *torque, static_cast<std::size_t>(std::distance(presets.begin(), sequenced)), stepped);
    // Drift's tempo-synced grains with stepped Freeze and interval lanes.
    const auto* drift = synth::find_engine("drift");
    REQUIRE_NE(drift, nullptr, "Drift is registered");
    const auto drift_presets = drift->descriptor->presets;
    const auto synced = std::ranges::find_if(
        drift_presets, [](const auto& entry) { return entry.name == "Stutter Clock"; });
    REQUIRE_NE(synced, drift_presets.end(), "Drift has a synced preset");
    const std::array<SteppedPoint, 2> grains{
        {{"drift.freeze", 1.0}, {"drift.grain.intervals", 2.0}}};
    (void)export_nulls_against_live_playback(
        *drift, static_cast<std::size_t>(std::distance(drift_presets.begin(), synced)), grains);
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "synth export parity passed\n";
  return 0;
}
