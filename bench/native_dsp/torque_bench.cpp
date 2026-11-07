#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"

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
  const auto& table = *torque::engine().descriptor;
  for (const auto& p : table.presets) {
    if (p.name == name) {
      return preset_values(table, p).value();
    }
  }
  return default_values(table);
}

void set(std::vector<double>& values, const std::string& id, double value) {
  values[*find_parameter(*torque::engine().descriptor, id)] = value;
}

void run(const char* label, const std::vector<double>& values, int notes, std::size_t block) {
  constexpr double rate = 48'000.0;
  constexpr double seconds = 10.0;
  auto runtime = std::move(NativeSynthRuntime::create(torque::engine(), values, rate).value());
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> chord;
  for (int i = 0; i < notes; ++i) {
    engine::BlockMidiEvent event;
    event.clip_id_bits = {1, 1};
    event.note_id_bits = {0, static_cast<std::uint64_t>(i + 1)};
    event.kind = engine::MidiEventKind::note_on;
    event.key = static_cast<std::uint8_t>(36 + 7 * i);
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
  // Mono: one held note (the budget case is Reese with seven saws).
  auto budget = preset("Neuro Growl");
  set(budget, "torque.reese.voices", 7);
  set(budget, "torque.fx.chorus", 3);
  set(budget, "torque.fx.delay", 0.3);
  run("Torque Reese 7 saws, all effects", budget, 1, 64);
  run("Torque Reese 7 saws, all effects", budget, 1, 256);
  run("Torque Acid Classic", preset("Acid Classic"), 1, 64);
  run("Torque Seq Acid Line (sequencer)", preset("Seq Acid Line"), 1, 64);
  run("Torque Moog Bass", preset("Moog Bass"), 1, 64);
  run("Torque Distorted 808", preset("Distorted 808"), 1, 64);
  return 0;
}
