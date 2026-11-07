#include "pads.hpp"

#include "audition.hpp"

#include "oscillate/application/midi_file_import.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::tools::pads {
namespace {

namespace fs = std::filesystem;
using audition::done;
using audition::fail;
using audition::take;

constexpr std::int64_t quarter = timeline::ticks_per_quarter;
constexpr int beats_per_bar = 4;
// The pad starts alone; the lead comes in after these bars.
constexpr int lead_in_bars = 2;
// The pad's clip: the lead-in, the lead's eight bars and two to close.
constexpr int part_bars = 12;
constexpr std::uint32_t render_rate = 48'000;
// The pad's track level under the lead, linear.
constexpr double pad_level = 0.5;
constexpr std::string_view lead_engine = "wren";
constexpr std::string_view lead_preset = "Lyrical Air";
constexpr std::string_view space_engine = "expanse";
constexpr std::string_view space_mix_id = "expanse.mix";

// C minor, with the B natural of Hijaz on G.
enum class Pitch : std::uint8_t { c = 0, d = 2, eb = 3, f = 5, g = 7, ab = 8, b = 11 };
constexpr int keys_per_octave = 12;
constexpr int octave_of_key_zero = -1; // Key 60 is C4.
constexpr std::uint8_t key(Pitch pitch, int octave) {
  return static_cast<std::uint8_t>((octave - octave_of_key_zero) * keys_per_octave +
                                   static_cast<int>(pitch));
}

constexpr std::uint8_t soft = 72;
constexpr std::uint8_t medium = 84;
constexpr std::uint8_t firm = 100;

struct Note {
  double beat;
  double length;
  std::uint8_t key;
  std::uint8_t velocity;
};
using P = Pitch;

// Sustained voicings that move with the lead's phrases but enter late or
// leave early, with gaps: an open fifth, then C minor, A♭, the G of the
// Hijaz, back to C, and C to close. The lead starts on beat 8.5.
std::vector<Note> held_part() {
  return {
      {0.0, 7.0, key(P::c, 3), medium},      {0.0, 7.0, key(P::g, 3), medium},
      {2.5, 4.5, key(P::eb, 4), soft},       {9.75, 7.25, key(P::c, 3), medium},
      {9.75, 7.25, key(P::g, 3), medium},    {11.5, 4.5, key(P::eb, 4), soft},
      {17.5, 6.5, key(P::ab, 2), medium},    {17.5, 6.5, key(P::eb, 3), medium},
      {18.25, 5.75, key(P::c, 4), soft},     {25.25, 5.0, key(P::g, 2), medium},
      {25.25, 5.0, key(P::d, 3), medium},    {26.5, 3.75, key(P::g, 3), soft},
      {31.0, 8.5, key(P::c, 3), medium},     {31.0, 8.5, key(P::g, 3), medium},
      {32.75, 6.75, key(P::eb, 4), soft},    {34.5, 3.0, key(P::f, 3), soft},
      {40.75, 7.25, key(P::c, 2), medium},   {40.75, 7.25, key(P::g, 2), medium},
      {41.5, 6.5, key(P::c, 3), soft},
  };
}

// Single strikes and the santur's quick double strokes, sparse and off the
// beat, leaving the tails to ring under the lead.
std::vector<Note> santur_part() {
  constexpr double strike = 0.2;
  return {
      {0.5, strike, key(P::c, 4), firm},     {0.75, strike, key(P::g, 4), medium},
      {2.25, strike, key(P::eb, 4), medium}, {4.0, strike, key(P::g, 3), soft},
      {5.75, strike, key(P::c, 5), medium},  {9.0, strike, key(P::c, 3), firm},
      {9.0, strike, key(P::c, 4), medium},   {10.75, strike, key(P::g, 4), soft},
      {13.5, strike, key(P::ab, 4), medium}, {13.75, strike, key(P::ab, 4), soft},
      {17.25, strike, key(P::c, 5), firm},   {17.5, strike, key(P::g, 4), medium},
      {20.75, strike, key(P::eb, 4), soft},  {24.5, strike, key(P::d, 5), medium},
      {24.75, strike, key(P::g, 4), soft},   {28.0, strike, key(P::c, 3), firm},
      {28.0, strike, key(P::c, 4), medium},  {31.25, strike, key(P::g, 4), medium},
      {31.5, strike, key(P::c, 5), soft},    {35.0, strike, key(P::ab, 4), medium},
      {38.75, strike, key(P::c, 4), firm},   {39.0, strike, key(P::g, 3), soft},
      {42.5, strike, key(P::g, 4), medium},  {42.75, strike, key(P::c, 5), soft},
      {44.75, strike, key(P::c, 4), soft},
  };
}

// The floor: the root an octave or two under the lead's low range, moving
// to A♭ and G with the harmony, with a fifth now and then.
std::vector<Note> low_part() {
  return {
      {0.0, 15.5, key(P::c, 3), medium},   {6.5, 8.0, key(P::g, 3), soft},
      {17.5, 7.0, key(P::ab, 2), medium},  {25.25, 5.25, key(P::g, 2), medium},
      {31.0, 8.5, key(P::c, 3), medium},   {33.5, 5.0, key(P::g, 3), soft},
      {40.75, 7.25, key(P::c, 3), medium},
  };
}

struct Audition {
  std::string_view name; // "<NN direction>"
  std::string_view engine;
  std::string_view preset;
  std::string_view space; // Expanse preset.
  double space_mix;
  std::vector<Note> (*part)();
  int octaves = 0; // The part transposed by this many octaves.
};

constexpr double wet = 0.45;
constexpr double cloud = 0.6;
// A flute plays nothing below C4, so a flute pad plays the part an octave up.
constexpr int flute_octaves = 1;

const std::vector<Audition>& auditions() {
  static const std::vector<Audition> list{
      {"02 breath", "nova", "Ney Breath", "Ambient Wash", wet, held_part},
      {"03 santur", "nova", "Santur Bloom", "Shimmer Heaven", wet, santur_part},
      {"06 flute cloud", "drift", "Ney Mist", "Endless Cloud", cloud, held_part},
      {"06b flute cloud", "wren", "Two Flutes", "Endless Cloud", cloud, held_part,
       flute_octaves},
      {"07 low", "nova", "Warm Sub Bed", "Dark Hall", wet, low_part},
  };
  return list;
}

std::size_t preset_index(const native_dsp::synth::EngineDescriptor& engine,
                         std::string_view name) {
  for (std::size_t i = 0; i < engine.presets.size(); ++i)
    if (engine.presets[i].name == name)
      return i;
  fail("no " + std::string(engine.id) + " preset " + std::string(name));
}

const native_dsp::synth::EngineDescriptor& synth(std::string_view engine) {
  const auto* found = native_dsp::synth::find_engine(engine);
  if (found == nullptr)
    fail("no synth " + std::string(engine));
  return *found->descriptor;
}

const native_dsp::synth::EngineDescriptor& effect(std::string_view engine) {
  const auto* found = native_dsp::effects::find_engine(engine);
  if (found == nullptr)
    fail("no effect " + std::string(engine));
  return *found->descriptor;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    fail("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

project::TrackId add_lead(application::ProjectSession& session,
                          const std::vector<std::uint8_t>& midi) {
  const auto track = take(session.instruments().add_native_instrument_track(
      lead_engine, preset_index(synth(lead_engine), lead_preset)));
  application::MidiImportOptions options;
  options.target = application::MidiImportOptions::Target::track;
  options.track = track;
  options.zero_based_bar = lead_in_bars;
  take(session.imports().import_midi_bytes(midi, options));
  return track;
}

void add_pad(application::ProjectSession& session, const Audition& audition) {
  const auto track = take(session.instruments().add_native_instrument_track(
      audition.engine, preset_index(synth(audition.engine), audition.preset)));
  done(session.midi_clips().add_midi_clip(track, {0}));
  const auto& placed = session.snapshot().tracks[*session.tracks().track_index(track)];
  const auto clip = placed.midi_clips.front().id;
  done(session.midi_clips().resize_midi_clip(clip, {part_bars * beats_per_bar * quarter}));
  for (const auto& note : audition.part()) {
    session.notes().set_new_note_velocity(note.velocity);
    const auto shifted = static_cast<std::uint8_t>(note.key + audition.octaves * keys_per_octave);
    done(session.notes().add_note_with_duration(
        clip, {static_cast<std::int64_t>(note.beat * static_cast<double>(quarter))},
        {static_cast<std::int64_t>(note.length * static_cast<double>(quarter))}, shifted));
  }
  const auto& space = effect(space_engine);
  const auto slot = take(session.effects().add_effect_engine(track, 0, space_engine,
                                                             preset_index(space, audition.space)));
  auto values = take(session.effects().native_effect_values(track, slot));
  const auto mix = native_dsp::synth::find_parameter(space, space_mix_id);
  if (!mix)
    fail("no parameter " + std::string(space_mix_id));
  values[*mix] = audition.space_mix;
  done(session.effects().set_native_effect_values(track, slot, values));
  done(session.tracks().set_track_level_pan(track, pad_level, 0.0));
}

} // namespace

void render_set(const fs::path& output, const fs::path& lead_midi) {
  fs::create_directories(output);
  const auto midi = read_file(lead_midi);
  const auto work = output / ".projects";
  fs::remove_all(work);
  fs::create_directories(work);
  for (const auto& item : auditions()) {
    const std::string label = std::string(item.name) + " - " + std::string(item.preset);
    std::cout << label << '\n';
    const auto root = work / label;
    auto session = take(application::ProjectSession::create(root));
    const auto lead = add_lead(*session, midi);
    add_pad(*session, item);
    audition::render(*session, root, output / (label + ".wav"), render_rate);
    done(session->tracks().set_track_muted(lead, true));
    audition::render(*session, root, output / (label + " (pad alone).wav"), render_rate);
  }
  fs::remove_all(work);
}

} // namespace oscillate::tools::pads
