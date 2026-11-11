#include "project_bridge.hpp"

#include "routed_playback.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

// Live monitoring: notes played while the transport is stopped (piano-roll
// keys, the computer keyboard) run through the project's own routed graph,
// prepared from the current snapshot with every clip removed. The song stays
// silent, while the played track's effects, sends, the master chain, the
// analysers and the meters all hear the notes, as in Live. The run starts on
// the first note, takes the project's edits like playback does, gives way to
// Play, and stops itself after a quiet spell.
namespace oscillate::quick {
namespace {

// A monitor with nothing played for this long lets the device go.
constexpr qint64 monitor_idle_ms = 20'000;

// The project with its clips removed: the graph, instruments, inserts and
// sends stay, the song is silent.
project::Snapshot muted(project::Snapshot snapshot) {
  // A frozen track's render is a clip too; its devices answer live notes.
  for (auto& track : snapshot.tracks) {
    track.clips.clear();
    track.midi_clips.clear();
    track.frozen.reset();
  }
  return snapshot;
}

} // namespace

bool ProjectBridge::transport_running() const noexcept {
  return routed_completion_ || routed_playback_ || routed_stop_completion_ ||
         routed_transport_completion_ || (playback_ && playback_->playing());
}

RoutedPlayback* ProjectBridge::routed_run() const noexcept {
  return routed_stop_completion_ ? nullptr : routed_playback_.get();
}

RoutedPlayback* ProjectBridge::live_run() const noexcept {
  if (routed_playback_)
    return routed_run();
  return monitor_.get();
}

void ProjectBridge::start_monitor() {
  if (!session_ || monitor_ || monitor_completion_ || transport_running())
    return;
  auto reservation = session_->reserve_playback_start();
  if (!reservation)
    return;
  auto token = std::make_shared<std::optional<application::ControlTransportGate::PlaybackToken>>(
      std::move(reservation).value());
  auto completion = std::make_shared<RoutedCompletion>();
  const auto target_serial = audio_output_->target_serial();
  try {
    monitor_worker_ = std::jthread(
        [root = waveform_root_, snapshot = muted(session_->snapshot()), token, completion,
         factory = audio_output_->factory(), target_serial, buffer = audio_output_->buffer_frames(),
         first = playhead_sample_, qualification = vst3_live_qualification_]() mutable {
          try {
            completion->result.emplace(RoutedPlayback::prepare_and_start(
                root, std::move(snapshot), std::move(**token), std::move(factory), target_serial,
                buffer, first, std::move(qualification)));
          } catch (const std::exception& error) {
            (*token)->finish_after_join();
            completion->result.emplace(foundation::Result<std::unique_ptr<RoutedPlayback>>::failure(
                {foundation::ErrorCode::internal,
                 std::string("Live monitoring failed to start: ") + error.what()}));
          }
          completion->done.store(true, std::memory_order_release);
        });
  } catch (const std::exception&) {
    (*token)->finish_after_join();
    return;
  }
  monitor_completion_ = std::move(completion);
  monitor_idle_.start();
  monitor_timer_.start();
}

bool ProjectBridge::stop_monitor_now() {
  if (monitor_completion_) {
    monitor_completion_->stop_requested.store(true, std::memory_order_release);
    return false;
  }
  if (monitor_) {
    (void)monitor_->stop_and_retire();
    monitor_.reset();
    meter_->invalidate_after_quiesce();
    true_peak_->invalidate_after_quiesce();
  }
  live_note_tracks_.clear();
  monitor_pending_.clear();
  monitor_timer_.stop();
  return true;
}

void ProjectBridge::poll_monitor() {
  drain_realtime_diagnostics();
  if (monitor_completion_) {
    if (!monitor_completion_->done.load(std::memory_order_acquire))
      return;
    monitor_worker_.join();
    auto completion = std::move(monitor_completion_);
    const bool stop = completion->stop_requested.load(std::memory_order_acquire);
    if (!completion->result || !*completion->result) {
      set_status(QStringLiteral("Live monitoring is unavailable: %1")
                     .arg(completion->result
                              ? QString::fromStdString(completion->result->error().message)
                              : QStringLiteral("no result")));
      // The notes still sound, dry, on the preview device.
      for (const auto& note : std::exchange(monitor_pending_, {})) {
        if (note.on)
          play_note_dry(QString::fromStdString(note.track.to_string()), note.key, note.velocity);
      }
      monitor_timer_.stop();
      return;
    }
    monitor_ = std::move(completion->result->value());
    if (stop || play_after_monitor_) {
      stop_monitor_now();
      if (std::exchange(play_after_monitor_, false))
        toggle_pause();
      return;
    }
    for (const auto& note : std::exchange(monitor_pending_, {}))
      (void)monitor_->live_note(note.track, note.key, note.velocity, note.on);
    // The project may have changed while the graph was prepared.
    submit_playback_edit();
    emit playbackChanged();
  }
  if (!monitor_)
    return;
  const auto state = monitor_->output_status().state;
  if (state == audio_io::OutputState::error || state == audio_io::OutputState::disconnected ||
      (live_note_tracks_.empty() && monitor_idle_.elapsed() > monitor_idle_ms) ||
      monitor_->source_failure()) {
    stop_monitor_now();
    emit playbackChanged();
  }
}

void ProjectBridge::send_live_note(const QString& track_id, int key, int velocity, bool on) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!track || key < 0 || key > 127)
    return;
  monitor_idle_.start();
  // Live notes play just in time from now on (ADR-0128).
  live_note_hold_.start(live_note_hold_ms);
  if (!live_notes_held_) {
    live_notes_held_ = true;
    apply_render_ahead();
  }
  const auto k = static_cast<std::uint8_t>(key);
  const auto v = static_cast<std::uint8_t>(std::clamp(velocity, 1, 127));
  if (auto* run = live_run(); run && run->live_note(track.value(), k, v, on))
    return;
  if (monitor_completion_) {
    if (on) {
      monitor_pending_.push_back({track.value(), k, v, true});
    } else {
      // A key let go before the graph was ready plays nothing.
      std::erase_if(monitor_pending_, [&](const PendingLiveNote& note) {
        return note.on && note.key == k && note.track == track.value();
      });
    }
  }
}

} // namespace oscillate::quick
