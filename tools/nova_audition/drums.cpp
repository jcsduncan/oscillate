#include "drums.hpp"

#include "audition.hpp"
#include "parts.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/export/wav_writer.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::tools::drums {
namespace {

namespace fs = std::filesystem;
using audition::done;
using audition::fail;
using audition::take;
using parts::beats_per_bar;
using parts::Note;
using Session = application::ProjectSession;

constexpr std::uint32_t render_rate = 48'000;
constexpr std::string_view engine = "stick";
constexpr double track_level = 1.0;

// Stick's keys (ADR-0114, ADR-0121), General MIDI.
constexpr std::uint8_t kick = 36;
constexpr std::uint8_t side_stick = 37;
constexpr std::uint8_t snare = 38;
constexpr std::uint8_t second_snare = 40;
constexpr std::uint8_t floor_tom = 41;
constexpr std::uint8_t hat = 42;
constexpr std::uint8_t low_tom = 45;
constexpr std::uint8_t hat_pedal = 44;
constexpr std::uint8_t open_hat = 46;
constexpr std::uint8_t mid_tom = 47;
constexpr std::uint8_t high_tom = 50;
constexpr std::uint8_t crash = 49;
constexpr std::uint8_t ride = 51;
constexpr std::uint8_t ride_bell = 53;

// A pattern is a row of sixteenth steps per key. Each character is a
// stroke: accent, normal, soft, ghost; '.' is a rest.
constexpr std::uint8_t accent = 118;
constexpr std::uint8_t normal = 96;
constexpr std::uint8_t soft = 70;
constexpr std::uint8_t ghost = 36;
constexpr int steps_per_beat = 4;
constexpr double step_beats = 1.0 / steps_per_beat;
constexpr int steps_per_bar = steps_per_beat * beats_per_bar;
// A stroke's note: drums play out whatever the note-off.
constexpr double stroke_beats = step_beats;
// Strokes vary by up to this much velocity, the same every render.
constexpr int velocity_spread = 5;
constexpr int velocity_min = 1;
constexpr int velocity_max = 127;
// Spreads keys apart in a stroke's seed.
constexpr std::size_t seed_stride = 131;
constexpr int bars = 8;
constexpr int phrase_bars = 4;
constexpr double crash_length_beats = beats_per_bar;
// A hit alone: two seconds at this tempo.
constexpr double hit_bpm = 120.0;
constexpr double hit_beats = 4.0;

struct Row {
  std::uint8_t key;
  std::string_view steps; // one bar, sixteen steps
};

struct Groove {
  std::string_view name;
  std::string_view preset;
  double bpm;
  // How far each off-beat step is pushed late, in steps: 0 straight, 1/3
  // a sixteenth swing, 2/3 on the off-beat eighths a triplet feel.
  double swing_steps;
  bool swing_eighths;
  std::vector<Row> bar_a;
  std::vector<Row> bar_b;
  std::vector<Row> fill;
};

std::uint8_t velocity_of(char c) {
  switch (c) {
  case 'X':
    return accent;
  case 'x':
    return normal;
  case 'o':
    return soft;
  case 'g':
    return ghost;
  default:
    fail(std::string("unknown stroke '") + c + "'");
  }
}

// A small, fixed variation per stroke, so repeats are not identical.
int wobble(std::size_t seed) {
  constexpr std::uint64_t mix = 0x9E3779B97F4A7C15ULL;
  constexpr int shift = 33;
  const std::uint64_t h = (static_cast<std::uint64_t>(seed) + 1) * mix;
  return static_cast<int>((h >> shift) % (2 * velocity_spread + 1)) - velocity_spread;
}

void place(std::vector<Note>& notes, const Groove& groove, const std::vector<Row>& rows, int bar) {
  for (const auto& row : rows) {
    if (row.steps.size() != steps_per_bar)
      fail(std::string(groove.name) + ": a row is not sixteen steps");
    for (int step = 0; step < steps_per_bar; ++step) {
      const char c = row.steps[static_cast<std::size_t>(step)];
      if (c == '.')
        continue;
      const bool off_beat = groove.swing_eighths ? step % steps_per_beat == 2 : step % 2 == 1;
      const double late = off_beat ? groove.swing_steps : 0.0;
      const double beat = bar * beats_per_bar + (static_cast<double>(step) + late) * step_beats;
      const auto seed =
          static_cast<std::size_t>(bar * steps_per_bar + step) * seed_stride + row.key;
      const int v = std::clamp(velocity_of(c) + wobble(seed), velocity_min, velocity_max);
      notes.push_back({beat, stroke_beats, row.key, static_cast<std::uint8_t>(v)});
    }
  }
}

std::vector<Note> arrange(const Groove& groove) {
  std::vector<Note> notes;
  for (int bar = 0; bar < bars; ++bar) {
    const bool last_of_phrase = bar % phrase_bars == phrase_bars - 1;
    const auto& rows = last_of_phrase ? groove.fill : bar % 2 == 0 ? groove.bar_a : groove.bar_b;
    place(notes, groove, rows, bar);
  }
  // Into bar 5 and out at the end: a crash with the kick.
  for (const int bar : {phrase_bars, bars}) {
    notes.push_back({static_cast<double>(bar * beats_per_bar), crash_length_beats, crash, accent});
    notes.push_back({static_cast<double>(bar * beats_per_bar), stroke_beats, kick, accent});
  }
  return notes;
}

const std::vector<Groove>& grooves() {
  static const std::vector<Groove> list{
      {"01 funk sixteenths, ghost notes",
       "Funk Pocket",
       98.0,
       0.0,
       false,
       {{kick, "X.....x...x....."}, {snare, "....X..g.g..X..g"}, {hat, "XoxoXoxoXoxoXoxo"}},
       {{kick, "X..x......x..x.."},
        {snare, "....X..g.g..X.g."},
        {hat, "XoxoXoxoXoxoXo.o"},
        {open_hat, "..............x."}},
       {{kick, "X.....x...x....."},
        {snare, "....X..g.g..XgXg"},
        {hat, "XoxoXoxoXoxo...."},
        {high_tom, "............x..."},
        {mid_tom, ".............x.."},
        {floor_tom, "..............Xx"}}},
      {"02 dry break",
       "Break Crunch",
       92.0,
       0.0,
       false,
       {{kick, "X.X.......X..X.."}, {snare, "....X..g.g.gX..g"}, {hat, "x.x.x.x.x.x.xox."}},
       {{kick, "X.X.....X.X....."},
        {snare, "....X..g.g..X.g."},
        {hat, "x.x.x.x.x.x.x.x."},
        {open_hat, "..............o."}},
       {{kick, "X.X.......X....."}, {snare, "....X..g.g..XxXx"}, {hat, "x.x.x.x.x.x....."}}},
      {"03 half time in the room",
       "Big Room",
       84.0,
       0.0,
       false,
       {{kick, "X.......x.x....."},
        {snare, "........X......."},
        {ride, "x.x.x.x.x.x.x.x."},
        {ride_bell, "X.......X......."}},
       {{kick, "X.x.......x....."},
        {snare, "........X......g"},
        {ride, "x.x.x.x.x.x.x.x."},
        {ride_bell, "X.......X......."}},
       {{kick, "X.......x......."},
        {snare, "........X...xx.."},
        {high_tom, "..........xx...."},
        {low_tom, "..............x."},
        {floor_tom, "...............X"},
        {ride, "x.x.x.x.x......."}}},
      {"04 lo-fi, laid back",
       "Lo-Fi Dust",
       86.0,
       0.38,
       false,
       {{kick, "X......x..X....."}, {snare, "....X.......X..."}, {hat, "x.o.x.o.x.o.x.oo"}},
       {{kick, "X.x.......X....x"},
        {snare, "....X.......X..."},
        {hat, "x.o.x.o.x.o.x.o."},
        {side_stick, "...........o...."}},
       {{kick, "X......x..X....."}, {snare, "....X.......X.gX"}, {hat, "x.o.x.o.x.o.x..."}}},
      {"05 brushes, swung",
       "Brushes",
       84.0,
       2.0 / 3.0,
       true,
       {{kick, "o.......o......."},
        {snare, "..g...g...g...g."},
        {ride, "x...x.o.x...x.o."},
        {hat_pedal, "....x.......x..."}},
       {{kick, "o.......o......."},
        {snare, "..g...g...g.o.g."},
        {ride, "x...x.o.x...x.o."},
        {hat_pedal, "....x.......x..."}},
       {{kick, "o.......o......."},
        {snare, "..g...g...o.o.x."},
        {ride, "x...x.o.x......."},
        {hat_pedal, "....x.......x..."}}},
      {"06 the kit as recorded",
       "Natural",
       108.0,
       0.0,
       false,
       {{kick, "X.....X...X....."},
        {snare, "....X.......X..."},
        {second_snare, "................"},
        {hat, "x.x.x.x.x.x.x.x."}},
       {{kick, "X.....X...X..x.."},
        {snare, "....X.......X..g"},
        {hat, "x.x.x.x.x.x.x..."},
        {open_hat, "..............x."}},
       {{kick, "X.....X........."},
        {second_snare, "....X..........."},
        {high_tom, "........xx......"},
        {mid_tom, "..........xx...."},
        {low_tom, "............xx.."},
        {floor_tom, "..............XX"},
        {hat, "x.x.x.x........."}}},
      {"07 house, four to the floor",
       "House Tight",
       124.0,
       0.0,
       false,
       {{kick, "X...X...X...X..."},
        {snare, "....x.......x..."},
        {hat, "o.o.o.o.o.o.o.o."},
        {open_hat, "..x...x...x...x."}},
       {{kick, "X...X...X...X..."},
        {snare, "....x.......x..."},
        {side_stick, "..........g....."},
        {hat, "o.o.o.o.o.o.o.oo"},
        {open_hat, "..x...x...x...x."}},
       {{kick, "X...X...X...X..."},
        {snare, "....x.......x.gx"},
        {hat, "o.o.o.o.o.o.o..."},
        {open_hat, "..x...x...x....."}}},
  };
  return list;
}

} // namespace

void render_set(const fs::path& output) {
  fs::create_directories(output);
  const auto work = output / ".projects";
  // Never clear a directory the tool did not make.
  if (!fs::create_directory(work))
    fail("scratch directory already exists: " + work.string());
  for (const auto& groove : grooves()) {
    const std::string label = std::string(groove.name) + " - " + std::string(groove.preset);
    std::cout << label << '\n';
    const auto root = work / label;
    auto session = take(Session::create(root));
    done(session->song().set_tempo_event({0}, groove.bpm, timeline::TempoShape::step));
    const auto track = parts::add_track(*session, engine, groove.preset, track_level);
    parts::add_clip(*session, track, 0.0, arrange(groove), 0,
                    static_cast<double>(bars * beats_per_bar) + crash_length_beats);
    audition::render(*session, root, output / (label + ".wav"), render_rate,
                     exporting::WavFormat::float32);
  }
  // Each preset's kick and snare struck once, alone, for measuring.
  const auto hits = output / "hits";
  const auto hit_work = work / "hits";
  fs::create_directories(hits);
  fs::create_directories(hit_work);
  for (const auto& groove : grooves()) {
    for (const auto& [name, key] : {std::pair{"kick", kick}, std::pair{"snare", snare}}) {
      const std::string label = std::string(groove.preset) + " - " + name;
      const auto root = hit_work / label;
      if (fs::exists(root))
        continue; // A preset two grooves share.
      auto session = take(Session::create(root));
      done(session->song().set_tempo_event({0}, hit_bpm, timeline::TempoShape::step));
      const auto track = parts::add_track(*session, engine, groove.preset, track_level);
      parts::add_clip(*session, track, 0.0, {{0.0, stroke_beats, key, accent}}, 0, hit_beats);
      audition::render(*session, root, hits / (label + ".wav"), render_rate,
                       exporting::WavFormat::float32);
    }
  }
  fs::remove_all(work);
}

} // namespace oscillate::tools::drums
