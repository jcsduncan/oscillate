#pragma once

#include "oscillate/application/instrument_playback.hpp"

#include <QString>
#include <QtTypes>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

class QApplication;
class QQmlApplicationEngine;
class QQuickItem;

namespace oscillate::quick {
class ProjectBridge;
}

namespace quick_smoke {

struct SmokeDeviceState {
  std::atomic<std::uint64_t> selected_serial{0};
  std::atomic<bool> disconnected{false};
  std::atomic<bool> hide_selected{false};
  std::atomic<bool> discovery_failure{false};
  std::atomic<float> first_sample{0.0F};
  std::atomic<std::uint64_t> first_completed_sample{0};
  std::atomic<bool> stopped_off_ui{false};
  std::atomic<bool> delay_start{false};
  std::atomic<std::uint32_t> requested_buffer_frames{0};
  std::atomic<int> backend_id{-1};
  bool meter_full_window = false;
  std::thread::id ui_thread;
};

using AudioBackendVisibleCount = qsizetype (*)(const QQuickItem*, const QString&);

void run_audio_backend(QApplication& app, QQmlApplicationEngine& engine,
                       oscillate::quick::ProjectBridge& bridge,
                       const std::shared_ptr<SmokeDeviceState>& device_state,
                       oscillate::application::PlaybackOutputFactory jack_output_factory,
                       AudioBackendVisibleCount visible_count);

} // namespace quick_smoke
