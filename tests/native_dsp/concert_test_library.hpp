#pragma once

// A small synthetic library in Concert's installed format (ADR-0071), so
// tests hear Concert without the 1.3 GB Salamander download and check it
// against known tones. Regions every three semitones, as the Salamander
// maps them; three velocity layers of the same decaying harmonic tone at
// three levels; a noise click per key for the key release; a short tone
// per region for the string release; two pedal thumps each way.

#include "oscillate/native_dsp/synth/concert.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace concert_test {

namespace concert = oscillate::native_dsp::synth::concert;

inline constexpr int region_spacing = 3;
inline constexpr int last_damped_key = 88;
inline constexpr double rate = concert::library::pack_rate;
struct Layer {
  int low_velocity;
  int high_velocity;
  double amplitude;
};
inline constexpr std::array<Layer, 3> layers{{{1, 40, 0.15}, {41, 90, 0.4}, {91, 127, 0.9}}};
inline constexpr std::size_t partials = 4;
inline constexpr double decay_seconds = 0.6;
inline constexpr double attack_seconds = 0.002;
inline constexpr double right_share = 0.8; // The right channel's level.
inline constexpr double full_scale = 32'000.0;
inline constexpr double release_seconds = 0.05;
inline constexpr double release_gain_db = -30.0;
inline constexpr double release_track = 0.8;
inline constexpr double resonance_seconds = 0.2;
inline constexpr double resonance_gain_db = -20.0;
inline constexpr double pedal_seconds = 0.1;
inline constexpr double pedal_gain_db = -20.0;
// Notes longer than the resident start stream from disk; shorter ones stay
// whole in memory, so a render cannot outrun the disk (live-rendering tests).
inline constexpr double streamed_note_seconds = 1.25;
inline constexpr double resident_note_seconds = 0.3;

inline double root_hz(int key) { return 440.0 * std::exp2(static_cast<double>(key - 69) / 12.0); }

// The region a key plays from: roots at A0 and every third semitone up.
inline int root_of(int key) {
  const int below =
      concert::first_key + (key - concert::first_key + 1) / region_spacing * region_spacing;
  return std::min(below, concert::last_key);
}

// A layer's tone at (fractional) source frame `frame`, before int16
// rounding (left channel).
inline double tone_at(int root, double amplitude, double frame) {
  const double t = frame / rate;
  if (t < 0.0) {
    return 0.0;
  }
  double sum = 0.0;
  for (std::size_t n = 1; n <= partials; ++n) {
    sum += std::sin(2.0 * std::numbers::pi * root_hz(root) * static_cast<double>(n) * t) /
           static_cast<double>(n);
  }
  const double attack =
      t < attack_seconds ? 0.5 - 0.5 * std::cos(std::numbers::pi * t / attack_seconds) : 1.0;
  return amplitude * 0.5 * sum * attack * std::exp(-t / decay_seconds);
}

inline double tone(int root, double amplitude, std::size_t i) {
  return tone_at(root, amplitude, static_cast<double>(i));
}

inline std::int16_t quantise(double x) {
  return static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0, 1.0) * full_scale));
}

// Writes the library into `directory` (created or replaced).
inline void write_library(const std::filesystem::path& directory, double note_seconds) {
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  std::ofstream pack(directory / "concert.pack", std::ios::binary);
  std::vector<std::string> lines;
  std::uint64_t offset = 0;
  const auto add = [&](std::string_view kind, int first, int last, int root, int low, int high,
                       const std::vector<std::int16_t>& pcm, double peak_db, double gain_db,
                       double track, double decay) {
    const auto frames = pcm.size() / 2;
    pack.write(reinterpret_cast<const char*>(pcm.data()),
               static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
    char line[256];
    std::snprintf(line, sizeof line, "sample %s %d %d %d %d %d %llu %zu %.9e %.3f %g %g %g x",
                  std::string(kind).c_str(), first, last, root, low, high,
                  static_cast<unsigned long long>(offset), frames, 1.0 / full_scale, peak_db,
                  gain_db, track, decay);
    lines.emplace_back(line);
    offset += frames;
  };
  const auto note_frames = static_cast<std::size_t>(note_seconds * rate);
  for (int root = concert::first_key; root <= concert::last_key; root += region_spacing) {
    const int first = root == concert::first_key ? root : root - 1;
    const int last = std::min(root + 1, concert::last_key);
    for (const auto& layer : layers) {
      std::vector<std::int16_t> pcm(note_frames * 2);
      for (std::size_t i = 0; i < note_frames; ++i) {
        const double x = tone(root, layer.amplitude, i);
        pcm[2 * i] = quantise(x);
        pcm[2 * i + 1] = quantise(right_share * x);
      }
      add("note", first, last, root, layer.low_velocity, layer.high_velocity, pcm,
          20.0 * std::log10(layer.amplitude), 0.0, 0.0, 0.0);
    }
    const auto ring = static_cast<std::size_t>(resonance_seconds * rate);
    std::vector<std::int16_t> resonance(ring * 2);
    for (std::size_t i = 0; i < ring; ++i) {
      const double x =
          tone(root, 0.5, i) * (1.0 - static_cast<double>(i) / static_cast<double>(ring));
      resonance[2 * i] = quantise(x);
      resonance[2 * i + 1] = quantise(x);
    }
    add("resonance", first, last, root, 1, 127, resonance, -6.0, resonance_gain_db, 0.9, 6.0);
  }
  std::uint32_t seed = 1;
  const auto noise = [&] {
    seed = seed * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(seed >> 8U) / 8'388'608.0 - 1.0;
  };
  for (int key = concert::first_key; key <= concert::last_key; ++key) {
    const auto frames = static_cast<std::size_t>(release_seconds * rate);
    std::vector<std::int16_t> click(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
      const double x = 0.5 * noise() * (1.0 - static_cast<double>(i) / static_cast<double>(frames));
      click[2 * i] = quantise(x);
      click[2 * i + 1] = quantise(x);
    }
    add("release", key, key, key, 1, 127, click, -6.0, release_gain_db, release_track, 2.0);
  }
  for (const std::string_view kind : {"pedal_down", "pedal_down", "pedal_up", "pedal_up"}) {
    const auto frames = static_cast<std::size_t>(pedal_seconds * rate);
    std::vector<std::int16_t> thump(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double x = 0.5 * std::sin(2.0 * std::numbers::pi * 50.0 * t) * std::exp(-t / 0.03);
      thump[2 * i] = quantise(x);
      thump[2 * i + 1] = quantise(x);
    }
    add(kind, 0, 0, 0, 1, 127, thump, -6.0, pedal_gain_db, 0.0, 0.0);
  }
  pack.close();
  std::ofstream manifest(directory / concert::library::manifest_file);
  manifest << concert::library::format_line << "\nname Concert test tones\n"
           << "attribution Synthetic tones for Oscillate's tests\n"
           << "rate " << concert::library::pack_rate << "\npack concert.pack "
           << offset * 2 * sizeof(std::int16_t) << "\nlast_damped_key " << last_damped_key << "\n";
  for (const auto& line : lines) {
    manifest << line << '\n';
  }
}

// A per-process directory in the build's scratch (TMPDIR), removed at exit.
inline std::filesystem::path scratch_directory(std::string_view name) {
  struct Remover {
    std::vector<std::filesystem::path> paths;
    ~Remover() {
      for (const auto& path : paths) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    }
  };
  static Remover remover;
  auto path = std::filesystem::temp_directory_path() /
              (std::string(name) + "-" + std::to_string(::getpid()));
  remover.paths.push_back(path);
  return path;
}

// Writes the library and makes it the one Concert plays, for this process.
inline std::filesystem::path install(double note_seconds = streamed_note_seconds) {
  const auto directory = scratch_directory("concert-test-library");
  write_library(directory, note_seconds);
  const auto status = concert::library::load(directory);
  if (status.state != concert::library::State::ready) {
    throw std::runtime_error("the Concert test library did not load: " + status.message);
  }
  return directory;
}

} // namespace concert_test
