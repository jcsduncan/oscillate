#include "oscillate/application/cache_control.hpp"

#include "oscillate/foundation/file_io.hpp"
#include "oscillate/persistence/store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace oscillate::application {
namespace {
using foundation::ErrorCode;
using foundation::Result;

constexpr char registry_name[] = "cache-registry.json";
constexpr std::uint64_t default_budget = 20ULL * 1024 * 1024 * 1024;
constexpr std::size_t max_registry_bytes = std::size_t{1024} * 1024;
constexpr std::size_t max_projects = 256;
constexpr std::size_t max_path_bytes = 4096;

using Fd = foundation::UniqueFd;
using foundation::io_error;

[[nodiscard]] bool valid_root(const std::filesystem::path& root) {
  const auto text = root.string();
  return root.is_absolute() && !text.empty() && text.size() <= max_path_bytes &&
         text.find('\0') == std::string::npos && root == root.lexically_normal() &&
         std::ranges::none_of(root, [](const auto& part) { return part == ".."; });
}

[[nodiscard]] Result<std::optional<std::string>> read_registry(int directory_fd) {
  Fd file(::openat(directory_fd, registry_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.get() < 0) {
    if (errno == ENOENT) {
      return Result<std::optional<std::string>>::success(std::nullopt);
    }
    return Result<std::optional<std::string>>::failure(io_error("open cache registry"));
  }
  struct stat status{};
  if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
      status.st_size > static_cast<off_t>(max_registry_bytes)) {
    return Result<std::optional<std::string>>::failure(
        {ErrorCode::corrupt_data, "invalid cache registry file"});
  }
  std::string data(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto count = ::read(file.get(), data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return Result<std::optional<std::string>>::failure(io_error("read cache registry"));
    }
    offset += static_cast<std::size_t>(count);
  }
  return Result<std::optional<std::string>>::success(std::move(data));
}

struct Inspection {
  CacheProjectStatus status = CacheProjectStatus::unreachable;
  std::string detail;
  std::unique_ptr<media::CacheStore> store;
};

using DeviceInode = std::pair<std::uint64_t, std::uint64_t>;

[[nodiscard]] Inspection inspect(const std::filesystem::path& root, DeviceInode root_identity,
                                 DeviceInode cache_identity) {
  struct stat root_status{};
  if (::lstat(root.c_str(), &root_status) != 0) {
    return {CacheProjectStatus::unreachable, "Project path is unavailable", {}};
  }
  if (!S_ISDIR(root_status.st_mode) ||
      static_cast<std::uint64_t>(root_status.st_dev) != root_identity.first ||
      static_cast<std::uint64_t>(root_status.st_ino) != root_identity.second) {
    return {CacheProjectStatus::moved, "Project path no longer names the registered directory", {}};
  }
  auto project = persistence::ProjectStore::open_reader(root);
  if (!project) {
    return {CacheProjectStatus::failed, project.error().message, {}};
  }
  auto current = project.value().load_current();
  if (!current) {
    return {CacheProjectStatus::failed, current.error().message, {}};
  }
  auto cache_fd = project.value().open_cache_directory();
  if (!cache_fd) {
    return {CacheProjectStatus::cache_unavailable, cache_fd.error().message, {}};
  }
  Fd pinned(cache_fd.value());
  struct stat cache_status{};
  if (::fstat(pinned.get(), &cache_status) != 0 || !S_ISDIR(cache_status.st_mode)) {
    return {CacheProjectStatus::cache_unavailable, "Project cache directory is unavailable", {}};
  }
  if (static_cast<std::uint64_t>(cache_status.st_dev) != cache_identity.first ||
      static_cast<std::uint64_t>(cache_status.st_ino) != cache_identity.second) {
    return {CacheProjectStatus::cache_unavailable,
            "Project cache directory changed; reopen the project to register it",
            {}};
  }
  struct stat still_registered{};
  if (::lstat(root.c_str(), &still_registered) != 0 ||
      still_registered.st_dev != root_status.st_dev ||
      still_registered.st_ino != root_status.st_ino) {
    return {CacheProjectStatus::moved, "Project moved during cache inspection", {}};
  }
  auto adopted = media::CacheStore::adopt_directory_fd(pinned.release());
  if (!adopted) {
    return {CacheProjectStatus::failed, adopted.error().message, {}};
  }
  return {CacheProjectStatus::reachable,
          {},
          std::make_unique<media::CacheStore>(std::move(adopted).value())};
}

} // namespace

CacheControl::CacheControl(Descriptors descriptors, std::vector<Entry> entries,
                           std::uint64_t budget_bytes)
    : directory_fd_(descriptors.directory), lock_fd_(descriptors.lock),
      entries_(std::move(entries)), budget_bytes_(budget_bytes) {}

CacheControl::~CacheControl() {
  if (lock_fd_ >= 0) {
    ::close(lock_fd_);
  }
  if (directory_fd_ >= 0) {
    ::close(directory_fd_);
  }
}

Result<std::unique_ptr<CacheControl>>
CacheControl::open(const std::filesystem::path& profile_directory) {
  if (!valid_root(profile_directory)) {
    return Result<std::unique_ptr<CacheControl>>::failure(
        {ErrorCode::invalid_argument,
         "cache profile directory must be a normalized absolute path"});
  }
  std::error_code error;
  std::filesystem::create_directories(profile_directory, error);
  if (error) {
    return Result<std::unique_ptr<CacheControl>>::failure(
        {ErrorCode::io_failure, "create cache profile directory: " + error.message()});
  }
  Fd directory(::open(profile_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (directory.get() < 0) {
    return Result<std::unique_ptr<CacheControl>>::failure(io_error("open cache profile directory"));
  }
  Fd lock(::openat(directory.get(), "cache-registry.lock",
                   O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
  struct stat lock_status{};
  if (lock.get() < 0 || ::fstat(lock.get(), &lock_status) != 0 || !S_ISREG(lock_status.st_mode)) {
    return Result<std::unique_ptr<CacheControl>>::failure(io_error("open cache registry lock"));
  }
  if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
    return Result<std::unique_ptr<CacheControl>>::failure(
        {ErrorCode::conflict, "another cache registry controller is active"});
  }
  auto data = read_registry(directory.get());
  if (!data) {
    return Result<std::unique_ptr<CacheControl>>::failure(data.error());
  }
  std::vector<Entry> entries;
  std::uint64_t budget = default_budget;
  if (data.value().has_value()) {
    try {
      const auto document = nlohmann::json::parse(data.value().value_or(std::string{}));
      if (!document.is_object() || document.at("version").get<int>() != 1 ||
          !document.at("projects").is_array() || document.at("projects").size() > max_projects) {
        throw std::runtime_error("invalid cache registry schema");
      }
      // A whole, non-negative number: a stored -1 or 1.5 is corrupt, not a budget.
      const auto& stored_budget = document.at("budget_bytes");
      if (!stored_budget.is_number_unsigned()) {
        throw std::runtime_error("invalid cache registry budget");
      }
      budget = stored_budget.get<std::uint64_t>();
      for (const auto& item : document.at("projects")) {
        Entry entry{item.at("root").get<std::string>(), item.at("root_device").get<std::uint64_t>(),
                    item.at("root_inode").get<std::uint64_t>(),
                    item.at("cache_device").get<std::uint64_t>(),
                    item.at("cache_inode").get<std::uint64_t>()};
        if (!valid_root(entry.root) ||
            std::ranges::any_of(entries, [&entry](const Entry& existing) {
              return existing.root == entry.root || (existing.root_device == entry.root_device &&
                                                     existing.root_inode == entry.root_inode);
            })) {
          throw std::runtime_error("invalid cache registry project");
        }
        entries.push_back(std::move(entry));
      }
    } catch (const std::exception&) {
      return Result<std::unique_ptr<CacheControl>>::failure(
          {ErrorCode::corrupt_data, "invalid cache registry document"});
    }
  }
  return Result<std::unique_ptr<CacheControl>>::success(std::unique_ptr<CacheControl>(
      new CacheControl({directory.release(), lock.release()}, std::move(entries), budget)));
}

Result<void> CacheControl::persist(const std::vector<Entry>& entries, std::uint64_t budget_bytes,
                                   bool& published) const {
  published = false;
  nlohmann::json document{
      {"version", 1}, {"budget_bytes", budget_bytes}, {"projects", nlohmann::json::array()}};
  for (const auto& entry : entries) {
    document["projects"].push_back({{"root", entry.root.string()},
                                    {"root_device", entry.root_device},
                                    {"root_inode", entry.root_inode},
                                    {"cache_device", entry.cache_device},
                                    {"cache_inode", entry.cache_inode}});
  }
  const auto bytes = document.dump(2);
  if (bytes.size() > max_registry_bytes) {
    return Result<void>::failure({ErrorCode::resource_exhausted, "cache registry size limit"});
  }
  static std::atomic<std::uint64_t> nonce{0};
  const auto temporary = ".cache-registry." + std::to_string(::getpid()) + "." +
                         std::to_string(nonce.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
  auto replaced = foundation::replace_file(directory_fd_, temporary, registry_name,
                                           std::as_bytes(std::span(bytes.data(), bytes.size())),
                                           "cache registry");
  if (!replaced) {
    return Result<void>::failure(std::move(replaced.error()));
  }
  published = true;
  if (replaced.value() != foundation::Durability::durable) {
    return Result<void>::failure(
        {ErrorCode::io_failure, "flush cache registry directory: durability is uncertain"});
  }
  return Result<void>::success();
}

Result<void> CacheControl::register_project(const std::filesystem::path& root) {
  if (!valid_root(root)) {
    return Result<void>::failure({ErrorCode::invalid_argument, "invalid project registry path"});
  }
  struct stat root_status{};
  if (::lstat(root.c_str(), &root_status) != 0 || !S_ISDIR(root_status.st_mode)) {
    return Result<void>::failure(io_error("inspect project registry path"));
  }
  auto project = persistence::ProjectStore::open_reader(root);
  if (!project) {
    return Result<void>::failure(project.error());
  }
  auto current = project.value().load_current();
  if (!current) {
    return Result<void>::failure(current.error());
  }
  auto cache_fd = project.value().open_cache_directory();
  if (!cache_fd) {
    return Result<void>::failure(cache_fd.error());
  }
  Fd cache(cache_fd.value());
  struct stat cache_status{};
  struct stat still_root{};
  if (::fstat(cache.get(), &cache_status) != 0 || ::lstat(root.c_str(), &still_root) != 0 ||
      !S_ISDIR(cache_status.st_mode) || still_root.st_dev != root_status.st_dev ||
      still_root.st_ino != root_status.st_ino) {
    return Result<void>::failure({ErrorCode::conflict, "project changed during registration"});
  }
  Entry entry{root, static_cast<std::uint64_t>(root_status.st_dev),
              static_cast<std::uint64_t>(root_status.st_ino),
              static_cast<std::uint64_t>(cache_status.st_dev),
              static_cast<std::uint64_t>(cache_status.st_ino)};
  std::lock_guard lock(mutex_);
  auto updated = entries_;
  const auto matches = [&entry](const Entry& existing) {
    return existing.root == entry.root ||
           (existing.root_device == entry.root_device && existing.root_inode == entry.root_inode);
  };
  auto found = std::ranges::find_if(updated, matches);
  if (found != updated.end()) {
    // A project moved onto another's former path matches two entries (one
    // by identity, one by path): keep the first, drop the rest.
    const auto keep = static_cast<std::size_t>(found - updated.begin());
    const auto tail = std::remove_if(found + 1, updated.end(), matches);
    updated.erase(tail, updated.end());
    found = updated.begin() + static_cast<std::ptrdiff_t>(keep);
  }
  if (found == updated.end()) {
    if (updated.size() == max_projects) {
      return Result<void>::failure({ErrorCode::resource_exhausted, "cache project limit"});
    }
    updated.push_back(std::move(entry));
  } else {
    *found = std::move(entry);
  }
  bool published = false;
  auto saved = persist(updated, budget_bytes_, published);
  if (saved || published) {
    entries_ = std::move(updated);
  }
  return saved;
}

Result<void> CacheControl::forget_project(const std::filesystem::path& root) {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find(entries_, root, &Entry::root);
  if (found == entries_.end()) {
    return Result<void>::failure({ErrorCode::not_found, "project is not registered"});
  }
  auto updated = entries_;
  updated.erase(updated.begin() + (found - entries_.begin()));
  bool published = false;
  auto saved = persist(updated, budget_bytes_, published);
  if (saved || published) {
    entries_ = std::move(updated);
  }
  return saved;
}

Result<CacheHealth> CacheControl::health(media::CacheStore* shared_cache) const {
  std::lock_guard lock(mutex_);
  CacheHealth health{budget_bytes_, {}, {}};
  health.projects.reserve(entries_.size());
  for (const auto& entry : entries_) {
    auto inspected = inspect(entry.root, {entry.root_device, entry.root_inode},
                             {entry.cache_device, entry.cache_inode});
    CacheProjectHealth project{entry.root, inspected.status, {}, std::move(inspected.detail)};
    if (inspected.store) {
      auto usage = inspected.store->usage();
      if (!usage) {
        project.status = CacheProjectStatus::failed;
        project.detail = usage.error().message;
      } else {
        project.usage = usage.value();
        if (project.usage.bytes > std::numeric_limits<std::uint64_t>::max() - health.global.bytes ||
            project.usage.entries >
                std::numeric_limits<std::uint64_t>::max() - health.global.entries) {
          return Result<CacheHealth>::failure(
              {ErrorCode::resource_exhausted, "global cache usage overflow"});
        }
        health.global.bytes += project.usage.bytes;
        health.global.entries += project.usage.entries;
        health.global.recency_degraded |= project.usage.recency_degraded;
      }
    }
    health.projects.push_back(std::move(project));
  }
  if (shared_cache) {
    auto usage = shared_cache->usage();
    if (!usage) {
      return Result<CacheHealth>::failure(usage.error());
    }
    if (usage.value().bytes > std::numeric_limits<std::uint64_t>::max() - health.global.bytes ||
        usage.value().entries > std::numeric_limits<std::uint64_t>::max() - health.global.entries) {
      return Result<CacheHealth>::failure(
          {ErrorCode::resource_exhausted, "global cache usage overflow"});
    }
    health.global.bytes += usage.value().bytes;
    health.global.entries += usage.value().entries;
    health.global.recency_degraded |= usage.value().recency_degraded;
  }
  return Result<CacheHealth>::success(std::move(health));
}

Result<void> CacheControl::set_budget(std::uint64_t bytes) {
  std::lock_guard lock(mutex_);
  bool published = false;
  auto saved = persist(entries_, bytes, published);
  if (saved || published) {
    budget_bytes_ = bytes;
  }
  return saved;
}

Result<media::CacheBudgetResult> CacheControl::enforce_budget(media::CacheStore* shared_cache) {
  std::lock_guard lock(mutex_);
  std::vector<std::unique_ptr<media::CacheStore>> owned;
  std::vector<media::CacheStore*> stores;
  for (const auto& entry : entries_) {
    auto inspected = inspect(entry.root, {entry.root_device, entry.root_inode},
                             {entry.cache_device, entry.cache_inode});
    if (inspected.status == CacheProjectStatus::failed) {
      return Result<media::CacheBudgetResult>::failure(
          {ErrorCode::unavailable,
           "registered project cache inspection failed: " + inspected.detail});
    }
    if (inspected.store) {
      stores.push_back(inspected.store.get());
      owned.push_back(std::move(inspected.store));
    }
  }
  if (shared_cache) {
    stores.push_back(shared_cache);
  }
  return media::enforce_global_cache_budget(stores, budget_bytes_);
}

Result<void> CacheControl::clear_project(const std::filesystem::path& root) {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find(entries_, root, &Entry::root);
  if (found == entries_.end()) {
    return Result<void>::failure({ErrorCode::not_found, "project is not registered"});
  }
  auto inspected = inspect(found->root, {found->root_device, found->root_inode},
                           {found->cache_device, found->cache_inode});
  if (!inspected.store) {
    return Result<void>::failure({ErrorCode::unavailable, inspected.detail});
  }
  return inspected.store->clear();
}

Result<std::uint64_t> CacheControl::clear_reachable() {
  std::lock_guard lock(mutex_);
  std::uint64_t removed = 0;
  for (const auto& entry : entries_) {
    auto inspected = inspect(entry.root, {entry.root_device, entry.root_inode},
                             {entry.cache_device, entry.cache_inode});
    if (inspected.status == CacheProjectStatus::failed) {
      return Result<std::uint64_t>::failure(
          {ErrorCode::unavailable,
           "registered project cache inspection failed: " + inspected.detail});
    }
    if (!inspected.store) {
      continue;
    }
    auto usage = inspected.store->usage();
    if (!usage) {
      return Result<std::uint64_t>::failure(usage.error());
    }
    if (auto cleared = inspected.store->clear(); !cleared) {
      return Result<std::uint64_t>::failure(cleared.error());
    }
    removed += usage.value().entries;
  }
  return Result<std::uint64_t>::success(removed);
}

} // namespace oscillate::application
