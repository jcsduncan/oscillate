#include "model_internal.hpp"

#include "model_validate.hpp"

#include "oscillate/foundation/midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Snapshot validation: the whole-snapshot check every command runs before
// it publishes, and the source review checks.
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
          if constexpr (std::is_same_v<T, NativeEffectPatch>) {
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
