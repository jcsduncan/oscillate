#pragma once

// A small synthetic library in Bow's installed format (ADR-0111), so tests
// hear Bow without the downloads and can tell its samples apart:
// - every take is its key's fundamental, exactly periodic over its loop
//   (its pitch rounded to a whole number of cycles per loop, and that
//   rounding written as the sample's cents, as the installer measures);
// - its attack (before attack_end) carries a fifth-harmonic chiff, so a
//   legato onset (which starts past it) is told from a fresh one;
// - layers differ in recorded level (the scale turns each int16 back into
//   it, and level_db is the steady level) and in phase, a quarter turn
//   apart, so neighbouring layers are uncorrelated as different recordings
//   are and the wheel's crossfade is heard as level;
// - sections are stereo, the solo takes mono, as installed;
// - shorts (staccato, pizzicato) are one-shots that decay to silence;
//   sustains and tremolos loop.
// Keys are sampled every few semitones, so most notes play a neighbour
// repitched. Solo has only sustains and pizzicati (its staccato and tremolo
// fall back to the section's), as the real library has for violas.

#include "oscillate/native_dsp/synth/bow.hpp"
#include "wren_test_library.hpp"

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
#include <vector>

namespace bow_test {

namespace bow = oscillate::native_dsp::synth::bow;

inline constexpr double rate = bow::library::pack_rate;
inline constexpr double a4_hz = 440.0;
inline constexpr int a4_key = 69;
inline constexpr double semitones_per_octave = 12.0;
inline constexpr double cents_per_octave = 1'200.0;
inline constexpr double full_scale = 32'000.0;
inline constexpr double decibels = 20.0;
inline constexpr std::uint32_t attack_harmonic = 5;
inline constexpr double body_level = 0.5;
inline constexpr double attack_level = 0.3;
inline constexpr double quarter_turn = std::numbers::pi / 2.0;
// Times within each take.
inline constexpr double attack_seconds = 0.05;
inline constexpr double loop_start_seconds = 0.12;
inline constexpr double loop_seconds = 0.2;
inline constexpr double one_shot_seconds = 0.25;
inline constexpr std::uint32_t attack_end = static_cast<std::uint32_t>(attack_seconds * rate);
inline constexpr std::uint32_t loop_start = static_cast<std::uint32_t>(loop_start_seconds * rate);
inline constexpr std::uint32_t loop_frames = static_cast<std::uint32_t>(loop_seconds * rate);
inline constexpr std::uint32_t loop_end = loop_start + loop_frames;
inline constexpr std::uint32_t one_shot_frames =
    static_cast<std::uint32_t>(one_shot_seconds * rate);
// Sampled every this many semitones from each instrument's lowest key.
inline constexpr int key_step = 5;
// Recorded levels, softest first: a section's two layers, a solo's three.
inline constexpr std::array<double, 2> section_layer_db{-18.0, -6.0};
inline constexpr std::array<double, bow::max_layers> solo_layer_db{-24.0, -12.0, -2.0};
inline constexpr std::uint32_t short_round_robins = 2;
inline constexpr std::size_t alignment_bytes = 4'096;
// A steady sine's RMS against its peak.
inline const double sine_rms_db = decibels * std::log10(std::numbers::sqrt2 / 2.0);

inline constexpr std::array<std::string_view, 2> mode_names{"solo", "section"};
inline constexpr std::array<std::string_view, bow::instrument_count> instrument_names{
    "violin", "viola", "cello", "bass"};
inline constexpr std::array<std::string_view, 4> recorded_names{"sustain", "staccato", "pizzicato",
                                                                "tremolo"};
enum class Recorded : std::uint8_t { sustain, staccato, pizzicato, tremolo };

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

inline bool one_shot(Recorded recorded) {
  return recorded == Recorded::staccato || recorded == Recorded::pizzicato;
}

inline double sample_at(Recorded recorded, int key, std::uint32_t layer, double frame) {
  const double t = frame / rate;
  const double w = 2.0 * std::numbers::pi * played_hz(key) * t;
  double x = body_level * std::sin(w + quarter_turn * layer);
  if (frame < attack_end) {
    const double fade = 0.5 + 0.5 * std::cos(std::numbers::pi * frame / attack_end);
    x += attack_level * fade * std::sin(w * attack_harmonic);
  }
  if (one_shot(recorded)) {
    x *= std::max(0.0, 1.0 - frame / static_cast<double>(one_shot_frames));
  }
  return x;
}

inline std::int16_t quantise(double x) {
  return static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0, 1.0) * full_scale));
}

struct PackWriter {
  std::ofstream file;
  std::uint32_t channels = 1;
  std::uint64_t bytes = 0;

  // Writes a take and returns its first frame.
  std::uint64_t write(const std::vector<std::int16_t>& mono) {
    const std::uint64_t first = bytes / (channels * sizeof(std::int16_t));
    std::vector<std::int16_t> frames;
    frames.reserve(mono.size() * channels);
    for (const auto value : mono) {
      for (std::uint32_t c = 0; c < channels; ++c) {
        frames.push_back(value);
      }
    }
    file.write(reinterpret_cast<const char*>(frames.data()),
               static_cast<std::streamsize>(frames.size() * sizeof(std::int16_t)));
    bytes += frames.size() * sizeof(std::int16_t);
    const std::size_t pad = (alignment_bytes - bytes % alignment_bytes) % alignment_bytes;
    const std::vector<char> zeros(pad, 0);
    file.write(zeros.data(), static_cast<std::streamsize>(pad));
    bytes += pad;
    return first;
  }
};

// Writes the library into `directory` (created or replaced).
inline void write_library(const std::filesystem::path& directory) {
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  constexpr std::uint32_t mono_channels = 1;
  constexpr std::uint32_t stereo_channels = 2;
  PackWriter mono{std::ofstream(directory / "bow-mono.pack", std::ios::binary), mono_channels};
  PackWriter stereo{std::ofstream(directory / "bow-stereo.pack", std::ios::binary),
                    stereo_channels};
  std::vector<std::string> lines;
  for (std::size_t m = 0; m < mode_names.size(); ++m) {
    const bool solo = m == static_cast<std::size_t>(bow::Mode::solo);
    for (std::size_t i = 0; i < bow::instrument_count; ++i) {
      const auto range = bow::instrument_ranges[i];
      for (std::size_t r = 0; r < recorded_names.size(); ++r) {
        const auto recorded = static_cast<Recorded>(r);
        if (solo && (recorded == Recorded::staccato || recorded == Recorded::tremolo)) {
          continue;
        }
        const auto layers = solo ? solo_layer_db.size() : section_layer_db.size();
        const auto robins = one_shot(recorded) ? short_round_robins : 1U;
        for (int key = range.low; key <= range.high; key += key_step) {
          for (std::uint32_t layer = 0; layer < layers; ++layer) {
            const double layer_db = solo ? solo_layer_db[layer] : section_layer_db[layer];
            for (std::uint32_t robin = 0; robin < robins; ++robin) {
              const std::uint32_t frames = one_shot(recorded) ? one_shot_frames : loop_end;
              std::vector<std::int16_t> pcm(frames);
              for (std::size_t f = 0; f < pcm.size(); ++f) {
                pcm[f] = quantise(sample_at(recorded, key, layer, static_cast<double>(f)));
              }
              auto& pack = solo ? mono : stereo;
              const auto offset = pack.write(pcm);
              const double level = std::pow(10.0, layer_db / decibels);
              const double steady_db = layer_db + decibels * std::log10(body_level) + sine_rms_db;
              char line[320];
              std::snprintf(
                  line, sizeof line, "sample %u %s %s %s %d %u %u %llu %u %u %u %u %.6f %.9e %.3f",
                  pack.channels, std::string(mode_names[m]).c_str(),
                  std::string(instrument_names[i]).c_str(), std::string(recorded_names[r]).c_str(),
                  key, layer, robin, static_cast<unsigned long long>(offset), frames,
                  one_shot(recorded) ? 0U : attack_end, one_shot(recorded) ? 0U : loop_start,
                  one_shot(recorded) ? 0U : loop_end, cents_of(key), level / full_scale, steady_db);
              lines.emplace_back(line);
            }
          }
        }
      }
    }
  }
  mono.file.close();
  stereo.file.close();
  std::ofstream manifest(directory / bow::library::manifest_file);
  manifest << bow::library::format_line << "\nname Bow test tones\n"
           << "attribution Synthetic tones for Oscillate's tests\n"
           << "rate " << bow::library::pack_rate << "\nchannels " << stereo_channels
           << "\npack bow-stereo.pack " << stereo.bytes << "\nmono_pack bow-mono.pack "
           << mono.bytes << "\n";
  for (const auto& line : lines) {
    manifest << line << '\n';
  }
}

// Writes the library and makes it the one Bow plays, for this process.
inline std::filesystem::path install(std::string_view name = "bow-test-library") {
  const auto directory = wren_test::scratch_directory(name);
  write_library(directory);
  const auto status = bow::library::load(directory);
  if (status.state != bow::library::State::ready) {
    throw std::runtime_error("the Bow test library did not load: " + status.message);
  }
  return directory;
}

// Engine-agnostic tests play bass and drum keys (36 and up) on every engine.
// Bow's sections start as high as the violins' G3, so its keys move up by
// octaves until every section plays them; Wren's move into the flute's
// range; other engines keep theirs.
inline int sounding_key(std::string_view engine, int key) {
  constexpr int octave = 12;
  if (engine != "bow") {
    return wren_test::sounding_key(engine, key);
  }
  int lowest = bow::first_key;
  for (const auto& range : bow::instrument_ranges) {
    lowest = std::max(lowest, range.low);
  }
  while (key < lowest) {
    key += octave;
  }
  return key;
}

} // namespace bow_test
