#include "oscillate/application/cache_control.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/source_analysis_cache_directory.hpp"
#include "oscillate/media/cache_identity.hpp"
#include "oscillate/media/cache_store.hpp"
#include "oscillate/persistence/store.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace app = oscillate::application;
namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-cache-control") {}
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return path(); }
};

media::CacheKey key(std::uint32_t version) {
  const auto source_id = take(project::SourceId::from_bits({0, 1}));
  project::SourceRecord source{
      source_id, project::SourceEncoding::wav, std::string(64, 'a'), 60, 48000, 2, 8};
  media::CacheDescriptor descriptor{media::CacheArtifact::source_analysis,
                                    std::move(source),
                                    {},
                                    {},
                                    "cache-control.v1",
                                    version,
                                    1,
                                    {std::byte{0x01}},
                                    {}};
  return take(media::make_cache_key(descriptor));
}

void make_project(const fs::path& root) {
  auto session = take(app::ProjectSession::create(root));
  REQUIRE_NE(session, nullptr, "project was not created");
}

void put(const fs::path& root, std::uint32_t version, std::size_t bytes) {
  auto project_store = take(persistence::ProjectStore::open_reader(root));
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  take(cache.put(key(version), std::vector<std::byte>(bytes, std::byte{0x7a})));
}

void set_access_time(const fs::path& root, std::uint32_t version, std::int64_t seconds) {
  const auto path = root / "cache" / (key(version).hex() + ".cache");
  const timespec times[2]{{seconds, 0}, {0, UTIME_OMIT}};
  REQUIRE_EQ(::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW), 0,
             "could not set fixture access time");
}

void registry_budget_and_clear_preserve_essential_data() {
  Directory fixture;
  const auto first = fixture.root() / "alpha";
  const auto second = fixture.root() / "beta";
  make_project(first);
  make_project(second);
  fs::create_directories(first / "renders");
  {
    std::ofstream render(first / "renders" / "master.wav");
    render << "user render";
  }
  {
    std::ofstream foreign(first / "cache" / "foreign.bin");
    foreign << "not managed";
  }
  put(first, 1, 100);
  put(second, 2, 200);
  set_access_time(first, 1, 100);
  set_access_time(second, 2, 200);
  auto control = take(app::CacheControl::open(fixture.root() / "profile"));
  take(control->register_project(first));
  take(control->register_project(second));
  take(control->register_project(first));
  auto state = take(control->health());
  REQUIRE_EQ(state.projects.size(), 2, "registry did not deduplicate or count both caches");
  REQUIRE_EQ(state.global.entries, 2, "registry did not deduplicate or count both caches");
  REQUIRE_GT(state.global.bytes, 300, "registry did not deduplicate or count both caches");
  const auto second_bytes = state.projects[1].usage.bytes;
  take(control->set_budget(second_bytes));
  auto result = take(control->enforce_budget());
  REQUIRE_EQ(result.evicted_entries, 1, "global budget did not remove oldest registry entry");
  REQUIRE_EQ(result.after.bytes, second_bytes,
             "global budget did not remove oldest registry entry");
  REQUIRE_TRUE(fs::exists(first / "state" / "CURRENT"), "budget removed essential or foreign data");
  REQUIRE_TRUE(fs::exists(first / "renders" / "master.wav"),
               "budget removed essential or foreign data");
  REQUIRE_TRUE(fs::exists(first / "cache" / "foreign.bin"),
               "budget removed essential or foreign data");
  REQUIRE_EQ(take(control->clear_reachable()), 1, "clear did not remove remaining managed entry");
  REQUIRE_TRUE(fs::exists(second / "state" / "CURRENT"),
               "clear removed project state or foreign data");
  REQUIRE_TRUE(fs::exists(first / "cache" / "foreign.bin"),
               "clear removed project state or foreign data");
  control.reset();
  auto reopened_project = take(app::ProjectSession::open(first));
  REQUIRE_NE(reopened_project, nullptr, "project did not reopen after cache maintenance");
  auto reopened = take(app::CacheControl::open(fixture.root() / "profile"));
  take(reopened->register_project(first));
  state = take(reopened->health());
  REQUIRE_EQ(state.projects.size(), 2, "registry or budget did not persist across reopen");
  REQUIRE_EQ(state.budget_bytes, second_bytes, "registry or budget did not persist across reopen");
  REQUIRE_EQ(state.global.entries, 0, "registry or budget did not persist across reopen");
}

// Forgetting a project drops only its registry entry: its files and cache
// stay, the rest stay registered, it persists, and a deleted project can go.
void forgotten_projects_leave_the_registry() {
  Directory fixture;
  const auto kept = fixture.root() / "kept";
  const auto forgotten = fixture.root() / "forgotten";
  const auto deleted = fixture.root() / "deleted";
  constexpr std::uint32_t version = 1;
  constexpr std::size_t bytes = 100;
  make_project(kept);
  make_project(forgotten);
  make_project(deleted);
  put(forgotten, version, bytes);
  const auto profile = fixture.root() / "profile";
  auto control = take(app::CacheControl::open(profile));
  take(control->register_project(kept));
  take(control->register_project(forgotten));
  take(control->register_project(deleted));
  take(control->forget_project(forgotten));
  REQUIRE_TRUE(fs::exists(forgotten / "state" / "CURRENT"), "forgetting touched the project");
  REQUIRE_TRUE(fs::exists(forgotten / "cache" / (key(version).hex() + ".cache")),
               "forgetting touched the project's cache");
  fs::remove_all(deleted);
  take(control->forget_project(deleted));
  const auto unknown = control->forget_project(forgotten);
  REQUIRE_TRUE(!unknown, "an unregistered project was forgotten");
  REQUIRE_EQ(unknown.error().code, foundation::ErrorCode::not_found,
             "an unregistered project was forgotten");
  control.reset();
  auto reopened = take(app::CacheControl::open(profile));
  const auto state = take(reopened->health());
  REQUIRE_EQ(state.projects.size(), 1, "forgetting did not persist");
  REQUIRE_EQ(state.projects.front().root, kept, "forgetting dropped the wrong project");
}

void shared_source_analysis_counts_once_and_survives_project_clear() {
  Directory fixture;
  const auto root = fixture.root() / "song";
  make_project(root);
  put(root, 21, 100);
  set_access_time(root, 21, 100);
  const auto cache_home = fixture.root() / "xdg-cache";
  auto shared = take(app::open_user_source_analysis_cache(cache_home));
  take(shared.put(key(22), std::vector<std::byte>(96, std::byte{0x58})));
  const auto shared_path =
      cache_home / "oscillate" / "source-analysis" / "v1" / (key(22).hex() + ".cache");
  const timespec times[2]{{200, 0}, {0, UTIME_OMIT}};
  REQUIRE_EQ(::utimensat(AT_FDCWD, shared_path.c_str(), times, AT_SYMLINK_NOFOLLOW), 0,
             "could not set shared cache access time");
  const auto shared_bytes = take(shared.usage()).bytes;
  auto control = take(app::CacheControl::open(fixture.root() / "profile"));
  take(control->register_project(root));
  auto state = take(control->health(&shared));
  REQUIRE_EQ(state.global.entries, 2, "shared source cache was not counted exactly once");
  REQUIRE_EQ(state.projects.size(), 1, "shared source cache was not counted exactly once");
  REQUIRE_EQ(state.global.bytes, state.projects.front().usage.bytes + shared_bytes,
             "shared source cache was not counted exactly once");
  take(control->set_budget(shared_bytes));
  const auto result = take(control->enforce_budget(&shared));
  REQUIRE_EQ(result.evicted_entries, 1, "global eviction did not retain the newer shared entry");
  REQUIRE_EQ(result.after.entries, 1, "global eviction did not retain the newer shared entry");
  REQUIRE_EQ(result.after.bytes, shared_bytes,
             "global eviction did not retain the newer shared entry");
  REQUIRE_EQ(take(shared.usage()).entries, 1,
             "global eviction did not retain the newer shared entry");
  take(control->clear_project(root));
  REQUIRE_EQ(take(control->health(&shared)).global.entries, 1,
             "clearing one project removed shared analysis");
  REQUIRE_TRUE(fs::exists(shared_path), "clearing one project removed shared analysis");
}

void moved_and_replaced_paths_are_never_cleared() {
  Directory fixture;
  const auto original = fixture.root() / "original";
  const auto moved = fixture.root() / "moved";
  make_project(original);
  put(original, 3, 42);
  auto control = take(app::CacheControl::open(fixture.root() / "profile"));
  take(control->register_project(original));
  fs::rename(original, moved);
  make_project(original);
  put(original, 4, 35);
  auto state = take(control->health());
  REQUIRE_EQ(state.projects.size(), 1, "replaced project path was treated as registered");
  REQUIRE_EQ(state.projects[0].status, app::CacheProjectStatus::moved,
             "replaced project path was treated as registered");
  REQUIRE_EQ(state.global.entries, 0, "replaced project path was treated as registered");
  require(!control->clear_project(original), "clear followed replaced project path");
  take(control->set_budget(0));
  REQUIRE_EQ(take(control->enforce_budget()).evicted_entries, 0,
             "budget evicted an unregistered replacement project");
  require(fs::exists(original / "cache" / (key(4).hex() + ".cache")),
          "replacement project cache was modified");
  take(control->register_project(moved));
  state = take(control->health());
  REQUIRE_EQ(state.projects.size(), 1, "moved project was not rebound in registry order");
  REQUIRE_EQ(state.projects[0].root, moved, "moved project was not rebound in registry order");
  REQUIRE_EQ(state.projects[0].status, app::CacheProjectStatus::reachable,
             "moved project was not rebound in registry order");
  take(control->clear_project(moved));
  REQUIRE_EQ(take(control->health()).global.entries, 0, "rebound project cache did not clear");
}

void missing_cache_and_corrupt_registry_are_reported() {
  Directory fixture;
  const auto root = fixture.root() / "song";
  make_project(root);
  put(root, 5, 31);
  auto control = take(app::CacheControl::open(fixture.root() / "profile"));
  take(control->register_project(root));
  require(!app::CacheControl::open(fixture.root() / "profile"),
          "second controller bypassed profile lock");
  fs::rename(root / "cache", root / "cache-old");
  auto state = take(control->health());
  REQUIRE_EQ(state.projects[0].status, app::CacheProjectStatus::cache_unavailable,
             "missing cache was not reported and omitted");
  REQUIRE_EQ(state.global.entries, 0, "missing cache was not reported and omitted");
  require(!control->clear_project(root), "clear followed a missing cache");
  require(fs::exists(root / "cache-old"), "missing cache data was deleted");
  {
    auto writer = take(persistence::ProjectStore::open_writer(root, false));
    require(fs::is_directory(root / "cache"), "project did not rebuild disposable cache directory");
  }
  state = take(control->health());
  REQUIRE_EQ(state.projects[0].status, app::CacheProjectStatus::cache_unavailable,
             "recreated cache was silently accepted without registration");
  take(control->register_project(root));
  REQUIRE_EQ(take(control->health()).projects[0].status, app::CacheProjectStatus::reachable,
             "reopened project did not register its recreated cache");
  control.reset();
  {
    std::ofstream registry(fixture.root() / "profile" / "cache-registry.json", std::ios::trunc);
    registry << "{broken";
  }
  require(!app::CacheControl::open(fixture.root() / "profile"),
          "corrupt registry was silently replaced");
  {
    std::ofstream empty(fixture.root() / "profile" / "cache-registry.json", std::ios::trunc);
  }
  require(!app::CacheControl::open(fixture.root() / "profile"),
          "empty registry was silently replaced");
}

void registry_rejects_invalid_projects_and_reports_damaged_state() {
  Directory fixture;
  const auto root = fixture.root() / "song";
  make_project(root);
  auto control = take(app::CacheControl::open(fixture.root() / "profile"));
  require(!control->register_project(fs::path{"relative"}),
          "relative project path entered registry");
  require(!control->register_project(fixture.root() / "missing"),
          "missing project entered registry");
  require(!control->clear_project(root), "unregistered project was cleared");
  const auto alias = fixture.root() / "song-link";
  fs::create_directory_symlink(root, alias);
  require(!control->register_project(alias), "symlink project root entered registry");
  take(control->register_project(root));
  fs::rename(root / "state" / "CURRENT", root / "state" / "CURRENT-hidden");
  auto state = take(control->health());
  REQUIRE_EQ(state.projects[0].status, app::CacheProjectStatus::failed,
             "damaged project state was not reported");
  REQUIRE_EQ(state.projects[0].usage.entries, 0, "damaged project state was not reported");
  REQUIRE_TRUE(!control->enforce_budget(),
               "failed project inspection was silently ignored by destructive controls");
  REQUIRE_TRUE(!control->clear_reachable(),
               "failed project inspection was silently ignored by destructive controls");
  fs::rename(root / "state" / "CURRENT-hidden", root / "state" / "CURRENT");
  take(control->register_project(root));
  fs::rename(root / "cache", root / "cache-hidden");
  fs::create_directory_symlink(fixture.root(), root / "cache");
  state = take(control->health());
  REQUIRE_EQ(state.projects[0].status, app::CacheProjectStatus::cache_unavailable,
             "symlink cache directory was followed");
  REQUIRE_TRUE(!control->clear_project(root), "clear followed symlink cache directory");
  REQUIRE_TRUE(fs::is_directory(root / "cache-hidden"), "clear followed symlink cache directory");
}

void service_controls_serialize_with_live_cache_producers() {
  Directory fixture;
  const auto root = fixture.root() / "song";
  make_project(root);
  auto project_store = take(persistence::ProjectStore::open_reader(root));
  auto first =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  auto second =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  auto control = take(app::CacheControl::open(fixture.root() / "profile"));
  take(control->register_project(root));
  take(control->set_budget(0));
  const std::vector<std::byte> payload(2048, std::byte{0x4b});
  std::atomic<bool> coherent{true};
  const auto produce = [&](media::CacheStore& store, std::uint32_t first_version) {
    for (std::uint32_t index = 0; index < 40; ++index) {
      const auto entry_key = key(first_version + index % 4);
      if (!store.put(entry_key, payload)) {
        coherent.store(false);
        return;
      }
      const auto observed = store.get(entry_key);
      if (!observed || (observed.value().has_value() && *observed.value() != payload)) {
        coherent.store(false);
        return;
      }
    }
  };
  std::thread first_worker([&] { produce(first, 100); });
  std::thread second_worker([&] { produce(second, 200); });
  for (int attempt = 0; attempt < 40; ++attempt) {
    const auto observed = control->health();
    if (!observed || observed.value().projects.size() != 1 ||
        observed.value().projects[0].status != app::CacheProjectStatus::reachable ||
        !control->enforce_budget() || !control->clear_project(root)) {
      coherent.store(false);
      break;
    }
  }
  first_worker.join();
  second_worker.join();
  require(coherent.load(), "live producer and cache controls raced or reported an error");
  REQUIRE_OK(control->clear_reachable());
  REQUIRE_EQ(take(control->health()).global.entries, 0,
             "cache clear missed a completed live producer entry");
  require(fs::exists(root / "state" / "CURRENT"), "cache controls damaged project state");
}

} // namespace

int main() {
  try {
    registry_budget_and_clear_preserve_essential_data();
    forgotten_projects_leave_the_registry();
    shared_source_analysis_counts_once_and_survives_project_clear();
    moved_and_replaced_paths_are_never_cleared();
    missing_cache_and_corrupt_registry_are_reported();
    registry_rejects_invalid_projects_and_reports_damaged_state();
    service_controls_serialize_with_live_cache_producers();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
