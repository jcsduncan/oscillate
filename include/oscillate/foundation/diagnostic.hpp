#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/foundation/id.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace oscillate::foundation {

// Stable values: they index log filters and appear in diagnostic records. Append new ones.
enum class DiagnosticCategory : std::uint8_t {
  engine = 1,
  device = 2,
  plugin = 3,
  media = 4,
  project = 5,
  processing = 6,
  application = 7,
};

// What happened, for records published without text. Stable values; append new ones.
enum class DiagnosticKind : std::uint8_t {
  unspecified = 0,
  // The device callback output silence instead of a rendered block.
  // detail: frames; sample_position: the transport sample, -1 when unknown.
  block_silenced = 1,
  // The audio server reported an xrun (JACK notification, PipeWire graph recovery).
  xrun = 2,
  // The server asked for a block larger than the output can render. detail: frames.
  oversized_block = 3,
  // The server handed the callback no usable buffer.
  invalid_buffer = 4,
};

// Fixed-size capture payload. Detail and sample_position are code-specific; text and paths are
// resolved/redacted off-thread. A zero subject or correlation means unavailable.
struct DiagnosticEvent {
  IdBits subject;
  std::uint64_t correlation = 0;
  std::int64_t sample_position = 0;
  std::uint32_t detail = 0;
  ErrorCode code = ErrorCode::internal;
  DiagnosticCategory category = DiagnosticCategory::engine;
  DiagnosticKind kind = DiagnosticKind::unspecified;
};

static_assert(std::is_trivially_copyable_v<DiagnosticEvent>);
static_assert(sizeof(DiagnosticEvent) <= 48);

// Audio callback users must supply a bounded, measured, non-blocking implementation. Failed
// publication is explicit; the caller may count drops with an atomic, without formatting text.
class DiagnosticSink {
public:
  constexpr DiagnosticSink() noexcept = default;
  DiagnosticSink(const DiagnosticSink&) = delete;
  DiagnosticSink& operator=(const DiagnosticSink&) = delete;
  virtual ~DiagnosticSink() = default;
  [[nodiscard]] virtual bool try_publish(DiagnosticEvent event) noexcept = 0;
};

// A fixed-capacity, lock-free multi-producer queue of diagnostic records
// (Vyukov's bounded queue). Any thread, the audio callback included, may
// publish: no allocation, lock or system call. A full ring rejects the record
// and counts it as dropped; publishing never waits. One control thread drains.
class DiagnosticRing final : public DiagnosticSink {
public:
  static constexpr std::size_t capacity = 256;
  static_assert((capacity & (capacity - 1)) == 0, "capacity is a power of two");

  constexpr DiagnosticRing() noexcept = default;

  [[nodiscard]] bool try_publish(DiagnosticEvent event) noexcept override;
  // Control only. False when the ring is empty.
  [[nodiscard]] bool try_take(DiagnosticEvent& event) noexcept;
  // Records rejected since the last call.
  [[nodiscard]] std::uint64_t take_dropped() noexcept;

private:
  // A cell's turn is stored relative to its index, so zero-initialised
  // atomics are the empty state and the ring needs no constructor loop.
  struct alignas(64) Cell {
    std::atomic<std::uint64_t> turn{0};
    DiagnosticEvent event{};
  };
  alignas(64) std::atomic<std::uint64_t> head_{0}; // Next position to publish.
  alignas(64) std::atomic<std::uint64_t> tail_{0}; // Next position to take.
  alignas(64) std::atomic<std::uint64_t> dropped_{0};
  std::array<Cell, capacity> cells_;
};

// The process-wide ring that device callbacks, audio workers and engines
// publish to. Statically initialised: safe to use from the first callback.
[[nodiscard]] DiagnosticRing& realtime_diagnostics() noexcept;

} // namespace oscillate::foundation
