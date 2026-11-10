// Renders Nova's Analogue presets (ADR-0073) playing a short phrase each,
// with Analogue on as saved and again with it off, so the difference can
// be heard: 8 bars in C minor with a little reverb.
//   oscillate_nova_audition <output directory> [preset...]
// Named presets play the pad part (held chords), or a bassline when the
// name ends in ":bass"; "|<Chop preset>" after it puts Chop (ADR-0075) on
// the track with that preset. With none, the five Analogue presets play
// their own parts.
//   oscillate_nova_audition --pads <output directory> <lead.mid>
// renders the pad audition set under a flute lead (pads.hpp).
//   oscillate_nova_audition --breakdown <output directory> [--stems]
// renders the breakdown audition set (breakdown.hpp).
//   oscillate_nova_audition --keys|--keys2 <output directory> [--stems]
// renders the keys audition sets (keys.hpp).
//   oscillate_nova_audition --strings <output directory> [--stems]
// renders the strings audition set (strings.hpp), and
//   oscillate_nova_audition --drums <output directory>
// the Stick grooves (drums.hpp).
#include "audition.hpp"
#include "breakdown.hpp"
#include "drums.hpp"
#include "keys.hpp"
#include "pads.hpp"
#include "strings.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace oscillate;
using tools::audition::done;
using tools::audition::fail;
using tools::audition::render;
using tools::audition::take;

constexpr std::int64_t quarter = timeline::ticks_per_quarter;
constexpr int bars = 8;

struct Note {
  double beat;
  double length;
  std::uint8_t key;
  std::uint8_t velocity;
};

// Cm9, Abmaj7, Fm9, Gm7(11), two bars each.
const std::vector<std::vector<std::uint8_t>> chords{
    {48, 63, 67, 70, 74}, {44, 60, 63, 67, 72}, {41, 56, 60, 63, 67}, {43, 58, 62, 65, 72}};

std::vector<Note> stabs() {
  // Off-beat stabs, displaced in the second bar of each chord.
  std::vector<Note> notes;
  const std::vector<double> first{0.5, 1.75, 3.0};
  const std::vector<double> second{0.75, 2.5, 3.5};
  for (int bar = 0; bar < bars; ++bar) {
    const auto& chord = chords[static_cast<std::size_t>(bar / 2) % chords.size()];
    for (const double at : bar % 2 == 0 ? first : second)
      for (std::size_t i = 1; i < chord.size(); ++i)
        notes.push_back({bar * 4.0 + at, 0.3, chord[i], 96});
  }
  return notes;
}

std::vector<Note> held(double length_beats, std::uint8_t velocity) {
  std::vector<Note> notes;
  for (int bar = 0; bar < bars; bar += 2) {
    const auto& chord = chords[static_cast<std::size_t>(bar / 2) % chords.size()];
    for (const auto key : chord)
      notes.push_back({bar * 4.0, length_beats, key, velocity});
  }
  return notes;
}

std::vector<Note> lead() {
  // A three-beat cell over the bar, resting every fourth bar.
  const std::vector<std::uint8_t> cell{67, 70, 72, 70, 75, 74};
  std::vector<Note> notes;
  int step = 0;
  for (double at = 0.5; at < bars * 4.0; at += 1.5, ++step) {
    if (static_cast<int>(at / 4.0) % 4 == 3)
      continue;
    notes.push_back({at, 1.1, cell[static_cast<std::size_t>(step) % cell.size()], 100});
  }
  return notes;
}

std::vector<Note> bassline() {
  // B♭ minor around B♭2 (the tutorial's line sits near 130 Hz), sixteenths with gaps; overlapping
  // notes slide (legato) on presets that glide.
  struct Step {
    double at;
    double length;
    std::uint8_t key;
  };
  const std::vector<Step> bar{{0.0, 0.45, 46}, {0.75, 0.2, 46}, {1.0, 0.7, 58}, {1.75, 0.45, 53},
                              {2.5, 0.2, 46},  {2.75, 0.6, 49}, {3.25, 0.5, 51}};
  std::vector<Note> notes;
  for (int b = 0; b < bars; ++b)
    for (const auto& step : bar)
      notes.push_back({b * 4.0 + step.at, step.length, step.key,
                       static_cast<std::uint8_t>(step.at == 0.0 ? 110 : 92)});
  return notes;
}

std::size_t preset_index(std::string_view name) {
  const auto& presets = native_dsp::synth::find_engine("nova")->descriptor->presets;
  for (std::size_t i = 0; i < presets.size(); ++i)
    if (presets[i].name == name)
      return i;
  fail("no preset " + std::string(name));
}

std::size_t effect_preset(std::string_view engine, std::string_view name) {
  const auto& presets = native_dsp::effects::find_engine(engine)->descriptor->presets;
  for (std::size_t i = 0; i < presets.size(); ++i)
    if (presets[i].name == name)
      return i;
  fail("no " + std::string(engine) + " preset " + std::string(name));
}

void set_value(application::ProjectSession& session, project::TrackId track, std::string_view id,
               double value) {
  const auto& table = *native_dsp::synth::find_engine("nova")->descriptor;
  auto values = take(session.instruments().native_instrument_values(track));
  const auto index = native_dsp::find_parameter(table, id);
  if (!index)
    fail("no parameter " + std::string(id));
  values[*index] = value;
  done(session.instruments().set_native_instrument_values(track, values));
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2)
    fail("usage: oscillate_nova_audition <output directory> [preset...] | --pads <output "
         "directory> <lead.mid> | --breakdown <output directory> [--stems] | --keys|--keys2 "
         "<output directory> [--stems] | --strings <output directory> [--stems] | --drums <output "
         "directory>");
  if (std::string_view(argv[1]) == tools::pads::flag) {
    if (argc != 4)
      fail("usage: oscillate_nova_audition --pads <output directory> <lead.mid>");
    tools::pads::render_set(argv[2], argv[3]);
    return 0;
  }
  if (std::string_view(argv[1]) == tools::breakdown::flag) {
    const bool stems = tools::audition::stems_requested(argc, argv);
    if (argc != 3 && !stems)
      fail("usage: oscillate_nova_audition --breakdown <output directory> [--stems]");
    tools::breakdown::render_set(argv[2], stems);
    return 0;
  }
  if (std::string_view(argv[1]) == tools::keys::flag ||
      std::string_view(argv[1]) == tools::keys::breakdown_flag) {
    const bool stems = tools::audition::stems_requested(argc, argv);
    if (argc != 3 && !stems)
      fail("usage: oscillate_nova_audition --keys|--keys2 <output directory> [--stems]");
    const auto round = std::string_view(argv[1]) == tools::keys::flag
                           ? tools::keys::Round::first
                           : tools::keys::Round::breakdown;
    tools::keys::render_set(argv[2], stems, round);
    return 0;
  }
  if (std::string_view(argv[1]) == tools::strings::flag) {
    const bool stems = tools::audition::stems_requested(argc, argv);
    if (argc != 3 && !stems)
      fail("usage: oscillate_nova_audition --strings <output directory> [--stems]");
    tools::strings::render_set(argv[2], stems);
    return 0;
  }
  if (std::string_view(argv[1]) == tools::drums::flag) {
    if (argc != 3)
      fail("usage: oscillate_nova_audition --drums <output directory>");
    tools::drums::render_set(argv[2]);
    return 0;
  }
  const fs::path out = argv[1];
  fs::create_directories(out);
  // Each part's optional Chop preset, by part index.
  std::map<std::size_t, std::string> chops;
  std::vector<std::pair<std::string_view, std::vector<Note>>> parts{
      {"Detroit Chord Stab", stabs()},
      {"Warehouse Poly Pad", held(7.5, 80)},
      {"Worn Unison Lead", lead()},
      {"Dub Chord Echo", stabs()},
      {"Old Card Brass", held(3.0, 100)}};
  if (argc > 2) {
    parts.clear();
    constexpr std::string_view bass_suffix = ":bass";
    for (int index = 2; index < argc; ++index) {
      std::string_view name = argv[index];
      if (const auto bar = name.find('|'); bar != std::string_view::npos) {
        chops.emplace(parts.size(), std::string(name.substr(bar + 1)));
        name = name.substr(0, bar);
      }
      const bool bass = name.ends_with(bass_suffix);
      if (bass)
        name.remove_suffix(bass_suffix.size());
      parts.emplace_back(name, bass ? bassline() : held(7.5, 80));
    }
  }
  const auto work = out / ".projects";
  fs::remove_all(work);
  fs::create_directories(work);
  for (std::size_t part = 0; part < parts.size(); ++part) {
    const auto& [name, notes] = parts[part];
    const auto chop = chops.find(part);
    const std::string label =
        std::string(name) + (chop == chops.end() ? "" : " + Chop " + chop->second);
    std::cout << label << '\n';
    const auto root = work / label;
    auto session = take(application::ProjectSession::create(root));
    const auto track =
        take(session->instruments().add_native_instrument_track("nova", preset_index(name)));
    done(session->midi_clips().add_midi_clip(track, {0}));
    const auto clip =
        session->snapshot().tracks[*session->tracks().track_index(track)].midi_clips.front().id;
    done(session->midi_clips().resize_midi_clip(clip, {bars * 4 * quarter}));
    for (const auto& note : notes) {
      session->notes().set_new_note_velocity(note.velocity);
      done(session->notes().add_note_with_duration(
          clip, {static_cast<std::int64_t>(note.beat * quarter)},
          {static_cast<std::int64_t>(note.length * quarter)}, note.key));
    }
    for (const auto& item : session->snapshot().tracks)
      if (item.kind == project::TrackKind::return_bus &&
          item.name.find("Reverb") != std::string::npos)
        take(session->routing().add_send(track, item.id, 0.25));
    if (chop != chops.end()) {
      // Chop last on the track, then the dry bass for comparison.
      const auto slot = take(session->effects().add_effect_engine(
          track, 0, "chop", effect_preset("chop", chop->second)));
      render(*session, root, out / (label + ".wav"));
      done(session->effects().remove_effect_insert(track, slot));
      render(*session, root, out / (std::string(name) + " - dry.wav"));
      continue;
    }
    render(*session, root, out / (label + " - Analogue on.wav"));
    set_value(*session, track, "nova.analogue.on", 0.0);
    render(*session, root, out / (label + " - Analogue off.wav"));
  }
  fs::remove_all(work);
  return 0;
}
