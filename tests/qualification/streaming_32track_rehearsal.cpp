#include "allocation_probe.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/pipewire_output.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#if defined(OSCILLATE_QUALIFICATION_VST3)
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace synth = oscillate::native_dsp::synth;
#if defined(OSCILLATE_QUALIFICATION_VST3)
namespace plugins = oscillate::plugins;
#endif
namespace fs = std::filesystem;

constexpr std::size_t track_count = 32;
constexpr std::size_t sample_rate = oscillate::test::sample_rate_48k;
constexpr std::size_t default_source_seconds = 8;
constexpr std::size_t max_source_seconds = 120;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-streaming-32") {}
};

void put16(std::ofstream& out, std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value), static_cast<char>(value >> 8U)};
  out.write(bytes.data(), bytes.size());
}

void put32(std::ofstream& out, std::uint32_t value) {
  put16(out, static_cast<std::uint16_t>(value));
  put16(out, static_cast<std::uint16_t>(value >> 16U));
}

fs::path make_wav(const fs::path& directory, std::size_t track, std::size_t source_frames,
                  bool warp_fixture = false) {
  if (source_frames > (std::numeric_limits<std::uint32_t>::max() - 36U) / 2U)
    throw std::runtime_error("generated WAV exceeds RIFF size limit");
  const auto data_bytes = static_cast<std::uint32_t>(source_frames * 2U);
  const auto path = directory / ("source-" + std::to_string(track) + ".wav");
  std::ofstream out(path, std::ios::binary);
  out.write("RIFF", 4);
  put32(out, 36U + data_bytes);
  out.write("WAVEfmt ", 8);
  put32(out, 16);
  put16(out, 1);
  put16(out, 1);
  put32(out, sample_rate);
  put32(out, sample_rate * 2);
  put16(out, 2);
  put16(out, 16);
  out.write("data", 4);
  put32(out, data_bytes);
  std::array<char, 8'192> samples{};
  for (std::size_t start = 0; start < source_frames; start += samples.size() / 2) {
    const auto count = std::min(samples.size() / 2, source_frames - start);
    for (std::size_t i = 0; i < count; ++i) {
      const auto phase = ((start + i) * (track + 7U)) % 257U;
      auto sample = static_cast<std::uint16_t>(1'024 + phase * 16U);
      if (warp_fixture) {
        const auto frame = start + i;
        const double tone = 9'000.0 * std::sin(2.0 * std::numbers::pi *
                                               (220.0 + static_cast<double>(track) * 55.0) *
                                               static_cast<double>(frame) / sample_rate);
        const double click = frame % 12'000 == 6'000 ? 12'000.0 : 0.0;
        sample = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(tone + click)));
      }
      samples[i * 2] = static_cast<char>(sample);
      samples[i * 2 + 1] = static_cast<char>(sample >> 8U);
    }
    out.write(samples.data(), static_cast<std::streamsize>(count * 2));
  }
  if (!out)
    throw std::runtime_error("WAV fixture write failed");
  return path;
}

struct Options {
  std::size_t frames = 64;
  std::size_t repetitions = 3;
  std::size_t blocks = 2'000;
  std::size_t warmup = 100;
  std::size_t pace_multiplier = 1;
  std::size_t source_seconds = default_source_seconds;
  std::optional<std::uint64_t> pipewire_target;
  bool vst3_insert = false;
  bool vst3_instrument = false;
  std::size_t processed_clips = 0;
  bool native_synths = false;
};

struct Vst3Evidence {
  bool effect = false;
  bool instrument = false;
#if defined(OSCILLATE_QUALIFICATION_VST3)
  plugins::ScanResult effect_scan;
  plugins::ScanResult instrument_scan;
#endif
};

Vst3Evidence verify_plugin(bool effect, bool instrument) {
  if (!effect && !instrument)
    return {};
#if defined(OSCILLATE_QUALIFICATION_VST3)
  plugins::Vst3Scanner scanner(VST3_QUALIFICATION_HELPER);
  Vst3Evidence verified{};
  verified.effect = effect;
  verified.instrument = instrument;
  if (effect) {
    verified.effect_scan = scanner.scan(VST3_QUALIFICATION_GAIN_BUNDLE);
    if (!verified.effect_scan.ok() || verified.effect_scan.metadata.classes.size() != 1)
      throw std::runtime_error("verified qualification VST3 effect scan failed");
  }
  if (instrument) {
    verified.instrument_scan = scanner.scan(VST3_QUALIFICATION_INSTRUMENT_BUNDLE);
    if (!verified.instrument_scan.ok() || verified.instrument_scan.metadata.classes.size() != 1)
      throw std::runtime_error("verified qualification VST3 instrument scan failed");
  }
  return verified;
#else
  throw std::runtime_error("VST3 rehearsal needs OSCILLATE_BUILD_VST3_SCANNER=ON");
#endif
}

std::size_t positive_number(std::string_view text) {
  std::size_t value = 0;
  if (text.empty())
    throw std::runtime_error("option needs a positive number");
  for (const char digit : text) {
    if (digit < '0' || digit > '9' ||
        value > (1'000'000U - static_cast<std::size_t>(digit - '0')) / 10U)
      throw std::runtime_error("option must be from 1 to 1000000");
    value = value * 10 + static_cast<std::size_t>(digit - '0');
  }
  if (value == 0)
    throw std::runtime_error("option needs a positive number");
  return value;
}

Options parse(int argc, char** argv) {
  if (argc % 2 == 0)
    throw std::runtime_error("usage: rehearsal [--frames 64|128] [--repetitions N] "
                             "[--blocks N] [--warmup N] [--pace-multiplier N] "
                             "[--source-seconds 1..120] "
                             "[--pipewire-target SERIAL] [--vst3-insert 1] "
                             "[--vst3-instrument 1] [--processed-clips 4] "
                             "[--native-synths 1]");
  Options options;
  for (int i = 1; i < argc; i += 2) {
    const std::string_view key(argv[i]);
    if (key == "--pipewire-target") {
      std::uint64_t serial = 0;
      const std::string_view value(argv[i + 1]);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), serial);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || serial == 0)
        throw std::runtime_error("PipeWire target needs a positive object serial");
      options.pipewire_target = serial;
      continue;
    }
    const auto value = positive_number(argv[i + 1]);
    if (key == "--frames")
      options.frames = value;
    else if (key == "--repetitions")
      options.repetitions = value;
    else if (key == "--blocks")
      options.blocks = value;
    else if (key == "--warmup")
      options.warmup = value;
    else if (key == "--pace-multiplier")
      options.pace_multiplier = value;
    else if (key == "--source-seconds")
      options.source_seconds = value;
    else if (key == "--processed-clips")
      options.processed_clips = value;
    else if (key == "--native-synths") {
      if (value != 1)
        throw std::runtime_error("--native-synths accepts only 1");
      options.native_synths = true;
    } else if (key == "--vst3-insert") {
      if (value != 1)
        throw std::runtime_error("--vst3-insert accepts only 1");
      options.vst3_insert = true;
    } else if (key == "--vst3-instrument") {
      if (value != 1)
        throw std::runtime_error("--vst3-instrument accepts only 1");
      options.vst3_instrument = true;
    } else
      throw std::runtime_error("unknown option");
  }
  if (options.frames != 64 && options.frames != 128)
    throw std::runtime_error("frames must be 64 or 128");
  if (options.pace_multiplier > 16)
    throw std::runtime_error("pace multiplier must be from 1 to 16");
  if (options.pipewire_target && options.pace_multiplier != 1)
    throw std::runtime_error("pace multiplier only applies to the fake callback");
  if (options.pipewire_target && options.repetitions > 3)
    throw std::runtime_error("live PipeWire rehearsal is limited to three repetitions");
  if (options.source_seconds > max_source_seconds)
    throw std::runtime_error("source seconds must be from 1 to 120");
  if (options.processed_clips != 0 && options.processed_clips != 4)
    throw std::runtime_error("processed clips must be 4 when enabled");
  if (options.processed_clips && options.source_seconds < 2)
    throw std::runtime_error("processed clips need at least two seconds of source");
  if (options.processed_clips && options.pipewire_target)
    throw std::runtime_error("processed-clip rehearsal is fake-callback only");
  if (options.native_synths && options.vst3_instrument)
    throw std::runtime_error("VST3 instrument and other instrument sources cannot share this "
                             "routed playback path yet");
  if (options.native_synths && options.pipewire_target)
    throw std::runtime_error("native-synth mixed rehearsal is fake-callback only");
  if (options.blocks > std::numeric_limits<std::size_t>::max() - options.warmup)
    throw std::runtime_error("rehearsal block count exceeds supported range");
  const auto requested_blocks = options.blocks + options.warmup;
  if (requested_blocks > options.source_seconds * sample_rate / options.frames)
    throw std::runtime_error("rehearsal duration exceeds generated source");
  return options;
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

#if defined(OSCILLATE_QUALIFICATION_VST3)
app::Vst3InsertIdentity identity_from(const plugins::ScanResult& scan) {
  const auto& klass = scan.metadata.classes.front();
  return {.class_id = klass.id,
          .vendor = klass.vendor,
          .name = klass.name,
          .version = klass.version,
          .canonical_bundle_path = scan.binary.canonical_bundle.string(),
          .binary_sha256 = scan.binary.sha256};
}
#endif

std::unique_ptr<app::ProjectSession>
prepare_project(const fs::path& directory, const Vst3Evidence& vst3, std::size_t source_seconds,
                std::size_t processed_clips, bool native_synths) {
  const auto root = directory / "project";
  auto session = take(app::ProjectSession::create(root));
  const auto group_a = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  const auto group_b = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  const auto return_a = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  const auto return_b = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  const auto audio_track_count = track_count - (native_synths ? 2U : 0U);
  std::optional<project::TrackId> first_audio_track;
  std::optional<project::SendId> automated_send;
  for (std::size_t index = 0; index < audio_track_count; ++index) {
    const auto wav =
        make_wav(directory, index, source_seconds * sample_rate, index < processed_clips);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
      throw std::runtime_error("fixture WAV open failed");
    auto imported = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    const auto track = take(std::move(imported));
    if (index < processed_clips) {
      const auto found = std::ranges::find(session->snapshot().tracks, track, &project::Track::id);
      if (found == session->snapshot().tracks.end() || found->clips.size() != 1)
        throw std::runtime_error("processed fixture import lost its audio clip");
      project::ClipProcessing processing;
      processing.warp.mode =
          index % 2 == 0 ? project::WarpMode::repitch : project::WarpMode::complex;
      processing.warp.mode_overridden = true;
      if (index % 2 == 0)
        processing.reviewed_warp_markers.push_back(
            {{static_cast<std::int64_t>(sample_rate)},
             timeline::SamplePosition{static_cast<std::int64_t>(sample_rate * 5 / 4)}});
      else
        processing.pitch_semitones = index == 1 ? 2.0 : -3.0;
      take(session->audio_clips().set_audio_clip_processing(found->clips.front().id, processing));
    }
    if (!first_audio_track)
      first_audio_track = track;
    take(session->routing().set_main_output(track,
                                            index < audio_track_count / 2 ? group_a : group_b));
    const auto send = take(session->routing().add_send(
        track, index < audio_track_count / 2 ? return_a : return_b, 0.125));
    if (index == 0)
      automated_send = send;
  }
  if (!automated_send)
    throw std::runtime_error("automation fixture has no send");
  for (const auto [bar, gain] :
       {std::pair{0, 0.125}, std::pair{1, 0.5}, std::pair{2, 0.25}, std::pair{3, 0.125}})
    (void)take(
        session->parameter_automation().set_send_gain_automation_point(*automated_send, bar, gain));
  // The master chain: EQ, compressor, reverb and limiter as built-in effects.
  for (const auto [index, engine_id] : {std::pair{0U, "shape"}, std::pair{1U, "press"},
                                        std::pair{2U, "expanse"}, std::pair{3U, "ceiling"}})
    (void)take(session->effects().add_effect_engine(project::MasterSignalPath{}, index, engine_id));
  if (native_synths) {
    for (const auto engine_id : {"prism", "ember"}) {
      if (!synth::find_engine(engine_id))
        throw std::runtime_error("required native synth engine is not registered");
      const auto instrument = take(session->instruments().add_synth_track());
      take(session->instruments().select_native_instrument(instrument, engine_id));
      take(session->routing().set_main_output(instrument, group_a));
      (void)take(session->routing().add_send(instrument, return_a, 0.125));
      for (std::int64_t bar = 0; bar < static_cast<std::int64_t>((source_seconds + 1) / 2); ++bar) {
        take(session->midi_clips().add_midi_clip(instrument,
                                                 {bar * 4 * timeline::ticks_per_quarter}));
        const auto& clip = session->snapshot().tracks.back().midi_clips.back();
        take(session->notes().add_note(clip.id, {0},
                                       engine_id == std::string_view("prism") ? 60 : 67));
      }
    }
  }
#if defined(OSCILLATE_QUALIFICATION_VST3)
  if (vst3.instrument) {
    const auto instrument = take(
        session->instruments().create_vst3_instrument_track(identity_from(vst3.instrument_scan)));
    take(session->routing().set_main_output(instrument, group_a));
    (void)take(session->routing().add_send(instrument, return_a, 0.125));
    for (std::int64_t bar = 0; bar < static_cast<std::int64_t>((source_seconds + 1) / 2); ++bar) {
      take(
          session->midi_clips().add_midi_clip(instrument, {bar * 4 * timeline::ticks_per_quarter}));
      const auto& clip = session->snapshot().tracks.back().midi_clips.back();
      take(session->notes().add_note(clip.id, {0}, 60));
    }
  }
  if (vst3.effect) {
    if (!first_audio_track)
      throw std::runtime_error("VST3 fixture has no imported audio track");
    (void)take(session->effects().insert_vst3_plugin(*first_audio_track, 0,
                                                     identity_from(vst3.effect_scan)));
  }
#else
  (void)vst3;
#endif
  take(session->saving().save());
  return session;
}

#if defined(OSCILLATE_QUALIFICATION_VST3)
std::unique_ptr<plugins::Vst3TrackInsertProvider> make_provider(const project::Snapshot& snapshot,
                                                                const Vst3Evidence& vst3) {
  if (!vst3.effect)
    return {};
  const auto found = std::ranges::find_if(snapshot.tracks, [](const project::Track& track) {
    return track.effects.size() == 1 &&
           std::holds_alternative<project::Vst3PluginInstance>(track.effects.front().processor);
  });
  if (found == snapshot.tracks.end())
    throw std::runtime_error("saved VST3 insert is absent after reopen");
  auto provider = std::make_unique<plugins::Vst3TrackInsertProvider>();
  provider->set_scan_receipt(
      std::get<project::Vst3PluginInstance>(found->effects.front().processor).id, vst3.effect_scan);
  return provider;
}

std::unique_ptr<plugins::Vst3InstrumentTrackProvider>
make_instrument_provider(const project::Snapshot& snapshot, const Vst3Evidence& vst3) {
  if (!vst3.instrument)
    return {};
  const auto found = std::ranges::find_if(snapshot.tracks, [](const project::Track& track) {
    return track.kind == project::TrackKind::instrument &&
           std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source);
  });
  if (found == snapshot.tracks.end())
    throw std::runtime_error("saved VST3 instrument is absent after reopen");
  auto provider = std::make_unique<plugins::Vst3InstrumentTrackProvider>();
  provider->set_scan_receipt(std::get<project::Vst3PluginInstance>(found->instrument_source).id,
                             vst3.instrument_scan);
  return provider;
}

class MixedProvider final : public app::TrackInsertProvider {
public:
  MixedProvider(std::unique_ptr<plugins::Vst3TrackInsertProvider> effect,
                std::unique_ptr<plugins::Vst3InstrumentTrackProvider> instrument,
                project::PluginInstanceId effect_id)
      : effect_(std::move(effect)), instrument_(std::move(instrument)), effect_id_(effect_id) {}

  foundation::Result<app::PreparedTrackInsert> prepare(const project::Snapshot& snapshot,
                                                       const project::Vst3PluginInstance& saved,
                                                       std::uint32_t frames) override {
    return saved.id == effect_id_ ? effect_->prepare(snapshot, saved, frames)
                                  : instrument_->prepare(snapshot, saved, frames);
  }

  [[nodiscard]] bool stop_after_join() noexcept { return instrument_->stop_after_join(); }

private:
  std::unique_ptr<plugins::Vst3TrackInsertProvider> effect_;
  std::unique_ptr<plugins::Vst3InstrumentTrackProvider> instrument_;
  project::PluginInstanceId effect_id_;
};

std::unique_ptr<app::TrackInsertProvider> make_live_provider(const project::Snapshot& snapshot,
                                                             const Vst3Evidence& vst3) {
  if (vst3.effect && vst3.instrument) {
    const auto found = std::ranges::find_if(snapshot.tracks, [](const project::Track& track) {
      return track.effects.size() == 1 &&
             std::holds_alternative<project::Vst3PluginInstance>(track.effects.front().processor);
    });
    if (found == snapshot.tracks.end())
      throw std::runtime_error("mixed provider lost the saved effect");
    return std::make_unique<MixedProvider>(
        make_provider(snapshot, vst3), make_instrument_provider(snapshot, vst3),
        std::get<project::Vst3PluginInstance>(found->effects.front().processor).id);
  }
  if (vst3.effect)
    return make_provider(snapshot, vst3);
  return make_instrument_provider(snapshot, vst3);
}

void stop_instrument(app::TrackInsertProvider* provider, const Vst3Evidence& vst3) {
  if (!vst3.instrument)
    return;
  const bool stopped =
      vst3.effect ? static_cast<MixedProvider*>(provider)->stop_after_join()
                  : static_cast<plugins::Vst3InstrumentTrackProvider*>(provider)->stop_after_join();
  if (!stopped)
    throw std::runtime_error("VST3 instrument did not stop cleanly");
}

double route_probe_energy(const project::Snapshot& snapshot, project::TrackId track,
                          plugins::Vst3TrackInsertProvider* provider) {
  Gate gate;
  auto model = take(project::ProjectModel::open(snapshot, gate));
  app::RoutedPlaybackControl control(model, {{4'096, 4'096}, 128, 1ULL << 20, 16ULL << 20},
                                     provider);
  take(control.submit_current());
  std::vector<std::span<const engine::StereoFrame>> slots(control.topological_order().size());
  std::array<engine::StereoFrame, 128> source{};
  source.fill({0.025F, -0.025F});
  const auto found = std::ranges::find(control.source_layout(), track,
                                       &oscillate::routing::ProjectRouteSource::track);
  if (found == control.source_layout().end())
    throw std::runtime_error("verified insert track has no source slot");
  slots[found->topological_index] = source;
  std::array<engine::StereoFrame, 128> output{};
  double energy = 0.0;
  for (std::uint64_t block = 0; block < 16; ++block) {
    if (control.render_block(slots, output, {block * 128, 1}) !=
        engine::RoutedRenderStatus::rendered)
      throw std::runtime_error("verified insert probe graph did not render");
    if (block >= 8) {
      for (const auto frame : output)
        energy += std::abs(frame.left) + std::abs(frame.right);
    }
  }
  control.observe();
  if (control.status().active_graph_revision != snapshot.revision || control.processor_fault())
    throw std::runtime_error("verified insert probe did not activate cleanly");
  return energy;
}

void verify_insert_audible(const project::Snapshot& snapshot, const Vst3Evidence& vst3) {
  if (!vst3.effect)
    return;
  const auto found = std::ranges::find_if(snapshot.tracks, [](const project::Track& track) {
    return track.effects.size() == 1 &&
           std::holds_alternative<project::Vst3PluginInstance>(track.effects.front().processor);
  });
  if (found == snapshot.tracks.end())
    throw std::runtime_error("saved VST3 insert is absent for audible probe");
  auto effect_snapshot = snapshot;
  std::erase_if(effect_snapshot.tracks, [](const project::Track& track) {
    return track.kind == project::TrackKind::instrument;
  });
  auto provider = make_provider(effect_snapshot, vst3);
  const auto wet = route_probe_energy(effect_snapshot, found->id, provider.get());
  auto dry_snapshot = effect_snapshot;
  auto dry_track = std::ranges::find(dry_snapshot.tracks, found->id, &project::Track::id);
  dry_track->effects.clear();
  const auto dry = route_probe_energy(dry_snapshot, found->id, nullptr);
  const auto ratio = wet / dry;
  std::cout << "vst3_probe wet_energy=" << wet << " dry_energy=" << dry << " ratio=" << ratio
            << '\n';
  if (!std::isfinite(ratio) || std::abs(ratio - 2.0) > 0.05)
    throw std::runtime_error("verified VST3 gain is not audible against the dry route oracle");
}

void verify_instrument_audible(const project::Snapshot& snapshot, const Vst3Evidence& vst3) {
  if (!vst3.instrument)
    return;
  Gate gate;
  auto model = take(project::ProjectModel::open(snapshot, gate));
  auto provider = make_live_provider(snapshot, vst3);
  app::RoutedPlaybackControl control(model, {{4'096, 4'096}, 128, 1ULL << 20, 16ULL << 20},
                                     provider.get());
  take(control.submit_current());
  std::vector<std::span<const engine::StereoFrame>> silent(control.topological_order().size());
  std::array<engine::StereoFrame, 128> output{};
  double energy = 0.0;
  for (std::uint64_t block = 0; block < 16; ++block) {
    if (control.render_block(silent, output, {block * 128, 1}) !=
        engine::RoutedRenderStatus::rendered)
      throw std::runtime_error("verified instrument probe graph did not render");
    for (const auto frame : output)
      energy += std::abs(frame.left) + std::abs(frame.right);
  }
  control.observe();
  stop_instrument(provider.get(), vst3);
  std::cout << "vst3_instrument_probe silent_audio_input_energy=" << energy << '\n';
  if (!std::isfinite(energy) || energy <= 1.0 || control.processor_fault() ||
      control.status().active_graph_revision != snapshot.revision)
    throw std::runtime_error("verified MIDI instrument did not produce routed audio");
}
#else
void stop_instrument(app::TrackInsertProvider*, const Vst3Evidence&) {}
#endif

double automation_probe_energy(const project::Snapshot& project_snapshot, std::uint64_t sample) {
  auto snapshot = project_snapshot;
  snapshot.master_effects.clear();
  std::erase_if(snapshot.tracks, [](const project::Track& track) {
    return track.kind == project::TrackKind::instrument;
  });
  for (auto& track : snapshot.tracks)
    track.effects.clear();
  const auto first = std::ranges::find_if(snapshot.tracks, [](const project::Track& track) {
    return track.kind == project::TrackKind::audio && !track.automation.empty();
  });
  if (first == snapshot.tracks.end())
    throw std::runtime_error("send automation probe lost its source track");
  Gate gate;
  auto model = take(project::ProjectModel::open(snapshot, gate));
  app::RoutedPlaybackControl control(model, {{4'096, 4'096}, 128, 1ULL << 20, 16ULL << 20});
  take(control.submit_current());
  std::vector<std::span<const engine::StereoFrame>> slots(control.topological_order().size());
  std::array<engine::StereoFrame, 128> source{};
  source.fill({0.025F, -0.025F});
  const auto found = std::ranges::find(control.source_layout(), first->id,
                                       &oscillate::routing::ProjectRouteSource::track);
  if (found == control.source_layout().end())
    throw std::runtime_error("send automation source slot is absent");
  slots[found->topological_index] = source;
  std::array<engine::StereoFrame, 128> output{};
  if (control.render_block(slots, output, {sample, 1}) != engine::RoutedRenderStatus::rendered)
    throw std::runtime_error("send automation probe did not render");
  double energy = 0.0;
  for (const auto frame : output)
    energy += std::abs(frame.left) + std::abs(frame.right);
  return energy;
}

void verify_send_automation_audible(const project::Snapshot& snapshot) {
  const auto start = automation_probe_energy(snapshot, 0);
  const auto next_bar = automation_probe_energy(snapshot, 96'000);
  const auto ratio = next_bar / start;
  std::cout << "send_automation_probe start_energy=" << start << " next_bar_energy=" << next_bar
            << " ratio=" << ratio << '\n';
  if (!std::isfinite(ratio) || ratio < 1.2 || ratio > 1.5)
    throw std::runtime_error("saved send automation did not change the routed mix");
}

double percentile(std::span<const double> sorted, std::size_t numerator) {
  const auto index = (sorted.size() * numerator + 99) / 100 - 1;
  return sorted[index];
}

void run_pipewire(const Options& options, const fs::path& root, const project::Snapshot& snapshot,
                  const Vst3Evidence& vst3) {
  using namespace std::chrono_literals;
  const auto target_serial = options.pipewire_target.value_or(0);
  const auto target_frames = (options.warmup + options.blocks) * options.frames;
  const auto run_duration = std::chrono::duration<double>(static_cast<double>(target_frames) /
                                                          static_cast<double>(sample_rate));
  for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
    Gate gate;
    auto model = take(project::ProjectModel::open(snapshot, gate));
    const engine::RoutedMixerLimits limits{{4'096, 4'096}, 128, 1ULL << 20, 16ULL << 20};
    std::unique_ptr<app::TrackInsertProvider> provider;
#if defined(OSCILLATE_QUALIFICATION_VST3)
    provider = make_live_provider(snapshot, vst3);
#else
    (void)vst3;
#endif
    // The helpers live playback renders with (ADR-0048).
    engine::AudioWorkerPool workers(engine::AudioWorkerPool::default_worker_count());
    app::RoutedPlaybackControl control(model, limits, provider.get());
    take(control.submit_current());
    auto source = take(
        app::RoutedAudioSource::prepare(root, *model.snapshot(), control, track_count * 4'096));
    audio::TransportCursor cursor;
    std::atomic<bool> allowed{false};
    const audio::TransportPermission permission{
        &allowed, [](const void* context) noexcept {
          return static_cast<const std::atomic<bool>*>(context)->load(std::memory_order_acquire);
        }};
    audio::PipeWireOutput output(source->render_source(&workers), cursor, permission);
    take(output.start(sample_rate, static_cast<std::uint32_t>(options.frames), target_serial));
    const auto ready_by = std::chrono::steady_clock::now() + 5s;
    auto status = output.status();
    while (std::chrono::steady_clock::now() < ready_by) {
      status = output.status();
      if (status.state == audio::OutputState::disconnected ||
          status.state == audio::OutputState::error)
        throw std::runtime_error("selected PipeWire target failed before playback");
      if (status.state == audio::OutputState::streaming && status.negotiated_client_rate &&
          status.observed_block_frames && status.observed_graph_rate)
        break;
      std::this_thread::sleep_for(10ms);
    }
    if (status.state != audio::OutputState::streaming ||
        status.negotiated_client_rate != sample_rate || !status.observed_graph_rate ||
        !status.observed_block_frames)
      throw std::runtime_error("PipeWire did not negotiate a bounded 48 kHz stream");
    const auto observed_start = *status.observed_block_frames;
    allowed.store(true, std::memory_order_release);
    std::cout << "live_ready run=" << repetition + 1 << " requested_frames=" << options.frames
              << " source_seconds=" << options.source_seconds
              << " observed_frames=" << observed_start
              << " client_rate=" << *status.negotiated_client_rate
              << " graph_rate=" << *status.observed_graph_rate << " target_serial=" << target_serial
              << '\n'
              << std::flush;
    const auto finish_by =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(run_duration * 3.0 + 3s);
    while (cursor.completed_snapshot().next_sample < target_frames &&
           std::chrono::steady_clock::now() < finish_by) {
      status = output.status();
      if (status.state == audio::OutputState::disconnected ||
          status.state == audio::OutputState::error || source->status().worker_failed)
        break;
      std::this_thread::sleep_for(10ms);
    }
    allowed.store(false, std::memory_order_release);
    status = output.status();
    output.stop();
    stop_instrument(provider.get(), vst3);
    control.observe();
    const auto completed = cursor.completed_snapshot().next_sample;
    const auto source_status = source->status();
    const auto revision = control.status();
    const auto vst3_fault = control.processor_fault();
    std::cout << "live_result run=" << repetition + 1 << " requested_frames=" << options.frames
              << " source_seconds=" << options.source_seconds
              << " observed_frames=" << status.observed_block_frames.value_or(0)
              << " client_rate=" << status.negotiated_client_rate.value_or(0)
              << " graph_rate=" << status.observed_graph_rate.value_or(0)
              << " completed_frames=" << completed << " target_frames=" << target_frames
              << " render_failures=" << status.render_failures
              << " missing_frames=" << source_status.missing_frames
              << " worker_failed=" << source_status.worker_failed
              << " active_revision=" << revision.active_graph_revision
              << " oversized_blocks=" << status.oversized_blocks
              << " invalid_buffers=" << status.invalid_buffers
              << " measured_callback_blocks=" << status.measured_callback_blocks.value_or(0)
              << " callback_over_period_blocks=" << status.callback_over_period_blocks.value_or(0)
              << " max_callback_duration_ns=" << status.max_callback_duration_ns.value_or(0)
              << " graph_xrun_recovery_events=";
    if (status.graph_xrun_recovery_events)
      std::cout << *status.graph_xrun_recovery_events;
    else
      std::cout << "unavailable";
    std::cout << " xruns=";
    if (status.xruns)
      std::cout << *status.xruns;
    else
      std::cout << "unavailable";
    std::cout << " vst3_insert=" << vst3.effect << " vst3_instrument=" << vst3.instrument
              << " vst3_fault=" << vst3_fault.has_value() << '\n'
              << std::flush;
    if (completed < target_frames || revision.application != app::PlaybackApplication::active ||
        revision.active_graph_revision != snapshot.revision ||
        status.state != audio::OutputState::streaming ||
        status.negotiated_client_rate != sample_rate ||
        status.observed_block_frames != options.frames || status.render_failures != 0 ||
        status.oversized_blocks != 0 || status.invalid_buffers != 0 ||
        !status.measured_callback_blocks || !status.callback_over_period_blocks ||
        !status.max_callback_duration_ns || source_status.missing_frames != 0 ||
        source_status.worker_failed || vst3_fault)
      throw std::runtime_error("live PipeWire rehearsal failed a bounded playback invariant");
  }
}

void run(const Options& options) {
  Directory temporary;
  const auto root = temporary.path() / "project";
  const auto vst3 = verify_plugin(options.vst3_insert, options.vst3_instrument);
  auto session = prepare_project(temporary.path(), vst3, options.source_seconds,
                                 options.processed_clips, options.native_synths);
  session.reset();
  auto reopened = take(app::ProjectSession::open(root));
  const auto& snapshot = reopened->snapshot();
  std::size_t audio_tracks = 0;
  std::size_t group_buses = 0;
  std::size_t return_buses = 0;
  std::size_t sends = 0;
  std::size_t vst3_inserts = 0;
  std::size_t vst3_instruments = 0;
  std::size_t native_instruments = 0;
  bool has_prism = false;
  bool has_ember = false;
  std::size_t automated_sends = 0;
  std::size_t processed_clips = 0;
  std::size_t repitch_clips = 0;
  std::size_t pitched_clips = 0;
  std::size_t pitched_plus_two = 0;
  std::size_t pitched_minus_three = 0;
  for (const auto& track : snapshot.tracks) {
    audio_tracks += track.kind == project::TrackKind::audio ? 1U : 0U;
    group_buses += track.kind == project::TrackKind::group_bus ? 1U : 0U;
    return_buses += track.kind == project::TrackKind::return_bus ? 1U : 0U;
    sends += track.sends.size();
    vst3_inserts +=
        static_cast<std::size_t>(std::ranges::count_if(track.effects, [](const auto& slot) {
          return std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
        }));
    vst3_instruments +=
        track.kind == project::TrackKind::instrument &&
                std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source)
            ? 1U
            : 0U;
    if (const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source)) {
      ++native_instruments;
      has_prism |= patch->engine == "prism";
      has_ember |= patch->engine == "ember";
      if (track.midi_clips.size() != (options.source_seconds + 1) / 2)
        throw std::runtime_error("native synth MIDI clips were not restored after reopen");
    }
    automated_sends +=
        static_cast<std::size_t>(std::ranges::count_if(track.automation, [](const auto& lane) {
          return std::holds_alternative<project::SendGainTarget>(lane.target) &&
                 lane.points.size() == 4;
        }));
    for (const auto& clip : track.clips) {
      if (!clip.processing.requires_rendering())
        continue;
      ++processed_clips;
      const auto& settings = clip.processing;
      const bool exact_marker =
          settings.reviewed_warp_markers.size() == 1 &&
          settings.reviewed_warp_markers.front().source_frame.value ==
              static_cast<std::int64_t>(sample_rate) &&
          std::holds_alternative<timeline::SamplePosition>(
              settings.reviewed_warp_markers.front().target_offset) &&
          std::get<timeline::SamplePosition>(settings.reviewed_warp_markers.front().target_offset)
                  .value == static_cast<std::int64_t>(sample_rate * 5 / 4);
      repitch_clips += settings.warp.mode_overridden &&
                               settings.warp.mode == project::WarpMode::repitch && exact_marker
                           ? 1U
                           : 0U;
      pitched_clips += clip.processing.warp.mode == project::WarpMode::complex &&
                               clip.processing.warp.mode_overridden &&
                               clip.processing.reviewed_warp_markers.empty() &&
                               clip.processing.pitch_semitones != 0.0
                           ? 1U
                           : 0U;
      pitched_plus_two += settings.pitch_semitones == 2.0 ? 1U : 0U;
      pitched_minus_three += settings.pitch_semitones == -3.0 ? 1U : 0U;
    }
  }
  const auto has_master = [&snapshot](std::string_view engine) {
    return std::ranges::any_of(snapshot.master_effects, [engine](const auto& slot) {
      const auto* patch = std::get_if<project::NativeEffectPatch>(&slot.processor);
      return slot.enabled && patch != nullptr && patch->engine == engine;
    });
  };
  const bool eq = has_master("shape");
  const bool compressor = has_master("press");
  const bool reverb = has_master("expanse");
  const bool limiter = has_master("ceiling");
  const auto expected_audio_tracks = track_count - (options.native_synths ? 2U : 0U);
  if (audio_tracks != expected_audio_tracks || group_buses != 2 || return_buses != 2 ||
      sends !=
          expected_audio_tracks + (vst3.instrument ? 1U : 0U) + (options.native_synths ? 2U : 0U) ||
      snapshot.sources.size() != expected_audio_tracks || vst3_inserts != (vst3.effect ? 1U : 0U) ||
      vst3_instruments != (vst3.instrument ? 1U : 0U) || automated_sends != 1 || !eq ||
      native_instruments != (options.native_synths ? 2U : 0U) ||
      (options.native_synths && (!has_prism || !has_ember)) ||
      processed_clips != options.processed_clips || repitch_clips != options.processed_clips / 2 ||
      pitched_clips != options.processed_clips / 2 ||
      pitched_plus_two != options.processed_clips / 4 ||
      pitched_minus_three != options.processed_clips / 4 || !compressor || !reverb || !limiter) {
    std::cerr << "observed audio=" << audio_tracks << " group=" << group_buses
              << " return=" << return_buses << " sends=" << sends
              << " sources=" << snapshot.sources.size() << " eq=" << eq
              << " vst3_inserts=" << vst3_inserts << " compressor=" << compressor
              << " vst3_instruments=" << vst3_instruments << " automated_sends=" << automated_sends
              << " native_instruments=" << native_instruments
              << " processed_clips=" << processed_clips << " repitch_clips=" << repitch_clips
              << " pitched_clips=" << pitched_clips << " reverb=" << reverb
              << " limiter=" << limiter << '\n';
    throw std::runtime_error("saved project reopened with the wrong rehearsal workload");
  }
#if defined(OSCILLATE_QUALIFICATION_VST3)
  if (vst3.effect) {
    const auto found = std::ranges::find_if(snapshot.tracks, [](const project::Track& track) {
      return track.effects.size() == 1 &&
             std::holds_alternative<project::Vst3PluginInstance>(track.effects.front().processor);
    });
    if (found == snapshot.tracks.end())
      throw std::runtime_error("saved VST3 insert is absent after reopen");
    const auto& saved = std::get<project::Vst3PluginInstance>(found->effects.front().processor);
    const auto& klass = vst3.effect_scan.metadata.classes.front();
    if (saved.class_id != klass.id ||
        saved.bundle_path != vst3.effect_scan.binary.canonical_bundle.string() ||
        saved.binary_sha256 != vst3.effect_scan.binary.sha256)
      throw std::runtime_error("reopened plugin identity differs from verified scan receipt");
    verify_insert_audible(snapshot, vst3);
  }
  if (vst3.instrument) {
    const auto found = std::ranges::find_if(snapshot.tracks, [](const project::Track& track) {
      return track.kind == project::TrackKind::instrument &&
             std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source);
    });
    if (found == snapshot.tracks.end() ||
        found->midi_clips.size() != (options.source_seconds + 1) / 2)
      throw std::runtime_error("saved VST3 instrument MIDI is absent after reopen");
    const auto& saved = std::get<project::Vst3PluginInstance>(found->instrument_source);
    const auto& klass = vst3.instrument_scan.metadata.classes.front();
    if (saved.class_id != klass.id ||
        saved.bundle_path != vst3.instrument_scan.binary.canonical_bundle.string() ||
        saved.binary_sha256 != vst3.instrument_scan.binary.sha256)
      throw std::runtime_error("reopened instrument identity differs from verified scan receipt");
    verify_instrument_audible(snapshot, vst3);
  }
#endif
  verify_send_automation_audible(snapshot);
  if (options.pipewire_target) {
    run_pipewire(options, root, snapshot, vst3);
    return;
  }
  const auto period = std::chrono::duration<double>(static_cast<double>(options.frames) /
                                                    static_cast<double>(sample_rate));
  const double period_us = std::chrono::duration<double, std::micro>(period).count();
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "workload="
            << (options.native_synths ? "native-synth-32-routed" : "managed-wav-32-routed")
            << " rate=oscillate::test::sample_rate_48k frames=" << options.frames
            << " source_seconds=" << options.source_seconds << " source_tracks=" << audio_tracks
            << " group_buses=2 return_buses=2 sends=" << sends
            << " master=eq+compressor+reverb+limiter"
            << " vst3_inserts=" << vst3_inserts << " vst3_instruments=" << vst3_instruments
            << " native_instruments=" << native_instruments
            << " processed_clips=" << processed_clips << " repitch_clips=" << repitch_clips
            << " pitched_clips=" << pitched_clips << " automated_sends=" << automated_sends
            << " warmup_blocks=" << options.warmup << " measured_blocks=" << options.blocks
            << " repetitions=" << options.repetitions << " period_us=" << period_us
            << " pace_multiplier=" << options.pace_multiplier << '\n';
  for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
    Gate gate;
    auto model = take(project::ProjectModel::open(reopened->snapshot(), gate));
    const engine::RoutedMixerLimits limits{{4'096, 4'096}, 128, 1ULL << 20, 16ULL << 20};
    std::unique_ptr<app::TrackInsertProvider> provider;
#if defined(OSCILLATE_QUALIFICATION_VST3)
    provider = make_live_provider(snapshot, vst3);
#endif
    // The helpers live playback renders with (ADR-0048).
    engine::AudioWorkerPool workers(engine::AudioWorkerPool::default_worker_count());
    app::RoutedPlaybackControl control(model, limits, provider.get());
    take(control.submit_current());
    auto source = take(
        app::RoutedAudioSource::prepare(root, *model.snapshot(), control,
                                        track_count * (options.processed_clips ? 8'192U : 4'096U)));
    if (options.native_synths) {
      for (const auto& track : snapshot.tracks) {
        if (std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source) &&
            !source->synth_runtime(track.id))
          throw std::runtime_error("saved native synth has no routed runtime");
      }
    }
    if (options.processed_clips) {
      const auto prepared = source->status();
      if (prepared.processed_clips != options.processed_clips ||
          prepared.reduced_quality_clips != 0 || prepared.quality_step_downs != 0)
        throw std::runtime_error("saved processed clips were not prepared at full quality");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (true) {
        const auto clips = source->processed_clip_status();
        if (clips.size() == options.processed_clips &&
            std::ranges::all_of(clips,
                                [](const auto& clip) { return clip.buffered_frames >= 4'096; }))
          break;
        if (source->status().worker_failed || std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("processed clip workers did not prebuffer");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    const auto callback = source->render_source(&workers);
    workers.start_owned_threads(workers.wanted_workers());
    audio::TransportCursor cursor;
    std::array<engine::StereoFrame, 128> stereo{};
    std::array<float, 256> interleaved{};
    std::vector<double> measurements(options.blocks);
    std::size_t render_failures = 0;
    std::size_t over_period = 0;
    std::size_t audible_blocks = 0;
    std::size_t synth_voice_blocks = 0;
    double total_us = 0.0;
    const auto frame_span = std::span(stereo).first(options.frames);
    const auto sample_span = std::span(interleaved).first(options.frames * 2);
    const auto wall_started = std::chrono::steady_clock::now();
    const auto cpu_started = std::clock();
    auto deadline = std::chrono::steady_clock::now();
    for (std::size_t block = 0; block < options.warmup + options.blocks; ++block) {
      const auto started = std::chrono::steady_clock::now();
      begin_allocation_probe();
      const auto result =
          audio::render_transport_block(callback, cursor, true, frame_span, sample_span);
      end_allocation_probe();
      const double elapsed =
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
              .count();
      if (result != audio::BlockResult::rendered)
        ++render_failures;
      if (std::ranges::any_of(sample_span, [](float sample) { return !std::isfinite(sample); }))
        throw std::runtime_error("non-finite routed output");
      if (std::ranges::any_of(sample_span, [](float sample) { return std::abs(sample) > 0.001F; }))
        ++audible_blocks;
      if (options.native_synths) {
        bool both_sounding = true;
        for (const auto& track : snapshot.tracks) {
          if (std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source))
            both_sounding &= source->synth_runtime(track.id)->observed_status().active_voices > 0;
        }
        synth_voice_blocks += both_sounding ? 1U : 0U;
      }
      if (block >= options.warmup) {
        measurements[block - options.warmup] = elapsed;
        total_us += elapsed;
        over_period += elapsed > period_us ? 1U : 0U;
      }
      deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          period * static_cast<double>(options.pace_multiplier));
      std::this_thread::sleep_until(deadline);
    }
    const auto cpu_finished = std::clock();
    const double wall_us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - wall_started)
            .count();
    const double process_cpu_us =
        1'000'000.0 * static_cast<double>(cpu_finished - cpu_started) / CLOCKS_PER_SEC;
    workers.stop_owned_threads();
    const auto status = source->status();
    stop_instrument(provider.get(), vst3);
    const auto allocation_count = allocation_probe_count();
    const auto vst3_fault = control.processor_fault();
    std::ranges::sort(measurements);
    std::cout << "run=" << repetition + 1
              << " mean_us=" << total_us / static_cast<double>(options.blocks)
              << " p50_us=" << percentile(measurements, 50)
              << " p95_us=" << percentile(measurements, 95)
              << " p99_us=" << percentile(measurements, 99) << " max_us=" << measurements.back()
              << " over_period=" << over_period << " callback_allocations=" << allocation_count
              << " render_failures=" << render_failures
              << " missing_frames=" << status.missing_frames
              << " worker_failed=" << status.worker_failed << " audible_blocks=" << audible_blocks
              << " synth_voice_blocks=" << synth_voice_blocks
              << " processed_clips=" << status.processed_clips
              << " reduced_quality_clips=" << status.reduced_quality_clips
              << " quality_step_downs=" << status.quality_step_downs
              << " process_cpu_us=" << process_cpu_us << " wall_us=" << wall_us
              << " vst3_fault=" << vst3_fault.has_value() << '\n';
    if (render_failures != 0 || allocation_count != 0 || status.missing_frames != 0 ||
        status.worker_failed || status.processed_clips != options.processed_clips ||
        status.reduced_quality_clips != 0 || status.quality_step_downs != 0 || vst3_fault ||
        audible_blocks == 0 || (options.native_synths && synth_voice_blocks == 0) ||
        cursor.completed_snapshot().next_sample !=
            (options.warmup + options.blocks) * options.frames)
      throw std::runtime_error("streaming rehearsal failed a correctness invariant");
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    run(parse(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "Streaming rehearsal: " << error.what() << '\n';
    return 1;
  }
}
