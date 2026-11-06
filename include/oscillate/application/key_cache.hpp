#pragma once

#include "oscillate/analysis/key.hpp"
#include "oscillate/media/cache_store.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

namespace oscillate::persistence {
class ProjectStore;
}

namespace oscillate::application {

struct KeyCacheResult {
  analysis::KeyEstimate estimate;
  bool cache_hit = false;
  std::string cache_warning;
};

// Stable semantic key for immutable encoded source bytes and this estimator's
// version/settings. The caller must rederive it when installing an async result.
[[nodiscard]] foundation::Result<media::CacheKey>
key_cache_key(const project::SourceRecord& source);

// Worker-only. Verifies media before either a cache hit or analysis.
// The consumer still validates the current semantic key, generation and
// cancellation ticket before publishing the result. No project state changes.
[[nodiscard]] foundation::Result<KeyCacheResult>
load_key_suggestion(persistence::ProjectStore& store, media::CacheStore* cache,
                    const project::SourceRecord& source, std::uint64_t generation,
                    const std::function<bool()>& cancelled = {}, const std::stop_token& stop = {});

} // namespace oscillate::application
