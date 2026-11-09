#include "oscillate/application/project_session.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/persistence/rack_file.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Racks and macro ranges (ADR-0059): a track's built-in devices and macros
// round-trip through a rack file with the rack's own slot ids, load onto
// another track with fresh slots and retargeted macros in one undo step,
// and malformed racks are refused without changing the project.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace persistence = oscillate::persistence;
namespace foundation = oscillate::foundation;
namespace native_inserts = oscillate::native_inserts;
namespace synth = oscillate::native_dsp::synth;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

void accepted(foundation::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-racks") {}
};

const project::Track& track_of(const application::ProjectSession& session, project::TrackId id) {
  const auto& tracks = session.snapshot().tracks;
  const auto found = std::ranges::find(tracks, id, &project::Track::id);
  REQUIRE_NE(found, tracks.end(), "track missing");
  return *found;
}

double synth_value(const application::ProjectSession& session, project::TrackId track,
                   std::string_view id) {
  const auto values = take(session.instruments().native_instrument_values(track));
  const auto* engine = synth::find_engine("prism");
  const auto index = oscillate::native_dsp::find_parameter(*engine->descriptor, id);
  REQUIRE_TRUE(index, "unknown prism parameter");
  return values[*index];
}

// A Prism track with Contour (switched off) and an arpeggiator, a macro on
// the synth's sub level and the filter's cutoff, named and turned halfway.
struct Source {
  project::TrackId track;
  project::InsertSlotId contour;
  project::InsertSlotId arp;
};

Source make_source(application::ProjectSession& session) {
  const auto track = take(session.instruments().add_native_instrument_track("prism", std::nullopt));
  const auto contour = take(session.effects().add_effect_engine(track, 0, "contour"));
  Source source{track, contour, take(session.track_devices().add_note_effect(track, 0, "arp"))};
  accepted(session.effects().set_effect_insert_enabled(source.track, source.contour, false));
  accepted(session.track_devices().add_macro_mapping(
      source.track, 1, project::NativeParameterTarget{source.track, "prism.sub.level"}, 0.0, 1.0));
  accepted(session.track_devices().add_macro_mapping(
      source.track, 1, project::NativeEffectParameterTarget{source.contour, "contour.cutoff"},
      200.0, 4'000.0));
  accepted(session.track_devices().set_macro_name(source.track, 1, "Weight"));
  accepted(session.track_devices().set_macro_value(source.track, 1, 0.5));
  return source;
}

void racks_round_trip_with_their_own_ids() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto source = make_source(*session);
  auto captured = take(session->track_devices().capture_track_rack(source.track));
  REQUIRE_EQ(captured.skipped, 0, "every device is built in");
  const auto& rack = captured.rack;
  REQUIRE_TRUE(rack.instrument,
               "the rack holds the synth, the effect, the note effect and the macros");
  REQUIRE_EQ(rack.instrument->engine, "prism",
             "the rack holds the synth, the effect, the note effect and the macros");
  REQUIRE_EQ(rack.effects.size(), 1,
             "the rack holds the synth, the effect, the note effect and the macros");
  REQUIRE_EQ(rack.note_effects.size(), 1,
             "the rack holds the synth, the effect, the note effect and the macros");
  REQUIRE_EQ(rack.macros.size(), 2,
             "the rack holds the synth, the effect, the note effect and the macros");
  require(!rack.effects.front().enabled, "a switched-off effect stays off");
  // Note effects take the first rack ids, then effects.
  require(rack.note_effects.front().id == persistence::rack_slot_id(1) &&
              rack.effects.front().id == persistence::rack_slot_id(2),
          "the rack names its own slots");
  const auto& weight = rack.macros[1];
  REQUIRE_EQ(weight.name, "Weight", "the macro keeps its name, value and mappings");
  REQUIRE_EQ(weight.value, 0.5, "the macro keeps its name, value and mappings");
  REQUIRE_EQ(weight.mappings.size(), 2, "the macro keeps its name, value and mappings");
  REQUIRE_EQ(std::get<project::NativeParameterTarget>(weight.mappings[0].destination).track,
             persistence::rack_track_id(), "the macro maps the rack's own synth and slot");
  REQUIRE_EQ(std::get<project::NativeEffectParameterTarget>(weight.mappings[1].destination).slot,
             persistence::rack_slot_id(2), "the macro maps the rack's own synth and slot");
  const auto text = persistence::encode_rack(rack);
  REQUIRE_EQ(take(persistence::decode_rack(text)), rack, "a rack file round-trips exactly");
}

void malformed_racks_are_refused() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto source = make_source(*session);
  const auto rack = take(session->track_devices().capture_track_rack(source.track)).rack;
  const auto text = persistence::encode_rack(rack);
  const auto refused = [](std::string_view candidate, std::string_view why) {
    require(!persistence::decode_rack(candidate), why);
  };
  refused("{}", "an empty object is not a rack");
  refused("not json", "text is not a rack");
  auto other_kind = text;
  other_kind.replace(other_kind.find("oscillate.rack"), 14, "oscillate.rock");
  refused(other_kind, "another kind is refused");
  auto extra = text;
  extra.insert(extra.find('{') + 1, "\"extra\": 1, ");
  refused(extra, "an unknown field is refused");
  require(!persistence::decode_rack(std::string(persistence::max_rack_bytes + 1, ' ')),
          "an oversized rack is refused");

  auto stray = rack;
  std::get<project::NativeEffectParameterTarget>(stray.macros[1].mappings[1].destination).slot =
      persistence::rack_slot_id(9);
  require(!persistence::validate_rack(stray), "a mapping to a slot the rack lacks is refused");
  require(!persistence::decode_rack(persistence::encode_rack(stray)),
          "a file mapping a missing slot is refused");
  auto twice = rack;
  twice.effects.front().id = twice.note_effects.front().id;
  require(!persistence::validate_rack(twice), "two slots with one id are refused");
  auto no_synth = rack;
  no_synth.instrument.reset();
  require(!persistence::validate_rack(no_synth), "a synth mapping without the synth is refused");
  auto convolution = rack;
  convolution.effects.front().processor = project::NativeConvolution{project::ManagedAudioReference{
      take(project::SourceId::from_bits({1, 1})), std::string(64, 'a'), 44, 48'000, 2, 1}};
  require(!persistence::validate_rack(convolution), "only built-in effects travel in racks");

  // A rack an engine rejects changes nothing.
  auto unknown = rack;
  std::get<project::NativeEffectPatch>(unknown.effects.front().processor).engine = "nothing";
  const auto target = take(session->instruments().add_synth_track());
  const auto before = session->snapshot().revision;
  REQUIRE_TRUE(!session->track_devices().load_track_rack(target, unknown),
               "an unknown engine leaves the project alone");
  REQUIRE_EQ(session->snapshot().revision, before, "an unknown engine leaves the project alone");
}

void loaded_racks_get_fresh_slots_and_macros() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto source = make_source(*session);
  const auto rack = take(session->track_devices().capture_track_rack(source.track)).rack;

  // The target already has an effect of its own, a lane on the synth's sub
  // level, and a macro the rack replaces.
  const auto target =
      take(session->instruments().add_native_instrument_track("prism", std::nullopt));
  const auto old_effect = take(session->effects().add_effect_engine(target, 0, "press"));
  (void)take(session->automation().add_automation_point(
      target, project::NativeParameterTarget{target, "prism.sub.level"}, 0, 0.75));
  accepted(session->track_devices().set_macro_name(target, 5, "Gone"));
  const auto revision = session->snapshot().revision;
  const auto serial = session->snapshot().next_insert_slot_serial;
  accepted(session->track_devices().load_track_rack(target, rack));
  REQUIRE_EQ(session->snapshot().revision, revision + 1, "loading a rack is one revision");

  const auto& loaded = track_of(*session, target);
  REQUIRE_EQ(loaded.effects.size(), 1, "the rack's devices replace the track's built-in ones");
  REQUIRE_EQ(loaded.note_effects.size(), 1, "the rack's devices replace the track's built-in ones");
  const auto contour = loaded.effects.front().id;
  const auto arp = loaded.note_effects.front().id;
  REQUIRE_NE(contour, old_effect, "loaded devices get fresh slots");
  REQUIRE_NE(contour, source.contour, "loaded devices get fresh slots");
  REQUIRE_NE(arp, source.arp, "loaded devices get fresh slots");
  REQUIRE_NE(contour, persistence::rack_slot_id(2), "loaded devices get fresh slots");
  REQUIRE_GE(contour.bits().low, serial, "loaded devices get fresh slots");
  REQUIRE_GE(arp.bits().low, serial, "loaded devices get fresh slots");
  require(!loaded.effects.front().enabled, "an effect saved off loads off");
  REQUIRE_EQ(loaded.macros.size(), 2, "the rack's macros replace the track's");
  REQUIRE_EQ(loaded.macros[1].name, "Weight", "the rack's macros replace the track's");
  REQUIRE_EQ(loaded.macros[1].value, 0.5, "the rack's macros replace the track's");
  const auto& mappings = loaded.macros[1].mappings;
  REQUIRE_EQ(std::get<project::NativeParameterTarget>(mappings[0].destination).track, target,
             "the macros drive the new track's synth and slot");
  REQUIRE_EQ(std::get<project::NativeEffectParameterTarget>(mappings[1].destination).slot, contour,
             "the macros drive the new track's synth and slot");
  require(std::ranges::none_of(loaded.automation,
                               [](const project::AutomationLane& lane) {
                                 return std::holds_alternative<project::NativeParameterTarget>(
                                     lane.target);
                               }),
          "a lane on a parameter the macro now drives goes");
  REQUIRE_LT(std::abs(synth_value(*session, target, "prism.sub.level") -
                      synth_value(*session, source.track, "prism.sub.level")),
             tolerance::double_accumulation, "the synth sounds as it did on the source track");

  // The new macro turns the new devices.
  accepted(session->track_devices().set_macro_value(target, 1, 1.0));
  const auto cutoff_index = take(native_inserts::native_effect_parameter_index(
      *take(native_inserts::resolve_native_effect(
                std::get<project::NativeEffectPatch>(loaded.effects.front().processor)))
           .engine,
      "contour.cutoff"));
  REQUIRE_LT(std::abs(take(session->effects().native_effect_values(target, contour))[cutoff_index] -
                      4'000.0),
             tolerance::float_round_trip, "the loaded macro drives the loaded effect");

  accepted(session->history().undo());
  accepted(session->history().undo());
  const auto& restored = track_of(*session, target);
  REQUIRE_EQ(restored.effects.size(), 1, "one undo takes the whole rack away");
  REQUIRE_EQ(restored.effects.front().id, old_effect, "one undo takes the whole rack away");
  REQUIRE_TRUE(restored.note_effects.empty(), "one undo takes the whole rack away");
  REQUIRE_EQ(restored.macros.size(), 6, "one undo takes the whole rack away");
  REQUIRE_EQ(restored.macros[5].name, "Gone", "one undo takes the whole rack away");
  REQUIRE_TRUE(!restored.automation.empty(), "one undo takes the whole rack away");

  // Note effects need a built-in synth to play into.
  const auto audio = take(session->tracks().add_audio_track());
  require(!session->track_devices().load_track_rack(audio, rack),
          "a synth rack does not load on an audio track");
}

void mapping_ranges_move_their_parameters() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto source = make_source(*session);
  const project::MacroDestination sub =
      project::NativeParameterTarget{source.track, "prism.sub.level"};
  accepted(session->track_devices().set_macro_mapping_range(source.track, 1, sub, 0.2, 0.6, 7));
  accepted(session->track_devices().set_macro_mapping_range(source.track, 1, sub, 0.2, 0.8, 7));
  REQUIRE_LT(std::abs(synth_value(*session, source.track, "prism.sub.level") - 0.5),
             tolerance::double_accumulation,
             "the parameter moves to the new range at the macro's value");
  const auto& mapping = track_of(*session, source.track).macros[1].mappings[0];
  REQUIRE_EQ(mapping.minimum, 0.2, "the range is saved");
  REQUIRE_EQ(mapping.maximum, 0.8, "the range is saved");
  require(!session->track_devices().set_macro_mapping_range(source.track, 1, sub, -1.0, 0.8),
          "a range outside the parameter's is refused");
  require(!session->track_devices().set_macro_mapping_range(source.track, 0, sub, 0.2, 0.8),
          "a macro that does not map the parameter is refused");
  accepted(session->history().undo());
  const auto& undone = track_of(*session, source.track).macros[1].mappings[0];
  REQUIRE_EQ(undone.minimum, 0.0, "one gesture undoes in one step");
  REQUIRE_EQ(undone.maximum, 1.0, "one gesture undoes in one step");
  REQUIRE_LT(std::abs(synth_value(*session, source.track, "prism.sub.level") - 0.5),
             tolerance::double_accumulation, "one gesture undoes in one step");
}

} // namespace

int main() {
  try {
    racks_round_trip_with_their_own_ids();
    malformed_racks_are_refused();
    loaded_racks_get_fresh_slots_and_macros();
    mapping_ranges_move_their_parameters();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "track rack contract passed\n";
  return 0;
}
