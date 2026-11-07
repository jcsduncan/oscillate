#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Run in an uninstrumented Release build; timing is not a CTest gate.
// Anvil plays four-to-the-floor at 124 BPM: every hit runs the 4x body, the
// rumble and the oversampled master; then the panel's preview of one beat.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp::synth;

void run(std::string_view name, std::size_t block) {
  constexpr double rate = 48'000.0;
  constexpr double seconds = 10.0;
  const auto& kick = anvil::engine();
  const auto& table = *kick.descriptor;
  std::size_t index = 0;
  while (table.presets[index].name != name)
    ++index;
  const auto values = preset_values(table, table.presets[index]).value();
  auto runtime = std::move(
      NativeSynthRuntime::create(kick, values, kick.preset_document(index), {}, rate).value());
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> hit(1);
  hit[0].clip_id_bits = {1, 1};
  hit[0].kind = engine::MidiEventKind::note_on;
  hit[0].key = 36;
  hit[0].velocity = 110;
  const auto beat = static_cast<std::uint64_t>(60.0 / 124.0 * rate);
  const auto blocks = static_cast<std::size_t>(seconds * rate / static_cast<double>(block));
  std::vector<double> block_ns;
  block_ns.reserve(blocks);
  for (std::size_t b = 0; b < blocks; ++b) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    const std::uint64_t start = b * block;
    const std::uint64_t next = (start + beat - 1) / beat * beat;
    std::span<const engine::BlockMidiEvent> events{};
    if (next < start + block) {
      hit[0].offset = static_cast<std::uint32_t>(next - start);
      hit[0].note_id_bits = {0, next / beat + 1};
      events = hit;
    }
    const auto t0 = std::chrono::steady_clock::now();
    (void)runtime->process(buffer, events, start, 1, {}, {});
    const auto t1 = std::chrono::steady_clock::now();
    block_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  const std::size_t skip = blocks / 10;
  double total = 0.0;
  for (std::size_t b = skip; b < blocks; ++b)
    total += block_ns[b];
  std::vector<double> sorted(block_ns.begin() + static_cast<std::ptrdiff_t>(skip), block_ns.end());
  std::sort(sorted.begin(), sorted.end());
  const double budget_ns = static_cast<double>(block) / rate * 1e9;
  const double mean = total / static_cast<double>(blocks - skip);
  std::printf("Anvil %-22s block %3zu: mean %6.2f%% core, p99 block %6.2f%%, max %6.2f%%\n",
              std::string(name).c_str(), block, 100.0 * mean / budget_ns,
              100.0 * sorted[sorted.size() * 99 / 100] / budget_ns,
              100.0 * sorted.back() / budget_ns);

  const auto t0 = std::chrono::steady_clock::now();
  const auto preview =
      anvil::render_preview(values, kick.preset_document(index), nullptr, 48'000, 124.0);
  const auto t1 = std::chrono::steady_clock::now();
  std::printf("Anvil %-22s preview of one beat (%zu frames): %.2f ms\n", std::string(name).c_str(),
              preview.master.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());
}

} // namespace

int main() {
  run("Warehouse Rumble", 128);
  run("Warehouse Rumble", 64);
  run("Hard Techno Screech", 128);
  run("Tech House Thump", 128);
  return 0;
}
