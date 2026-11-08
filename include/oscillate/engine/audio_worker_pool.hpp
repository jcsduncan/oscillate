#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace oscillate::engine {

// Real-time helpers for the one audio callback thread (ADR-0048). The
// callback hands a batch of independent tasks to run(); attached workers and
// the callback itself claim tasks until all have finished. A task must touch
// only state that no other task in the same batch touches, and must not
// allocate, lock or block.
//
// Workers are threads that call run_worker(). The audio output starts them
// with the callback's real-time priority; export and tests may use
// start_owned_threads(). With no attached worker, run() executes inline, in
// task order, so results never depend on how many workers took part.
//
// Synchronisation: a batch is published by one release store of a packed
// claim word (batch number, task count, next task). A worker claims a task
// with an acquire compare-and-swap on that word, so it sees every write the
// callback made before publishing, and can never claim a task of a batch it
// did not observe. Each finished task is counted with a release increment;
// run() returns only after an acquire load sees every task finished, so all
// task writes are visible to the callback and to whichever thread touches the
// same state in the next batch.
//
// Nested batches (ADR-0085): a task may itself hand out a batch with
// run_nested(), such as one heavy instrument's voices. It goes into one of
// nested_slots slots of its own, with the same claim word and counting;
// workers with nothing left of the outer batch claim from it, and the
// dispatching task works on it too. With no free slot or no attached worker
// it runs inline, in task order.
class AudioWorkerPool final {
public:
  // A task receives its index in [0, count) and a lane in [0, count) that no
  // other thread uses during the same batch, for per-thread scratch.
  using Task = void (*)(void* context, std::size_t task, std::size_t lane) noexcept;

  static constexpr std::size_t max_workers = 15;
  static constexpr std::size_t max_tasks = (std::size_t{1} << 20) - 1;
  // The most threads that can take part in one batch: every worker and the
  // dispatching thread. Lanes are below this as well as below the count.
  static constexpr std::size_t max_lanes = max_workers + 1;
  // Nested batches that can run at once, each from a different task.
  static constexpr std::size_t nested_slots = 4;

  explicit AudioWorkerPool(std::size_t wanted_workers) noexcept;
  AudioWorkerPool(const AudioWorkerPool&) = delete;
  AudioWorkerPool& operator=(const AudioWorkerPool&) = delete;
  // Every worker must have returned from run_worker() and been joined.
  ~AudioWorkerPool();

  // The default worker count for live playback on this machine: one fewer
  // than the physical cores, assuming two hardware threads per core, capped.
  [[nodiscard]] static std::size_t default_worker_count() noexcept;

  [[nodiscard]] std::size_t wanted_workers() const noexcept { return wanted_; }
  [[nodiscard]] std::size_t attached_workers() const noexcept {
    return attached_.load(std::memory_order_acquire);
  }

  // Worker thread entry. Serves batches until request_stop(); a task already
  // claimed finishes first. Returns at once while a stop is pending, or when
  // max_workers threads are already attached.
  void run_worker() noexcept;
  // Control side. Wakes and releases every worker; the caller then joins
  // them. run() keeps working inline. Call reset_after_join() before reuse.
  void request_stop() noexcept;
  void reset_after_join() noexcept;

  // Control side, for offline rendering and tests: ordinary threads.
  void start_owned_threads(std::size_t count);
  void stop_owned_threads() noexcept;

  // Callback only: one dispatching thread at a time. Runs every task once and
  // returns after all have finished.
  void run(std::size_t count, Task task, void* context) noexcept;
  // Callback side, from inside a task of run() (or the dispatching thread):
  // runs every task once, helped by any worker that is free, and returns
  // after all have finished. Tasks must not depend on which thread runs
  // them; with no free slot or no attached worker they run inline, in
  // order, on lane 0.
  void run_nested(std::size_t count, Task task, void* context) noexcept;

private:
  // One published batch: the outer one, or a nested slot's.
  struct Batch {
    // batch(24 bits) | count(20) | next(20)
    std::atomic<std::uint64_t> claim{0};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> lanes{0};
    std::atomic<Task> task{nullptr};
    std::atomic<void*> context{nullptr};
    std::uint32_t number = 0; // Written only by the batch's dispatcher.
  };
  static constexpr std::size_t outer = 0;

  // Publishes `count` tasks on `batch` and wakes the helpers it can use.
  void publish(Batch& batch, std::size_t count, Task task, void* context) noexcept;
  // The dispatcher's share, then a bounded spin and a futex wait for the rest.
  void finish(Batch& batch, std::size_t count) noexcept;
  void work(Batch& batch, std::uint64_t observed) noexcept;
  // Every batch a free thread can help with: the outer one, then each
  // nested slot.
  void work_any() noexcept;
  // Only the nested slots: what a waiting dispatcher helps with.
  void work_nested() noexcept;
  // Claims the next task of the batch in `word`, never one of a later batch:
  // its index, or nothing when that batch has none left.
  static std::optional<std::size_t> claim(Batch& batch, std::uint64_t& word) noexcept;

  std::size_t wanted_;
  std::atomic<std::size_t> attached_{0};
  std::atomic<bool> stop_{false};
  // Futex word: bumped once per batch, nested ones included, and on stop.
  std::atomic<std::uint32_t> wake_{0};
  // The outer batch, then the nested slots.
  std::array<Batch, 1 + nested_slots> batches_{};
  // Bit s set: nested slot s is taken.
  std::atomic<std::uint32_t> nested_busy_{0};
  std::vector<std::thread> owned_;
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
};

} // namespace oscillate::engine
