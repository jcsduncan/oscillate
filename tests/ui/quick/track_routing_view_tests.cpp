#include "../../../apps/daw_quick/src/track_routing_view.hpp"
#include "oscillate/test/check.hpp"

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
} // namespace

int main() { routing_projection(); }
