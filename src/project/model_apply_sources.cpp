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

[[nodiscard]] SourceTempoReview* find_tempo_review(Snapshot& state, SourceId id) {
  const auto found = std::ranges::find_if(state.source_tempo_reviews,
                                          [id](const auto& review) { return review.source == id; });
  return found == state.source_tempo_reviews.end() ? nullptr : &*found;
}

[[nodiscard]] SourceKeyReview* find_key_review(Snapshot& state, SourceId id) {
  const auto found = std::ranges::find_if(state.source_key_reviews,
                                          [id](const auto& review) { return review.source == id; });
  return found == state.source_key_reviews.end() ? nullptr : &*found;
}

} // namespace

namespace {

template <typename T> Result<void> apply_key_review_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  const auto source = std::ranges::find_if(
      candidate.sources, [&](const SourceRecord& item) { return item.id == command.source; });
  if (source == candidate.sources.end()) {
    return Result<void>::failure({ErrorCode::not_found, "source key target not found"});
  }
  if constexpr (std::is_same_v<T, ApplySourceKeyAnalysis>) {
    if (command.origin_revision != candidate.revision || command.source_sha256 != source->sha256 ||
        command.sample_rate != source->sample_rate || command.frames != source->frames) {
      return Result<void>::failure(conflict("stale source key analysis"));
    }
  }
  auto* review = find_key_review(candidate, command.source);
  if (review == nullptr) {
    candidate.source_key_reviews.push_back({command.source, std::nullopt, 0.0, {}, std::nullopt});
    review = &candidate.source_key_reviews.back();
  }
  if constexpr (std::is_same_v<T, ApplySourceKeyAnalysis>) {
    review->detected = command.detected;
    review->confidence = command.confidence;
    review->compatible = command.compatible;
  } else {
    review->correction = command.correction;
  }
  add_unique(changes.source_key_reviews, command.source);
  return Result<void>::success();
}

template <typename T>
Result<void> apply_tempo_review_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  const auto source = std::ranges::find_if(
      candidate.sources, [&](const SourceRecord& item) { return item.id == command.source; });
  if (source == candidate.sources.end()) {
    return Result<void>::failure({ErrorCode::not_found, "source tempo target not found"});
  }
  if constexpr (std::is_same_v<T, ApplySourceTempoAnalysis> ||
                std::is_same_v<T, ApplySourceOnsetAnalysis>) {
    if (command.origin_revision != candidate.revision || command.source_sha256 != source->sha256 ||
        command.sample_rate != source->sample_rate || command.frames != source->frames) {
      return Result<void>::failure(conflict("stale source tempo analysis"));
    }
  } else if constexpr (std::is_same_v<T, SetSourceUserBeatFrames> ||
                       std::is_same_v<T, SetSourceDismissedOnsets>) {
    if (!valid_beat_frames(command.frames, source->frames))
      return Result<void>::failure(invalid("invalid source beat markers"));
  } else if constexpr (std::is_same_v<T, SetSourceDownbeat>) {
    if (command.frame && *command.frame >= source->frames)
      return Result<void>::failure(invalid("downbeat lies outside the source"));
  }
  if constexpr (std::is_same_v<T, ApplySourceOnsetAnalysis>) {
    if (!valid_beat_frames(command.generated_onset_frames, source->frames))
      return Result<void>::failure(invalid("invalid source onset suggestions"));
  }
  auto* review = find_tempo_review(candidate, command.source);
  if (review == nullptr) {
    candidate.source_tempo_reviews.push_back({.source = command.source});
    review = &candidate.source_tempo_reviews.back();
  }
  if constexpr (std::is_same_v<T, ApplySourceTempoAnalysis>) {
    review->detected_bpm = command.bpm;
    review->confidence = command.confidence;
    review->generated_beat_frames = command.generated_beat_frames;
    review->suggestions_truncated = command.suggestions_truncated;
    // A manually placed marker wins at the same source frame.
    std::erase_if(review->generated_beat_frames, [&](std::uint64_t frame) {
      return std::ranges::binary_search(review->user_beat_frames, frame);
    });
  } else if constexpr (std::is_same_v<T, SetSourceTempoCorrection>) {
    review->corrected_bpm = command.bpm;
  } else if constexpr (std::is_same_v<T, SetSourceDownbeat>) {
    review->downbeat_frame = command.frame;
  } else if constexpr (std::is_same_v<T, ApplySourceOnsetAnalysis>) {
    // Reanalysis replaces generated onsets only; a dismissal wins.
    review->generated_onset_frames = command.generated_onset_frames;
    review->onsets_truncated = command.truncated;
    std::erase_if(review->generated_onset_frames, [&](std::uint64_t frame) {
      return onset_dismissed(review->dismissed_onset_frames, frame);
    });
  } else if constexpr (std::is_same_v<T, SetSourceDismissedOnsets>) {
    review->dismissed_onset_frames = command.frames;
    std::erase_if(review->generated_onset_frames, [&](std::uint64_t frame) {
      return onset_dismissed(review->dismissed_onset_frames, frame);
    });
  } else {
    review->user_beat_frames = command.frames;
    std::erase_if(review->generated_beat_frames, [&](std::uint64_t frame) {
      return std::ranges::binary_search(review->user_beat_frames, frame);
    });
  }
  add_unique(changes.source_tempo_reviews, command.source);
  return Result<void>::success();
}

} // namespace

Result<void> apply_command(ApplyContext& context, const AddSource& command) {
  auto& candidate = context.candidate;
  auto& source_ids = context.source_ids;
  auto& changed_source_ids = context.changed_source_ids;
  if (candidate.sources.size() == max_source_records) {
    return Result<void>::failure(exhausted("source record limit exceeded"));
  }
  if (!source_ids.insert(command.source.id).second) {
    return Result<void>::failure(
        conflict("source already exists: " + command.source.id.to_string()));
  }
  auto valid = validate_source(command.source);
  if (!valid) {
    return valid;
  }
  candidate.sources.push_back(command.source);
  changed_source_ids.insert(command.source.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const RemoveSource& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto& source_ids = context.source_ids;
  auto& changed_source_ids = context.changed_source_ids;
  if (source_ids.erase(command.id) == 0) {
    return Result<void>::failure(
        {ErrorCode::not_found, "source not found: " + command.id.to_string()});
  }
  std::erase_if(candidate.sources,
                [&](const SourceRecord& source) { return source.id == command.id; });
  if (std::erase_if(candidate.source_tempo_reviews, [&](const SourceTempoReview& review) {
        return review.source == command.id;
      }) != 0) {
    add_unique(changes.source_tempo_reviews, command.id);
  }
  if (std::erase_if(candidate.source_key_reviews, [&](const SourceKeyReview& review) {
        return review.source == command.id;
      }) != 0) {
    add_unique(changes.source_key_reviews, command.id);
  }
  changed_source_ids.insert(command.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const RelinkSource& command) {
  auto& candidate = context.candidate;
  auto& changed_source_ids = context.changed_source_ids;
  auto found = std::ranges::find(candidate.sources, command.id, &SourceRecord::id);
  if (found == candidate.sources.end())
    return Result<void>::failure({ErrorCode::not_found, "source to relink not found"});
  if (!found->external_link)
    return Result<void>::failure(invalid("managed source cannot be relinked externally"));
  if (command.origin_revision != candidate.revision ||
      command.expected_last_known_path != found->external_link.value().last_known_path)
    return Result<void>::failure(conflict("stale external source link"));
  if (command.replacement_path == found->external_link.value().last_known_path)
    return Result<void>::failure(invalid("external source link is unchanged"));
  auto replacement = *found;
  replacement.external_link.value().last_known_path = command.replacement_path;
  auto valid = validate_source(replacement);
  if (!valid)
    return valid;
  *found = std::move(replacement);
  changed_source_ids.insert(command.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const ApplySourceKeyAnalysis& command) {
  return apply_key_review_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetSourceKeyCorrection& command) {
  return apply_key_review_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const ApplySourceTempoAnalysis& command) {
  return apply_tempo_review_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetSourceTempoCorrection& command) {
  return apply_tempo_review_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetSourceDownbeat& command) {
  return apply_tempo_review_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetSourceUserBeatFrames& command) {
  return apply_tempo_review_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const ApplySourceOnsetAnalysis& command) {
  return apply_tempo_review_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetSourceDismissedOnsets& command) {
  return apply_tempo_review_edit(context, command);
}

} // namespace oscillate::project::detail
