#pragma once

#include "oscillate/foundation/error.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Local crash reports. Nothing here sends anything anywhere.
//
// At startup the application records a description of the session and arms
// handlers for fatal signals. A crash leaves only a small marker file, written
// with async-signal-safe calls. The next start finds the marker and gathers a
// plain-text report from the crashed session's description and the log files,
// for the user to review and attach to an issue. docs/crash-reports.md lists
// exactly what a report contains.
namespace oscillate::foundation::crash_report {

// File names inside the crash-report directory.
inline constexpr std::string_view marker_name = "crashed";
inline constexpr std::string_view session_name = "session.txt";
inline constexpr std::string_view report_prefix = "oscillate-crash-";
inline constexpr std::string_view report_extension = ".txt";
// Home-directory prefixes in a report are replaced with this.
inline constexpr std::string_view home_placeholder = "~";

// $XDG_STATE_HOME/oscillate/crash-reports, else
// $HOME/.local/state/oscillate/crash-reports. Empty when neither variable
// names an absolute path.
[[nodiscard]] std::filesystem::path default_directory(const char* xdg_state_home, const char* home);

// One "name: value" line in the session description.
struct Fact {
  std::string name;
  std::string value;
};

// The running session: the caller's application facts, then the system
// (kernel, distribution, processors, memory) and the environment variables
// that change Oscillate's behaviour (OSCILLATE_*, the session type, the Qt
// platform and the audio server's variables).
[[nodiscard]] std::string describe_session(std::span<const Fact> application);

// Writes `session` to the directory (created 0700) and installs handlers for
// SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT. A handler appends one line to
// the marker file, restores the handler that was there before and re-raises,
// so the process still dies with its signal. Process-wide; call once from the
// main thread before other threads start.
[[nodiscard]] Result<void> arm(const std::filesystem::path& directory, std::string_view session);

// Notes a fatal error that is not a signal (a Qt fatal message, an unhandled
// exception) in the marker, so the next start reports it. Not for signal
// handlers. Does nothing before arm().
void note_fatal_error(std::string_view message) noexcept;

struct CollectOptions {
  // Where arm() wrote the marker and the session description.
  std::filesystem::path directory;
  // The log directory (foundation::log::Options::directory); its rotated
  // oscillate*.log files go into the report, oldest first: at most the
  // newest log::Options::default_max_files, and the last
  // default_max_file_bytes of each.
  std::filesystem::path log_directory;
  // The user's home directory, replaced with "~" throughout the report.
  // Empty: nothing is replaced.
  std::string home;
};

// When the previous run left a marker, writes a report into the directory,
// removes the marker and returns the report's path. Returns nullopt when the
// previous run did not crash. Call before arm() so the crashed session's
// description is still there.
[[nodiscard]] Result<std::optional<std::filesystem::path>> collect(const CollectOptions& options);

// `text` with every occurrence of `home` (as a whole path component prefix)
// replaced with "~".
[[nodiscard]] std::string redact(std::string_view text, std::string_view home);

} // namespace oscillate::foundation::crash_report
