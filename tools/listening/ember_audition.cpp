#include "oscillate/native_dsp/synth/ember.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Renders Ember's presets to 24-bit WAVs for listening, each playing a
// phrase that suits its category (chords for pads and keys, a line for
// leads and basses, an arpeggio for plucks), and prints its level. Usage:
//   oscillate_ember_audition <directory> [preset name ...]
namespace {
using namespace synth_test;

constexpr double rate = 48'000.0;
constexpr double bpm = 96.0;
constexpr double tail_seconds = 4.0;

struct Note {
  double beat;
  double length;
  std::uint8_t key;
  std::uint8_t velocity;
};

// Am9, Fmaj7, Cmaj7/E, G6: four beats each for pads, two for keys.
std::vector<Note> chords(double beats_each) {
  const std::vector<std::vector<std::uint8_t>> shapes{
      {57, 60, 64, 67, 71}, {53, 57, 60, 64, 69}, {52, 55, 60, 64, 71}, {55, 59, 62, 64, 67}};
  std::vector<Note> out;
  for (std::size_t c = 0; c < shapes.size(); ++c) {
    for (const auto key : shapes[c]) {
      out.push_back({static_cast<double>(c) * beats_each, beats_each * 0.95, key, 92});
    }
  }
  return out;
}

std::vector<Note> melody() {
  const std::vector<std::pair<std::uint8_t, double>> line{
      {69, 1.0}, {72, 0.5}, {74, 0.5}, {76, 1.5}, {74, 0.5}, {72, 1.0}, {71, 1.0},
      {69, 0.5}, {67, 0.5}, {69, 2.0}, {64, 1.0}, {67, 1.0}, {69, 3.0}};
  std::vector<Note> out;
  double beat = 0.0;
  for (const auto& [key, length] : line) {
    out.push_back({beat, length * 0.98, key, 100});
    beat += length;
  }
  return out;
}

std::vector<Note> bassline() {
  const std::vector<std::uint8_t> keys{33, 33, 45, 33, 36, 36, 48, 31,
                                       29, 29, 41, 29, 31, 43, 31, 35};
  std::vector<Note> out;
  for (std::size_t i = 0; i < 32; ++i) {
    const auto key = keys[i % keys.size()];
    out.push_back(
        {0.5 * static_cast<double>(i), 0.4, key, static_cast<std::uint8_t>(i % 4 == 0 ? 120 : 88)});
  }
  return out;
}

std::vector<Note> arpeggio() {
  const std::vector<std::uint8_t> up{57, 60, 64, 67, 71, 67, 64, 60};
  const std::vector<int> roots{0, -4, -5, -2};
  std::vector<Note> out;
  for (std::size_t bar = 0; bar < roots.size(); ++bar) {
    for (std::size_t i = 0; i < 16; ++i) {
      out.push_back({4.0 * static_cast<double>(bar) + 0.25 * static_cast<double>(i), 0.2,
                     static_cast<std::uint8_t>(up[i % up.size()] + roots[bar]), 96});
    }
  }
  return out;
}

std::vector<Note> phrase_for(std::string_view category, std::string_view name) {
  if (category == "Bass") {
    return bassline();
  }
  if (category == "Lead") {
    return melody();
  }
  if (category == "Pluck") {
    return arpeggio();
  }
  if (name == "Fifths Drone") {
    return {{0.0, 8.0, 45, 90}, {8.0, 8.0, 41, 90}};
  }
  if (category == "Pad" || category == "Strings" || category == "Circuit") {
    return chords(4.0);
  }
  return chords(2.0);
}

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

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <directory> [preset ...]\n", argv[0]);
    return 2;
  }
  const std::string directory = argv[1];
  const auto& engine = ember::engine();
  const auto& table = *engine.descriptor;
  for (const auto& entry : table.presets) {
    bool wanted = argc == 2;
    for (int a = 2; a < argc; ++a) {
      wanted = wanted || entry.name == argv[a];
    }
    if (!wanted) {
      continue;
    }
    auto runtime = make_runtime(engine, preset(table, entry.name), rate);
    std::vector<Event> events;
    std::uint64_t id = 1;
    double last_beat = 0.0;
    const double frames_per_beat = 60.0 / bpm * rate;
    for (const auto& n : phrase_for(entry.category, entry.name)) {
      const auto start = static_cast<std::uint64_t>(std::lround(n.beat * frames_per_beat));
      const auto length = static_cast<std::uint64_t>(std::lround(n.length * frames_per_beat));
      auto e = note(start, length, n.key, id++, n.velocity);
      events.insert(events.end(), e.begin(), e.end());
      last_beat = std::max(last_beat, n.beat + n.length);
    }
    const auto frames = static_cast<std::size_t>(last_beat * frames_per_beat + tail_seconds * rate);
    const auto out = render(*runtime, events, frames, 128, rate, bpm);
    double peak = 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i) {
      peak = std::max({peak, std::fabs(out.left[i]), std::fabs(out.right[i])});
      sum += 0.5 * (out.left[i] * out.left[i] + out.right[i] * out.right[i]);
    }
    const double rms = std::sqrt(sum / static_cast<double>(out.left.size()));
    std::string file(entry.name);
    std::replace(file.begin(), file.end(), ' ', '_');
    std::erase(file, '\'');
    write_wav(directory + "/" + file + ".wav", out.left, out.right);
    std::printf("%-18s %-8s rms %6.1f dBFS  peak %6.1f dBFS\n", std::string(entry.name).c_str(),
                std::string(entry.category).c_str(), 20.0 * std::log10(rms + 1e-12),
                20.0 * std::log10(peak + 1e-12));
  }
  return 0;
}
