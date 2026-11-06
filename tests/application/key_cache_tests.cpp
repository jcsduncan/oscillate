#include "oscillate/application/key_cache.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;
using oscillate::test::take;
class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-key-cache") {}
};
project::SourceRecord stage(persistence::ProjectStore& store, project::SourceId id,
                            const fs::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  REQUIRE_GE(fd, 0, "key-cache source fixture missing");
  auto staged = store.stage_wav(id, fd);
  ::close(fd);
  return take(std::move(staged)).record();
}

void verified_cache_lifecycle_and_source_version() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  const auto source =
      stage(store, take(project::SourceId::from_bits({0, 3})), OSCILLATE_STARTER_PLUCK);
  const auto expected_key = take(application::key_cache_key(source)).hex();
  REQUIRE_EQ(expected_key.size(), 64, "key cache identity is not SHA-256 hex");
  std::string key;
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto first = take(application::load_key_suggestion(store, &cache, source, 1));
    REQUIRE_TRUE(!first.cache_hit,
                 "first sparse-source request must return explicit unknown and publish cache");
    REQUIRE_TRUE(first.cache_warning.empty(),
                 "first sparse-source request must return explicit unknown and publish cache");
    REQUIRE_EQ(first.estimate.frames, source.frames,
               "first sparse-source request must return explicit unknown and publish cache");
    REQUIRE_TRUE(!first.estimate.key,
                 "first sparse-source request must return explicit unknown and publish cache");
    REQUIRE_EQ(first.estimate.confidence, 0.0,
               "first sparse-source request must return explicit unknown and publish cache");
    REQUIRE_TRUE(first.estimate.compatible_keys.empty(),
                 "first sparse-source request must return explicit unknown and publish cache");
    key = first.estimate.identity.source_key;
    REQUIRE_EQ(key, expected_key, "key cache used wrong semantic key or generation");
    REQUIRE_EQ(first.estimate.identity.generation, 1,
               "key cache used wrong semantic key or generation");
  }
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto reopened = take(application::load_key_suggestion(store, &cache, source, 2));
    REQUIRE_TRUE(reopened.cache_hit,
                 "reopen failed to reuse unknown key result with current generation");
    REQUIRE_EQ(reopened.estimate.identity.source_key, key,
               "reopen failed to reuse unknown key result with current generation");
    REQUIRE_EQ(reopened.estimate.identity.generation, 2,
               "reopen failed to reuse unknown key result with current generation");
    REQUIRE_TRUE(!reopened.estimate.key,
                 "reopen failed to reuse unknown key result with current generation");
    REQUIRE_TRUE(reopened.estimate.compatible_keys.empty(),
                 "reopen failed to reuse unknown key result with current generation");
    REQUIRE_TRUE(!application::load_key_suggestion(store, &cache, source, 0),
                 "invalid or cancelled key request returned cached data");
    REQUIRE_TRUE(!application::load_key_suggestion(store, &cache, source, 3, [] { return true; }),
                 "invalid or cancelled key request returned cached data");
    int checks = 0;
    const auto cancelled_after_verify =
        application::load_key_suggestion(store, &cache, source, 3, [&] { return ++checks >= 2; });
    REQUIRE_TRUE(!cancelled_after_verify,
                 "cancellation after managed-source verification returned cached data");
    REQUIRE_EQ(cancelled_after_verify.error().code, foundation::ErrorCode::cancelled,
               "cancellation after managed-source verification returned cached data");
    checks = 0;
    const auto cancelled_on_cache_hit =
        application::load_key_suggestion(store, &cache, source, 3, [&] { return ++checks >= 3; });
    REQUIRE_TRUE(!cancelled_on_cache_hit, "cancellation during cache reuse returned stale data");
    REQUIRE_EQ(cancelled_on_cache_hit.error().code, foundation::ErrorCode::cancelled,
               "cancellation during cache reuse returned stale data");
  }
  {
    std::ofstream corrupt(root / "cache" / (key + ".cache"), std::ios::binary | std::ios::trunc);
    corrupt << "corrupt";
  }
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto rebuilt = take(application::load_key_suggestion(store, &cache, source, 4));
    REQUIRE_TRUE(!rebuilt.cache_hit, "corrupt disposable key entry was not rebuilt");
    REQUIRE_EQ(rebuilt.estimate.identity.generation, 4,
               "corrupt disposable key entry was not rebuilt");
    require(take(application::load_key_suggestion(store, &cache, source, 5)).cache_hit,
            "rebuilt key entry did not become reusable");
    REQUIRE_OK(cache.clear(), "cannot clear key fixture cache");
    require(!take(application::load_key_suggestion(store, &cache, source, 6)).cache_hit,
            "deleted key cache was not rebuilt");
  }
  {
    auto failed_cache = take(media::CacheStore::adopt_directory_fd(
        take(store.open_cache_directory()), [](media::CacheWriteStage stage) {
          return stage == media::CacheWriteStage::temporary_created;
        }));
    REQUIRE_OK(failed_cache.clear(), "cannot clear key fixture cache");
    auto fallback = take(application::load_key_suggestion(store, &failed_cache, source, 7));
    REQUIRE_TRUE(!fallback.cache_hit, "cache write failure hid a usable key suggestion");
    REQUIRE_TRUE(!fallback.cache_warning.empty(),
                 "cache write failure hid a usable key suggestion");
    REQUIRE_EQ(fallback.estimate.frames, source.frames,
               "cache write failure hid a usable key suggestion");
    auto without_cache = take(application::load_key_suggestion(store, nullptr, source, 8));
    REQUIRE_TRUE(!without_cache.cache_hit, "key analysis incorrectly requires a cache");
    REQUIRE_TRUE(without_cache.cache_warning.empty(), "key analysis incorrectly requires a cache");
    REQUIRE_EQ(without_cache.estimate.frames, source.frames,
               "key analysis incorrectly requires a cache");
  }
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    int checks = 0;
    const auto interrupted =
        application::load_key_suggestion(store, &cache, source, 9, [&] { return ++checks >= 4; });
    REQUIRE_TRUE(!interrupted, "analysis cancellation must not return partial result");
    REQUIRE_EQ(interrupted.error().code, foundation::ErrorCode::cancelled,
               "analysis cancellation must not return partial result");
    require(!take(application::load_key_suggestion(store, &cache, source, 10)).cache_hit,
            "cancelled analysis published a disposable cache entry");
  }
  {
    bool published = false;
    auto cache = take(media::CacheStore::adopt_directory_fd(
        take(store.open_cache_directory()), [&](media::CacheWriteStage stage) {
          if (stage == media::CacheWriteStage::directory_flushed) {
            published = true;
          }
          return false;
        }));
    REQUIRE_OK(cache.clear(), "cannot reset cache for publication race");
    const auto cancelled_after_write =
        application::load_key_suggestion(store, &cache, source, 11, [&] { return published; });
    REQUIRE_TRUE(published,
                 "cancellation after cache publication returned a project-visible result");
    REQUIRE_TRUE(!cancelled_after_write,
                 "cancellation after cache publication returned a project-visible result");
    REQUIRE_EQ(cancelled_after_write.error().code, foundation::ErrorCode::cancelled,
               "cancellation after cache publication returned a project-visible result");
  }
  // Different encoded bytes with equal decoded shape must produce a different
  // semantic key and never reuse the first source's result.
  std::ifstream original(OSCILLATE_STARTER_PLUCK, std::ios::binary);
  std::vector<char> bytes{std::istreambuf_iterator<char>(original),
                          std::istreambuf_iterator<char>()};
  REQUIRE_GT(bytes.size(), 100, "starter fixture is unexpectedly small");
  bytes.back() ^= 1;
  const auto changed_path = temporary.path() / "changed.wav";
  std::ofstream changed(changed_path, std::ios::binary | std::ios::trunc);
  changed.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  changed.close();
  const auto changed_source =
      stage(store, take(project::SourceId::from_bits({0, 4})), changed_path);
  REQUIRE_EQ(changed_source.sample_rate, source.sample_rate,
             "source-version fixture lacks equal decoded shape and distinct encoded bytes");
  REQUIRE_EQ(changed_source.frames, source.frames,
             "source-version fixture lacks equal decoded shape and distinct encoded bytes");
  REQUIRE_NE(changed_source.sha256, source.sha256,
             "source-version fixture lacks equal decoded shape and distinct encoded bytes");
  REQUIRE_NE(take(application::key_cache_key(changed_source)).hex(), key,
             "changed immutable source bytes kept the old cache identity");
  {
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    auto changed_result = take(application::load_key_suggestion(store, &cache, changed_source, 11));
    REQUIRE_TRUE(!changed_result.cache_hit, "changed source version reused old key cache entry");
    REQUIRE_NE(changed_result.estimate.identity.source_key, key,
               "changed source version reused old key cache entry");
    auto forged = source;
    forged.sha256 = std::string(64, '0');
    require(!application::load_key_suggestion(store, &cache, forged, 12),
            "unverified source record was accepted before cache lookup");
  }
}
} // namespace

int main() {
  try {
    verified_cache_lifecycle_and_source_version();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
