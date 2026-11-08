#include "track_commands.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace oscillate::quick {

TrackCommands::TrackCommands(Session session, CanEdit can_create, CanEdit can_edit, Report report,
                             Commit commit, Created created, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_create_(std::move(can_create)),
      can_edit_(std::move(can_edit)), report_(std::move(report)), commit_(std::move(commit)),
      created_(std::move(created)) {}

application::ProjectSession* TrackCommands::editable_session() {
  if (can_edit_())
    return session_();
  return nullptr;
}

void TrackCommands::failure(const QString& context, const foundation::Error& error) {
  report_(QStringLiteral("%1: %2").arg(context, QString::fromStdString(error.message)));
}

void TrackCommands::accepted(const QString& message) {
  commit_();
  report_(QStringLiteral("%1 · revision %2 · Save to keep")
              .arg(message)
              .arg(session_()->snapshot().revision));
}

void TrackCommands::add_instrument_track() {
  if (!can_create_())
    return;
  auto* current = session_();
  const auto result = current->tracks().add_instrument_track();
  if (!result)
    return failure(QStringLiteral("Track change"), result.error());
  const auto index = static_cast<int>(current->tracks().track_index(result.value()).value_or(0));
  commit_();
  created_(index);
  report_(QStringLiteral("Instrument track added · revision %1").arg(current->snapshot().revision));
}

void TrackCommands::add_synth_track() {
  if (!can_create_())
    return;
  auto* current = session_();
  const auto result = current->instruments().add_synth_track();
  if (!result)
    return failure(QStringLiteral("Track change"), result.error());
  const auto index = static_cast<int>(current->tracks().track_index(result.value()).value_or(0));
  commit_();
  created_(index);
  report_(QStringLiteral("Prism track added · revision %1 · Save to keep")
              .arg(current->snapshot().revision));
}

void TrackCommands::add_audio_track() {
  auto* current = editable_session();
  if (!current)
    return;
  const auto result = current->tracks().add_audio_track();
  if (!result)
    return failure(QStringLiteral("Track change"), result.error());
  const auto index = static_cast<int>(current->tracks().track_index(result.value()).value_or(0));
  commit_();
  created_(index);
  report_(QStringLiteral("Audio track added"));
}

void TrackCommands::add_bus_track(const QString& kind) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto bus_kind = kind == QStringLiteral("group")    ? project::TrackKind::group_bus
                        : kind == QStringLiteral("return") ? project::TrackKind::return_bus
                                                           : project::TrackKind::audio;
  if (bus_kind == project::TrackKind::audio) {
    report_(QStringLiteral("Choose a group or return bus"));
    return;
  }
  const auto result = current->tracks().add_bus_track(bus_kind);
  if (!result)
    return failure(QStringLiteral("Bus"), result.error());
  const auto index = static_cast<int>(current->tracks().track_index(result.value()).value_or(0));
  commit_();
  created_(index);
  report_(QStringLiteral("%1 added · revision %2 · Save to keep")
              .arg(kind == QStringLiteral("group") ? QStringLiteral("Group bus")
                                                   : QStringLiteral("Return bus"))
              .arg(current->snapshot().revision));
}

std::optional<std::vector<project::TrackId>> TrackCommands::parse_tracks(const QStringList& ids) {
  std::vector<project::TrackId> tracks;
  tracks.reserve(static_cast<std::size_t>(ids.size()));
  for (const auto& id : ids) {
    const auto parsed = project::TrackId::parse(id.toStdString());
    if (!parsed) {
      report_(QStringLiteral("Select valid tracks to route"));
      return std::nullopt;
    }
    tracks.push_back(parsed.value());
  }
  return tracks;
}

void TrackCommands::route_to_bus(const QStringList& track_ids, const QString& bus_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto tracks = parse_tracks(track_ids);
  if (!tracks)
    return;
  std::optional<project::TrackId> bus;
  if (!bus_id.isEmpty()) {
    const auto parsed = project::TrackId::parse(bus_id.toStdString());
    if (!parsed) {
      report_(QStringLiteral("Choose a valid group bus"));
      return;
    }
    bus = parsed.value();
  }
  const auto result = current->routing().route_tracks_to_bus(*tracks, bus);
  if (!result)
    return failure(QStringLiteral("Send to bus"), result.error());
  const auto& snapshot = current->snapshot();
  const auto found = std::ranges::find(snapshot.tracks, result.value(), &project::Track::id);
  const auto name =
      found != snapshot.tracks.end() ? QString::fromStdString(found->name) : QString{};
  commit_();
  if (!bus)
    created_(static_cast<int>(current->tracks().track_index(result.value()).value_or(0)));
  report_(
      QStringLiteral("%1 track(s) sent to %2 · Undo is available").arg(tracks->size()).arg(name));
}

void TrackCommands::route_to_master(const QStringList& track_ids) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto tracks = parse_tracks(track_ids);
  if (!tracks)
    return;
  const auto revision = current->snapshot().revision;
  const auto result = current->routing().route_tracks_to_master(*tracks);
  if (!result)
    return failure(QStringLiteral("Back to master"), result.error());
  if (current->snapshot().revision == revision)
    return;
  accepted(QStringLiteral("Sent straight to the master"));
}

void TrackCommands::place_tracks(const QStringList& track_ids, const QString& bus_id,
                                 const QString& before_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto tracks = parse_tracks(track_ids);
  if (!tracks)
    return;
  std::optional<project::TrackId> bus;
  std::optional<project::TrackId> before;
  const auto parse_optional = [this](const QString& text, std::optional<project::TrackId>& out) {
    if (text.isEmpty())
      return true;
    const auto id = project::TrackId::parse(text.toStdString());
    if (!id) {
      report_(QStringLiteral("Choose a valid place in the track list"));
      return false;
    }
    out = id.value();
    return true;
  };
  if (!parse_optional(bus_id, bus) || !parse_optional(before_id, before))
    return;
  const auto revision = current->snapshot().revision;
  const auto result = current->routing().place_tracks(*tracks, bus, before);
  if (!result)
    return failure(QStringLiteral("Move track"), result.error());
  if (current->snapshot().revision == revision)
    return;
  const auto& snapshot = current->snapshot();
  const auto found =
      bus ? std::ranges::find(snapshot.tracks, *bus, &project::Track::id) : snapshot.tracks.end();
  const auto destination = found != snapshot.tracks.end() && !found->name.empty()
                               ? QString::fromStdString(found->name)
                           : bus ? QStringLiteral("the bus")
                                 : QStringLiteral("the master");
  commit_();
  report_(QStringLiteral("%1 track(s) routed to %2 · Undo is available")
              .arg(tracks->size())
              .arg(destination));
}

void TrackCommands::remove_track(const QString& track_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    report_(QStringLiteral("Select a valid track to delete"));
    return;
  }
  const auto result = current->tracks().remove_track(parsed.value());
  if (!result)
    return failure(QStringLiteral("Delete track"), result.error());
  commit_();
  report_(QStringLiteral("Track deleted · revision %1 · Undo is available")
              .arg(current->snapshot().revision));
}

QString TrackCommands::duplicate_track(const QString& track_id) {
  auto* current = editable_session();
  if (!current)
    return {};
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    report_(QStringLiteral("Select a valid track to duplicate"));
    return {};
  }
  const auto result = current->tracks().duplicate_track(parsed.value());
  if (!result) {
    failure(QStringLiteral("Duplicate track"), result.error());
    return {};
  }
  const auto& tracks = current->snapshot().tracks;
  const auto index = static_cast<int>(
      std::ranges::find(tracks, result.value(), &project::Track::id) - tracks.begin());
  commit_();
  created_(index);
  report_(QStringLiteral("Track duplicated · revision %1 · Undo is available")
              .arg(current->snapshot().revision));
  return QString::fromStdString(result.value().to_string());
}

bool TrackCommands::move_track(const QString& track_id, int to_index) {
  auto* current = editable_session();
  if (!current)
    return false;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed || to_index < 0 ||
      static_cast<std::size_t>(to_index) >= current->snapshot().tracks.size()) {
    report_(QStringLiteral("Choose a valid track position"));
    return false;
  }
  const auto moved =
      current->tracks().move_track(parsed.value(), static_cast<std::size_t>(to_index));
  if (!moved) {
    failure(QStringLiteral("Move track"), moved.error());
    return false;
  }
  commit_();
  report_(QStringLiteral("Track moved · revision %1 · Undo is available")
              .arg(current->snapshot().revision));
  return true;
}

void TrackCommands::set_track_muted(const QString& track_id, bool muted) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    report_(QStringLiteral("Select a valid track to mute"));
    return;
  }
  const auto result = current->tracks().set_track_muted(parsed.value(), muted);
  if (!result)
    return failure(QStringLiteral("Track mute"), result.error());
  accepted(muted ? QStringLiteral("Track muted") : QStringLiteral("Track unmuted"));
}

void TrackCommands::set_track_soloed(const QString& track_id, bool soloed) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    report_(QStringLiteral("Select a valid track to solo"));
    return;
  }
  const auto result = current->tracks().set_track_soloed(parsed.value(), soloed);
  if (!result)
    return failure(QStringLiteral("Track solo"), result.error());
  accepted(soloed ? QStringLiteral("Track soloed") : QStringLiteral("Track unsoloed"));
}

void TrackCommands::lock_kick_and_bass(const QString& track_id, bool on) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    report_(QStringLiteral("Select a Nova bass track to lock"));
    return;
  }
  const auto revision = current->snapshot().revision;
  const auto result = current->instruments().lock_kick_and_bass(parsed.value(), on);
  if (!result)
    return failure(on ? QStringLiteral("Lock kick & bass") : QStringLiteral("Unlock kick & bass"),
                   result.error());
  if (current->snapshot().revision == revision)
    return;
  accepted(on ? QStringLiteral("Kick & bass locked") : QStringLiteral("Kick & bass unlocked"));
}

void TrackCommands::set_track_level_pan(const QString& track_id, double level, double pan) {
  set_track_level_pan_impl(track_id, level, pan, std::nullopt);
}

void TrackCommands::set_track_level_pan_impl(const QString& track_id, double level, double pan,
                                             std::optional<std::uint64_t> gesture) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    report_(QStringLiteral("Select a valid track for channel controls"));
    return;
  }
  const auto result = current->tracks().set_track_level_pan(parsed.value(), level, pan, gesture);
  if (!result)
    return failure(QStringLiteral("Channel control"), result.error());
  accepted(QStringLiteral("Channel level/pan"));
}

void TrackCommands::set_master_level(double level) { set_master_level_impl(level, std::nullopt); }

void TrackCommands::set_master_level_impl(double level, std::optional<std::uint64_t> gesture) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto result = current->tracks().set_master_level(level, gesture);
  if (!result)
    return failure(QStringLiteral("Master level"), result.error());
  accepted(QStringLiteral("Master level"));
}

qulonglong TrackCommands::begin_channel_gesture() {
  const auto next = next_gesture_++;
  if (next_gesture_ == 0)
    next_gesture_ = 1;
  return next;
}

void TrackCommands::set_track_level_pan_gesture(const QString& track_id, double level, double pan,
                                                qulonglong gesture) {
  if (gesture == 0) {
    report_(QStringLiteral("Select a valid track and channel gesture"));
    return;
  }
  set_track_level_pan_impl(track_id, level, pan, gesture);
}

void TrackCommands::set_master_level_gesture(double level, qulonglong gesture) {
  if (gesture == 0) {
    report_(QStringLiteral("Select a valid master gesture"));
    return;
  }
  set_master_level_impl(level, gesture);
}

} // namespace oscillate::quick
