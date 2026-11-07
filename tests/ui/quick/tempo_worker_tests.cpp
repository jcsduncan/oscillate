#include "../../../apps/daw_quick/src/tempo_worker.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace oscillate;

using oscillate::test::require;

project::SourceRecord source(std::uint64_t number) {
  auto id = project::SourceId::from_bits({1, number});
  require(id.has_value(), "fixture source ID invalid");
  return {.id = id.value(),
          .encoding = project::SourceEncoding::wav,
          .sha256 = std::string(64, static_cast<char>('a' + number - 1)),
          .byte_length = 8,
          .sample_rate = 48'000,
          .channels = 1,
          .frames = 8};
}

application::TempoCacheResult estimate(std::uint64_t generation) {
  application::TempoCacheResult result;
  result.estimate.identity = {.source_key = std::string(64, 'a'), .generation = generation};
  result.estimate.sample_rate = 48'000;
  result.estimate.frames = 8;
  result.estimate.bpm = 120;
  result.estimate.confidence = 0.9;
  result.estimate.suggested_beat_frames = {0, 4};
  return result;
}

void latest_selection_wins() {
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  quick::TempoWorker worker([&](const auto&, const auto&, std::uint64_t generation, const auto&) {
    if (generation == 1) {
      entered.release();
      release.acquire();
    }
    return foundation::Result<application::TempoCacheResult>::success(estimate(generation));
  });
  worker.submit({}, source(1), 1);
  entered.acquire();
  worker.submit({}, source(2), 2);
  release.release();
  std::optional<quick::TempoWorker::Outcome> result;
  oscillate::test::wait_until(
      [&] {
        result = worker.collect();
        return result.has_value();
      },
      "the tempo worker delivered an outcome");
  require(result && result->generation == 2 && result->source == source(2) && result->result,
          "stale tempo selection installed or latest analysis was lost");
}

void close_does_not_join_blocked_reader() {
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  std::binary_semaphore exited{0};
  std::atomic<bool> cancelled{false};
  auto worker = std::make_unique<quick::TempoWorker>(
      [&](const auto&, const auto&, std::uint64_t generation, const auto& stop) {
        entered.release();
        release.acquire();
        cancelled.store(stop(), std::memory_order_release);
        exited.release();
        return foundation::Result<application::TempoCacheResult>::success(estimate(generation));
      });
  worker->submit({}, source(1), 1);
  entered.acquire();
  worker.reset();
  release.release();
  exited.acquire();
  require(cancelled.load(std::memory_order_acquire), "late result was not cancelled");
}
} // namespace

int main() {
  try {
    latest_selection_wins();
    close_does_not_join_blocked_reader();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
