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

// Schema 1.32 (ADR-0035): MIDI clips carry pedal lanes. Main's 1.31 added
// VST3 controller state; 1.32 must keep it, so a file carrying both
// round-trips both, and 1.31 files load unchanged with no pedals.
namespace {
using namespace oscillate;
using namespace oscillate::project;
using namespace oscillate::persistence;
using Json = nlohmann::json;

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

constexpr std::int64_t quarter = timeline::ticks_per_quarter;

Vst3PluginInstance instrument() {
  Vst3PluginInstance plugin{
      id<PluginInstanceId>(5), std::string(32, 'a'), "Fixture",        "Processor",  "1.2",
      "/tmp/Fixture.vst3",     std::string(64, 'b'), {0, 1, 254, 255}, {{7, 0.625}}, false};
  plugin.controller_state = std::vector<std::uint8_t>{9, 8, 7};
  return plugin;
}

MidiClip clip(std::vector<MidiPedalLane> pedals) {
  return {id<ClipId>(20),
          MusicalInterval{{0}, {8 * quarter}},
          {4 * quarter},
          true,
          {{id<NoteId>(21), {0}, {quarter}, 60, 100},
           {id<NoteId>(22), {quarter}, {2 * quarter}, 64, 90}},
          3,
          {},
          std::move(pedals)};
}

std::vector<MidiPedalLane> pedals() {
  return {{MidiPedal::sustain, {{{0}, 1.0}, {{quarter / 2}, 0.5}, {{3 * quarter}, 0.0}}},
          {MidiPedal::soft, {{{quarter}, 1.0}}}};
}

Snapshot snapshot(std::vector<MidiPedalLane> lanes) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  Snapshot state{id<ProjectId>(1), 2, 48'000, std::move(tempo), std::move(meter), {}, {}};
  Track track{id<TrackId>(10), {}, TrackKind::instrument, instrument()};
  track.midi_clips.push_back(clip(std::move(lanes)));
  state.tracks.push_back(std::move(track));
  return state;
}

Json encoded(const Snapshot& state) {
  return Json::parse(
      detail::encode_manifest(state, std::string(32, 'a'), std::nullopt, SaveKind::explicit_save));
}

void pedals_and_controller_state_round_trip_together() {
  const auto original = snapshot(pedals());
  const auto json = encoded(original);
  REQUIRE_EQ(json["schema"]["minor"], 48, "pedals save in the current schema");
  const auto& track = json["project"]["tracks"][0];
  REQUIRE_EQ(track["instrument_source"]["payload"]["controller_state_base64"], "CQgH",
             "1.32 keeps 1.31's VST3 controller state");
  const auto& lanes = track["midi_clips"][0]["pedals"];
  REQUIRE_EQ(lanes.size(), 2, "pedal lanes are saved by name with their points");
  REQUIRE_EQ(lanes[0]["pedal"], "sustain", "pedal lanes are saved by name with their points");
  REQUIRE_EQ(lanes[1]["pedal"], "soft", "pedal lanes are saved by name with their points");
  REQUIRE_EQ(lanes[0]["points"][1]["value"], 0.5,
             "pedal lanes are saved by name with their points");
  const auto decoded = take(detail::decode_manifest(json.dump()));
  REQUIRE_EQ(decoded.snapshot.tracks, original.tracks,
             "pedals, notes and controller state survive the round trip exactly");

  const auto empty = encoded(snapshot({}));
  require(empty["project"]["tracks"][0]["midi_clips"][0]["pedals"].empty(),
          "a clip without pedals saves an empty list");
}

void other_schemas_are_rejected() {
  for (const int minor : {46, 48}) {
    auto other = encoded(snapshot(pedals()));
    other["schema"]["minor"] = minor;
    require(!detail::decode_manifest(other.dump()), "another schema minor is rejected");
  }
  auto other = encoded(snapshot(pedals()));
  other["schema"]["major"] = 2;
  require(!detail::decode_manifest(other.dump()), "another schema major is rejected");
}

void malformed_pedals_are_rejected() {
  const auto json = encoded(snapshot(pedals()));
  const auto rejects = [&](auto&& change, std::string_view message) {
    auto malformed = json;
    change(malformed["project"]["tracks"][0]["midi_clips"][0]["pedals"]);
    require(!detail::decode_manifest(malformed.dump()), message);
  };
  rejects([](Json& p) { p[0]["pedal"] = "damper"; }, "unknown pedal names are rejected");
  rejects([](Json& p) { p[1]["pedal"] = "sustain"; }, "a pedal has one lane");
  rejects([](Json& p) { p[0]["points"][1]["position"] = 0; }, "positions strictly increase");
  // A point past the pattern end is kept, silent, for when the clip grows.
  auto past_end = json;
  past_end["project"]["tracks"][0]["midi_clips"][0]["pedals"][0]["points"][2]["position"] =
      4 * quarter;
  REQUIRE_OK(detail::decode_manifest(past_end.dump()),
             "a pedal point past the pattern end was refused");
  rejects([](Json& p) { p[0]["points"][0]["value"] = 1.5; }, "values are 0..1");
  rejects([](Json& p) { p[1]["points"] = Json::array(); }, "an empty lane is not stored");
  rejects([](Json& p) { p[0]["extra"] = 1; }, "unknown lane fields are rejected");
  rejects([](Json& p) { p[0]["points"][0]["extra"] = 1; }, "unknown point fields are rejected");
}

} // namespace

int main() {
  try {
    pedals_and_controller_state_round_trip_together();
    other_schemas_are_rejected();
    malformed_pedals_are_rejected();
    std::cout << "MIDI pedal schema contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
