#include "oscillate/application/session/source_analysis_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/project/lookup.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
using project::find_clip;
template <typename T> using Result = foundation::Result<T>;
using Follow = std::pair<const project::Clip*, ClipWarpProposal>;

} // namespace

foundation::Result<void>
SourceAnalysisService::accept_source_tempo_analysis(project::SourceId source_id,
                                                    std::uint64_t origin_revision,
                                                    const analysis::TempoEstimate& estimate) {
  using Result = foundation::Result<void>;
  const auto source = std::ranges::find_if(
      snapshot().sources, [&](const auto& candidate) { return candidate.id == source_id; });
  if (source == snapshot().sources.end()) {
    return Result::failure({foundation::ErrorCode::not_found, "source tempo target not found"});
  }
  auto key = tempo_cache_key(*source);
  if (!key) {
    return Result::failure(key.error());
  }
  if (estimate.identity.source_key != key.value().hex() || estimate.identity.generation == 0 ||
      estimate.sample_rate != source->sample_rate || estimate.frames != source->frames) {
    return Result::failure({foundation::ErrorCode::conflict, "tempo result source mismatch"});
  }
  auto applied = model().apply({project::ApplySourceTempoAnalysis{
      origin_revision, source_id, source->sha256, source->sample_rate, source->frames, estimate.bpm,
      estimate.confidence, estimate.suggested_beat_frames, estimate.suggestions_truncated}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void>
SourceAnalysisService::set_source_tempo_correction(project::SourceId source,
                                                   std::optional<double> bpm) {
  auto applied = model().apply({project::SetSourceTempoCorrection{source, bpm}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void>
SourceAnalysisService::accept_source_key_analysis(project::SourceId source_id,
                                                  std::uint64_t origin_revision,
                                                  const analysis::KeyEstimate& estimate) {
  using Result = foundation::Result<void>;
  const auto source = std::ranges::find_if(
      snapshot().sources, [&](const auto& candidate) { return candidate.id == source_id; });
  if (source == snapshot().sources.end()) {
    return Result::failure({foundation::ErrorCode::not_found, "source key target not found"});
  }
  auto key = key_cache_key(*source);
  if (!key) {
    return Result::failure(key.error());
  }
  if (estimate.identity.source_key != key.value().hex() || estimate.identity.generation == 0 ||
      estimate.sample_rate != source->sample_rate || estimate.frames != source->frames) {
    return Result::failure({foundation::ErrorCode::conflict, "key result source mismatch"});
  }
  const auto convert_key =
      [](analysis::MusicalKey value) -> foundation::Result<project::MusicalKey> {
    if (value.mode == analysis::KeyMode::major) {
      return foundation::Result<project::MusicalKey>::success(
          {value.tonic, project::KeyMode::major});
    }
    if (value.mode == analysis::KeyMode::minor) {
      return foundation::Result<project::MusicalKey>::success(
          {value.tonic, project::KeyMode::minor});
    }
    return foundation::Result<project::MusicalKey>::failure(
        {foundation::ErrorCode::invalid_argument, "invalid key mode"});
  };
  std::optional<project::MusicalKey> detected;
  if (estimate.key.has_value()) {
    auto converted = convert_key(estimate.key.value_or(analysis::MusicalKey{}));
    if (!converted) {
      return Result::failure(converted.error());
    }
    detected = converted.value();
  }
  if (estimate.compatible_keys.size() > project::max_compatible_keys_per_source) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "too many compatible key suggestions"});
  }
  std::vector<project::CompatibleKey> compatible;
  compatible.reserve(estimate.compatible_keys.size());
  for (const auto& suggestion : estimate.compatible_keys) {
    auto converted = convert_key(suggestion.key);
    if (!converted) {
      return Result::failure(converted.error());
    }
    project::KeyRelation relation;
    switch (suggestion.relation) {
    case analysis::KeyRelation::relative:
      relation = project::KeyRelation::relative;
      break;
    case analysis::KeyRelation::dominant:
      relation = project::KeyRelation::dominant;
      break;
    case analysis::KeyRelation::subdominant:
      relation = project::KeyRelation::subdominant;
      break;
    default:
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "invalid compatible key relation"});
    }
    compatible.push_back({converted.value(), relation});
  }
  auto applied = model().apply({project::ApplySourceKeyAnalysis{
      origin_revision, source_id, source->sha256, source->sample_rate, source->frames, detected,
      estimate.confidence, std::move(compatible)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void>
SourceAnalysisService::set_source_key_correction(project::SourceId source,
                                                 std::optional<project::MusicalKey> correction) {
  auto applied = model().apply({project::SetSourceKeyCorrection{source, correction}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void>
SourceAnalysisService::set_source_user_beat_frames(project::SourceId source,
                                                   std::vector<std::uint64_t> frames) {
  auto applied = model().apply({project::SetSourceUserBeatFrames{source, std::move(frames)}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

Result<void> SourceAnalysisService::accept_source_onset_analysis(
    const project::SourceRecord& analysed, std::uint64_t origin_revision,
    std::vector<std::uint64_t> frames, bool truncated) {
  auto applied = model().apply({project::ApplySourceOnsetAnalysis{
      origin_revision, analysed.id, analysed.sha256, analysed.sample_rate, analysed.frames,
      std::move(frames), truncated}});
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

Result<void> SourceAnalysisService::set_source_dismissed_onsets(project::SourceId source,
                                                                std::vector<std::uint64_t> frames) {
  auto applied = model().apply({project::SetSourceDismissedOnsets{source, std::move(frames)}});
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

} // namespace oscillate::application
