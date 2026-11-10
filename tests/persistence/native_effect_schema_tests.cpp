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

// Schema 1.33 (ADR-0040, ADR-0043): built-in effect slots on tracks and the
// master, and lanes that automate their parameters.
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
  Track track{id<TrackId>(2), {}, TrackKind::audio};
  // ADR-0058: effects carry drawn curves like synths.
  NativeSynthDocument curves;
  curves.curves = {{"shape", {{0.0, 0.1, 0.25}, {0.3, 0.9, -0.5}, {1.0, 1.0}}}};
  track.effects.push_back(
      {id<InsertSlotId>(5), true,
       NativeEffectPatch{"contour",
                         1,
                         {{"contour.cutoff", 812.123456789}, {"contour.model", 2}},
                         3,
                         curves,
                         // Linked instances share their document.
                         "kick-duck-7"}});
  track.automation.push_back({id<AutomationLaneId>(3),
                              NativeEffectParameterTarget{id<InsertSlotId>(5), "contour.cutoff"},
                              {AutomationRepresentation::physical, "contour.cutoff", "Hz", 20.0,
                               20'000.0, 1'000.0, false, 0},
                              AutomationTimebase::musical,
                              {{id<AutomationPointId>(4), 0, 300.0, AutomationShape::exponential}},
                              1});
  state.tracks.push_back(std::move(track));
  state.master_effects.push_back(
      {id<InsertSlotId>(6), false, NativeEffectPatch{"vista", 1, {}, 1}});
  state.master_effects.push_back(
      {id<InsertSlotId>(7), true, NativeEffectPatch{"loudness", 1, {{"loudness.target", -14}}, 2}});
  return state;
}

std::string encode(const Snapshot& state) {
  return detail::encode_manifest(state, std::string(32, 'a'), std::nullopt,
                                 SaveKind::explicit_save);
}

void effects_round_trip_exactly() {
  const auto original = snapshot();
  const auto encoded = encode(original);
  const auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 49, "built-in effects save in the current schema");
  const auto& slot = json["project"]["tracks"][0]["effects"][0];
  REQUIRE_EQ(slot["kind"], "native_effect", "the effect payload is saved");
  REQUIRE_EQ(slot["payload"]["engine"], "contour", "the effect payload is saved");
  REQUIRE_EQ(slot["payload"]["generation"], 3, "the effect payload is saved");
  REQUIRE_EQ(slot["payload"]["parameters"].size(), 2, "the effect payload is saved");
  REQUIRE_EQ(slot["payload"]["document"]["curves"].size(), 1, "the effect document is saved");
  REQUIRE_EQ(slot["payload"]["link"], "kick-duck-7", "the link is saved, empty when unlinked");
  REQUIRE_EQ(json["project"]["master_effects"][0]["payload"]["link"], "",
             "the link is saved, empty when unlinked");
  REQUIRE_EQ(json["project"]["tracks"][0]["automation"][0]["target"]["kind"],
             "native_effect_parameter", "the lane names an effect parameter");
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks, original.tracks,
             "slots, values and lanes survive the round trip bit for bit");
  REQUIRE_EQ(decoded.snapshot.master_effects, original.master_effects,
             "slots, values and lanes survive the round trip bit for bit");
}

void malformed_manifests_are_rejected() {
  const auto json = Json::parse(encode(snapshot()));
  for (const char* missing :
       {"generation", "engine", "engine_version", "parameters", "document", "link"}) {
    auto malformed = json;
    malformed["project"]["tracks"][0]["effects"][0]["payload"].erase(missing);
    require(!detail::decode_manifest(malformed.dump()), "missing payload fields are rejected");
  }
  auto malformed = json;
  malformed["project"]["tracks"][0]["effects"][0]["payload"]["parameters"][1]["id"] =
      "contour.cutoff";
  require(!detail::decode_manifest(malformed.dump()), "duplicate parameter ids are rejected");
  malformed = json;
  malformed["project"]["tracks"][0]["effects"][0]["payload"]["engine"] = "../contour";
  require(!detail::decode_manifest(malformed.dump()), "engine ids are restricted");
  malformed = json;
  malformed["project"]["tracks"][0]["effects"][0]["payload"]["link"] = "Kick Duck";
  require(!detail::decode_manifest(malformed.dump()), "links are native identifiers");
  malformed = json;
  malformed["project"]["tracks"][0]["effects"][0]["payload"]["generation"] = 0;
  require(!detail::decode_manifest(malformed.dump()), "generations start at one");
  malformed = json;
  malformed["project"]["tracks"][0]["automation"][0]["target"]["slot_id"] =
      json["project"]["master_effects"][1]["id"];
  require(!detail::decode_manifest(malformed.dump()),
          "a lane may only target an effect on its own track");
}

// Schema 1.43 (ADR-0059): note effects, macros and a frozen render.
Snapshot devices_snapshot() {
  auto state = snapshot();
  const auto synth = id<TrackId>(10);
  const AutomationDomain cutoff{
      AutomationRepresentation::physical, "contour.cutoff", "Hz", 20.0, 20'000.0, 1'000.0, false, 0,
      AutomationScale::logarithmic};
  const AutomationDomain rate{
      AutomationRepresentation::physical, "arp.rate", "steps", 0.0, 8.0, 2.0, true, 8};
  const AutomationDomain drive{
      AutomationRepresentation::physical, "prism.drive", "", 0.0, 1.0, 0.0, false, 0};
  Track track{synth, {}, TrackKind::instrument, NativeInstrumentPatch{"prism", 1, {}}};
  track.note_effects = {{id<InsertSlotId>(8), true, "arp", 1, {{"arp.rate", 3.0}}},
                        {id<InsertSlotId>(9), false, "chord", 2, {}}};
  track.effects.push_back({id<InsertSlotId>(11), true, NativeEffectPatch{"contour", 1, {}, 1}});
  track.macros = {{"Tone",
                   0.25,
                   {{NativeEffectParameterTarget{id<InsertSlotId>(11), "contour.cutoff"}, cutoff,
                     4'000.0, 200.0},
                    {NativeEffectParameterTarget{id<InsertSlotId>(8), "arp.rate"}, rate, 1.0, 6.0},
                    {NativeParameterTarget{synth, "prism.drive"}, drive, 0.0, 0.5}}},
                  {"", 0.0, {}}};
  track.automation.push_back({id<AutomationLaneId>(12),
                              MacroTarget{synth, 0},
                              macro_automation_domain(0.25),
                              AutomationTimebase::musical,
                              {{id<AutomationPointId>(13), 0, 0.75, AutomationShape::linear}},
                              1});
  state.sources.push_back(
      {id<SourceId>(14), SourceEncoding::wav, std::string(64, 'd'), 300'044, 48'000, 2, 75'000});
  track.frozen =
      FrozenTrack{{id<SourceId>(14), std::string(64, 'd'), 300'044, 48'000, 2, 75'000}, 960, 1};
  state.tracks.push_back(std::move(track));
  return state;
}

void track_devices_round_trip_and_validate() {
  const auto original = devices_snapshot();
  const auto encoded = encode(original);
  const auto json = Json::parse(encoded);
  const auto& track = json["project"]["tracks"][1];
  REQUIRE_EQ(track["note_effects"].size(), 2,
             "note effects, macros, macro lanes and the freeze are saved");
  REQUIRE_EQ(track["note_effects"][0]["engine"], "arp",
             "note effects, macros, macro lanes and the freeze are saved");
  REQUIRE_EQ(track["macros"][0]["mappings"].size(), 3,
             "note effects, macros, macro lanes and the freeze are saved");
  REQUIRE_EQ(track["macros"][0]["mappings"][0]["destination"]["kind"], "native_effect_parameter",
             "note effects, macros, macro lanes and the freeze are saved");
  REQUIRE_EQ(track["frozen"]["first_frame"], 960,
             "note effects, macros, macro lanes and the freeze are saved");
  REQUIRE_EQ(track["automation"][0]["target"]["kind"], "macro",
             "note effects, macros, macro lanes and the freeze are saved");
  REQUIRE_TRUE(json["project"]["tracks"][0]["frozen"].is_null(),
               "note effects, macros, macro lanes and the freeze are saved");
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks, original.tracks, "they survive the round trip exactly");

  const auto rejected = [&](auto change, std::string_view message) {
    auto malformed = json;
    change(malformed["project"]);
    require(!detail::decode_manifest(malformed.dump()), message);
  };
  for (const char* missing : {"note_effects", "macros", "frozen"})
    rejected([&](Json& project) { project["tracks"][1].erase(missing); },
             "missing 1.43 track fields are rejected");
  rejected(
      [](Json& project) {
        auto effects = project["tracks"][1]["note_effects"];
        effects[0]["id"] = "00000000-0000-0000-0000-000000000041";
        effects[1]["id"] = "00000000-0000-0000-0000-000000000042";
        project["tracks"][0]["note_effects"] = effects;
      },
      "note effects need an instrument track");
  rejected(
      [](Json& project) {
        project["tracks"][1]["note_effects"][1]["id"] = project["tracks"][1]["effects"][0]["id"];
      },
      "note effect and effect slot ids are one namespace");
  rejected([](Json& project) { project["tracks"][1]["macros"][0]["value"] = 1.5; },
           "a macro turns from 0 to 1");
  rejected(
      [](Json& project) {
        project["tracks"][1]["macros"][0]["mappings"][0]["destination"]["slot_id"] =
            project["tracks"][0]["effects"][0]["id"];
      },
      "a macro maps parameters on its own track");
  rejected(
      [](Json& project) {
        project["tracks"][1]["macros"][1]["mappings"].push_back(
            project["tracks"][1]["macros"][0]["mappings"][2]);
      },
      "a parameter has one macro");
  rejected([](Json& project) { project["tracks"][1]["macros"][0]["mappings"][0]["maximum"] = 1e9; },
           "a mapping stays inside the parameter's domain");
  rejected(
      [](Json& project) {
        auto lane = project["tracks"][0]["automation"][0];
        lane["target"]["slot_id"] = project["tracks"][1]["effects"][0]["id"];
        lane["id"] = "00000000-0000-0000-0000-000000000063";
        lane["points"][0]["id"] = "00000000-0000-0000-0000-000000000064";
        project["tracks"][1]["automation"].push_back(lane);
      },
      "a mapped parameter has no lane of its own");
  rejected([](Json& project) { project["tracks"][1]["automation"][0]["target"]["index"] = 2; },
           "a macro lane names a macro");
  rejected([](Json& project) { project["tracks"][1]["frozen"]["audio"]["sample_rate"] = 44'100; },
           "a freeze matches its source");
  rejected([](Json& project) { project["tracks"][1]["frozen"]["rendered_revision"] = 0; },
           "a freeze names the revision it rendered");
}

} // namespace

int main() {
  try {
    effects_round_trip_exactly();
    malformed_manifests_are_rejected();
    track_devices_round_trip_and_validate();
    std::cout << "Native effect schema contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
