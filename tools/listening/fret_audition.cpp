#include "oscillate/native_dsp/synth/fret.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <vector>

// Fret on the installed library (ADR-0088), outside CTest:
//   oscillate_fret_audition render <out.wav> [preset]   the original line, drawn flat
//   oscillate_fret_audition bench                        whole-process CPU at real-time pace
// The line is Feel's eight-bar audition phrase, every note at velocity 100 with its clip length, so
// Feel decides everything. The bench plays it (light) and a dense sixteenth line with slides and
// releases (heavy) paced to real time, and reports the process's CPU (every
// thread, the streamer included) and each block's p99 and worst time
// against its budget.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp::synth;

constexpr double rate = 48'000.0;
constexpr double bpm = 104.0;
constexpr double beats_per_bar = 4.0;
constexpr std::size_t block = 128;
constexpr std::uint8_t flat_velocity = 100;
constexpr std::size_t wav_header_bytes = 44;
constexpr double seconds_per_minute = 60.0;

struct Drawn {
  double beat;
  std::uint8_t key;
  double length;
};

// Sounding keys of the line.
constexpr std::uint8_t b0 = 23, e1 = 28, g1 = 31, a1 = 33, b1 = 35, d2 = 38, e2 = 40;
constexpr std::array<Drawn, 16> phrase{{
    {0.00, e1, 0.40},
    {0.50, e1, 0.15},
    {0.75, e2, 0.20},
    {1.25, d2, 0.15},
    {1.50, b1, 0.45},
    {2.50, e1, 0.20},
    {2.75, e1, 0.15},
    {3.00, g1, 0.50},
    {3.75, a1, 0.20},
    {4.25, e1, 0.30},
    {4.75, e1, 0.15},
    {5.00, e2, 0.15},
    {5.50, d2, 0.40},
    {6.25, b1, 0.20},
    {6.50, a1, 0.15},
    {6.75, g1, 0.60},
}};
constexpr std::array<Drawn, 4> fill{
    {{7.00, g1, 0.22}, {7.25, a1, 0.22}, {7.50, b1, 0.22}, {7.75, d2, 0.22}}};
constexpr double phrase_beats = 8.0;

std::vector<Drawn> line() {
  std::vector<Drawn> notes;
  for (int p = 0; p < 4; ++p) {
    const double at = p * phrase_beats;
    for (const auto& n : phrase) {
      if (p == 3 && n.beat >= 4.0) {
        break;
      }
      if (p % 2 == 1 && n.beat >= 6.75) {
        break;
      }
      notes.push_back({at + n.beat, n.key, n.length});
    }
    if (p % 2 == 1) {
      for (const auto& n : fill) {
        notes.push_back({at + n.beat, n.key, n.length});
      }
    }
  }
  const double last = 3 * phrase_beats + beats_per_bar;
  notes.push_back({last, b0, 0.75});
  notes.push_back({last + 1.0, e1, 3.0});
  return notes;
}

// Sixteenths for `bars`, walking a minor pentatonic, every other note
// overlapping the next (slides).
std::vector<Drawn> dense(int bars) {
  constexpr std::array<std::uint8_t, 5> walk{e1, g1, a1, b1, d2};
  constexpr double sixteenth = 0.25;
  constexpr double overlap = 0.3;
  std::vector<Drawn> notes;
  for (int i = 0; i < bars * 16; ++i) {
    notes.push_back({i * sixteenth, walk[static_cast<std::size_t>(i) % walk.size()],
                     i % 2 == 0 ? overlap : 0.2});
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

std::vector<Event> events_of(const std::vector<Drawn>& notes) {
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
    const SynthTransport transport{beat, beats_per_frame, beat / beats_per_bar, beats_per_bar};
    (void)runtime->process(buffer, due, {}, position, 1, transport, {});
  }
};

std::vector<double> values_of(std::string_view preset) {
  const auto& table = *fret::engine().descriptor;
  for (const auto& entry : table.presets) {
    if (entry.name == preset) {
      return preset_values(table, entry).value();
    }
  }
  std::fprintf(stderr, "no preset %s; using the defaults\n", std::string(preset).c_str());
  return default_values(table);
}

bool ready() {
  const auto status = fret::library::status();
  if (status.state != fret::library::State::ready) {
    std::fprintf(stderr, "Fret's library is not ready: %s\nInstall it with %s\n",
                 status.message.c_str(), std::string(fret::library::install_command).c_str());
    return false;
  }
  return true;
}

int render(const char* path, std::string_view preset) {
  if (!ready()) {
    return 1;
  }
  Player player;
  player.runtime =
      std::move(NativeSynthRuntime::create(fret::engine(), values_of(preset), rate).value());
  player.events = events_of(line());
  const auto tail = fret::engine().tail_frames(values_of(preset), rate);
  const auto total = player.events.back().frame + tail;
  std::vector<std::int16_t> pcm;
  for (std::uint64_t position = 0; position < total; position += block) {
    player.runtime->await_content();
    player.block_at(position);
    for (const auto& frame : player.buffer) {
      constexpr double full_scale = 32'767.0;
      pcm.push_back(static_cast<std::int16_t>(
          std::lround(std::clamp<double>(frame.left, -1.0, 1.0) * full_scale)));
      pcm.push_back(static_cast<std::int16_t>(
          std::lround(std::clamp<double>(frame.right, -1.0, 1.0) * full_scale)));
    }
  }
  std::ofstream out(path, std::ios::binary);
  const auto data = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
  const auto rate_int = static_cast<std::uint32_t>(rate);
  const auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  const auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  constexpr std::uint16_t channels = 2;
  constexpr std::uint16_t bits = 16;
  out.write("RIFF", 4);
  put32(static_cast<std::uint32_t>(wav_header_bytes - 8) + data);
  out.write("WAVEfmt ", 8);
  put32(16);
  put16(1);
  put16(channels);
  put32(rate_int);
  put32(rate_int * channels * bits / 8);
  put16(channels * bits / 8);
  put16(bits);
  out.write("data", 4);
  put32(data);
  out.write(reinterpret_cast<const char*>(pcm.data()), data);
  std::printf("wrote %s (%s, %.1f s)\n", path, std::string(preset).c_str(),
              static_cast<double>(pcm.size() / 2) / rate);
  return 0;
}

double cpu_seconds() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto seconds = [](const timeval& t) {
    constexpr double micro = 1e-6;
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * micro;
  };
  return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

void bench(const char* label, const std::vector<Drawn>& notes, std::string_view preset) {
  constexpr double seconds = 15.0;
  constexpr double p99 = 0.99;
  Player player;
  player.runtime =
      std::move(NativeSynthRuntime::create(fret::engine(), values_of(preset), rate).value());
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
    // Real-time pace: the streamer thread fills the rings meanwhile, as
    // alongside a real callback.
    std::this_thread::sleep_until(wall_start +
                                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      budget * static_cast<double>(b + 1)));
  }
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
  const double cpu = cpu_seconds() - cpu_start;
  std::ranges::sort(block_us);
  const double budget_us = std::chrono::duration<double, std::micro>(budget).count();
  std::vector<float> telemetry;
  (void)player.runtime->read_telemetry(telemetry);
  std::printf("%-28s process CPU %5.2f %% of a core   block p99 %6.1f us  worst %6.1f us  (budget "
              "%.0f us)  "
              "underruns %.0f\n",
              label, 100.0 * cpu / wall, block_us[static_cast<std::size_t>(p99 * blocks)],
              block_us.back(), budget_us,
              telemetry.size() > fret::telemetry::underruns ? telemetry[fret::telemetry::underruns]
                                                            : -1.0F);
}

} // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "render") == 0) {
    return render(argv[2], argc >= 4 ? argv[3] : "Funk Pocket");
  }
  if (argc >= 2 && std::strcmp(argv[1], "bench") == 0) {
    if (!ready()) {
      return 1;
    }
    constexpr int dense_bars = 8;
    bench("light: the line, Funk", line(), "Funk Pocket");
    bench("heavy: dense, slides+release", dense(dense_bars), "Laid Back Soul");
    return 0;
  }
  std::fprintf(stderr, "usage: %s render <out.wav> [preset] | bench\n", argv[0]);
  return 2;
}
