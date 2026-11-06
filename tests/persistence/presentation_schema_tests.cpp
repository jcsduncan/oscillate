#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "private.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Track names and colours, clip colours and arrangement markers. None
// affects audio; all round-trip exactly.
namespace {
using namespace oscillate;
using namespace oscillate::project;
using namespace oscillate::persistence;
using Json = nlohmann::json;

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

constexpr std::int64_t quarter = timeline::ticks_per_quarter;

Snapshot snapshot() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  Snapshot state{id<ProjectId>(1), 2, 48'000, std::move(tempo), std::move(meter), {}, {}};
  Track bass{id<TrackId>(2), {}, TrackKind::instrument};
  bass.name = "Bass · sub";
  bass.color = 0x8F'A6'FF;
  MidiClip line{id<ClipId>(3), MusicalInterval{{0}, {4 * quarter}}, {4 * quarter}, false, {}};
  line.color = 0xF5'B1'4C;
  bass.midi_clips.push_back(std::move(line));
  state.tracks.push_back(std::move(bass));
  Track plain{id<TrackId>(4), {}, TrackKind::return_bus};
  state.tracks.push_back(std::move(plain));
  state.markers = {{id<MarkerId>(5), {0}, "Intro"}, {id<MarkerId>(6), {16 * quarter}, "Drop"}};
  state.key = MusicalKey{9, KeyMode::minor}; // A minor (schema 1.36)
  return state;
}

std::string encode(const Snapshot& state) {
  return detail::encode_manifest(state, std::string(32, 'a'), std::nullopt,
                                 SaveKind::explicit_save);
}

void presentation_round_trips() {
  const auto original = snapshot();
  const auto encoded = encode(original);
  const auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 47, "presentation saves in the current schema");
  REQUIRE_EQ(json["project"]["tracks"][0]["name"], "Bass · sub",
             "names, colours, markers and the key are written");
  REQUIRE_EQ(json["project"]["tracks"][0]["color"], 0x8F'A6'FF,
             "names, colours, markers and the key are written");
  REQUIRE_TRUE(json["project"]["tracks"][1]["color"].is_null(),
               "names, colours, markers and the key are written");
  REQUIRE_EQ(json["project"]["tracks"][0]["midi_clips"][0]["color"], 0xF5'B1'4C,
             "names, colours, markers and the key are written");
  REQUIRE_EQ(json["project"]["markers"].size(), 2,
             "names, colours, markers and the key are written");
  REQUIRE_EQ(json["project"]["key"]["tonic"], 9, "names, colours, markers and the key are written");
  REQUIRE_EQ(json["project"]["key"]["mode"], "minor",
             "names, colours, markers and the key are written");
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks, original.tracks,
             "names, colours, markers and the key survive the round trip");
  REQUIRE_EQ(decoded.snapshot.markers, original.markers,
             "names, colours, markers and the key survive the round trip");
  REQUIRE_EQ(decoded.snapshot.key, original.key,
             "names, colours, markers and the key survive the round trip");
  auto keyless = original;
  keyless.key.reset();
  const auto keyless_json = Json::parse(encode(keyless));
  REQUIRE_TRUE(keyless_json["project"]["key"].is_null(),
               "an unset key is written as null and reads back unset");
  REQUIRE_TRUE(!take(detail::decode_manifest(keyless_json.dump())).snapshot.key,
               "an unset key is written as null and reads back unset");
}

void invalid_presentation_is_rejected() {
  const auto json = Json::parse(encode(snapshot()));
  auto malformed = json;
  malformed["project"]["tracks"][0]["color"] = 0x1'00'00'00;
  require(!detail::decode_manifest(malformed.dump()), "colours are 24-bit");
  malformed = json;
  malformed["project"]["markers"][1]["position"] = 0;
  require(!detail::decode_manifest(malformed.dump()), "one marker per position");
  malformed = json;
  malformed["project"]["markers"][0]["position"] = 32 * quarter;
  require(!detail::decode_manifest(malformed.dump()), "markers are sorted by position");
  malformed = json;
  malformed["project"]["tracks"][0]["name"] = "bad\nname";
  require(!detail::decode_manifest(malformed.dump()), "names are single-line text");
  malformed = json;
  malformed["project"]["tracks"][0].erase("color");
  require(!detail::decode_manifest(malformed.dump()), "tracks carry a colour field");
  malformed = json;
  malformed["project"]["key"]["tonic"] = 12;
  require(!detail::decode_manifest(malformed.dump()), "a key's tonic is a pitch class");
  malformed = json;
  malformed["project"]["key"]["mode"] = "dorian";
  require(!detail::decode_manifest(malformed.dump()), "a key is major or minor");
  malformed = json;
  malformed["project"].erase("key");
  require(!detail::decode_manifest(malformed.dump()), "the project carries a key field");
}

} // namespace

int main() {
  try {
    presentation_round_trips();
    invalid_presentation_is_rejected();
    std::cout << "Presentation schema contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
