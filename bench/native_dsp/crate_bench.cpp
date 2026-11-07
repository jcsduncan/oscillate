#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Run in an uninstrumented Release build; timing is not a CTest gate.
// Crate holds `voices` long gate chops sounding at once, each pitched so
// every voice takes the band-limited scaled path, in a given time mode and
// era, for ten seconds at 48 kHz; reports the callback's share of a core.
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp::synth;

constexpr double rate = 48'000.0;
constexpr double seconds = 10.0;
constexpr std::size_t sample_frames = 48'000 * 8;

struct Load {
  std::string_view name;
  std::size_t voices;
  double time;  // crate.time
  double era;   // crate.era
  double pitch; // Semitones on every chop.
};

void run(const Load& load, std::size_t block) {
  const auto& engine = crate::engine();
  const auto& table = *engine.descriptor;
  auto values = default_values(table);
  const auto at = [&](std::string_view id) { return *find_parameter(table, id); };
  values[at("crate.time")] = load.time;
  values[at("crate.era")] = load.era;
  values[at("crate.voices")] = 32.0;
  project::NativeSynthDocument document;
  auto asset = std::make_shared<SynthMediaAsset>();
  asset->source_rate = 48'000;
  asset->frames.resize(sample_frames);
  std::uint32_t seed = 1;
  for (auto& frame : asset->frames) {
    seed = seed * 1'664'525U + 1'013'904'223U;
    frame.left = static_cast<float>(static_cast<double>(seed >> 8U) / 16'777'216.0 - 0.5);
    frame.right = frame.left;
  }
  document.media.push_back(
      {std::string(crate::sample_slot),
       project::ManagedAudioReference{project::SourceId::from_bits({1, 1}).value(),
                                      std::string(64, 'b'), 1, 48'000, 2, sample_frames},
       0});
  for (std::uint32_t slot = 0; slot < crate::max_chops; ++slot) {
    document.regions.push_back({slot, std::string(crate::sample_slot),
                                slot * (sample_frames / crate::max_chops), "other", ""});
    values[at(crate::chop_parameter_id(slot, "mode"))] = 1.0; // Gate: held for the run.
    values[at(crate::chop_parameter_id(slot, "pitch"))] = load.pitch;
    values[at(crate::chop_parameter_id(slot, "filter"))] = 1.0;
    values[at(crate::chop_parameter_id(slot, "cutoff"))] = 6'000.0;
  }
  document.labels.push_back({std::string(crate::bpm_label), "96"});
  SynthMedia media{{std::string(crate::sample_slot), std::string(64, 'b'), std::move(asset)}};
  auto runtime = std::move(
      NativeSynthRuntime::create(engine, values, document, std::move(media), rate).value());

  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> notes(load.voices);
  for (std::size_t v = 0; v < load.voices; ++v) {
    notes[v].clip_id_bits = {1, 1};
    notes[v].note_id_bits = {0, v + 1};
    notes[v].kind = engine::MidiEventKind::note_on;
    notes[v].key = static_cast<std::uint8_t>(crate::first_pad_key + v % crate::max_chops);
    notes[v].velocity = 100;
  }
  const double beats_per_frame = 124.0 / 60.0 / rate;
  const auto blocks = static_cast<std::size_t>(seconds * rate / static_cast<double>(block));
  std::vector<double> block_ns;
  block_ns.reserve(blocks);
  for (std::size_t b = 0; b < blocks; ++b) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    const double beat = static_cast<double>(b * block) * beats_per_frame;
    const SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    const auto events = b == 0 ? std::span<const engine::BlockMidiEvent>(notes)
                               : std::span<const engine::BlockMidiEvent>{};
    const auto t0 = std::chrono::steady_clock::now();
    (void)runtime->process(buffer, events, b * block, 1, transport, {});
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
  std::printf("Crate %-28s %2zu voices, block %3zu: mean %6.2f%% core, p99 %6.2f%%, max %6.2f%%\n",
              std::string(load.name).c_str(), load.voices, block, 100.0 * mean / budget_ns,
              100.0 * sorted[sorted.size() * 99 / 100] / budget_ns,
              100.0 * sorted.back() / budget_ns);
}

} // namespace

int main() {
  const Load loads[] = {
      {"repitch clean, unity", 16, 0.0, 0.0, 0.0},
      {"repitch clean, +7 st", 16, 0.0, 0.0, 7.0},
      {"repitch clean, +7 st", 32, 0.0, 0.0, 7.0},
      {"repitch clean, +24 st", 32, 0.0, 0.0, 24.0},
      {"repitch sp12, +7 st", 32, 0.0, 1.0, 7.0},
      {"repitch tape, +7 st", 32, 0.0, 4.0, 7.0},
      {"follow tempo clean, +7 st", 16, 1.0, 0.0, 7.0},
      {"follow tempo clean, +7 st", 32, 1.0, 0.0, 7.0},
  };
  for (const std::size_t block : {std::size_t{64}, std::size_t{128}}) {
    for (const auto& load : loads)
      run(load, block);
  }
  return 0;
}
