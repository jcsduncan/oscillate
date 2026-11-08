#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Run in an uninstrumented Release build; timing is not a CTest gate.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;
using namespace oscillate::native_dsp::synth;

std::vector<double> kit(std::string_view name) {
  const auto& table = *knock::engine().descriptor;
  for (const auto& entry : table.presets) {
    if (entry.name == name) {
      return preset_values(table, entry).value();
    }
  }
  return default_values(table);
}

// Every pad at full Width: every mode placed, every hit scattered and the
// far ear delayed, so each voice pays for its stereo modes and Haas line.
std::vector<double> wide(std::vector<double> values) {
  const auto& table = *knock::engine().descriptor;
  for (std::size_t pad = 1; pad <= knock::pad_count; ++pad) {
    const std::string prefix = "knock.pad" + std::to_string(pad) + ".width.";
    values[find_parameter(table, prefix + "spread").value()] = 1.0;
    values[find_parameter(table, prefix + "motion").value()] = 2.0; // Scatter.
    values[find_parameter(table, prefix + "depth").value()] = 0.8;
    values[find_parameter(table, prefix + "haas").value()] = 10.0;
  }
  return values;
}

// Every pad on every sixteenth at 124 BPM for ten seconds, so both voices
// of each pad ring. The kit loads a sixteenth before the first hits, as a
// kit chosen while playing does: the kernel calibrates its pads over its
// first control ticks, so "first hits" is the block where all eight land.
// With `sweep`, that parameter is automated across its range and back every
// two seconds, as a knob turned while the pattern plays.
void run(const char* label, const std::vector<double>& values, std::size_t block,
         std::string_view sweep = {}) {
  constexpr double rate = 48'000.0;
  auto runtime = std::move(NativeSynthRuntime::create(knock::engine(), values, rate).value());
  std::vector<engine::StereoFrame> buffer(block);
  const auto sixteenth = static_cast<std::size_t>(60.0 / 124.0 / 4.0 * rate);
  const auto blocks = static_cast<std::size_t>(10.0 * rate / static_cast<double>(block));
  std::vector<double> block_ns;
  std::vector<engine::BlockMidiEvent> hits;
  const auto& table = *knock::engine().descriptor;
  std::uint32_t swept = 0;
  for (std::uint32_t i = 0; i < table.parameters.size(); ++i) {
    if (table.parameters[i].id == sweep) {
      swept = i;
    }
  }
  std::vector<double> lane_values(block);
  std::vector<ParameterLane> lanes;
  std::uint64_t id = 1;
  for (std::size_t b = 0; b < blocks; ++b) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    hits.clear();
    const std::size_t from = b * block;
    const std::size_t next = (from + sixteenth - 1) / sixteenth * sixteenth;
    if (next > 0 && next < from + block) {
      for (std::uint8_t pad = 0; pad < knock::pad_count; ++pad) {
        engine::BlockMidiEvent event;
        event.offset = static_cast<std::uint32_t>(next - from);
        event.clip_id_bits = {1, 1};
        event.note_id_bits = {0, id++};
        event.kind = engine::MidiEventKind::note_on;
        event.key = static_cast<std::uint8_t>(knock::first_key + pad);
        event.velocity = 110;
        hits.push_back(event);
      }
    }
    lanes.clear();
    if (!sweep.empty()) {
      const auto& parameter = table.parameters[swept];
      const double period = 2.0 * rate;
      for (std::size_t i = 0; i < block; ++i) {
        const double phase = std::fmod(static_cast<double>(from + i) / period, 1.0);
        const double t = phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase;
        lane_values[i] = parameter.minimum + t * (parameter.maximum - parameter.minimum);
      }
      lanes.push_back({swept, lane_values});
    }
    const auto start = std::chrono::steady_clock::now();
    (void)runtime->process(buffer, hits, from, 1, {}, lanes);
    const auto stop = std::chrono::steady_clock::now();
    block_ns.push_back(std::chrono::duration<double, std::nano>(stop - start).count());
  }
  const std::size_t first_hits = (sixteenth + block - 1) / block - 1;
  const double first = block_ns.at(first_hits);
  const auto skip = static_cast<std::ptrdiff_t>(rate / static_cast<double>(block));
  std::vector<double> sorted(block_ns.begin() + skip, block_ns.end());
  double total = 0.0;
  for (const double ns : sorted) {
    total += ns;
  }
  std::sort(sorted.begin(), sorted.end());
  const double budget = static_cast<double>(block) / rate * 1e9;
  std::printf("%-30s block %3zu: mean %6.2f%% core, p99 block %6.2f%%, max %7.1f%%, first hits "
              "%7.1f%%\n",
              label, block, 100.0 * total / static_cast<double>(sorted.size()) / budget,
              100.0 * sorted[sorted.size() * 99 / 100] / budget, 100.0 * sorted.back() / budget,
              100.0 * first / budget);
}

} // namespace

int main() {
  for (std::size_t block : {std::size_t{64}, std::size_t{128}}) {
    for (const char* name : {"Minimal", "Dub Techno", "Organic", "Glass Garden"}) {
      run(name, kit(name), block);
    }
    run("Organic, turning Pitch", kit("Organic"), block, "knock.pad1.res.pitch");
    run("Organic, all wide", wide(kit("Organic")), block);
    run("Organic, turning Decay", kit("Organic"), block, "knock.decay");
  }
  return 0;
}
