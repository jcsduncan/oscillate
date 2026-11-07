#include "oscillate/persistence/project_creation.hpp"

#include "private.hpp"

#include "oscillate/foundation/hex.hpp"

#include <cerrno>
#include <climits>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace oscillate::persistence {
namespace {

using foundation::ErrorCode;
using foundation::io_error;
using foundation::UniqueFd;
template <typename T> using Result = foundation::Result<T>;

constexpr std::string_view stage_prefix = ".";
constexpr std::string_view stage_marker = ".creating-";
// A project library is a user folder; refuse to scan one without bound.
constexpr std::size_t max_scanned_entries = 8192;
constexpr std::size_t stage_suffix_length =
    stage_marker.size() + detail::random_name_bytes * foundation::hex_digits_per_byte;

timespec realtime_now() noexcept {
  timespec now{};
  (void)::clock_gettime(CLOCK_REALTIME, &now);
  return now;
}

// Captured while the library loads: stages modified before this instant
// belong to a process that started earlier than this one.
const timespec process_started = realtime_now();

bool before_process_start(const timespec& time) noexcept {
  return time.tv_sec < process_started.tv_sec ||
         (time.tv_sec == process_started.tv_sec && time.tv_nsec < process_started.tv_nsec);
}

bool injected(const FaultHook& fault, SaveStage stage) { return fault && fault(stage); }

struct DirectoryCloser {
  void operator()(DIR* stream) const noexcept { (void)::closedir(stream); }
};
using DirectoryStream = std::unique_ptr<DIR, DirectoryCloser>;

// A fresh stage name. It keeps as much of the project name as NAME_MAX
// allows, so a leftover stage is still recognisable in a file browser.
Result<std::string> stage_name_for(const std::string& final_name) {
  auto random = detail::random_name();
  if (!random)
    return Result<std::string>::failure(random.error());
  const auto room = NAME_MAX - stage_prefix.size() - stage_suffix_length;
  std::string name(stage_prefix);
  name += final_name.substr(0, room);
  name += stage_marker;
  name += random.value();
  return Result<std::string>::success(std::move(name));
}

void remove_stage(const std::filesystem::path& parent, int parent_fd, const std::string& name) {
  std::error_code ignored;
  if (std::filesystem::remove_all(parent / name, ignored) > 0)
    (void)::fsync(parent_fd);
}

// Evidence that a stage-named folder was made by a creation: it holds only
// the folders a new project's writer lays out, and no imported media yet.
bool fresh_project_layout(const std::filesystem::path& stage) {
  std::error_code error;
  for (std::filesystem::directory_iterator it(stage, error), end; !error && it != end;
       it.increment(error)) {
    const auto name = it->path().filename().string();
    if ((name != "assets" && name != "state" && name != "cache") ||
        it->symlink_status(error).type() != std::filesystem::file_type::directory)
      return false;
  }
  if (error)
    return false;
  // A creation killed before laying out assets/ leaves none.
  const auto assets = stage / "assets";
  struct stat status{};
  if (::lstat(assets.c_str(), &status) != 0)
    return errno == ENOENT;
  return std::filesystem::is_empty(assets, error) && !error;
}

// The stage may be removed only when a creation made it, nothing committed it
// and no live writer holds it. The returned lock, when held, keeps another
// writer out meanwhile.
bool abandoned_stage(int parent_fd, const std::filesystem::path& parent, const char* name,
                     UniqueFd& lock) {
  struct stat status{};
  if (::fstatat(parent_fd, name, &status, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(status.st_mode) ||
      !before_process_start(status.st_mtim) || !fresh_project_layout(parent / name))
    return false;
  UniqueFd stage(::openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!stage)
    return false;
  UniqueFd state(::openat(stage.get(), "state", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!state)
    return errno == ENOENT;
  struct stat pointer{};
  if (::fstatat(state.get(), "CURRENT", &pointer, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
    return false;
  lock.reset(::openat(state.get(), "WRITER.lock", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!lock)
    return errno == ENOENT;
  return ::flock(lock.get(), LOCK_EX | LOCK_NB) == 0;
}

} // namespace

ProjectCreation::ProjectCreation(std::filesystem::path parent, UniqueFd parent_fd,
                                 std::string final_name, foundation::PublishMode mode,
                                 FaultHook fault)
    : parent_(std::move(parent)), parent_fd_(std::move(parent_fd)),
      final_name_(std::move(final_name)), mode_(mode), fault_(std::move(fault)) {}

ProjectCreation::ProjectCreation(ProjectCreation&& other) noexcept
    : parent_(std::move(other.parent_)), parent_fd_(std::move(other.parent_fd_)),
      stage_name_(std::move(other.stage_name_)), final_name_(std::move(other.final_name_)),
      mode_(other.mode_), fault_(std::move(other.fault_)),
      published_(std::exchange(other.published_, true)) {}

ProjectCreation::~ProjectCreation() {
  if (!published_ && parent_fd_ && !stage_name_.empty())
    remove_stage(parent_, parent_fd_.get(), stage_name_);
}

bool ProjectCreation::is_stage_name(std::string_view name) noexcept {
  if (!name.starts_with(stage_prefix) || name.size() <= stage_prefix.size() + stage_suffix_length)
    return false;
  const auto suffix = name.substr(name.size() - stage_suffix_length);
  return suffix.starts_with(stage_marker) && detail::hex_name(suffix.substr(stage_marker.size()));
}

Result<StartedCreation> ProjectCreation::begin(const std::filesystem::path& root, FaultHook fault) {
  const auto final_name = root.filename().string();
  if (final_name.empty() || final_name == "." || final_name == "..")
    return Result<StartedCreation>::failure(
        {ErrorCode::invalid_argument, "project root must name a directory"});
  // Cleanup would treat a project under a stage-shaped name as a stage.
  if (is_stage_name(final_name))
    return Result<StartedCreation>::failure(
        {ErrorCode::invalid_argument, "Project name is reserved for project creation"});
  const auto parent = root.parent_path().empty() ? std::filesystem::path{"."} : root.parent_path();
  UniqueFd parent_fd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!parent_fd)
    return Result<StartedCreation>::failure(io_error("open project parent"));
  // Best effort: a stage that cannot be removed must not block creation.
  (void)clean_abandoned(parent);

  // An existing empty folder is replaced by the rename; anything else blocks.
  auto mode = foundation::PublishMode::no_replace;
  struct stat existing{};
  if (::fstatat(parent_fd.get(), final_name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
    std::error_code error;
    const bool empty =
        S_ISDIR(existing.st_mode) && std::filesystem::is_empty(parent / final_name, error);
    if (error)
      return Result<StartedCreation>::failure(
          {ErrorCode::io_failure, "Cannot inspect project folder"});
    if (!empty)
      return Result<StartedCreation>::failure(
          {ErrorCode::conflict, "New project folder must be empty"});
    mode = foundation::PublishMode::replace;
  } else if (errno != ENOENT) {
    return Result<StartedCreation>::failure(io_error("inspect project folder"));
  }
  if (injected(fault, SaveStage::creation_began))
    return Result<StartedCreation>::failure(
        {ErrorCode::io_failure, "injected project-creation interruption"});

  auto stage_name = stage_name_for(final_name);
  if (!stage_name)
    return Result<StartedCreation>::failure(stage_name.error());
  ProjectCreation creation(parent, std::move(parent_fd), final_name, mode, fault);
  creation.stage_name_ = stage_name.value();
  // open_writer makes the stage, flushes its entry in the parent and lays out
  // the project folders; a failure leaves the stage for the destructor.
  auto writer = ProjectStore::open_writer(parent / creation.stage_name_, true, std::move(fault));
  if (!writer)
    return Result<StartedCreation>::failure(writer.error());
  return Result<StartedCreation>::success(
      StartedCreation{std::move(creation), std::move(writer).value()});
}

Result<void> ProjectCreation::publish(const ProjectStore& writer) {
  if (published_)
    return Result<void>::failure({ErrorCode::conflict, "New project is already published"});
  auto current = writer.load_current();
  if (!current)
    return Result<void>::failure(
        {ErrorCode::conflict, "New project has no committed save to publish"});
  UniqueFd stage(::openat(parent_fd_.get(), stage_name_.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!stage)
    return Result<void>::failure(io_error("open new project stage"));
  if (auto flushed = foundation::sync(stage.get(), "flush new project folder"); !flushed)
    return flushed;
  auto renamed = foundation::rename_in(parent_fd_.get(), stage_name_, final_name_, mode_,
                                       "publish new project folder");
  if (!renamed) {
    const int failure = errno; // Read while the failed result still holds it.
    if (failure == EEXIST || failure == ENOTEMPTY)
      return Result<void>::failure({ErrorCode::conflict, "New project folder must be empty"});
    return renamed;
  }
  published_ = true;
  if (injected(fault_, SaveStage::creation_published))
    return Result<void>::failure(
        {ErrorCode::io_failure, "injected project-publication interruption"});
  if (::fsync(parent_fd_.get()) != 0)
    return Result<void>::failure(
        {ErrorCode::io_failure, "New project is published but its folder durability is uncertain"});
  return Result<void>::success();
}

Result<std::size_t> ProjectCreation::clean_abandoned(const std::filesystem::path& parent) {
  UniqueFd parent_fd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!parent_fd)
    return Result<std::size_t>::failure(io_error("open project parent"));
  UniqueFd scan_fd(::openat(parent_fd.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!scan_fd)
    return Result<std::size_t>::failure(io_error("scan project parent"));
  const int raw_fd = scan_fd.release();
  DIR* raw = ::fdopendir(raw_fd);
  if (!raw) {
    const auto failure = io_error("scan project parent");
    ::close(raw_fd);
    return Result<std::size_t>::failure(failure);
  }
  DirectoryStream stream(raw);
  std::size_t scanned = 0;
  std::size_t removed = 0;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(stream.get());
    if (!entry) {
      if (errno != 0)
        return Result<std::size_t>::failure(io_error("scan project parent"));
      break;
    }
    if (++scanned > max_scanned_entries)
      break; // Bounded and best effort: stages past the bound are left alone.
    if (!is_stage_name(entry->d_name))
      continue;
    UniqueFd lock;
    if (!abandoned_stage(parent_fd.get(), parent, entry->d_name, lock))
      continue;
    // A stage that cannot be removed is skipped; it never blocks the rest.
    std::error_code error;
    std::filesystem::remove_all(parent / entry->d_name, error);
    if (!error)
      ++removed;
  }
  if (removed > 0) {
    if (auto flushed = foundation::sync(parent_fd.get(), "flush project parent"); !flushed)
      return Result<std::size_t>::failure(flushed.error());
  }
  return Result<std::size_t>::success(removed);
}

} // namespace oscillate::persistence
