#include "oscillate/automation/curve.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {
// The largest block measured, the sample stride between spans and how many
// distinct spans the timed loops cycle through.
constexpr std::size_t max_block_frames = 128;
constexpr std::int64_t block_stride = 128;
constexpr int span_count = 512;
} // namespace

int main() {
  try {
    using namespace oscillate;
    auto tempo_result = timeline::TempoMap::create(
        {{timeline::TickPosition{0}, 120.0, timeline::TempoShape::linear_bpm},
         {timeline::TickPosition{timeline::ticks_per_quarter * 4}, 90.0,
          timeline::TempoShape::step}},
        1);
    if (!tempo_result) {
      return 1;
    }
    auto tempo = std::make_shared<const timeline::TempoMap>(std::move(tempo_result.value()));
    const automation::Curve curve{
        automation::Timebase::musical,
        {{0, 0.1, automation::Shape::bezier, {0.3, 0.1, 0.7, 0.9}},
         {timeline::ticks_per_quarter * 2, 0.8, automation::Shape::bezier},
         {timeline::ticks_per_quarter * 4, 0.2}}};
    auto prepared = automation::PreparedAutomation::create({0.0, 1.0, 0.5, false}, curve,
                                                           std::nullopt, tempo, 48'000);
    if (!prepared) {
      return 1;
    }

    std::array<double, max_block_frames> output{};
    std::array<timeline::TickPosition, max_block_frames> ticks{};
    double checksum = 0.0;
    for (std::size_t frames : {64U, 128U}) {
      constexpr int iterations = 20'000;
      for (int warmup = 0; warmup < 100; ++warmup) {
        if (!prepared.value().fill_musical_ticks(
                timeline::SamplePosition{static_cast<std::int64_t>(warmup) * block_stride},
                {ticks.data(), frames}) ||
            !prepared.value().evaluate_block(
                timeline::SamplePosition{static_cast<std::int64_t>(warmup) * block_stride},
                {ticks.data(), frames}, {output.data(), frames})) {
          return 1;
        }
      }
      const auto start = std::chrono::steady_clock::now();
      for (int iteration = 0; iteration < iterations; ++iteration) {
        // Repeat the same span to measure evaluator cost, without transport-end effects.
        if (!prepared.value().fill_musical_ticks(
                timeline::SamplePosition{static_cast<std::int64_t>(iteration % span_count) *
                                         block_stride},
                {ticks.data(), frames})) {
          return 1;
        }
        checksum += output[static_cast<std::size_t>(iteration) % frames];
      }
      const auto elapsed =
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
              .count();
      // Each span's own ticks, filled outside the timed loop, so the curve is
      // evaluated over the whole range rather than the last span's ticks.
      std::vector<std::array<timeline::TickPosition, max_block_frames>> span_ticks(span_count);
      for (std::size_t span = 0; span < span_ticks.size(); ++span) {
        if (!prepared.value().fill_musical_ticks(
                timeline::SamplePosition{static_cast<std::int64_t>(span) * block_stride},
                {span_ticks[span].data(), frames})) {
          return 1;
        }
      }
      const auto evaluate_start = std::chrono::steady_clock::now();
      for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto span = static_cast<std::size_t>(iteration % span_count);
        if (!prepared.value().evaluate_block(
                timeline::SamplePosition{static_cast<std::int64_t>(span) * block_stride},
                {span_ticks[span].data(), frames}, {output.data(), frames})) {
          return 1;
        }
        checksum += output[static_cast<std::size_t>(iteration) % frames];
      }
      const auto evaluate_elapsed = std::chrono::duration<double, std::micro>(
                                        std::chrono::steady_clock::now() - evaluate_start)
                                        .count();
      std::cout << frames << " frames: ticks " << elapsed / iterations << " us/block, curve "
                << evaluate_elapsed / iterations << " us/block\n";
    }
    std::cout << "checksum: " << checksum << '\n';
  } catch (...) {
    return 1;
  }
}
