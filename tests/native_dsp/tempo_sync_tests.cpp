#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/transport.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/timeline/bar_grid.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;
using namespace oscillate::native_dsp::synth;

using oscillate::test::require;

bool near(double a, double b) { return std::fabs(a - b) < tolerance::double_accumulation; }

constexpr std::int64_t quarter = timeline::ticks_per_quarter;

void bar_grid_follows_the_meter() {
  // Two bars of 4/4, then three of 3/4, then 6/8.
  auto meter =
      timeline::MeterMap::create({{{0}, 4, 4}, {{8 * quarter}, 3, 4}, {{17 * quarter}, 6, 8}}, 1);
  REQUIRE_OK(meter, "meter map is valid");
  const timeline::BarGrid grid(meter.value());
  double bar = 0.0;
  double beats = 0.0;
  grid.locate(0, bar, beats);
  REQUIRE_TRUE(near(bar, 0.0), "bar 0 starts at tick 0 in 4/4");
  REQUIRE_TRUE(near(beats, 4.0), "bar 0 starts at tick 0 in 4/4");
  grid.locate(6 * quarter, bar, beats);
  REQUIRE_TRUE(near(bar, 1.5), "beat 3 of the second bar is bar 1.5");
  REQUIRE_TRUE(near(beats, 4.0), "beat 3 of the second bar is bar 1.5");
  grid.locate(8 * quarter, bar, beats);
  REQUIRE_TRUE(near(bar, 2.0), "3/4 starts at bar 2");
  REQUIRE_TRUE(near(beats, 3.0), "3/4 starts at bar 2");
  grid.locate(15 * quarter + quarter / 2, bar, beats);
  REQUIRE_TRUE(near(bar, 2.0 + 7.5 / 3.0), "bars count three beats in 3/4");
  REQUIRE_TRUE(near(beats, 3.0), "bars count three beats in 3/4");
  grid.locate(17 * quarter, bar, beats);
  REQUIRE_TRUE(near(bar, 5.0), "6/8 is three quarter notes a bar");
  REQUIRE_TRUE(near(beats, 3.0), "6/8 is three quarter notes a bar");
  grid.locate(17 * quarter + 3 * quarter / 2, bar, beats);
  require(near(bar, 5.5), "half a 6/8 bar later");
}

void bar_divisions_scale_with_the_meter() {
  Transport waltz;
  waltz.beats_per_bar = 3.0;
  waltz.bar_at_block_start = 2.5;
  waltz.beat_at_block_start = 7.5;
  waltz.beats_per_frame = 2.0 / 48'000.0; // 120 BPM.
  const std::size_t one_bar = first_bar_division;
  const std::size_t quarter_note = 8;
  REQUIRE_EQ(sync_divisions[one_bar], "1 Bar", "the shared division table");
  REQUIRE_EQ(sync_divisions[quarter_note], "1/4", "the shared division table");
  require(near(sync_beats(one_bar, waltz), 3.0), "1 Bar in 3/4 is three beats");
  require(near(sync_beats(one_bar + 1, waltz), 6.0), "2 Bars in 3/4 is six beats");
  require(near(sync_beats(quarter_note, waltz), 1.0), "note divisions ignore the meter");
  require(near(sync_cycles(one_bar, waltz), 2.5), "bar LFOs count bars from the grid");
  require(near(sync_cycles(one_bar + 2, waltz), 2.5 / 4.0), "4 Bars cycles a quarter as fast");
  require(near(sync_cycles(quarter_note, waltz), 7.5), "beat LFOs count beats");
  require(near(sync_seconds(one_bar, waltz, 48'000.0), 1.5), "a 3/4 bar at 120 BPM is 1.5 s");
  Transport unknown;
  require(near(sync_seconds(one_bar, unknown, 48'000.0), 2.0),
          "with no tempo, 120 BPM in 4/4 is assumed");
}

// A kernel that records the transport each control tick receives.
std::vector<Transport> seen;
class RecordingKernel final : public SynthKernel {
public:
  void control(std::span<const double>, std::uint32_t, const Transport& t) noexcept override {
    seen.push_back(t);
  }
  void note_on(const NoteEvent&) noexcept override {}
  void note_off(const NoteEvent&) noexcept override {}
  void render(std::span<engine::StereoFrame>) noexcept override {}
  void reset() noexcept override {}
  [[nodiscard]] KernelStatus status() const noexcept override { return {}; }
};

void runtime_advances_bars_between_ticks() {
  static constexpr std::array<ParameterDescriptor, 1> parameters{{{"test.level",
                                                                   "Level",
                                                                   "Test",
                                                                   "",
                                                                   ParameterScale::linear,
                                                                   0,
                                                                   1,
                                                                   0.5,
                                                                   {},
                                                                   ParameterTiming::live,
                                                                   0.0}}};
  static const EngineDescriptor descriptor{"test", "Test", 1, parameters, {}};
  static const SynthEngine test_engine{
      &descriptor,
      [](double) -> std::unique_ptr<SynthKernel> { return std::make_unique<RecordingKernel>(); },
      [](std::span<const double>, double) -> std::uint64_t { return 0; }};
  const std::vector<double> values{0.5};
  auto runtime = NativeSynthRuntime::create(test_engine, values, 48'000.0);
  REQUIRE_OK(runtime, "runtime is created");
  std::vector<engine::StereoFrame> buffer(64);
  Transport t;
  t.beat_at_block_start = 3.0;
  t.bar_at_block_start = 1.0;
  t.beats_per_bar = 3.0;
  t.beats_per_frame = 0.001;
  seen.clear();
  (void)runtime.value()->process(buffer, {}, 0, 1, t, {});
  REQUIRE_EQ(seen.size(), 2, "two control ticks in a 64-frame block");
  REQUIRE_TRUE(near(seen[1].beat_at_block_start, 3.0 + 32 * 0.001),
               "the second tick sees the bar position advanced in the meter");
  REQUIRE_TRUE(near(seen[1].bar_at_block_start, 1.0 + 32 * 0.001 / 3.0),
               "the second tick sees the bar position advanced in the meter");
  REQUIRE_TRUE(near(seen[1].beats_per_bar, 3.0),
               "the second tick sees the bar position advanced in the meter");
}

} // namespace

int main() {
  try {
    bar_grid_follows_the_meter();
    bar_divisions_scale_with_the_meter();
    runtime_advances_bars_between_ticks();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "tempo sync contract passed\n";
  return 0;
}
