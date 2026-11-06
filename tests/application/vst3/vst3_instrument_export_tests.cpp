#include "format_oracle.hpp"
#include "insert_fixtures.hpp"
#include "oscillate/application/vst3/instrument_export.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/synth/player.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

// Two float32 WAV byte images of the same length whose samples agree within
// `tolerance`.
[[nodiscard]] bool near_wav(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                            double tolerance) {
  if (a.size() != b.size() || a.size() < 56)
    return false;
  for (std::size_t at = 56; at + 4 <= a.size(); at += 4) {
    float left = 0.0F;
    float right = 0.0F;
    std::memcpy(&left, a.data() + at, 4);
    std::memcpy(&right, b.data() + at, 4);
    if (std::fabs(static_cast<double>(left) - right) > tolerance)
      return false;
  }
  return true;
}
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t value) { return take(T::from_bits({0, value})); }

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
      fs::temp_directory_path() / ("oscillate-vst3-export-" + std::to_string(::getpid()));
  Directory() {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~Directory() { fs::remove_all(path); }
};

std::vector<std::uint8_t> bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "WAV did not open");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
float sample(const std::vector<std::uint8_t>& wav, std::size_t frame) {
  REQUIRE_GE(wav.size(), 56 + (frame + 1) * 8, "WAV frame is missing");
  float value = 0.0F;
  std::memcpy(&value, wav.data() + 56 + frame * 8, sizeof(value));
  return value;
}
float right_sample(const std::vector<std::uint8_t>& wav, std::size_t frame) {
  REQUIRE_GE(wav.size(), 56 + (frame + 1) * 8, "WAV frame is missing");
  float value = 0.0F;
  std::memcpy(&value, wav.data() + 60 + frame * 8, sizeof(value));
  return value;
}
void near(float actual, float expected) {
  if (std::fabs(actual - expected) >= 0.00001F)
    throw std::runtime_error("WAV sample differs: actual=" + std::to_string(actual) +
                             " expected=" + std::to_string(expected));
}

project::Snapshot saved_project(Directory& directory, Gate& gate,
                                const plugins::ScanResult& scanned);

void mixed_native_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scanned.ok(), "mixed fixture scan failed");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot enable finite mixed fixture tail");
  for (const std::string_view engine : {"prism", "ember"}) {
    auto snapshot = saved_project(directory, gate, scanned);
    snapshot.tracks.front().sends.clear();
    snapshot.tracks.pop_back();
    auto native = snapshot.tracks.front();
    native.id = id<project::TrackId>(40);
    native.instrument_source = project::NativeInstrumentPatch{std::string(engine), 1, {}};
    native.midi_clips.front().id = id<project::ClipId>(41);
    native.midi_clips.front().notes.front().id = id<project::NoteId>(42);
    native.level = 0.5;
    native.pan = 0.5;
    snapshot.tracks.push_back(native);
    ++snapshot.revision;
    const auto root = directory.path / std::string(engine);
    auto writer = take(persistence::ProjectStore::open_writer(root, true));
    REQUIRE_EQ(take(writer.save(snapshot, persistence::SaveKind::explicit_save)).status,
               persistence::SaveStatus::committed, "mixed native/VST3 project did not save");
    auto store = take(persistence::ProjectStore::open_reader(root));
    const auto reopened = take(store.load_current()).snapshot;
    REQUIRE_EQ(reopened.tracks, snapshot.tracks,
               "mixed native/VST3 route identities changed on reopen");
    REQUIRE_EQ(
        std::get<project::NativeInstrumentPatch>(reopened.tracks.back().instrument_source).engine,
        engine, "mixed native/VST3 route identities changed on reopen");
    REQUIRE_EQ(test_insert::vst3_source(reopened.tracks.front()).id,
               id<project::PluginInstanceId>(5),
               "mixed native/VST3 route identities changed on reopen");
    const auto output = directory.path / (std::string(engine) + "-mixed.wav");
    auto lease = take(exporting::StoppedExportAuthority::acquire(reopened, gate));
    REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(reopened, std::move(lease),
                                                                 scanner, {output})),
               exporting::PublishOutcome::durable, "mixed native/VST3 master did not publish");
    const auto master = bytes(output);
    const auto native_path = directory.path / (std::string(engine) + "-native-stem.wav");
    lease = take(exporting::StoppedExportAuthority::acquire(reopened, gate));
    REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_stem(reopened, std::move(lease), scanner,
                                                               native.id, {native_path})),
               exporting::PublishOutcome::durable, "selected native source stem did not publish");
    const auto native_stem = bytes(native_path);
    const auto vst3_path = directory.path / (std::string(engine) + "-vst3-stem.wav");
    lease = take(exporting::StoppedExportAuthority::acquire(reopened, gate));
    REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_stem(
                   reopened, std::move(lease), scanner, reopened.tracks.front().id, {vst3_path})),
               exporting::PublishOutcome::durable, "selected VST3 source stem did not publish");
    const auto vst3_stem = bytes(vst3_path);
    REQUIRE_EQ(master.size(), native_stem.size(),
               "mixed source stems have unequal or truncated finite tails");
    REQUIRE_EQ(master.size(), vst3_stem.size(),
               "mixed source stems have unequal or truncated finite tails");
    REQUIRE_GT(master.size(), 56 + 264 * 8,
               "mixed source stems have unequal or truncated finite tails");
    auto native_only = reopened;
    native_only.tracks.erase(native_only.tracks.begin());
    ++native_only.revision;
    const auto native_reference_path =
        directory.path / (std::string(engine) + "-native-reference.wav");
    lease = take(exporting::StoppedExportAuthority::acquire(native_only, gate));
    REQUIRE_EQ(take(exporting::render_wav_master(native_only, store, std::move(lease),
                                                 {native_reference_path})),
               exporting::PublishOutcome::durable,
               "independent native synth reference did not publish");
    const auto native_reference = bytes(native_reference_path);
    auto vst3_only = reopened;
    vst3_only.tracks.pop_back();
    ++vst3_only.revision;
    const auto vst3_reference_path = directory.path / (std::string(engine) + "-vst3-reference.wav");
    lease = take(exporting::StoppedExportAuthority::acquire(vst3_only, gate));
    REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(vst3_only, std::move(lease),
                                                                 scanner, {vst3_reference_path})),
               exporting::PublishOutcome::durable, "independent VST3 reference did not publish");
    const auto vst3_reference = bytes(vst3_reference_path);
    const auto reference_sample = [](const auto& wav, std::size_t frame, bool right) {
      if (frame >= (wav.size() - 56) / 8)
        return 0.0F;
      return right ? right_sample(wav, frame) : sample(wav, frame);
    };
    bool native_sounded = false;
    for (std::size_t frame = 0; frame < (master.size() - 56) / 8; ++frame) {
      near(sample(master, frame), sample(native_stem, frame) + sample(vst3_stem, frame));
      near(right_sample(master, frame),
           right_sample(native_stem, frame) + right_sample(vst3_stem, frame));
      near(sample(native_stem, frame), reference_sample(native_reference, frame, false));
      near(right_sample(native_stem, frame), reference_sample(native_reference, frame, true));
      near(sample(vst3_stem, frame), reference_sample(vst3_reference, frame, false));
      near(right_sample(vst3_stem, frame), reference_sample(vst3_reference, frame, true));
      native_sounded = native_sounded || std::fabs(sample(native_stem, frame)) > 0.00001F;
    }
    REQUIRE_TRUE(native_sounded, "mixed export omitted a routed instrument source");
    REQUIRE_GT(std::fabs(sample(vst3_stem, 5)), 0.1F,
               "mixed export omitted a routed instrument source");
    auto wet = reopened;
    const auto reverb = test_insert::expanse(0.2, 0.4);
    test_insert::set_master(wet, reverb);
    ++wet.revision;
    REQUIRE_EQ(take(writer.save(wet, persistence::SaveKind::explicit_save)).status,
               persistence::SaveStatus::committed, "mixed master reverb did not save");
    auto wet_store = take(persistence::ProjectStore::open_reader(root));
    wet = take(wet_store.load_current()).snapshot;
    REQUIRE_EQ(wet.master_effects.size(), 1, "mixed master reverb changed on reopen");
    REQUIRE_TRUE(wet.master_effects.front().enabled, "mixed master reverb changed on reopen");
    REQUIRE_EQ(std::get<project::NativeEffectPatch>(wet.master_effects.front().processor), reverb,
               "mixed master reverb changed on reopen");
    const auto render_wet = [&](std::optional<project::TrackId> stem, const fs::path& target) {
      auto authority = take(exporting::StoppedExportAuthority::acquire(wet, gate));
      const auto result = stem ? exporting::render_vst3_instrument_wav_stem(
                                     wet, std::move(authority), scanner, *stem, {target})
                               : exporting::render_vst3_instrument_wav_master(
                                     wet, std::move(authority), scanner, {target});
      REQUIRE_EQ(take(std::move(result)), exporting::PublishOutcome::durable,
                 "mixed master reverb did not publish");
      return bytes(target);
    };
    const auto wet_output = directory.path / (std::string(engine) + "-wet-master.wav");
    const auto wet_master = render_wet(std::nullopt, wet_output);
    const auto wet_native =
        render_wet(native.id, directory.path / (std::string(engine) + "-wet-native.wav"));
    const auto wet_vst3 = render_wet(reopened.tracks.front().id,
                                     directory.path / (std::string(engine) + "-wet-vst3.wav"));
    // The reverb has zero latency and a bounded drain. Both stems retain the
    // same absolute timing as the master.
    const auto reverb_tail_frames = test_insert::tail_frames(reverb, 48'000);
    REQUIRE_EQ(wet_master.size(), master.size() + reverb_tail_frames * 8,
               "mixed reverb tail or selected stem extent is wrong");
    REQUIRE_EQ(wet_native.size(), wet_master.size(),
               "mixed reverb tail or selected stem extent is wrong");
    REQUIRE_EQ(wet_vst3.size(), wet_master.size(),
               "mixed reverb tail or selected stem extent is wrong");
    near(sample(wet_master, 4), 0.0F);
    REQUIRE_NE(wet_master, master, "mixed reverb did not process the master");
    for (std::size_t frame = 0; frame < (wet_master.size() - 56) / 8; ++frame) {
      near(sample(wet_master, frame), sample(wet_native, frame) + sample(wet_vst3, frame));
      near(right_sample(wet_master, frame),
           right_sample(wet_native, frame) + right_sample(wet_vst3, frame));
    }
    REQUIRE_EQ(render_wet(std::nullopt, directory.path / (std::string(engine) + "-wet-repeat.wav")),
               wet_master, "mixed master reverb repeat changed bytes");
    auto stale_wet = wet;
    auto& stale_hash = test_insert::vst3_source(stale_wet.tracks.front()).binary_sha256[0];
    stale_hash = stale_hash == '0' ? '1' : '0';
    lease = take(exporting::StoppedExportAuthority::acquire(stale_wet, gate));
    REQUIRE_TRUE(!exporting::render_vst3_instrument_wav_master(stale_wet, std::move(lease), scanner,
                                                               {wet_output}),
                 "stale VST3 identity replaced the mixed reverb master");
    REQUIRE_EQ(bytes(wet_output), wet_master,
               "stale VST3 identity replaced the mixed reverb master");
    auto extra_effect = wet;
    test_insert::set_master(
        extra_effect, test_insert::shape({native_dsp::EqBandKind::low_pass, 12'000.0, 0.707, 0.0}));
    lease = take(exporting::StoppedExportAuthority::acquire(extra_effect, gate));
    const auto extra_result = exporting::render_vst3_instrument_wav_master(
        extra_effect, std::move(lease), scanner, {wet_output});
    REQUIRE_TRUE(!extra_result, "second mixed master processor replaced a valid reverb render");
    REQUIRE_NE(extra_result.error().message.find("at most one built-in master effect"),
               std::string::npos, "second mixed master processor replaced a valid reverb render");
    REQUIRE_EQ(bytes(wet_output), wet_master,
               "second mixed master processor replaced a valid reverb render");
    const auto prior = master;
    auto wrong = reopened;
    auto& hash = test_insert::vst3_source(wrong.tracks.front()).binary_sha256[0];
    hash = hash == '0' ? '1' : '0';
    lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    const auto rejected =
        exporting::render_vst3_instrument_wav_master(wrong, std::move(lease), scanner, {output});
    REQUIRE_TRUE(!rejected, "changed VST3 binary replaced a valid mixed master");
    REQUIRE_NE(rejected.error().message.find("binary differs"), std::string::npos,
               "changed VST3 binary replaced a valid mixed master");
    REQUIRE_EQ(bytes(output), prior, "changed VST3 binary replaced a valid mixed master");
    wrong = reopened;
    test_insert::vst3_source(wrong.tracks.front()).bundle_path =
        (directory.path / "missing.vst3").string();
    lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    const auto missing =
        exporting::render_vst3_instrument_wav_master(wrong, std::move(lease), scanner, {output});
    REQUIRE_TRUE(!missing, "missing VST3 binary replaced a valid mixed master");
    REQUIRE_EQ(bytes(output), prior, "missing VST3 binary replaced a valid mixed master");
    wrong = reopened;
    test_insert::set_master(wrong, test_insert::ceiling({-6.0, 5.0, 70.0}));
    lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
    const auto expanded =
        exporting::render_vst3_instrument_wav_master(wrong, std::move(lease), scanner, {output});
    REQUIRE_TRUE(!expanded, "unqualified mixed effect graph replaced a valid master");
    REQUIRE_NE(expanded.error().message.find("master effect latency"), std::string::npos,
               "unqualified mixed effect graph replaced a valid master");
    REQUIRE_EQ(bytes(output), prior, "unqualified mixed effect graph replaced a valid master");
  }
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0,
             "cannot clear finite mixed fixture tail");
}

project::Snapshot saved_project(Directory& directory, Gate& gate,
                                const plugins::ScanResult& scanned) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  auto model = take(project::ProjectModel::create(id<project::ProjectId>(1), 48'000,
                                                  std::move(tempo), std::move(meter), gate));
  const auto& klass = scanned.metadata.classes.front();
  project::Vst3PluginInstance plugin{.id = id<project::PluginInstanceId>(5),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = scanned.binary.canonical_bundle.string(),
                                     .binary_sha256 = scanned.binary.sha256,
                                     .component_state = {},
                                     .parameters = {},
                                     .missing = true};
  const float level = 0.5F;
  plugin.component_state.resize(sizeof(level));
  std::memcpy(plugin.component_state.data(), &level, sizeof(level));
  plugin.controller_state = plugin.component_state;
  const project::MidiClip clip{id<project::ClipId>(4),
                               project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
                               {timeline::ticks_per_quarter},
                               false,
                               {{id<project::NoteId>(6), {200'000}, {10'240'000}, 60, 127}},
                               1};
  const auto track = id<project::TrackId>(2);
  const auto return_bus = id<project::TrackId>(3);
  (void)take(
      model.apply({project::AddTrack{track, project::TrackKind::instrument},
                   project::AddTrack{return_bus, project::TrackKind::return_bus},
                   project::AddMidiClip{track, clip}, project::InsertVst3Plugin{track, 0, plugin},
                   project::AddSendRoute{track, {id<project::SendId>(7), return_bus, 0.5}}}));
  auto writer = take(persistence::ProjectStore::open_writer(directory.path / "project", true));
  REQUIRE_EQ(take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "VST3 export project save failed");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  return take(reader.load_current()).snapshot;
}

void export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto initial = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(initial.ok(), "fixture scan failed");
  auto snapshot = saved_project(directory, gate, initial);
  require(test_insert::vst3_source(snapshot.tracks.front()).missing,
          "saved missing flag did not survive reopen");
  auto native_store = take(persistence::ProjectStore::open_reader(directory.path / "project"));
  const auto native_rejected = directory.path / "native-rejected.wav";
  auto native_candidate = snapshot;
  native_candidate.tracks.front().sends.clear();
  native_candidate.tracks.pop_back();
  auto native_lease = take(exporting::StoppedExportAuthority::acquire(native_candidate, gate));
  auto native_result = exporting::render_wav_master(native_candidate, native_store,
                                                    std::move(native_lease), {native_rejected});
  REQUIRE_TRUE(!native_result, "ordinary master silently omitted the saved VST3 generator");
  REQUIRE_NE(native_result.error().message.find("saved VST3 instrument"), std::string::npos,
             "ordinary master silently omitted the saved VST3 generator");
  REQUIRE_TRUE(!fs::exists(native_rejected),
               "ordinary master silently omitted the saved VST3 generator");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot enable finite tail fixture");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "require-offline", 1), 0,
             "cannot require offline instrument processing");
  const auto first = directory.path / "master.wav";
  auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(
                 snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {first})),
             exporting::PublishOutcome::durable, "VST3 master export failed");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
             "cannot clear offline instrument fixture mode");
  const auto master = bytes(first);
  // Shared MIDI compiler places note-on at sample 5 and note-off at 256.
  // Direct main route (0.5) and a half-level return sum to 0.75.
  near(sample(master, 4), 0.0F);
  near(sample(master, 5), 0.75F);
  near(sample(master, 256), 0.75F);
  near(sample(master, 263), 0.09375F);
  near(sample(master, 264), 0.0F);
  auto automated = snapshot;
  automated.tracks.front().automation.push_back(
      {id<project::AutomationLaneId>(70),
       project::Vst3ParameterTarget{id<project::PluginInstanceId>(5), 1},
       {project::AutomationRepresentation::vst3_normalized, "level", "", 0.0, 1.0, 0.5, false, 0},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(71), 0, 0.25, project::AutomationShape::linear},
        {id<project::AutomationPointId>(72), 16, 0.75}},
       1});
  const auto automated_path = directory.path / "automated-master.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(automated, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_instrument_wav_master(automated, std::move(authority), scanner,
                                                        VST3_INSTRUMENT_BUNDLE, {automated_path})),
      exporting::PublishOutcome::durable,
      "VST3 instrument automation was omitted from the master render");
  const auto automated_master = bytes(automated_path);
  near(sample(automated_master, 5), 0.609375F);
  near(sample(automated_master, 16), 1.125F);
  const auto automated_stem = directory.path / "automated-stem.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(automated, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_instrument_wav_stem(automated, std::move(authority), scanner,
                                                      VST3_INSTRUMENT_BUNDLE, {automated_stem})),
      exporting::PublishOutcome::durable,
      "VST3 instrument automation changed between master and processed stem");
  REQUIRE_EQ(bytes(automated_stem), automated_master,
             "VST3 instrument automation changed between master and processed stem");
  const auto stem_path = directory.path / "stem.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_stem(
                 snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {stem_path})),
             exporting::PublishOutcome::durable,
             "single-source processed stem differs from master");
  REQUIRE_EQ(bytes(stem_path), master, "single-source processed stem differs from master");
  const auto repeat = directory.path / "repeat.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(
                 snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {repeat})),
             exporting::PublishOutcome::durable, "repeated VST3 render differs");
  REQUIRE_EQ(bytes(repeat), master, "repeated VST3 render differs");

  auto own_reverb = snapshot;
  own_reverb.tracks.front().effects.push_back(
      {test_insert::next_slot(own_reverb), true, test_insert::expanse(0.2, 1.0)});
  const auto own_path = directory.path / "instrument-reverb.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(own_reverb, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(
                 own_reverb, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {own_path})),
             exporting::PublishOutcome::durable, "VST3 instrument-owned reverb did not publish");
  const auto own_render = bytes(own_path);
  const auto reverb_tail = test_insert::tail_frames(test_insert::expanse(0.2, 1.0), 48'000);
  REQUIRE_EQ(own_render.size(), 56 + (265 + reverb_tail) * 8,
             "instrument-owned reverb tail was truncated");
  const auto own_stem = directory.path / "instrument-reverb-stem.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(own_reverb, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_stem(
                 own_reverb, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {own_stem})),
             exporting::PublishOutcome::durable,
             "instrument-owned native reverb stem differs from the master");
  REQUIRE_EQ(bytes(own_stem), own_render,
             "instrument-owned native reverb stem differs from the master");
  own_reverb.tracks.front().effects.front().enabled = false;
  const auto own_bypass = directory.path / "instrument-reverb-bypassed.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(own_reverb, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(
                 own_reverb, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {own_bypass})),
             exporting::PublishOutcome::durable,
             "bypassed instrument-owned native effect changed dry export");
  REQUIRE_EQ(bytes(own_bypass), master,
             "bypassed instrument-owned native effect changed dry export");
  auto own_limiter = snapshot;
  own_limiter.tracks.front().effects.push_back(
      {test_insert::next_slot(own_limiter), true, test_insert::ceiling({0.0, 1.0, 100.0})});
  const auto limited_path = directory.path / "instrument-limiter.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(own_limiter, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_instrument_wav_master(own_limiter, std::move(authority), scanner,
                                                        VST3_INSTRUMENT_BUNDLE, {limited_path})),
      exporting::PublishOutcome::durable,
      "instrument-owned limiter shifted a below-ceiling export or changed its extent");
  REQUIRE_EQ(bytes(limited_path), master,
             "instrument-owned limiter shifted a below-ceiling export or changed its extent");

  auto wet_snapshot = snapshot;
  test_insert::set_master(wet_snapshot, test_insert::expanse(0.2, 1.0));
  const auto wet_path = directory.path / "wet-master.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(wet_snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(
                 wet_snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {wet_path})),
             exporting::PublishOutcome::durable, "VST3 instrument master reverb did not publish");
  const auto wet = bytes(wet_path);
  // The same Expanse on the master and on the track agree to float rounding;
  // only the buffers it runs on differ.
  require(wet.size() == 56 + (265 + reverb_tail) * 8 && near_wav(wet, own_render, 1e-6),
          "VST3 instrument master reverb tail extent is wrong");
  const auto wet_stem = directory.path / "wet-stem.wav";
  authority = take(exporting::StoppedExportAuthority::acquire(wet_snapshot, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_stem(
                 wet_snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {wet_stem})),
             exporting::PublishOutcome::durable,
             "VST3 instrument master reverb stem differs from the master");
  REQUIRE_EQ(bytes(wet_stem), wet, "VST3 instrument master reverb stem differs from the master");

  auto wrong = snapshot;
  auto& stale_hash = test_insert::vst3_source(wrong.tracks.front()).binary_sha256[0];
  stale_hash = stale_hash == '0' ? '1' : '0';
  const auto rejected = directory.path / "rejected.wav";
  auto reject = [&](const project::Snapshot& candidate, const fs::path& candidate_bundle,
                    const exporting::MasterRenderOptions& requested) {
    auto lease = take(exporting::StoppedExportAuthority::acquire(candidate, gate));
    REQUIRE_TRUE(!exporting::render_vst3_instrument_wav_master(candidate, std::move(lease), scanner,
                                                               candidate_bundle, requested),
                 "unsupported export published an output");
    REQUIRE_TRUE(!fs::exists(requested.target), "unsupported export published an output");
  };
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  auto stale = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  auto changed_revision = snapshot;
  ++changed_revision.revision;
  REQUIRE_TRUE(!exporting::render_vst3_instrument_wav_master(
                   changed_revision, std::move(stale), scanner, VST3_INSTRUMENT_BUNDLE, {rejected}),
               "stale stopped authority admitted a changed revision");
  REQUIRE_TRUE(!fs::exists(rejected), "stale stopped authority admitted a changed revision");
  reject(snapshot, VST3_INSTRUMENT_BUNDLE, {rejected, exporting::WavFormat::pcm16, 0, 22'050});
  wrong = snapshot;
  wrong.sample_rate = 1;
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  test_insert::set_master(
      wrong, test_insert::with_raw_value(test_insert::expanse(0.2, 1.0), "expanse.mix", 2.0));
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  wrong.tracks.back().midi_clips = wrong.tracks.front().midi_clips;
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  wrong.tracks.erase(wrong.tracks.begin());
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  reject(snapshot, directory.path / "missing.vst3", {rejected});
  wrong = snapshot;
  test_insert::vst3_source(wrong.tracks.front()).class_id = "00000000000000000000000000000000";
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  test_insert::vst3_source(wrong.tracks.front()).component_state = {255, 255, 255, 255};
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  test_insert::vst3_source(wrong.tracks.front()).controller_state =
      std::vector<std::uint8_t>{255, 255, 255, 255};
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  wrong.tracks.front().midi_clips.clear();
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  auto second_instrument = wrong.tracks.front();
  second_instrument.id = id<project::TrackId>(30);
  second_instrument.midi_clips.front().id = id<project::ClipId>(40);
  second_instrument.midi_clips.front().notes.front().id = id<project::NoteId>(41);
  second_instrument.midi_clips.front().notes.front().start = {400'000};
  second_instrument.midi_clips.front().notes.front().end = {10'440'000};
  second_instrument.sends.clear();
  second_instrument.level = 0.5;
  second_instrument.pan = 0.5;
  test_insert::vst3_source(second_instrument).id = id<project::PluginInstanceId>(31);
  const float second_level = 0.25F;
  auto& second_plugin = test_insert::vst3_source(second_instrument);
  std::memcpy(second_plugin.component_state.data(), &second_level, sizeof(second_level));
  second_plugin.controller_state = second_plugin.component_state;
  wrong.tracks.push_back(std::move(second_instrument));
  ++wrong.revision;
  const auto two_source_root = directory.path / "two-source-project";
  auto two_source_writer = take(persistence::ProjectStore::open_writer(two_source_root, true));
  REQUIRE_EQ(take(two_source_writer.save(wrong, persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "two VST3 sources did not save");
  auto two_source_reader = take(persistence::ProjectStore::open_reader(two_source_root));
  wrong = take(two_source_reader.load_current()).snapshot;
  REQUIRE_EQ(test_insert::vst3_source(wrong.tracks.front()).id, id<project::PluginInstanceId>(5),
             "two VST3 source identities changed on reopen");
  REQUIRE_EQ(test_insert::vst3_source(wrong.tracks.back()).id, id<project::PluginInstanceId>(31),
             "two VST3 source identities changed on reopen");
  const auto two_master_path = directory.path / "two-source-master.wav";
  auto second_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(wrong, std::move(second_lease),
                                                               scanner, {two_master_path})),
             exporting::PublishOutcome::durable, "two-source VST3 master did not publish");
  const auto two_master = bytes(two_master_path);
  REQUIRE_EQ(two_master.size(), 56 + 270 * 8, "two-source finite tail extent is wrong");
  near(sample(two_master, 5), 0.75F);
  near(sample(two_master, 10), 0.8125F);
  near(right_sample(two_master, 10), -0.875F);
  const auto first_stem_path = directory.path / "two-source-first-stem.wav";
  second_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_stem(
                 wrong, std::move(second_lease), scanner, VST3_INSTRUMENT_BUNDLE,
                 id<project::TrackId>(2), {first_stem_path})),
             exporting::PublishOutcome::durable,
             "first processed VST3 source stem did not publish");
  const auto first_stem = bytes(first_stem_path);
  near(sample(first_stem, 5), 0.75F);
  near(sample(first_stem, 10), 0.75F);
  const auto second_stem_path = directory.path / "two-source-second-stem.wav";
  second_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  REQUIRE_EQ(
      take(exporting::render_vst3_instrument_wav_stem(
          wrong, std::move(second_lease), scanner, id<project::TrackId>(30), {second_stem_path})),
      exporting::PublishOutcome::durable, "second processed VST3 source stem did not publish");
  const auto second_stem = bytes(second_stem_path);
  REQUIRE_EQ(second_stem.size(), two_master.size(), "two source stem lengths disagree");
  near(sample(second_stem, 5), 0.0F);
  near(sample(second_stem, 10), 0.0625F);
  near(right_sample(second_stem, 10), -0.125F);
  for (std::size_t frame = 0; frame < 270; ++frame)
    near(sample(two_master, frame), sample(first_stem, frame) + sample(second_stem, frame));
  auto two_wet = wrong;
  test_insert::set_master(two_wet, test_insert::expanse(0.2, 1.0));
  const auto two_wet_master_path = directory.path / "two-source-wet-master.wav";
  second_lease = take(exporting::StoppedExportAuthority::acquire(two_wet, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_master(two_wet, std::move(second_lease),
                                                               scanner, {two_wet_master_path})),
             exporting::PublishOutcome::durable, "two-source master reverb did not publish");
  const auto two_wet_master = bytes(two_wet_master_path);
  REQUIRE_EQ(two_wet_master.size(),
             56 + (270 + test_insert::tail_frames(test_insert::expanse(0.2, 1.0), 48'000)) * 8,
             "two-source master effect tail was truncated");
  const auto two_wet_stem_path = directory.path / "two-source-wet-stem.wav";
  second_lease = take(exporting::StoppedExportAuthority::acquire(two_wet, gate));
  REQUIRE_EQ(take(exporting::render_vst3_instrument_wav_stem(two_wet, std::move(second_lease),
                                                             scanner, id<project::TrackId>(30),
                                                             {two_wet_stem_path})),
             exporting::PublishOutcome::durable, "two-source processed stem omitted master reverb");
  const auto two_wet_stem = bytes(two_wet_stem_path);
  REQUIRE_EQ(two_wet_stem.size(), two_wet_master.size(),
             "processed stem truncated the shared master effect tail");
  second_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  const auto retained_stem = exporting::render_vst3_instrument_wav_stem(
      wrong, std::move(second_lease), scanner, VST3_INSTRUMENT_BUNDLE, {wet_stem});
  REQUIRE_TRUE(!retained_stem, "ambiguous stem request replaced an existing render");
  REQUIRE_NE(retained_stem.error().message.find("explicit stem track"), std::string::npos,
             "ambiguous stem request replaced an existing render");
  REQUIRE_EQ(bytes(wet_stem), wet, "ambiguous stem request replaced an existing render");
  auto unsupported_two = wrong;
  unsupported_two.tracks.back().effects.push_back(
      {test_insert::next_slot(unsupported_two), true, test_insert::expanse(0.2, 0.2)});
  second_lease = take(exporting::StoppedExportAuthority::acquire(unsupported_two, gate));
  const auto retained_master = exporting::render_vst3_instrument_wav_master(
      unsupported_two, std::move(second_lease), scanner, VST3_INSTRUMENT_BUNDLE, {first});
  require(!retained_master, "two-source insert path unexpectedly rendered");
  REQUIRE_NE(retained_master.error().message.find("one effect per source"), std::string::npos,
             retained_master.error().message);
  REQUIRE_EQ(bytes(first), master, "two-source insert refusal replaced an existing render");
  auto wrong_second = wrong;
  auto& second_hash = test_insert::vst3_source(wrong_second.tracks.back()).binary_sha256[0];
  second_hash = second_hash == '0' ? '1' : '0';
  second_lease = take(exporting::StoppedExportAuthority::acquire(wrong_second, gate));
  const auto mismatched = exporting::render_vst3_instrument_wav_master(
      wrong_second, std::move(second_lease), scanner, {first});
  REQUIRE_TRUE(!mismatched, "second source binary mismatch replaced an existing render");
  REQUIRE_NE(mismatched.error().message.find("binary differs"), std::string::npos,
             "second source binary mismatch replaced an existing render");
  REQUIRE_EQ(bytes(first), master, "second source binary mismatch replaced an existing render");
  second_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  const auto unknown_stem = exporting::render_vst3_instrument_wav_stem(
      wrong, std::move(second_lease), scanner, VST3_INSTRUMENT_BUNDLE, id<project::TrackId>(999),
      {wet_stem});
  REQUIRE_TRUE(!unknown_stem, "unknown second-source stem replaced an existing render");
  REQUIRE_EQ(unknown_stem.error().code, foundation::ErrorCode::invalid_argument,
             "unknown second-source stem replaced an existing render");
  REQUIRE_EQ(bytes(wet_stem), wet, "unknown second-source stem replaced an existing render");
  wrong = snapshot;
  auto master_plugin = test_insert::vst3_source(wrong.tracks.front());
  master_plugin.id = id<project::PluginInstanceId>(34);
  master_plugin.missing = false;
  wrong.master_effects.push_back({test_insert::next_slot(wrong), true, master_plugin});
  auto master_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  auto master_result = exporting::render_vst3_instrument_wav_master(
      wrong, std::move(master_lease), scanner, VST3_INSTRUMENT_BUNDLE, {rejected});
  REQUIRE_TRUE(!master_result, "unrendered VST3 master insert reached the VST3 writer");
  REQUIRE_NE(master_result.error().message.find("VST3 master insert"), std::string::npos,
             "unrendered VST3 master insert reached the VST3 writer");
  REQUIRE_TRUE(!fs::exists(rejected), "unrendered VST3 master insert reached the VST3 writer");
  wrong.master_effects.back().enabled = false;
  auto bypassed_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  auto bypassed_result = exporting::render_vst3_instrument_wav_master(
      wrong, std::move(bypassed_lease), scanner, VST3_INSTRUMENT_BUNDLE, {rejected});
  REQUIRE_TRUE(bypassed_result, "bypassed VST3 master slot changed the rendered instrument master");
  REQUIRE_EQ(bypassed_result.value(), exporting::PublishOutcome::durable,
             "bypassed VST3 master slot changed the rendered instrument master");
  REQUIRE_EQ(bytes(rejected), master,
             "bypassed VST3 master slot changed the rendered instrument master");
  fs::remove(rejected);
  wrong = snapshot;
  auto missing_plugin = test_insert::vst3_source(wrong.tracks.front());
  missing_plugin.id = id<project::PluginInstanceId>(35);
  missing_plugin.missing = true;
  test_insert::append_effect(wrong, wrong.tracks.front(), missing_plugin);
  auto missing_lease = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  auto missing_result = exporting::render_vst3_instrument_wav_master(
      wrong, std::move(missing_lease), scanner, VST3_INSTRUMENT_BUNDLE, {rejected});
  REQUIRE_TRUE(!missing_result,
               "missing saved effect lost its diagnostic or reached the VST3 writer");
  REQUIRE_NE(missing_result.error().message.find("VST3 audio effect is missing"), std::string::npos,
             "missing saved effect lost its diagnostic or reached the VST3 writer");
  REQUIRE_TRUE(!fs::exists(rejected),
               "missing saved effect lost its diagnostic or reached the VST3 writer");
  wrong = snapshot;
  wrong.tracks.emplace_back(id<project::TrackId>(20));
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  wrong = snapshot;
  wrong.tracks.front().main_output = wrong.tracks.front().id;
  reject(wrong, VST3_INSTRUMENT_BUNDLE, {rejected});
  reject(snapshot, VST3_INSTRUMENT_BUNDLE, {directory.path / "absent" / "master.wav"});
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "two-event-buses", 1), 0,
             "cannot enable incompatible bus fixture");
  reject(snapshot, VST3_INSTRUMENT_BUNDLE, {rejected});
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
             "cannot clear incompatible bus fixture");
  wrong = snapshot;
  test_insert::vst3_source(wrong.tracks.front()).bundle_path = "/missing/relocated.vst3";
  authority = take(exporting::StoppedExportAuthority::acquire(wrong, gate));
  REQUIRE_TRUE(!exporting::render_vst3_instrument_wav_master(wrong, std::move(authority), scanner,
                                                             VST3_INSTRUMENT_BUNDLE, {rejected}),
               "same-byte plugin at a different saved path bypassed explicit relink");
  REQUIRE_TRUE(!fs::exists(rejected),
               "same-byte plugin at a different saved path bypassed explicit relink");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "infinite", 1), 0,
             "cannot enable infinite tail fixture");
  authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_TRUE(!exporting::render_vst3_instrument_wav_master(
                   snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {rejected}),
               "infinite tail was silently truncated");
  REQUIRE_TRUE(!fs::exists(rejected), "infinite tail was silently truncated");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "huge", 1), 0,
             "cannot enable excessive tail fixture");
  authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_TRUE(!exporting::render_vst3_instrument_wav_master(
                   snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {rejected}),
               "oversized finite tail was silently truncated");
  REQUIRE_TRUE(!fs::exists(rejected), "oversized finite tail was silently truncated");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL"), 0, "cannot clear tail fixture");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable process fault fixture");
  authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_TRUE(!exporting::render_vst3_instrument_wav_master(
                   snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE, {rejected}),
               "plugin process failure published partial WAV");
  REQUIRE_TRUE(!fs::exists(rejected), "plugin process failure published partial WAV");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0, "cannot clear process fault fixture");
  REQUIRE_EQ(test_insert::vst3_source(snapshot.tracks.front()).binary_sha256, initial.binary.sha256,
             "offline render mutated saved plugin identity");
}

// The instrument still runs at the 48 kHz project rate (the fixture rejects
// any other rate); integer, converted and encoded targets share the native
// master output stage after its tail has been rendered.
void output_format_export_checks() {
  Directory directory;
  Gate gate;
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto initial = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(initial.ok(), "format fixture scan failed");
  const auto snapshot = saved_project(directory, gate, initial);
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1), 0,
             "cannot enable finite tail fixture");
  auto master = [&](const exporting::MasterOutputOptions& options) {
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    return exporting::render_vst3_instrument_master(snapshot, std::move(authority), scanner,
                                                    VST3_INSTRUMENT_BUNDLE, options);
  };
  auto durable = [](foundation::Result<exporting::PublishOutcome> result, const std::string& what) {
    require(result && result.value() == exporting::PublishOutcome::durable,
            what + " did not publish: " + (result ? std::string() : result.error().message));
  };
  auto close_to = [](const format_oracle::Decoded& actual, const format_oracle::Decoded& expected,
                     double tolerance, const std::string& what) {
    require(actual.sample_rate == expected.sample_rate && actual.frames == expected.frames,
            what + " rate or duration differs");
    for (std::size_t index = 0; index < actual.samples.size(); ++index)
      REQUIRE_LE(std::fabs(static_cast<double>(actual.samples[index]) - expected.samples[index]),
                 tolerance, what + " sample " + std::to_string(index) + " differs");
  };
  // Note-off at 256 plus the eight-frame reported tail: 265 project frames.
  constexpr std::uint64_t project_frames = 265;
  const auto reference_path = directory.path / "reference.wav";
  durable(master(exporting::MasterRenderOptions{reference_path}), "float reference");
  const auto reference = format_oracle::decode_wav(reference_path);
  REQUIRE_EQ(reference.frames, project_frames, "instrument float reference duration differs");
  REQUIRE_EQ(reference.sample_rate, 48'000, "instrument float reference duration differs");
  for (const auto& [format, bits] :
       {std::pair{exporting::WavFormat::pcm24, 24U}, std::pair{exporting::WavFormat::pcm16, 16U}}) {
    const auto path = directory.path / ("pcm" + std::to_string(bits) + ".wav");
    durable(master(exporting::MasterRenderOptions{path, format, 21}), "integer WAV");
    const auto decoded = format_oracle::decode_wav(path);
    REQUIRE_EQ(decoded.format_tag, 1, "integer WAV header differs");
    REQUIRE_EQ(decoded.bits, bits, "integer WAV header differs");
    close_to(decoded, reference, 2.0 / static_cast<double>(1U << (bits - 1U)), "dithered PCM");
  }

  for (const auto rate : {44'100U, 88'200U, 96'000U}) {
    const auto path = directory.path / ("rate-" + std::to_string(rate) + ".wav");
    const exporting::MasterRenderOptions options{path, exporting::WavFormat::float32, 0, rate};
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
    durable(master(options), "converted float WAV");
    const auto converted = format_oracle::decode_wav(path);
    const auto expected = format_oracle::converted_frames(project_frames, 48'000, rate);
    REQUIRE_EQ(converted.sample_rate, rate, "converted instrument duration differs");
    REQUIRE_EQ(converted.frames, expected, "converted instrument duration differs");
    auto at = [&](std::uint64_t project_frame) {
      return converted.samples[format_oracle::converted_frames(project_frame, 48'000, rate) * 2];
    };
    // Band-limited conversion preserves the sustained 0.75 level by time,
    // independent of the converter's coefficients.
    for (const std::uint64_t frame : {64U, 130U, 200U})
      REQUIRE_LT(std::fabs(at(frame) - 0.75F), 0.01F, "converted sustain level or timing differs");
    float tail_peak = 0.0F;
    for (std::uint64_t frame = 258; frame < 263; ++frame)
      tail_peak = std::max(tail_peak, std::fabs(at(frame)));
    REQUIRE_GT(tail_peak, 0.05F, "converted instrument release tail was truncated");
    const auto stem_path = directory.path / ("stem-" + std::to_string(rate) + ".wav");
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    durable(exporting::render_vst3_instrument_wav_stem(
                snapshot, std::move(authority), scanner, VST3_INSTRUMENT_BUNDLE,
                {stem_path, exporting::WavFormat::float32, 0, rate}),
            "converted processed stem");
    REQUIRE_EQ(format_oracle::file_bytes(stem_path), format_oracle::file_bytes(path),
               "converted processed stem differs from its master");
#else
    require(!master(options) && !fs::exists(path),
            "conversion without the resampler build published output");
#endif
  }

#if OSCILLATE_HAVE_CODEC
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  constexpr std::array encoded_rates{48'000U, 96'000U};
#else
  constexpr std::array encoded_rates{48'000U};
#endif
  for (const auto rate : encoded_rates) {
    const auto float_path = directory.path / "encoded-reference.wav";
    durable(
        master(exporting::MasterRenderOptions{float_path, exporting::WavFormat::float32, 0, rate}),
        "encoded float reference");
    const auto float_reference = format_oracle::decode_wav(float_path);
    for (const auto format :
         {exporting::EncodedMasterFormat::flac16, exporting::EncodedMasterFormat::flac24}) {
      const auto path = directory.path / "master.flac";
      durable(master(exporting::EncodedMasterOptions{path, format, 4, 0.5, rate}), "FLAC master");
      const auto decoded = format_oracle::decode_encoded(path);
      REQUIRE_EQ(decoded.format_tag, (SF_FORMAT_FLAC >> 16), "FLAC container or bit depth differs");
      REQUIRE_EQ(
          decoded.bits,
          (format == exporting::EncodedMasterFormat::flac16 ? SF_FORMAT_PCM_16 : SF_FORMAT_PCM_24),
          "FLAC container or bit depth differs");
      close_to(decoded, float_reference,
               format == exporting::EncodedMasterFormat::flac16 ? 2.0 / 32'768.0
                                                                : 2.0 / 8'388'608.0,
               "FLAC instrument master");
    }
  }
  const auto mp3_path = directory.path / "master.mp3";
  durable(master(exporting::EncodedMasterOptions{mp3_path, exporting::EncodedMasterFormat::mp3}),
          "MP3 master");
  const auto mp3 = format_oracle::decode_encoded(mp3_path);
  REQUIRE_EQ(mp3.format_tag, (SF_FORMAT_MPEG >> 16), "MP3 rate or duration differs");
  REQUIRE_EQ(mp3.sample_rate, 48'000, "MP3 rate or duration differs");
  REQUIRE_EQ(mp3.frames, project_frames, "MP3 rate or duration differs");
  const auto prior_mp3 = format_oracle::file_bytes(mp3_path);
  REQUIRE_TRUE(!master(exporting::EncodedMasterOptions{
                   mp3_path, exporting::EncodedMasterFormat::mp3, 0, 0.5, 96'000}),
               "rejected 96 kHz MP3 replaced the prior master");
  REQUIRE_EQ(format_oracle::file_bytes(mp3_path), prior_mp3,
             "rejected 96 kHz MP3 replaced the prior master");
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable process fault fixture");
  const auto flac_path = directory.path / "master.flac";
  const auto prior_flac = format_oracle::file_bytes(flac_path);
  REQUIRE_TRUE(
      !master(exporting::EncodedMasterOptions{flac_path, exporting::EncodedMasterFormat::flac24}),
      "failed instrument FLAC render replaced the prior master");
  REQUIRE_EQ(format_oracle::file_bytes(flac_path), prior_flac,
             "failed instrument FLAC render replaced the prior master");
#else
  REQUIRE_EQ(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
             "cannot enable process fault fixture");
#endif
  const auto pcm_path = directory.path / "pcm24.wav";
  const auto prior_pcm = format_oracle::file_bytes(pcm_path);
  REQUIRE_TRUE(
      !master(exporting::MasterRenderOptions{pcm_path, exporting::WavFormat::pcm24, 21, 44'100}),
      "failed converted PCM24 render replaced the prior master");
  REQUIRE_EQ(format_oracle::file_bytes(pcm_path), prior_pcm,
             "failed converted PCM24 render replaced the prior master");
  REQUIRE_EQ(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0, "cannot clear process fault fixture");
  require(!format_oracle::has_private_temporaries(directory.path),
          "format export left a private temporary file");
}
} // namespace

int main() {
  try {
    export_checks();
    output_format_export_checks();
    mixed_native_export_checks();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "VST3 instrument export: %s\n", error.what());
    return 1;
  }
}
