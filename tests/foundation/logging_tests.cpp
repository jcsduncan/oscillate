#include "oscillate/foundation/diagnostic.hpp"
#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/test/check.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

namespace log = oscillate::foundation::log;
namespace trace = oscillate::foundation::trace;
using oscillate::foundation::DiagnosticCategory;
using oscillate::foundation::DiagnosticEvent;
using oscillate::foundation::DiagnosticKind;
using oscillate::foundation::DiagnosticRing;
using oscillate::test::require;

constexpr std::uint32_t block_frames = 128;

std::filesystem::path scratch(std::string_view name) {
  auto path = std::filesystem::temp_directory_path() /
              ("oscillate-" + std::string(name) + "-" + std::to_string(::getpid()));
  std::filesystem::remove_all(path);
  return path;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}

void level_parsing() {
  REQUIRE_EQ(log::parse_level("debug"), log::Level::debug);
  REQUIRE_EQ(log::parse_level(" warning "), log::Level::warning);
  REQUIRE_FALSE(log::parse_level("loud").has_value());
  REQUIRE_EQ(log::parse_category("audio"), DiagnosticCategory::device, "audio is the device alias");
  REQUIRE_EQ(log::parse_category("app"), DiagnosticCategory::application);

  const auto empty = log::parse_filter("");
  REQUIRE_EQ(empty.filter.level, log::Filter::default_level);
  REQUIRE_TRUE(empty.rejected.empty());

  const auto parsed = log::parse_filter("audio=debug, warning,plugin=error");
  REQUIRE_EQ(parsed.filter.level, log::Level::warning);
  REQUIRE_TRUE(parsed.filter.passes(DiagnosticCategory::device, log::Level::debug));
  REQUIRE_FALSE(parsed.filter.passes(DiagnosticCategory::engine, log::Level::info));
  REQUIRE_TRUE(parsed.filter.passes(DiagnosticCategory::engine, log::Level::warning));
  REQUIRE_FALSE(parsed.filter.passes(DiagnosticCategory::plugin, log::Level::warning));
  REQUIRE_TRUE(parsed.filter.passes(DiagnosticCategory::plugin, log::Level::error));

  const auto rejected = log::parse_filter("bogus,plugin=loud,nothing=debug,info");
  REQUIRE_EQ(rejected.rejected.size(), 3U, "unknown items are reported, not applied");
  REQUIRE_EQ(rejected.filter.level, log::Level::info);
}

void directory_fallback() {
  REQUIRE_EQ(log::default_directory("/state", "/home/me"),
             std::filesystem::path("/state/oscillate/logs"));
  REQUIRE_EQ(log::default_directory("relative", "/home/me"),
             std::filesystem::path("/home/me/.local/state/oscillate/logs"),
             "a relative XDG_STATE_HOME is ignored, as the XDG spec says");
  REQUIRE_EQ(log::default_directory(nullptr, "/home/me"),
             std::filesystem::path("/home/me/.local/state/oscillate/logs"));
  REQUIRE_TRUE(log::default_directory(nullptr, nullptr).empty());
}

void rotation_is_bounded() {
  constexpr std::uint64_t max_file_bytes = 512;
  constexpr std::size_t max_files = 3;
  constexpr int messages = 200;
  const auto directory = scratch("log-rotation");
  log::Options options;
  options.filter.level = log::Level::debug;
  options.to_stderr = false;
  options.directory = directory;
  options.max_file_bytes = max_file_bytes;
  options.max_files = max_files;
  log::configure(options);
  for (int index = 0; index < messages; ++index)
    log::info(DiagnosticCategory::engine, "message {:04}", index);
  log::debug(DiagnosticCategory::application, "the last one");

  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    ++files;
    REQUIRE_LE(entry.file_size(), max_file_bytes, "no file grows past the rotation size");
  }
  REQUIRE_EQ(files, max_files, "rotation keeps a bounded number of files");
  REQUIRE_EQ(log::current_file(), directory / "oscillate.log");
  const auto live = read_file(directory / "oscillate.log");
  REQUIRE_NE(live.find("debug app ["), std::string::npos, "lines carry level and category");
  REQUIRE_NE(live.find("the last one"), std::string::npos, "the newest line is in the live file");
  const auto oldest = read_file(directory / "oscillate.2.log");
  REQUIRE_EQ(oldest.find("message 0000"), std::string::npos, "the oldest lines were rotated away");

  options.filter.level = log::Level::warning;
  log::configure(options);
  REQUIRE_FALSE(log::enabled(DiagnosticCategory::engine, log::Level::info));
  log::info(DiagnosticCategory::engine, "filtered out");
  REQUIRE_EQ(read_file(directory / "oscillate.log").find("filtered out"), std::string::npos);
  log::configure({.filter = {}, .to_stderr = false, .directory = {}});
  std::filesystem::remove_all(directory);
}

DiagnosticEvent event(DiagnosticKind kind, std::uint32_t detail) {
  return {.subject = {},
          .correlation = 0,
          .sample_position = 0,
          .detail = detail,
          .code = oscillate::foundation::ErrorCode::internal,
          .category = DiagnosticCategory::device,
          .kind = kind};
}

void ring_overflow_drops_and_never_blocks() {
  DiagnosticRing ring;
  for (std::uint32_t index = 0; index < DiagnosticRing::capacity; ++index)
    REQUIRE_TRUE(ring.try_publish(event(DiagnosticKind::xrun, index)));
  REQUIRE_FALSE(ring.try_publish(event(DiagnosticKind::xrun, DiagnosticRing::capacity)),
                "a full ring rejects rather than waits");
  REQUIRE_EQ(ring.take_dropped(), 1U);
  REQUIRE_EQ(ring.take_dropped(), 0U, "the drop count resets when taken");
  DiagnosticEvent taken{};
  for (std::uint32_t index = 0; index < DiagnosticRing::capacity; ++index) {
    REQUIRE_TRUE(ring.try_take(taken));
    REQUIRE_EQ(taken.detail, index, "records come out in order");
  }
  REQUIRE_FALSE(ring.try_take(taken), "an empty ring has nothing to take");

  // Producers racing with no consumer: exactly the capacity is accepted.
  constexpr std::size_t producers = 4;
  constexpr std::size_t per_producer = DiagnosticRing::capacity;
  std::atomic<std::size_t> accepted{0};
  std::vector<std::thread> threads;
  threads.reserve(producers);
  for (std::size_t producer = 0; producer < producers; ++producer) {
    threads.emplace_back([&] {
      for (std::size_t index = 0; index < per_producer; ++index)
        if (ring.try_publish(event(DiagnosticKind::block_silenced, block_frames)))
          accepted.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto& thread : threads)
    thread.join();
  REQUIRE_EQ(accepted.load(), DiagnosticRing::capacity);
  REQUIRE_EQ(ring.take_dropped(), producers * per_producer - DiagnosticRing::capacity,
             "every rejected record is counted");
}

void drain_writes_one_line_per_kind() {
  constexpr std::uint32_t silenced = 3;
  constexpr std::int64_t sample = 4800;
  const auto directory = scratch("log-drain");
  log::configure({.filter = {}, .to_stderr = false, .directory = directory});
  auto& ring = oscillate::foundation::realtime_diagnostics();
  for (std::uint32_t index = 0; index < silenced; ++index) {
    auto record = event(DiagnosticKind::block_silenced, block_frames);
    record.sample_position = sample;
    REQUIRE_TRUE(ring.try_publish(record));
  }
  REQUIRE_TRUE(ring.try_publish(event(DiagnosticKind::xrun, 0)));
  const auto summary = log::drain_diagnostics();
  REQUIRE_EQ(summary.events, silenced + 1);
  REQUIRE_EQ(summary.dropped, 0U);
  REQUIRE_NE(summary.status.find("Audio block silenced"), std::string::npos,
             "an error outranks a warning in the status line");
  const auto text = read_file(directory / "oscillate.log");
  REQUIRE_NE(text.find("error device ["), std::string::npos);
  REQUIRE_NE(text.find("(3 times), 128 frames at sample 4800"), std::string::npos,
             "a burst becomes one line");
  REQUIRE_NE(text.find("warning device ["), std::string::npos);
  REQUIRE_TRUE(log::drain_diagnostics().status.empty(), "a second drain finds nothing");
  log::configure({.filter = {}, .to_stderr = false, .directory = {}});
  std::filesystem::remove_all(directory);
}

void realtime_marker_is_per_thread() {
  using oscillate::foundation::on_realtime_thread;
  REQUIRE_FALSE(on_realtime_thread());
  bool marked = false;
  std::thread([&] {
    const oscillate::foundation::RealtimeThreadScope realtime;
    marked = on_realtime_thread();
  }).join();
  REQUIRE_TRUE(marked);
  REQUIRE_FALSE(on_realtime_thread(), "marking is per thread");
}

// A fake driver runs the callback on the calling (control) thread: the mark
// must end with the callback, or a later control-side log line would assert.
void realtime_scope_restores_a_borrowed_thread() {
  using oscillate::foundation::on_realtime_thread;
  using oscillate::foundation::RealtimeThreadScope;
  REQUIRE_FALSE(on_realtime_thread());
  {
    const RealtimeThreadScope callback;
    REQUIRE_TRUE(on_realtime_thread(), "marked inside the callback");
    {
      // A nested worker loop on the callback thread keeps the outer mark.
      const RealtimeThreadScope nested;
      REQUIRE_TRUE(on_realtime_thread());
    }
    REQUIRE_TRUE(on_realtime_thread(), "an inner scope restores the outer mark");
  }
  REQUIRE_FALSE(on_realtime_thread(), "the borrowed thread is control again");
  // The assertion in log::write would fire here if the mark had stuck.
  log::configure({.filter = {}, .to_stderr = false, .directory = {}});
  log::write(log::Level::info, DiagnosticCategory::application,
             "control-side line after a borrowed callback");
}

void tracer_buffer_sizing() {
  constexpr std::size_t default_bytes = trace::default_buffer_megabytes * trace::bytes_per_megabyte;
  constexpr std::size_t requested_megabytes = 64;
  REQUIRE_EQ(trace::buffer_bytes_from(nullptr), default_bytes);
  REQUIRE_EQ(trace::buffer_bytes_from("64"), requested_megabytes * trace::bytes_per_megabyte);
  for (const char* invalid : {"", "0", "abc", "12x", "-3", "5000"})
    REQUIRE_EQ(trace::buffer_bytes_from(invalid), default_bytes, invalid);
  REQUIRE_LT(default_bytes, std::size_t{256} * trace::bytes_per_megabyte,
             "the default pool is smaller than the old fixed 256 MiB of rings");

  // Labels cost nothing while off, so one made before a recording is absent.
  trace::label("node", 1, "Before");
  REQUIRE_TRUE(trace::start(1), "a tiny budget still records");
  REQUIRE_EQ(trace::capacity_events(), trace::chunk_events, "at least one chunk");
  trace::label("node", 2, "During");
  {
    OSCILLATE_TRACE_SPAN("node", 1);
    OSCILLATE_TRACE_SPAN("node", 2);
  }
  const auto path = scratch("trace-sizing.json");
  REQUIRE_EQ(oscillate::test::take(trace::stop_and_write(path)), 2U);
  const auto json = read_file(path);
  REQUIRE_EQ(json.find("Before"), std::string::npos);
  REQUIRE_NE(json.find("During"), std::string::npos);
  REQUIRE_NE(json.find("\"dropped_events\":0"), std::string::npos);

  // A new recording starts with no labels.
  REQUIRE_TRUE(trace::start(trace::chunk_bytes));
  {
    OSCILLATE_TRACE_SPAN("node", 2);
  }
  REQUIRE_EQ(oscillate::test::take(trace::stop_and_write(path)), 1U);
  REQUIRE_EQ(read_file(path).find("During"), std::string::npos);
  std::filesystem::remove(path);
  REQUIRE_NE(trace::next_instance(), trace::next_instance(), "instances are distinct");
}

} // namespace

int main() {
  try {
    level_parsing();
    directory_fallback();
    rotation_is_bounded();
    ring_overflow_drops_and_never_blocks();
    drain_writes_one_line_per_kind();
    realtime_marker_is_per_thread();
    realtime_scope_restores_a_borrowed_thread();
    tracer_buffer_sizing();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
