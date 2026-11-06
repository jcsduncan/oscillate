#include "oscillate/application/spectrum_tap.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using oscillate::application::SpectrumWindow;
using oscillate::application::StereoSpectrumTap;
using oscillate::engine::StereoFrame;

using oscillate::test::require;

template <std::size_t N>
void feed(StereoSpectrumTap& tap, std::uint64_t position, std::uint64_t generation,
          std::array<StereoFrame, N>& block) {
  for (std::size_t index = 0; index < N; ++index) {
    const auto sample = static_cast<float>(position + index);
    block[index] = {sample, -sample};
  }
  require(tap.observe(block, {position, generation}), "valid callback block rejected");
}

void overlapping_windows_and_positions() {
  auto tap = std::make_unique<StereoSpectrumTap>();
  std::array<StereoFrame, 64> block{};
  SpectrumWindow output{};
  require(!tap->try_pop(output), "empty tap returned a window");
  for (std::uint64_t position = 0; position < SpectrumWindow::frame_count;
       position += block.size()) {
    feed(*tap, position, 1, block);
  }
  REQUIRE_TRUE(tap->try_pop(output), "first complete window metadata failed");
  REQUIRE_EQ(output.end_sample, 2048, "first complete window metadata failed");
  REQUIRE_EQ(output.transport_generation, 1, "first complete window metadata failed");
  REQUIRE_EQ(output.epoch, tap->epoch(), "first complete window metadata failed");
  for (std::size_t index = 0; index < output.frames.size(); ++index) {
    REQUIRE_EQ(output.frames[index].left, static_cast<float>(index),
               "first window was not chronological stereo audio");
    REQUIRE_EQ(output.frames[index].right, -static_cast<float>(index),
               "first window was not chronological stereo audio");
  }
  require(!tap->try_pop(output), "published before 1024-frame hop");
  for (std::uint64_t position = 2048; position < 3072; position += block.size()) {
    feed(*tap, position, 1, block);
  }
  REQUIRE_TRUE(tap->try_pop(output), "overlapped window missing");
  REQUIRE_EQ(output.end_sample, 3072, "overlapped window missing");
  for (std::size_t index = 0; index < output.frames.size(); ++index) {
    REQUIRE_EQ(output.frames[index].left, static_cast<float>(1024 + index),
               "overlapped window contains wrong sample interval");
  }
  REQUIRE_TRUE(!tap->try_pop(output), "extra window or false overrun was reported");
  REQUIRE_EQ(tap->dropped_windows(), 0, "extra window or false overrun was reported");
}

void epoch_discards_stale_windows() {
  auto tap = std::make_unique<StereoSpectrumTap>();
  std::array<StereoFrame, 128> block{};
  SpectrumWindow output{};
  for (std::uint64_t position = 0; position < 3072; position += block.size()) {
    feed(*tap, position, 1, block);
  }
  const auto first_epoch = tap->epoch();
  for (std::uint64_t position = 5000; position < 7048; position += block.size()) {
    feed(*tap, position, 2, block);
  }
  REQUIRE_NE(tap->epoch(), first_epoch, "seek returned queued audio from previous generation");
  REQUIRE_TRUE(tap->try_pop(output), "seek returned queued audio from previous generation");
  REQUIRE_EQ(output.transport_generation, 2, "seek returned queued audio from previous generation");
  REQUIRE_EQ(output.end_sample, 7048, "seek returned queued audio from previous generation");
  REQUIRE_EQ(output.frames.front().left, 5000.0F,
             "seek returned queued audio from previous generation");
  for (std::uint64_t position = 10'000; position < 12'048; position += block.size()) {
    feed(*tap, position, 2, block);
  }
  REQUIRE_TRUE(tap->try_pop(output), "same-generation discontinuity retained an older window");
  REQUIRE_EQ(output.end_sample, 12'048, "same-generation discontinuity retained an older window");
  REQUIRE_EQ(output.frames.front().left, 10'000.0F,
             "same-generation discontinuity retained an older window");
  const auto before_stop = tap->epoch();
  tap->invalidate_after_quiesce();
  REQUIRE_NE(tap->epoch(), before_stop, "stopped transport returned queued audio");
  REQUIRE_TRUE(!tap->try_pop(output), "stopped transport returned queued audio");
}

void overrun_and_recovery() {
  auto tap = std::make_unique<StereoSpectrumTap>();
  std::array<StereoFrame, 128> block{};
  for (std::uint64_t position = 0; position < 8192; position += block.size()) {
    feed(*tap, position, 1, block);
  }
  REQUIRE_EQ(tap->dropped_windows(), 3, "full queue did not count dropped new windows");
  SpectrumWindow output{};
  for (std::uint64_t end = 2048; end <= 5120; end += 1024) {
    REQUIRE_TRUE(tap->try_pop(output), "queue overrun replaced an unread window");
    REQUIRE_EQ(output.end_sample, end, "queue overrun replaced an unread window");
  }
  require(!tap->try_pop(output), "full queue returned a fifth window");
  for (std::uint64_t position = 8192; position < 9216; position += block.size()) {
    feed(*tap, position, 1, block);
  }
  REQUIRE_TRUE(tap->try_pop(output), "tap did not recover after worker drained queue");
  REQUIRE_EQ(output.end_sample, 9216, "tap did not recover after worker drained queue");
  REQUIRE_EQ(output.frames.front().left, 7168.0F, "tap did not recover after worker drained queue");
}

void rejected_blocks_invalidate_window() {
  auto tap = std::make_unique<StereoSpectrumTap>();
  std::array<StereoFrame, 1024> block{};
  std::array<StereoFrame, 1025> oversized{};
  SpectrumWindow output{};
  REQUIRE_TRUE(!tap->observe(block, {0, 0}), "invalid callback input was accepted");
  REQUIRE_TRUE(!tap->observe(oversized, {0, 1}), "invalid callback input was accepted");
  REQUIRE_TRUE(!tap->observe(block, {std::numeric_limits<std::uint64_t>::max(), 1}),
               "invalid callback input was accepted");
  REQUIRE_EQ(tap->rejected_blocks(), 3, "invalid callback input was accepted");
  REQUIRE_TRUE(!tap->try_pop(output), "invalid callback input was accepted");
  feed(*tap, 100, 4, block);
  require(!tap->try_pop(output), "incomplete window was published after invalid input");
  feed(*tap, 1124, 4, block);
  REQUIRE_TRUE(tap->try_pop(output), "rejected input contaminated next valid window");
  REQUIRE_EQ(output.end_sample, 2148, "rejected input contaminated next valid window");
  REQUIRE_EQ(output.frames.front().left, 100.0F, "rejected input contaminated next valid window");
}

void callback_does_not_allocate() {
  auto tap = std::make_unique<StereoSpectrumTap>();
  std::array<StereoFrame, 64> block{};
  block.fill({0.25F, -0.125F});
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t position = 0; position < 8192; position += block.size()) {
    if (!tap->observe(block, {position, 1})) {
      counting_allocations = false;
      throw std::runtime_error("valid callback block rejected");
    }
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "spectrum callback tap allocated memory");
}

void concurrent_consumer_sees_complete_windows() {
  auto tap = std::make_unique<StereoSpectrumTap>();
  std::atomic<bool> done{false};
  std::atomic<bool> bad{false};
  std::atomic<std::size_t> received{0};
  std::thread worker([&] {
    SpectrumWindow output{};
    while (true) {
      if (!tap->try_pop(output)) {
        if (done.load()) {
          break;
        }
        std::this_thread::yield();
        continue;
      }
      ++received;
      if (output.transport_generation != 1 || output.epoch != tap->epoch() ||
          output.end_sample < SpectrumWindow::frame_count) {
        bad.store(true);
      }
      const auto start = output.end_sample - SpectrumWindow::frame_count;
      for (std::size_t index = 0; index < output.frames.size(); ++index) {
        if (output.frames[index].left != static_cast<float>(start + index) ||
            output.frames[index].right != -static_cast<float>(start + index)) {
          bad.store(true);
          break;
        }
      }
    }
  });
  std::array<StereoFrame, 128> block{};
  for (std::uint64_t position = 0; position < 1024ULL * 40; position += block.size()) {
    feed(*tap, position, 1, block);
  }
  done.store(true);
  worker.join();
  REQUIRE_TRUE(!bad.load(), "worker observed a torn or missing window");
  REQUIRE_GT(received.load(), 0, "worker observed a torn or missing window");
}
} // namespace

int main() {
  try {
    overlapping_windows_and_positions();
    epoch_discards_stale_windows();
    overrun_and_recovery();
    rejected_blocks_invalidate_window();
    callback_does_not_allocate();
    concurrent_consumer_sees_complete_windows();
  } catch (const std::exception& error) {
    std::cerr << "spectrum tap contract: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
