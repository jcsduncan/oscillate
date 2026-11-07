#include "oscillate/application/session/warp_service.hpp"

#include "edit_support.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/project/lookup.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
using project::find_clip;
template <typename T> using Result = foundation::Result<T>;
using Follow = std::pair<const project::Clip*, ClipWarpProposal>;

// Placement and processing operations for a proposal, skipping what is
// unchanged.
void append_proposal(std::vector<project::Operation>& operations, const project::Clip& clip,
                     ClipWarpProposal proposal) {
  if (proposal.placement != clip.placement)
    operations.emplace_back(project::SetClipPlacement{clip.id, proposal.placement});
  if (proposal.processing != clip.processing)
    operations.emplace_back(project::SetClipProcessing{clip.id, std::move(proposal.processing)});
}

// Applies `prefix`, then the follow proposals as one edit. A proposal that
// starts before zero moves later by whole bars, so its downbeat lands on the
// first bar that keeps the clip on the timeline. Following never adds bars
// before bar 1: a clip that starts a hair early must not put a silent bar in
// front of the song.
[[nodiscard]] Result<void> apply_follows(project::ProjectModel& model,
                                         const project::Snapshot& state,
                                         std::vector<project::Operation> operations,
                                         std::vector<Follow> follows) {
  const auto bar = opening_bar_ticks(state);
  for (auto& [clip, proposal] : follows) {
    auto* musical = std::get_if<project::MusicalInterval>(&proposal.placement);
    if (musical == nullptr || musical->start.value >= 0)
      continue;
    const auto later = (-musical->start.value + bar - 1) / bar * bar;
    *musical = {{musical->start.value + later}, {musical->end.value + later}};
  }
  for (auto& [clip, proposal] : follows)
    append_proposal(operations, *clip, std::move(proposal));
  if (operations.empty())
    return Result<void>::success();
  auto applied = model.apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

} // namespace

Result<void> WarpService::apply_audio_clip_warp(project::ClipId clip,
                                                std::uint64_t settings_generation,
                                                project::Placement placement,
                                                project::ClipProcessing processing) {
  const auto* found = find_clip(snapshot(), clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  if (found->settings_generation != settings_generation)
    return Result<void>::failure(
        {ErrorCode::conflict, "The clip changed since this warp proposal was made"});
  std::vector<project::Operation> operations;
  if (placement != found->placement)
    operations.emplace_back(project::SetClipPlacement{clip, placement});
  if (processing != found->processing)
    operations.emplace_back(project::SetClipProcessing{clip, std::move(processing)});
  if (operations.empty())
    return Result<void>::failure({ErrorCode::invalid_argument, "The clip already plays this warp"});
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

Result<void> WarpService::set_audio_clip_follow(project::ClipId clip, bool follow) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  auto proposal = follow ? propose_follow(state, clip) : propose_own_tempo(state, clip);
  if (!proposal)
    return Result<void>::failure(proposal.error());
  std::vector<Follow> follows;
  follows.emplace_back(found, std::move(proposal).value());
  return apply_follows(model(), state, {}, std::move(follows));
}

Result<void> WarpService::set_audio_clip_source_bpm(project::ClipId clip,
                                                    std::optional<double> bpm) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  if (bpm && (!std::isfinite(*bpm) || *bpm < 20.0 || *bpm > 999.0))
    return Result<void>::failure({ErrorCode::invalid_argument, "A tempo must be 20 to 999 BPM"});
  const auto source = found->source;
  std::optional<double> effective = bpm;
  if (!effective) {
    const auto review =
        std::ranges::find(state.source_tempo_reviews, source, &project::SourceTempoReview::source);
    if (review == state.source_tempo_reviews.end() || review->detected_bpm <= 0.0)
      return Result<void>::failure(
          {ErrorCode::unavailable, "There is no detected tempo to go back to"});
    effective = review->detected_bpm;
  }
  std::vector<Follow> follows;
  for (const auto& track : state.tracks) {
    for (const auto& other : track.clips) {
      if (other.source != source ||
          !std::holds_alternative<project::MusicalInterval>(other.placement))
        continue;
      // The downbeat stays where it plays now; the grid rescales around it.
      auto keep = clip_downbeat_tick(state, other.id);
      auto proposal =
          propose_follow(state, other.id, effective, std::nullopt,
                         keep ? std::optional<std::int64_t>(keep.value()) : std::nullopt);
      if (!proposal)
        return Result<void>::failure(proposal.error());
      follows.emplace_back(&other, std::move(proposal).value());
    }
  }
  return apply_follows(model(), state, {project::SetSourceTempoCorrection{source, bpm}},
                       std::move(follows));
}

Result<void> WarpService::set_audio_clip_downbeat(project::ClipId clip,
                                                  std::uint64_t source_frame) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  std::vector<Follow> follows;
  if (std::holds_alternative<project::MusicalInterval>(found->placement)) {
    auto proposal = propose_follow(state, clip, std::nullopt, source_frame);
    if (!proposal)
      return Result<void>::failure(proposal.error());
    follows.emplace_back(found, std::move(proposal).value());
  }
  return apply_follows(model(), state, {project::SetSourceDownbeat{found->source, source_frame}},
                       std::move(follows));
}

} // namespace oscillate::application
