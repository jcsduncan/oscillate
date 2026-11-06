#include "oscillate/foundation/file_io.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace oscillate::foundation {
namespace {

constexpr mode_t private_file_mode = 0600;

[[nodiscard]] std::string verb_subject(std::string_view verb, std::string_view subject) {
  std::string action(verb);
  action += ' ';
  action += subject;
  return action;
}

} // namespace

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
  if (this != &other)
    reset(std::exchange(other.fd_, -1));
  return *this;
}

UniqueFd::~UniqueFd() { reset(); }

int UniqueFd::release() noexcept { return std::exchange(fd_, -1); }

void UniqueFd::reset(int fd) noexcept {
  if (fd_ >= 0)
    (void)::close(fd_);
  fd_ = fd;
}

Result<void> UniqueFd::close(std::string_view action) {
  if (::close(release()) != 0)
    return Result<void>::failure(io_error(action));
  return Result<void>::success();
}

Error io_error(std::string_view action) {
  const auto saved = errno;
  Error error{ErrorCode::io_failure, std::string(action) + ": " + std::strerror(saved)};
  errno = saved;
  return error;
}

Result<void> write_all(int fd, std::span<const std::byte> bytes, std::string_view action,
                       WriteFunction write_fn) {
  while (!bytes.empty()) {
    const auto count = write_fn(fd, bytes.data(), bytes.size());
    if (count > 0) {
      bytes = bytes.subspan(static_cast<std::size_t>(count));
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else if (count == 0) {
      return Result<void>::failure(
          {ErrorCode::io_failure, std::string(action) + ": write made no progress"});
    } else {
      return Result<void>::failure(io_error(action));
    }
  }
  return Result<void>::success();
}

Result<void> sync(int fd, std::string_view action) {
  if (::fsync(fd) != 0)
    return Result<void>::failure(io_error(action));
  return Result<void>::success();
}

Result<void> rename_in(int directory, const std::string& from, const std::string& to,
                       PublishMode mode, std::string_view action) {
  const auto renamed = mode == PublishMode::replace
                           ? ::renameat(directory, from.c_str(), directory, to.c_str())
                           : static_cast<int>(::syscall(SYS_renameat2, directory, from.c_str(),
                                                        directory, to.c_str(), RENAME_NOREPLACE));
  if (renamed != 0)
    return Result<void>::failure(io_error(action));
  return Result<void>::success();
}

Result<void> write_new_synced(int directory, const std::string& name,
                              std::span<const std::byte> bytes, std::string_view subject) {
  UniqueFd file(::openat(directory, name.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, private_file_mode));
  if (!file)
    return Result<void>::failure(io_error(verb_subject("create", subject)));
  if (auto written = write_all(file.get(), bytes, verb_subject("write", subject)); !written)
    return written;
  return sync(file.get(), verb_subject("flush", subject));
}

Result<Durability> replace_file(int directory, const std::string& temporary,
                                const std::string& target, std::span<const std::byte> bytes,
                                std::string_view subject) {
  const auto fail = [&](Error error) {
    (void)::unlinkat(directory, temporary.c_str(), 0);
    return Result<Durability>::failure(std::move(error));
  };
  UniqueFd file(::openat(directory, temporary.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, private_file_mode));
  if (!file)
    return Result<Durability>::failure(io_error(verb_subject("create", subject)));
  if (auto written = write_all(file.get(), bytes, verb_subject("write", subject)); !written)
    return fail(std::move(written.error()));
  auto published =
      publish_synced(std::move(file), directory, temporary, target, PublishMode::replace, subject);
  if (!published)
    return fail(std::move(published.error()));
  return published;
}

Result<Durability> publish_synced(UniqueFd file, int directory, const std::string& temporary,
                                  const std::string& target, PublishMode mode,
                                  std::string_view subject) {
  if (auto synced = sync(file.get(), verb_subject("flush", subject)); !synced)
    return Result<Durability>::failure(std::move(synced.error()));
  if (auto closed = file.close(verb_subject("close", subject)); !closed)
    return Result<Durability>::failure(std::move(closed.error()));
  if (auto renamed =
          rename_in(directory, temporary, target, mode, verb_subject("publish", subject));
      !renamed)
    return Result<Durability>::failure(std::move(renamed.error()));
  if (::fsync(directory) != 0)
    return Result<Durability>::success(Durability::uncertain);
  return Result<Durability>::success(Durability::durable);
}

} // namespace oscillate::foundation
