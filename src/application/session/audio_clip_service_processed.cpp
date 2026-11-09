#include "oscillate/application/session/audio_clip_service.hpp"

#include "edit_support.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/project/lookup.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Edits to a processed audio clip (warped, pitched or Clay-edited): split,
// slip and trim keep its processing window, and a move before zero.
namespace oscillate::application {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using project::find_clip;

// Placement length in its own timebase (ticks or project samples).
[[nodiscard]] std::int64_t placement_length(const project::Placement& placement) noexcept {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement))
    return musical->end.value - musical->start.value;
  // A placement always holds one of the two.
  const auto* elapsed = std::get_if<project::ElapsedInterval>(&placement);
  return elapsed != nullptr ? elapsed->end.value - elapsed->start.value : 0;
}

void apply_window(std::vector<project::Operation>& operations, const project::Clip& clip,
                  const ProcessedClipWindow& window) {
  if (window.placement != clip.placement)
    operations.emplace_back(project::SetClipPlacement{clip.id, window.placement});
  if (window.source_range != clip.source_range)
    operations.emplace_back(project::SetClipSourceRange{clip.id, window.source_range});
  if (window.processing != clip.processing)
    operations.emplace_back(project::SetClipProcessing{clip.id, window.processing});
}

[[nodiscard]] Result<void> keeps_markers(const ProcessedClipWindow& window, std::string_view edit) {
  if (window.removed_markers == 0)
    return Result<void>::success();
  return Result<void>::failure(
      {ErrorCode::conflict,
       std::string(edit) + " would leave " + std::to_string(window.removed_markers) +
           " reviewed warp marker(s) outside the clip; move or delete them first"});
}

} // namespace

Result<bool> AudioClipService::audio_clip_is_processed(project::ClipId clip) const {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<bool>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  // A placeholder for a missing source keeps the raw edit rules; its warp
  // map cannot be resolved, so saved processing intent cannot be re-windowed.
  if (std::ranges::find(state.sources, found->source, &project::SourceRecord::id) ==
      state.sources.end()) {
    if (found->processing.requires_rendering())
      return Result<bool>::failure(
          {ErrorCode::unavailable, "Cannot edit a processed clip while its source is missing"});
    return Result<bool>::success(false);
  }
  auto request = processing::processed_clip_request(state, clip);
  if (!request)
    return Result<bool>::failure(request.error());
  return Result<bool>::success(request.value().has_value());
}

Result<project::ClipId> AudioClipService::split_processed_audio_clip(project::ClipId clip,
                                                                     std::int64_t offset,
                                                                     project::ClipId right_id,
                                                                     SplitHalves halves) {
  const auto& left_fades = halves.left.fades;
  const auto& right_fades = halves.right.fades;
  auto& left_automation = halves.left.automation;
  auto& right_automation = halves.right.automation;
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<project::ClipId>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  auto left = rewindow_processed_clip(state, clip, 0, offset, false);
  auto right =
      rewindow_processed_clip(state, clip, offset, placement_length(found->placement), false);
  if (!left || !right)
    return Result<project::ClipId>::failure(!left ? left.error() : right.error());
  std::vector<project::Operation> operations;
  apply_window(operations, *found, left.value());
  if (left_fades != found->fades)
    operations.emplace_back(project::SetClipFades{clip, left_fades});
  for (std::size_t index = 0; index < found->automation.size(); ++index) {
    auto& lane = left_automation[index];
    if (lane != found->automation[index])
      operations.emplace_back(project::ReplaceAutomationLane{lane.id, std::move(lane)});
  }
  project::Clip copy = *found;
  copy.id = right_id;
  copy.placement = right.value().placement;
  copy.source_range = right.value().source_range;
  copy.processing = std::move(right.value().processing);
  copy.fades = right_fades;
  copy.automation = std::move(right_automation);
  const auto& track = *std::ranges::find_if(state.tracks, [clip](const auto& item) {
    return std::ranges::find(item.clips, clip, &project::Clip::id) != item.clips.end();
  });
  operations.emplace_back(project::AddClip{track.id, std::move(copy)});
  auto applied = model().apply(operations);
  if (!applied)
    return Result<project::ClipId>::failure(applied.error());
  return Result<project::ClipId>::success(right_id);
}

Result<void> AudioClipService::slip_processed_audio_clip(project::ClipId clip,
                                                         std::int64_t source_frame_delta) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  auto offset = processed_slip_offset(state, clip, source_frame_delta);
  if (!offset)
    return Result<void>::failure(offset.error());
  if (offset.value() == 0)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Slip is smaller than one step of this clip's timeline"});
  auto window = rewindow_processed_clip(state, clip, offset.value(),
                                        offset.value() + placement_length(found->placement), true);
  if (!window)
    return Result<void>::failure(window.error());
  if (auto kept = keeps_markers(window.value(), "Slip"); !kept)
    return kept;
  std::vector<project::Operation> operations;
  apply_window(operations, *found, window.value());
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

Result<void> AudioClipService::trim_processed_audio_clip(project::ClipId clip, AudioClipEdge edge,
                                                         timeline::SamplePosition new_edge) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  std::int64_t position = 0;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
    auto tick = state.tempo.sample_to_tick(new_edge, state.sample_rate);
    if (!tick)
      return Result<void>::failure(tick.error());
    auto round_trip = state.tempo.tick_to_sample(tick.value(), state.sample_rate);
    if (!round_trip || round_trip.value() != new_edge)
      return Result<void>::failure(
          {ErrorCode::invalid_argument,
           "Musical audio trim edge must align exactly with a tempo-map tick"});
    position = tick.value().value - musical->start.value;
  } else {
    position = new_edge.value - std::get<project::ElapsedInterval>(found->placement).start.value;
  }
  const auto length = placement_length(found->placement);
  if ((edge == AudioClipEdge::left && position == 0) ||
      (edge == AudioClipEdge::right && position == length))
    return Result<void>::success();
  auto window = edge == AudioClipEdge::left
                    ? rewindow_processed_clip(state, clip, position, length, false)
                    : rewindow_processed_clip(state, clip, 0, position, false);
  if (!window)
    return Result<void>::failure(window.error());
  if (auto kept = keeps_markers(window.value(), "Trim"); !kept)
    return kept;
  std::vector<project::Operation> operations;
  apply_window(operations, *found, window.value());
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

Result<void> AudioClipService::move_audio_clip_before_zero(project::ClipId clip,
                                                           timeline::TickPosition start) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  const auto bar = opening_bar_ticks(state);
  if (start.value >= 0 || bar <= 0)
    return Result<void>::failure({ErrorCode::invalid_argument, "The clip must start before bar 1"});
  // The distance before bar 1, without negating INT64_MIN, checked as a wide
  // count before it narrows.
  const auto ahead = static_cast<std::uint64_t>(-(start.value + 1)) + 1;
  const auto wide_bars = ahead / static_cast<std::uint64_t>(bar) +
                         (ahead % static_cast<std::uint64_t>(bar) != 0 ? 1 : 0);
  if (wide_bars > project::max_lead_bars)
    return Result<void>::failure({ErrorCode::invalid_argument, "Too many bars before bar 1"});
  const auto bars = static_cast<std::uint32_t>(wide_bars);
  std::vector<project::Operation> operations;
  const std::array placed{clip};
  auto shift = lead_bar_operations(state, bars, placed, operations);
  if (!shift)
    return Result<void>::failure(shift.error());
  const auto moved = start.value + shift.value().ticks;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
    operations.emplace_back(project::SetClipPlacement{
        clip,
        project::MusicalInterval{{moved}, {moved + musical->end.value - musical->start.value}}});
  } else {
    // Inside the new bars the opening tempo holds, so time is proportional.
    const auto& elapsed = std::get<project::ElapsedInterval>(found->placement);
    const auto sample = std::llround(static_cast<long double>(moved) * shift.value().samples /
                                     static_cast<long double>(shift.value().ticks));
    operations.emplace_back(project::SetClipPlacement{
        clip,
        project::ElapsedInterval{{sample}, {sample + elapsed.end.value - elapsed.start.value}}});
  }
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

} // namespace oscillate::application
