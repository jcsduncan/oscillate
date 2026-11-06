#include "oscillate/automation_project/preparation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
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

namespace oscillate::automation_project {
namespace {
using foundation::ErrorCode;
using foundation::Result;

static_assert(static_cast<unsigned>(project::AutomationShape::hold) ==
              static_cast<unsigned>(automation::Shape::hold));
static_assert(static_cast<unsigned>(project::AutomationShape::linear) ==
              static_cast<unsigned>(automation::Shape::linear));
static_assert(static_cast<unsigned>(project::AutomationShape::quadratic) ==
              static_cast<unsigned>(automation::Shape::quadratic));
static_assert(static_cast<unsigned>(project::AutomationShape::exponential) ==
              static_cast<unsigned>(automation::Shape::exponential));
static_assert(static_cast<unsigned>(project::AutomationShape::ease_in_out) ==
              static_cast<unsigned>(automation::Shape::ease_in_out));
static_assert(static_cast<unsigned>(project::AutomationShape::bezier) ==
              static_cast<unsigned>(automation::Shape::bezier));

[[nodiscard]] foundation::Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}

[[nodiscard]] Result<std::int64_t> add_coordinate(std::int64_t anchor, std::int64_t local) {
  if (anchor < 0 || local < 0 || local > std::numeric_limits<std::int64_t>::max() - anchor) {
    return Result<std::int64_t>::failure(
        {ErrorCode::resource_exhausted, "automation coordinate overflow"});
  }
  return Result<std::int64_t>::success(anchor + local);
}

[[nodiscard]] Result<automation::Curve> absolute_curve(const project::AutomationLane& lane,
                                                       std::int64_t anchor,
                                                       const timeline::TempoMap& tempo,
                                                       std::uint32_t sample_rate) {
  if (lane.timebase != project::AutomationTimebase::musical &&
      lane.timebase != project::AutomationTimebase::elapsed) {
    return Result<automation::Curve>::failure(invalid("invalid automation timebase"));
  }
  automation::Curve curve;
  curve.timebase = lane.timebase == project::AutomationTimebase::musical
                       ? automation::Timebase::musical
                       : automation::Timebase::elapsed;
  curve.points.reserve(lane.points.size());
  std::int64_t previous_coordinate = -1;
  std::int64_t previous_sample = -1;
  for (const auto& point : lane.points) {
    auto absolute = add_coordinate(anchor, point.coordinate);
    if (!absolute) {
      return Result<automation::Curve>::failure(absolute.error());
    }
    if (absolute.value() <= previous_coordinate) {
      return Result<automation::Curve>::failure(invalid("automation coordinates collide"));
    }
    previous_coordinate = absolute.value();
    if (curve.timebase == automation::Timebase::musical) {
      auto sample = tempo.tick_to_sample({absolute.value()}, sample_rate);
      if (!sample) {
        return Result<automation::Curve>::failure(sample.error());
      }
      if (sample.value().value <= previous_sample) {
        return Result<automation::Curve>::failure(
            invalid("musical automation points collide at project sample rate"));
      }
      previous_sample = sample.value().value;
    }
    curve.points.push_back(
        {absolute.value(),
         point.value,
         static_cast<automation::Shape>(point.to_next),
         {point.handles.x1, point.handles.y1, point.handles.x2, point.handles.y2}});
  }
  return Result<automation::Curve>::success(std::move(curve));
}

struct Interval {
  timeline::SamplePosition begin;
  timeline::SamplePosition end;
  std::int64_t tick_anchor;
};

[[nodiscard]] Result<Interval> clip_interval(const project::Placement& placement,
                                             const timeline::TempoMap& tempo,
                                             std::uint32_t sample_rate) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    if (musical->start.value < 0 || musical->end <= musical->start) {
      return Result<Interval>::failure(invalid("invalid musical clip placement"));
    }
    auto begin = tempo.tick_to_sample(musical->start, sample_rate);
    auto end = tempo.tick_to_sample(musical->end, sample_rate);
    if (!begin || !end) {
      return Result<Interval>::failure(!begin ? begin.error() : end.error());
    }
    if (end.value() <= begin.value()) {
      return Result<Interval>::failure(invalid("clip renders to zero samples"));
    }
    return Result<Interval>::success({begin.value(), end.value(), musical->start.value});
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(placement);
  if (elapsed.start.value < 0 || elapsed.end <= elapsed.start) {
    return Result<Interval>::failure(invalid("invalid elapsed clip placement"));
  }
  auto tick = tempo.sample_to_tick(elapsed.start, sample_rate);
  if (!tick) {
    return Result<Interval>::failure(tick.error());
  }
  return Result<Interval>::success({elapsed.start, elapsed.end, tick.value().value});
}

[[nodiscard]] Result<void> validate_target(const project::Track& track,
                                           const project::AutomationLane& lane) {
  if (lane.settings_generation == 0 ||
      lane.points.size() > project::max_automation_points_per_lane ||
      lane.timebase > project::AutomationTimebase::elapsed || !std::isfinite(lane.domain.minimum) ||
      !std::isfinite(lane.domain.maximum) || !std::isfinite(lane.domain.default_value) ||
      lane.domain.minimum >= lane.domain.maximum ||
      lane.domain.default_value < lane.domain.minimum ||
      lane.domain.default_value > lane.domain.maximum) {
    return Result<void>::failure(invalid("invalid automation lane"));
  }
  if (const auto* control = std::get_if<project::TrackControlTarget>(&lane.target)) {
    const auto expected = project::channel_control_domain(control->key);
    if (control->track != track.id || lane.domain.semantic_key != expected.semantic_key ||
        lane.domain.representation != project::AutomationRepresentation::physical ||
        lane.domain.minimum != expected.minimum || lane.domain.maximum != expected.maximum ||
        lane.domain.discrete != expected.discrete ||
        lane.domain.step_count != expected.step_count) {
      return Result<void>::failure(invalid("automation target does not match track control"));
    }
    return Result<void>::success();
  }
  if (const auto* native = std::get_if<project::NativeParameterTarget>(&lane.target)) {
    // Native lanes may be stepped (ADR-0034); the model validates the domain.
    if (native->track != track.id || lane.domain.semantic_key != native->parameter_id ||
        lane.domain.representation != project::AutomationRepresentation::physical) {
      return Result<void>::failure(invalid("automation target does not match native parameter"));
    }
    if (track.kind != project::TrackKind::instrument ||
        !std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source)) {
      return Result<void>::failure(
          {ErrorCode::unavailable, "native parameter target has no built-in instrument"});
    }
    return Result<void>::success();
  }
  if (const auto* effect = std::get_if<project::NativeEffectParameterTarget>(&lane.target)) {
    if (lane.domain.semantic_key != effect->parameter_id ||
        lane.domain.representation != project::AutomationRepresentation::physical) {
      return Result<void>::failure(invalid("automation target does not match effect parameter"));
    }
    const auto slot = std::ranges::find(track.effects, effect->slot, &project::EffectInsert::id);
    if (slot == track.effects.end() ||
        !std::holds_alternative<project::NativeEffectPatch>(slot->processor)) {
      return Result<void>::failure(
          {ErrorCode::unavailable, "effect parameter target has no built-in effect"});
    }
    return Result<void>::success();
  }
  if (const auto* macro = std::get_if<project::MacroTarget>(&lane.target)) {
    if (macro->track != track.id || macro->index >= track.macros.size() ||
        lane.domain.representation != project::AutomationRepresentation::physical ||
        lane.domain.minimum != 0.0 || lane.domain.maximum != 1.0 || lane.domain.discrete) {
      return Result<void>::failure(invalid("automation target does not match a macro"));
    }
    return Result<void>::success();
  }
  if (const auto* send_target = std::get_if<project::SendGainTarget>(&lane.target)) {
    if (std::ranges::none_of(
            track.sends,
            [&](const project::SendRoute& send) { return send.id == send_target->send; }) ||
        lane.domain.representation != project::AutomationRepresentation::physical ||
        lane.domain.semantic_key != "mixer.send_gain" || lane.domain.minimum != 0.0 ||
        lane.domain.maximum != 4.0 || lane.domain.discrete || lane.domain.step_count != 0) {
      return Result<void>::failure(invalid("automation target does not match send gain"));
    }
    return Result<void>::success();
  }
  const auto& parameter = std::get<project::Vst3ParameterTarget>(lane.target);
  const auto effect = std::ranges::find_if(track.effects, [&](const auto& candidate) {
    const auto* saved = std::get_if<project::Vst3PluginInstance>(&candidate.processor);
    return saved != nullptr && saved->id == parameter.plugin;
  });
  const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
  const bool source_target = effect == track.effects.end() &&
                             track.kind == project::TrackKind::instrument && source != nullptr &&
                             source->id == parameter.plugin;
  const auto* plugin = effect == track.effects.end()
                           ? (source_target ? source : nullptr)
                           : std::get_if<project::Vst3PluginInstance>(&effect->processor);
  if (plugin == nullptr || (effect != track.effects.end() && !effect->enabled)) {
    return Result<void>::failure(
        {ErrorCode::unavailable, "VST3 automation target is missing or unavailable"});
  }
  if (lane.domain.representation != project::AutomationRepresentation::vst3_normalized ||
      lane.domain.minimum != 0.0 || lane.domain.maximum != 1.0 || lane.domain.discrete ||
      lane.domain.step_count != 0)
    return Result<void>::failure(
        invalid("VST3 automation target needs a continuous normalized domain"));
  return Result<void>::success();
}

struct TargetLess {
  [[nodiscard]] bool operator()(const project::AutomationTarget& left,
                                const project::AutomationTarget& right) const {
    if (left.index() != right.index()) {
      return left.index() < right.index();
    }
    if (const auto* left_control = std::get_if<project::TrackControlTarget>(&left)) {
      const auto& right_control = std::get<project::TrackControlTarget>(right);
      return left_control->track != right_control.track ? left_control->track < right_control.track
                                                        : left_control->key < right_control.key;
    }
    if (const auto* left_send = std::get_if<project::SendGainTarget>(&left)) {
      return left_send->send < std::get<project::SendGainTarget>(right).send;
    }
    if (const auto* left_native = std::get_if<project::NativeParameterTarget>(&left)) {
      const auto& right_native = std::get<project::NativeParameterTarget>(right);
      return left_native->track != right_native.track
                 ? left_native->track < right_native.track
                 : left_native->parameter_id < right_native.parameter_id;
    }
    if (const auto* left_effect = std::get_if<project::NativeEffectParameterTarget>(&left)) {
      const auto& right_effect = std::get<project::NativeEffectParameterTarget>(right);
      return left_effect->slot != right_effect.slot
                 ? left_effect->slot < right_effect.slot
                 : left_effect->parameter_id < right_effect.parameter_id;
    }
    if (const auto* left_macro = std::get_if<project::MacroTarget>(&left)) {
      const auto& right_macro = std::get<project::MacroTarget>(right);
      return left_macro->track != right_macro.track ? left_macro->track < right_macro.track
                                                    : left_macro->index < right_macro.index;
    }
    const auto& left_parameter = std::get<project::Vst3ParameterTarget>(left);
    const auto& right_parameter = std::get<project::Vst3ParameterTarget>(right);
    return left_parameter.plugin != right_parameter.plugin
               ? left_parameter.plugin < right_parameter.plugin
               : left_parameter.parameter_id < right_parameter.parameter_id;
  }
};

struct ClipLaneRef {
  const project::AutomationLane* lane;
  project::ClipId clip;
  const project::Placement* placement;
};

struct LaneGroup {
  const project::AutomationLane* track_lane = nullptr;
  std::vector<ClipLaneRef> clips;
};

class SemanticKey {
public:
  void add(std::uint64_t value) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) {
      value_ ^= value & 0xffU;
      value_ *= 1099511628211ULL;
      value >>= 8U;
    }
  }
  void add(std::string_view value) noexcept {
    add(value.size());
    for (const auto character : value) {
      value_ ^= static_cast<unsigned char>(character);
      value_ *= 1099511628211ULL;
    }
  }
  void add(foundation::IdBits bits) noexcept {
    add(bits.high);
    add(bits.low);
  }
  void add_double(double value) noexcept { add(std::bit_cast<std::uint64_t>(value)); }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_ == 0 ? 1 : value_; }

private:
  std::uint64_t value_{14695981039346656037ULL};
};

void add_lane_key(SemanticKey& key, const project::AutomationLane& lane) noexcept {
  key.add(lane.id.bits());
  key.add(lane.settings_generation);
  key.add(static_cast<std::uint64_t>(lane.timebase));
  key.add(static_cast<std::uint64_t>(lane.domain.representation));
  key.add(lane.domain.semantic_key);
  key.add(lane.domain.unit);
  key.add_double(lane.domain.minimum);
  key.add_double(lane.domain.maximum);
  key.add_double(lane.domain.default_value);
  key.add(lane.domain.discrete ? 1U : 0U);
  key.add(lane.domain.step_count);
  key.add(lane.points.size());
  for (const auto& point : lane.points) {
    key.add(point.id.bits());
    key.add(static_cast<std::uint64_t>(point.coordinate));
    key.add_double(point.value);
    key.add(static_cast<std::uint64_t>(point.to_next));
    key.add_double(point.handles.x1);
    key.add_double(point.handles.y1);
    key.add_double(point.handles.x2);
    key.add_double(point.handles.y2);
  }
}

[[nodiscard]] automation::Warp warp_of(project::AutomationScale scale) noexcept {
  return scale == project::AutomationScale::decibel       ? automation::Warp::decibel
         : scale == project::AutomationScale::logarithmic ? automation::Warp::logarithmic
                                                          : automation::Warp::linear;
}

[[nodiscard]] double mapped_value(const MacroValueMapping& mapping, double value) noexcept {
  auto mapped = automation::blend(mapping.warp, mapping.minimum, mapping.maximum,
                                  std::clamp(value, 0.0, 1.0));
  const auto span = mapping.domain_maximum - mapping.domain_minimum;
  // Steps need a finite, positive span (bounds near the double range overflow it).
  if (mapping.steps != 0 && std::isfinite(span) && span > 0.0) {
    mapped =
        mapping.domain_minimum +
        std::round((mapped - mapping.domain_minimum) / span * mapping.steps) * span / mapping.steps;
  }
  return std::clamp(mapped, mapping.domain_minimum, mapping.domain_maximum);
}

} // namespace

Result<std::vector<PreparedTargetAutomation>>
prepare_project_automation(const project::Snapshot& snapshot, std::uint64_t expected_revision) {
  if (expected_revision == 0 || snapshot.revision != expected_revision) {
    return Result<std::vector<PreparedTargetAutomation>>::failure(
        {ErrorCode::conflict, "automation snapshot revision is stale"});
  }
  if (!timeline::supported_sample_rate(snapshot.sample_rate)) {
    return Result<std::vector<PreparedTargetAutomation>>::failure(
        invalid("invalid automation project sample rate"));
  }
  auto tempo = std::make_shared<const timeline::TempoMap>(snapshot.tempo);
  std::vector<PreparedTargetAutomation> result;
  for (const auto& track : snapshot.tracks) {
    // One ordered index replaces a scan of every lane and clip for each target.
    // Preparation is O(lanes log targets), outside the callback.
    std::map<project::AutomationTarget, LaneGroup, TargetLess> groups;
    // Parameters a macro drives; each has one driver and no lane of its own.
    std::set<project::AutomationTarget, TargetLess> macro_driven;
    for (const auto& lane : track.automation) {
      auto& group = groups[lane.target];
      if (group.track_lane) {
        return Result<std::vector<PreparedTargetAutomation>>::failure(
            invalid("duplicate track automation target"));
      }
      group.track_lane = &lane;
    }
    for (const auto& clip : track.clips) {
      for (const auto& lane : clip.automation) {
        const auto* control = std::get_if<project::TrackControlTarget>(&lane.target);
        if (!std::holds_alternative<project::SendGainTarget>(lane.target) &&
            !std::holds_alternative<project::Vst3ParameterTarget>(lane.target) &&
            !std::holds_alternative<project::NativeEffectParameterTarget>(lane.target) &&
            !std::holds_alternative<project::MacroTarget>(lane.target) &&
            (control == nullptr || !project::is_channel_control(control->key))) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(
              {ErrorCode::unsupported, "audio clip automation has no backed processor target"});
        }
        groups[lane.target].clips.push_back({&lane, clip.id, &clip.placement});
      }
    }
    for (const auto& clip : track.midi_clips) {
      for (const auto& lane : clip.automation) {
        groups[lane.target].clips.push_back({&lane, clip.id, &clip.placement});
      }
    }
    for (const auto& [target, group] : groups) {
      const auto* track_lane = group.track_lane;
      const auto* domain_lane = track_lane ? track_lane : group.clips.front().lane;
      auto binding = validate_target(track, *domain_lane);
      if (!binding) {
        return Result<std::vector<PreparedTargetAutomation>>::failure(binding.error());
      }
      // Every other lane on the target is checked as the domain lane is.
      for (const auto& clip : group.clips) {
        if (clip.lane == domain_lane) {
          continue;
        }
        if (auto checked = validate_target(track, *clip.lane); !checked) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(checked.error());
        }
      }
      // Discrete domains evaluate as held steps.
      // Segments blend in the space the lane is drawn in.
      const auto scale = domain_lane->domain.scale;
      automation::ParameterDescriptor descriptor{
          domain_lane->domain.minimum, domain_lane->domain.maximum,
          domain_lane->domain.default_value, domain_lane->domain.discrete,
          scale == project::AutomationScale::decibel       ? automation::Warp::decibel
          : scale == project::AutomationScale::logarithmic ? automation::Warp::logarithmic
                                                           : automation::Warp::linear};
      // Persisted descriptor defaults describe the parameter. The backing
      // control supplies the current unautomated value.
      if (std::holds_alternative<project::TrackControlTarget>(target)) {
        const auto& control = std::get<project::TrackControlTarget>(target);
        switch (control.key) {
        case project::TrackControlTarget::Key::channel_level:
          descriptor.default_value = track.level;
          break;
        case project::TrackControlTarget::Key::channel_pan:
          descriptor.default_value = track.pan;
          break;
        case project::TrackControlTarget::Key::channel_mute:
          descriptor.default_value = track.muted ? 1.0 : 0.0;
          break;
        }
      } else if (const auto* send_target = std::get_if<project::SendGainTarget>(&target)) {
        const auto send = std::ranges::find_if(track.sends, [&](const project::SendRoute& route) {
          return route.id == send_target->send;
        });
        if (send == track.sends.end()) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(
              {ErrorCode::unavailable, "send gain automation route is missing"});
        }
        descriptor.default_value = send->gain;
      } else if (const auto* native = std::get_if<project::NativeParameterTarget>(&target)) {
        // Sparse patches omit default values; the lane domain carries that
        // default, so an absent id keeps descriptor.default_value.
        const auto& patch = std::get<project::NativeInstrumentPatch>(track.instrument_source);
        const auto saved = std::ranges::find(patch.parameters, native->parameter_id,
                                             &project::NativeInstrumentParameter::id);
        if (saved != patch.parameters.end()) {
          if (saved->value < descriptor.minimum || saved->value > descriptor.maximum) {
            return Result<std::vector<PreparedTargetAutomation>>::failure(
                invalid("native parameter value is outside its automation domain"));
          }
          descriptor.default_value = saved->value;
        }
      } else if (const auto* effect_target =
                     std::get_if<project::NativeEffectParameterTarget>(&target)) {
        // As for instruments: an absent sparse id keeps the domain default.
        const auto slot =
            std::ranges::find(track.effects, effect_target->slot, &project::EffectInsert::id);
        const auto& patch = std::get<project::NativeEffectPatch>(slot->processor);
        const auto saved = std::ranges::find(patch.parameters, effect_target->parameter_id,
                                             &project::NativeInstrumentParameter::id);
        if (saved != patch.parameters.end()) {
          if (saved->value < descriptor.minimum || saved->value > descriptor.maximum) {
            return Result<std::vector<PreparedTargetAutomation>>::failure(
                invalid("effect parameter value is outside its automation domain"));
          }
          descriptor.default_value = saved->value;
        }
      } else if (const auto* macro = std::get_if<project::MacroTarget>(&target)) {
        descriptor.default_value = track.macros[macro->index].value;
      } else if (const auto* parameter = std::get_if<project::Vst3ParameterTarget>(&target)) {
        const auto effect = std::ranges::find_if(track.effects, [&](const auto& candidate) {
          const auto* saved = std::get_if<project::Vst3PluginInstance>(&candidate.processor);
          return saved != nullptr && saved->id == parameter->plugin;
        });
        const auto* plugin =
            effect == track.effects.end()
                ? std::get_if<project::Vst3PluginInstance>(&track.instrument_source)
                : std::get_if<project::Vst3PluginInstance>(&effect->processor);
        if (plugin == nullptr || plugin->id != parameter->plugin) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(
              {ErrorCode::unavailable, "VST3 automation source is missing"});
        }
        const auto reference = std::ranges::find(plugin->parameters, parameter->parameter_id,
                                                 &project::Vst3ParameterReference::id);
        if (reference != plugin->parameters.end())
          descriptor.default_value = reference->normalized_value;
      }
      std::optional<automation::Curve> track_curve;
      if (track_lane) {
        auto curve = absolute_curve(*track_lane, 0, *tempo, snapshot.sample_rate);
        if (!curve) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(curve.error());
        }
        track_curve = std::move(curve.value());
      }
      auto baseline = automation::PreparedAutomation::create(descriptor, track_curve, std::nullopt,
                                                             tempo, snapshot.sample_rate);
      if (!baseline) {
        return Result<std::vector<PreparedTargetAutomation>>::failure(baseline.error());
      }
      struct OrderedClip {
        ClipLaneRef ref;
        Interval interval;
      };
      std::vector<OrderedClip> ordered;
      ordered.reserve(group.clips.size());
      for (const auto& clip : group.clips) {
        const auto& lane = *clip.lane;
        if (lane.domain != domain_lane->domain) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(
              invalid("automation target domains differ"));
        }
        auto interval = clip_interval(*clip.placement, *tempo, snapshot.sample_rate);
        if (!interval) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(interval.error());
        }
        ordered.push_back({clip, interval.value()});
      }
      std::ranges::sort(ordered, {}, [](const OrderedClip& clip) { return clip.interval.begin; });
      for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (ordered[index].interval.begin < ordered[index - 1].interval.end) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(
              {ErrorCode::conflict, "automation clips overlap on one target"});
        }
      }
      std::vector<PreparedTargetAutomation::ClipRegion> clips;
      clips.reserve(ordered.size());
      for (const auto& clip : ordered) {
        const auto& lane = *clip.ref.lane;
        auto absolute = absolute_curve(lane,
                                       lane.timebase == project::AutomationTimebase::musical
                                           ? clip.interval.tick_anchor
                                           : clip.interval.begin.value,
                                       *tempo, snapshot.sample_rate);
        if (!absolute) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(absolute.error());
        }
        automation::ClipEnvelope envelope{std::move(absolute.value()), clip.interval.begin,
                                          clip.interval.end};
        auto prepared = automation::PreparedAutomation::create(
            descriptor, std::nullopt, std::move(envelope), tempo, snapshot.sample_rate);
        if (!prepared) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(prepared.error());
        }
        clips.push_back(
            {clip.ref.clip, clip.interval.begin, clip.interval.end, std::move(prepared.value())});
      }
      SemanticKey key;
      key.add(track.id.bits());
      key.add(target.index());
      if (const auto* control = std::get_if<project::TrackControlTarget>(&target)) {
        key.add(control->track.bits());
        key.add(static_cast<std::uint64_t>(control->key));
      } else if (const auto* send = std::get_if<project::SendGainTarget>(&target)) {
        key.add(send->send.bits());
      } else if (const auto* native = std::get_if<project::NativeParameterTarget>(&target)) {
        key.add(native->track.bits());
        key.add(std::string_view(native->parameter_id));
      } else if (const auto* effect = std::get_if<project::NativeEffectParameterTarget>(&target)) {
        key.add(effect->slot.bits());
        key.add(std::string_view(effect->parameter_id));
      } else if (const auto* macro = std::get_if<project::MacroTarget>(&target)) {
        key.add(macro->track.bits());
        key.add(std::uint64_t{macro->index});
      } else {
        const auto& parameter = std::get<project::Vst3ParameterTarget>(target);
        key.add(parameter.plugin.bits());
        key.add(parameter.parameter_id);
      }
      key.add(track_lane ? 1U : 0U);
      if (track_lane)
        add_lane_key(key, *track_lane);
      key.add(ordered.size());
      for (const auto& clip : ordered) {
        key.add(clip.ref.clip.bits());
        key.add(static_cast<std::uint64_t>(clip.interval.begin.value));
        key.add(static_cast<std::uint64_t>(clip.interval.end.value));
        key.add(static_cast<std::uint64_t>(clip.interval.tick_anchor));
        add_lane_key(key, *clip.ref.lane);
      }
      const auto* macro = std::get_if<project::MacroTarget>(&target);
      if (macro == nullptr) {
        result.push_back(PreparedTargetAutomation(track.id, target, snapshot,
                                                  std::move(baseline.value()), std::move(clips),
                                                  key.value()));
        continue;
      }
      // A macro lane drives each mapped parameter (ADR-0059). The model keeps
      // a mapped parameter free of lanes of its own, so each target appears
      // once in the result.
      for (const auto& mapping : track.macros[macro->index].mappings) {
        const auto& domain = mapping.domain;
        const MacroValueMapping mapped{.warp = warp_of(domain.scale),
                                       .minimum = mapping.minimum,
                                       .maximum = mapping.maximum,
                                       .domain_minimum = domain.minimum,
                                       .domain_maximum = domain.maximum,
                                       .steps = domain.discrete ? domain.step_count : 0U};
        auto mapping_key = key;
        auto destination =
            std::visit([](const auto& item) -> project::AutomationTarget { return item; },
                       mapping.destination);
        // A destination is on this track: its instrument, or one of its
        // built-in effects or note effects.
        const bool owned = std::visit(
            [&](const auto& item) {
              using Destination = std::decay_t<decltype(item)>;
              if constexpr (std::is_same_v<Destination, project::NativeParameterTarget>) {
                return item.track == track.id;
              } else {
                return std::ranges::any_of(track.effects,
                                           [&](const project::EffectInsert& slot) {
                                             return slot.id == item.slot;
                                           }) ||
                       std::ranges::any_of(track.note_effects,
                                           [&](const project::NoteEffectInsert& slot) {
                                             return slot.id == item.slot;
                                           });
              }
            },
            mapping.destination);
        if (!owned) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(
              invalid("macro mapping names no parameter on its track"));
        }
        if (groups.contains(destination) || !macro_driven.insert(destination).second) {
          return Result<std::vector<PreparedTargetAutomation>>::failure(
              {ErrorCode::conflict, "a macro-mapped parameter has another automation source"});
        }
        mapping_key.add(destination.index());
        if (const auto* native = std::get_if<project::NativeParameterTarget>(&destination)) {
          mapping_key.add(std::string_view(native->parameter_id));
        } else {
          const auto& effect = std::get<project::NativeEffectParameterTarget>(destination);
          mapping_key.add(effect.slot.bits());
          mapping_key.add(std::string_view(effect.parameter_id));
        }
        mapping_key.add(static_cast<std::uint64_t>(mapped.warp));
        mapping_key.add_double(mapped.minimum);
        mapping_key.add_double(mapped.maximum);
        mapping_key.add_double(mapped.domain_minimum);
        mapping_key.add_double(mapped.domain_maximum);
        mapping_key.add(std::uint64_t{mapped.steps});
        result.push_back(PreparedTargetAutomation(track.id, std::move(destination), snapshot,
                                                  baseline.value(), clips, mapping_key.value(),
                                                  mapped));
      }
    }
  }
  return Result<std::vector<PreparedTargetAutomation>>::success(std::move(result));
}

double macro_parameter_value(const project::MacroMapping& mapping, double value) noexcept {
  const auto& domain = mapping.domain;
  return mapped_value({.warp = warp_of(domain.scale),
                       .minimum = mapping.minimum,
                       .maximum = mapping.maximum,
                       .domain_minimum = domain.minimum,
                       .domain_maximum = domain.maximum,
                       .steps = domain.discrete ? domain.step_count : 0U},
                      value);
}

void PreparedTargetAutomation::map_macro(std::span<double> output) const noexcept {
  if (!macro_)
    return;
  const auto& macro = *macro_;
  for (auto& value : output)
    value = mapped_value(macro, value);
}

bool PreparedTargetAutomation::evaluate_block(timeline::SamplePosition first,
                                              std::span<const timeline::TickPosition> ticks,
                                              std::span<double> output) const {
  if (!baseline_.evaluate_block(first, ticks, output)) {
    return false;
  }
  const auto end = first.value + static_cast<std::int64_t>(output.size());
  const auto first_clip = std::ranges::partition_point(
      clips_, [first](const ClipRegion& clip) { return clip.end <= first; });
  for (auto current = first_clip; current != clips_.end() && current->begin.value < end;
       ++current) {
    const auto& clip = *current;
    const auto begin = std::max(first.value, clip.begin.value);
    const auto finish = std::min(end, clip.end.value);
    const auto offset = static_cast<std::size_t>(begin - first.value);
    const auto length = static_cast<std::size_t>(finish - begin);
    if (!clip.prepared.evaluate_block({begin}, ticks.subspan(offset, length),
                                      output.subspan(offset, length))) {
      return false;
    }
  }
  map_macro(output);
  return true;
}

bool PreparedTargetAutomation::evaluate_block(timeline::SamplePosition first,
                                              std::span<double> output) const {
  if (!baseline_.evaluate_block(first, output)) {
    return false;
  }
  const auto end = first.value + static_cast<std::int64_t>(output.size());
  const auto first_clip = std::ranges::partition_point(
      clips_, [first](const ClipRegion& clip) { return clip.end <= first; });
  for (auto current = first_clip; current != clips_.end() && current->begin.value < end;
       ++current) {
    const auto& clip = *current;
    const auto begin = std::max(first.value, clip.begin.value);
    const auto finish = std::min(end, clip.end.value);
    const auto offset = static_cast<std::size_t>(begin - first.value);
    const auto length = static_cast<std::size_t>(finish - begin);
    if (!clip.prepared.evaluate_block({begin}, output.subspan(offset, length))) {
      return false;
    }
  }
  map_macro(output);
  return true;
}

} // namespace oscillate::automation_project
