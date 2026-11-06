#include "oscillate/foundation/log.hpp"

#include "oscillate/foundation/file_io.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace oscillate::foundation {
namespace {
thread_local bool realtime_thread = false;
} // namespace

RealtimeThreadScope::RealtimeThreadScope() noexcept
    : previous_(std::exchange(realtime_thread, true)) {}
RealtimeThreadScope::~RealtimeThreadScope() { realtime_thread = previous_; }
bool on_realtime_thread() noexcept { return realtime_thread; }

} // namespace oscillate::foundation

namespace oscillate::foundation::log {
namespace {

constexpr std::array<std::string_view, 4> level_names{"error", "warning", "info", "debug"};
constexpr std::array<std::string_view, category_count> category_names{
    "engine", "device", "plugin", "media", "project", "processing", "app"};
constexpr std::string_view device_alias = "audio";
// A longer message is cut, so one line never forces more than one rotation.
constexpr std::size_t max_message_bytes = 16 * 1024;
constexpr std::string_view truncated_marker = " [truncated]";
constexpr mode_t directory_mode = 0700;
constexpr mode_t file_mode = 0600;
constexpr int milliseconds_per_second = 1000;

std::size_t category_index(Category category) noexcept {
  return static_cast<std::size_t>(category) - 1;
}

bool valid_category(Category category) noexcept {
  const auto value = static_cast<std::size_t>(category);
  return value >= 1 && value <= category_count;
}

std::string_view trim(std::string_view text) noexcept {
  constexpr std::string_view space = " \t\n\r";
  const auto first = text.find_first_not_of(space);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

void publish_filter(std::array<std::atomic<std::uint8_t>, category_count>& thresholds,
                    const Filter& filter) noexcept;

// Sinks and filter. The filter is published as one word per category so
// enabled() takes no lock.
struct State {
  std::mutex mutex;
  Options options;              // Under mutex.
  UniqueFd file;                // Under mutex.
  std::uint64_t file_bytes = 0; // Under mutex.
  bool file_failed = false;     // Under mutex: reported once, then quiet.
  std::array<std::atomic<std::uint8_t>, category_count> thresholds{};
  std::atomic<bool> to_stderr{true};

  State() noexcept { publish_filter(thresholds, Filter{}); }
};

void publish_filter(std::array<std::atomic<std::uint8_t>, category_count>& thresholds,
                    const Filter& filter) noexcept {
  for (std::size_t index = 0; index < category_count; ++index) {
    const auto level = filter.categories[index].value_or(filter.level);
    thresholds[index].store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
  }
}

State& state() {
  static State instance;
  return instance;
}

std::filesystem::path numbered(const Options& options, std::size_t number) {
  const std::string stem(Options::file_stem);
  return options.directory /
         (number == 0 ? stem + ".log" : stem + "." + std::to_string(number) + ".log");
}

// Under mutex. Opens the live file for appending, creating its directory.
bool open_file(State& s) noexcept {
  if (s.options.directory.empty() || s.file_failed)
    return false;
  if (s.file)
    return true;
  std::error_code error;
  std::filesystem::create_directories(s.options.directory, error);
  if (!error)
    std::filesystem::permissions(s.options.directory,
                                 static_cast<std::filesystem::perms>(directory_mode), error);
  const auto path = numbered(s.options, 0);
  s.file.reset(
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, file_mode));
  if (!s.file) {
    s.file_failed = true;
    return false;
  }
  struct stat info{};
  s.file_bytes = ::fstat(s.file.get(), &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
  return true;
}

// Under mutex. oscillate.log becomes oscillate.1.log and so on; the oldest
// beyond max_files is removed.
void rotate(State& s) noexcept {
  s.file.reset();
  s.file_bytes = 0;
  std::error_code error;
  const auto files = std::max<std::size_t>(s.options.max_files, 1);
  std::filesystem::remove(numbered(s.options, files - 1), error);
  for (std::size_t number = files - 1; number > 0; --number)
    std::filesystem::rename(numbered(s.options, number - 1), numbered(s.options, number), error);
}

void append_timestamp(std::string& line) {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
      milliseconds_per_second;
  std::tm utc{};
  ::gmtime_r(&seconds, &utc);
  std::array<char, 32> text{};
  const auto length = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &utc);
  line.append(text.data(), length);
  line += std::format(".{:03}Z", millis);
}

std::string_view kind_sentence(DiagnosticKind kind) noexcept {
  switch (kind) {
  case DiagnosticKind::block_silenced:
    return "audio block silenced because rendering failed";
  case DiagnosticKind::xrun:
    return "audio server xrun";
  case DiagnosticKind::oversized_block:
    return "audio block larger than the output can render";
  case DiagnosticKind::invalid_buffer:
    return "audio server gave no usable buffer";
  case DiagnosticKind::unspecified:
    break;
  }
  return "diagnostic event";
}

Level kind_level(DiagnosticKind kind) noexcept {
  switch (kind) {
  case DiagnosticKind::block_silenced:
  case DiagnosticKind::oversized_block:
    return Level::error;
  case DiagnosticKind::xrun:
  case DiagnosticKind::invalid_buffer:
  case DiagnosticKind::unspecified:
    break;
  }
  return Level::warning;
}

} // namespace

std::string_view level_name(Level level) noexcept {
  const auto index = static_cast<std::size_t>(level);
  return index < level_names.size() ? level_names[index] : "unknown";
}

std::string_view category_name(Category category) noexcept {
  return valid_category(category) ? category_names[category_index(category)] : "unknown";
}

std::optional<Level> parse_level(std::string_view text) noexcept {
  text = trim(text);
  for (std::size_t index = 0; index < level_names.size(); ++index)
    if (text == level_names[index])
      return static_cast<Level>(index);
  return std::nullopt;
}

std::optional<Category> parse_category(std::string_view text) noexcept {
  text = trim(text);
  if (text == device_alias)
    return Category::device;
  for (std::size_t index = 0; index < category_names.size(); ++index)
    if (text == category_names[index])
      return static_cast<Category>(index + 1);
  return std::nullopt;
}

bool Filter::passes(Category category, Level message) const noexcept {
  const auto threshold =
      valid_category(category) ? categories[category_index(category)].value_or(level) : level;
  return message <= threshold;
}

ParsedFilter parse_filter(std::string_view spec) {
  ParsedFilter parsed;
  while (!spec.empty()) {
    const auto comma = spec.find(',');
    const auto item = trim(spec.substr(0, comma));
    spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
    if (item.empty())
      continue;
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) {
      if (const auto level = parse_level(item))
        parsed.filter.level = *level;
      else
        parsed.rejected.emplace_back(item);
      continue;
    }
    const auto category = parse_category(item.substr(0, equals));
    const auto level = parse_level(item.substr(equals + 1));
    if (category && level)
      parsed.filter.categories[category_index(*category)] = *level;
    else
      parsed.rejected.emplace_back(item);
  }
  return parsed;
}

std::filesystem::path default_directory(const char* xdg_state_home, const char* home) {
  const auto absolute = [](const char* text) {
    return text != nullptr && text[0] == '/' ? std::filesystem::path(text)
                                             : std::filesystem::path{};
  };
  if (auto base = absolute(xdg_state_home); !base.empty())
    return base / "oscillate" / "logs";
  if (auto base = absolute(home); !base.empty())
    return base / ".local" / "state" / "oscillate" / "logs";
  return {};
}

Options options_from_environment() {
  Options options;
  if (const char* spec = std::getenv("OSCILLATE_LOG")) {
    options.filter = parse_filter(spec).filter;
    options.stderr_level = Level::debug;
  }
  options.directory = default_directory(std::getenv("XDG_STATE_HOME"), std::getenv("HOME"));
  return options;
}

void configure(Options options) {
  State& s = state();
  const std::lock_guard lock(s.mutex);
  s.file.reset();
  s.file_bytes = 0;
  s.file_failed = false;
  s.options = std::move(options);
  s.options.max_files = std::max<std::size_t>(s.options.max_files, 1);
  publish_filter(s.thresholds, s.options.filter);
  s.to_stderr.store(s.options.to_stderr, std::memory_order_relaxed);
}

std::filesystem::path current_file() {
  State& s = state();
  const std::lock_guard lock(s.mutex);
  return s.options.directory.empty() ? std::filesystem::path{} : numbered(s.options, 0);
}

bool enabled(Category category, Level level) noexcept {
  if (!valid_category(category))
    return false;
  return static_cast<std::uint8_t>(level) <=
         state().thresholds[category_index(category)].load(std::memory_order_relaxed);
}

void write(Level level, Category category, std::string_view message) noexcept {
  assert(!on_realtime_thread() && "never log from the audio callback or its workers");
  if (!enabled(category, level))
    return;
  try {
    std::string line;
    append_timestamp(line);
    line += std::format(" {} {} [{}]: ", level_name(level), category_name(category),
                        static_cast<long>(::syscall(SYS_gettid)));
    if (message.size() > max_message_bytes) {
      line.append(message.substr(0, max_message_bytes));
      line += truncated_marker;
    } else {
      line += message;
    }
    line += '\n';
    State& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.to_stderr.load(std::memory_order_relaxed) && level <= s.options.stderr_level)
      (void)write_all(STDERR_FILENO, line, "log to stderr");
    if (!open_file(s))
      return;
    if (s.file_bytes != 0 && s.file_bytes + line.size() > s.options.max_file_bytes) {
      rotate(s);
      if (!open_file(s))
        return;
    }
    if (write_all(s.file.get(), line, "log to file"))
      s.file_bytes += line.size();
  } catch (...) { // NOLINT(bugprone-empty-catch): logging must never throw into its caller.
  }
}

void flush() noexcept {
  State& s = state();
  const std::lock_guard lock(s.mutex);
  if (s.file)
    (void)::fdatasync(s.file.get());
}

std::string describe(const DiagnosticEvent& event, std::uint64_t repeats) {
  std::string text(kind_sentence(event.kind));
  if (repeats > 1)
    text += std::format(" ({} times)", repeats);
  if (event.kind == DiagnosticKind::block_silenced || event.kind == DiagnosticKind::oversized_block)
    text += std::format(", {} frames", event.detail);
  if (event.sample_position >= 0 && event.kind == DiagnosticKind::block_silenced)
    text += std::format(" at sample {}", event.sample_position);
  return text;
}

DrainSummary drain_diagnostics() {
  DiagnosticRing& ring = realtime_diagnostics();
  DrainSummary summary;
  // One line per kind: a burst of xruns is one line, not hundreds.
  struct Bucket {
    DiagnosticEvent last{};
    std::uint64_t count = 0;
  };
  constexpr std::size_t kinds = static_cast<std::size_t>(DiagnosticKind::invalid_buffer) + 1;
  std::array<Bucket, kinds> buckets{};
  DiagnosticEvent event{};
  // Bounded: a producer outrunning the drain cannot hold the UI thread.
  for (std::size_t taken = 0; taken < DiagnosticRing::capacity && ring.try_take(event); ++taken) {
    ++summary.events;
    auto index = static_cast<std::size_t>(event.kind);
    if (index >= kinds)
      index = 0;
    buckets[index].last = event;
    ++buckets[index].count;
  }
  summary.dropped = ring.take_dropped();
  for (const auto& bucket : buckets) {
    if (bucket.count == 0)
      continue;
    const auto sentence = describe(bucket.last, bucket.count);
    write(kind_level(bucket.last.kind), bucket.last.category, sentence);
    if (summary.status.empty() || kind_level(bucket.last.kind) == Level::error) {
      summary.status = sentence;
      summary.status.front() =
          static_cast<char>(std::toupper(static_cast<unsigned char>(summary.status.front())));
    }
  }
  if (summary.dropped != 0)
    write(Level::warning, Category::engine,
          std::format("{} realtime diagnostics dropped: the ring was full", summary.dropped));
  return summary;
}

} // namespace oscillate::foundation::log
