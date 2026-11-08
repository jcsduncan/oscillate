#include "oscillate/application/project_session.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/effect_stage.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// ADR-0043: built-in effects in the session — add from the registry, edit by
// dense values with gesture coalescing, keep their engine, automate, and
// take their lanes with them when removed; all undoable.
namespace {
using namespace oscillate;
namespace fs = std::filesystem;
namespace effects = oscillate::native_dsp::effects;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

using oscillate::test::take;

void accepted(foundation::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-effect-session") {}
};

const effects::EffectEngine& contour() { return *effects::find_engine("contour"); }

const project::NativeEffectPatch& patch(const application::ProjectSession& session,
                                        project::TrackId track) {
  for (const auto& item : session.snapshot().tracks) {
    if (item.id == track)
      return std::get<project::NativeEffectPatch>(item.effects.front().processor);
  }
  throw std::runtime_error("missing track");
}

void effects_are_added_edited_and_undone() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  require(!session->effects().add_effect_engine(track, 0, "no-such-effect"),
          "unknown engines are refused");
  const auto slot = take(session->effects().add_effect_engine(track, 0, "contour"));
  REQUIRE_EQ(patch(*session, track).engine, "contour", "the default patch stores no values");
  REQUIRE_TRUE(patch(*session, track).parameters.empty(), "the default patch stores no values");
  const auto defaults = take(session->effects().native_effect_values(track, slot));
  REQUIRE_EQ(defaults, synth::default_values(*contour().descriptor),
             "values resolve to the engine defaults");

  // Two steps of one gesture are one undo step.
  const auto cutoff = *synth::find_parameter(*contour().descriptor, "contour.cutoff");
  auto values = defaults;
  values[cutoff] = 400.0;
  accepted(session->effects().set_native_effect_values(track, slot, values, 7));
  values[cutoff] = 300.0;
  accepted(session->effects().set_native_effect_values(track, slot, values, 7));
  REQUIRE_EQ(patch(*session, track).parameters.size(), 1, "only the non-default value is stored");
  REQUIRE_EQ(patch(*session, track).parameters.front().value, 300.0,
             "only the non-default value is stored");
  accepted(session->effects().set_native_effect_values(track, slot, values, 8));
  accepted(session->history().undo());
  require(patch(*session, track).parameters.empty(), "the gesture undoes as one step");
  accepted(session->history().redo());

  // A slot keeps its engine.
  const project::NativeEffectPatch other{"orbit", 1, {}, 1};
  require(!session->effects().set_native_effect(track, slot, other),
          "a slot's engine cannot change");

  // Master effects may repeat.
  const auto master = project::SignalPathId{project::MasterSignalPath{}};
  (void)take(session->effects().add_effect_engine(master, 0, "vista"));
  (void)take(session->effects().add_effect_engine(master, 1, "vista", 1));
  REQUIRE_EQ(session->snapshot().master_effects.size(), 2, "two Vistas sit on the master");
}

void automation_follows_its_slot() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  const auto slot = take(session->effects().add_effect_engine(track, 0, "contour"));
  require(!session->parameter_automation().set_effect_automation_point(track, slot,
                                                                       "contour.cutoff", 0, 1e9),
          "out-of-range points are refused");
  (void)take(session->parameter_automation().set_effect_automation_point(
      track, slot, "contour.cutoff", 0, 200.0));
  (void)take(session->parameter_automation().set_effect_automation_point(
      track, slot, "contour.cutoff", 1, 4'000.0));
  const auto lanes = [&] { return session->snapshot().tracks.front().automation.size(); };
  REQUIRE_EQ(lanes(), 1, "one lane with two points");
  REQUIRE_EQ(session->snapshot().tracks.front().automation.front().points.size(), 2,
             "one lane with two points");
  const auto& model = contour().descriptor->parameters;
  for (const auto& parameter : model) {
    if (parameter.automatable && synth::is_discrete(parameter)) {
      (void)take(session->parameter_automation().set_effect_automation_point(
          track, slot, parameter.id, 0, parameter.minimum));
      require(session->snapshot().tracks.front().automation.back().domain.discrete,
              "discrete parameters get stepped lanes");
      break;
    }
  }
  const auto with_lanes = lanes();
  accepted(session->effects().remove_effect_insert(track, slot));
  REQUIRE_EQ(lanes(), 0, "removing the slot removes its lanes");
  accepted(session->history().undo());
  REQUIRE_EQ(lanes(), with_lanes, "undo restores the slot and its lanes");
}

// ADR-0058: Pump's drawn shape is the effect's document. It arrives with
// the defaults and presets, survives value edits, edits as one undoable
// gesture, and is checked by the engine and the model.
void documents_travel_with_the_effect() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  const auto slot = take(session->effects().add_effect_engine(track, 0, "pump"));
  const auto classic = take(session->effects().native_effect_document(track, slot));
  REQUIRE_EQ(classic.curves.size(), 1, "a new Pump starts with Classic Pump's shape");
  REQUIRE_EQ(classic.curves.front().id, "shape", "a new Pump starts with Classic Pump's shape");

  auto values = take(session->effects().native_effect_values(track, slot));
  values[*synth::find_parameter(*effects::find_engine("pump")->descriptor, "pump.depth")] = 0.5;
  accepted(session->effects().set_native_effect_values(track, slot, values));
  REQUIRE_EQ(take(session->effects().native_effect_document(track, slot)), classic,
             "a value edit keeps the shape");

  auto drawn = classic;
  drawn.curves.front().points = {{0.0, 0.0}, {0.5, 1.0}, {1.0, 1.0}};
  accepted(session->effects().set_native_effect_document(track, slot, drawn, 9));
  drawn.curves.front().points[1].x = 0.4;
  accepted(session->effects().set_native_effect_document(track, slot, drawn, 9));
  REQUIRE_EQ(take(session->effects().native_effect_document(track, slot)), drawn,
             "the drawn shape is saved");
  accepted(session->history().undo());
  REQUIRE_EQ(take(session->effects().native_effect_document(track, slot)), classic,
             "the drag undoes as one step");

  auto wrong = drawn;
  wrong.curves.front().id = "lfo";
  require(!session->effects().set_native_effect_document(track, slot, wrong),
          "the engine checks meaning");
  wrong = drawn;
  wrong.curves.front().points.front().y = 2.0;
  require(!session->effects().set_native_effect_document(track, slot, wrong),
          "the model checks structure");
  const auto filter = take(session->effects().add_effect_engine(track, 1, "contour"));
  require(!session->effects().set_native_effect_document(track, filter, drawn),
          "effects without documents refuse one");

  const auto& presets = effects::find_engine("pump")->descriptor->presets;
  std::size_t gate = 0;
  while (presets[gate].name != "Hard Gate")
    ++gate;
  accepted(session->effects().set_native_effect_preset(track, slot, gate));
  const auto gated = take(session->effects().native_effect_document(track, slot));
  REQUIRE_EQ(gated.curves.front().points.size(), 4, "a preset brings its shape");
  REQUIRE_EQ(gated.curves.front().points[1].x, 0.18, "a preset brings its shape");
}

// Pump keyed by a kick's MIDI notes (ADR-0058), and what it plays.
constexpr std::size_t pump_block_frames = 128;
constexpr std::uint8_t kick_key = 36;
constexpr std::size_t hard_gate_preset = 3;
constexpr double midi_trigger = 2.0;
// The duck is down this far into the kick note, and nothing ducks this far
// before it.
constexpr std::size_t duck_settle_frames = 96;
constexpr std::size_t before_note_frames = 480;
constexpr std::uint64_t after_note_frames = 4'800;
constexpr float unducked_floor = 0.99F;
constexpr float ducked_ceiling = 0.05F;

struct KeyedPump {
  std::unique_ptr<application::ProjectSession> session;
  project::TrackId kick;
  project::TrackId bass;
  project::TrackId pad;
  project::InsertSlotId slot;
  project::SendId key;
  std::size_t note;
};

KeyedPump keyed_pump(const fs::path& root) {
  auto session = take(application::ProjectSession::create(root));
  const auto kick = take(session->instruments().add_synth_track());
  const auto bass = take(session->instruments().add_synth_track());
  const auto pad = take(session->instruments().add_synth_track());
  accepted(session->midi_clips().add_midi_clip(kick, {0}));
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  accepted(session->notes().add_note(clip, {timeline::ticks_per_quarter * 3 / 2}, kick_key));
  const auto slot = take(session->effects().add_effect_engine(bass, 0, "pump", hard_gate_preset));
  const auto key = take(session->routing().add_sidechain(kick, bass, slot, 1.0));
  auto values = take(session->effects().native_effect_values(bass, slot));
  const auto trigger_index =
      synth::find_parameter(*effects::find_engine("pump")->descriptor, "pump.trigger");
  REQUIRE_TRUE(trigger_index.has_value(), "Pump has a trigger parameter");
  values[*trigger_index] = midi_trigger;
  accepted(session->effects().set_native_effect_values(bass, slot, values));
  std::size_t note = 0;
  const auto plan = take(engine::compile_midi_events(session->snapshot()));
  for (const auto& instrument : plan.instruments()) {
    if (instrument.track() == kick)
      note = static_cast<std::size_t>(instrument.events().front().sample.value);
  }
  REQUIRE_GT(note, before_note_frames, "the kick note is on the timeline");
  return {std::move(session), kick, bass, pad, slot, key, note};
}

std::unique_ptr<native_inserts::NativeEffectStage> pump_stage(const KeyedPump& pump) {
  const auto& snapshot = pump.session->snapshot();
  const auto bass = std::ranges::find(snapshot.tracks, pump.bass, &project::Track::id);
  return take(native_inserts::NativeEffectStage::create(
      snapshot, pump.slot, std::get<project::NativeEffectPatch>(bass->effects.front().processor),
      {}, {.max_frames = pump_block_frames}));
}

// Unit input through the stage from the start, past the kick note.
std::vector<engine::StereoFrame> play(native_inserts::NativeEffectStage& stage,
                                      const KeyedPump& pump, std::uint64_t generation) {
  std::vector<engine::StereoFrame> out;
  for (std::uint64_t position = 0; position < pump.note + after_note_frames;
       position += pump_block_frames) {
    std::vector<engine::StereoFrame> block(pump_block_frames, engine::StereoFrame{1.0F, 1.0F});
    stage.process(block, {position, generation});
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

bool ducks(native_inserts::NativeEffectStage& stage, const KeyedPump& pump,
           std::uint64_t generation = 1) {
  const auto out = play(stage, pump, generation);
  REQUIRE_GT(out[pump.note - before_note_frames].left, unducked_floor,
             "nothing ducks before the kick note");
  return out[pump.note + duck_settle_frames].left < ducked_ceiling;
}

// ADR-0058: Pump's MIDI trigger follows the notes of the track keying it,
// read from the project when the stage is prepared and again on every
// publish, so the duck lands on each hit with no lookahead.
void pump_follows_its_key_tracks_notes() {
  Directory directory;
  const auto pump = keyed_pump(directory.path() / "song");
  auto stage = pump_stage(pump);
  require(ducks(*stage, pump), "the duck is down 2 ms into the kick note");
}

// Issue #245: only a key you can hear keys Pump's MIDI trigger. Its notes
// follow the mixer's mute and solo (a soloed bass still ducks to its kick)
// and the key's gain, and a kept stage picks up the change.
void pump_hears_only_audible_keys() {
  Directory directory;
  const auto pump = keyed_pump(directory.path() / "song");
  auto& session = *pump.session;
  accepted(session.tracks().set_track_muted(pump.kick, true));
  auto stage = pump_stage(pump);
  require(!ducks(*stage, pump), "a muted kick keys nothing");

  accepted(session.tracks().set_track_muted(pump.kick, false));
  accepted(stage->refresh_key_notes(session.snapshot()));
  constexpr std::uint64_t after_unmute = 2;
  require(ducks(*stage, pump, after_unmute), "unmuting the kick brings the ducks back");

  accepted(session.tracks().set_track_soloed(pump.pad, true));
  require(!ducks(*pump_stage(pump), pump), "soloing another track silences the kick's key");
  accepted(session.tracks().set_track_soloed(pump.pad, false));

  accepted(session.tracks().set_track_soloed(pump.bass, true));
  require(ducks(*pump_stage(pump), pump), "a soloed bass still ducks to its kick");
  accepted(session.tracks().set_track_soloed(pump.bass, false));

  accepted(session.routing().set_send_gain(pump.key, 0.0));
  require(!ducks(*pump_stage(pump), pump), "a key at no gain keys nothing");
}

// Issue #248, ADR-0094: a mute lane on the kick drops its notes wherever it
// mutes the kick. The lane, not the kick's mute switch, decides.
void pump_follows_its_keys_mute_lane() {
  constexpr auto mute = project::TrackControlTarget::Key::channel_mute;
  constexpr double lane_muted = 1.0;
  constexpr double lane_unmuted = 0.0;
  Directory directory;
  const auto pump = keyed_pump(directory.path() / "song");
  auto& session = *pump.session;
  const auto point = take(
      session.parameter_automation().set_channel_control_point(pump.kick, mute, 0, lane_muted));
  auto stage = pump_stage(pump);
  require(!ducks(*stage, pump), "a kick its lane mutes keys nothing");

  accepted(session.parameter_automation().set_channel_control_value(pump.kick, mute, point,
                                                                    lane_unmuted));
  accepted(stage->refresh_key_notes(session.snapshot()));
  constexpr std::uint64_t after_unmute = 2;
  require(ducks(*stage, pump, after_unmute), "a kick its lane leaves audible keys Pump");

  accepted(session.tracks().set_track_muted(pump.kick, true));
  require(ducks(*pump_stage(pump), pump), "the lane, not the mute switch, decides");
}

// ADR-0058: linked Pumps share their shape; knobs stay their own. One edit,
// preset or link is one undo step, and the link survives a reopen.
void linked_effects_share_their_shape() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(application::ProjectSession::create(root));
  const auto pad = take(session->instruments().add_synth_track());
  const auto bass = take(session->instruments().add_synth_track());
  const auto keys = take(session->instruments().add_synth_track());
  const auto a = take(session->effects().add_effect_engine(pad, 0, "pump"));
  const auto b = take(session->effects().add_effect_engine(bass, 0, "pump", 3)); // Hard Gate.
  const auto c = take(session->effects().add_effect_engine(keys, 0, "pump"));
  const auto filter = take(session->effects().add_effect_engine(keys, 1, "contour"));
  REQUIRE_EQ(session->effects().native_effect_link_count(a), 1, "a lone Pump is a group of one");
  const std::vector<project::InsertSlotId> mixed{a, filter};
  require(!session->effects().link_native_effects(mixed), "only one engine links");

  const auto before = session->snapshot().revision;
  const std::vector<project::InsertSlotId> group{a, b, c};
  accepted(session->effects().link_native_effects(group));
  REQUIRE_EQ(session->snapshot().revision, before + 1, "linking is one step");
  const auto shared = take(session->effects().native_effect_document(pad, a));
  REQUIRE_EQ(take(session->effects().native_effect_document(bass, b)), shared,
             "the group takes the first one's shape");
  REQUIRE_EQ(session->effects().native_effect_link_count(b), 3,
             "the group takes the first one's shape");

  auto drawn = shared;
  drawn.curves.front().points = {{0.0, 0.0}, {0.3, 1.0}, {1.0, 1.0}};
  accepted(session->effects().set_native_effect_document(bass, b, drawn, 4));
  REQUIRE_EQ(take(session->effects().native_effect_document(pad, a)), drawn,
             "a drawn edit reaches every linked Pump");
  REQUIRE_EQ(take(session->effects().native_effect_document(keys, c)), drawn,
             "a drawn edit reaches every linked Pump");
  auto values = take(session->effects().native_effect_values(pad, a));
  values[*synth::find_parameter(*effects::find_engine("pump")->descriptor, "pump.depth")] = 0.25;
  accepted(session->effects().set_native_effect_values(pad, a, values));
  REQUIRE_NE(take(session->effects().native_effect_values(bass, b)), values,
             "knobs stay per instance and keep the link");
  REQUIRE_EQ(session->effects().native_effect_link_count(a), 3,
             "knobs stay per instance and keep the link");
  accepted(session->history().undo());
  accepted(session->history().undo());
  REQUIRE_EQ(take(session->effects().native_effect_document(pad, a)), shared,
             "one undo restores the whole group's shape");
  REQUIRE_EQ(take(session->effects().native_effect_document(keys, c)), shared,
             "one undo restores the whole group's shape");

  accepted(session->effects().unlink_native_effect(keys, c));
  REQUIRE_EQ(session->effects().native_effect_link_count(c), 1, "unlinking leaves the rest linked");
  REQUIRE_EQ(session->effects().native_effect_link_count(a), 2, "unlinking leaves the rest linked");
  accepted(session->effects().set_native_effect_document(pad, a, drawn));
  REQUIRE_EQ(take(session->effects().native_effect_document(keys, c)), shared,
             "an unlinked Pump keeps its own shape");
  accepted(session->saving().save());
  session.reset();
  auto reopened = take(application::ProjectSession::open(root));
  REQUIRE_EQ(reopened->effects().native_effect_link_count(b), 2, "the link survives a reopen");
  REQUIRE_EQ(take(reopened->effects().native_effect_document(bass, b)), drawn,
             "the link survives a reopen");
  accepted(reopened->effects().unlink_native_effect(bass, b));
  REQUIRE_EQ(reopened->effects().native_effect_link_count(a), 1,
             "the last one left is unlinked with it");
}

fs::path render_wav(const fs::path& root, std::uint32_t rate, std::uint32_t frames);

// The saved patch of a built-in effect, wherever it sits.
const project::NativeEffectPatch& saved_patch(const project::Snapshot& snapshot,
                                              project::InsertSlotId slot) {
  for (const auto& track : snapshot.tracks)
    for (const auto& effect : track.effects)
      if (effect.id == slot)
        return std::get<project::NativeEffectPatch>(effect.processor);
  throw std::runtime_error("effect not found");
}

std::string saved_link(const project::Snapshot& snapshot, project::InsertSlotId slot) {
  return saved_patch(snapshot, slot).link;
}

// An effect left alone in its group, by linking its partners elsewhere or
// removing them (or their track), is unlinked in the same step; undo
// brings the group back.
void lone_links_are_released() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto pad = take(session->instruments().add_synth_track());
  const auto bass = take(session->instruments().add_synth_track());
  const auto keys = take(session->instruments().add_synth_track());
  const auto a = take(session->effects().add_effect_engine(pad, 0, "pump"));
  const auto b = take(session->effects().add_effect_engine(bass, 0, "pump"));
  const auto c = take(session->effects().add_effect_engine(keys, 0, "pump"));
  const std::vector<project::InsertSlotId> first{a, b};
  const std::vector<project::InsertSlotId> second{b, c};
  constexpr std::size_t pair = 2;

  accepted(session->effects().link_native_effects(first));
  const auto generation = saved_patch(session->snapshot(), a).generation;
  accepted(session->effects().link_native_effects(second));
  REQUIRE_TRUE(saved_link(session->snapshot(), a).empty(),
               "relinking a partner elsewhere unlinks the one left behind");
  REQUIRE_GT(saved_patch(session->snapshot(), a).generation, generation,
             "releasing a lone link advances its generation");
  REQUIRE_EQ(session->effects().native_effect_link_count(b), pair,
             "relinking a partner elsewhere unlinks the one left behind");
  accepted(session->history().undo());
  REQUIRE_EQ(session->effects().native_effect_link_count(a), pair, "undo restores the group");

  accepted(session->effects().remove_effect_insert(bass, b));
  REQUIRE_TRUE(saved_link(session->snapshot(), a).empty(),
               "removing a partner unlinks the one left behind");
  accepted(session->history().undo());
  REQUIRE_EQ(session->effects().native_effect_link_count(a), pair, "undo restores the group");

  accepted(session->tracks().remove_track(bass));
  REQUIRE_TRUE(saved_link(session->snapshot(), a).empty(),
               "removing a partner's track unlinks the one left behind");
  REQUIRE_TRUE(saved_link(session->snapshot(), c).empty(), "an unlinked effect is untouched");

  // A partner left on a frozen track keeps its link rather than blocking
  // the removal.
  accepted(session->history().undo());
  constexpr std::uint32_t render_frames = 64;
  const auto path = render_wav(directory.path(), session->snapshot().sample_rate, render_frames);
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "render open failed");
  auto stage = session->freeze().begin_track_freeze(pad, fd);
  (void)::close(fd);
  auto frozen = take(std::move(stage));
  accepted(session->freeze().finish_track_freeze(take(std::move(frozen.task).run()),
                                                 frozen.origin_revision, pad));
  const auto kept = saved_link(session->snapshot(), a);
  REQUIRE_TRUE(!kept.empty(), "the frozen partner is linked before the removal");
  accepted(session->effects().remove_effect_insert(bass, b));
  REQUIRE_EQ(saved_link(session->snapshot(), a), kept, "a frozen partner keeps its link");
}

// ADR-0059: MIDI effects sit before an instrument track's synth, in order,
// with their own ids from the effect slots' allocator; every edit undoes.
void note_effects_sit_before_the_synth() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  const auto audio = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  require(!session->track_devices().add_note_effect(audio, 0, "arp"),
          "only instrument tracks take note effects");
  const auto arp = take(session->track_devices().add_note_effect(track, 0, "arp"));
  const auto chord = take(session->track_devices().add_note_effect(track, 0, "chord"));
  const auto effects = [&] { return session->snapshot().tracks.front().note_effects; };
  REQUIRE_EQ(effects().size(), 2, "note effects keep their order and ids");
  REQUIRE_EQ(effects()[0].id, chord, "note effects keep their order and ids");
  REQUIRE_EQ(effects()[1].id, arp, "note effects keep their order and ids");
  REQUIRE_EQ(effects()[0].engine, "chord", "note effects keep their order and ids");
  REQUIRE_NE(arp, chord, "note effects keep their order and ids");
  require(!session->track_devices().add_note_effect(track, 0, "Not An Engine"),
          "engines are native identifiers");
  accepted(session->track_devices().move_note_effect(track, chord, 1));
  REQUIRE_EQ(effects()[1].id, chord, "a note effect moves");
  accepted(session->track_devices().set_note_effect_enabled(track, arp, false));
  require(!effects()[0].enabled, "a note effect switches off");
  accepted(session->track_devices().set_note_effect_parameters(track, arp, {{"arp.rate", 2.0}}, 9));
  accepted(session->track_devices().set_note_effect_parameters(track, arp, {{"arp.rate", 5.0}}, 9));
  REQUIRE_EQ(effects()[0].parameters.front().value, 5.0, "values are saved");
  // Values the engine does not know, or out of range, are refused; a
  // default is saved by leaving it out.
  const bool unknown_refused =
      !session->track_devices().set_note_effect_parameters(track, arp, {{"arp.nonsense", 1.0}});
  const bool out_of_range_refused =
      !session->track_devices().set_note_effect_parameters(track, arp, {{"arp.octaves", 9.0}});
  require(unknown_refused && out_of_range_refused, "the engine checks its values");
  accepted(session->history().undo());
  require(effects()[0].parameters.empty(), "one gesture is one undo step");
  // A macro maps a note effect's parameter with the engine's domain.
  const project::MacroDestination gate = project::NativeEffectParameterTarget{arp, "arp.gate"};
  require(!session->track_devices().add_macro_mapping(track, 0, gate, 0.0, 1.0),
          "the range stays inside the parameter's");
  accepted(session->track_devices().add_macro_mapping(track, 0, gate, 0.25, 1.75));
  accepted(session->track_devices().set_macro_value(track, 0, 1.0));
  REQUIRE_EQ(effects()[0].parameters.size(), 1, "turning the macro sets the note effect's value");
  REQUIRE_EQ(effects()[0].parameters.front().id, "arp.gate",
             "turning the macro sets the note effect's value");
  REQUIRE_EQ(effects()[0].parameters.front().value, 1.75,
             "turning the macro sets the note effect's value");
  accepted(session->history().undo());
  accepted(session->history().undo());
  const auto copy = take(session->tracks().duplicate_track(track));
  const auto& copied = session->snapshot().tracks[1];
  REQUIRE_EQ(copied.id, copy, "a duplicate takes fresh note effect slots");
  REQUIRE_EQ(copied.note_effects.size(), 2, "a duplicate takes fresh note effect slots");
  REQUIRE_NE(copied.note_effects[0].id, arp, "a duplicate takes fresh note effect slots");
  REQUIRE_EQ(copied.note_effects[0].engine, "arp", "a duplicate takes fresh note effect slots");
  REQUIRE_TRUE(!copied.note_effects[0].enabled, "a duplicate takes fresh note effect slots");
  accepted(session->history().undo());
  accepted(session->track_devices().remove_note_effect(track, arp));
  REQUIRE_EQ(effects().size(), 1, "a note effect is removed");
  accepted(session->history().undo());
  REQUIRE_EQ(effects().size(), 2, "undo brings it back");
}

// ADR-0059: a macro turns every parameter it maps, in the same command; a
// lane on the macro is prepared as a lane on each mapped parameter.
void macros_drive_mapped_parameters() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  const auto slot = take(session->effects().add_effect_engine(track, 0, "contour"));
  const auto cutoff_index =
      take(native_inserts::native_effect_parameter_index(contour(), "contour.cutoff"));
  const auto cutoff = [&] {
    return take(session->effects().native_effect_values(track, slot))[cutoff_index];
  };
  const project::MacroDestination destination =
      project::NativeEffectParameterTarget{slot, "contour.cutoff"};
  (void)take(session->parameter_automation().set_effect_automation_point(
      track, slot, "contour.cutoff", 0, 300.0));
  require(!session->track_devices().add_macro_mapping(track, 0, destination, 200.0, 4'000.0),
          "a parameter with its own lane cannot be mapped");
  accepted(
      session->parameter_automation().remove_effect_automation_lane(track, slot, "contour.cutoff"));
  require(!session->track_devices().add_macro_mapping(track, 8, destination, 200.0, 4'000.0),
          "a track has eight macros");
  require(!session->track_devices().add_macro_mapping(track, 0, destination, 0.0, 1e9),
          "the range stays inside the parameter's");
  accepted(session->track_devices().add_macro_mapping(track, 2, destination, 200.0, 4'000.0));
  const auto& macros = session->snapshot().tracks.front().macros;
  REQUIRE_EQ(macros.size(), 3,
             "mapping adds the macros up to it and sets the parameter from the macro");
  REQUIRE_EQ(macros[2].mappings.size(), 1,
             "mapping adds the macros up to it and sets the parameter from the macro");
  REQUIRE_EQ(macros[2].mappings.front().domain.semantic_key, "contour.cutoff",
             "mapping adds the macros up to it and sets the parameter from the macro");
  REQUIRE_LT(std::abs(cutoff() - 200.0), tolerance::float_round_trip,
             "mapping adds the macros up to it and sets the parameter from the macro");
  require(!session->track_devices().add_macro_mapping(track, 1, destination, 200.0, 4'000.0),
          "a parameter has one macro");
  accepted(session->track_devices().set_macro_value(track, 2, 1.0, 4));
  REQUIRE_LT(std::abs(cutoff() - 4'000.0), tolerance::float_round_trip,
             "the macro's top is the mapping's maximum");
  accepted(session->track_devices().set_macro_value(track, 2, 0.5, 4));
  const auto& mapping = session->snapshot().tracks.front().macros[2].mappings.front();
  REQUIRE_LT(std::abs(cutoff() - automation_project::macro_parameter_value(mapping, 0.5)),
             tolerance::double_accumulation,
             "the parameter follows the macro through the domain's scale");
  accepted(session->history().undo());
  REQUIRE_EQ(session->snapshot().tracks.front().macros[2].value, 0.0,
             "a macro gesture undoes in one step with its parameters");
  REQUIRE_LT(std::abs(cutoff() - 200.0), tolerance::float_round_trip,
             "a macro gesture undoes in one step with its parameters");
  accepted(session->track_devices().set_macro_name(track, 2, "Brightness"));
  REQUIRE_EQ(session->snapshot().tracks.front().macros[2].name, "Brightness", "macros are named");

  // A lane on the macro drives the cutoff; no prepared lane targets a macro.
  (void)take(
      session->automation().add_automation_point(track, project::MacroTarget{track, 2}, 0, 1.0));
  const auto& state = session->snapshot();
  auto prepared = take(automation_project::prepare_project_automation(state, state.revision));
  REQUIRE_EQ(prepared.size(), 1, "a macro lane is prepared on its mapped parameter");
  REQUIRE_TRUE(std::holds_alternative<project::NativeEffectParameterTarget>(prepared[0].target()),
               "a macro lane is prepared on its mapped parameter");
  std::vector<double> values(8);
  REQUIRE_TRUE(prepared[0].evaluate_block({0}, values), "the prepared lane plays the mapped value");
  REQUIRE_LT(std::abs(values[0] - 4'000.0), tolerance::float_round_trip,
             "the prepared lane plays the mapped value");

  // Removing the effect drops its mapping; undo restores both.
  accepted(session->effects().remove_effect_insert(track, slot));
  require(session->snapshot().tracks.front().macros[2].mappings.empty(),
          "a mapping goes with its effect");
  accepted(session->history().undo());
  REQUIRE_EQ(session->snapshot().tracks.front().macros[2].mappings.size(), 1,
             "undo brings the mapping back");
  accepted(session->track_devices().remove_macro_mapping(track, 2, destination));
  require(session->snapshot().tracks.front().macros[2].mappings.empty(), "a mapping is removed");
}

void put16(std::vector<char>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(value & 0xffU));
  bytes.push_back(static_cast<char>(value >> 8U));
}

void put32(std::vector<char>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value & 0xffffU));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// A stereo 16-bit render at `rate`.
fs::path render_wav(const fs::path& root, std::uint32_t rate, std::uint32_t frames) {
  const auto path = root / "render.wav";
  std::vector<char> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 2);
  put32(bytes, rate);
  put32(bytes, rate * 4);
  put16(bytes, 4);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 4);
  for (std::uint32_t sample = 0; sample < frames * 2; ++sample)
    put16(bytes, 4'096);
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "render write failed");
  return path;
}

// ADR-0059: a frozen track plays its render; what was rendered cannot change
// until it is unfrozen, while its channel stays live.
void frozen_tracks_keep_their_sound() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  const auto slot = take(session->effects().add_effect_engine(track, 0, "contour"));
  const auto rate = session->snapshot().sample_rate;
  const auto path = render_wav(directory.path(), rate, 64);
  const auto freeze = [&] {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "render open failed");
    auto stage = session->freeze().begin_track_freeze(track, fd);
    (void)::close(fd);
    return take(std::move(stage));
  };

  // A render made before an audible edit is stale.
  auto stale = freeze();
  auto stale_render = take(std::move(stale.task).run());
  accepted(session->tracks().set_track_level_pan(track, 0.5, 0.0));
  require(
      !session->freeze().finish_track_freeze(std::move(stale_render), stale.origin_revision, track),
      "a render older than the sound is refused");

  auto stage = freeze();
  auto render = take(std::move(stage.task).run());
  accepted(session->freeze().finish_track_freeze(std::move(render), stage.origin_revision, track));
  const auto& frozen = session->snapshot().tracks.front().frozen;
  REQUIRE_TRUE(frozen, "the track plays its render");
  REQUIRE_EQ(frozen->audio.channels, 2, "the track plays its render");
  REQUIRE_EQ(frozen->audio.frames, 64, "the track plays its render");
  REQUIRE_EQ(frozen->rendered_revision, stage.origin_revision, "the track plays its render");
  auto values = take(session->effects().native_effect_values(track, slot));
  const auto cutoff =
      take(native_inserts::native_effect_parameter_index(contour(), "contour.cutoff"));
  values[cutoff] = 1'000.0;
  require(!session->effects().set_native_effect_values(track, slot, values),
          "a frozen track's effects cannot change");
  require(!session->track_devices().add_note_effect(track, 0, "arp"), "nor its note effects");
  accepted(session->tracks().set_track_level_pan(track, 0.8, -0.2));
  require(!session->freeze().begin_track_freeze(track, -1), "a frozen track does not freeze again");

  accepted(session->freeze().unfreeze_track(track));
  accepted(session->effects().set_native_effect_values(track, slot, values));
  accepted(session->history().undo());
  accepted(session->history().undo());
  REQUIRE_OK(session->snapshot().tracks.front().frozen, "unfreezing undoes");
}

} // namespace

int main() {
  try {
    effects_are_added_edited_and_undone();
    automation_follows_its_slot();
    documents_travel_with_the_effect();
    pump_follows_its_key_tracks_notes();
    pump_hears_only_audible_keys();
    pump_follows_its_keys_mute_lane();
    linked_effects_share_their_shape();
    lone_links_are_released();
    note_effects_sit_before_the_synth();
    macros_drive_mapped_parameters();
    frozen_tracks_keep_their_sound();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native effect session contract passed\n";
  return 0;
}
