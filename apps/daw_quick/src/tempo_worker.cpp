#include "tempo_worker.hpp"

#include "oscillate/application/source_analysis_cache_directory.hpp"
#include "oscillate/persistence/store.hpp"

#include <utility>

namespace oscillate::quick {
namespace {
template <typename T> using Result = foundation::Result<T>;

Result<application::TempoCacheResult>
analyze_tempo(const std::filesystem::path& root, const project::SourceRecord& source,
              std::uint64_t generation, const std::function<bool()>& cancelled,
              bool& cache_degraded, const std::stop_token& stop) {
  auto store = persistence::ProjectStore::open_reader(root);
  if (!store) {
    return Result<application::TempoCacheResult>::failure(store.error());
  }
  std::optional<media::CacheStore> cache;
  std::string cache_warning;
  auto opened = application::open_user_source_analysis_cache();
  if (opened) {
    cache.emplace(std::move(opened.value()));
  } else {
    cache_degraded = true;
    cache_warning = opened.error().message;
  }
  auto result = application::load_tempo_suggestions(store.value(), cache ? &*cache : nullptr,
                                                    source, generation, cancelled, stop);
  if (result) {
    if (!result.value().cache_warning.empty()) {
      cache_degraded = true;
      if (!cache_warning.empty())
        cache_warning += "; ";
      cache_warning += result.value().cache_warning;
    }
  }
  if (cache && (!result || !result.value().cache_hit)) {
    auto enforced = application::enforce_source_analysis_cache_budget(*cache);
    if (!enforced) {
      cache_degraded = true;
      if (!cache_warning.empty())
        cache_warning += "; ";
      cache_warning += enforced.error().message;
    }
  }
  if (result)
    result.value().cache_warning = std::move(cache_warning);
  return result;
}
} // namespace

TempoWorker::TempoWorker(Analyzer analyzer)
    : worker_([analyzer = std::move(analyzer)](
                  const std::filesystem::path& root, const project::SourceRecord& source,
                  std::uint64_t generation, const std::function<bool()>& cancelled,
                  bool& cache_degraded, const std::stop_token& stop) {
        if (analyzer) {
          return analyzer(root, source, generation, cancelled);
        }
        return analyze_tempo(root, source, generation, cancelled, cache_degraded, stop);
      }) {}

TempoWorker::~TempoWorker() = default;

void TempoWorker::submit(std::filesystem::path root, std::optional<project::SourceRecord> source,
                         std::uint64_t generation) {
  worker_.submit(std::move(root), std::move(source), generation);
}

std::optional<TempoWorker::Outcome> TempoWorker::collect() { return worker_.collect(); }

} // namespace oscillate::quick
