#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace oscillate::timeline {
namespace {

template <typename T> foundation::Result<T> invalid(const char* message) {
  return foundation::Result<T>::failure({foundation::ErrorCode::invalid_argument, message});
}

template <typename T> foundation::Result<T> exhausted(const char* message) {
  return foundation::Result<T>::failure({foundation::ErrorCode::resource_exhausted, message});
}

long double seconds_for_quarters(long double quarters, long double length, long double first_bpm,
                                 long double last_bpm) {
  const long double delta = last_bpm - first_bpm;
  if (delta == 0.0L) {
    return 60.0L * quarters / first_bpm;
  }
  return 60.0L * length / delta * std::log1p(delta * quarters / (length * first_bpm));
}

long double quarters_for_seconds(long double seconds, long double length, long double first_bpm,
                                 long double last_bpm) {
  const long double delta = last_bpm - first_bpm;
  if (delta == 0.0L) {
    return seconds * first_bpm / 60.0L;
  }
  return length * first_bpm / delta * std::expm1(seconds * delta / (60.0L * length));
}

bool rounded_nonnegative_into(long double value, std::int64_t& output) noexcept {
  constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
  if (!std::isfinite(value) || value > maximum || value < 0.0L)
    return false;
  const long double lower = std::floor(value);
  auto rounded = static_cast<std::int64_t>(lower);
  const long double fraction = value - lower;
  // Logarithms and division can put an exact half sample a few ulps either
  // side of 0.5. Bound the correction to avoid changing a meaningful offset.
  const long double tie_tolerance =
      std::min(1.0e-9L, 8.0L * std::numeric_limits<long double>::epsilon() * std::max(value, 1.0L));
  const bool tie = std::fabs(fraction - 0.5L) <= tie_tolerance;
  if ((fraction > 0.5L && !tie) || (tie && (rounded & 1) != 0)) {
    if (rounded == std::numeric_limits<std::int64_t>::max())
      return false;
    ++rounded;
  }
  output = rounded;
  return true;
}

foundation::Result<std::int64_t> rounded_nonnegative(long double value) {
  std::int64_t rounded = 0;
  if (rounded_nonnegative_into(value, rounded))
    return foundation::Result<std::int64_t>::success(rounded);
  return value < 0.0L ? invalid<std::int64_t>("negative timing coordinate")
                      : exhausted<std::int64_t>("timing coordinate exceeds signed 64-bit range");
}

std::int64_t beat_ticks(const MeterEvent& event) {
  return ticks_per_quarter * 4 / static_cast<std::int64_t>(event.denominator);
}

std::int64_t bar_ticks(const MeterEvent& event) {
  return beat_ticks(event) * static_cast<std::int64_t>(event.numerator);
}

} // namespace

bool supported_sample_rate(std::uint32_t rate) noexcept {
  return rate == 44'100 || rate == 48'000 || rate == 88'200 || rate == 96'000;
}

foundation::Result<SamplePosition>
scale_sample_position(SamplePosition position, std::uint32_t old_rate, std::uint32_t new_rate) {
  if (position.value < 0 || !supported_sample_rate(old_rate) || !supported_sample_rate(new_rate)) {
    return invalid<SamplePosition>("invalid position or project sample rate");
  }
  const auto quotient = position.value / static_cast<std::int64_t>(old_rate);
  const auto remainder = position.value % static_cast<std::int64_t>(old_rate);
  const auto new_rate_signed = static_cast<std::int64_t>(new_rate);
  const auto old_rate_signed = static_cast<std::int64_t>(old_rate);
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  if (quotient > maximum / new_rate_signed) {
    return exhausted<SamplePosition>("scaled sample coordinate exceeds signed 64-bit range");
  }
  const auto whole = quotient * new_rate_signed;
  const auto product = remainder * new_rate_signed; // Both rates are at most 96,000.
  auto fractional = product / old_rate_signed;
  const auto residue = product % old_rate_signed;
  if (fractional > maximum - whole) {
    return exhausted<SamplePosition>("scaled sample coordinate exceeds signed 64-bit range");
  }
  const auto base = whole + fractional;
  if (residue * 2 > old_rate_signed || (residue * 2 == old_rate_signed && (base & 1) != 0)) {
    if (base == maximum) {
      return exhausted<SamplePosition>("rounded sample coordinate exceeds signed 64-bit range");
    }
    ++fractional;
  }
  return foundation::Result<SamplePosition>::success({whole + fractional});
}

foundation::Result<TempoMap> TempoMap::create(std::vector<TempoEvent> events,
                                              std::uint64_t revision) {
  if (events.empty() || events.front().tick.value != 0) {
    return invalid<TempoMap>("tempo map needs an event at tick zero");
  }
  for (std::size_t index = 0; index < events.size(); ++index) {
    const auto& event = events[index];
    if (event.tick.value < 0 || !std::isfinite(event.bpm) || event.bpm <= 0.0 ||
        (index > 0 && event.tick.value <= events[index - 1].tick.value) ||
        (index + 1 == events.size() && event.shape_to_next != TempoShape::step) ||
        static_cast<unsigned>(event.shape_to_next) >
            static_cast<unsigned>(TempoShape::ease_in_out)) {
      return invalid<TempoMap>("invalid tempo event sequence, BPM or shape");
    }
  }

  std::vector<Segment> segments;
  segments.reserve(events.size());
  long double elapsed = 0.0L;
  for (std::size_t index = 0; index < events.size(); ++index) {
    Segment segment{.first_second = elapsed, .last_second = elapsed, .pieces = {}};
    if (index + 1 < events.size()) {
      const auto& start = events[index];
      const auto& end = events[index + 1];
      const auto quarters = static_cast<long double>(end.tick.value - start.tick.value) /
                            static_cast<long double>(ticks_per_quarter);
      long double positions[4] = {0.0L, 1.0L, 0.0L, 0.0L};
      long double changes[4] = {0.0L, 1.0L, 0.0L, 0.0L};
      std::size_t knot_count = 2;
      switch (start.shape_to_next) {
      case TempoShape::step:
        changes[1] = 0.0L;
        break;
      case TempoShape::linear_bpm:
        break;
      case TempoShape::ease_in:
      case TempoShape::ease_out:
        positions[1] = 0.5L;
        positions[2] = 1.0L;
        changes[1] = start.shape_to_next == TempoShape::ease_in ? 0.25L : 0.75L;
        changes[2] = 1.0L;
        knot_count = 3;
        break;
      case TempoShape::ease_in_out:
        positions[1] = 0.25L;
        positions[2] = 0.75L;
        positions[3] = 1.0L;
        changes[1] = 0.125L;
        changes[2] = 0.875L;
        changes[3] = 1.0L;
        knot_count = 4;
        break;
      }
      for (std::size_t knot = 1; knot < knot_count; ++knot) {
        const long double first_quarter = positions[knot - 1] * quarters;
        const long double count = (positions[knot] - positions[knot - 1]) * quarters;
        const long double bpm_delta = static_cast<long double>(end.bpm) - start.bpm;
        const long double first_bpm = start.bpm + bpm_delta * changes[knot - 1];
        const long double last_bpm = start.bpm + bpm_delta * changes[knot];
        const long double last_second =
            elapsed + seconds_for_quarters(count, count, first_bpm, last_bpm);
        if (!std::isfinite(last_second) || last_second <= elapsed) {
          return exhausted<TempoMap>("tempo duration exceeds numeric precision");
        }
        segment.pieces.push_back({first_quarter, count, first_bpm, last_bpm, elapsed, last_second});
        elapsed = last_second;
      }
      segment.last_second = elapsed;
    } else {
      segment.last_second = std::numeric_limits<long double>::infinity();
    }
    segments.push_back(std::move(segment));
  }
  return foundation::Result<TempoMap>::success(
      TempoMap(std::move(events), revision, std::move(segments)));
}

foundation::Result<SamplePosition> TempoMap::tick_to_sample(TickPosition tick,
                                                            std::uint32_t rate) const {
  if (tick.value < 0 || !supported_sample_rate(rate)) {
    return invalid<SamplePosition>("invalid tick or project sample rate");
  }
  const auto next = std::ranges::upper_bound(
      events_, tick.value, {}, [](const TempoEvent& event) { return event.tick.value; });
  const auto index = static_cast<std::size_t>(std::distance(events_.begin(), next) - 1);
  const auto& segment = segments_[index];
  long double seconds = segment.first_second;
  const auto quarters = static_cast<long double>(tick.value - events_[index].tick.value) /
                        static_cast<long double>(ticks_per_quarter);
  if (index + 1 == events_.size()) {
    seconds += 60.0L * quarters / events_[index].bpm;
  } else {
    for (const auto& piece : segment.pieces) {
      if (quarters < piece.first_quarter + piece.quarter_count) {
        seconds = piece.first_second + seconds_for_quarters(quarters - piece.first_quarter,
                                                            piece.quarter_count, piece.first_bpm,
                                                            piece.last_bpm);
        break;
      }
    }
  }
  auto rounded = rounded_nonnegative(seconds * rate);
  if (!rounded) {
    return foundation::Result<SamplePosition>::failure(rounded.error());
  }
  return foundation::Result<SamplePosition>::success({rounded.value()});
}

foundation::Result<TickPosition> TempoMap::sample_to_tick(SamplePosition sample,
                                                          std::uint32_t rate) const {
  if (sample.value < 0 || !supported_sample_rate(rate)) {
    return invalid<TickPosition>("invalid sample or project sample rate");
  }
  TickPosition tick{};
  if (!sample_to_tick_rt(sample, rate, tick))
    return exhausted<TickPosition>("timing coordinate exceeds signed 64-bit range");
  return foundation::Result<TickPosition>::success(tick);
}

bool TempoMap::sample_to_tick_rt(SamplePosition sample, std::uint32_t rate,
                                 TickPosition& output) const noexcept {
  if (sample.value < 0 || !supported_sample_rate(rate))
    return false;
  const long double seconds = static_cast<long double>(sample.value) / rate;
  const auto next = std::ranges::upper_bound(
      segments_, seconds, {}, [](const Segment& segment) { return segment.first_second; });
  const auto index = static_cast<std::size_t>(std::distance(segments_.begin(), next) - 1);
  const auto& segment = segments_[index];
  long double quarters = 0.0L;
  if (index + 1 == events_.size()) {
    quarters = (seconds - segment.first_second) * events_[index].bpm / 60.0L;
  } else {
    for (const auto& piece : segment.pieces) {
      if (seconds < piece.last_second) {
        quarters = piece.first_quarter + quarters_for_seconds(seconds - piece.first_second,
                                                              piece.quarter_count, piece.first_bpm,
                                                              piece.last_bpm);
        break;
      }
    }
  }
  std::int64_t rounded = 0;
  if (!rounded_nonnegative_into(static_cast<long double>(events_[index].tick.value) +
                                    quarters * ticks_per_quarter,
                                rounded))
    return false;
  output = {rounded};
  return true;
}

bool TempoMap::bpm_at_sample_rt(SamplePosition sample, std::uint32_t rate,
                                double& output) const noexcept {
  if (sample.value < 0 || !supported_sample_rate(rate))
    return false;
  const long double seconds = static_cast<long double>(sample.value) / rate;
  const auto next = std::ranges::upper_bound(
      segments_, seconds, {}, [](const Segment& segment) { return segment.first_second; });
  const auto index = static_cast<std::size_t>(std::distance(segments_.begin(), next) - 1);
  if (index + 1 == events_.size()) {
    output = events_[index].bpm;
    return true;
  }
  for (const auto& piece : segments_[index].pieces) {
    if (seconds < piece.last_second) {
      const auto quarters = quarters_for_seconds(seconds - piece.first_second, piece.quarter_count,
                                                 piece.first_bpm, piece.last_bpm);
      output = static_cast<double>(piece.first_bpm + (piece.last_bpm - piece.first_bpm) *
                                                         (quarters / piece.quarter_count));
      return std::isfinite(output) && output > 0.0;
    }
  }
  return false;
}

foundation::Result<MeterMap> MeterMap::create(std::vector<MeterEvent> events,
                                              std::uint64_t revision) {
  if (events.empty() || events.front().tick.value != 0) {
    return invalid<MeterMap>("meter map needs an event at tick zero");
  }
  std::vector<std::int64_t> first_bars;
  first_bars.reserve(events.size());
  first_bars.push_back(1);
  for (std::size_t index = 0; index < events.size(); ++index) {
    const auto& event = events[index];
    if (event.tick.value < 0 || event.numerator < 1 || event.numerator > 32 ||
        event.denominator < 1 || event.denominator > 64 ||
        (event.denominator & (event.denominator - 1)) != 0 ||
        (index > 0 && event.tick.value <= events[index - 1].tick.value)) {
      return invalid<MeterMap>("invalid meter event sequence or signature");
    }
    if (index > 0) {
      const auto preceding_bar = bar_ticks(events[index - 1]);
      const auto distance = event.tick.value - events[index - 1].tick.value;
      if (distance % preceding_bar != 0) {
        return invalid<MeterMap>("meter change is not at a preceding bar boundary");
      }
      first_bars.push_back(first_bars.back() + distance / preceding_bar);
    }
  }
  return foundation::Result<MeterMap>::success(
      MeterMap(std::move(events), revision, std::move(first_bars)));
}

foundation::Result<BarBeat> MeterMap::locate(TickPosition tick) const {
  if (tick.value < 0) {
    return invalid<BarBeat>("negative tick coordinate");
  }
  const auto next = std::ranges::upper_bound(
      events_, tick.value, {}, [](const MeterEvent& event) { return event.tick.value; });
  const auto index = static_cast<std::size_t>(std::distance(events_.begin(), next) - 1);
  const auto distance = tick.value - events_[index].tick.value;
  const auto bar_length = bar_ticks(events_[index]);
  const auto beat_length = beat_ticks(events_[index]);
  const auto within_bar = distance % bar_length;
  return foundation::Result<BarBeat>::success(
      {first_bars_[index] + distance / bar_length,
       static_cast<std::uint8_t>(within_bar / beat_length + 1), within_bar % beat_length});
}

foundation::Result<TickPosition> MeterMap::tick_at(BarBeat position) const {
  if (position.bar < 1) {
    return invalid<TickPosition>("bar number must be positive");
  }
  const auto next = std::ranges::upper_bound(first_bars_, position.bar);
  const auto index = static_cast<std::size_t>(std::distance(first_bars_.begin(), next) - 1);
  const auto& event = events_[index];
  const auto beat_length = beat_ticks(event);
  if (position.beat < 1 || position.beat > event.numerator || position.tick_within_beat < 0 ||
      position.tick_within_beat >= beat_length) {
    return invalid<TickPosition>("beat or tick is outside the meter");
  }
  const auto bar_offset = position.bar - first_bars_[index];
  const auto bar_length = bar_ticks(event);
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto local =
      static_cast<std::int64_t>(position.beat - 1) * beat_length + position.tick_within_beat;
  if (local > maximum - event.tick.value ||
      bar_offset > (maximum - event.tick.value - local) / bar_length) {
    return exhausted<TickPosition>("bar position exceeds signed 64-bit range");
  }
  const auto tick = event.tick.value + bar_offset * bar_length + local;
  return foundation::Result<TickPosition>::success({tick});
}

} // namespace oscillate::timeline
