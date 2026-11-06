#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

namespace oscillate::foundation::trace {
namespace {

// A span ends after it starts; an instant has no end; a counter carries its
// value's bits in `end`.
constexpr std::uint64_t counter_number = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t no_chunk = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t nanoseconds_per_microsecond = 1000;
constexpr std::size_t cache_line = 64;

struct Event {
  const char* name;
  std::uint64_t number;
  std::uint64_t start;
  std::uint64_t end;
};
static_assert(sizeof(Event) == event_bytes);

// One thread's events: a chain of pool chunks, oldest first. Written only by
// its thread while recording, read only after quiesce().
struct ThreadRing {
  long tid = 0;
  const char* name = nullptr;
  std::uint32_t first = no_chunk;
  std::uint32_t last = no_chunk;
  std::size_t used = 0; // Events in `last`.
  std::uint64_t dropped = 0;
};

struct Recording {
  explicit Recording(std::size_t chunks)
      : chunk_count(chunks), events(chunks * chunk_events), next(chunks, no_chunk) {}
  std::uint64_t generation = 0;
  std::uint64_t origin_ns = 0;
  std::size_t chunk_count;
  // Value-initialised here, so the callback never faults in a fresh page.
  std::vector<Event> events;
  std::vector<std::uint32_t> next; // Each chunk's successor in its thread's chain.
  std::atomic<std::size_t> chunks_claimed{0};
  std::array<ThreadRing, max_threads> rings{};
  std::atomic<std::size_t> rings_claimed{0};
};

// Writers announce themselves on their own cache line; quiesce() waits for
// every guard to read zero after clearing the flag. The last guard is shared
// by threads that hold no ring in this recording yet (rare: once per thread).
struct alignas(cache_line) Guard {
  std::atomic<std::uint32_t> inside{0};
};
std::array<Guard, max_threads + 1> guards;

struct ThreadState {
  std::uint64_t generation = 0;
  ThreadRing* ring = nullptr;
  std::size_t guard = max_threads;
  const char* name = nullptr;
};

std::mutex control_mutex;         // start, stop and labels.
std::unique_ptr<Recording> owned; // Control only.
std::atomic<Recording*> current{nullptr};
std::atomic<std::size_t> capacity{0};
std::uint64_t next_generation = 1; // Control only.
// Keyed by text: a literal's address differs between shared libraries.
std::map<std::pair<std::string, std::uint64_t>, std::string, std::less<>> labels; // Control only.
thread_local ThreadState local;

// The calling thread's ring in `recording`, claimed on first use.
ThreadRing* ring_for_thread(Recording& recording) noexcept {
  if (local.generation != recording.generation) {
    local.generation = recording.generation;
    const auto index = recording.rings_claimed.fetch_add(1, std::memory_order_relaxed);
    local.ring = index < max_threads ? &recording.rings[index] : nullptr;
    local.guard = index < max_threads ? index : max_threads;
    if (local.ring != nullptr) {
      local.ring->tid = static_cast<long>(::syscall(SYS_gettid));
      local.ring->name = local.name;
    }
  }
  return local.ring;
}

void append(Recording& recording, ThreadRing& ring, const Event& event) noexcept {
  if (ring.last == no_chunk || ring.used == chunk_events) {
    const auto fresh = recording.chunks_claimed.fetch_add(1, std::memory_order_relaxed);
    if (fresh < recording.chunk_count) {
      const auto chunk = static_cast<std::uint32_t>(fresh);
      if (ring.last == no_chunk)
        ring.first = chunk;
      else
        recording.next[ring.last] = chunk;
      ring.last = chunk;
    } else if (ring.last == no_chunk) {
      ++ring.dropped;
      return;
    } else {
      // The pool is spent: reuse this thread's oldest chunk.
      const auto oldest = ring.first;
      ring.dropped += chunk_events;
      if (oldest != ring.last) {
        ring.first = recording.next[oldest];
        recording.next[oldest] = no_chunk;
        recording.next[ring.last] = oldest;
        ring.last = oldest;
      }
    }
    ring.used = 0;
  }
  recording.events[(static_cast<std::size_t>(ring.last) * chunk_events) + ring.used++] = event;
}

// Runs `body` with the recording while tracing is on. The recording is freed
// only after quiesce(), which waits until no thread is inside.
template <typename Body> void guarded(Body&& body) noexcept {
  auto& guard = guards[local.guard].inside;
  guard.fetch_add(1, std::memory_order_seq_cst);
  if (detail::enabled.load(std::memory_order_seq_cst)) {
    if (Recording* const recording = current.load(std::memory_order_acquire))
      body(*recording);
  }
  guard.fetch_sub(1, std::memory_order_release);
}

void write(const Event& event) noexcept {
  guarded([&](Recording& recording) {
    if (ThreadRing* const ring = ring_for_thread(recording))
      append(recording, *ring, event);
  });
}

void quiesce() noexcept {
  detail::enabled.store(false, std::memory_order_seq_cst);
  for (auto& guard : guards)
    while (guard.inside.load(std::memory_order_seq_cst) != 0)
      std::this_thread::yield();
}

void release_pool() noexcept {
  current.store(nullptr, std::memory_order_release);
  owned.reset();
}

void append_escaped(std::string& out, std::string_view text) {
  constexpr unsigned first_printable = 0x20;
  for (const char ch : text) {
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      if (static_cast<unsigned char>(ch) < first_printable) {
        std::array<char, 8> code{};
        std::snprintf(code.data(), code.size(), "\\u%04x", static_cast<unsigned>(ch));
        out += code.data();
      } else {
        out += ch;
      }
    }
  }
}

void append_microseconds(std::string& out, std::uint64_t ns) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "%llu.%03llu",
                static_cast<unsigned long long>(ns / nanoseconds_per_microsecond),
                static_cast<unsigned long long>(ns % nanoseconds_per_microsecond));
  out += text.data();
}

// One event's JSON object, into `out` (reused between events).
void format_event(std::string& out, const Event& event, long pid, long tid,
                  std::uint64_t origin_ns) {
  const auto found = labels.find(std::pair<std::string, std::uint64_t>{event.name, event.number});
  const std::string_view name =
      found != labels.end() ? std::string_view(found->second) : std::string_view(event.name);
  out += "{\"name\":\"";
  append_escaped(out, name);
  out += "\",\"pid\":" + std::to_string(pid) + ",\"tid\":" + std::to_string(tid) + ",\"ts\":";
  append_microseconds(out, event.start - origin_ns);
  if (event.number == counter_number) {
    std::array<char, 48> value{};
    std::snprintf(value.data(), value.size(), "%.6g", std::bit_cast<double>(event.end));
    out += ",\"ph\":\"C\",\"args\":{\"value\":";
    out += value.data();
    out += "}}";
  } else if (event.end == 0) {
    out += ",\"ph\":\"i\",\"s\":\"t\",\"args\":{\"n\":" + std::to_string(event.number) + "}}";
  } else {
    out += ",\"ph\":\"X\",\"dur\":";
    append_microseconds(out, event.end - event.start);
    out += ",\"args\":{\"kind\":\"";
    append_escaped(out, event.name);
    out += "\",\"n\":" + std::to_string(event.number) + "}}";
  }
}

} // namespace

namespace detail {

std::uint64_t now_ns() noexcept {
  timespec time{};
  ::clock_gettime(CLOCK_MONOTONIC, &time);
  constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;
  // Never zero: a zero start means "not recording" to Span.
  return (static_cast<std::uint64_t>(time.tv_sec) * nanoseconds_per_second) +
         static_cast<std::uint64_t>(time.tv_nsec) + 1;
}

void record_span(const char* name, std::uint64_t number, std::uint64_t start_ns,
                 std::uint64_t end_ns) noexcept {
  write({name, number, start_ns, std::max(end_ns, start_ns + 1)});
}

} // namespace detail

std::size_t buffer_bytes_from(const char* megabytes) noexcept {
  constexpr std::size_t fallback = default_buffer_megabytes * bytes_per_megabyte;
  if (megabytes == nullptr)
    return fallback;
  const std::string_view text(megabytes);
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 ||
      value > max_buffer_megabytes)
    return fallback;
  return value * bytes_per_megabyte;
}

std::size_t buffer_bytes_from_environment() noexcept {
  return buffer_bytes_from(std::getenv("OSCILLATE_TRACE_BUFFER_MB"));
}

bool start(std::size_t buffer_bytes) {
  const std::lock_guard lock(control_mutex);
  if (detail::enabled.load(std::memory_order_acquire))
    return false;
  // A stopped recording has no writers (quiesce), so the old pool can go.
  release_pool();
  labels.clear();
  auto recording =
      std::make_unique<Recording>(std::max<std::size_t>(buffer_bytes / chunk_bytes, 1));
  recording->generation = next_generation++;
  recording->origin_ns = detail::now_ns();
  capacity.store(recording->chunk_count * chunk_events, std::memory_order_relaxed);
  owned = std::move(recording);
  current.store(owned.get(), std::memory_order_release);
  detail::enabled.store(true, std::memory_order_seq_cst);
  return true;
}

std::size_t capacity_events() noexcept { return capacity.load(std::memory_order_relaxed); }

void stop() noexcept {
  const std::lock_guard lock(control_mutex);
  quiesce();
  release_pool();
}

Result<std::size_t> stop_and_write(const std::filesystem::path& path) {
  const std::lock_guard lock(control_mutex);
  quiesce();
  if (!owned)
    return Result<std::size_t>::failure({ErrorCode::invalid_argument, "No trace was recorded"});
  const Recording& recording = *owned;
  const auto pid = static_cast<long>(::getpid());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  std::string out;
  out += "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
  std::size_t written = 0;
  std::uint64_t dropped = 0;
  bool first = true;
  // Streams one object at a time; `out` never holds more than one event.
  const auto emit = [&] {
    if (!first)
      file << ",\n";
    first = false;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    out.clear();
  };
  file.write(out.data(), static_cast<std::streamsize>(out.size()));
  out.clear();
  const auto claimed = std::min(recording.rings_claimed.load(), max_threads);
  for (std::size_t r = 0; r < claimed && file; ++r) {
    const ThreadRing& ring = recording.rings[r];
    dropped += ring.dropped;
    if (ring.name != nullptr) {
      out += "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":" + std::to_string(pid) +
             ",\"tid\":" + std::to_string(ring.tid) + ",\"args\":{\"name\":\"";
      append_escaped(out, ring.name);
      out += "\"}}";
      emit();
    }
    for (auto chunk = ring.first; chunk != no_chunk; chunk = recording.next[chunk]) {
      const auto count = chunk == ring.last ? ring.used : chunk_events;
      const auto base = static_cast<std::size_t>(chunk) * chunk_events;
      for (std::size_t i = 0; i < count; ++i) {
        const Event& event = recording.events[base + i];
        if (event.start < recording.origin_ns)
          continue;
        format_event(out, event, pid, ring.tid, recording.origin_ns);
        emit();
        ++written;
      }
    }
  }
  file << "\n],\"otherData\":{\"dropped_events\":" << dropped << "}}\n";
  file.close();
  release_pool();
  if (!file)
    return Result<std::size_t>::failure(
        {ErrorCode::io_failure, "Cannot write the trace to " + path.string()});
  return Result<std::size_t>::success(written);
}

void name_thread(const char* name) noexcept {
  local.name = name;
  guarded([&](Recording& recording) {
    if (local.ring != nullptr && local.generation == recording.generation)
      local.ring->name = name;
  });
}

void label(const char* name, std::uint64_t number, std::string_view text) {
  if (!enabled())
    return;
  const std::lock_guard lock(control_mutex);
  auto key = std::pair<std::string, std::uint64_t>{name, number};
  if (const auto found = labels.find(key); found != labels.end())
    found->second.assign(text);
  else if (labels.size() < max_labels)
    labels.emplace(std::move(key), std::string(text));
}

std::uint64_t next_instance() noexcept {
  static std::atomic<std::uint64_t> instances{0};
  return instances.fetch_add(1, std::memory_order_relaxed) + 1;
}

void instant(const char* name, std::uint64_t number) noexcept {
  if (enabled())
    write({name, number, detail::now_ns(), 0});
}

void counter(const char* name, double value) noexcept {
  if (enabled())
    write({name, counter_number, detail::now_ns(), std::bit_cast<std::uint64_t>(value)});
}

} // namespace oscillate::foundation::trace
