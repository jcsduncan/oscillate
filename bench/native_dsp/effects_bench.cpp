#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

// Realtime share of one core for every native effect preset: ten seconds of
// programme-like stereo noise at 48 kHz in 128-frame blocks, best of three.
int main() {
  using namespace effect_test;
  const auto input = noise(0.3, 10 * 48'000);
  std::vector<engine::StereoFrame> buffer(128);
  for (const auto* engine : effects::engines()) {
    for (const auto& entry : engine->descriptor->presets) {
      double best = 1e9;
      for (int run = 0; run < 3; ++run) {
        auto runtime = make_effect(*engine, preset(*engine->descriptor, entry.name));
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t position = 0; position + 128 <= input.size(); position += 128) {
          for (std::size_t i = 0; i < 128; ++i) {
            buffer[i] = {static_cast<float>(input.left[position + i]),
                         static_cast<float>(input.right[position + i])};
          }
          const SynthTransport transport{static_cast<double>(position) / 24'000.0, 1.0 / 24'000.0,
                                         static_cast<double>(position) / 96'000.0, 4.0};
          (void)runtime->process(buffer, std::span(buffer).first(0), position, 1, transport, {});
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        best = std::min(best, elapsed.count());
      }
      std::printf("%-8s %-24s %5.1f%%\n", std::string(engine->descriptor->id).c_str(),
                  std::string(entry.name).c_str(), best / 10.0 * 100.0);
    }
  }
}
