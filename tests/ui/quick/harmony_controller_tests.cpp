#include "../../../apps/daw_quick/src/harmony_controller.hpp"

#include "oscillate/application/comp_patterns.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/harmony/clip.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

// ADR-0084: the chord tools write chords into a MIDI clip as notes, one
// undo step per choice (a mood-pad drag is one), read the chords back from
// the notes, change the selected chord in its rhythm, follow undo, and turn
// held chords into a rhythm as notes or live through a Chord Comp.
namespace {
namespace application = oscillate::application;
namespace harmony = oscillate::harmony;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

constexpr std::int64_t quarter = oscillate::timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * quarter;
constexpr std::int64_t clip_bars = 4;

using oscillate::test::require;

template <typename T> T take(oscillate::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-harmony-ui-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "fixture directory creation failed");
    path_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

struct Fixture {
  Directory directory;
  std::unique_ptr<application::ProjectSession> session;
  project::TrackId track;
  project::ClipId clip;
  int commits = 0;
  std::unique_ptr<quick::HarmonyController> controller;

  Fixture()
      : session(take(application::ProjectSession::create(directory.path() / "song"))),
        track(take(session->instruments().add_synth_track())),
        clip(take(
            session->midi_clips().add_midi_clip_with_notes(track, {0}, {clip_bars * bar}, {}))) {
    controller = std::make_unique<quick::HarmonyController>(
        quick::HarmonyController::Hooks{.session = [this] { return session.get(); },
                                        .can_edit = [] { return true; },
                                        .commit = [this] { ++commits; }});
    controller->set_clip_id(QString::fromStdString(clip.to_string()));
  }
  [[nodiscard]] const project::MidiClip& midi() const {
    for (const auto& owner : session->snapshot().tracks)
      for (const auto& candidate : owner.midi_clips)
        if (candidate.id == clip)
          return candidate;
    throw std::runtime_error("the clip went missing");
  }
  [[nodiscard]] std::size_t steps() const { return session->history().undo_steps().size(); }
};

void write_reads_back_from_the_notes() {
  Fixture f;
  auto& controller = *f.controller;
  QSignalSpy heard(&controller, &quick::HarmonyController::auditionRequested);
  require(controller.count() == 0, "an empty clip has no chords");
  require(controller.key_name() == QStringLiteral("A minor"),
          "without a key the Lab works in A minor");
  require(controller.suggestions().size() == static_cast<int>(harmony::variant_count),
          "four suggestions wait before anything is written");

  // A suggestion writes notes into the clip, as one undo step.
  const auto steps = f.steps();
  controller.choose_suggestion(0);
  require(f.steps() == steps + 1, "a suggestion is one undo step");
  require(f.commits == 1, "and refreshes playback");
  require(!f.midi().notes.empty(), "the chords are notes in the clip");
  require(controller.count() > 0 && controller.lane().front().position.value == 0,
          "the strip reads them back from bar 1");
  require(controller.lane() == harmony::recognise(f.midi().notes, f.midi().pattern_length),
          "the strip is what the notes make");
  require(heard.count() == 1 && !heard.takeFirst().at(1).toList().isEmpty(),
          "and plays the first chord");
  const auto rows = static_cast<quick::KeyedListModel*>(controller.chords());
  require(rows->get(0).value(QStringLiteral("name")).toString().startsWith(QStringLiteral("A")),
          "the default loop starts on the tonic: " +
              rows->get(0).value(QStringLiteral("name")).toString().toStdString());

  // A shape and a device each write again, one step each.
  controller.set_shape(static_cast<int>(harmony::Shape::fall));
  controller.toggle_device(static_cast<int>(harmony::Device::pedal));
  require(f.steps() == steps + 3, "each Lab choice is one undo step");

  // A mood-pad drag across several modes writes as it goes but is one step.
  const auto notes_before_drag = f.midi().notes;
  controller.set_mood(0.05, 0.2);
  controller.set_mood(0.5, 0.6);
  controller.set_mood(0.95, 0.9);
  controller.finish_mood();
  require(f.steps() == steps + 4, "a mood-pad drag is one undo step");
  require(static_cast<bool>(f.session->history().undo()), "the drag undoes");
  require(f.midi().notes == notes_before_drag, "undo restores the notes before the drag");
  controller.project_changed();
  require(controller.lane() == harmony::recognise(f.midi().notes, f.midi().pattern_length),
          "the strip follows undo");
}

void a_song_key_keeps_the_pad_in_it_unless_asked() {
  Fixture f;
  auto& controller = *f.controller;
  constexpr std::uint8_t c = 0;
  require(static_cast<bool>(
              f.session->song().set_project_key(project::MusicalKey{c, project::KeyMode::minor})),
          "the song is in C minor");
  controller.project_changed();
  require(controller.has_key() && controller.key_modes_only(), "the pad starts in the key's modes");
  require(controller.mode_names().size() == 3, "three minor modes are offered");
  require(controller.mode_name() == QStringLiteral("Aeolian"), "starting on Aeolian");
  controller.set_mood(1.0, 0.5);
  controller.finish_mood();
  require(controller.lane().front().chord.root == c &&
              harmony::is_minor(controller.lane().front().chord.quality),
          "even the brightest mood keeps C minor");
  controller.set_key_modes_only(false);
  require(controller.mode_names().size() == static_cast<int>(harmony::mode_count),
          "All modes offers all six");
  controller.set_mood(1.0, 0.5);
  controller.finish_mood();
  require(!harmony::is_minor(controller.lane().front().chord.quality),
          "and the bright end can leave the key");
}

// Write's Complexity: simple is triads changing every two bars; turned up,
// sevenths and lush chords, faster changes and devices. A drag is one step.
void harmony_complexity_sets_colour_pace_and_devices() {
  Fixture f;
  auto& controller = *f.controller;
  require(controller.harmony_complexity() == quick::HarmonyController::default_harmony_complexity,
          "the knob starts where the Lab's defaults sit");
  const auto steps = f.steps();
  controller.set_harmony_complexity(0.1);
  controller.set_harmony_complexity(0.5);
  controller.set_harmony_complexity(0.95);
  controller.finish_harmony_complexity();
  require(f.steps() == steps + 1, "a Complexity drag is one undo step");
  require(controller.colour() == static_cast<int>(harmony::Colour::lush) &&
              controller.pace_beats() == quick::HarmonyController::pace_choices.back(),
          "at the top: lush chords every half bar");
  const auto on = [&](harmony::Device device) {
    return controller.devices()
        .at(static_cast<int>(device))
        .toMap()
        .value(QStringLiteral("on"))
        .toBool();
  };
  require(on(harmony::Device::passing) && on(harmony::Device::walk) && !on(harmony::Device::pedal),
          "with its devices, the pedal left to the musician");
  controller.set_harmony_complexity(0.0);
  controller.finish_harmony_complexity();
  require(controller.colour() == static_cast<int>(harmony::Colour::triads) &&
              controller.pace_beats() == quick::HarmonyController::pace_choices.front() &&
              !on(harmony::Device::drone),
          "at the bottom: plain triads every two bars");
  for (const auto& region : controller.lane())
    require(region.chord.quality == harmony::ChordQuality::minor ||
                region.chord.quality == harmony::ChordQuality::major,
            "and the clip holds plain triads");
}

// Rhythm's Complexity sets density and Motion; its middle is the defaults.
void rhythm_complexity_sets_density_and_motion() {
  Fixture f;
  auto& controller = *f.controller;
  require(controller.rhythm_complexity() == quick::HarmonyController::default_rhythm_complexity &&
              controller.variation() == quick::HarmonyController::default_variation &&
              controller.density() == quick::HarmonyController::default_density,
          "the knob starts at the defaults");
  controller.set_rhythm_complexity(quick::HarmonyController::default_rhythm_complexity);
  require(controller.variation() == quick::HarmonyController::default_variation &&
              controller.swell() == quick::HarmonyController::default_swell &&
              controller.turnaround() && controller.drift() && controller.top_cycle() == 0,
          "its middle is the defaults");
  controller.set_rhythm_complexity(0.0);
  require(controller.variation() == 0.0 && controller.density() == 0.0 &&
              !controller.turnaround() && !controller.drift() && controller.swell() == 0.0,
          "simple: thinned, the same every bar");
  const auto own_push = controller.push_chance();
  controller.set_rhythm_complexity(1.0);
  require(controller.density() == 1.0 && controller.top_cycle() == 2 &&
              controller.push_chance() > own_push,
          "intricate: fills, a 7-step top and more pushes");
}

// The Open voicing is the default and keeps the chord above a low bass.
void open_is_the_default_voicing() {
  Fixture f;
  auto& controller = *f.controller;
  require(controller.voicing_style() == static_cast<int>(harmony::VoicingStyle::open),
          "Open is the default voicing");
  require(controller.voicing_styles().front().toMap().value(QStringLiteral("name")).toString() ==
              QStringLiteral("Open"),
          "and listed first");
  controller.set_shape(static_cast<int>(harmony::Shape::loop));
  for (const auto& note : f.midi().notes)
    if (note.key < harmony::open_lowest)
      require(note.key >= harmony::open_bass_lowest, "the bass sits in Open's low register");

  // The bass switch: off takes the bass notes out as one undo step, the
  // chords unchanged; new writes stay without; on brings it back.
  const auto has_bass = [&f] {
    return std::ranges::any_of(f.midi().notes,
                               [](const auto& n) { return n.key < harmony::open_lowest; });
  };
  require(controller.bass() && has_bass(), "the bass starts on");
  const auto lane = controller.lane();
  const auto steps = f.steps();
  controller.set_bass(false);
  require(!controller.bass() && !has_bass() && f.steps() == steps + 1,
          "off takes the bass out as one undo step");
  require(controller.lane() == lane, "and the chords read the same");
  controller.set_shape(static_cast<int>(harmony::Shape::anthem));
  require(!has_bass(), "a new progression is written without a bass");
  controller.set_bass(true);
  require(controller.bass() && has_bass(), "on brings the bass back");

  // A progression that does not fill the clip grows it: Odd loop's three
  // bars run against four in twelve; Long phrase's eight against twelve
  // would need 24, past the limit, so two whole passes make sixteen.
  controller.set_shape(static_cast<int>(harmony::Shape::odd_loop));
  constexpr std::int64_t odd_loop_bars = 12;
  require(f.midi().pattern_length.value == odd_loop_bars * bar,
          "Odd loop grows the clip to twelve bars: " +
              std::to_string(f.midi().pattern_length.value / bar));
  controller.set_shape(static_cast<int>(harmony::Shape::long_phrase));
  constexpr std::int64_t long_phrase_bars = 16;
  require(f.midi().pattern_length.value == long_phrase_bars * bar,
          "Long phrase fits: " + std::to_string(f.midi().pattern_length.value / bar));
}

// Layers: each adds a track under the chords' with a clip in the same
// place; the pluck carries a Chord Comp, the rhythmic supporter.
void layers_add_tracks_that_follow() {
  Fixture f;
  auto& controller = *f.controller;
  controller.choose_suggestion(0);
  const auto tracks = f.session->snapshot().tracks.size();
  for (int kind = 0; kind < controller.layers().size(); ++kind)
    controller.add_layer(kind);
  const auto& after = f.session->snapshot().tracks;
  require(after.size() == tracks + static_cast<std::size_t>(controller.layers().size()),
          "each layer is a new track: " + controller.status().toStdString());
  int combs = 0;
  for (std::size_t t = 1; t < after.size(); ++t) {
    require(after[t].midi_clips.size() == 1 && !after[t].midi_clips.front().notes.empty() &&
                after[t].midi_clips.front().pattern_length == f.midi().pattern_length,
            "with a clip as long as the chords'");
    for (const auto& effect : after[t].note_effects)
      combs += effect.engine == application::comp::engine_id ? 1 : 0;
  }
  require(combs == 1, "the pluck is rhythmized by a Chord Comp");
  controller.set_inversion(1);
  require(controller.lane()[static_cast<std::size_t>(controller.selected())].chord.bass.has_value(),
          "first inversion puts the third in the bass");
}

// Held is the default rhythm: Apply anticipates the changes (varied) and
// strikes each chord once; Push and land touches the next chord; movement
// adds Pump, a dotted-8th filter and delay to the chords' track.
void held_rhythm_and_movement() {
  Fixture f;
  auto& controller = *f.controller;
  controller.choose_suggestion(0);
  require(controller.rhythm_pattern() == quick::HarmonyController::held_rhythm &&
              controller.anticipation() == static_cast<int>(harmony::Anticipation::varied),
          "held chords, changes arriving early, by default");
  require(!controller.rhythm_patterns().at(0).toMap().value(QStringLiteral("restruck")).toBool() &&
              controller.rhythm_patterns()
                  .at(quick::HarmonyController::first_comp_rhythm)
                  .toMap()
                  .value(QStringLiteral("restruck"))
                  .toBool(),
          "the re-struck patterns come after");
  const auto lane = controller.lane();
  controller.apply_rhythm();
  require(controller.lane().size() == lane.size() &&
              controller.lane()[1].position.value < lane[1].position.value,
          "the second chord now arrives early: " + controller.status().toStdString());
  controller.set_rhythm_pattern(quick::HarmonyController::push_land_rhythm);
  controller.apply_rhythm();
  bool touched = false;
  for (const auto& note : f.midi().notes)
    touched = touched || note.velocity == harmony::touch_velocity;
  require(touched, "push and land touches the next chord");
  const auto effects = f.session->snapshot().tracks.front().effects.size();
  for (int kind = 0; kind < controller.movements().size(); ++kind)
    controller.add_movement(kind);
  require(f.session->snapshot().tracks.front().effects.size() ==
              effects + static_cast<std::size_t>(controller.movements().size()),
          "Pump, the filter and the delay join the track: " + controller.status().toStdString());
}

void chord_edits_rewrite_the_notes() {
  Fixture f;
  auto& controller = *f.controller;
  controller.choose_suggestion(0);
  require(controller.count() >= 2, "a progression to edit");

  // A new quality re-voices the selected chord; the others keep their notes.
  controller.select(1);
  const auto before = controller.lane();
  const auto steps = f.steps();
  controller.set_quality(static_cast<int>(harmony::ChordQuality::dominant_seventh));
  require(f.steps() == steps + 1, "a chord change is one undo step");
  require(controller.lane().size() == before.size(), "no chord is lost");
  require(controller.lane()[1].chord.quality == harmony::ChordQuality::dominant_seventh,
          "the strip reads the new chord from the notes");
  require(controller.lane()[0].chord == before[0].chord, "the chord before it is untouched");

  controller.set_root((controller.lane()[1].chord.root + 2) % harmony::pitch_classes);
  require(controller.selection().value(QStringLiteral("root")).toInt() ==
              (before[1].chord.root + 2) % harmony::pitch_classes,
          "a new root lands");
  controller.set_bass((controller.lane()[1].chord.root + 4) % harmony::pitch_classes);
  require(controller.lane()[1].chord.bass.has_value(), "a slash bass lands");
  controller.set_bass(-1);
  require(!controller.lane()[1].chord.bass.has_value(), "and the root goes back in the bass");

  // Voicing re-voices only the selected chord.
  const auto first_notes =
      std::ranges::count_if(f.midi().notes, [&](const project::MidiNote& note) {
        return note.start.value < before[1].position.value;
      });
  controller.set_voicing_style(static_cast<int>(harmony::VoicingStyle::spread));
  require(std::ranges::count_if(f.midi().notes,
                                [&](const project::MidiNote& note) {
                                  return note.start.value < before[1].position.value;
                                }) == first_notes,
          "voicing leaves the other chords alone");

  // What comes next: split in adds a chord in the second half.
  controller.select(0);
  require(!controller.next().isEmpty(), "what comes next is offered");
  const auto count = controller.count();
  controller.set_insert_next(true);
  controller.use_next(0);
  require(controller.count() == count + 1 && controller.selected() == 1,
          "split in puts the next chord in the second half");

  // Substitutions and hints each rewrite the notes in one step.
  require(!controller.substitutions().isEmpty(), "the selected chord has substitutions");
  const auto edit_steps = f.steps();
  controller.apply_substitution(0);
  require(f.steps() == edit_steps + 1, "a substitution is one step");
  const auto hints = controller.hints();
  for (int i = 0; i < hints.size(); ++i)
    if (!hints.at(i).toMap().value(QStringLiteral("already")).toBool()) {
      controller.apply_hint(i);
      require(f.steps() == edit_steps + 2, "a hint is one step");
      break;
    }

  // Removing a chord deletes its notes.
  const auto selected = controller.lane()[static_cast<std::size_t>(controller.selected())];
  controller.remove(controller.selected());
  require(std::ranges::none_of(f.midi().notes,
                               [&](const project::MidiNote& note) {
                                 return note.start.value >= selected.position.value &&
                                        note.start.value <
                                            selected.position.value + selected.length_ticks;
                               }),
          "remove deletes the chord's notes");
}

// Changing patterns one after another, at every complexity, pace and
// anticipation and with the bass on or off, always rebuilds the clip. It
// starts the way users met "A note is outside the pattern": Motion grew the
// clip to its phrase, the clip was shortened again (its later notes kept,
// silent), and the next Apply carried them over.
void every_pattern_applies_after_any_other() {
  Fixture f;
  auto& controller = *f.controller;
  controller.choose_suggestion(0);
  controller.set_rhythm_pattern(quick::HarmonyController::first_comp_rhythm);
  controller.apply_rhythm();
  require(f.midi().pattern_length.value > clip_bars * bar, "Motion grows the clip");
  require(f.session->midi_clips().resize_midi_clip(f.clip, {clip_bars * bar}).has_value(),
          "shortened again");
  controller.project_changed();
  require(std::ranges::any_of(f.midi().notes,
                              [&](const auto& n) { return n.start >= f.midi().pattern_length; }),
          "the notes after the new end are kept, silent");
  controller.set_rhythm_pattern(quick::HarmonyController::held_rhythm);
  controller.apply_rhythm();
  require(controller.status().isEmpty(),
          "Apply after shortening: " + controller.status().toStdString());
  require(std::ranges::none_of(f.midi().notes,
                               [&](const auto& n) { return n.end > f.midi().pattern_length; }),
          "the whole clip is rebuilt, nothing left past its end");
  const auto patterns = static_cast<int>(controller.rhythm_patterns().size());
  for (const bool bass : {true, false}) {
    controller.set_bass(bass);
    for (const int pace : {8, 4, 2}) {
      controller.set_pace_beats(pace);
      for (const double complexity : {0.0, 0.5, 1.0}) {
        controller.set_rhythm_complexity(complexity);
        for (int pattern = 0; pattern < patterns; ++pattern) {
          controller.set_anticipation(pattern % 5);
          controller.set_rhythm_pattern(pattern);
          const auto steps = f.steps();
          controller.apply_rhythm();
          require(f.steps() == steps + 1, "pattern " + std::to_string(pattern) + " at complexity " +
                                              std::to_string(complexity) + ", pace " +
                                              std::to_string(pace) +
                                              (bass ? ", bass" : ", no bass") +
                                              " applies: " + controller.status().toStdString());
        }
      }
    }
  }
}

void rhythm_as_notes_and_live() {
  Fixture f;
  auto& controller = *f.controller;
  controller.choose_suggestion(0);
  const auto lane = controller.lane();
  require(controller.rhythm_patterns().size() > 1, "Held and Comp's patterns are offered");

  // Apply rewrites the notes in the rhythm, still naming the same chords.
  const auto held_notes = f.midi().notes.size();
  controller.set_rhythm_pattern(quick::HarmonyController::first_comp_rhythm);
  const auto steps = f.steps();
  controller.apply_rhythm();
  require(f.steps() == steps + 1, "apply is one undo step: " + controller.status().toStdString());
  require(f.midi().notes.size() > held_notes, "the rhythm strikes the chords more often");
  // Motion is on by default and varies over 8 bars: the 4-bar clip grows to
  // hold the phrase, its chords repeated.
  constexpr std::int64_t phrase_bars = 8;
  require(f.midi().pattern_length.value == phrase_bars * bar, "the clip grows to the phrase");
  bool same_chords = controller.lane().size() == 2 * lane.size();
  for (std::size_t i = 0; same_chords && i < controller.lane().size(); ++i)
    same_chords = controller.lane()[i].chord == lane[i % lane.size()].chord;
  std::string read;
  for (const auto& region : controller.lane())
    read += harmony::name(region.chord, quick::HarmonyController::fallback_key) + "@" +
            std::to_string(region.position.value) + " ";
  std::string wrote;
  for (const auto& region : lane)
    wrote += harmony::name(region.chord, quick::HarmonyController::fallback_key) + "@" +
             std::to_string(region.position.value) + " ";
  require(same_chords,
          "the rhythm's notes still make the same chords: wrote " + wrote + "read " + read);

  // Held puts them back as held chords.
  controller.set_rhythm_pattern(quick::HarmonyController::held_rhythm);
  controller.apply_rhythm();
  require(std::ranges::all_of(f.midi().notes,
                              [&](const project::MidiNote& note) {
                                return std::ranges::any_of(controller.lane(),
                                                           [&](const auto& region) {
                                                             return region.position == note.start;
                                                           });
                              }),
          "Held strikes each chord once, at its start");

  // With Motion off a rhythm keeps the clip's length.
  {
    Fixture plain;
    plain.controller->choose_suggestion(0);
    plain.controller->set_variation(0.0);
    plain.controller->set_swell(0.0);
    plain.controller->set_turnaround(false);
    plain.controller->set_drift(false);
    plain.controller->set_rhythm_pattern(quick::HarmonyController::first_comp_rhythm);
    plain.controller->apply_rhythm();
    require(plain.midi().pattern_length.value == clip_bars * bar, "no Motion, no growing");
  }

  // Play live: a Chord Comp on the track plays the rhythm over the held chords.
  require(!controller.live_rhythm(), "nothing plays live yet");
  controller.set_live_rhythm(true);
  require(controller.live_rhythm(), "play live turns on a Chord Comp");
  const auto& effects = f.session->snapshot().tracks.front().note_effects;
  require(effects.size() == 1 && effects.front().engine == application::comp::engine_id,
          "the Comp sits first in the track's MIDI effects");
  const auto live_steps = f.steps();
  controller.set_groove(0);
  require(f.steps() == live_steps + 1, "a rhythm choice updates the live Comp in one step");
  const auto value_of = [&](std::string_view id) {
    for (const auto& parameter :
         f.session->snapshot().tracks.front().note_effects.front().parameters)
      if (parameter.id == id)
        return parameter.value;
    return -1.0;
  };
  controller.set_variation(0.6);
  controller.set_top_cycle(1);
  require(value_of("comp.variation") == 0.6 &&
              value_of("comp.top_length") == quick::HarmonyController::top_cycle_steps[1],
          "Motion reaches the live Comp");
  controller.set_live_rhythm(false);
  require(!controller.live_rhythm(), "and play live turns it off");

  // Back on with a rhythm chosen meanwhile: its settings and the switch are
  // one undo step.
  constexpr int other_groove = 1;
  controller.set_groove(other_groove);
  const auto off_steps = f.steps();
  controller.set_live_rhythm(true);
  require(controller.live_rhythm() && f.steps() == off_steps + 1,
          "turning the live Comp back on took more than one undo step");

  // One slider drag over a live Comp is one undo step; the next drag is another.
  constexpr double first_variation = 0.2;
  constexpr double second_variation = 0.3;
  constexpr double dragged_swell = 0.4;
  constexpr double later_variation = 0.5;
  const auto before_drag = f.steps();
  controller.set_variation(first_variation);
  controller.set_variation(second_variation);
  controller.set_swell(dragged_swell);
  controller.finish_rhythm_drag();
  require(f.steps() == before_drag + 1 && value_of("comp.variation") == second_variation,
          "a rhythm drag over the live Comp was not one undo step");
  controller.set_variation(later_variation);
  controller.finish_rhythm_drag();
  require(f.steps() == before_drag + 2, "the next rhythm drag joined the previous one");
}

} // namespace

int main(int argc, char** argv) {
  QStandardPaths::setTestModeEnabled(true);
  QCoreApplication app(argc, argv);
  try {
    write_reads_back_from_the_notes();
    a_song_key_keeps_the_pad_in_it_unless_asked();
    harmony_complexity_sets_colour_pace_and_devices();
    every_pattern_applies_after_any_other();
    open_is_the_default_voicing();
    layers_add_tracks_that_follow();
    held_rhythm_and_movement();
    rhythm_complexity_sets_density_and_motion();
    chord_edits_rewrite_the_notes();
    rhythm_as_notes_and_live();
  } catch (const std::exception& error) {
    std::cerr << "harmony controller: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "harmony controller contract passed\n";
  return EXIT_SUCCESS;
}
