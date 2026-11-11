#include "analysis_worker.hpp"

#include "oscillate/engine/denormals.hpp"
#include "oscillate/foundation/trace.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::effects {

void AnalysisSlot::analyse() {
  if (kernel != nullptr) {
    const engine::DenormalsFlushed flushed;
    kernel->analyse();
  }
}

namespace analysis_worker {
namespace {

// Often enough that a capture (two seconds of room) never fills while the
// worker keeps up; seldom enough that waking costs nothing measurable.
constexpr auto drain_interval = std::chrono::milliseconds{20};

class Worker {
public:
  void add(std::shared_ptr<AnalysisSlot> slot) {
    const std::lock_guard lock(mutex_);
    slots_.push_back(std::move(slot));
    if (!thread_.joinable()) {
      thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
    }
    wake_.notify_one();
  }

  void remove(const AnalysisSlot* slot) {
    const std::lock_guard lock(mutex_);
    std::erase_if(slots_, [slot](const auto& held) { return held.get() == slot; });
  }

private:
  void run(const std::stop_token& stop) {
    foundation::trace::name_thread("effect analysis");
    std::vector<std::shared_ptr<AnalysisSlot>> batch;
    while (!stop.stop_requested()) {
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, stop, [this] { return !slots_.empty(); });
        (void)wake_.wait_for(lock, stop, drain_interval, [] { return false; });
        batch = slots_;
      }
      for (const auto& slot : batch) {
        const std::lock_guard lock(slot->mutex);
        slot->analyse();
      }
      batch.clear();
    }
  }

  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::vector<std::shared_ptr<AnalysisSlot>> slots_;
  std::jthread thread_;
};

// Never destroyed, so a runtime that outlives static destruction can still
// remove itself; the thread ends with the process.
Worker& worker() {
  static auto* const instance = new Worker();
  return *instance;
}

} // namespace

void add(std::shared_ptr<AnalysisSlot> slot) { worker().add(std::move(slot)); }

void remove(const AnalysisSlot* slot) { worker().remove(slot); }

} // namespace analysis_worker

} // namespace oscillate::native_dsp::effects
