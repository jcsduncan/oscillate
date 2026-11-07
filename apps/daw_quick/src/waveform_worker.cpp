#include "waveform_worker.hpp"

#include "oscillate/application/source_analysis_cache_directory.hpp"
#include "oscillate/application/waveform_cache.hpp"
#include "oscillate/persistence/store.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace oscillate::quick {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

} // namespace

WaveformWorker::WaveformWorker(Analyzer analyzer) {
  thread_ =
      std::jthread([shared = shared_, analyzer = std::move(analyzer)](std::stop_token stopped) {
        while (!stopped.stop_requested()) {
          Batch batch;
          {
            std::unique_lock lock(shared->mutex);
            if (!shared->changed.wait(lock, stopped, [&] { return shared->pending.has_value(); })) {
              break;
            }
            batch = std::move(*shared->pending);
            shared->pending.reset();
          }
          const auto cancelled = [&] {
            return stopped.stop_requested() || batch.stop.stop_requested() ||
                   shared->generation.load(std::memory_order_acquire) != batch.generation;
          };
          if (cancelled()) {
            continue;
          }
          if (batch.sources.empty()) {
            continue;
          }
          auto store = analyzer ? foundation::Result<persistence::ProjectStore>::failure(
                                      {ErrorCode::unavailable, "Analyzer supplies its own source"})
                                : persistence::ProjectStore::open_reader(batch.root);
          std::optional<media::CacheStore> cache;
          bool cache_degraded = false;
          if (store) {
            auto opened = application::open_user_source_analysis_cache();
            if (opened) {
              cache.emplace(std::move(opened.value()));
            } else {
              cache_degraded = true;
            }
          }
          for (const auto& source : batch.sources) {
            if (cancelled()) {
              break;
            }
            Result<analysis::WaveformSummary> result = Result<analysis::WaveformSummary>::failure(
                {ErrorCode::unavailable, "Audio source unavailable"});
            bool source_cache_degraded = cache_degraded;
            try {
              if (analyzer) {
                result = analyzer(batch.root, source, batch.generation, cancelled);
              } else if (store) {
                auto loaded = application::load_waveform_summary(
                    store.value(), cache ? &*cache : nullptr, source, batch.generation, cancelled,
                    batch.stop,
                    batch.rebuild_source == source.id ? application::WaveformCachePolicy::rebuild
                                                      : application::WaveformCachePolicy::reuse);
                if (loaded) {
                  source_cache_degraded =
                      source_cache_degraded || !loaded.value().cache_warning.empty();
                }
                if (cache && (!loaded || !loaded.value().cache_hit) &&
                    !application::enforce_source_analysis_cache_budget(*cache)) {
                  source_cache_degraded = true;
                }
                result = loaded ? Result<analysis::WaveformSummary>::success(
                                      std::move(loaded.value().summary))
                                : Result<analysis::WaveformSummary>::failure(loaded.error());
              } else {
                result = Result<analysis::WaveformSummary>::failure(store.error());
              }
            } catch (const std::exception& error) {
              result = Result<analysis::WaveformSummary>::failure(
                  {ErrorCode::internal, std::string("Waveform analysis failed: ") + error.what()});
            } catch (...) {
              result = Result<analysis::WaveformSummary>::failure(
                  {ErrorCode::internal, "Waveform analysis failed"});
            }
            if (cancelled()) {
              break;
            }
            std::lock_guard lock(shared->mutex);
            shared->ready.push_back(
                {batch.generation, source, std::move(result), source_cache_degraded});
          }
        }
      });
}

WaveformWorker::~WaveformWorker() {
  thread_.request_stop();
  shared_->stop.request_stop();
  shared_->changed.notify_all();
  // The worker owns its mailbox and all source inputs. A slow integrity check
  // or decoder read may finish after the window closes, without keeping the UI
  // in a synchronous join. Its next cancellation check discards the result.
  thread_.detach();
}

void WaveformWorker::submit(std::filesystem::path root, std::vector<project::SourceRecord> sources,
                            std::uint64_t generation,
                            std::optional<project::SourceId> rebuild_source) {
  {
    std::lock_guard lock(shared_->mutex);
    shared_->stop.request_stop();
    shared_->stop = std::stop_source{};
    shared_->generation.store(generation, std::memory_order_release);
    shared_->pending = Batch{std::move(root), std::move(sources), generation,
                             shared_->stop.get_token(), rebuild_source};
    shared_->ready.clear();
  }
  shared_->changed.notify_one();
}

std::vector<WaveformWorker::Outcome> WaveformWorker::collect() {
  std::lock_guard lock(shared_->mutex);
  std::vector<Outcome> output;
  output.swap(shared_->ready);
  return output;
}

} // namespace oscillate::quick
