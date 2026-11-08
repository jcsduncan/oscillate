#include "smoke_audio_backend_scenario.hpp"

#include "project_bridge.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>

#include <cmath>
#include <iostream>
#include <vector>

namespace quick_smoke {

namespace app = oscillate::application;

void run_audio_backend(QApplication& app, QQmlApplicationEngine& engine,
                       oscillate::quick::ProjectBridge& bridge,
                       const std::shared_ptr<SmokeDeviceState>& device_state,
                       const oscillate::application::PlaybackOutputFactory& jack_output_factory,
                       AudioBackendVisibleCount visible_count) {
  auto* probe = new QTimer(&app);
  probe->setInterval(20);
  QElapsedTimer elapsed;
  elapsed.start();
  auto stage = std::make_shared<int>(0);
  QObject::connect(
      probe, &QTimer::timeout, &app,
      [&app, &engine, &bridge, device_state, stage, elapsed, probe, jack_output_factory,
       visible_count] {
        if (elapsed.elapsed() > 10'000) {
          std::cerr
              << "Audio output smoke: timed out at stage " << *stage << "; status "
              << bridge.playback_status().toStdString() << "; backend "
              << device_state->backend_id.load(std::memory_order_acquire) << "; active "
              << bridge.playback_active() << "; current " << bridge.routed_graph_current()
              << "; phase "
              << bridge.playback_state().value(QStringLiteral("phase")).toString().toStdString()
              << "; application "
              << bridge.playback_state()
                     .value(QStringLiteral("application"))
                     .toString()
                     .toStdString()
              << "; active revision "
              << bridge.playback_state().value(QStringLiteral("activeRevision")).toULongLong()
              << "; model revision " << bridge.revision() << '\n';
          probe->stop();
          app.exit(1);
          return;
        }
        if (*stage == 0) {
          if (bridge.audio_output()->audio_device_busy()) {
            return;
          }
          auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
          engine.rootObjects().front()->setProperty("audioMenuOpen", true);
          QCoreApplication::processEvents();
          if (!window || bridge.audio_output()->audio_backend() != QStringLiteral("pipewire") ||
              bridge.audio_output()->audio_devices().size() != 3 ||
              visible_count(window->contentItem(), QStringLiteral("audioOutputPanel")) != 1 ||
              visible_count(window->contentItem(), QStringLiteral("audioOutputChoice")) != 3 ||
              visible_count(window->contentItem(), QStringLiteral("audioBackendChoice")) != 2) {
            std::cerr << "Audio output smoke: discovery or output panel failed\n";
            probe->stop();
            app.exit(1);
            return;
          }
          bridge.audio_output()->select_audio_device(QStringLiteral("101"));
          bridge.audio_output()->select_buffer_frames(64);
          if (bridge.audio_output()->selected_audio_device_name() !=
                  QStringLiteral("Studio monitors") ||
              QSettings{}.value(QStringLiteral("audioOutputSerial")).toString() !=
                  QStringLiteral("101") ||
              bridge.audio_output()->requested_buffer_frames() != 64 ||
              QSettings{}.value(QStringLiteral("audioBufferFrames")).toInt() != 64 ||
              !bridge.audio_output()->buffer_size_status().contains(
                  QStringLiteral("Request 64 frames")) ||
              visible_count(window->contentItem(), QStringLiteral("audioBufferChoice")) !=
                  static_cast<int>(app::playback_buffer_frames.size())) {
            std::cerr << "Audio output smoke: selected sink was not persisted\n";
            probe->stop();
            app.exit(1);
            return;
          }
          bridge.toggle_playback();
          *stage = 1;
        } else if (*stage == 1) {
          if (!bridge.playback_active() ||
              device_state->selected_serial.load(std::memory_order_acquire) != 101) {
            return;
          }
          bridge.audio_output()->select_audio_device(QStringLiteral("202"));
          bridge.audio_output()->select_buffer_frames(128);
          bridge.audio_output()->select_audio_backend(QStringLiteral("jack"));
          if (bridge.audio_output()->selected_audio_device() != QStringLiteral("101") ||
              bridge.audio_output()->audio_backend() != QStringLiteral("pipewire") ||
              device_state->backend_id.load(std::memory_order_acquire) != 0 ||
              bridge.audio_output()->requested_buffer_frames() != 64 ||
              device_state->requested_buffer_frames.load(std::memory_order_acquire) != 64) {
            std::cerr << "Audio output smoke: active playback changed output\n";
            probe->stop();
            app.exit(1);
            return;
          }
          device_state->hide_selected.store(true, std::memory_order_release);
          device_state->disconnected.store(true, std::memory_order_release);
          *stage = 2;
        } else if (*stage == 2 && !bridge.playback_active() &&
                   !bridge.audio_output()->audio_device_busy() &&
                   bridge.audio_output()->selected_audio_device_name().startsWith(
                       QStringLiteral("Missing output"))) {
          if (!bridge.playback_status().contains(QStringLiteral("Audio device lost")) ||
              bridge.audio_output()->selected_audio_device() != QStringLiteral("101") ||
              !bridge.audio_output()->audio_device_status().contains(
                  QStringLiteral("unavailable"))) {
            std::cerr << "Audio output smoke: disconnect lost recovery state\n";
            probe->stop();
            app.exit(1);
            return;
          }
          bridge.audio_output()->select_audio_device(QStringLiteral("default"));
          constexpr auto largest = static_cast<int>(app::max_playback_buffer_frames);
          bridge.audio_output()->select_buffer_frames(largest);
          if (bridge.audio_output()->selected_audio_device() != QStringLiteral("default") ||
              bridge.audio_output()->selected_audio_device_name() !=
                  QStringLiteral("System default") ||
              bridge.audio_output()->requested_buffer_frames() != largest ||
              QSettings{}.value(QStringLiteral("audioBufferFrames")).toInt() != largest ||
              !bridge.audio_output()->buffer_size_status().contains(
                  QStringLiteral("Request %1 frames").arg(largest))) {
            std::cerr << "Audio output smoke: default recovery unavailable\n";
            probe->stop();
            app.exit(1);
            return;
          }
          device_state->discovery_failure.store(true, std::memory_order_release);
          bridge.audio_output()->refresh_audio_devices();
          *stage = 3;
        } else if (*stage == 3 && !bridge.audio_output()->audio_device_busy()) {
          if (!bridge.audio_output()->audio_device_status().contains(
                  QStringLiteral("Fixture output discovery failed")) ||
              bridge.audio_output()->selected_audio_device() != QStringLiteral("default")) {
            std::cerr << "Audio output smoke: discovery failure lost selection\n";
            probe->stop();
            app.exit(1);
            return;
          }
          auto empty_discovery = [] {
            return oscillate::foundation::Result<
                std::vector<oscillate::audio_io::PipeWireSink>>::success({});
          };
          constexpr auto smallest = static_cast<int>(app::playback_buffer_frames.front());
          constexpr auto fallback = static_cast<int>(app::default_playback_buffer_frames);
          QSettings{}.setValue(QStringLiteral("audioBufferFrames"), smallest);
          oscillate::quick::ProjectBridge persisted({}, nullptr, {}, empty_discovery);
          QSettings{}.setValue(QStringLiteral("audioBufferFrames"), 96);
          oscillate::quick::ProjectBridge invalid({}, nullptr, {}, empty_discovery);
          QSettings{}.remove(QStringLiteral("audioBufferFrames"));
          oscillate::quick::ProjectBridge fresh({}, nullptr, {}, empty_discovery);
          QSettings{}.setValue(QStringLiteral("audioBufferFrames"), 128);
          if (persisted.audio_output()->requested_buffer_frames() != smallest ||
              invalid.audio_output()->requested_buffer_frames() != fallback ||
              fresh.audio_output()->requested_buffer_frames() != fallback) {
            std::cerr << "Audio output smoke: saved or invalid buffer preference failed\n";
            probe->stop();
            app.exit(1);
            return;
          }
          QSettings{}.setValue(QStringLiteral("audioBackend"), QStringLiteral("jack"));
          oscillate::quick::ProjectBridge without_server({}, nullptr, {}, empty_discovery,
                                                         jack_output_factory, [] { return false; });
          if (without_server.audio_output()->audio_backend() != QStringLiteral("pipewire")) {
            std::cerr << "Audio output smoke: saved JACK selection ignored server loss\n";
            probe->stop();
            app.exit(1);
            return;
          }
          without_server.audio_output()->select_audio_backend(QStringLiteral("jack"));
          if (without_server.audio_output()->audio_backend() != QStringLiteral("pipewire") ||
              !without_server.audio_output()->audio_device_status().contains(
                  QStringLiteral("No JACK server"))) {
            std::cerr << "Audio output smoke: unavailable JACK server was selectable\n";
            probe->stop();
            app.exit(1);
            return;
          }
          QSettings{}.setValue(QStringLiteral("audioBackend"), QStringLiteral("pipewire"));
          bridge.audio_output()->select_audio_backend(QStringLiteral("jack"));
          QCoreApplication::processEvents();
          oscillate::quick::ProjectBridge restored({}, nullptr, {}, empty_discovery,
                                                   jack_output_factory, [] { return true; });
          auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
          if (bridge.audio_output()->audio_backend() != QStringLiteral("jack") ||
              restored.audio_output()->audio_backend() != QStringLiteral("jack") ||
              bridge.audio_output()->selected_audio_device_name() !=
                  QStringLiteral("JACK server") ||
              !bridge.audio_output()->jack_playback_available() || !window ||
              QSettings{}.value(QStringLiteral("audioBackend")).toString() !=
                  QStringLiteral("jack") ||
              visible_count(window->contentItem(), QStringLiteral("audioOutputChoice")) != 0) {
            std::cerr << "Audio output smoke: JACK selection failed\n";
            probe->stop();
            app.exit(1);
            return;
          }
          device_state->disconnected.store(false, std::memory_order_release);
          bridge.toggle_playback();
          *stage = 4;
        } else if (*stage == 4) {
          if (!bridge.playback_active() ||
              device_state->backend_id.load(std::memory_order_acquire) != 1)
            return;
          bridge.audio_output()->select_audio_backend(QStringLiteral("pipewire"));
          if (bridge.audio_output()->audio_backend() != QStringLiteral("jack")) {
            std::cerr << "Audio output smoke: active JACK playback changed backend\n";
            probe->stop();
            app.exit(1);
            return;
          }
          bridge.toggle_playback();
          *stage = 5;
        } else if (*stage == 5 && !bridge.playback_active() &&
                   bridge.audio_output()->audio_device_selection_enabled()) {
          const auto revision = bridge.revision();
          const auto track_id =
              bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
          bridge.select_native_instrument(track_id, QStringLiteral("prism"));
          if (bridge.revision() != revision + 1) {
            std::cerr << "Audio output smoke: routed fixture was not prepared; track "
                      << track_id.toStdString() << "; status " << bridge.status().toStdString()
                      << "; revision " << bridge.revision() << '\n';
            probe->stop();
            app.exit(1);
            return;
          }
          bridge.toggle_playback();
          *stage = 6;
        } else if (*stage == 6) {
          if (!bridge.playback_active() || !bridge.routed_graph_current() ||
              device_state->backend_id.load(std::memory_order_acquire) != 1)
            return;
          bridge.toggle_playback();
          *stage = 7;
        } else if (*stage == 7 && !bridge.playback_active() &&
                   bridge.audio_output()->audio_device_selection_enabled()) {
          bridge.audio_output()->select_audio_backend(QStringLiteral("pipewire"));
          if (bridge.audio_output()->audio_backend() != QStringLiteral("pipewire") ||
              QSettings{}.value(QStringLiteral("audioBackend")).toString() !=
                  QStringLiteral("pipewire")) {
            std::cerr << "Audio output smoke: PipeWire recovery failed\n";
            probe->stop();
            app.exit(1);
            return;
          }
          std::cout << "Qt Quick audio output selection and recovery ready\n";
          probe->stop();
          app.quit();
        }
      });
  probe->start();
}

} // namespace quick_smoke
