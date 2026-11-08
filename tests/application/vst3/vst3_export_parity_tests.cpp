#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/offline_export.hpp"
#include "oscillate/application/vst3/stage_provider.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// VST3 export parity (ADR-0050, ADR-0090): a VST3 effect's master and stems,
// its finite tail, a latent plugin's timing on a track and on the master, and
// a VST3 instrument export exactly what live routed playback plays. Live
// playback is the oracle: the export, with its route latency trimmed, must
// null against it.
namespace {
using namespace oscillate;
namespace fs = std::filesystem;
namespace tolerance = oscillate::test::tolerance;

using oscillate::test::require;
using oscillate::test::take;

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

// The largest sample difference an export may show against live playback,
// as for the built-in effects (application_effect_export_parity).
constexpr double parity_tolerance = tolerance::float_round_trip;
constexpr std::uint32_t sample_rate = oscillate::test::sample_rate_48k;
constexpr double tempo_bpm = 120.0;
// Every fixture plugin accepts 128-frame host blocks; LoadedGain also 64.
constexpr std::size_t block_64 = oscillate::test::block_64;
constexpr std::size_t block_128 = oscillate::test::block_128;
// Delay compensation the live graph may apply: above the latent fixture's
// 96 frames.
constexpr std::uint32_t live_compensation_frames = 256;
constexpr std::size_t live_buffer_bytes = 1U << 20U;
// Frames the live source may buffer across its clip rings.
constexpr std::size_t live_stream_frames = 1U << 16U;
// The live graph takes device blocks of up to this many frames. Above the
// source clip's length, so the source primes each clip whole before the
// first block and no block races the clip streamer; the device then plays
// 64- or 128-frame blocks, which reach each VST3 host as they are.
constexpr std::size_t live_graph_max_frames = oscillate::test::block_1024;

// The source clip: a decaying 220 Hz tone, long enough to span several
// blocks of either size.
constexpr std::size_t source_frames = 700;
static_assert(live_graph_max_frames >= source_frames, "the live source primes each clip whole");
constexpr double source_hz = 220.0;
constexpr double source_peak = 0.5;
constexpr double source_decay_frames = 400.0;
constexpr double pcm16_full_scale = 32'767.0;
// The right channel plays the left at this level, so a channel swap shows.
constexpr double right_channel_level = 0.5;
// The wet clip starts off a block boundary; the dry clip overlaps it.
constexpr std::int64_t wet_clip_first = 37;
constexpr std::int64_t dry_clip_first = 300;
constexpr double send_gain = 0.5;

// LoadedGain's reserved state -96: a 96-frame true latency at gain 0.5,
// which crosses both block sizes.
constexpr float latent_state = -96.0F;
constexpr std::uint32_t latent_frames = 96;
// TailGain's declared tail: its eight-frame history.
constexpr std::uint64_t tail_gain_tail_frames = 8;
// TimedInstrument declares, and rings for, an eight-frame release with this
// setting of OSCILLATE_VST3_INSTRUMENT_TAIL.
constexpr const char* instrument_tail_setting = "8";
constexpr std::size_t instrument_tail_frames = 8;
constexpr std::uint8_t instrument_pitch = 60; // The only pitch the fixture plays.
constexpr std::uint8_t instrument_velocity = 100;
constexpr float instrument_level = 0.5F;
// Live frames past the export's end checked for silence.
constexpr std::size_t silence_check_frames = 256;
// Live blocks rendered beyond the export and its latency.
constexpr std::size_t live_margin_blocks = 4;

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

project::Vst3PluginInstance plugin_instance(const plugins::ScanResult& scan, std::uint64_t low) {
  const auto& klass = scan.metadata.classes.front();
  return {.id = id<project::PluginInstanceId>(low),
          .class_id = klass.id,
          .vendor = klass.vendor,
          .name = klass.name,
          .version = klass.version,
          .bundle_path = scan.binary.canonical_bundle.string(),
          .binary_sha256 = scan.binary.sha256,
          .component_state = {},
          .parameters = {},
          .missing = false};
}

void set_float_state(project::Vst3PluginInstance& plugin, float value) {
  plugin.component_state.resize(sizeof(value));
  std::memcpy(plugin.component_state.data(), &value, sizeof(value));
}

std::vector<std::int16_t> source_samples() {
  std::vector<std::int16_t> interleaved;
  interleaved.reserve(source_frames * 2);
  for (std::size_t frame = 0; frame < source_frames; ++frame) {
    const double phase = 2.0 * std::numbers::pi * source_hz * static_cast<double>(frame) /
                         static_cast<double>(sample_rate);
    const double value =
        source_peak * std::exp(-static_cast<double>(frame) / source_decay_frames) * std::sin(phase);
    interleaved.push_back(static_cast<std::int16_t>(std::lround(value * pcm16_full_scale)));
    interleaved.push_back(
        static_cast<std::int16_t>(std::lround(right_channel_level * value * pcm16_full_scale)));
  }
  return interleaved;
}

project::ProjectModel new_model(Gate& gate) {
  auto tempo = take(timeline::TempoMap::create({{{0}, tempo_bpm, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return take(project::ProjectModel::create(id<project::ProjectId>(1), sample_rate,
                                            std::move(tempo), std::move(meter), gate));
}

struct EffectProject {
  fs::path root;
  project::Snapshot snapshot;
  project::TrackId wet;
  project::TrackId dry;
  project::PluginInstanceId plugin;
};

// A wet audio track with one VST3 effect and a send to a return bus, beside
// a dry audio track. `state` is the effect's saved component state.
EffectProject effect_project(const fs::path& root, const plugins::ScanResult& scan,
                             std::optional<float> state) {
  Gate gate;
  auto model = new_model(gate);
  auto plugin = plugin_instance(scan, 8);
  if (state)
    set_float_state(plugin, *state);
  auto writer = take(persistence::ProjectStore::open_writer(root, true));
  const auto input = root.parent_path() / (root.filename().string() + "-source.wav");
  const auto samples = source_samples();
  oscillate::test::write_wav_pcm16(input, sample_rate, oscillate::test::stereo_channels, samples);
  const int descriptor = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "parity source could not open");
  auto staged = writer.stage_wav(id<project::SourceId>(3), descriptor);
  REQUIRE_EQ(::close(descriptor), 0, "parity source could not close");
  auto receipt = take(std::move(staged));
  const auto wet = id<project::TrackId>(2);
  const auto bus = id<project::TrackId>(4);
  const auto dry = id<project::TrackId>(9);
  const auto frames = static_cast<std::int64_t>(source_frames);
  const project::Clip wet_clip{
      id<project::ClipId>(5),
      receipt.record().id,
      {{0}, {frames}},
      project::ElapsedInterval{{wet_clip_first}, {wet_clip_first + frames}}};
  const project::Clip dry_clip{
      id<project::ClipId>(6),
      receipt.record().id,
      {{0}, {frames}},
      project::ElapsedInterval{{dry_clip_first}, {dry_clip_first + frames}}};
  (void)take(
      model.apply({project::AddTrack{wet}, project::AddTrack{bus, project::TrackKind::return_bus},
                   project::AddTrack{dry, project::TrackKind::audio, 2},
                   project::AddSource{receipt.record()}, project::AddClip{wet, wet_clip},
                   project::AddClip{dry, dry_clip}, project::InsertVst3Plugin{wet, 0, plugin},
                   project::AddSendRoute{wet, {id<project::SendId>(7), bus, send_gain}}}));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save,
                              std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "parity project did not save");
  auto reader = take(persistence::ProjectStore::open_reader(root));
  return {root, take(reader.load_current()).snapshot, wet, dry, plugin.id};
}

struct InstrumentProject {
  fs::path root;
  project::Snapshot snapshot;
  project::TrackId track;
  project::PluginInstanceId plugin;
};

// One VST3 instrument track playing one note, with a send to a return bus.
InstrumentProject instrument_project(const fs::path& root, const plugins::ScanResult& scan) {
  Gate gate;
  auto model = new_model(gate);
  auto plugin = plugin_instance(scan, 5);
  set_float_state(plugin, instrument_level);
  plugin.controller_state = plugin.component_state;
  // A note starting off a block boundary that ends inside the third block.
  constexpr std::int64_t note_start = 200'000;
  constexpr std::int64_t note_length = 10'240'000;
  const project::MidiClip clip{id<project::ClipId>(4),
                               project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
                               {timeline::ticks_per_quarter},
                               false,
                               {{id<project::NoteId>(6),
                                 {note_start},
                                 {note_length},
                                 instrument_pitch,
                                 instrument_velocity}},
                               1};
  const auto track = id<project::TrackId>(2);
  const auto bus = id<project::TrackId>(3);
  (void)take(
      model.apply({project::AddTrack{track, project::TrackKind::instrument},
                   project::AddTrack{bus, project::TrackKind::return_bus},
                   project::AddMidiClip{track, clip}, project::InsertVst3Plugin{track, 0, plugin},
                   project::AddSendRoute{track, {id<project::SendId>(7), bus, send_gain}}}));
  auto writer = take(persistence::ProjectStore::open_writer(root, true));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "parity instrument project did not save");
  auto reader = take(persistence::ProjectStore::open_reader(root));
  return {root, take(reader.load_current()).snapshot, track, plugin.id};
}

struct LiveRender {
  std::vector<float> samples; // Interleaved stereo, from stream frame 0.
  std::uint32_t latency = 0;
};

// Plays `snapshot` through the live routed graph, as the device callback
// does, for at least `frames` frames past the route latency.
LiveRender play_live(const fs::path& root, const project::Snapshot& snapshot,
                     std::span<const plugins::ScanResult> scans, std::size_t block_frames,
                     std::size_t frames) {
  // Each plugin's receipt is the scan of its saved bundle.
  const auto receipt = [&](const project::Vst3PluginInstance& plugin) {
    const auto found = std::ranges::find_if(scans, [&](const plugins::ScanResult& scan) {
      return scan.binary.canonical_bundle.string() == plugin.bundle_path;
    });
    require(found != scans.end(), "the live render has a scan of " + plugin.bundle_path);
    return *found;
  };
  Gate gate;
  auto model = take(project::ProjectModel::open(snapshot, gate));
  plugins::Vst3InstrumentTrackProvider instruments;
  plugins::Vst3TrackInsertProvider effects;
  for (const auto& track : snapshot.tracks) {
    if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source))
      instruments.set_scan_receipt(source->id, receipt(*source));
    for (const auto& slot : track.effects)
      if (const auto* effect = std::get_if<project::Vst3PluginInstance>(&slot.processor))
        effects.set_scan_receipt(effect->id, receipt(*effect));
  }
  for (const auto& slot : snapshot.master_effects)
    if (const auto* effect = std::get_if<project::Vst3PluginInstance>(&slot.processor))
      effects.set_scan_receipt(effect->id, receipt(*effect));
  plugins::Vst3StageProvider stages(instruments, effects);
  application::RoutedPlaybackControl control(
      model,
      {{1, live_compensation_frames}, live_graph_max_frames, live_buffer_bytes, live_buffer_bytes},
      &stages);
  const auto submitted = control.submit_current();
  REQUIRE_OK(submitted, "live graph prepared");
  auto source = take(application::RoutedAudioSource::prepare(root, *model.snapshot(), control,
                                                             live_stream_frames));
  control.bind_live_source(*source);
  const auto device = source->render_source();
  audio_io::TransportCursor transport;
  std::vector<engine::StereoFrame> stereo(block_frames);
  std::vector<float> interleaved(2 * block_frames);
  LiveRender live;
  live.latency = control.master_output_latency_frames();
  const auto wanted = 2 * (frames + live.latency + live_margin_blocks * block_frames);
  live.samples.reserve(wanted + interleaved.size());
  while (live.samples.size() < wanted) {
    REQUIRE_EQ(audio_io::render_transport_block(device, transport, true, stereo, interleaved),
               audio_io::BlockResult::rendered, "live block rendered");
    REQUIRE_TRUE(!control.processor_fault(), "live VST3 stage faulted");
    live.samples.insert(live.samples.end(), interleaved.begin(), interleaved.end());
  }
  control.unbind_live_source();
  require(instruments.stop_after_join(), "live VST3 instrument did not stop after playing");
  return live;
}

double peak(std::span<const float> samples) {
  double largest = 0.0;
  for (const float sample : samples)
    largest = std::max(largest, std::fabs(static_cast<double>(sample)));
  return largest;
}

// The export nulls against live playback once the live route latency is
// trimmed, and live playback is silent after the export ends.
void require_nulls(const std::vector<float>& exported, const LiveRender& live,
                   const std::string& what) {
  REQUIRE_GT(peak(exported), tolerance::coarse, what + ": the export is audible");
  const std::size_t offset = 2 * static_cast<std::size_t>(live.latency);
  REQUIRE_GE(live.samples.size(), offset + exported.size() + 2 * silence_check_frames,
             what + ": live render is long enough");
  double largest = 0.0;
  for (std::size_t index = 0; index < exported.size(); ++index)
    largest = std::max(
        largest, std::fabs(static_cast<double>(exported[index]) - live.samples[offset + index]));
  std::cout << what << ": latency " << live.latency << ", largest difference " << largest << '\n';
  REQUIRE_LT(largest, parity_tolerance, what + ": the export nulls against live playback");
  const auto after = std::span<const float>(live.samples)
                         .subspan(offset + exported.size(), 2 * silence_check_frames);
  REQUIRE_LT(peak(after), parity_tolerance,
             what + ": live playback is silent after the export ends (no tail cut)");
}

// The shared offline render with VST3 stages: the master, or one stem.
std::vector<float> export_vst3(const fs::path& root, const project::Snapshot& snapshot,
                               plugins::Vst3Scanner& scanner, const fs::path& target,
                               std::optional<project::TrackId> stem = std::nullopt) {
  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(root));
  auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  const auto published =
      stem ? exporting::render_vst3_stem(snapshot, store, std::move(authority), scanner, *stem,
                                         {target})
           : exporting::render_vst3_master(snapshot, store, std::move(authority), scanner,
                                           exporting::MasterRenderOptions{target});
  REQUIRE_OK(published, "VST3 export published");
  return oscillate::test::read_float_wav(target);
}

project::Snapshot muted_except(project::Snapshot snapshot, project::TrackId kept) {
  for (auto& track : snapshot.tracks)
    if (track.kind == project::TrackKind::audio && track.id != kept)
      track.muted = true;
  return snapshot;
}

// A VST3 effect's master and its stems null against live playback, and its
// finite tail is exported whole.
void effect_master_and_stems(plugins::Vst3Scanner& scanner) {
  const oscillate::test::TempDir directory("oscillate-vst3-export-parity");
  const auto scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(scan.ok(), "tail fixture scan failed: " + scan.diagnostic);
  const auto project = effect_project(directory / "effect", scan, std::nullopt);
  const auto master =
      export_vst3(project.root, project.snapshot, scanner, directory / "master.wav");
  // Finite tail: the export ends where the plugin's declared tail does.
  const auto source_end = static_cast<std::size_t>(dry_clip_first) + source_frames;
  REQUIRE_EQ(master.size(), 2 * (source_end + tail_gain_tail_frames),
             "the export ends at the last clip plus the declared tail");
  const auto wet_end = static_cast<std::size_t>(wet_clip_first) + source_frames;
  const auto wet_tail =
      std::span<const float>(master).subspan(2 * wet_end, 2 * tail_gain_tail_frames);
  REQUIRE_GT(peak(wet_tail), tolerance::coarse, "the effect's tail is exported");
  const auto wet =
      export_vst3(project.root, project.snapshot, scanner, directory / "wet.wav", project.wet);
  const auto dry =
      export_vst3(project.root, project.snapshot, scanner, directory / "dry.wav", project.dry);
  REQUIRE_EQ(wet.size(), master.size(), "the wet stem is as long as the master");
  REQUIRE_EQ(dry.size(), master.size(), "the dry stem is as long as the master");
  // TailGain only takes 128-frame host blocks.
  const auto frames = master.size() / 2;
  require_nulls(master,
                play_live(project.root, project.snapshot, std::span(&scan, 1), block_128, frames),
                "effect master");
  require_nulls(wet,
                play_live(project.root, muted_except(project.snapshot, project.wet),
                          std::span(&scan, 1), block_128, frames),
                "effect wet stem");
  require_nulls(dry,
                play_live(project.root, muted_except(project.snapshot, project.dry),
                          std::span(&scan, 1), block_128, frames),
                "effect dry stem");
}

// A latent plugin's export lines up with its parallel dry path and with live
// playback, whose route latency it trims.
void latent_effect_timing(plugins::Vst3Scanner& scanner) {
  const oscillate::test::TempDir directory("oscillate-vst3-export-parity");
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "latent fixture scan failed: " + scan.diagnostic);
  const auto project = effect_project(directory / "latent", scan, latent_state);
  const auto master =
      export_vst3(project.root, project.snapshot, scanner, directory / "master.wav");
  // The latency is trimmed: nothing sounds before the first clip, which
  // sounds from its first frame.
  const auto lead =
      std::span<const float>(master).first(2 * static_cast<std::size_t>(wet_clip_first));
  REQUIRE_LT(peak(lead), parity_tolerance, "nothing sounds before the first clip");
  const auto frames = master.size() / 2;
  for (const auto block_frames : {block_64, block_128}) {
    const auto live =
        play_live(project.root, project.snapshot, std::span(&scan, 1), block_frames, frames);
    REQUIRE_EQ(live.latency, latent_frames, "live playback reports the plugin's latency");
    require_nulls(master, live, "latent master at " + std::to_string(block_frames) + " frames");
  }
  const auto dry =
      export_vst3(project.root, project.snapshot, scanner, directory / "dry.wav", project.dry);
  require_nulls(dry,
                play_live(project.root, muted_except(project.snapshot, project.dry),
                          std::span(&scan, 1), block_128, frames),
                "latent dry stem");
}

// A latent VST3 effect on the master runs after the mix, as live playback
// runs it on the master node: the master and each stem (the master chain
// included) null against live playback.
void master_slot(plugins::Vst3Scanner& scanner) {
  const oscillate::test::TempDir directory("oscillate-vst3-export-parity");
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "latent fixture scan failed: " + scan.diagnostic);
  auto project = effect_project(directory / "master-slot", scan, latent_state);
  auto& wet = *std::ranges::find(project.snapshot.tracks, project.wet, &project::Track::id);
  project.snapshot.master_effects.push_back(std::move(wet.effects.front()));
  wet.effects.clear();
  const auto master =
      export_vst3(project.root, project.snapshot, scanner, directory / "master.wav");
  const auto frames = master.size() / 2;
  for (const auto block_frames : {block_64, block_128}) {
    const auto live =
        play_live(project.root, project.snapshot, std::span(&scan, 1), block_frames, frames);
    REQUIRE_EQ(live.latency, latent_frames, "live playback reports the master plugin's latency");
    require_nulls(master, live, "master slot at " + std::to_string(block_frames) + " frames");
  }
  for (const auto track : {project.wet, project.dry})
    require_nulls(
        export_vst3(project.root, project.snapshot, scanner, directory / "stem.wav", track),
        play_live(project.root, muted_except(project.snapshot, track), std::span(&scan, 1),
                  block_128, frames),
        "master slot stem");
}

// A VST3 instrument's master, its stem and its finite release null against
// live playback.
void instrument_master_and_stem(plugins::Vst3Scanner& scanner) {
  const oscillate::test::TempDir directory("oscillate-vst3-export-parity");
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "instrument fixture scan failed: " + scan.diagnostic);
  const auto project = instrument_project(directory / "instrument", scan);
  const auto master =
      export_vst3(project.root, project.snapshot, scanner, directory / "master.wav");
  const auto stem =
      export_vst3(project.root, project.snapshot, scanner, directory / "stem.wav", project.track);
  REQUIRE_EQ(stem.size(), master.size(), "the one-source stem is as long as the master");
  // The export ends with the declared release, which still sounds.
  const auto release = std::span<const float>(master).last(2 * instrument_tail_frames);
  REQUIRE_GT(peak(release), tolerance::coarse, "the instrument's release is exported");
  require_nulls(
      master,
      play_live(project.root, project.snapshot, std::span(&scan, 1), block_128, master.size() / 2),
      "instrument master");
  require_nulls(
      stem,
      play_live(project.root, project.snapshot, std::span(&scan, 1), block_128, stem.size() / 2),
      "instrument stem");
}

// A VST3 instrument through a latent VST3 effect on its own track, beside an
// audio track: the master nulls against live playback, and the stem bundle
// holds exactly the single stems.
void instrument_through_latent_effect(plugins::Vst3Scanner& scanner) {
  const oscillate::test::TempDir directory("oscillate-vst3-export-parity");
  const std::vector scans{scanner.scan(VST3_INSTRUMENT_BUNDLE), scanner.scan(VST3_GAIN_BUNDLE)};
  for (const auto& scan : scans)
    require(scan.ok(), "fixture scan failed: " + scan.diagnostic);
  auto audio = effect_project(directory / "song", scans[1], latent_state);
  const auto instrument = instrument_project(directory / "instrument", scans[0]);
  // The instrument track joins the audio project, its plugin's identity and
  // MIDI unchanged; the latent effect moves from the wet audio track onto it.
  auto snapshot = audio.snapshot;
  auto& wet = *std::ranges::find(snapshot.tracks, audio.wet, &project::Track::id);
  auto track = instrument.snapshot.tracks.front();
  constexpr std::uint64_t joined_track = 20; // Free in the audio project.
  track.id = id<project::TrackId>(joined_track);
  track.sends.clear();
  track.effects.push_back(std::move(wet.effects.front()));
  wet.effects.clear();
  snapshot.tracks.push_back(std::move(track));
  const auto master = export_vst3(audio.root, snapshot, scanner, directory / "master.wav");
  require_nulls(master, play_live(audio.root, snapshot, scans, block_128, master.size() / 2),
                "instrument through a latent effect");
  const auto bundle = directory / "stems";
  {
    Gate gate;
    auto store = take(persistence::ProjectStore::open_reader(audio.root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    REQUIRE_OK(
        exporting::render_vst3_stems(snapshot, store, std::move(authority), scanner, {bundle}),
        "VST3 stem bundle published");
  }
  for (const auto& source : snapshot.tracks) {
    if (source.kind != project::TrackKind::audio && source.kind != project::TrackKind::instrument)
      continue;
    const auto single =
        export_vst3(audio.root, snapshot, scanner, directory / "single.wav", source.id);
    REQUIRE_EQ(
        oscillate::test::read_float_wav(bundle / ("track-" + source.id.to_string() + ".wav")),
        single, "a bundled stem is the single stem");
    REQUIRE_EQ(single.size(), master.size(), "every stem is as long as the master");
  }
}

} // namespace

int main() {
  try {
    REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", instrument_tail_setting, 1), 0,
               "cannot enable the finite-tail instrument fixture");
    plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
    effect_master_and_stems(scanner);
    latent_effect_timing(scanner);
    master_slot(scanner);
    instrument_through_latent_effect(scanner);
    instrument_master_and_stem(scanner);
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "VST3 export parity passed\n";
  return 0;
}
