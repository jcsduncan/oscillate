#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/wren.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <vector>

// Wren on the installed library (ADR-0089), outside CTest:
//   oscillate_wren_audition render <out.wav> [preset]   the audition line, drawn flat
//   oscillate_wren_audition bench                        whole-process CPU at real-time pace
// The line is Breath's eight-bar D dorian audition phrase, every note at
// velocity 100 with its clip length, at 88 BPM, so Breath decides everything. No project key
// is set: graces step a whole tone up, as dorian does from A and D. The
// bench plays it (light, Lyrical Air) and two parallel lines of fast
// overlapping sixteenths on Poly 4 with the hall (heavy), paced to real
// time, and reports the process's CPU (every thread) and each block's mean,
// p99 and worst time against its budget.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;
using namespace oscillate::native_dsp::synth;

constexpr double rate = 48'000.0;
constexpr double bpm = 88.0;
constexpr double beats_per_bar = 4.0;
constexpr std::size_t block = 128;
constexpr std::uint8_t flat_velocity = 100;
constexpr std::size_t wav_header_bytes = 44;
constexpr std::size_t riff_preamble_bytes = 8;
constexpr std::uint32_t fmt_chunk_bytes = 16;
constexpr std::uint16_t pcm_format = 1;
constexpr std::uint16_t channels = 2;
constexpr std::uint16_t bits = 16;
constexpr std::uint16_t bits_per_byte = 8;
constexpr double full_scale = 32'767.0;
constexpr double seconds_per_minute = 60.0;
constexpr double micro = 1e-6;
constexpr double percent = 100.0;

struct Drawn {
  double beat;
  std::uint8_t key;
  double length;
};

// Keys of the line.
constexpr std::uint8_t d4 = 62, f4 = 65, g4 = 67, a4 = 69, c5 = 72, d5 = 74, e5 = 76, f5 = 77,
                       g5 = 79, a5 = 81, c6 = 84;
// The audition line (its velocities ignored: drawn flat).
constexpr std::array<Drawn, 36> line{{
    {0.5, a4, 0.5},   {1.0, d5, 1.6},   {2.75, c5, 0.25},  {3.0, d5, 0.5},   {3.5, f5, 2.1},
    {5.5, e5, 0.6},   {6.0, d5, 2.0},   {8.5, a5, 0.25},   {8.75, g5, 0.25}, {9.0, f5, 0.55},
    {9.5, e5, 0.55},  {10.0, d5, 0.75}, {11.0, c5, 1.0},   {12.0, a4, 2.5},  {16.5, d5, 0.55},
    {17.0, f5, 0.55}, {17.5, a5, 1.0},  {18.75, c6, 0.25}, {19.0, a5, 1.5},  {20.75, g5, 0.25},
    {21.0, f5, 0.55}, {21.5, g5, 0.55}, {22.0, e5, 2.0},   {24.5, d5, 0.25}, {24.75, d5, 0.25},
    {25.0, f5, 0.25}, {25.5, a5, 0.5},  {26.0, g5, 0.75},  {27.0, e5, 0.5},  {27.5, c5, 0.55},
    {28.0, d5, 3.5},  {32.5, d4, 1.0},  {33.5, f4, 0.55},  {34.0, a4, 0.55}, {34.5, g4, 1.5},
    {36.0, d4, 3.5},
}};

// Two lines a third apart, sixteenths each held into the next (slurred
// runs), walking D dorian up and down.
std::vector<Drawn> runs(int bars) {
  constexpr std::array<std::uint8_t, 8> walk{d5, e5, f5, g5, a5, g5, f5, e5};
  constexpr int third = 3;
  constexpr double sixteenth = 0.25;
  constexpr double held = 0.3; // Over the next note: slurred.
  constexpr int sixteenths_per_bar = 16;
  std::vector<Drawn> notes;
  for (int i = 0; i < bars * sixteenths_per_bar; ++i) {
    const auto key = walk[static_cast<std::size_t>(i) % walk.size()];
    notes.push_back({i * sixteenth, key, held});
    notes.push_back({i * sixteenth, static_cast<std::uint8_t>(key - third), held});
  }
  return notes;
}

struct Event {
  std::uint64_t frame;
  bool on;
  std::uint8_t key;
  std::uint64_t id;
  std::uint32_t frames;
};

std::vector<Event> events_of(std::span<const Drawn> notes) {
  const double frames_per_beat = seconds_per_minute / bpm * rate;
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto& n : notes) {
    const auto start = static_cast<std::uint64_t>(std::llround(n.beat * frames_per_beat));
    const auto length = static_cast<std::uint32_t>(std::llround(n.length * frames_per_beat));
    events.push_back({start, true, n.key, id, length});
    events.push_back({start + length, false, n.key, id, 0});
    ++id;
  }
  std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
    return a.frame != b.frame ? a.frame < b.frame : !a.on && b.on;
  });
  return events;
}

struct Player {
  std::unique_ptr<NativeSynthRuntime> runtime;
  std::vector<Event> events;
  std::size_t next = 0;
  std::vector<engine::StereoFrame> buffer = std::vector<engine::StereoFrame>(block);
  std::vector<engine::BlockMidiEvent> due;

  void block_at(std::uint64_t position) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    due.clear();
    while (next < events.size() && events[next].frame < position + block) {
      const auto& e = events[next++];
      engine::BlockMidiEvent event;
      event.offset = static_cast<std::uint32_t>(e.frame - position);
      event.clip_id_bits = {1, 1};
      event.note_id_bits = {0, e.id};
      event.kind = e.on ? engine::MidiEventKind::note_on : engine::MidiEventKind::note_off;
      event.key = e.key;
      event.velocity = e.on ? flat_velocity : 0;
      event.frames = e.frames;
      due.push_back(event);
    }
    const double beats_per_frame = bpm / seconds_per_minute / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const Transport transport{beat, beats_per_frame, beat / beats_per_bar, beats_per_bar};
    (void)runtime->process(buffer, due, {}, position, 1, transport, {});
  }
};

std::vector<double> values_of(std::string_view preset) {
  const auto& table = *wren::engine().descriptor;
  for (const auto& entry : table.presets) {
    if (entry.name == preset) {
      return preset_values(table, entry).value();
    }
  }
  std::fprintf(stderr, "no preset %s; using the defaults\n", std::string(preset).c_str());
  return default_values(table);
}

bool ready() {
  const auto status = wren::library::status();
  if (status.state != wren::library::State::ready) {
    std::fprintf(stderr, "Wren's library is not ready: %s\nInstall it with %s\n",
                 status.message.c_str(), std::string(wren::library::install_command).c_str());
    return false;
  }
  return true;
}

void write_wav(const char* path, const std::vector<std::int16_t>& pcm) {
  std::ofstream out(path, std::ios::binary);
  const auto data = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
  const auto rate_int = static_cast<std::uint32_t>(rate);
  const auto put32 = [&](std::uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof v);
  };
  const auto put16 = [&](std::uint16_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof v);
  };
  constexpr std::uint16_t frame_bytes = channels * bits / bits_per_byte;
  out.write("RIFF", 4);
  put32(static_cast<std::uint32_t>(wav_header_bytes - riff_preamble_bytes) + data);
  out.write("WAVEfmt ", 8);
  put32(fmt_chunk_bytes);
  put16(pcm_format);
  put16(channels);
  put32(rate_int);
  put32(rate_int * frame_bytes);
  put16(frame_bytes);
  put16(bits);
  out.write("data", 4);
  put32(data);
  out.write(reinterpret_cast<const char*>(pcm.data()), data);
}

int render(const char* path, std::string_view preset) {
  if (!ready()) {
    return 1;
  }
  Player player;
  player.runtime =
      std::move(NativeSynthRuntime::create(wren::engine(), values_of(preset), rate).value());
  player.events = events_of(line);
  const auto tail = wren::engine().tail_frames(values_of(preset), rate);
  const auto total = player.events.back().frame + tail;
  std::vector<std::int16_t> pcm;
  for (std::uint64_t position = 0; position < total; position += block) {
    player.block_at(position);
    for (const auto& frame : player.buffer) {
      pcm.push_back(static_cast<std::int16_t>(
          std::lround(std::clamp<double>(frame.left, -1.0, 1.0) * full_scale)));
      pcm.push_back(static_cast<std::int16_t>(
          std::lround(std::clamp<double>(frame.right, -1.0, 1.0) * full_scale)));
    }
  }
  write_wav(path, pcm);
  std::printf("wrote %s (%s, %.1f s)\n", path, std::string(preset).c_str(),
              static_cast<double>(pcm.size() / channels) / rate);
  return 0;
}

double cpu_seconds() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto seconds = [](const timeval& t) {
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * micro;
  };
  return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

void bench(const char* label, std::span<const Drawn> notes, std::string_view preset) {
  constexpr double seconds = 15.0;
  constexpr double p99 = 0.99;
  Player player;
  player.runtime =
      std::move(NativeSynthRuntime::create(wren::engine(), values_of(preset), rate).value());
  player.events = events_of(notes);
  const auto blocks = static_cast<std::size_t>(seconds * rate / static_cast<double>(block));
  const auto budget = std::chrono::duration<double>(static_cast<double>(block) / rate);
  std::vector<double> block_us;
  block_us.reserve(blocks);
  const double cpu_start = cpu_seconds();
  const auto wall_start = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < blocks; ++b) {
    const auto start = std::chrono::steady_clock::now();
    player.block_at(b * block);
    const auto stop = std::chrono::steady_clock::now();
    block_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    std::this_thread::sleep_until(wall_start +
                                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      budget * static_cast<double>(b + 1)));
  }
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
  const double cpu = cpu_seconds() - cpu_start;
  double mean = 0.0;
  for (const double us : block_us) {
    mean += us;
  }
  mean /= static_cast<double>(block_us.size());
  std::ranges::sort(block_us);
  const double budget_us = std::chrono::duration<double, std::micro>(budget).count();
  std::printf(
      "%-34s process CPU %5.2f %% of a core   block mean %6.1f us  p99 %6.1f us  worst %6.1f us  "
      "(budget %.0f us)\n",
      label, percent * cpu / wall, mean,
      block_us[static_cast<std::size_t>(p99 * static_cast<double>(blocks))], block_us.back(),
      budget_us);
}

} // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "render") == 0) {
    return render(argv[2], argc >= 4 ? argv[3] : "Lyrical Air");
  }
  if (argc >= 2 && std::strcmp(argv[1], "bench") == 0) {
    if (!ready()) {
      return 1;
    }
    constexpr int run_bars = 8;
    bench("light: the line, Lyrical Air", line, "Lyrical Air");
    const auto heavy = runs(run_bars);
    bench("heavy: Poly 4 slurred runs, hall", heavy, "Two Flutes");
    return 0;
  }
  std::fprintf(stderr, "usage: %s render <out.wav> [preset] | bench\n", argv[0]);
  return 2;
}
