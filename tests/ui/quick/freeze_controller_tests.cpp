#include "../../../apps/daw_quick/src/freeze_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

// ADR-0059: the track menu's Freeze, Unfreeze, Bounce, Flatten, Bounce the
// loop and Consolidate, through the controller the QML calls.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr std::int64_t bar = 4 * timeline::ticks_per_quarter;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-freeze-ui-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "fixture directory creation failed");
    path_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

const project::Track& find(const project::Snapshot& snapshot, project::TrackId id) {
  const auto found = std::ranges::find(snapshot.tracks, id, &project::Track::id);
  require(found != snapshot.tracks.end(), "track present");
  return *found;
}

void the_menu_freezes_bounces_and_consolidates() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened.value());
  const auto track = session->instruments().add_synth_track().value();
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(), "clip");
  const auto clip = find(session->snapshot(), track).midi_clips.front().id;
  // A long note, so a render has blocks enough to cancel.
  require(session->midi_clips().resize_midi_clip(clip, {24 * bar}).has_value() &&
              session->notes().add_note_with_duration(clip, {0}, {24 * bar}, 48).has_value(),
          "notes");
  require(session->midi_clips().add_midi_clip(track, {24 * bar}).has_value(), "second clip");
  require(session->saving().save().has_value(), "save");

  QString status;
  int commits = 0;
  quick::FreezeController freeze([&] { return session.get(); }, [] { return true; },
                                 [&](QString message) { status = std::move(message); },
                                 [&] { ++commits; });
  const auto id = QString::fromStdString(track.to_string());

  freeze.freeze(id);
  require(freeze.busy() && freeze.action() == QStringLiteral("Freezing") && freeze.track_id() == id,
          "a freeze runs in the background");
  freeze.freeze(id);
  require(status.contains("wait or cancel"), "one job at a time");
  require(freeze.wait(60'000), "the freeze finished");
  require(commits == 1 && find(session->snapshot(), track).frozen.has_value() &&
              status.contains("Frozen"),
          "Freeze commits one step: " + status.toStdString());
  freeze.freeze(id);
  require(status.contains("already frozen") && !freeze.busy(), "a frozen track stays frozen");
  freeze.unfreeze(id);
  require(commits == 2 && !find(session->snapshot(), track).frozen, "Unfreeze commits");

  // Cancel: nothing is saved.
  const auto revision = session->snapshot().revision;
  freeze.freeze(id);
  freeze.cancel();
  require(freeze.wait(60'000), "the cancelled job ended");
  require(session->snapshot().revision == revision && status.contains("cancelled"),
          "a cancelled freeze changes nothing: " + status.toStdString());

  // Bounce the loop: bars 2 to 4 become a new audio track.
  const auto tracks = session->snapshot().tracks.size();
  freeze.bounce_bars(id, 2, 4);
  require(freeze.wait(60'000) && commits == 3, "the loop bounced: " + status.toStdString());
  const auto& bounced = session->snapshot();
  const auto source = std::ranges::find(bounced.tracks, track, &project::Track::id);
  require(bounced.tracks.size() == tracks + 1 && source->muted &&
              (source + 1)->kind == project::TrackKind::audio &&
              (source + 1)->clips.front().source_range.end.value ==
                  bounced.tempo.tick_to_sample({3 * bar}, bounced.sample_rate).value().value -
                      bounced.tempo.tick_to_sample({bar}, bounced.sample_rate).value().value,
          "the loop's bars, exactly, below a muted source");
  require(session->history().undo().has_value(), "undo the bounce");

  // Consolidate the loop over both clips.
  freeze.consolidate_bars(id, 1, 26);
  require(commits == 4 && find(session->snapshot(), track).midi_clips.size() == 1,
          "Consolidate makes one clip: " + status.toStdString());
  freeze.consolidate(id, 0.5, 10.0);
  require(status.contains("Select a time range"), "a fractional tick is refused");

  // Flatten replaces the track.
  freeze.flatten(id);
  require(freeze.wait(60'000) && commits == 5, "flattened: " + status.toStdString());
  require(std::ranges::none_of(session->snapshot().tracks,
                               [&](const project::Track& item) { return item.id == track; }),
          "the source is gone");
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    the_menu_freezes_bounces_and_consolidates();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAILED: %s\n", error.what());
    return 1;
  }
  return 0;
}
