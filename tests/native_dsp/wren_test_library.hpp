#pragma once

// A small synthetic library in Wren's installed format (ADR-0089), so tests
// hear Wren without the Iowa download and can tell its samples apart:
// - the plain take is the key's fundamental, the vibrato take its second
//   harmonic, so a spectrum says which take sounds;
// - every take carries a third harmonic whose level names its key (key mod
//   3), so a repitched neighbour is told from the key's own sample;
// - the attack (before attack_end) carries a fifth-harmonic chiff, so a
//   slurred onset (which starts past it) is told from a tongued one;
// - between the attack and the tail the tone is exactly periodic over the
//   loop, its pitch rounded to a whole number of cycles per loop, and that
//   rounding written as the sample's cents, as the installer measures;
// - after the loop the tone fades to silence: the recording's tail;
// - layers differ only in recorded level: as the installer writes it, the
//   scale turns each int16 back into that level and peak_db records the
//   peak.
// Some cells are left out on purpose to exercise the fallback.

#include "oscillate/native_dsp/synth/wren.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace wren_test {

namespace wren = oscillate::native_dsp::synth::wren;

inline constexpr double rate = wren::library::pack_rate;
inline constexpr double a4_hz = 440.0;
inline constexpr int a4_key = 69;
inline constexpr double semitones_per_octave = 12.0;
inline constexpr double cents_per_octave = 1'200.0;
inline constexpr double full_scale = 32'000.0;
inline constexpr double decibels = 20.0;
// The takes' harmonics.
inline constexpr std::uint32_t plain_harmonic = 1;
inline constexpr std::uint32_t vibrato_harmonic = 2;
inline constexpr std::uint32_t key_harmonic = 3;
inline constexpr std::uint32_t attack_harmonic = 5;
inline constexpr double body_level = 0.5;
// The key's mark: (1 + key mod 3) of this, on the third harmonic.
inline constexpr double key_mark_step = 0.05;
inline constexpr int key_marks = 3;
inline constexpr double attack_level = 0.2;
// Times within each take.
inline constexpr double attack_seconds = 0.05;
inline constexpr double loop_start_seconds = 0.2;
inline constexpr double loop_seconds = 0.2;
// Long enough for the tail after the loop.
inline constexpr double note_seconds = 0.6;
inline constexpr std::uint32_t attack_end = static_cast<std::uint32_t>(attack_seconds * rate);
inline constexpr std::uint32_t loop_start = static_cast<std::uint32_t>(loop_start_seconds * rate);
inline constexpr std::uint32_t loop_frames = static_cast<std::uint32_t>(loop_seconds * rate);
inline constexpr std::uint32_t loop_end = loop_start + loop_frames;
inline constexpr std::uint32_t note_frames = static_cast<std::uint32_t>(note_seconds * rate);
// Recorded level of each layer, softest first.
inline constexpr std::array<double, wren::layer_count> layer_db{-20.0, -10.0, -2.0};
inline constexpr std::size_t alignment_frames = 2'048; // 4 KiB of mono int16.

enum class Kind : std::uint8_t { vibrato, plain };
inline constexpr std::array<std::string_view, 2> kind_names{"vibrato", "plain"};

// Cells the default library leaves out: plain C#4 mf (its neighbours C4 and
// D4 are there: a tie, so the lower plays), and the vibrato take's whole pp
// layer (mf plays it).
inline constexpr int missing_plain_key = 61; // C#4.
inline constexpr std::uint32_t missing_plain_layer = 1;
inline constexpr std::uint32_t missing_vibrato_layer = 0;

using Keep = std::function<bool(Kind, int, std::uint32_t)>;

inline bool default_keep(Kind kind, int key, std::uint32_t layer) {
  if (kind == Kind::plain) {
    return !(key == missing_plain_key && layer == missing_plain_layer);
  }
  return layer != missing_vibrato_layer;
}

inline double key_hz(int key) {
  return a4_hz * std::exp2(static_cast<double>(key - a4_key) / semitones_per_octave);
}

// The take's actual pitch: a whole number of cycles per loop.
inline double played_hz(int key) {
  const double cycles = std::round(key_hz(key) * loop_seconds);
  return cycles / loop_seconds;
}

inline double cents_of(int key) {
  return cents_per_octave * std::log2(played_hz(key) / key_hz(key));
}

inline double key_mark(int key) { return key_mark_step * (1 + key % key_marks); }

inline double sample_at(Kind kind, int key, double frame) {
  const double t = frame / rate;
  const double w = 2.0 * std::numbers::pi * played_hz(key) * t;
  const double body =
      kind == Kind::plain ? std::sin(w * plain_harmonic) : std::sin(w * vibrato_harmonic);
  double x = body_level * body + key_mark(key) * std::sin(w * key_harmonic);
  if (frame < attack_end) {
    const double fade = 0.5 + 0.5 * std::cos(std::numbers::pi * frame / attack_end);
    x += attack_level * fade * std::sin(w * attack_harmonic);
  }
  if (frame >= loop_end) {
    x *= 1.0 - (frame - loop_end) / static_cast<double>(note_frames - loop_end);
  }
  return x;
}

inline std::int16_t quantise(double x) {
  return static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0, 1.0) * full_scale));
}

// Writes the library into `directory` (created or replaced), keeping only
// the cells `keep` accepts.
inline void write_library(const std::filesystem::path& directory, const Keep& keep = default_keep) {
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  std::ofstream pack(directory / "wren.pack", std::ios::binary);
  std::vector<std::string> lines;
  std::uint64_t offset = 0;
  for (const auto kind : {Kind::vibrato, Kind::plain}) {
    for (int key = wren::first_key; key <= wren::last_key; ++key) {
      for (std::uint32_t layer = 0; layer < wren::layer_count; ++layer) {
        if (!keep(kind, key, layer)) {
          continue;
        }
        const double level = std::pow(10.0, layer_db[layer] / decibels);
        std::vector<std::int16_t> pcm(note_frames);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
          pcm[i] = quantise(sample_at(kind, key, static_cast<double>(i)));
        }
        pack.write(reinterpret_cast<const char*>(pcm.data()),
                   static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
        char line[256];
        std::snprintf(line, sizeof line, "sample %s %d %u %llu %u %u %u %u %.6f %.9e %.3f",
                      std::string(kind_names[static_cast<std::size_t>(kind)]).c_str(), key, layer,
                      static_cast<unsigned long long>(offset), note_frames, attack_end, loop_start,
                      loop_end, cents_of(key), level / full_scale, layer_db[layer]);
        lines.emplace_back(line);
        offset += pcm.size();
        // Align the next sample as the installer does.
        const std::size_t pad = (alignment_frames - offset % alignment_frames) % alignment_frames;
        const std::vector<std::int16_t> zeros(pad, 0);
        pack.write(reinterpret_cast<const char*>(zeros.data()),
                   static_cast<std::streamsize>(pad * sizeof(std::int16_t)));
        offset += pad;
      }
    }
  }
  pack.close();
  std::ofstream manifest(directory / wren::library::manifest_file);
  manifest << wren::library::format_line << "\nname Wren test tones\n"
           << "attribution Synthetic tones for Oscillate's tests\n"
           << "rate " << wren::library::pack_rate << "\nchannels " << wren::library::pack_channels
           << "\npack wren.pack " << offset * sizeof(std::int16_t) << "\n";
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

// Writes the library and makes it the one Wren plays, for this process.
inline std::filesystem::path install(const Keep& keep = default_keep,
                                     std::string_view name = "wren-test-library") {
  const auto directory = scratch_directory(name);
  write_library(directory, keep);
  const auto status = wren::library::load(directory);
  if (status.state != wren::library::State::ready) {
    throw std::runtime_error("the Wren test library did not load: " + status.message);
  }
  return directory;
}

// Engine-agnostic tests play bass and drum keys (36 and up) on every engine;
// a flute sounds only from B3. For Wren the key moves up by octaves into its
// range; other engines keep it.
inline int sounding_key(std::string_view engine, int key) {
  constexpr int octave = 12;
  if (engine != "wren") {
    return key;
  }
  while (key < wren::first_key) {
    key += octave;
  }
  return key;
}

} // namespace wren_test
