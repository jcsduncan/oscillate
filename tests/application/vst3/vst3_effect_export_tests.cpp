#include "format_oracle.hpp"
#include "insert_fixtures.hpp"
#include "oscillate/application/vst3/offline_export.hpp"
#include "oscillate/application/vst3/project_binding.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

// TailGain's declared tail in its "overlong-tail" mode: eleven seconds at
// 48 kHz.
constexpr std::size_t overlong_tail_frames = std::size_t{48'000} * 11U;

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
      fs::temp_directory_path() / ("oscillate-vst3-effect-export-" + std::to_string(::getpid()));
  Directory() {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~Directory() { fs::remove_all(path); }
};
void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  u16(bytes, static_cast<std::uint16_t>(value));
  u16(bytes, static_cast<std::uint16_t>(value >> 16U));
}
std::vector<std::uint8_t> source_wav() {
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  u32(bytes, 36 + 16 * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(bytes, 16);
  u16(bytes, 1);
  u16(bytes, 2);
  u32(bytes, 48'000);
  u32(bytes, 48'000 * 4);
  u16(bytes, 4);
  u16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  u32(bytes, 16 * 4);
  for (int frame = 0; frame < 16; ++frame) {
    u16(bytes, frame == 2 ? 8'192 : frame == 3 ? 4'096 : 0);
    u16(bytes, frame == 2 ? 4'096 : frame == 3 ? 2'048 : 0);
  }
  return bytes;
}
std::vector<std::uint8_t> read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "effect WAV could not open");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
float sample(const std::vector<std::uint8_t>& wav, std::size_t frame, std::size_t channel = 0) {
  REQUIRE_GE(wav.size(), 56 + (frame + 1) * 8, "effect WAV frame missing");
  float value = 0.0F;
  std::memcpy(&value, wav.data() + 56 + frame * 8 + channel * 4, sizeof(value));
  return value;
}
void near(float value, float expected) {
  if (std::fabs(value - expected) > 0.00001F)
    throw std::runtime_error("effect WAV sample=" + std::to_string(value) +
                             " expected=" + std::to_string(expected));
}

struct Saved {
  project::Snapshot snapshot;
  persistence::ProjectStore store;
};
Saved make_project(Directory& directory, Gate& gate, const plugins::ScanResult& scan,
                   bool serial = false) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  auto model = take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000,
                                                  std::move(tempo), std::move(meter), gate));
  const auto& klass = scan.metadata.classes.front();
  project::Vst3PluginInstance plugin{.id = id<project::PluginInstanceId>(8),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = scan.binary.canonical_bundle.string(),
                                     .binary_sha256 = scan.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  const float gain = serial ? 0.5F : -0.5F;
  plugin.component_state.resize(sizeof(gain));
  std::memcpy(plugin.component_state.data(), &gain, sizeof(gain));
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", true));
  const auto input_path = directory.path / "input.wav";
  const auto wav = source_wav();
  {
    std::ofstream output(input_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(wav.data()),
                 static_cast<std::streamsize>(wav.size()));
    require(output.good(), "effect source fixture write failed");
  }
  const int descriptor = ::open(input_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "effect source fixture open failed");
  auto staged = writer.stage_wav(id<project::SourceId>(3), descriptor);
  REQUIRE_EQ(::close(descriptor), 0, "effect source fixture close failed");
  auto receipt = take(std::move(staged));
  const auto audio = id<project::TrackId>(2);
  const auto bus = id<project::TrackId>(4);
  const auto dry = id<project::TrackId>(9);
  const project::Clip first{id<project::ClipId>(5),
                            receipt.record().id,
                            {{2}, {10}},
                            project::ElapsedInterval{{63}, {71}}};
  const project::Clip second{id<project::ClipId>(6),
                             receipt.record().id,
                             {{2}, {10}},
                             project::ElapsedInterval{{127}, {135}}};
  const project::Clip dry_clip{id<project::ClipId>(10),
                               receipt.record().id,
                               {{2}, {10}},
                               project::ElapsedInterval{{63}, {71}}};
  (void)take(model.apply(
      {project::AddTrack{audio}, project::AddTrack{bus, project::TrackKind::return_bus},
       // Last, after the return bus, as the fixture's rows expect.
       project::AddTrack{dry, project::TrackKind::audio, 2}, project::AddSource{receipt.record()},
       project::AddClip{audio, first}, project::AddClip{audio, second},
       project::AddClip{dry, dry_clip}, project::InsertVst3Plugin{audio, 0, plugin},
       project::AddSendRoute{audio, {id<project::SendId>(7), bus, 0.5}}}));
  if (serial) {
    auto second_plugin = plugin;
    second_plugin.id = id<project::PluginInstanceId>(18);
    const float second_gain = 0.25F;
    std::memcpy(second_plugin.component_state.data(), &second_gain, sizeof(second_gain));
    (void)take(model.apply({project::InsertVst3Plugin{audio, 1, second_plugin}}));
  }
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save,
                              std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "effect project save did not commit");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  auto reopened = take(reader.load_current());
  REQUIRE_EQ(reopened.snapshot.tracks.front().effects.size(), (serial ? 2U : 1U),
             "effect insert identity or state did not survive reopen");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 reopened.snapshot.tracks.front().effects.front().processor),
             plugin, "effect insert identity or state did not survive reopen");
  if (serial) {
    const auto& second = std::get<project::Vst3PluginInstance>(
        reopened.snapshot.tracks.front().effects.back().processor);
    float restored_gain = 0.0F;
    if (second.component_state.size() == sizeof(restored_gain))
      std::memcpy(&restored_gain, second.component_state.data(), sizeof(restored_gain));
    REQUIRE_EQ(second.id, id<project::PluginInstanceId>(18),
               "second serial effect identity/state did not survive reopen");
    REQUIRE_EQ(second.component_state.size(), 4,
               "second serial effect identity/state did not survive reopen");
    REQUIRE_EQ(restored_gain, 0.25F, "second serial effect identity/state did not survive reopen");
  }
  return {std::move(reopened.snapshot), std::move(reader)};
}

void serial_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "serial effect fixture scan failed");
  auto saved = make_project(directory, gate, scan, true);
  auto render = [&](const project::Snapshot& snapshot, const fs::path& target,
                    std::optional<project::TrackId> stem = std::nullopt) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    if (stem)
      return exporting::render_vst3_stem(snapshot, saved.store, std::move(authority), scanner,
                                         *stem, {target});
    return exporting::render_vst3_master(snapshot, saved.store, std::move(authority), scanner,
                                         exporting::MasterRenderOptions{target});
  };
  const auto master_path = directory.path / "serial-master.wav";
  REQUIRE_EQ(take(render(saved.snapshot, master_path)), exporting::PublishOutcome::durable,
             "serial master did not publish");
  const auto master = read_file(master_path);
  REQUIRE_EQ(master.size(), 56 + 135 * 8, "serial latency/tail output length changed");
  near(sample(master, 62), 0.0F);
  near(sample(master, 63), 0.296875F); // Wet 0.046875 plus aligned dry 0.25.
  near(sample(master, 64), 0.1484375F);
  near(sample(master, 127), 0.046875F);
  near(sample(master, 128), 0.0234375F);
  near(sample(master, 134), 0.0F);
  const auto wet_path = directory.path / "serial-wet.wav";
  const auto dry_path = directory.path / "serial-dry.wav";
  REQUIRE_EQ(take(render(saved.snapshot, wet_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable, "serial processed stem did not publish");
  REQUIRE_EQ(take(render(saved.snapshot, dry_path, id<project::TrackId>(9))),
             exporting::PublishOutcome::durable, "serial processed stem did not publish");
  const auto wet = read_file(wet_path);
  const auto dry = read_file(dry_path);
  REQUIRE_EQ(wet.size(), master.size(), "serial stems lost common PDC alignment");
  REQUIRE_EQ(dry.size(), master.size(), "serial stems lost common PDC alignment");
  for (std::size_t frame = 0; frame < 135; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(master, frame, channel),
           sample(wet, frame, channel) + sample(dry, frame, channel));

  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "second serial tail fixture scan failed");
  auto tail_snapshot = saved.snapshot;
  auto& tail_effect =
      std::get<project::Vst3PluginInstance>(tail_snapshot.tracks.front().effects.back().processor);
  tail_effect.bundle_path = tail_scan.binary.canonical_bundle.string();
  tail_effect.binary_sha256 = tail_scan.binary.sha256;
  const auto& tail_class = tail_scan.metadata.classes.front();
  tail_effect.class_id = tail_class.id;
  tail_effect.vendor = tail_class.vendor;
  tail_effect.name = tail_class.name;
  tail_effect.version = tail_class.version;
  auto& last_clip = tail_snapshot.tracks.front().clips.back();
  last_clip.source_range = {{2}, {3}};
  last_clip.placement = project::ElapsedInterval{{134}, {135}};
  const auto serial_tail_path = directory.path / "serial-tail.wav";
  REQUIRE_EQ(take(render(tail_snapshot, serial_tail_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable, "serial second effect tail did not publish");
  const auto serial_tail = read_file(serial_tail_path);
  REQUIRE_EQ(serial_tail.size(), 56 + 143 * 8, "serial second effect latency/tail length changed");
  near(sample(serial_tail, 141), 0.0F);
  near(sample(serial_tail, 142), 0.046875F);

  const auto& original = master;
  auto failed = saved.snapshot;
  auto& second =
      std::get<project::Vst3PluginInstance>(failed.tracks.front().effects.back().processor);
  const float failing_gain = 999.0F;
  std::memcpy(second.component_state.data(), &failing_gain, sizeof(failing_gain));
  REQUIRE_TRUE(!render(failed, master_path),
               "failure of second serial processor replaced a valid master");
  REQUIRE_EQ(read_file(master_path), original,
             "failure of second serial processor replaced a valid master");
  const auto rejected = directory.path / "serial-rejected.wav";
  auto reject = [&](const project::Snapshot& candidate) {
    REQUIRE_TRUE(!render(candidate, rejected), "unsupported serial chain opened output");
    REQUIRE_TRUE(!fs::exists(rejected), "unsupported serial chain opened output");
  };
  // A bypassed stage and a longer chain are ordinary chains (ADR-0050).
  const auto accept = [&](const project::Snapshot& candidate) {
    REQUIRE_EQ(take(render(candidate, rejected)), exporting::PublishOutcome::durable,
               "a supported serial chain did not export");
    fs::remove(rejected);
  };
  auto wrong = saved.snapshot;
  wrong.tracks.front().effects.back().enabled = false;
  accept(wrong);
  wrong = saved.snapshot;
  auto third = wrong.tracks.front().effects.back();
  third.id = id<project::InsertSlotId>(99);
  std::get<project::Vst3PluginInstance>(third.processor).id = id<project::PluginInstanceId>(99);
  wrong.tracks.front().effects.push_back(third);
  auto fourth = third;
  fourth.id = id<project::InsertSlotId>(100);
  std::get<project::Vst3PluginInstance>(fourth.processor).id = id<project::PluginInstanceId>(100);
  wrong.tracks.front().effects.push_back(fourth);
  accept(wrong);
  wrong = saved.snapshot;
  std::get<project::Vst3PluginInstance>(wrong.tracks.front().effects.back().processor)
      .binary_sha256.assign(64, '0');
  reject(wrong);
}

void sidechain_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto gain_scan = scanner.scan(VST3_GAIN_BUNDLE);
  const auto sidechain_scan = scanner.scan(VST3_SIDECHAIN_TAIL_BUNDLE);
  REQUIRE_TRUE(gain_scan.ok(), "sidechain export fixture scans failed");
  REQUIRE_TRUE(sidechain_scan.ok(), "sidechain export fixture scans failed");
  auto saved = make_project(directory, gate, gain_scan);
  auto& effect = saved.snapshot.tracks.front().effects.front();
  auto& plugin = std::get<project::Vst3PluginInstance>(effect.processor);
  const auto& klass = sidechain_scan.metadata.classes.front();
  plugin.class_id = klass.id;
  plugin.vendor = klass.vendor;
  plugin.name = klass.name;
  plugin.version = klass.version;
  plugin.bundle_path = sidechain_scan.binary.canonical_bundle.string();
  plugin.binary_sha256 = sidechain_scan.binary.sha256;
  const float gain = 1.0F;
  std::memcpy(plugin.component_state.data(), &gain, sizeof(gain));
  for (const std::size_t frames : {64U, 128U}) {
    auto bound = plugins::bind_vst3_project_effect(
        plugin, sidechain_scan, 48'000, static_cast<std::uint32_t>(frames),
        plugins::Vst3ProcessingMode::offline, plugins::default_vst3_parameter_events, true);
    REQUIRE_TRUE(bound.ready(),
                 "64/128-frame detector fixture did not activate with truthful latency and tail");
    REQUIRE_TRUE(bound.effect->host().routed_sidechain_enabled(),
                 "64/128-frame detector fixture did not activate with truthful latency and tail");
    REQUIRE_EQ(bound.effect->host().latency_samples(), 7,
               "64/128-frame detector fixture did not activate with truthful latency and tail");
    REQUIRE_EQ(bound.effect->host().finite_tail_samples(), 3,
               "64/128-frame detector fixture did not activate with truthful latency and tail");
    std::array<float, 128> input_left{}, input_right{}, detector_left{}, detector_right{},
        output_left{}, output_right{};
    input_left[0] = 0.25F;
    input_right[0] = 0.125F;
    detector_left[0] = 0.125F;
    detector_right[0] = 0.0625F;
    const auto first = [frames](auto& values) { return std::span(values).first(frames); };
    REQUIRE_EQ(bound.effect->host().process({first(input_left), first(input_right),
                                             first(output_left), first(output_right), 0, 0.0, 120.0,
                                             true, first(detector_left), first(detector_right)}),
               plugins::Vst3ProcessStatus::ok,
               "64/128-frame detector fixture could not process its auxiliary input");
    for (std::size_t frame = 0; frame < frames; ++frame) {
      near(output_left[frame], frame == 7 ? 0.375F : frame == 10 ? 0.1875F : 0.0F);
      near(output_right[frame], frame == 7 ? 0.1875F : frame == 10 ? 0.09375F : 0.0F);
    }
  }
  const auto wet_id = saved.snapshot.tracks.front().id;
  const auto dry_id = saved.snapshot.tracks.back().id;
  saved.snapshot.tracks.back().sends.push_back(
      {id<project::SendId>(30), wet_id, 0.5, project::SendPurpose::sidechain, effect.id});
  auto render = [&](const project::Snapshot& candidate, const fs::path& path,
                    std::optional<project::TrackId> stem = std::nullopt) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    if (stem)
      return exporting::render_vst3_stem(candidate, saved.store, std::move(authority), scanner,
                                         *stem, {path});
    return exporting::render_vst3_master(candidate, saved.store, std::move(authority), scanner,
                                         exporting::MasterRenderOptions{path});
  };
  const auto master_path = directory.path / "sidechain-master.wav";
  const auto wet_path = directory.path / "sidechain-wet.wav";
  const auto dry_path = directory.path / "sidechain-dry.wav";
  REQUIRE_EQ(take(render(saved.snapshot, master_path)), exporting::PublishOutcome::durable,
             "sidechain master or selected stems did not publish");
  REQUIRE_EQ(take(render(saved.snapshot, wet_path, wet_id)), exporting::PublishOutcome::durable,
             "sidechain master or selected stems did not publish");
  REQUIRE_EQ(take(render(saved.snapshot, dry_path, dry_id)), exporting::PublishOutcome::durable,
             "sidechain master or selected stems did not publish");
  const auto master = read_file(master_path);
  const auto wet = read_file(wet_path);
  const auto dry = read_file(dry_path);
  REQUIRE_EQ(master.size(), 56 + 138 * 8, "sidechain master/stem extent or PDC changed");
  REQUIRE_EQ(wet.size(), master.size(), "sidechain master/stem extent or PDC changed");
  REQUIRE_EQ(dry.size(), master.size(), "sidechain master/stem extent or PDC changed");
  near(sample(master, 62), 0.0F);
  near(sample(master, 63), 0.8125F);
  near(sample(wet, 63), 0.5625F);
  near(sample(dry, 63), 0.25F);
  near(sample(master, 64), 0.40625F);
  near(sample(wet, 64), 0.28125F);
  near(sample(dry, 64), 0.125F);
  near(sample(master, 66), 0.28125F); // Three-frame wet effect tail, not dry bleed.
  near(sample(wet, 66), 0.28125F);
  near(sample(dry, 66), 0.0F);
  near(sample(master, 67), 0.140625F);
  near(sample(master, 134), 0.0F);
  for (std::size_t frame = 0; frame < 138; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(master, frame, channel),
           sample(wet, frame, channel) + sample(dry, frame, channel));

  const auto preserved = master;
  auto missing = saved.snapshot;
  missing.tracks.back().sends.clear();
  REQUIRE_TRUE(!render(missing, master_path),
               "absent detector send replaced the valid sidechain master");
  REQUIRE_EQ(read_file(master_path), preserved,
             "absent detector send replaced the valid sidechain master");
  auto incompatible = saved.snapshot;
  auto& incompatible_plugin =
      std::get<project::Vst3PluginInstance>(incompatible.tracks.front().effects.front().processor);
  const auto& gain_class = gain_scan.metadata.classes.front();
  incompatible_plugin.class_id = gain_class.id;
  incompatible_plugin.vendor = gain_class.vendor;
  incompatible_plugin.name = gain_class.name;
  incompatible_plugin.version = gain_class.version;
  incompatible_plugin.bundle_path = gain_scan.binary.canonical_bundle.string();
  incompatible_plugin.binary_sha256 = gain_scan.binary.sha256;
  const auto refused = render(incompatible, master_path);
  REQUIRE_TRUE(!refused, "effect without activated auxiliary bus replaced the valid master");
  REQUIRE_NE(refused.error().message.find("exactly one stereo auxiliary input"), std::string::npos,
             "effect without activated auxiliary bus replaced the valid master");
  REQUIRE_EQ(read_file(master_path), preserved,
             "effect without activated auxiliary bus replaced the valid master");
}

void three_serial_effect_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "three-stage gain fixture scan failed");
  auto saved = make_project(directory, gate, scan, true);
  auto model = take(project::ProjectModel::open(saved.snapshot, gate));
  auto third = std::get<project::Vst3PluginInstance>(
      model.snapshot()->tracks.front().effects.front().processor);
  third.id = id<project::PluginInstanceId>(28);
  const float gain = 0.75F;
  std::memcpy(third.component_state.data(), &gain, sizeof(gain));
  (void)take(model.apply({project::InsertVst3Plugin{id<project::TrackId>(2), 2, third}}));
  const auto& snapshot = *model.snapshot();
  REQUIRE_EQ(snapshot.tracks.front().effects.size(), 3,
             "third saved effect did not retain its exact slot order");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(snapshot.tracks.front().effects[2].processor).id,
             third.id, "third saved effect did not retain its exact slot order");
  auto render = [&](const project::Snapshot& candidate, const fs::path& target,
                    std::optional<project::TrackId> stem = std::nullopt) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    if (stem)
      return exporting::render_vst3_stem(candidate, saved.store, std::move(authority), scanner,
                                         *stem, {target});
    return exporting::render_vst3_master(candidate, saved.store, std::move(authority), scanner,
                                         exporting::MasterRenderOptions{target});
  };
  const auto master_path = directory.path / "three-master.wav";
  const auto wet_path = directory.path / "three-wet.wav";
  const auto dry_path = directory.path / "three-dry.wav";
  REQUIRE_EQ(take(render(snapshot, master_path)), exporting::PublishOutcome::durable,
             "three-stage master or stems did not publish");
  REQUIRE_EQ(take(render(snapshot, wet_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable, "three-stage master or stems did not publish");
  REQUIRE_EQ(take(render(snapshot, dry_path, id<project::TrackId>(9))),
             exporting::PublishOutcome::durable, "three-stage master or stems did not publish");
  const auto master = read_file(master_path);
  const auto wet = read_file(wet_path);
  const auto dry = read_file(dry_path);
  REQUIRE_EQ(master.size(), 56 + 135 * 8, "three-stage PDC or finite output interval changed");
  REQUIRE_EQ(wet.size(), master.size(), "three-stage PDC or finite output interval changed");
  REQUIRE_EQ(dry.size(), master.size(), "three-stage PDC or finite output interval changed");
  near(sample(master, 62), 0.0F);
  near(sample(master, 63), 0.28515625F);
  for (std::size_t frame = 0; frame < 135; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(master, frame, channel),
           sample(wet, frame, channel) + sample(dry, frame, channel));
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "three-stage tail fixture scan failed");
  auto tailed = snapshot;
  auto& tail_effect =
      std::get<project::Vst3PluginInstance>(tailed.tracks.front().effects.back().processor);
  tail_effect.bundle_path = tail_scan.binary.canonical_bundle.string();
  tail_effect.binary_sha256 = tail_scan.binary.sha256;
  const auto& tail_class = tail_scan.metadata.classes.front();
  tail_effect.class_id = tail_class.id;
  tail_effect.vendor = tail_class.vendor;
  tail_effect.name = tail_class.name;
  tail_effect.version = tail_class.version;
  auto& last_clip = tailed.tracks.front().clips.back();
  last_clip.source_range = {{2}, {3}};
  last_clip.placement = project::ElapsedInterval{{134}, {135}};
  const auto tail_path = directory.path / "three-tail-wet.wav";
  REQUIRE_EQ(take(render(tailed, tail_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable, "third-stage finite tail did not publish");
  const auto tail = read_file(tail_path);
  REQUIRE_EQ(tail.size(), 56 + 143 * 8, "third-stage finite tail was shortened or lost");
  REQUIRE_NE(sample(tail, 142), 0.0F, "third-stage finite tail was shortened or lost");
  const auto original = master;
  auto unsupported = snapshot;
  auto fourth = third;
  fourth.id = id<project::PluginInstanceId>(29);
  auto fourth_model = take(project::ProjectModel::open(std::move(unsupported), gate));
  (void)take(fourth_model.apply({project::InsertVst3Plugin{id<project::TrackId>(2), 3, fourth}}));
  // A fourth stage is an ordinary chain (ADR-0050).
  const auto four_path = directory.path / "four-stage.wav";
  REQUIRE_EQ(take(render(*fourth_model.snapshot(), four_path)), exporting::PublishOutcome::durable,
             "four-stage export failed or changed existing output");
  REQUIRE_EQ(read_file(master_path), original,
             "four-stage export failed or changed existing output");
}

void mixed_eq_vst3_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "mixed effect fixture scan failed");
  auto saved = make_project(directory, gate, scan);
  auto mixed = saved.snapshot;
  mixed.tracks.front().effects.insert(
      mixed.tracks.front().effects.begin(),
      {id<project::InsertSlotId>(3100), true,
       test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0})});
  auto render = [&](const project::Snapshot& snapshot, const fs::path& target,
                    std::optional<project::TrackId> stem = std::nullopt) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    if (stem)
      return exporting::render_vst3_stem(snapshot, saved.store, std::move(authority), scanner,
                                         *stem, {target});
    return exporting::render_vst3_master(snapshot, saved.store, std::move(authority), scanner,
                                         exporting::MasterRenderOptions{target});
  };
  const auto master_path = directory.path / "mixed-master.wav";
  REQUIRE_EQ(take(render(mixed, master_path)), exporting::PublishOutcome::durable,
             "mixed EQ/VST3 master did not publish");
  const auto master = read_file(master_path);
  REQUIRE_GT(master.size(), 56 + 135 * 8, "mixed EQ IIR tail was truncated");
  const std::array<native_dsp::EqBandSettings, 1> bands{
      {{native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}};
  native_dsp::EqRuntime oracle(take(native_dsp::prepare_eq(bands, 48'000)));
  std::array<engine::StereoFrame, 135> wet{};
  wet[63] = {0.25F, 0.125F};
  wet[64] = {0.125F, 0.0625F};
  wet[127] = {0.25F, 0.125F};
  wet[128] = {0.125F, 0.0625F};
  oracle.process(wet, {0, 1});
  for (const auto frame : {63U, 64U, 127U, 128U}) {
    const float dry = frame == 63 ? 0.25F : frame == 64 ? 0.125F : 0.0F;
    near(sample(master, frame), dry + wet[frame].left * 0.75F);
  }
  for (std::size_t frame = 0; frame < (master.size() - 56) / 8; ++frame)
    require(std::isfinite(sample(master, frame)) && std::isfinite(sample(master, frame, 1)),
            "mixed EQ/VST3 export emitted non-finite samples");
  const auto wet_path = directory.path / "mixed-wet.wav";
  const auto dry_path = directory.path / "mixed-dry.wav";
  REQUIRE_EQ(take(render(mixed, wet_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable, "mixed EQ/VST3 stems did not publish");
  REQUIRE_EQ(take(render(mixed, dry_path, id<project::TrackId>(9))),
             exporting::PublishOutcome::durable, "mixed EQ/VST3 stems did not publish");
  const auto wet_stem = read_file(wet_path);
  const auto dry_stem = read_file(dry_path);
  REQUIRE_EQ(wet_stem.size(), master.size(), "mixed stems lost common tail/PDC duration");
  REQUIRE_EQ(dry_stem.size(), master.size(), "mixed stems lost common tail/PDC duration");
  for (std::size_t frame = 0; frame < (master.size() - 56) / 8; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(master, frame, channel),
           sample(wet_stem, frame, channel) + sample(dry_stem, frame, channel));
  const auto repeat_path = directory.path / "mixed-repeat.wav";
  REQUIRE_EQ(take(render(mixed, repeat_path)), exporting::PublishOutcome::durable,
             "mixed EQ/VST3 repeat render changed output");
  REQUIRE_EQ(read_file(repeat_path), master, "mixed EQ/VST3 repeat render changed output");
  const auto rejected = directory.path / "mixed-rejected.wav";
  auto nonlinear_before = mixed;
  auto& nonlinear_plugin =
      std::get<project::Vst3PluginInstance>(nonlinear_before.tracks.front().effects[1].processor);
  const float nonlinear_state = -94.0F;
  std::memcpy(nonlinear_plugin.component_state.data(), &nonlinear_state, sizeof(nonlinear_state));
  auto nonlinear_after = nonlinear_before;
  std::ranges::swap(nonlinear_after.tracks.front().effects[0],
                    nonlinear_after.tracks.front().effects[1]);
  const auto before_path = directory.path / "eq-before-nonlinear.wav";
  const auto after_path = directory.path / "nonlinear-before-eq.wav";
  REQUIRE_EQ(take(render(nonlinear_before, before_path)), exporting::PublishOutcome::durable,
             "both saved mixed orders did not publish");
  REQUIRE_EQ(take(render(nonlinear_after, after_path)), exporting::PublishOutcome::durable,
             "both saved mixed orders did not publish");
  const auto before_wav = read_file(before_path);
  const auto after_wav = read_file(after_path);
  REQUIRE_EQ(before_wav.size(), after_wav.size(),
             "nonlinear order fixture did not distinguish the saved slot orders");
  REQUIRE_NE(before_wav, after_wav,
             "nonlinear order fixture did not distinguish the saved slot orders");
  std::array<engine::StereoFrame, 135> source{};
  source[63] = {0.25F, 0.125F};
  source[64] = {0.125F, 0.0625F};
  source[127] = {0.25F, 0.125F};
  source[128] = {0.125F, 0.0625F};
  auto source_after = source;
  native_dsp::EqRuntime before_eq(take(native_dsp::prepare_eq(bands, 48'000)));
  before_eq.process(source, {0, 1});
  for (auto& frame : source) {
    frame.left *= std::fabs(frame.left);
    frame.right *= std::fabs(frame.right);
  }
  for (auto& frame : source_after) {
    frame.left *= std::fabs(frame.left);
    frame.right *= std::fabs(frame.right);
  }
  native_dsp::EqRuntime after_eq(take(native_dsp::prepare_eq(bands, 48'000)));
  after_eq.process(source_after, {0, 1});
  for (const auto frame : {63U, 64U, 65U, 127U, 128U, 129U}) {
    const float dry = frame == 63 ? 0.25F : frame == 64 ? 0.125F : 0.0F;
    near(sample(before_wav, frame), dry + source[frame].left * 1.5F);
    near(sample(after_wav, frame), dry + source_after[frame].left * 1.5F);
  }
  const auto after_wet_path = directory.path / "nonlinear-before-eq-wet.wav";
  const auto after_dry_path = directory.path / "nonlinear-before-eq-dry.wav";
  REQUIRE_EQ(take(render(nonlinear_after, after_wet_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable,
             "VST3-then-EQ processed/dry stems did not publish");
  REQUIRE_EQ(take(render(nonlinear_after, after_dry_path, id<project::TrackId>(9))),
             exporting::PublishOutcome::durable,
             "VST3-then-EQ processed/dry stems did not publish");
  const auto after_wet = read_file(after_wet_path);
  const auto after_dry = read_file(after_dry_path);
  REQUIRE_EQ(after_wet.size(), after_wav.size(), "VST3-then-EQ stem tails or PDC lengths changed");
  REQUIRE_EQ(after_dry.size(), after_wav.size(), "VST3-then-EQ stem tails or PDC lengths changed");
  for (std::size_t frame = 0; frame < (after_wav.size() - 56) / 8; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(after_wav, frame, channel),
           sample(after_wet, frame, channel) + sample(after_dry, frame, channel));
  const auto after_repeat = directory.path / "nonlinear-before-eq-repeat.wav";
  REQUIRE_EQ(take(render(nonlinear_after, after_repeat)), exporting::PublishOutcome::durable,
             "VST3-then-EQ repeat render changed samples");
  REQUIRE_EQ(read_file(after_repeat), after_wav, "VST3-then-EQ repeat render changed samples");
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "VST3-then-EQ tail fixture scan failed");
  auto tail_after = nonlinear_after;
  auto& tail_plugin =
      std::get<project::Vst3PluginInstance>(tail_after.tracks.front().effects[0].processor);
  tail_plugin.bundle_path = tail_scan.binary.canonical_bundle.string();
  tail_plugin.binary_sha256 = tail_scan.binary.sha256;
  const auto& tail_class = tail_scan.metadata.classes.front();
  tail_plugin.class_id = tail_class.id;
  tail_plugin.vendor = tail_class.vendor;
  tail_plugin.name = tail_class.name;
  tail_plugin.version = tail_class.version;
  const float tail_gain = 0.5F;
  std::memcpy(tail_plugin.component_state.data(), &tail_gain, sizeof(tail_gain));
  auto& last_clip = tail_after.tracks.front().clips.back();
  last_clip.source_range = {{2}, {3}};
  last_clip.placement = project::ElapsedInterval{{134}, {135}};
  const auto tail_path = directory.path / "vst3-tail-before-eq.wav";
  auto tail_authority = take(exporting::StoppedExportAuthority::acquire(tail_after, gate));
  REQUIRE_EQ(take(exporting::render_vst3_stem(tail_after, saved.store, std::move(tail_authority),
                                              scanner, id<project::TrackId>(2), {tail_path})),
             exporting::PublishOutcome::durable, "VST3-then-EQ tail stem did not publish");
  const auto tail_wav = read_file(tail_path);
  REQUIRE_EQ(tail_wav.size(), after_wav.size() + std::size_t{8} * 8,
             "VST3 tail into downstream EQ was truncated");
  REQUIRE_GT(std::fabs(sample(tail_wav, 142)), 0.00001F,
             "VST3 tail into downstream EQ was truncated");
  auto bypass = mixed;
  bypass.tracks.front().effects[0].enabled = false;
  // A bypassed stage simply leaves the chain (ADR-0050).
  REQUIRE_EQ(take(render(bypass, rejected)), exporting::PublishOutcome::durable,
             "a mixed chain with its EQ bypassed did not export");
  fs::remove(rejected);
  auto missing = mixed;
  std::get<project::Vst3PluginInstance>(missing.tracks.front().effects[1].processor)
      .binary_sha256.assign(64, '0');
  REQUIRE_TRUE(!render(missing, rejected), "mixed export ignored stale VST3 binary identity");
  REQUIRE_TRUE(!fs::exists(rejected), "mixed export ignored stale VST3 binary identity");
  auto invalid = nonlinear_after;
  invalid.tracks.front().effects[1].processor = test_insert::with_raw_value(
      std::get<project::NativeEffectPatch>(invalid.tracks.front().effects[1].processor),
      "shape.kind_1", 255.0);
  REQUIRE_TRUE(!render(invalid, after_path),
               "invalid VST3-then-EQ settings replaced the valid durable master");
  REQUIRE_EQ(read_file(after_path), after_wav,
             "invalid VST3-then-EQ settings replaced the valid durable master");
}

void mixed_compressor_vst3_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "mixed compressor fixture scan failed");
  auto saved = make_project(directory, gate, scan);
  auto mixed = saved.snapshot;
  const native_dsp::CompressorSettings settings{-24.0, 6.0, 0.0, 0.1, 100.0, 0.0};
  mixed.tracks.front().effects.insert(
      mixed.tracks.front().effects.begin(),
      {id<project::InsertSlotId>(3200), true, test_insert::press(settings)});
  auto& plugin = std::get<project::Vst3PluginInstance>(mixed.tracks.front().effects[1].processor);
  const float nonlinear_state = -94.0F;
  std::memcpy(plugin.component_state.data(), &nonlinear_state, sizeof(nonlinear_state));
  auto render = [&](const project::Snapshot& snapshot, const fs::path& target,
                    std::optional<project::TrackId> stem = std::nullopt) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    if (stem)
      return exporting::render_vst3_stem(snapshot, saved.store, std::move(authority), scanner,
                                         *stem, {target});
    return exporting::render_vst3_master(snapshot, saved.store, std::move(authority), scanner,
                                         exporting::MasterRenderOptions{target});
  };
  auto reversed = mixed;
  std::ranges::swap(reversed.tracks.front().effects[0], reversed.tracks.front().effects[1]);
  const auto before_path = directory.path / "compressor-before-vst3.wav";
  const auto after_path = directory.path / "vst3-before-compressor.wav";
  REQUIRE_EQ(take(render(mixed, before_path)), exporting::PublishOutcome::durable,
             "compressor/VST3 orders did not publish");
  REQUIRE_EQ(take(render(reversed, after_path)), exporting::PublishOutcome::durable,
             "compressor/VST3 orders did not publish");
  const auto before = read_file(before_path);
  const auto after = read_file(after_path);
  REQUIRE_EQ(before.size(), 56 + 135 * 8, "compressor/VST3 order or PDC duration changed");
  REQUIRE_EQ(before.size(), after.size(), "compressor/VST3 order or PDC duration changed");
  REQUIRE_NE(before, after, "compressor/VST3 order or PDC duration changed");
  std::array<engine::StereoFrame, 142> source{};
  source[63] = {0.25F, 0.125F};
  source[64] = {0.125F, 0.0625F};
  source[127] = {0.25F, 0.125F};
  source[128] = {0.125F, 0.0625F};
  const auto prepared = take(native_dsp::prepare_compressor(settings, 48'000));
  auto native_before = source;
  native_dsp::CompressorRuntime before_oracle(prepared);
  before_oracle.process(native_before, {0, 1});
  auto plugin_before = source;
  for (std::size_t frame = 7; frame < plugin_before.size(); ++frame) {
    plugin_before[frame].left =
        native_before[frame - 7].left * std::fabs(native_before[frame - 7].left);
    plugin_before[frame].right =
        native_before[frame - 7].right * std::fabs(native_before[frame - 7].right);
  }
  auto plugin_after = source;
  plugin_after.fill({});
  for (std::size_t frame = 7; frame < plugin_after.size(); ++frame) {
    plugin_after[frame].left = source[frame - 7].left * std::fabs(source[frame - 7].left);
    plugin_after[frame].right = source[frame - 7].right * std::fabs(source[frame - 7].right);
  }
  native_dsp::CompressorRuntime after_oracle(prepared);
  after_oracle.process(plugin_after, {0, 1});
  for (const auto frame : {62U, 63U, 64U, 65U, 127U, 128U, 129U, 134U}) {
    const float dry = frame == 63 ? 0.25F : frame == 64 ? 0.125F : 0.0F;
    near(sample(before, frame), dry + plugin_before[frame + 7].left * 1.5F);
    near(sample(after, frame), dry + plugin_after[frame + 7].left * 1.5F);
  }
  for (std::size_t frame = 0; frame < 135; ++frame)
    require(std::isfinite(sample(before, frame)) && std::isfinite(sample(after, frame)),
            "compressor/VST3 export emitted non-finite audio");
  const auto wet_path = directory.path / "compressor-vst3-wet.wav";
  const auto dry_path = directory.path / "compressor-vst3-dry.wav";
  REQUIRE_EQ(take(render(reversed, wet_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable, "VST3-before-compressor stems did not publish");
  REQUIRE_EQ(take(render(reversed, dry_path, id<project::TrackId>(9))),
             exporting::PublishOutcome::durable, "VST3-before-compressor stems did not publish");
  const auto wet = read_file(wet_path);
  const auto dry = read_file(dry_path);
  REQUIRE_EQ(wet.size(), after.size(), "compressor/VST3 stems lost common PDC duration");
  REQUIRE_EQ(dry.size(), after.size(), "compressor/VST3 stems lost common PDC duration");
  for (std::size_t frame = 0; frame < 135; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(after, frame, channel),
           sample(wet, frame, channel) + sample(dry, frame, channel));
  const auto repeat_path = directory.path / "compressor-vst3-repeat.wav";
  REQUIRE_EQ(take(render(reversed, repeat_path)), exporting::PublishOutcome::durable,
             "compressor/VST3 repeat render changed samples");
  REQUIRE_EQ(read_file(repeat_path), after, "compressor/VST3 repeat render changed samples");
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "compressor/VST3 tail fixture scan failed");
  auto tail = reversed;
  auto& tail_plugin =
      std::get<project::Vst3PluginInstance>(tail.tracks.front().effects[0].processor);
  tail_plugin.bundle_path = tail_scan.binary.canonical_bundle.string();
  tail_plugin.binary_sha256 = tail_scan.binary.sha256;
  const auto& klass = tail_scan.metadata.classes.front();
  tail_plugin.class_id = klass.id;
  tail_plugin.vendor = klass.vendor;
  tail_plugin.name = klass.name;
  tail_plugin.version = klass.version;
  const float tail_gain = 0.5F;
  std::memcpy(tail_plugin.component_state.data(), &tail_gain, sizeof(tail_gain));
  auto& last_clip = tail.tracks.front().clips.back();
  last_clip.source_range = {{2}, {3}};
  last_clip.placement = project::ElapsedInterval{{134}, {135}};
  const auto tail_path = directory.path / "vst3-tail-before-compressor.wav";
  auto authority = take(exporting::StoppedExportAuthority::acquire(tail, gate));
  REQUIRE_EQ(take(exporting::render_vst3_stem(tail, saved.store, std::move(authority), scanner,
                                              id<project::TrackId>(2), {tail_path})),
             exporting::PublishOutcome::durable, "VST3 tail into compressor did not publish");
  const auto tail_wav = read_file(tail_path);
  REQUIRE_EQ(tail_wav.size(), 56 + 143 * 8, "VST3 tail into downstream compressor was truncated");
  REQUIRE_GT(std::fabs(sample(tail_wav, 142)), 0.00001F,
             "VST3 tail into downstream compressor was truncated");
  auto bypass = mixed;
  bypass.tracks.front().effects[0].enabled = false;
  const auto rejected = directory.path / "compressor-rejected.wav";
  // A bypassed or added stage is an ordinary chain (ADR-0050).
  REQUIRE_EQ(take(render(bypass, rejected)), exporting::PublishOutcome::durable,
             "bypassed mixed compressor did not export");
  fs::remove(rejected);
  auto third = mixed;
  third.tracks.front().effects.push_back(
      {id<project::InsertSlotId>(3201), true, test_insert::flat_shape()});
  // A bypassed or added stage is an ordinary chain (ADR-0050).
  REQUIRE_EQ(take(render(third, rejected)), exporting::PublishOutcome::durable,
             "third stage in mixed compressor chain did not export");
  fs::remove(rejected);
  auto invalid = reversed;
  invalid.tracks.front().effects[1].processor = test_insert::with_raw_value(
      std::get<project::NativeEffectPatch>(invalid.tracks.front().effects[1].processor),
      "press.ratio", -1.0);
  REQUIRE_TRUE(!render(invalid, after_path), "invalid compressor settings replaced durable master");
  REQUIRE_EQ(read_file(after_path), after, "invalid compressor settings replaced durable master");
}

// Ceiling (ADR-0117) as the export fixture sets it, run over `input` and
// `latency` frames of silence after it: the oracle for a chain holding it.
std::vector<engine::StereoFrame> ceiling_oracle(const native_dsp::LimiterSettings& settings,
                                                std::span<const engine::StereoFrame> input) {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr std::size_t block = 128;
  const auto patch = test_insert::ceiling(settings);
  const auto& limiter = native_dsp::effects::ceiling::engine();
  auto values = native_dsp::default_values(*limiter.descriptor);
  for (const auto& parameter : patch.parameters)
    values[*native_dsp::find_parameter(*limiter.descriptor, parameter.id)] = parameter.value;
  auto runtime = take(native_dsp::effects::NativeEffectRuntime::create(limiter, values, rate));
  std::vector<engine::StereoFrame> out(input.begin(), input.end());
  out.resize(input.size() + runtime->latency_frames());
  const native_dsp::Transport stopped{0.0, 0.0, 0.0, 4.0};
  for (std::size_t at = 0; at < out.size(); at += block) {
    const auto span = std::span(out).subspan(at, std::min(block, out.size() - at));
    (void)runtime->process(span, {}, at, 1, stopped, {});
  }
  return out;
}

void mixed_limiter_vst3_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "mixed limiter fixture scan failed");
  auto saved = make_project(directory, gate, scan);
  auto mixed = saved.snapshot;
  const native_dsp::LimiterSettings settings{-18.0, 0.5, 100.0};
  mixed.tracks.front().effects.insert(
      mixed.tracks.front().effects.begin(),
      {id<project::InsertSlotId>(3400), true, test_insert::ceiling(settings)});
  auto& plugin = std::get<project::Vst3PluginInstance>(mixed.tracks.front().effects[1].processor);
  const float nonlinear_state = -94.0F;
  std::memcpy(plugin.component_state.data(), &nonlinear_state, sizeof(nonlinear_state));
  auto render = [&](const project::Snapshot& snapshot, const fs::path& target,
                    std::optional<project::TrackId> stem = std::nullopt) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    if (stem)
      return exporting::render_vst3_stem(snapshot, saved.store, std::move(authority), scanner,
                                         *stem, {target});
    return exporting::render_vst3_master(snapshot, saved.store, std::move(authority), scanner,
                                         exporting::MasterRenderOptions{target});
  };
  auto reversed = mixed;
  std::ranges::swap(reversed.tracks.front().effects[0], reversed.tracks.front().effects[1]);
  const auto before_path = directory.path / "limiter-before-vst3.wav";
  const auto after_path = directory.path / "vst3-before-limiter.wav";
  REQUIRE_EQ(take(render(mixed, before_path)), exporting::PublishOutcome::durable,
             "limiter/VST3 orders did not publish");
  REQUIRE_EQ(take(render(reversed, after_path)), exporting::PublishOutcome::durable,
             "limiter/VST3 orders did not publish");
  const auto before = read_file(before_path);
  const auto after = read_file(after_path);
  REQUIRE_EQ(before.size(), 56 + 135 * 8,
             "limiter/VST3 order or latency-stripped duration changed");
  REQUIRE_EQ(before.size(), after.size(),
             "limiter/VST3 order or latency-stripped duration changed");
  REQUIRE_NE(before, after, "limiter/VST3 order or latency-stripped duration changed");
  // The VST3 gain fixture's latency, and Ceiling's.
  constexpr std::size_t plugin_latency = 7;
  const std::size_t limiter_latency = test_insert::ceiling_latency(settings.lookahead_ms);
  const std::size_t shift = limiter_latency + plugin_latency;
  std::array<engine::StereoFrame, 154> source{};
  source[63] = {0.25F, 0.125F};
  source[64] = {0.125F, 0.0625F};
  source[127] = {0.25F, 0.125F};
  source[128] = {0.125F, 0.0625F};
  const auto nonlinear = [](float value) { return value * std::fabs(value); };
  const auto limited_first = ceiling_oracle(settings, source);
  std::vector<engine::StereoFrame> expected_before(limited_first.size() + plugin_latency);
  for (std::size_t frame = plugin_latency; frame < expected_before.size(); ++frame)
    expected_before[frame] = {nonlinear(limited_first[frame - plugin_latency].left),
                              nonlinear(limited_first[frame - plugin_latency].right)};
  std::vector<engine::StereoFrame> plugin_first(source.size() + plugin_latency);
  for (std::size_t frame = plugin_latency; frame < plugin_first.size(); ++frame)
    plugin_first[frame] = {nonlinear(source[frame - plugin_latency].left),
                           nonlinear(source[frame - plugin_latency].right)};
  const auto expected_after = ceiling_oracle(settings, plugin_first);
  for (const auto frame : {62U, 63U, 64U, 65U, 127U, 128U, 129U, 134U}) {
    const float dry = frame == 63 ? 0.25F : frame == 64 ? 0.125F : 0.0F;
    near(sample(before, frame), dry + expected_before[frame + shift].left * 1.5F);
    near(sample(after, frame), dry + expected_after[frame + shift].left * 1.5F);
  }
  for (std::size_t frame = 0; frame < 135; ++frame)
    require(std::isfinite(sample(before, frame)) && std::isfinite(sample(after, frame)),
            "limiter/VST3 export emitted non-finite audio");
  const auto wet_path = directory.path / "limiter-vst3-wet.wav";
  const auto dry_path = directory.path / "limiter-vst3-dry.wav";
  REQUIRE_EQ(take(render(reversed, wet_path, id<project::TrackId>(2))),
             exporting::PublishOutcome::durable, "VST3-before-limiter stems did not publish");
  REQUIRE_EQ(take(render(reversed, dry_path, id<project::TrackId>(9))),
             exporting::PublishOutcome::durable, "VST3-before-limiter stems did not publish");
  const auto wet = read_file(wet_path);
  const auto dry = read_file(dry_path);
  REQUIRE_EQ(wet.size(), after.size(), "limiter/VST3 stems lost common PDC duration");
  REQUIRE_EQ(dry.size(), after.size(), "limiter/VST3 stems lost common PDC duration");
  for (std::size_t frame = 0; frame < 135; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(after, frame, channel),
           sample(wet, frame, channel) + sample(dry, frame, channel));
  const auto repeat_path = directory.path / "limiter-vst3-repeat.wav";
  REQUIRE_EQ(take(render(reversed, repeat_path)), exporting::PublishOutcome::durable,
             "limiter/VST3 repeat render changed samples");
  REQUIRE_EQ(read_file(repeat_path), after, "limiter/VST3 repeat render changed samples");
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "limiter/VST3 tail fixture scan failed");
  for (const bool native_first : {false, true}) {
    auto tail = native_first ? mixed : reversed;
    auto& tail_plugin = std::get<project::Vst3PluginInstance>(
        tail.tracks.front().effects[native_first ? 1 : 0].processor);
    tail_plugin.bundle_path = tail_scan.binary.canonical_bundle.string();
    tail_plugin.binary_sha256 = tail_scan.binary.sha256;
    const auto& klass = tail_scan.metadata.classes.front();
    tail_plugin.class_id = klass.id;
    tail_plugin.vendor = klass.vendor;
    tail_plugin.name = klass.name;
    tail_plugin.version = klass.version;
    const float tail_gain = 0.5F;
    std::memcpy(tail_plugin.component_state.data(), &tail_gain, sizeof(tail_gain));
    auto& last_clip = tail.tracks.front().clips.back();
    last_clip.source_range = {{2}, {3}};
    last_clip.placement = project::ElapsedInterval{{134}, {135}};
    const auto tail_path = directory.path / (native_first ? "limiter-before-vst3-tail.wav"
                                                          : "vst3-before-limiter-tail.wav");
    auto authority = take(exporting::StoppedExportAuthority::acquire(tail, gate));
    REQUIRE_EQ(take(exporting::render_vst3_stem(tail, saved.store, std::move(authority), scanner,
                                                id<project::TrackId>(2), {tail_path})),
               exporting::PublishOutcome::durable, "limiter/VST3 plugin-tail stem did not publish");
    const auto tail_wav = read_file(tail_path);
    REQUIRE_EQ(tail_wav.size(), 56 + 143 * 8,
               "plugin tail or limiter lookahead was truncated or double-counted");
    REQUIRE_GT(std::fabs(sample(tail_wav, 142)), 0.00001F,
               "plugin tail or limiter lookahead was truncated or double-counted");
  }
  const auto rejected = directory.path / "limiter-rejected.wav";
  auto bypass = mixed;
  bypass.tracks.front().effects[0].enabled = false;
  // A bypassed or added stage is an ordinary chain (ADR-0050).
  REQUIRE_EQ(take(render(bypass, rejected)), exporting::PublishOutcome::durable,
             "bypassed mixed limiter did not export");
  fs::remove(rejected);
  auto stale = mixed;
  std::get<project::Vst3PluginInstance>(stale.tracks.front().effects[1].processor)
      .binary_sha256.assign(64, '0');
  REQUIRE_TRUE(!render(stale, rejected), "mixed limiter export accepted stale plugin identity");
  REQUIRE_TRUE(!fs::exists(rejected), "mixed limiter export accepted stale plugin identity");
  auto third = mixed;
  third.tracks.front().effects.push_back(
      {id<project::InsertSlotId>(3401), true, test_insert::flat_shape()});
  // A bypassed or added stage is an ordinary chain (ADR-0050).
  REQUIRE_EQ(take(render(third, rejected)), exporting::PublishOutcome::durable,
             "third stage in mixed limiter chain did not export");
  fs::remove(rejected);
  auto invalid = reversed;
  invalid.tracks.front().effects[1].processor = test_insert::with_raw_value(
      std::get<project::NativeEffectPatch>(invalid.tracks.front().effects[1].processor),
      "ceiling.lookahead", 11.0);
  REQUIRE_TRUE(!render(invalid, after_path), "invalid limiter settings replaced durable master");
  REQUIRE_EQ(read_file(after_path), after, "invalid limiter settings replaced durable master");
}

void pure_bus_vst3_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "bus effect fixture scan failed");
  auto saved = make_project(directory, gate, scan);
  for (const auto kind : {project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    auto state = saved.snapshot;
    auto& bus = state.tracks[1];
    bus.kind = kind;
    bus.effects = std::move(state.tracks[0].effects);
    const float gain = 2.0F;
    auto& plugin = std::get<project::Vst3PluginInstance>(bus.effects.front().processor);
    std::memcpy(plugin.component_state.data(), &gain, sizeof(gain));
    if (kind == project::TrackKind::group_bus) {
      state.tracks[0].main_output = bus.id;
      state.tracks[0].sends.clear();
    }
    const auto prefix = kind == project::TrackKind::group_bus ? "group" : "return";
    const auto master_path = directory.path / (std::string(prefix) + "-effect-master.wav");
    auto authority = take(exporting::StoppedExportAuthority::acquire(state, gate));
    REQUIRE_EQ(take(exporting::render_vst3_master(state, saved.store, std::move(authority), scanner,
                                                  exporting::MasterRenderOptions{master_path})),
               exporting::PublishOutcome::durable, "bus effect master did not publish");
    const auto master = read_file(master_path);
    REQUIRE_EQ(master.size(), 56 + 135 * 8, "bus effect output extent changed");
    near(sample(master, 63), 0.75F);
    near(sample(master, 64), 0.375F);
    const auto wet_path = directory.path / (std::string(prefix) + "-effect-source.wav");
    authority = take(exporting::StoppedExportAuthority::acquire(state, gate));
    (void)take(exporting::render_vst3_stem(state, saved.store, std::move(authority), scanner,
                                           id<project::TrackId>(2), {wet_path}));
    const auto dry_path = directory.path / (std::string(prefix) + "-effect-dry.wav");
    authority = take(exporting::StoppedExportAuthority::acquire(state, gate));
    (void)take(exporting::render_vst3_stem(state, saved.store, std::move(authority), scanner,
                                           id<project::TrackId>(9), {dry_path}));
    const auto wet = read_file(wet_path);
    const auto dry = read_file(dry_path);
    for (std::size_t frame = 0; frame < 135; ++frame)
      near(sample(master, frame), sample(wet, frame) + sample(dry, frame));
  }
}

void mixed_bus_vst3_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok(), "mixed bus fixture scan failed");
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "mixed bus tail fixture scan failed");
  auto saved = make_project(directory, gate, scan);
  const native_dsp::LimiterSettings settings{-18.0, 0.5, 100.0};
  auto render = [&](const project::Snapshot& snapshot, const fs::path& target,
                    std::optional<project::TrackId> stem = std::nullopt) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    if (stem)
      return exporting::render_vst3_stem(snapshot, saved.store, std::move(authority), scanner,
                                         *stem, {target});
    return exporting::render_vst3_master(snapshot, saved.store, std::move(authority), scanner,
                                         exporting::MasterRenderOptions{target});
  };
  constexpr std::size_t plugin_latency = 7;
  const std::size_t shift = test_insert::ceiling_latency(settings.lookahead_ms) + plugin_latency;
  std::array<engine::StereoFrame, 154> source{};
  source[63] = {0.25F, 0.125F};
  source[64] = {0.125F, 0.0625F};
  source[127] = {0.25F, 0.125F};
  source[128] = {0.125F, 0.0625F};
  const auto nonlinear = [](float value) { return value * std::fabs(value); };
  for (const auto bus_kind : {project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    for (const bool native_first : {false, true}) {
      auto mixed = saved.snapshot;
      auto& bus = mixed.tracks[1];
      bus.kind = bus_kind;
      if (bus_kind == project::TrackKind::group_bus) {
        mixed.tracks[0].main_output = bus.id;
        mixed.tracks[0].sends.clear();
      }
      bus.effects = std::move(mixed.tracks[0].effects);
      bus.effects.insert(bus.effects.begin(),
                         {id<project::InsertSlotId>(3500), true, test_insert::ceiling(settings)});
      if (!native_first)
        std::ranges::swap(bus.effects[0], bus.effects[1]);
      auto& plugin =
          std::get<project::Vst3PluginInstance>(bus.effects[native_first ? 1 : 0].processor);
      const float nonlinear_state = -94.0F;
      std::memcpy(plugin.component_state.data(), &nonlinear_state, sizeof(nonlinear_state));
      const auto path =
          directory.path /
          (std::string(bus_kind == project::TrackKind::group_bus ? "group" : "return") +
           (native_first ? "-limiter-vst3.wav" : "-vst3-limiter.wav"));
      REQUIRE_EQ(take(render(mixed, path)), exporting::PublishOutcome::durable,
                 "mixed bus master did not publish");
      const auto bytes = read_file(path);
      REQUIRE_EQ(bytes.size(), 56 + 135 * 8, "mixed bus PDC changed output duration");
      auto expected_input = source;
      const float bus_gain = bus_kind == project::TrackKind::group_bus ? 1.0F : 0.5F;
      for (auto& frame : expected_input) {
        frame.left *= bus_gain;
        frame.right *= bus_gain;
      }
      std::vector<engine::StereoFrame> expected;
      if (native_first) {
        const auto limited = ceiling_oracle(settings, expected_input);
        expected.resize(limited.size() + plugin_latency);
        for (std::size_t frame = plugin_latency; frame < expected.size(); ++frame)
          expected[frame] = {nonlinear(limited[frame - plugin_latency].left),
                             nonlinear(limited[frame - plugin_latency].right)};
      } else {
        std::vector<engine::StereoFrame> shaped(expected_input.size() + plugin_latency);
        for (std::size_t frame = plugin_latency; frame < shaped.size(); ++frame)
          shaped[frame] = {nonlinear(expected_input[frame - plugin_latency].left),
                           nonlinear(expected_input[frame - plugin_latency].right)};
        expected = ceiling_oracle(settings, shaped);
      }
      for (std::size_t frame = 0; frame < 135; ++frame) {
        const float dry = frame == 63 ? 0.25F : frame == 64 ? 0.125F : 0.0F;
        const float direct = bus_kind == project::TrackKind::return_bus ? source[frame].left : 0.0F;
        near(sample(bytes, frame), direct + dry + expected[frame + shift].left);
        require(std::isfinite(sample(bytes, frame)), "mixed bus emitted non-finite audio");
      }
      const auto wet_path = directory.path / "mixed-bus-source-stem.wav";
      const auto dry_path = directory.path / "mixed-bus-dry-stem.wav";
      REQUIRE_EQ(take(render(mixed, wet_path, id<project::TrackId>(2))),
                 exporting::PublishOutcome::durable, "mixed bus source stems did not publish");
      REQUIRE_EQ(take(render(mixed, dry_path, id<project::TrackId>(9))),
                 exporting::PublishOutcome::durable, "mixed bus source stems did not publish");
      const auto wet = read_file(wet_path);
      const auto dry = read_file(dry_path);
      REQUIRE_EQ(wet.size(), bytes.size(), "mixed bus stems lost common PDC duration");
      REQUIRE_EQ(dry.size(), bytes.size(), "mixed bus stems lost common PDC duration");
      for (std::size_t frame = 0; frame < 135; ++frame)
        near(sample(bytes, frame), sample(wet, frame) + sample(dry, frame));
      const auto repeat = directory.path / "mixed-bus-repeat.wav";
      REQUIRE_EQ(take(render(mixed, repeat)), exporting::PublishOutcome::durable,
                 "mixed bus repeat render changed bytes");
      REQUIRE_EQ(read_file(repeat), bytes, "mixed bus repeat render changed bytes");
      const auto rejected = directory.path / "mixed-bus-rejected.wav";
      auto third = mixed;
      third.tracks[1].effects.push_back(
          {id<project::InsertSlotId>(3501), true, test_insert::flat_shape()});
      REQUIRE_EQ(take(render(third, rejected)), exporting::PublishOutcome::durable,
                 "a mixed bus with a third stage did not export");
      fs::remove(rejected);
      auto bypass = mixed;
      bypass.tracks[1].effects[0].enabled = false;
      REQUIRE_EQ(take(render(bypass, rejected)), exporting::PublishOutcome::durable,
                 "a mixed bus with a bypassed stage did not export");
      fs::remove(rejected);
      auto stale = mixed;
      std::get<project::Vst3PluginInstance>(stale.tracks[1].effects[native_first ? 1 : 0].processor)
          .binary_sha256.assign(64, '0');
      REQUIRE_TRUE(!render(stale, rejected), "mixed bus accepted stale VST3 binary identity");
      REQUIRE_TRUE(!fs::exists(rejected), "mixed bus accepted stale VST3 binary identity");
      auto invalid = mixed;
      auto& invalid_slot = invalid.tracks[1].effects[native_first ? 0 : 1].processor;
      invalid_slot = test_insert::with_raw_value(std::get<project::NativeEffectPatch>(invalid_slot),
                                                 "ceiling.lookahead", 11.0);
      REQUIRE_TRUE(!render(invalid, path), "mixed bus invalid settings replaced durable master");
      REQUIRE_EQ(read_file(path), bytes, "mixed bus invalid settings replaced durable master");
      auto tail = mixed;
      auto& tail_plugin = std::get<project::Vst3PluginInstance>(
          tail.tracks[1].effects[native_first ? 1 : 0].processor);
      tail_plugin.bundle_path = tail_scan.binary.canonical_bundle.string();
      tail_plugin.binary_sha256 = tail_scan.binary.sha256;
      const auto& klass = tail_scan.metadata.classes.front();
      tail_plugin.class_id = klass.id;
      tail_plugin.vendor = klass.vendor;
      tail_plugin.name = klass.name;
      tail_plugin.version = klass.version;
      const float tail_gain = 0.5F;
      std::memcpy(tail_plugin.component_state.data(), &tail_gain, sizeof(tail_gain));
      auto& last_clip = tail.tracks[0].clips.back();
      last_clip.source_range = {{2}, {3}};
      last_clip.placement = project::ElapsedInterval{{134}, {135}};
      const auto tail_path = directory.path / "mixed-bus-tail.wav";
      auto authority = take(exporting::StoppedExportAuthority::acquire(tail, gate));
      REQUIRE_EQ(
          take(exporting::render_vst3_master(tail, saved.store, std::move(authority), scanner,
                                             exporting::MasterRenderOptions{tail_path})),
          exporting::PublishOutcome::durable, "mixed bus master plugin tail did not publish");
      const auto tail_bytes = read_file(tail_path);
      REQUIRE_EQ(tail_bytes.size(), 56 + 143 * 8,
                 "mixed bus plugin tail was truncated or lookahead was double-counted");
      REQUIRE_GT(std::fabs(sample(tail_bytes, 142)), 0.00001F,
                 "mixed bus plugin tail was truncated or lookahead was double-counted");
      const auto tail_stem_path = directory.path / "mixed-bus-tail-stem.wav";
      authority = take(exporting::StoppedExportAuthority::acquire(tail, gate));
      REQUIRE_EQ(take(exporting::render_vst3_stem(tail, saved.store, std::move(authority), scanner,
                                                  id<project::TrackId>(2), {tail_stem_path})),
                 exporting::PublishOutcome::durable, "mixed bus processed stem lost plugin tail");
      REQUIRE_GT(std::fabs(sample(read_file(tail_stem_path), 142)), 0.00001F,
                 "mixed bus processed stem lost plugin tail");
    }
  }
}

void export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_GAIN_BUNDLE);
  require(scanned.ok(), "effect fixture scan failed: " + scanned.diagnostic);
  auto saved = make_project(directory, gate, scanned);
  auto render = [&](const project::Snapshot& snapshot, const fs::path& target, bool stem) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    return stem ? exporting::render_vst3_stem(snapshot, saved.store, std::move(authority), scanner,
                                              id<project::TrackId>(2), {target})
                : exporting::render_vst3_master(snapshot, saved.store, std::move(authority),
                                                scanner, exporting::MasterRenderOptions{target});
  };
  const auto master_path = directory.path / "master.wav";
  REQUIRE_EQ(take(render(saved.snapshot, master_path, false)), exporting::PublishOutcome::durable,
             "effect master export did not publish");
  const auto master = read_file(master_path);
  auto offline_mode = saved.snapshot;
  const float offline_fixture_gain = -98.0F;
  auto& offline_plugin =
      std::get<project::Vst3PluginInstance>(offline_mode.tracks.front().effects.front().processor);
  std::memcpy(offline_plugin.component_state.data(), &offline_fixture_gain,
              sizeof(offline_fixture_gain));
  const auto offline_path = directory.path / "offline-mode.wav";
  REQUIRE_EQ(take(render(offline_mode, offline_path, false)), exporting::PublishOutcome::durable,
             "effect exporter did not request VST3 offline processing mode");
  near(sample(read_file(offline_path), 63), 0.4375F);
  REQUIRE_EQ(master.size(), 56 + 135 * 8, "effect tail/PDC duration was truncated");
  near(sample(master, 62), 0.0F);
  near(sample(master, 63), 0.4375F);
  near(sample(master, 64), 0.21875F);
  near(sample(master, 70), 0.0F);
  near(sample(master, 127), 0.1875F);
  near(sample(master, 128), 0.09375F);
  near(sample(master, 128, 1), 0.046875F);
  near(sample(master, 134), 0.0F);
  auto unprocessed = saved.snapshot;
  unprocessed.tracks.front().effects.clear();
  const auto native_path = directory.path / "native-onset.wav";
  auto native_authority = take(exporting::StoppedExportAuthority::acquire(unprocessed, gate));
  REQUIRE_EQ(take(exporting::render_wav_master(unprocessed, saved.store,
                                               std::move(native_authority), {native_path})),
             exporting::PublishOutcome::durable, "native comparison master did not publish");
  const auto native = read_file(native_path);
  auto first_audible = [](const auto& wav) {
    for (std::size_t frame = 0; frame < (wav.size() - 56) / 8; ++frame)
      if (std::fabs(sample(wav, frame)) > 0.00001F)
        return frame;
    throw std::runtime_error("comparison master has no audible frames");
  };
  REQUIRE_EQ(first_audible(native), 63,
             "effect export shifted the source onset relative to native export");
  REQUIRE_EQ(first_audible(master), first_audible(native),
             "effect export shifted the source onset relative to native export");
  auto long_latency = saved.snapshot;
  const float long_latency_fixture_gain = -96.0F;
  auto& long_latency_plugin =
      std::get<project::Vst3PluginInstance>(long_latency.tracks.front().effects.front().processor);
  std::memcpy(long_latency_plugin.component_state.data(), &long_latency_fixture_gain,
              sizeof(long_latency_fixture_gain));
  const auto long_latency_path = directory.path / "long-latency-stem.wav";
  REQUIRE_EQ(take(render(long_latency, long_latency_path, true)),
             exporting::PublishOutcome::durable,
             "effect export rejected a verified 96-frame plugin latency");
  const auto long_latency_stem = read_file(long_latency_path);
  REQUIRE_EQ(long_latency_stem.size(), native.size(),
             "96-frame plugin latency changed processed-stem timing or duration");
  REQUIRE_EQ(first_audible(long_latency_stem), first_audible(native),
             "96-frame plugin latency changed processed-stem timing or duration");
  near(sample(long_latency_stem, 63), 0.1875F);
  auto over_limit = saved.snapshot;
  const float over_limit_fixture_gain = -95.0F;
  auto& over_limit_plugin =
      std::get<project::Vst3PluginInstance>(over_limit.tracks.front().effects.front().processor);
  std::memcpy(over_limit_plugin.component_state.data(), &over_limit_fixture_gain,
              sizeof(over_limit_fixture_gain));
  const auto rejected_latency = render(over_limit, master_path, false);
  REQUIRE_TRUE(!rejected_latency, "over-budget plugin latency replaced a valid master");
  REQUIRE_EQ(rejected_latency.error().code, foundation::ErrorCode::resource_exhausted,
             "over-budget plugin latency replaced a valid master");
  REQUIRE_EQ(read_file(master_path), master, "over-budget plugin latency replaced a valid master");
  const auto stem_path = directory.path / "stem.wav";
  REQUIRE_EQ(take(render(saved.snapshot, stem_path, true)), exporting::PublishOutcome::durable,
             "processed wet stem did not publish");
  const auto wet = read_file(stem_path);
  const auto dry_path = directory.path / "dry.wav";
  auto dry_authority = take(exporting::StoppedExportAuthority::acquire(saved.snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_stem(saved.snapshot, saved.store, std::move(dry_authority),
                                              scanner, id<project::TrackId>(9), {dry_path})),
             exporting::PublishOutcome::durable, "processed dry stem did not publish");
  const auto dry = read_file(dry_path);
  REQUIRE_EQ(wet.size(), master.size(), "processed stems lost common output alignment");
  REQUIRE_EQ(dry.size(), master.size(), "processed stems lost common output alignment");
  near(sample(wet, 63), 0.1875F);
  near(sample(wet, 70), 0.0F);
  near(sample(dry, 63), 0.25F);
  near(sample(dry, 70), 0.0F);
  for (std::size_t frame = 0; frame < 135; ++frame)
    for (std::size_t channel = 0; channel < 2; ++channel)
      near(sample(master, frame, channel),
           sample(wet, frame, channel) + sample(dry, frame, channel));
  auto automated = saved.snapshot;
  ++automated.revision;
  const project::AutomationDomain send_domain{project::AutomationRepresentation::physical,
                                              "mixer.send_gain",
                                              "linear_gain",
                                              0.0,
                                              4.0,
                                              0.5,
                                              false,
                                              0,
                                              project::AutomationScale::decibel};
  automated.tracks.front().automation.push_back({id<project::AutomationLaneId>(20),
                                                 project::SendGainTarget{id<project::SendId>(7)},
                                                 send_domain,
                                                 project::AutomationTimebase::elapsed,
                                                 {{id<project::AutomationPointId>(21), 0, 0.25}},
                                                 1});
  automated.tracks.front().clips.front().automation.push_back(
      {id<project::AutomationLaneId>(22),
       project::SendGainTarget{id<project::SendId>(7)},
       send_domain,
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(23), 0, 1.0}},
       1});
  const auto automated_path = directory.path / "automated.wav";
  REQUIRE_EQ(take(render(automated, automated_path, false)), exporting::PublishOutcome::durable,
             "effect export rejected saved send automation");
  const auto automated_wav = read_file(automated_path);
  near(sample(automated_wav, 63), 0.5F);
  near(sample(automated_wav, 127), 0.15625F);
  const auto repeat_path = directory.path / "repeat.wav";
  REQUIRE_EQ(take(render(saved.snapshot, repeat_path, false)), exporting::PublishOutcome::durable,
             "effect export repeat render changed samples");
  REQUIRE_EQ(read_file(repeat_path), master, "effect export repeat render changed samples");
  auto musical = saved.snapshot;
  musical.tracks.front().clips.front().placement =
      project::MusicalInterval{{2'520'000}, {2'840'000}};
  musical.tracks.front().clips.back().placement =
      project::MusicalInterval{{5'080'000}, {5'400'000}};
  musical.tracks.back().clips.front().placement =
      project::MusicalInterval{{2'520'000}, {2'840'000}};
  const auto musical_path = directory.path / "musical.wav";
  REQUIRE_EQ(take(render(musical, musical_path, false)), exporting::PublishOutcome::durable,
             "musical and elapsed clip placement produced different effect timing");
  REQUIRE_EQ(read_file(musical_path), master,
             "musical and elapsed clip placement produced different effect timing");
  auto gapped = saved.snapshot;
  gapped.tracks.front().clips.back().placement = project::ElapsedInterval{{260}, {268}};
  const auto gapped_path = directory.path / "gapped.wav";
  REQUIRE_EQ(take(render(gapped, gapped_path, false)), exporting::PublishOutcome::durable,
             "effect export could not advance across a silent clip gap");
  const auto gapped_wav = read_file(gapped_path);
  near(sample(gapped_wav, 128), 0.0F);
  near(sample(gapped_wav, 260), 0.1875F);

  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "positive-tail effect fixture scan failed: " + tail_scan.diagnostic);
  auto tail_snapshot = saved.snapshot;
  auto& tail_audio = tail_snapshot.tracks.front();
  tail_audio.clips.erase(tail_audio.clips.begin() + 1, tail_audio.clips.end());
  tail_audio.clips.front().source_range = {{2}, {3}};
  tail_audio.clips.front().placement = project::ElapsedInterval{{127}, {128}};
  auto& tail_plugin = std::get<project::Vst3PluginInstance>(tail_audio.effects.front().processor);
  tail_plugin.bundle_path = tail_scan.binary.canonical_bundle.string();
  tail_plugin.binary_sha256 = tail_scan.binary.sha256;
  const auto tail_path = directory.path / "tail.wav";
  auto tail_authority = take(exporting::StoppedExportAuthority::acquire(tail_snapshot, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_master(tail_snapshot, saved.store, std::move(tail_authority),
                                         scanner, exporting::MasterRenderOptions{tail_path})),
      exporting::PublishOutcome::durable, "positive-tail effect master did not publish");
  const auto tail_wav = read_file(tail_path);
  REQUIRE_EQ(tail_wav.size(), 56 + 136 * 8, "positive effect tail was truncated");
  near(sample(tail_wav, 127), 0.0F);
  near(sample(tail_wav, 134), 0.0F);
  near(sample(tail_wav, 135), 0.1875F);
  const auto tail_stem_path = directory.path / "tail-stem.wav";
  tail_authority = take(exporting::StoppedExportAuthority::acquire(tail_snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_stem(tail_snapshot, saved.store, std::move(tail_authority),
                                              scanner, id<project::TrackId>(2), {tail_stem_path})),
             exporting::PublishOutcome::durable, "processed positive-tail stem was truncated");
  REQUIRE_EQ(sample(read_file(tail_stem_path), 135), 0.1875F,
             "processed positive-tail stem was truncated");

  const auto rejected = directory.path / "rejected.wav";
  auto reject = [&](const project::Snapshot& candidate) {
    REQUIRE_TRUE(!render(candidate, rejected, false),
                 "unsupported effect export published a master");
    REQUIRE_TRUE(!fs::exists(rejected), "unsupported effect export published a master");
  };
  auto wrong = saved.snapshot;
  wrong.tracks.front().effects.front().enabled = false;
  const auto bypassed_path = directory.path / "bypassed.wav";
  const auto bypassed_stem_path = directory.path / "bypassed-stem.wav";
  REQUIRE_EQ(take(render(wrong, bypassed_path, false)), exporting::PublishOutcome::durable,
             "bypassed VST3 master did not equal independently rendered dry master");
  REQUIRE_EQ(read_file(bypassed_path), native,
             "bypassed VST3 master did not equal independently rendered dry master");
  REQUIRE_EQ(take(render(wrong, bypassed_stem_path, true)), exporting::PublishOutcome::durable,
             "bypassed VST3 stem did not publish");
  const auto bypassed_stem = read_file(bypassed_stem_path);
  REQUIRE_EQ(bypassed_stem.size(), native.size(), "bypassed stem duration changed");
  near(sample(bypassed_stem, 62), 0.0F);
  near(sample(bypassed_stem, 63), 0.375F); // 0.25 source through main plus 0.5 send.
  near(sample(bypassed_stem, 64), 0.1875F);
  wrong = saved.snapshot;
  wrong.tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::step}, {{2'560'000}, 96.0, timeline::TempoShape::step}},
      2));
  const auto stepped_master = directory.path / "stepped-master.wav";
  const auto stepped_stem = directory.path / "stepped-stem.wav";
  REQUIRE_EQ(take(render(wrong, stepped_master, false)), exporting::PublishOutcome::durable,
             "stepped tempo changed effect master or processed-stem audio alignment");
  REQUIRE_EQ(read_file(stepped_master), master,
             "stepped tempo changed effect master or processed-stem audio alignment");
  REQUIRE_EQ(take(render(wrong, stepped_stem, true)), exporting::PublishOutcome::durable,
             "stepped tempo changed effect master or processed-stem audio alignment");
  REQUIRE_EQ(read_file(stepped_stem), wet,
             "stepped tempo changed effect master or processed-stem audio alignment");
  wrong.tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::linear_bpm},
       {{timeline::ticks_per_quarter}, 96.0, timeline::TempoShape::step}},
      3));
  const auto gradual_master = directory.path / "gradual-master.wav";
  const auto gradual_stem = directory.path / "gradual-stem.wav";
  REQUIRE_EQ(take(render(wrong, gradual_master, false)), exporting::PublishOutcome::durable,
             "gradual tempo changed effect master or processed-stem audio alignment");
  REQUIRE_EQ(read_file(gradual_master), master,
             "gradual tempo changed effect master or processed-stem audio alignment");
  REQUIRE_EQ(take(render(wrong, gradual_stem, true)), exporting::PublishOutcome::durable,
             "gradual tempo changed effect master or processed-stem audio alignment");
  REQUIRE_EQ(read_file(gradual_stem), wet,
             "gradual tempo changed effect master or processed-stem audio alignment");
  // Without its plugin the project renders as the native master does: one
  // renderer serves both (ADR-0090).
  REQUIRE_EQ(take(render(unprocessed, rejected, false)), exporting::PublishOutcome::durable,
             "a project without VST3 stages did not export");
  REQUIRE_EQ(read_file(rejected), native, "a project without VST3 stages differs from native");
  fs::remove(rejected);
  wrong = saved.snapshot;
  // A built-in effect in the plugin's place is an ordinary chain (ADR-0050).
  wrong.tracks.front().effects.front().id = id<project::InsertSlotId>(99);
  wrong.tracks.front().effects.front().processor = test_insert::flat_shape();
  REQUIRE_EQ(take(render(wrong, rejected, false)), exporting::PublishOutcome::durable,
             "a built-in effect in the plugin's place did not export");
  fs::remove(rejected);
  wrong = saved.snapshot;
  std::get<project::Vst3PluginInstance>(wrong.tracks.front().effects.front().processor)
      .binary_sha256.assign(64, '0');
  reject(wrong);
  // Any chain exports beside the plugin (ADR-0050): a master reverb, whose
  // tail lengthens the master, and built-in effects on the dry track and on
  // the return bus.
  wrong = saved.snapshot;
  test_insert::set_master(wrong, test_insert::expanse(0.2, 0.5));
  REQUIRE_EQ(take(render(wrong, rejected, false)), exporting::PublishOutcome::durable,
             "a master reverb beside a VST3 effect did not export");
  REQUIRE_GT(read_file(rejected).size(), master.size(), "the master reverb's tail was cut");
  fs::remove(rejected);
  for (const std::size_t track : {std::size_t{1}, std::size_t{2}}) {
    wrong = saved.snapshot;
    wrong.tracks[track].effects.push_back(
        {id<project::InsertSlotId>(99), true, test_insert::flat_shape()});
    REQUIRE_EQ(take(render(wrong, rejected, false)), exporting::PublishOutcome::durable,
               "a built-in effect on another path did not export");
    fs::remove(rejected);
  }
  wrong = saved.snapshot;
  const float failing_gain = 999.0F;
  auto& failing_plugin =
      std::get<project::Vst3PluginInstance>(wrong.tracks.front().effects.front().processor);
  std::memcpy(failing_plugin.component_state.data(), &failing_gain, sizeof(failing_gain));
  reject(wrong);
  auto bad_authority = take(exporting::StoppedExportAuthority::acquire(saved.snapshot, gate));
  REQUIRE_TRUE(!exporting::render_vst3_stem(saved.snapshot, saved.store, std::move(bad_authority),
                                            scanner, id<project::TrackId>(99), {rejected}),
               "unknown effect stem source published output");
  REQUIRE_TRUE(!fs::exists(rejected), "unknown effect stem source published output");

  const auto rejected_risk = directory.path / "rejected-risk.wav";
  auto render_options = [&](const project::Snapshot& candidate,
                            const exporting::MasterRenderOptions& options) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    return exporting::render_vst3_master(candidate, saved.store, std::move(authority), scanner,
                                         options);
  };
  auto reject_diagnostic = [&](const project::Snapshot& candidate,
                               const exporting::MasterRenderOptions& options,
                               std::string_view diagnostic) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    auto result = exporting::render_vst3_master(candidate, saved.store, std::move(authority),
                                                scanner, options);
    require(!result && result.error().message.find(diagnostic) != std::string::npos &&
                !fs::exists(options.target),
            "effect export expected '" + std::string(diagnostic) + "', got '" +
                (result ? std::string("success") : result.error().message) + "'");
  };
  auto stale = take(exporting::StoppedExportAuthority::acquire(saved.snapshot, gate));
  wrong = saved.snapshot;
  ++wrong.revision;
  auto stale_result = exporting::render_vst3_master(wrong, saved.store, std::move(stale), scanner,
                                                    exporting::MasterRenderOptions{rejected_risk});
  REQUIRE_TRUE(!stale_result, "stale stopped authority opened an output");
  REQUIRE_NE(stale_result.error().message.find("stale"), std::string::npos,
             "stale stopped authority opened an output");
  REQUIRE_TRUE(!fs::exists(rejected_risk), "stale stopped authority opened an output");
  reject_diagnostic(saved.snapshot, {rejected_risk, exporting::WavFormat::pcm16, 0, 22'050},
                    "sample rate is unsupported");
  // The shared renderer measures a VST3 master as it measures any other.
  exporting::RenderLoudnessReport measured;
  REQUIRE_EQ(take(render_options(saved.snapshot,
                                 {rejected_risk, exporting::WavFormat::float32, 0, 0, &measured})),
             exporting::PublishOutcome::durable, "a VST3 master with a loudness report failed");
  REQUIRE_EQ(measured.frames, (read_file(rejected_risk).size() - 56) / 8,
             "the loudness report measured the published master");
  fs::remove(rejected_risk);
  // Shapes the former effect exporter refused are ordinary projects now
  // (ADR-0050): a track without clips, a third audio track, an empty
  // instrument track and a built-in effect on another path.
  const auto exports = [&](const project::Snapshot& candidate, std::string_view what) {
    REQUIRE_EQ(take(render_options(candidate, {rejected_risk})), exporting::PublishOutcome::durable,
               std::string(what) + " did not export");
    fs::remove(rejected_risk);
  };
  wrong = saved.snapshot;
  wrong.tracks.back().clips.clear();
  exports(wrong, "a track without clips");
  wrong = saved.snapshot;
  wrong.tracks.emplace_back(id<project::TrackId>(31));
  exports(wrong, "a third audio track");
  wrong = saved.snapshot;
  wrong.tracks.emplace_back(id<project::TrackId>(32), std::vector<project::Clip>{},
                            project::TrackKind::instrument);
  exports(wrong, "an empty instrument track");
  wrong = saved.snapshot;
  wrong.tracks[1].effects.push_back(
      {test_insert::next_slot(wrong), true,
       test_insert::shape({native_dsp::EqBandKind::peaking, 1'000.0, 0.707, 3.0})});
  exports(wrong, "a second processed path");
  wrong = saved.snapshot;
  wrong.sources.front().encoding = project::SourceEncoding::flac;
  reject_diagnostic(wrong, {rejected_risk}, "managed source missing");
  wrong = saved.snapshot;
  wrong.tracks.front().clips.front().placement = project::ElapsedInterval{{63}, {70}};
  {
    // A stretched clip now renders through the shared warp path.
    const auto stretched = directory.path / "stretched-effect.wav";
    auto lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    REQUIRE_EQ(take(exporting::render_vst3_master(wrong, saved.store, std::move(lease), scanner,
                                                  exporting::MasterRenderOptions{stretched})),
               exporting::PublishOutcome::durable,
               "stretched clip did not render through the VST3 effect warp path");
    REQUIRE_TRUE(!fs::exists(rejected_risk),
                 "stretched clip did not render through the VST3 effect warp path");
  }
  wrong = saved.snapshot;
  std::get<project::Vst3PluginInstance>(wrong.tracks.front().effects.front().processor)
      .bundle_path = (directory.path / "absent.vst3").string();
  reject_diagnostic(wrong, {rejected_risk}, "fresh scan failed");
  wrong = saved.snapshot;
  auto& wrong_plugin =
      std::get<project::Vst3PluginInstance>(wrong.tracks.front().effects.front().processor);
  wrong_plugin.name = "Unexpected saved class";
  reject_diagnostic(wrong, {rejected_risk}, "class differs");
  wrong = saved.snapshot;
  auto& malformed_state =
      std::get<project::Vst3PluginInstance>(wrong.tracks.front().effects.front().processor);
  malformed_state.component_state = {1};
  reject_diagnostic(wrong, {rejected_risk}, "state restore failed");
  // Beside a saved state, a reference to a parameter the plugin lacks is
  // only a reference: the state carries the plugin's values, so the export
  // plays the state, as live playback does. (Static values for an unknown
  // parameter still fail; see static_parameter_export_checks.)
  wrong = saved.snapshot;
  std::get<project::Vst3PluginInstance>(wrong.tracks.front().effects.front().processor)
      .parameters.push_back({1, 0.5});
  REQUIRE_EQ(take(render_options(wrong, {rejected_risk})), exporting::PublishOutcome::durable,
             "an unknown saved parameter reference failed the export");
  REQUIRE_EQ(read_file(rejected_risk), master, "an unknown parameter reference changed the export");
  fs::remove(rejected_risk);
  reject_diagnostic(saved.snapshot, {directory.path / "absent" / "master.wav"}, "open");
  wrong = saved.snapshot;
  wrong.tracks.front().clips.front().source = id<project::SourceId>(99);
  reject_diagnostic(wrong, {rejected_risk}, "source record is missing");
  wrong = saved.snapshot;
  wrong.sources.front().sha256.assign(64, '0');
  reject_diagnostic(wrong, {rejected_risk}, "managed");
  const auto managed_asset =
      directory.path / "project" / "assets" / (saved.snapshot.sources.front().sha256 + ".wav");
  const auto intact_asset = read_file(managed_asset);
  auto damaged_asset = intact_asset;
  damaged_asset.back() ^= 1U;
  {
    std::ofstream output(managed_asset, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(damaged_asset.data()),
                 static_cast<std::streamsize>(damaged_asset.size()));
    require(output.good(), "cannot write damaged managed source fixture");
  }
  reject_diagnostic(saved.snapshot, {rejected_risk}, "hash mismatch");
  {
    std::ofstream output(managed_asset, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(intact_asset.data()),
                 static_cast<std::streamsize>(intact_asset.size()));
    require(output.good(), "cannot restore managed source fixture");
  }
  wrong = saved.snapshot;
  wrong.sources.front().channels = 1;
  reject_diagnostic(wrong, {rejected_risk}, "metadata");
  wrong = saved.snapshot;
  wrong.tracks.front().automation.push_back(
      {id<project::AutomationLaneId>(40),
       project::Vst3ParameterTarget{id<project::PluginInstanceId>(8), 1},
       {project::AutomationRepresentation::vst3_normalized, "plugin.parameter", "", 0.0, 1.0, 0.5,
        false, 0},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(41), 0, 0.5}},
       1});
  reject_diagnostic(wrong, {rejected_risk}, "automation target");

  REQUIRE_EQ(::setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", "no-instance", 1), 0,
             "cannot enable VST3 load failure fixture");
  reject_diagnostic(saved.snapshot, {rejected_risk}, "component creation failed");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL"), 0,
             "cannot clear VST3 load failure fixture");
  // A declared tail of any finite length rings out whole.
  REQUIRE_EQ(::setenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE", "overlong-tail", 1), 0,
             "cannot enable VST3 long-tail fixture");
  REQUIRE_EQ(take(render_options(tail_snapshot, {rejected_risk})),
             exporting::PublishOutcome::durable, "a long declared tail did not export");
  REQUIRE_GE((read_file(rejected_risk).size() - 56) / 8, overlong_tail_frames,
             "a long declared tail was cut");
  fs::remove(rejected_risk);
  REQUIRE_EQ(::unsetenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE"), 0,
             "cannot clear VST3 long-tail fixture");
  for (const auto& [mode, expected] : std::array<std::pair<const char*, const char*>, 3>{
           {{"infinite-tail", "tail is unbounded"},
            {"latency-over-budget", "compensation"},
            {"activate-fail", "activation failed"}}}) {
    REQUIRE_EQ(::setenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE", mode, 1), 0,
               "cannot enable VST3 tail/latency fixture");
    reject_diagnostic(tail_snapshot, {rejected_risk}, expected);
    REQUIRE_EQ(::unsetenv("OSCILLATE_TEST_EXPORT_EFFECT_MODE"), 0,
               "cannot clear VST3 tail/latency fixture");
  }
}
void static_parameter_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_CONNECTED_BUNDLE);
  require(scanned.ok(), "connected effect fixture scan failed");
  auto saved = make_project(directory, gate, scanned);
  auto& plugin = std::get<project::Vst3PluginInstance>(
      saved.snapshot.tracks.front().effects.front().processor);
  plugin.component_state.clear();
  plugin.parameters = {{101, 0.25}};
  auto render_snapshot = [&](const project::Snapshot& snapshot, const fs::path& target, bool stem) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    return stem ? exporting::render_vst3_stem(snapshot, saved.store, std::move(authority), scanner,
                                              id<project::TrackId>(2), {target})
                : exporting::render_vst3_master(snapshot, saved.store, std::move(authority),
                                                scanner, exporting::MasterRenderOptions{target});
  };
  auto render = [&](const fs::path& target, bool stem) {
    return render_snapshot(saved.snapshot, target, stem);
  };
  const auto master_path = directory.path / "static-master.wav";
  const auto wet_path = directory.path / "static-wet.wav";
  REQUIRE_EQ(take(render(master_path, false)), exporting::PublishOutcome::durable,
             "static VST3 parameter did not render master and stem");
  REQUIRE_EQ(take(render(wet_path, true)), exporting::PublishOutcome::durable,
             "static VST3 parameter did not render master and stem");
  const auto master = read_file(master_path);
  const auto wet = read_file(wet_path);
  std::size_t first_nonzero = wet.size();
  for (std::size_t frame = 0; frame < (wet.size() - 56) / 8; ++frame) {
    if (std::fabs(sample(wet, frame)) > 0.00001F) {
      first_nonzero = frame;
      break;
    }
  }
  REQUIRE_EQ(master.size(), wet.size(), "static VST3 effect did not contribute to processed stem");
  REQUIRE_NE(first_nonzero, wet.size(), "static VST3 effect did not contribute to processed stem");
  plugin.parameters = {{101, 0.5}};
  const auto louder_path = directory.path / "static-louder.wav";
  REQUIRE_EQ(take(render(louder_path, true)), exporting::PublishOutcome::durable,
             "second static VST3 value did not render");
  near(sample(read_file(louder_path), first_nonzero), sample(wet, first_nonzero) * 2.0F);
  plugin.parameters = {{101, 0.25}};
  const auto gain_lane = project::AutomationLane{
      id<project::AutomationLaneId>(31),
      project::Vst3ParameterTarget{plugin.id, 101},
      {project::AutomationRepresentation::vst3_normalized, "gain", "", 0.0, 1.0, 0.25, false, 0},
      project::AutomationTimebase::elapsed,
      {{id<project::AutomationPointId>(32), 63, 0.25, project::AutomationShape::hold, {}},
       {id<project::AutomationPointId>(33), 64, 0.5, project::AutomationShape::hold, {}}},
      1};
  saved.snapshot.tracks.front().automation.push_back(gain_lane);
  const auto automated_path = directory.path / "automated-wet.wav";
  const auto automated_master_path = directory.path / "automated-master.wav";
  REQUIRE_EQ(take(render(automated_path, true)), exporting::PublishOutcome::durable,
             "sample-offset VST3 automation did not render master and stem");
  REQUIRE_EQ(take(render(automated_master_path, false)), exporting::PublishOutcome::durable,
             "sample-offset VST3 automation did not render master and stem");
  const auto automated = read_file(automated_path);
  const auto automated_master = read_file(automated_master_path);
  near(sample(automated, first_nonzero), sample(wet, first_nonzero));
  near(sample(automated, first_nonzero + 1), sample(wet, first_nonzero + 1) * 2.0F);
  near(sample(automated_master, first_nonzero + 1) - sample(master, first_nonzero + 1),
       sample(automated, first_nonzero + 1) - sample(wet, first_nonzero + 1));
  saved.snapshot.tracks.front().automation.front().target =
      project::Vst3ParameterTarget{plugin.id, 999};
  const auto unsupported_automation_path = directory.path / "unknown-automation.wav";
  REQUIRE_TRUE(!render(unsupported_automation_path, false),
               "unsupported VST3 automation parameter published an output");
  REQUIRE_TRUE(!fs::exists(unsupported_automation_path),
               "unsupported VST3 automation parameter published an output");
  saved.snapshot.tracks.front().automation.clear();
  plugin.parameters = {{999, 0.5}};
  const auto rejected_path = directory.path / "static-unknown.wav";
  REQUIRE_TRUE(!render(rejected_path, false), "unknown saved VST3 parameter published an export");
  REQUIRE_TRUE(!fs::exists(rejected_path), "unknown saved VST3 parameter published an export");
}
} // namespace

// Integer WAV, converted-rate and encoded VST3 masters reuse the native
// output stage while the plugins still run at the 48 kHz project rate (the
// fixtures reject any other processing rate).
void output_format_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_GAIN_BUNDLE);
  require(scanned.ok(), "format fixture scan failed: " + scanned.diagnostic);
  auto saved = make_project(directory, gate, scanned);
  // A unity-gain effect behind PDC must reproduce the native mix exactly, so
  // native output at each rate is an oracle for the VST3 output stage.
  auto unity = saved.snapshot;
  const float unity_gain = 1.0F;
  std::memcpy(std::get<project::Vst3PluginInstance>(unity.tracks.front().effects.front().processor)
                  .component_state.data(),
              &unity_gain, sizeof(unity_gain));
  auto native_snapshot = saved.snapshot;
  native_snapshot.tracks.front().effects.clear();
  auto vst3 = [&](const project::Snapshot& snapshot,
                  const exporting::MasterOutputOptions& options) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    return exporting::render_vst3_master(snapshot, saved.store, std::move(authority), scanner,
                                         options);
  };
  auto native = [&](const fs::path& target, std::uint32_t rate) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(native_snapshot, gate));
    REQUIRE_EQ(take(exporting::render_wav_master(native_snapshot, saved.store, std::move(authority),
                                                 {target, exporting::WavFormat::float32, 0, rate})),
               exporting::PublishOutcome::durable, "native format oracle did not publish");
    return format_oracle::decode_wav(target);
  };
  auto close_to = [](const format_oracle::Decoded& actual, const format_oracle::Decoded& expected,
                     double tolerance, const std::string& what) {
    require(actual.sample_rate == expected.sample_rate && actual.frames == expected.frames,
            what + " rate or duration differs");
    for (std::size_t index = 0; index < actual.samples.size(); ++index)
      REQUIRE_LE(std::fabs(static_cast<double>(actual.samples[index]) - expected.samples[index]),
                 tolerance, what + " sample " + std::to_string(index) + " differs");
  };
  auto durable = [](foundation::Result<exporting::PublishOutcome> result, const std::string& what) {
    require(result && result.value() == exporting::PublishOutcome::durable,
            what + " did not publish: " + (result ? std::string() : result.error().message));
  };
  constexpr std::uint64_t project_frames = 135;
  const auto reference_path = directory.path / "reference.wav";
  durable(vst3(unity, exporting::MasterRenderOptions{reference_path}), "float reference");
  const auto reference = format_oracle::decode_wav(reference_path);
  REQUIRE_EQ(reference.format_tag, 3, "float reference format or PDC duration differs");
  REQUIRE_EQ(reference.bits, 32, "float reference format or PDC duration differs");
  REQUIRE_EQ(reference.sample_rate, 48'000, "float reference format or PDC duration differs");
  REQUIRE_EQ(reference.frames, project_frames, "float reference format or PDC duration differs");
  close_to(reference, native(directory.path / "native-48k.wav", 0), 1e-6,
           "unity VST3 master versus native");

  for (const auto& [format, bits] :
       {std::pair{exporting::WavFormat::pcm24, 24U}, std::pair{exporting::WavFormat::pcm16, 16U}}) {
    const auto path = directory.path / ("pcm" + std::to_string(bits) + ".wav");
    const exporting::MasterRenderOptions options{path, format, 11};
    durable(vst3(unity, options), "integer WAV");
    const auto decoded = format_oracle::decode_wav(path);
    REQUIRE_EQ(decoded.format_tag, 1, "integer WAV header differs");
    REQUIRE_EQ(decoded.bits, bits, "integer WAV header differs");
    close_to(decoded, reference, 2.0 / static_cast<double>(1U << (bits - 1U)), "dithered PCM");
    const auto first = format_oracle::file_bytes(path);
    durable(vst3(unity, options), "repeat integer WAV");
    REQUIRE_EQ(format_oracle::file_bytes(path), first, "seeded PCM dither is not deterministic");
  }

  // The tail fixture retains an eight-frame drain after its last input frame.
  const auto tail_scan = scanner.scan(VST3_TAIL_BUNDLE);
  require(tail_scan.ok(), "format tail fixture scan failed");
  auto tail = saved.snapshot;
  auto& tail_effect =
      std::get<project::Vst3PluginInstance>(tail.tracks.front().effects.front().processor);
  tail_effect.bundle_path = tail_scan.binary.canonical_bundle.string();
  tail_effect.binary_sha256 = tail_scan.binary.sha256;
  const auto& tail_class = tail_scan.metadata.classes.front();
  tail_effect.class_id = tail_class.id;
  tail_effect.vendor = tail_class.vendor;
  tail_effect.name = tail_class.name;
  tail_effect.version = tail_class.version;
  auto& last_clip = tail.tracks.front().clips.back();
  last_clip.source_range = {{2}, {3}};
  last_clip.placement = project::ElapsedInterval{{134}, {135}};
  constexpr std::uint64_t tail_frames = 143;
  const auto tail_path = directory.path / "tail-48k.wav";
  durable(vst3(tail, exporting::MasterRenderOptions{tail_path}), "tail master");
  const auto tail_reference = format_oracle::decode_wav(tail_path);
  REQUIRE_EQ(tail_reference.frames, tail_frames, "tail reference lost its final drained frame");
  REQUIRE_GT(std::fabs(tail_reference.samples[(tail_frames - 1) * 2]), 0.01F,
             "tail reference lost its final drained frame");

  for (const auto rate : {44'100U, 88'200U, 96'000U}) {
    const auto path = directory.path / ("rate-" + std::to_string(rate) + ".wav");
    const exporting::MasterRenderOptions options{path, exporting::WavFormat::float32, 0, rate};
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
    durable(vst3(unity, options), "converted float WAV");
    const auto converted = format_oracle::decode_wav(path);
    REQUIRE_EQ(converted.frames, format_oracle::converted_frames(project_frames, 48'000, rate),
               "converted VST3 duration differs");
    close_to(converted, native(directory.path / "native-rate.wav", rate), 1e-5,
             "converted VST3 master versus native conversion");
    const auto pcm_path = directory.path / ("rate-" + std::to_string(rate) + "-pcm24.wav");
    durable(
        vst3(unity, exporting::MasterRenderOptions{pcm_path, exporting::WavFormat::pcm24, 5, rate}),
        "converted PCM24");
    close_to(format_oracle::decode_wav(pcm_path), converted, 2.0 / 8'388'608.0, "converted PCM24");

    const auto tail_rate = directory.path / ("tail-" + std::to_string(rate) + ".wav");
    durable(vst3(tail,
                 exporting::MasterRenderOptions{tail_rate, exporting::WavFormat::float32, 0, rate}),
            "converted tail");
    const auto converted_tail = format_oracle::decode_wav(tail_rate);
    const auto expected_tail = format_oracle::converted_frames(tail_frames, 48'000, rate);
    REQUIRE_EQ(converted_tail.sample_rate, rate, "converted tail duration differs");
    REQUIRE_EQ(converted_tail.frames, expected_tail, "converted tail duration differs");
    // The drained impulse sits at 142/48000 s; its converted energy must
    // remain inside the final five output frames rather than be truncated.
    float peak = 0.0F;
    for (auto frame = expected_tail - 5; frame < expected_tail; ++frame)
      peak = std::max(peak, std::fabs(converted_tail.samples[frame * 2]));
    REQUIRE_GT(peak, 0.01F, "converted VST3 tail was truncated");
#else
    require(!vst3(unity, options) && !fs::exists(path),
            "conversion without the resampler build published output");
#endif
  }

#if OSCILLATE_HAVE_CODEC
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  constexpr std::array encoded_rates{48'000U, 44'100U, 96'000U};
#else
  constexpr std::array encoded_rates{48'000U};
#endif
  for (const auto rate : encoded_rates) {
    const auto float_path = directory.path / "encoded-reference.wav";
    durable(vst3(unity, exporting::MasterRenderOptions{float_path, exporting::WavFormat::float32, 0,
                                                       rate}),
            "encoded float reference");
    const auto float_reference = format_oracle::decode_wav(float_path);
    for (const auto format :
         {exporting::EncodedMasterFormat::flac16, exporting::EncodedMasterFormat::flac24,
          exporting::EncodedMasterFormat::mp3}) {
      const bool mp3 = format == exporting::EncodedMasterFormat::mp3;
      const auto path = directory.path / (mp3 ? "master.mp3" : "master.flac");
      const exporting::EncodedMasterOptions options{path, format, 9, 0.5, rate};
      if (mp3 && rate > 48'000) {
        const auto prior = format_oracle::file_bytes(path);
        REQUIRE_TRUE(!vst3(unity, options), "MP3 above 48 kHz replaced the prior master");
        REQUIRE_EQ(format_oracle::file_bytes(path), prior,
                   "MP3 above 48 kHz replaced the prior master");
        continue;
      }
      durable(vst3(unity, options), "encoded master");
      const auto decoded = format_oracle::decode_encoded(path);
      const auto subtype = format == exporting::EncodedMasterFormat::flac16 ? SF_FORMAT_PCM_16
                           : format == exporting::EncodedMasterFormat::flac24
                               ? SF_FORMAT_PCM_24
                               : SF_FORMAT_MPEG_LAYER_III;
      REQUIRE_EQ(decoded.format_tag, ((mp3 ? SF_FORMAT_MPEG : SF_FORMAT_FLAC) >> 16),
                 "encoded container or bit depth differs");
      REQUIRE_EQ(decoded.bits, subtype, "encoded container or bit depth differs");
      if (mp3) {
        REQUIRE_EQ(decoded.sample_rate, rate, "MP3 rate or duration differs");
        REQUIRE_EQ(decoded.frames, float_reference.frames, "MP3 rate or duration differs");
        double squared = 0.0;
        for (std::size_t index = 0; index < decoded.samples.size(); ++index) {
          const double delta = decoded.samples[index] - float_reference.samples[index];
          squared += delta * delta;
        }
        REQUIRE_LT(std::sqrt(squared / static_cast<double>(decoded.samples.size())), 0.08,
                   "MP3 differs from the float reference");
      } else {
        close_to(decoded, float_reference,
                 format == exporting::EncodedMasterFormat::flac16 ? 2.0 / 32'768.0
                                                                  : 2.0 / 8'388'608.0,
                 "FLAC master");
      }
      const auto first = format_oracle::file_bytes(path);
      durable(vst3(unity, options), "repeat encoded master");
      REQUIRE_EQ(format_oracle::file_bytes(path), first,
                 "encoded VST3 master is not deterministic");
    }
  }
  // A late codec rejection and a mid-render plugin failure keep the prior file.
  const auto flac_target = directory.path / "master.flac";
  const auto prior_flac = format_oracle::file_bytes(flac_target);
  auto failing = unity;
  const float failing_gain = 999.0F;
  std::memcpy(
      std::get<project::Vst3PluginInstance>(failing.tracks.front().effects.front().processor)
          .component_state.data(),
      &failing_gain, sizeof(failing_gain));
  REQUIRE_TRUE(
      !vst3(failing,
            exporting::EncodedMasterOptions{flac_target, exporting::EncodedMasterFormat::flac24}),
      "failed VST3 FLAC render replaced the prior master");
  REQUIRE_EQ(format_oracle::file_bytes(flac_target), prior_flac,
             "failed VST3 FLAC render replaced the prior master");
#endif

  const auto pcm_target = directory.path / "pcm16.wav";
  const auto prior_pcm = format_oracle::file_bytes(pcm_target);
  auto failing_pcm = unity;
  const float fail_gain = 999.0F;
  std::memcpy(
      std::get<project::Vst3PluginInstance>(failing_pcm.tracks.front().effects.front().processor)
          .component_state.data(),
      &fail_gain, sizeof(fail_gain));
  REQUIRE_TRUE(
      !vst3(failing_pcm,
            exporting::MasterRenderOptions{pcm_target, exporting::WavFormat::pcm16, 3, 44'100}),
      "failed converted PCM16 render replaced the prior master");
  REQUIRE_EQ(format_oracle::file_bytes(pcm_target), prior_pcm,
             "failed converted PCM16 render replaced the prior master");
  REQUIRE_TRUE(!vst3(unity, exporting::MasterRenderOptions{pcm_target, exporting::WavFormat::pcm16,
                                                           3, 22'050}),
               "unsupported output rate replaced the prior master");
  REQUIRE_EQ(format_oracle::file_bytes(pcm_target), prior_pcm,
             "unsupported output rate replaced the prior master");
  require(!format_oracle::has_private_temporaries(directory.path),
          "format export left a private temporary file");
}

int main() {
  try {
    export_checks();
    output_format_export_checks();
    static_parameter_export_checks();
    serial_export_checks();
    sidechain_export_checks();
    three_serial_effect_export_checks();
    mixed_eq_vst3_export_checks();
    mixed_compressor_vst3_export_checks();
    mixed_limiter_vst3_export_checks();
    pure_bus_vst3_export_checks();
    mixed_bus_vst3_export_checks();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "VST3 effect export: %s\n", error.what());
    return 1;
  }
}
