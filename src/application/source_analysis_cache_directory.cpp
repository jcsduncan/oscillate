#include "oscillate/application/source_analysis_cache_directory.hpp"

#include "oscillate/foundation/file_io.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace oscillate::application {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr std::uint64_t source_cache_budget_bytes = 2ULL * 1024 * 1024 * 1024;
constexpr std::size_t max_path_bytes = 4096;

using Fd = foundation::UniqueFd;
using foundation::io_error;

[[nodiscard]] bool valid_path(const std::filesystem::path& path) {
  const auto& value = path.native();
  return path.is_absolute() && path == path.lexically_normal() && value.size() <= max_path_bytes &&
         value.find('\0') == std::string::npos;
}

[[nodiscard]] Result<Fd> child_directory(int parent, std::string_view name, bool private_mode,
                                         bool protected_by_private_ancestor = false) {
  const std::string component(name);
  if (::mkdirat(parent, component.c_str(), 0700) != 0 && errno != EEXIST) {
    return Result<Fd>::failure(io_error("create source analysis cache directory"));
  }
  Fd child(::openat(parent, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (child.get() < 0) {
    return Result<Fd>::failure(io_error("open source analysis cache directory"));
  }
  struct stat status{};
  if (::fstat(child.get(), &status) != 0) {
    return Result<Fd>::failure(io_error("inspect source analysis cache directory"));
  }
  const bool trusted_owner = status.st_uid == ::geteuid() || status.st_uid == 0;
  const bool shared_parent = status.st_uid == 0 && (status.st_mode & S_ISVTX) != 0;
  // A user-owned, group-writable parent remains safe when an earlier directory
  // in this path is private: other users cannot traverse into it. Never accept
  // world-writable cache parents or relax the private analysis subtree.
  const bool protected_group_writable =
      protected_by_private_ancestor && status.st_uid == ::geteuid() && (status.st_mode & 002) == 0;
  const bool safe_parent =
      trusted_owner && ((status.st_mode & 022) == 0 || shared_parent || protected_group_writable);
  if (!S_ISDIR(status.st_mode) ||
      (private_mode ? (status.st_uid != ::geteuid() || (status.st_mode & 077) != 0)
                    : !safe_parent)) {
    return Result<Fd>::failure(
        {ErrorCode::conflict,
         "source analysis cache directory is not private to this user: " + component});
  }
  return Result<Fd>::success(std::move(child));
}

struct CacheHome {
  Fd directory;
  bool protected_path = false;
};

[[nodiscard]] Result<CacheHome> open_cache_home(const std::filesystem::path& path) {
  if (!valid_path(path)) {
    return Result<CacheHome>::failure({ErrorCode::invalid_argument, "invalid XDG cache home"});
  }
  Fd parent(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (parent.get() < 0)
    return Result<CacheHome>::failure(io_error("open filesystem root"));
  bool protected_path = false;
  for (const auto& item : path.relative_path()) {
    if (item.empty() || item == "." || item == "..") {
      return Result<CacheHome>::failure({ErrorCode::invalid_argument, "invalid XDG cache home"});
    }
    auto child = child_directory(parent.get(), item.string(), false, protected_path);
    if (!child)
      return Result<CacheHome>::failure(child.error());
    parent = std::move(child).value();
    struct stat status{};
    if (::fstat(parent.get(), &status) != 0)
      return Result<CacheHome>::failure(io_error("inspect XDG cache path"));
    protected_path =
        protected_path || (status.st_uid == ::geteuid() && (status.st_mode & 077) == 0);
  }
  struct stat status{};
  if (::fstat(parent.get(), &status) != 0) {
    return Result<CacheHome>::failure(io_error("inspect XDG cache home"));
  }
  if (status.st_uid != ::geteuid() || (status.st_mode & 002) != 0 ||
      ((status.st_mode & 020) != 0 && !protected_path)) {
    return Result<CacheHome>::failure({ErrorCode::conflict, "XDG cache home is not owned safely"});
  }
  return Result<CacheHome>::success({std::move(parent), protected_path});
}
} // namespace

Result<media::CacheStore> open_user_source_analysis_cache(const std::filesystem::path& cache_home) {
  auto home = open_cache_home(cache_home);
  if (!home)
    return Result<media::CacheStore>::failure(home.error());
  // Qt may create the application's general XDG cache directory first with
  // ordinary user-owned directory permissions. Keep the analysis subtree
  // private; the shared application parent need only be safe to traverse.
  auto app = child_directory(home.value().directory.get(), "oscillate", false,
                             home.value().protected_path);
  if (!app)
    return Result<media::CacheStore>::failure(app.error());
  auto analysis = child_directory(app.value().get(), "source-analysis", true);
  if (!analysis)
    return Result<media::CacheStore>::failure(analysis.error());
  auto version = child_directory(analysis.value().get(), "v1", true);
  if (!version)
    return Result<media::CacheStore>::failure(version.error());
  return media::CacheStore::adopt_directory_fd(version.value().release());
}

Result<media::CacheStore> open_user_source_analysis_cache() {
  const auto* xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg != nullptr && *xdg != '\0') {
    const std::filesystem::path candidate(xdg);
    if (candidate.is_absolute())
      return open_user_source_analysis_cache(candidate);
  }
  const auto* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return Result<media::CacheStore>::failure(
        {ErrorCode::unavailable, "cannot locate user cache home"});
  }
  return open_user_source_analysis_cache(std::filesystem::path(home) / ".cache");
}

foundation::Result<void> enforce_source_analysis_cache_budget(media::CacheStore& cache) {
  auto enforced = cache.enforce_budget(source_cache_budget_bytes);
  return enforced ? foundation::Result<void>::success()
                  : foundation::Result<void>::failure(enforced.error());
}

} // namespace oscillate::application
