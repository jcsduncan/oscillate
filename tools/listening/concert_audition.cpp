#include "oscillate/native_dsp/synth/concert.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// Renders Concert to 24-bit WAVs for listening and for comparison with
// recorded pianos: single notes across the keyboard at three velocities,
// and a pedalled phrase, from the installed library (ADR-0071). Usage:
//   oscillate_concert_audition <directory> [preset]
namespace {
using namespace synth_test;

constexpr double rate = 48'000.0;
constexpr double bpm = 80.0;
constexpr double note_seconds = 5.0;
constexpr double tail_seconds = 3.0;

void write_wav(const std::string& path, const std::vector<double>& left,
               const std::vector<double>& right) {
  std::ofstream file(path, std::ios::binary);
  const auto put = [&](std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
      file.put(static_cast<char>((value >> (8 * i)) & 0xFFU));
    }
  };
  const auto frames = static_cast<std::uint32_t>(left.size());
  const std::uint32_t data = frames * 2 * 3;
  file.write("RIFF", 4);
  put(36 + data, 4);
  file.write("WAVEfmt ", 8);
  put(16, 4);
  put(1, 2);
  put(2, 2);
  put(static_cast<std::uint32_t>(rate), 4);
  put(static_cast<std::uint32_t>(rate) * 2 * 3, 4);
  put(6, 2);
  put(24, 2);
  file.write("data", 4);
  put(data, 4);
  for (std::size_t i = 0; i < left.size(); ++i) {
    for (const double x : {left[i], right[i]}) {
      const auto sample =
          static_cast<std::int32_t>(std::lround(std::clamp(x, -1.0, 1.0) * 8'388'607.0));
      put(static_cast<std::uint32_t>(sample), 3);
    }
  }
}

void report(const std::string& name, const Rendered& out) {
  double peak_value = 0.0;
  for (std::size_t i = 0; i < out.left.size(); ++i) {
    peak_value = std::max({peak_value, std::fabs(out.left[i]), std::fabs(out.right[i])});
  }
  std::printf("%-22s peak %6.1f dBFS\n", name.c_str(), 20.0 * std::log10(peak_value + 1e-12));
}

// A short piece in C minor at 66 BPM: a left-hand arpeggio in eighths under
// a right-hand melody, the pedal changed just after each bar line, a swell
// through the middle and a soft close.
void render_piece(const SynthEngine& engine, const std::vector<double>& values,
                  const std::string& directory) {
  constexpr double piece_bpm = 66.0;
  struct Bar {
    int root;                                  // Left hand's root.
    std::vector<std::array<double, 3>> melody; // Beat, length, key.
  };
  const std::vector<Bar> bars{
      {36, {{1, 1, 67}, {2, 2, 75}}},
      {32, {{0, 1.5, 72}, {1.5, 0.5, 70}, {2, 2, 68}}},
      {39, {{0, 2, 67}, {2, 1, 70}, {3, 1, 75}}},
      {34, {{0, 3, 74}, {3, 1, 72}}},
      {36, {{0, 1, 72}, {1, 1, 75}, {2, 1.5, 79}, {3.5, 0.5, 77}}},
      {32, {{0, 2, 75}, {2, 1, 72}, {3, 1, 75}}},
      {29, {{0, 1.5, 77}, {1.5, 0.5, 75}, {2, 2, 72}}},
      {31, {{0, 2, 74}, {2, 2, 71}}},
      {36, {{0, 1, 67}, {1, 1, 72}, {2, 2, 75}}},
      {32, {{0, 2, 72}, {2, 2, 68}}},
      {29, {{0, 2, 72}, {2, 2, 71}}},
  };
  // Root, fifth, tenth, the next fifth, the octave and back: minor tenths.
  constexpr std::array<int, 8> arpeggio{0, 7, 15, 19, 24, 19, 15, 7};
  // Phrase dynamics per bar: rising to bar 7, then falling away.
  constexpr std::array<double, 12> swell{0.80, 0.84, 0.88, 0.90, 0.96, 1.00,
                                         1.04, 0.98, 0.88, 0.82, 0.78, 0.70};
  const double frames_per_beat = 60.0 / piece_bpm * rate;
  const auto at = [&](double beat) { return static_cast<std::uint64_t>(beat * frames_per_beat); };
  std::vector<Event> events;
  std::vector<PedalEvent> pedals;
  std::uint64_t id = 1;
  const auto play = [&](double beat, double length, int key, double velocity) {
    auto e = note(at(beat), at(length), static_cast<std::uint8_t>(key), id++,
                  static_cast<std::uint8_t>(std::clamp(velocity, 1.0, 127.0)));
    events.insert(events.end(), e.begin(), e.end());
  };
  for (std::size_t b = 0; b < bars.size(); ++b) {
    const double start = 4.0 * static_cast<double>(b);
    // In the eleventh bar the harmony moves to G for its second half.
    for (std::size_t i = 0; i < arpeggio.size(); ++i) {
      const int root = (b == 10 && i >= 4) ? 31 : bars[b].root;
      const int step =
          (b == 10 && i >= 4) ? std::array<int, 8>{0, 7, 16, 19, 23, 19, 16, 7}[i] : arpeggio[i];
      play(start + 0.5 * static_cast<double>(i), 0.48, root + step,
           (i % 2 == 0 ? 54.0 : 46.0) * swell[b]);
    }
    for (const auto& [beat, length, key] : bars[b].melody)
      play(start + beat, length * 0.97, static_cast<int>(key), 78.0 * swell[b]);
    pedals.push_back({at(start + 0.12), oscillate::project::MidiPedal::sustain, 1.0});
    pedals.push_back({at(start + 3.92), oscillate::project::MidiPedal::sustain, 0.0});
  }
  // The close: C minor, held under the pedal.
  const double end = 4.0 * static_cast<double>(bars.size());
  for (const int key : {36, 43, 48, 63, 67, 72})
    play(end, 5.0, key, (key < 60 ? 50.0 : 58.0) * swell.back());
  pedals.push_back({at(end + 0.12), oscillate::project::MidiPedal::sustain, 1.0});
  pedals.push_back({at(end + 6.0), oscillate::project::MidiPedal::sustain, 0.0});
  auto runtime = make_runtime(engine, values, rate);
  const auto frames =
      static_cast<std::size_t>(at(end + 6.0)) + static_cast<std::size_t>(4.0 * rate);
  const auto out = render(*runtime, events, frames, 128, rate, piece_bpm, pedals);
  write_wav(directory + "/piece.wav", out.left, out.right);
  report("piece", out);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <directory> [preset]\n", argv[0]);
    return 2;
  }
  const std::string directory = argv[1];
  const std::string preset_name = argc > 2 ? argv[2] : "Concert Grand";
  const auto library = concert::library::status();
  if (library.state != concert::library::State::ready) {
    std::fprintf(stderr, "Concert has no library (%s).\nInstall it with: %s\n",
                 library.message.c_str(), std::string(concert::library::install_command).c_str());
    return 1;
  }
  std::printf("%s from %s\n", library.name.c_str(), library.directory.c_str());
  const auto& engine = concert::engine();
  const auto values = preset(*engine.descriptor, preset_name);

  // Single notes, held for the whole render: A0, C2, C3, C4, C5, C6, C7.
  for (const std::uint8_t key : {21, 36, 48, 60, 72, 84, 96}) {
    for (const std::uint8_t velocity : {30, 70, 110}) {
      auto runtime = make_runtime(engine, values, rate);
      const auto frames = static_cast<std::size_t>(note_seconds * rate);
      const auto out =
          render(*runtime, note(0, frames - 1, key, 1, velocity), frames, 128, rate, bpm);
      const std::string name = "note_" + std::to_string(key) + "_v" + std::to_string(velocity);
      write_wav(directory + "/" + name + ".wav", out.left, out.right);
      report(name, out);
    }
  }

  // A pedalled phrase: a left-hand broken chord under a right-hand line.
  struct N {
    double beat;
    double length;
    std::uint8_t key;
    std::uint8_t velocity;
  };
  const std::vector<N> phrase{
      {0.0, 0.9, 45, 70},  {0.5, 0.9, 52, 60},  {1.0, 0.9, 57, 62},  {1.5, 0.9, 60, 60},
      {0.0, 1.9, 76, 88},  {2.0, 0.9, 74, 80},  {3.0, 0.9, 72, 78},  {4.0, 0.9, 41, 72},
      {4.5, 0.9, 48, 60},  {5.0, 0.9, 53, 62},  {5.5, 0.9, 57, 60},  {4.0, 1.4, 72, 86},
      {5.5, 0.5, 74, 74},  {6.0, 1.9, 76, 90},  {8.0, 0.9, 43, 72},  {8.5, 0.9, 50, 60},
      {9.0, 0.9, 55, 62},  {9.5, 0.9, 59, 60},  {8.0, 1.9, 79, 92},  {10.0, 0.9, 77, 80},
      {11.0, 0.9, 76, 78}, {12.0, 4.0, 45, 66}, {12.0, 4.0, 52, 58}, {12.0, 4.0, 57, 60},
      {12.0, 4.0, 60, 62}, {12.0, 4.0, 64, 72}, {12.0, 4.0, 69, 84}};
  const double frames_per_beat = 60.0 / bpm * rate;
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto& n : phrase) {
    auto e = note(static_cast<std::uint64_t>(n.beat * frames_per_beat),
                  static_cast<std::uint64_t>(n.length * frames_per_beat), n.key, id++, n.velocity);
    events.insert(events.end(), e.begin(), e.end());
  }
  // Pedal changes just after each bar's bass note, the way a pianist does.
  std::vector<PedalEvent> pedals;
  for (const double beat : {0.0, 4.0, 8.0, 12.0}) {
    const auto at = static_cast<std::uint64_t>((beat + 0.15) * frames_per_beat);
    if (beat > 0.0) {
      pedals.push_back({at - static_cast<std::uint64_t>(0.1 * frames_per_beat),
                        oscillate::project::MidiPedal::sustain, 0.0});
    }
    pedals.push_back({at, oscillate::project::MidiPedal::sustain, 1.0});
  }
  pedals.push_back({static_cast<std::uint64_t>(16.0 * frames_per_beat),
                    oscillate::project::MidiPedal::sustain, 0.0});
  auto runtime = make_runtime(engine, values, rate);
  const auto frames = static_cast<std::size_t>(16.0 * frames_per_beat + tail_seconds * rate);
  const auto out = render(*runtime, events, frames, 128, rate, bpm, pedals);
  write_wav(directory + "/phrase.wav", out.left, out.right);
  report("phrase", out);
  render_piece(engine, values, directory);
  return 0;
}
