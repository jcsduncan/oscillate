#include "oscillate/media/cache_store.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T take_present(std::optional<T> value) {
  if (!value) {
    throw std::runtime_error("expected a cache hit");
  }
  return std::move(*value);
}

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-cache") {}
};

media::CacheKey key(std::uint32_t version = 1) {
  const auto source_id = take(project::SourceId::from_bits({0, 1}));
  project::SourceRecord source{
      source_id, project::SourceEncoding::wav, std::string(64, 'a'), 60, 48000, 2, 8};
  media::CacheDescriptor descriptor{media::CacheArtifact::source_analysis,
                                    std::move(source),
                                    {},
                                    {},
                                    "waveform.v1",
                                    version,
                                    1,
                                    {std::byte{0x01}},
                                    {}};
  return take(media::make_cache_key(descriptor));
}

void set_access_time(const fs::path& path, std::int64_t seconds, long nanoseconds = 0) {
  const timespec times[2]{{seconds, nanoseconds}, {0, UTIME_OMIT}};
  REQUIRE_EQ(::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW), 0,
             "could not set fixture access time");
}

void budget_uses_explicit_recency_and_preserves_foreign_data() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const std::array<std::byte, 2> payload{std::byte{0x10}, std::byte{0x20}};
  const auto first = key(30);
  const auto second = key(31);
  const auto third = key(32);
  for (const auto& entry_key : {first, second, third}) {
    take(cache.put(entry_key, payload));
  }
  const auto path_for = [&](const media::CacheKey& entry_key) {
    return root / "cache" / (entry_key.hex() + ".cache");
  };
  const auto entry_bytes = fs::file_size(path_for(first));
  set_access_time(path_for(first), 100);
  set_access_time(path_for(second), 200);
  set_access_time(path_for(third), 300);
  const auto modification_time = fs::last_write_time(path_for(first));
  REQUIRE_EQ(take(cache.get(first)), std::vector<std::byte>(payload.begin(), payload.end()),
             "cache access failed");
  REQUIRE_EQ(fs::last_write_time(path_for(first)), modification_time,
             "cache access changed modification time");
  const auto initial = take(cache.usage());
  REQUIRE_EQ(initial.bytes, 3 * entry_bytes, "cache usage was not counted");
  REQUIRE_EQ(initial.entries, 3, "cache usage was not counted");
  REQUIRE_TRUE(!initial.recency_degraded, "cache usage was not counted");
  const auto unchanged = take(cache.enforce_budget(initial.bytes));
  REQUIRE_EQ(unchanged.after.bytes, initial.bytes, "sufficient budget evicted an entry");
  REQUIRE_EQ(unchanged.evicted_entries, 0, "sufficient budget evicted an entry");
  const auto reduced = take(cache.enforce_budget(2 * entry_bytes));
  REQUIRE_EQ(reduced.before.bytes, initial.bytes, "cache budget did not report the reduction");
  REQUIRE_EQ(reduced.after.bytes, 2 * entry_bytes, "cache budget did not report the reduction");
  REQUIRE_EQ(reduced.evicted_entries, 1, "cache budget did not report the reduction");
  REQUIRE_TRUE(fs::exists(path_for(first)), "explicit access did not protect the recent entry");
  REQUIRE_TRUE(!fs::exists(path_for(second)), "explicit access did not protect the recent entry");
  REQUIRE_TRUE(fs::exists(path_for(third)), "explicit access did not protect the recent entry");
  const auto reduced_again = take(cache.enforce_budget(entry_bytes));
  REQUIRE_EQ(reduced_again.after.bytes, entry_bytes,
             "second eviction did not remove the next oldest entry");
  REQUIRE_EQ(reduced_again.evicted_entries, 1,
             "second eviction did not remove the next oldest entry");
  REQUIRE_TRUE(!fs::exists(path_for(third)),
               "second eviction did not remove the next oldest entry");
  REQUIRE_TRUE(fs::exists(path_for(first)), "second eviction did not remove the next oldest entry");

  take(cache.put(second, payload));
  set_access_time(path_for(first), 400, 1);
  set_access_time(path_for(second), 400, 1);
  const auto alphabetically_first = first.hex() < second.hex() ? first : second;
  REQUIRE_OK(cache.enforce_budget(entry_bytes));
  require(!fs::exists(path_for(alphabetically_first)), "equal-time eviction was not deterministic");

  const auto asset = root / "assets" / "source.wav";
  std::ofstream(asset) << "source";
  const auto foreign = root / "cache" / "unmanaged.txt";
  std::ofstream(foreign) << "foreign";
  const auto corrupt = path_for(key(33));
  std::ofstream(corrupt) << "corrupt";
  const auto interrupted = root / "cache" / (key(34).hex() + ".tmp.0000000000000000");
  std::ofstream(interrupted) << "temporary";
  const auto linked = path_for(key(35));
  fs::create_symlink(asset, linked);
  const auto with_disposable = take(cache.usage());
  REQUIRE_EQ(with_disposable.entries, 3,
             "usage did not distinguish regular managed files from a symlink or foreign file");
  REQUIRE_EQ(with_disposable.bytes,
             entry_bytes + fs::file_size(corrupt) + fs::file_size(interrupted),
             "usage did not distinguish regular managed files from a symlink or foreign file");
  const auto purged = take(cache.enforce_budget(0));
  REQUIRE_EQ(purged.after.bytes, 0, "zero budget did not remove all reconstructable files");
  REQUIRE_EQ(purged.after.entries, 0, "zero budget did not remove all reconstructable files");
  REQUIRE_EQ(purged.evicted_entries, 3, "zero budget did not remove all reconstructable files");
  REQUIRE_TRUE(fs::exists(asset), "eviction touched source, foreign data or symlink");
  REQUIRE_TRUE(fs::exists(foreign), "eviction touched source, foreign data or symlink");
  REQUIRE_TRUE(fs::is_symlink(linked), "eviction touched source, foreign data or symlink");
}

void budget_and_reads_are_serialized() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const auto entry_key = key(36);
  const std::vector<std::byte> payload(std::size_t{256} * 1024U, std::byte{0x7f});
  take(cache.put(entry_key, payload));
  std::atomic<bool> coherent{true};
  std::thread reader([&] {
    for (int attempt = 0; attempt < 32; ++attempt) {
      const auto read = cache.get(entry_key);
      if (!read || (read.value() && read.value() != payload)) {
        coherent.store(false);
      }
    }
  });
  for (int attempt = 0; attempt < 32; ++attempt) {
    REQUIRE_OK(cache.enforce_budget(0));
    take(cache.put(entry_key, payload));
  }
  reader.join();
  require(coherent.load(), "reader observed partial entry during eviction");
}

void global_budget_orders_projects_and_preserves_essential_files() {
  Directory temporary;
  const auto first_root = temporary.path() / "first";
  const auto second_root = temporary.path() / "second";
  auto first_project = take(persistence::ProjectStore::open_writer(first_root, true));
  auto second_project = take(persistence::ProjectStore::open_writer(second_root, true));
  auto first =
      take(media::CacheStore::adopt_directory_fd(take(first_project.open_cache_directory())));
  auto second =
      take(media::CacheStore::adopt_directory_fd(take(second_project.open_cache_directory())));
  const std::array<std::byte, 2> payload{std::byte{0x31}, std::byte{0x42}};
  const auto oldest = key(40);
  const auto middle = key(41);
  const auto newest = key(42);
  take(first.put(oldest, payload));
  take(second.put(middle, payload));
  take(first.put(newest, payload));
  const auto first_path = [&](const media::CacheKey& entry_key) {
    return first_root / "cache" / (entry_key.hex() + ".cache");
  };
  const auto second_path = [&](const media::CacheKey& entry_key) {
    return second_root / "cache" / (entry_key.hex() + ".cache");
  };
  const auto entry_bytes = fs::file_size(first_path(oldest));
  set_access_time(first_path(oldest), 100);
  set_access_time(second_path(middle), 200);
  set_access_time(first_path(newest), 300);
  const auto source = second_root / "assets" / "source.wav";
  const auto render = second_root / "render.wav";
  std::ofstream(source) << "source";
  std::ofstream(render) << "render";
  const auto foreign = second_root / "cache" / "notes";
  std::ofstream(foreign) << "foreign";
  std::array<media::CacheStore*, 2> stores{&first, &second};
  const auto reduced = take(media::enforce_global_cache_budget(stores, 2 * entry_bytes));
  REQUIRE_EQ(reduced.before.bytes, 3 * entry_bytes,
             "global budget failed to evict the oldest cross-project entry");
  REQUIRE_EQ(reduced.after.bytes, 2 * entry_bytes,
             "global budget failed to evict the oldest cross-project entry");
  REQUIRE_EQ(reduced.evicted_entries, 1,
             "global budget failed to evict the oldest cross-project entry");
  REQUIRE_TRUE(!fs::exists(first_path(oldest)),
               "global budget failed to evict the oldest cross-project entry");
  REQUIRE_TRUE(fs::exists(second_path(middle)),
               "global budget failed to evict the oldest cross-project entry");
  REQUIRE_TRUE(fs::exists(first_path(newest)),
               "global budget failed to evict the oldest cross-project entry");
  REQUIRE_TRUE(fs::exists(source), "global budget damaged essential or foreign data");
  REQUIRE_TRUE(fs::exists(render), "global budget damaged essential or foreign data");
  REQUIRE_TRUE(fs::exists(foreign), "global budget damaged essential or foreign data");

  set_access_time(second_path(middle), 400);
  set_access_time(first_path(newest), 400);
  const auto tie = take(media::enforce_global_cache_budget(stores, entry_bytes));
  REQUIRE_EQ(tie.after.bytes, entry_bytes,
             "global budget did not use caller's stable project order on a tie");
  REQUIRE_TRUE(!fs::exists(first_path(newest)),
               "global budget did not use caller's stable project order on a tie");
  REQUIRE_TRUE(fs::exists(second_path(middle)),
               "global budget did not use caller's stable project order on a tie");
  const auto interrupted = first_root / "cache" / (key(43).hex() + ".tmp.0000000000000000");
  std::ofstream(interrupted) << "interrupted";
  const auto enough_space = take(media::enforce_global_cache_budget(stores, entry_bytes));
  REQUIRE_EQ(enough_space.after.bytes, entry_bytes,
             "global budget did not discard interrupted temporary first");
  REQUIRE_TRUE(!fs::exists(interrupted),
               "global budget did not discard interrupted temporary first");
  REQUIRE_TRUE(fs::exists(second_path(middle)),
               "global budget did not discard interrupted temporary first");

  std::array<media::CacheStore*, 2> duplicate{&first, &first};
  require(!media::enforce_global_cache_budget(duplicate, 0), "duplicate store was counted twice");
  auto alias =
      take(media::CacheStore::adopt_directory_fd(take(first_project.open_cache_directory())));
  duplicate = {&first, &alias};
  require(!media::enforce_global_cache_budget(duplicate, 0),
          "same directory through distinct stores was counted twice");
  require(fs::exists(second_path(middle)), "invalid global budget call deleted an entry");
  std::array<media::CacheStore*, 1> missing{nullptr};
  require(!media::enforce_global_cache_budget(missing, 0), "null store was accepted");
}

void global_budget_serializes_with_writes_and_reads() {
  Directory temporary;
  auto first_project =
      take(persistence::ProjectStore::open_writer(temporary.path() / "first", true));
  auto second_project =
      take(persistence::ProjectStore::open_writer(temporary.path() / "second", true));
  auto first =
      take(media::CacheStore::adopt_directory_fd(take(first_project.open_cache_directory())));
  auto second =
      take(media::CacheStore::adopt_directory_fd(take(second_project.open_cache_directory())));
  const auto first_key = key(44);
  const auto second_key = key(45);
  const std::vector<std::byte> payload(std::size_t{64} * 1024U, std::byte{0x5a});
  take(first.put(first_key, payload));
  take(second.put(second_key, payload));
  std::array<media::CacheStore*, 2> forward{&first, &second};
  std::array<media::CacheStore*, 2> reverse{&second, &first};
  std::atomic<bool> coherent{true};
  std::thread worker([&] {
    for (int attempt = 0; attempt < 20; ++attempt) {
      const auto observed = second.get(second_key);
      if (!observed || (observed.value() && observed.value() != payload) ||
          !second.put(second_key, payload)) {
        coherent.store(false);
      }
      if (!media::enforce_global_cache_budget(reverse, 0)) {
        coherent.store(false);
      }
    }
  });
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!first.put(first_key, payload) || !media::enforce_global_cache_budget(forward, 0)) {
      coherent.store(false);
    }
  }
  worker.join();
  require(coherent.load(), "global budget raced a read/write or deadlocked");
}

void global_budget_locks_distinct_instances_in_directory_order() {
  Directory temporary;
  auto first_project =
      take(persistence::ProjectStore::open_writer(temporary.path() / "first", true));
  auto second_project =
      take(persistence::ProjectStore::open_writer(temporary.path() / "second", true));
  auto first_a =
      take(media::CacheStore::adopt_directory_fd(take(first_project.open_cache_directory())));
  auto second_a =
      take(media::CacheStore::adopt_directory_fd(take(second_project.open_cache_directory())));
  auto first_b =
      take(media::CacheStore::adopt_directory_fd(take(first_project.open_cache_directory())));
  auto second_b =
      take(media::CacheStore::adopt_directory_fd(take(second_project.open_cache_directory())));
  std::array<media::CacheStore*, 2> forward{&first_a, &second_a};
  std::array<media::CacheStore*, 2> reverse{&second_b, &first_b};
  const std::array<std::byte, 1> payload{std::byte{0x39}};
  std::atomic<bool> coherent{true};
  std::thread worker([&] {
    for (int attempt = 0; attempt < 20; ++attempt) {
      if (!first_b.put(key(50), payload) || !media::enforce_global_cache_budget(reverse, 0)) {
        coherent.store(false);
      }
    }
  });
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!second_a.put(key(51), payload) || !media::enforce_global_cache_budget(forward, 0)) {
      coherent.store(false);
    }
  }
  worker.join();
  require(coherent.load(), "separate global cache owners deadlocked or failed");
}

void round_trip_corruption_and_clear() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const auto entry_key = key();
  require(!take(cache.get(entry_key)).has_value(), "new cache unexpectedly hit");

  const std::vector<std::byte> payload{std::byte{0x00}, std::byte{0xa5}, std::byte{0xff}};
  take(cache.put(entry_key, payload));
  REQUIRE_EQ(take(cache.get(entry_key)), payload, "cache payload failed round trip");

  const auto entry = root / "cache" / (entry_key.hex() + ".cache");
  {
    std::fstream file(entry, std::ios::binary | std::ios::in | std::ios::out);
    require(file.good(), "cache entry could not be altered for test");
    file.seekp(-1, std::ios::end);
    file.put('x');
  }
  require(!take(cache.get(entry_key)).has_value(), "damaged payload was accepted");
  take(cache.put(entry_key, payload));
  REQUIRE_EQ(take(cache.get(entry_key)), payload, "valid rewrite did not replace damaged entry");

  {
    std::ofstream file(entry, std::ios::binary | std::ios::trunc);
    file.put('x');
  }
  require(!take(cache.get(entry_key)).has_value(), "truncated entry was accepted");
  take(cache.put(entry_key, payload));

  {
    std::fstream file(entry, std::ios::binary | std::ios::in | std::ios::out);
    file.put('X');
  }
  require(!take(cache.get(entry_key)).has_value(), "wrong cache header was accepted");
  take(cache.put(entry_key, payload));

  const auto interrupted = root / "cache" / (entry_key.hex() + ".tmp.0000000000000000");
  {
    std::ofstream file(interrupted, std::ios::binary);
    file << "interrupted";
  }
  const auto asset = root / "assets" / "source.wav";
  const auto foreign = root / "cache" / "user-note";
  {
    std::ofstream file(asset);
    file << "keep source";
  }
  {
    std::ofstream file(foreign);
    file << "keep foreign";
  }
  const std::array malformed_names{
      std::string("short"), std::string(64, 'z') + ".cache", entry_key.hex() + ".other",
      entry_key.hex() + ".bad.0000000000000000", entry_key.hex() + ".tmp.000000000000000g"};
  for (const auto& name : malformed_names) {
    std::ofstream(root / "cache" / name) << "keep foreign";
  }
  take(cache.clear());
  REQUIRE_TRUE(!fs::exists(entry), "clear left a reconstructable entry or interrupted temporary");
  REQUIRE_TRUE(!fs::exists(interrupted),
               "clear left a reconstructable entry or interrupted temporary");
  REQUIRE_TRUE(fs::exists(asset), "clear damaged non-cache data");
  REQUIRE_TRUE(fs::exists(foreign), "clear damaged non-cache data");
  for (const auto& name : malformed_names) {
    require(fs::exists(root / "cache" / name), "clear removed an unrecognized entry");
  }
  require(!take(cache.get(entry_key)).has_value(), "cleared entry remained readable");

  take(cache.put(entry_key, payload));
  take(cache.clear());
  require(!fs::exists(entry), "second clear missed a new entry");

  fs::create_symlink(asset, entry);
  require(!take(cache.get(entry_key)).has_value(), "symlink cache entry was followed");
  take(cache.clear());
  REQUIRE_TRUE(!fs::exists(entry), "clearing a cache symlink touched its target");
  REQUIRE_TRUE(fs::exists(asset), "clearing a cache symlink touched its target");
}

void deletion_reopen_and_symlink_rejection() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  {
    auto store = take(persistence::ProjectStore::open_writer(root, true));
    auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
    const std::array<std::byte, 1> payload{std::byte{0x42}};
    take(cache.put(key(), payload));
    fs::remove_all(root / "cache");
    require(!store.open_cache_directory(), "deleted cache directory still opened by path");
  }
  {
    auto reopened = take(persistence::ProjectStore::open_writer(root, false));
    auto cache = take(media::CacheStore::adopt_directory_fd(take(reopened.open_cache_directory())));
    require(!take(cache.get(key())).has_value(), "deleted cache was not a miss");
  }
  fs::remove_all(root / "cache");
  const auto outside = temporary.path() / "outside";
  fs::create_directory(outside);
  fs::create_directory_symlink(outside, root / "cache");
  require(!persistence::ProjectStore::open_writer(root, false),
          "symlinked cache directory was accepted");
  require(fs::is_empty(outside), "cache reopen touched external directory");
}

void interrupted_writes_remain_disposable() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  const std::array<std::byte, 2> payload{std::byte{0x13}, std::byte{0x37}};
  const std::array stages{
      media::CacheWriteStage::temporary_created, media::CacheWriteStage::header_written,
      media::CacheWriteStage::payload_written,   media::CacheWriteStage::file_flushed,
      media::CacheWriteStage::entry_published,   media::CacheWriteStage::directory_flushed};
  for (std::size_t index = 0; index < stages.size(); ++index) {
    auto cache = take(media::CacheStore::adopt_directory_fd(
        take(store.open_cache_directory()),
        [stage = stages[index]](media::CacheWriteStage reached) { return reached == stage; }));
    const auto entry_key = key(static_cast<std::uint32_t>(index + 2));
    require(!cache.put(entry_key, payload), "injected cache interruption was ignored");
    const auto visible = take(cache.get(entry_key));
    REQUIRE_EQ(visible.has_value(), (index >= 4), "cache publication boundary changed");
    for (const auto& entry : fs::directory_iterator(root / "cache")) {
      REQUIRE_NE(entry.path().extension(), ".tmp", "incomplete cache temporary survived");
      REQUIRE_EQ(entry.path().filename().string().find(".tmp."), std::string::npos,
                 "incomplete cache temporary survived");
    }
  }
}

void move_assignment_retains_pinned_directory() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  auto first = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  auto second = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  first = std::move(second);
  const std::array<std::byte, 1> payload{std::byte{0x42}};
  take(first.put(key(), payload));
  REQUIRE_EQ(take(first.get(key())), std::vector<std::byte>(payload.begin(), payload.end()),
             "moved cache handle lost pinned directory");
}

void separate_instances_lock_even_when_adopting_duplicated_descriptors() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  const int original = take(store.open_cache_directory());
  const int duplicate = ::dup(original);
  REQUIRE_GE(duplicate, 0, "could not duplicate cache directory descriptor");
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  auto writer =
      take(media::CacheStore::adopt_directory_fd(original, [&](media::CacheWriteStage stage) {
        if (stage != media::CacheWriteStage::temporary_created) {
          return false;
        }
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
        return true;
      }));
  auto reader = take(media::CacheStore::adopt_directory_fd(duplicate));
  const int probe = ::open((root / "cache").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  REQUIRE_GE(probe, 0, "could not open independent cache lock probe");
  const std::array<std::byte, 1> payload{std::byte{0x42}};
  const auto entry_key = key(91);
  std::atomic<bool> interrupted{false};
  std::thread worker([&] { interrupted.store(!writer.put(entry_key, payload)); });
  {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return entered; });
  }
  const int attempt = ::flock(probe, LOCK_EX | LOCK_NB);
  const bool contended = attempt != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
  if (attempt == 0) {
    (void)::flock(probe, LOCK_UN);
  }
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  worker.join();
  require(contended, "cache write did not hold a cross-instance directory lock");
  require(interrupted.load(), "fault-injected write did not return failure");
  REQUIRE_EQ(::flock(probe, LOCK_EX | LOCK_NB), 0, "cache lock leaked after interrupted write");
  REQUIRE_EQ(::flock(probe, LOCK_UN), 0, "could not release probe lock");
  (void)::close(probe);
  require(!take(reader.get(entry_key)).has_value(), "partial interrupted entry became visible");
  take(reader.put(entry_key, payload));
  REQUIRE_EQ(take(writer.get(entry_key)), std::vector<std::byte>(payload.begin(), payload.end()),
             "second cache instance could not read published entry");
}

void access_failures_are_reported() {
  if (::geteuid() == 0) {
    return;
  }
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const std::array<std::byte, 1> payload{std::byte{0x42}};
  take(cache.put(key(), payload));
  const auto cache_path = root / "cache";
  struct RestorePermissions {
    fs::path path;
    ~RestorePermissions() { (void)::chmod(path.c_str(), 0700); }
  } restore{cache_path};
  const int inaccessible_fd = ::open(cache_path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  REQUIRE_GE(inaccessible_fd, 0, "could not pin cache before removing access");
  REQUIRE_EQ(::chmod(cache_path.c_str(), 0000), 0, "could not remove cache access");
  require(!media::CacheStore::adopt_directory_fd(inaccessible_fd),
          "cache store accepted a directory it could not lock");
  require(!cache.put(key(20), payload), "unwritable cache accepted a new entry");
  require(!cache.get(key()), "unreadable cache did not report I/O failure");
  require(!cache.clear(), "unscannable cache did not report I/O failure");
  require(!cache.usage(), "unscannable cache reported usage");
  require(!cache.enforce_budget(0), "unscannable cache enforced a budget");
  REQUIRE_EQ(::chmod(cache_path.c_str(), 0500), 0, "could not make cache read-only");
  require(!cache.clear(), "cache clear ignored unlink permission failure");
  require(!cache.enforce_budget(0), "cache eviction ignored unlink permission failure");
}

void oversized_entry_is_rejected_before_writing() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const std::vector<std::byte> oversized(std::size_t{64} * 1024U * 1024U + 1U);
  const auto result = cache.put(key(), oversized);
  REQUIRE_TRUE(!result, "oversized cache entry was accepted");
  REQUIRE_EQ(result.error().code, foundation::ErrorCode::resource_exhausted,
             "oversized cache entry was accepted");
  require(!take(cache.get(key())).has_value(), "oversized entry was partially published");
}

void streaming_entry_exceeds_small_cache_limit_without_large_buffer() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto project_store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  const auto entry_key = key(101);
  std::array<std::byte, std::size_t{64} * 1024U> block{};
  block.fill(std::byte{0x5a});
  constexpr std::uint64_t entry_size = std::uint64_t{65} * 1024U * 1024U;
  take(cache.put_stream(entry_key, entry_size, [&](const media::CacheChunkAppend& append) {
    for (std::uint64_t count = 0; count < entry_size / block.size(); ++count) {
      if (auto result = append(block); !result) {
        return result;
      }
    }
    return foundation::Result<void>::success();
  }));
  require(!take(cache.get(entry_key)).has_value(), "small get loaded a large stream");
  std::uint64_t received = 0;
  const auto streamed = take(cache.get_stream(entry_key, [&](std::span<const std::byte> chunk) {
    REQUIRE_TRUE(!chunk.empty(), "stream reader sent an oversized chunk");
    REQUIRE_LE(chunk.size(), std::size_t{256} * 1024U, "stream reader sent an oversized chunk");
    for (const auto value : chunk) {
      REQUIRE_EQ(value, std::byte{0x5a}, "stream reader changed payload bytes");
    }
    received += chunk.size();
    return foundation::Result<void>::success();
  }));
  REQUIRE_EQ(streamed, entry_size, "large streaming entry did not round trip");
  REQUIRE_EQ(received, entry_size, "large streaming entry did not round trip");

  const auto path = root / "cache" / (entry_key.hex() + ".cache");
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    require(file.good(), "large entry could not be altered");
    const auto header_size = fs::file_size(path) - entry_size;
    file.seekp(static_cast<std::streamoff>(header_size + entry_size / 2));
    file.put('x');
  }
  std::uint64_t callbacks = 0;
  const auto corrupt_read = take(cache.get_stream(entry_key, [&](std::span<const std::byte>) {
    ++callbacks;
    return foundation::Result<void>::success();
  }));
  REQUIRE_TRUE(!corrupt_read.has_value(),
               "corrupt streaming entry exposed bytes before validation");
  REQUIRE_EQ(callbacks, 0, "corrupt streaming entry exposed bytes before validation");
  const auto removed = take(cache.enforce_budget(0));
  REQUIRE_EQ(removed.evicted_entries, 1,
             "large stream did not participate in ordinary cache eviction");
  REQUIRE_EQ(removed.after.bytes, 0, "large stream did not participate in ordinary cache eviction");
  REQUIRE_TRUE(!fs::exists(path), "large stream did not participate in ordinary cache eviction");
}

void validated_reader_seeks_and_survives_eviction_and_replacement() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto project_store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  const auto entry_key = key(109);
  const std::array<std::byte, 7> original{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                          std::byte{5}, std::byte{6}, std::byte{7}};
  take(cache.put_stream(entry_key, original.size(),
                        [&](const media::CacheChunkAppend& append) { return append(original); }));
  auto reader = take_present(take(cache.open_validated(entry_key)));
  REQUIRE_EQ(reader.size_bytes(), original.size(), "valid cache entry was not opened");
  std::array<std::byte, 4> bytes{};
  REQUIRE_EQ(take(reader.read_at(2, bytes)), 4, "validated reader did not seek to payload offset");
  REQUIRE_EQ(bytes,
             (std::array<std::byte, 4>{std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}}),
             "validated reader did not seek to payload offset");
  REQUIRE_EQ(take(reader.read_at(6, bytes)), 1, "validated reader did not bound the final chunk");
  REQUIRE_EQ(bytes[0], std::byte{7}, "validated reader did not bound the final chunk");
  REQUIRE_EQ(take(reader.read_at(7, bytes)), 0, "validated reader did not bound the final chunk");
  std::vector<std::byte> oversized(std::size_t{256} * 1024U + 1U);
  REQUIRE_TRUE(!reader.read_at(8, bytes),
               "validated reader accepted an invalid offset or oversized request");
  REQUIRE_TRUE(!reader.read_at(0, oversized),
               "validated reader accepted an invalid offset or oversized request");

  REQUIRE_OK(cache.enforce_budget(0));
  require(!take(cache.open_validated(entry_key)).has_value(), "evicted name remained visible");
  REQUIRE_EQ(take(reader.read_at(0, bytes)), 4, "pinned validated inode was lost after eviction");
  REQUIRE_EQ(bytes[0], original[0], "pinned validated inode was lost after eviction");
  const std::array<std::byte, 2> replacement{std::byte{0x55}, std::byte{0x66}};
  take(cache.put(entry_key, replacement));
  auto replacement_reader = take_present(take(cache.open_validated(entry_key)));
  REQUIRE_EQ(replacement_reader.size_bytes(), replacement.size(),
             "replacement altered an already validated handle");
  REQUIRE_EQ(take(replacement_reader.read_at(0, bytes)), replacement.size(),
             "replacement altered an already validated handle");
  REQUIRE_EQ(bytes[0], replacement[0], "replacement altered an already validated handle");
  REQUIRE_EQ(take(reader.read_at(0, bytes)), 4, "replacement altered an already validated handle");
  REQUIRE_EQ(bytes[0], original[0], "replacement altered an already validated handle");
}

void validated_reader_rejects_corruption_and_oversize_before_exposure() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto project_store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  const auto entry_key = key(110);
  const auto path = root / "cache" / (entry_key.hex() + ".cache");
  const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2}, std::byte{3}};
  take(cache.put(entry_key, payload));
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(-1, std::ios::end);
    file.put('x');
  }
  require(!take(cache.open_validated(entry_key)).has_value(),
          "corrupt payload produced a validated handle");
  take(cache.put(entry_key, payload));
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(8);
    file.put('x');
  }
  require(!take(cache.open_validated(entry_key)).has_value(),
          "corrupt header produced a validated handle");
  take(cache.put(entry_key, payload));
  fs::resize_file(path, (std::uint64_t{1} << 30U) + 4096U);
  require(!take(cache.open_validated(entry_key)).has_value(),
          "oversized entry produced a validated handle");
}

void validated_reader_and_clear_do_not_hold_store_lock() {
  Directory temporary;
  auto project_store =
      take(persistence::ProjectStore::open_writer(temporary.path() / "project", true));
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  const auto entry_key = key(111);
  const std::vector<std::byte> payload(std::size_t{256} * 1024U, std::byte{0x42});
  take(cache.put(entry_key, payload));
  auto pinned = take(cache.open_validated(entry_key));
  REQUIRE_OK(pinned, "concurrent reader fixture missing");
  std::atomic<bool> coherent{true};
  std::thread reader([&] {
    std::array<std::byte, 1024> chunk{};
    for (std::size_t offset = 0; offset < payload.size(); offset += chunk.size()) {
      const auto read = pinned->read_at(offset, chunk);
      if (!read || read.value() != chunk.size() ||
          !std::ranges::all_of(chunk, [](std::byte byte) { return byte == std::byte{0x42}; })) {
        coherent.store(false);
      }
    }
  });
  take(cache.clear());
  reader.join();
  REQUIRE_TRUE(coherent, "concurrent clear invalidated the pinned reader");
  REQUIRE_TRUE(!take(cache.open_validated(entry_key)).has_value(),
               "concurrent clear invalidated the pinned reader");
}

void streaming_failures_keep_prior_entry_and_remove_temporary() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto project_store = take(persistence::ProjectStore::open_writer(root, true));
  const auto entry_key = key(102);
  const std::array<std::byte, 3> original{std::byte{1}, std::byte{2}, std::byte{3}};
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  take(cache.put(entry_key, original));
  const std::array<std::byte, 2> partial{std::byte{4}, std::byte{5}};
  const auto cancelled = cache.put_stream(entry_key, 4, [&](const media::CacheChunkAppend& append) {
    take(append(partial));
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::io_failure, "producer cancelled"});
  });
  REQUIRE_TRUE(!cancelled, "cancelled stream replaced the old entry");
  REQUIRE_EQ(take(cache.get(entry_key)), std::vector<std::byte>(original.begin(), original.end()),
             "cancelled stream replaced the old entry");
  const auto short_write = cache.put_stream(
      entry_key, 4, [&](const media::CacheChunkAppend& append) { return append(partial); });
  REQUIRE_TRUE(!short_write, "short stream replaced the old entry");
  REQUIRE_EQ(take(cache.get(entry_key)), std::vector<std::byte>(original.begin(), original.end()),
             "short stream replaced the old entry");
  const auto overrun = cache.put_stream(entry_key, 1, [&](const media::CacheChunkAppend& append) {
    (void)append(partial); // A producer cannot hide an append failure.
    return foundation::Result<void>::success();
  });
  REQUIRE_TRUE(!overrun, "overrun stream replaced the old entry");
  REQUIRE_EQ(take(cache.get(entry_key)), std::vector<std::byte>(original.begin(), original.end()),
             "overrun stream replaced the old entry");
  const auto zero_chunk =
      cache.put_stream(entry_key, 1, [&](const media::CacheChunkAppend& append) {
        (void)append({});
        return foundation::Result<void>::success();
      });
  require(!zero_chunk, "zero-sized stream chunk was accepted");
  std::uint64_t temporary_files = 0;
  for (const auto& path : fs::directory_iterator(root / "cache")) {
    temporary_files += path.path().filename().string().find(".tmp.") != std::string::npos;
  }
  REQUIRE_EQ(temporary_files, 0, "failed streaming write left temporary files");
  bool producer_called = false;
  REQUIRE_TRUE(!cache.put_stream(entry_key, (std::uint64_t{1} << 30U) + 1U,
                                 [&](const media::CacheChunkAppend&) {
                                   producer_called = true;
                                   return foundation::Result<void>::success();
                                 }),
               "oversized stream invoked producer");
  REQUIRE_TRUE(!producer_called, "oversized stream invoked producer");
}

void streaming_consumer_failure_and_fault_stage() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto project_store = take(persistence::ProjectStore::open_writer(root, true));
  const auto entry_key = key(103);
  const std::array<std::byte, 2> payload{std::byte{0x11}, std::byte{0x22}};
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  take(cache.put_stream(entry_key, payload.size(),
                        [&](const media::CacheChunkAppend& append) { return append(payload); }));
  const auto failed_read = cache.get_stream(entry_key, [&](std::span<const std::byte>) {
    return foundation::Result<void>::failure({foundation::ErrorCode::io_failure, "sink cancelled"});
  });
  REQUIRE_TRUE(!failed_read, "consumer failure changed the cache entry");
  REQUIRE_EQ(take(cache.get(entry_key)), std::vector<std::byte>(payload.begin(), payload.end()),
             "consumer failure changed the cache entry");
  auto fault_cache = take(media::CacheStore::adopt_directory_fd(
      take(project_store.open_cache_directory()), [](media::CacheWriteStage stage) {
        return stage == media::CacheWriteStage::payload_written;
      }));
  const auto fault_key = key(104);
  REQUIRE_TRUE(!fault_cache.put_stream(
                   fault_key, payload.size(),
                   [&](const media::CacheChunkAppend& append) { return append(payload); }),
               "faulted stream was published");
  REQUIRE_TRUE(!take(cache.get_stream(fault_key,
                                      [&](std::span<const std::byte>) {
                                        return foundation::Result<void>::success();
                                      }))
                    .has_value(),
               "faulted stream was published");
}

void streaming_producer_does_not_block_clear_or_lose_its_temporary() {
  Directory temporary;
  auto project_store =
      take(persistence::ProjectStore::open_writer(temporary.path() / "project", true));
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  const auto entry_key = key(105);
  const std::array<std::byte, 1> payload{std::byte{0x66}};
  std::mutex gate_mutex;
  std::condition_variable gate;
  bool producer_paused = false;
  bool resume = false;
  bool cleared = false;
  bool produced = false;
  std::thread writer([&] {
    const auto result =
        cache.put_stream(entry_key, payload.size(), [&](const media::CacheChunkAppend& append) {
          {
            std::lock_guard lock(gate_mutex);
            producer_paused = true;
          }
          gate.notify_all();
          std::unique_lock lock(gate_mutex);
          gate.wait(lock, [&] { return resume; });
          return append(payload);
        });
    produced = result.has_value();
  });
  {
    std::unique_lock lock(gate_mutex);
    gate.wait(lock, [&] { return producer_paused; });
  }
  std::thread clearer([&] {
    const auto result = cache.clear();
    {
      std::lock_guard lock(gate_mutex);
      cleared = result.has_value();
    }
    gate.notify_all();
  });
  bool cleared_before_resume = false;
  {
    std::unique_lock lock(gate_mutex);
    cleared_before_resume = gate.wait_for(lock, std::chrono::seconds(2), [&] { return cleared; });
    resume = true;
  }
  gate.notify_all();
  clearer.join();
  writer.join();
  REQUIRE_TRUE(cleared_before_resume, "active stream blocked clear or lost its leased temporary");
  REQUIRE_TRUE(produced, "active stream blocked clear or lost its leased temporary");
  REQUIRE_EQ(take(cache.get(entry_key)), std::vector<std::byte>(payload.begin(), payload.end()),
             "active stream blocked clear or lost its leased temporary");
}

void older_same_key_stream_cannot_replace_newer_publication() {
  Directory temporary;
  auto project_store =
      take(persistence::ProjectStore::open_writer(temporary.path() / "project", true));
  auto first =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  auto second =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  const auto entry_key = key(106);
  const std::array<std::byte, 1> old_payload{std::byte{0x10}};
  const std::array<std::byte, 1> new_payload{std::byte{0x20}};
  take(first.put(entry_key, old_payload));
  std::mutex gate_mutex;
  std::condition_variable gate;
  bool paused = false;
  bool resume = false;
  foundation::ErrorCode first_error = foundation::ErrorCode::internal;
  std::thread older([&] {
    auto result =
        first.put_stream(entry_key, old_payload.size(), [&](const media::CacheChunkAppend& append) {
          {
            std::lock_guard lock(gate_mutex);
            paused = true;
          }
          gate.notify_all();
          std::unique_lock lock(gate_mutex);
          gate.wait(lock, [&] { return resume; });
          return append(old_payload);
        });
    first_error = result ? foundation::ErrorCode::internal : result.error().code;
  });
  {
    std::unique_lock lock(gate_mutex);
    gate.wait(lock, [&] { return paused; });
  }
  REQUIRE_EQ(take(first.get(entry_key)),
             std::vector<std::byte>(old_payload.begin(), old_payload.end()),
             "read during pending same-key stream lost prior entry");
  const auto newer =
      second.put_stream(entry_key, new_payload.size(),
                        [&](const media::CacheChunkAppend& append) { return append(new_payload); });
  {
    std::lock_guard lock(gate_mutex);
    resume = true;
  }
  gate.notify_all();
  older.join();
  REQUIRE_TRUE(newer, "older same-key stream replaced a newer published entry");
  REQUIRE_EQ(first_error, foundation::ErrorCode::conflict,
             "older same-key stream replaced a newer published entry");
  REQUIRE_EQ(take(first.get(entry_key)),
             std::vector<std::byte>(new_payload.begin(), new_payload.end()),
             "older same-key stream replaced a newer published entry");
}

void replaced_temporary_name_is_not_removed_by_cleanup() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto project_store = take(persistence::ProjectStore::open_writer(root, true));
  fs::path replacement;
  auto cache = take(media::CacheStore::adopt_directory_fd(
      take(project_store.open_cache_directory()), [&](media::CacheWriteStage stage) {
        if (stage == media::CacheWriteStage::file_flushed) {
          for (const auto& entry : fs::directory_iterator(root / "cache")) {
            if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
              replacement = entry.path();
              fs::rename(replacement, root / "cache" / "moved-inode");
              std::ofstream(replacement) << "foreign replacement";
              break;
            }
          }
        }
        return false;
      }));
  const std::array<std::byte, 1> payload{std::byte{0x77}};
  const auto result =
      cache.put_stream(key(108), payload.size(),
                       [&](const media::CacheChunkAppend& append) { return append(payload); });
  REQUIRE_TRUE(!result, "replaced temporary name was published or removed");
  REQUIRE_EQ(result.error().code, foundation::ErrorCode::conflict,
             "replaced temporary name was published or removed");
  REQUIRE_TRUE(!replacement.empty(), "replaced temporary name was published or removed");
  REQUIRE_TRUE(fs::exists(replacement), "replaced temporary name was published or removed");
  std::ifstream replacement_bytes(replacement);
  std::string contents;
  std::getline(replacement_bytes, contents);
  REQUIRE_EQ(contents, "foreign replacement", "cleanup changed replacement bytes");
}

void producer_exception_removes_its_temporary() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto project_store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache =
      take(media::CacheStore::adopt_directory_fd(take(project_store.open_cache_directory())));
  bool caught = false;
  try {
    (void)cache.put_stream(key(107), 1,
                           [&](const media::CacheChunkAppend&) -> foundation::Result<void> {
                             throw std::runtime_error("producer failure");
                           });
  } catch (const std::runtime_error&) {
    caught = true;
  }
  REQUIRE_TRUE(caught, "throwing producer left a managed temporary");
  REQUIRE_EQ(take(cache.usage()).entries, 0, "throwing producer left a managed temporary");
}

void obstructed_entry_and_nonflushable_directory_report_failure() {
  Directory temporary;
  const auto root = temporary.path() / "project";
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const std::array<std::byte, 1> payload{std::byte{0x42}};
  const auto obstructed = root / "cache" / (key().hex() + ".cache");
  fs::create_directory(obstructed);
  require(!cache.put(key(), payload), "directory obstructing an entry was overwritten");
  require(fs::is_directory(obstructed), "failed publish removed unrelated directory");
  fs::remove(obstructed);

  const int path_fd = ::open((root / "cache").c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  REQUIRE_GE(path_fd, 0, "could not open nonflushable cache capability");
  auto nonflushable = take(media::CacheStore::adopt_directory_fd(path_fd));
  require(!nonflushable.put(key(), payload), "nonflushable cache publication claimed durability");
  REQUIRE_EQ(take(nonflushable.get(key())), std::vector<std::byte>(payload.begin(), payload.end()),
             "published cache entry was damaged by directory flush failure");
  require(!nonflushable.clear(), "nonflushable cache clear claimed durability");
  require(!fs::exists(obstructed), "failed cache clear left an entry behind");
  require(!nonflushable.put(key(), payload), "nonflushable cache unexpectedly wrote durably");
  require(!nonflushable.enforce_budget(0), "nonflushable eviction claimed durability");
  require(!fs::exists(obstructed), "failed eviction left an entry behind");
}

void invalid_directory_handle_is_rejected() {
  require(!media::CacheStore::adopt_directory_fd(-1), "invalid cache fd was accepted");
}

} // namespace

int main() {
  try {
    round_trip_corruption_and_clear();
    deletion_reopen_and_symlink_rejection();
    interrupted_writes_remain_disposable();
    move_assignment_retains_pinned_directory();
    separate_instances_lock_even_when_adopting_duplicated_descriptors();
    access_failures_are_reported();
    oversized_entry_is_rejected_before_writing();
    streaming_entry_exceeds_small_cache_limit_without_large_buffer();
    validated_reader_seeks_and_survives_eviction_and_replacement();
    validated_reader_rejects_corruption_and_oversize_before_exposure();
    validated_reader_and_clear_do_not_hold_store_lock();
    streaming_failures_keep_prior_entry_and_remove_temporary();
    streaming_consumer_failure_and_fault_stage();
    streaming_producer_does_not_block_clear_or_lose_its_temporary();
    older_same_key_stream_cannot_replace_newer_publication();
    replaced_temporary_name_is_not_removed_by_cleanup();
    producer_exception_removes_its_temporary();
    obstructed_entry_and_nonflushable_directory_report_failure();
    invalid_directory_handle_is_rejected();
    budget_uses_explicit_recency_and_preserves_foreign_data();
    budget_and_reads_are_serialized();
    global_budget_orders_projects_and_preserves_essential_files();
    global_budget_serializes_with_writes_and_reads();
    global_budget_locks_distinct_instances_in_directory_order();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
