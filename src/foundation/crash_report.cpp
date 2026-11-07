#include "oscillate/foundation/crash_report.hpp"

#include "oscillate/foundation/file_io.hpp"
#include "oscillate/foundation/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

extern char** environ; // NOLINT(readability-redundant-declaration): POSIX declares it nowhere else.

namespace oscillate::foundation::crash_report {
namespace {

constexpr mode_t directory_mode = 0700;
constexpr mode_t file_mode = 0600;
constexpr std::string_view report_directory = "crash-reports";
constexpr std::string_view temporary_suffix = ".partial";
constexpr std::string_view signal_tag = "signal";
constexpr std::string_view fatal_tag = "fatal";
// A fatal message longer than this is cut; the full text is in the log.
constexpr std::size_t max_fatal_message_bytes = 512;
// The handlers run on their own stack, so a stack overflow on the main thread
// can still write the marker.
constexpr std::size_t alternate_stack_bytes = std::size_t{64} * 1024;
// Room for "signal <number> <seconds>\n".
constexpr std::size_t signal_line_bytes = 64;
constexpr int decimal_base = 10;
// The log's file names: oscillate.log, oscillate.1.log...
constexpr std::string_view log_stem = log::Options::file_stem;
constexpr std::string_view log_extension = ".log";

struct FatalSignal {
  int number;
  std::string_view name;
  std::string_view meaning;
};

constexpr std::array<FatalSignal, 5> fatal_signals{{
    {SIGSEGV, "SIGSEGV", "invalid memory access"},
    {SIGBUS, "SIGBUS", "bus error"},
    {SIGFPE, "SIGFPE", "arithmetic fault"},
    {SIGILL, "SIGILL", "illegal instruction"},
    {SIGABRT, "SIGABRT", "abort (an assertion or fatal error)"},
}};

// Variables outside OSCILLATE_* that change how Oscillate runs.
constexpr std::array<std::string_view, 12> reported_variables{"XDG_SESSION_TYPE",
                                                              "XDG_CURRENT_DESKTOP",
                                                              "WAYLAND_DISPLAY",
                                                              "DISPLAY",
                                                              "QT_QPA_PLATFORM",
                                                              "QT_SCALE_FACTOR",
                                                              "QT_QUICK_BACKEND",
                                                              "PIPEWIRE_LATENCY",
                                                              "PIPEWIRE_QUANTUM",
                                                              "JACK_DEFAULT_SERVER",
                                                              "LANG",
                                                              "LC_ALL"};
constexpr std::string_view own_prefix = "OSCILLATE_";

// Signal-handler state, written by arm() before any handler is installed.
std::array<char, PATH_MAX> marker_path{};
std::array<struct sigaction, fatal_signals.size()> previous_actions{};
std::atomic<bool> armed{false};
alignas(std::max_align_t) std::array<std::byte, alternate_stack_bytes> alternate_stack{};

// Async-signal-safe: appends `value` in decimal.
void append_decimal(std::array<char, signal_line_bytes>& line, std::size_t& length,
                    std::uint64_t value) noexcept {
  std::array<char, signal_line_bytes> digits{};
  std::size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % decimal_base);
    value /= decimal_base;
  } while (value != 0 && count < digits.size());
  while (count != 0 && length < line.size())
    line[length++] = digits[--count];
}

void append_text(std::array<char, signal_line_bytes>& line, std::size_t& length,
                 std::string_view text) noexcept {
  for (const char c : text)
    if (length < line.size())
      line[length++] = c;
}

// Async-signal-safe: open, write and close only.
void append_marker(const char* bytes, std::size_t size) noexcept {
  const int fd =
      ::open(marker_path.data(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, file_mode);
  if (fd < 0)
    return;
  while (size != 0) {
    const auto written = ::write(fd, bytes, size);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      break;
    bytes += written;
    size -= static_cast<std::size_t>(written);
  }
  ::close(fd);
}

void on_fatal_signal(int number, siginfo_t* /*info*/, void* /*context*/) {
  const int saved_errno = errno;
  std::array<char, signal_line_bytes> line{};
  std::size_t length = 0;
  timespec now{};
  ::clock_gettime(CLOCK_REALTIME, &now);
  append_text(line, length, signal_tag);
  append_text(line, length, " ");
  append_decimal(line, length, static_cast<std::uint64_t>(number));
  append_text(line, length, " ");
  append_decimal(line, length, static_cast<std::uint64_t>(std::max<time_t>(now.tv_sec, 0)));
  append_text(line, length, "\n");
  append_marker(line.data(), length);
  // Hand the signal back to whoever had it, so the process still dies with
  // it (and a core dump or a sanitizer report still happens).
  for (std::size_t index = 0; index < fatal_signals.size(); ++index) {
    if (fatal_signals[index].number != number)
      continue;
    struct sigaction restore = previous_actions[index];
    if (restore.sa_handler == SIG_IGN)
      restore.sa_handler = SIG_DFL;
    ::sigaction(number, &restore, nullptr);
  }
  errno = saved_errno;
  ::raise(number);
}

std::string utc_time(std::int64_t seconds) {
  const auto value = static_cast<std::time_t>(seconds);
  std::tm utc{};
  ::gmtime_r(&value, &utc);
  std::array<char, 32> text{};
  const auto length = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return {text.data(), length};
}

std::string compact_utc_time(std::int64_t seconds) {
  const auto value = static_cast<std::time_t>(seconds);
  std::tm utc{};
  ::gmtime_r(&value, &utc);
  std::array<char, 32> text{};
  const auto length = std::strftime(text.data(), text.size(), "%Y%m%dT%H%M%SZ", &utc);
  return {text.data(), length};
}

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(file), {});
}

std::string_view trim(std::string_view text) noexcept {
  constexpr std::string_view space = " \t\r\n\"";
  const auto first = text.find_first_not_of(space);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

// The value of `key=` in a KEY=value file such as /etc/os-release.
std::string key_value(std::string_view text, std::string_view key) {
  while (!text.empty()) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == '=')
      return std::string(trim(line.substr(key.size() + 1)));
  }
  return {};
}

// The first line of `text` starting with `prefix`, without the prefix.
std::string line_after(std::string_view text, std::string_view prefix) {
  const auto at = text.find(prefix);
  if (at == std::string_view::npos)
    return {};
  auto rest = text.substr(at + prefix.size());
  return std::string(trim(rest.substr(0, rest.find('\n'))));
}

bool reported_variable(std::string_view name) {
  return name.starts_with(own_prefix) ||
         std::ranges::find(reported_variables, name) != reported_variables.end();
}

std::string signal_description(int number) {
  for (const auto& known : fatal_signals)
    if (known.number == number)
      return std::format("{} ({}, {})", number, known.name, known.meaning);
  return std::to_string(number);
}

struct Marker {
  std::optional<int> signal;
  std::optional<std::int64_t> time;
  std::vector<std::string> fatal_messages;
};

template <typename T> std::optional<T> parse_number(std::string_view text) {
  T value{};
  const auto* end = text.data() + text.size();
  const auto [rest, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc{} || rest != end)
    return std::nullopt;
  return value;
}

// Lines: "signal <number> <seconds>" from a handler, "fatal <seconds> <message>"
// from note_fatal_error. Unreadable lines are skipped.
Marker parse_marker(std::string_view text) {
  Marker marker;
  while (!text.empty()) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    const auto first = line.find(' ');
    if (first == std::string_view::npos)
      continue;
    const auto tag = line.substr(0, first);
    const auto rest = line.substr(first + 1);
    const auto second = rest.find(' ');
    const auto field = rest.substr(0, second);
    const auto tail =
        second == std::string_view::npos ? std::string_view{} : rest.substr(second + 1);
    if (tag == signal_tag) {
      marker.signal = parse_number<int>(field);
      if (const auto seconds = parse_number<std::int64_t>(tail))
        marker.time = seconds;
    } else if (tag == fatal_tag) {
      if (const auto seconds = parse_number<std::int64_t>(field); seconds && !marker.time)
        marker.time = seconds;
      marker.fatal_messages.emplace_back(tail);
    }
  }
  return marker;
}

// The rotated log files that exist, oldest first: oscillate.<n>.log from the
// highest n down, then oscillate.log.
std::vector<std::filesystem::path> log_files(const std::filesystem::path& directory) {
  std::vector<std::pair<int, std::filesystem::path>> numbered;
  std::vector<std::filesystem::path> files;
  if (directory.empty())
    return files;
  std::error_code error;
  const auto live_name = std::format("{}{}", log_stem, log_extension);
  std::optional<std::filesystem::path> live;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (!entry.is_regular_file(error))
      continue;
    const auto name = entry.path().filename().string();
    if (name == live_name) {
      live = entry.path();
      continue;
    }
    const std::string_view view(name);
    if (!view.starts_with(log_stem) || !view.ends_with(log_extension) ||
        view.size() <= log_stem.size() + log_extension.size() + 1 || view[log_stem.size()] != '.')
      continue;
    const auto digits =
        view.substr(log_stem.size() + 1, view.size() - log_stem.size() - 1 - log_extension.size());
    if (const auto number = parse_number<int>(digits))
      numbered.emplace_back(*number, entry.path());
  }
  std::ranges::sort(numbered, std::greater{}, &std::pair<int, std::filesystem::path>::first);
  for (auto& [number, path] : numbered)
    files.push_back(std::move(path));
  if (live)
    files.push_back(std::move(*live));
  return files;
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
         std::format("crash reports: cannot prepare {}: {}", directory.string(), error.message())});
  return Result<void>::success();
}

Result<void> write_file(const std::filesystem::path& directory, const std::string& name,
                        std::string_view text, PublishMode mode) {
  const UniqueFd dir(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!dir)
    return Result<void>::failure(io_error("crash reports: open directory"));
  const std::string temporary = name + std::string(temporary_suffix);
  UniqueFd file(::openat(dir.get(), temporary.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, file_mode));
  if (!file)
    return Result<void>::failure(io_error("crash reports: create file"));
  if (auto written = write_all(file.get(), text, "crash reports: write file"); !written) {
    (void)::unlinkat(dir.get(), temporary.c_str(), 0);
    return written;
  }
  auto published = publish_synced(std::move(file), dir.get(), temporary, name, mode,
                                  "crash reports: publish file");
  if (!published) {
    (void)::unlinkat(dir.get(), temporary.c_str(), 0);
    return Result<void>::failure(published.error());
  }
  return Result<void>::success();
}

std::string build_report(const Marker& marker, const std::optional<std::string>& session,
                         const std::vector<std::filesystem::path>& logs) {
  std::string report;
  report += "Oscillate crash report\n";
  report += "======================\n\n";
  report += "Oscillate wrote this file on this computer after it stopped unexpectedly.\n";
  report += "Nothing has been sent anywhere. Read it before attaching it to an issue:\n";
  report += "the log names projects, tracks, files and plugins. Your home folder is\n";
  report += "shown as ~.\n\n";

  report += "## Crash\n\n";
  if (marker.signal)
    report += std::format("signal: {}\n", signal_description(*marker.signal));
  for (const auto& message : marker.fatal_messages)
    report += std::format("fatal error: {}\n", message);
  if (!marker.signal && marker.fatal_messages.empty())
    report += "cause: not recorded\n";
  if (marker.time)
    report += std::format("time: {}\n", utc_time(*marker.time));
  report += std::format("report written: {}\n\n", utc_time(now_seconds()));

  report += "## Session\n\n";
  report += session ? *session : std::string("not recorded\n");
  if (!report.ends_with('\n'))
    report += '\n';

  for (const auto& path : logs) {
    report += std::format("\n## Log: {}\n\n", path.filename().string());
    const auto text = read_text(path);
    report += text ? *text : std::string("unreadable\n");
    if (!report.ends_with('\n'))
      report += '\n';
  }
  if (logs.empty())
    report += "\n## Log\n\nno log files\n";
  return report;
}

} // namespace

std::filesystem::path default_directory(const char* xdg_state_home, const char* home) {
  const auto absolute = [](const char* text) {
    return text != nullptr && text[0] == '/' ? std::filesystem::path(text)
                                             : std::filesystem::path{};
  };
  if (auto base = absolute(xdg_state_home); !base.empty())
    return base / "oscillate" / report_directory;
  if (auto base = absolute(home); !base.empty())
    return base / ".local" / "state" / "oscillate" / report_directory;
  return {};
}

std::string describe_session(std::span<const Fact> application) {
  std::string text;
  for (const auto& fact : application)
    text += std::format("{}: {}\n", fact.name, fact.value);
  text += std::format("started: {}\n", utc_time(now_seconds()));

  struct utsname kernel{};
  if (::uname(&kernel) == 0)
    text += std::format("kernel: {} {} {} {}\n", kernel.sysname, kernel.release, kernel.version,
                        kernel.machine);
  if (const auto release = read_text("/etc/os-release"))
    if (auto name = key_value(*release, "PRETTY_NAME"); !name.empty())
      text += std::format("distribution: {}\n", name);
  text += std::format("processors: {}\n", std::thread::hardware_concurrency());
  if (const auto memory = read_text("/proc/meminfo"))
    if (auto total = line_after(*memory, "MemTotal:"); !total.empty())
      text += std::format("memory: {}\n", total);

  std::vector<std::string> variables;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view assignment(*entry);
    const auto equals = assignment.find('=');
    if (equals != std::string_view::npos && reported_variable(assignment.substr(0, equals)))
      variables.emplace_back(assignment);
  }
  std::ranges::sort(variables);
  text += "environment:";
  if (variables.empty())
    text += " none set";
  text += '\n';
  for (const auto& variable : variables)
    text += std::format("  {}\n", variable);
  return text;
}

Result<void> arm(const std::filesystem::path& directory, std::string_view session) {
  if (armed.load(std::memory_order_acquire))
    return Result<void>::failure(
        {ErrorCode::conflict, "crash reports: the handlers are already armed"});
  const auto marker = (directory / marker_name).string();
  if (directory.empty() || marker.size() >= marker_path.size())
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "crash reports: no usable directory"});
  if (auto prepared = prepare_directory(directory); !prepared)
    return prepared;
  if (auto written =
          write_file(directory, std::string(session_name), session, PublishMode::replace);
      !written)
    return written;

  std::ranges::fill(marker_path, '\0');
  std::ranges::copy(marker, marker_path.begin());

  stack_t current{};
  if (::sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) != 0) {
    stack_t stack{};
    stack.ss_sp = alternate_stack.data();
    stack.ss_size = alternate_stack.size();
    (void)::sigaltstack(&stack, nullptr);
  }
  struct sigaction action{};
  action.sa_sigaction = on_fatal_signal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&action.sa_mask);
  for (std::size_t index = 0; index < fatal_signals.size(); ++index)
    if (::sigaction(fatal_signals[index].number, &action, &previous_actions[index]) != 0)
      return Result<void>::failure(io_error("crash reports: install signal handler"));
  armed.store(true, std::memory_order_release);
  return Result<void>::success();
}

void note_fatal_error(std::string_view message) noexcept {
  if (!armed.load(std::memory_order_acquire))
    return;
  try {
    std::string flat(message.substr(0, max_fatal_message_bytes));
    std::ranges::replace(flat, '\n', ' ');
    const auto line = std::format("{} {} {}\n", fatal_tag, now_seconds(), flat);
    append_marker(line.data(), line.size());
  } catch (...) { // NOLINT(bugprone-empty-catch): the process is already failing.
  }
}

Result<std::optional<std::filesystem::path>> collect(const CollectOptions& options) {
  using Collected = Result<std::optional<std::filesystem::path>>;
  if (options.directory.empty())
    return Collected::success(std::nullopt);
  const auto marker_file = options.directory / marker_name;
  std::error_code error;
  if (!std::filesystem::is_regular_file(marker_file, error))
    return Collected::success(std::nullopt);
  const auto marker = parse_marker(read_text(marker_file).value_or(std::string{}));
  const auto session = read_text(options.directory / session_name);
  const auto report =
      redact(build_report(marker, session, log_files(options.log_directory)), options.home);

  const auto stamp = compact_utc_time(marker.time.value_or(now_seconds()));
  std::string name;
  for (int attempt = 0;; ++attempt) {
    name = attempt == 0 ? std::format("{}{}{}", report_prefix, stamp, report_extension)
                        : std::format("{}{}-{}{}", report_prefix, stamp, attempt, report_extension);
    auto written = write_file(options.directory, name, report, PublishMode::no_replace);
    if (written)
      break;
    if (!std::filesystem::exists(options.directory / name, error))
      return Collected::failure(written.error());
  }
  std::filesystem::remove(marker_file, error);
  if (error)
    return Collected::failure(
        {ErrorCode::io_failure, std::format("crash reports: cannot remove {}: {}",
                                            marker_file.string(), error.message())});
  return Collected::success(options.directory / name);
}

std::string redact(std::string_view text, std::string_view home) {
  while (home.size() > 1 && home.ends_with('/'))
    home.remove_suffix(1);
  if (home.empty() || home == "/")
    return std::string(text);
  const auto continues_name = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
  };
  std::string result;
  result.reserve(text.size());
  std::size_t from = 0;
  for (auto at = text.find(home); at != std::string_view::npos; at = text.find(home, from)) {
    const auto after = at + home.size();
    result.append(text.substr(from, at - from));
    if (after < text.size() && continues_name(text[after]))
      result.append(home);
    else
      result.append(home_placeholder);
    from = after;
  }
  result.append(text.substr(from));
  return result;
}

} // namespace oscillate::foundation::crash_report
