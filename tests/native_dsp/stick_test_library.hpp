#pragma once

// A small synthetic kit in Stick's installed format (ADR-0114), so tests
// hear Stick without the downloads and can tell its mics and hits apart:
// - every mic plays its own frequency (mic_hz), so the output's spectrum
//   says which mics sound;
// - a hit is heard loudest on its own close mics, as bleed on the others
//   and on the overhead and room pairs; a cymbal (Crash 1) has no close mic;
// - each stroke has hits at five recorded powers, two round robins each,
//   with their own phases, so different hits render differently;
// - hits decay exponentially and outlast the resident start (unless
//   installed short), so playing them streams;
// - the hi-hat's closed, open and pedal strokes ring for different times,
//   and the snare has a brush stroke a fifth above its sticks.
// install() loads it as every kit; write_kits() puts it in every kit's
// folder, under the kit's name.

#include "oscillate/native_dsp/synth/stick.hpp"
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

namespace stick_test {

namespace stick = oscillate::native_dsp::synth::stick;

inline constexpr double rate = stick::library::pack_rate;
inline constexpr double full_scale = 32'000.0;
inline constexpr double decibels = 20.0;

// The mics, in pack order: their group, piece (empty for a pair), side and
// frequency.
struct MicSpec {
  std::string_view group;
  std::string_view piece;
  std::string_view side;
  double hz = 0.0;
};
inline constexpr std::size_t kick_in = 0;
inline constexpr std::size_t snare_top = 1;
inline constexpr std::size_t snare_bottom = 2;
inline constexpr std::size_t tom = 3;
inline constexpr std::size_t hihat = 4;
inline constexpr std::size_t overhead_left = 5;
inline constexpr std::size_t overhead_right = 6;
inline constexpr std::size_t room_left = 7;
inline constexpr std::size_t room_right = 8;
inline constexpr std::array<MicSpec, 9> mics{{
    {"kick_in", "kick", "mono", 110.0},
    {"snare_top", "snare", "mono", 330.0},
    {"snare_bottom", "snare", "mono", 550.0},
    {"toms", "tom1", "mono", 770.0},
    {"hihat", "hihat", "mono", 990.0},
    {"overheads", "", "left", 1'210.0},
    {"overheads", "", "right", 1'430.0},
    {"room", "", "left", 1'650.0},
    {"room", "", "right", 1'870.0},
}};
// How loud each mic hears a hit: its own close mics, the rest of the close
// mics (bleed), the overheads and the room.
inline constexpr double own_level = 1.0;
inline constexpr double bleed_level = 0.1;
inline constexpr double overhead_level = 0.5;
inline constexpr double room_level = 0.3;
// A brush stroke plays its mics this much higher.
inline constexpr double brush_ratio = 1.5;

// The strokes recorded: piece, articulation, striker, decay time and
// length in seconds.
struct StrokeSpec {
  std::string_view piece;
  std::string_view articulation;
  std::string_view striker;
  double decay_seconds = 0.0;
  double seconds = 0.0;
};
inline constexpr double drum_decay_seconds = 0.15;
inline constexpr double drum_seconds = 0.6;
inline constexpr double closed_decay_seconds = 0.03;
inline constexpr double closed_seconds = 0.15;
inline constexpr double open_decay_seconds = 0.4;
inline constexpr double open_seconds = 1.2;
inline constexpr double cymbal_decay_seconds = 0.5;
inline constexpr double cymbal_seconds = 1.5;
inline constexpr std::array<StrokeSpec, 10> strokes{{
    {"kick", "hit", "stick", drum_decay_seconds, drum_seconds},
    {"kick", "alt", "stick", drum_decay_seconds, drum_seconds},
    {"snare", "hit", "stick", drum_decay_seconds, drum_seconds},
    {"snare", "sidestick", "stick", closed_decay_seconds, closed_seconds},
    {"snare", "hit", "brush", drum_decay_seconds, drum_seconds},
    {"tom1", "hit", "stick", drum_decay_seconds, drum_seconds},
    {"hihat", "closed", "stick", closed_decay_seconds, closed_seconds},
    {"hihat", "open", "stick", open_decay_seconds, open_seconds},
    {"hihat", "pedal", "stick", closed_decay_seconds, closed_seconds},
    {"crash1", "hit", "stick", cymbal_decay_seconds, cymbal_seconds},
}};
// Each stroke's hits: these recorded powers (dB), two round robins each.
inline constexpr std::array<double, 5> powers_db{-30.0, -24.0, -18.0, -12.0, -6.0};
inline constexpr std::size_t round_robins = 2;
inline constexpr std::size_t hits_per_stroke = powers_db.size() * round_robins;
// Installed short, every hit fits in the resident start: live playback in
// tests never waits on the disk.
inline constexpr double resident_seconds = 0.08;

inline double hz_of(std::size_t mic, std::string_view striker) {
  return mics[mic].hz * (striker == "brush" ? brush_ratio : 1.0);
}

// How loud mic `m` hears a hit of `piece`.
inline double mic_level(std::size_t m, std::string_view piece) {
  const auto& mic = mics[m];
  if (mic.piece.empty()) {
    return mic.group == "room" ? room_level : overhead_level;
  }
  return mic.piece == piece ? own_level : bleed_level;
}

// The hit's own value on mic `m` at `frame`: its power's amplitude, a
// phase of its own and its decay.
inline double hit_value(const StrokeSpec& s, std::size_t power, std::size_t robin, std::size_t m,
                        double frame) {
  const double t = frame / rate;
  const double phase =
      std::numbers::pi * (static_cast<double>(robin) / 3.0 + static_cast<double>(power) / 7.0);
  const double amplitude = std::pow(10.0, powers_db[power] / decibels);
  return amplitude * mic_level(m, s.piece) * std::exp(-t / s.decay_seconds) *
         std::sin(2.0 * std::numbers::pi * hz_of(m, s.striker) * t + phase);
}

// Writes the kit into `directory` (created or replaced). `longest` caps
// every hit's length in seconds.
inline void write_kit(const std::filesystem::path& directory, std::string_view name,
                      double longest = 1e9) {
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  constexpr std::size_t channels = mics.size();
  std::ofstream pack(directory / "stick.pack", std::ios::binary);
  std::uint64_t frames_written = 0;
  std::vector<std::string> lines;
  for (const auto& s : strokes) {
    const auto frames = static_cast<std::size_t>(std::min(s.seconds, longest) * rate);
    for (std::size_t power = 0; power < powers_db.size(); ++power) {
      for (std::size_t robin = 0; robin < round_robins; ++robin) {
        const double peak = std::pow(10.0, powers_db[power] / decibels);
        std::vector<std::int16_t> pcm(frames * channels);
        for (std::size_t f = 0; f < frames; ++f) {
          for (std::size_t m = 0; m < channels; ++m) {
            const double x = hit_value(s, power, robin, m, static_cast<double>(f)) / peak;
            pcm[f * channels + m] =
                static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0, 1.0) * full_scale));
          }
        }
        pack.write(reinterpret_cast<const char*>(pcm.data()),
                   static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
        char line[256];
        std::snprintf(line, sizeof line, "hit %s %s %s %llu %zu %.9e %.3f",
                      std::string(s.piece).c_str(), std::string(s.articulation).c_str(),
                      std::string(s.striker).c_str(),
                      static_cast<unsigned long long>(frames_written), frames, peak / full_scale,
                      powers_db[power]);
        lines.emplace_back(line);
        frames_written += frames;
      }
    }
  }
  pack.close();
  std::ofstream manifest(directory / stick::library::manifest_file);
  manifest << stick::library::format_line << "\nname " << name
           << "\nattribution Synthetic tones for Oscillate's tests\nrate "
           << stick::library::pack_rate << "\nchannels " << channels << "\npack stick.pack "
           << frames_written * channels * sizeof(std::int16_t) << "\n";
  for (std::size_t m = 0; m < channels; ++m) {
    manifest << "mic " << m << ' ' << mics[m].group << ' '
             << (mics[m].piece.empty() ? std::string_view("-") : mics[m].piece) << ' '
             << mics[m].side << '\n';
  }
  for (const auto& line : lines) {
    manifest << line << '\n';
  }
}

// Writes every kit's folder under `base`.
inline void write_kits(const std::filesystem::path& base, double longest = 1e9) {
  for (std::size_t k = 0; k < stick::kit_count; ++k) {
    const auto& facts = stick::library::kits[k];
    write_kit(base / facts.folder, facts.name, longest);
  }
}

// Writes the kit once and makes it the one every kit plays, for this
// process. `longest` caps each hit (resident_seconds keeps every hit in
// memory).
inline std::filesystem::path install(double longest = 1e9,
                                     std::string_view name = "stick-test-kits") {
  const auto directory = wren_test::scratch_directory(name);
  write_kit(directory, "Stick test kit", longest);
  for (std::size_t k = 0; k < stick::kit_count; ++k) {
    const auto kit = static_cast<stick::Kit>(k);
    const auto status = stick::library::load(kit, directory);
    if (status.state != stick::library::State::ready) {
      throw std::runtime_error("the Stick test kit did not load: " + status.message);
    }
  }
  return directory;
}

} // namespace stick_test
