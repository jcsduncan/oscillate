#include "oscillate/plugins/vst3_scan.hpp"

#include "oscillate/foundation/log.hpp"

#include "scan_protocol.hpp"
#include "vst3_binary_identity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace oscillate::plugins {
namespace {

// What a plugin prints to stderr while scanning is kept for the log, up to
// this many bytes; the rest is read and discarded so the helper never blocks.
constexpr std::size_t max_plugin_stderr_bytes = 8 * 1024;
constexpr std::size_t pipe_read_bytes = 4096;
constexpr int max_poll_wait_ms = 25;

ScanResult failure(ScanStatus status, std::string diagnostic) {
  return {.status = status, .metadata = {}, .diagnostic = std::move(diagnostic), .binary = {}};
}

// The plugin's stderr from one scan, bounded.
struct PluginStderr {
  std::string text;
  std::size_t total = 0;

  // Reads what is available; false at end of file or on a pipe error.
  bool read_from(int fd) {
    std::array<char, pipe_read_bytes> buffer{};
    while (true) {
      const auto count = read(fd, buffer.data(), buffer.size());
      if (count > 0) {
        const auto bytes = static_cast<std::size_t>(count);
        total += bytes;
        const auto room = max_plugin_stderr_bytes - std::min(text.size(), max_plugin_stderr_bytes);
        text.append(buffer.data(), std::min(bytes, room));
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        return count < 0 && errno == EAGAIN;
      }
    }
  }
};

void log_scan(const std::filesystem::path& bundle, const ScanResult& result,
              const PluginStderr& plugin_stderr) {
  using foundation::DiagnosticCategory;
  if (!result.ok())
    foundation::log::warning(DiagnosticCategory::plugin, "VST3 scan of {} failed: {}",
                             bundle.string(), result.diagnostic);
  if (plugin_stderr.total == 0)
    return;
  const auto level = result.ok() ? foundation::log::Level::debug : foundation::log::Level::warning;
  foundation::log::message(
      level, DiagnosticCategory::plugin, "VST3 scan of {} wrote {} bytes to stderr{}:\n{}",
      bundle.string(), plugin_stderr.total,
      plugin_stderr.total > plugin_stderr.text.size() ? " (truncated)" : "", plugin_stderr.text);
}

ScanResult run_helper(const std::filesystem::path& helper, const std::filesystem::path& bundle,
                      const ScanLimits& limits, PluginStderr& plugin_stderr) {
  if (helper.empty() || bundle.empty() || limits.timeout.count() <= 0 ||
      limits.max_response_bytes < 8) {
    return failure(ScanStatus::scanner_error, "Invalid scanner configuration");
  }
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0)
    return failure(ScanStatus::scanner_error, "Cannot create scanner pipe");
  const auto read_flags = fcntl(pipe_fds[0], F_GETFL);
  // POSIX fcntl file-status flags are specified as signed int bitmasks.
  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  if (read_flags < 0 || fcntl(pipe_fds[0], F_SETFL, read_flags | O_NONBLOCK) < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return failure(ScanStatus::scanner_error, "Cannot configure scanner pipe");
  }

  // The plugin's stderr goes to the log; without this pipe it is /dev/null.
  int error_fds[2] = {-1, -1};
  if (pipe2(error_fds, O_CLOEXEC) == 0) {
    const auto error_flags = fcntl(error_fds[0], F_GETFL);
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    if (error_flags < 0 || fcntl(error_fds[0], F_SETFL, error_flags | O_NONBLOCK) < 0) {
      close(error_fds[0]);
      close(error_fds[1]);
      error_fds[0] = error_fds[1] = -1;
    }
  }
  const auto helper_string = helper.string();
  const auto bundle_string = bundle.string();
  const auto child = fork();
  if (child == -1) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    if (error_fds[0] >= 0) {
      close(error_fds[0]);
      close(error_fds[1]);
    }
    return failure(ScanStatus::scanner_error, "Cannot start scanner");
  }
  if (child == 0) {
    setpgid(0, 0);
    const rlimit no_core{.rlim_cur = 0, .rlim_max = 0};
    setrlimit(RLIMIT_CORE, &no_core);
    close(pipe_fds[0]);
    if (dup2(pipe_fds[1], STDOUT_FILENO) == -1)
      _exit(125);
    close(pipe_fds[1]);
    if (error_fds[1] >= 0) {
      close(error_fds[0]);
      dup2(error_fds[1], STDERR_FILENO);
      close(error_fds[1]);
    } else {
      const int null_fd = open("/dev/null", O_WRONLY);
      if (null_fd >= 0) {
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
      }
    }
    execl(helper_string.c_str(), helper_string.c_str(), bundle_string.c_str(), nullptr);
    _exit(127);
  }
  close(pipe_fds[1]);
  if (error_fds[1] >= 0)
    close(error_fds[1]);
  int error_fd = error_fds[0]; // -1 once at end of file: poll skips it.
  setpgid(child, child);
  std::vector<std::uint8_t> bytes;
  bytes.reserve(std::min<std::size_t>(limits.max_response_bytes, 4096));
  const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
  bool eof = false;
  bool oversized = false;
  bool timed_out = false;
  bool transport_failed = false;
  int status = 0;
  bool reaped = false;
  while (!eof || !reaped) {
    if (!reaped) {
      const auto waited = waitpid(child, &status, WNOHANG);
      if (waited == child)
        reaped = true;
      if (waited < 0 && errno != EINTR) {
        transport_failed = true;
        break;
      }
    }
    if (eof && reaped)
      break;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      timed_out = true;
      break;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    std::array<pollfd, 2> descriptors{
        pollfd{.fd = pipe_fds[0], .events = POLLIN | POLLHUP, .revents = 0},
        pollfd{.fd = error_fd, .events = POLLIN | POLLHUP, .revents = 0}};
    const auto polled =
        poll(descriptors.data(), descriptors.size(),
             static_cast<int>(std::clamp<std::int64_t>(remaining, 1, max_poll_wait_ms)));
    if (polled < 0 && errno != EINTR) {
      transport_failed = true;
      break;
    }
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    if (polled > 0 && error_fd >= 0 &&
        (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
        !plugin_stderr.read_from(error_fd)) {
      close(error_fd);
      error_fd = -1;
    }
    const pollfd& descriptor = descriptors[0];
    // A broken or invalid pipe is a scanner transport failure. Waiting for the
    // deadline would misreport it as a plugin hang.
    // poll uses signed short event bitmasks.
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    if (polled > 0 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
      transport_failed = true;
      break;
    }
    // poll uses signed short event bitmasks.
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    if (polled > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
      std::array<std::uint8_t, pipe_read_bytes> buffer{};
      while (true) {
        const auto count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count > 0) {
          if (bytes.size() + static_cast<std::size_t>(count) > limits.max_response_bytes) {
            oversized = true;
            break;
          }
          bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
        } else if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
          break;
        } else {
          eof = true;
          break;
        }
      }
    }
    if (oversized)
      break;
  }
  close(pipe_fds[0]);
  if (error_fd >= 0) {
    (void)plugin_stderr.read_from(error_fd); // What the helper wrote before exiting.
    close(error_fd);
  }
  // The direct helper can exit while a plugin-created descendant still holds
  // stdout open. That descendant belongs to the helper's process group and
  // must not outlive a timed-out or broken scan.
  if (!eof || !reaped)
    kill(-child, SIGKILL);
  if (!reaped) {
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  }
  if (timed_out)
    return failure(ScanStatus::timed_out, "Plugin scanner timed out");
  if (oversized)
    return failure(ScanStatus::oversized_response, "Plugin scanner output too large");
  if (transport_failed)
    return failure(ScanStatus::scanner_error, "Plugin scanner pipe or process wait failed");
  if (!WIFEXITED(status))
    return failure(ScanStatus::crashed, "Plugin scanner terminated abnormally");
  if (WEXITSTATUS(status) != 0)
    return failure(ScanStatus::scanner_error, "Plugin scanner could not start or complete");
  auto result = detail::decode_scan_result(bytes);
  if (!result)
    return failure(ScanStatus::invalid_metadata, "Invalid scanner response");
  return *result;
}

} // namespace

std::vector<std::filesystem::path>
discover_vst3_bundles(std::span<const std::filesystem::path> roots, std::size_t max_bundles,
                      std::size_t max_entries) {
  std::set<std::filesystem::path> found;
  std::size_t visited = 0;
  for (const auto& root : roots) {
    if (visited >= max_entries || found.size() >= max_bundles)
      break;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
      continue;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end && found.size() < max_bundles && visited < max_entries) {
      ++visited;
      const auto& item = *it;
      const auto path = item.path();
      const auto is_bundle = path.extension() == ".vst3" && item.is_directory(error);
      if (!error && is_bundle) {
        found.insert(path.lexically_normal());
        it.disable_recursion_pending();
      }
      it.increment(error);
    }
  }
  return {found.begin(), found.end()};
}

Vst3Scanner::Vst3Scanner(std::filesystem::path helper, ScanLimits limits)
    : helper_(std::move(helper)), limits_(limits) {}

std::string Vst3Scanner::key(const std::filesystem::path& bundle) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(bundle, error);
  return (error ? bundle.lexically_normal() : canonical).string();
}

ScanResult Vst3Scanner::scan(const std::filesystem::path& bundle) {
  const auto before = detail::fingerprint_vst3_binary(bundle);
  if (!before)
    return failure(ScanStatus::invalid_bundle, "VST3 binary is missing or invalid");
  const auto identity = before->canonical_bundle.string();
  std::uint64_t retry_epoch = 0;
  {
    std::lock_guard lock(failures_mutex_);
    const auto found = failures_.find(identity);
    if (limits_.quarantine_after_failures > 0 && found != failures_.end() &&
        found->second >= limits_.quarantine_after_failures)
      return failure(ScanStatus::quarantined, "Plugin quarantined after repeated scan failures");
    retry_epoch = retry_epochs_[identity];
  }
  PluginStderr plugin_stderr;
  auto result = run_helper(helper_, bundle, limits_, plugin_stderr);
  if (result.ok()) {
    const auto after = detail::fingerprint_vst3_binary(bundle);
    if (!after || before->byte_size != after->byte_size || before->sha256 != after->sha256 ||
        before->canonical_binary != after->canonical_binary ||
        before->canonical_bundle != after->canonical_bundle) {
      result = failure(ScanStatus::invalid_bundle, "VST3 binary changed during scan");
    } else {
      result.binary = *after;
    }
  }
  log_scan(bundle, result, plugin_stderr);
  std::lock_guard lock(failures_mutex_);
  if (retry_epochs_[identity] != retry_epoch)
    return result; // A later explicit Retry superseded this in-flight scan.
  if (result.ok()) {
    failures_.erase(identity);
  } else if (result.status != ScanStatus::invalid_bundle && limits_.quarantine_after_failures > 0) {
    auto& failures = failures_[identity];
    if (failures < limits_.quarantine_after_failures)
      ++failures;
  }
  return result;
}

void Vst3Scanner::retry(const std::filesystem::path& bundle) {
  const auto identity = key(bundle);
  std::lock_guard lock(failures_mutex_);
  ++retry_epochs_[identity];
  failures_.erase(identity);
}

bool Vst3Scanner::is_quarantined(const std::filesystem::path& bundle) const {
  const auto identity = key(bundle);
  std::lock_guard lock(failures_mutex_);
  const auto found = failures_.find(identity);
  return limits_.quarantine_after_failures > 0 && found != failures_.end() &&
         found->second >= limits_.quarantine_after_failures;
}

} // namespace oscillate::plugins
