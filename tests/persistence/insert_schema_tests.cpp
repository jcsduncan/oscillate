#include "oscillate/persistence/store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"
#include "private.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::project;
using namespace oscillate::persistence;
using Json = nlohmann::json;
namespace fs = std::filesystem;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-insert-schema") {}
};

Vst3PluginInstance plugin(std::uint64_t number, bool missing = false) {
  return {
      id<PluginInstanceId>(number), std::string(32, 'a'), "Fixture",        "Processor",  "1.2",
      "/tmp/Fixture.vst3",          std::string(64, 'b'), {0, 1, 254, 255}, {{7, 0.625}}, missing};
}

Snapshot state() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  Snapshot saved{id<ProjectId>(1),
                 1,
                 48'000,
                 std::move(tempo),
                 std::move(meter),
                 {Track{id<TrackId>(10)},
                  Track{id<TrackId>(20), {}, TrackKind::instrument, plugin(103, true)},
                  Track{id<TrackId>(30), {}, TrackKind::instrument, plugin(104)}}};
  saved.tracks[0].effects = {{id<InsertSlotId>(1001), true, plugin(101, true)},
                             {id<InsertSlotId>(1002), true, plugin(102)}};
  saved.master_effects = {
      {id<InsertSlotId>(2001), true, NativeEffectPatch{"shape", 1, {{"shape.gain_1", 2.0}}, 7}},
      {id<InsertSlotId>(2002), true, NativeEffectPatch{"press", 1, {}, 8}},
      {id<InsertSlotId>(2003), true, NativeEffectPatch{"expanse", 1, {}, 9}},
      {id<InsertSlotId>(2004), true, NativeEffectPatch{"ceiling", 1, {}, 10}}};
  saved.next_insert_slot_serial = 15;
  return saved;
}

std::string manifest(const Snapshot& snapshot) {
  return detail::encode_manifest(snapshot, std::string(32, 'a'), std::nullopt,
                                 SaveKind::explicit_save);
}

void writer_round_trip_and_validation() {
  auto saved = state();
  const auto encoded = manifest(saved);
  const auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 47, "writer wrote retired fields or omitted allocator state");
  REQUIRE_TRUE(!json["project"].contains("master_eq"),
               "writer wrote retired fields or omitted allocator state");
  REQUIRE_TRUE(!json["project"]["tracks"][0].contains("vst3_plugins"),
               "writer wrote retired fields or omitted allocator state");
  REQUIRE_EQ(json["project"]["tracks"][0]["effects"][0]["enabled"], true,
             "writer wrote retired fields or omitted allocator state");
  REQUIRE_EQ(json["project"]["next_insert_slot_serial"], 15,
             "writer wrote retired fields or omitted allocator state");
  auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks, saved.tracks,
             "slot order, state, roles or allocator serial changed on round trip");
  REQUIRE_EQ(decoded.snapshot.master_effects, saved.master_effects,
             "slot order, state, roles or allocator serial changed on round trip");
  REQUIRE_EQ(decoded.snapshot.next_insert_slot_serial, 15,
             "slot order, state, roles or allocator serial changed on round trip");
  auto filtered = saved;
  std::get<project::NativeEffectPatch>(filtered.master_effects[1].processor).parameters = {
      {"press.detector_freq", 240.0}};
  const auto filtered_json = Json::parse(manifest(filtered));
  REQUIRE_EQ(take(detail::decode_manifest(filtered_json.dump())).snapshot.master_effects[1],
             filtered.master_effects[1],
             "a built-in effect's parameter did not survive save/reopen");
  auto malformed = json;
  malformed["project"]["tracks"][0]["effects"][1]["id"] =
      malformed["project"]["tracks"][0]["effects"][0]["id"];
  require(!detail::decode_manifest(malformed.dump()), "duplicate slot ID decoded");
  malformed = json;
  malformed["project"]["tracks"][0]["instrument_source"] =
      json["project"]["tracks"][1]["instrument_source"];
  require(!detail::decode_manifest(malformed.dump()), "audio path accepted instrument source");
  malformed = json;
  malformed["project"]["next_insert_slot_serial"] = 0;
  require(!detail::decode_manifest(malformed.dump()), "zero allocator serial decoded");
}

void controller_state_round_trip() {
  auto saved = state();
  auto& first = std::get<Vst3PluginInstance>(saved.tracks[0].effects[0].processor);
  auto& second = std::get<Vst3PluginInstance>(saved.tracks[0].effects[1].processor);
  auto& missing = std::get<Vst3PluginInstance>(saved.tracks[1].instrument_source);
  first.controller_state = std::vector<std::uint8_t>{9, 8, 7};
  second.controller_state = std::vector<std::uint8_t>{};
  missing.controller_state = std::vector<std::uint8_t>{6, 5};
  const auto encoded = manifest(saved);
  const auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 47, "controller chunks lost present, empty or absent state");
  REQUIRE_EQ(json["project"]["tracks"][0]["effects"][0]["payload"]["controller_state_base64"],
             "CQgH", "controller chunks lost present, empty or absent state");
  REQUIRE_EQ(json["project"]["tracks"][0]["effects"][1]["payload"]["controller_state_base64"], "",
             "controller chunks lost present, empty or absent state");
  REQUIRE_TRUE(
      json["project"]["tracks"][2]["instrument_source"]["payload"]["controller_state_base64"]
          .is_null(),
      "controller chunks lost present, empty or absent state");
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks, saved.tracks,
             "controller state or missing-plugin identity changed");
  REQUIRE_TRUE(missing.missing, "controller state or missing-plugin identity changed");
  REQUIRE_EQ(
      std::get<Vst3PluginInstance>(decoded.snapshot.tracks[1].instrument_source).controller_state,
      missing.controller_state, "controller state or missing-plugin identity changed");

  auto malformed = json;
  malformed["project"]["tracks"][0]["effects"][0]["payload"].erase("controller_state_base64");
  require(!detail::decode_manifest(malformed.dump()), "schema 1.31 accepted a missing state key");
  malformed = json;
  malformed["project"]["tracks"][0]["effects"][0]["payload"]["controller_state_base64"] = "A===";
  require(!detail::decode_manifest(malformed.dump()),
          "schema 1.31 accepted noncanonical controller state");
  malformed = json;
  malformed["project"]["tracks"][0]["effects"][0]["payload"]["controller_state_base64"] = 42;
  require(!detail::decode_manifest(malformed.dump()),
          "schema 1.31 accepted non-string controller state");
}

void sidechain_send_round_trips_with_target_identity() {
  auto saved = state();
  const auto destination = id<TrackId>(50);
  const auto compressor = id<InsertSlotId>(3001);
  saved.tracks.push_back(Track{destination});
  saved.tracks.back().effects.push_back({compressor, true, NativeEffectPatch{"press"}});
  const SendRoute route{id<SendId>(3002), destination, 0.75, SendPurpose::sidechain, compressor};
  saved.tracks.front().sends.push_back(route);
  const auto encoded = manifest(saved);
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks.front().sends, std::vector<SendRoute>{route},
             "sidechain send lost its kind, gain or target effect on reopen");
  auto malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0]["sends"][0]["purpose"] = "unknown";
  require(!detail::decode_manifest(malformed.dump()), "unknown sidechain purpose decoded");
  malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0]["sends"][0]["target_insert"] = nullptr;
  require(!detail::decode_manifest(malformed.dump()),
          "sidechain route without target effect identity decoded");
}

void write_ir_wav(const fs::path& path) {
  // Half, quarter and eighth of full scale.
  constexpr std::array<std::int16_t, 3> taps{16'384, 8'192, 4'096};
  oscillate::test::write_wav_pcm16(path, oscillate::test::sample_rate_48k,
                                   oscillate::test::mono_channels, taps);
}

void convolution_identity_round_trips_and_missing_asset_reopens() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto input = temp.path() / "ir.wav";
  write_ir_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "IR WAV fixture could not be opened");
  auto writer = take(ProjectStore::open_writer(root, true));
  auto receipt = take(writer.stage_wav(id<SourceId>(60), fd));
  ::close(fd);
  auto saved = state();
  const auto source = receipt.record();
  saved.sources.push_back(source);
  const ManagedAudioReference ir{source.id,          source.sha256,   source.byte_length,
                                 source.sample_rate, source.channels, source.frames};
  saved.master_effects.push_back({id<InsertSlotId>(2005), true, NativeConvolution{ir, 0.6, 7}});
  const auto encoded = manifest(saved);
  const auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 47,
             "new convolution manifest kind or schema was not written");
  REQUIRE_EQ(json["project"]["master_effects"].back()["kind"], "native_convolution",
             "new convolution manifest kind or schema was not written");
  auto malformed = json;
  malformed["project"]["master_effects"].back()["payload"]["ir"]["unknown"] = 1;
  require(!detail::decode_manifest(malformed.dump()), "unknown IR identity field was accepted");
  malformed = json;
  malformed["project"]["master_effects"].back()["payload"]["ir"]["sha256"] = std::string(64, 'b');
  require(!detail::decode_manifest(malformed.dump()), "mismatched IR identity was accepted");
  malformed = json;
  malformed["project"]["master_effects"].back()["payload"]["wet"] = 2.0;
  require(!detail::decode_manifest(malformed.dump()), "out-of-range IR wet level was accepted");
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  REQUIRE_OK(writer.save(saved, SaveKind::explicit_save, receipts));
  auto reopened = take(ProjectStore::open_reader(root));
  auto loaded = take(reopened.load_current());
  REQUIRE_EQ(loaded.snapshot.master_effects, saved.master_effects,
             "convolution IR settings or source identity changed on save/reopen");
  REQUIRE_EQ(loaded.snapshot.sources, saved.sources,
             "convolution IR settings or source identity changed on save/reopen");
  REQUIRE_TRUE(loaded.source_diagnostics.empty(),
               "convolution IR settings or source identity changed on save/reopen");
  fs::remove(root / "assets" / (source.sha256 + ".wav"));
  auto missing = take(reopened.load_current());
  REQUIRE_EQ(missing.snapshot.master_effects, saved.master_effects,
             "missing IR media discarded the saved insert identity");
  REQUIRE_EQ(missing.source_diagnostics.size(), 1,
             "missing IR media discarded the saved insert identity");
  REQUIRE_EQ(missing.source_diagnostics.front().id, source.id,
             "missing IR media discarded the saved insert identity");
}

} // namespace

int main() {
  try {
    writer_round_trip_and_validation();
    controller_state_round_trip();
    sidechain_send_round_trips_with_target_identity();
    convolution_identity_round_trips_and_missing_asset_reopens();
  } catch (const std::exception& error) {
    std::cerr << "insert schema: " << error.what() << '\n';
    return 1;
  }
  std::cout << "insert schema passed\n";
}
