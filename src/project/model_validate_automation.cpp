#include "model_internal.hpp"

#include "model_validate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>
#include <vector>

// Validation of automation lanes, note effects, macros and frozen tracks.
namespace oscillate::project::detail {
namespace {

// An automation domain's bounds: its discrete steps and the lengths of its
// semantic key and unit.
constexpr std::uint32_t max_domain_steps = 16'384;
constexpr std::size_t max_semantic_key_bytes = 128;
constexpr std::size_t max_unit_bytes = 64;

// A discrete domain's values sit on one of step_count + 1 evenly spaced steps
// from minimum to maximum.
constexpr double discrete_step_tolerance = 1e-9;

[[nodiscard]] bool on_discrete_step(const AutomationDomain& domain, double value) {
  const double steps =
      (value - domain.minimum) / (domain.maximum - domain.minimum) * domain.step_count;
  return std::abs(steps - std::round(steps)) <= discrete_step_tolerance;
}

[[nodiscard]] bool valid_sparse_parameters(const std::vector<NativeInstrumentParameter>& values) {
  std::set<std::string_view> ids;
  return values.size() <= max_native_instrument_parameters &&
         std::ranges::all_of(values, [&](const NativeInstrumentParameter& p) {
           return valid_native_identifier(p.id) && std::isfinite(p.value) &&
                  ids.insert(p.id).second;
         });
}

[[nodiscard]] AutomationTarget automation_target(const MacroDestination& destination) {
  return std::visit([](const auto& target) -> AutomationTarget { return target; }, destination);
}

} // namespace

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
        !valid_text(domain.semantic_key, max_semantic_key_bytes, true) ||
        !valid_text(domain.unit, max_unit_bytes, false) || !std::isfinite(domain.minimum) ||
        !std::isfinite(domain.maximum) || !std::isfinite(domain.default_value) ||
        domain.minimum >= domain.maximum || domain.default_value < domain.minimum ||
        domain.default_value > domain.maximum || domain.step_count > max_domain_steps ||
        domain.discrete != (domain.step_count != 0) ||
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

// Structure only (ADR-0059); the engine resolves ids and ranges.
[[nodiscard]] Result<void> validate_note_effect(const NoteEffectInsert& effect) {
  if (!valid_native_identifier(effect.engine) || effect.engine_version == 0 ||
      !valid_sparse_parameters(effect.parameters)) {
    return Result<void>::failure(invalid("invalid note effect"));
  }
  return Result<void>::success();
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
          domain.step_count > max_domain_steps || domain.discrete != (domain.step_count != 0) ||
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

} // namespace oscillate::project::detail
