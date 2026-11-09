#include "project_bridge.hpp"

#include "meter_model.hpp"
#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"

#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

// Playback state and the meters, CPU load and analysers QML reads.
namespace oscillate::quick {

bool ProjectBridge::playback_active() const {
  return audio_edit_completion_ || routed_completion_ || routed_playback_ ||
         routed_stop_completion_ || (playback_ && playback_->playing());
}

QVariantMap ProjectBridge::master_meter() const {
  const auto reading = meter_->latest();
  const bool valid = reading && reading->transport_generation != 0;
  return {{"valid", valid},
          {"leftPeak", valid ? reading->left_peak : 0.0F},
          {"rightPeak", valid ? reading->right_peak : 0.0F},
          {"leftRms", valid ? reading->left_rms : 0.0F},
          {"rightRms", valid ? reading->right_rms : 0.0F},
          {"clipped", valid && reading->clipped},
          {"invalidFrames", valid ? reading->invalid_frames : 0U},
          {"endSample", valid ? static_cast<qulonglong>(reading->end_sample) : 0ULL},
          {"generation", valid ? static_cast<qulonglong>(reading->transport_generation) : 0ULL}};
}

void ProjectBridge::drain_realtime_diagnostics() {
  // What the audio callback could not say in text: silenced blocks, xruns.
  // An empty ring costs one atomic load.
  if (const auto drained = foundation::log::drain_diagnostics(); !drained.status.empty())
    set_status(QString::fromStdString(drained.status));
}

void ProjectBridge::poll_meters() {
  drain_realtime_diagnostics();
  // Edits poll playback every 5 ms; meters need no more than ~20 Hz. The
  // final poll that stops the timer always refreshes, clearing stale levels.
  if (playback_timer_.isActive() && meter_clock_.isValid() && meter_clock_.elapsed() < 45)
    return;
  refresh_meters();
}

QObject* ProjectBridge::meters() const noexcept { return meter_model_.get(); }

bool ProjectBridge::track_meters_live() const noexcept {
  // The routed graph measures each track's node after level, pan, mute and
  // inserts. A paused, changing, stopping or direct-instrument transport
  // has no live node meters, so every track reads invalid.
  if (!routed_playback_ && monitor_)
    return monitor_->output_status().state == audio_io::OutputState::streaming;
  return routed_playback_ && !routed_completion_ && !routed_stop_completion_ &&
         !routed_transport_completion_ && !routed_playback_->paused() &&
         routed_playback_->output_status().state == audio_io::OutputState::streaming;
}

void ProjectBridge::refresh_meters() {
  OSCILLATE_TRACE_SPAN("ui.refresh_meters");
  meter_clock_.start();
  // The display clock runs while there is anything to show.
  if (meter_model_)
    meter_model_->wake();
  QVariantMap meters;
  if (session_) {
    const bool live = track_meters_live();
    for (const auto& track : session_->snapshot().tracks) {
      const auto reading = live ? live_run()->track_meter(track.id) : std::nullopt;
      const bool valid = reading && reading->transport_generation != 0;
      meters.insert(QString::fromStdString(track.id.to_string()),
                    QVariantMap{{"valid", valid},
                                {"leftPeak", valid ? reading->left_peak : 0.0F},
                                {"rightPeak", valid ? reading->right_peak : 0.0F},
                                {"leftRms", valid ? reading->left_rms : 0.0F},
                                {"rightRms", valid ? reading->right_rms : 0.0F},
                                {"clipped", valid && reading->clipped}});
    }
  }
  track_meters_ = std::move(meters);
  emit metersChanged();
}

QVariant ProjectBridge::cpu_load() const {
  // The whole device callback against its period, from the output's
  // DeviceTelemetry (PipeWire or JACK): the same figure as the trace's audio.load.
  std::optional<audio_io::CallbackLoad> load;
  if (routed_playback_) {
    if (const auto* run = routed_run())
      load = run->output_status().callback_load;
  } else if (playback_)
    load = playback_->output_status().callback_load;
  else if (monitor_)
    load = monitor_->output_status().callback_load;
  return load ? QVariant{std::clamp(load->average, 0.0, 1.0)} : QVariant{};
}

int ProjectBridge::audio_threads() const {
  const auto* run = routed_run();
  if (!run)
    return 0;
  const auto output = run->output_status();
  return output.state == audio_io::OutputState::streaming
             ? 1 + static_cast<int>(output.realtime_workers)
             : 0;
}

QVariantMap ProjectBridge::master_true_peak() const {
  const auto reading = true_peak_->latest();
  const bool incomplete = true_peak_->incomplete();
  const bool complete = reading && reading->complete && !incomplete && reading->peak.frames != 0 &&
                        reading->transport_generation != 0;
  return {
      {"valid", complete},
      {"incomplete", incomplete},
      {"left", complete ? reading->peak.left_true_peak : 0.0F},
      {"right", complete ? reading->peak.right_true_peak : 0.0F},
      {"invalidFrames", complete ? static_cast<qulonglong>(reading->peak.invalid_frames) : 0ULL},
      {"droppedBlocks", static_cast<qulonglong>(true_peak_->dropped_blocks())},
      {"endSample", complete ? static_cast<qulonglong>(reading->end_sample) : 0ULL}};
}

QVariantList ProjectBridge::master_spectrum() const {
  const auto reading = spectrum_->latest();
  if (!reading)
    return {};
  constexpr int bands = 48;
  constexpr double low_hz = 20.0;
  const auto high_hz = std::min(20'000.0, static_cast<double>(reading->spectrum.sample_rate) / 2.0);
  const auto bin_scale = static_cast<double>(native_dsp::StereoSpectrum::window_frames) /
                         static_cast<double>(reading->spectrum.sample_rate);
  QVariantList values;
  values.reserve(bands);
  for (int band = 0; band < bands; ++band) {
    const auto low = low_hz * std::pow(high_hz / low_hz, static_cast<double>(band) / bands);
    const auto high = low_hz * std::pow(high_hz / low_hz, static_cast<double>(band + 1) / bands);
    const auto first = std::clamp(static_cast<std::size_t>(std::floor(low * bin_scale)),
                                  std::size_t{1}, native_dsp::StereoSpectrum::bin_count - 1);
    const auto last = std::clamp(static_cast<std::size_t>(std::ceil(high * bin_scale)), first,
                                 native_dsp::StereoSpectrum::bin_count - 1);
    double amplitude = 0.0;
    for (auto bin = first; bin <= last; ++bin) {
      amplitude = std::max(amplitude, static_cast<double>(std::max(reading->spectrum.left[bin],
                                                                   reading->spectrum.right[bin])));
    }
    const auto db = 20.0 * std::log10(std::max(amplitude, 1e-6));
    values.push_back(std::clamp((db + 72.0) / 72.0, 0.0, 1.0));
  }
  return values;
}

bool ProjectBridge::playback_enabled() const {
  if (routed_transport_completion_) {
    return false;
  }
  if (routed_completion_) {
    return !routed_completion_->stop_requested.load(std::memory_order_acquire) &&
           !close_after_playback_;
  }
  if (routed_stop_completion_) {
    return false;
  }
  if (routed_playback_) {
    return !close_after_playback_;
  }
  if (audio_edit_completion_) {
    return !audio_edit_completion_->stop_requested && !close_after_playback_;
  }
  return session_ && !playback_completion_ && (!playback_ || !playback_->retiring()) &&
         !export_busy() && !import_busy() && !managed_media_busy() && !close_after_playback_ &&
         !closing_after_export() && !imports_->audio().closing();
}

} // namespace oscillate::quick
