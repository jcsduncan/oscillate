#pragma once

#include "oscillate/foundation/error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

// A performance tracer (ADR-0061) for every thread, the audio callback
// included: timed spans, instants and counters, written out as a Chrome trace
// that ui.perfetto.dev opens.
//
// start() allocates one event pool from a byte budget (OSCILLATE_TRACE_BUFFER_MB);
// threads take fixed-size chunks of it as they record, so a busy thread gets
// more of the pool than an idle one. Recording allocates nothing and takes no
// lock; each thread enters through its own guard word, so writers share no
// atomic. While tracing is off, a span costs one relaxed load. Names are
// string literals (their pointers are stored); a span's number tells
// instances apart, and label() gives a (name, number) pair readable text.
namespace oscillate::foundation::trace {

inline constexpr std::size_t event_bytes = 32;
// Events per chunk: the unit a thread takes from the pool, and drops when the
// pool is spent (its oldest chunk is reused, so the newest events stay).
inline constexpr std::size_t chunk_events = 2048;
inline constexpr std::size_t chunk_bytes = chunk_events * event_bytes;
inline constexpr std::size_t bytes_per_megabyte = std::size_t{1} << 20;
inline constexpr std::size_t default_buffer_megabytes = 32;
inline constexpr std::size_t max_buffer_megabytes = 4096;
// Threads beyond this many record nothing.
inline constexpr std::size_t max_threads = 64;
// Distinct labels kept per recording; later ones are ignored.
inline constexpr std::size_t max_labels = 4096;

namespace detail {
inline std::atomic<bool> enabled{false};
[[nodiscard]] std::uint64_t now_ns() noexcept;
void record_span(const char* name, std::uint64_t number, std::uint64_t start_ns,
                 std::uint64_t end_ns) noexcept;
} // namespace detail

[[nodiscard]] inline bool enabled() noexcept {
  return detail::enabled.load(std::memory_order_relaxed);
}

// The pool size for an OSCILLATE_TRACE_BUFFER_MB value: whole megabytes in
// [1, max_buffer_megabytes]; null or anything else gives the default.
[[nodiscard]] std::size_t buffer_bytes_from(const char* megabytes) noexcept;
[[nodiscard]] std::size_t buffer_bytes_from_environment() noexcept;

// Control only. Starts a recording into a pool of `buffer_bytes` (rounded
// down to whole chunks, at least one). False while one is already recording.
bool start(std::size_t buffer_bytes = buffer_bytes_from_environment());
// Events the current (or last) recording's pool holds; zero before any.
[[nodiscard]] std::size_t capacity_events() noexcept;
// Control only. Ends the recording, streams it to `path` as Chrome trace
// JSON and frees the pool. Returns the number of events written.
[[nodiscard]] Result<std::size_t> stop_and_write(const std::filesystem::path& path);
// Control only. Ends the recording without writing it and frees the pool.
void stop() noexcept;

// Any thread: the name this thread shows in the trace (a string literal).
void name_thread(const char* name) noexcept;
// Control only: readable text for the spans named `name` with `number`,
// such as a track's name for its node index. Does nothing (and allocates
// nothing) unless recording; a new recording starts with no labels.
void label(const char* name, std::uint64_t number, std::string_view text);

// Any thread: a process-unique number for a span's instances, such as one
// synth engine among several of the same kind. Never zero.
[[nodiscard]] std::uint64_t next_instance() noexcept;

// Any thread: the trace clock, and a span measured with it, for spans that
// open and close in different calls (an event loop's busy spell).
[[nodiscard]] inline std::uint64_t now_ns() noexcept { return detail::now_ns(); }
inline void span(const char* name, std::uint64_t number, std::uint64_t start_ns,
                 std::uint64_t end_ns) noexcept {
  if (enabled())
    detail::record_span(name, number, start_ns, end_ns);
}

// Any thread: a moment, such as an over-budget block or a published edit.
void instant(const char* name, std::uint64_t number = 0) noexcept;
// Any thread: a sampled value, drawn as a track of its own.
void counter(const char* name, double value) noexcept;

// A timed span from construction to destruction.
class Span {
public:
  explicit Span(const char* name, std::uint64_t number = 0) noexcept
      : name_(name), number_(number), start_(enabled() ? detail::now_ns() : 0) {}
  ~Span() {
    if (start_ != 0)
      detail::record_span(name_, number_, start_, detail::now_ns());
  }
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

private:
  const char* name_;
  std::uint64_t number_;
  std::uint64_t start_;
};

} // namespace oscillate::foundation::trace

#define OSCILLATE_TRACE_CONCAT_INNER(a, b) a##b
#define OSCILLATE_TRACE_CONCAT(a, b) OSCILLATE_TRACE_CONCAT_INNER(a, b)
// A span for the rest of the enclosing scope.
#define OSCILLATE_TRACE_SPAN(...)                                                                  \
  const ::oscillate::foundation::trace::Span OSCILLATE_TRACE_CONCAT(oscillate_trace_span_,         \
                                                                    __LINE__)(__VA_ARGS__)
