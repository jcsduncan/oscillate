#include "oscillate/routing/key_mute_lanes.hpp"

#include "oscillate/routing/project_route_plan.hpp"

#include <algorithm>
#include <array>
#include <variant>

namespace oscillate::routing {
namespace {

// Whether `prepared` is the channel-mute lane of one of `tracks`.
[[nodiscard]] bool mute_lane_of(const automation_project::PreparedTargetAutomation& prepared,
                                std::span<const project::TrackId> tracks) {
  const auto* control = std::get_if<project::TrackControlTarget>(&prepared.target());
  return control != nullptr && control->key == project::TrackControlTarget::Key::channel_mute &&
         std::ranges::find(tracks, prepared.track()) != tracks.end();
}

} // namespace

// Prepares the whole project's automation only when a key source has a mute
// lane, and keeps just those lanes.
foundation::Result<KeyMuteLanes> KeyMuteLanes::prepare(const project::Snapshot& snapshot,
                                                       std::span<const project::TrackId> tracks) {
  using R = foundation::Result<KeyMuteLanes>;
  KeyMuteLanes result;
  const bool laned = std::ranges::any_of(snapshot.tracks, [&](const project::Track& track) {
    return std::ranges::find(tracks, track.id) != tracks.end() && has_mute_automation(track);
  });
  if (!laned)
    return R::success(std::move(result));
  auto prepared = automation_project::prepare_project_automation(snapshot, snapshot.revision);
  if (!prepared)
    return R::failure(prepared.error());
  for (auto& lane : prepared.value()) {
    if (mute_lane_of(lane, tracks))
      result.lanes_.push_back(std::move(lane));
  }
  return R::success(std::move(result));
}

// Evaluates the track's lane at one sample, as the mixer's channel gain does.
foundation::Result<bool> KeyMuteLanes::mutes(project::TrackId track,
                                             timeline::SamplePosition sample) const {
  using R = foundation::Result<bool>;
  const auto lane =
      std::ranges::find(lanes_, track, &automation_project::PreparedTargetAutomation::track);
  if (lane == lanes_.end())
    return R::success(false);
  std::array<double, 1> value{};
  if (!lane->evaluate_block(sample, value))
    return R::failure(
        {foundation::ErrorCode::invalid_argument, "A key source's mute lane cannot be evaluated"});
  return R::success(mute_lane_mutes(value.front()));
}

} // namespace oscillate::routing
