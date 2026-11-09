#pragma once

// Engine-agnostic helpers for native synth engine tests: absolute MIDI events
// sliced into blocks of any size, a zero-crossing pitch oracle and golden
// preset fingerprints.

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth_test {

using namespace oscillate;
using namespace oscillate::native_dsp;
using namespace oscillate::native_dsp::synth;

using oscillate::test::require;

inline std::size_t index(const EngineDescriptor& table, std::string_view id) {
  const auto found = find_parameter(table, id);
  REQUIRE_OK(found, "parameter exists: " + std::string(id));
  return *found;
}

inline std::vector<double> preset(const EngineDescriptor& table, std::string_view name) {
  for (const auto& entry : table.presets) {
    if (entry.name == name) {
      auto values = preset_values(table, entry);
      REQUIRE_OK(values, "preset resolves");
      return values.value();
    }
  }
  throw std::runtime_error("missing preset " + std::string(name));
}

struct Event {
  std::uint64_t frame;
  engine::MidiEventKind kind;
  std::uint8_t key;
  std::uint8_t velocity;
  std::uint64_t note;
  std::uint32_t frames = 0; // A note-on's known length (BlockMidiEvent::frames).
};

inline std::vector<Event> note(std::uint64_t start, std::uint64_t length, std::uint8_t key,
                               std::uint64_t id, std::uint8_t velocity = 100) {
  return {{start, engine::MidiEventKind::note_on, key, velocity, id},
          {start + length, engine::MidiEventKind::note_off, key, 0, id}};
}

struct PedalEvent {
  std::uint64_t frame;
  project::MidiPedal pedal;
  double value;
};

inline std::unique_ptr<NativeSynthRuntime>
make_runtime(const SynthEngine& engine, const std::vector<double>& values,
             double rate = oscillate::test::sample_rate_48k_hz) {
  auto runtime = NativeSynthRuntime::create(engine, values, rate);
  REQUIRE_OK(runtime, "runtime is created");
  return std::move(runtime.value());
}

struct Rendered {
  std::vector<double> left;
  std::vector<double> right;
  KernelStatus last{};
};

// Renders from frame zero with a fixed block size (0 cycles an irregular
// pattern), slicing the absolute event list into blocks. A generation other
// than the runtime's last is a seek: the kernel resets first.
inline Rendered render(NativeSynthRuntime& runtime, std::vector<Event> events, std::size_t frames,
                       std::size_t block = 128, double rate = oscillate::test::sample_rate_48k_hz,
                       double bpm = 120.0, std::vector<PedalEvent> pedals = {},
                       std::uint64_t generation = 1) {
  std::ranges::stable_sort(
      pedals, [](const PedalEvent& a, const PedalEvent& b) { return a.frame < b.frame; });
  std::size_t next_pedal = 0;
  std::vector<engine::BlockPedalEvent> block_pedals;
  std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
    if (a.frame != b.frame) {
      return a.frame < b.frame;
    }
    if (a.kind != b.kind) {
      return a.kind < b.kind;
    }
    return a.note < b.note;
  });
  Rendered out;
  out.left.resize(frames);
  out.right.resize(frames);
  const std::size_t irregular[] = {37, 100, 513, 64, 1, 255};
  std::size_t next = 0;
  std::size_t position = 0;
  std::size_t cycle = 0;
  std::vector<engine::StereoFrame> buffer;
  std::vector<engine::BlockMidiEvent> block_events;
  while (position < frames) {
    const std::size_t size =
        std::min(frames - position, block == 0 ? irregular[cycle++ % 6] : block);
    buffer.assign(size, {0.0F, 0.0F});
    block_events.clear();
    while (next < events.size() && events[next].frame < position + size) {
      const auto& e = events[next++];
      engine::BlockMidiEvent event;
      event.offset = static_cast<std::uint32_t>(e.frame - position);
      event.clip_id_bits = {1, 1};
      event.note_id_bits = {0, e.note};
      event.kind = e.kind;
      event.key = e.key;
      event.velocity = e.velocity;
      event.frames = e.frames;
      block_events.push_back(event);
    }
    const double beats_per_frame = bpm / 60.0 / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const Transport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    block_pedals.clear();
    while (next_pedal < pedals.size() && pedals[next_pedal].frame < position + size) {
      const auto& p = pedals[next_pedal++];
      block_pedals.push_back(
          {static_cast<std::uint32_t>(p.frame - position), p.pedal, static_cast<float>(p.value)});
    }
    // An offline render: streamed content is ready before each block.
    runtime.await_content();
    const auto report =
        runtime.process(buffer, block_events, block_pedals, position, generation, transport, {});
    REQUIRE_EQ(report.status, SynthProcessStatus::rendered, "block renders");
    out.last = report.kernel;
    for (std::size_t i = 0; i < size; ++i) {
      out.left[position + i] = buffer[i].left;
      out.right[position + i] = buffer[i].right;
    }
    position += size;
  }
  return out;
}

inline double measured_hz(const std::vector<double>& x, std::size_t begin, std::size_t end,
                          double rate) {
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = begin + 1; i < end; ++i) {
    if (x[i - 1] < 0.0 && x[i] >= 0.0) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0.0) {
        first = t;
      } else {
        ++crossings;
      }
      last = t;
    }
  }
  return rate * crossings / (last - first);
}

inline double cents(double measured, double expected) {
  return 1200.0 * std::log2(measured / expected);
}

inline double peak(const std::vector<double>& x, std::size_t from = 0,
                   std::size_t to = static_cast<std::size_t>(-1)) {
  double result = 0.0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) {
    result = std::max(result, std::fabs(x[i]));
  }
  return result;
}

inline void fft(std::vector<std::complex<double>>& data) {
  const std::size_t n = data.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1U;
    for (; (j & bit) != 0; bit >>= 1U) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(data[i], data[j]);
    }
  }
  for (std::size_t length = 2; length <= n; length <<= 1U) {
    const double angle = -2.0 * 3.14159265358979323846 / static_cast<double>(length);
    const std::complex<double> root(std::cos(angle), std::sin(angle));
    for (std::size_t start = 0; start < n; start += length) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < length / 2; ++k) {
        const auto even = data[start + k];
        const auto odd = data[start + k + length / 2] * w;
        data[start + k] = even + odd;
        data[start + k + length / 2] = even - odd;
        w *= root;
      }
    }
  }
}

struct Fingerprint {
  double hold_db;
  double tail_db;
  double centroid_hz;
};

// A one-second C major chord, then one second of release, at 48 kHz.
inline Fingerprint fingerprint(const SynthEngine& engine, std::string_view name) {
  auto runtime = make_runtime(engine, preset(*engine.descriptor, name));
  std::vector<Event> chord;
  std::uint64_t id = 1;
  for (std::uint8_t key :
       {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}}) {
    auto n = note(0, 48'000, key, id++);
    chord.insert(chord.end(), n.begin(), n.end());
  }
  const auto out = render(*runtime, chord, 96'000);
  const auto rms_db = [&](std::size_t from, std::size_t to) {
    double power = 0.0;
    for (std::size_t i = from; i < to; ++i) {
      power += out.left[i] * out.left[i] + out.right[i] * out.right[i];
    }
    return 10.0 * std::log10(power / (2.0 * static_cast<double>(to - from)) + 1e-30);
  };
  std::vector<std::complex<double>> window(16'384);
  for (std::size_t i = 0; i < window.size(); ++i) {
    const double hann = oscillate::native_dsp::dsp::hann(static_cast<double>(i),
                                                         static_cast<double>(window.size()));
    window[i] = (out.left[4'096 + i] + out.right[4'096 + i]) * hann;
  }
  fft(window);
  double weighted = 0.0;
  double total = 0.0;
  for (std::size_t bin = 1; bin < window.size() / 2; ++bin) {
    const double magnitude = std::abs(window[bin]);
    weighted +=
        magnitude * static_cast<double>(bin) * 48'000.0 / static_cast<double>(window.size());
    total += magnitude;
  }
  return {rms_db(12'000, 48'000), rms_db(60'000, 72'000), total > 0.0 ? weighted / total : 0.0};
}

} // namespace synth_test
