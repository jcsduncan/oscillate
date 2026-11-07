#include "route_commands.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace oscillate::quick {
RouteCommands::RouteCommands(Session session, CanEdit can_edit, Report report, Commit commit,
                             QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)) {}

application::ProjectSession* RouteCommands::editable_session() {
  if (can_edit_())
    return session_();
  return nullptr;
}

void RouteCommands::failure(const QString& context, const foundation::Error& error) {
  report_(QStringLiteral("%1: %2").arg(context, QString::fromStdString(error.message)));
}

void RouteCommands::accepted(const QString& message) {
  commit_();
  report_(QStringLiteral("%1 · revision %2 · Save to keep")
              .arg(message)
              .arg(session_()->snapshot().revision));
}

void RouteCommands::set_main_route(const QString& track_id, const QString& destination_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto source = project::TrackId::parse(track_id.toStdString());
  if (!source) {
    report_(QStringLiteral("Select a track and a valid group bus"));
    return;
  }
  std::optional<project::TrackId> output;
  if (!destination_id.isEmpty()) {
    const auto destination = project::TrackId::parse(destination_id.toStdString());
    if (!destination) {
      report_(QStringLiteral("Select a valid group bus"));
      return;
    }
    output = destination.value();
  }
  const auto found =
      std::ranges::find(current->snapshot().tracks, source.value(), &project::Track::id);
  if (found != current->snapshot().tracks.end() && found->main_output == output) {
    report_(QStringLiteral("Main output is already selected"));
    return;
  }
  const auto result = current->routing().set_main_output(source.value(), output);
  if (!result)
    return failure(QStringLiteral("Main route"), result.error());
  accepted(QStringLiteral("Main output changed"));
}

void RouteCommands::add_send_route(const QString& track_id, const QString& destination_id,
                                   double gain) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto source = project::TrackId::parse(track_id.toStdString());
  const auto destination = project::TrackId::parse(destination_id.toStdString());
  if (!source || !destination || !std::isfinite(gain) || gain < 0.0 || gain > 4.0) {
    report_(QStringLiteral("Select a bus and enter a send gain from 0 to 4"));
    return;
  }
  const auto result = current->routing().add_send(source.value(), destination.value(), gain);
  if (!result)
    return failure(QStringLiteral("Send"), result.error());
  accepted(QStringLiteral("Send added"));
}

void RouteCommands::add_sidechain_route(const QString& track_id, const QString& destination_id,
                                        const QString& effect_slot_id, double gain) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto source = project::TrackId::parse(track_id.toStdString());
  const auto destination = project::TrackId::parse(destination_id.toStdString());
  const auto slot = project::InsertSlotId::parse(effect_slot_id.toStdString());
  if (!source || !destination || !slot || !std::isfinite(gain) || gain < 0.0 || gain > 4.0) {
    report_(QStringLiteral("Select an enabled effect input and enter a gain from 0 to 4"));
    return;
  }
  const auto result =
      current->routing().add_sidechain(source.value(), destination.value(), slot.value(), gain);
  if (!result)
    return failure(QStringLiteral("Sidechain"), result.error());
  accepted(QStringLiteral("Sidechain send saved; playback status shows when audible"));
}

void RouteCommands::set_sidechain_source(const QString& destination_id,
                                         const QString& effect_slot_id, const QString& source_id,
                                         double gain) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto destination = project::TrackId::parse(destination_id.toStdString());
  const auto slot = project::InsertSlotId::parse(effect_slot_id.toStdString());
  std::optional<project::TrackId> source;
  if (!source_id.isEmpty()) {
    const auto parsed = project::TrackId::parse(source_id.toStdString());
    if (!parsed) {
      report_(QStringLiteral("Choose a track to key the sidechain"));
      return;
    }
    source = parsed.value();
  }
  if (!destination || !slot || !std::isfinite(gain) || gain < 0.0 || gain > 4.0) {
    report_(QStringLiteral("Choose an effect input and a key gain from 0 to 4"));
    return;
  }
  const auto result =
      current->routing().set_sidechain_source(destination.value(), slot.value(), source, gain);
  if (!result)
    return failure(QStringLiteral("Sidechain"), result.error());
  accepted(source ? QStringLiteral("Sidechain key saved; playback status shows when audible")
                  : QStringLiteral("Sidechain key removed"));
}

void RouteCommands::duck_tracks_to(const QString& source_id, const QStringList& target_ids) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto source = project::TrackId::parse(source_id.toStdString());
  std::vector<project::TrackId> targets;
  for (const auto& id : target_ids) {
    if (const auto target = project::TrackId::parse(id.toStdString()))
      targets.push_back(target.value());
  }
  if (!source || targets.empty()) {
    report_(QStringLiteral("Choose a key track and the tracks to duck"));
    return;
  }
  const auto result = current->effects().duck_tracks_to(source.value(), targets);
  if (!result)
    return failure(QStringLiteral("Duck"), result.error());
  const auto& tracks = current->snapshot().tracks;
  const auto key = std::ranges::find(tracks, source.value(), &project::Track::id);
  const auto name = key != tracks.end() ? QString::fromStdString(key->name) : QString();
  const auto count = result.value();
  accepted(QStringLiteral("Pump ducks %1 %2 to %3")
               .arg(count)
               .arg(count == 1 ? QStringLiteral("track") : QStringLiteral("tracks"))
               .arg(name.isEmpty() ? QStringLiteral("the key") : name));
}

void RouteCommands::set_send_gain(const QString& send_id, double gain) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::SendId::parse(send_id.toStdString());
  if (!parsed || !std::isfinite(gain) || gain < 0.0 || gain > 4.0) {
    report_(QStringLiteral("Send gain must be from 0 to 4"));
    return;
  }
  const auto result = current->routing().set_send_gain(parsed.value(), gain);
  if (!result)
    return failure(QStringLiteral("Send gain"), result.error());
  accepted(QStringLiteral("Send gain changed"));
}

void RouteCommands::remove_send(const QString& send_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::SendId::parse(send_id.toStdString());
  if (!parsed) {
    report_(QStringLiteral("Select a send to remove"));
    return;
  }
  const auto result = current->routing().remove_send(parsed.value());
  if (!result)
    return failure(QStringLiteral("Remove send"), result.error());
  accepted(QStringLiteral("Send removed"));
}

} // namespace oscillate::quick
