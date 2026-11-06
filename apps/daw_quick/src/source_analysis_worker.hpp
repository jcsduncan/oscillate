#pragma once

#include "oscillate/project/model.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace oscillate::quick {

// One latest-selection worker. A blocked reader never joins the UI thread;
// cancellation and the mailbox survive destruction of the facade.
template <typename Value> class SourceAnalysisWorker {
public:
  using Analyzer = std::function<foundation::Result<Value>(
      const std::filesystem::path&, const project::SourceRecord&, std::uint64_t,
      const std::function<bool()>&, bool&, const std::stop_token&)>;
  struct Outcome {
    std::uint64_t generation;
    project::SourceRecord source;
    foundation::Result<Value> result;
    bool cache_degraded = false;
  };

  explicit SourceAnalysisWorker(Analyzer analyzer) {
    thread_ = std::jthread([shared = shared_,
                            analyzer = std::move(analyzer)](std::stop_token stopped) {
      while (!stopped.stop_requested()) {
        Request request;
        {
          std::unique_lock lock(shared->mutex);
          if (!shared->changed.wait(lock, stopped, [&] { return shared->pending.has_value(); })) {
            break;
          }
          request = std::move(*shared->pending);
          shared->pending.reset();
        }
        const auto cancelled = [&] {
          return stopped.stop_requested() || request.stop.stop_requested() ||
                 shared->generation.load(std::memory_order_acquire) != request.generation;
        };
        if (cancelled() || !request.source) {
          continue;
        }
        bool cache_degraded = false;
        foundation::Result<Value> result = foundation::Result<Value>::failure(
            {foundation::ErrorCode::unavailable, "Managed audio unavailable"});
        try {
          result = analyzer(request.root, *request.source, request.generation, cancelled,
                            cache_degraded, request.stop);
        } catch (const std::exception& error) {
          result = foundation::Result<Value>::failure(
              {foundation::ErrorCode::internal, std::string("Analysis failed: ") + error.what()});
        } catch (...) {
          result = foundation::Result<Value>::failure(
              {foundation::ErrorCode::internal, "Analysis failed"});
        }
        if (cancelled()) {
          continue;
        }
        std::lock_guard lock(shared->mutex);
        shared->ready.emplace(Outcome{request.generation, std::move(*request.source),
                                      std::move(result), cache_degraded});
      }
    });
  }
  SourceAnalysisWorker(const SourceAnalysisWorker&) = delete;
  SourceAnalysisWorker& operator=(const SourceAnalysisWorker&) = delete;
  ~SourceAnalysisWorker() {
    thread_.request_stop();
    shared_->stop.request_stop();
    shared_->changed.notify_all();
    thread_.detach();
  }

  void submit(std::filesystem::path root, std::optional<project::SourceRecord> source,
              std::uint64_t generation) {
    {
      std::lock_guard lock(shared_->mutex);
      shared_->stop.request_stop();
      shared_->stop = std::stop_source{};
      shared_->generation.store(generation, std::memory_order_release);
      shared_->pending =
          Request{std::move(root), std::move(source), generation, shared_->stop.get_token()};
      shared_->ready.reset();
    }
    shared_->changed.notify_one();
  }

  [[nodiscard]] std::optional<Outcome> collect() {
    std::lock_guard lock(shared_->mutex);
    auto outcome = std::move(shared_->ready);
    shared_->ready.reset();
    return outcome;
  }

private:
  struct Request {
    std::filesystem::path root;
    std::optional<project::SourceRecord> source;
    std::uint64_t generation;
    std::stop_token stop;
  };
  struct Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<Request> pending;
    std::optional<Outcome> ready;
    std::atomic<std::uint64_t> generation{0};
    std::stop_source stop;
  };
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  std::jthread thread_;
};

} // namespace oscillate::quick
