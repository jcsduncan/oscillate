// ProjectBridge: MIDI recording, the click and the count-in (ADR-0086).

#include "loop_transport.hpp"
#include "project_bridge.hpp"
#include "recording_controller.hpp"

#include "oscillate/application/midi_recording.hpp"
#include "oscillate/audio_io/metronome.hpp"

#include <chrono>

namespace oscillate::quick {

namespace {
// Clicks run this many bars past the arrangement's end.
constexpr int click_bars_past_end = 8;
} // namespace

QObject* ProjectBridge::recording() const noexcept { return recording_.get(); }

bool ProjectBridge::song_playing() const {
  if (routed_playback_)
    return !routed_stop_completion_ && !routed_playback_->paused();
  return playback_ && playback_->playing();
}

std::optional<std::uint64_t> ProjectBridge::heard_sample() const {
  if (!session_)
    return std::nullopt;
  if (routed_playback_ && !routed_stop_completion_) {
    if (const auto point = routed_playback_->audible_point()) {
      const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
      return application::heard_sample(*point, now, session_->snapshot().sample_rate,
                                       loop_transport_ ? loop_transport_->loop_samples()
                                                       : std::nullopt);
    }
  }
  // No device timing (a test output): the last block played.
  return playhead_sample_;
}

void ProjectBridge::publish_click_schedule() {
  if (!session_ || !metronome_)
    return;
  const auto& snapshot = session_->snapshot();
  const std::pair maps{snapshot.tempo.revision(), snapshot.meter.revision()};
  if (click_maps_ == maps)
    return;
  const auto end = transport_controller_.sample_at_bar(
      &snapshot, static_cast<double>(timeline_bars_ + click_bars_past_end));
  if (!end)
    return;
  metronome_->publish(application::click_schedule(snapshot, *end), snapshot.sample_rate);
  click_maps_ = maps;
}

void ProjectBridge::make_recording() {
  metronome_ = std::make_shared<audio_io::Metronome>();
  recording_ = std::make_unique<RecordingController>(
      RecordingController::Hooks{
          .session = [this] { return session_.get(); },
          .playing = [this] { return song_playing(); },
          .heard_sample = [this] { return heard_sample(); },
          .loop =
              [this] { return loop_transport_ ? loop_transport_->loop_samples() : std::nullopt; },
          .play =
              [this](bool count_in) {
                if (!session_)
                  return;
                publish_click_schedule();
                if (count_in) {
                  if (const auto bar =
                          application::count_in_at(session_->snapshot(), playhead_sample_))
                    metronome_->request_count_in(bar->beats, bar->beat_frames);
                }
                toggle_pause();
              },
          .device_counting_in = [this] { return metronome_->counting_in(); },
          .click_changed = [this](bool on) { metronome_->set_enabled(on); },
          .bar_at =
              [this](std::uint64_t sample) {
                return transport_controller_.playhead_bar(
                    session_ ? &session_->snapshot() : nullptr, sample);
              },
          .commit =
              [this] {
                refresh();
                submit_playback_edit();
              },
          .status = [this](QString message) { set_status(std::move(message)); }},
      this);
  connect(this, &ProjectBridge::playbackChanged, recording_.get(),
          &RecordingController::transport_changed);
  // A tempo or meter edit moves the beats.
  connect(this, &ProjectBridge::revisionChanged, this, [this] { publish_click_schedule(); });
}

} // namespace oscillate::quick
