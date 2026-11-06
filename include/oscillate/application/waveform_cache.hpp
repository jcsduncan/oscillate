#pragma once

#include "oscillate/analysis/waveform.hpp"
#include "oscillate/media/cache_store.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

namespace oscillate::persistence {
class ProjectStore;
}

namespace oscillate::application {

struct WaveformCacheResult {
  analysis::WaveformSummary summary;
  bool cache_hit = false;
  // A disposable cache failure does not hide a usable waveform.
  std::string cache_warning;
};

enum class WaveformCachePolicy : std::uint8_t { reuse, rebuild };

// Worker-only. Verifies source bytes before using a semantic cache hit.
// A missing or invalid entry is rebuilt; caller still checks its generation
// before publishing the result to the UI.
[[nodiscard]] foundation::Result<WaveformCacheResult>
load_waveform_summary(persistence::ProjectStore& store, media::CacheStore* cache,
                      const project::SourceRecord& source, std::uint64_t generation,
                      const std::function<bool()>& cancelled = {}, const std::stop_token& stop = {},
                      WaveformCachePolicy policy = WaveformCachePolicy::reuse);

} // namespace oscillate::application
