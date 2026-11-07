#include "insert_fixtures.hpp"
#include "oscillate/application/vst3/offline_export.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "vst3_export_fixtures.hpp"
#include "warp_oracle.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};
struct Directory {
  fs::path path =
      fs::temp_directory_path() / ("oscillate-vst3-mixed-export-" + std::to_string(::getpid()));
  Directory() {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~Directory() { fs::remove_all(path); }
};

void u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  u16(output, static_cast<std::uint16_t>(value));
  u16(output, static_cast<std::uint16_t>(value >> 16U));
}
struct ActiveSamples {
  std::uint16_t left = 4'096;
  std::uint16_t right = 8'192;
};
std::vector<std::uint8_t> source_wav(ActiveSamples active = {}) {
  std::vector<std::uint8_t> output{'R', 'I', 'F', 'F'};
  u32(output, 36 + 12 * 4);
  output.insert(output.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(output, 16);
  u16(output, 1);
  u16(output, 2);
  u32(output, 48'000);
  u32(output, 48'000 * 4);
  u16(output, 4);
  u16(output, 16);
  output.insert(output.end(), {'d', 'a', 't', 'a'});
  u32(output, 12 * 4);
  for (int frame = 0; frame < 12; ++frame) {
    // Out-of-range samples are deliberately different to catch trim errors.
    u16(output, frame >= 2 && frame < 10 ? active.left : 16'384);
    u16(output, frame >= 2 && frame < 10 ? active.right : 24'576);
  }
  return output;
}
std::vector<std::uint8_t> sine_source_wav() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_44k;
  constexpr std::uint32_t frames = 4'410;
  std::vector<std::uint8_t> output{'R', 'I', 'F', 'F'};
  u32(output, 36 + frames * 4);
  output.insert(output.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(output, 16);
  u16(output, 1);
  u16(output, 2);
  u32(output, rate);
  u32(output, rate * 4);
  u16(output, 4);
  u16(output, 16);
  output.insert(output.end(), {'d', 'a', 't', 'a'});
  u32(output, frames * 4);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = 2.0 * std::numbers::pi * 440.0 * frame / rate;
    const auto left = static_cast<std::int16_t>(std::lround(4'096.0 * std::sin(phase)));
    const auto right = static_cast<std::int16_t>(std::lround(-2'048.0 * std::sin(phase)));
    u16(output, static_cast<std::uint16_t>(left));
    u16(output, static_cast<std::uint16_t>(right));
  }
  return output;
}
std::vector<std::uint8_t> read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "rendered WAV could not open");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
float sample(const std::vector<std::uint8_t>& wav, std::size_t frame, std::size_t channel = 0) {
  REQUIRE_GE(wav.size(), 56 + (frame + 1) * 8, "rendered WAV frame is missing");
  float value = 0.0F;
  std::memcpy(&value, wav.data() + 56 + frame * 8 + channel * 4, sizeof(value));
  return value;
}
void near(float value, float expected) {
  if (std::fabs(value - expected) > 0.00001F)
    throw std::runtime_error("mixed export sample=" + std::to_string(value) +
                             " expected=" + std::to_string(expected));
}

struct MixedProject {
  project::Snapshot snapshot;
  persistence::ProjectStore store;
};
MixedProject make_project(Directory& directory, Gate& gate, const plugins::ScanResult& scan) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  auto model = take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000,
                                                  std::move(tempo), std::move(meter), gate));
  const auto& klass = scan.metadata.classes.front();
  project::Vst3PluginInstance plugin{.id = id<project::PluginInstanceId>(5),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = scan.binary.canonical_bundle.string(),
                                     .binary_sha256 = scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  const float level = 0.5F;
  plugin.component_state.resize(sizeof(level));
  std::memcpy(plugin.component_state.data(), &level, sizeof(level));
  const auto instrument = id<project::TrackId>(2);
  const auto return_bus = id<project::TrackId>(3);
  const auto audio = id<project::TrackId>(9);
  const project::MidiClip midi{id<project::ClipId>(4),
                               project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
                               {timeline::ticks_per_quarter},
                               false,
                               {{id<project::NoteId>(6), {200'000}, {10'240'000}, 60, 127}},
                               1};
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", true));
  const auto input = directory.path / "input.wav";
  const auto wav = source_wav();
  {
    std::ofstream file(input, std::ios::binary);
    file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    require(file.good(), "input WAV fixture write failed");
  }
  const int descriptor = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "input WAV fixture open failed");
  auto staged = writer.stage_wav(id<project::SourceId>(8), descriptor);
  REQUIRE_EQ(::close(descriptor), 0, "input WAV fixture close failed");
  auto receipt = take(std::move(staged));
  const project::Clip clip{id<project::ClipId>(10),
                           receipt.record().id,
                           {{2}, {10}},
                           project::ElapsedInterval{{260}, {268}}};
  (void)take(
      model.apply({project::AddTrack{instrument, project::TrackKind::instrument},
                   project::AddTrack{return_bus, project::TrackKind::return_bus},
                   // Last, after the return bus, as the fixture's rows expect.
                   project::AddTrack{audio, project::TrackKind::audio, 2},
                   project::AddSource{receipt.record()}, project::AddMidiClip{instrument, midi},
                   project::InsertVst3Plugin{instrument, 0, plugin}, project::AddClip{audio, clip},
                   project::AddSendRoute{instrument, {id<project::SendId>(7), return_bus, 0.5}},
                   project::AddSendRoute{audio, {id<project::SendId>(11), return_bus, 0.5}}}));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save,
                              std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "mixed project save did not commit");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  auto reopened = take(reader.load_current());
  REQUIRE_EQ(reopened.snapshot.tracks.size(), 3,
             "mixed project audio source or trim did not reopen");
  REQUIRE_EQ(reopened.snapshot.sources.size(), 1,
             "mixed project audio source or trim did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks.back().clips.front(), clip,
             "mixed project audio source or trim did not reopen");
  return {std::move(reopened.snapshot), std::move(reader)};
}

MixedProject make_multi_project(Directory& directory, Gate& gate, const plugins::ScanResult& scan) {
  auto mixed = make_project(directory, gate, scan);
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", false));
  auto model = take(project::ProjectModel::open(mixed.snapshot, gate));
  const auto input = directory.path / "second.wav";
  const auto wav = source_wav({2'048, 4'096});
  {
    std::ofstream file(input, std::ios::binary);
    file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    require(file.good(), "second input WAV fixture write failed");
  }
  const int descriptor = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "second input WAV fixture open failed");
  auto staged = writer.stage_wav(id<project::SourceId>(13), descriptor);
  REQUIRE_EQ(::close(descriptor), 0, "second input WAV fixture close failed");
  auto receipt = take(std::move(staged));
  const auto first_audio = id<project::TrackId>(9);
  const auto second_audio = id<project::TrackId>(12);
  const auto return_bus = id<project::TrackId>(3);
  const auto first_source = id<project::SourceId>(8);
  const project::Clip earlier{
      id<project::ClipId>(14), first_source, {{2}, {6}}, project::ElapsedInterval{{126}, {130}}};
  const project::Clip later{
      id<project::ClipId>(16), first_source, {{6}, {10}}, project::ElapsedInterval{{270}, {274}}};
  const project::Clip second{id<project::ClipId>(17),
                             receipt.record().id,
                             {{2}, {6}},
                             project::ElapsedInterval{{127}, {131}}};
  const project::Clip shared{
      id<project::ClipId>(18), first_source, {{0}, {2}}, project::ElapsedInterval{{140}, {142}}};
  (void)take(model.apply(
      {project::AddTrack{second_audio, project::TrackKind::audio, model.snapshot()->tracks.size()},
       project::AddSource{receipt.record()}, project::AddClip{first_audio, earlier},
       project::AddClip{first_audio, later}, project::AddClip{second_audio, second},
       project::AddClip{second_audio, shared},
       project::AddSendRoute{second_audio, {id<project::SendId>(19), return_bus, 0.5}}}));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save,
                              std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "multi-audio project save did not commit");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  auto reopened = take(reader.load_current());
  REQUIRE_EQ(reopened.snapshot.tracks.size(), 4,
             "multi-audio clips, order or source identity did not reopen");
  REQUIRE_EQ(reopened.snapshot.sources.size(), 2,
             "multi-audio clips, order or source identity did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks[2].clips.size(), 3,
             "multi-audio clips, order or source identity did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks[2].clips[1], earlier,
             "multi-audio clips, order or source identity did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks[3].clips.size(), 2,
             "multi-audio clips, order or source identity did not reopen");
  REQUIRE_EQ(reopened.snapshot.tracks[3].clips[1], shared,
             "multi-audio clips, order or source identity did not reopen");
  return {std::move(reopened.snapshot), std::move(reader)};
}

void simultaneous_instrument_effect_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto instrument_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  REQUIRE_TRUE(instrument_scan.ok(), "mixed plugin fixture scans failed");
  REQUIRE_TRUE(effect_scan.ok(), "mixed plugin fixture scans failed");
  auto mixed = make_project(directory, gate, instrument_scan);
  const auto& klass = effect_scan.metadata.classes.front();
  project::Vst3PluginInstance effect{.id = id<project::PluginInstanceId>(30),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = effect_scan.binary.canonical_bundle.string(),
                                     .binary_sha256 = effect_scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  const float gain = 2.0F;
  effect.component_state.resize(sizeof(gain));
  std::memcpy(effect.component_state.data(), &gain, sizeof(gain));
  auto model = take(project::ProjectModel::open(mixed.snapshot, gate));
  (void)take(model.apply({project::InsertVst3Plugin{id<project::TrackId>(9), 0, effect}}));
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", false));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save, {})).status,
             persistence::SaveStatus::committed, "simultaneous plugin project save failed");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  mixed.snapshot = take(reader.load_current()).snapshot;
  mixed.store = std::move(reader);
  REQUIRE_EQ(mixed.snapshot.tracks.back().effects.size(), 1,
             "saved audio effect identity or state did not reopen");
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(mixed.snapshot.tracks.back().effects.front().processor),
      effect, "saved audio effect identity or state did not reopen");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set simultaneous instrument tail");
  const auto master_path = directory.path / "simultaneous-master.wav";
  auto lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         exporting::MasterRenderOptions{master_path})),
      exporting::PublishOutcome::durable, "simultaneous instrument/effect master failed");
  const auto master = read_file(master_path);
  // The audio clip ends at 268; the shared render pads the latest content
  // with the longest path's tail, the instrument's eight frames.
  REQUIRE_EQ(master.size(), 56 + 276 * 8, "simultaneous master latency or tail is incorrect");
  near(sample(master, 5), 0.75F);
  near(sample(master, 260), 0.75F);
  near(sample(master, 260, 1), 0.375F);
  const auto instrument_path = directory.path / "simultaneous-instrument.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(2), {instrument_path}));
  const auto audio_path = directory.path / "simultaneous-audio.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(9), {audio_path}));
  const auto instrument = read_file(instrument_path);
  const auto audio = read_file(audio_path);
  REQUIRE_EQ(instrument.size(), master.size(), "simultaneous processed stems lost common duration");
  REQUIRE_EQ(audio.size(), master.size(), "simultaneous processed stems lost common duration");
  near(sample(instrument, 260), 0.375F);
  near(sample(audio, 260), 0.375F);
  for (std::size_t frame = 0; frame < 276; ++frame) {
    near(sample(master, frame), sample(instrument, frame) + sample(audio, frame));
    near(sample(master, frame, 1), sample(instrument, frame, 1) + sample(audio, frame, 1));
  }
  const auto repeat_path = directory.path / "simultaneous-repeat.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                           exporting::MasterRenderOptions{repeat_path}));
  REQUIRE_EQ(read_file(repeat_path), master, "simultaneous repeat render differs");

  auto long_latency = mixed.snapshot;
  const float latency_setting = -96.0F;
  auto& delayed_effect =
      std::get<project::Vst3PluginInstance>(long_latency.tracks.back().effects.front().processor);
  std::memcpy(delayed_effect.component_state.data(), &latency_setting, sizeof(latency_setting));
  const auto latency_path = directory.path / "simultaneous-latency.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(long_latency, gate));
  (void)take(exporting::render_vst3_master(long_latency, mixed.store, std::move(lease), scanner,
                                           exporting::MasterRenderOptions{latency_path}));
  const auto delayed = read_file(latency_path);
  REQUIRE_EQ(delayed.size(), master.size(), "96-frame effect latency changed output extent");
  near(sample(delayed, 5), 0.75F);
  near(sample(delayed, 260), 0.46875F);
  near(sample(delayed, 260, 1), -0.1875F);

  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "mixed positive-tail fixture scan failed");
  auto tail_snapshot = mixed.snapshot;
  auto& tail_effect =
      std::get<project::Vst3PluginInstance>(tail_snapshot.tracks.back().effects.front().processor);
  const auto& tail_class = tail_scan.metadata.classes.front();
  tail_effect.class_id = tail_class.id;
  tail_effect.vendor = tail_class.vendor;
  tail_effect.name = tail_class.name;
  tail_effect.version = tail_class.version;
  tail_effect.bundle_path = tail_scan.binary.canonical_bundle.string();
  tail_effect.binary_sha256 = tail_scan.binary.sha256;
  const auto tail_path = directory.path / "simultaneous-tail.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(tail_snapshot, gate));
  (void)take(exporting::render_vst3_master(tail_snapshot, mixed.store, std::move(lease), scanner,
                                           exporting::MasterRenderOptions{tail_path}));
  const auto tailed = read_file(tail_path);
  REQUIRE_EQ(tailed.size(), 56 + 276 * 8, "mixed positive effect tail is truncated");
  near(sample(tailed, 267), 0.0F);
  near(sample(tailed, 268), 0.375F);
  near(sample(tailed, 275), 0.375F);

  auto rejected = mixed.snapshot;
  auto& saved_effect =
      std::get<project::Vst3PluginInstance>(rejected.tracks.back().effects.front().processor);
  saved_effect.binary_sha256[0] ^= 1;
  const auto rejected_path = directory.path / "rejected.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(rejected, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(rejected, mixed.store, std::move(lease), scanner,
                                              exporting::MasterRenderOptions{rejected_path}),
               "changed audio effect was omitted or published");
  REQUIRE_TRUE(!fs::exists(rejected_path), "changed audio effect was omitted or published");
  rejected = mixed.snapshot;
  auto& wrong_class =
      std::get<project::Vst3PluginInstance>(rejected.tracks.back().effects.front().processor);
  wrong_class.version += "-changed";
  lease = take(exporting::StoppedExportAuthority::acquire(rejected, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(rejected, mixed.store, std::move(lease), scanner,
                                              exporting::MasterRenderOptions{rejected_path}),
               "changed audio effect class was omitted or published");
  REQUIRE_TRUE(!fs::exists(rejected_path), "changed audio effect class was omitted or published");
  rejected = mixed.snapshot;
  auto& missing_effect =
      std::get<project::Vst3PluginInstance>(rejected.tracks.back().effects.front().processor);
  missing_effect.bundle_path = (directory.path / "missing-effect.vst3").string();
  lease = take(exporting::StoppedExportAuthority::acquire(rejected, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(rejected, mixed.store, std::move(lease), scanner,
                                              exporting::MasterRenderOptions{rejected_path}),
               "missing audio effect was omitted or published");
  REQUIRE_TRUE(!fs::exists(rejected_path), "missing audio effect was omitted or published");
  rejected = mixed.snapshot;
  const auto bad_gain = std::numeric_limits<float>::quiet_NaN();
  auto& invalid_state =
      std::get<project::Vst3PluginInstance>(rejected.tracks.back().effects.front().processor);
  std::memcpy(invalid_state.component_state.data(), &bad_gain, sizeof(bad_gain));
  lease = take(exporting::StoppedExportAuthority::acquire(rejected, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(rejected, mixed.store, std::move(lease), scanner,
                                              exporting::MasterRenderOptions{rejected_path}),
               "invalid audio effect state was omitted or published");
  REQUIRE_TRUE(!fs::exists(rejected_path), "invalid audio effect state was omitted or published");
  rejected = mixed.snapshot;
  const float failing_gain = 999.0F;
  auto& failing_effect =
      std::get<project::Vst3PluginInstance>(rejected.tracks.back().effects.front().processor);
  std::memcpy(failing_effect.component_state.data(), &failing_gain, sizeof(failing_gain));
  lease = take(exporting::StoppedExportAuthority::acquire(rejected, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(rejected, mixed.store, std::move(lease), scanner,
                                              exporting::MasterRenderOptions{master_path}),
               "effect process fault replaced existing output or left a partial WAV");
  REQUIRE_EQ(read_file(master_path), master,
             "effect process fault replaced existing output or left a partial WAV");
  rejected = mixed.snapshot;
  const float excessive_latency = -95.0F;
  auto& over_budget =
      std::get<project::Vst3PluginInstance>(rejected.tracks.back().effects.front().processor);
  std::memcpy(over_budget.component_state.data(), &excessive_latency, sizeof(excessive_latency));
  lease = take(exporting::StoppedExportAuthority::acquire(rejected, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(rejected, mixed.store, std::move(lease), scanner,
                                              exporting::MasterRenderOptions{rejected_path}),
               "over-budget effect latency was silently rendered");
  REQUIRE_TRUE(!fs::exists(rejected_path), "over-budget effect latency was silently rendered");
  // A bypassed effect beside the instrument renders as the project without
  // it: the audio track plays dry.
  auto bypassed = mixed.snapshot;
  bypassed.tracks.back().effects.front().enabled = false;
  auto dry = mixed.snapshot;
  dry.tracks.back().effects.clear();
  const auto bypassed_path = directory.path / "simultaneous-bypassed.wav";
  const auto dry_path = directory.path / "simultaneous-dry.wav";
  for (const auto& [candidate, path] :
       {std::pair{&bypassed, &bypassed_path}, std::pair{&dry, &dry_path}}) {
    lease = take(exporting::StoppedExportAuthority::acquire(*candidate, gate));
    REQUIRE_EQ(take(exporting::render_vst3_master(*candidate, mixed.store, std::move(lease),
                                                  scanner, exporting::MasterRenderOptions{*path})),
               exporting::PublishOutcome::durable, "a bypassed effect beside the source failed");
  }
  REQUIRE_EQ(read_file(bypassed_path), read_file(dry_path),
             "a bypassed effect changed the mixed master");
  for (const auto* mode : {"infinite-tail", "activate-fail"}) {
    REQUIRE_EQ(::setenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE", mode, 1), 0,
               "cannot select effect tail failure mode");
    lease = take(exporting::StoppedExportAuthority::acquire(tail_snapshot, gate));
    REQUIRE_TRUE(!exporting::render_vst3_master(tail_snapshot, mixed.store, std::move(lease),
                                                scanner,
                                                exporting::MasterRenderOptions{rejected_path}),
                 "invalid effect tail or activation was published");
    REQUIRE_TRUE(!fs::exists(rejected_path), "invalid effect tail or activation was published");
    REQUIRE_EQ(::unsetenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE"), 0,
               "cannot clear effect tail failure mode");
  }
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear simultaneous instrument tail");
}

void instrument_track_effect_survives_reopen_and_export() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto instrument_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  REQUIRE_TRUE(instrument_scan.ok(), "instrument-chain scans failed");
  REQUIRE_TRUE(effect_scan.ok(), "instrument-chain scans failed");
  auto mixed = make_project(directory, gate, instrument_scan);
  const auto& klass = effect_scan.metadata.classes.front();
  project::Vst3PluginInstance effect{.id = id<project::PluginInstanceId>(33),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = effect_scan.binary.canonical_bundle.string(),
                                     .binary_sha256 = effect_scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  const float gain = 3.0F;
  effect.component_state.resize(sizeof(gain));
  std::memcpy(effect.component_state.data(), &gain, sizeof(gain));
  auto model = take(project::ProjectModel::open(mixed.snapshot, gate));
  (void)take(model.apply({project::InsertVst3Plugin{id<project::TrackId>(2), 0, effect}}));
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", false));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save, {})).status,
             persistence::SaveStatus::committed, "instrument-chain project save failed");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  mixed.snapshot = take(reader.load_current()).snapshot;
  mixed.store = std::move(reader);
  REQUIRE_EQ(mixed.snapshot.tracks.front().effects.size(), 1,
             "instrument-track effect changed on reopen");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 mixed.snapshot.tracks.front().effects.front().processor),
             effect, "instrument-track effect changed on reopen");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set instrument-chain fixture tail");
  const auto master_path = directory.path / "instrument-chain-master.wav";
  auto lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                           exporting::MasterRenderOptions{master_path}));
  const auto stem_path = directory.path / "instrument-chain-stem.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(2), {stem_path}));
  const auto master = read_file(master_path);
  const auto stem = read_file(stem_path);
  auto dry = mixed.snapshot;
  dry.tracks.front().effects.clear();
  const auto dry_path = directory.path / "instrument-chain-dry.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(dry, gate));
  (void)take(exporting::render_vst3_stem(dry, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(2), {dry_path}));
  const auto unprocessed = read_file(dry_path);
  REQUIRE_GT(std::fabs(sample(unprocessed, 5)), 0.01F,
             "instrument-chain fixture did not produce a note");
  near(sample(stem, 5), sample(unprocessed, 5) * gain);
  near(sample(stem, 260), sample(unprocessed, 260) * gain);
  const auto audio_path = directory.path / "instrument-chain-audio.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(9), {audio_path}));
  const auto audio = read_file(audio_path);
  near(sample(master, 5), sample(stem, 5) + sample(audio, 5));
  REQUIRE_EQ(master.size(), stem.size(), "instrument-chain stem lost effect tail");
  auto serial = mixed.snapshot;
  auto second = effect;
  second.id = id<project::PluginInstanceId>(35);
  const float second_gain = 2.0F;
  std::memcpy(second.component_state.data(), &second_gain, sizeof(second_gain));
  auto serial_model = take(project::ProjectModel::open(serial, gate));
  (void)take(serial_model.apply({project::InsertVst3Plugin{id<project::TrackId>(2), 1, second}}));
  serial = *serial_model.snapshot();
  const auto serial_master_path = directory.path / "instrument-two-effects-master.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(serial, gate));
  (void)take(exporting::render_vst3_master(serial, mixed.store, std::move(lease), scanner,
                                           exporting::MasterRenderOptions{serial_master_path}));
  const auto serial_stem_path = directory.path / "instrument-two-effects-stem.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(serial, gate));
  (void)take(exporting::render_vst3_stem(serial, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(2), {serial_stem_path}));
  const auto serial_master = read_file(serial_master_path);
  const auto serial_stem = read_file(serial_stem_path);
  near(sample(serial_stem, 5), sample(unprocessed, 5) * gain * second_gain);
  near(sample(serial_stem, 260), sample(unprocessed, 260) * gain * second_gain);
  REQUIRE_EQ(serial_master.size(), serial_stem.size(),
             "two-effect instrument stem lost master extent");
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "serial instrument tail fixture scan failed");
  auto serial_tail = serial;
  auto& last =
      std::get<project::Vst3PluginInstance>(serial_tail.tracks.front().effects.back().processor);
  const auto& tail_class = tail_scan.metadata.classes.front();
  last.class_id = tail_class.id;
  last.vendor = tail_class.vendor;
  last.name = tail_class.name;
  last.version = tail_class.version;
  last.bundle_path = tail_scan.binary.canonical_bundle.string();
  last.binary_sha256 = tail_scan.binary.sha256;
  const auto serial_tail_path = directory.path / "instrument-two-effects-tail.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(serial_tail, gate));
  (void)take(exporting::render_vst3_stem(serial_tail, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(2), {serial_tail_path}));
  const auto tailed = read_file(serial_tail_path);
  REQUIRE_EQ(tailed.size(), serial_stem.size() + 8 * 8,
             "second serial VST3 effect tail was truncated");
  near(sample(tailed, 13), sample(serial_stem, 5));
  auto serial_rejected = serial;
  serial_rejected.tracks.front().effects.back().enabled = false;
  // A bypassed second effect leaves an ordinary one-effect chain.
  lease = take(exporting::StoppedExportAuthority::acquire(serial_rejected, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_master(
          serial_rejected, mixed.store, std::move(lease), scanner,
          exporting::MasterRenderOptions{directory.path / "instrument-two-effects-bypassed.wav"})),
      exporting::PublishOutcome::durable, "a bypassed second effect failed the export");
  serial_rejected = serial;
  serial_rejected.tracks.front().effects.push_back(serial_rejected.tracks.front().effects.back());
  lease = take(exporting::StoppedExportAuthority::acquire(serial_rejected, gate));
  require(!exporting::render_vst3_master(
              serial_rejected, mixed.store, std::move(lease), scanner,
              exporting::MasterRenderOptions{directory.path / "instrument-three-effects.wav"}),
          "a repeated slot identity reached the writer");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear instrument-chain fixture tail");
}

void paired_instrument_effect_paths_export() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto instrument_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto gain_scan = scanner.scan(VST3_GAIN_BUNDLE);
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  REQUIRE_TRUE(instrument_scan.ok(), "paired source/effect fixture scans failed");
  REQUIRE_TRUE(gain_scan.ok(), "paired source/effect fixture scans failed");
  REQUIRE_TRUE(tail_scan.ok(), "paired source/effect fixture scans failed");
  auto snapshot = make_project(directory, gate, instrument_scan).snapshot;
  snapshot.tracks.erase(snapshot.tracks.begin() + 1, snapshot.tracks.end());
  snapshot.sources.clear();
  snapshot.tracks.front().sends.clear();
  auto second = snapshot.tracks.front();
  second.id = id<project::TrackId>(30);
  second.midi_clips.front().id = id<project::ClipId>(40);
  second.midi_clips.front().notes.front().id = id<project::NoteId>(41);
  second.midi_clips.front().notes.front().start = {400'000};
  second.midi_clips.front().notes.front().end = {10'440'000};
  second.level = 0.5;
  second.pan = 0.5;
  auto& second_source = test_insert::vst3_source(second);
  second_source.id = id<project::PluginInstanceId>(31);
  const float second_level = 0.25F;
  std::memcpy(second_source.component_state.data(), &second_level, sizeof(second_level));
  snapshot.tracks.push_back(std::move(second));
  auto effect = [](project::PluginInstanceId id, const plugins::ScanResult& scan, float gain) {
    const auto& klass = scan.metadata.classes.front();
    project::Vst3PluginInstance saved{.id = id,
                                      .class_id = klass.id,
                                      .vendor = klass.vendor,
                                      .name = klass.name,
                                      .version = klass.version,
                                      .bundle_path = scan.binary.canonical_bundle.string(),
                                      .binary_sha256 = scan.binary.sha256,
                                      .component_state = {},
                                      .parameters = {},
                                      .missing = true};
    saved.component_state.resize(sizeof(gain));
    std::memcpy(saved.component_state.data(), &gain, sizeof(gain));
    return saved;
  };
  test_insert::append_effect(snapshot, snapshot.tracks.front(),
                             effect(id<project::PluginInstanceId>(33), tail_scan, 2.0F));
  test_insert::append_effect(snapshot, snapshot.tracks.back(),
                             effect(id<project::PluginInstanceId>(35), gain_scan, -96.0F));
  ++snapshot.revision;
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "paired", true));
  REQUIRE_EQ(take(writer.save(snapshot, persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "paired source/effect project did not save");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "paired"));
  snapshot = take(reader.load_current()).snapshot;
  REQUIRE_EQ(snapshot.tracks.size(), 2, "paired source/effect identities changed on reopen");
  REQUIRE_EQ(snapshot.tracks.front().effects.size(), 1,
             "paired source/effect identities changed on reopen");
  REQUIRE_EQ(snapshot.tracks.back().effects.size(), 1,
             "paired source/effect identities changed on reopen");
  REQUIRE_EQ(test_insert::vst3_source(snapshot.tracks.back()).id, id<project::PluginInstanceId>(31),
             "paired source/effect identities changed on reopen");
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(snapshot.tracks.back().effects.front().processor).id,
      id<project::PluginInstanceId>(35), "paired source/effect identities changed on reopen");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set paired instrument tail");
  const auto render_master = [&](const project::Snapshot& project, const fs::path& path) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(project, gate));
    return exporting::render_vst3_master(project, reader, std::move(lease), scanner,
                                         exporting::MasterRenderOptions{path});
  };
  const auto master_path = directory.path / "paired-master.wav";
  REQUIRE_EQ(take(render_master(snapshot, master_path)), exporting::PublishOutcome::durable,
             "paired source/effect master failed");
  const auto master = read_file(master_path);
  const auto frames = (master.size() - 56) / 8;
  // The first source's release, delayed eight frames by TailGain, ends at 272.
  REQUIRE_GE(frames, 272, "paired source/effect tail was truncated");
  std::array<std::vector<std::uint8_t>, 2> stems;
  for (std::size_t index = 0; index < 2; ++index) {
    const auto stem_path = directory.path / ("paired-stem-" + std::to_string(index) + ".wav");
    auto lease = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    REQUIRE_EQ(take(exporting::render_vst3_stem(snapshot, reader, std::move(lease), scanner,
                                                snapshot.tracks[index].id, {stem_path})),
               exporting::PublishOutcome::durable, "paired source/effect stem failed");
    stems[index] = read_file(stem_path);
    REQUIRE_EQ(stems[index].size(), master.size(), "paired stem lost master extent");
    // Hand-calculated fixture oracle: 120 BPM at 48 kHz places the first
    // note at frames 5..255 and the second at 10..260. Note-off releases eight
    // frames. TailGain delays eight frames and doubles the first source;
    // LoadedGain's -96 fixture state delays 96 frames at half gain, which the
    // graph's 96-frame output alignment removes from the second source.
    const auto& track = snapshot.tracks[index];
    const auto voice = [index](std::size_t frame) {
      const auto first = index == 0 ? 5U : 10U;
      const auto off = index == 0 ? 256U : 261U;
      const float level = index == 0 ? 0.5F : 0.25F;
      if (frame < first || frame >= off + 8)
        return 0.0F;
      return frame < off ? level : level * static_cast<float>(off + 8 - frame) / 8.0F;
    };
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const auto processed =
          index == 0 ? (frame >= 8 ? 2.0F * voice(frame - 8) : 0.0F) : 0.5F * voice(frame);
      const auto left = processed * static_cast<float>(track.level) *
                        (track.pan > 0.0 ? static_cast<float>(1.0 - track.pan) : 1.0F);
      const auto right = -processed * static_cast<float>(track.level) *
                         (track.pan < 0.0 ? static_cast<float>(1.0 + track.pan) : 1.0F);
      near(sample(stems[index], frame), left);
      near(sample(stems[index], frame, 1), right);
    }
  }
  for (std::size_t frame = 0; frame < frames; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(master, frame, channel),
           sample(stems[0], frame, channel) + sample(stems[1], frame, channel));
  auto changed = snapshot;
  auto& changed_effect =
      std::get<project::Vst3PluginInstance>(changed.tracks.back().effects.front().processor);
  changed_effect.binary_sha256[0] = changed_effect.binary_sha256[0] == '0' ? '1' : '0';
  const auto refused = render_master(changed, master_path);
  REQUIRE_TRUE(!refused, "changed second effect replaced an existing master");
  REQUIRE_NE(refused.error().message.find("binary differs"), std::string::npos,
             "changed second effect replaced an existing master");
  REQUIRE_EQ(read_file(master_path), master, "changed second effect replaced an existing master");
  const auto retained_stem_path = directory.path / "paired-stem-1.wav";
  auto stem_lease = take(exporting::StoppedExportAuthority::acquire(changed, gate));
  REQUIRE_TRUE(!exporting::render_vst3_stem(changed, reader, std::move(stem_lease), scanner,
                                            snapshot.tracks.back().id, {retained_stem_path}),
               "changed second effect replaced an existing stem");
  REQUIRE_EQ(read_file(retained_stem_path), stems[1],
             "changed second effect replaced an existing stem");
  changed = snapshot;
  auto& missing_effect =
      std::get<project::Vst3PluginInstance>(changed.tracks.back().effects.front().processor);
  missing_effect.bundle_path = (directory.path / "missing-second-effect.vst3").string();
  REQUIRE_TRUE(!render_master(changed, master_path),
               "missing second effect replaced an existing master");
  REQUIRE_EQ(read_file(master_path), master, "missing second effect replaced an existing master");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear paired instrument tail");
}

void instrument_and_bus_effect_share_offline_routing() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto source_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  REQUIRE_TRUE(source_scan.ok(), "instrument/bus scans failed");
  REQUIRE_TRUE(effect_scan.ok(), "instrument/bus scans failed");
  auto mixed = make_project(directory, gate, source_scan);
  const auto& klass = effect_scan.metadata.classes.front();
  project::Vst3PluginInstance effect{.id = id<project::PluginInstanceId>(34),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = effect_scan.binary.canonical_bundle.string(),
                                     .binary_sha256 = effect_scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  const float gain = 2.0F;
  effect.component_state.resize(sizeof(gain));
  std::memcpy(effect.component_state.data(), &gain, sizeof(gain));
  auto model = take(project::ProjectModel::open(mixed.snapshot, gate));
  (void)take(model.apply({project::InsertVst3Plugin{id<project::TrackId>(3), 0, effect}}));
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", false));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save, {})).status,
             persistence::SaveStatus::committed, "instrument/bus project save failed");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  mixed.snapshot = take(reader.load_current()).snapshot;
  mixed.store = std::move(reader);
  REQUIRE_EQ(mixed.snapshot.tracks[1].effects.size(), 1, "instrument/bus effect changed on reopen");
  REQUIRE_EQ(
      std::get<project::Vst3PluginInstance>(mixed.snapshot.tracks[1].effects.front().processor),
      effect, "instrument/bus effect changed on reopen");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "instrument/bus tail fixture failed");
  const auto master_path = directory.path / "instrument-bus-master.wav";
  auto lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                           exporting::MasterRenderOptions{master_path}));
  const auto instrument_path = directory.path / "instrument-bus-instrument.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(2), {instrument_path}));
  const auto audio_path = directory.path / "instrument-bus-audio.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  (void)take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         id<project::TrackId>(9), {audio_path}));
  const auto master = read_file(master_path);
  const auto instrument = read_file(instrument_path);
  const auto audio = read_file(audio_path);
  REQUIRE_EQ(master.size(), instrument.size(),
             "instrument/bus stems lost common latency or tail extent");
  REQUIRE_EQ(instrument.size(), audio.size(),
             "instrument/bus stems lost common latency or tail extent");
  for (std::size_t frame = 0; frame < (master.size() - 56) / 8; ++frame)
    near(sample(master, frame), sample(instrument, frame) + sample(audio, frame));
  near(sample(master, 5), 1.0F);
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "instrument/bus tail fixture cleanup failed");
}

void mixed_render_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "instrument fixture scan failed");
  auto mixed = make_project(directory, gate, scan);
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set finite tail fixture");
  const auto master_path = directory.path / "mixed-master.wav";
  auto lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                         exporting::MasterRenderOptions{master_path})),
      exporting::PublishOutcome::durable, "mixed master render failed");
  const auto master = read_file(master_path);
  // The audio clip ends at 268; the shared render pads the latest content
  // with the longest path's tail, the instrument's eight frames.
  REQUIRE_EQ(master.size(), 56 + 276 * 8, "mixed master duration omitted audio or tail");
  const auto instrument_path = directory.path / "instrument-stem.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease),
                                              scanner, id<project::TrackId>(2), {instrument_path})),
             exporting::PublishOutcome::durable, "instrument processed stem failed");
  const auto instrument = read_file(instrument_path);
  const auto audio_path = directory.path / "audio-stem.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease),
                                              scanner, id<project::TrackId>(9), {audio_path})),
             exporting::PublishOutcome::durable, "audio processed stem failed");
  const auto audio = read_file(audio_path);
  REQUIRE_EQ(instrument.size(), master.size(), "mixed stems are not time-aligned");
  REQUIRE_EQ(audio.size(), master.size(), "mixed stems are not time-aligned");
  near(sample(master, 4), 0.0F);
  near(sample(master, 5), 0.75F);
  near(sample(instrument, 260), 0.375F);
  near(sample(instrument, 263), 0.09375F);
  near(sample(instrument, 264), 0.0F);
  near(sample(audio, 259), 0.0F);
  near(sample(audio, 260), 0.1875F);
  near(sample(audio, 260, 1), 0.375F);
  near(sample(audio, 267), 0.1875F);
  near(sample(master, 260), 0.5625F);
  near(sample(master, 260, 1), 0.0F);
  for (std::size_t frame = 0; frame < 276; ++frame) {
    near(sample(master, frame), sample(instrument, frame) + sample(audio, frame));
    near(sample(master, frame, 1), sample(instrument, frame, 1) + sample(audio, frame, 1));
  }
  auto musical = mixed.snapshot;
  musical.tracks.back().clips.front().placement =
      project::MusicalInterval{{10'400'000}, {10'720'000}};
  const auto musical_path = directory.path / "musical-placement.wav";
  lease = take(exporting::StoppedExportAuthority::acquire(musical, gate));
  REQUIRE_EQ(take(exporting::render_vst3_master(musical, mixed.store, std::move(lease), scanner,
                                                exporting::MasterRenderOptions{musical_path})),
             exporting::PublishOutcome::durable,
             "musical and elapsed audio placement disagree at shared tempo");
  REQUIRE_EQ(read_file(musical_path), master,
             "musical and elapsed audio placement disagree at shared tempo");
  const auto rejected = directory.path / "rejected.wav";
  auto reject = [&](const project::Snapshot& candidate, project::TrackId stem) {
    auto stopped = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    REQUIRE_TRUE(!exporting::render_vst3_stem(candidate, mixed.store, std::move(stopped), scanner,
                                              stem, {rejected}),
                 "unsupported mixed stem published output");
    REQUIRE_TRUE(!fs::exists(rejected), "unsupported mixed stem published output");
  };
  auto wrong = mixed.snapshot;
  wrong.sources.front().sample_rate = oscillate::test::sample_rate_44k;
  reject(wrong, id<project::TrackId>(9));
  wrong = mixed.snapshot;
  wrong.tracks.back().clips.front().source_range.end.value = 9;
  {
    // A stretched clip now renders through the shared warp path.
    const auto stretched = directory.path / "stretched-stem.wav";
    auto lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    REQUIRE_EQ(take(exporting::render_vst3_stem(wrong, mixed.store, std::move(lease), scanner,
                                                id<project::TrackId>(9), {stretched})),
               exporting::PublishOutcome::durable,
               "stretched clip did not render through the mixed export warp path");
    // The processed audio stem follows the direct Q0 stream of its clip.
    const auto stem = read_file(stretched);
    const auto expected = warp_oracle::render(directory.path / "project", wrong,
                                              wrong.tracks.back().clips.front().id, 64);
    // The audio track's stem is its dry clip plus its 0.5 send to the
    // return bus: 1.5 times the processed clip.
    for (std::size_t frame = 0; frame < expected.left.size(); ++frame) {
      REQUIRE_LT(std::fabs(sample(stem, expected.first + frame) - 1.5F * expected.left[frame]),
                 tolerance::float_round_trip_f,
                 "mixed export processed stem differs from the routed direct stream");
      REQUIRE_LT(std::fabs(sample(stem, expected.first + frame, 1) - 1.5F * expected.right[frame]),
                 tolerance::float_round_trip_f,
                 "mixed export processed stem differs from the routed direct stream");
    }
  }
  wrong = mixed.snapshot;
  wrong.tracks.back().clips.front().placement = project::ElapsedInterval{{268}, {260}};
  reject(wrong, id<project::TrackId>(9));
  wrong = mixed.snapshot;
  wrong.sources.push_back(wrong.sources.front());
  reject(wrong, id<project::TrackId>(9));
  wrong = mixed.snapshot;
  auto bus_effect = test_insert::vst3_source(wrong.tracks.front());
  bus_effect.id = id<project::PluginInstanceId>(22);
  bus_effect.missing = false;
  test_insert::append_effect(wrong, wrong.tracks[1], bus_effect);
  auto bus_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  auto bus_result = exporting::render_vst3_master(wrong, mixed.store, std::move(bus_lease), scanner,
                                                  exporting::MasterRenderOptions{rejected});
  REQUIRE_TRUE(!bus_result,
               "instrument-class bus effect was accepted as a verified audio processor");
  REQUIRE_TRUE(!fs::exists(rejected),
               "instrument-class bus effect was accepted as a verified audio processor");
  // An empty audio track beside them is an ordinary project.
  wrong = mixed.snapshot;
  wrong.tracks.emplace_back(id<project::TrackId>(20));
  auto extra_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  REQUIRE_EQ(take(exporting::render_vst3_stem(wrong, mixed.store, std::move(extra_lease), scanner,
                                              id<project::TrackId>(9), {rejected})),
             exporting::PublishOutcome::durable, "an empty audio track stopped the stem");
  REQUIRE_EQ(read_file(rejected), audio, "an empty audio track changed the audio stem");
  fs::remove(rejected);
  wrong = mixed.snapshot;
  wrong.sources.front().sha256[0] = '0';
  reject(wrong, id<project::TrackId>(9));
  reject(mixed.snapshot, id<project::TrackId>(3));
  reject(mixed.snapshot, id<project::TrackId>(99));
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable instrument fault fixture");
  auto stopped = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(stopped),
                                              scanner, exporting::MasterRenderOptions{rejected}),
               "instrument fault published partial mixed master");
  REQUIRE_TRUE(!fs::exists(rejected), "instrument fault published partial mixed master");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
             "cannot clear instrument fault fixture");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0, "cannot clear tail fixture");
  REQUIRE_EQ(test_insert::vst3_source(mixed.snapshot.tracks.front()).binary_sha256,
             scan.binary.sha256, "mixed export changed saved plugin identity");
}

void mixed_clip_automation_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "automation instrument fixture scan failed");
  auto mixed = make_project(directory, gate, scan);
  auto& snapshot = mixed.snapshot;
  ++snapshot.revision;
  snapshot.tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::step}, {{10'560'000}, 60.0, timeline::TempoShape::step}},
      2));
  const auto at = [&snapshot](std::int64_t tick) {
    return take(snapshot.tempo.tick_to_sample({tick}, snapshot.sample_rate)).value;
  };
  REQUIRE_EQ(at(10'400'000), 260, "tempo fixture did not place its clip boundaries exactly");
  REQUIRE_EQ(at(10'560'000), 264, "tempo fixture did not place its clip boundaries exactly");
  REQUIRE_EQ(at(10'720'000), 272, "tempo fixture did not place its clip boundaries exactly");
  REQUIRE_EQ(at(10'880'000), 280, "tempo fixture did not place its clip boundaries exactly");
  auto& track = snapshot.tracks.back();
  const auto audio_track = track.id;
  const project::TrackControlTarget target{track.id,
                                           project::TrackControlTarget::Key::channel_level};
  const project::AutomationDomain domain{project::AutomationRepresentation::physical,
                                         "mixer.channel_level",
                                         "linear_gain",
                                         0.0,
                                         4.0,
                                         1.0,
                                         false,
                                         0,
                                         project::AutomationScale::decibel};
  track.automation.push_back({id<project::AutomationLaneId>(50),
                              target,
                              domain,
                              project::AutomationTimebase::elapsed,
                              {{id<project::AutomationPointId>(51), 0, 0.25},
                               {id<project::AutomationPointId>(52), 265, 0.5}},
                              1});
  auto& first = track.clips.front();
  const auto audio_source = first.source;
  first.source_range = {{2}, {6}};
  first.placement = project::MusicalInterval{{10'400'000}, {10'560'000}};
  first.automation.push_back({id<project::AutomationLaneId>(53),
                              target,
                              domain,
                              project::AutomationTimebase::musical,
                              {{id<project::AutomationPointId>(54), 0, 0.75}},
                              1});
  project::Clip second{id<project::ClipId>(55),
                       audio_source,
                       {{6}, {10}},
                       project::MusicalInterval{{10'720'000}, {10'800'000}}};
  second.automation.push_back({id<project::AutomationLaneId>(56),
                               target,
                               domain,
                               project::AutomationTimebase::elapsed,
                               {{id<project::AutomationPointId>(57), 0, 1.0}},
                               1});
  track.clips.push_back(std::move(second));
  track.clips.emplace_back(id<project::ClipId>(58), audio_source, project::SourceRange{{2}, {4}},
                           project::ElapsedInterval{{276}, {278}});
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", false));
  REQUIRE_EQ(take(writer.save(snapshot, persistence::SaveKind::explicit_save, {})).status,
             persistence::SaveStatus::committed, "mixed automation fixture save failed");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  auto reopened = take(reader.load_current());
  snapshot = std::move(reopened.snapshot);
  REQUIRE_EQ(snapshot.tracks.back().automation.size(), 1,
             "mixed automation did not survive reopen");
  REQUIRE_EQ(snapshot.tracks.back().clips[0].automation.size(), 1,
             "mixed automation did not survive reopen");
  REQUIRE_EQ(snapshot.tracks.back().clips[1].automation.size(), 1,
             "mixed automation did not survive reopen");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set automation tail fixture");
  const auto render = [&](std::optional<project::TrackId> stem, const fs::path& output) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    const auto result =
        stem ? exporting::render_vst3_stem(snapshot, reader, std::move(lease), scanner, *stem,
                                           {output})
             : exporting::render_vst3_master(snapshot, reader, std::move(lease), scanner,
                                             exporting::MasterRenderOptions{output});
    REQUIRE_EQ(take(std::move(result)), exporting::PublishOutcome::durable,
               "mixed automation render failed");
    return read_file(output);
  };
  const auto audio = render(audio_track, directory.path / "automation-audio.wav");
  const auto instrument =
      render(id<project::TrackId>(2), directory.path / "automation-instrument.wav");
  const auto master = render(std::nullopt, directory.path / "automation-master.wav");
  REQUIRE_EQ(audio.size(), 56 + 286 * 8, "mixed automation master/stem extents differ");
  REQUIRE_EQ(instrument.size(), audio.size(), "mixed automation master/stem extents differ");
  REQUIRE_EQ(master.size(), audio.size(), "mixed automation master/stem extents differ");
  for (std::size_t frame = 0; frame < 278; ++frame) {
    // PCM 4096/8192 is 1/8 and 1/4; the saved send adds half of the
    // post-level signal. These expected gains are independent of the curve
    // evaluator, and expose both the tempo boundary and clip release.
    const float gain = frame >= 260 && frame < 264   ? 0.75F
                       : frame >= 272 && frame < 276 ? 1.0F
                       : frame >= 276 && frame < 278 ? 0.5F
                                                     : 0.0F;
    near(sample(audio, frame), 0.1875F * gain);
    near(sample(audio, frame, 1), 0.375F * gain);
    near(sample(master, frame), sample(instrument, frame) + sample(audio, frame));
    near(sample(master, frame, 1), sample(instrument, frame, 1) + sample(audio, frame, 1));
  }
  REQUIRE_EQ(render(std::nullopt, directory.path / "automation-repeat.wav"), master,
             "mixed automation repeat render changed bytes");
  auto unsupported = snapshot;
  auto& lane = unsupported.tracks.back().clips.front().automation.front();
  // A clip lane naming another track's channel cannot be prepared.
  lane.target = project::TrackControlTarget{unsupported.tracks.front().id,
                                            project::TrackControlTarget::Key::channel_level};
  const auto preserved = directory.path / "automation-preserved.wav";
  {
    std::ofstream file(preserved, std::ios::binary);
    file.write(reinterpret_cast<const char*>(master.data()),
               static_cast<std::streamsize>(master.size()));
    require(file.good(), "cannot seed automation output preservation fixture");
  }
  auto lease = take(exporting::StoppedExportAuthority::acquire(unsupported, gate));
  REQUIRE_TRUE(!exporting::render_vst3_master(unsupported, reader, std::move(lease), scanner,
                                              exporting::MasterRenderOptions{preserved}),
               "unsupported clip target replaced a valid mixed render");
  REQUIRE_EQ(read_file(preserved), master, "unsupported clip target replaced a valid mixed render");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear automation tail fixture");
}

void multi_audio_render_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "multi-audio instrument fixture scan failed");
  auto mixed = make_multi_project(directory, gate, scan);
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set multi-audio tail fixture");
  auto render = [&](std::optional<project::TrackId> stem, const fs::path& target) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
    auto outcome =
        stem ? exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                           *stem, {target})
             : exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                             exporting::MasterRenderOptions{target});
    REQUIRE_EQ(take(std::move(outcome)), exporting::PublishOutcome::durable,
               "multi-audio render did not publish");
    return read_file(target);
  };
  const auto master = render(std::nullopt, directory.path / "multi-master.wav");
  const auto instrument = render(id<project::TrackId>(2), directory.path / "multi-instrument.wav");
  const auto first = render(id<project::TrackId>(9), directory.path / "multi-first.wav");
  const auto second = render(id<project::TrackId>(12), directory.path / "multi-second.wav");
  REQUIRE_EQ(master.size(), 56 + 282 * 8,
             "multi-audio master and stems do not share the full duration");
  REQUIRE_EQ(instrument.size(), master.size(),
             "multi-audio master and stems do not share the full duration");
  REQUIRE_EQ(first.size(), master.size(),
             "multi-audio master and stems do not share the full duration");
  REQUIRE_EQ(second.size(), master.size(),
             "multi-audio master and stems do not share the full duration");
  for (std::size_t frame = 0; frame < 274; ++frame) {
    const float instrument_left = frame < 5     ? 0.0F
                                  : frame < 256 ? 0.75F
                                  : frame < 264 ? 0.75F * static_cast<float>(264 - frame) / 8.0F
                                                : 0.0F;
    const bool first_active = (frame >= 126 && frame < 130) || (frame >= 260 && frame < 268) ||
                              (frame >= 270 && frame < 274);
    const float first_left = first_active ? 0.1875F : 0.0F;
    const float first_right = first_active ? 0.375F : 0.0F;
    const float second_left = frame >= 127 && frame < 131   ? 0.09375F
                              : frame >= 140 && frame < 142 ? 0.75F
                                                            : 0.0F;
    const float second_right = frame >= 127 && frame < 131   ? 0.1875F
                               : frame >= 140 && frame < 142 ? 1.125F
                                                             : 0.0F;
    near(sample(instrument, frame), instrument_left);
    near(sample(instrument, frame, 1), -instrument_left);
    near(sample(first, frame), first_left);
    near(sample(first, frame, 1), first_right);
    near(sample(second, frame), second_left);
    near(sample(second, frame, 1), second_right);
    near(sample(master, frame), instrument_left + first_left + second_left);
    near(sample(master, frame, 1), -instrument_left + first_right + second_right);
  }
  REQUIRE_EQ(render(std::nullopt, directory.path / "multi-repeat.wav"), master,
             "multi-audio repeat render changed bytes");

  const auto target = directory.path / "preserved.wav";
  const std::vector<std::uint8_t> sentinel{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'};
  {
    std::ofstream file(target, std::ios::binary);
    file.write(reinterpret_cast<const char*>(sentinel.data()),
               static_cast<std::streamsize>(sentinel.size()));
    require(file.good(), "cannot seed preserved output");
  }
  auto reject = [&](const project::Snapshot& candidate) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    REQUIRE_TRUE(!exporting::render_vst3_master(candidate, mixed.store, std::move(lease), scanner,
                                                exporting::MasterRenderOptions{target}),
                 "rejected multi-audio render replaced existing output");
    REQUIRE_EQ(read_file(target), sentinel, "rejected multi-audio render replaced existing output");
  };
  auto wrong = mixed.snapshot;
  wrong.sources.back().sha256[0] = wrong.sources.back().sha256[0] == '0' ? '1' : '0';
  reject(wrong);
  wrong = mixed.snapshot;
  wrong.tracks.back().clips.back().placement = project::ElapsedInterval{{129}, {131}};
  reject(wrong);
  wrong = mixed.snapshot;
  wrong.tracks[2].clips.back().source_range.end.value = 9;
  {
    // A stretched clip renders through the shared warp path instead.
    auto lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    REQUIRE_EQ(take(exporting::render_vst3_master(
                   wrong, mixed.store, std::move(lease), scanner,
                   exporting::MasterRenderOptions{directory.path / "multi-stretched.wav"})),
               exporting::PublishOutcome::durable,
               "stretched multi-audio clip did not render through the warp path");
    REQUIRE_EQ(read_file(target), sentinel,
               "stretched multi-audio clip did not render through the warp path");
  }
  // More tracks than the former exporter's limit are an ordinary project.
  wrong = mixed.snapshot;
  for (std::uint64_t index = 0; index < 7; ++index)
    wrong.tracks.emplace_back(id<project::TrackId>(20 + index));
  {
    auto lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    REQUIRE_EQ(take(exporting::render_vst3_master(
                   wrong, mixed.store, std::move(lease), scanner,
                   exporting::MasterRenderOptions{directory.path / "multi-many.wav"})),
               exporting::PublishOutcome::durable, "more empty audio tracks stopped the export");
    REQUIRE_EQ(read_file(directory.path / "multi-many.wav"), master,
               "empty audio tracks changed the master");
  }
  const auto asset =
      directory.path / "project/assets" / (mixed.snapshot.sources.back().sha256 + ".wav");
  require(fs::is_regular_file(asset), "second managed source asset is missing");
  const auto saved_asset = read_file(asset);
  auto damaged = saved_asset;
  damaged.back() ^= 1;
  {
    std::ofstream file(asset, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(damaged.data()),
               static_cast<std::streamsize>(damaged.size()));
    require(file.good(), "cannot damage managed fixture");
  }
  reject(mixed.snapshot);
  {
    std::ofstream file(asset, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(saved_asset.data()),
               static_cast<std::streamsize>(saved_asset.size()));
    require(file.good(), "cannot restore managed fixture");
  }
  const auto missing = asset.string() + ".missing";
  fs::rename(asset, missing);
  reject(mixed.snapshot);
  fs::rename(missing, asset);
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable multi-audio plugin fault");
  reject(mixed.snapshot);
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
             "cannot clear multi-audio plugin fault");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear multi-audio tail fixture");
}

void source_rate_render_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "source-rate instrument fixture scan failed");
  auto mixed = make_project(directory, gate, scan);
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", false));
  const auto input = directory.path / "44100.wav";
  const auto wav = sine_source_wav();
  {
    std::ofstream file(input, std::ios::binary);
    file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    require(file.good(), "source-rate input WAV write failed");
  }
  const int descriptor = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "source-rate input WAV open failed");
  auto staged = writer.stage_wav(id<project::SourceId>(20), descriptor);
  REQUIRE_EQ(::close(descriptor), 0, "source-rate input WAV close failed");
  auto receipt = take(std::move(staged));
  auto model = take(project::ProjectModel::open(mixed.snapshot, gate));
  const auto converted_track = id<project::TrackId>(21);
  const project::Clip first{id<project::ClipId>(22),
                            receipt.record().id,
                            {{441}, {1'323}},
                            project::ElapsedInterval{{127}, {1'087}}};
  const project::Clip second{id<project::ClipId>(23),
                             receipt.record().id,
                             {{2'205}, {2'646}},
                             project::ElapsedInterval{{1'400}, {1'880}}};
  (void)take(
      model.apply({project::AddTrack{converted_track, project::TrackKind::audio,
                                     model.snapshot()->tracks.size()},
                   project::AddSource{receipt.record()}, project::AddClip{converted_track, first},
                   project::AddClip{converted_track, second},
                   project::AddSendRoute{
                       converted_track, {id<project::SendId>(24), id<project::TrackId>(3), 0.5}}}));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save,
                              std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "source-rate project save failed");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  mixed.snapshot = take(reader.load_current()).snapshot;
  mixed.store = std::move(reader);
  REQUIRE_EQ(mixed.snapshot.sources.back().sample_rate, 44'100,
             "source-rate source coordinates or clips changed on reopen");
  REQUIRE_EQ(mixed.snapshot.tracks.back().clips.front(), first,
             "source-rate source coordinates or clips changed on reopen");
  REQUIRE_EQ(mixed.snapshot.tracks.back().clips.back(), second,
             "source-rate source coordinates or clips changed on reopen");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set source-rate instrument tail");
  auto render = [&](std::optional<project::TrackId> stem, const fs::path& target) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(mixed.snapshot, gate));
    auto outcome =
        stem ? exporting::render_vst3_stem(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                           *stem, {target})
             : exporting::render_vst3_master(mixed.snapshot, mixed.store, std::move(lease), scanner,
                                             exporting::MasterRenderOptions{target});
    REQUIRE_EQ(take(std::move(outcome)), exporting::PublishOutcome::durable,
               "source-rate mixed render did not publish");
    return read_file(target);
  };
  const auto target = directory.path / "preserved-rate.wav";
  const std::vector<std::uint8_t> sentinel{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'};
  {
    std::ofstream file(target, std::ios::binary);
    file.write(reinterpret_cast<const char*>(sentinel.data()),
               static_cast<std::streamsize>(sentinel.size()));
    require(file.good(), "cannot seed source-rate output");
  }
  auto reject = [&](const project::Snapshot& candidate) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    REQUIRE_TRUE(!exporting::render_vst3_master(candidate, mixed.store, std::move(lease), scanner,
                                                exporting::MasterRenderOptions{target}),
                 "rejected source-rate render replaced existing output");
    REQUIRE_EQ(read_file(target), sentinel, "rejected source-rate render replaced existing output");
  };
  if (!media::source_rate_supported(44'100, 48'000)) {
    reject(mixed.snapshot);
    REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
               "cannot clear source-rate instrument tail");
    return;
  }
  const auto master = render(std::nullopt, directory.path / "rate-master.wav");
  const auto converted = render(converted_track, directory.path / "rate-stem.wav");
  const auto same_rate = render(id<project::TrackId>(9), directory.path / "same-rate-stem.wav");
  const auto instrument = render(id<project::TrackId>(2), directory.path / "rate-instrument.wav");
  REQUIRE_EQ(master.size(), 56 + 1'888 * 8,
             "source-rate master/stem duration differs from rounded trim endpoints");
  REQUIRE_EQ(converted.size(), master.size(),
             "source-rate master/stem duration differs from rounded trim endpoints");
  REQUIRE_EQ(same_rate.size(), master.size(),
             "source-rate master/stem duration differs from rounded trim endpoints");
  REQUIRE_EQ(instrument.size(), master.size(),
             "source-rate master/stem duration differs from rounded trim endpoints");
  for (std::size_t frame = 0; frame < 1'880; ++frame) {
    near(sample(master, frame),
         sample(converted, frame) + sample(same_rate, frame) + sample(instrument, frame));
    near(sample(master, frame, 1),
         sample(converted, frame, 1) + sample(same_rate, frame, 1) + sample(instrument, frame, 1));
  }
  near(sample(converted, 126), 0.0F);
  near(sample(converted, 1'087), 0.0F);
  near(sample(converted, 1'399), 0.0F);
  for (const auto [placement, source_first, duration] :
       std::array<std::array<std::size_t, 3>, 2>{{{127, 480, 960}, {1'400, 2'400, 480}}}) {
    for (std::size_t offset = 100; offset + 100 < duration; offset += 137) {
      const double phase =
          2.0 * std::numbers::pi * 440.0 * static_cast<double>(source_first + offset) / 48'000.0;
      const float expected = static_cast<float>(0.1875 * std::sin(phase));
      REQUIRE_LT(std::fabs(sample(converted, placement + offset) - expected), 0.003F,
                 "resampled source pitch or trimmed timing differs from independent sine oracle");
      REQUIRE_LT(std::fabs(sample(converted, placement + offset, 1) + expected * 0.5F), 0.003F,
                 "resampled source pitch or trimmed timing differs from independent sine oracle");
    }
  }
  REQUIRE_EQ(render(std::nullopt, directory.path / "rate-repeat.wav"), master,
             "source-rate master changed between repeated renders");
  auto wrong = mixed.snapshot;
  wrong.tracks.back().clips.front().placement = project::ElapsedInterval{{127}, {1'086}};
  {
    // A converted-rate clip stretched by one frame renders through the warp path.
    auto lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    REQUIRE_EQ(take(exporting::render_vst3_master(
                   wrong, mixed.store, std::move(lease), scanner,
                   exporting::MasterRenderOptions{directory.path / "rate-stretched.wav"})),
               exporting::PublishOutcome::durable,
               "stretched converted-rate clip did not render through the warp path");
    REQUIRE_EQ(read_file(target), sentinel,
               "stretched converted-rate clip did not render through the warp path");
  }
  wrong = mixed.snapshot;
  wrong.tracks.back().clips.front().source_range = {{441}, {442}};
  {
    // One source frame across the whole placement is an extreme but valid
    // stretch, rendered by the warp path rather than refused.
    auto lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    REQUIRE_EQ(take(exporting::render_vst3_master(
                   wrong, mixed.store, std::move(lease), scanner,
                   exporting::MasterRenderOptions{directory.path / "rate-extreme.wav"})),
               exporting::PublishOutcome::durable,
               "extreme stretch did not render through the warp path");
    REQUIRE_EQ(read_file(target), sentinel, "extreme stretch did not render through the warp path");
  }
  wrong = mixed.snapshot;
  wrong.sources.back().sample_rate = 384'000;
  wrong.tracks.back().clips.front().source_range = {{0}, {1}};
  reject(wrong);
  wrong = mixed.snapshot;
  wrong.sources.back().sha256[0] = wrong.sources.back().sha256[0] == '0' ? '1' : '0';
  reject(wrong);
  const auto asset =
      directory.path / "project/assets" / (mixed.snapshot.sources.back().sha256 + ".wav");
  const auto saved_asset = read_file(asset);
  auto damaged = saved_asset;
  damaged.back() ^= 1;
  {
    std::ofstream file(asset, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(damaged.data()),
               static_cast<std::streamsize>(damaged.size()));
    require(file.good(), "cannot damage source-rate managed media");
  }
  reject(mixed.snapshot);
  {
    std::ofstream file(asset, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(saved_asset.data()),
               static_cast<std::streamsize>(saved_asset.size()));
    require(file.good(), "cannot restore source-rate managed media");
  }
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear source-rate instrument tail");
}

void native_master_reverb_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "master reverb fixture scan failed");
  auto mixed = make_project(directory, gate, scan);
  auto& impulse = mixed.snapshot.tracks.back().clips.front();
  impulse.source_range.end.value = impulse.source_range.start.value + 1;
  impulse.placement = project::ElapsedInterval{{270}, {271}};
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set master reverb instrument tail");
  auto render = [&](const project::Snapshot& candidate, std::optional<project::TrackId> stem,
                    const fs::path& target) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    auto outcome =
        stem ? exporting::render_vst3_stem(candidate, mixed.store, std::move(lease), scanner, *stem,
                                           {target})
             : exporting::render_vst3_master(candidate, mixed.store, std::move(lease), scanner,
                                             exporting::MasterRenderOptions{target});
    REQUIRE_EQ(take(std::move(outcome)), exporting::PublishOutcome::durable,
               "master reverb fixture render did not publish");
    return read_file(target);
  };
  const auto dry = render(mixed.snapshot, std::nullopt, directory.path / "dry.wav");
  auto wet = mixed.snapshot;
  const auto reverb = test_insert::expanse(0.2, 1.0);
  test_insert::set_master(wet, reverb);
  const auto reverb_tail = static_cast<std::size_t>(test_insert::tail_frames(reverb, 48'000));
  constexpr std::size_t base_end = 279;
  const auto master = render(wet, std::nullopt, directory.path / "wet-master.wav");
  const auto audio = render(wet, id<project::TrackId>(9), directory.path / "wet-audio.wav");
  const auto instrument =
      render(wet, id<project::TrackId>(2), directory.path / "wet-instrument.wav");
  REQUIRE_EQ(master.size(), 56 + (base_end + reverb_tail) * 8,
             "native master reverb tail or common stem extent is wrong");
  REQUIRE_EQ(audio.size(), master.size(),
             "native master reverb tail or common stem extent is wrong");
  REQUIRE_EQ(instrument.size(), master.size(),
             "native master reverb tail or common stem extent is wrong");
  // The reverb is linear, so the master is the sum of its processed stems.
  for (std::size_t frame = 0; frame < base_end + reverb_tail; ++frame) {
    near(sample(master, frame), sample(audio, frame) + sample(instrument, frame));
    near(sample(master, frame, 1), sample(audio, frame, 1) + sample(instrument, frame, 1));
  }
  REQUIRE_EQ(render(wet, std::nullopt, directory.path / "wet-repeat.wav"), master,
             "native master reverb repeat changed bytes");
  const auto target = directory.path / "preserved.wav";
  const std::vector<std::uint8_t> sentinel{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'};
  {
    std::ofstream file(target, std::ios::binary);
    file.write(reinterpret_cast<const char*>(sentinel.data()),
               static_cast<std::streamsize>(sentinel.size()));
    require(file.good(), "cannot seed master reverb preserved output");
  }
  auto reject = [&](const project::Snapshot& candidate) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    REQUIRE_TRUE(!exporting::render_vst3_master(candidate, mixed.store, std::move(lease), scanner,
                                                exporting::MasterRenderOptions{target}),
                 "rejected native master reverb render replaced existing output");
    REQUIRE_EQ(read_file(target), sentinel,
               "rejected native master reverb render replaced existing output");
  };
  auto invalid = wet;
  test_insert::set_master(invalid, test_insert::with_raw_value(reverb, "expanse.mix", 2.0));
  reject(invalid);
  // A latent master limiter beside the reverb renders (ADR-0050).
  invalid = wet;
  test_insert::set_master(invalid, test_insert::ceiling({-6.0, 5.0, 70.0}));
  REQUIRE_GE(render(invalid, std::nullopt, directory.path / "reverb-limited.wav").size(),
             master.size(), "a master limiter cut the reverb's tail");
  invalid = wet;
  invalid.master_effects.front().enabled = false;
  REQUIRE_EQ(render(invalid, std::nullopt, directory.path / "bypassed-reverb.wav"), dry,
             "bypassed master reverb changed samples or retained its tail");
  invalid = wet;
  test_insert::set_master(
      invalid, test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}));
  std::ranges::reverse(invalid.master_effects);
  const auto reversed = render(invalid, std::nullopt, directory.path / "reverb-before-eq.wav");
  REQUIRE_GT(reversed.size(), master.size(),
             "reordered master EQ/reverb lost the conservative EQ drain");
  REQUIRE_NE(reversed, master, "reordered master EQ/reverb lost the conservative EQ drain");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable native master reverb plugin fault");
  reject(wet);
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
             "cannot clear native master reverb plugin fault");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear master reverb instrument tail");
}

void native_master_eq_compressor_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "master EQ/compressor fixture scan failed");
  auto mixed = make_project(directory, gate, scan);
  auto& impulse = mixed.snapshot.tracks.back().clips.front();
  impulse.source_range.end.value = impulse.source_range.start.value + 1;
  impulse.placement = project::ElapsedInterval{{270}, {271}};
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot set master EQ/compressor instrument tail");
  auto render = [&](const project::Snapshot& candidate, std::optional<project::TrackId> stem,
                    const fs::path& target) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    auto result =
        stem ? exporting::render_vst3_stem(candidate, mixed.store, std::move(lease), scanner, *stem,
                                           {target})
             : exporting::render_vst3_master(candidate, mixed.store, std::move(lease), scanner,
                                             exporting::MasterRenderOptions{target});
    REQUIRE_EQ(take(std::move(result)), exporting::PublishOutcome::durable,
               "master EQ/compressor render did not publish");
    return read_file(target);
  };
  auto eq = mixed.snapshot;
  test_insert::set_master(eq, test_insert::shape({native_dsp::EqBandKind::low_pass, 12'000.0,
                                                  std::numbers::sqrt2 / 2.0, 0.0}));
  const auto eq_audio = render(eq, id<project::TrackId>(9), directory.path / "eq-audio.wav");
  const float b0 = static_cast<float>(1.0 - std::numbers::sqrt2 / 2.0);
  REQUIRE_EQ(eq_audio.size(), 56 + (279 + 304) * 8,
             "master EQ conservative tail or common extent is wrong");
  near(sample(eq_audio, 269), 0.0F);
  near(sample(eq_audio, 270), 0.1875F * b0);
  near(sample(eq_audio, 270, 1), 0.375F * b0);
  near(sample(eq_audio, 271), 0.1875F * 2.0F * b0);
  REQUIRE_EQ(render(eq, id<project::TrackId>(9), directory.path / "eq-repeat.wav"), eq_audio,
             "master EQ repeat render changed bytes");

  auto compressor_only = mixed.snapshot;
  const auto compressor = test_insert::press({-60.0, 8.0, 0.0, 0.1, 100.0, 0.0});
  test_insert::set_master(compressor_only, compressor);
  const auto compressed_audio =
      render(compressor_only, id<project::TrackId>(9), directory.path / "compressor-audio.wav");
  const double compressor_envelope = 0.375 * (1.0 - std::exp(-1.0 / 4.8));
  const double compressor_reduction =
      (20.0 * std::log10(compressor_envelope) + 60.0) * (1.0 - 1.0 / 8.0);
  REQUIRE_EQ(compressed_audio.size(), 56 + 279 * 8,
             "zero-latency compressor added a silent export tail");
  near(sample(compressed_audio, 270),
       static_cast<float>(0.1875 * std::pow(10.0, -compressor_reduction / 20.0)));

  auto compressor_then_eq = eq;
  test_insert::set_master(compressor_then_eq, compressor);
  std::ranges::reverse(compressor_then_eq.master_effects);
  const auto compressed_then_filtered =
      render(compressor_then_eq, id<project::TrackId>(9), directory.path / "compressor-eq.wav");
  REQUIRE_EQ(compressed_then_filtered.size(), eq_audio.size(),
             "reordered master chain lost the EQ drain");
  // The compressor sees the one-frame impulse first. The EQ's first two
  // coefficients then filter that independently measured compressor output.
  near(sample(compressed_then_filtered, 270), sample(compressed_audio, 270) * b0);
  near(sample(compressed_then_filtered, 271), sample(compressed_audio, 270) * 2.0F * b0);
  auto eq_then_compressor = compressor_then_eq;
  std::ranges::reverse(eq_then_compressor.master_effects);
  const auto filtered_then_compressed =
      render(eq_then_compressor, id<project::TrackId>(9), directory.path / "eq-compressor.wav");
  REQUIRE_GT(
      std::fabs(sample(filtered_then_compressed, 270) - sample(compressed_then_filtered, 270)),
      0.0001F, "reordering nonlinear master processors did not change their execution order");
  compressor_then_eq.master_effects.front().enabled = false;
  REQUIRE_EQ(render(compressor_then_eq, id<project::TrackId>(9),
                    directory.path / "bypassed-compressor.wav"),
             eq_audio, "bypassed master compressor changed EQ output");
  compressor_then_eq.master_effects.front().enabled = true;
  compressor_then_eq.master_effects.back().enabled = false;
  REQUIRE_EQ(
      render(compressor_then_eq, id<project::TrackId>(9), directory.path / "bypassed-eq.wav"),
      compressed_audio, "bypassed master EQ changed compressor output or duration");

  auto chain = eq;
  chain.tracks.back().clips.front().placement = project::ElapsedInterval{{128}, {129}};
  test_insert::set_master(chain, compressor);
  const auto master = render(chain, std::nullopt, directory.path / "chain-master.wav");
  const auto audio = render(chain, id<project::TrackId>(9), directory.path / "chain-audio.wav");
  const auto instrument =
      render(chain, id<project::TrackId>(2), directory.path / "chain-instrument.wav");
  REQUIRE_EQ(master.size(), 56 + (264 + 304) * 8,
             "EQ/compressor chain lost the source or the EQ drain");
  REQUIRE_EQ(audio.size(), master.size(), "EQ/compressor chain lost the source or the EQ drain");
  REQUIRE_EQ(instrument.size(), master.size(),
             "EQ/compressor chain lost the source or the EQ drain");
  // First audio-stem frame has no prior filter or detector history. The
  // right channel drives the linked detector.
  const double first_left = 0.1875 * static_cast<double>(b0);
  const double first_right = 0.375 * static_cast<double>(b0);
  const double envelope = first_right * (1.0 - std::exp(-1.0 / 4.8));
  const double reduction_db = (20.0 * std::log10(envelope) + 60.0) * (1.0 - 1.0 / 8.0);
  const float expected = static_cast<float>(first_left * std::pow(10.0, -reduction_db / 20.0));
  near(sample(audio, 127), 0.0F);
  near(sample(audio, 128), expected);
  REQUIRE_GT(std::fabs(sample(master, 128) - (sample(audio, 128) + sample(instrument, 128))),
             0.0001F, "nonlinear compressor was treated as summable processed stems");
  REQUIRE_EQ(render(chain, std::nullopt, directory.path / "chain-repeat.wav"), master,
             "EQ/compressor repeat render changed bytes");

  const auto target = directory.path / "preserved-chain.wav";
  const std::vector<std::uint8_t> sentinel{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'};
  {
    std::ofstream file(target, std::ios::binary);
    file.write(reinterpret_cast<const char*>(sentinel.data()),
               static_cast<std::streamsize>(sentinel.size()));
    require(file.good(), "cannot seed EQ/compressor output");
  }
  auto reject = [&](const project::Snapshot& candidate) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    REQUIRE_TRUE(!exporting::render_vst3_master(candidate, mixed.store, std::move(lease), scanner,
                                                exporting::MasterRenderOptions{target}),
                 "rejected EQ/compressor chain replaced existing output");
    REQUIRE_EQ(read_file(target), sentinel,
               "rejected EQ/compressor chain replaced existing output");
  };
  auto invalid = chain;
  test_insert::set_master(
      invalid,
      test_insert::with_raw_value(test_insert::shape({native_dsp::EqBandKind::low_pass, 12'000.0,
                                                      std::numbers::sqrt2 / 2.0, 0.0}),
                                  "shape.freq_1", 0.0));
  reject(invalid);
  invalid = chain;
  test_insert::set_master(invalid, test_insert::with_raw_value(compressor, "press.attack", 0.0));
  reject(invalid);
  // A latent master limiter after the EQ and compressor renders (ADR-0050).
  invalid = chain;
  test_insert::set_master(invalid, test_insert::ceiling({-6.0, 5.0, 70.0}));
  REQUIRE_GE(render(invalid, std::nullopt, directory.path / "chain-limited.wav").size(),
             master.size(), "a master limiter cut the EQ's drain");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable master EQ/compressor plugin fault");
  reject(chain);
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
             "cannot clear master EQ/compressor plugin fault");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear master EQ/compressor instrument tail");
}

void three_source_native_vst3_audio_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok(), "three-source instrument fixture scan failed");
  auto mixed = make_project(directory, gate, scan);
  auto native = mixed.snapshot.tracks.front();
  native.id = id<project::TrackId>(40);
  native.instrument_source = project::NativeInstrumentPatch{"prism", 1, {}};
  native.midi_clips.front().id = id<project::ClipId>(41);
  native.midi_clips.front().notes.front().id = id<project::NoteId>(42);
  native.sends = {{id<project::SendId>(43), id<project::TrackId>(3), 0.5}};
  native.level = 0.5;
  native.pan = 0.5;
  mixed.snapshot.tracks.push_back(native);
  ++mixed.snapshot.revision;
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", false));
  REQUIRE_EQ(take(writer.save(mixed.snapshot, persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "three-source project did not save");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  mixed.snapshot = take(reader.load_current()).snapshot;
  mixed.store = std::move(reader);
  REQUIRE_EQ(mixed.snapshot.tracks.size(), 4, "three-source project did not reopen intact");
  REQUIRE_EQ(mixed.snapshot.sources.size(), 1, "three-source project did not reopen intact");
  REQUIRE_EQ(
      std::get<project::NativeInstrumentPatch>(mixed.snapshot.tracks.back().instrument_source)
          .engine,
      "prism", "three-source project did not reopen intact");

  auto render = [&](const project::Snapshot& snapshot, std::optional<project::TrackId> stem,
                    const fs::path& target) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    const auto result =
        stem ? exporting::render_vst3_stem(snapshot, mixed.store, std::move(lease), scanner, *stem,
                                           {target})
             : exporting::render_vst3_master(snapshot, mixed.store, std::move(lease), scanner,
                                             exporting::MasterRenderOptions{target});
    REQUIRE_EQ(take(std::move(result)), exporting::PublishOutcome::durable,
               "three-source render did not publish");
    return read_file(target);
  };
  const auto master = render(mixed.snapshot, std::nullopt, directory.path / "three-master.wav");
  const auto audio =
      render(mixed.snapshot, id<project::TrackId>(9), directory.path / "three-audio.wav");
  const auto vst3 =
      render(mixed.snapshot, id<project::TrackId>(2), directory.path / "three-vst3.wav");
  const auto prism = render(mixed.snapshot, native.id, directory.path / "three-prism.wav");
  REQUIRE_EQ(master.size(), audio.size(), "three-source output duration or stem alignment differs");
  REQUIRE_EQ(master.size(), vst3.size(), "three-source output duration or stem alignment differs");
  REQUIRE_EQ(master.size(), prism.size(), "three-source output duration or stem alignment differs");
  REQUIRE_GE(master.size(), 56 + 268 * 8, "three-source output duration or stem alignment differs");
  near(sample(vst3, 5), 0.75F);
  near(sample(audio, 5), 0.0F);
  near(sample(audio, 260), 0.1875F);
  near(sample(audio, 260, 1), 0.375F);
  bool native_audible = false;
  for (std::size_t frame = 0; frame < 268; ++frame)
    native_audible |= std::fabs(sample(prism, frame)) > 0.0001F;
  require(native_audible, "selected native source is silent in three-source stem");
  for (std::size_t frame = 0; frame < (master.size() - 56) / 8; ++frame) {
    near(sample(master, frame), sample(audio, frame) + sample(vst3, frame) + sample(prism, frame));
    near(sample(master, frame, 1),
         sample(audio, frame, 1) + sample(vst3, frame, 1) + sample(prism, frame, 1));
  }
  REQUIRE_EQ(render(mixed.snapshot, std::nullopt, directory.path / "three-repeat.wav"), master,
             "three-source repeated render changed bytes");

  auto wet = mixed.snapshot;
  const auto reverb = test_insert::expanse(0.2, 1.0);
  test_insert::set_master(wet, reverb);
  const auto wet_master = render(wet, std::nullopt, directory.path / "wet-three-master.wav");
  const auto wet_audio =
      render(wet, id<project::TrackId>(9), directory.path / "wet-three-audio.wav");
  const auto wet_vst3 = render(wet, id<project::TrackId>(2), directory.path / "wet-three-vst3.wav");
  const auto wet_prism = render(wet, native.id, directory.path / "wet-three-prism.wav");
  REQUIRE_EQ(wet_master.size(), master.size() + test_insert::tail_frames(reverb, 48'000) * 8,
             "three-source master reverb tail or stem extent differs");
  REQUIRE_EQ(wet_master.size(), wet_audio.size(),
             "three-source master reverb tail or stem extent differs");
  REQUIRE_EQ(wet_master.size(), wet_vst3.size(),
             "three-source master reverb tail or stem extent differs");
  REQUIRE_EQ(wet_master.size(), wet_prism.size(),
             "three-source master reverb tail or stem extent differs");
  for (std::size_t frame = 0; frame < (wet_master.size() - 56) / 8; ++frame) {
    near(sample(wet_master, frame),
         sample(wet_audio, frame) + sample(wet_vst3, frame) + sample(wet_prism, frame));
    near(sample(wet_master, frame, 1),
         sample(wet_audio, frame, 1) + sample(wet_vst3, frame, 1) + sample(wet_prism, frame, 1));
  }

  const auto preserved = directory.path / "three-preserved.wav";
  const std::vector<std::uint8_t> sentinel{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'};
  {
    std::ofstream file(preserved, std::ios::binary);
    file.write(reinterpret_cast<const char*>(sentinel.data()),
               static_cast<std::streamsize>(sentinel.size()));
  }
  auto reject = [&](const project::Snapshot& candidate) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    REQUIRE_TRUE(!exporting::render_vst3_master(candidate, mixed.store, std::move(lease), scanner,
                                                exporting::MasterRenderOptions{preserved}),
                 "rejected three-source render changed target");
    REQUIRE_EQ(read_file(preserved), sentinel, "rejected three-source render changed target");
  };
  auto invalid = wet;
  auto& binary_digit = test_insert::vst3_source(invalid.tracks.front()).binary_sha256[0];
  binary_digit = binary_digit == '0' ? '1' : '0';
  reject(invalid);
  // A reverb on the VST3 source's track is an ordinary chain (ADR-0050).
  invalid = wet;
  invalid.tracks.front().effects.push_back(
      {test_insert::next_slot(invalid), true, test_insert::expanse(0.2, 0.2)});
  REQUIRE_GT(render(invalid, std::nullopt, directory.path / "three-source-reverb.wav").size(),
             wet_master.size(), "the source track's reverb tail was cut");
  invalid = wet;
  invalid.sources.front().sha256 = std::string(64, '0');
  reject(invalid);
  const auto media_path =
      directory.path / "project/assets" / (mixed.snapshot.sources.front().sha256 + ".wav");
  const auto missing_path = directory.path / "missing-managed-source.wav";
  fs::rename(media_path, missing_path);
  reject(wet);
  fs::rename(missing_path, media_path);
  auto stale = take(exporting::StoppedExportAuthority::acquire(wet, gate));
  invalid = wet;
  ++invalid.revision;
  REQUIRE_TRUE(!exporting::render_vst3_master(invalid, mixed.store, std::move(stale), scanner,
                                              exporting::MasterRenderOptions{preserved}),
               "stale three-source authority changed target");
  REQUIRE_EQ(read_file(preserved), sentinel, "stale three-source authority changed target");
}
} // namespace

int main() {
  try {
    simultaneous_instrument_effect_checks();
    instrument_track_effect_survives_reopen_and_export();
    paired_instrument_effect_paths_export();
    instrument_and_bus_effect_share_offline_routing();
    mixed_render_checks();
    mixed_clip_automation_checks();
    multi_audio_render_checks();
    source_rate_render_checks();
    native_master_reverb_checks();
    native_master_eq_compressor_checks();
    three_source_native_vst3_audio_checks();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "VST3 mixed export: %s\n", error.what());
    return 1;
  }
}
