#include "../../../apps/daw_quick/src/track_routing_view.hpp"
#include "oscillate/test/check.hpp"

#include <QStringList>
#include <QVariantMap>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

project::TrackId track_id(std::uint64_t serial) {
  return project::TrackId::from_bits({1, serial}).value();
}

project::InsertSlotId slot_id(std::uint64_t serial) {
  return project::InsertSlotId::from_bits({1, serial}).value();
}

project::Snapshot snapshot_with_tracks() {
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  require(tempo && meter, "fixture timing maps invalid");
  project::Snapshot snapshot{project::ProjectId::from_bits({1, 1}).value(),
                             1,
                             48'000,
                             std::move(tempo).value(),
                             std::move(meter).value(),
                             {}};
  snapshot.tracks.emplace_back(track_id(1));
  snapshot.tracks.emplace_back(track_id(2));
  snapshot.tracks.emplace_back(track_id(3), std::vector<project::Clip>{},
                               project::TrackKind::instrument);
  for (std::uint64_t serial = 4; serial <= 20; ++serial)
    snapshot.tracks.emplace_back(track_id(serial), std::vector<project::Clip>{},
                                 project::TrackKind::group_bus);
  snapshot.tracks.emplace_back(track_id(21), std::vector<project::Clip>{},
                               project::TrackKind::return_bus);
  return snapshot;
}

void routing_projection() {
  auto snapshot = snapshot_with_tracks();
  const auto first_only =
      project::Snapshot{snapshot.id,    snapshot.revision, snapshot.sample_rate,
                        snapshot.tempo, snapshot.meter,    {snapshot.tracks.front()}};
  require(!quick::TrackRoutingView(first_only).active(),
          "an unrouted audio track should not activate routing");
  const quick::TrackRoutingView view(snapshot);
  require(view.name(track_id(1)) == "Audio 1" && view.name(track_id(2)) == "Audio 2" &&
              view.name(track_id(3)) == "Instrument 1" && view.name(track_id(4)) == "Group 1" &&
              view.name(track_id(20)) == "Group 17" && view.name(track_id(21)) == "Return 1",
          "track names must count independently by kind and project order");
  require(view.kind(track_id(4)) == "group" && view.kind(track_id(21)) == "return" &&
              view.target_count() == 18 && view.targets().size() == 16 && view.active(),
          "route targets must retain the displayed cap and full target count");
  require(view.targets().front().toMap().value("id").toString() ==
                  QString::fromStdString(track_id(4).to_string()) &&
              view.targets().back().toMap().value("name").toString() == "Group 16",
          "route target order or identity changed");
  require(view.main_destination_name(std::nullopt) == "Master" &&
              view.main_destination_name(track_id(4)) == "Group 1" &&
              view.send_destination_name(track_id(21)) == "Return 1" &&
              view.send_destination_name(track_id(99)) == "Missing bus" &&
              view.main_destination_name(track_id(99)) == "Master",
          "main and send destination fallback labels changed");
  snapshot.tracks[3].effects.push_back(
      {slot_id(1), true,
       project::Vst3PluginInstance{project::PluginInstanceId::from_bits({1, 1}).value(),
                                   "fixture",
                                   "Vendor",
                                   "Aux Gate",
                                   "1.0",
                                   "/opt/fixture",
                                   "sha",
                                   {},
                                   {},
                                   false,
                                   std::nullopt}});
  const quick::TrackRoutingView sidechain_view(snapshot);
  require(sidechain_view.sidechain_target_name(track_id(4), slot_id(1)) == "Aux Gate" &&
              sidechain_view.sidechain_target_name(track_id(4), slot_id(2)) == "Missing effect",
          "sidechain target label must identify the exact saved effect slot");
}

// The track list's order: a group bus with its members right under it (in
// saved order, wherever they are saved), the returns last; a return names
// the tracks whose sends feed it, and a silent send does not count.
void nesting_order() {
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  require(tempo && meter, "fixture timing maps invalid");
  project::Snapshot snapshot{project::ProjectId::from_bits({1, 1}).value(),
                             1,
                             48'000,
                             std::move(tempo).value(),
                             std::move(meter).value(),
                             {}};
  // Saved: Audio 1, Group 1, Audio 2 (in the group), Audio 3, Return 1,
  // Audio 4 (in the group).
  const auto group = track_id(2);
  const auto send_back = track_id(5);
  snapshot.tracks.emplace_back(track_id(1));
  snapshot.tracks.emplace_back(group, std::vector<project::Clip>{}, project::TrackKind::group_bus);
  snapshot.tracks.emplace_back(track_id(3));
  snapshot.tracks.emplace_back(track_id(4));
  snapshot.tracks.emplace_back(send_back, std::vector<project::Clip>{},
                               project::TrackKind::return_bus);
  snapshot.tracks.emplace_back(track_id(6));
  snapshot.tracks[2].main_output = group;
  snapshot.tracks[5].main_output = group;
  constexpr double audible_gain = 0.5;
  constexpr double silent_gain = 0.0;
  constexpr int members = 2;
  snapshot.tracks[3].sends.push_back(
      {project::SendId::from_bits({1, 1}).value(), send_back, audible_gain});
  snapshot.tracks[5].sends.push_back(
      {project::SendId::from_bits({1, 2}).value(), send_back, silent_gain});
  const quick::TrackRoutingView view(snapshot);
  const std::vector<std::pair<std::uint64_t, int>> rows{{1, 0}, {2, 1}, {3, 2},
                                                        {6, 3}, {4, 4}, {5, 5}};
  for (const auto& [serial, row] : rows)
    require(view.nesting(track_id(serial)).display_index == row,
            "track " + std::to_string(serial) + " is not on row " + std::to_string(row));
  require(view.nesting(group).member_count == members && view.nesting(track_id(3)).bus == group &&
              view.nesting(track_id(6)).bus == group && !view.nesting(track_id(4)).bus,
          "members must nest under their group bus");
  require(view.nesting(track_id(6)).last_member && !view.nesting(track_id(3)).last_member,
          "the bus's last member ends its rail");
  require(view.nesting(send_back).fed_by == QStringList{QStringLiteral("Audio 3")},
          "a return must name the tracks whose audible sends feed it");
  snapshot.tracks[1].name = "Music";
  require(quick::TrackRoutingView(snapshot).main_destination_name(group) == "Music",
          "a destination shows the bus's own name");
}
} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main() {
  routing_projection();
  nesting_order();
}
