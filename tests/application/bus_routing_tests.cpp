#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// Send to bus: selected tracks go to a new group bus (just above the first of
// them, named for what they are) or to an existing one, and back to the
// master, each as one undo step.
namespace {

namespace application = oscillate::application;
namespace project = oscillate::project;
namespace fs = std::filesystem;

using oscillate::test::require;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-bus") {}
};

const project::Track& track_of(const application::ProjectSession& session, project::TrackId id) {
  const auto& tracks = session.snapshot().tracks;
  const auto found = std::ranges::find(tracks, id, &project::Track::id);
  REQUIRE_NE(found, tracks.end(), "track missing");
  return *found;
}

std::size_t index_of(const application::ProjectSession& session, project::TrackId id) {
  const auto& tracks = session.snapshot().tracks;
  return static_cast<std::size_t>(std::ranges::find(tracks, id, &project::Track::id) -
                                  tracks.begin());
}

project::TrackId named_track(application::ProjectSession& session, std::string name) {
  const auto id = session.tracks().add_audio_track();
  REQUIRE_OK(id, "track creation failed");
  REQUIRE_OK(session.tracks().set_track_name(id.value(), std::move(name)), "track creation failed");
  return id.value();
}

void drums_go_to_a_new_bus_in_one_step() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(opened, "project creation failed");
  auto& session = *opened.value();
  const auto pad = named_track(session, "Pad");
  const auto kick = named_track(session, "Kick");
  const auto snare = named_track(session, "Block (Snare)");
  const auto hats = named_track(session, "Hats");
  const std::vector selection{hats, kick, snare};

  const auto bus = session.routing().route_tracks_to_bus(selection);
  REQUIRE_OK(bus, "send to a new bus failed");
  const auto& made = track_of(session, bus.value());
  REQUIRE_EQ(made.kind, project::TrackKind::group_bus, "the new bus is a group bus");
  REQUIRE_EQ(made.name, "Drums", "drum tracks name their bus Drums");
  REQUIRE_EQ(index_of(session, bus.value()), index_of(session, kick) - 1,
             "the bus sits just above the first selected track");
  for (const auto id : selection)
    REQUIRE_EQ(track_of(session, id).main_output, bus.value(),
               "every selected track goes to the bus");
  require(!track_of(session, pad).main_output, "an unselected track stays on the master");

  // One undo step takes the bus and the routing away together.
  REQUIRE_OK(session.history().undo(), "undo failed");
  require(std::ranges::none_of(
              session.snapshot().tracks,
              [](const project::Track& t) { return t.kind == project::TrackKind::group_bus; }),
          "undo removes the bus");
  for (const auto id : selection)
    require(!track_of(session, id).main_output, "undo routes the tracks back to the master");
}

void tracks_join_an_existing_bus_and_go_back() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(opened, "project creation failed");
  auto& session = *opened.value();
  const auto pad = named_track(session, "Pad");
  const auto keys = named_track(session, "Keys");
  const auto first = session.routing().route_tracks_to_bus(std::vector{pad});
  REQUIRE_OK(first, "other tracks name their bus Bus N");
  REQUIRE_EQ(track_of(session, first.value()).name, "Bus 1", "other tracks name their bus Bus N");
  // The bus itself in the selection is left alone.
  const auto joined =
      session.routing().route_tracks_to_bus(std::vector{keys, first.value()}, first.value());
  REQUIRE_OK(joined, "send to an existing bus failed");
  REQUIRE_EQ(joined.value(), first.value(), "send to an existing bus failed");
  REQUIRE_EQ(track_of(session, keys).main_output, first.value(), "a track joins the existing bus");
  require(!track_of(session, first.value()).main_output, "the bus does not route into itself");

  REQUIRE_OK(session.routing().route_tracks_to_master(std::vector{pad, keys}),
             "back to master failed");
  REQUIRE_TRUE(!track_of(session, pad).main_output,
               "back to master routes the tracks straight out");
  REQUIRE_TRUE(!track_of(session, keys).main_output,
               "back to master routes the tracks straight out");
  // A bus alone has nothing to send.
  require(!session.routing().route_tracks_to_bus(std::vector{first.value()}).has_value(),
          "a selection of buses only is refused");
}

} // namespace

int main() {
  try {
    drums_go_to_a_new_bus_in_one_step();
    tracks_join_an_existing_bus_and_go_back();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "bus routing tests passed\n";
  return EXIT_SUCCESS;
}
