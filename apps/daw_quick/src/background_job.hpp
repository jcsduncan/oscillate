#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// One piece of work on its own thread whose result the UI thread collects
// later (from a poll timer): the completion flag, the result slot, catching a
// worker's exception, joining, and the generation that says whether the
// result still applies. Controllers keep the inputs that identify a result
// (a revision, a project) beside the job and check them when they take it.
// Nothing here waits for running work on the calling thread except wait()
// and destruction: a job given up (abandon(), or start() over a running one)
// finishes on its own thread and is joined once it has.
namespace oscillate::quick {

template <typename Result> class BackgroundJob {
public:
  struct Finished {
    Result result;
    // The generation the job started in: a result from an older one no
    // longer applies (AGENTS.md: results install only when inputs match).
    std::uint64_t generation = 0;
  };

  BackgroundJob() = default;
  BackgroundJob(const BackgroundJob&) = delete;
  BackgroundJob& operator=(const BackgroundJob&) = delete;
  // Running and abandoned threads are joined on destruction (std::jthread).
  ~BackgroundJob() = default;

  // Runs `work()` (returning Result) on a new thread. A worker exception
  // becomes `failure(what)` (empty for an exception of unknown type).
  // Returns the reason when the thread cannot start; the job is then idle.
  // Work still running from an earlier start is abandoned, not waited for.
  template <typename Work, typename Failure>
  [[nodiscard]] std::optional<std::string> start(std::uint64_t generation, Work work,
                                                 Failure failure) {
    abandon();
    auto state = std::make_shared<State>();
    try {
      worker_ = std::jthread([state, work = std::move(work), failure]() mutable {
        try {
          state->result.emplace(work());
        } catch (const std::exception& error) {
          report(*state, failure, std::string(error.what()));
        } catch (...) {
          report(*state, failure, std::string{});
        }
        state->done.store(true, std::memory_order_release);
      });
    } catch (const std::exception& error) {
      return std::string(error.what());
    }
    state_ = std::move(state);
    generation_ = generation;
    return std::nullopt;
  }

  // Started and not yet taken.
  [[nodiscard]] bool running() const noexcept { return state_ != nullptr; }
  // The work has finished and its result is ready to take.
  [[nodiscard]] bool finished() const noexcept {
    return state_ != nullptr && state_->done.load(std::memory_order_acquire);
  }
  // Blocks until the work finishes (for a caller that must run after it).
  void wait() {
    if (worker_.joinable())
      worker_.join();
  }
  // Gives the running work up without waiting for it: its result is
  // dropped and the job is idle at once. The caller asks the work to stop
  // through its own flag first; the thread is joined once it has finished.
  void abandon() {
    reap();
    if (!state_)
      return;
    retired_.push_back({std::exchange(state_, {}), std::move(worker_)});
  }
  // Abandoned work that has not finished yet.
  [[nodiscard]] std::size_t abandoned() {
    reap();
    return retired_.size();
  }
  // The result once finished, leaving the job idle; nothing while it runs
  // (or when the failure callback itself threw).
  [[nodiscard]] std::optional<Finished> take() {
    if (!finished())
      return std::nullopt;
    wait();
    reap();
    auto state = std::exchange(state_, {});
    if (!state->result)
      return std::nullopt; // The failure callback threw: the job is idle with no result.
    return Finished{std::move(*state->result), generation_};
  }

private:
  struct State {
    std::atomic<bool> done{false};
    std::optional<Result> result;
  };
  // The failure callback's result for a worker exception. A callback that
  // itself throws leaves no result (taken as a failed job) rather than
  // escaping the thread and terminating the process.
  template <typename Failure>
  static void report(State& state, Failure& failure, std::string what) noexcept {
    try {
      state.result.emplace(failure(std::move(what)));
    } catch (...) {
      state.result.reset();
    }
  }

  struct Retired {
    std::shared_ptr<State> state;
    std::jthread worker;
  };
  // Joins abandoned threads whose work has finished: no wait.
  void reap() {
    std::erase_if(retired_, [](const Retired& retired) {
      return retired.state->done.load(std::memory_order_acquire);
    });
  }

  std::shared_ptr<State> state_;
  std::uint64_t generation_ = 0;
  std::jthread worker_;
  // Declared last: destroyed (joined) before the running worker.
  std::vector<Retired> retired_;
};

} // namespace oscillate::quick
