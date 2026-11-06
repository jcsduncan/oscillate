#include "oscillate/application/waveform_cache.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-waveform-cache") {}
};

void cache_reopen_corruption_and_cancel() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  const auto source_id = take(project::SourceId::from_bits({0, 3}));
  const int fd = ::open(OSCILLATE_STARTER_PLUCK, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  REQUIRE_GE(fd, 0, "starter fixture missing");
  auto staged = store.stage_wav(source_id, fd);
  ::close(fd);
  const auto source = take(std::move(staged)).record();
  std::string key;
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto first = take(application::load_waveform_summary(store, &cache, source, 1));
    REQUIRE_TRUE(!first.cache_hit, "first request must analyze and publish waveform");
    REQUIRE_TRUE(first.cache_warning.empty(), "first request must analyze and publish waveform");
    REQUIRE_TRUE(!first.summary.levels.empty(), "first request must analyze and publish waveform");
    key = first.summary.identity.source_key;
    REQUIRE_EQ(key.size(), 64, "cache key or request generation missing");
    REQUIRE_EQ(first.summary.identity.generation, 1, "cache key or request generation missing");
  }
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto reopened = take(application::load_waveform_summary(store, &cache, source, 2));
    REQUIRE_TRUE(reopened.cache_hit, "reopen failed to reuse semantic result with new generation");
    REQUIRE_EQ(reopened.summary.identity.source_key, key,
               "reopen failed to reuse semantic result with new generation");
    REQUIRE_EQ(reopened.summary.identity.generation, 2,
               "reopen failed to reuse semantic result with new generation");
    REQUIRE_GT(reopened.summary.levels.front().buckets.size(), 1,
               "reopen failed to reuse semantic result with new generation");
  }
  {
    std::ofstream corrupt(root / "cache" / (key + ".cache"), std::ios::binary | std::ios::trunc);
    corrupt << "corrupt";
  }
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto rebuilt = take(application::load_waveform_summary(store, &cache, source, 3));
    REQUIRE_TRUE(!rebuilt.cache_hit, "corrupt disposable entry was not rebuilt");
    REQUIRE_EQ(rebuilt.summary.identity.generation, 3, "corrupt disposable entry was not rebuilt");
    auto again = take(application::load_waveform_summary(store, &cache, source, 4));
    require(again.cache_hit, "rebuilt entry did not become reusable");
    auto forced = take(application::load_waveform_summary(
        store, &cache, source, 5, {}, {}, application::WaveformCachePolicy::rebuild));
    REQUIRE_TRUE(!forced.cache_hit, "explicit waveform rebuild reused the cached summary");
    REQUIRE_EQ(forced.summary.identity.generation, 5,
               "explicit waveform rebuild reused the cached summary");
    require(take(application::load_waveform_summary(store, &cache, source, 6)).cache_hit,
            "explicit waveform rebuild was not saved for later use");
    auto cancelled =
        application::load_waveform_summary(store, &cache, source, 7, [] { return true; });
    REQUIRE_TRUE(!cancelled, "cancelled request must not publish cached data");
    REQUIRE_EQ(cancelled.error().code, foundation::ErrorCode::cancelled,
               "cancelled request must not publish cached data");
  }
  {
    auto failed_cache = take(media::CacheStore::adopt_directory_fd(
        take(store.open_cache_directory()), [](media::CacheWriteStage stage) {
          return stage == media::CacheWriteStage::temporary_created;
        }));
    REQUIRE_OK(failed_cache.clear(), "cannot clear fixture cache");
    auto fallback = take(application::load_waveform_summary(store, &failed_cache, source, 8));
    REQUIRE_TRUE(!fallback.cache_hit,
                 "cache publication failure must preserve preview and report degradation");
    REQUIRE_TRUE(!fallback.cache_warning.empty(),
                 "cache publication failure must preserve preview and report degradation");
    REQUIRE_TRUE(!fallback.summary.levels.empty(),
                 "cache publication failure must preserve preview and report degradation");
  }
}

} // namespace

int main() {
  try {
    cache_reopen_corruption_and_cancel();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
