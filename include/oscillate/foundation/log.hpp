#pragma once

#include "oscillate/foundation/diagnostic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Text logging for control and worker threads: levels, categories, stderr and
// a size-bounded rotating file under $XDG_STATE_HOME/oscillate/logs.
//
// Never log from the audio callback or its helpers (AGENTS.md: the callback
// does not format text or do I/O). Those threads publish fixed-size
// DiagnosticEvent records to realtime_diagnostics() instead, and a control
// thread drains them into the log (drain_diagnostics). Debug builds assert
// that a thread inside a RealtimeThreadScope never reaches write().
namespace oscillate::foundation::log {

// Ordered by severity: a filter at `info` passes error, warning and info.
enum class Level : std::uint8_t { error = 0, warning = 1, info = 2, debug = 3 };

using Category = DiagnosticCategory;
inline constexpr std::size_t category_count = 7;

[[nodiscard]] std::string_view level_name(Level level) noexcept;
[[nodiscard]] std::string_view category_name(Category category) noexcept;
[[nodiscard]] std::optional<Level> parse_level(std::string_view text) noexcept;
// Category names as they appear in OSCILLATE_LOG; "audio" is the device category.
[[nodiscard]] std::optional<Category> parse_category(std::string_view text) noexcept;

// Which messages pass. A category's own level, when set, replaces the default.
struct Filter {
  static constexpr Level default_level = Level::info;
  Level level = default_level;
  std::array<std::optional<Level>, category_count> categories{};

  [[nodiscard]] bool passes(Category category, Level message) const noexcept;
};

// OSCILLATE_LOG syntax: comma-separated items, each a level ("debug") or a
// category's level ("audio=debug"). Later items win. Unknown items are
// returned in `rejected` and otherwise ignored.
struct ParsedFilter {
  Filter filter;
  std::vector<std::string> rejected;
};
[[nodiscard]] ParsedFilter parse_filter(std::string_view spec);

struct Options {
  static constexpr std::uint64_t default_max_file_bytes = std::uint64_t{1} << 20;
  static constexpr std::size_t default_max_files = 5;
  static constexpr std::string_view file_stem = "oscillate";

  Filter filter;
  bool to_stderr = true;
  // The terminal shows only lines at least this severe; the file keeps
  // everything the filter passes. Setting OSCILLATE_LOG makes the terminal
  // follow the filter too.
  Level stderr_level = Level::warning;
  // Empty: no log file. The directory is created (0700) when needed.
  std::filesystem::path directory;
  // The live file rotates before it would exceed this; at most max_files
  // files exist (oscillate.log, oscillate.1.log ... oscillate.<max_files-1>.log).
  std::uint64_t max_file_bytes = default_max_file_bytes;
  std::size_t max_files = default_max_files;
};

// $XDG_STATE_HOME/oscillate/logs, else $HOME/.local/state/oscillate/logs.
// Empty when neither variable names an absolute path.
[[nodiscard]] std::filesystem::path default_directory(const char* xdg_state_home, const char* home);
// OSCILLATE_LOG, XDG_STATE_HOME and HOME as read from the environment.
[[nodiscard]] Options options_from_environment();

// Control only, normally once at startup. Replaces the sinks; rejected
// OSCILLATE_LOG items are reported as warnings by the caller's choice.
void configure(Options options);
// The file being written, empty when there is none.
[[nodiscard]] std::filesystem::path current_file();

[[nodiscard]] bool enabled(Category category, Level level) noexcept;
// Any non-realtime thread. Serialised by a mutex; never throws.
void write(Level level, Category category, std::string_view message) noexcept;
void flush() noexcept;

// Formats only when the message passes the filter. Never throws: a message
// that cannot be formatted (out of memory) is lost.
template <typename... Args>
void message(Level level, Category category, std::format_string<Args...> format,
             Args&&... args) noexcept {
  if (!enabled(category, level))
    return;
  try {
    write(level, category, std::format(format, std::forward<Args>(args)...));
  } catch (...) { // NOLINT(bugprone-empty-catch): logging never throws into its caller.
  }
}
template <typename... Args>
void error(Category category, std::format_string<Args...> format, Args&&... args) noexcept {
  message(Level::error, category, format, std::forward<Args>(args)...);
}
template <typename... Args>
void warning(Category category, std::format_string<Args...> format, Args&&... args) noexcept {
  message(Level::warning, category, format, std::forward<Args>(args)...);
}
template <typename... Args>
void info(Category category, std::format_string<Args...> format, Args&&... args) noexcept {
  message(Level::info, category, format, std::forward<Args>(args)...);
}
template <typename... Args>
void debug(Category category, std::format_string<Args...> format, Args&&... args) noexcept {
  message(Level::debug, category, format, std::forward<Args>(args)...);
}

// What one drain of the realtime diagnostics found, for user-facing status.
struct DrainSummary {
  std::size_t events = 0;
  // Records lost because the ring was full since the previous drain.
  std::uint64_t dropped = 0;
  // A short sentence for the status line, empty when nothing happened.
  std::string status;
};
// Control only, for example on the UI tick: drains realtime_diagnostics(),
// writes one log line per kind of event and returns a summary.
DrainSummary drain_diagnostics();
// The sentence a record becomes in the log and the status line.
[[nodiscard]] std::string describe(const DiagnosticEvent& event, std::uint64_t repeats = 1);

} // namespace oscillate::foundation::log

namespace oscillate::foundation {

// Marks the calling thread as real-time (audio callback, audio worker) while
// the scope lives and restores the previous marking when it ends, so a
// callback that runs on a borrowed thread (a fake driver on a test or control
// thread) does not leave that thread marked. Cheap enough for every callback.
class RealtimeThreadScope {
public:
  RealtimeThreadScope() noexcept;
  RealtimeThreadScope(const RealtimeThreadScope&) = delete;
  RealtimeThreadScope& operator=(const RealtimeThreadScope&) = delete;
  RealtimeThreadScope(RealtimeThreadScope&&) = delete;
  RealtimeThreadScope& operator=(RealtimeThreadScope&&) = delete;
  ~RealtimeThreadScope();

private:
  bool previous_;
};
[[nodiscard]] bool on_realtime_thread() noexcept;

} // namespace oscillate::foundation
