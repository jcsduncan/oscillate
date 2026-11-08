#include "oscillate/application/history_labels.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The history panel's labels: each undo step is named from the snapshots
// either side of it, and the session lists the undo steps oldest first and
// the redo steps in the order they redo.
namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

void accepted(foundation::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-history") {}
};

std::vector<std::string> labels(const application::ProjectSession& session) {
  const auto undo = session.history().undo_steps();
  const auto redo = session.history().redo_steps();
  std::vector<std::string> result;
  result.reserve(undo.size() + redo.size());
  for (const auto& step : undo)
    result.push_back(application::describe_change(*step.before, *step.after));
  for (const auto& step : redo)
    result.push_back(application::describe_change(*step.before, *step.after) + " (undone)");
  return result;
}

std::string joined(const std::vector<std::string>& items) {
  std::string text;
  for (const auto& item : items)
    text += "[" + item + "]";
  return text;
}

void steps_are_named_by_what_they_changed() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  // A new project seeds its returns without history.
  const auto before = session->history().undo_steps().size();
  const auto track = take(session->instruments().add_synth_track());
  accepted(session->tracks().set_track_name(track, "Bass"));
  (void)take(session->effects().add_effect_engine(track, 0, "pump"));
  accepted(session->tracks().set_track_muted(track, true));
  accepted(session->midi_clips().add_midi_clip(track, timeline::TickPosition{0}));
  const auto& clips =
      std::ranges::find(session->snapshot().tracks, track, &project::Track::id)->midi_clips;
  require(!clips.empty(), "the clip was not added");
  accepted(session->notes().add_note(clips.front().id, timeline::TickPosition{0}, 36));
  (void)take(session->song().add_marker(timeline::TickPosition{0}, "Drop"));
  accepted(session->song().set_project_key(project::MusicalKey{9, project::KeyMode::minor}));

  auto named = labels(*session);
  named.erase(named.begin(), named.begin() + static_cast<std::ptrdiff_t>(before));
  // An unnamed track is numbered by its row.
  const auto& tracks = session->snapshot().tracks;
  const auto row = std::ranges::find(tracks, track, &project::Track::id) - tracks.begin() + 1;
  const std::vector<std::string> expected{"Add track Track " + std::to_string(row),
                                          "Rename track Bass",
                                          "Add Pump on Bass",
                                          "Mute Bass",
                                          "Add clip on Bass",
                                          "Edit notes on Bass",
                                          "Add marker",
                                          "Set song key"};
  REQUIRE_EQ(named, expected, "history labels: " + joined(named));

  // Undone steps stay listed, after the done ones, next to redo first.
  accepted(session->history().undo());
  accepted(session->history().undo());
  named = labels(*session);
  named.erase(named.begin(), named.begin() + static_cast<std::ptrdiff_t>(before));
  require(named.size() == expected.size() && named[5] == "Edit notes on Bass" &&
              named[6] == "Add marker (undone)" && named[7] == "Set song key (undone)",
          "undone steps: " + joined(named));
  REQUIRE_EQ(session->history().undo_steps().size(), before + 6, "the history split moved");
  REQUIRE_EQ(session->history().redo_steps().size(), 2, "the history split moved");
}

void gestures_across_tracks_read_as_one_action() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto kick = take(session->instruments().add_synth_track());
  constexpr int target_count = 3;
  std::vector<project::TrackId> targets;
  targets.reserve(target_count);
  for (int i = 0; i < target_count; ++i)
    targets.push_back(take(session->instruments().add_synth_track()));
  const project::Snapshot copy = session->snapshot();
  (void)take(session->effects().duck_tracks_to(kick, targets));
  const auto label = application::describe_change(copy, session->snapshot());
  REQUIRE_EQ(label, "Add Pump on 3 tracks and more", "a duck gesture read as: " + label);
}

void nothing_changed_still_has_a_label() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto& snapshot = session->snapshot();
  REQUIRE_EQ(application::describe_change(snapshot, snapshot), "Edit",
             "an empty change had no label");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main() {
  steps_are_named_by_what_they_changed();
  gestures_across_tracks_read_as_one_action();
  nothing_changed_still_has_a_label();
}
