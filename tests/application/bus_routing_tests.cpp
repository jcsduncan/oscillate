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

std::vector<project::TrackId> order_of(const application::ProjectSession& session) {
  std::vector<project::TrackId> order;
  for (const auto& track : session.snapshot().tracks)
    order.push_back(track.id);
  return order;
}

// A track list drop: the tracks move to the drop point and route to the bus
// (or the master) there, as one undo step.
void a_drop_routes_and_moves_in_one_step() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(opened, "project creation failed");
  auto& session = *opened.value();
  const auto kick = named_track(session, "Kick");
  const auto bass = named_track(session, "Bass");
  const auto pad = named_track(session, "Pad");
  const auto chords = named_track(session, "Chords");
  const auto bus = session.routing().route_tracks_to_bus(std::vector{chords});
  REQUIRE_OK(bus, "the Music bus was not made");
  const auto music = bus.value();
  const std::vector before_drop{kick, bass, pad, music, chords};
  REQUIRE_EQ(order_of(session), before_drop, "the fixture's track order");
  const auto revision = session.snapshot().revision;

  // Onto the bus header: the last member.
  REQUIRE_OK(session.routing().place_tracks(std::vector{pad}, music), "the drop into a bus failed");
  REQUIRE_EQ(track_of(session, pad).main_output, music, "the dropped track feeds the bus");
  REQUIRE_EQ(order_of(session), (std::vector{kick, bass, music, chords, pad}),
             "the dropped track follows the bus's last member");
  REQUIRE_EQ(session.snapshot().revision, revision + 1, "the drop is one revision");
  REQUIRE_OK(session.history().undo(), "undo failed");
  REQUIRE_EQ(order_of(session), before_drop, "one undo restores the order");
  require(!track_of(session, pad).main_output, "one undo restores the routing");

  // Into the bracket, above a member; several tracks keep their order.
  REQUIRE_OK(session.routing().place_tracks(std::vector{pad, kick}, music, chords),
             "a multi-track drop failed");
  REQUIRE_EQ(order_of(session), (std::vector{bass, music, kick, pad, chords}),
             "the dropped tracks sit above the member, in their saved order");
  REQUIRE_EQ(track_of(session, kick).main_output, music, "every dropped track feeds the bus");
  REQUIRE_EQ(track_of(session, pad).main_output, music, "every dropped track feeds the bus");

  // Out of the bracket, above the bus: back to the master.
  REQUIRE_OK(session.routing().place_tracks(std::vector{pad}, std::nullopt, music),
             "the drop out of the bus failed");
  require(!track_of(session, pad).main_output, "a drop outside every bus goes to the master");
  REQUIRE_EQ(order_of(session), (std::vector{bass, pad, music, kick, chords}),
             "the track sits where it was dropped");

  // A drop where the track already is makes no undo step.
  const auto settled = session.snapshot().revision;
  REQUIRE_OK(session.routing().place_tracks(std::vector{pad}, std::nullopt, pad),
             "a drop in place failed");
  REQUIRE_EQ(session.snapshot().revision, settled, "a drop in place changes nothing");

  // A bus moves on the master, but never into a bus, itself included.
  const auto other = session.routing().route_tracks_to_bus(std::vector{bass});
  REQUIRE_OK(other, "the second bus was not made");
  require(!session.routing().place_tracks(std::vector{other.value()}, music).has_value(),
          "a group bus dropped into a bus is refused");
  require(!session.routing().place_tracks(std::vector{music}, music).has_value(),
          "a bus dropped into itself is refused");
  REQUIRE_OK(session.routing().place_tracks(std::vector{music}, std::nullopt),
             "a bus moves to the end of the list");
  REQUIRE_EQ(order_of(session).back(), music, "a bus moves to the end of the list");
  require(!track_of(session, music).main_output, "the moved bus stays on the master");
}

} // namespace

int main() {
  try {
    drums_go_to_a_new_bus_in_one_step();
    tracks_join_an_existing_bus_and_go_back();
    a_drop_routes_and_moves_in_one_step();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "bus routing tests passed\n";
  return EXIT_SUCCESS;
}
