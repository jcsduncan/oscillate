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
  Track track{id<TrackId>(2),
              {},
              TrackKind::instrument,
              NativeInstrumentPatch{"prism",
                                    1,
                                    {{"prism.filter.cutoff", 1'234.5678901234},
                                     {"prism.osc1.unison", 5},
                                     {"prism.perf.bend", -0.25}}}};
  track.automation.push_back({id<AutomationLaneId>(3),
                              NativeParameterTarget{id<TrackId>(2), "prism.filter.cutoff"},
                              {AutomationRepresentation::physical, "prism.filter.cutoff", "Hz",
                               20.0, 20'000.0, 2'400.0, false, 0},
                              AutomationTimebase::musical,
                              {{id<AutomationPointId>(4), 0, 300.0, AutomationShape::exponential}},
                              1});
  state.tracks.push_back(std::move(track));
  return state;
}

// Every kind of structured state (ADR-0057), including a managed media slot.
Snapshot document_snapshot() {
  auto state = snapshot();
  const SourceRecord table{
      id<SourceId>(9), SourceEncoding::wav, std::string(64, 'c'), 16'428, 48'000, 1, 4'096};
  state.sources.push_back(table);
  auto& patch = std::get<NativeInstrumentPatch>(state.tracks[0].instrument_source);
  patch.document.routes = {{0, "lfo.1", "", "prism.filter.cutoff", false, 0.0},
                           {255, "env.2", "macro.1", "prism.osc1.unison", true, -0.75, 0.5}};
  patch.document.curves = {{"lfo.1",
                            {{0.0, 0.1, 0.0, NativeCurveSegment::curve},
                             {0.3333333333333333, 1.0, 0.25, NativeCurveSegment::step},
                             {1.0, 0.1, 0.0, NativeCurveSegment::curve}}}};
  patch.document.chains = {{"fx.main", {{3, "distortion"}, {1, "reverb"}}}, {"fx.bus.1", {}}};
  patch.document.sequences = {{"clip.1", {3'840}, {{{0}, {480}, 60, 100}}}};
  patch.document.media = {{"osc.a",
                           ManagedAudioReference{table.id, table.sha256, table.byte_length,
                                                 table.sample_rate, table.channels, table.frames},
                           2'048},
                          {"osc.b", NativeBuiltinMedia{"basic-shapes"}, 0}};
  patch.document.labels = {{"macro.1", "Grôwl"}};
  return state;
}

void synth_document_round_trips_exactly() {
  const auto original = document_snapshot();
  const auto encoded = detail::encode_manifest(original, std::string(32, 'a'), std::nullopt,
                                               SaveKind::explicit_save);
  const auto json = Json::parse(encoded);
  const auto& document = json["project"]["tracks"][0]["instrument_source"]["payload"]["document"];
  REQUIRE_EQ(document["routes"].size(), 2, "the document is saved with the patch");
  REQUIRE_EQ(document["media"][0]["managed"]["sha256"], std::string(64, 'c'),
             "the document is saved with the patch");
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks, original.tracks,
             "the document survives the manifest round trip bit for bit");
  REQUIRE_EQ(decoded.snapshot.sources, original.sources,
             "the document survives the manifest round trip bit for bit");

  const auto payload = json["project"]["tracks"][0]["instrument_source"]["payload"];
  const auto reject = [&](auto mutate, std::string_view why) {
    auto malformed = json;
    mutate(malformed["project"]["tracks"][0]["instrument_source"]["payload"]["document"]);
    require(!detail::decode_manifest(malformed.dump()), why);
  };
  reject([](Json& d) { d.erase("labels"); }, "every document field is required");
  reject([](Json& d) { d["routes"][0]["extra"] = 1; }, "unknown route fields are rejected");
  reject([](Json& d) { d["curves"][0]["points"][1]["segment"] = "smooth"; },
         "unknown curve segments are rejected");
  reject([](Json& d) { d["media"][1]["managed"] = d["media"][0]["managed"]; },
         "a media slot is builtin or managed, not both");
  reject([](Json& d) { d["media"][0]["managed"]["sha256"] = std::string(64, 'd'); },
         "managed media must match their source");
  reject([](Json& d) { d["routes"][1]["slot"] = 0; }, "duplicate route slots are rejected");
}

void native_patch_round_trips_exactly() {
  const auto original = snapshot();
  const auto encoded = detail::encode_manifest(original, std::string(32, 'a'), std::nullopt,
                                               SaveKind::explicit_save);
  const auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 48, "native instruments save in the current schema");
  const auto& source = json["project"]["tracks"][0]["instrument_source"];
  REQUIRE_EQ(source["kind"], "native_instrument", "native instrument payload is saved");
  REQUIRE_EQ(source["payload"]["engine"], "prism", "native instrument payload is saved");
  REQUIRE_EQ(source["payload"]["parameters"].size(), 3, "native instrument payload is saved");
  const auto decoded = take(detail::decode_manifest(encoded));
  REQUIRE_EQ(decoded.snapshot.tracks, original.tracks,
             "patch and native lanes survive the manifest round trip bit for bit");
}

void malformed_manifests_are_rejected() {
  const auto encoded = detail::encode_manifest(snapshot(), std::string(32, 'a'), std::nullopt,
                                               SaveKind::explicit_save);
  const auto json = Json::parse(encoded);

  auto future = json;
  future["schema"]["minor"] = 49;
  require(!detail::decode_manifest(future.dump()), "future schema is rejected");

  for (const char* missing : {"engine", "engine_version", "parameters", "document"}) {
    auto malformed = json;
    malformed["project"]["tracks"][0]["instrument_source"]["payload"].erase(missing);
    require(!detail::decode_manifest(malformed.dump()), "missing payload fields are rejected");
  }
  auto malformed = json;
  malformed["project"]["tracks"][0]["instrument_source"]["payload"]["parameters"][0]["extra"] = 1;
  require(!detail::decode_manifest(malformed.dump()), "unknown parameter fields are rejected");
  malformed = json;
  malformed["project"]["tracks"][0]["instrument_source"]["payload"]["parameters"][1]["id"] =
      "prism.filter.cutoff";
  require(!detail::decode_manifest(malformed.dump()), "duplicate parameter ids are rejected");
  malformed = json;
  malformed["project"]["tracks"][0]["instrument_source"]["payload"]["engine"] = "../prism";
  require(!detail::decode_manifest(malformed.dump()), "engine ids are restricted");
  malformed = json;
  malformed["project"]["tracks"][0]["automation"][0]["domain"]["semantic_key"] = "other";
  require(!detail::decode_manifest(malformed.dump()), "lane domains name their parameter");
}

} // namespace

int main() {
  try {
    native_patch_round_trips_exactly();
    malformed_manifests_are_rejected();
    synth_document_round_trips_exactly();
    std::cout << "Native instrument schema contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
