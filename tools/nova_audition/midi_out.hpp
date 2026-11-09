#pragma once

// A part as a Standard MIDI File (format 0, one track): its notes and
// controller changes, with the tempo and a 4/4 meter, for dropping into a
// project.
#include "audition.hpp"
#include "parts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace oscillate::tools::midi_out {

inline constexpr std::uint8_t mod_wheel_cc = 1;
inline constexpr std::uint8_t sustain_cc = 64;
inline constexpr std::uint8_t soft_cc = 67;

// A controller change at a beat, its value 0..1.
struct Control {
  double beat;
  std::uint8_t controller;
  double value;
};

namespace detail {

inline constexpr std::uint16_t division = 480;
inline constexpr std::uint32_t micros_per_minute = 60'000'000;
inline constexpr std::uint8_t controller_max = 127;

struct Event {
  std::uint32_t tick;
  int order; // At one tick: controllers, note-offs, then note-ons.
  std::vector<std::uint8_t> bytes;
};

inline void put_be(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i)
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
}

inline void put_vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
  std::array<std::uint8_t, 5> buffer{};
  std::size_t count = 0;
  buffer[count++] = static_cast<std::uint8_t>(value & 0x7F);
  while ((value >>= 7) != 0)
    buffer[count++] = static_cast<std::uint8_t>((value & 0x7F) | 0x80);
  while (count > 0)
    out.push_back(buffer[--count]);
}

inline std::uint32_t tick(double beat) {
  return static_cast<std::uint32_t>(std::lround(beat * division));
}

} // namespace detail

inline void write(const std::filesystem::path& path, std::string_view name, double tempo_bpm,
                  const std::vector<parts::Note>& notes, const std::vector<Control>& controls) {
  using namespace detail;
  constexpr std::uint8_t note_on = 0x90;
  constexpr std::uint8_t note_off = 0x80;
  constexpr std::uint8_t control = 0xB0;
  std::vector<Event> events;
  for (const auto& note : notes) {
    events.push_back({tick(note.beat), 2, {note_on, note.key, note.velocity}});
    events.push_back({tick(note.beat + note.length), 1, {note_off, note.key, 0}});
  }
  for (const auto& change : controls)
    events.push_back({tick(change.beat),
                      0,
                      {control, change.controller,
                       static_cast<std::uint8_t>(
                           std::lround(std::clamp(change.value, 0.0, 1.0) * controller_max))}});
  std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
  });

  std::vector<std::uint8_t> track;
  const auto meta = [&](std::uint8_t type, const std::vector<std::uint8_t>& data) {
    put_vlq(track, 0);
    track.push_back(0xFF);
    track.push_back(type);
    put_vlq(track, static_cast<std::uint32_t>(data.size()));
    track.insert(track.end(), data.begin(), data.end());
  };
  meta(0x03, {name.begin(), name.end()});
  std::vector<std::uint8_t> tempo;
  put_be(tempo, static_cast<std::uint32_t>(micros_per_minute / tempo_bpm), 3);
  meta(0x51, tempo);
  meta(0x58, {parts::beats_per_bar, 2, 24, 8});
  std::uint32_t now = 0;
  for (const auto& event : events) {
    put_vlq(track, event.tick - now);
    now = event.tick;
    track.insert(track.end(), event.bytes.begin(), event.bytes.end());
  }
  put_vlq(track, 0);
  track.insert(track.end(), {0xFF, 0x2F, 0x00});

  std::vector<std::uint8_t> file{'M', 'T', 'h', 'd'};
  put_be(file, 6, 4);
  put_be(file, 0, 2); // Format 0: one track.
  put_be(file, 1, 2);
  put_be(file, division, 2);
  file.insert(file.end(), {'M', 'T', 'r', 'k'});
  put_be(file, static_cast<std::uint32_t>(track.size()), 4);
  file.insert(file.end(), track.begin(), track.end());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  if (!out)
    audition::fail("cannot write " + path.string());
  std::cout << "  " << path.string() << '\n';
}

} // namespace oscillate::tools::midi_out
