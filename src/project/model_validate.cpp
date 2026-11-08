#include "model_internal.hpp"

#include "oscillate/foundation/midi.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/project/clay_limits.hpp"
#include "oscillate/project/insert_projection.hpp"
#include "oscillate/project/lookup.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oscillate::project::detail {

namespace {

[[nodiscard]] bool valid_tempo_review(const SourceTempoReview& review,
                                      std::uint64_t source_frames) {
  if (!std::isfinite(review.detected_bpm) || !std::isfinite(review.confidence) ||
      review.confidence < 0.0 || review.confidence > 1.0 ||
      (review.detected_bpm != 0.0 && (review.detected_bpm < timeline::min_detected_bpm ||
                                      review.detected_bpm > timeline::max_detected_bpm)) ||
      (review.detected_bpm == 0.0 &&
       (review.confidence != 0.0 || !review.generated_beat_frames.empty() ||
        review.suggestions_truncated)) ||
      (review.corrected_bpm && (!std::isfinite(*review.corrected_bpm) ||
                                *review.corrected_bpm <= 0.0 || *review.corrected_bpm > 1'000.0)) ||
      !valid_beat_frames(review.generated_beat_frames, source_frames) ||
      !valid_beat_frames(review.user_beat_frames, source_frames) ||
      !valid_beat_frames(review.generated_onset_frames, source_frames) ||
      !valid_beat_frames(review.dismissed_onset_frames, source_frames) ||
      (review.downbeat_frame && *review.downbeat_frame >= source_frames) ||
      std::ranges::any_of(review.generated_onset_frames, [&](std::uint64_t frame) {
        return onset_dismissed(review.dismissed_onset_frames, frame);
      })) {
    return false;
  }
  const auto& generated = review.generated_beat_frames;
  const auto& user = review.user_beat_frames;
  auto left = generated.begin();
  auto right = user.begin();
  while (left != generated.end() && right != user.end()) {
    if (*left == *right) {
      return false;
    }
    if (*left < *right) {
      ++left;
    } else {
      ++right;
    }
  }
  return true;
}

[[nodiscard]] MusicalKey related_key(MusicalKey key, KeyRelation relation) {
  if (relation == KeyRelation::relative) {
    return {static_cast<std::uint8_t>((key.tonic + (key.mode == KeyMode::major ? 9 : 3)) %
                                      foundation::semitones_per_octave),
            key.mode == KeyMode::major ? KeyMode::minor : KeyMode::major};
  }
  return {static_cast<std::uint8_t>((key.tonic + (relation == KeyRelation::dominant ? 7 : 5)) %
                                    foundation::semitones_per_octave),
          key.mode};
}

[[nodiscard]] bool valid_key_review(const SourceKeyReview& review) {
  if (!std::isfinite(review.confidence) || review.confidence < 0.0 || review.confidence > 1.0 ||
      review.compatible.size() > max_compatible_keys_per_source ||
      (review.detected && !valid_key(*review.detected)) ||
      (review.correction && !valid_key(*review.correction)) ||
      (!review.detected && (review.confidence != 0.0 || !review.compatible.empty())) ||
      (review.detected && review.confidence <= 0.0)) {
    return false;
  }
  std::set<std::pair<std::uint8_t, KeyMode>> distinct;
  std::set<KeyRelation> relations;
  if (review.detected) {
    distinct.emplace(review.detected->tonic, review.detected->mode);
  }
  for (const auto& compatible : review.compatible) {
    const auto key = compatible.key;
    if (!review.detected || !valid_key(key) ||
        (compatible.relation != KeyRelation::relative &&
         compatible.relation != KeyRelation::dominant &&
         compatible.relation != KeyRelation::subdominant) ||
        !relations.insert(compatible.relation).second ||
        key != related_key(*review.detected, compatible.relation) ||
        !distinct.emplace(key.tonic, key.mode).second) {
      return false;
    }
  }
  return true;
}

// A discrete domain's values sit on one of step_count + 1 evenly spaced steps
// from minimum to maximum.
constexpr double discrete_step_tolerance = 1e-9;
[[nodiscard]] bool on_discrete_step(const AutomationDomain& domain, double value) {
  const double steps =
      (value - domain.minimum) / (domain.maximum - domain.minimum) * domain.step_count;
  return std::abs(steps - std::round(steps)) <= discrete_step_tolerance;
}

[[nodiscard]] Result<void> validate_automation_lanes(const Track& track,
                                                     const std::vector<AutomationLane>& lanes,
                                                     std::size_t owner_limit, bool clip_owned,
                                                     std::set<AutomationLaneId>& lane_ids,
                                                     std::set<AutomationPointId>& point_ids,
                                                     std::size_t& point_count) {
  if (lanes.size() > owner_limit ||
      lanes.size() > max_automation_lanes_per_project - lane_ids.size()) {
    return Result<void>::failure(exhausted("automation lane limit exceeded"));
  }
  std::vector<AutomationTarget> targets;
  targets.reserve(lanes.size());
  for (const auto& lane : lanes) {
    if (!lane_ids.insert(lane.id).second ||
        std::ranges::find(targets, lane.target) != targets.end()) {
      return Result<void>::failure(conflict("duplicate automation lane ID or owner target"));
    }
    targets.push_back(lane.target);
    const auto& domain = lane.domain;
    if (lane.settings_generation == 0 ||
        (lane.timebase != AutomationTimebase::musical &&
         lane.timebase != AutomationTimebase::elapsed) ||
        !valid_text(domain.semantic_key, 128, true) || !valid_text(domain.unit, 64, false) ||
        !std::isfinite(domain.minimum) || !std::isfinite(domain.maximum) ||
        !std::isfinite(domain.default_value) || domain.minimum >= domain.maximum ||
        domain.default_value < domain.minimum || domain.default_value > domain.maximum ||
        domain.step_count > 16'384 || domain.discrete != (domain.step_count != 0) ||
        (domain.discrete && !on_discrete_step(domain, domain.default_value)) ||
        static_cast<unsigned>(domain.scale) > static_cast<unsigned>(AutomationScale::decibel) ||
        (domain.scale != AutomationScale::linear && domain.discrete) ||
        (domain.scale == AutomationScale::logarithmic && !(domain.minimum > 0.0)) ||
        (domain.scale == AutomationScale::decibel && domain.minimum < 0.0)) {
      return Result<void>::failure(invalid("invalid automation lane or domain"));
    }
    if (const auto* control = std::get_if<TrackControlTarget>(&lane.target)) {
      const auto expected = channel_control_domain(control->key);
      if (control->track != track.id || domain.semantic_key != expected.semantic_key ||
          domain.representation != AutomationRepresentation::physical ||
          domain.minimum != expected.minimum || domain.maximum != expected.maximum ||
          domain.discrete != expected.discrete || domain.step_count != expected.step_count ||
          domain.scale != expected.scale) {
        return Result<void>::failure(invalid("unbacked track automation control"));
      }
    } else if (const auto* native = std::get_if<NativeParameterTarget>(&lane.target)) {
      if (native->track != track.id || native_source(track) == nullptr ||
          !valid_native_identifier(native->parameter_id) ||
          domain.semantic_key != native->parameter_id ||
          domain.representation != AutomationRepresentation::physical) {
        return Result<void>::failure(invalid("orphan or invalid native instrument automation"));
      }
    } else if (const auto* effect = std::get_if<NativeEffectParameterTarget>(&lane.target)) {
      if (std::ranges::none_of(track.effects,
                               [&](const EffectInsert& slot) {
                                 return slot.id == effect->slot &&
                                        std::holds_alternative<NativeEffectPatch>(slot.processor);
                               }) ||
          !valid_native_identifier(effect->parameter_id) ||
          domain.semantic_key != effect->parameter_id ||
          domain.representation != AutomationRepresentation::physical) {
        return Result<void>::failure(invalid("orphan or invalid built-in effect automation"));
      }
    } else if (const auto* macro = std::get_if<MacroTarget>(&lane.target)) {
      if (macro->track != track.id || macro->index >= track.macros.size() ||
          domain.representation != AutomationRepresentation::physical ||
          domain.semantic_key != "macro" || domain.unit != "normalized" || domain.minimum != 0.0 ||
          domain.maximum != 1.0 || domain.discrete || domain.scale != AutomationScale::linear) {
        return Result<void>::failure(invalid("orphan or invalid macro automation"));
      }
    } else if (const auto* send_target = std::get_if<SendGainTarget>(&lane.target)) {
      if (std::ranges::none_of(
              track.sends, [&](const SendRoute& send) { return send.id == send_target->send; }) ||
          domain.representation != AutomationRepresentation::physical ||
          domain.semantic_key != "mixer.send_gain" || domain.minimum != 0.0 ||
          domain.maximum != 4.0 || domain.discrete || domain.step_count != 0 ||
          domain.scale != AutomationScale::decibel) {
        return Result<void>::failure(invalid("orphan or invalid send gain automation target"));
      }
    } else {
      const auto& vst = std::get<Vst3ParameterTarget>(lane.target);
      const auto plugins = track_plugins(track);
      if (std::ranges::none_of(
              plugins,
              [&](const Vst3PluginInstance* plugin) { return plugin->id == vst.plugin; }) ||
          domain.representation != AutomationRepresentation::vst3_normalized ||
          domain.minimum != 0.0 || domain.maximum != 1.0 ||
          domain.scale != AutomationScale::linear) {
        return Result<void>::failure(invalid("orphan or invalid VST3 automation target"));
      }
    }
    if ((clip_owned && lane.points.empty()) ||
        lane.points.size() > max_automation_points_per_lane ||
        lane.points.size() > max_automation_points_per_project - point_count) {
      return Result<void>::failure(exhausted("automation point limit exceeded"));
    }
    std::int64_t previous = -1;
    for (const auto& point : lane.points) {
      const auto& h = point.handles;
      if (!point_ids.insert(point.id).second || point.coordinate <= previous ||
          point.to_next > AutomationShape::bezier || !std::isfinite(point.value) ||
          point.value < domain.minimum || point.value > domain.maximum || !std::isfinite(h.x1) ||
          !std::isfinite(h.y1) || !std::isfinite(h.x2) || !std::isfinite(h.y2) || h.x1 < 0.0 ||
          h.x1 > h.x2 || h.x2 > 1.0 || h.y1 < 0.0 || h.y1 > 1.0 || h.y2 < 0.0 || h.y2 > 1.0 ||
          (domain.discrete &&
           (point.to_next != AutomationShape::hold || !on_discrete_step(domain, point.value)))) {
        return Result<void>::failure(invalid("invalid automation point"));
      }
      previous = point.coordinate;
    }
    point_count += lane.points.size();
  }
  return Result<void>::success();
}

[[nodiscard]] bool valid_sparse_parameters(const std::vector<NativeInstrumentParameter>& values) {
  std::set<std::string_view> ids;
  return values.size() <= max_native_instrument_parameters &&
         std::ranges::all_of(values, [&](const NativeInstrumentParameter& p) {
           return valid_native_identifier(p.id) && std::isfinite(p.value) &&
                  ids.insert(p.id).second;
         });
}

// Structure only (ADR-0059); the engine resolves ids and ranges.
[[nodiscard]] Result<void> validate_note_effect(const NoteEffectInsert& effect) {
  if (!valid_native_identifier(effect.engine) || effect.engine_version == 0 ||
      !valid_sparse_parameters(effect.parameters)) {
    return Result<void>::failure(invalid("invalid note effect"));
  }
  return Result<void>::success();
}

[[nodiscard]] AutomationTarget automation_target(const MacroDestination& destination) {
  return std::visit([](const auto& target) -> AutomationTarget { return target; }, destination);
}

// A macro's mappings must name parameters on its own track, each mapped by
// at most one macro, with a sound copy of the parameter's domain.
[[nodiscard]] Result<void> validate_macros(const Track& track) {
  if (track.macros.size() > max_macros_per_track) {
    return Result<void>::failure(exhausted("macro limit exceeded"));
  }
  std::vector<MacroDestination> mapped;
  for (const auto& macro : track.macros) {
    if (!valid_text(macro.name, max_macro_name_bytes, false) || !std::isfinite(macro.value) ||
        macro.value < 0.0 || macro.value > 1.0) {
      return Result<void>::failure(invalid("invalid macro"));
    }
    if (macro.mappings.size() > max_macro_mappings) {
      return Result<void>::failure(exhausted("macro mapping limit exceeded"));
    }
    for (const auto& mapping : macro.mappings) {
      const auto& domain = mapping.domain;
      if (std::ranges::find(mapped, mapping.destination) != mapped.end()) {
        return Result<void>::failure(conflict("a parameter is mapped by more than one macro"));
      }
      mapped.push_back(mapping.destination);
      const bool owned = std::visit(
          [&](const auto& destination) {
            using D = std::decay_t<decltype(destination)>;
            if constexpr (std::is_same_v<D, NativeParameterTarget>) {
              return destination.track == track.id && native_source(track) != nullptr &&
                     valid_native_identifier(destination.parameter_id) &&
                     domain.semantic_key == destination.parameter_id;
            } else {
              const bool effect = std::ranges::any_of(track.effects, [&](const EffectInsert& slot) {
                return slot.id == destination.slot &&
                       std::holds_alternative<NativeEffectPatch>(slot.processor);
              });
              const bool note =
                  std::ranges::any_of(track.note_effects, [&](const NoteEffectInsert& slot) {
                    return slot.id == destination.slot;
                  });
              return (effect || note) && valid_native_identifier(destination.parameter_id) &&
                     domain.semantic_key == destination.parameter_id;
            }
          },
          mapping.destination);
      if (!owned) {
        return Result<void>::failure(invalid("macro mapping names no parameter on its track"));
      }
      if (domain.representation != AutomationRepresentation::physical ||
          !std::isfinite(domain.minimum) || !std::isfinite(domain.maximum) ||
          !std::isfinite(domain.default_value) || domain.minimum >= domain.maximum ||
          domain.step_count > 16'384 || domain.discrete != (domain.step_count != 0) ||
          static_cast<unsigned>(domain.scale) > static_cast<unsigned>(AutomationScale::decibel) ||
          (domain.scale != AutomationScale::linear && domain.discrete) ||
          (domain.scale == AutomationScale::logarithmic && !(domain.minimum > 0.0)) ||
          (domain.scale == AutomationScale::decibel && domain.minimum < 0.0) ||
          !std::isfinite(mapping.minimum) || !std::isfinite(mapping.maximum) ||
          mapping.minimum < domain.minimum || mapping.minimum > domain.maximum ||
          mapping.maximum < domain.minimum || mapping.maximum > domain.maximum) {
        return Result<void>::failure(invalid("invalid macro mapping range"));
      }
    }
  }
  // A mapped parameter is driven by its macro; a lane of its own would fight it.
  const auto own_lane = [&](const AutomationLane& lane) {
    return std::ranges::any_of(mapped, [&](const MacroDestination& destination) {
      return automation_target(destination) == lane.target;
    });
  };
  if (std::ranges::any_of(track.automation, own_lane) ||
      std::ranges::any_of(
          track.clips,
          [&](const Clip& clip) { return std::ranges::any_of(clip.automation, own_lane); }) ||
      std::ranges::any_of(track.midi_clips, [&](const MidiClip& clip) {
        return std::ranges::any_of(clip.automation, own_lane);
      })) {
    return Result<void>::failure(
        conflict("a macro-mapped parameter cannot have its own automation lane"));
  }
  return Result<void>::success();
}

[[nodiscard]] Result<void> validate_frozen(const Track& track, const Snapshot& snapshot) {
  if (!track.frozen) {
    return Result<void>::success();
  }
  const auto& frozen = *track.frozen;
  const auto* source = managed_audio_source(frozen.audio, snapshot);
  if ((track.kind != TrackKind::audio && track.kind != TrackKind::instrument) ||
      frozen.rendered_revision == 0 || source == nullptr ||
      source->encoding != SourceEncoding::wav || frozen.audio.sample_rate != snapshot.sample_rate ||
      frozen.audio.channels != 2 || frozen.audio.frames == 0 ||
      frozen.first_frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                               frozen.audio.frames) {
    return Result<void>::failure(invalid("frozen track render does not match a managed source"));
  }
  return Result<void>::success();
}

struct RenderedInterval {
  std::int64_t start;
  std::int64_t end;
  ClipId id;
};

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

} // namespace

[[nodiscard]] Result<void> validate(const Snapshot& state) {
  if (!timeline::supported_sample_rate(state.sample_rate)) {
    return Result<void>::failure(invalid("unsupported project sample rate"));
  }
  if (!std::isfinite(state.master_level) || state.master_level < 0.0 || state.master_level > 4.0)
    return Result<void>::failure(invalid("invalid master level"));
  if (state.next_insert_slot_serial == 0) {
    return Result<void>::failure(invalid("zero effect slot allocator serial"));
  }
  if (state.key && !valid_key(*state.key)) {
    return Result<void>::failure(invalid("invalid project key"));
  }
  if (state.markers.size() > max_markers_per_project) {
    return Result<void>::failure(exhausted("arrangement marker limit exceeded"));
  }
  {
    std::set<MarkerId> arrangement_marker_ids;
    for (std::size_t index = 0; index < state.markers.size(); ++index) {
      const auto& marker = state.markers[index];
      if (!arrangement_marker_ids.insert(marker.id).second) {
        return Result<void>::failure(conflict("duplicate arrangement marker"));
      }
      // Sorted by position, one marker per position.
      if (marker.position.value < 0 ||
          (index > 0 && state.markers[index - 1].position.value >= marker.position.value) ||
          !valid_text(marker.name, max_marker_name_bytes, false)) {
        return Result<void>::failure(invalid("invalid arrangement marker"));
      }
    }
  }
  if (state.sources.size() > max_source_records) {
    return Result<void>::failure(exhausted("source record limit exceeded"));
  }
  if (state.tracks.size() > max_routing_tracks) {
    return Result<void>::failure(exhausted("routing track limit exceeded"));
  }
  std::map<SourceId, std::uint64_t> source_frames;
  for (const auto& source : state.sources) {
    if (!source_frames.emplace(source.id, source.frames).second) {
      return Result<void>::failure(conflict("duplicate source " + source.id.to_string()));
    }
    auto valid = validate_source(source);
    if (!valid) {
      return valid;
    }
  }
  if (state.source_tempo_reviews.size() > state.sources.size()) {
    return Result<void>::failure(exhausted("source tempo review limit exceeded"));
  }
  std::set<SourceId> review_ids;
  std::size_t marker_count = 0;
  for (const auto& review : state.source_tempo_reviews) {
    const auto source = source_frames.find(review.source);
    if (source == source_frames.end() || !review_ids.insert(review.source).second) {
      return Result<void>::failure(invalid("missing or duplicate source tempo review"));
    }
    if (review.generated_beat_frames.size() > max_tempo_markers_per_source ||
        review.user_beat_frames.size() > max_tempo_markers_per_source ||
        review.generated_onset_frames.size() > max_tempo_markers_per_source ||
        review.dismissed_onset_frames.size() > max_tempo_markers_per_source) {
      return Result<void>::failure(exhausted("source tempo marker limit exceeded"));
    }
    const auto count = review.generated_beat_frames.size() + review.user_beat_frames.size() +
                       review.generated_onset_frames.size() + review.dismissed_onset_frames.size();
    if (count > max_tempo_markers_per_project - marker_count) {
      return Result<void>::failure(exhausted("source tempo marker budget exceeded"));
    }
    marker_count += count;
    if (!valid_tempo_review(review, source->second)) {
      return Result<void>::failure(invalid("invalid source tempo review"));
    }
  }
  if (state.source_key_reviews.size() > state.sources.size()) {
    return Result<void>::failure(exhausted("source key review limit exceeded"));
  }
  std::set<SourceId> key_review_ids;
  for (const auto& review : state.source_key_reviews) {
    if (!source_frames.contains(review.source) || !key_review_ids.insert(review.source).second ||
        !valid_key_review(review)) {
      return Result<void>::failure(invalid("invalid or duplicate source key review"));
    }
  }
  // Ordered sets keep decoded-manifest validation bounded for large projects,
  // including adversarial duplicate IDs near the end of the input.
  std::set<TrackId> track_ids;
  std::set<ClipId> clip_ids;
  std::set<NoteId> note_ids;
  std::set<PluginInstanceId> plugin_ids;
  std::set<AutomationLaneId> automation_lane_ids;
  std::set<AutomationPointId> automation_point_ids;
  std::size_t automation_point_count = 0;
  std::size_t plugin_state_bytes = 0;
  std::size_t midi_event_count = 0;
  std::size_t effect_count = 0;
  std::set<InsertSlotId> slot_ids;
  const auto validate_saved_plugin = [&](const Vst3PluginInstance& plugin) -> Result<void> {
    if (plugin_ids.size() >= max_vst3_plugins_per_project) {
      return Result<void>::failure(exhausted("VST3 plugin count limit exceeded"));
    }
    if (!plugin_ids.insert(plugin.id).second) {
      return Result<void>::failure(conflict("duplicate VST3 plugin instance"));
    }
    auto valid = validate_plugin(plugin);
    if (!valid) {
      return valid;
    }
    if (plugin.component_state.size() > max_vst3_project_state_bytes - plugin_state_bytes) {
      return Result<void>::failure(exhausted("VST3 project state limit exceeded"));
    }
    plugin_state_bytes += plugin.component_state.size();
    const auto controller_bytes = plugin.controller_state ? plugin.controller_state->size() : 0;
    if (controller_bytes > max_vst3_project_state_bytes - plugin_state_bytes)
      return Result<void>::failure(exhausted("VST3 project state limit exceeded"));
    plugin_state_bytes += controller_bytes;
    return Result<void>::success();
  };
  const auto validate_effect = [&](const EffectInsert& slot) -> Result<void> {
    if (slot.id.bits().high == command_slot_namespace &&
        slot.id.bits().low >= state.next_insert_slot_serial) {
      return Result<void>::failure(invalid("effect slot exceeds allocator high-water mark"));
    }
    if (!slot_ids.insert(slot.id).second) {
      return Result<void>::failure(conflict("duplicate effect slot ID"));
    }
    return std::visit(
        [&](const auto& processor) -> Result<void> {
          using T = std::decay_t<decltype(processor)>;
          if constexpr (std::is_same_v<T, NativeConvolution>) {
            return validate_native_convolution(processor, state);
          } else if constexpr (std::is_same_v<T, NativeEffectPatch>) {
            return validate_native_effect_patch(processor, state);
          } else {
            return validate_saved_plugin(processor);
          }
        },
        slot.processor);
  };
  for (const auto& track : state.tracks) {
    if (!track_ids.insert(track.id).second) {
      return Result<void>::failure(conflict("duplicate track " + track.id.to_string()));
    }
    if (!std::isfinite(track.level) || track.level < 0.0 || track.level > 4.0 ||
        !std::isfinite(track.pan) || track.pan < -1.0 || track.pan > 1.0)
      return Result<void>::failure(invalid("invalid track level or pan"));
    const auto valid_color = [](const std::optional<std::uint32_t>& color) {
      return !color || *color <= max_presentation_color;
    };
    if (!valid_text(track.name, max_track_name_bytes, false) || !valid_color(track.color) ||
        !std::ranges::all_of(track.clips,
                             [&](const Clip& clip) {
                               return valid_color(clip.color) &&
                                      valid_text(clip.name, max_track_name_bytes, false);
                             }) ||
        !std::ranges::all_of(track.midi_clips,
                             [&](const MidiClip& clip) { return valid_color(clip.color); })) {
      return Result<void>::failure(invalid("invalid track name or colour"));
    }
    std::vector<RenderedInterval> intervals;
    intervals.reserve(track.clips.size() + track.midi_clips.size());
    if (track.kind != TrackKind::audio && track.kind != TrackKind::instrument &&
        track.kind != TrackKind::group_bus && track.kind != TrackKind::return_bus) {
      return Result<void>::failure(invalid("invalid track kind"));
    }
    if (track.kind == TrackKind::audio &&
        (!std::holds_alternative<std::monostate>(track.instrument_source) ||
         !track.midi_clips.empty())) {
      return Result<void>::failure(invalid("audio track contains instrument state"));
    }
    if (track.kind == TrackKind::instrument && !track.clips.empty()) {
      return Result<void>::failure(invalid("instrument track contains audio clips"));
    }
    if ((track.kind == TrackKind::group_bus || track.kind == TrackKind::return_bus) &&
        (!track.clips.empty() || !track.midi_clips.empty() ||
         !std::holds_alternative<std::monostate>(track.instrument_source))) {
      return Result<void>::failure(invalid("bus track contains source clips or instrument source"));
    }
    if (const auto* native = native_source(track)) {
      std::set<std::string_view> ids;
      if (track.kind != TrackKind::instrument || !valid_native_identifier(native->engine) ||
          native->engine_version == 0 ||
          native->parameters.size() > max_native_instrument_parameters ||
          !std::ranges::all_of(native->parameters, [&](const NativeInstrumentParameter& p) {
            return valid_native_identifier(p.id) && std::isfinite(p.value) &&
                   ids.insert(p.id).second;
          })) {
        return Result<void>::failure(invalid("invalid native instrument patch"));
      }
      auto document = validate_native_document(native->document, state);
      if (!document) {
        return document;
      }
    }
    if (track.effects.size() > max_effect_slots_per_path ||
        track.effects.size() > max_effect_slots_per_project - effect_count) {
      return Result<void>::failure(exhausted("effect slot limit exceeded"));
    }
    if (track_plugins(track).size() > max_vst3_plugins_per_track) {
      return Result<void>::failure(exhausted("track VST3 plugin limit exceeded"));
    }
    effect_count += track.effects.size();
    for (const auto& slot : track.effects) {
      auto valid = validate_effect(slot);
      if (!valid) {
        return valid;
      }
    }
    if (const auto* plugin = std::get_if<Vst3PluginInstance>(&track.instrument_source)) {
      auto valid = validate_saved_plugin(*plugin);
      if (!valid)
        return valid;
    }
    if (!track.note_effects.empty() && track.kind != TrackKind::instrument) {
      return Result<void>::failure(invalid("note effects need an instrument track"));
    }
    if (track.note_effects.size() > max_note_effects_per_track) {
      return Result<void>::failure(exhausted("note effect limit exceeded"));
    }
    for (const auto& slot : track.note_effects) {
      if (slot.id.bits().high == command_slot_namespace &&
          slot.id.bits().low >= state.next_insert_slot_serial) {
        return Result<void>::failure(invalid("note effect slot exceeds allocator high-water mark"));
      }
      if (!slot_ids.insert(slot.id).second) {
        return Result<void>::failure(conflict("duplicate effect slot ID"));
      }
      auto valid = validate_note_effect(slot);
      if (!valid)
        return valid;
    }
    if (auto valid = validate_frozen(track, state); !valid) {
      return valid;
    }
    auto track_lanes = validate_automation_lanes(
        track, track.automation, max_automation_lanes_per_track, false, automation_lane_ids,
        automation_point_ids, automation_point_count);
    if (!track_lanes) {
      return track_lanes;
    }
    for (const auto& clip : track.clips) {
      if (!clip_ids.insert(clip.id).second) {
        return Result<void>::failure(conflict("duplicate clip " + clip.id.to_string()));
      }
      if (clip.source_range.start.value < 0 || clip.source_range.end <= clip.source_range.start) {
        return Result<void>::failure(
            invalid("invalid source range for clip " + clip.id.to_string()));
      }
      const auto known_source = source_frames.find(clip.source);
      if (known_source != source_frames.end() &&
          static_cast<std::uint64_t>(clip.source_range.end.value) > known_source->second) {
        return Result<void>::failure(
            invalid("clip source range exceeds known source frames: " + clip.id.to_string()));
      }
      if (clip.settings_generation == 0) {
        return Result<void>::failure(
            invalid("zero settings generation for clip " + clip.id.to_string()));
      }
      auto rendered = render_clip(state, clip);
      if (!rendered) {
        return Result<void>::failure(rendered.error());
      }
      auto processing = validate_clip_processing(state, clip, rendered.value());
      if (!processing)
        return processing;
      if (clip.clay) {
        if (auto clay = validate_clay(*clip.clay); !clay)
          return clay;
      }
      const auto& fades = clip.fades;
      if (!std::isfinite(fades.in_fraction) || !std::isfinite(fades.out_fraction) ||
          fades.in_fraction < 0.0 || fades.out_fraction < 0.0 ||
          fades.in_fraction + fades.out_fraction > 1.0 ||
          (fades.in_curve != FadeCurve::linear && fades.in_curve != FadeCurve::smoothstep) ||
          (fades.out_curve != FadeCurve::linear && fades.out_curve != FadeCurve::smoothstep) ||
          (rendered.value().end - rendered.value().start == 1 &&
           (fades.in_fraction > 0.0 || fades.out_fraction > 0.0)))
        return Result<void>::failure(invalid("invalid fades for clip " + clip.id.to_string()));
      intervals.push_back(rendered.value());
      auto clip_lanes = validate_automation_lanes(
          track, clip.automation, max_automation_lanes_per_clip, true, automation_lane_ids,
          automation_point_ids, automation_point_count);
      if (!clip_lanes) {
        return clip_lanes;
      }
    }
    for (const auto& clip : track.midi_clips) {
      if (!clip_ids.insert(clip.id).second) {
        return Result<void>::failure(conflict("duplicate clip " + clip.id.to_string()));
      }
      for (const auto& note : clip.notes) {
        if (!note_ids.insert(note.id).second) {
          return Result<void>::failure(conflict("duplicate MIDI note " + note.id.to_string()));
        }
        // A note may lie before tick 0 or past the pattern's end (kept,
        // silent; see MidiClip).
        if (note.end <= note.start || note.key > foundation::max_midi_key ||
            note.velocity < foundation::min_note_velocity ||
            note.velocity > foundation::max_note_velocity) {
          return Result<void>::failure(invalid("invalid MIDI note " + note.id.to_string()));
        }
      }
      auto valid_pedals = validate_midi_pedals(clip);
      if (!valid_pedals) {
        return valid_pedals;
      }
      auto rendered = render_placement(state, clip.placement, clip.id);
      if (!rendered) {
        return Result<void>::failure(rendered.error());
      }
      auto valid_midi = validate_midi_timing(state, clip, rendered.value(), midi_event_count);
      if (!valid_midi) {
        return valid_midi;
      }
      intervals.push_back(rendered.value());
      auto clip_lanes = validate_automation_lanes(
          track, clip.automation, max_automation_lanes_per_clip, true, automation_lane_ids,
          automation_point_ids, automation_point_count);
      if (!clip_lanes) {
        return clip_lanes;
      }
    }
    if (auto valid = validate_macros(track); !valid) {
      return valid;
    }
    std::ranges::sort(intervals, [](const RenderedInterval& a, const RenderedInterval& b) {
      return a.start < b.start;
    });
    for (std::size_t i = 1; i < intervals.size(); ++i) {
      if (intervals[i].start < intervals[i - 1].end) {
        return Result<void>::failure(conflict("track " + track.id.to_string() + ": clips " +
                                              intervals[i - 1].id.to_string() + " and " +
                                              intervals[i].id.to_string() + " overlap"));
      }
    }
  }
  if (state.master_effects.size() > max_effect_slots_per_path ||
      state.master_effects.size() > max_effect_slots_per_project - effect_count) {
    return Result<void>::failure(exhausted("master effect slot limit exceeded"));
  }
  for (const auto& slot : state.master_effects) {
    auto valid = validate_effect(slot);
    if (!valid) {
      return valid;
    }
  }
  std::map<TrackId, std::size_t> routing_index;
  for (std::size_t index = 0; index < state.tracks.size(); ++index) {
    routing_index.emplace(state.tracks[index].id, index);
  }
  std::set<SendId> send_ids;
  std::map<TrackId, InsertSlotId> sidechain_targets;
  std::vector<std::vector<std::size_t>> outgoing(state.tracks.size());
  std::vector<std::size_t> indegree(state.tracks.size(), 0);
  for (std::size_t index = 0; index < state.tracks.size(); ++index) {
    const auto& track = state.tracks[index];
    if (track.sends.size() > max_sends_per_track ||
        track.sends.size() > max_sends_per_project - send_ids.size()) {
      return Result<void>::failure(exhausted("send route count limit exceeded"));
    }
    auto add_edge = [&](TrackId destination, bool main, bool sidechain = false) -> Result<void> {
      const auto found = routing_index.find(destination);
      if (found == routing_index.end()) {
        return Result<void>::failure(
            invalid("routing destination is missing: " + destination.to_string()));
      }
      const auto kind = state.tracks[found->second].kind;
      if ((main && kind != TrackKind::group_bus) ||
          (!main && !sidechain && kind != TrackKind::group_bus && kind != TrackKind::return_bus)) {
        return Result<void>::failure(invalid("routing destination has unsupported track kind"));
      }
      outgoing[index].push_back(found->second);
      ++indegree[found->second];
      return Result<void>::success();
    };
    if (track.main_output) {
      auto valid = add_edge(*track.main_output, true);
      if (!valid) {
        return valid;
      }
    }
    for (const auto& send : track.sends) {
      if (!send_ids.insert(send.id).second) {
        return Result<void>::failure(conflict("duplicate send route ID"));
      }
      if (!std::isfinite(send.gain) || send.gain < 0.0 || send.gain > 4.0) {
        return Result<void>::failure(invalid("send gain must be finite and within 0..4"));
      }
      if (send.purpose != SendPurpose::audible && send.purpose != SendPurpose::sidechain)
        return Result<void>::failure(invalid("send purpose is invalid"));
      if ((send.purpose == SendPurpose::audible && send.target_insert) ||
          (send.purpose == SendPurpose::sidechain && !send.target_insert))
        return Result<void>::failure(invalid("send target does not match its purpose"));
      const bool sidechain = send.purpose == SendPurpose::sidechain;
      auto valid = add_edge(send.destination, false, sidechain);
      if (!valid) {
        return valid;
      }
      if (sidechain) {
        const auto [target, inserted] =
            sidechain_targets.emplace(send.destination, send.target_insert.value());
        if (!inserted && target->second != send.target_insert.value())
          return Result<void>::failure(
              invalid("one track cannot route sidechains to multiple effect slots"));
        const auto destination = routing_index.at(send.destination);
        const auto& effects = state.tracks[destination].effects;
        if (!std::ranges::any_of(effects, [&](const EffectInsert& effect) {
              // A switched-off target keeps its key; it is inactive until
              // the effect is on again.
              return effect.id == send.target_insert.value() &&
                     (std::holds_alternative<Vst3PluginInstance>(effect.processor) ||
                      std::holds_alternative<NativeEffectPatch>(effect.processor));
            }))
          // Whether a built-in effect engine reads a detector is the route
          // plan's check; the model sees only structure.
          return Result<void>::failure(
              invalid("sidechain target must be a built-in effect or VST3 effect"));
      }
    }
  }
  std::vector<std::size_t> ready;
  ready.reserve(state.tracks.size());
  for (std::size_t index = 0; index < indegree.size(); ++index) {
    if (indegree[index] == 0) {
      ready.push_back(index);
    }
  }
  for (std::size_t next = 0; next < ready.size(); ++next) {
    for (const auto destination : outgoing[ready[next]]) {
      if (--indegree[destination] == 0) {
        ready.push_back(destination);
      }
    }
  }
  if (ready.size() != state.tracks.size()) {
    return Result<void>::failure(conflict("routing cycle detected"));
  }
  return Result<void>::success();
}

} // namespace oscillate::project::detail
