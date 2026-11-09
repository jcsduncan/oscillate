#include "project_bridge.hpp"

#include "project_bridge_projection.hpp"

#include "oscillate/application/midi_note_tools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

// Selection and clipboard: the selected clips, tracks and notes, and the
// copy, cut, paste, duplicate, move, nudge, delete and merge edits on them.
namespace oscillate::quick {
namespace {
using application::midi_grid_ticks;
using application::supported_midi_grid;
using detail::tick_at_bars;

template <typename Id> [[nodiscard]] std::vector<Id> parse_ids(const QStringList& texts) {
  std::vector<Id> ids;
  for (const auto& text : texts) {
    if (const auto parsed = Id::parse(text.toStdString()))
      ids.push_back(parsed.value());
  }
  return ids;
}

} // namespace

void ProjectBridge::set_selected_clip_ids(const QStringList& ids) {
  if (selected_clip_ids_ == ids)
    return;
  selected_clip_ids_ = ids;
  emit selectionChanged();
}

void ProjectBridge::set_selected_track_id(const QString& id) {
  if (selected_track_id_ == id)
    return;
  selected_track_id_ = id;
  emit selectionChanged();
}

void ProjectBridge::set_selected_note_clip_id(const QString& id) {
  if (selected_note_clip_id_ == id)
    return;
  selected_note_clip_id_ = id;
  emit selectionChanged();
}

void ProjectBridge::set_selected_note_ids(const QStringList& ids) {
  if (selected_note_ids_ == ids)
    return;
  selected_note_ids_ = ids;
  emit selectionChanged();
}

void ProjectBridge::copy_selection() {
  if (!session_)
    return;
  const auto note_clip = project::ClipId::parse(selected_note_clip_id_.toStdString());
  const auto notes = parse_ids<project::NoteId>(selected_note_ids_);
  if (note_clip && !notes.empty()) {
    const auto copied = session_->clipboard().copy_notes(note_clip.value(), notes);
    set_status(
        copied ? QStringLiteral("%1 notes copied").arg(static_cast<qulonglong>(notes.size()))
               : QStringLiteral("Copy: %1").arg(QString::fromStdString(copied.error().message)));
    return;
  }
  const auto clips = parse_ids<project::ClipId>(selected_clip_ids_);
  if (clips.empty()) {
    set_status(QStringLiteral("Select clips or notes to copy"));
    return;
  }
  const auto copied = session_->clipboard().copy_clips(clips);
  set_status(copied
                 ? QStringLiteral("%1 clips copied").arg(static_cast<qulonglong>(clips.size()))
                 : QStringLiteral("Copy: %1").arg(QString::fromStdString(copied.error().message)));
}

void ProjectBridge::remove_notes(const QString& clip_id, const QStringList& note_ids) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI notes"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto notes = parse_ids<project::NoteId>(note_ids);
  if (!clip || notes.empty()) {
    set_status(QStringLiteral("Select MIDI notes to delete"));
    return;
  }
  const auto result = session_->notes().remove_notes(clip.value(), notes);
  if (!result) {
    set_status(
        QStringLiteral("MIDI notes: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  set_selected_note_ids({});
  refresh();
  submit_playback_edit();
  set_status(notes.size() == 1 ? QStringLiteral("Note deleted")
                               : QStringLiteral("%1 notes deleted").arg(notes.size()));
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): QML calls this positionally.
void ProjectBridge::move_notes_grid(const QString& clip_id, const QStringList& note_ids,
                                    int step_delta, int key_delta, int steps_per_bar) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI notes"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto notes = parse_ids<project::NoteId>(note_ids);
  if (!clip || notes.empty() || !supported_midi_grid(steps_per_bar)) {
    set_status(QStringLiteral("Select MIDI notes to move"));
    return;
  }
  const auto delta_ticks = static_cast<std::int64_t>(step_delta) * midi_grid_ticks(steps_per_bar);
  const auto before = session_->snapshot().revision;
  const auto result = session_->notes().move_notes(clip.value(), notes, delta_ticks, key_delta);
  if (!result) {
    set_status(QStringLiteral("MIDI notes not moved: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  if (session_->snapshot().revision != before) {
    refresh();
    submit_playback_edit();
  }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): QML calls this positionally.
QStringList ProjectBridge::copy_notes_grid(const QString& clip_id, const QStringList& note_ids,
                                           int step_delta, int key_delta, int steps_per_bar) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI notes"));
    return {};
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto notes = parse_ids<project::NoteId>(note_ids);
  if (!clip || notes.empty() || !supported_midi_grid(steps_per_bar)) {
    set_status(QStringLiteral("Select MIDI notes to copy"));
    return {};
  }
  const auto delta_ticks = static_cast<std::int64_t>(step_delta) * midi_grid_ticks(steps_per_bar);
  const auto copies = session_->notes().copy_notes_by(clip.value(), notes, delta_ticks, key_delta);
  if (!copies) {
    set_status(QStringLiteral("MIDI notes not copied: %1")
                   .arg(QString::fromStdString(copies.error().message)));
    return {};
  }
  refresh();
  submit_playback_edit();
  QStringList ids;
  for (const auto& id : copies.value())
    ids.append(QString::fromStdString(id.to_string()));
  return ids;
}

void ProjectBridge::cut_selection() {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before cutting"));
    return;
  }
  const auto note_clip = project::ClipId::parse(selected_note_clip_id_.toStdString());
  const auto notes = parse_ids<project::NoteId>(selected_note_ids_);
  if (note_clip && !notes.empty()) {
    const auto copied = session_->clipboard().copy_notes(note_clip.value(), notes);
    if (!copied) {
      set_status(QStringLiteral("Cut: %1").arg(QString::fromStdString(copied.error().message)));
      return;
    }
    // One undo step for the whole cut.
    const auto removed = session_->notes().remove_notes(note_clip.value(), notes);
    if (!removed) {
      set_status(QStringLiteral("Cut: %1").arg(QString::fromStdString(removed.error().message)));
      return;
    }
    set_selected_note_ids({});
    refresh();
    submit_playback_edit();
    return;
  }
  const auto clips = parse_ids<project::ClipId>(selected_clip_ids_);
  if (clips.empty()) {
    set_status(QStringLiteral("Select clips or notes to cut"));
    return;
  }
  const auto cut = session_->clipboard().cut_clips(clips);
  if (!cut) {
    set_status(QStringLiteral("Cut: %1").arg(QString::fromStdString(cut.error().message)));
    return;
  }
  set_selected_clip_ids({});
  refresh();
  submit_audio_clip_edit();
  set_status(
      QStringLiteral("%1 clips cut · Undo to restore").arg(static_cast<qulonglong>(clips.size())));
}

void ProjectBridge::paste_at_playhead() {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before pasting"));
    return;
  }
  const auto& snapshot = session_->snapshot();
  const auto playhead = snapshot.tempo.sample_to_tick(
      {static_cast<std::int64_t>(std::min<std::uint64_t>(
          playhead_sample_, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))},
      snapshot.sample_rate);
  if (!playhead) {
    set_status(QStringLiteral("Paste: the playhead position is invalid"));
    return;
  }
  const auto note_clip = project::ClipId::parse(selected_note_clip_id_.toStdString());
  if (note_clip && session_->clipboard().clipboard_has_notes()) {
    // Notes paste at the playhead's position inside the clip.
    std::optional<std::int64_t> clip_start;
    for (const auto& track : snapshot.tracks) {
      for (const auto& clip : track.midi_clips) {
        if (clip.id != note_clip.value())
          continue;
        if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
          clip_start = musical->start.value;
        } else if (const auto start = snapshot.tempo.sample_to_tick(
                       std::get<project::ElapsedInterval>(clip.placement).start,
                       snapshot.sample_rate)) {
          clip_start = start.value().value;
        }
      }
    }
    const auto local = std::max<std::int64_t>(0, playhead.value().value - clip_start.value_or(0));
    const auto pasted = session_->clipboard().paste_notes(note_clip.value(), {local});
    if (!pasted) {
      set_status(QStringLiteral("Paste: %1").arg(QString::fromStdString(pasted.error().message)));
      return;
    }
    QStringList ids;
    for (const auto& id : pasted.value())
      ids.push_back(QString::fromStdString(id.to_string()));
    set_selected_note_ids(ids);
    refresh();
    submit_playback_edit();
    return;
  }
  if (!session_->clipboard().clipboard_has_clips()) {
    set_status(QStringLiteral("Nothing to paste"));
    return;
  }
  const auto track = project::TrackId::parse(selected_track_id_.toStdString());
  if (!track) {
    set_status(QStringLiteral("Select a track to paste onto"));
    return;
  }
  const auto pasted = session_->clipboard().paste_clips(track.value(), playhead.value());
  if (!pasted) {
    set_status(QStringLiteral("Paste: %1").arg(QString::fromStdString(pasted.error().message)));
    return;
  }
  QStringList ids;
  for (const auto& id : pasted.value())
    ids.push_back(QString::fromStdString(id.to_string()));
  set_selected_clip_ids(ids);
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("%1 clips pasted").arg(ids.size()));
}

void ProjectBridge::duplicate_selection() {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before duplicating"));
    return;
  }
  const auto note_clip = project::ClipId::parse(selected_note_clip_id_.toStdString());
  const auto notes = parse_ids<project::NoteId>(selected_note_ids_);
  if (note_clip && !notes.empty()) {
    const auto duplicated = session_->clipboard().duplicate_notes(note_clip.value(), notes);
    if (!duplicated) {
      set_status(
          QStringLiteral("Duplicate: %1").arg(QString::fromStdString(duplicated.error().message)));
      return;
    }
    QStringList ids;
    for (const auto& id : duplicated.value())
      ids.push_back(QString::fromStdString(id.to_string()));
    set_selected_note_ids(ids);
    refresh();
    submit_playback_edit();
    return;
  }
  const auto clips = parse_ids<project::ClipId>(selected_clip_ids_);
  if (clips.empty()) {
    set_status(QStringLiteral("Select clips or notes to duplicate"));
    return;
  }
  const auto duplicated = session_->clipboard().duplicate_clips(clips);
  if (!duplicated) {
    set_status(
        QStringLiteral("Duplicate: %1").arg(QString::fromStdString(duplicated.error().message)));
    return;
  }
  QStringList ids;
  for (const auto& id : duplicated.value())
    ids.push_back(QString::fromStdString(id.to_string()));
  set_selected_clip_ids(ids);
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("%1 clips duplicated").arg(ids.size()));
}

void ProjectBridge::move_selected_clips(double from, double to, int track_delta) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before moving clips"));
    return;
  }
  const auto clips = parse_ids<project::ClipId>(selected_clip_ids_);
  if (clips.empty() || !std::isfinite(from) || !std::isfinite(to) || timeline_bars_ < 1) {
    set_status(QStringLiteral("Select clips to move"));
    return;
  }
  const auto bars = static_cast<double>(timeline_bars_);
  const auto& snapshot = session_->snapshot();
  const auto start = tick_at_bars(snapshot, std::clamp(from * bars, 0.0, bars));
  const auto end = tick_at_bars(snapshot, std::clamp(to * bars, 0.0, bars));
  if (!start || !end) {
    set_status(QStringLiteral("Move rejected: that position is outside the meter"));
    return;
  }
  const auto before = snapshot.revision;
  const auto moved =
      session_->clipboard().move_clips(clips, end.value().value - start.value().value, track_delta);
  if (!moved) {
    set_status(
        QStringLiteral("Move rejected: %1").arg(QString::fromStdString(moved.error().message)));
    return;
  }
  refresh();
  if (before == revision_)
    return;
  submit_audio_clip_edit();
  set_status(QStringLiteral("%1 clips moved").arg(static_cast<qulonglong>(clips.size())));
}

void ProjectBridge::copy_clips_to(const QStringList& clip_ids, double from, double to,
                                  int track_delta) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before copying clips"));
    return;
  }
  const auto clips = parse_ids<project::ClipId>(clip_ids);
  if (clips.empty() || !std::isfinite(from) || !std::isfinite(to) || timeline_bars_ < 1) {
    set_status(QStringLiteral("Select clips to copy"));
    return;
  }
  // An audio drag may drop left of bar 1; a copy there is refused, not clamped.
  if (to < 0.0) {
    set_status(QStringLiteral("Copy rejected: the copies cannot start before bar 1"));
    return;
  }
  const auto bars = static_cast<double>(timeline_bars_);
  const auto& snapshot = session_->snapshot();
  const auto start = tick_at_bars(snapshot, std::clamp(from * bars, 0.0, bars));
  const auto end = tick_at_bars(snapshot, std::clamp(to * bars, 0.0, bars));
  if (!start || !end) {
    set_status(QStringLiteral("Copy rejected: that position is outside the meter"));
    return;
  }
  const auto copied = session_->clipboard().copy_clips_by(
      clips, end.value().value - start.value().value, track_delta);
  if (!copied) {
    set_status(
        QStringLiteral("Copy rejected: %1").arg(QString::fromStdString(copied.error().message)));
    return;
  }
  QStringList ids;
  for (const auto& id : copied.value())
    ids.push_back(QString::fromStdString(id.to_string()));
  set_selected_clip_ids(ids);
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("%1 clips copied").arg(ids.size()));
}

void ProjectBridge::nudge_selected_clips(int beat_direction) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before moving clips"));
    return;
  }
  const auto clips = parse_ids<project::ClipId>(selected_clip_ids_);
  if (clips.empty() || (beat_direction != -1 && beat_direction != 1)) {
    set_status(QStringLiteral("Select clips and a move direction"));
    return;
  }
  const auto moved =
      session_->clipboard().move_clips(clips, beat_direction * timeline::ticks_per_quarter, 0);
  if (!moved) {
    set_status(
        QStringLiteral("Move rejected: %1").arg(QString::fromStdString(moved.error().message)));
    return;
  }
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("%1 clips moved").arg(static_cast<qulonglong>(clips.size())));
}

void ProjectBridge::delete_selected_clips() {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before deleting clips"));
    return;
  }
  const auto clips = parse_ids<project::ClipId>(selected_clip_ids_);
  const auto removed = session_->clipboard().remove_clips(clips);
  if (!removed) {
    set_status(QStringLiteral("Delete: %1").arg(QString::fromStdString(removed.error().message)));
    return;
  }
  set_selected_clip_ids({});
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("%1 clips deleted · Undo to restore")
                 .arg(static_cast<qulonglong>(clips.size())));
}

void ProjectBridge::merge_selected_clips() {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before merging clips"));
    return;
  }
  const auto clips = parse_ids<project::ClipId>(selected_clip_ids_);
  const auto merged = session_->midi_clips().merge_midi_clips(clips);
  if (!merged) {
    set_status(QStringLiteral("Merge: %1").arg(QString::fromStdString(merged.error().message)));
    return;
  }
  set_selected_clip_ids({QString::fromStdString(merged.value().to_string())});
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("%1 clips merged into one").arg(static_cast<qulonglong>(clips.size())));
}

} // namespace oscillate::quick
