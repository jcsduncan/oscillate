#include <oscillate/application/graph_control.hpp>

#include <algorithm>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
[[nodiscard]] Result<void> rejected(ErrorCode code, const char* message) {
  return Result<void>::failure({code, message});
}

} // namespace

GraphControl::GraphControl(project::ProjectModel& model, engine::GraphPublisher& graph,
                           std::size_t max_frames)
    : model_(model), graph_(graph), max_frames_(max_frames),
      revisions_(model.snapshot()->revision, graph.active_revision(),
                 ChangeDescriptor{"open project", {}}) {}

Result<void> GraphControl::fail(std::uint64_t revision, Error error) {
  (void)revisions_.fail_revision(revision, error);
  return Result<void>::failure(std::move(error));
}

Result<void>
GraphControl::build_and_publish(const std::shared_ptr<const project::Snapshot>& built_snapshot,
                                std::span<const ClipFeed> feeds) {
  observe();
  const auto& snapshot = *built_snapshot;
  // Master effects render only on the routed graph.
  if (std::ranges::any_of(snapshot.master_effects, &project::EffectInsert::enabled))
    return fail(snapshot.revision,
                {ErrorCode::unsupported,
                 "The direct live graph has no master effects; routed playback renders them"});
  if (project::requires_routed_rendering(snapshot)) {
    return fail(snapshot.revision,
                {ErrorCode::unsupported, "Live graph cannot render saved buses or sends yet"});
  }
  // The direct graph plays audio clips only: instruments, effects and
  // automation render on the routed graph. An empty instrument track is silent.
  if (std::ranges::any_of(snapshot.tracks, [](const project::Track& track) {
        return !track.effects.empty() ||
               !std::holds_alternative<std::monostate>(track.instrument_source);
      })) {
    return fail(snapshot.revision,
                {ErrorCode::unsupported,
                 "Live graph cannot render saved instruments or track effects yet"});
  }
  if (project::requires_automation_rendering(snapshot)) {
    return fail(snapshot.revision,
                {ErrorCode::unsupported, "Live graph cannot apply automation; routed playback "
                                         "renders it"});
  }
  std::size_t clip_count = 0;
  for (const auto& track : snapshot.tracks) {
    clip_count += track.clips.size();
  }
  if (clip_count != feeds.size()) {
    return fail(snapshot.revision,
                {ErrorCode::unavailable,
                 "clip feeds do not match project; provide them and retry or undo"});
  }

  std::vector<engine::NodeRequest> requests;
  requests.reserve(feeds.size());
  for (std::size_t index = 0; index < feeds.size(); ++index) {
    const auto& feed = feeds[index];
    if (std::ranges::any_of(feeds.first(index), [&feed](const ClipFeed& earlier) {
          return earlier.clip == feed.clip;
        })) {
      return fail(snapshot.revision, {ErrorCode::conflict, "audio clip has duplicate feeds"});
    }
    auto prepared = make_clip_request(snapshot, feed.clip, feed.ring, feed.readiness,
                                      feed.instance_id, max_frames_);
    if (!prepared) {
      return fail(snapshot.revision, prepared.error());
    }
    requests.push_back(std::move(prepared).value());
  }
  for (const auto& track : snapshot.tracks) {
    for (const auto& clip : track.clips) {
      if (std::ranges::none_of(feeds,
                               [&clip](const ClipFeed& feed) { return feed.clip == clip.id; })) {
        return fail(snapshot.revision, {ErrorCode::unavailable, "audio clip has no prepared feed"});
      }
    }
  }
  auto graph =
      engine::prepare_graph(snapshot.revision, max_frames_, requests, graph_.active_plan());
  if (!graph) {
    return fail(snapshot.revision, graph.error());
  }
  switch (graph_.publish(std::move(graph).value())) {
  case engine::PublishStatus::accepted:
    return Result<void>::success();
  case engine::PublishStatus::invalid_plan:
    return fail(snapshot.revision, {ErrorCode::internal, "invalid prepared graph; retry or undo"});
  case engine::PublishStatus::stale_revision:
    return fail(snapshot.revision, {ErrorCode::conflict,
                                    "graph publication was superseded; observe and retry or undo"});
  }
  return fail(snapshot.revision, {ErrorCode::internal, "unknown graph publication result"});
}

Result<void> GraphControl::submit(const project::AppliedRevision& applied,
                                  const std::optional<ClipFeed>& feed,
                                  std::string_view command_label) {
  return submit(applied, feed ? std::span(&*feed, 1) : std::span<const ClipFeed>{}, command_label);
}

Result<void> GraphControl::submit(const project::AppliedRevision& applied,
                                  std::span<const ClipFeed> feeds, std::string_view command_label) {
  const auto current = model_.snapshot();
  if (applied.snapshot != current) {
    return rejected(ErrorCode::conflict, "committed revision was superseded; submit current state");
  }
  if (current->revision > revisions_.snapshot().model_revision) {
    (void)revisions_.accept_model_revision(
        current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  }
  return build_and_publish(current, feeds);
}

Result<void> GraphControl::submit_current(const std::optional<ClipFeed>& feed) {
  return submit_current(feed ? std::span(&*feed, 1) : std::span<const ClipFeed>{});
}

Result<void> GraphControl::submit_current(std::span<const ClipFeed> feeds) {
  const auto current = model_.snapshot();
  if (current->revision > revisions_.snapshot().model_revision) {
    (void)revisions_.accept_model_revision(current->revision,
                                           ChangeDescriptor{"project refresh", {}});
  }
  return build_and_publish(current, feeds);
}

Result<void> GraphControl::retry(const std::optional<ClipFeed>& feed) {
  return retry(feed ? std::span(&*feed, 1) : std::span<const ClipFeed>{});
}

Result<void> GraphControl::retry(std::span<const ClipFeed> feeds) {
  if (revisions_.snapshot().application != PlaybackApplication::failed) {
    return rejected(ErrorCode::conflict, "only a failed current revision can be retried");
  }
  const auto current = model_.snapshot();
  if (current->revision != revisions_.snapshot().model_revision) {
    return rejected(ErrorCode::conflict, "project advanced; submit its current revision");
  }
  (void)revisions_.retry_current_revision();
  return build_and_publish(current, feeds);
}

Result<project::AppliedRevision>
GraphControl::undo_and_submit(const std::optional<ClipFeed>& feed) {
  return undo_and_submit(feed ? std::span(&*feed, 1) : std::span<const ClipFeed>{});
}

Result<project::AppliedRevision> GraphControl::undo_and_submit(std::span<const ClipFeed> feeds) {
  auto undone = model_.undo();
  if (!undone) {
    return undone;
  }
  // A graph failure does not reverse the accepted undo. Inspect status().
  (void)submit(undone.value(), feeds, "undo");
  return undone;
}

void GraphControl::observe() {
  const auto collected = graph_.collect_retired();
  const auto active_revision = graph_.active_revision();
  if (collected.rejected_revision != 0) {
    (void)revisions_.fail_revision(
        collected.rejected_revision,
        {ErrorCode::conflict, "graph base changed before activation; retry or undo"});
  }
  (void)revisions_.observe_active_graph(active_revision);
}

Result<void> GraphControl::require_export_ready(std::uint64_t intended_revision) const {
  const auto current = model_.snapshot();
  const auto state = revisions_.snapshot();
  if (intended_revision != current->revision || state.model_revision != current->revision ||
      state.application != PlaybackApplication::active ||
      state.active_graph_revision != intended_revision) {
    return rejected(ErrorCode::unavailable,
                    "requested project revision is not active; apply or retry before export");
  }
  return Result<void>::success();
}

} // namespace oscillate::application
