#include "oscillate/project/insert_projection.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/check.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::project;
using foundation::Result;

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

struct Gate final : TransportGate {
  struct Lease final : StoppedTransportLease {};
  Result<std::unique_ptr<StoppedTransportLease>> acquire_stopped() override {
    return Result<std::unique_ptr<StoppedTransportLease>>::success(std::make_unique<Lease>());
  }
};

ProjectModel create(Gate& gate) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return take(
      ProjectModel::create(id<ProjectId>(1), 48'000, std::move(tempo), std::move(meter), gate));
}

NativeInstrumentPatch prism(double cutoff = 1'200.0) {
  return {"prism", 1, {{"prism.filter.cutoff", cutoff}, {"prism.osc1.unison", 4}}};
}

AutomationLane native_lane(std::uint64_t lane, TrackId track, std::string parameter) {
  AutomationLane result{
      id<AutomationLaneId>(lane),
      NativeParameterTarget{track, parameter},
      {AutomationRepresentation::physical, parameter, "Hz", 20.0, 20'000.0, 2'400.0, false, 0},
      AutomationTimebase::musical,
      {{id<AutomationPointId>(lane * 10), 0, 300.0}},
      1};
  return result;
}

void select_edit_undo_and_reopen() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  REQUIRE_OK(project.apply({AddTrack{track, TrackKind::instrument}}));
  auto selected = take(project.apply({SetNativeInstrumentPatch{track, prism()}}));
  REQUIRE_EQ(std::get<NativeInstrumentPatch>(selected.snapshot->tracks[0].instrument_source),
             prism(), "native patch was not published exactly");
  REQUIRE_EQ(selected.changes.tracks.size(), 1, "track change was not reported");

  // A gesture groups a knob drag into one undo step.
  REQUIRE_OK(project.apply({SetNativeInstrumentPatch{track, prism(900.0)}}, 7));
  REQUIRE_OK(project.apply({SetNativeInstrumentPatch{track, prism(600.0)}}, 7));
  auto undone = take(project.undo());
  REQUIRE_EQ(std::get<NativeInstrumentPatch>(undone.snapshot->tracks[0].instrument_source), prism(),
             "a drag gesture undoes to the patch before the drag");

  auto reopened = take(ProjectModel::open(*undone.snapshot, gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0], undone.snapshot->tracks[0],
             "native patch survives model rehydration");

  const auto projection = take(project_insert_chains(*undone.snapshot));
  const auto* source = std::get_if<ProjectedNativeInstrumentSource>(&projection.paths[0].source);
  REQUIRE_NE(source, nullptr, "insert projection names the native engine");
  REQUIRE_EQ(source->engine, "prism", "insert projection names the native engine");
}

// Regions (ADR-0063): the model checks structure only; the engine checks
// meaning (Crate's slot range, its one media and tag vocabulary).
void regions_are_checked_structurally() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  REQUIRE_OK(project.apply({AddTrack{track, TrackKind::instrument}}));
  const auto with_regions = [](std::vector<NativeRegion> regions) {
    NativeInstrumentPatch patch{"crate", 1, {}};
    patch.document.media = {{"sample", NativeBuiltinMedia{"break"}, 0},
                            {"other", NativeBuiltinMedia{"tone"}, 0}};
    patch.document.regions = std::move(regions);
    return patch;
  };
  const auto valid = with_regions({{3, "sample", 0, "kick", "Kick"},
                                   {0, "sample", 4'800, "snare", ""},
                                   {7, "other", 0, "other", "Pad"},
                                   {1, "sample", 9'600, "", "Hat"}});
  require(!valid.document.empty(), "a document with regions is not empty");
  auto applied = take(project.apply({SetNativeInstrumentPatch{track, valid}}));
  REQUIRE_EQ(std::get<NativeInstrumentPatch>(applied.snapshot->tracks[0].instrument_source), valid,
             "valid regions publish exactly, in vector order");
  auto reopened = take(ProjectModel::open(*applied.snapshot, gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0], applied.snapshot->tracks[0],
             "regions survive model rehydration");

  std::vector<NativeRegion> too_many;
  for (std::uint32_t slot = 0; slot <= max_native_regions; ++slot)
    too_many.push_back({slot, "sample", slot * 10ULL, "other", ""});
  const std::vector<std::pair<std::vector<NativeRegion>, std::string_view>> rejected{
      {{{0, "sample", 0, "kick", ""}, {0, "sample", 10, "hat", ""}}, "duplicate slots"},
      {{{0, "missing", 0, "kick", ""}}, "a region in an unknown media slot"},
      {{{0, "sample", 10, "kick", ""}, {1, "sample", 10, "hat", ""}}, "equal starts in one media"},
      {{{0, "sample", 20, "kick", ""}, {1, "sample", 10, "hat", ""}},
       "decreasing starts in one media"},
      {{{0, "sample", 0, std::string(max_native_region_tag + 1, 'k'), ""}}, "an over-long tag"},
      {{{0, "sample", 0, "kick", std::string(max_native_region_name + 1, 'n')}},
       "an over-long name"},
      {{{0, "sample", 0, "kick", std::string("bad\x01name")}}, "a control character in a name"},
      {std::move(too_many), "more than max_native_regions regions"},
  };
  for (const auto& [regions, what] : rejected) {
    const auto before = project.snapshot();
    require(!project.apply({SetNativeInstrumentPatch{track, with_regions(regions)}}),
            std::string("the model accepted ") + std::string(what));
    REQUIRE_EQ(project.snapshot(), before, "a rejected region edit changed the project");
  }
}

void native_lanes_follow_their_engine() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  const auto other = id<TrackId>(3);
  REQUIRE_OK(
      project.apply({AddTrack{track, TrackKind::instrument}, AddTrack{other, TrackKind::instrument},
                     SetNativeInstrumentPatch{track, prism()}}));
  REQUIRE_OK(
      project.apply({AddAutomationLane{track, native_lane(6, track, "prism.filter.cutoff")}}));
  require(!project.apply({AddAutomationLane{other, native_lane(7, other, "prism.filter.cutoff")}}),
          "a lane on a track without a synth is orphaned");
  require(!project.apply({AddAutomationLane{track, native_lane(8, other, "prism.filter.cutoff")}}),
          "a lane must target its owning track");
  auto mismatched = native_lane(9, track, "prism.filter.resonance");
  mismatched.domain.semantic_key = "prism.filter.cutoff";
  require(!project.apply({AddAutomationLane{track, mismatched}}),
          "the domain must name the same parameter");

  auto same_engine = take(project.apply({SetNativeInstrumentPatch{track, prism(5'000.0)}}));
  REQUIRE_EQ(same_engine.snapshot->tracks[0].automation.size(), 1,
             "editing the patch keeps its lanes");
  auto other_engine = prism();
  other_engine.engine = "warm";
  auto replaced = take(project.apply({SetNativeInstrumentPatch{track, other_engine}}));
  require(replaced.snapshot->tracks[0].automation.empty(),
          "a different engine drops the old engine's lanes");
  auto removed = take(project.apply({SetNativeInstrumentPatch{track, std::nullopt}}));
  require(std::holds_alternative<std::monostate>(removed.snapshot->tracks[0].instrument_source),
          "removing the synth empties the source");
}

void invalid_patches_are_rejected_without_change() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  const auto audio = id<TrackId>(3);
  REQUIRE_OK(project.apply({AddTrack{track, TrackKind::instrument}, AddTrack{audio}}));
  const auto before = project.snapshot();
  require(!project.apply({SetNativeInstrumentPatch{audio, prism()}}),
          "audio tracks cannot host a synth");
  require(!project.apply({SetNativeInstrumentPatch{id<TrackId>(77), prism()}}),
          "unknown tracks are rejected");
  auto bad = prism();
  bad.engine = "Prism!";
  require(!project.apply({SetNativeInstrumentPatch{track, bad}}), "engine ids are restricted");
  bad = prism();
  bad.engine_version = 0;
  require(!project.apply({SetNativeInstrumentPatch{track, bad}}), "engine version is nonzero");
  bad = prism();
  bad.parameters.push_back({"prism.filter.cutoff", 3.0});
  require(!project.apply({SetNativeInstrumentPatch{track, bad}}), "duplicate ids are rejected");
  bad = prism();
  bad.parameters[0].value = std::numeric_limits<double>::infinity();
  require(!project.apply({SetNativeInstrumentPatch{track, bad}}), "values must be finite");
  bad = prism();
  bad.parameters.assign(max_native_instrument_parameters + 1, {"x", 0.0});
  for (std::size_t i = 0; i < bad.parameters.size(); ++i) {
    bad.parameters[i].id = "p" + std::to_string(i);
  }
  require(!project.apply({SetNativeInstrumentPatch{track, bad}}), "parameter count is bounded");
  REQUIRE_EQ(project.snapshot(), before, "rejected commands publish nothing");
}

} // namespace

SourceRecord table_source() {
  return {id<SourceId>(40), SourceEncoding::wav, std::string(64, 'c'), 16'428, 48'000, 1, 4'096};
}

// Structured synth state (ADR-0057): the model checks structure, limits and
// media identity; the engine checks meaning.
NativeSynthDocument full_document() {
  const auto source = table_source();
  NativeSynthDocument document;
  document.routes = {{0, "lfo.1", "", "nova.osc.a.position", false, 0.0},
                     {7, "env.2", "macro.1", "nova.filter.1.cutoff", true, -0.5}};
  document.curves = {{"lfo.1",
                      {{0.0, 0.1, 0.0, NativeCurveSegment::curve},
                       {0.25, 1.0, -0.5, NativeCurveSegment::step},
                       {1.0, 0.1, 0.0, NativeCurveSegment::curve}}}};
  document.chains = {{"fx.main", {{3, "distortion"}, {1, "reverb"}}}, {"fx.bus.1", {{2, "delay"}}}};
  document.sequences = {{"clip.1", {3'840}, {{{0}, {480}, 60, 100}, {{960}, {240}, 67, 90}}}};
  document.media = {{"osc.a",
                     ManagedAudioReference{source.id, source.sha256, source.byte_length,
                                           source.sample_rate, source.channels, source.frames},
                     2'048},
                    {"osc.b", NativeBuiltinMedia{"basic-shapes"}, 2'048}};
  document.labels = {{"macro.1", "Growl"}, {"macro.2", "Width"}};
  return document;
}

void documents_are_validated_and_undone() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  REQUIRE_OK(project.apply({AddTrack{track, TrackKind::instrument}, AddSource{table_source()}}));
  auto patch = prism();
  patch.document = full_document();
  REQUIRE_OK(project.apply({SetNativeInstrumentPatch{track, patch}}));
  REQUIRE_EQ(std::get<NativeInstrumentPatch>(project.snapshot()->tracks[0].instrument_source),
             patch, "a full document is stored with the patch");
  require(!project.apply({RemoveSource{table_source().id}}),
          "a source a media slot plays cannot be removed");

  const auto rejects = [&](auto mutate, std::string_view why) {
    auto bad = patch;
    mutate(bad.document);
    const auto before = project.snapshot();
    require(!project.apply({SetNativeInstrumentPatch{track, bad}}), why);
    REQUIRE_EQ(project.snapshot(), before, "a rejected document changes nothing");
  };
  rejects([](NativeSynthDocument& d) { d.routes[1].slot = 0; }, "route slots are unique");
  rejects([](NativeSynthDocument& d) { d.routes[0].slot = max_native_synth_routes; },
          "route slots are bounded");
  rejects([](NativeSynthDocument& d) { d.routes[0].destination = "Bad Id"; },
          "route ids are restricted");
  rejects([](NativeSynthDocument& d) { d.routes[0].tension = 1.5; }, "tension is -1..1");
  rejects([](NativeSynthDocument& d) { d.routes[0].scale = -0.1; }, "scale is 0..1");
  rejects([](NativeSynthDocument& d) { d.curves[0].points[1].x = 0.0; },
          "curve points move strictly forwards");
  rejects([](NativeSynthDocument& d) { d.curves[0].points.back().x = 0.9; },
          "curves span the whole cycle");
  rejects([](NativeSynthDocument& d) { d.curves[0].points[1].y = std::nan(""); },
          "curve values are finite");
  rejects([](NativeSynthDocument& d) { d.curves.push_back(d.curves[0]); }, "curve ids are unique");
  rejects([](NativeSynthDocument& d) { d.chains[1].modules[0].uid = 3; },
          "slot uids are unique across chains");
  rejects([](NativeSynthDocument& d) { d.chains[0].modules.resize(max_native_chain_slots + 1); },
          "chains are bounded");
  rejects([](NativeSynthDocument& d) { d.sequences[0].notes[0].start = {3'840}; },
          "sequence notes start inside the sequence");
  rejects([](NativeSynthDocument& d) { d.sequences[0].notes[0].velocity = 0; },
          "sequence notes are struck");
  rejects(
      [](NativeSynthDocument& d) {
        std::get<ManagedAudioReference>(d.media[0].media).sha256 = std::string(64, 'd');
      },
      "media slots match their source's identity");
  rejects([](NativeSynthDocument& d) { d.media[0].frame_length = 8'193; },
          "frame lengths are bounded");
  rejects([](NativeSynthDocument& d) { d.labels[0].text = std::string(65, 'x'); },
          "labels are short");
  rejects([](NativeSynthDocument& d) { d.labels[0].text.clear(); },
          "a label has text; no label means the engine's own name");

  // Undo restores the previous document exactly.
  auto edited = patch;
  edited.document.routes.pop_back();
  REQUIRE_OK(project.apply({SetNativeInstrumentPatch{track, edited}}));
  REQUIRE_OK(project.undo());
  REQUIRE_EQ(std::get<NativeInstrumentPatch>(project.snapshot()->tracks[0].instrument_source),
             patch, "undo restores the document");
}

int main() {
  try {
    select_edit_undo_and_reopen();
    regions_are_checked_structurally();
    native_lanes_follow_their_engine();
    invalid_patches_are_rejected_without_change();
    documents_are_validated_and_undone();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "native instrument model contract passed\n";
  return 0;
}
