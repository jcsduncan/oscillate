#include "oscillate/native_dsp/synth/drift_engine.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace synth_test;

// A kernel whose telemetry is a whole frame of one counter value: a torn
// frame would mix two values.
class CountingKernel final : public SynthKernel {
public:
  void control(std::span<const double>, std::uint32_t, const SynthTransport&) noexcept override {}
  void note_on(const NoteEvent&) noexcept override {}
  void note_off(const NoteEvent&) noexcept override {}
  void render(std::span<engine::StereoFrame>) noexcept override {}
  void reset() noexcept override {}
  [[nodiscard]] KernelStatus status() const noexcept override { return {}; }
  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    ++blocks_;
    std::fill(out.begin(), out.end(), static_cast<float>(blocks_));
    return out.size();
  }

private:
  mutable std::uint32_t blocks_ = 0;
};

const SynthEngine& counting_engine() {
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
  static const SynthEngine engine{
      &descriptor,
      [](double) -> std::unique_ptr<SynthKernel> { return std::make_unique<CountingKernel>(); },
      [](std::span<const double>, double) -> std::uint64_t { return 0; }};
  return engine;
}

void frames_are_never_torn_and_never_allocate() {
  auto runtime = make_runtime(counting_engine(), {0.5});
  std::vector<float> frame;
  require(!runtime->read_telemetry(frame), "nothing before the first block");
  std::vector<engine::StereoFrame> buffer(64);
  (void)runtime->process(buffer, {}, 0, 1, {}, {});
  REQUIRE_TRUE(runtime->read_telemetry(frame), "the first block's frame is readable");
  REQUIRE_EQ(frame.size(), max_telemetry_values, "the first block's frame is readable");
  REQUIRE_EQ(frame.front(), 1.0F, "the first block's frame is readable");
  std::atomic<bool> done{false};
  std::atomic<bool> torn{false};
  std::atomic<std::uint32_t> reads{0};
  std::thread reader([&] {
    std::vector<float> seen;
    float last = 0.0F;
    while (!done.load()) {
      if (runtime->read_telemetry(seen)) {
        const float value = seen.front();
        if (std::any_of(seen.begin(), seen.end(), [&](float x) { return x != value; }) ||
            value < last) {
          torn = true;
        }
        last = value;
        ++reads;
      }
    }
  });
  counting_allocations = true;
  counted_allocations = 0;
  for (std::uint64_t block = 1; block < 20'000; ++block) {
    (void)runtime->process(buffer, {}, block * 64, 1, {}, {});
  }
  const std::size_t made = counted_allocations;
  counting_allocations = false;
  done = true;
  reader.join();
  REQUIRE_EQ(made, 0, "publishing telemetry never allocates");
  require(!torn.load(), "the reader never sees a torn or stale-then-older frame");
  REQUIRE_GT(reads.load(), 0, "the reader saw frames while the callback ran");
}

void torque_reports_its_sequencer_step() {
  auto values = preset(*torque::engine().descriptor, "Seq Acid Line");
  values[index(*torque::engine().descriptor, "torque.seq.swing")] = 0; // Straight steps.
  auto runtime = make_runtime(torque::engine(), values);
  // 120 BPM sixteenths: 6000 frames a step. Render into step 5.
  (void)render(*runtime, note(0, 96'000, 48, 1), 6'000 * 5 + 100);
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "Torque publishes its layout");
  REQUIRE_EQ(frame.size(), torque::telemetry::size, "Torque publishes its layout");
  REQUIRE_EQ(frame[torque::telemetry::running], 1.0F, "the sequencer is running");
  REQUIRE_EQ(frame[torque::telemetry::step], 5.0F, "the playing step is step 5 (zero-based)");
  REQUIRE_GT(frame[torque::telemetry::cutoff], 20.0F, "the live cutoff is reported");
}

void pulse_reports_pads_and_gain_reduction() {
  auto values = preset(*pulse::engine().descriptor, "Industrial");
  auto runtime = make_runtime(pulse::engine(), values);
  (void)render(*runtime, note(0, 100, 36, 1, 127), 2'000);
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "Pulse publishes its layout");
  REQUIRE_EQ(frame.size(), pulse::telemetry::size, "Pulse publishes its layout");
  REQUIRE_GT(frame[pulse::telemetry::first_pad], 0.1F, "the kick pad is sounding");
  REQUIRE_EQ(frame[pulse::telemetry::first_pad + 2], 0.0F, "the snare pad is silent");
  REQUIRE_GT(frame[pulse::telemetry::gain_reduction], 0.0F, "the glue compressor is working");
}

void drift_reports_its_real_grains() {
  auto values = preset(*drift::engine().descriptor, "Init");
  auto runtime = make_runtime(drift::engine(), values);
  (void)render(*runtime, note(0, 96'000, 60, 1), 48'000);
  std::vector<float> frame;
  require(runtime->read_telemetry(frame), "Drift publishes");
  const auto grains = static_cast<std::size_t>(frame[drift::telemetry::count]);
  REQUIRE_GT(grains, 0, "a count then one record per grain");
  REQUIRE_EQ(frame.size(), drift::telemetry::first_grain + grains * drift::telemetry::stride,
             "a count then one record per grain");
  for (std::size_t g = 0; g < grains; ++g) {
    const float* record = &frame[drift::telemetry::first_grain + g * drift::telemetry::stride];
    REQUIRE_EQ(record[drift::telemetry::key], 60.0F, "grains belong to the held note");
    REQUIRE_GE(record[drift::telemetry::behind], 0.0F, "grains read behind the write head");
    REQUIRE_LT(record[drift::telemetry::behind], static_cast<float>(drift::buffer_seconds),
               "grains read behind the write head");
    REQUIRE_GE(record[drift::telemetry::phase], 0.0F, "window phase is within the grain");
    REQUIRE_LT(record[drift::telemetry::phase], 1.0F, "window phase is within the grain");
    REQUIRE_EQ(record[drift::telemetry::frozen], 0.0F, "the voice is not frozen");
  }
  values[index(*drift::engine().descriptor, "drift.freeze")] = 1;
  auto frozen = make_runtime(drift::engine(), values);
  (void)render(*frozen, note(0, 144'000, 60, 1), 96'000);
  REQUIRE_TRUE(frozen->read_telemetry(frame), "a frozen voice says so");
  REQUIRE_GT(frame[drift::telemetry::count], 0.0F, "a frozen voice says so");
  REQUIRE_EQ(frame[drift::telemetry::first_grain + drift::telemetry::frozen], 1.0F,
             "a frozen voice says so");
}

} // namespace

int main() {
  try {
    frames_are_never_torn_and_never_allocate();
    torque_reports_its_sequencer_step();
    pulse_reports_pads_and_gain_reduction();
    drift_reports_its_real_grains();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "synth telemetry contract passed\n";
  return 0;
}
