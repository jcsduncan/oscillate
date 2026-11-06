#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/cache_store.hpp"

#include <filesystem>

namespace oscillate::application {

// Worker-side source-derived cache. The caller may supply an absolute XDG
// cache home for tests; neither overload may run on the audio or UI thread.
[[nodiscard]] foundation::Result<media::CacheStore> open_user_source_analysis_cache();
[[nodiscard]] foundation::Result<media::CacheStore>
open_user_source_analysis_cache(const std::filesystem::path& cache_home);

// An additional per-store safety limit for source-cache writes. The cache
// controller also counts this store toward the user-wide budget when available.
[[nodiscard]] foundation::Result<void>
enforce_source_analysis_cache_budget(media::CacheStore& cache);

} // namespace oscillate::application
