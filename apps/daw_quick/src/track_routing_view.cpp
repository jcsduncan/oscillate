#include "track_routing_view.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace oscillate::quick {
namespace {
constexpr std::size_t max_quick_route_targets = 16;

[[nodiscard]] bool is_group(const project::Track& track) {
  return track.kind == project::TrackKind::group_bus;
}

[[nodiscard]] bool is_return(const project::Track& track) {
  return track.kind == project::TrackKind::return_bus;
}
} // namespace

TrackRoutingView::TrackRoutingView(const project::Snapshot& snapshot) {
  int audio_ordinal = 1;
  int instrument_ordinal = 1;
  int group_ordinal = 1;
  int return_ordinal = 1;
  for (const auto& track : snapshot.tracks) {
    QString track_kind;
    QString track_name;
    switch (track.kind) {
    case project::TrackKind::audio:
      track_kind = QStringLiteral("audio");
      track_name = QStringLiteral("Audio %1").arg(audio_ordinal++);
      break;
    case project::TrackKind::instrument:
      track_kind = QStringLiteral("instrument");
      track_name = QStringLiteral("Instrument %1").arg(instrument_ordinal++);
      break;
    case project::TrackKind::group_bus:
      track_kind = QStringLiteral("group");
      track_name = QStringLiteral("Group %1").arg(group_ordinal++);
      break;
    case project::TrackKind::return_bus:
      track_kind = QStringLiteral("return");
      track_name = QStringLiteral("Return %1").arg(return_ordinal++);
      break;
    }
    names_.emplace(track.id, track_name);
    shown_names_.emplace(track.id,
                         track.name.empty() ? track_name : QString::fromStdString(track.name));
    kinds_.emplace(track.id, track_kind);
    for (const auto& effect : track.effects) {
      if (const auto* vst3 = std::get_if<project::Vst3PluginInstance>(&effect.processor))
        sidechain_target_names_.emplace(std::make_pair(track.id, effect.id),
                                        QString::fromStdString(vst3->name));
      else if (const auto* patch = std::get_if<project::NativeEffectPatch>(&effect.processor)) {
        // A built-in effect is a target only when its engine reads a detector.
        const auto* engine = native_dsp::effects::find_engine(patch->engine);
        if (engine && engine->sidechain)
          sidechain_target_names_.emplace(
              std::make_pair(track.id, effect.id),
              QString::fromStdString(std::string(engine->descriptor->name)));
      }
    }
    if (track.kind == project::TrackKind::group_bus ||
        track.kind == project::TrackKind::return_bus) {
      ++target_count_;
      if (targets_.size() < static_cast<qsizetype>(max_quick_route_targets)) {
        targets_.push_back(QVariantMap{{"id", QString::fromStdString(track.id.to_string())},
                                       {"name", shown_names_.at(track.id)},
                                       {"kind", track_kind}});
      }
      active_ = true;
    }
    active_ = active_ || track.main_output.has_value() || !track.sends.empty();
  }
  nest(snapshot);
}

void TrackRoutingView::nest(const project::Snapshot& snapshot) {
  const auto& tracks = snapshot.tracks;
  const auto group_of = [&](const project::Track& track) -> std::optional<project::TrackId> {
    if (is_group(track) || is_return(track) || !track.main_output)
      return std::nullopt;
    const auto bus = std::ranges::find(tracks, *track.main_output, &project::Track::id);
    return bus != tracks.end() && is_group(*bus) ? std::optional{bus->id} : std::nullopt;
  };
  std::vector<const project::Track*> order;
  order.reserve(tracks.size());
  for (const auto& track : tracks) {
    if (is_return(track) || group_of(track))
      continue;
    order.push_back(&track);
    if (!is_group(track))
      continue;
    const project::Track* last = nullptr;
    for (const auto& member : tracks) {
      if (group_of(member) != track.id)
        continue;
      order.push_back(&member);
      nesting_[member.id].bus = track.id;
      ++nesting_[track.id].member_count;
      last = &member;
    }
    if (last != nullptr)
      nesting_[last->id].last_member = true;
  }
  for (const auto& track : tracks) {
    if (is_return(track))
      order.push_back(&track);
  }
  int row = 0;
  for (const auto* track : order) {
    nesting_[track->id].display_index = row++;
    for (const auto& send : track->sends) {
      if (send.purpose != project::SendPurpose::audible || send.gain <= 0.0)
        continue;
      const auto destination = std::ranges::find(tracks, send.destination, &project::Track::id);
      if (destination != tracks.end() && is_return(*destination))
        nesting_[send.destination].fed_by.push_back(shown_names_.at(track->id));
    }
  }
}

QString TrackRoutingView::main_destination_name(std::optional<project::TrackId> destination) const {
  const auto found = destination ? shown_names_.find(*destination) : shown_names_.end();
  return found == shown_names_.end() ? QStringLiteral("Master") : found->second;
}

QString TrackRoutingView::send_destination_name(project::TrackId destination) const {
  const auto found = shown_names_.find(destination);
  return found == shown_names_.end() ? QStringLiteral("Missing bus") : found->second;
}

QString TrackRoutingView::sidechain_target_name(project::TrackId destination,
                                                project::InsertSlotId slot) const {
  const auto found = sidechain_target_names_.find(std::make_pair(destination, slot));
  return found == sidechain_target_names_.end() ? QStringLiteral("Missing effect") : found->second;
}

} // namespace oscillate::quick
