#include "oscillate/native_dsp/synth/ember.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

// Run in an uninstrumented Release build; timing is not a CTest gate.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp::synth;

std::vector<double> preset(std::string_view name) {
  const auto& table = *ember::engine().descriptor;
  for (const auto& p : table.presets) {
    if (p.name == name) {
      return preset_values(table, p).value();
    }
  }
  return default_values(table);
}

void set(std::vector<double>& values, std::string_view id, double value) {
  values[*find_parameter(*ember::engine().descriptor, id)] = value;
}

void run(const char* label, const std::vector<double>& values, int notes, std::size_t block) {
  constexpr double rate = 48'000.0;
  constexpr double seconds = 10.0;
  auto runtime = std::move(NativeSynthRuntime::create(ember::engine(), values, rate).value());
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
  auto worst = preset("Age Max"); // Unison, chorus I+II, tape, Age 100%.
  set(worst, "ember.fx.ensemble", 1);
  set(worst, "ember.fx.delay", 0.5);
  set(worst, "ember.pm.osc", 0.5);
  set(worst, "ember.pm.cutoff", 1);
  set(worst, "ember.dco.noise", 0.2);
  set(worst, "ember.dco.sub", 0.5);
  auto poly = worst;
  set(poly, "ember.voice.mode", 0);
  const auto keys = preset("Ember Default");
  auto dry = keys;
  set(dry, "ember.fx.chorus", 0);
  set(dry, "ember.fx.tape", 0);
  set(dry, "ember.fx.wobble", 0);
  set(dry, "ember.fx.hiss", 0);
  auto ageless = dry;
  set(ageless, "ember.age", 0);
  for (std::size_t block : {std::size_t{64}, std::size_t{128}}) {
    run("8 voices unison, all effects, Age 100%", worst, 8, block);
    run("8 notes poly, all effects, poly-mod", poly, 8, block);
    run("8 notes Ember Default", keys, 8, block);
    run("1 note Ember Default", keys, 1, block);
    run("1 note, effects off", dry, 1, block);
    run("8 notes, effects off", dry, 8, block);
    run("8 notes, effects off, Age 0", ageless, 8, block);
  }
  return 0;
}
