#include "../../../apps/daw_quick/src/route_commands.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-sidechain-ui-XXXXXX").string();
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

void saved_route_and_rejection() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto opened = application::ProjectSession::create(root);
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened.value());
  const auto source = session->tracks().add_bus_track(project::TrackKind::group_bus);
  const auto destination = session->tracks().add_bus_track(project::TrackKind::return_bus);
  require(source && destination, "bus creation failed");
  const application::Vst3InsertIdentity identity{std::string(32, 'a'), "Fixture Vendor",
                                                 "Aux Effect",         "1.0",
                                                 "/opt/aux.vst3",      std::string(64, 'b')};
  require(session->effects().insert_vst3_plugin(destination.value(), 0, identity).has_value(),
          "target effect insertion failed");
  const auto& target_track = session->snapshot().tracks.back();
  require(target_track.effects.size() == 1, "target effect absent");
  const auto slot = target_track.effects.front().id;
  QString status;
  int commits = 0;
  quick::RouteCommands commands([&] { return session.get(); }, [] { return true; },
                                [&](QString message) { status = std::move(message); },
                                [&] { ++commits; });
  const auto source_id = QString::fromStdString(source.value().to_string());
  const auto destination_id = QString::fromStdString(destination.value().to_string());
  const auto slot_id = QString::fromStdString(slot.to_string());
  const auto before = session->snapshot().revision;
  commands.add_sidechain_route(source_id, destination_id, slot_id, 0.5);
  require(commits == 1 && session->snapshot().revision == before + 1 && status.contains("saved") &&
              session->snapshot().tracks.front().sends.size() == 1,
          "sidechain command did not commit one saved revision");
  const auto& saved = session->snapshot().tracks.front().sends.front();
  require(saved.destination == destination.value() && saved.target_insert == slot &&
              saved.purpose == project::SendPurpose::sidechain && saved.gain == 0.5,
          "sidechain target or gain changed");
  const auto send_id = saved.id;
  commands.add_sidechain_route(source_id, source_id, slot_id, 1.0);
  require(commits == 1 && session->snapshot().revision == before + 1 &&
              status.contains("Sidechain:"),
          "invalid route changed saved state");
  require(session->history().undo() && session->snapshot().tracks.front().sends.empty() &&
              session->history().redo() &&
              session->snapshot().tracks.front().sends.front().id == send_id &&
              session->saving().save(),
          "sidechain undo, redo or save failed");
  session.reset();
  auto reopened = application::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().tracks.front().sends.front().id == send_id &&
              reopened.value()->snapshot().tracks.front().sends.front().target_insert == slot,
          "sidechain target did not survive reopen");
}
// One kick keys every track's Pump: sidechain keys are not mixer send
// knobs, so only the model's per-track limit applies, not a UI cap.
void a_kick_keys_many_effects() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened.value());
  const auto kick = session->instruments().add_synth_track();
  require(kick.has_value(), "kick track failed");
  QString status;
  quick::RouteCommands commands([&] { return session.get(); }, [] { return true; },
                                [&](QString message) { status = std::move(message); }, [] {});
  const auto kick_id = QString::fromStdString(kick.value().to_string());
  constexpr int keyed = 6;
  for (int i = 0; i < keyed; ++i) {
    const auto track = session->instruments().add_synth_track();
    require(track.has_value(), "keyed track failed");
    const auto slot = session->effects().add_effect_engine(track.value(), 0, "pump");
    require(slot.has_value(), "Pump insertion failed");
    commands.set_sidechain_source(QString::fromStdString(track.value().to_string()),
                                  QString::fromStdString(slot.value().to_string()), kick_id, 1.0);
  }
  const auto& sends = session->snapshot().tracks.front().sends;
  require(sends.size() == static_cast<std::size_t>(keyed),
          "the kick keys every Pump: " + status.toStdString());
  // Audible sends are not capped at four either.
  const auto bus = session->tracks().add_bus_track(project::TrackKind::return_bus);
  require(bus.has_value(), "return bus failed");
  commands.add_send_route(kick_id, QString::fromStdString(bus.value().to_string()), 0.5);
  require(session->snapshot().tracks.front().sends.size() == keyed + 1,
          "a send still fits beside six keys: " + status.toStdString());
}

// Duck to the kick in one gesture: every target gets a Pump keyed from the
// kick (one it had is reused), all linked to one shape, undone in one step.
void tracks_duck_to_a_kick_at_once() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened.value());
  const auto kick = session->instruments().add_synth_track();
  require(kick.has_value(), "kick track failed");
  QStringList targets;
  std::vector<project::TrackId> ids;
  for (int i = 0; i < 5; ++i) {
    const auto track = session->instruments().add_synth_track();
    require(track.has_value(), "target track failed");
    ids.push_back(track.value());
    targets.push_back(QString::fromStdString(track.value().to_string()));
  }
  const auto existing = session->effects().add_effect_engine(ids[2], 0, "pump", 3); // Hard Gate.
  require(existing.has_value(), "existing Pump failed");
  const auto kick_id = QString::fromStdString(kick.value().to_string());
  targets.push_back(kick_id); // The key never ducks itself.
  QString status;
  int commits = 0;
  quick::RouteCommands commands([&] { return session.get(); }, [] { return true; },
                                [&](QString message) { status = std::move(message); },
                                [&] { ++commits; });
  const auto before = session->snapshot().revision;
  commands.duck_tracks_to(kick_id, targets);
  require(commits == 1 && session->snapshot().revision == before + 1 &&
              status.contains("Pump ducks 5 tracks"),
          "the gesture is one saved revision: " + status.toStdString());
  const auto& snapshot = session->snapshot();
  require(snapshot.tracks.front().sends.size() == 5 && snapshot.tracks.front().effects.empty(),
          "the kick keys five Pumps and gains none");
  const auto& reused = snapshot.tracks[3];
  require(reused.effects.size() == 1 && reused.effects.front().id == existing.value(),
          "a track's own Pump is reused");
  const auto shape =
      std::get<project::NativeEffectPatch>(reused.effects.front().processor).document;
  for (std::size_t i = 1; i < snapshot.tracks.size(); ++i) {
    const auto& track = snapshot.tracks[i];
    require(track.effects.size() == 1, "each target has one Pump");
    const auto& patch = std::get<project::NativeEffectPatch>(track.effects.front().processor);
    require(patch.engine == "pump" && patch.document == shape &&
                session->effects().native_effect_link_count(track.effects.front().id) == 5,
            "every Pump is linked to the reused one's shape");
    require(std::ranges::any_of(snapshot.tracks.front().sends,
                                [&](const project::SendRoute& send) {
                                  return send.destination == track.id &&
                                         send.target_insert == track.effects.front().id &&
                                         send.purpose == project::SendPurpose::sidechain;
                                }),
            "every Pump is keyed from the kick");
  }
  require(session->history().undo().has_value() &&
              session->snapshot().tracks.front().sends.empty() &&
              session->snapshot().tracks[1].effects.empty() &&
              session->snapshot().tracks[3].effects.size() == 1 &&
              session->effects().native_effect_link_count(existing.value()) == 1,
          "one undo removes the whole gesture");
}
} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  saved_route_and_rejection();
  a_kick_keys_many_effects();
  tracks_duck_to_a_kick_at_once();
}
