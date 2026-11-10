#include "effects/capture_queue.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// CaptureQueue (ADR-0125) with a real producer and consumer thread: frames
// arrive in order and whole; nothing written before a restart is read
// after the reader starts the new run; a drop followed by a write that fits
// starts exactly one run.
namespace {
using oscillate::native_dsp::effects::CaptureQueue;
using oscillate::test::require;

// A frame carries its writer's run and its index in that run.
struct Frame {
  std::uint64_t run = 0;
  std::uint64_t index = 0;
};

constexpr std::size_t capacity = 256;
constexpr std::size_t block = 16;
constexpr std::uint64_t blocks = 20'000;
// The writer restarts every this many blocks, as a seek would.
constexpr std::uint64_t restart_every = 97;

void two_threads_keep_order_and_runs() {
  CaptureQueue<Frame> queue(capacity);
  std::atomic<bool> done{false};
  // Writer-side runs: one per restart and per write that fits after a drop.
  std::atomic<std::uint64_t> writer_runs{0};
  std::thread producer([&] {
    std::uint64_t run = 0;
    std::uint64_t index = 0;
    for (std::uint64_t b = 0; b < blocks; ++b) {
      if (b % restart_every == restart_every - 1) {
        queue.restart();
        ++run;
        index = 0;
      }
      bool dropped = false;
      while (!queue.write(block, [&](std::size_t i) { return Frame{run, index + i}; })) {
        // Full: this block is dropped; the next one that fits starts a run.
        if (!dropped) {
          dropped = true;
          ++run;
          index = 0;
        }
        std::this_thread::yield();
      }
      index += block;
    }
    writer_runs.store(run, std::memory_order_release);
    done.store(true, std::memory_order_release);
  });

  std::uint64_t starts = 0;
  bool started = false;
  Frame last{};
  std::uint64_t taken = 0;
  const auto read = [&] {
    queue.read(
        [&] {
          ++starts;
          started = true;
        },
        [&](const Frame& frame) {
          if (started) {
            require(frame.index == 0, "a run starts at its first frame");
            require(frame.run > last.run || taken == 0, "a new run is a later one");
            started = false;
          } else if (taken > 0) {
            require(frame.run == last.run && frame.index == last.index + 1,
                    "frames arrive in order within a run");
          }
          last = frame;
          ++taken;
        });
  };
  while (!done.load(std::memory_order_acquire)) {
    read();
  }
  read();
  producer.join();
  require(taken > 0, "frames arrived");
  // Two runs begun with no frame between them read as one.
  require(starts > 0 && starts <= writer_runs.load(std::memory_order_acquire),
          "the reader starts at most once for each run the writer began");
}

void a_drop_then_a_fit_starts_one_run() {
  CaptureQueue<Frame> queue(capacity);
  std::uint64_t starts = 0;
  std::vector<Frame> got;
  const auto fill = [](std::uint64_t run) {
    return [run](std::size_t i) { return Frame{run, i}; };
  };
  for (std::size_t written = 0; written < capacity; written += block) {
    require(queue.write(block, fill(0)), "fits until full");
  }
  require(!queue.write(block, fill(1)), "a full queue drops");
  require(!queue.write(block, fill(1)), "and keeps dropping");
  queue.read([&] { ++starts; }, [&](const Frame& frame) { got.push_back(frame); });
  require(starts == 0 && got.size() == capacity, "what fitted is read, in one run");
  require(queue.write(block, fill(2)), "room again");
  got.clear();
  queue.read([&] { ++starts; }, [&](const Frame& frame) { got.push_back(frame); });
  require(starts == 1, "the first write after the drop starts one new run");
  require(got.size() == block && got.front().run == 2, "and only its frames follow");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native capture queue contract passed",
      {
          {"two_threads_keep_order_and_runs", [] { two_threads_keep_order_and_runs(); }},
          {"a_drop_then_a_fit_starts_one_run", [] { a_drop_then_a_fit_starts_one_run(); }},
      });
}
