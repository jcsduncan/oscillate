#include "project_bridge.hpp"

#include "clip_warp_controller.hpp"
#include "project_bridge_projection.hpp"
#include "route_commands.hpp"
#include "tempo_lane_controller.hpp"
#include "track_commands.hpp"
#include "waveform_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <system_error>
#include <utility>

// Arrangement edits from QML: tempo, tracks, routes, MIDI and audio clips,
// send automation, and undo and redo.
namespace oscillate::quick {
namespace {

foundation::Result<timeline::TickPosition> timeline_position(const project::Snapshot& snapshot,
                                                             double normalized, bool snap,
                                                             int timeline_bars) {
  if (!std::isfinite(normalized) || normalized > 1.0 ||
      normalized * timeline_bars < -static_cast<double>(project::max_lead_bars)) {
    return foundation::Result<timeline::TickPosition>::failure(
        {foundation::ErrorCode::invalid_argument, "Audio edit is outside the visible timeline"});
  }
  const double bars = normalized * timeline_bars;
  if (bars < 0.0) {
    // Before zero, in the opening meter's bars: the session adds bars
    // before bar 1 for it (ADR-0053).
    const auto& opening = snapshot.meter.events().front();
    const auto beat =
        4 * timeline::ticks_per_quarter / static_cast<std::int64_t>(opening.denominator);
    const auto ticks = bars * static_cast<double>(beat * opening.numerator);
    return foundation::Result<timeline::TickPosition>::success(
        {snap ? std::llround(ticks / static_cast<double>(beat)) * beat
              : static_cast<std::int64_t>(std::llround(ticks))});
  }
  const auto bar_index = std::min(static_cast<int>(std::floor(bars)), timeline_bars - 1);
  const auto first = snapshot.meter.tick_at({bar_index + 1, 1, 0});
  const auto last = snapshot.meter.tick_at({bar_index + 2, 1, 0});
  if (!first || !last) {
    return foundation::Result<timeline::TickPosition>::failure(!first ? first.error()
                                                                      : last.error());
  }
  const auto offset = static_cast<std::int64_t>(std::llround(
      (bars - bar_index) * static_cast<double>(last.value().value - first.value().value)));
  const timeline::TickPosition unsnapped{first.value().value + offset};
  if (!snap) {
    return foundation::Result<timeline::TickPosition>::success(unsnapped);
  }
  const auto location = snapshot.meter.locate(unsnapped);
  if (!location) {
    return foundation::Result<timeline::TickPosition>::failure(location.error());
  }
  const auto beat_start = snapshot.meter.tick_at({location.value().bar, location.value().beat, 0});
  auto next = snapshot.meter.tick_at(
      {location.value().bar, static_cast<std::uint8_t>(location.value().beat + 1), 0});
  if (!next) {
    next = snapshot.meter.tick_at({location.value().bar + 1, 1, 0});
  }
  if (!beat_start || !next) {
    return foundation::Result<timeline::TickPosition>::failure(!beat_start ? beat_start.error()
                                                                           : next.error());
  }
  return foundation::Result<timeline::TickPosition>::success(
      unsnapped.value - beat_start.value().value <= next.value().value - unsnapped.value
          ? beat_start.value()
          : next.value());
}

} // namespace

void ProjectBridge::rebuild_clip_waveform(const QString& clip_id) {
  waveform_->rebuild_clip(clip_id);
}

void ProjectBridge::select_audio_clip(const QString& clip_id) {
  if (!session_) {
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  std::optional<project::SourceRecord> source;
  if (parsed) {
    for (const auto& track : session_->snapshot().tracks) {
      const auto clip = std::ranges::find_if(
          track.clips, [&](const auto& candidate) { return candidate.id == parsed.value(); });
      if (clip != track.clips.end()) {
        const auto found =
            std::ranges::find_if(session_->snapshot().sources, [&](const auto& candidate) {
              return candidate.id == clip->source;
            });
        if (found != session_->snapshot().sources.end()) {
          source = *found;
        }
        break;
      }
    }
  }
  key_review_->select_clip(source ? clip_id : QString(), source);
  clip_warp_->select_clip(source ? clip_id : QString());
}

void ProjectBridge::set_initial_tempo_bpm(double bpm) { tempo_lane_->set_initial_bpm(bpm); }

void ProjectBridge::set_initial_meter(int numerator, int denominator) {
  tempo_lane_->set_initial_meter(numerator, denominator);
}

void ProjectBridge::set_status(QString value) {
  status_log_.note("status", value);
  status_ = std::move(value);
  emit statusChanged();
}

void ProjectBridge::add_instrument_track() { track_commands_->add_instrument_track(); }

void ProjectBridge::remove_track(const QString& track_id) {
  track_commands_->remove_track(track_id);
}

void ProjectBridge::duplicate_track(const QString& track_id) {
  if (const auto copy = track_commands_->duplicate_track(track_id); !copy.isEmpty())
    set_selected_track_id(copy);
}

bool ProjectBridge::move_track(const QString& track_id, int to_index) {
  return track_commands_->move_track(track_id, to_index);
}

void ProjectBridge::set_track_muted(const QString& track_id, bool muted) {
  track_commands_->set_track_muted(track_id, muted);
}

void ProjectBridge::set_track_soloed(const QString& track_id, bool soloed) {
  track_commands_->set_track_soloed(track_id, soloed);
}

void ProjectBridge::set_track_level_pan(const QString& track_id, double level, double pan) {
  track_commands_->set_track_level_pan(track_id, level, pan);
}

void ProjectBridge::set_master_level(double level) { track_commands_->set_master_level(level); }

bool ProjectBridge::route_edit_allowed() {
  if (!session_ || control_busy() || playback_completion_ || export_busy() || import_busy()) {
    set_status(QStringLiteral("Finish the current project operation before editing routes"));
    return false;
  }
  return true;
}

void ProjectBridge::add_bus_track(const QString& kind) { track_commands_->add_bus_track(kind); }

void ProjectBridge::set_main_route(const QString& track_id, const QString& destination_id) {
  route_commands_->set_main_route(track_id, destination_id);
}

void ProjectBridge::add_send_route(const QString& track_id, const QString& destination_id,
                                   double gain) {
  route_commands_->add_send_route(track_id, destination_id, gain);
}

void ProjectBridge::set_send_gain(const QString& send_id, double gain) {
  route_commands_->set_send_gain(send_id, gain);
}

void ProjectBridge::remove_send(const QString& send_id) { route_commands_->remove_send(send_id); }

void ProjectBridge::add_midi_clip(const QString& track_id, int bar_index) {
  if (control_busy()) {
    set_status(managed_media_busy() ? QStringLiteral("Finish media verification")
                                    : QStringLiteral("Wait for the audio edit to finish"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed || bar_index < 0 || bar_index >= detail::visible_bars) {
    set_status(QStringLiteral("Select a track and a bar from 1 to 16"));
    return;
  }
  const auto tick = session_->snapshot().meter.tick_at({bar_index + 1, 1, 0});
  if (!tick) {
    set_status(QStringLiteral("That bar is unavailable"));
    return;
  }
  const auto result = session_->midi_clips().add_midi_clip(parsed.value(), tick.value());
  if (!result) {
    set_status(QStringLiteral("MIDI clip: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI clip added · revision %1").arg(revision_));
}

void ProjectBridge::add_note(const QString& clip_id, int step_index, int key) {
  if (control_busy()) {
    set_status(managed_media_busy() ? QStringLiteral("Finish media verification")
                                    : QStringLiteral("Wait for the audio edit to finish"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || step_index < 0 || step_index >= 16 || key < 0 || key > 127) {
    set_status(QStringLiteral("Select a MIDI clip, step and pitch"));
    return;
  }
  const auto result =
      session_->notes().add_note(parsed.value(), {step_index * (timeline::ticks_per_quarter / 4)},
                                 static_cast<std::uint8_t>(key));
  if (!result) {
    set_status(QStringLiteral("MIDI note: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("MIDI note added · revision %1").arg(revision_));
}

bool ProjectBridge::audio_edit_enabled() const {
  return session_ && !audio_edit_completion_ && !playback_completion_ && !export_busy() &&
         !import_busy() && !managed_media_busy() && !close_after_playback_ &&
         !closing_after_export() && !imports_->audio().closing() && playback_ &&
         !playback_->retiring() &&
         (!playback_->playing() ||
          (playback_->output_state() == audio_io::OutputState::streaming &&
           playback_->status().application != application::PlaybackApplication::pending));
}

void ProjectBridge::move_audio_clip(const QString& clip_id, double normalized_start,
                                    bool snap_to_beat) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to move"));
    return;
  }
  const auto start =
      timeline_position(session_->snapshot(), normalized_start, snap_to_beat, timeline_bars_);
  if (!start) {
    set_status(QStringLiteral("Audio move: %1").arg(QString::fromStdString(start.error().message)));
    return;
  }
  const auto before = session_->snapshot().revision;
  const auto result = session_->audio_clips().move_audio_clip(parsed.value(), start.value());
  if (!result) {
    set_status(QStringLiteral("Audio move rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(before == revision_
                 ? QStringLiteral("Audio clip stayed at its original position")
                 : QStringLiteral("Audio clip moved · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::move_audio_clip_to_track(const QString& clip_id, const QString& track_id,
                                             double normalized_start) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!parsed || !track) {
    set_status(QStringLiteral("Select an audio clip and a track to move it to"));
    return;
  }
  const auto start =
      timeline_position(session_->snapshot(), normalized_start, false, timeline_bars_);
  if (!start) {
    set_status(QStringLiteral("Audio move: %1").arg(QString::fromStdString(start.error().message)));
    return;
  }
  if (auto moved = session_->audio_clips().move_audio_clip_to_track(parsed.value(), track.value(),
                                                                    start.value());
      !moved) {
    set_status(QStringLiteral("Audio move rejected: %1")
                   .arg(QString::fromStdString(moved.error().message)));
    return;
  }
  refresh();
  submit_audio_clip_edit();
  set_status(QStringLiteral("Audio clip moved to another track · revision %1").arg(revision_));
}

void ProjectBridge::nudge_audio_clip(const QString& clip_id, int beat_direction) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed || (beat_direction != -1 && beat_direction != 1)) {
    set_status(QStringLiteral("Select an audio clip and a move direction"));
    return;
  }
  const auto& snapshot = session_->snapshot();
  const project::Clip* clip = nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find_if(
        track.clips, [id = parsed.value()](const auto& item) { return item.id == id; });
    if (found != track.clips.end()) {
      clip = &*found;
      break;
    }
  }
  if (!clip) {
    set_status(QStringLiteral("Audio clip no longer exists"));
    return;
  }
  foundation::Result<timeline::TickPosition> start =
      std::holds_alternative<project::MusicalInterval>(clip->placement)
          ? foundation::Result<timeline::TickPosition>::success(
                std::get<project::MusicalInterval>(clip->placement).start)
          : snapshot.tempo.sample_to_tick(std::get<project::ElapsedInterval>(clip->placement).start,
                                          snapshot.sample_rate);
  if (!start) {
    set_status(QStringLiteral("Audio move: %1").arg(QString::fromStdString(start.error().message)));
    return;
  }
  auto location = snapshot.meter.locate(start.value());
  if (!location) {
    set_status(
        QStringLiteral("Audio move: %1").arg(QString::fromStdString(location.error().message)));
    return;
  }
  auto target = snapshot.meter.tick_at({location.value().bar, location.value().beat, 0});
  if (beat_direction > 0) {
    target = snapshot.meter.tick_at(
        {location.value().bar, static_cast<std::uint8_t>(location.value().beat + 1), 0});
    if (!target) {
      target = snapshot.meter.tick_at({location.value().bar + 1, 1, 0});
    }
  } else if (target && start.value() == target.value()) {
    if (start.value().value == 0) {
      set_status(QStringLiteral("Audio clip cannot move before the project start"));
      return;
    }
    location = snapshot.meter.locate({start.value().value - 1});
    if (location) {
      target = snapshot.meter.tick_at({location.value().bar, location.value().beat, 0});
    }
  }
  if (!target) {
    set_status(
        QStringLiteral("Audio move: %1").arg(QString::fromStdString(target.error().message)));
    return;
  }
  const auto before = snapshot.revision;
  const auto result = session_->audio_clips().move_audio_clip(parsed.value(), target.value());
  if (!result) {
    set_status(QStringLiteral("Audio move rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(before == revision_
                 ? QStringLiteral("Audio clip stayed at its original position")
                 : QStringLiteral("Audio clip moved · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::trim_audio_clip(const QString& clip_id, bool left_edge, double normalized_edge,
                                    bool snap_to_beat) {
  if (!audio_edit_enabled()) {
    set_status(QStringLiteral(
        "Finish the current playback, import or export transition before editing audio clips"));
    return;
  }
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Select an audio clip to trim"));
    return;
  }
  const auto tick =
      timeline_position(session_->snapshot(), normalized_edge, snap_to_beat, timeline_bars_);
  if (!tick) {
    set_status(QStringLiteral("Audio trim: %1").arg(QString::fromStdString(tick.error().message)));
    return;
  }
  const auto sample =
      session_->snapshot().tempo.tick_to_sample(tick.value(), session_->snapshot().sample_rate);
  if (!sample) {
    set_status(
        QStringLiteral("Audio trim: %1").arg(QString::fromStdString(sample.error().message)));
    return;
  }
  const auto before = session_->snapshot().revision;
  const auto result = session_->audio_clips().trim_audio_clip(
      parsed.value(),
      left_edge ? application::AudioClipEdge::left : application::AudioClipEdge::right,
      sample.value());
  if (!result) {
    set_status(QStringLiteral("Audio trim rejected: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  if (before != revision_)
    submit_audio_clip_edit();
  set_status(
      before == revision_
          ? QStringLiteral("Audio clip edge stayed at its original position")
          : QStringLiteral("Audio clip trimmed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::set_send_automation_point(const QString& send_id, int zero_based_bar,
                                              double value) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  if (!send || zero_based_bar < 0 || zero_based_bar >= detail::visible_bars ||
      !std::isfinite(value) || value < 0.0 || value > 4.0) {
    set_status(QStringLiteral("Send point needs a bar from 1 to 16 and gain from 0 to 4"));
    return;
  }
  const auto track = std::ranges::find_if(session_->snapshot().tracks, [send](const auto& item) {
    return std::ranges::any_of(item.sends,
                               [send](const auto& route) { return route.id == send.value(); });
  });
  if (track == session_->snapshot().tracks.end()) {
    set_status(QStringLiteral("Select a send to automate"));
    return;
  }
  const auto projected =
      detail::send_automation_projection(session_->snapshot(), *track, send.value());
  const auto shown_points = projected.value(QStringLiteral("points")).toList();
  const bool replacing = std::ranges::any_of(shown_points, [zero_based_bar](const auto& item) {
    return item.toMap().value(QStringLiteral("bar")).toInt() == zero_based_bar;
  });
  if (projected.value(QStringLiteral("pointCount")).toInt() >=
          static_cast<int>(detail::max_quick_automation_points) &&
      !replacing) {
    set_status(QStringLiteral("Select a send with fewer than four Quick editor points"));
    return;
  }
  const auto result = session_->parameter_automation().set_send_gain_automation_point(
      send.value(), zero_based_bar, value);
  if (!result) {
    set_status(
        QStringLiteral("Send automation: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send point accepted · revision %1 · Save to keep; check transport "
                            "status for live application")
                 .arg(revision_));
}

void ProjectBridge::set_send_automation_value(const QString& send_id, const QString& point_id,
                                              double value) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!send || !point || !std::isfinite(value) || value < 0.0 || value > 4.0) {
    set_status(QStringLiteral("Select a send point and enter gain from 0 to 4"));
    return;
  }
  const auto result = session_->parameter_automation().set_send_gain_automation_value(
      send.value(), point.value(), value);
  if (!result) {
    set_status(
        QStringLiteral("Send automation: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send gain point accepted · revision %1 · Save to keep; check "
                            "transport status for live application")
                 .arg(revision_));
}

void ProjectBridge::remove_send_automation_point(const QString& send_id, const QString& point_id) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!send || !point) {
    set_status(QStringLiteral("Select a send automation point"));
    return;
  }
  const auto result = session_->parameter_automation().remove_send_gain_automation_point(
      send.value(), point.value());
  if (!result) {
    set_status(QStringLiteral("Remove send point: %1")
                   .arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send point removed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::remove_send_automation_lane(const QString& send_id) {
  if (!route_edit_allowed()) {
    return;
  }
  const auto send = project::SendId::parse(send_id.toStdString());
  if (!send) {
    set_status(QStringLiteral("Select a send automation lane"));
    return;
  }
  const auto result =
      session_->parameter_automation().remove_send_gain_automation_lane(send.value());
  if (!result) {
    set_status(
        QStringLiteral("Remove send lane: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  refresh();
  submit_playback_edit();
  set_status(QStringLiteral("Send lane removed · revision %1 · Save to keep").arg(revision_));
}

void ProjectBridge::undo() {
  if (control_busy()) {
    set_status(managed_media_busy()
                   ? QStringLiteral("Finish media verification before undo")
                   : QStringLiteral("Wait for the audio edit to finish before undo"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto result = session_->history().undo();
  if (result) {
    refresh();
    submit_playback_edit();
    if (!playback_busy())
      set_playback_status(QStringLiteral("Playback stopped; model revision %1 will be prepared on "
                                         "the next Play")
                              .arg(revision_));
  }
  set_status(
      result
          ? QStringLiteral("Undo applied · revision %1").arg(revision_)
          : QStringLiteral("Undo failed: %1").arg(QString::fromStdString(result.error().message)));
}

void ProjectBridge::redo() {
  if (control_busy()) {
    set_status(managed_media_busy()
                   ? QStringLiteral("Finish media verification before redo")
                   : QStringLiteral("Wait for the audio edit to finish before redo"));
    return;
  }
  if (!session_) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto result = session_->history().redo();
  if (result) {
    refresh();
    submit_playback_edit();
    if (!playback_busy())
      set_playback_status(QStringLiteral("Playback stopped; model revision %1 will be prepared on "
                                         "the next Play")
                              .arg(revision_));
  }
  set_status(
      result
          ? QStringLiteral("Redo applied · revision %1").arg(revision_)
          : QStringLiteral("Redo failed: %1").arg(QString::fromStdString(result.error().message)));
}

} // namespace oscillate::quick
