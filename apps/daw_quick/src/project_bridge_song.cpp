#include "project_bridge.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

// Song-level edits: markers, the project key, and track and clip names,
// colours and send levels.
namespace oscillate::quick {
namespace {

// "#rrggbb" -> 0xRRGGBB; empty -> no colour; anything else is rejected.
[[nodiscard]] std::optional<std::optional<std::uint32_t>> parse_color(const QString& text) {
  if (text.isEmpty())
    return std::optional<std::uint32_t>{};
  if (text.size() != 7 || text.front() != QLatin1Char('#'))
    return std::nullopt;
  bool ok = false;
  const auto value = text.mid(1).toUInt(&ok, 16);
  if (!ok)
    return std::nullopt;
  return std::optional<std::uint32_t>{static_cast<std::uint32_t>(value)};
}

} // namespace

QVariantList ProjectBridge::markers() const {
  QVariantList result;
  if (!session_)
    return result;
  const auto& snapshot = session_->snapshot();
  for (const auto& marker : snapshot.markers) {
    const auto sample = snapshot.tempo.tick_to_sample(marker.position, snapshot.sample_rate);
    // Zero-based, fractional bars, as the arrangement lays clips out.
    double bars = 0.0;
    if (const auto located = snapshot.meter.locate(marker.position);
        located && located.value().bar >= 1) {
      const auto bar = located.value().bar;
      const auto begin = snapshot.meter.tick_at({bar, 1, 0});
      const auto next = snapshot.meter.tick_at({bar + 1, 1, 0});
      bars = static_cast<double>(bar - 1);
      if (begin && next && next.value().value > begin.value().value)
        bars += static_cast<double>(marker.position.value - begin.value().value) /
                static_cast<double>(next.value().value - begin.value().value);
    }
    result.push_back(QVariantMap{
        {"id", QString::fromStdString(marker.id.to_string())},
        {"bar", bars},
        {"name", QString::fromStdString(marker.name)},
        {"ticks", static_cast<qlonglong>(marker.position.value)},
        {"sample", sample ? static_cast<qlonglong>(sample.value().value) : qlonglong{-1}}});
  }
  return result;
}

QString ProjectBridge::project_key() const {
  return session_ && session_->snapshot().key ? key_name(*session_->snapshot().key, true)
                                              : QString();
}

QVariantList ProjectBridge::project_scale() const {
  QVariantList scale;
  if (!session_ || !session_->snapshot().key)
    return scale;
  const auto key = *session_->snapshot().key;
  // Major, or natural minor.
  static constexpr std::array<int, 7> major{0, 2, 4, 5, 7, 9, 11};
  static constexpr std::array<int, 7> minor{0, 2, 3, 5, 7, 8, 10};
  for (const int step : key.mode == project::KeyMode::major ? major : minor)
    scale.append((static_cast<int>(key.tonic) + step) % 12);
  return scale;
}

QString ProjectBridge::suggested_project_key() const {
  const auto suggested = session_ ? session_->song().suggested_project_key() : std::nullopt;
  return suggested ? key_name(*suggested, true) : QString();
}

void ProjectBridge::set_project_key(int tonic, const QString& mode) {
  if (!session_)
    return;
  if (tonic < 0 || tonic > 11 ||
      (mode != QStringLiteral("major") && mode != QStringLiteral("minor"))) {
    set_status(QStringLiteral("Choose a key from C to B, major or minor"));
    return;
  }
  const auto applied = session_->song().set_project_key(project::MusicalKey{
      static_cast<std::uint8_t>(tonic),
      mode == QStringLiteral("major") ? project::KeyMode::major : project::KeyMode::minor});
  if (!applied) {
    set_status(QStringLiteral("Key: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  refresh();
  set_status(QStringLiteral("Project key %1").arg(project_key()));
}

void ProjectBridge::use_suggested_project_key() {
  if (!session_)
    return;
  const auto suggested = session_->song().suggested_project_key();
  if (!suggested) {
    set_status(QStringLiteral("No key has been detected in this project's audio"));
    return;
  }
  set_project_key(suggested->tonic, suggested->mode == project::KeyMode::major
                                        ? QStringLiteral("major")
                                        : QStringLiteral("minor"));
}

void ProjectBridge::clear_project_key() {
  if (!session_ || !session_->snapshot().key)
    return;
  const auto applied = session_->song().set_project_key(std::nullopt);
  if (!applied) {
    set_status(QStringLiteral("Key: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  refresh();
  set_status(QStringLiteral("Project key cleared"));
}

void ProjectBridge::add_marker_at_playhead() {
  if (!session_)
    return;
  const auto& snapshot = session_->snapshot();
  const auto tick = snapshot.tempo.sample_to_tick(
      {static_cast<std::int64_t>(std::min<std::uint64_t>(
          playhead_sample_, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))},
      snapshot.sample_rate);
  if (!tick) {
    set_status(QStringLiteral("Marker: the playhead position is invalid"));
    return;
  }
  const auto name =
      QStringLiteral("Marker %1").arg(static_cast<qulonglong>(snapshot.markers.size() + 1));
  const auto added = session_->song().add_marker(tick.value(), name.toStdString());
  if (!added) {
    set_status(QStringLiteral("Marker: %1").arg(QString::fromStdString(added.error().message)));
    return;
  }
  refresh();
}

void ProjectBridge::jump_to_marker(int direction) {
  if (!session_ || direction == 0)
    return;
  const auto& snapshot = session_->snapshot();
  const auto tick = snapshot.tempo.sample_to_tick(
      {static_cast<std::int64_t>(std::min<std::uint64_t>(
          playhead_sample_, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))},
      snapshot.sample_rate);
  if (!tick)
    return;
  const auto marker = direction > 0 ? session_->song().next_marker(tick.value())
                                    : session_->song().previous_marker(tick.value());
  if (!marker) {
    set_status(direction > 0 ? QStringLiteral("No marker after the playhead")
                             : QStringLiteral("No marker before the playhead"));
    return;
  }
  const auto sample = snapshot.tempo.tick_to_sample(marker->position, snapshot.sample_rate);
  if (sample && sample.value().value >= 0)
    seek_to_sample(static_cast<qulonglong>(sample.value().value));
}

void ProjectBridge::rename_marker(const QString& marker_id, const QString& name) {
  const auto marker = project::MarkerId::parse(marker_id.toStdString());
  if (!session_ || !marker)
    return;
  const auto renamed = session_->song().rename_marker(marker.value(), name.toStdString());
  if (!renamed) {
    set_status(QStringLiteral("Marker: %1").arg(QString::fromStdString(renamed.error().message)));
    return;
  }
  refresh();
}

void ProjectBridge::remove_marker(const QString& marker_id) {
  const auto marker = project::MarkerId::parse(marker_id.toStdString());
  if (!session_ || !marker)
    return;
  const auto removed = session_->song().remove_marker(marker.value());
  if (!removed) {
    set_status(QStringLiteral("Marker: %1").arg(QString::fromStdString(removed.error().message)));
    return;
  }
  refresh();
}

void ProjectBridge::set_track_color(const QString& track_id, const QString& color) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto parsed = parse_color(color);
  if (!session_ || !track || !parsed) {
    set_status(QStringLiteral("Choose a track and a #rrggbb colour"));
    return;
  }
  const auto applied = session_->tracks().set_track_color(track.value(), *parsed);
  if (!applied) {
    set_status(QStringLiteral("Colour: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  refresh();
}

void ProjectBridge::set_clip_color(const QString& clip_id, const QString& color) {
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto parsed = parse_color(color);
  if (!session_ || !clip || !parsed) {
    set_status(QStringLiteral("Choose a clip and a #rrggbb colour"));
    return;
  }
  const auto applied = session_->audio_clips().set_clip_color(clip.value(), *parsed);
  if (!applied) {
    set_status(QStringLiteral("Colour: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  refresh();
}

void ProjectBridge::set_audio_clip_name(const QString& clip_id, const QString& name) {
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!session_ || !clip)
    return;
  const auto applied =
      session_->audio_clips().set_audio_clip_name(clip.value(), name.trimmed().toStdString());
  if (!applied) {
    set_status(QStringLiteral("Name: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  refresh();
}

void ProjectBridge::set_track_name(const QString& track_id, const QString& name) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!session_ || !track)
    return;
  const auto applied =
      session_->tracks().set_track_name(track.value(), name.trimmed().toStdString());
  if (!applied) {
    set_status(QStringLiteral("Name: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  refresh();
}

void ProjectBridge::set_send_level(const QString& track_id, const QString& return_id, double gain) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto destination = project::TrackId::parse(return_id.toStdString());
  if (!session_ || !track || !destination || !std::isfinite(gain))
    return;
  const auto applied = session_->routing().set_send_level(track.value(), destination.value(),
                                                          std::clamp(gain, 0.0, 4.0));
  if (!applied) {
    set_status(QStringLiteral("Send: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
}

} // namespace oscillate::quick
