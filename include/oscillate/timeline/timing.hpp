#pragma once

#include "oscillate/foundation/error.hpp"

#include <compare>
#include <cstdint>
#include <utility>
#include <vector>

namespace oscillate::timeline {

inline constexpr std::int64_t ticks_per_quarter = 960'000'000;
inline constexpr std::uint32_t timing_map_version = 1;
// The tempo range analysis searches and a detected tempo must fall within.
inline constexpr double min_detected_bpm = 30.0;
inline constexpr double max_detected_bpm = 300.0;

struct TickPosition {
  std::int64_t value;
  auto operator<=>(const TickPosition&) const = default;
};

struct SamplePosition {
  std::int64_t value;
  auto operator<=>(const SamplePosition&) const = default;
};

static_assert(sizeof(TickPosition) == sizeof(std::int64_t));
static_assert(sizeof(SamplePosition) == sizeof(std::int64_t));

enum class TempoShape : std::uint8_t { step, linear_bpm, ease_in, ease_out, ease_in_out };

struct TempoEvent {
  TickPosition tick;
  double bpm;
  TempoShape shape_to_next;
};

struct MeterEvent {
  TickPosition tick;
  std::uint8_t numerator;
  std::uint8_t denominator;
};

struct BarBeat {
  std::int64_t bar;              // One-based, continuous across meter changes.
  std::uint8_t beat;             // One-based under the current meter.
  std::int64_t tick_within_beat; // Zero-based.
  auto operator<=>(const BarBeat&) const = default;
};

[[nodiscard]] bool supported_sample_rate(std::uint32_t rate) noexcept;

// Exact rational scaling of an elapsed-time anchor. Callers retain the original
// coordinate for undo rather than reverse-scaling a rounded result.
[[nodiscard]] foundation::Result<SamplePosition>
scale_sample_position(SamplePosition position, std::uint32_t old_rate, std::uint32_t new_rate);

class TempoMap {
public:
  [[nodiscard]] static foundation::Result<TempoMap> create(std::vector<TempoEvent> events,
                                                           std::uint64_t revision);

  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] const std::vector<TempoEvent>& events() const noexcept { return events_; }
  [[nodiscard]] foundation::Result<SamplePosition> tick_to_sample(TickPosition tick,
                                                                  std::uint32_t rate) const;
  [[nodiscard]] foundation::Result<TickPosition> sample_to_tick(SamplePosition sample,
                                                                std::uint32_t rate) const;
  // Callback-safe conversion. Failure has no diagnostic allocation; control
  // callers should use sample_to_tick when they need an actionable error.
  [[nodiscard]] bool sample_to_tick_rt(SamplePosition sample, std::uint32_t rate,
                                       TickPosition& output) const noexcept;
  // Instantaneous BPM at an elapsed sample. Uses the same piecewise tempo
  // segments as sample_to_tick_rt; safe to query at a VST3 block boundary.
  [[nodiscard]] bool bpm_at_sample_rt(SamplePosition sample, std::uint32_t rate,
                                      double& output) const noexcept;

private:
  struct Piece {
    long double first_quarter;
    long double quarter_count;
    long double first_bpm;
    long double last_bpm;
    long double first_second;
    long double last_second;
  };
  struct Segment {
    long double first_second;
    long double last_second;
    std::vector<Piece> pieces;
  };

  TempoMap(std::vector<TempoEvent> events, std::uint64_t revision, std::vector<Segment> segments)
      : events_(std::move(events)), revision_(revision), segments_(std::move(segments)) {}

  std::vector<TempoEvent> events_;
  std::uint64_t revision_;
  std::vector<Segment> segments_;
};

class MeterMap {
public:
  [[nodiscard]] static foundation::Result<MeterMap> create(std::vector<MeterEvent> events,
                                                           std::uint64_t revision);

  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] const std::vector<MeterEvent>& events() const noexcept { return events_; }
  [[nodiscard]] foundation::Result<BarBeat> locate(TickPosition tick) const;
  [[nodiscard]] foundation::Result<TickPosition> tick_at(BarBeat position) const;

private:
  MeterMap(std::vector<MeterEvent> events, std::uint64_t revision,
           std::vector<std::int64_t> first_bars)
      : events_(std::move(events)), revision_(revision), first_bars_(std::move(first_bars)) {}

  std::vector<MeterEvent> events_;
  std::uint64_t revision_;
  std::vector<std::int64_t> first_bars_;
};

} // namespace oscillate::timeline
