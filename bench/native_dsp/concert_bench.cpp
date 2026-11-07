#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

// Run in an uninstrumented Release build; timing is not a CTest gate.
// Concert's cost grows with the voices that sound (two layers per note,
// each resampled through a 32-tap sinc unless it plays at its own pitch)
// and with the resonance loops the dampers free: a dry chord, then the
// sustain pedal down (every loop running). Plays the installed library;
// the disk is serviced between blocks, outside the timing, as the
// streamer thread does alongside a real callback.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp::synth;

void run(const char* label, int notes, bool pedal, std::size_t block) {
  constexpr double rate = 48'000.0;
  constexpr double seconds = 10.0;
  const auto& table = *concert::engine().descriptor;
  const auto values = preset_values(table, table.presets.front()).value();
  auto runtime = std::move(NativeSynthRuntime::create(concert::engine(), values, rate).value());
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
  const std::vector<engine::BlockPedalEvent> down{{0, project::MidiPedal::sustain, 1.0F}};
  const auto blocks = static_cast<std::size_t>(seconds * rate / static_cast<double>(block));
  std::vector<double> block_ns;
  block_ns.reserve(blocks);
  for (std::size_t b = 0; b < blocks; ++b) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    runtime->await_content();
    const auto start = std::chrono::steady_clock::now();
    (void)runtime->process(buffer, b == 0 ? std::span(chord) : std::span(chord).subspan(0, 0),
                           b == 0 && pedal ? std::span(down) : std::span(down).subspan(0, 0),
                           b * block, 1, {}, {});
    const auto stop = std::chrono::steady_clock::now();
    block_ns.push_back(std::chrono::duration<double, std::nano>(stop - start).count());
  }
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
  std::printf("%-40s block %3zu: mean %6.2f%% core, p99 block %6.2f%%, max %6.2f%%\n", label, block,
              100.0 * mean / budget_ns, 100.0 * p99 / budget_ns, 100.0 * sorted.back() / budget_ns);
}

} // namespace

int main() {
  const auto library = concert::library::status();
  if (library.state != concert::library::State::ready) {
    std::fprintf(stderr, "Concert has no library: %s\n", library.message.c_str());
    return 1;
  }
  run("Concert 10-note chord, dampers down", 10, false, 128);
  run("Concert 10-note chord, sustain pedal", 10, true, 128);
  run("Concert 20-note chord, sustain pedal", 20, true, 64);
  return 0;
}
