#include "smoke_waveform_scenario.hpp"
#include "smoke_tracks.hpp"

#include "project_bridge.hpp"
#include "smoke_import_scenario.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

#include <cmath>
#include <iostream>

namespace quick_smoke {

void start_waveform(QApplication& app, QQmlApplicationEngine& engine,
                    oscillate::quick::ProjectBridge& bridge, WaveformItemLookup find_item) {
  auto* timer = new QTimer(&app);
  timer->setInterval(20);
  QElapsedTimer elapsed;
  elapsed.start();
  QObject::connect(
      timer, &QTimer::timeout, &app, [&app, &bridge, &engine, timer, elapsed, find_item] {
        if (elapsed.elapsed() > 10'000) {
          std::cerr << "Waveform smoke: analysis timed out\n";
          timer->stop();
          app.exit(1);
          return;
        }
        const auto track = audio_track(bridge);
        if (!track.isValid()) {
          std::cerr << "Waveform smoke: audio track missing\n";
          app.exit(1);
          return;
        }
        const auto clips = track.toMap().value(QStringLiteral("clips")).toList();
        if (clips.size() != 1) {
          std::cerr << "Waveform smoke: audio clip missing\n";
          app.exit(1);
          return;
        }
        const auto clip = clips.front().toMap();
        if (clip.value(QStringLiteral("waveformState")).toString() == QStringLiteral("loading")) {
          return;
        }
        if (clip.value(QStringLiteral("waveformState")).toString() != QStringLiteral("ready")) {
          std::cerr << "Waveform smoke: analysis unavailable: "
                    << clip.value(QStringLiteral("waveformMessage")).toString().toStdString()
                    << '\n';
          app.exit(1);
          return;
        }
        // The columns in view are their own map (zooming re-projects only them).
        const auto columns_of = [&bridge](const QVariantMap& of) {
          return bridge.clip_waveform(of.value(QStringLiteral("id")).toString());
        };
        const auto waveform = columns_of(clip);
        if (waveform.size() < 16 ||
            std::abs(
                waveform[waveform.size() / 8].toMap().value(QStringLiteral("max")).toDouble()) >
                0.01 ||
            waveform[waveform.size() * 7 / 8].toMap().value(QStringLiteral("max")).toDouble() <
                0.45 ||
            count_visible_items(
                qobject_cast<QQuickWindow*>(engine.rootObjects().front())->contentItem(),
                QStringLiteral("audioClipWaveform")) != 1) {
          std::cerr << "Waveform smoke: source envelope or visible canvas wrong\n";
          app.exit(1);
          return;
        }
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        auto* clip_item = find_item(window->contentItem(), QStringLiteral("arrangementClip"));
        const auto original_width = clip_item ? clip_item->width() : 0.0;
        engine.rootObjects().front()->setProperty("visibleBars", 2);
        QCoreApplication::processEvents();
        clip_item = find_item(window->contentItem(), QStringLiteral("arrangementClip"));
        const auto zoomed_waveform = columns_of(clip);
        if (!clip_item || clip_item->width() < original_width * 5 ||
            zoomed_waveform.size() <= waveform.size() ||
            count_visible_items(window->contentItem(), QStringLiteral("arrangementZoomControl")) !=
                4) {
          std::cerr << "Waveform smoke: zoom did not expand the clip or render controls\n";
          app.exit(1);
          return;
        }
        engine.rootObjects().front()->setProperty("viewStartBar", 2);
        QCoreApplication::processEvents();
        if (count_visible_items(window->contentItem(), QStringLiteral("arrangementClip")) != 0) {
          std::cerr << "Waveform smoke: panning did not change the visible bars\n";
          app.exit(1);
          return;
        }
        engine.rootObjects().front()->setProperty("viewStartBar", 0);
        QCoreApplication::processEvents();
        bridge.trim_audio_clip(clip.value(QStringLiteral("id")).toString(), true, 1.0 / 32.0,
                               false);
        const auto trimmed =
            audio_track(bridge).toMap().value(QStringLiteral("clips")).toList().front().toMap();
        const auto trimmed_waveform = columns_of(trimmed);
        if (!bridge.status().startsWith(QStringLiteral("Audio clip trimmed")) ||
            trimmed_waveform.size() < 16 ||
            trimmed_waveform[trimmed_waveform.size() / 8]
                    .toMap()
                    .value(QStringLiteral("max"))
                    .toDouble() < 0.45) {
          std::cerr << "Waveform smoke: left trim did not reveal later source frames: "
                    << bridge.status().toStdString() << '\n';
          app.exit(1);
          return;
        }
        std::cout << "Qt Quick waveform analysis and source trim projected\n";
        timer->stop();
        app.quit();
      });
  timer->start();
}

} // namespace quick_smoke
