#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/midi.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oscillate::engine {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}
[[nodiscard]] Error exhausted(std::string message) {
  return {ErrorCode::resource_exhausted, std::move(message)};
}
[[nodiscard]] Result<std::int64_t> checked_add(std::int64_t a, std::int64_t b) {
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
    return Result<std::int64_t>::failure(exhausted("MIDI event position overflows"));
  }
  return Result<std::int64_t>::success(a + b);
}

struct ClipInterval {
  std::int64_t start;
  std::int64_t end;
};
[[nodiscard]] Result<ClipInterval> clip_interval(const project::Snapshot& snapshot,
                                                 const project::MidiClip& clip) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    if (musical->start.value < 0 || musical->end <= musical->start) {
      return Result<ClipInterval>::failure(invalid("invalid musical MIDI placement"));
    }
    auto start = snapshot.tempo.tick_to_sample(musical->start, snapshot.sample_rate);
    auto end = snapshot.tempo.tick_to_sample(musical->end, snapshot.sample_rate);
    if (!start || !end) {
      return Result<ClipInterval>::failure(!start ? start.error() : end.error());
    }
    if (end.value() <= start.value()) {
      return Result<ClipInterval>::failure(invalid("MIDI clip renders zero samples"));
    }
    return Result<ClipInterval>::success({start.value().value, end.value().value});
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
  if (elapsed.start.value < 0 || elapsed.end <= elapsed.start) {
    return Result<ClipInterval>::failure(invalid("invalid elapsed MIDI placement"));
  }
  return Result<ClipInterval>::success({elapsed.start.value, elapsed.end.value});
}

[[nodiscard]] Result<std::int64_t> sample_at(const project::Snapshot& snapshot,
                                             const project::MidiClip& clip,
                                             std::int64_t relative_tick) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    auto tick = checked_add(musical->start.value, relative_tick);
    if (!tick) {
      return tick;
    }
    auto sample = snapshot.tempo.tick_to_sample({tick.value()}, snapshot.sample_rate);
    if (!sample) {
      return Result<std::int64_t>::failure(sample.error());
    }
    return Result<std::int64_t>::success(sample.value().value);
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
  auto anchor_tick = snapshot.tempo.sample_to_tick(elapsed.start, snapshot.sample_rate);
  if (!anchor_tick) {
    return Result<std::int64_t>::failure(anchor_tick.error());
  }
  auto anchor_sample = snapshot.tempo.tick_to_sample(anchor_tick.value(), snapshot.sample_rate);
  if (!anchor_sample) {
    return Result<std::int64_t>::failure(anchor_sample.error());
  }
  auto absolute_tick = checked_add(anchor_tick.value().value, relative_tick);
  if (!absolute_tick) {
    return absolute_tick;
  }
  auto mapped = snapshot.tempo.tick_to_sample({absolute_tick.value()}, snapshot.sample_rate);
  if (!mapped) {
    return Result<std::int64_t>::failure(mapped.error());
  }
  if (mapped.value().value < anchor_sample.value().value) {
    return Result<std::int64_t>::failure(invalid("MIDI tempo mapping is not monotonic"));
  }
  return checked_add(elapsed.start.value, mapped.value().value - anchor_sample.value().value);
}

[[nodiscard]] Result<std::int64_t> iteration_count(const project::Snapshot& snapshot,
                                                   const project::MidiClip& clip) {
  if (!clip.loop) {
    return Result<std::int64_t>::success(1);
  }
  std::int64_t first = 0;
  std::int64_t last = 0;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    first = musical->start.value;
    last = musical->end.value;
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
    auto start = snapshot.tempo.sample_to_tick(elapsed.start, snapshot.sample_rate);
    auto end = snapshot.tempo.sample_to_tick(elapsed.end, snapshot.sample_rate);
    if (!start || !end) {
      return Result<std::int64_t>::failure(!start ? start.error() : end.error());
    }
    first = start.value().value;
    last = end.value().value;
  }
  if (last <= first) {
    return Result<std::int64_t>::failure(invalid("MIDI loop has no tick duration"));
  }
  const auto quotient = (last - first) / clip.pattern_length.value;
  if (quotient > 99'998) {
    return Result<std::int64_t>::failure(exhausted("MIDI loop iteration limit exceeded"));
  }
  return Result<std::int64_t>::success(quotient + 2);
}

// A clip's pedal points per rendered repeat, and a release at its end for
// every pedal still down there, as notes are cropped at the clip's end.
[[nodiscard]] Result<void> append_pedals(const project::Snapshot& snapshot,
                                         const project::MidiClip& clip, std::int64_t iterations,
                                         const ClipInterval& interval,
                                         std::vector<ScheduledPedalEvent>& pedals) {
  for (const auto& lane : clip.pedals) {
    // The model validates lanes; a snapshot built elsewhere must not index
    // past the pedal tables or hide points behind unsorted ones.
    if (static_cast<std::size_t>(lane.pedal) >= project::midi_pedal_count ||
        lane.points.size() > project::max_midi_pedal_points_per_lane) {
      return Result<void>::failure(invalid("invalid MIDI pedal lane"));
    }
    std::int64_t previous = -1;
    for (const auto& point : lane.points) {
      if (point.position.value <= previous || !std::isfinite(point.value) || point.value < 0.0 ||
          point.value > 1.0) {
        return Result<void>::failure(invalid("invalid MIDI pedal point"));
      }
      previous = point.position.value;
    }
    // A lane with no point inside the pattern is silent in every repeat.
    if (lane.points.empty() || lane.points.front().position >= clip.pattern_length) {
      continue;
    }
    double last = 0.0;
    for (std::int64_t iteration = 0; iteration < iterations; ++iteration) {
      if (iteration > std::numeric_limits<std::int64_t>::max() / clip.pattern_length.value) {
        return Result<void>::failure(exhausted("MIDI loop tick overflow"));
      }
      const auto base = iteration * clip.pattern_length.value;
      bool past_end = false;
      for (const auto& point : lane.points) {
        if (point.position >= clip.pattern_length) {
          break; // Points are sorted; the rest are kept past the pattern, silent.
        }
        auto relative = checked_add(base, point.position.value);
        if (!relative) {
          return Result<void>::failure(relative.error());
        }
        auto sample = sample_at(snapshot, clip, relative.value());
        if (!sample) {
          return Result<void>::failure(sample.error());
        }
        if (sample.value() >= interval.end) {
          past_end = true;
          break;
        }
        if (pedals.size() >= project::max_midi_events_per_project) {
          return Result<void>::failure(exhausted("MIDI pedal event count exceeded"));
        }
        pedals.push_back({{sample.value()}, clip.id, lane.pedal, point.value, false});
        last = point.value;
      }
      if (past_end) {
        break;
      }
    }
    if (last != 0.0) {
      if (pedals.size() >= project::max_midi_events_per_project) {
        return Result<void>::failure(exhausted("MIDI pedal event count exceeded"));
      }
      pedals.push_back({{interval.end}, clip.id, lane.pedal, 0.0, true});
    }
  }
  return Result<void>::success();
}

// A note's length for its note-on, held at the largest a frame count holds.
[[nodiscard]] std::uint32_t note_frames(std::int64_t frames) noexcept {
  return static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(frames, 0, std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] Result<void> append_clip(const project::Snapshot& snapshot,
                                       const project::MidiClip& clip,
                                       std::vector<ScheduledMidiEvent>& events,
                                       std::vector<ScheduledPedalEvent>& pedals) {
  if (clip.pattern_length.value <= 0 || clip.notes.size() > project::max_midi_notes_per_clip) {
    return Result<void>::failure(invalid("invalid MIDI pattern or note count"));
  }
  auto interval = clip_interval(snapshot, clip);
  if (!interval) {
    return Result<void>::failure(interval.error());
  }
  if (clip.notes.empty() && clip.pedals.empty()) {
    return Result<void>::success();
  }
  auto iterations = iteration_count(snapshot, clip);
  if (!iterations) {
    return Result<void>::failure(iterations.error());
  }
  auto pedalled = append_pedals(snapshot, clip, iterations.value(), interval.value(), pedals);
  if (!pedalled) {
    return pedalled;
  }
  for (std::int64_t iteration = 0; iteration < iterations.value(); ++iteration) {
    if (iteration > std::numeric_limits<std::int64_t>::max() / clip.pattern_length.value) {
      return Result<void>::failure(exhausted("MIDI loop tick overflows"));
    }
    const auto base = iteration * clip.pattern_length.value;
    auto first_sample = sample_at(snapshot, clip, base);
    if (!first_sample) {
      return Result<void>::failure(first_sample.error());
    }
    if (first_sample.value() >= interval.value().end) {
      break;
    }
    for (const auto& note : clip.notes) {
      if (note.start.value < 0 || note.end <= note.start || note.key > foundation::max_midi_key ||
          note.velocity < foundation::min_note_velocity ||
          note.velocity > foundation::max_note_velocity) {
        return Result<void>::failure(invalid("invalid MIDI note"));
      }
      if (note.start >= clip.pattern_length) {
        continue; // Kept past the pattern's end, silent.
      }
      // A note crossing the pattern's end is cut there, as by the clip end.
      const bool cut = note.end > clip.pattern_length;
      auto relative_start = checked_add(base, note.start.value);
      auto relative_end = checked_add(base, std::min(note.end, clip.pattern_length).value);
      if (!relative_start || !relative_end) {
        return Result<void>::failure(!relative_start ? relative_start.error()
                                                     : relative_end.error());
      }
      auto start = sample_at(snapshot, clip, relative_start.value());
      auto end = sample_at(snapshot, clip, relative_end.value());
      if (!start || !end) {
        return Result<void>::failure(!start ? start.error() : end.error());
      }
      // So is one whose cut leaves less than a sample.
      if (start.value() >= interval.value().end || (cut && end.value() <= start.value())) {
        continue;
      }
      const auto off = std::min(end.value(), interval.value().end);
      if (start.value() < interval.value().start || off <= start.value()) {
        return Result<void>::failure(invalid("MIDI note renders zero samples"));
      }
      if (events.size() > project::max_midi_events_per_project - 2) {
        return Result<void>::failure(exhausted("MIDI event count exceeded"));
      }
      events.push_back({{start.value()},
                        clip.id,
                        note.id,
                        MidiEventKind::note_on,
                        note.key,
                        note.velocity,
                        false,
                        note_frames(off - start.value())});
      events.push_back({{off},
                        clip.id,
                        note.id,
                        MidiEventKind::note_off,
                        note.key,
                        0,
                        cut || off != end.value()});
    }
  }
  return Result<void>::success();
}

[[nodiscard]] bool event_less(const ScheduledMidiEvent& a, const ScheduledMidiEvent& b) {
  if (a.sample != b.sample) {
    return a.sample < b.sample;
  }
  if (a.kind != b.kind) {
    return a.kind < b.kind;
  }
  return a.note < b.note;
}
} // namespace

bool pedal_event_less(const ScheduledPedalEvent& a, const ScheduledPedalEvent& b) noexcept {
  if (a.sample != b.sample) {
    return a.sample < b.sample;
  }
  if (a.clip_end != b.clip_end) {
    return a.clip_end;
  }
  if (a.pedal != b.pedal) {
    return a.pedal < b.pedal;
  }
  return a.clip < b.clip;
}

BlockMidiResult
InstrumentEvents::copy_pedal_block(timeline::SamplePosition start, std::uint32_t frames,
                                   std::span<BlockPedalEvent> output) const noexcept {
  if (start.value < 0 || frames == 0 || frames > max_midi_block_frames ||
      start.value > std::numeric_limits<std::int64_t>::max() - frames) {
    return {BlockMidiStatus::invalid_range, 0};
  }
  const auto end = start.value + frames;
  const auto projection = [](const ScheduledPedalEvent& event) { return event.sample.value; };
  const auto first = std::ranges::lower_bound(pedals_, start.value, std::less<>{}, projection);
  const auto last = std::ranges::lower_bound(first, pedals_.end(), end, std::less<>{}, projection);
  const auto count = static_cast<std::size_t>(last - first);
  if (count > output.size()) {
    return {BlockMidiStatus::insufficient_storage, 0};
  }
  for (std::size_t i = 0; i < count; ++i) {
    const auto& event = first[static_cast<std::ptrdiff_t>(i)];
    output[i] = {static_cast<std::uint32_t>(event.sample.value - start.value), event.pedal,
                 static_cast<float>(event.value)};
  }
  return {BlockMidiStatus::ready, count};
}

BlockMidiResult InstrumentEvents::copy_pedals(timeline::SamplePosition start, std::uint32_t frames,
                                              bool chase,
                                              std::span<BlockPedalEvent> output) const noexcept {
  std::size_t chased = 0;
  if (chase && start.value >= 0) {
    const auto held = pedal_state_at(start);
    for (std::size_t pedal = 0; pedal < held.size(); ++pedal) {
      if (held[pedal] == 0.0) {
        continue;
      }
      if (chased == output.size()) {
        return {BlockMidiStatus::insufficient_storage, 0};
      }
      output[chased++] = {0, static_cast<project::MidiPedal>(pedal),
                          static_cast<float>(held[pedal])};
    }
  }
  const auto copied = copy_pedal_block(start, frames, output.subspan(chased));
  return copied.status == BlockMidiStatus::ready
             ? BlockMidiResult{BlockMidiStatus::ready, chased + copied.count}
             : copied;
}

PedalState InstrumentEvents::pedal_state_at(timeline::SamplePosition sample) const noexcept {
  PedalState state{};
  const auto projection = [](const ScheduledPedalEvent& event) { return event.sample.value; };
  const auto end = std::ranges::lower_bound(pedals_, sample.value, std::less<>{}, projection);
  // Walk back from the newest until every pedal is known.
  std::array<bool, project::midi_pedal_count> known{};
  std::size_t remaining = project::midi_pedal_count;
  for (auto it = end; it != pedals_.begin() && remaining > 0;) {
    --it;
    const auto index = static_cast<std::size_t>(it->pedal);
    if (!known[index]) {
      known[index] = true;
      state[index] = it->value;
      --remaining;
    }
  }
  return state;
}

std::vector<ScheduledPedalEvent> InstrumentEvents::pedals_until(std::int64_t end) const {
  std::vector<ScheduledPedalEvent> cut;
  for (const auto& event : pedals_) {
    if (event.sample.value >= end) {
      break;
    }
    cut.push_back(event);
  }
  const auto state = pedal_state_at({end});
  for (std::size_t index = 0; index < state.size(); ++index) {
    if (state[index] == 0.0) {
      continue;
    }
    // A pedal down at the cut was set by an earlier event: release it for
    // the same clip.
    const auto pedal = static_cast<project::MidiPedal>(index);
    for (auto it = cut.rbegin(); it != cut.rend(); ++it) {
      if (it->pedal == pedal) {
        cut.push_back({{end}, it->clip, pedal, 0.0, true});
        break;
      }
    }
  }
  return cut;
}

BlockMidiResult InstrumentEvents::copy_block(timeline::SamplePosition start, std::uint32_t frames,
                                             std::span<BlockMidiEvent> output) const noexcept {
  if (start.value < 0 || frames == 0 || frames > max_midi_block_frames ||
      start.value > std::numeric_limits<std::int64_t>::max() - frames) {
    return {BlockMidiStatus::invalid_range, 0};
  }
  const auto end = start.value + frames;
  const auto projection = [](const ScheduledMidiEvent& event) { return event.sample.value; };
  const auto first = std::ranges::lower_bound(events_, start.value, std::less<>{}, projection);
  const auto last = std::ranges::lower_bound(first, events_.end(), end, std::less<>{}, projection);
  const auto count = static_cast<std::size_t>(last - first);
  if (count > output.size()) {
    return {BlockMidiStatus::insufficient_storage, 0};
  }
  for (std::size_t i = 0; i < count; ++i) {
    const auto& event = first[static_cast<std::ptrdiff_t>(i)];
    output[i] = {static_cast<std::uint32_t>(event.sample.value - start.value),
                 event.clip.bits(),
                 event.note.bits(),
                 event.kind,
                 event.key,
                 event.velocity,
                 event.boundary_crop,
                 event.frames};
  }
  return {BlockMidiStatus::ready, count};
}

Result<CompiledMidiPlan> compile_midi_events(const project::Snapshot& snapshot) {
  if (!timeline::supported_sample_rate(snapshot.sample_rate)) {
    return Result<CompiledMidiPlan>::failure(invalid("unsupported project sample rate"));
  }
  std::vector<InstrumentEvents> instruments;
  std::size_t total_events = 0;
  for (const auto& track : snapshot.tracks) {
    if (track.kind != project::TrackKind::instrument) {
      if (!track.midi_clips.empty()) {
        return Result<CompiledMidiPlan>::failure(invalid("audio track contains MIDI clips"));
      }
      continue;
    }
    std::vector<ScheduledMidiEvent> events;
    std::vector<ScheduledPedalEvent> pedals;
    for (const auto& clip : track.midi_clips) {
      auto appended = append_clip(snapshot, clip, events, pedals);
      if (!appended) {
        return Result<CompiledMidiPlan>::failure(appended.error());
      }
    }
    if (events.size() > project::max_midi_events_per_project - total_events ||
        pedals.size() > project::max_midi_events_per_project - total_events - events.size()) {
      return Result<CompiledMidiPlan>::failure(exhausted("project MIDI event count exceeded"));
    }
    total_events += events.size() + pedals.size();
    // Stable: distinct ticks can land on one sample, and their points must
    // keep the lane's order (a down then an up stays up).
    std::ranges::stable_sort(pedals, pedal_event_less);
    for (std::size_t i = max_pedal_events_per_block; i < pedals.size(); ++i) {
      if (pedals[i].sample.value - pedals[i - max_pedal_events_per_block].sample.value <
          max_midi_block_frames) {
        return Result<CompiledMidiPlan>::failure(
            exhausted("more than 256 pedal events in an 8192-sample window"));
      }
    }
    std::ranges::sort(events, event_less);
    for (std::size_t i = max_midi_events_per_block; i < events.size(); ++i) {
      if (events[i].sample.value - events[i - max_midi_events_per_block].sample.value <
          max_midi_block_frames) {
        return Result<CompiledMidiPlan>::failure(
            exhausted("more than 1024 MIDI events in an 8192-sample window"));
      }
    }
    instruments.emplace_back(track.id, std::move(events), std::move(pedals));
  }
  return Result<CompiledMidiPlan>::success(CompiledMidiPlan(std::move(instruments)));
}

BlockMidiResult HeldNotes::copy_block(const InstrumentEvents& events,
                                      timeline::SamplePosition start, std::uint32_t frames,
                                      std::uint64_t generation,
                                      std::span<BlockMidiEvent> output) noexcept {
  if (output.size() < block_capacity)
    return {BlockMidiStatus::insufficient_storage, 0};
  if (!started_ || generation != generation_)
    count_ = 0;
  std::size_t released = 0;
  const bool jumped = started_ && generation == generation_ && start.value != next_sample_;
  if (jumped) {
    for (std::size_t i = 0; i < count_; ++i) {
      auto off = held_[i];
      off.offset = 0;
      off.kind = MidiEventKind::note_off;
      off.velocity = 0;
      off.boundary_crop = true;
      output[released++] = off;
    }
  }
  auto copied =
      events.copy_block(start, frames, output.subspan(released, max_midi_events_per_block));
  // A failed block keeps the held notes, so a retry still releases them.
  if (copied.status != BlockMidiStatus::ready)
    return copied;
  if (jumped)
    count_ = 0;
  // Track the plan's own notes before the releases are merged in.
  for (const auto& event : output.subspan(released, copied.count)) {
    if (event.kind == MidiEventKind::note_on) {
      if (count_ < capacity)
        held_[count_++] = event;
      continue;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      if (held_[i].note_id_bits == event.note_id_bits &&
          held_[i].clip_id_bits == event.clip_id_bits) {
        held_[i] = held_[--count_];
        break;
      }
    }
  }
  const auto block = output.first(released + copied.count);
  if (released != 0)
    std::ranges::sort(block, [](const BlockMidiEvent& a, const BlockMidiEvent& b) {
      return std::tie(a.offset, a.kind, a.note_id_bits) <
             std::tie(b.offset, b.kind, b.note_id_bits);
    });
  started_ = true;
  generation_ = generation;
  next_sample_ = start.value + frames;
  return {BlockMidiStatus::ready, block.size()};
}
} // namespace oscillate::engine
