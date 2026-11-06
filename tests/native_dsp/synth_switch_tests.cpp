#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Engine-generic contract (ADR-0034): automating any live discrete parameter
// mid-note never clicks. A stepped lane switches the value at a control
// tick; the switch must not produce a transient larger than the sound's own
// steady state before or after it. The click measure is the largest second
// difference, which a step or a corner makes spike and smooth timbre does not.
namespace {
using namespace synth_test;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t block = 64;
constexpr std::size_t switch_frame = 24'000; // On the 32-frame control grid.
constexpr std::size_t total = 563 * block;   // A whole number of blocks.

std::vector<double> render_switch(const SynthEngine& engine, std::size_t parameter, double from,
                                  double to) {
  auto values = preset(*engine.descriptor, engine.descriptor->presets.front().name);
  values[parameter] = from;
  auto runtime = make_runtime(engine, values, rate);
  const bool drums = engine.descriptor->id == "pulse";
  const std::uint8_t keys[] = {static_cast<std::uint8_t>(drums ? 36 : 48),
                               static_cast<std::uint8_t>(drums ? 42 : 55),
                               static_cast<std::uint8_t>(drums ? 46 : 60)};
  std::vector<double> out(total);
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<double> lane(block);
  for (std::size_t position = 0; position < total; position += block) {
    std::vector<engine::BlockMidiEvent> events;
    // Drums are one-shots: re-hit them so something rings at the switch.
    if (position == 0 || (drums && position % 12'000 == 0)) {
      for (std::uint64_t k = 0; k < 3; ++k) {
        engine::BlockMidiEvent event;
        event.clip_id_bits = {1, 1};
        event.note_id_bits = {0, position + k + 1};
        event.kind = engine::MidiEventKind::note_on;
        event.key = keys[k];
        event.velocity = 100;
        events.push_back(event);
      }
    }
    for (std::size_t i = 0; i < block; ++i) {
      lane[i] = position + i < switch_frame ? from : to;
    }
    const ParameterLane lanes[] = {{static_cast<std::uint32_t>(parameter), lane}};
    buffer.assign(block, {0.0F, 0.0F});
    const auto report = runtime->process(buffer, events, position, 1, {}, lanes);
    REQUIRE_EQ(report.status, SynthProcessStatus::rendered, "stepped lane accepted");
    for (std::size_t i = 0; i < block; ++i) {
      out[position + i] = 0.5 * (buffer[i].left + buffer[i].right);
    }
  }
  return out;
}

double peak_curvature(const std::vector<double>& x, std::size_t from, std::size_t to) {
  double peak = 0.0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i + 1 < to; ++i) {
    peak = std::max(peak, std::fabs(x[i + 1] - 2.0 * x[i] + x[i - 1]));
  }
  return peak;
}

} // namespace

int main() {
  const bool report_only = std::getenv("SWITCH_REPORT") != nullptr;
  int failures = 0;
  for (const auto* engine : engines()) {
    const auto& table = *engine->descriptor;
    for (std::size_t p = 0; p < table.parameters.size(); ++p) {
      const auto& parameter = table.parameters[p];
      if (!is_discrete(parameter) || parameter.timing != ParameterTiming::live ||
          !parameter.automatable) {
        continue;
      }
      const auto defaults = preset(table, table.presets.front().name);
      const double base = defaults[p];
      // Switch from the preset's value to each other end of the range.
      for (double other : {parameter.minimum, parameter.maximum,
                           std::round(0.5 * (parameter.minimum + parameter.maximum))}) {
        if (other == base) {
          continue;
        }
        if (std::getenv("SWITCH_TRACE") != nullptr) {
          std::fprintf(stderr, "%s %g -> %g\n", std::string(parameter.id).c_str(), base, other);
        }
        const auto x = render_switch(*engine, p, base, other);
        const double before = peak_curvature(x, switch_frame - 4'800, switch_frame - 64);
        const double after = peak_curvature(x, switch_frame + 2'400, switch_frame + 7'200);
        const double around = peak_curvature(x, switch_frame - 32, switch_frame + 512);
        const double steady = std::max({before, after, 1e-4});
        const double ratio = around / steady;
        if (ratio > 2.0) {
          ++failures;
          std::printf("CLICK %-8s %-28s %g -> %g: transient %.2fx steady\n",
                      std::string(table.id).c_str(), std::string(parameter.id).c_str(), base, other,
                      ratio);
        } else if (report_only) {
          std::printf("ok    %-8s %-28s %g -> %g: %.2fx\n", std::string(table.id).c_str(),
                      std::string(parameter.id).c_str(), base, other, ratio);
        }
      }
    }
  }
  if (failures > 0) {
    std::cerr << "FAILED: " << failures << " discrete switches click\n";
    return 1;
  }
  std::cout << "discrete switches are click-free in every engine\n";
  return 0;
}
