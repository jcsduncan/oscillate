#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/cache_store.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace oscillate::application {

enum class CacheProjectStatus : std::uint8_t {
  reachable,
  unreachable,
  moved,
  cache_unavailable,
  failed
};

struct CacheProjectHealth {
  std::filesystem::path root;
  CacheProjectStatus status = CacheProjectStatus::unreachable;
  media::CacheUsage usage;
  std::string detail;
};

struct CacheHealth {
  std::uint64_t budget_bytes = 0;
  media::CacheUsage global;
  std::vector<CacheProjectHealth> projects;
};

// Per-user, control/worker-side cache registry. Open one instance per app
// profile. Its advisory lock excludes another instance from registry changes
// in the same profile; CacheStore coordinates cache I/O across instances by
// locking each pinned directory. Never call it on the audio thread.
class CacheControl {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<CacheControl>>
  open(const std::filesystem::path& profile_directory);

  CacheControl(const CacheControl&) = delete;
  CacheControl& operator=(const CacheControl&) = delete;
  ~CacheControl();

  // Registration verifies a committed project and records root/cache inode
  // identities. Re-registering a moved project updates its path in place.
  [[nodiscard]] foundation::Result<void> register_project(const std::filesystem::path& root);
  // Include the shared source-analysis store once when supplied by the caller.
  [[nodiscard]] foundation::Result<CacheHealth>
  health(media::CacheStore* shared_cache = nullptr) const;
  [[nodiscard]] foundation::Result<void> set_budget(std::uint64_t bytes);
  [[nodiscard]] foundation::Result<media::CacheBudgetResult>
  enforce_budget(media::CacheStore* shared_cache = nullptr);
  [[nodiscard]] foundation::Result<void> clear_project(const std::filesystem::path& root);
  // Returns entries observed before each project clear. Concurrent producers
  // can add entries between projects, so this is a maintenance count rather
  // than a transaction-wide exact removal total.
  [[nodiscard]] foundation::Result<std::uint64_t> clear_reachable();

private:
  struct Descriptors {
    int directory;
    int lock;
  };
  struct Entry {
    std::filesystem::path root;
    std::uint64_t root_device;
    std::uint64_t root_inode;
    std::uint64_t cache_device;
    std::uint64_t cache_inode;
  };
  CacheControl(Descriptors descriptors, std::vector<Entry> entries, std::uint64_t budget_bytes);
  [[nodiscard]] foundation::Result<void> persist(const std::vector<Entry>& entries,
                                                 std::uint64_t budget_bytes, bool& published) const;

  int directory_fd_ = -1;
  int lock_fd_ = -1;
  std::vector<Entry> entries_;
  std::uint64_t budget_bytes_ = 0;
  mutable std::mutex mutex_;
};

} // namespace oscillate::application
