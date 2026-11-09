#include "strings.hpp"

#include "audition.hpp"
#include "midi_out.hpp"
#include "parts.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/export/wav_writer.hpp"
#include "oscillate/native_dsp/synth/bow.hpp"
#include "oscillate/project/automation_types.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::tools::strings {
namespace {

namespace fs = std::filesystem;
namespace bow = native_dsp::synth::bow;
using audition::done;
using audition::fail;
using audition::take;
using parts::beats_per_bar;
using parts::key;
using parts::keys_per_octave;
using parts::Note;
using parts::ticks;
using P = parts::Pitch;
using Session = application::ProjectSession;

constexpr std::uint32_t render_rate = 48'000;
constexpr double tempo_bpm = 120.0;
constexpr int bars = 16;
constexpr double part_beats = bars * beats_per_bar;
constexpr std::string_view bow_engine = "bow";
constexpr std::string_view expanse = parts::space_engine;
constexpr std::string_view space_preset = "Endless Cloud";
constexpr std::string_view wheel_id = "bow.perf.wheel";

// Where the pads' chords change over sixteen bars (parts::held_part and
// parts::low_part, looped after bar 12): C minor, A♭, the open G of the
// Hijaz, C minor with a suspended F, low C, and C minor again.
constexpr double a_flat_at = 17.5;
constexpr double g_at = 25.25;
constexpr double sus_at = 31.0;
constexpr double low_c_at = 40.75;
constexpr double again_at = 48.0;

// ---- Dynamics (the mod wheel, CC1) ----------------------------------------------------------

struct Point {
  double beat;
  double value; // 0..1.
};

// A breath over each note: from `low` it swells to `high` a little past the
// middle of the note and eases back to `low` as it ends, so a held line
// breathes instead of sitting flat.
constexpr double swell_peak_at = 0.55;
std::vector<Point> breathe(const std::vector<Note>& notes, double low, double high) {
  std::vector<Point> points;
  for (const auto& note : notes) {
    if (bow::is_keyswitch(note.key))
      continue;
    points.push_back({note.beat, low});
    points.push_back({note.beat + note.length * swell_peak_at, high});
    points.push_back({note.beat + note.length, low});
  }
  return points;
}

// One arc over a phrase: notes that touch are one phrase.
std::vector<Point> phrase_arcs(const std::vector<Note>& notes, double low,
                               const std::vector<double>& peaks) {
  std::vector<Point> points;
  std::size_t phrase = 0;
  std::size_t i = 0;
  constexpr double touching = 0.01;
  while (i < notes.size()) {
    if (bow::is_keyswitch(notes[i].key)) {
      ++i;
      continue;
    }
    const double start = notes[i].beat;
    double end = notes[i].beat + notes[i].length;
    std::size_t j = i + 1;
    while (j < notes.size() && notes[j].beat <= end + touching) {
      end = std::max(end, notes[j].beat + notes[j].length);
      ++j;
    }
    const double peak = peaks[std::min(phrase, peaks.size() - 1)];
    points.push_back({start, low});
    points.push_back({start + (end - start) * swell_peak_at, peak});
    points.push_back({end, low});
    ++phrase;
    i = j;
  }
  return points;
}

// ---- Parts ---------------------------------------------------------------------------------

using Values = std::vector<std::pair<std::string_view, double>>;

struct Part {
  std::string_view name; // The section, for its MIDI file.
  std::string_view preset;
  Values values; // Over the preset.
  std::vector<Note> notes;
  std::vector<Point> wheel;
  double level; // Linear, before the clip's make-up gain.
};

constexpr double section(bow::Section s) { return static_cast<double>(s); }
constexpr double articulation(bow::Articulation a) { return static_cast<double>(a); }
constexpr std::uint8_t keyswitch(bow::Articulation a) {
  return static_cast<std::uint8_t>(bow::first_keyswitch + static_cast<int>(a));
}
constexpr double keyswitch_length = 0.1;
// A keyswitch a sixteenth before the note it changes.
constexpr double keyswitch_lead = 0.25;

// Sul tasto: the Tone control toward the fingerboard.
constexpr double sul_tasto = 0.2;
constexpr double soft_tasto = 0.3;

// 1. Three sections holding open voicings that move one voice at a time,
// staggered: violins high above the flutes, violas inside, cellos on the
// roots only through the middle. The violins' A♭ to B natural is the
// Hijaz step; it resolves up to C.
std::vector<Part> sul_tasto_voicings() {
  const std::vector<Note> violins{
      {2.5, 13.5, key(P::g, 5), 40},   {19.0, 3.0, key(P::g, 5), 44},
      {22.0, 3.25, key(P::ab, 5), 46}, {26.0, 4.75, key(P::b, 5), 48},
      {32.0, 6.25, key(P::c, 6), 52},  {38.5, 2.0, key(P::d, 6), 50},
      {42.0, 5.5, key(P::c, 6), 46},   {50.5, 9.5, key(P::g, 5), 38},
  };
  const std::vector<Note> violas{
      {4.0, 12.5, key(P::eb, 4), 40},  {18.25, 6.5, key(P::eb, 4), 42},
      {26.25, 4.25, key(P::b, 3), 44}, {31.75, 8.0, key(P::f, 4), 46},
      {41.5, 6.0, key(P::eb, 4), 42},  {49.0, 6.0, key(P::eb, 4), 38},
      {55.5, 6.0, key(P::d, 4), 34},
  };
  const std::vector<Note> cellos{
      {18.0, 6.5, key(P::ab, 2), 42},
      {25.5, 5.0, key(P::g, 2), 44},
      {31.5, 8.5, key(P::c, 3), 46},
      {41.0, 6.5, key(P::c, 3), 40},
  };
  constexpr double low = 0.12;
  const Values tasto{{"bow.tone", sul_tasto}};
  return {
      {"Violins I", "Sul Tasto Pad",
       Values{{"bow.section", section(bow::Section::violins_1)}, {"bow.tone", sul_tasto}}, violins,
       breathe(violins, low, 0.5), 0.8},
      {"Violas", "Sul Tasto Pad", tasto, violas, breathe(violas, low, 0.45), 1.0},
      {"Cellos", "Sul Tasto Pad",
       Values{{"bow.section", section(bow::Section::cellos)}, {"bow.tone", sul_tasto}}, cellos,
       breathe(cellos, low, 0.42), 0.9},
  };
}

// 2. A Hijaz line in the violas, phrases with rests between them, its peak
// on F in bar 10; the cellos play it an octave down a beat late, as a canon.
std::vector<Note> hijaz_line() {
  return {
      {5.5, 1.0, key(P::g, 4), 52},   {6.5, 0.5, key(P::ab, 4), 50},
      {7.0, 1.5, key(P::b, 4), 56},   {8.5, 3.0, key(P::c, 5), 58},
      {14.5, 1.0, key(P::eb, 5), 54}, {15.5, 1.0, key(P::d, 5), 52},
      {16.5, 2.5, key(P::c, 5), 56},  {19.0, 1.0, key(P::b, 4), 50},
      {20.0, 3.0, key(P::ab, 4), 52}, {26.5, 1.0, key(P::b, 4), 54},
      {27.5, 0.5, key(P::c, 5), 50},  {28.0, 1.5, key(P::d, 5), 58},
      {29.5, 0.5, key(P::c, 5), 52},  {30.0, 2.0, key(P::b, 4), 56},
      {34.0, 1.0, key(P::c, 5), 58},  {35.0, 1.0, key(P::d, 5), 60},
      {36.0, 1.0, key(P::eb, 5), 64}, {37.0, 2.5, key(P::f, 5), 70},
      {39.5, 1.0, key(P::eb, 5), 62}, {40.5, 2.5, key(P::d, 5), 58},
      {45.0, 1.0, key(P::c, 5), 54},  {46.0, 1.0, key(P::b, 4), 52},
      {47.0, 1.0, key(P::ab, 4), 50}, {48.0, 4.0, key(P::g, 4), 48},
      {56.0, 1.5, key(P::ab, 4), 44}, {57.5, 4.0, key(P::g, 4), 40},
  };
}

std::vector<Part> hijaz_canon() {
  const auto violas = hijaz_line();
  constexpr double late = 1.0;
  std::vector<Note> cellos;
  for (const auto& note : violas)
    if (note.beat + late < part_beats)
      cellos.push_back({note.beat + late, std::min(note.length, part_beats - note.beat - late),
                        static_cast<std::uint8_t>(note.key - keys_per_octave),
                        static_cast<std::uint8_t>(note.velocity - 6)});
  constexpr double low = 0.2;
  const std::vector<double> peaks{0.48, 0.5, 0.52, 0.66, 0.46, 0.36};
  constexpr double cello_trim = 0.06;
  std::vector<double> cello_peaks;
  for (const double peak : peaks)
    cello_peaks.push_back(peak - cello_trim);
  return {
      {"Violas", "Ensemble Legato",
       Values{{"bow.section", section(bow::Section::violas)},
              {"bow.players", 8.0},
              {"bow.tone", soft_tasto}},
       violas, phrase_arcs(violas, low, peaks), 1.0},
      {"Cellos", "Ensemble Legato",
       Values{{"bow.section", section(bow::Section::cellos)},
              {"bow.players", 6.0},
              {"bow.tone", soft_tasto}},
       cellos, phrase_arcs(cellos, low, cello_peaks), 0.85},
  };
}

// 3. Pizzicato on every third eighth (three against four), walking a
// four-note shape so the shape and the pulse drift apart; a beat drops out
// now and then, and bars 8 and 16 rest. The basses take the root on the
// first of each three plucks.
struct Shape {
  std::uint8_t root; // Cellos' octave.
  std::array<std::uint8_t, 4> walk;
};
Shape shape_at(double beat) {
  if (beat >= again_at || beat < a_flat_at)
    return {key(P::c, 3), {key(P::c, 3), key(P::g, 3), key(P::eb, 4), key(P::g, 3)}};
  if (beat < g_at)
    return {key(P::ab, 2), {key(P::ab, 2), key(P::eb, 3), key(P::c, 4), key(P::eb, 3)}};
  if (beat < sus_at)
    return {key(P::g, 2), {key(P::g, 2), key(P::d, 3), key(P::b, 3), key(P::d, 3)}};
  if (beat < low_c_at)
    return {key(P::c, 3), {key(P::c, 3), key(P::g, 3), key(P::f, 3), key(P::g, 3)}};
  return {key(P::c, 3), {key(P::c, 3), key(P::g, 3), key(P::c, 4), key(P::g, 3)}};
}

std::vector<Part> pizzicato_three_four() {
  constexpr double pulse = 1.5;
  constexpr double first = 0.5;
  constexpr double pluck = 0.4;
  constexpr int drop_every = 7;
  constexpr int bass_every = 3;
  constexpr int resting_bar = 8;
  constexpr std::array<std::uint8_t, 4> accents{72, 56, 62, 50};
  std::vector<Note> cellos;
  std::vector<Note> basses;
  int index = 0;
  for (double at = first; at < part_beats; at += pulse, ++index) {
    const int bar = static_cast<int>(at / beats_per_bar) + 1;
    if (bar % resting_bar == 0 || index % drop_every == drop_every - 1)
      continue;
    const auto shape = shape_at(at);
    const auto velocity = accents[static_cast<std::size_t>(index) % accents.size()];
    cellos.push_back(
        {at, pluck, shape.walk[static_cast<std::size_t>(index) % shape.walk.size()], velocity});
    if (index % bass_every == 0)
      basses.push_back({at, pluck, static_cast<std::uint8_t>(shape.root - keys_per_octave),
                        static_cast<std::uint8_t>(velocity - 4)});
  }
  // Plucks read their velocity; the wheel still rides the phrase for the
  // MIDI, gently.
  const std::vector<Point> wheel{{0.0, 0.4}, {36.0, 0.6}, {part_beats, 0.35}};
  return {
      {"Cellos", "Pizzicato Ostinato", Values{{"bow.section", section(bow::Section::cellos)}},
       cellos, wheel, 1.0},
      {"Basses", "Pizzicato Ostinato", Values{{"bow.section", section(bow::Section::basses)}},
       basses, wheel, 0.8},
  };
}

// 4. Violins high and sul tasto, barely there, three long notes in twelve
// bars; at bar 13 a keyswitch turns them to tremolo and the two sections
// climb, a third or sixth apart, swelling softly to the end.
std::vector<Part> tremolo_rise() {
  constexpr double rise_at = again_at;
  const auto ks_legato = keyswitch(bow::Articulation::legato);
  const auto ks_tremolo = keyswitch(bow::Articulation::tremolo);
  const std::vector<Note> first{
      {0.0, keyswitch_length, ks_legato, 64},
      {6.0, 10.0, key(P::g, 5), 36},
      {22.0, 8.0, key(P::ab, 5), 38},
      {33.0, 10.0, key(P::c, 6), 40},
      {rise_at - keyswitch_lead, keyswitch_length, ks_tremolo, 64},
      {rise_at, 4.0, key(P::g, 5), 44},
      {52.0, 4.0, key(P::ab, 5), 48},
      {56.0, 4.0, key(P::b, 5), 52},
      {60.0, 4.0, key(P::c, 6), 56},
  };
  const std::vector<Note> second{
      {0.0, keyswitch_length, ks_legato, 64},
      {26.0, 5.0, key(P::d, 5), 36},
      {rise_at - keyswitch_lead, keyswitch_length, ks_tremolo, 64},
      {rise_at + 0.5, 3.5, key(P::eb, 5), 40},
      {52.5, 3.5, key(P::f, 5), 44},
      {56.5, 3.5, key(P::d, 5), 48},
      {60.5, 3.5, key(P::eb, 5), 52},
  };
  // Quiet and breathing before the rise, then one long swell from almost
  // nothing.
  const auto swell = [&](std::vector<Note> notes, double top) {
    std::vector<Note> held;
    for (const auto& note : notes)
      if (note.beat < rise_at - keyswitch_lead)
        held.push_back(note);
    auto points = breathe(held, 0.06, 0.2);
    points.push_back({rise_at, 0.06});
    points.push_back({part_beats, top});
    return points;
  };
  return {
      {"Violins I", "Tremolo Swell",
       Values{{"bow.articulation", articulation(bow::Articulation::legato)},
              {"bow.tone", sul_tasto}},
       first, swell(first, 0.65), 1.0},
      {"Violins II", "Tremolo Swell",
       Values{{"bow.section", section(bow::Section::violins_2)},
              {"bow.articulation", articulation(bow::Articulation::legato)},
              {"bow.tone", sul_tasto}},
       second, swell(second, 0.6), 0.85},
  };
}

// 5. One cello in its tenor register, phrases with rests between them: up
// from G, the Hijaz turn over the open G, a peak on C5 in bar 10 falling
// back through B and A♭, and a last phrase settling on C.
std::vector<Part> solo_cello() {
  const std::vector<Note> line{
      {3.0, 1.5, key(P::g, 3), 50},   {4.5, 1.0, key(P::c, 4), 54},   {5.5, 0.5, key(P::d, 4), 52},
      {6.0, 3.0, key(P::eb, 4), 58},  {9.0, 1.0, key(P::d, 4), 52},   {10.0, 3.5, key(P::c, 4), 50},
      {17.0, 1.0, key(P::eb, 4), 54}, {18.0, 1.0, key(P::f, 4), 56},  {19.0, 2.5, key(P::g, 4), 60},
      {21.5, 1.0, key(P::f, 4), 54},  {22.5, 2.5, key(P::eb, 4), 52}, {26.0, 1.0, key(P::d, 4), 56},
      {27.0, 0.5, key(P::eb, 4), 52}, {27.5, 0.5, key(P::d, 4), 50},  {28.0, 1.0, key(P::b, 3), 56},
      {29.0, 0.5, key(P::ab, 3), 50}, {29.5, 2.5, key(P::b, 3), 54},  {33.5, 1.0, key(P::c, 4), 58},
      {34.5, 1.0, key(P::eb, 4), 62}, {35.5, 1.0, key(P::g, 4), 66},  {36.5, 3.0, key(P::c, 5), 72},
      {39.5, 1.0, key(P::b, 4), 64},  {40.5, 1.0, key(P::ab, 4), 60}, {41.5, 3.0, key(P::g, 4), 58},
      {46.0, 1.0, key(P::f, 4), 52},  {47.0, 1.0, key(P::eb, 4), 50}, {48.0, 2.0, key(P::d, 4), 50},
      {50.0, 4.0, key(P::c, 4), 46},  {57.0, 1.5, key(P::eb, 4), 42}, {58.5, 4.5, key(P::c, 4), 38},
  };
  const std::vector<double> peaks{0.55, 0.6, 0.58, 0.8, 0.5, 0.4};
  return {{"Solo Cello", "Solo Cello", Values{}, line, phrase_arcs(line, 0.22, peaks), 1.0}};
}

// ---- Clips ---------------------------------------------------------------------------------

struct Clip {
  std::string_view name;   // "<NN idea>"
  std::string_view preset; // For the file name.
  double space_mix;        // Endless Cloud on each section.
  // Make-up gain in dB over every section, so the loudest passages sit a
  // few dB over the pads: Trim before the reverb, then the track level.
  double gain_db;
  std::vector<Part> (*parts)();
};

const std::vector<Clip>& clips() {
  static const std::vector<Clip> list{
      {"01 sul tasto voicings", "Sul Tasto Pad", 0.5, 7.2, sul_tasto_voicings},
      {"02 hijaz canon", "Ensemble Legato", 0.5, 9.3, hijaz_canon},
      {"03 pizzicato three against four", "Pizzicato Ostinato", 0.45, 5.0, pizzicato_three_four},
      {"04 tremolo rise", "Tremolo Swell", 0.55, 6.8, tremolo_rise},
      {"05 solo cello", "Solo Cello", 0.5, 10.4, solo_cello},
  };
  return list;
}

constexpr std::string_view trim = "trim";
constexpr std::string_view trim_unity = "Unity";
constexpr double trim_max_db = 24.0;
constexpr double trim_min_db = -36.0;
constexpr double decibels_per_decade = 20.0;
constexpr double decade = 10.0;

void add_part(Session& session, const Clip& clip, const Part& part) {
  const double trimmed = std::clamp(clip.gain_db, trim_min_db, trim_max_db);
  const double level =
      part.level * std::pow(decade, (clip.gain_db - trimmed) / decibels_per_decade);
  const auto track = parts::add_track(session, bow_engine, part.preset, level);
  if (!part.values.empty()) {
    const auto& table = audition::synth(bow_engine);
    auto values = take(session.instruments().native_instrument_values(track));
    for (const auto& [id, value] : part.values) {
      const auto index = native_dsp::find_parameter(table, id);
      if (!index)
        fail("no parameter " + std::string(id));
      values[*index] = value;
    }
    done(session.instruments().set_native_instrument_values(track, values));
  }
  const auto space = parts::add_effect(session, track, expanse, space_preset);
  parts::set_effect_value(session, track, space, expanse, parts::space_mix_id, clip.space_mix);
  const auto gain = parts::add_effect(session, track, trim, trim_unity);
  parts::set_effect_value(session, track, gain, trim, "trim.gain", trimmed);
  parts::add_clip(session, track, 0.0, part.notes, 0, part_beats);
  const project::AutomationTarget wheel =
      project::NativeParameterTarget{track, std::string(wheel_id)};
  for (const auto& point : part.wheel)
    take(session.automation().add_automation_point(track, wheel, ticks(point.beat), point.value));
}

void write_part(const fs::path& path, std::string_view name, const Part& part) {
  std::vector<midi_out::Control> controls;
  for (const auto& point : part.wheel)
    controls.push_back({point.beat, midi_out::mod_wheel_cc, point.value});
  midi_out::write(path, name, tempo_bpm, part.notes, controls);
}

} // namespace

void render_set(const fs::path& output, bool stems) {
  fs::create_directories(output);
  const auto work = output / ".projects";
  // Never clear a directory the tool did not make.
  if (!fs::create_directory(work))
    fail("scratch directory already exists: " + work.string());
  for (const auto& clip : clips()) {
    const std::string label = std::string(clip.name) + " - " + std::string(clip.preset);
    std::cout << label << '\n';
    const auto root = work / label;
    auto session = take(Session::create(root));
    done(session->song().set_tempo_event({0}, tempo_bpm, timeline::TempoShape::step));
    parts::add_picked_pads(*session, part_beats);
    const auto arrangement = clip.parts();
    for (const auto& part : arrangement) {
      add_part(*session, clip, part);
      write_part(output / (std::string(clip.name) + " - " + std::string(part.name) + ".mid"),
                 part.name, part);
    }
    audition::render(*session, root, output / (label + ".wav"), render_rate,
                     exporting::WavFormat::float32);
    if (stems)
      audition::render_stems(*session, root, output, label, render_rate);
  }
  fs::remove_all(work);
}

} // namespace oscillate::tools::strings
