#pragma once

// A small synthetic library in Fret's installed format (ADR-0088), so tests
// hear Fret without the Black and Blue download and can tell its samples
// apart. Every key from B0 to E4 has each articulation as a decaying tone at
// the key's pitch on its own harmonic (pluck the fundamental, staccato the
// second, ghost the third, behind the bridge the fourth), so a spectrum
// says which articulation played. Layers differ in recorded level, round
// robins in a small level step, and releases are short noise bursts.

#include "oscillate/native_dsp/synth/fret.hpp"
#include "oscillate/test/check.hpp"

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
#include <system_error>
#include <unistd.h>
#include <vector>

namespace fret_test {

namespace fret = oscillate::native_dsp::synth::fret;

inline constexpr double rate = fret::library::pack_rate;
inline constexpr double a4_hz = 440.0;
inline constexpr int a4_key = 69;
inline constexpr double semitones_per_octave = 12.0;
inline constexpr double full_scale = 32'000.0;
inline constexpr double peak = 0.9; // Each sample's peak, as the installer normalises.
inline constexpr double decay_seconds = 0.5;
inline constexpr double attack_seconds = 0.002;
inline constexpr double release_seconds = 0.05;
inline constexpr double release_db = -10.0;
inline constexpr std::uint32_t round_robins = 2;
// Each round robin plays this much quieter than the one before.
inline constexpr double round_robin_step_db = -1.0;
// Notes longer than the resident start stream; shorter ones stay whole.
inline constexpr double streamed_note_seconds = 1.0;
inline constexpr double resident_note_seconds = 0.3;
inline constexpr std::size_t alignment_frames = 2'048; // 4 KiB of mono int16.

struct Articulation {
  std::string_view name;
  std::uint32_t harmonic;
  std::vector<double> layer_db; // Recorded level of each layer, softest first.
};

inline const std::array<Articulation, 4>& articulations() {
  static const std::array<Articulation, 4> all{{
      {"pluck", 1, {-18.0, -12.0, -6.0, -1.0}},
      {"staccato", 2, {-8.0}},
      {"ghost", 3, {-20.0, -14.0}},
      {"bridge", 4, {-7.0}},
  }};
  return all;
}

inline double key_hz(int key) {
  return a4_hz * std::exp2(static_cast<double>(key - a4_key) / semitones_per_octave);
}

inline double tone(int key, std::uint32_t harmonic, double frame) {
  const double t = frame / rate;
  const double attack =
      t < attack_seconds ? 0.5 - 0.5 * std::cos(std::numbers::pi * t / attack_seconds) : 1.0;
  return peak * attack * std::exp(-t / decay_seconds) *
         std::sin(2.0 * std::numbers::pi * key_hz(key) * harmonic * t);
}

inline constexpr double decibels_per_decade = 20.0;

inline double db_gain(double db) { return std::pow(10.0, db / decibels_per_decade); }

inline std::int16_t quantise(double x) {
  return static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0, 1.0) * full_scale));
}

// Writes the library into `directory` (created or replaced).
inline void write_library(const std::filesystem::path& directory, double note_seconds) {
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  std::ofstream pack(directory / "fret.pack", std::ios::binary);
  std::vector<std::string> lines;
  std::uint64_t offset = 0;
  const auto add = [&](std::string_view kind, int key, std::size_t layer, std::size_t layers,
                       std::uint32_t robin, const std::vector<std::int16_t>& pcm, double peak_db) {
    pack.write(reinterpret_cast<const char*>(pcm.data()),
               static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
    char line[256];
    std::snprintf(line, sizeof line, "sample %s %d %zu %zu %u %llu %zu %.9e %.3f",
                  std::string(kind).c_str(), key, layer, layers, robin,
                  static_cast<unsigned long long>(offset), pcm.size(), 1.0 / full_scale, peak_db);
    lines.emplace_back(line);
    offset += pcm.size();
    // Align the next sample as the installer does.
    const std::size_t pad = (alignment_frames - offset % alignment_frames) % alignment_frames;
    const std::vector<std::int16_t> zeros(pad, 0);
    pack.write(reinterpret_cast<const char*>(zeros.data()),
               static_cast<std::streamsize>(pad * sizeof(std::int16_t)));
    offset += pad;
  };
  const auto note_frames = static_cast<std::size_t>(note_seconds * rate);
  for (const auto& a : articulations()) {
    for (int key = fret::first_key; key <= fret::last_key; ++key) {
      for (std::size_t layer = 0; layer < a.layer_db.size(); ++layer) {
        for (std::uint32_t robin = 0; robin < round_robins; ++robin) {
          const double scale = db_gain(round_robin_step_db * robin);
          std::vector<std::int16_t> pcm(note_frames);
          for (std::size_t i = 0; i < note_frames; ++i) {
            // Recorded level baked in, as the installer's scale restores it.
            pcm[i] = quantise(scale * db_gain(a.layer_db[layer]) *
                              tone(key, a.harmonic, static_cast<double>(i)));
          }
          add(a.name, key, layer, a.layer_db.size(), robin, pcm, a.layer_db[layer]);
        }
      }
    }
  }
  std::uint32_t seed = 1;
  const auto noise = [&] {
    constexpr std::uint32_t lcg_a = 1'664'525U;
    constexpr std::uint32_t lcg_c = 1'013'904'223U;
    constexpr double unit = 1.0 / 8'388'608.0;
    seed = seed * lcg_a + lcg_c;
    return static_cast<double>(seed >> 8U) * unit - 1.0;
  };
  const auto release_frames = static_cast<std::size_t>(release_seconds * rate);
  for (int key = fret::first_key; key <= fret::last_key; ++key) {
    for (std::uint32_t robin = 0; robin < round_robins; ++robin) {
      std::vector<std::int16_t> pcm(release_frames);
      for (std::size_t i = 0; i < release_frames; ++i) {
        pcm[i] = quantise(db_gain(release_db) * peak * noise() *
                          (1.0 - static_cast<double>(i) / static_cast<double>(release_frames)));
      }
      add("release", key, 0, 1, robin, pcm, release_db);
    }
  }
  pack.close();
  oscillate::test::require(!pack.fail(), "the test library's pack is written");
  std::ofstream manifest(directory / fret::library::manifest_file);
  manifest << fret::library::format_line << "\nname Fret test tones\n"
           << "attribution Synthetic tones for oscillate's tests\n"
           << "rate " << fret::library::pack_rate << "\nchannels " << fret::library::pack_channels
           << "\npack fret.pack " << offset * sizeof(std::int16_t) << "\n";
  for (const auto& line : lines) {
    manifest << line << '\n';
  }
  manifest.close();
  oscillate::test::require(!manifest.fail(), "the test library's manifest is written");
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

// Writes the library and makes it the one Fret plays, for this process.
inline std::filesystem::path install(double note_seconds = streamed_note_seconds) {
  const auto directory = scratch_directory("fret-test-library");
  write_library(directory, note_seconds);
  const auto status = fret::library::load(directory);
  if (status.state != fret::library::State::ready) {
    throw std::runtime_error("the Fret test library did not load: " + status.message);
  }
  return directory;
}

} // namespace fret_test
