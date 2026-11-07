#pragma once

#include "oscillate/foundation/crash_report.hpp"
#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// A diagnostics bundle: one zip the user saves on request (command palette or
// --save-diagnostics) and may attach to an issue. Nothing is sent anywhere.
// It gathers what crash_report gathers (the session description, the log
// files and the latest crash report) plus the build's features, the plug-in
// scan results and, when asked, a performance trace. It never holds media,
// project audio or project files, and the user's home directory is shown as
// "~" throughout. docs/diagnostics.md lists exactly what a bundle contains.
namespace oscillate::foundation::diagnostics_bundle {

inline constexpr std::string_view bundle_prefix = "oscillate-diagnostics-";
inline constexpr std::string_view bundle_extension = ".zip";
// Entry names inside the zip.
inline constexpr std::string_view readme_entry = "README.txt";
inline constexpr std::string_view session_entry = "session.txt";
inline constexpr std::string_view build_entry = "build.txt";
inline constexpr std::string_view plugins_entry = "plugins.txt";
inline constexpr std::string_view logs_folder = "logs/";
inline constexpr std::string_view crash_folder = "crash-report/";
inline constexpr std::string_view trace_folder = "trace/";
// A trace larger than this, or with a line longer than max_trace_line_bytes
// (the tracer writes one event per line), is left out and the README says so.
inline constexpr std::uintmax_t max_trace_bytes = std::uintmax_t{512} << 20U;
inline constexpr std::size_t max_trace_line_bytes = std::size_t{1} << 20U;
// A trace is read, redacted and written this many bytes at a time.
inline constexpr std::size_t trace_read_bytes = std::size_t{1} << 20U;

// $XDG_STATE_HOME/oscillate/diagnostics, else
// $HOME/.local/state/oscillate/diagnostics. Empty when neither variable names
// an absolute path.
[[nodiscard]] std::filesystem::path default_directory(const char* xdg_state_home, const char* home);

// One scanned plug-in, or one bundle that failed to scan: its name, its
// version and the scan's status. Nothing else about it goes into the bundle.
struct Plugin {
  std::string name;
  std::string version;
  std::string status;
};

struct Options {
  // Where the bundle is written (created 0700).
  std::filesystem::path directory;
  // crash_report's directory (its latest report is included) and the log
  // directory (crash_report::log_files).
  std::filesystem::path crash_directory;
  std::filesystem::path log_directory;
  // Replaced with "~" throughout. Empty: nothing is replaced.
  std::string home;
  // The application facts at the top of session.txt (crash_report::describe_session).
  std::vector<crash_report::Fact> application;
  // build.txt: the build's features, one "name: value" line each.
  std::vector<crash_report::Fact> build;
  // plugins.txt: `plugin_note` first when it is not empty (no scan has run,
  // or the build has no plug-in host), then one line per plug-in.
  std::vector<Plugin> plugins;
  std::string plugin_note;
  // A performance trace (Chrome trace JSON) to include. Empty: none.
  std::filesystem::path trace;
};

// Writes oscillate-diagnostics-<UTC time>.zip (0600) into the directory and
// returns its path. Missing inputs (no crash report, no log, an unreadable
// or oversized trace) are noted in the README; only a failure to write the
// bundle itself is an error, and then no partial file is left.
[[nodiscard]] Result<std::filesystem::path> write(const Options& options);

} // namespace oscillate::foundation::diagnostics_bundle
