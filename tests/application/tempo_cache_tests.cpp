#include "oscillate/application/tempo_cache.hpp"
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
  Directory() : TempDir("oscillate-tempo-cache") {}
};

void verified_source_reopen_corruption_and_cancel() {
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
    auto first = take(application::load_tempo_suggestions(store, &cache, source, 1));
    REQUIRE_TRUE(!first.cache_hit, "first request must analyze and publish tempo suggestions");
    REQUIRE_TRUE(first.cache_warning.empty(),
                 "first request must analyze and publish tempo suggestions");
    REQUIRE_EQ(first.estimate.frames, source.frames,
               "first request must analyze and publish tempo suggestions");
    key = first.estimate.identity.source_key;
    REQUIRE_EQ(key.size(), 64, "tempo cache key or request generation missing");
    REQUIRE_EQ(first.estimate.identity.generation, 1,
               "tempo cache key or request generation missing");
  }
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto reopened = take(application::load_tempo_suggestions(store, &cache, source, 2));
    REQUIRE_TRUE(reopened.cache_hit,
                 "reopen failed to reuse semantic tempo result with new generation");
    REQUIRE_EQ(reopened.estimate.identity.source_key, key,
               "reopen failed to reuse semantic tempo result with new generation");
    REQUIRE_EQ(reopened.estimate.identity.generation, 2,
               "reopen failed to reuse semantic tempo result with new generation");
    REQUIRE_EQ(reopened.estimate.frames, source.frames,
               "reopen failed to reuse semantic tempo result with new generation");
    require(!application::load_tempo_suggestions(store, &cache, source, 3, [] { return true; }),
            "cancelled tempo request returned cached data");
  }
  {
    std::ofstream corrupt(root / "cache" / (key + ".cache"), std::ios::binary | std::ios::trunc);
    corrupt << "corrupt";
  }
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto rebuilt = take(application::load_tempo_suggestions(store, &cache, source, 4));
    REQUIRE_TRUE(!rebuilt.cache_hit, "corrupt disposable tempo entry was not rebuilt");
    REQUIRE_EQ(rebuilt.estimate.identity.generation, 4,
               "corrupt disposable tempo entry was not rebuilt");
    auto again = take(application::load_tempo_suggestions(store, &cache, source, 5));
    require(again.cache_hit, "rebuilt tempo entry did not become reusable");
  }
  {
    auto failed_cache = take(media::CacheStore::adopt_directory_fd(
        take(store.open_cache_directory()), [](media::CacheWriteStage stage) {
          return stage == media::CacheWriteStage::temporary_created;
        }));
    REQUIRE_OK(failed_cache.clear(), "cannot clear tempo fixture cache");
    auto fallback = take(application::load_tempo_suggestions(store, &failed_cache, source, 6));
    REQUIRE_TRUE(!fallback.cache_hit, "cache write failure hid a usable tempo result");
    REQUIRE_TRUE(!fallback.cache_warning.empty(), "cache write failure hid a usable tempo result");
    REQUIRE_EQ(fallback.estimate.frames, source.frames,
               "cache write failure hid a usable tempo result");
    auto without_cache = take(application::load_tempo_suggestions(store, nullptr, source, 7));
    REQUIRE_TRUE(!without_cache.cache_hit, "tempo analysis incorrectly requires a cache");
    REQUIRE_TRUE(without_cache.cache_warning.empty(),
                 "tempo analysis incorrectly requires a cache");
    REQUIRE_EQ(without_cache.estimate.frames, source.frames,
               "tempo analysis incorrectly requires a cache");
  }
}
} // namespace

int main() {
  try {
    verified_source_reopen_corruption_and_cancel();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
