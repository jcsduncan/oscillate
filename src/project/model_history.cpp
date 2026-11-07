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
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oscillate::project {

using namespace detail;

namespace {

void add_all_ids(const Snapshot& state, ChangeSet& changes) {
  for (const auto& track : state.tracks) {
    add_unique(changes.tracks, track.id);
    for (const auto& lane : track.automation) {
      add_unique(changes.automation_lanes, lane.id);
    }
    for (const auto& clip : track.clips) {
      add_unique(changes.clips, clip.id);
      for (const auto& lane : clip.automation) {
        add_unique(changes.automation_lanes, lane.id);
      }
    }
    for (const auto& clip : track.midi_clips) {
      add_unique(changes.clips, clip.id);
      for (const auto& lane : clip.automation) {
        add_unique(changes.automation_lanes, lane.id);
      }
    }
    for (const auto* plugin : track_plugins(track)) {
      add_unique(changes.plugins, plugin->id);
    }
  }
  for (const auto& source : state.sources) {
    changes.sources.push_back(source.id);
  }
  changes.automation = !changes.automation_lanes.empty();
}

[[nodiscard]] bool same_tempo(const timeline::TempoMap& a, const timeline::TempoMap& b) {
  if (a.revision() != b.revision() || a.events().size() != b.events().size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.events().size(); ++i) {
    const auto& left = a.events()[i];
    const auto& right = b.events()[i];
    if (left.tick != right.tick || left.bpm != right.bpm ||
        left.shape_to_next != right.shape_to_next) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool same_meter(const timeline::MeterMap& a, const timeline::MeterMap& b) {
  if (a.revision() != b.revision() || a.events().size() != b.events().size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.events().size(); ++i) {
    const auto& left = a.events()[i];
    const auto& right = b.events()[i];
    if (left.tick != right.tick || left.numerator != right.numerator ||
        left.denominator != right.denominator) {
      return false;
    }
  }
  return true;
}

// True when nothing but names, colours or markers changed.
[[nodiscard]] bool presentation_only(const ChangeSet& changes) {
  return changes.presentation && !changes.key && changes.tracks.empty() && changes.clips.empty() &&
         changes.sources.empty() && changes.source_tempo_reviews.empty() &&
         changes.source_key_reviews.empty() && changes.plugins.empty() &&
         changes.automation_lanes.empty() && !changes.automation && !changes.timing &&
         !changes.sample_rate && !changes.master_effects && !changes.routing;
}

// The two snapshots sound the same: they differ at most in names, colours
// and markers. The key is heard: Knock's pads tune to it.
[[nodiscard]] bool same_audio(const Snapshot& from, const Snapshot& to) {
  const auto unpresented = [](std::vector<Track> tracks) {
    for (auto& track : tracks) {
      track.name.clear();
      track.color.reset();
      for (auto& clip : track.clips) {
        clip.color.reset();
        clip.name.clear();
      }
      for (auto& clip : track.midi_clips)
        clip.color.reset();
    }
    return tracks;
  };
  return from.sample_rate == to.sample_rate && from.key == to.key &&
         same_tempo(from.tempo, to.tempo) && same_meter(from.meter, to.meter) &&
         from.sources == to.sources && from.master_effects == to.master_effects &&
         from.master_level == to.master_level &&
         from.source_tempo_reviews == to.source_tempo_reviews &&
         from.source_key_reviews == to.source_key_reviews &&
         unpresented(from.tracks) == unpresented(to.tracks);
}

// What a frozen track's render depends on: everything but its presentation,
// its channel (fader, pan, mute, solo), its sends and routing, and lanes on
// those, which still play live after the render.
[[nodiscard]] Track rendered_part(Track track) {
  const auto live_lane = [](const AutomationLane& lane) {
    const auto* control = std::get_if<TrackControlTarget>(&lane.target);
    return (control != nullptr && is_channel_control(control->key)) ||
           std::holds_alternative<SendGainTarget>(lane.target);
  };
  track.name.clear();
  track.color.reset();
  track.main_output.reset();
  track.sends.clear();
  track.muted = false;
  track.soloed = false;
  track.level = 1.0;
  track.pan = 0.0;
  std::erase_if(track.automation, live_lane);
  for (auto& clip : track.clips) {
    clip.name.clear();
    clip.color.reset();
    std::erase_if(clip.automation, live_lane);
  }
  for (auto& clip : track.midi_clips) {
    clip.color.reset();
    std::erase_if(clip.automation, live_lane);
  }
  for (auto& macro : track.macros)
    macro.name.clear();
  return track;
}

// A command may freeze or unfreeze a track, but not change what a track that
// stays frozen rendered (ADR-0059).
[[nodiscard]] Result<void> check_frozen_tracks(const Snapshot& before, const Snapshot& after) {
  bool frozen = false;
  for (const auto& track : after.tracks) {
    if (!track.frozen)
      continue;
    const auto old = std::ranges::find(before.tracks, track.id, &Track::id);
    if (old == before.tracks.end() || old->frozen != track.frozen)
      continue;
    frozen = true;
    if (rendered_part(*old) != rendered_part(track)) {
      return Result<void>::failure(
          conflict("the track is frozen; unfreeze it to change its sound"));
    }
  }
  if (frozen &&
      (!same_tempo(before.tempo, after.tempo) || before.sample_rate != after.sample_rate)) {
    return Result<void>::failure(
        conflict("frozen tracks keep the tempo and sample rate; unfreeze them first"));
  }
  return Result<void>::success();
}

[[nodiscard]] ChangeSet broad_changes(const Snapshot& from, const Snapshot& to) {
  ChangeSet changes;
  if (from.source_tempo_reviews != to.source_tempo_reviews) {
    for (const auto& review : from.source_tempo_reviews) {
      add_unique(changes.source_tempo_reviews, review.source);
    }
    for (const auto& review : to.source_tempo_reviews) {
      add_unique(changes.source_tempo_reviews, review.source);
    }
  }
  if (from.source_key_reviews != to.source_key_reviews) {
    for (const auto& review : from.source_key_reviews) {
      add_unique(changes.source_key_reviews, review.source);
    }
    for (const auto& review : to.source_key_reviews) {
      add_unique(changes.source_key_reviews, review.source);
    }
  }
  changes.timing = !same_tempo(from.tempo, to.tempo) || !same_meter(from.meter, to.meter);
  changes.sample_rate = from.sample_rate != to.sample_rate;
  changes.master_effects = from.master_effects != to.master_effects;
  changes.routing = from.master_level != to.master_level ||
                    !std::ranges::equal(from.tracks, to.tracks, [](const Track& a, const Track& b) {
                      // As for the forward edits: mixer controls and freezing are routing.
                      return a.id == b.id && a.kind == b.kind && a.main_output == b.main_output &&
                             a.sends == b.sends && a.muted == b.muted && a.soloed == b.soloed &&
                             a.level == b.level && a.pan == b.pan && a.frozen == b.frozen;
                    });
  // Undo, redo and restore may change any name, colour or marker.
  changes.presentation = true;
  add_all_ids(from, changes);
  add_all_ids(to, changes);
  std::ranges::sort(changes.sources);
  const auto duplicate_tail = std::ranges::unique(changes.sources);
  changes.sources.erase(duplicate_tail.begin(), duplicate_tail.end());
  return changes;
}

} // namespace

Result<AppliedRevision> ProjectModel::apply(const std::vector<Operation>& operations,
                                            std::optional<std::uint64_t> gesture) {
  OSCILLATE_TRACE_SPAN("edit.apply");
  if (operations.empty()) {
    return Result<AppliedRevision>::failure(invalid("empty project command"));
  }
  if (gesture && *gesture == 0) {
    return Result<AppliedRevision>::failure(invalid("zero gesture ID is reserved"));
  }
  if (current_->revision == std::numeric_limits<std::uint64_t>::max()) {
    return Result<AppliedRevision>::failure(exhausted("project revision exhausted"));
  }
  const bool needs_stopped = std::ranges::any_of(operations, [](const Operation& operation) {
    return std::holds_alternative<SetSampleRate>(operation);
  });
  std::unique_ptr<StoppedTransportLease> lease;
  if (needs_stopped) {
    auto acquired = transport_->acquire_stopped();
    if (!acquired) {
      return Result<AppliedRevision>::failure(acquired.error());
    }
    lease = std::move(acquired).value();
    if (!lease) {
      return Result<AppliedRevision>::failure(
          {ErrorCode::internal, "transport gate returned no lease"});
    }
  }
  Snapshot candidate = *current_;
  ChangeSet changes;
  std::set<SourceId> source_ids;
  for (const auto& source : candidate.sources) {
    source_ids.insert(source.id);
  }
  std::set<SourceId> changed_source_ids;
  ApplyContext context{candidate, changes, source_ids, changed_source_ids};
  for (const auto& operation : operations) {
    auto result = apply_one(context, operation);
    if (!result) {
      return Result<AppliedRevision>::failure(result.error());
    }
  }
  if (auto released = release_lone_links(*current_, context); !released) {
    return Result<AppliedRevision>::failure(released.error());
  }
  changes.sources.assign(changed_source_ids.begin(), changed_source_ids.end());
  auto valid = validate(candidate);
  if (!valid) {
    return Result<AppliedRevision>::failure(valid.error());
  }
  if (auto frozen = check_frozen_tracks(*current_, candidate); !frozen) {
    return Result<AppliedRevision>::failure(frozen.error());
  }
  if (changes.timing || changes.sample_rate) {
    add_all_ids(candidate, changes);
  }
  if (!changed_source_ids.empty()) {
    std::set<TrackId> changed_tracks(changes.tracks.begin(), changes.tracks.end());
    std::set<ClipId> changed_clips(changes.clips.begin(), changes.clips.end());
    for (const auto& track : candidate.tracks) {
      // A built-in synth plays its managed media slots.
      if (const auto* native = native_source(track);
          native && std::ranges::any_of(native->document.media, [&](const NativeMediaSlot& slot) {
            const auto* managed = std::get_if<ManagedAudioReference>(&slot.media);
            return managed != nullptr && changed_source_ids.contains(managed->source);
          })) {
        changed_tracks.insert(track.id);
      }
      for (const auto& clip : track.clips) {
        if (changed_source_ids.contains(clip.source)) {
          changed_tracks.insert(track.id);
          changed_clips.insert(clip.id);
        }
      }
    }
    changes.tracks.assign(changed_tracks.begin(), changed_tracks.end());
    changes.clips.assign(changed_clips.begin(), changed_clips.end());
  }
  candidate.revision = current_->revision + 1;
  candidate.audible_revision =
      presentation_only(changes) ? current_->audible_revision : candidate.revision;
  auto next = std::make_shared<const Snapshot>(std::move(candidate));
  if (gesture && eligible_gesture_ == gesture && !undo_.empty() &&
      undo_.back().gesture == gesture) {
    // The step's `before` is unchanged; the replaced current was its
    // `after` and is released.
    redo_.clear();
    current_ = std::move(next);
    current_bytes_ = estimated_bytes(*current_);
    undo_.back().after = current_;
    undo_.back().after_bytes = current_bytes_;
    recount_history();
  } else {
    push_undo(std::move(next), gesture);
  }
  eligible_gesture_ = gesture;
  return Result<AppliedRevision>::success({current_, std::move(changes), std::move(lease)});
}

Result<AppliedRevision> ProjectModel::restore_snapshot(Snapshot saved) {
  if (saved.id != current_->id)
    return Result<AppliedRevision>::failure(conflict("saved snapshot belongs to another project"));
  if (saved.revision == 0)
    return Result<AppliedRevision>::failure(invalid("zero saved project revision is reserved"));
  if (current_->revision == std::numeric_limits<std::uint64_t>::max())
    return Result<AppliedRevision>::failure(exhausted("project revision exhausted"));
  saved.next_insert_slot_serial =
      std::max(saved.next_insert_slot_serial, current_->next_insert_slot_serial);
  auto valid = validate(saved);
  if (!valid)
    return Result<AppliedRevision>::failure(valid.error());
  std::unique_ptr<StoppedTransportLease> lease;
  if (saved.sample_rate != current_->sample_rate) {
    auto acquired = transport_->acquire_stopped();
    if (!acquired)
      return Result<AppliedRevision>::failure(acquired.error());
    lease = std::move(acquired).value();
    if (!lease)
      return Result<AppliedRevision>::failure(
          {ErrorCode::internal, "transport gate returned no lease"});
  }
  saved.revision = current_->revision + 1;
  saved.audible_revision =
      same_audio(*current_, saved) ? current_->audible_revision : saved.revision;
  auto changes = broad_changes(*current_, saved);
  push_undo(std::make_shared<const Snapshot>(std::move(saved)), std::nullopt);
  eligible_gesture_.reset();
  return Result<AppliedRevision>::success({current_, std::move(changes), std::move(lease)});
}

Result<AppliedRevision> ProjectModel::adopt_authoritative_snapshot(Snapshot saved) {
  if (saved.id != current_->id || saved.revision <= current_->revision)
    return Result<AppliedRevision>::failure(conflict("playback snapshot is foreign or stale"));
  if (saved.sample_rate != current_->sample_rate)
    return Result<AppliedRevision>::failure(
        conflict("playback sample rate needs a stopped device transition"));
  saved.next_insert_slot_serial =
      std::max(saved.next_insert_slot_serial, current_->next_insert_slot_serial);
  auto valid = validate(saved);
  if (!valid)
    return Result<AppliedRevision>::failure(valid.error());
  saved.audible_revision =
      same_audio(*current_, saved) ? current_->audible_revision : saved.revision;
  auto changes = broad_changes(*current_, saved);
  current_ = std::make_shared<const Snapshot>(std::move(saved));
  current_bytes_ = estimated_bytes(*current_);
  undo_.clear();
  redo_.clear();
  recount_history();
  eligible_gesture_.reset();
  return Result<AppliedRevision>::success({current_, std::move(changes), nullptr});
}

Result<AppliedRevision> ProjectModel::publish_history(const Snapshot& target, bool undoing) {
  if (current_->revision == std::numeric_limits<std::uint64_t>::max()) {
    return Result<AppliedRevision>::failure(exhausted("project revision exhausted"));
  }
  std::unique_ptr<StoppedTransportLease> lease;
  if (target.sample_rate != current_->sample_rate) {
    auto acquired = transport_->acquire_stopped();
    if (!acquired) {
      return Result<AppliedRevision>::failure(acquired.error());
    }
    lease = std::move(acquired).value();
    if (!lease) {
      return Result<AppliedRevision>::failure(
          {ErrorCode::internal, "transport gate returned no lease"});
    }
  }
  Snapshot candidate = target;
  candidate.next_insert_slot_serial =
      std::max(candidate.next_insert_slot_serial, current_->next_insert_slot_serial);
  auto valid = validate(candidate);
  if (!valid) {
    return Result<AppliedRevision>::failure(valid.error());
  }
  candidate.revision = current_->revision + 1;
  candidate.audible_revision =
      same_audio(*current_, candidate) ? current_->audible_revision : candidate.revision;
  auto changes = broad_changes(*current_, candidate);
  auto next = std::make_shared<const Snapshot>(std::move(candidate));
  if (undoing) {
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
  } else {
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
  }
  // The new current is a copy of the target with its own revision; the
  // history keeps the snapshots it already held.
  current_ = std::move(next);
  current_bytes_ = estimated_bytes(*current_);
  recount_history();
  return Result<AppliedRevision>::success({current_, std::move(changes), std::move(lease)});
}

void ProjectModel::push_undo(std::shared_ptr<const Snapshot> next,
                             std::optional<std::uint64_t> gesture) {
  redo_.clear();
  const auto before_bytes = current_bytes_;
  auto before = std::exchange(current_, std::move(next));
  current_bytes_ = estimated_bytes(*current_);
  undo_.push_back({std::move(before), current_, gesture, before_bytes, current_bytes_});
  trim_history();
}

void ProjectModel::recount_history() {
  // Neighbouring steps share snapshots, and the newest step's may be the
  // current one: count each snapshot only the history keeps alive once.
  std::unordered_set<const Snapshot*> counted{current_.get()};
  history_bytes_ = 0;
  const auto count = [&](const std::shared_ptr<const Snapshot>& snapshot, std::size_t bytes) {
    if (counted.insert(snapshot.get()).second)
      history_bytes_ += bytes;
  };
  for (const auto* history : {&undo_, &redo_})
    for (const auto& entry : *history) {
      count(entry.before, entry.before_bytes);
      count(entry.after, entry.after_bytes);
    }
}

void ProjectModel::trim_history() {
  recount_history();
  const auto over_limits = [&] {
    const bool too_deep = undo_.size() > limits_.steps;
    const bool too_large = history_bytes_ > limits_.bytes && undo_.size() > limits_.kept_steps;
    return too_deep || too_large;
  };
  while (!undo_.empty() && over_limits()) {
    undo_.pop_front();
    recount_history();
  }
}

void ProjectModel::set_history_limits(HistoryLimits limits) {
  limits_ = limits;
  trim_history();
}

Result<AppliedRevision> ProjectModel::undo() {
  eligible_gesture_.reset();
  if (undo_.empty()) {
    return Result<AppliedRevision>::failure({ErrorCode::not_found, "undo history is empty"});
  }
  return publish_history(*undo_.back().before, true);
}

Result<AppliedRevision> ProjectModel::redo() {
  eligible_gesture_.reset();
  if (redo_.empty()) {
    return Result<AppliedRevision>::failure({ErrorCode::not_found, "redo history is empty"});
  }
  return publish_history(*redo_.back().after, false);
}

std::vector<ProjectModel::HistoryStep> ProjectModel::undo_steps() const {
  std::vector<HistoryStep> steps;
  steps.reserve(undo_.size());
  for (const auto& entry : undo_)
    steps.push_back({entry.before, entry.after});
  return steps;
}

std::vector<ProjectModel::HistoryStep> ProjectModel::redo_steps() const {
  std::vector<HistoryStep> steps;
  steps.reserve(redo_.size());
  for (const auto& entry : std::views::reverse(redo_))
    steps.push_back({entry.before, entry.after});
  return steps;
}

} // namespace oscillate::project
