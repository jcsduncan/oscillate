#include "project_bridge.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::quick {
namespace {

[[nodiscard]] std::optional<project::SignalPathId>
editable_effect_path(const project::Snapshot& snapshot, const QString& track_id) {
  if (track_id == QStringLiteral("master"))
    return project::MasterSignalPath{};
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed)
    return std::nullopt;
  const auto found = std::ranges::find_if(snapshot.tracks, [&](const auto& track) {
    return track.id == parsed.value() &&
           (track.kind == project::TrackKind::audio ||
            (track.kind == project::TrackKind::instrument &&
             (std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source) ||
              std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source))) ||
            track.kind == project::TrackKind::group_bus ||
            track.kind == project::TrackKind::return_bus);
  });
  return found == snapshot.tracks.end() ? std::nullopt
                                        : std::optional<project::SignalPathId>{parsed.value()};
}

[[nodiscard]] const std::vector<project::EffectInsert>*
effect_slots(const project::Snapshot& snapshot, const project::SignalPathId& path) {
  if (std::holds_alternative<project::MasterSignalPath>(path))
    return &snapshot.master_effects;
  const auto owner =
      std::ranges::find(snapshot.tracks, std::get<project::TrackId>(path), &project::Track::id);
  return owner == snapshot.tracks.end() ? nullptr : &owner->effects;
}

} // namespace

bool ProjectBridge::insert_chain_edit_enabled() const {
  return session_ && !playback_busy() && !export_busy() && !import_busy() && !vst3_browser_busy() &&
         !audio_output_->audio_device_busy() && !close_after_playback_ && !closing_after_export() &&
         !imports_->audio().closing();
}

// While playing, any chain takes any edit (add, remove, reorder, switch,
// built-in or VST3): the changed chain switches live (ADR-0050).
bool ProjectBridge::live_chain_structure_edit_enabled(const QString& track_id) const {
  if (!live_routed_insert_edit_enabled())
    return false;
  const auto& snapshot = session_->snapshot();
  const auto path = editable_effect_path(snapshot, track_id);
  return path && effect_slots(snapshot, *path) != nullptr;
}

bool ProjectBridge::chain_edit_enabled(const QString& path_id) const {
  return insert_chain_edit_enabled() || live_chain_structure_edit_enabled(path_id);
}

bool ProjectBridge::live_routed_insert_edit_enabled() const {
  return session_ && routed_playback_ && !routed_stop_completion_ && !routed_playback_->paused() &&
         !routed_completion_ && !routed_transport_completion_ && !export_busy() && !import_busy() &&
         !managed_media_busy() && !close_after_playback_ && !closing_after_export() &&
         !imports_->audio().closing();
}

void ProjectBridge::finish_insert_chain_edit(const foundation::Result<void>& result,
                                             const QString& action) {
  if (!result) {
    set_status(QStringLiteral("Insert chain edit rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  schedule_background_save();
  if (routed_playback_)
    submit_playback_edit();
  else
    set_playback_status(
        QStringLiteral("Playback stopped; project insert revision %1 is not active. "
                       "Play will validate the selected chain.")
            .arg(revision_));
  set_status(QStringLiteral("%1 · revision %2 · save queued").arg(action).arg(revision_));
}

// QML passes the selected path and stable slot identity as separate strings.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ProjectBridge::move_effect_insert(const QString& track_id, const QString& slot_id, int index) {
  if (!insert_chain_edit_enabled() && !live_chain_structure_edit_enabled(track_id)) {
    set_status(QStringLiteral("Stop playback and finish current work before editing inserts"));
    return;
  }
  const auto path = editable_effect_path(session_->snapshot(), track_id);
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!path || !slot || index < 0) {
    set_status(QStringLiteral("Select a saved insert and valid position"));
    return;
  }
  finish_insert_chain_edit(
      session_->effects().move_effect_insert(*path, slot.value(), static_cast<std::size_t>(index)),
      QStringLiteral("Insert order changed"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ProjectBridge::set_effect_insert_enabled(const QString& track_id, const QString& slot_id,
                                              bool enabled) {
  if (!insert_chain_edit_enabled() && !live_chain_structure_edit_enabled(track_id)) {
    set_status(QStringLiteral("Stop playback and finish current work before editing inserts"));
    return;
  }
  const auto path = editable_effect_path(session_->snapshot(), track_id);
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!path || !slot) {
    set_status(QStringLiteral("Select a saved insert"));
    return;
  }
  finish_insert_chain_edit(
      session_->effects().set_effect_insert_enabled(*path, slot.value(), enabled),
      enabled ? QStringLiteral("Insert enabled") : QStringLiteral("Insert bypassed"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ProjectBridge::remove_effect_insert(const QString& track_id, const QString& slot_id) {
  if (!insert_chain_edit_enabled() && !live_chain_structure_edit_enabled(track_id)) {
    set_status(QStringLiteral("Stop playback and finish current work before editing inserts"));
    return;
  }
  const auto path = editable_effect_path(session_->snapshot(), track_id);
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!path || !slot) {
    set_status(QStringLiteral("Select a saved insert"));
    return;
  }
  finish_insert_chain_edit(session_->effects().remove_effect_insert(*path, slot.value()),
                           QStringLiteral("Insert removed"));
}

} // namespace oscillate::quick
