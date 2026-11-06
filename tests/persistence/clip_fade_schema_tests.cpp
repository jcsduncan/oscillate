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

namespace {
using namespace oscillate;
using namespace oscillate::project;
using namespace oscillate::persistence;
using Json = nlohmann::json;

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

Snapshot snapshot() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  Snapshot state{id<ProjectId>(1), 2, 48'000, std::move(tempo), std::move(meter), {}, {}};
  Track track{id<TrackId>(2)};
  track.clips.emplace_back(id<ClipId>(3), id<SourceId>(4), SourceRange{{0}, {8}},
                           ElapsedInterval{{0}, {8}}, 2, std::vector<AutomationLane>{},
                           ClipFades{0.5, 0.25, FadeCurve::linear, FadeCurve::smoothstep});
  state.tracks.push_back(std::move(track));
  return state;
}

void fades_round_trip() {
  const auto original = snapshot();
  const auto encoded = detail::encode_manifest(original, std::string(32, 'a'), std::nullopt,
                                               SaveKind::explicit_save);
  auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 47, "clip fade schema was not saved");
  REQUIRE_EQ(json["project"]["tracks"][0]["clips"][0]["fades"]["out_curve"], "smoothstep",
             "clip fade schema was not saved");
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks.front().clips.front().fades,
             original.tracks.front().clips.front().fades,
             "clip fade settings did not survive manifest round trip");
  for (const auto& field : {"in_fraction", "out_fraction", "in_curve", "out_curve"}) {
    auto malformed = json;
    malformed["project"]["tracks"][0]["clips"][0]["fades"].erase(field);
    require(!detail::decode_manifest(malformed.dump()), "missing fade field was accepted");
  }
  auto malformed = json;
  malformed["project"]["tracks"][0]["clips"][0]["fades"]["out_curve"] = "equal_power";
  require(!detail::decode_manifest(malformed.dump()), "unknown fade curve was accepted");
  malformed = json;
  malformed["project"]["tracks"][0]["clips"][0]["fades"]["in_fraction"] = 0.9;
  require(!detail::decode_manifest(malformed.dump()), "overlapping fade lengths were accepted");
  for (const int other : {46, 48}) {
    malformed = json;
    malformed["schema"]["minor"] = other;
    require(!detail::decode_manifest(malformed.dump()), "another schema minor was accepted");
  }
  malformed = json;
  malformed["schema"]["major"] = 2;
  require(!detail::decode_manifest(malformed.dump()), "another schema major was accepted");
}

} // namespace

int main() {
  try {
    fades_round_trip();
    std::cout << "Clip fade schema contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
