#include "oscillate/foundation/diagnostics_bundle.hpp"

#include "oscillate/foundation/file_io.hpp"
#include "oscillate/foundation/zip_writer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oscillate::foundation::diagnostics_bundle {
namespace {

constexpr mode_t directory_mode = 0700;
constexpr mode_t file_mode = 0600;
constexpr std::string_view bundle_directory = "diagnostics";
constexpr std::string_view temporary_suffix = ".partial";
constexpr std::uintmax_t bytes_per_mebibyte = std::uintmax_t{1} << 20U;
// Bundles written in the same second get "-1", "-2"... up to this many.
constexpr int max_name_attempts = 100;
constexpr std::size_t time_text_bytes = 32;

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string format_utc(std::int64_t seconds, const char* format) {
  const auto value = static_cast<std::time_t>(seconds);
  std::tm utc{};
  ::gmtime_r(&value, &utc);
  std::array<char, time_text_bytes> text{};
  const auto length = std::strftime(text.data(), text.size(), format, &utc);
  return {text.data(), length};
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(file), {});
}

std::string facts_text(const std::vector<crash_report::Fact>& facts) {
  std::string text;
  for (const auto& fact : facts)
    text += std::format("{}: {}\n", fact.name, fact.value);
  return text;
}

std::string plugins_text(const Options& options) {
  std::string text;
  if (!options.plugin_note.empty())
    text += options.plugin_note + "\n";
  for (const auto& plugin : options.plugins)
    text += std::format("{} | {} | {}\n", plugin.name,
                        plugin.version.empty() ? std::string("no version") : plugin.version,
                        plugin.status);
  if (text.empty())
    text = "No plug-ins found.\n";
  return text;
}

// Why the trace stays out, or nullopt when it can go in: it must be a
// readable file of at most max_trace_bytes whose lines are all at most
// max_trace_line_bytes, so it can be redacted a line-bounded chunk at a time.
std::optional<std::string> trace_problem(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || !std::filesystem::is_regular_file(path, error))
    return std::format("{} could not be read", path.filename().string());
  if (size > max_trace_bytes)
    return std::format("{} is larger than {} MiB", path.filename().string(),
                       max_trace_bytes / bytes_per_mebibyte);
  std::ifstream file(path, std::ios::binary);
  std::string chunk(trace_read_bytes, '\0');
  std::size_t line = 0;
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = static_cast<std::size_t>(file.gcount());
    for (std::size_t index = 0; index < count; ++index) {
      line = chunk[index] == '\n' ? 0 : line + 1;
      if (line > max_trace_line_bytes)
        return std::format("{} has a line longer than {} MiB", path.filename().string(),
                           max_trace_line_bytes / bytes_per_mebibyte);
    }
  }
  if (!file.eof())
    return std::format("{} could not be read", path.filename().string());
  return std::nullopt;
}

// Streams the trace into the open entry, redacting whole lines at a time: the
// home path never spans a newline, so no match is split between chunks.
Result<void> append_trace(zip::Writer& zip, const std::filesystem::path& path,
                          std::string_view home) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return Result<void>::failure(
        {ErrorCode::io_failure, std::format("diagnostics: cannot read {}", path.string())});
  std::string pending;
  std::string chunk(trace_read_bytes, '\0');
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    pending.append(chunk.data(), static_cast<std::size_t>(file.gcount()));
    const auto last_newline = pending.rfind('\n');
    if (last_newline == std::string::npos)
      continue;
    const auto complete = std::string_view(pending).substr(0, last_newline + 1);
    if (auto appended = zip.append(crash_report::redact(complete, home)); !appended)
      return appended;
    pending.erase(0, last_newline + 1);
  }
  if (!file.eof())
    return Result<void>::failure(
        {ErrorCode::io_failure, std::format("diagnostics: cannot read {}", path.string())});
  return zip.append(crash_report::redact(pending, home));
}

Result<void> prepare_directory(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (!error)
    std::filesystem::permissions(directory, static_cast<std::filesystem::perms>(directory_mode),
                                 error);
  if (error)
    return Result<void>::failure(
        {ErrorCode::io_failure,
         std::format("diagnostics: cannot prepare {}: {}", directory.string(), error.message())});
  return Result<void>::success();
}

// The bundle's entries, README last so it can say what was left out.
Result<void> write_entries(zip::Writer& zip, const Options& options, std::int64_t written_at) {
  const auto redacted = [&options](std::string_view text) {
    return crash_report::redact(text, options.home);
  };
  std::vector<std::string> notes;

  if (auto added =
          zip.add(session_entry, redacted(crash_report::describe_session(options.application)));
      !added)
    return added;
  if (auto added = zip.add(build_entry, redacted(facts_text(options.build))); !added)
    return added;
  if (auto added = zip.add(plugins_entry, redacted(plugins_text(options))); !added)
    return added;

  const auto logs = crash_report::log_files(options.log_directory);
  for (const auto& path : logs) {
    const auto text = crash_report::read_log(path);
    const auto name = path.filename().string();
    if (!text) {
      notes.push_back(std::format("Log {} could not be read.", name));
      continue;
    }
    if (auto added = zip.add(std::string(logs_folder) + name, redacted(*text)); !added)
      return added;
  }
  if (logs.empty())
    notes.emplace_back("No log files were found.");

  const auto report = crash_report::latest_report(options.crash_directory);
  const auto report_text = report ? read_text(*report) : std::nullopt;
  if (report_text) {
    if (auto added = zip.add(std::string(crash_folder) + report->filename().string(),
                             redacted(*report_text));
        !added)
      return added;
  } else {
    notes.emplace_back(report ? "The latest crash report could not be read."
                              : "No crash report: Oscillate has not recorded a crash.");
  }

  if (options.trace.empty()) {
    notes.emplace_back("No performance trace: none was requested.");
  } else if (const auto problem = trace_problem(options.trace)) {
    notes.push_back(std::format("No performance trace: {}.", *problem));
  } else {
    if (auto begun = zip.begin(std::string(trace_folder) + options.trace.filename().string());
        !begun)
      return begun;
    if (auto appended = append_trace(zip, options.trace, options.home); !appended)
      return appended;
    if (auto ended = zip.end(); !ended)
      return ended;
  }

  std::string readme;
  readme += "Oscillate diagnostics\n";
  readme += "=====================\n\n";
  readme += "Oscillate saved this bundle on this computer when you asked for it.\n";
  readme += "Nothing has been sent anywhere. Read it before attaching it to an issue:\n";
  readme += "the logs name projects, tracks, files and plugins. Your home folder is\n";
  readme += "shown as ~. It holds no audio and no project files.\n\n";
  readme += std::format("written: {}\n\n", format_utc(written_at, "%Y-%m-%dT%H:%M:%SZ"));
  readme += "session.txt    Oscillate, Qt, the system and the environment variables\n";
  readme += "               that change how Oscillate runs\n";
  readme += "build.txt      this build's optional features\n";
  readme += "plugins.txt    the VST3 plug-in scan: names, versions and status\n";
  readme += "logs/          the application log files, oldest first\n";
  readme += "crash-report/  the latest crash report, if there is one\n";
  readme += "trace/         a performance trace, if one was asked for\n";
  if (!notes.empty()) {
    readme += "\nLeft out:\n";
    for (const auto& note : notes)
      readme += std::format("  {}\n", note);
  }
  return zip.add(readme_entry, redacted(readme));
}

} // namespace

std::filesystem::path default_directory(const char* xdg_state_home, const char* home) {
  const auto absolute = [](const char* text) {
    return text != nullptr && text[0] == '/' ? std::filesystem::path(text)
                                             : std::filesystem::path{};
  };
  if (auto base = absolute(xdg_state_home); !base.empty())
    return base / "oscillate" / bundle_directory;
  if (auto base = absolute(home); !base.empty())
    return base / ".local" / "state" / "oscillate" / bundle_directory;
  return {};
}

Result<std::filesystem::path> write(const Options& options) {
  using Written = Result<std::filesystem::path>;
  if (options.directory.empty())
    return Written::failure({ErrorCode::invalid_argument, "diagnostics: no directory"});
  if (auto prepared = prepare_directory(options.directory); !prepared)
    return Written::failure(prepared.error());
  const UniqueFd directory(::open(options.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory)
    return Written::failure(io_error("diagnostics: open directory"));

  const auto written_at = now_seconds();
  const auto stamp = format_utc(written_at, "%Y%m%dT%H%M%SZ");
  std::string name;
  for (int attempt = 0;; ++attempt) {
    if (attempt == max_name_attempts)
      return Written::failure({ErrorCode::conflict, "diagnostics: no free bundle name"});
    name = attempt == 0 ? std::format("{}{}{}", bundle_prefix, stamp, bundle_extension)
                        : std::format("{}{}-{}{}", bundle_prefix, stamp, attempt, bundle_extension);
    struct stat existing{};
    if (::fstatat(directory.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) != 0)
      break;
  }
  const auto temporary = name + std::string(temporary_suffix);
  UniqueFd file(::openat(directory.get(), temporary.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, file_mode));
  if (!file)
    return Written::failure(io_error("diagnostics: create bundle"));
  const auto discard = [&directory, &temporary](Error error) {
    (void)::unlinkat(directory.get(), temporary.c_str(), 0);
    return Written::failure(std::move(error));
  };

  zip::Writer zip(file.get(), written_at);
  if (auto entries = write_entries(zip, options, written_at); !entries)
    return discard(entries.error());
  if (auto finished = zip.finish(); !finished)
    return discard(finished.error());
  auto published = publish_synced(std::move(file), directory.get(), temporary, name,
                                  PublishMode::no_replace, "diagnostics bundle");
  if (!published)
    return discard(published.error());
  return Written::success(options.directory / name);
}

} // namespace oscillate::foundation::diagnostics_bundle
