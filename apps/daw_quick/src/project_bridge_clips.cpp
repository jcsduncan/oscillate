#include "project_bridge.hpp"

#include "project_bridge_projection.hpp"

#include "oscillate/application/midi_note_tools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace oscillate::quick {

namespace detail {

std::optional<timeline::TickPosition> tick_at_bars(const project::Snapshot& snapshot, double bars) {
  // To a 2^-20 bar grid: every power-of-two grid line lands exactly, and
  // rounding in the timeline fraction cannot leave a clip a few ticks short.
  bars = std::round(bars * 1'048'576.0) / 1'048'576.0;
  const auto whole = static_cast<int>(std::floor(bars));
  const auto begin = snapshot.meter.tick_at({whole + 1, 1, 0});
  if (!begin)
    return std::nullopt;
  const double fraction = bars - static_cast<double>(whole);
  if (fraction <= 0.0)
    return begin.value();
  const auto next = snapshot.meter.tick_at({whole + 2, 1, 0});
  if (!next)
    return std::nullopt;
  const auto span = static_cast<double>(next.value().value - begin.value().value);
  return timeline::TickPosition{begin.value().value + std::llround(fraction * span)};
}

} // namespace detail

namespace {
using application::midi_grid_ticks;
using application::supported_midi_grid;
using detail::tick_at_bars;

} // namespace

void ProjectBridge::toggle_note_grid(const QString& clip_id, int step, int key, int steps_per_bar) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI notes"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  // Steps count from the clip's start; the session keeps notes in the pattern.
  if (!parsed || !supported_midi_grid(steps_per_bar) || step < 0 || key < 0 || key > 127) {
    set_status(QStringLiteral("Select a MIDI clip, grid cell and pitch"));
    return;
  }
  const auto duration = midi_grid_ticks(steps_per_bar);
  const auto start = timeline::TickPosition{step * duration};
  std::optional<project::NoteId> existing;
  for (const auto& track : session_->snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, parsed.value(), &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    const auto note = std::ranges::find_if(found->notes, [start, key](const auto& item) {
      return item.start == start && item.key == key;
    });
    if (note != found->notes.end())
      existing = note->id;
    break;
  }
  const auto result =
      existing ? session_->notes().remove_note(parsed.value(), *existing)
               : session_->notes().add_note_with_duration(parsed.value(), start, {duration},
                                                          static_cast<std::uint8_t>(key));
  if (!result) {
    set_status(QStringLiteral("MIDI note: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(existing ? QStringLiteral("MIDI note removed · Undo to restore")
                      : QStringLiteral("MIDI note added · Save to keep"));
}

void ProjectBridge::move_note_grid(const QString& clip_id, const QString& note_id, int to_step,
                                   int to_key, int steps_per_bar) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI notes"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto note = project::NoteId::parse(note_id.toStdString());
  if (!clip || !note || !supported_midi_grid(steps_per_bar) || to_step < 0 || to_key < 0 ||
      to_key > 127) {
    set_status(QStringLiteral("Select a MIDI note and destination cell"));
    return;
  }
  const auto start = timeline::TickPosition{to_step * midi_grid_ticks(steps_per_bar)};
  const auto before = session_->snapshot().revision;
  const auto result = session_->notes().move_note(clip.value(), note.value(), start,
                                                  static_cast<std::uint8_t>(to_key));
  if (!result) {
    set_status(QStringLiteral("MIDI note move rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  if (session_->snapshot().revision != before) {
    refresh();
    submit_playback_edit();
  }
}

void ProjectBridge::remove_note_id(const QString& clip_id, const QString& note_id) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI notes"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto note = project::NoteId::parse(note_id.toStdString());
  if (!clip || !note) {
    set_status(QStringLiteral("Select a MIDI note"));
    return;
  }
  const auto result = session_->notes().remove_note(clip.value(), note.value());
  if (!result) {
    set_status(QStringLiteral("MIDI note: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
}

void ProjectBridge::resize_note_grid(const QString& clip_id, const QString& note_id, int end_step,
                                     int steps_per_bar) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before resizing a MIDI note"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto note = project::NoteId::parse(note_id.toStdString());
  if (!clip || !note || !supported_midi_grid(steps_per_bar) || end_step < 1) {
    set_status(QStringLiteral("Select a MIDI note and valid grid endpoint"));
    return;
  }
  const auto end = timeline::TickPosition{end_step * midi_grid_ticks(steps_per_bar)};
  const auto before = session_->snapshot().revision;
  const auto result = session_->notes().resize_note(clip.value(), note.value(), end);
  if (!result) {
    set_status(QStringLiteral("MIDI note resize rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  if (session_->snapshot().revision != before) {
    refresh();
    submit_playback_edit();
  }
}

void ProjectBridge::quantize_midi_clip(const QString& clip_id, int steps_per_bar) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before quantising MIDI"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip || !supported_midi_grid(steps_per_bar)) {
    set_status(QStringLiteral("Select a MIDI clip and grid division"));
    return;
  }
  const auto before = session_->snapshot().revision;
  const auto result = session_->midi_clips().quantize_midi_clip(clip.value(), steps_per_bar);
  if (!result) {
    set_status(QStringLiteral("Quantise: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  if (session_->snapshot().revision == before) {
    set_status(QStringLiteral("MIDI notes are already on this grid"));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI notes quantised · one Undo step · Save to keep"));
}

void ProjectBridge::move_midi_clip(const QString& clip_id, double normalized_start) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || !std::isfinite(normalized_start) || normalized_start < 0.0 ||
      normalized_start > 1.0 || timeline_bars_ < 1) {
    set_status(QStringLiteral("Select a MIDI clip and valid bar position"));
    return;
  }
  const auto tick =
      tick_at_bars(session_->snapshot(), std::clamp(normalized_start * timeline_bars_, 0.0,
                                                    static_cast<double>(timeline_bars_)));
  if (!tick) {
    set_status(QStringLiteral("MIDI move rejected: that position is outside the meter"));
    return;
  }
  const auto before = session_->snapshot().revision;
  auto result = session_->midi_clips().move_midi_clip(parsed.value(), tick.value());
  if (!result) {
    set_status(QStringLiteral("MIDI move rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_playback_edit();
  set_status(before == revision_
                 ? QStringLiteral("MIDI clip stayed at its original position")
                 : QStringLiteral("MIDI clip moved · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::move_midi_clip_to_track(const QString& clip_id, const QString& track_id,
                                            double normalized_start) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!clip || !track || !std::isfinite(normalized_start) || timeline_bars_ < 1) {
    set_status(QStringLiteral("Select a MIDI clip and an instrument track"));
    return;
  }
  const auto tick =
      tick_at_bars(session_->snapshot(), std::clamp(normalized_start * timeline_bars_, 0.0,
                                                    static_cast<double>(timeline_bars_)));
  if (!tick) {
    set_status(QStringLiteral("MIDI move rejected: that position is outside the meter"));
    return;
  }
  const auto result =
      session_->midi_clips().move_midi_clip_to_track(clip.value(), track.value(), tick.value());
  if (!result) {
    set_status(QStringLiteral("MIDI move rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI clip moved to another track"));
}

void ProjectBridge::nudge_midi_clip(const QString& clip_id, int beat_direction) {
  if (!session_ || beat_direction == 0)
    return;
  if (control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip)
    return;
  for (const auto& track : session_->snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip.value(), &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    std::optional<timeline::TickPosition> start;
    if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
      start = musical->start;
    } else if (const auto tick = session_->snapshot().tempo.sample_to_tick(
                   std::get<project::ElapsedInterval>(found->placement).start,
                   session_->snapshot().sample_rate)) {
      start = tick.value();
    }
    if (!start)
      return;
    const auto target = start->value + (beat_direction > 0 ? 1 : -1) * timeline::ticks_per_quarter;
    if (target < 0)
      return;
    const auto result = session_->midi_clips().move_midi_clip(clip.value(), {target});
    if (!result) {
      set_status(QStringLiteral("MIDI move rejected: %1")
                     .arg(QString::fromStdString(result.error().message)));
      return;
    }
    refresh();
    submit_playback_edit();
    return;
  }
}

void ProjectBridge::resize_midi_clip(const QString& clip_id, double normalized_end) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || !std::isfinite(normalized_end) || normalized_end < 0.0 || normalized_end > 1.0 ||
      timeline_bars_ < 1) {
    set_status(QStringLiteral("Select a MIDI clip and valid ending bar"));
    return;
  }
  const auto tick =
      tick_at_bars(session_->snapshot(), std::clamp(normalized_end * timeline_bars_, 0.0,
                                                    static_cast<double>(timeline_bars_)));
  if (!tick) {
    set_status(QStringLiteral("MIDI resize rejected: that position is outside the meter"));
    return;
  }
  const auto before = session_->snapshot().revision;
  auto result = session_->midi_clips().resize_midi_clip(parsed.value(), tick.value());
  if (!result) {
    set_status(QStringLiteral("MIDI resize rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  bool looping = false;
  for (const auto& track : session_->snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, parsed.value(), &project::MidiClip::id);
    if (found != track.midi_clips.end())
      looping = found->loop;
  }
  refresh();
  if (before != revision_)
    submit_playback_edit();
  set_status(before == revision_ ? QStringLiteral("MIDI clip length is unchanged")
             : looping ? QStringLiteral("MIDI clip loop resized · revision %1").arg(revision_)
                       : QStringLiteral("MIDI clip resized · revision %1").arg(revision_));
}

void ProjectBridge::trim_midi_clip_start(const QString& clip_id, double normalized_start) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || !std::isfinite(normalized_start) || normalized_start < 0.0 ||
      normalized_start > 1.0 || timeline_bars_ < 1) {
    set_status(QStringLiteral("Select a MIDI clip and valid starting bar"));
    return;
  }
  const auto tick =
      tick_at_bars(session_->snapshot(), std::clamp(normalized_start * timeline_bars_, 0.0,
                                                    static_cast<double>(timeline_bars_)));
  if (!tick) {
    set_status(QStringLiteral("MIDI trim rejected: that position is outside the meter"));
    return;
  }
  const auto before = session_->snapshot().revision;
  auto result = session_->midi_clips().trim_midi_clip_start(parsed.value(), tick.value());
  if (!result) {
    set_status(QStringLiteral("MIDI trim rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_playback_edit();
  set_status(before == revision_
                 ? QStringLiteral("MIDI clip start is unchanged")
                 : QStringLiteral("MIDI clip start trimmed · revision %1").arg(revision_));
}

void ProjectBridge::set_midi_clip_loop(const QString& clip_id, bool loop) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select a MIDI clip to loop"));
    return;
  }
  const auto before = session_->snapshot().revision;
  auto result = session_->midi_clips().set_midi_clip_loop(parsed.value(), loop);
  if (!result) {
    set_status(QStringLiteral("MIDI loop change rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  if (before == session_->snapshot().revision)
    return;
  refresh();
  submit_playback_edit();
  set_status(loop ? QStringLiteral("MIDI clip loops · drag its right edge to repeat it")
                  : QStringLiteral("MIDI clip loop off · drag its right edge to lengthen it"));
}

void ProjectBridge::duplicate_midi_clip_after(const QString& clip_id) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select a MIDI clip to duplicate"));
    return;
  }
  auto result = session_->midi_clips().duplicate_midi_clip_after(parsed.value());
  if (!result) {
    set_status(QStringLiteral("MIDI duplicate rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI clip duplicated · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::delete_midi_clip(const QString& clip_id) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select a MIDI clip to delete"));
    return;
  }
  auto result = session_->midi_clips().remove_midi_clip(parsed.value());
  if (!result) {
    set_status(QStringLiteral("MIDI delete rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI clip deleted · revision %1 · Undo to restore").arg(revision_));
}

void ProjectBridge::toggle_midi_clip_timebase(const QString& clip_id) {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing MIDI clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select a MIDI clip to change its timebase"));
    return;
  }
  auto result = session_->midi_clips().toggle_midi_clip_timebase(parsed.value());
  if (!result) {
    set_status(QStringLiteral("MIDI timebase change rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(
      QStringLiteral("MIDI clip timebase changed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::set_audio_clip_fade(const QString& clip_id, bool fade_in, double fraction,
                                        int curve_index) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before editing clip fades"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0 ||
      (curve_index != 0 && curve_index != 1)) {
    set_status(QStringLiteral("Invalid clip fade setting"));
    return;
  }
  const project::Clip* found = nullptr;
  for (const auto& track : session_->snapshot().tracks) {
    const auto clip = std::ranges::find(track.clips, parsed.value(), &project::Clip::id);
    if (clip != track.clips.end()) {
      found = &*clip;
      break;
    }
  }
  if (!found) {
    set_status(QStringLiteral("Select an audio clip to fade"));
    return;
  }
  auto fades = found->fades;
  const auto curve = curve_index == 1 ? project::FadeCurve::smoothstep : project::FadeCurve::linear;
  if (fade_in) {
    fades.in_fraction = fraction;
    fades.in_curve = curve;
  } else {
    fades.out_fraction = fraction;
    fades.out_curve = curve;
  }
  const auto before = session_->snapshot().revision;
  auto result = session_->audio_clips().set_audio_clip_fades(parsed.value(), fades);
  if (!result) {
    set_status(QStringLiteral("Audio fade rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(QStringLiteral("Audio fade edited · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::duplicate_audio_clip_after(const QString& clip_id) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before duplicating a clip"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to duplicate"));
    return;
  }
  auto duplicated = session_->audio_clips().duplicate_audio_clip_after(parsed.value());
  if (!duplicated) {
    set_status(QStringLiteral("Duplicate rejected: %1")
                   .arg(QString::fromStdString(duplicated.error().message)));
    return;
  }
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("Audio clip duplicated · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::split_audio_clip_at_playhead(const QString& clip_id) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before splitting a clip"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed ||
      playhead_sample_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    set_status(QStringLiteral("Select an audio clip and a valid split position"));
    return;
  }
  auto split = session_->audio_clips().split_audio_clip_at_sample(
      parsed.value(), {static_cast<std::int64_t>(playhead_sample_)});
  if (!split) {
    set_status(
        QStringLiteral("Split rejected: %1").arg(QString::fromStdString(split.error().message)));
    return;
  }
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("Audio clip split · revision %1 · Undo to rejoin").arg(revision_));
}

void ProjectBridge::split_midi_clip_at_playhead(const QString& clip_id) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before splitting a clip"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  const auto& snapshot = session_->snapshot();
  const auto tick =
      playhead_sample_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
          ? foundation::Result<timeline::TickPosition>::failure(
                {foundation::ErrorCode::invalid_argument, "invalid playhead"})
          : snapshot.tempo.sample_to_tick({static_cast<std::int64_t>(playhead_sample_)},
                                          snapshot.sample_rate);
  if (!parsed || !tick) {
    set_status(QStringLiteral("Select a MIDI clip and a valid split position"));
    return;
  }
  auto split = session_->midi_clips().split_midi_clip_at_tick(parsed.value(), tick.value());
  if (!split) {
    set_status(
        QStringLiteral("Split rejected: %1").arg(QString::fromStdString(split.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI clip split · Undo to rejoin"));
}

void ProjectBridge::slip_audio_clip(const QString& clip_id, double source_frame_delta) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before slipping a clip"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || !std::isfinite(source_frame_delta) ||
      source_frame_delta <= static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      source_frame_delta >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    set_status(QStringLiteral("Select an audio clip and a valid slip distance"));
    return;
  }
  const auto before = session_->snapshot().revision;
  auto slipped = session_->audio_clips().slip_audio_clip(
      parsed.value(), static_cast<std::int64_t>(std::llround(source_frame_delta)));
  if (!slipped) {
    set_status(
        QStringLiteral("Slip rejected: %1").arg(QString::fromStdString(slipped.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(
      before == revision_
          ? QStringLiteral("Audio clip source stayed in place")
          : QStringLiteral("Audio source slipped · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::toggle_audio_clip_timebase(const QString& clip_id) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before changing clip timebase"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to change its timebase"));
    return;
  }
  auto changed = session_->audio_clips().toggle_audio_clip_timebase(parsed.value());
  if (!changed) {
    set_status(QStringLiteral("Timebase change rejected: %1")
                   .arg(QString::fromStdString(changed.error().message)));
    return;
  }
  refresh();
  submit_audio_clip_edit();
  set_status(
      QStringLiteral("Audio clip timebase changed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::delete_audio_clip(const QString& clip_id) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral("Finish active project work before deleting a clip"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to delete"));
    return;
  }
  auto removed = session_->audio_clips().remove_audio_clip(parsed.value());
  if (!removed) {
    set_status(
        QStringLiteral("Delete rejected: %1").arg(QString::fromStdString(removed.error().message)));
    return;
  }
  select_audio_clip({});
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("Audio clip deleted · revision %1 · Undo to restore").arg(revision_));
}

void ProjectBridge::convert_audio_clip_to_sampler(const QString& clip_id) {
  if (!session_ || control_busy() || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish active project work before converting a clip"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to convert"));
    return;
  }
  auto converted = session_->audio_clips().convert_audio_clip_to_sampler(parsed.value());
  if (!converted) {
    set_status(QStringLiteral("Convert to sampler rejected: %1")
                   .arg(QString::fromStdString(converted.error().message)));
    return;
  }
  const auto track = converted.value().track;
  refresh();
  submit_playback_edit();
  schedule_background_save();
  emit trackCreated(static_cast<int>(session_->tracks().track_index(track).value_or(0)));
  set_status(QStringLiteral("Converted to Crate: %1 chops · one undo step · mute the audio track "
                            "to hear only the sampler")
                 .arg(static_cast<qulonglong>(converted.value().chops)));
}

} // namespace oscillate::quick
