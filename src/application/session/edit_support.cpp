#include "edit_support.hpp"

#include "midi_clips.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/automation/curve.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/project/lookup.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
using project::find_track;
template <typename T> using Result = foundation::Result<T>;
using project::find_clip;
using Follow = std::pair<const project::Clip*, ClipWarpProposal>;

struct AutomationSegmentHalf {
  project::AutomationShape shape;
  project::AutomationHandles handles;
};

struct SubdividedAutomationSegment {
  double value;
  AutomationSegmentHalf left;
  AutomationSegmentHalf right;
};

struct CubicControl {
  double x;
  double y;
};

[[nodiscard]] CubicControl interpolate(CubicControl first, CubicControl second, double fraction) {
  return {std::lerp(first.x, second.x, fraction), std::lerp(first.y, second.y, fraction)};
}

[[nodiscard]] double cubic_coordinate(double t, double first, double second) {
  const double remaining = 1.0 - t;
  return 3.0 * remaining * remaining * t * first + 3.0 * remaining * t * t * second + t * t * t;
}

// The shared evaluator solves Bézier X with 24 bisections. Use the same
// parameter at the cut; de Casteljau then preserves both cubic subcurves.
[[nodiscard]] double bezier_parameter_at(double x, const project::AutomationHandles& handles) {
  double low = 0.0;
  double high = 1.0;
  for (int iteration = 0; iteration < 24; ++iteration) {
    const double middle = (low + high) * 0.5;
    if (cubic_coordinate(middle, handles.x1, handles.x2) < x)
      low = middle;
    else
      high = middle;
  }
  return (low + high) * 0.5;
}

[[nodiscard]] automation::Warp warp_of(project::AutomationScale scale) noexcept {
  return scale == project::AutomationScale::decibel       ? automation::Warp::decibel
         : scale == project::AutomationScale::logarithmic ? automation::Warp::logarithmic
                                                          : automation::Warp::linear;
}

// The cut's value blends in the lane's scale, as playback does, so both
// halves keep the segment's exact path.
[[nodiscard]] foundation::Result<SubdividedAutomationSegment>
subdivide_automation_segment(const project::AutomationPoint& first,
                             const project::AutomationPoint& last, double fraction,
                             project::AutomationScale scale) {
  const auto warp = warp_of(scale);
  using foundation::ErrorCode;
  using Result = foundation::Result<SubdividedAutomationSegment>;
  const auto shape = first.to_next;
  if (shape == project::AutomationShape::hold || first.value == last.value) {
    return Result::success(
        {first.value, {project::AutomationShape::hold, {}}, {project::AutomationShape::hold, {}}});
  }
  if (shape == project::AutomationShape::linear) {
    return Result::success({automation::blend(warp, first.value, last.value, fraction),
                            {project::AutomationShape::linear, first.handles},
                            {project::AutomationShape::linear, first.handles}});
  }
  if (shape == project::AutomationShape::exponential) {
    return Result::failure(
        {ErrorCode::unsupported,
         "Split inside exponential automation needs a saved curvature parameter"});
  }

  project::AutomationHandles handles = first.handles;
  if (shape == project::AutomationShape::quadratic) {
    handles = {1.0 / 3.0, 0.0, 2.0 / 3.0, 1.0 / 3.0};
  } else if (shape == project::AutomationShape::ease_in_out) {
    handles = {1.0 / 3.0, 0.0, 2.0 / 3.0, 1.0};
  }
  if (shape != project::AutomationShape::quadratic &&
      shape != project::AutomationShape::ease_in_out && shape != project::AutomationShape::bezier) {
    return Result::failure({ErrorCode::unsupported, "Unsupported automation curve at split"});
  }
  const double t =
      shape == project::AutomationShape::bezier ? bezier_parameter_at(fraction, handles) : fraction;
  const CubicControl a{0.0, 0.0};
  const CubicControl b{handles.x1, handles.y1};
  const CubicControl c{handles.x2, handles.y2};
  const CubicControl d{1.0, 1.0};
  const auto ab = interpolate(a, b, t);
  const auto bc = interpolate(b, c, t);
  const auto cd = interpolate(c, d, t);
  const auto abc = interpolate(ab, bc, t);
  const auto bcd = interpolate(bc, cd, t);
  const auto cut = interpolate(abc, bcd, t);
  if (!(cut.x > 0.0 && cut.x < 1.0 && cut.y > 0.0 && cut.y < 1.0)) {
    return Result::failure(
        {ErrorCode::unsupported, "Automation curve is numerically degenerate at split"});
  }
  auto normalized = [](const CubicControl& p, const CubicControl& start, const CubicControl& end) {
    return CubicControl{(p.x - start.x) / (end.x - start.x), (p.y - start.y) / (end.y - start.y)};
  };
  const auto left_b = normalized(ab, a, cut);
  const auto left_c = normalized(abc, a, cut);
  const auto right_b = normalized(bcd, cut, d);
  const auto right_c = normalized(cd, cut, d);
  const project::AutomationHandles left_handles{left_b.x, left_b.y, left_c.x, left_c.y};
  const project::AutomationHandles right_handles{right_b.x, right_b.y, right_c.x, right_c.y};
  auto valid = [](const project::AutomationHandles& h) {
    return std::isfinite(h.x1) && std::isfinite(h.y1) && std::isfinite(h.x2) &&
           std::isfinite(h.y2) && 0.0 <= h.x1 && h.x1 <= h.x2 && h.x2 <= 1.0 && 0.0 <= h.y1 &&
           h.y1 <= h.y2 && h.y2 <= 1.0;
  };
  if (!valid(left_handles) || !valid(right_handles)) {
    return Result::failure(
        {ErrorCode::unsupported, "Automation curve cannot be normalized exactly at split"});
  }
  return Result::success({automation::blend(warp, first.value, last.value, cut.y),
                          {project::AutomationShape::bezier, left_handles},
                          {project::AutomationShape::bezier, right_handles}});
}

} // namespace

[[nodiscard]] project::Vst3PluginInstance make_vst3_instance(project::PluginInstanceId id,
                                                             Vst3InsertIdentity identity) {
  return {.id = id,
          .class_id = std::move(identity.class_id),
          .vendor = std::move(identity.vendor),
          .name = std::move(identity.name),
          .version = std::move(identity.version),
          .bundle_path = std::move(identity.canonical_bundle_path),
          .binary_sha256 = std::move(identity.binary_sha256),
          .component_state = {},
          .parameters = {},
          .missing = false,
          .controller_state = std::nullopt};
}

[[nodiscard]] std::uint64_t next_number(std::uint64_t greatest) {
  return greatest == std::numeric_limits<std::uint64_t>::max() ? 0 : greatest + 1;
}

[[nodiscard]] const project::AutomationLane* find_lane(const project::Track& track,
                                                       const project::AutomationTarget& target) {
  const auto lane = std::ranges::find_if(
      track.automation, [&target](const auto& candidate) { return candidate.target == target; });
  return lane == track.automation.end() ? nullptr : &*lane;
}

[[nodiscard]] const project::Vst3PluginInstance*
saved_automatable_plugin(const project::Track& track, project::PluginInstanceId id) {
  if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
      source != nullptr && track.kind == project::TrackKind::instrument && source->id == id)
    return source;
  const auto effect = std::ranges::find_if(track.effects, [id](const auto& candidate) {
    const auto* saved = std::get_if<project::Vst3PluginInstance>(&candidate.processor);
    return candidate.enabled && saved != nullptr && saved->id == id;
  });
  return effect == track.effects.end()
             ? nullptr
             : std::get_if<project::Vst3PluginInstance>(&effect->processor);
}

[[nodiscard]] foundation::Result<std::int64_t> point_coordinate(const project::Snapshot& snapshot,
                                                                const project::AutomationLane& lane,
                                                                timeline::TickPosition tick) {
  if (lane.timebase == project::AutomationTimebase::musical) {
    return foundation::Result<std::int64_t>::success(tick.value);
  }
  auto sample = snapshot.tempo.tick_to_sample(tick, snapshot.sample_rate);
  return sample ? foundation::Result<std::int64_t>::success(sample.value().value)
                : foundation::Result<std::int64_t>::failure(sample.error());
}

[[nodiscard]] foundation::Result<project::Placement>
convert_clip_timebase(const project::Snapshot& state, const project::Placement& placement) {
  using foundation::ErrorCode;
  using Result = foundation::Result<project::Placement>;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    auto start = state.tempo.tick_to_sample(musical->start, state.sample_rate);
    auto end = state.tempo.tick_to_sample(musical->end, state.sample_rate);
    if (!start || !end)
      return Result::failure(!start ? start.error() : end.error());
    return Result::success(project::ElapsedInterval{start.value(), end.value()});
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(placement);
  auto start = state.tempo.sample_to_tick(elapsed.start, state.sample_rate);
  auto end = state.tempo.sample_to_tick(elapsed.end, state.sample_rate);
  if (!start || !end)
    return Result::failure(!start ? start.error() : end.error());
  auto start_sample = state.tempo.tick_to_sample(start.value(), state.sample_rate);
  auto end_sample = state.tempo.tick_to_sample(end.value(), state.sample_rate);
  if (!start_sample || !end_sample || start_sample.value() != elapsed.start ||
      end_sample.value() != elapsed.end)
    return Result::failure(
        {ErrorCode::unsupported, "Clip boundaries cannot be preserved on musical ticks"});
  return Result::success(project::MusicalInterval{start.value(), end.value()});
}

[[nodiscard]] foundation::Result<void>
assign_fresh_automation_ids(std::vector<project::AutomationLane>& lanes,
                            std::uint64_t current_revision) {
  if (lanes.empty())
    return foundation::Result<void>::success();
  // One model command creates one revision. Its revision-derived ID namespace
  // remains retired after undo, even when a copy adds many points in one step.
  const auto namespace_id = next_number(current_revision);
  if (namespace_id == 0) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::resource_exhausted, "Project automation ID space exhausted"});
  }
  std::uint64_t next_lane = 1;
  std::uint64_t next_point = 1;
  for (auto& lane : lanes) {
    lane.id = project::AutomationLaneId::from_bits({namespace_id, next_lane++}).value();
    for (auto& point : lane.points) {
      point.id = project::AutomationPointId::from_bits({namespace_id, next_point++}).value();
    }
  }
  return foundation::Result<void>::success();
}

[[nodiscard]] foundation::Result<SplitClipAutomation>
split_clip_automation(const project::Snapshot& state,
                      const std::vector<project::AutomationLane>& lanes, std::int64_t tick_offset,
                      std::int64_t sample_offset) {
  using foundation::ErrorCode;
  using Result = foundation::Result<SplitClipAutomation>;
  SplitClipAutomation split;
  if (lanes.empty())
    return Result::success(std::move(split));
  if (sample_offset <= 0 || tick_offset < 0)
    return Result::failure({ErrorCode::invalid_argument, "Invalid clip automation split"});

  split.left = lanes;
  split.right = lanes;
  std::vector<std::size_t> new_left_points;
  for (std::size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const auto& lane = lanes[lane_index];
    const auto offset =
        lane.timebase == project::AutomationTimebase::musical ? tick_offset : sample_offset;
    const auto next =
        std::ranges::lower_bound(lane.points, offset, {}, &project::AutomationPoint::coordinate);
    const auto next_index = static_cast<std::size_t>(next - lane.points.begin());
    const bool exact = next != lane.points.end() && next->coordinate == offset;
    const auto* previous = next_index == 0 ? nullptr : &lane.points[next_index - 1];
    double boundary_value = previous ? previous->value : lane.points.front().value;
    AutomationSegmentHalf left_segment{project::AutomationShape::hold, {}};
    AutomationSegmentHalf right_segment{project::AutomationShape::hold, {}};
    if (previous && next != lane.points.end() && !exact) {
      const auto fraction = static_cast<double>(offset - previous->coordinate) /
                            static_cast<double>(next->coordinate - previous->coordinate);
      auto subdivided = subdivide_automation_segment(*previous, *next, fraction, lane.domain.scale);
      if (!subdivided)
        return Result::failure(subdivided.error());
      boundary_value = subdivided.value().value;
      left_segment = subdivided.value().left;
      right_segment = subdivided.value().right;
    }

    auto& left = split.left[lane_index];
    auto& right = split.right[lane_index];
    left.points.assign(lane.points.begin(), next + static_cast<std::ptrdiff_t>(exact));
    if (previous && next != lane.points.end() && !exact) {
      left.points.back().to_next = left_segment.shape;
      left.points.back().handles = left_segment.handles;
    }
    if (left.points.empty() || (previous && next != lane.points.end() && !exact &&
                                left_segment.shape != project::AutomationShape::hold)) {
      auto point = lane.points.front(); // replaced with a fresh ID below
      point.coordinate = left.points.empty() ? 0 : offset;
      point.value = boundary_value;
      point.to_next = project::AutomationShape::hold;
      left.points.push_back(point);
      new_left_points.push_back(lane_index);
    }
    right.points.assign(next, lane.points.end());
    for (auto& point : right.points)
      point.coordinate -= offset;
    if (!exact && previous) {
      auto point = *previous; // the right copy receives fresh IDs below
      point.coordinate = 0;
      point.value = boundary_value;
      point.to_next = right_segment.shape;
      point.handles = right_segment.handles;
      right.points.insert(right.points.begin(), point);
    }
    if (right.points.empty()) {
      auto point = lane.points.back();
      point.coordinate = 0;
      point.value = boundary_value;
      point.to_next = project::AutomationShape::hold;
      right.points.push_back(point);
    }
    if (left != lane)
      ++left.settings_generation;
  }
  auto fresh = assign_fresh_automation_ids(split.right, state.revision);
  if (!fresh)
    return Result::failure(fresh.error());
  std::uint64_t next_point = 1;
  for (const auto& lane : split.right)
    next_point += lane.points.size();
  const auto namespace_id = next_number(state.revision);
  for (const auto lane_index : new_left_points) {
    split.left[lane_index].points.back().id =
        project::AutomationPointId::from_bits({namespace_id, next_point++}).value();
  }
  return Result::success(std::move(split));
}

foundation::Result<void> change_lane_point_value(project::ProjectModel& model,
                                                 const project::AutomationLane& lane,
                                                 project::AutomationPointId point_id,
                                                 double value) {
  using Result = foundation::Result<void>;
  auto replacement = lane;
  const auto point = std::ranges::find_if(
      replacement.points, [point_id](const auto& item) { return item.id == point_id; });
  if (point == replacement.points.end()) {
    return Result::failure({foundation::ErrorCode::not_found, "Automation point not found"});
  }
  if (point->value == value) {
    return Result::success();
  }
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max()) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Automation lane generation exhausted"});
  }
  point->value = value;
  ++replacement.settings_generation;
  auto applied = model.apply({project::ReplaceAutomationLane{lane.id, std::move(replacement)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> erase_lane_point(project::ProjectModel& model,
                                          const project::AutomationLane& lane,
                                          project::AutomationPointId point_id) {
  using Result = foundation::Result<void>;
  auto replacement = lane;
  if (std::erase_if(replacement.points,
                    [point_id](const auto& item) { return item.id == point_id; }) == 0) {
    return Result::failure({foundation::ErrorCode::not_found, "Automation point not found"});
  }
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max()) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Automation lane generation exhausted"});
  }
  ++replacement.settings_generation;
  auto applied = model.apply({project::ReplaceAutomationLane{lane.id, std::move(replacement)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> erase_lane(project::ProjectModel& model,
                                    const project::AutomationLane& lane) {
  auto applied = model.apply({project::RemoveAutomationLane{lane.id}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<project::Placement> audio_placement_at(const project::Placement& original,
                                                          timeline::TickPosition start,
                                                          const project::Snapshot& snapshot) {
  const foundation::Error invalid{foundation::ErrorCode::invalid_argument,
                                  "Audio clip position is out of range"};
  if (start.value < 0) {
    return foundation::Result<project::Placement>::failure(invalid);
  }
  if (const auto* musical = std::get_if<project::MusicalInterval>(&original)) {
    const auto duration = musical->end.value - musical->start.value;
    if (start.value > std::numeric_limits<std::int64_t>::max() - duration) {
      return foundation::Result<project::Placement>::failure(invalid);
    }
    return foundation::Result<project::Placement>::success(
        project::MusicalInterval{start, {start.value + duration}});
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(original);
  auto sample = snapshot.tempo.tick_to_sample(start, snapshot.sample_rate);
  if (!sample) {
    return foundation::Result<project::Placement>::failure(sample.error());
  }
  const auto duration = elapsed.end.value - elapsed.start.value;
  if (sample.value().value > std::numeric_limits<std::int64_t>::max() - duration) {
    return foundation::Result<project::Placement>::failure(invalid);
  }
  return foundation::Result<project::Placement>::success(
      project::ElapsedInterval{sample.value(), {sample.value().value + duration}});
}

foundation::Result<NativeTrack> native_track(const project::Snapshot& snapshot,
                                             project::TrackId track_id) {
  using Result = foundation::Result<NativeTrack>;
  const auto track = std::ranges::find(snapshot.tracks, track_id, &project::Track::id);
  if (track == snapshot.tracks.end()) {
    return Result::failure({foundation::ErrorCode::not_found, "Synth track not found"});
  }
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track->instrument_source);
  if (track->kind != project::TrackKind::instrument || patch == nullptr) {
    return Result::failure({foundation::ErrorCode::unavailable, "Track has no built-in synth"});
  }
  auto resolved = native_dsp::synth::resolve_native_instrument(*patch);
  if (!resolved) {
    return Result::failure(resolved.error());
  }
  return Result::success({&*track, std::move(resolved).value()});
}

foundation::Result<NativeEffectSlot> native_effect_slot(const project::Snapshot& snapshot,
                                                        const project::SignalPathId& path,
                                                        project::InsertSlotId slot_id) {
  using Result = foundation::Result<NativeEffectSlot>;
  const project::Track* track = nullptr;
  const std::vector<project::EffectInsert>* effects = &snapshot.master_effects;
  if (const auto* track_id = std::get_if<project::TrackId>(&path)) {
    const auto found = std::ranges::find(snapshot.tracks, *track_id, &project::Track::id);
    if (found == snapshot.tracks.end())
      return Result::failure({foundation::ErrorCode::not_found, "Effect track not found"});
    track = &*found;
    effects = &found->effects;
  }
  const auto slot = std::ranges::find(*effects, slot_id, &project::EffectInsert::id);
  if (slot == effects->end())
    return Result::failure({foundation::ErrorCode::not_found, "Effect slot not on signal path"});
  const auto* patch = std::get_if<project::NativeEffectPatch>(&slot->processor);
  if (patch == nullptr)
    return Result::failure({foundation::ErrorCode::unavailable, "Slot is not a built-in effect"});
  auto resolved = native_inserts::resolve_native_effect(*patch);
  if (!resolved)
    return Result::failure(resolved.error());
  return Result::success({track, &*slot, patch, std::move(resolved).value()});
}

// A clip name the model accepts: control characters dropped, cut to the
// limit at a character boundary.
[[nodiscard]] std::string presentable_name(std::string_view name) {
  std::string result;
  for (const char character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 0x20 && byte != 0x7f)
      result.push_back(character);
  }
  if (result.size() > project::max_track_name_bytes) {
    auto cut = project::max_track_name_bytes;
    while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0U) == 0x80U)
      --cut;
    result.resize(cut);
  }
  return result;
}

[[nodiscard]] foundation::Result<FreshIdentities> fresh_identities(std::uint64_t revision) {
  const auto space = next_number(revision);
  if (space == 0)
    return foundation::Result<FreshIdentities>::failure(
        {foundation::ErrorCode::resource_exhausted, "Project ID space exhausted"});
  return foundation::Result<FreshIdentities>::success({space});
}

void refresh_lanes(std::vector<project::AutomationLane>& lanes, FreshIdentities& ids) {
  for (auto& lane : lanes) {
    lane.id = project::AutomationLaneId::from_bits({ids.space, ids.next_lane++}).value();
    for (auto& point : lane.points)
      point.id = project::AutomationPointId::from_bits({ids.space, ids.next_point++}).value();
  }
}

[[nodiscard]] Result<void> done(const Result<project::AppliedRevision>& applied) {
  return applied ? Result<void>::success() : Result<void>::failure(applied.error());
}

// A meter's denominator divides a whole note, which lasts four quarters.
constexpr std::int64_t quarters_per_whole_note = 4;

[[nodiscard]] std::int64_t opening_bar_ticks(const project::Snapshot& state) {
  const auto& first = state.meter.events().front();
  return static_cast<std::int64_t>(first.numerator) * quarters_per_whole_note *
         timeline::ticks_per_quarter / static_cast<std::int64_t>(first.denominator);
}

// The operations that put `bars` more bars before bar 1: everything on the
// timeline moves later by that span at the opening tempo and meter (the
// opening tempo holds through the new bars, so a ramp keeps its timing),
// except the clips in `placed`, which the caller places itself.
[[nodiscard]] Result<LeadShift> lead_bar_operations(const project::Snapshot& state,
                                                    std::uint32_t bars,
                                                    std::span<const project::ClipId> placed,
                                                    std::vector<project::Operation>& operations) {
  if (bars == 0 || state.lead_bars + bars > project::max_lead_bars)
    return Result<LeadShift>::failure({ErrorCode::invalid_argument, "Too many bars before bar 1"});
  LeadShift shift;
  shift.ticks = static_cast<std::int64_t>(bars) * opening_bar_ticks(state);
  const auto& tempo = state.tempo.events();
  std::vector<timeline::TempoEvent> moved_tempo{
      {{0}, tempo.front().bpm, timeline::TempoShape::step}};
  if (tempo.size() > 1 || tempo.front().shape_to_next != timeline::TempoShape::step)
    moved_tempo.push_back({{shift.ticks}, tempo.front().bpm, tempo.front().shape_to_next});
  for (std::size_t index = 1; index < tempo.size(); ++index)
    moved_tempo.push_back(
        {{tempo[index].tick.value + shift.ticks}, tempo[index].bpm, tempo[index].shape_to_next});
  auto map = timeline::TempoMap::create(std::move(moved_tempo), state.tempo.revision() + 1);
  if (!map)
    return Result<LeadShift>::failure(map.error());
  auto samples = map.value().tick_to_sample({shift.ticks}, state.sample_rate);
  if (!samples)
    return Result<LeadShift>::failure(samples.error());
  shift.samples = samples.value().value;
  operations.emplace_back(project::ReplaceTempoMap{std::move(map).value()});
  if (const auto& meter = state.meter.events(); meter.size() > 1) {
    std::vector<timeline::MeterEvent> moved{meter.front()};
    for (std::size_t index = 1; index < meter.size(); ++index)
      moved.push_back({{meter[index].tick.value + shift.ticks},
                       meter[index].numerator,
                       meter[index].denominator});
    auto meters = timeline::MeterMap::create(std::move(moved), state.meter.revision() + 1);
    if (!meters)
      return Result<LeadShift>::failure(meters.error());
    operations.emplace_back(project::ReplaceMeterMap{std::move(meters).value()});
  }
  const auto later = [&](const project::Placement& placement) -> project::Placement {
    if (const auto* musical = std::get_if<project::MusicalInterval>(&placement))
      return project::MusicalInterval{{musical->start.value + shift.ticks},
                                      {musical->end.value + shift.ticks}};
    const auto& elapsed = std::get<project::ElapsedInterval>(placement);
    return project::ElapsedInterval{{elapsed.start.value + shift.samples},
                                    {elapsed.end.value + shift.samples}};
  };
  for (const auto& track : state.tracks) {
    for (const auto& clip : track.clips)
      if (std::ranges::find(placed, clip.id) == placed.end())
        operations.emplace_back(project::SetClipPlacement{clip.id, later(clip.placement)});
    for (const auto& clip : track.midi_clips)
      operations.emplace_back(project::SetMidiClipPlacement{clip.id, later(clip.placement)});
    for (const auto& lane : track.automation) {
      if (lane.points.empty())
        continue;
      auto moved = lane;
      for (auto& point : moved.points)
        point.coordinate +=
            lane.timebase == project::AutomationTimebase::musical ? shift.ticks : shift.samples;
      operations.emplace_back(project::ReplaceAutomationLane{lane.id, std::move(moved)});
    }
  }
  // Last first, so no marker lands on another on the way.
  auto markers = state.markers;
  std::ranges::sort(markers, std::ranges::greater{},
                    [](const project::Marker& marker) { return marker.position.value; });
  for (const auto& marker : markers)
    operations.emplace_back(project::MoveMarker{marker.id, {marker.position.value + shift.ticks}});
  operations.emplace_back(project::SetLeadBars{state.lead_bars + bars});
  return Result<LeadShift>::success(shift);
}

} // namespace oscillate::application
