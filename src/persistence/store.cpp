#include "oscillate/persistence/store.hpp"

#include "store_support.hpp"

#include "private.hpp"

#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Opening, forking and closing a project store, and the abandoned-stage
// clean-up a writer runs when it opens.
namespace oscillate::persistence {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using detail::check_writer_lock;
using detail::Fd;
using detail::hex_name;
using detail::injected;
using detail::max_recovery_entries;
using detail::open_directory;
using detail::random_name;
using foundation::io_error;

Result<void> ensure_directory(int parent, const char* name, bool create) {
  if (create && ::mkdirat(parent, name, 0700) != 0 && errno != EEXIST) {
    return Result<void>::failure(io_error("create project directory"));
  }
  return Result<void>::success();
}

Result<void> clean_abandoned_wav_stages(int assets_fd) {
  Fd scan_fd(::openat(assets_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!scan_fd)
    return Result<void>::failure(io_error("scan staged sources"));
  const int directory_fd = scan_fd.release();
  struct Directory {
    DIR* value;
    ~Directory() {
      if (value)
        ::closedir(value);
    }
  } directory{::fdopendir(directory_fd)};
  if (!directory.value) {
    ::close(directory_fd);
    return Result<void>::failure(io_error("scan staged sources"));
  }
  // Opening the sole writer is the cleanup boundary for stages abandoned by a
  // terminated process. Never touch a published hash-named asset or a symlink.
  std::size_t scanned = 0;
  bool removed = false;
  for (;;) {
    errno = 0;
    auto* entry = ::readdir(directory.value);
    if (!entry) {
      if (errno != 0)
        return Result<void>::failure(io_error("scan staged sources"));
      break;
    }
    if (++scanned > max_recovery_entries)
      return Result<void>::failure(
          {ErrorCode::resource_exhausted, "too many entries for source stage cleanup"});
    const std::string_view name(entry->d_name);
    if (!name.starts_with(".stage-") || !hex_name(name.substr(7)))
      continue;
    struct stat status{};
    if (::fstatat(assets_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(status.st_mode))
      continue;
    if (::unlinkat(assets_fd, entry->d_name, 0) != 0 && errno != ENOENT)
      return Result<void>::failure(io_error("remove abandoned staged source"));
    removed = true;
  }
  if (removed && ::fsync(assets_fd) != 0)
    return Result<void>::failure(io_error("flush source stage cleanup"));
  return Result<void>::success();
}

struct DirectoryCloser {
  void operator()(DIR* stream) const noexcept { (void)::closedir(stream); }
};

using DirectoryStream = std::unique_ptr<DIR, DirectoryCloser>;

Result<void> visit_directory_entries(int directory_fd, const char* operation,
                                     const std::function<Result<void>(const char*)>& visit) {
  Fd scan_fd(::openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!scan_fd)
    return Result<void>::failure(io_error(operation));
  const int raw_fd = scan_fd.release();
  DIR* raw = ::fdopendir(raw_fd);
  if (!raw) {
    const auto failure = io_error(operation);
    ::close(raw_fd);
    return Result<void>::failure(failure);
  }
  DirectoryStream stream(raw);
  std::size_t scanned = 0;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(stream.get());
    if (!entry) {
      if (errno != 0)
        return Result<void>::failure(io_error(operation));
      return Result<void>::success();
    }
    if (++scanned > max_recovery_entries)
      return Result<void>::failure(
          {ErrorCode::resource_exhausted, "too many entries for stage cleanup"});
    auto handled = visit(entry->d_name);
    if (!handled)
      return handled;
  }
}

Result<void> clean_abandoned_pointer_stages(int state_fd) {
  bool removed = false;
  auto scanned =
      visit_directory_entries(state_fd, "scan temporary pointers", [&](const char* entry) {
        const std::string_view name(entry);
        const auto suffix = name.starts_with(".CURRENT-")    ? name.substr(9)
                            : name.starts_with(".AUTOSAVE-") ? name.substr(10)
                                                             : std::string_view{};
        if (!hex_name(suffix))
          return Result<void>::success();
        struct stat status{};
        if (::fstatat(state_fd, entry, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(status.st_mode))
          return Result<void>::success();
        if (::unlinkat(state_fd, entry, 0) != 0 && errno != ENOENT)
          return Result<void>::failure(io_error("remove abandoned pointer"));
        removed = true;
        return Result<void>::success();
      });
  if (!scanned)
    return scanned;
  if (removed && ::fsync(state_fd) != 0)
    return Result<void>::failure(io_error("flush pointer stage cleanup"));
  return Result<void>::success();
}

Result<void> clean_abandoned_generation_stages(int generations_fd) {
  bool removed = false;
  auto scanned =
      visit_directory_entries(generations_fd, "scan temporary generations", [&](const char* entry) {
        const std::string_view name(entry);
        if (!name.starts_with(".tmp-") || !hex_name(name.substr(5)))
          return Result<void>::success();
        auto stage = open_directory(generations_fd, entry);
        if (!stage) // Symlinks and non-directories are not ours to remove.
          return Result<void>::success();
        std::vector<std::string> files;
        bool known = true;
        auto contents = visit_directory_entries(
            stage.value().get(), "scan staged generation", [&](const char* item) {
              const std::string_view file(item);
              if (file == "." || file == "..")
                return Result<void>::success();
              struct stat status{};
              if ((file != "manifest.json" && file != "manifest.sha256") ||
                  ::fstatat(stage.value().get(), item, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
                  !S_ISREG(status.st_mode)) {
                known = false;
                return Result<void>::success();
              }
              files.emplace_back(file);
              return Result<void>::success();
            });
        if (!contents)
          return contents;
        if (!known) {
          return Result<void>::success();
        }
        for (const auto& file : files)
          if (::unlinkat(stage.value().get(), file.c_str(), 0) != 0 && errno != ENOENT)
            return Result<void>::failure(io_error("remove staged generation file"));
        struct stat held{};
        struct stat named{};
        if (::fstat(stage.value().get(), &held) != 0 ||
            ::fstatat(generations_fd, entry, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            held.st_dev != named.st_dev || held.st_ino != named.st_ino)
          return Result<void>::failure({ErrorCode::conflict, "temporary generation changed"});
        if (::unlinkat(generations_fd, entry, AT_REMOVEDIR) != 0)
          return Result<void>::failure(io_error("remove abandoned generation"));
        removed = true;
        return Result<void>::success();
      });
  if (!scanned)
    return scanned;
  if (removed && ::fsync(generations_fd) != 0)
    return Result<void>::failure(io_error("flush generation stage cleanup"));
  return Result<void>::success();
}

} // namespace

ProjectStore::ProjectStore(ProjectStore&& other) noexcept
    : root_fd_(std::exchange(other.root_fd_, -1)), assets_fd_(std::exchange(other.assets_fd_, -1)),
      state_fd_(std::exchange(other.state_fd_, -1)),
      generations_fd_(std::exchange(other.generations_fd_, -1)),
      lock_fd_(std::exchange(other.lock_fd_, -1)), fault_(std::move(other.fault_)),
      store_token_(std::move(other.store_token_)), save_gate_(std::move(other.save_gate_)),
      autosave_only_(other.autosave_only_), explicit_save_only_(other.explicit_save_only_) {}

ProjectStore& ProjectStore::operator=(ProjectStore&& other) noexcept {
  if (this != &other) {
    if (lock_fd_ >= 0)
      ::close(lock_fd_);
    if (generations_fd_ >= 0)
      ::close(generations_fd_);
    if (state_fd_ >= 0)
      ::close(state_fd_);
    if (assets_fd_ >= 0)
      ::close(assets_fd_);
    if (root_fd_ >= 0)
      ::close(root_fd_);
    root_fd_ = std::exchange(other.root_fd_, -1);
    assets_fd_ = std::exchange(other.assets_fd_, -1);
    state_fd_ = std::exchange(other.state_fd_, -1);
    generations_fd_ = std::exchange(other.generations_fd_, -1);
    lock_fd_ = std::exchange(other.lock_fd_, -1);
    fault_ = std::move(other.fault_);
    store_token_ = std::move(other.store_token_);
    save_gate_ = std::move(other.save_gate_);
    autosave_only_ = other.autosave_only_;
    explicit_save_only_ = other.explicit_save_only_;
  }
  return *this;
}

ProjectStore::~ProjectStore() {
  if (lock_fd_ >= 0)
    ::close(lock_fd_);
  if (generations_fd_ >= 0)
    ::close(generations_fd_);
  if (state_fd_ >= 0)
    ::close(state_fd_);
  if (assets_fd_ >= 0)
    ::close(assets_fd_);
  if (root_fd_ >= 0)
    ::close(root_fd_);
}

Result<ProjectStore> ProjectStore::fork_autosave_writer() const {
  if (lock_fd_ < 0 || !save_gate_ || autosave_only_ || explicit_save_only_)
    return Result<ProjectStore>::failure({ErrorCode::conflict, "project writer is unavailable"});
  Fd root(::fcntl(root_fd_, F_DUPFD_CLOEXEC, 0));
  Fd assets(::fcntl(assets_fd_, F_DUPFD_CLOEXEC, 0));
  Fd state(::fcntl(state_fd_, F_DUPFD_CLOEXEC, 0));
  Fd generations(::fcntl(generations_fd_, F_DUPFD_CLOEXEC, 0));
  Fd lock(::fcntl(lock_fd_, F_DUPFD_CLOEXEC, 0));
  if (!root || !assets || !state || !generations || !lock)
    return Result<ProjectStore>::failure(io_error("duplicate autosave writer"));
  ProjectStore copy(
      {root.release(), assets.release(), state.release(), generations.release(), lock.release()},
      fault_);
  copy.store_token_ = store_token_;
  copy.save_gate_ = save_gate_;
  copy.autosave_only_ = true;
  return Result<ProjectStore>::success(std::move(copy));
}

Result<ProjectStore> ProjectStore::fork_explicit_save_writer() const {
  auto writer = fork_autosave_writer();
  if (!writer)
    return writer;
  writer.value().autosave_only_ = false;
  writer.value().explicit_save_only_ = true;
  return writer;
}

Result<int> ProjectStore::open_cache_directory() const {
  auto directory = open_directory(root_fd_, "cache");
  if (!directory)
    return Result<int>::failure(directory.error());
  return Result<int>::success(directory.value().release());
}

Result<ProjectStore> ProjectStore::open_writer(const std::filesystem::path& root, bool create,
                                               FaultHook fault) {
  Fd root_fd;
  if (create) {
    const auto parent_path =
        root.parent_path().empty() ? std::filesystem::path{"."} : root.parent_path();
    const auto child = root.filename().string();
    if (child.empty() || child == "." || child == "..") {
      return Result<ProjectStore>::failure(
          {ErrorCode::invalid_argument, "project root must name a directory"});
    }
    Fd parent(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parent)
      return Result<ProjectStore>::failure(io_error("open project parent"));
    if (::mkdirat(parent.get(), child.c_str(), 0700) != 0 && errno != EEXIST) {
      return Result<ProjectStore>::failure(io_error("create project root"));
    }
    // The new root's directory entry belongs to the parent, not the root.
    // Repeat this flush when create=true on an existing root so an earlier
    // interrupted attempt cannot be mistaken for a durable project path.
    if (::fsync(parent.get()) != 0) {
      return Result<ProjectStore>::failure(io_error("flush project parent"));
    }
    if (fault && fault(SaveStage::root_parent_flushed)) {
      return Result<ProjectStore>::failure(
          {ErrorCode::io_failure, "injected project-root interruption"});
    }
    root_fd =
        Fd(::openat(parent.get(), child.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  } else {
    root_fd = Fd(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  }
  if (!root_fd)
    return Result<ProjectStore>::failure(io_error("open project root"));
  for (const auto* name : {"assets", "state"}) {
    auto made = ensure_directory(root_fd.get(), name, create);
    if (!made)
      return Result<ProjectStore>::failure(made.error());
    auto checked = open_directory(root_fd.get(), name);
    if (!checked)
      return Result<ProjectStore>::failure(checked.error());
  }
  // cache/ is disposable: reopening a valid project restores it if deleted.
  auto cache_made = ensure_directory(root_fd.get(), "cache", true);
  if (!cache_made)
    return Result<ProjectStore>::failure(cache_made.error());
  auto cache_checked = open_directory(root_fd.get(), "cache");
  if (!cache_checked)
    return Result<ProjectStore>::failure(cache_checked.error());
  auto state_fd = open_directory(root_fd.get(), "state");
  auto assets_fd = open_directory(root_fd.get(), "assets");
  if (!assets_fd)
    return Result<ProjectStore>::failure(assets_fd.error());
  if (!state_fd)
    return Result<ProjectStore>::failure(state_fd.error());
  auto made = ensure_directory(state_fd.value().get(), "generations", create);
  if (!made)
    return Result<ProjectStore>::failure(made.error());
  auto generations_fd = open_directory(state_fd.value().get(), "generations");
  if (!generations_fd)
    return Result<ProjectStore>::failure(generations_fd.error());
  Fd lock_fd(::openat(state_fd.value().get(), "WRITER.lock",
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
  if (!lock_fd)
    return Result<ProjectStore>::failure(io_error("open writer lock"));
  struct stat lock_status{};
  if (::fstat(lock_fd.get(), &lock_status) != 0 || !S_ISREG(lock_status.st_mode)) {
    return Result<ProjectStore>::failure({ErrorCode::corrupt_data, "invalid writer lock"});
  }
  if (::flock(lock_fd.get(), LOCK_EX | LOCK_NB) != 0) {
    return Result<ProjectStore>::failure({ErrorCode::conflict, "another project writer is active"});
  }
  auto lock_identity = check_writer_lock({state_fd.value().get(), lock_fd.get()});
  if (!lock_identity)
    return Result<ProjectStore>::failure(lock_identity.error());
  auto cleaned = clean_abandoned_wav_stages(assets_fd.value().get());
  if (!cleaned)
    return Result<ProjectStore>::failure(cleaned.error());
  cleaned = clean_abandoned_pointer_stages(state_fd.value().get());
  if (!cleaned)
    return Result<ProjectStore>::failure(cleaned.error());
  cleaned = clean_abandoned_generation_stages(generations_fd.value().get());
  if (!cleaned)
    return Result<ProjectStore>::failure(cleaned.error());
  if (create && (::fsync(state_fd.value().get()) != 0 || ::fsync(root_fd.get()) != 0)) {
    return Result<ProjectStore>::failure(io_error("flush project directory layout"));
  }
  auto token = random_name();
  if (!token)
    return Result<ProjectStore>::failure(token.error());
  ProjectStore store({root_fd.release(), assets_fd.value().release(), state_fd.value().release(),
                      generations_fd.value().release(), lock_fd.release()},
                     std::move(fault));
  store.store_token_ = std::move(token).value();
  return Result<ProjectStore>::success(std::move(store));
}

Result<ProjectStore> ProjectStore::open_reader(const std::filesystem::path& root) {
  Fd root_fd(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!root_fd)
    return Result<ProjectStore>::failure(io_error("open project root"));
  auto state_fd = open_directory(root_fd.get(), "state");
  auto assets_fd = open_directory(root_fd.get(), "assets");
  if (!assets_fd)
    return Result<ProjectStore>::failure(assets_fd.error());
  if (!state_fd)
    return Result<ProjectStore>::failure(state_fd.error());
  auto generations_fd = open_directory(state_fd.value().get(), "generations");
  if (!generations_fd)
    return Result<ProjectStore>::failure(generations_fd.error());
  return Result<ProjectStore>::success(
      ProjectStore({root_fd.release(), assets_fd.value().release(), state_fd.value().release(),
                    generations_fd.value().release(), -1},
                   {}));
}

} // namespace oscillate::persistence
