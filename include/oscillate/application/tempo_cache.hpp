#pragma once

#include "oscillate/analysis/tempo.hpp"
#include "oscillate/media/cache_store.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

namespace oscillate::persistence {
class ProjectStore;
}

namespace oscillate::application {

struct TempoCacheResult {
  analysis::TempoEstimate estimate;
  bool cache_hit = false;
  std::string cache_warning;
};

// The semantic analyzer key includes version and options; callers use this
// when verifying that a worker result belongs to the current source analysis.
[[nodiscard]] foundation::Result<media::CacheKey>
tempo_cache_key(const project::SourceRecord& source);

// Worker-only. Verifies the source before reading a disposable result.
// The consumer must still check source identity and request generation before
// installing suggestions in a project or UI. This never changes the tempo map.
[[nodiscard]] foundation::Result<TempoCacheResult>
load_tempo_suggestions(persistence::ProjectStore& store, media::CacheStore* cache,
                       const project::SourceRecord& source, std::uint64_t generation,
                       const std::function<bool()>& cancelled = {},
                       const std::stop_token& stop = {});

} // namespace oscillate::application
