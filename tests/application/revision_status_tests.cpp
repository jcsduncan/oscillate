#include "oscillate/test/check.hpp"

#include <oscillate/application/revision_status.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using oscillate::application::ChangeDescriptor;
using oscillate::application::PlaybackApplication;
using oscillate::application::RevisionTracker;
using oscillate::foundation::ErrorCode;

ChangeDescriptor change(std::string label, std::uint64_t track_number) {
  auto track = oscillate::project::TrackId::from_bits({0, track_number});
  if (!track) {
    throw std::runtime_error("fixture track ID failed");
  }
  ChangeDescriptor description{std::move(label), {}};
  description.affected.tracks.push_back(track.value());
  return description;
}

using oscillate::test::require;

void failed_edit_remains_distinct_from_playback() {
  RevisionTracker uninitialised(0);
  REQUIRE_EQ(uninitialised.snapshot().application, PlaybackApplication::pending,
             "an uninitialised revision is never audible");
  RevisionTracker tracker(1, 1);
  REQUIRE_EQ(tracker.snapshot().application, PlaybackApplication::active,
             "loaded revision initially matches playback");
  require(tracker.accept_model_revision(2, change("add-track", 7)),
          "accepted edit advances model revision");
  const auto accepted = tracker.snapshot();
  const auto accepted_change = accepted.change.value_or(ChangeDescriptor{});
  REQUIRE_EQ(tracker.snapshot().application, PlaybackApplication::pending,
             "accepted edit is pending until a graph switch");
  REQUIRE_EQ(accepted_change.label, "add-track", "accepted edit is pending until a graph switch");
  REQUIRE_EQ(accepted_change.affected.tracks.size(), 1,
             "accepted edit is pending until a graph switch");
  REQUIRE_EQ(accepted_change.affected.tracks[0].bits().low, 7,
             "accepted edit is pending until a graph switch");
  require(tracker.fail_revision(2, {ErrorCode::resource_exhausted, "graph exceeds budget"}),
          "failed graph build is recorded");
  const auto failed = tracker.snapshot();
  const auto failed_change = failed.change.value_or(ChangeDescriptor{});
  REQUIRE_EQ(failed.model_revision, 2, "failure cannot imply the saved edit is audible");
  REQUIRE_EQ(failed.active_graph_revision, 1, "failure cannot imply the saved edit is audible");
  REQUIRE_EQ(failed.application, PlaybackApplication::failed,
             "failure lost its command or stable affected track");
  REQUIRE_TRUE(failed.failure, "failure lost its command or stable affected track");
  REQUIRE_EQ(failed.failure->message, "graph exceeds budget",
             "failure lost its command or stable affected track");
  REQUIRE_EQ(failed_change.label, "add-track", "failure lost its command or stable affected track");
  REQUIRE_EQ(failed_change.affected.tracks.size(), 1,
             "failure lost its command or stable affected track");
  REQUIRE_EQ(failed_change.affected.tracks[0].bits().low, 7,
             "failure lost its command or stable affected track");
  require(tracker.retry_current_revision(), "failed edit could not retry");
  const auto retrying = tracker.snapshot();
  const auto retry_change = retrying.change.value_or(ChangeDescriptor{});
  REQUIRE_EQ(retrying.application, PlaybackApplication::pending,
             "retry dropped accepted change identity");
  REQUIRE_TRUE(!retrying.failure, "retry dropped accepted change identity");
  REQUIRE_EQ(retry_change.label, "add-track", "retry dropped accepted change identity");
  REQUIRE_EQ(retry_change.affected.tracks.size(), 1, "retry dropped accepted change identity");
  REQUIRE_EQ(retry_change.affected.tracks[0].bits().low, 7,
             "retry dropped accepted change identity");
  require(tracker.observe_active_graph(2), "successful retry may activate the saved edit");
  REQUIRE_EQ(tracker.snapshot().application, PlaybackApplication::active,
             "activation clears stale failure");
  REQUIRE_TRUE(!tracker.snapshot().failure, "activation clears stale failure");
  require(!tracker.retry_current_revision(), "active graph accepted retry");
}

void superseded_failure_cannot_poison_latest_edit() {
  RevisionTracker tracker(1, 1);
  require(tracker.accept_model_revision(2, change("first", 7)), "first edit accepted");
  require(tracker.accept_model_revision(3, change("second", 8)), "second edit accepted");
  require(!tracker.fail_revision(2, {ErrorCode::unavailable, "stale failure"}),
          "superseded failure is ignored");
  require(tracker.observe_active_graph(2), "older graph may activate before newest one");
  const auto pending = tracker.snapshot();
  const auto pending_change = pending.change.value_or(ChangeDescriptor{});
  REQUIRE_EQ(pending.model_revision, 3,
             "intermediate activation does not mark latest edit audible");
  REQUIRE_EQ(pending.active_graph_revision, 2,
             "intermediate activation does not mark latest edit audible");
  REQUIRE_EQ(pending.application, PlaybackApplication::pending,
             "intermediate activation does not mark latest edit audible");
  REQUIRE_EQ(pending_change.label, "second",
             "intermediate activation does not mark latest edit audible");
  REQUIRE_EQ(pending_change.affected.tracks.size(), 1,
             "intermediate activation does not mark latest edit audible");
  REQUIRE_EQ(pending_change.affected.tracks[0].bits().low, 8,
             "intermediate activation does not mark latest edit audible");
  require(!tracker.observe_active_graph(4), "future graph revision rejected");
  require(!tracker.observe_active_graph(1), "active revision cannot move backward");
  require(!tracker.accept_model_revision(3, change("stale", 9)), "model revision cannot repeat");
  require(tracker.fail_revision(3, {ErrorCode::unavailable, "latest failure"}),
          "latest failure is recorded");
  const auto latest = tracker.snapshot();
  REQUIRE_EQ(latest.active_graph_revision, 2, "older valid playback remains explicit");
  REQUIRE_EQ(latest.application, PlaybackApplication::failed,
             "older valid playback remains explicit");
  REQUIRE_EQ(latest.change.value_or(ChangeDescriptor{}).label, "second",
             "older valid playback remains explicit");
}
} // namespace

int main() {
  try {
    failed_edit_remains_distinct_from_playback();
    superseded_failure_cannot_poison_latest_edit();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
