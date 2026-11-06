#include "key_review_controller.hpp"
#include "project_bridge.hpp"

#include "oscillate/application/midi_note_tools.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace oscillate::quick {
namespace {
using application::midi_grid_ticks;
using application::supported_midi_grid;

// A zero-based, fractional bar position as a tick: whole bars by the meter,
// then the fraction across that bar. The arrangement snaps to its grid
// before it calls, so this keeps whatever position it chose.
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

namespace oscillate::quick {

// ---- Selection, clipboard, markers and colours ------------------------------

namespace {

template <typename Id> [[nodiscard]] std::vector<Id> parse_ids(const QStringList& texts) {
  std::vector<Id> ids;
  for (const auto& text : texts) {
    if (const auto parsed = Id::parse(text.toStdString()))
      ids.push_back(parsed.value());
  }
  return ids;
}

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

void ProjectBridge::move_notes_grid(const QString& clip_id, const QStringList& note_ids,
                                    int step_delta, int key_delta, int steps_per_bar) {
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

QStringList ProjectBridge::copy_notes_grid(const QString& clip_id, const QStringList& note_ids,
                                           int step_delta, int key_delta, int steps_per_bar) {
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
