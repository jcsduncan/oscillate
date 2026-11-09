#include "breakdown.hpp"

#include "audition.hpp"
#include "parts.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/export/wav_writer.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/project/automation_types.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::tools::breakdown {
namespace {

namespace fs = std::filesystem;
using audition::done;
using audition::fail;
using audition::preset_index;
using audition::take;
using parts::add_clip;
using parts::add_effect;
using parts::add_track;
using parts::beats_per_bar;
using parts::key;
using parts::keys_per_octave;
using parts::Note;
using parts::set_effect_value;
using parts::ticks;
using P = parts::Pitch;

constexpr std::uint32_t render_rate = 48'000;
constexpr double tempo_bpm = 120.0;

// The sections, in beats from bar 1: the pads alone, the build, the hit.
constexpr int pad_bars = 4;
constexpr int build_bars = 2;
constexpr int hit_bars = 8;
constexpr double build_start = pad_bars * beats_per_bar;
constexpr double hit_start = build_start + build_bars * beats_per_bar;
constexpr double hit_beats = hit_bars * beats_per_bar;
// A chord lasts two bars in the hit: C minor, A♭, E♭, B♭.
constexpr double chord_beats = 2 * beats_per_bar;
constexpr double sixteenth = 0.25;

// ---- Instruments and effects -------------------------------------------------------------

constexpr std::string_view nova = "nova";
constexpr std::string_view anvil = "anvil";
constexpr std::string_view pulse = "pulse";
constexpr std::string_view pump = "pump";
constexpr std::string_view expanse = parts::space_engine;

constexpr std::string_view rolling_preset = "Rolling Sixteenth Bass";
constexpr std::string_view riser_preset = "Noise Riser";
constexpr std::string_view swell_preset = "Reverse Swell";
constexpr std::string_view snare_kit = "Minimal";
constexpr std::string_view house_kick = "Tech House Thump";
constexpr std::string_view pump_preset = "Classic Pump";
constexpr std::string_view snare_space = "Bright Plate";

// Nova's LFO sync choice for two bars, so a riser's sweep spans the build.
constexpr double lfo_two_bars = 3.0;
constexpr std::uint8_t kick_key = 36;  // Anvil plays any key; C1 as its reference loops.
constexpr std::uint8_t snare_key = 38; // Pulse's snare pad, D1.
constexpr std::uint8_t kick_velocity = 120;
constexpr double kick_length = 0.5;

// Track levels, linear.
constexpr double kick_level = 0.35;
constexpr double rolling_level = 0.85;
constexpr double riser_level = 0.6;
constexpr double swell_noise_level = 0.35;
constexpr double snare_level = 0.4;
// Pump's depth on the bass, which the kick ducks hard.
constexpr double bass_duck = 0.85;

// ---- Session helpers ---------------------------------------------------------------------

using Session = application::ProjectSession;

void set_synth_value(Session& session, project::TrackId track, std::string_view engine,
                     std::string_view id, double value) {
  auto values = take(session.instruments().native_instrument_values(track));
  const auto index = native_dsp::find_parameter(audition::synth(engine), id);
  if (!index)
    fail("no parameter " + std::string(id));
  values[*index] = value;
  done(session.instruments().set_native_instrument_values(track, values));
}

// Ducks the track from the kick: Pump on the tempo, once a beat.
void add_pump(Session& session, project::TrackId track, double depth) {
  const auto slot = add_effect(session, track, pump, pump_preset);
  set_effect_value(session, track, slot, pump, "pump.depth", depth);
}

struct Point {
  double beat;
  double value;
};

void automate(Session& session, project::TrackId track, const project::AutomationTarget& target,
              const std::vector<Point>& points) {
  for (const auto& point : points)
    take(session.automation().add_automation_point(track, target, ticks(point.beat), point.value));
}

project::AutomationTarget level_of(project::TrackId track) {
  return project::TrackControlTarget{track, project::TrackControlTarget::Key::channel_level};
}

project::AutomationTarget effect_parameter(project::InsertSlotId slot, std::string_view id) {
  return project::NativeEffectParameterTarget{slot, std::string(id)};
}

// ---- Harmony -----------------------------------------------------------------------------

// The roots in the bass's octave, two bars each: C, A♭, E♭, B♭.
const std::array<std::uint8_t, 4>& roots() {
  static const std::array<std::uint8_t, 4> list{key(P::c, 2), key(P::ab, 1), key(P::eb, 2),
                                                key(P::bb, 1)};
  return list;
}

constexpr std::uint8_t accent = 112;
constexpr std::uint8_t strong = 100;
constexpr std::uint8_t normal = 88;
constexpr std::uint8_t light = 76;

// ---- Candidate parts (beats from the hit) ------------------------------------------------

// The root on the three sixteenths after each beat (the kick takes the
// beat): the "a" of beats 3 and 7 jumps an octave, beat 6 drops its first
// note, and each chord's last note leads into the next root.
std::vector<Note> rolling_bass() {
  constexpr double note = 0.2;
  constexpr std::array<std::uint8_t, 3> velocities{normal, accent, light};
  std::vector<Note> notes;
  for (std::size_t c = 0; c < roots().size(); ++c) {
    const auto root = roots()[c];
    const auto next = roots()[(c + 1) % roots().size()];
    for (int beat = 0; beat < chord_beats; ++beat) {
      for (int s = 1; s <= 3; ++s) {
        if (beat == 5 && s == 1)
          continue;
        auto k = root;
        if ((beat == 2 || beat == 6) && s == 3)
          k = static_cast<std::uint8_t>(root + keys_per_octave);
        if (beat == chord_beats - 1 && s == 3)
          k = next;
        notes.push_back({static_cast<double>(c) * chord_beats + beat + s * sixteenth, note, k,
                         velocities[static_cast<std::size_t>(s - 1)]});
      }
    }
  }
  return notes;
}

// ---- Sections ----------------------------------------------------------------------------

struct Pads {
  parts::PadTrack flutes;
  parts::PadTrack breath;
  parts::PadTrack sub;
};

// The picked pads as in the pad auditions, cut at the build.
Pads add_pads(Session& session) {
  return {parts::add_pad(session, parts::two_flutes, build_start, build_start),
          parts::add_pad(session, parts::ney_breath, build_start, build_start),
          parts::add_pad(session, parts::warm_sub_bed, build_start, build_start)};
}

enum class Build : std::uint8_t { riser, swell };

// The pads' reverb freezes as they stop and fades away under a two-bar
// noise riser.
constexpr double freeze_fade_floor = 0.05;
constexpr double fade_cut = 0.25; // Beats before the hit that the frozen tail is gone.
void riser_build(Session& session, const Pads& pads) {
  for (const auto* pad : {&pads.flutes, &pads.breath, &pads.sub}) {
    automate(session, pad->track, effect_parameter(pad->space, "expanse.freeze"),
             {{0.0, 0.0}, {build_start, 1.0}});
    automate(session, pad->track, level_of(pad->track),
             {{0.0, parts::pad_level},
              {build_start, parts::pad_level},
              {hit_start - fade_cut, freeze_fade_floor},
              {hit_start, 0.0}});
  }
  const auto riser = add_track(session, nova, riser_preset, riser_level);
  set_synth_value(session, riser, nova, "nova.lfo.1.sync", lfo_two_bars);
  add_clip(session, riser, build_start, {{0.0, build_bars * beats_per_bar, key(P::c, 4), strong}});
}

// The pads come back on C minor and swell as if reversed, washing fully
// into their reverb, and stop dead on the hit; a soft snare roll rises
// with them and leaves the last sixteenth empty.
constexpr double swell_floor = 0.03;
constexpr double swell_peak = 0.8;
constexpr double tail_fade_beats = 1.0;
constexpr double swell_cut = 0.05;
void swell_build(Session& session, const Pads& pads) {
  const std::vector<Note> chord{{0.0, build_bars * beats_per_bar, key(P::c, 3), parts::medium},
                                {0.0, build_bars * beats_per_bar, key(P::g, 3), parts::medium},
                                {0.0, build_bars * beats_per_bar, key(P::eb, 4), parts::soft}};
  for (const auto* pad : {&pads.flutes, &pads.breath}) {
    const int octaves = pad == &pads.flutes ? parts::flute_octaves : 0;
    add_clip(session, pad->track, build_start, chord, octaves);
    automate(session, pad->track, level_of(pad->track),
             {{0.0, parts::pad_level},
              {build_start, parts::pad_level},
              {build_start + tail_fade_beats, swell_floor},
              {hit_start - swell_cut, swell_peak},
              {hit_start, 0.0}});
    automate(session, pad->track, effect_parameter(pad->space, parts::space_mix_id),
             {{0.0, pad == &pads.flutes ? parts::cloud : parts::wet},
              {build_start, pad == &pads.flutes ? parts::cloud : parts::wet},
              {hit_start - swell_cut, 1.0}});
  }
  automate(session, pads.sub.track, level_of(pads.sub.track),
           {{0.0, parts::pad_level}, {build_start, parts::pad_level}, {hit_start, 0.0}});

  const auto noise = add_track(session, nova, swell_preset, swell_noise_level);
  set_synth_value(session, noise, nova, "nova.lfo.1.sync", lfo_two_bars);
  add_clip(session, noise, build_start,
           {{0.0, build_bars * beats_per_bar - swell_cut, key(P::c, 4), strong}});

  // Eighths off the beat, then sixteenths, then thirty-seconds, growing
  // from a whisper.
  constexpr std::uint8_t roll_from = 30;
  constexpr std::uint8_t roll_to = 96;
  constexpr double thirty_second = sixteenth / 2;
  constexpr double hit = 0.1;
  std::vector<double> at;
  for (double b = 0.5; b < beats_per_bar; b += 2 * sixteenth)
    at.push_back(b);
  for (double b = beats_per_bar; b < beats_per_bar + 2; b += sixteenth)
    at.push_back(b);
  for (double b = beats_per_bar + 2; b < build_bars * beats_per_bar - sixteenth; b += thirty_second)
    at.push_back(b);
  std::vector<Note> roll;
  const double span = build_bars * beats_per_bar;
  for (const double b : at)
    roll.push_back({b, hit, snare_key,
                    static_cast<std::uint8_t>(roll_from + (roll_to - roll_from) * b / span)});
  const auto snare = add_track(session, pulse, snare_kit, snare_level);
  add_effect(session, snare, expanse, snare_space);
  add_clip(session, snare, build_start, roll);
}

void add_kick(Session& session, std::string_view preset) {
  std::vector<Note> kicks;
  for (double b = 0.0; b < hit_beats; b += 1.0)
    kicks.push_back({b, kick_length, kick_key, kick_velocity});
  add_clip(session, add_track(session, anvil, preset, kick_level), hit_start, kicks);
}

// ---- Candidates --------------------------------------------------------------------------

struct Candidate {
  std::string_view name;   // "<NN candidate>"
  std::string_view preset; // For the file name.
  std::vector<Build> builds;
  std::string_view kick;
  void (*hit)(Session&);
};

// The rolling bass, ducked by Pump.
void hit_rolling(Session& session) {
  const auto track = add_track(session, nova, rolling_preset, rolling_level);
  add_pump(session, track, bass_duck);
  add_clip(session, track, hit_start, rolling_bass());
}

const std::vector<Candidate>& candidates() {
  static const std::vector<Candidate> list{
      {"03 rolling bass", rolling_preset, {Build::riser, Build::swell}, house_kick, hit_rolling},
  };
  return list;
}

std::string_view build_label(Build build) {
  return build == Build::riser ? " (A riser)" : " (B swell)";
}

} // namespace

void render_set(const fs::path& output, bool stems) {
  fs::create_directories(output);
  const auto work = output / ".projects";
  // Never clear a directory the tool did not make.
  if (!fs::create_directory(work))
    fail("scratch directory already exists: " + work.string());
  for (const auto& candidate : candidates()) {
    for (const auto build : candidate.builds) {
      std::string label = std::string(candidate.name) + " - " + std::string(candidate.preset);
      label += build_label(build);
      std::cout << label << '\n';
      const auto root = work / label;
      auto session = take(Session::create(root));
      done(session->song().set_tempo_event({0}, tempo_bpm, timeline::TempoShape::step));
      const auto pads = add_pads(*session);
      if (build == Build::riser)
        riser_build(*session, pads);
      else
        swell_build(*session, pads);
      add_kick(*session, candidate.kick);
      candidate.hit(*session);
      audition::render(*session, root, output / (label + ".wav"), render_rate,
                       exporting::WavFormat::float32);
      if (stems)
        audition::render_stems(*session, root, output, label, render_rate);
    }
  }
  fs::remove_all(work);
}

} // namespace oscillate::tools::breakdown
