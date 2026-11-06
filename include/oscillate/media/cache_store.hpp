#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/cache_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::media {

enum class CacheWriteStage : std::uint8_t {
  temporary_created,
  header_written,
  payload_written,
  file_flushed,
  entry_published,
  directory_flushed
};

using CacheFaultHook = std::function<bool(CacheWriteStage)>;
// Worker-side callbacks. A producer synchronously appends bounded chunks and
// must supply its exact declared byte count. Producers must not reenter this
// store or retain the append function. Neither runs on the audio callback.
using CacheChunkAppend = std::function<foundation::Result<void>(std::span<const std::byte>)>;
using CacheStreamProducer = std::function<foundation::Result<void>(const CacheChunkAppend&)>;
using CacheChunkConsumer = std::function<foundation::Result<void>(std::span<const std::byte>)>;

struct CacheUsage {
  std::uint64_t bytes = 0;
  std::uint64_t entries = 0;
  // A valid hit remains usable if its access timestamp could not be updated.
  bool recency_degraded = false;
};

struct CacheBudgetResult {
  CacheUsage before;
  CacheUsage after;
  std::uint64_t evicted_entries = 0;
};

class CacheStore;

// Worker-side, verified view of one immutable cache inode. The complete
// header, length and SHA-256 are checked before a handle is returned. Managed
// eviction or replacement may unlink its name without invalidating this fd.
// Direct writes to cache files outside CacheStore are unsupported.
class ValidatedCacheReader {
public:
  ValidatedCacheReader(const ValidatedCacheReader&) = delete;
  ValidatedCacheReader& operator=(const ValidatedCacheReader&) = delete;
  ValidatedCacheReader(ValidatedCacheReader&& other) noexcept;
  ValidatedCacheReader& operator=(ValidatedCacheReader&& other) noexcept;
  ~ValidatedCacheReader();

  [[nodiscard]] std::uint64_t size_bytes() const noexcept { return payload_bytes_; }
  // A single call reads at most 256 KiB; a request crossing the payload end
  // returns the available prefix. Calls are independent and may run on worker
  // threads concurrently. Never call this from the audio callback.
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t offset,
                                                        std::span<std::byte> output) const;

private:
  friend class CacheStore;
  struct Descriptor {
    int fd;
    std::uint64_t payload_bytes;
  };
  explicit ValidatedCacheReader(Descriptor descriptor) noexcept
      : fd_(descriptor.fd), payload_bytes_(descriptor.payload_bytes) {}
  int fd_ = -1;
  std::uint64_t payload_bytes_ = 0;
};

// Applies one byte budget to the project cache directories supplied by the
// caller. The caller supplies each reachable project once, in stable registry
// order, and keeps the stores alive for the call. Missing projects are omitted
// rather than removed. This is control/worker-side disk I/O, never callback work.
[[nodiscard]] foundation::Result<CacheBudgetResult>
enforce_global_cache_budget(std::span<CacheStore* const> stores, std::uint64_t max_bytes);

// Disposable derived data only. The caller transfers ownership of a pinned
// project cache-directory fd obtained from ProjectStore. No project paths are
// resolved by this class, and no method may run on the audio callback.
class CacheStore {
public:
  [[nodiscard]] static foundation::Result<CacheStore> adopt_directory_fd(int directory_fd,
                                                                         CacheFaultHook fault = {});

  CacheStore(const CacheStore&) = delete;
  CacheStore& operator=(const CacheStore&) = delete;
  CacheStore(CacheStore&& other) noexcept;
  CacheStore& operator=(CacheStore&& other) noexcept;
  ~CacheStore();

  [[nodiscard]] foundation::Result<void> put(const CacheKey& key,
                                             std::span<const std::byte> payload);
  // Large derived audio uses bounded chunks and an exact declared byte count.
  // The 1 GiB cap covers ten minutes of stereo f32 at 96 kHz. A failed
  // producer leaves the old entry in place and discards its temporary file.
  [[nodiscard]] foundation::Result<void>
  put_stream(const CacheKey& key, std::uint64_t payload_bytes, const CacheStreamProducer& producer);
  // Missing, truncated, or corrupt entries are misses; the producer rebuilds
  // them. Other filesystem failures are reported to the control side.
  [[nodiscard]] foundation::Result<std::optional<std::vector<std::byte>>>
  get(const CacheKey& key) const;
  // Hash and length are checked before the first consumer call. A missing or
  // corrupt entry returns a miss and calls no consumer. The caller verifies
  // source identity before requesting any derived cache hit.
  [[nodiscard]] foundation::Result<std::optional<std::uint64_t>>
  get_stream(const CacheKey& key, const CacheChunkConsumer& consumer) const;
  // Opens a fully validated, random-access read handle. Missing or corrupt
  // entries are misses. The caller verifies the source identity separately.
  [[nodiscard]] foundation::Result<std::optional<ValidatedCacheReader>>
  open_validated(const CacheKey& key) const;
  // Removes only recognized cache entries and interrupted temporary files.
  [[nodiscard]] foundation::Result<void> clear();
  // Includes recognized regular entries and interrupted temporary files.
  // All CacheStore instances coordinate through an exclusive lock on the
  // pinned directory, including instances in other processes. Direct cache
  // filesystem access outside CacheStore remains the caller's responsibility.
  [[nodiscard]] foundation::Result<CacheUsage> usage() const;
  // Removes interrupted temporaries first, then oldest entries. A successful
  // get explicitly refreshes access time, independent of filesystem relatime.
  [[nodiscard]] foundation::Result<CacheBudgetResult> enforce_budget(std::uint64_t max_bytes);

private:
  friend foundation::Result<CacheBudgetResult>
  enforce_global_cache_budget(std::span<CacheStore* const> stores, std::uint64_t max_bytes);
  struct Descriptors {
    int directory;
    int lock;
  };
  explicit CacheStore(Descriptors descriptors, CacheFaultHook fault)
      : directory_fd_(descriptors.directory), directory_lock_fd_(descriptors.lock),
        fault_(std::move(fault)) {}
  [[nodiscard]] bool fail(CacheWriteStage stage) const { return fault_ && fault_(stage); }
  int directory_fd_ = -1;
  // A distinct open-file description is required for flock to contend even
  // when callers adopted duplicates of the same source descriptor.
  int directory_lock_fd_ = -1;
  CacheFaultHook fault_;
  mutable std::mutex mutex_;
  mutable bool recency_degraded_ = false;
};

} // namespace oscillate::media
