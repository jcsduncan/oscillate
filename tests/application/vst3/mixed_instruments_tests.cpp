#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace fs = std::filesystem;
namespace plugins = oscillate::plugins;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;
using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-mixed-instruments") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

app::Vst3InsertIdentity identity(const plugins::ScanResult& scanned) {
  const auto& klass = scanned.metadata.classes.front();
  return {klass.id,
          klass.vendor,
          klass.name,
          klass.version,
          scanned.binary.canonical_bundle.string(),
          scanned.binary.sha256};
}

struct Rendered {
  std::vector<engine::StereoFrame> samples;
  std::uint32_t latency = 0;
};

Rendered render(const fs::path& root, project::Snapshot snapshot,
                const plugins::ScanResult& scanned, std::size_t frames,
                std::optional<project::TrackId> native_track,
                std::optional<project::TrackId> vst3_track,
                std::optional<project::PluginInstanceId> plugin) {
  Gate gate;
  auto model = take(project::ProjectModel::open(std::move(snapshot), gate));
  std::unique_ptr<plugins::Vst3InstrumentTrackProvider> provider;
  if (plugin) {
    provider = std::make_unique<plugins::Vst3InstrumentTrackProvider>();
    provider->set_scan_receipt(*plugin, scanned);
  }
  app::RoutedPlaybackControl control(model, {{1, 64}, frames, 1 << 20, 1 << 20}, provider.get());
  const auto prepared = control.submit_current();
  REQUIRE_OK(prepared, "mixed source graph did not prepare");
  require(!vst3_track || control.has_bound_instrument_track(*vst3_track),
          "VST3 source lost its exact graph binding");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, frames));
  require(!native_track || source->synth_runtime(*native_track) != nullptr,
          "native synth source was not prepared on its own track");
  control.bind_live_source(*source);
  const auto device = source->render_source();
  audio::TransportCursor cursor;
  std::vector<engine::StereoFrame> stereo(frames);
  std::vector<float> interleaved(frames * 2);
  Rendered result;
  result.latency = control.master_output_latency_frames();
  result.samples.reserve(frames * 8);
  for (int block = 0; block < 8; ++block) {
    counted_allocations = 0;
    counting_allocations = true;
    const auto status = audio::render_transport_block(device, cursor, true, stereo, interleaved);
    counting_allocations = false;
    REQUIRE_EQ(status, audio::BlockResult::rendered,
               "mixed source callback failed, allocated or faulted");
    REQUIRE_EQ(counted_allocations, 0, "mixed source callback failed, allocated or faulted");
    REQUIRE_TRUE(!control.processor_fault(), "mixed source callback failed, allocated or faulted");
    for (std::size_t frame = 0; frame < frames; ++frame)
      result.samples.push_back({interleaved[2 * frame], interleaved[2 * frame + 1]});
  }
  control.unbind_live_source();
  if (provider)
    require(provider->stop_after_join(), "VST3 source host did not stop after callbacks");
  return result;
}

void playable_mixed_source(const plugins::ScanResult& scanned, std::size_t frames) {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto native_track = take(session->instruments().add_synth_track());
  REQUIRE_OK(session->midi_clips().add_midi_clip(native_track, {0}),
             "native synth MIDI clip was not created");
  const auto native_clip = session->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session->notes().add_note(native_clip, {0}, 60), "native synth note was not saved");
  REQUIRE_OK(session->saving().save(), "native synth note was not saved");
  const auto native_only = session->snapshot();
  const auto vst3_track =
      take(session->instruments().create_vst3_instrument_track(identity(scanned)));
  REQUIRE_OK(session->midi_clips().add_midi_clip(vst3_track, {0}),
             "VST3 source MIDI clip was not created");
  const auto vst3_clip = session->snapshot().tracks[1].midi_clips.front().id;
  REQUIRE_OK(session->notes().add_note(vst3_clip, {0}, 60), "VST3 source note was not saved");
  REQUIRE_OK(session->saving().save(), "VST3 source note was not saved");
  const auto mixed = session->snapshot();
  const auto plugin = std::get<project::Vst3PluginInstance>(mixed.tracks[1].instrument_source).id;
  REQUIRE_NE(native_track, vst3_track, "mixed instrument MIDI or source identities were conflated");
  REQUIRE_NE(native_clip, vst3_clip, "mixed instrument MIDI or source identities were conflated");
  REQUIRE_NE(mixed.tracks[0].midi_clips.front().notes.front().id,
             mixed.tracks[1].midi_clips.front().notes.front().id,
             "mixed instrument MIDI or source identities were conflated");
  session.reset();
  auto reopened = take(app::ProjectSession::open(root));
  REQUIRE_EQ(reopened->snapshot().tracks, mixed.tracks,
             "mixed native and VST3 source identities or MIDI did not reopen");
  // A third source plays beside them too (ADR-0050).
  require(reopened->instruments().create_vst3_instrument_track(identity(scanned)).has_value() &&
              reopened->snapshot().tracks.size() == 3,
          "a third source beside mixed instruments was refused");
  REQUIRE_OK(reopened->history().undo(), "the third source did not undo");

  Gate reference_gate;
  auto vst3_model = take(project::ProjectModel::open(mixed, reference_gate));
  (void)take(vst3_model.apply({project::RemoveTrack{native_track}}));
  const auto vst3_only = *vst3_model.snapshot();
  const auto native_audio = render(root, native_only, scanned, frames, native_track, {}, {});
  const auto vst3_audio = render(root, vst3_only, scanned, frames, {}, vst3_track, plugin);
  const auto mixed_audio =
      render(root, reopened->snapshot(), scanned, frames, native_track, vst3_track, plugin);
  REQUIRE_EQ(mixed_audio.samples.size(), native_audio.samples.size(),
             "mixed source render lengths differ");
  REQUIRE_EQ(mixed_audio.samples.size(), vst3_audio.samples.size(),
             "mixed source render lengths differ");
  const auto align = [](const Rendered& audio, std::size_t sample, std::uint32_t target_latency) {
    const auto offset = target_latency - audio.latency;
    return sample < offset ? engine::StereoFrame{} : audio.samples[sample - offset];
  };
  REQUIRE_GE(mixed_audio.latency, native_audio.latency,
             "mixed source latency omitted an independent source path");
  REQUIRE_GE(mixed_audio.latency, vst3_audio.latency,
             "mixed source latency omitted an independent source path");
  bool native_sounded = false;
  bool plugin_sounded = false;
  for (std::size_t frame = 0; frame < mixed_audio.samples.size(); ++frame) {
    const auto native = align(native_audio, frame, mixed_audio.latency);
    const auto vst3 = align(vst3_audio, frame, mixed_audio.latency);
    const auto actual = mixed_audio.samples[frame];
    native_sounded = native_sounded || std::fabs(native.left) > 1e-5F;
    plugin_sounded = plugin_sounded || std::fabs(vst3.left) > 1e-5F;
    REQUIRE_LT(std::fabs(actual.left - (native.left + vst3.left)), tolerance::float_processing_f,
               "mixed audio differs from independently rendered source sum");
    REQUIRE_LT(std::fabs(actual.right - (native.right + vst3.right)), tolerance::float_processing_f,
               "mixed audio differs from independently rendered source sum");
  }
  REQUIRE_TRUE(native_sounded, "mixed source playback omitted the native synth or VST3 instrument");
  REQUIRE_TRUE(plugin_sounded, "mixed source playback omitted the native synth or VST3 instrument");
  const float fixture_note = 0.5F * (100.0F / 127.0F);
  REQUIRE_LT(std::fabs(vst3_audio.samples[4].left - fixture_note), tolerance::float_processing_f,
             "VST3 source MIDI did not produce the fixture's known sample");
  REQUIRE_LT(std::fabs(vst3_audio.samples[4].right + fixture_note), tolerance::float_processing_f,
             "VST3 source MIDI did not produce the fixture's known sample");
}

void two_vst3_beside_native_play(const plugins::ScanResult& scanned) {
  Directory directory;
  auto session = take(app::ProjectSession::create(directory.path() / "closed"));
  (void)take(session->instruments().create_vst3_instrument_track(identity(scanned)));
  (void)take(session->instruments().create_vst3_instrument_track(identity(scanned)));
  (void)take(session->instruments().add_synth_track());
  const auto snapshot = session->snapshot();
  const auto first = std::get<project::Vst3PluginInstance>(snapshot.tracks[0].instrument_source);
  const auto second = std::get<project::Vst3PluginInstance>(snapshot.tracks[1].instrument_source);
  Gate gate;
  auto model = take(project::ProjectModel::open(snapshot, gate));
  plugins::Vst3InstrumentTrackProvider provider;
  provider.set_scan_receipt(first.id, scanned);
  provider.set_scan_receipt(second.id, scanned);
  app::RoutedPlaybackControl control(model, {{1, 64}, 64, 1 << 20, 1 << 20}, &provider);
  // Any mix of instrument sources plays together (ADR-0050).
  REQUIRE_OK(control.submit_current(), "two VST3 sources beside a native synth were refused");

  auto two_native = take(app::ProjectSession::create(directory.path() / "two-native"));
  (void)take(two_native->instruments().add_synth_track());
  (void)take(two_native->instruments().add_synth_track());
  REQUIRE_OK(two_native->instruments().create_vst3_instrument_track(identity(scanned)),
             "a VST3 instrument beside two native synths was refused");
  REQUIRE_EQ(two_native->snapshot().tracks.size(), 3,
             "a VST3 instrument beside two native synths was refused");
}
} // namespace

int main() {
  try {
    plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
    const auto scanned = scanner.scan(fs::path(VST3_INSTRUMENT_BUNDLE));
    REQUIRE_TRUE(scanned.ok(), "mixed source fixture did not scan");
    REQUIRE_EQ(scanned.metadata.classes.size(), 1, "mixed source fixture did not scan");
    for (const std::size_t frames : {64U, 128U})
      playable_mixed_source(scanned, frames);
    two_vst3_beside_native_play(scanned);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
