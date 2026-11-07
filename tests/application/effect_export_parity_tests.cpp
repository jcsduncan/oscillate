#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

// ADR-0043: every built-in effect, on a track, on the master and (when it
// reads a detector) keyed by a sidechain, exports
// exactly what live routed playback plays, including its automation. With
// one source track, that track's stem (its routes and the master chain
// included) is the master, so it nulls against live playback too.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace effects = oscillate::native_dsp::effects;
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
  Directory() : TempDir("oscillate-effect-export") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

// Delay compensation covers at least a block's worth of the parallel paths
// and every frame of the effect's own latency (a keyed effect's key is
// delayed to meet it).
constexpr std::uint32_t minimum_compensation_frames = 64;

engine::RoutedMixerLimits limits(std::size_t max_frames, std::uint32_t effect_latency) {
  const std::uint32_t compensation = std::max(minimum_compensation_frames, effect_latency);
  return {{1, compensation},
          max_frames,
          7ULL * max_frames * sizeof(engine::StereoFrame),
          5ULL * (compensation + 1ULL) * sizeof(engine::StereoFrame)};
}

using oscillate::test::read_float_wav;

// The largest sample difference an export may show against live playback.
constexpr double null_tolerance = 1e-6;

// The effect's first continuous, automatable, live parameter.
std::string automated_parameter(const effects::EffectEngine& engine) {
  for (const auto& parameter : engine.descriptor->parameters) {
    if (parameter.automatable && !synth::is_discrete(parameter) &&
        parameter.timing == synth::ParameterTiming::live)
      return std::string(parameter.id);
  }
  return {};
}

enum class Placement { track, master, keyed };

std::vector<float> export_nulls_against_live(const effects::EffectEngine& effect,
                                             Placement placement) {
  const std::string id(effect.descriptor->id);
  std::cout << id
            << (placement == Placement::track    ? " on a track: "
                : placement == Placement::master ? " on the master: "
                                                 : " keyed by a sidechain: ");
  Directory directory;
  const auto root = directory.path() / "song";
  project::Snapshot saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    const auto track = take(session->instruments().add_synth_track());
    REQUIRE_OK(session->instruments().select_native_instrument(track, "prism", 2), "synth preset");
    REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
    const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
    REQUIRE_OK(session->notes().add_note(clip, {0}, 48), "notes");
    REQUIRE_OK(session->notes().add_note(clip, {timeline::ticks_per_quarter / 2}, 55), "notes");
    if (placement == Placement::keyed) {
      // The effect sits on a bus fed by the synth; a second synth keys it.
      const auto bus = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
      REQUIRE_OK(session->routing().set_main_output(track, bus), "synth routed to the bus");
      const auto slot = take(session->effects().add_effect_engine(bus, 0, id));
      const auto key = take(session->instruments().add_synth_track());
      REQUIRE_OK(session->midi_clips().add_midi_clip(key, {0}), "key clip");
      const auto key_clip = session->snapshot().tracks.back().midi_clips.front().id;
      REQUIRE_OK(session->notes().add_note(key_clip, {0}, 36), "key notes");
      REQUIRE_OK(session->notes().add_note(key_clip, {timeline::ticks_per_quarter}, 36),
                 "key notes");
      (void)take(session->routing().add_sidechain(key, bus, slot, 1.0));
      REQUIRE_OK(session->saving().save(), "project saved");
      return session->snapshot();
    }
    const project::SignalPathId path = placement == Placement::track
                                           ? project::SignalPathId{track}
                                           : project::SignalPathId{project::MasterSignalPath{}};
    const auto slot = take(session->effects().add_effect_engine(path, 0, id));
    const auto parameter = automated_parameter(effect);
    if (placement == Placement::track && !parameter.empty()) {
      const auto& table = *effect.descriptor;
      const auto& descriptor = table.parameters[*synth::find_parameter(table, parameter)];
      (void)take(session->parameter_automation().set_effect_automation_point(
          track, slot, parameter, 0, synth::from_normalised(descriptor, 0.25)));
      (void)take(session->parameter_automation().set_effect_automation_point(
          track, slot, parameter, 1, synth::from_normalised(descriptor, 0.75)));
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
  const auto exported = read_float_wav(output);
  std::vector<float> stem;
  if (placement != Placement::keyed) {
    const auto stems = directory.path() / "stems";
    Gate gate;
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(saved, gate));
    REQUIRE_EQ(take(exporting::render_wav_stems(saved, store, std::move(authority),
                                                {stems, exporting::WavFormat::float32})),
               exporting::PublishOutcome::durable, "stems published");
    stem = read_float_wav(stems / ("track-" + saved.tracks.front().id.to_string() + ".wav"));
    REQUIRE_EQ(stem.size(), exported.size(), "the one-source stem is as long as the master");
  }
  double peak = 0.0;
  for (const float sample : exported)
    peak = std::max(peak, std::fabs(static_cast<double>(sample)));
  REQUIRE_GT(peak, 1e-4, "the export is audible");
  for (const std::size_t block_frames : {64U, 128U}) {
    Gate gate;
    auto model = take(project::ProjectModel::open(saved, gate));
    app::RoutedPlaybackControl control(
        model, limits(block_frames, effect.latency_frames(synth::default_values(*effect.descriptor),
                                                          static_cast<double>(saved.sample_rate))));
    auto submitted = control.submit_current();
    REQUIRE_OK(submitted, submitted ? "" : "live graph prepared: " + submitted.error().message);
    auto source =
        take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, block_frames));
    audio::TransportCursor transport;
    std::vector<engine::StereoFrame> stereo(block_frames);
    std::vector<float> samples(2 * block_frames);
    const auto device = source->render_source();
    // The live stream emits project sample N at stream frame N plus the
    // route's output latency; the export trims that latency. Here it is
    // the effect's own, as the engine reports it for the saved values.
    const auto latency = control.master_output_latency_frames();
    REQUIRE_EQ(latency,
               effect.latency_frames(synth::default_values(*effect.descriptor),
                                     static_cast<double>(saved.sample_rate)),
               "the live route's latency is the effect's: " + std::to_string(latency));
    const std::size_t offset = 2 * static_cast<std::size_t>(latency);
    std::vector<float> live;
    live.reserve(exported.size() + offset + samples.size());
    while (live.size() < exported.size() + offset) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
                 audio::BlockResult::rendered, "live block rendered");
      live.insert(live.end(), samples.begin(), samples.end());
    }
    double largest = 0.0;
    for (std::size_t i = 0; i < exported.size(); ++i)
      largest = std::max(largest, std::fabs(static_cast<double>(exported[i]) - live[offset + i]));
    std::cout << block_frames << " frames: peak " << peak << ", largest difference " << largest
              << "; ";
    REQUIRE_LT(largest, null_tolerance, "offline export nulls against live playback");
    double stem_largest = 0.0;
    for (std::size_t i = 0; i < stem.size(); ++i)
      stem_largest =
          std::max(stem_largest, std::fabs(static_cast<double>(stem[i]) - live[offset + i]));
    REQUIRE_LT(stem_largest, null_tolerance, "the stem export nulls against live playback");
  }
  std::cout << exported.size() / 2 << " frames\n";
  return exported;
}

} // namespace

int main() {
  try {
    for (const auto* effect : effects::engines()) {
      (void)export_nulls_against_live(*effect, Placement::track);
      (void)export_nulls_against_live(*effect, Placement::master);
      if (effect->sidechain)
        (void)export_nulls_against_live(*effect, Placement::keyed);
    }
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "effect export parity passed\n";
  return 0;
}
