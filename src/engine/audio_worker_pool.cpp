#include "oscillate/engine/audio_worker_pool.hpp"

#include "oscillate/engine/denormals.hpp"
#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <bit>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace oscillate::engine {
namespace {

constexpr std::uint64_t field_mask = (std::uint64_t{1} << 20) - 1;
constexpr std::uint32_t batch_mask = (std::uint32_t{1} << 24) - 1;
// About 10–40 us: long enough to catch the next level of the same block
// without a futex round trip, short enough not to hold a core between blocks.
constexpr int worker_spin = 128;
constexpr int caller_spin = 4096;

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#endif
}

constexpr std::uint64_t pack(std::uint32_t batch, std::size_t count, std::size_t next) noexcept {
  return (std::uint64_t{batch} << 40) | (std::uint64_t{count} << 20) | std::uint64_t{next};
}

} // namespace

AudioWorkerPool::AudioWorkerPool(std::size_t wanted_workers) noexcept
    : wanted_(std::min(wanted_workers, max_workers)) {}

AudioWorkerPool::~AudioWorkerPool() { stop_owned_threads(); }

std::size_t AudioWorkerPool::default_worker_count() noexcept {
  const auto threads = static_cast<std::size_t>(std::thread::hardware_concurrency());
  const auto cores = std::max<std::size_t>(1, threads / 2);
  return std::min(cores - 1, max_workers);
}

void AudioWorkerPool::request_stop() noexcept {
  stop_.store(true, std::memory_order_release);
  wake_.fetch_add(1, std::memory_order_release);
  wake_.notify_all();
}

void AudioWorkerPool::reset_after_join() noexcept { stop_.store(false, std::memory_order_release); }

void AudioWorkerPool::start_owned_threads(std::size_t count) {
  // Only free worker places: a thread beyond max_workers would return at once.
  const auto before = std::min(attached_.load(std::memory_order_acquire), max_workers);
  count = std::min(count, max_workers - before);
  for (std::size_t index = 0; index < count; ++index)
    owned_.emplace_back([this] { run_worker(); });
  // A stop during startup sends workers home: stop waiting for them then;
  // stop_owned_threads() still joins them.
  while (attached_.load(std::memory_order_acquire) < std::min(before + count, max_workers) &&
         !stop_.load(std::memory_order_acquire))
    std::this_thread::yield();
}

void AudioWorkerPool::stop_owned_threads() noexcept {
  if (owned_.empty())
    return;
  request_stop();
  for (auto& thread : owned_)
    thread.join();
  owned_.clear();
  reset_after_join();
}

std::optional<std::size_t> AudioWorkerPool::claim(Batch& batch, std::uint64_t& word) noexcept {
  const auto number = word >> 40;
  for (;;) {
    // A newer batch has its own task, count and lanes; the caller must
    // observe it afresh.
    if ((word >> 40) != number)
      return std::nullopt;
    const auto count = static_cast<std::size_t>((word >> 20) & field_mask);
    const auto next = static_cast<std::size_t>(word & field_mask);
    if (next >= count)
      return std::nullopt;
    if (batch.claim.compare_exchange_weak(word, word + 1, std::memory_order_acquire,
                                          std::memory_order_acquire))
      return next;
  }
}

void AudioWorkerPool::work(Batch& batch, std::uint64_t observed) noexcept {
  auto index = claim(batch, observed);
  if (!index)
    return;
  // A successful claim belongs to an unfinished batch, so its task and
  // context cannot change until this thread reports the task done.
  const auto task = batch.task.load(std::memory_order_relaxed);
  auto* const context = batch.context.load(std::memory_order_relaxed);
  const auto count = static_cast<std::size_t>((observed >> 20) & field_mask);
  const auto lane = batch.lanes.fetch_add(1, std::memory_order_relaxed);
  do {
    task(context, *index, lane);
    if (batch.done.fetch_add(1, std::memory_order_acq_rel) + 1 == count)
      batch.done.notify_one();
    index = claim(batch, observed);
  } while (index);
}

void AudioWorkerPool::work_any() noexcept {
  for (auto& batch : batches_)
    work(batch, batch.claim.load(std::memory_order_acquire));
}

void AudioWorkerPool::work_nested() noexcept {
  for (std::size_t slot = 1; slot < batches_.size(); ++slot)
    work(batches_[slot], batches_[slot].claim.load(std::memory_order_acquire));
}

void AudioWorkerPool::run_worker() noexcept {
  // At most max_workers attach, whoever started them, so a batch's lanes stay
  // below max_lanes; a surplus thread returns at once.
  auto attached = attached_.load(std::memory_order_acquire);
  do {
    if (attached >= max_workers)
      return;
  } while (!attached_.compare_exchange_weak(attached, attached + 1, std::memory_order_acq_rel,
                                            std::memory_order_acquire));
  foundation::trace::name_thread("audio worker");
  const foundation::RealtimeThreadScope realtime; // Logging from here asserts in debug.
  const DenormalsFlushed flushed;                 // For the worker's life.
  auto seen = wake_.load(std::memory_order_acquire);
  while (!stop_.load(std::memory_order_acquire)) {
    work_any();
    for (int spin = 0; spin < worker_spin && wake_.load(std::memory_order_acquire) == seen &&
                       !stop_.load(std::memory_order_acquire);
         ++spin)
      cpu_relax();
    while (wake_.load(std::memory_order_acquire) == seen && !stop_.load(std::memory_order_acquire))
      wake_.wait(seen, std::memory_order_acquire);
    seen = wake_.load(std::memory_order_acquire);
  }
  attached_.fetch_sub(1, std::memory_order_acq_rel);
}

void AudioWorkerPool::publish(Batch& batch, std::size_t count, Task task, void* context) noexcept {
  batch.task.store(task, std::memory_order_relaxed);
  batch.context.store(context, std::memory_order_relaxed);
  batch.done.store(0, std::memory_order_relaxed);
  batch.lanes.store(0, std::memory_order_relaxed);
  batch.number = (batch.number + 1) & batch_mask;
  batch.claim.store(pack(batch.number, count, 0), std::memory_order_release);
  wake_.fetch_add(1, std::memory_order_release);
  // Wake only the helpers this batch can use; a spinning worker sees the
  // new batch without a wake, and one with nothing to claim sleeps again.
  const auto helpers = std::min(count - 1, attached_.load(std::memory_order_relaxed));
  for (std::size_t helper = 0; helper < helpers; ++helper)
    wake_.notify_one();
}

void AudioWorkerPool::finish(Batch& batch, std::size_t count) noexcept {
  work(batch, batch.claim.load(std::memory_order_acquire));
  // The caller's own share is done: the rest is waiting for helpers.
  OSCILLATE_TRACE_SPAN("pool.wait_for_helpers", count);
  // Meanwhile it helps with nested batches (never the outer one: a whole
  // outer task would hold up this batch's caller).
  for (int spin = 0; spin < caller_spin; ++spin) {
    if (batch.done.load(std::memory_order_acquire) == count)
      return;
    work_nested();
    cpu_relax();
  }
  // Never spin unbounded on a real-time thread: RTKit kills a process whose
  // real-time threads run too long without sleeping.
  for (;;) {
    const auto finished = batch.done.load(std::memory_order_acquire);
    if (finished == count)
      return;
    batch.done.wait(finished, std::memory_order_acquire);
  }
}

void AudioWorkerPool::run(std::size_t count, Task task, void* context) noexcept {
  if (count == 0 || task == nullptr)
    return;
  if (count == 1 || count > max_tasks || attached_.load(std::memory_order_acquire) == 0) {
    for (std::size_t index = 0; index < count; ++index)
      task(context, index, 0);
    return;
  }
  auto& batch = batches_[outer];
  publish(batch, count, task, context);
  finish(batch, count);
}

void AudioWorkerPool::run_nested(std::size_t count, Task task, void* context) noexcept {
  if (count == 0 || task == nullptr)
    return;
  std::size_t slot = nested_slots;
  if (count > 1 && count <= max_tasks && attached_.load(std::memory_order_acquire) != 0) {
    auto busy = nested_busy_.load(std::memory_order_relaxed);
    for (;;) {
      slot = static_cast<std::size_t>(std::countr_one(busy));
      if (slot >= nested_slots)
        break;
      // Acquire: the slot's last dispatcher released it after its batch
      // finished, so its words are this thread's to rewrite.
      if (nested_busy_.compare_exchange_weak(busy, busy | (std::uint32_t{1} << slot),
                                             std::memory_order_acquire, std::memory_order_relaxed))
        break;
    }
  }
  if (slot >= nested_slots) {
    for (std::size_t index = 0; index < count; ++index)
      task(context, index, 0);
    return;
  }
  auto& batch = batches_[1 + slot];
  publish(batch, count, task, context);
  finish(batch, count);
  nested_busy_.fetch_and(~(std::uint32_t{1} << slot), std::memory_order_release);
}

} // namespace oscillate::engine
