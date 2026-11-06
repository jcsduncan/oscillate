#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

// POSIX file helpers shared by every module that writes durable or cached files.
// Control and worker threads only: these perform blocking I/O.
namespace oscillate::foundation {

// Owns one file descriptor and closes it on destruction. Close errors are
// ignored here; call close() where a failed close must be reported.
class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept;
  UniqueFd& operator=(UniqueFd&& other) noexcept;
  ~UniqueFd();

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] int release() noexcept;
  void reset(int fd = -1) noexcept;
  // Closes the descriptor now and reports whether close succeeded.
  [[nodiscard]] Result<void> close(std::string_view action);

private:
  int fd_;
};

// An io_failure carrying `action` and the current errno text.
[[nodiscard]] Error io_error(std::string_view action);

using WriteFunction = ssize_t (*)(int, const void*, std::size_t);

// Writes every byte, retrying short writes and EINTR. A write that makes no
// progress fails. `write_fn` exists for fault-injection tests.
[[nodiscard]] Result<void> write_all(int fd, std::span<const std::byte> bytes,
                                     std::string_view action, WriteFunction write_fn = ::write);
[[nodiscard]] inline Result<void> write_all(int fd, std::string_view text,
                                            std::string_view action) {
  return write_all(fd, std::as_bytes(std::span(text.data(), text.size())), action);
}

// fsync with an io_failure naming `action`.
[[nodiscard]] Result<void> sync(int fd, std::string_view action);

enum class PublishMode : std::uint8_t {
  replace,    // renameat: atomically replace any existing target.
  no_replace, // renameat2(RENAME_NOREPLACE): fail if the target exists (errno EEXIST).
};

// Renames `from` to `to` inside one directory. Failures are io_failure and keep
// errno, so a caller can tell EEXIST apart.
[[nodiscard]] Result<void> rename_in(int directory, const std::string& from, const std::string& to,
                                     PublishMode mode, std::string_view action);

// Creates `name` exclusively (mode 0600, no symlink follow), writes `bytes` and
// fsyncs it. The partially written file is left for the caller's cleanup.
[[nodiscard]] Result<void> write_new_synced(int directory, const std::string& name,
                                            std::span<const std::byte> bytes,
                                            std::string_view subject);

enum class Durability : std::uint8_t {
  durable,
  uncertain, // The file was published but the directory flush failed.
};

// The atomic-replace sequence: write `bytes` to `temporary`, fsync, rename over
// `target` and fsync the directory. Before the rename a failure removes the
// temporary and leaves any existing target untouched.
[[nodiscard]] Result<Durability> replace_file(int directory, const std::string& temporary,
                                              const std::string& target,
                                              std::span<const std::byte> bytes,
                                              std::string_view subject);

// The tail of a streamed write: fsync and close `file`, rename `temporary` to
// `target` and fsync the directory. A failure before the rename leaves the
// temporary for the caller to remove.
[[nodiscard]] Result<Durability> publish_synced(UniqueFd file, int directory,
                                                const std::string& temporary,
                                                const std::string& target, PublishMode mode,
                                                std::string_view subject);

} // namespace oscillate::foundation
