#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

using oscillate::test::take;

void accepted(foundation::Result<void> result) {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-synth-session") {}
};

const native_dsp::EngineDescriptor& prism() { return *synth::default_engine().descriptor; }

std::size_t index(std::string_view id) { return take(native_dsp::find_parameter(prism(), id)); }

std::size_t preset(std::string_view name) {
  for (std::size_t i = 0; i < prism().presets.size(); ++i) {
    if (prism().presets[i].name == name) {
      return i;
    }
  }
  throw std::runtime_error("missing preset");
}

const project::NativeInstrumentPatch& patch(const application::ProjectSession& session,
                                            project::TrackId track) {
  for (const auto& item : session.snapshot().tracks) {
    if (item.id == track) {
      return std::get<project::NativeInstrumentPatch>(item.instrument_source);
    }
  }
  throw std::runtime_error("missing track");
}

void synth_track_defaults_edits_and_undo() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "project"));
  const auto track = take(session->instruments().add_synth_track());
  REQUIRE_EQ(patch(*session, track).engine, "prism",
             "a new synth track stores the default patch sparsely");
  REQUIRE_TRUE(patch(*session, track).parameters.empty(),
               "a new synth track stores the default patch sparsely");
  REQUIRE_EQ(take(session->instruments().native_instrument_values(track)),
             native_dsp::default_values(prism()), "the panel reads dense defaults");

  auto values = take(session->instruments().native_instrument_values(track));
  values[index("prism.filter.cutoff")] = 900.0;
  accepted(session->instruments().set_native_instrument_values(track, values, 41));
  values[index("prism.filter.cutoff")] = 600.0;
  accepted(session->instruments().set_native_instrument_values(track, values, 41));
  REQUIRE_EQ(patch(*session, track).parameters.size(), 1, "only the changed value is saved");
  REQUIRE_EQ(patch(*session, track).parameters[0].value, 600.0, "only the changed value is saved");
  accepted(session->history().undo());
  REQUIRE_EQ(take(session->instruments().native_instrument_values(track)),
             native_dsp::default_values(prism()), "one gesture undoes as one step");

  auto bad = values;
  bad[index("prism.filter.cutoff")] = 1e9;
  require(!session->instruments().set_native_instrument_values(track, bad),
          "out-of-range values are rejected");
  bad = values;
  bad.pop_back();
  require(!session->instruments().set_native_instrument_values(track, bad),
          "short value lists are rejected");
  require(!session->instruments().set_native_instrument_values(track, values, 0),
          "gesture zero is rejected");

  accepted(session->instruments().select_native_instrument(track, "prism", preset("Acid")));
  REQUIRE_EQ(take(session->instruments().native_instrument_values(track)),
             native_dsp::preset_values(prism(), prism().presets[preset("Acid")]).value(),
             "selecting a preset loads its values");
  require(!session->instruments().select_native_instrument(track, "nonexistent"),
          "unknown engines fail");
  require(!session->instruments().select_native_instrument(track, "prism", 999),
          "unknown presets fail");
}

// Engines without structured state (ADR-0057) keep an empty document.
void documents_follow_the_engine() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "project"));
  const auto track = take(session->instruments().add_synth_track());
  require(take(session->instruments().native_synth_document(track)).empty(),
          "Prism has an empty document");
  accepted(session->instruments().set_native_synth_document(track, {}));
  project::NativeSynthDocument routed;
  routed.routes.push_back({0, "lfo.1", "", "prism.filter.cutoff"});
  const auto revision = session->snapshot().revision;
  require(!session->instruments().set_native_synth_document(track, routed),
          "an engine without structured state rejects a document");
  REQUIRE_EQ(session->snapshot().revision, revision, "a rejected document changes nothing");
  require(!session->instruments().set_native_synth_document(track, {}, 0),
          "gesture zero is rejected");
  auto values = take(session->instruments().native_instrument_values(track));
  values[index("prism.filter.cutoff")] = 900.0;
  accepted(session->instruments().set_native_instrument_values(track, values));
  require(patch(*session, track).document.empty(), "value edits keep the document");
}

void automation_points_follow_engine_rules() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "project"));
  const auto track = take(session->instruments().add_synth_track());
  const auto point = take(session->parameter_automation().set_native_automation_point(
      track, "prism.filter.cutoff", 0, 500.0));
  const auto& lanes = session->snapshot().tracks.front().automation;
  REQUIRE_EQ(lanes.size(), 1, "the lane domain comes from the engine table");
  REQUIRE_EQ(lanes[0].domain.semantic_key, "prism.filter.cutoff",
             "the lane domain comes from the engine table");
  REQUIRE_EQ(lanes[0].domain.minimum, 20.0, "the lane domain comes from the engine table");
  REQUIRE_EQ(lanes[0].domain.maximum, 20'000.0, "the lane domain comes from the engine table");
  REQUIRE_EQ(lanes[0].domain.unit, "Hz", "the lane domain comes from the engine table");
  accepted(session->parameter_automation().set_native_automation_value(track, "prism.filter.cutoff",
                                                                       point, 800.0));
  REQUIRE_EQ(session->snapshot().tracks.front().automation[0].points[0].value, 800.0,
             "point value edits apply");
  require(!session->parameter_automation().set_native_automation_value(track, "prism.filter.cutoff",
                                                                       point, 1e6),
          "out-of-range automation is rejected");
  // Discrete and next-note parameters automate as held steps (ADR-0034).
  (void)take(session->parameter_automation().set_native_automation_point(track, "prism.osc1.wave",
                                                                         1, 1.0));
  (void)take(session->parameter_automation().set_native_automation_point(track, "prism.osc1.wave",
                                                                         3, 2.0));
  const auto& wave = session->snapshot().tracks.front().automation[1];
  REQUIRE_TRUE(wave.domain.discrete, "a discrete parameter gets a stepped lane of held points");
  REQUIRE_EQ(wave.domain.step_count, wave.domain.maximum - wave.domain.minimum,
             "a discrete parameter gets a stepped lane of held points");
  REQUIRE_EQ(wave.points.size(), 2, "a discrete parameter gets a stepped lane of held points");
  REQUIRE_EQ(wave.points[0].to_next, project::AutomationShape::hold,
             "a discrete parameter gets a stepped lane of held points");
  REQUIRE_EQ(wave.points[1].to_next, project::AutomationShape::hold,
             "a discrete parameter gets a stepped lane of held points");
  require(!session->parameter_automation().set_native_automation_point(track, "prism.osc1.wave", 2,
                                                                       1.5),
          "stepped lanes reject fractional values");
  (void)take(session->parameter_automation().set_native_automation_point(track, "prism.osc1.unison",
                                                                         1, 2.0));
  require(!session->parameter_automation().set_native_automation_point(track, "prism.nope", 1, 2.0),
          "unknown parameters are rejected");
  (void)take(session->parameter_automation().set_native_automation_point(
      track, "prism.filter.cutoff", 2, 3'000.0));
  REQUIRE_EQ(session->snapshot().tracks.front().automation[0].points.size(), 2,
             "a second bar adds a point to the same lane");
  accepted(session->parameter_automation().remove_native_automation_point(
      track, "prism.filter.cutoff", point));
  accepted(
      session->parameter_automation().remove_native_automation_lane(track, "prism.filter.cutoff"));
  accepted(session->parameter_automation().remove_native_automation_lane(track, "prism.osc1.wave"));
  accepted(
      session->parameter_automation().remove_native_automation_lane(track, "prism.osc1.unison"));
  require(session->snapshot().tracks.front().automation.empty(), "the lanes can be removed");
}

void synth_state_survives_save_and_reopen() {
  Directory directory;
  const auto root = directory.path() / "project";
  std::optional<project::TrackId> added;
  std::vector<double> saved;
  {
    auto session = take(application::ProjectSession::create(root));
    added = take(session->instruments().add_synth_track());
    const auto track = *added;
    accepted(session->instruments().select_native_instrument(track, "prism", preset("Wobble")));
    saved = take(session->instruments().native_instrument_values(track));
    saved[index("prism.filter.resonance")] = 0.6180339887498949;
    accepted(session->instruments().set_native_instrument_values(track, saved));
    (void)take(session->parameter_automation().set_native_automation_point(
        track, "prism.filter.cutoff", 1, 444.0));
    accepted(session->saving().save());
  }
  auto reopened = take(application::ProjectSession::open(root));
  REQUIRE_EQ(take(reopened->instruments().native_instrument_values(*added)), saved,
             "every synth value reopens bit for bit");
  REQUIRE_EQ(reopened->snapshot().tracks.front().automation.size(), 1, "synth automation reopens");
}

} // namespace

int main() {
  try {
    synth_track_defaults_edits_and_undo();
    documents_follow_the_engine();
    automation_points_follow_engine_rules();
    synth_state_survives_save_and_reopen();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "native instrument session contract passed\n";
  return 0;
}
