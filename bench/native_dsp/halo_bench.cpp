#include "oscillate/native_dsp/synth/halo.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// Run in an uninstrumented Release build; timing is not a CTest gate.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp::synth;

std::vector<double> preset(std::string_view name) {
  const auto& table = *halo::engine().descriptor;
  for (const auto& p : table.presets) {
    if (p.name == name) {
      return preset_values(table, p).value();
    }
  }
  return default_values(table);
}

void set(std::vector<double>& values, const std::string& id, double value) {
  values[*find_parameter(*halo::engine().descriptor, id)] = value;
}

void run(const char* label, const std::vector<double>& values, int notes, std::size_t block) {
  constexpr double rate = 48'000.0;
  constexpr double seconds = 10.0;
  auto runtime = std::move(NativeSynthRuntime::create(halo::engine(), values, rate).value());
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> chord;
  for (int i = 0; i < notes; ++i) {
    engine::BlockMidiEvent event;
    event.clip_id_bits = {1, 1};
    event.note_id_bits = {0, static_cast<std::uint64_t>(i + 1)};
    event.kind = engine::MidiEventKind::note_on;
    event.key = static_cast<std::uint8_t>(36 + 3 * i);
    event.velocity = 100;
    chord.push_back(event);
  }
  const auto blocks = static_cast<std::size_t>(seconds * rate / static_cast<double>(block));
  std::vector<double> block_ns;
  block_ns.reserve(blocks);
  for (std::size_t b = 0; b < blocks; ++b) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    const auto start = std::chrono::steady_clock::now();
    (void)runtime->process(buffer, b == 0 ? std::span(chord) : std::span(chord).subspan(0, 0),
                           b * block, 1, {}, {});
    const auto stop = std::chrono::steady_clock::now();
    block_ns.push_back(std::chrono::duration<double, std::nano>(stop - start).count());
  }
  // Skip the first second (attack, cache warm-up) for the steady figure.
  const std::size_t skip = blocks / 10;
  double total = 0.0;
  for (std::size_t b = skip; b < blocks; ++b) {
    total += block_ns[b];
  }
  std::vector<double> sorted(block_ns.begin() + static_cast<std::ptrdiff_t>(skip), block_ns.end());
  std::sort(sorted.begin(), sorted.end());
  const double budget_ns = static_cast<double>(block) / rate * 1e9;
  const double mean = total / static_cast<double>(blocks - skip);
  const double p99 = sorted[sorted.size() * 99 / 100];
  std::printf("%-34s block %3zu: mean %6.2f%% core, p99 block %6.2f%%, max %6.2f%%\n", label, block,
              100.0 * mean / budget_ns, 100.0 * p99 / budget_ns, 100.0 * sorted.back() / budget_ns);
}

} // namespace

int main() {
  // Budget case: 16 notes x 2 unison, all six operators sounding.
  auto budget = preset("Strings");
  set(budget, "halo.voice.unison", 2);
  run("Halo 16 notes x 2 unison", budget, 16, 64);
  run("Halo 16 notes x 2 unison", budget, 16, 256);
  auto dry = budget;
  set(dry, "halo.fx.chorus", 0);
  set(dry, "halo.fx.ensemble", 0);
  run("Halo 16x2, effects off", dry, 16, 64);
  auto single = dry;
  set(single, "halo.voice.unison", 1);
  run("Halo 16x1, effects off", single, 16, 64);
  auto filtered = budget;
  set(filtered, "halo.filter.type", 5);
  run("Halo 16x2 + ladder filter", filtered, 16, 64);
  auto wide = budget;
  set(wide, "halo.voice.unison", 4);
  run("Halo 16 notes x 4 unison", wide, 16, 64);
  run("Halo E-Piano 16 notes", preset("E-Piano"), 16, 64);
  return 0;
}
