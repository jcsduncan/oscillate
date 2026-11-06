#pragma once

// Engine-agnostic helpers for native effect tests (ADR-0040): deterministic
// test signals, block-sliced rendering through NativeEffectRuntime and
// level/spectrum measurements.

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "synth_test_support.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace effect_test {

using namespace synth_test;
namespace effects = oscillate::native_dsp::effects;

struct Stereo {
  std::vector<double> left;
  std::vector<double> right;
  [[nodiscard]] std::size_t size() const noexcept { return left.size(); }
};

inline Stereo sine(double hz, double amplitude, std::size_t frames,
                   double rate = oscillate::test::sample_rate_48k_hz) {
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  for (std::size_t i = 0; i < frames; ++i) {
    const double x =
        amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
    out.left[i] = x;
    out.right[i] = x;
  }
  return out;
}

// Seeded white noise, independent per channel unless `mono`.
inline Stereo noise(double amplitude, std::size_t frames, bool mono = false,
                    std::uint32_t seed = 12'345) {
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  std::uint32_t state = seed;
  const auto next = [&] {
    state = state * 1'664'525U + 1'013'904'223U;
    return (static_cast<double>(state >> 8U) / 8'388'608.0 - 1.0) * amplitude;
  };
  for (std::size_t i = 0; i < frames; ++i) {
    out.left[i] = next();
    out.right[i] = mono ? out.left[i] : next();
  }
  return out;
}

inline std::unique_ptr<effects::NativeEffectRuntime>
make_effect(const effects::EffectEngine& engine, const std::vector<double>& values,
            double rate = oscillate::test::sample_rate_48k_hz) {
  auto runtime = effects::NativeEffectRuntime::create(engine, values, rate);
  REQUIRE_OK(runtime, "effect runtime is created");
  return std::move(runtime.value());
}

struct RenderOptions {
  std::size_t block = 128; // 0 cycles an irregular pattern.
  const Stereo* sidechain = nullptr;
  std::uint64_t generation = 1;
  double rate = oscillate::test::sample_rate_48k_hz;
  double bpm = 120.0;
  // Called before the block that contains each frame listed, with the frame.
  std::vector<std::uint64_t> at{};
  std::function<void(std::uint64_t)> before{};
};

// Renders `input` from project frame `start`, slicing it into blocks.
inline Stereo process(effects::NativeEffectRuntime& runtime, const Stereo& input,
                      const RenderOptions& options = {}, std::uint64_t start = 0) {
  Stereo out{std::vector<double>(input.size()), std::vector<double>(input.size())};
  const std::size_t irregular[] = {37, 100, 513, 64, 1, 255};
  std::vector<engine::StereoFrame> buffer;
  std::vector<engine::StereoFrame> side;
  std::size_t position = 0;
  std::size_t cycle = 0;
  std::size_t next_hook = 0;
  while (position < input.size()) {
    std::size_t size = std::min(input.size() - position,
                                options.block == 0 ? irregular[cycle++ % 6] : options.block);
    // Every hook at this frame fires, then the block stops short of the next.
    while (next_hook < options.at.size() && options.at[next_hook] - start == position)
      options.before(options.at[next_hook++]);
    if (next_hook < options.at.size()) {
      const auto hook = options.at[next_hook];
      oscillate::test::require(hook >= start + position,
                               "render hooks ascend and none falls before the start");
      if (hook - start < position + size)
        size = hook - start - position;
    }
    buffer.resize(size);
    side.clear();
    for (std::size_t i = 0; i < size; ++i) {
      buffer[i] = {static_cast<float>(input.left[position + i]),
                   static_cast<float>(input.right[position + i])};
      if (options.sidechain != nullptr) {
        side.push_back({static_cast<float>(options.sidechain->left[position + i]),
                        static_cast<float>(options.sidechain->right[position + i])});
      }
    }
    const double beats_per_frame = options.bpm / 60.0 / options.rate;
    const double beat = static_cast<double>(start + position) * beats_per_frame;
    const SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    const auto status =
        runtime.process(buffer, side, start + position, options.generation, transport, {});
    REQUIRE_EQ(status, effects::EffectProcessStatus::rendered, "effect block renders");
    for (std::size_t i = 0; i < size; ++i) {
      out.left[position + i] = buffer[i].left;
      out.right[position + i] = buffer[i].right;
    }
    position += size;
  }
  return out;
}

using oscillate::test::rms;

inline double db(double gain) { return 20.0 * std::log10(gain + 1e-30); }

inline double centroid_hz(const std::vector<double>& x, std::size_t from,
                          double rate = oscillate::test::sample_rate_48k_hz) {
  std::vector<std::complex<double>> window(16'384);
  for (std::size_t i = 0; i < window.size(); ++i) {
    const double hann = oscillate::native_dsp::dsp::hann(static_cast<double>(i),
                                                         static_cast<double>(window.size()));
    window[i] = x[from + i] * hann;
  }
  fft(window);
  double weighted = 0.0;
  double total = 0.0;
  for (std::size_t bin = 1; bin < window.size() / 2; ++bin) {
    const double magnitude = std::abs(window[bin]);
    weighted += magnitude * static_cast<double>(bin) * rate / static_cast<double>(window.size());
    total += magnitude;
  }
  return total > 0.0 ? weighted / total : 0.0;
}

} // namespace effect_test
