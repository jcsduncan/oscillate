#include "oscillate/media/cache_store.hpp"

#include "oscillate/foundation/file_io.hpp"
#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace oscillate::media {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr std::string_view magic = "LDAWCACH";
constexpr std::uint32_t format_version = 1;
constexpr std::size_t max_payload_bytes = std::size_t{64} * 1024U * 1024U;
constexpr std::uint64_t max_stream_payload_bytes = std::uint64_t{1} << 30U;
constexpr std::size_t stream_chunk_bytes = std::size_t{256} * 1024U;
constexpr std::size_t max_scanned_entries = 262144;
constexpr std::size_t max_global_stores = 257; // 256 registered projects plus shared analysis.
constexpr std::size_t header_bytes = 8U + 4U + 64U + 8U + 32U;

using Fd = foundation::UniqueFd;
using foundation::io_error;

class DirectoryLock {
public:
  explicit DirectoryLock(int fd) noexcept : fd_(fd) {}
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;
  DirectoryLock(DirectoryLock&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)), locked_(std::exchange(other.locked_, false)) {}
  ~DirectoryLock() {
    if (locked_) {
      (void)::flock(fd_, LOCK_UN);
    }
  }

  [[nodiscard]] Result<void> acquire() {
    while (::flock(fd_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        return Result<void>::failure(io_error("lock cache directory"));
      }
    }
    locked_ = true;
    return Result<void>::success();
  }

private:
  int fd_;
  bool locked_ = false;
};

void put32(std::uint8_t* bytes, std::uint32_t value) noexcept {
  for (unsigned index = 0; index < 4; ++index) {
    bytes[index] = static_cast<std::uint8_t>(value >> (8U * index));
  }
}

void put64(std::uint8_t* bytes, std::uint64_t value) noexcept {
  for (unsigned index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(value >> (8U * index));
  }
}

std::uint32_t read32(const std::uint8_t* bytes) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
  }
  return value;
}

std::uint64_t read64(const std::uint8_t* bytes) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
  }
  return value;
}

Result<foundation::Sha256Digest> digest(std::span<const std::byte> bytes) {
  return foundation::sha256(bytes);
}

Result<void> write_all(int fd, std::span<const std::uint8_t> bytes) {
  return foundation::write_all(fd, std::as_bytes(bytes), "write cache entry");
}

Result<bool> read_all(int fd, std::span<std::uint8_t> bytes) {
  while (!bytes.empty()) {
    // CacheStore serializes bounded regular-file reads with eviction; it is never used by the
    // callback. NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
    const auto count = ::read(fd, bytes.data(), bytes.size());
    if (count > 0) {
      bytes = bytes.subspan(static_cast<std::size_t>(count));
    } else if (count == 0) {
      return Result<bool>::success(false);
    } else if (errno != EINTR) {
      return Result<bool>::failure(io_error("read cache entry"));
    }
  }
  return Result<bool>::success(true);
}

Result<bool> pread_all(int fd, std::uint64_t offset, std::span<std::uint8_t> bytes) {
  while (!bytes.empty()) {
    const auto count = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    if (count > 0) {
      const auto advanced = static_cast<std::size_t>(count);
      bytes = bytes.subspan(advanced);
      offset += advanced;
    } else if (count == 0) {
      return Result<bool>::success(false);
    } else if (errno != EINTR) {
      return Result<bool>::failure(io_error("read validated cache entry"));
    }
  }
  return Result<bool>::success(true);
}

bool same_contents_metadata(const struct stat& before, const struct stat& after) noexcept {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

Result<std::string> temporary_name(const std::string& key_hex) {
  std::array<std::uint8_t, 8> random{};
  std::size_t filled = 0;
  while (filled < random.size()) {
    const auto count = ::getrandom(random.data() + filled, random.size() - filled, 0);
    if (count > 0) {
      filled += static_cast<std::size_t>(count);
    } else if (count < 0 && errno != EINTR) {
      return Result<std::string>::failure(io_error("name cache temporary file"));
    }
  }
  return Result<std::string>::success(key_hex + ".tmp." + foundation::to_hex(std::span(random)));
}

bool managed_name(std::string_view name) noexcept {
  if (name.size() != 70 && name.size() != 85) {
    return false;
  }
  for (std::size_t index = 0; index < 64; ++index) {
    const char character = name[index];
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  if (name.size() == 70) {
    return name.substr(64) == ".cache";
  }
  if (name.substr(64, 5) != ".tmp.") {
    return false;
  }
  for (const char character : name.substr(69)) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

struct EntryIdentity {
  dev_t device;
  ino_t inode;
  friend bool operator==(const EntryIdentity& left, const EntryIdentity& right) noexcept {
    return left.device == right.device && left.inode == right.inode;
  }
};

Result<std::optional<EntryIdentity>> inspect_entry_identity(int directory_fd,
                                                            const std::string& name) {
  struct stat status{};
  if (::fstatat(directory_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return Result<std::optional<EntryIdentity>>::success(std::nullopt);
    }
    return Result<std::optional<EntryIdentity>>::failure(io_error("inspect cache publication"));
  }
  return Result<std::optional<EntryIdentity>>::success(EntryIdentity{status.st_dev, status.st_ino});
}

// The directory lock serializes managed operations. An active writer keeps a
// separate flock on its temporary inode while it renders outside that lock.
Result<bool> remove_disposable_entry(int directory_fd, const std::string& name, bool temporary) {
  Fd temp;
  if (temporary) {
    temp.reset(::openat(directory_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (temp.get() < 0) {
      if (errno == ENOENT) {
        return Result<bool>::success(false);
      }
      if (errno != ELOOP) {
        return Result<bool>::failure(io_error("open cache temporary for eviction"));
      }
    } else if (::flock(temp.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK) {
        return Result<bool>::success(false);
      }
      return Result<bool>::failure(io_error("inspect cache temporary lease"));
    }
  }
  if (::unlinkat(directory_fd, name.c_str(), 0) != 0) {
    if (errno == ENOENT) {
      return Result<bool>::success(false);
    }
    return Result<bool>::failure(io_error("remove disposable cache entry"));
  }
  return Result<bool>::success(true);
}

struct ScannedEntry {
  std::string name;
  std::uint64_t bytes = 0;
  timespec accessed{};
  bool temporary = false;
};

Result<std::vector<ScannedEntry>> scan_entries(int directory_fd) {
  const int scan_fd = ::openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (scan_fd < 0) {
    return Result<std::vector<ScannedEntry>>::failure(io_error("scan cache directory"));
  }
  DIR* directory = ::fdopendir(scan_fd);
  if (directory == nullptr) {
    const auto error = io_error("open cache scan");
    (void)::close(scan_fd);
    return Result<std::vector<ScannedEntry>>::failure(error);
  }
  std::vector<ScannedEntry> entries;
  std::size_t inspected = 0;
  errno = 0;
  while (auto* entry = ::readdir(directory)) {
    if (++inspected > max_scanned_entries) {
      (void)::closedir(directory);
      return Result<std::vector<ScannedEntry>>::failure(
          {ErrorCode::resource_exhausted, "cache directory scan limit exceeded"});
    }
    const std::string_view name(entry->d_name);
    if (managed_name(name)) {
      struct stat status{};
      if (::fstatat(directory_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) {
          const auto error = io_error("inspect cache usage");
          (void)::closedir(directory);
          return Result<std::vector<ScannedEntry>>::failure(error);
        }
      } else if (S_ISREG(status.st_mode) && status.st_size >= 0) {
        if (entries.size() == max_scanned_entries) {
          (void)::closedir(directory);
          return Result<std::vector<ScannedEntry>>::failure(
              {ErrorCode::resource_exhausted, "cache entry scan limit exceeded"});
        }
        entries.push_back({std::string(name), static_cast<std::uint64_t>(status.st_size),
                           status.st_atim, name.size() == 85});
      }
    }
    errno = 0;
  }
  const int scan_error = errno;
  (void)::closedir(directory);
  if (scan_error != 0) {
    errno = scan_error;
    return Result<std::vector<ScannedEntry>>::failure(io_error("scan cache directory"));
  }
  return Result<std::vector<ScannedEntry>>::success(std::move(entries));
}

Result<CacheUsage> count_usage(const std::vector<ScannedEntry>& entries, bool recency_degraded) {
  CacheUsage usage{0, static_cast<std::uint64_t>(entries.size()), recency_degraded};
  for (const auto& entry : entries) {
    if (entry.bytes > std::numeric_limits<std::uint64_t>::max() - usage.bytes) {
      return Result<CacheUsage>::failure({ErrorCode::resource_exhausted, "cache usage overflow"});
    }
    usage.bytes += entry.bytes;
  }
  return Result<CacheUsage>::success(usage);
}

} // namespace

ValidatedCacheReader::ValidatedCacheReader(ValidatedCacheReader&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), payload_bytes_(std::exchange(other.payload_bytes_, 0)) {}

ValidatedCacheReader& ValidatedCacheReader::operator=(ValidatedCacheReader&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
    payload_bytes_ = std::exchange(other.payload_bytes_, 0);
  }
  return *this;
}

ValidatedCacheReader::~ValidatedCacheReader() {
  if (fd_ >= 0) {
    (void)::close(fd_);
  }
}

Result<std::size_t> ValidatedCacheReader::read_at(std::uint64_t offset,
                                                  std::span<std::byte> output) const {
  if (fd_ < 0 || offset > payload_bytes_) {
    return Result<std::size_t>::failure({ErrorCode::invalid_argument, "invalid cache read offset"});
  }
  if (output.size() > stream_chunk_bytes) {
    return Result<std::size_t>::failure(
        {ErrorCode::resource_exhausted, "cache read exceeds chunk limit"});
  }
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), payload_bytes_ - offset));
  if (count == 0) {
    return Result<std::size_t>::success(0);
  }
  auto read = pread_all(fd_, header_bytes + offset,
                        {reinterpret_cast<std::uint8_t*>(output.data()), count});
  if (!read) {
    return Result<std::size_t>::failure(read.error());
  }
  if (!read.value()) {
    return Result<std::size_t>::failure(
        {ErrorCode::io_failure, "validated cache entry changed during read"});
  }
  return Result<std::size_t>::success(count);
}

CacheStore::CacheStore(CacheStore&& other) noexcept : directory_fd_(-1) {
  std::lock_guard lock(other.mutex_);
  directory_fd_ = std::exchange(other.directory_fd_, -1);
  directory_lock_fd_ = std::exchange(other.directory_lock_fd_, -1);
  fault_ = std::move(other.fault_);
  recency_degraded_ = std::exchange(other.recency_degraded_, false);
}

CacheStore& CacheStore::operator=(CacheStore&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    if (directory_fd_ >= 0) {
      (void)::close(directory_fd_);
    }
    if (directory_lock_fd_ >= 0) {
      (void)::close(directory_lock_fd_);
    }
    directory_fd_ = std::exchange(other.directory_fd_, -1);
    directory_lock_fd_ = std::exchange(other.directory_lock_fd_, -1);
    fault_ = std::move(other.fault_);
    recency_degraded_ = std::exchange(other.recency_degraded_, false);
  }
  return *this;
}

CacheStore::~CacheStore() {
  if (directory_fd_ >= 0) {
    (void)::close(directory_fd_);
  }
  if (directory_lock_fd_ >= 0) {
    (void)::close(directory_lock_fd_);
  }
}

Result<CacheStore> CacheStore::adopt_directory_fd(int directory_fd, CacheFaultHook fault) {
  Fd owned(directory_fd);
  struct stat status{};
  if (directory_fd < 0 || ::fstat(directory_fd, &status) != 0 || !S_ISDIR(status.st_mode)) {
    return Result<CacheStore>::failure(
        {ErrorCode::invalid_argument, "cache fd is not a directory"});
  }
  // Ownership passes to CacheStore only after validation.
  const int pinned = ::fcntl(directory_fd, F_DUPFD_CLOEXEC, 0);
  if (pinned < 0) {
    return Result<CacheStore>::failure(io_error("pin cache directory"));
  }
  Fd pinned_owner(pinned);
  // flock is bound to an open-file description. openat(".") gives every
  // instance its own lock even when the adopted descriptor was a dup.
  Fd lock(::openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (lock.get() < 0) {
    return Result<CacheStore>::failure(io_error("open cache directory lock"));
  }
  return Result<CacheStore>::success(
      CacheStore({pinned_owner.release(), lock.release()}, std::move(fault)));
}

Result<void> CacheStore::put(const CacheKey& key, std::span<const std::byte> payload) {
  std::lock_guard lock(mutex_);
  DirectoryLock directory_lock(directory_lock_fd_);
  if (auto acquired = directory_lock.acquire(); !acquired) {
    return acquired;
  }
  if (payload.size() > max_payload_bytes) {
    return Result<void>::failure({ErrorCode::resource_exhausted, "cache entry exceeds size limit"});
  }
  const auto hash = digest(payload);
  if (!hash) {
    return Result<void>::failure(hash.error());
  }
  const auto key_hex = key.hex();
  auto name = temporary_name(key_hex);
  if (!name) {
    return Result<void>::failure(name.error());
  }
  const std::string final_name = key_hex + ".cache";
  Fd file(::openat(directory_fd_, name.value().c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (file.get() < 0) {
    return Result<void>::failure(io_error("create cache temporary file"));
  }
  struct Cleanup {
    int directory;
    const char* name;
    bool published = false;
    ~Cleanup() {
      if (!published) {
        (void)::unlinkat(directory, name, 0);
      }
    }
  } cleanup{directory_fd_, name.value().c_str()};
  if (fail(CacheWriteStage::temporary_created)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  std::array<std::uint8_t, header_bytes> header{};
  std::ranges::copy(magic, header.begin());
  put32(header.data() + 8, format_version);
  std::ranges::copy(key_hex, header.begin() + 12);
  put64(header.data() + 76, payload.size());
  std::ranges::copy(hash.value(), header.begin() + 84);
  auto written = write_all(file.get(), header);
  if (!written) {
    return written;
  }
  if (fail(CacheWriteStage::header_written)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  written = write_all(file.get(),
                      {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
  if (!written) {
    return written;
  }
  if (fail(CacheWriteStage::payload_written)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  if (::fsync(file.get()) != 0) {
    return Result<void>::failure(io_error("flush cache entry"));
  }
  if (fail(CacheWriteStage::file_flushed)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  if (::renameat(directory_fd_, name.value().c_str(), directory_fd_, final_name.c_str()) != 0) {
    return Result<void>::failure(io_error("publish cache entry"));
  }
  cleanup.published = true;
  if (fail(CacheWriteStage::entry_published)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  if (::fsync(directory_fd_) != 0) {
    return Result<void>::failure(io_error("flush cache directory"));
  }
  if (fail(CacheWriteStage::directory_flushed)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  return Result<void>::success();
}

Result<void> CacheStore::put_stream(const CacheKey& key, std::uint64_t payload_bytes,
                                    const CacheStreamProducer& producer) {
  if (!producer) {
    return Result<void>::failure({ErrorCode::invalid_argument, "missing cache producer"});
  }
  if (payload_bytes > max_stream_payload_bytes) {
    return Result<void>::failure(
        {ErrorCode::resource_exhausted, "cache stream exceeds size limit"});
  }
  auto context = foundation::Sha256::create();
  if (!context) {
    return Result<void>::failure(context.error());
  }
  const auto key_hex = key.hex();
  auto name = temporary_name(key_hex);
  if (!name) {
    return Result<void>::failure(name.error());
  }
  const std::string final_name = key_hex + ".cache";
  Fd file;
  struct Cleanup {
    int directory;
    int directory_lock;
    Fd* file;
    const char* name;
    bool armed = false;
    bool published = false;
    ~Cleanup() {
      if (armed && !published) {
        DirectoryLock lock(directory_lock);
        (void)lock.acquire();
        struct stat opened{};
        struct stat named{};
        if (::fstat(file->get(), &opened) == 0 &&
            ::fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
            opened.st_dev == named.st_dev && opened.st_ino == named.st_ino) {
          (void)::unlinkat(directory, name, 0);
        }
      }
    }
  } cleanup{directory_fd_, directory_lock_fd_, &file, name.value().c_str()};
  std::optional<EntryIdentity> original_identity;
  {
    std::lock_guard lock(mutex_);
    DirectoryLock directory_lock(directory_lock_fd_);
    if (auto acquired = directory_lock.acquire(); !acquired) {
      return acquired;
    }
    const auto inspected = inspect_entry_identity(directory_fd_, final_name);
    if (!inspected) {
      return Result<void>::failure(inspected.error());
    }
    original_identity = inspected.value();
    file.reset(::openat(directory_fd_, name.value().c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.get() < 0) {
      return Result<void>::failure(io_error("create cache temporary file"));
    }
    cleanup.armed = true;
    if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
      return Result<void>::failure(io_error("lease cache temporary file"));
    }
    if (fail(CacheWriteStage::temporary_created)) {
      return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
    }
  }
  std::array<std::uint8_t, header_bytes> header{};
  std::ranges::copy(magic, header.begin());
  put32(header.data() + 8, format_version);
  std::ranges::copy(key_hex, header.begin() + 12);
  put64(header.data() + 76, payload_bytes);
  auto written = write_all(file.get(), header);
  if (!written) {
    return written;
  }
  if (fail(CacheWriteStage::header_written)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  std::uint64_t appended = 0;
  std::optional<foundation::Error> append_error;
  const CacheChunkAppend append = [&](std::span<const std::byte> chunk) -> Result<void> {
    if (append_error) {
      return Result<void>::failure(*append_error);
    }
    if (chunk.empty() || chunk.size() > stream_chunk_bytes ||
        chunk.size() > payload_bytes - appended) {
      append_error = {ErrorCode::invalid_argument, "invalid cache stream chunk"};
      return Result<void>::failure(*append_error);
    }
    auto result =
        write_all(file.get(), {reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size()});
    if (!result) {
      append_error = result.error();
      return result;
    }
    if (!context.value().update(std::as_bytes(std::span(chunk.data(), chunk.size())))) {
      append_error = {ErrorCode::internal, "cache digest update failed"};
      return Result<void>::failure(*append_error);
    }
    appended += chunk.size();
    return Result<void>::success();
  };
  auto produced = producer(append);
  if (!produced) {
    return produced;
  }
  if (append_error) {
    return Result<void>::failure(*append_error);
  }
  if (appended != payload_bytes) {
    return Result<void>::failure({ErrorCode::invalid_argument, "incomplete cache stream"});
  }
  if (fail(CacheWriteStage::payload_written)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  auto hash = context.value().finish();
  if (!hash) {
    return Result<void>::failure(hash.error());
  }
  std::ranges::copy(hash.value(), header.begin() + 84);
  if (::lseek(file.get(), 0, SEEK_SET) != 0) {
    return Result<void>::failure(io_error("seek cache header"));
  }
  written = write_all(file.get(), header);
  if (!written) {
    return written;
  }
  if (::fsync(file.get()) != 0) {
    return Result<void>::failure(io_error("flush cache entry"));
  }
  if (fail(CacheWriteStage::file_flushed)) {
    return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
  }
  {
    std::lock_guard lock(mutex_);
    DirectoryLock directory_lock(directory_lock_fd_);
    if (auto acquired = directory_lock.acquire(); !acquired) {
      return acquired;
    }
    const auto current_identity = inspect_entry_identity(directory_fd_, final_name);
    if (!current_identity) {
      return Result<void>::failure(current_identity.error());
    }
    if (current_identity.value() != original_identity) {
      return Result<void>::failure(
          {ErrorCode::conflict, "newer cache entry was published during stream preparation"});
    }
    const auto temporary_identity = inspect_entry_identity(directory_fd_, name.value());
    struct stat file_status{};
    if (!temporary_identity || !temporary_identity.value() ||
        ::fstat(file.get(), &file_status) != 0 ||
        temporary_identity.value()->device != file_status.st_dev ||
        temporary_identity.value()->inode != file_status.st_ino) {
      return Result<void>::failure(
          {ErrorCode::conflict, "cache temporary file changed before publication"});
    }
    if (::renameat(directory_fd_, name.value().c_str(), directory_fd_, final_name.c_str()) != 0) {
      return Result<void>::failure(io_error("publish cache entry"));
    }
    cleanup.published = true;
    if (fail(CacheWriteStage::entry_published)) {
      return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
    }
    if (::fsync(directory_fd_) != 0) {
      return Result<void>::failure(io_error("flush cache directory"));
    }
    if (fail(CacheWriteStage::directory_flushed)) {
      return Result<void>::failure({ErrorCode::io_failure, "interrupted cache write"});
    }
  }
  return Result<void>::success();
}

Result<std::optional<std::vector<std::byte>>> CacheStore::get(const CacheKey& key) const {
  std::lock_guard lock(mutex_);
  DirectoryLock directory_lock(directory_lock_fd_);
  if (auto acquired = directory_lock.acquire(); !acquired) {
    return Result<std::optional<std::vector<std::byte>>>::failure(acquired.error());
  }
  const auto key_hex = key.hex();
  const auto name = key_hex + ".cache";
  Fd file(::openat(directory_fd_, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.get() < 0) {
    if (errno == ENOENT || errno == ELOOP) {
      return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
    }
    return Result<std::optional<std::vector<std::byte>>>::failure(io_error("open cache entry"));
  }
  struct stat status{};
  if (::fstat(file.get(), &status) != 0) {
    return Result<std::optional<std::vector<std::byte>>>::failure(io_error("inspect cache entry"));
  }
  if (!S_ISREG(status.st_mode) || status.st_size < static_cast<off_t>(header_bytes) ||
      status.st_size > static_cast<off_t>(header_bytes + max_payload_bytes)) {
    return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
  }
  std::array<std::uint8_t, header_bytes> header{};
  auto header_read = read_all(file.get(), header);
  if (!header_read) {
    return Result<std::optional<std::vector<std::byte>>>::failure(header_read.error());
  }
  if (!header_read.value() || !std::ranges::equal(magic, std::span(header).first(magic.size())) ||
      read32(header.data() + 8) != format_version ||
      !std::ranges::equal(key_hex, std::span(header).subspan(12, 64)) ||
      read64(header.data() + 76) != static_cast<std::uint64_t>(status.st_size) - header_bytes) {
    return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
  }
  const auto length = static_cast<std::size_t>(read64(header.data() + 76));
  std::vector<std::byte> payload(length);
  auto payload_read =
      read_all(file.get(), {reinterpret_cast<std::uint8_t*>(payload.data()), payload.size()});
  if (!payload_read) {
    return Result<std::optional<std::vector<std::byte>>>::failure(payload_read.error());
  }
  if (!payload_read.value()) {
    return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
  }
  std::uint8_t extra = 0;
  ssize_t extra_read = 0;
  do {
    // The final bounded read shares the same control-side serialization contract.
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
    extra_read = ::read(file.get(), &extra, 1);
  } while (extra_read < 0 && errno == EINTR);
  if (extra_read < 0) {
    return Result<std::optional<std::vector<std::byte>>>::failure(io_error("read cache end"));
  }
  if (extra_read != 0) {
    return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
  }
  const auto hash = digest(payload);
  if (!hash) {
    return Result<std::optional<std::vector<std::byte>>>::failure(hash.error());
  }
  if (!std::ranges::equal(hash.value(), std::span(header).subspan(84, 32))) {
    return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
  }
  const timespec access_times[2]{{0, UTIME_NOW}, {0, UTIME_OMIT}};
  if (::futimens(file.get(), access_times) != 0) {
    recency_degraded_ = true;
  }
  return Result<std::optional<std::vector<std::byte>>>::success(std::move(payload));
}

Result<std::optional<std::uint64_t>>
CacheStore::get_stream(const CacheKey& key, const CacheChunkConsumer& consumer) const {
  if (!consumer) {
    return Result<std::optional<std::uint64_t>>::failure(
        {ErrorCode::invalid_argument, "missing cache consumer"});
  }
  Fd file;
  std::uint64_t payload_bytes = 0;
  std::array<std::uint8_t, header_bytes> header{};
  std::array<std::uint8_t, stream_chunk_bytes> chunk{};
  {
    std::lock_guard lock(mutex_);
    DirectoryLock directory_lock(directory_lock_fd_);
    if (auto acquired = directory_lock.acquire(); !acquired) {
      return Result<std::optional<std::uint64_t>>::failure(acquired.error());
    }
    const auto key_hex = key.hex();
    const auto name = key_hex + ".cache";
    file.reset(::openat(directory_fd_, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) {
      if (errno == ENOENT || errno == ELOOP) {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
      }
      return Result<std::optional<std::uint64_t>>::failure(io_error("open cache entry"));
    }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) {
      return Result<std::optional<std::uint64_t>>::failure(io_error("inspect cache entry"));
    }
    if (!S_ISREG(status.st_mode) || status.st_size < static_cast<off_t>(header_bytes) ||
        status.st_size > static_cast<off_t>(header_bytes + max_stream_payload_bytes)) {
      return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    auto header_read = read_all(file.get(), header);
    if (!header_read) {
      return Result<std::optional<std::uint64_t>>::failure(header_read.error());
    }
    if (!header_read.value() || !std::ranges::equal(magic, std::span(header).first(magic.size())) ||
        read32(header.data() + 8) != format_version ||
        !std::ranges::equal(key_hex, std::span(header).subspan(12, 64)) ||
        read64(header.data() + 76) != static_cast<std::uint64_t>(status.st_size) - header_bytes) {
      return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    payload_bytes = read64(header.data() + 76);
  }
  // The opened inode is pinned. Managed writers replace by rename and
  // eviction unlinks, so neither can change bytes during validation or replay.
  auto context = foundation::Sha256::create();
  if (!context) {
    return Result<std::optional<std::uint64_t>>::failure(context.error());
  }
  std::uint64_t remaining = payload_bytes;
  while (remaining != 0) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));
    auto read = read_all(file.get(), std::span(chunk).first(count));
    if (!read) {
      return Result<std::optional<std::uint64_t>>::failure(read.error());
    }
    if (!read.value()) {
      return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    if (!context.value().update(std::as_bytes(std::span(chunk.data(), count)))) {
      return Result<std::optional<std::uint64_t>>::failure(
          {ErrorCode::internal, "cache digest update failed"});
    }
    remaining -= count;
  }
  std::uint8_t extra = 0;
  ssize_t extra_read = 0;
  do {
    extra_read = ::read(file.get(), &extra, 1);
  } while (extra_read < 0 && errno == EINTR);
  if (extra_read < 0) {
    return Result<std::optional<std::uint64_t>>::failure(io_error("read cache end"));
  }
  if (extra_read != 0) {
    return Result<std::optional<std::uint64_t>>::success(std::nullopt);
  }
  const auto hash = context.value().finish();
  if (!hash) {
    return Result<std::optional<std::uint64_t>>::failure(hash.error());
  }
  if (!std::ranges::equal(hash.value(), std::span(header).subspan(84, 32))) {
    return Result<std::optional<std::uint64_t>>::success(std::nullopt);
  }
  const timespec access_times[2]{{0, UTIME_NOW}, {0, UTIME_OMIT}};
  if (::futimens(file.get(), access_times) != 0) {
    std::lock_guard lock(mutex_);
    recency_degraded_ = true;
  }
  if (::lseek(file.get(), static_cast<off_t>(header_bytes), SEEK_SET) !=
      static_cast<off_t>(header_bytes)) {
    return Result<std::optional<std::uint64_t>>::failure(io_error("seek cache payload"));
  }
  remaining = payload_bytes;
  while (remaining != 0) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));
    auto read = read_all(file.get(), std::span(chunk).first(count));
    if (!read) {
      return Result<std::optional<std::uint64_t>>::failure(read.error());
    }
    if (!read.value()) {
      return Result<std::optional<std::uint64_t>>::failure(
          {ErrorCode::io_failure, "cache entry changed during validated read"});
    }
    const auto consumed = consumer({reinterpret_cast<const std::byte*>(chunk.data()), count});
    if (!consumed) {
      return Result<std::optional<std::uint64_t>>::failure(consumed.error());
    }
    remaining -= count;
  }
  return Result<std::optional<std::uint64_t>>::success(payload_bytes);
}

Result<std::optional<ValidatedCacheReader>> CacheStore::open_validated(const CacheKey& key) const {
  Fd file;
  {
    std::lock_guard lock(mutex_);
    DirectoryLock directory_lock(directory_lock_fd_);
    if (auto acquired = directory_lock.acquire(); !acquired) {
      return Result<std::optional<ValidatedCacheReader>>::failure(acquired.error());
    }
    const auto name = key.hex() + ".cache";
    file.reset(::openat(directory_fd_, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) {
      if (errno == ENOENT || errno == ELOOP) {
        return Result<std::optional<ValidatedCacheReader>>::success(std::nullopt);
      }
      return Result<std::optional<ValidatedCacheReader>>::failure(io_error("open cache entry"));
    }
  }
  // The inode is pinned and CacheStore only replaces entries by rename. No
  // store or directory lock is held during the full digest pass or later reads.
  struct stat before{};
  if (::fstat(file.get(), &before) != 0) {
    return Result<std::optional<ValidatedCacheReader>>::failure(io_error("inspect cache entry"));
  }
  if (!S_ISREG(before.st_mode) || before.st_size < static_cast<off_t>(header_bytes) ||
      before.st_size > static_cast<off_t>(header_bytes + max_stream_payload_bytes)) {
    return Result<std::optional<ValidatedCacheReader>>::success(std::nullopt);
  }
  std::array<std::uint8_t, header_bytes> header{};
  auto header_read = pread_all(file.get(), 0, header);
  if (!header_read) {
    return Result<std::optional<ValidatedCacheReader>>::failure(header_read.error());
  }
  const auto key_hex = key.hex();
  if (!header_read.value() || !std::ranges::equal(magic, std::span(header).first(magic.size())) ||
      read32(header.data() + 8) != format_version ||
      !std::ranges::equal(key_hex, std::span(header).subspan(12, 64)) ||
      read64(header.data() + 76) != static_cast<std::uint64_t>(before.st_size) - header_bytes) {
    return Result<std::optional<ValidatedCacheReader>>::success(std::nullopt);
  }
  const auto payload_bytes = read64(header.data() + 76);
  auto context = foundation::Sha256::create();
  if (!context) {
    return Result<std::optional<ValidatedCacheReader>>::failure(context.error());
  }
  std::array<std::uint8_t, stream_chunk_bytes> chunk{};
  std::uint64_t offset = 0;
  while (offset < payload_bytes) {
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(payload_bytes - offset, chunk.size()));
    auto read = pread_all(file.get(), header_bytes + offset, std::span(chunk).first(count));
    if (!read) {
      return Result<std::optional<ValidatedCacheReader>>::failure(read.error());
    }
    if (!read.value()) {
      return Result<std::optional<ValidatedCacheReader>>::success(std::nullopt);
    }
    if (!context.value().update(std::as_bytes(std::span(chunk.data(), count)))) {
      return Result<std::optional<ValidatedCacheReader>>::failure(
          {ErrorCode::internal, "cache digest update failed"});
    }
    offset += count;
  }
  struct stat after{};
  if (::fstat(file.get(), &after) != 0) {
    return Result<std::optional<ValidatedCacheReader>>::failure(io_error("inspect cache entry"));
  }
  if (!same_contents_metadata(before, after)) {
    return Result<std::optional<ValidatedCacheReader>>::success(std::nullopt);
  }
  auto hash = context.value().finish();
  if (!hash) {
    return Result<std::optional<ValidatedCacheReader>>::failure(hash.error());
  }
  if (!std::ranges::equal(hash.value(), std::span(header).subspan(84, 32))) {
    return Result<std::optional<ValidatedCacheReader>>::success(std::nullopt);
  }
  const timespec access_times[2]{{0, UTIME_NOW}, {0, UTIME_OMIT}};
  if (::futimens(file.get(), access_times) != 0) {
    std::lock_guard lock(mutex_);
    recency_degraded_ = true;
  }
  return Result<std::optional<ValidatedCacheReader>>::success(
      ValidatedCacheReader({file.release(), payload_bytes}));
}

Result<void> CacheStore::clear() {
  std::lock_guard lock(mutex_);
  DirectoryLock directory_lock(directory_lock_fd_);
  if (auto acquired = directory_lock.acquire(); !acquired) {
    return acquired;
  }
  const int scan_fd = ::openat(directory_fd_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (scan_fd < 0) {
    return Result<void>::failure(io_error("scan cache directory"));
  }
  DIR* directory = ::fdopendir(scan_fd);
  if (directory == nullptr) {
    (void)::close(scan_fd);
    return Result<void>::failure(io_error("open cache scan"));
  }
  bool removed = false;
  errno = 0;
  while (auto* entry = ::readdir(directory)) {
    const std::string_view name(entry->d_name);
    if (managed_name(name)) {
      struct stat status{};
      if (::fstatat(directory_fd_, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) == 0 &&
          !S_ISDIR(status.st_mode)) {
        const auto removal = remove_disposable_entry(directory_fd_, std::string(name),
                                                     name.size() == 85 && S_ISREG(status.st_mode));
        if (!removal) {
          const auto& error = removal.error();
          (void)::closedir(directory);
          return Result<void>::failure(error);
        }
        removed |= removal.value();
      }
    }
    errno = 0;
  }
  const int scan_error = errno;
  (void)::closedir(directory);
  if (scan_error != 0) {
    errno = scan_error;
    return Result<void>::failure(io_error("scan cache directory"));
  }
  if (removed && ::fsync(directory_fd_) != 0) {
    return Result<void>::failure(io_error("flush cleared cache"));
  }
  return Result<void>::success();
}

Result<CacheUsage> CacheStore::usage() const {
  std::lock_guard lock(mutex_);
  DirectoryLock directory_lock(directory_lock_fd_);
  if (auto acquired = directory_lock.acquire(); !acquired) {
    return Result<CacheUsage>::failure(acquired.error());
  }
  const auto entries = scan_entries(directory_fd_);
  if (!entries) {
    return Result<CacheUsage>::failure(entries.error());
  }
  return count_usage(entries.value(), recency_degraded_);
}

Result<CacheBudgetResult> CacheStore::enforce_budget(std::uint64_t max_bytes) {
  std::lock_guard lock(mutex_);
  DirectoryLock directory_lock(directory_lock_fd_);
  if (auto acquired = directory_lock.acquire(); !acquired) {
    return Result<CacheBudgetResult>::failure(acquired.error());
  }
  auto entries = scan_entries(directory_fd_);
  if (!entries) {
    return Result<CacheBudgetResult>::failure(entries.error());
  }
  auto before = count_usage(entries.value(), recency_degraded_);
  if (!before) {
    return Result<CacheBudgetResult>::failure(before.error());
  }
  std::ranges::sort(entries.value(), [](const ScannedEntry& left, const ScannedEntry& right) {
    if (left.temporary != right.temporary) {
      return left.temporary;
    }
    if (left.accessed.tv_sec != right.accessed.tv_sec) {
      return left.accessed.tv_sec < right.accessed.tv_sec;
    }
    if (left.accessed.tv_nsec != right.accessed.tv_nsec) {
      return left.accessed.tv_nsec < right.accessed.tv_nsec;
    }
    return left.name < right.name;
  });
  std::uint64_t remaining = before.value().bytes;
  std::uint64_t evicted = 0;
  for (const auto& entry : entries.value()) {
    if (!entry.temporary && remaining <= max_bytes) {
      break;
    }
    const auto removal = remove_disposable_entry(directory_fd_, entry.name, entry.temporary);
    if (!removal) {
      return Result<CacheBudgetResult>::failure(removal.error());
    }
    if (removal.value()) {
      remaining -= entry.bytes;
      ++evicted;
    }
  }
  if (evicted != 0 && ::fsync(directory_fd_) != 0) {
    return Result<CacheBudgetResult>::failure(io_error("flush cache eviction"));
  }
  const auto after_entries = scan_entries(directory_fd_);
  if (!after_entries) {
    return Result<CacheBudgetResult>::failure(after_entries.error());
  }
  auto after = count_usage(after_entries.value(), recency_degraded_);
  if (!after) {
    return Result<CacheBudgetResult>::failure(after.error());
  }
  return Result<CacheBudgetResult>::success({before.value(), after.value(), evicted});
}

Result<CacheBudgetResult> enforce_global_cache_budget(std::span<CacheStore* const> stores,
                                                      std::uint64_t max_bytes) {
  if (stores.size() > max_global_stores) {
    return Result<CacheBudgetResult>::failure(
        {ErrorCode::resource_exhausted, "global cache store limit exceeded"});
  }
  std::vector<CacheStore*> lock_order(stores.begin(), stores.end());
  if (std::ranges::any_of(lock_order, [](const CacheStore* store) { return store == nullptr; })) {
    return Result<CacheBudgetResult>::failure(
        {ErrorCode::invalid_argument, "null global cache store"});
  }
  std::ranges::sort(lock_order);
  if (std::ranges::adjacent_find(lock_order) != lock_order.end()) {
    return Result<CacheBudgetResult>::failure(
        {ErrorCode::invalid_argument, "duplicate global cache store"});
  }
  // A fixed lock order permits concurrent per-store operations without a
  // coordinator lock held by the callback or a deadlock between coordinators.
  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(lock_order.size());
  for (auto* store : lock_order) {
    locks.emplace_back(store->mutex_);
  }

  // Distinct instances can pin the same underlying cache directory. Acquire
  // kernel locks in device/inode order before scanning or deleting anything,
  // independent of the caller's stable eviction-tie order.
  using DirectoryIdentity = std::pair<dev_t, ino_t>;
  std::vector<std::pair<DirectoryIdentity, CacheStore*>> directory_order;
  directory_order.reserve(stores.size());
  for (auto* store : stores) {
    struct stat directory_status{};
    if (::fstat(store->directory_fd_, &directory_status) != 0) {
      return Result<CacheBudgetResult>::failure(io_error("inspect global cache directory"));
    }
    directory_order.emplace_back(
        DirectoryIdentity{directory_status.st_dev, directory_status.st_ino}, store);
  }
  std::ranges::sort(directory_order,
                    [](const auto& left, const auto& right) { return left.first < right.first; });
  if (std::ranges::adjacent_find(directory_order, [](const auto& left, const auto& right) {
        return left.first == right.first;
      }) != directory_order.end()) {
    return Result<CacheBudgetResult>::failure(
        {ErrorCode::invalid_argument, "duplicate global cache directory"});
  }
  std::vector<DirectoryLock> directory_locks;
  directory_locks.reserve(directory_order.size());
  for (const auto& ordered : directory_order) {
    directory_locks.emplace_back(ordered.second->directory_lock_fd_);
    if (auto acquired = directory_locks.back().acquire(); !acquired) {
      return Result<CacheBudgetResult>::failure(acquired.error());
    }
  }

  struct GlobalEntry {
    std::size_t project;
    ScannedEntry entry;
  };
  std::vector<GlobalEntry> entries;
  CacheUsage before{};
  for (std::size_t project = 0; project < stores.size(); ++project) {
    auto* store = stores[project];
    const auto scanned = scan_entries(store->directory_fd_);
    if (!scanned) {
      return Result<CacheBudgetResult>::failure(scanned.error());
    }
    before.recency_degraded |= store->recency_degraded_;
    for (const auto& entry : scanned.value()) {
      if (entry.bytes > std::numeric_limits<std::uint64_t>::max() - before.bytes ||
          before.entries == std::numeric_limits<std::uint64_t>::max() ||
          entries.size() == max_scanned_entries) {
        return Result<CacheBudgetResult>::failure(
            {ErrorCode::resource_exhausted, "global cache scan limit exceeded"});
      }
      before.bytes += entry.bytes;
      ++before.entries;
      entries.push_back({project, entry});
    }
  }
  std::ranges::sort(entries, [](const GlobalEntry& left, const GlobalEntry& right) {
    if (left.entry.temporary != right.entry.temporary) {
      return left.entry.temporary;
    }
    if (left.entry.accessed.tv_sec != right.entry.accessed.tv_sec) {
      return left.entry.accessed.tv_sec < right.entry.accessed.tv_sec;
    }
    if (left.entry.accessed.tv_nsec != right.entry.accessed.tv_nsec) {
      return left.entry.accessed.tv_nsec < right.entry.accessed.tv_nsec;
    }
    if (left.project != right.project) {
      return left.project < right.project;
    }
    return left.entry.name < right.entry.name;
  });

  std::vector<bool> modified(stores.size());
  std::uint64_t remaining = before.bytes;
  std::uint64_t evicted = 0;
  for (const auto& candidate : entries) {
    if (!candidate.entry.temporary && remaining <= max_bytes) {
      break;
    }
    const int directory_fd = stores[candidate.project]->directory_fd_;
    const auto removal =
        remove_disposable_entry(directory_fd, candidate.entry.name, candidate.entry.temporary);
    if (!removal) {
      return Result<CacheBudgetResult>::failure(removal.error());
    }
    if (removal.value()) {
      remaining -= candidate.entry.bytes;
      ++evicted;
      modified[candidate.project] = true;
    }
  }
  for (std::size_t project = 0; project < stores.size(); ++project) {
    if (modified[project] && ::fsync(stores[project]->directory_fd_) != 0) {
      return Result<CacheBudgetResult>::failure(io_error("flush global cache eviction"));
    }
  }
  CacheUsage after{0, 0, before.recency_degraded};
  for (auto* store : stores) {
    const auto scanned = scan_entries(store->directory_fd_);
    if (!scanned) {
      return Result<CacheBudgetResult>::failure(scanned.error());
    }
    const auto usage = count_usage(scanned.value(), store->recency_degraded_);
    if (!usage) {
      return Result<CacheBudgetResult>::failure(usage.error());
    }
    if (usage.value().bytes > std::numeric_limits<std::uint64_t>::max() - after.bytes ||
        usage.value().entries > std::numeric_limits<std::uint64_t>::max() - after.entries) {
      return Result<CacheBudgetResult>::failure(
          {ErrorCode::resource_exhausted, "global cache usage overflow"});
    }
    after.bytes += usage.value().bytes;
    after.entries += usage.value().entries;
  }
  return Result<CacheBudgetResult>::success({before, after, evicted});
}

} // namespace oscillate::media
