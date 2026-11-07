#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
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

std::vector<double> kit(std::string_view name) {
  const auto& table = *pulse::engine().descriptor;
  for (const auto& entry : table.presets) {
    if (entry.name == name) {
      return preset_values(table, entry).value();
    }
  }
  return default_values(table);
}

// Every pad re-hit each second for ten seconds, so all twelve always ring.
void run(const char* label, const std::vector<double>& values, std::size_t block) {
  constexpr double rate = 48'000.0;
  auto runtime = std::move(NativeSynthRuntime::create(pulse::engine(), values, rate).value());
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> hits(pulse::pad_count);
  for (std::uint8_t pad = 0; pad < pulse::pad_count; ++pad) {
    hits[pad].clip_id_bits = {1, 1};
    hits[pad].note_id_bits = {0, pad + 1U};
    hits[pad].kind = engine::MidiEventKind::note_on;
    hits[pad].key = static_cast<std::uint8_t>(pulse::first_key + pad);
    hits[pad].velocity = 110;
  }
  const auto blocks = static_cast<std::size_t>(10.0 * rate / static_cast<double>(block));
  const auto per_second = static_cast<std::size_t>(rate / static_cast<double>(block));
  std::vector<double> block_ns;
  for (std::size_t b = 0; b < blocks; ++b) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    const bool hit = b % per_second == 0;
    const auto start = std::chrono::steady_clock::now();
    (void)runtime->process(buffer, hit ? std::span(hits) : std::span(hits).subspan(0, 0), b * block,
                           1, {}, {});
    const auto stop = std::chrono::steady_clock::now();
    block_ns.push_back(std::chrono::duration<double, std::nano>(stop - start).count());
  }
  std::vector<double> sorted(block_ns.begin() + static_cast<std::ptrdiff_t>(per_second),
                             block_ns.end());
  double total = 0.0;
  for (double ns : sorted) {
    total += ns;
  }
  std::sort(sorted.begin(), sorted.end());
  const double budget = static_cast<double>(block) / rate * 1e9;
  std::printf("%-34s block %3zu: mean %6.2f%% core, p99 block %6.2f%%\n", label, block,
              100.0 * total / static_cast<double>(sorted.size()) / budget,
              100.0 * sorted[sorted.size() * 99 / 100] / budget);
}

} // namespace

int main() {
  auto heavy = kit("Industrial"); // Drives, glue, saturation and a large room.
  for (std::size_t block : {std::size_t{64}, std::size_t{128}}) {
    run("12 pads ringing, Industrial", heavy, block);
    run("12 pads ringing, 808 Classic", kit("808 Classic"), block);
  }
  return 0;
}
