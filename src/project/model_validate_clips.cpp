#include "model_internal.hpp"

#include "model_validate.hpp"

#include "oscillate/project/clay_limits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Validation of audio clips (placement, warp, Clay and processing) and MIDI
// clips (note timing and pedals).
namespace oscillate::project::detail {
namespace {

[[nodiscard]] bool in_range(double value, double minimum, double maximum) noexcept {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] Result<void> validate_clip_warp_settings(const ClipWarpSettings& warp) {
  switch (warp.mode) {
  case WarpMode::repitch:
  case WarpMode::beats:
  case WarpMode::tones:
  case WarpMode::texture:
  case WarpMode::complex:
  case WarpMode::complex_pro:
    break;
  default:
    return Result<void>::failure(invalid("invalid clip warp mode"));
  }
  switch (warp.beats.loop) {
  case WarpBeatsLoop::off:
  case WarpBeatsLoop::forward:
  case WarpBeatsLoop::back_and_forth:
    break;
  default:
    return Result<void>::failure(invalid("invalid clip Beats loop mode"));
  }
  if (!in_range(warp.beats.envelope, 0.0, 100.0) || !in_range(warp.tones.grain_ms, 5.0, 200.0) ||
      !in_range(warp.texture.grain_ms, 10.0, 500.0) || !in_range(warp.texture.flux, 0.0, 100.0) ||
      !in_range(warp.complex_pro.formants, 0.0, 100.0) ||
      !in_range(warp.complex_pro.envelope, 8.0, 256.0)) {
    return Result<void>::failure(invalid("clip warp mode parameter out of range"));
  }
  return Result<void>::success();
}

[[nodiscard]] Result<std::int64_t> checked_add(std::int64_t a, std::int64_t b) {
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
    return Result<std::int64_t>::failure(exhausted("MIDI timing exceeds signed 64-bit range"));
  }
  return Result<std::int64_t>::success(a + b);
}

[[nodiscard]] Result<std::int64_t> note_sample(const Snapshot& state, const MidiClip& clip,
                                               std::int64_t relative_tick) {
  if (const auto* musical = std::get_if<MusicalInterval>(&clip.placement)) {
    auto tick = checked_add(musical->start.value, relative_tick);
    if (!tick) {
      return tick;
    }
    auto sample = state.tempo.tick_to_sample({tick.value()}, state.sample_rate);
    if (!sample) {
      return Result<std::int64_t>::failure(sample.error());
    }
    return Result<std::int64_t>::success(sample.value().value);
  }
  const auto& elapsed = std::get<ElapsedInterval>(clip.placement);
  auto anchor = state.tempo.sample_to_tick(elapsed.start, state.sample_rate);
  if (!anchor) {
    return Result<std::int64_t>::failure(anchor.error());
  }
  auto anchor_sample = state.tempo.tick_to_sample(anchor.value(), state.sample_rate);
  if (!anchor_sample) {
    return Result<std::int64_t>::failure(anchor_sample.error());
  }
  auto tick = checked_add(anchor.value().value, relative_tick);
  if (!tick) {
    return tick;
  }
  auto mapped = state.tempo.tick_to_sample({tick.value()}, state.sample_rate);
  if (!mapped) {
    return Result<std::int64_t>::failure(mapped.error());
  }
  // Both mapped positions are nonnegative; the difference is bounded by mapped.
  return checked_add(elapsed.start.value, mapped.value().value - anchor_sample.value().value);
}

} // namespace

[[nodiscard]] Result<RenderedInterval> render_placement(const Snapshot& state,
                                                        const Placement& placement, ClipId id) {
  if (const auto* musical = std::get_if<MusicalInterval>(&placement)) {
    if (musical->start.value < 0 || musical->end <= musical->start) {
      return Result<RenderedInterval>::failure(
          invalid("invalid musical interval for clip " + id.to_string()));
    }
    auto start = state.tempo.tick_to_sample(musical->start, state.sample_rate);
    if (!start) {
      return Result<RenderedInterval>::failure(start.error());
    }
    auto end = state.tempo.tick_to_sample(musical->end, state.sample_rate);
    if (!end) {
      return Result<RenderedInterval>::failure(end.error());
    }
    if (end.value() <= start.value()) {
      return Result<RenderedInterval>::failure(
          invalid("zero rendered length for clip " + id.to_string()));
    }
    return Result<RenderedInterval>::success({start.value().value, end.value().value, id});
  }
  const auto& elapsed = std::get<ElapsedInterval>(placement);
  if (elapsed.start.value < 0 || elapsed.end <= elapsed.start) {
    return Result<RenderedInterval>::failure(
        invalid("invalid elapsed interval for clip " + id.to_string()));
  }
  return Result<RenderedInterval>::success({elapsed.start.value, elapsed.end.value, id});
}

[[nodiscard]] Result<RenderedInterval> render_clip(const Snapshot& state, const Clip& clip) {
  return render_placement(state, clip.placement, clip.id);
}

// Structure and bounds only; whether the ids exist in the analysed take is
// checked when the take is rendered (ADR-0081).
[[nodiscard]] Result<void> validate_clay(const ClayTake& take) {
  const auto fail = [] { return Result<void>::failure(invalid("invalid Clay take")); };
  if (take.mode != ClayMode::melodic && take.mode != ClayMode::polyphonic &&
      take.mode != ClayMode::percussive)
    return fail();
  if (take.analysis_version != clay_analysis_version)
    return Result<void>::failure(invalid("Clay take was made by another analysis version"));
  if (take.ops.size() > max_clay_ops || take.edits.size() > max_clay_edits ||
      (take.key && !valid_key(*take.key)))
    return fail();
  for (const auto& op : take.ops) {
    if (op.note == 0)
      return fail();
    if (op.kind == ClayOpKind::split) {
      if (op.frame <= 0 || op.other != 0)
        return fail();
    } else if (op.kind == ClayOpKind::join) {
      if (op.other == 0 || op.other == op.note || op.frame != 0)
        return fail();
    } else {
      return fail();
    }
  }
  std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
  const auto finite = [](double value) { return std::isfinite(value); };
  for (const auto& edit : take.edits) {
    if (edit.note == 0 || edit.voice > max_clay_voices ||
        !seen.insert({edit.note, edit.voice}).second)
      return fail();
    if (!finite(edit.pitch_cents) || std::abs(edit.pitch_cents) > max_clay_edit_cents ||
        !finite(edit.formant_cents) || std::abs(edit.formant_cents) > max_clay_edit_cents ||
        !finite(edit.modulation) || edit.modulation < 0.0 ||
        edit.modulation > max_clay_modulation_scale || !finite(edit.drift) || edit.drift < 0.0 ||
        edit.drift > max_clay_drift_scale || !finite(edit.gain_db) ||
        edit.gain_db < min_clay_edit_gain_db || edit.gain_db > max_clay_edit_gain_db)
      return fail();
    if (edit.start.has_value() != edit.end.has_value() ||
        (edit.start && (*edit.start < 0 || *edit.end <= *edit.start)))
      return fail();
  }
  return Result<void>::success();
}

[[nodiscard]] Result<void> validate_clip_processing(const Snapshot& state, const Clip& clip,
                                                    const RenderedInterval& rendered) {
  const auto& processing = clip.processing;
  if (!std::isfinite(processing.pitch_semitones) ||
      processing.pitch_semitones < -max_clip_pitch_semitones ||
      processing.pitch_semitones > max_clip_pitch_semitones ||
      processing.reviewed_warp_markers.size() > max_clip_warp_markers) {
    return Result<void>::failure(invalid("invalid clip processing settings"));
  }
  if (auto warp = validate_clip_warp_settings(processing.warp); !warp)
    return warp;
  auto previous_source = clip.source_range.start.value;
  auto previous_target = rendered.start;
  for (const auto& marker : processing.reviewed_warp_markers) {
    if (marker.source_frame.value <= previous_source ||
        marker.source_frame.value >= clip.source_range.end.value) {
      return Result<void>::failure(invalid("clip warp source markers must increase within trim"));
    }
    std::int64_t target = 0;
    if (const auto* musical = std::get_if<MusicalInterval>(&clip.placement)) {
      // render_placement already established 0 <= start < end, so both the
      // subtraction and the checked-in-range addition below fit in int64_t.
      const auto* offset = std::get_if<timeline::TickPosition>(&marker.target_offset);
      if (offset == nullptr || offset->value <= 0 ||
          offset->value >= musical->end.value - musical->start.value) {
        return Result<void>::failure(invalid("invalid musical clip warp marker offset"));
      }
      auto sample =
          state.tempo.tick_to_sample({musical->start.value + offset->value}, state.sample_rate);
      if (!sample)
        return Result<void>::failure(sample.error());
      target = sample.value().value;
    } else {
      const auto* offset = std::get_if<timeline::SamplePosition>(&marker.target_offset);
      if (offset == nullptr || offset->value <= 0 ||
          offset->value >= rendered.end - rendered.start) {
        return Result<void>::failure(invalid("invalid elapsed clip warp marker offset"));
      }
      target = rendered.start + offset->value;
    }
    if (target <= previous_target || target >= rendered.end) {
      return Result<void>::failure(invalid("clip warp target markers collide at project rate"));
    }
    previous_source = marker.source_frame.value;
    previous_target = target;
  }
  return Result<void>::success();
}

[[nodiscard]] Result<void> validate_midi_timing(const Snapshot& state, const MidiClip& clip,
                                                const RenderedInterval& rendered,
                                                std::size_t& event_count) {
  if (clip.pattern_length.value <= 0 || clip.settings_generation == 0 ||
      clip.notes.size() > max_midi_notes_per_clip) {
    return Result<void>::failure(invalid("invalid MIDI clip pattern, generation or note count"));
  }
  // Only the pattern plays; points after its end are kept but silent. Points
  // before tick 0 bound the one value each repeat starts from.
  std::size_t pedal_points = 0;
  for (const auto& lane : clip.pedals) {
    pedal_points += static_cast<std::size_t>(
        std::ranges::count_if(lane.points, [&](const MidiPedalPoint& point) {
          return point.position < clip.pattern_length;
        }));
  }
  // Notes kept past the pattern's end are silent: collect the audible ones once
  // rather than skipping the silent ones in every repeat.
  std::vector<const MidiNote*> audible;
  audible.reserve(clip.notes.size());
  for (const auto& note : clip.notes) {
    if (note_plays(clip, note)) {
      audible.push_back(&note);
    }
  }
  if (audible.empty() && pedal_points == 0) {
    return Result<void>::success();
  }
  std::int64_t iterations = 1;
  if (clip.loop) {
    std::int64_t end_tick = 0;
    std::int64_t start_tick = 0;
    if (const auto* musical = std::get_if<MusicalInterval>(&clip.placement)) {
      start_tick = musical->start.value;
      end_tick = musical->end.value;
    } else {
      auto anchor = state.tempo.sample_to_tick(std::get<ElapsedInterval>(clip.placement).start,
                                               state.sample_rate);
      auto end = state.tempo.sample_to_tick(std::get<ElapsedInterval>(clip.placement).end,
                                            state.sample_rate);
      if (!anchor || !end) {
        return Result<void>::failure(!anchor ? anchor.error() : end.error());
      }
      start_tick = anchor.value().value;
      end_tick = end.value().value;
    }
    if (end_tick <= start_tick) {
      return Result<void>::failure(invalid("MIDI loop has no representable tick duration"));
    }
    const auto quotient = (end_tick - start_tick) / clip.pattern_length.value;
    if (quotient > 99'998) {
      return Result<void>::failure(exhausted("MIDI loop iteration limit exceeded"));
    }
    iterations = quotient + 2;
  }
  for (std::int64_t iteration = 0; iteration < iterations; ++iteration) {
    if (iteration > std::numeric_limits<std::int64_t>::max() / clip.pattern_length.value) {
      return Result<void>::failure(exhausted("MIDI loop tick overflow"));
    }
    const auto base = iteration * clip.pattern_length.value;
    auto iteration_start = note_sample(state, clip, base);
    if (!iteration_start) {
      return Result<void>::failure(iteration_start.error());
    }
    if (iteration_start.value() >= rendered.end) {
      break;
    }
    for (const auto* audible_note : audible) {
      const auto& note = *audible_note;
      // A note crossing the pattern's end is cut there, as by the clip end.
      const bool cut = note.end > clip.pattern_length;
      auto start_tick = checked_add(base, note.start.value);
      auto end_tick = checked_add(base, std::min(note.end, clip.pattern_length).value);
      if (!start_tick || !end_tick) {
        return Result<void>::failure(!start_tick ? start_tick.error() : end_tick.error());
      }
      auto start = note_sample(state, clip, start_tick.value());
      auto end = note_sample(state, clip, end_tick.value());
      if (!start || !end) {
        return Result<void>::failure(!start ? start.error() : end.error());
      }
      // So is one whose cut leaves less than a sample.
      if (start.value() >= rendered.end || (cut && end.value() <= start.value())) {
        continue;
      }
      const auto cropped_end = std::min(end.value(), rendered.end);
      if (start.value() < rendered.start || cropped_end <= start.value()) {
        return Result<void>::failure(invalid("MIDI note has zero rendered length"));
      }
      if (event_count > max_midi_events_per_project - 2) {
        return Result<void>::failure(exhausted("MIDI event limit exceeded"));
      }
      event_count += 2;
    }
    // Every pedal point is an event in each rendered repeat.
    if (event_count > max_midi_events_per_project - pedal_points) {
      return Result<void>::failure(exhausted("MIDI event limit exceeded"));
    }
    event_count += pedal_points;
  }
  // The clip end releases each pedal lane once.
  if (event_count > max_midi_events_per_project - clip.pedals.size()) {
    return Result<void>::failure(exhausted("MIDI event limit exceeded"));
  }
  event_count += clip.pedals.size();
  return Result<void>::success();
}

[[nodiscard]] Result<void> validate_midi_pedals(const MidiClip& clip) {
  std::array<bool, midi_pedal_count> seen{};
  for (const auto& lane : clip.pedals) {
    const auto index = static_cast<std::size_t>(lane.pedal);
    if (index >= midi_pedal_count || seen[index]) {
      return Result<void>::failure(invalid("invalid or duplicate MIDI pedal lane"));
    }
    seen[index] = true;
    if (lane.points.empty() || lane.points.size() > max_midi_pedal_points_per_lane) {
      return Result<void>::failure(invalid("invalid MIDI pedal point count"));
    }
    // Points before tick 0 are kept from a left trim (see MidiClip).
    std::optional<std::int64_t> previous;
    for (const auto& point : lane.points) {
      if ((previous && point.position.value <= *previous) || !std::isfinite(point.value) ||
          point.value < 0.0 || point.value > 1.0) {
        return Result<void>::failure(invalid("invalid MIDI pedal point"));
      }
      previous = point.position.value;
    }
  }
  return Result<void>::success();
}

} // namespace oscillate::project::detail
