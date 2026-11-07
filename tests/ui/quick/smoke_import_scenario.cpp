#include "smoke_import_scenario.hpp"
#include "smoke_tracks.hpp"

#include "project_bridge.hpp"

#include <QApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <cstdint>
#include <iostream>

namespace quick_smoke {
namespace {

// The revision the project opened at, before the import.
std::uint64_t opened_revision = 0;

} // namespace

qsizetype count_visible_items(const QQuickItem* item, const QString& name) {
  qsizetype count = item->isVisible() && item->objectName() == name ? 1 : 0;
  for (const auto* child : item->childItems()) {
    count += count_visible_items(child, name);
  }
  return count;
}

bool verify_reopened_import(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window) {
  const auto tracks = content_tracks(bridge);
  if (tracks.size() != 1 ||
      tracks.front().toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("audio") ||
      tracks.front().toMap().value(QStringLiteral("clips")).toList().size() != 1) {
    std::cerr << "WAV import smoke: saved audio track/clip did not reopen\n";
    return false;
  }
  if (!window ||
      count_visible_items(window->contentItem(), QStringLiteral("audioClipMoveHandle")) != 1 ||
      count_visible_items(window->contentItem(), QStringLiteral("audioClipLeftTrimHandle")) != 0 ||
      count_visible_items(window->contentItem(), QStringLiteral("audioClipRightTrimHandle")) != 0) {
    std::cerr << "WAV import smoke: short clip handle layout is unsafe\n";
    return false;
  }
  std::cout << "Qt Quick audio import reopened\n";
  return true;
}

bool start_import(ImportScenario scenario, QApplication& app,
                  oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                  const std::filesystem::path& project_root, const std::filesystem::path& source) {
  if (scenario == ImportScenario::none) {
    return true;
  }
  opened_revision = bridge.revision();
  // The import action lives in the context panel, which starts closed.
  if (window) {
    window->setProperty("contextPanelOpen", true);
    QCoreApplication::processEvents();
  }
  if (!window ||
      count_visible_items(window->contentItem(), QStringLiteral("importAudioButton")) != 1) {
    std::cerr << "WAV import smoke: import action not visible\n";
    return false;
  }
  bridge.imports()->import_audio(QUrl::fromLocalFile(QString::fromStdString(source.string())));
  if (!bridge.imports()->import_busy() || bridge.imports()->import_enabled() ||
      bridge.exports()->export_enabled() || bridge.playback_enabled()) {
    std::cerr << "WAV import smoke: worker or action guard did not start: "
              << bridge.imports()->import_status().toStdString() << '\n';
    return false;
  }
  const auto revision = bridge.revision();
  bridge.open_project(QUrl::fromLocalFile(QString::fromStdString(project_root.string())));
  if (bridge.revision() != revision || !bridge.status().contains(QStringLiteral("Finish import"))) {
    std::cerr << "WAV import smoke: project switch was allowed during import\n";
    return false;
  }
  if (scenario == ImportScenario::stale) {
    bridge.add_instrument_track();
    if (bridge.revision() != revision + 1) {
      std::cerr << "WAV import smoke: independent edit was blocked\n";
      return false;
    }
  }
  if (scenario == ImportScenario::close) {
    QObject::connect(&bridge, &oscillate::quick::ProjectBridge::readyToClose, &app,
                     [&app, &bridge, revision]() {
                       if (bridge.imports()->import_busy() || bridge.revision() != revision ||
                           !content_tracks(bridge).isEmpty()) {
                         std::cerr << "WAV import close smoke: import committed after close\n";
                         app.exit(1);
                         return;
                       }
                       std::cout << "Qt Quick WAV import cancelled before close\n";
                       app.quit();
                     });
    if (bridge.request_close()) {
      std::cerr << "WAV import close smoke: close did not await worker\n";
      return false;
    }
  }
  return true;
}

void observe_import(ImportScenario scenario, QApplication& app,
                    oscillate::quick::ProjectBridge& bridge) {
  if (scenario == ImportScenario::none) {
    return;
  }
  QTimer::singleShot(10'000, &app, [&app, &bridge]() {
    // The worker can complete on this exact tick before the 20 ms observer
    // validates the saved track. A completed import is not a timeout.
    if (!bridge.imports()->import_busy())
      return;
    std::cerr << "WAV import smoke: timed out: " << bridge.imports()->import_status().toStdString()
              << '\n';
    app.exit(1);
  });
  if (scenario == ImportScenario::close) {
    return;
  }
  auto* timer = new QTimer(&app);
  timer->setInterval(20);
  QObject::connect(timer, &QTimer::timeout, &app, [&app, &bridge, scenario, timer]() {
    if (bridge.imports()->import_busy()) {
      return;
    }
    timer->stop();
    const auto status = bridge.imports()->import_status();
    if (!bridge.imports()->import_enabled() || !bridge.exports()->export_enabled() ||
        !bridge.playback_enabled()) {
      std::cerr << "WAV import smoke: controls did not recover\n";
      app.exit(1);
      return;
    }
    if (scenario == ImportScenario::failure) {
      if (bridge.revision() != opened_revision || !content_tracks(bridge).isEmpty() ||
          !status.contains(QStringLiteral("failed"))) {
        std::cerr << "WAV import smoke: failure changed project: " << status.toStdString() << '\n';
        app.exit(1);
        return;
      }
      std::cout << "Qt Quick WAV import failure preserved\n";
    } else if (scenario == ImportScenario::stale) {
      if (bridge.revision() != opened_revision + 1 || content_tracks(bridge).size() != 1 ||
          !status.contains(QStringLiteral("stale"))) {
        std::cerr << "WAV import smoke: stale stage was installed: " << status.toStdString()
                  << '\n';
        app.exit(1);
        return;
      }
      std::cout << "Qt Quick stale WAV import rejected\n";
    } else {
      const auto tracks = content_tracks(bridge);
      if (bridge.revision() != opened_revision + 1 || tracks.size() != 1 ||
          tracks.front().toMap().value(QStringLiteral("kind")).toString() !=
              QStringLiteral("audio") ||
          tracks.front().toMap().value(QStringLiteral("clips")).toList().size() != 1 ||
          !status.startsWith(QStringLiteral("Audio imported and saved"))) {
        std::cerr << "WAV import smoke: source/track/clip did not commit: " << status.toStdString()
                  << '\n';
        app.exit(1);
        return;
      }
      std::cout << "Qt Quick audio imported\n";
      if (scenario == ImportScenario::undo) {
        bridge.undo();
        if (!content_tracks(bridge).isEmpty() ||
            !bridge.status().startsWith(QStringLiteral("Undo applied"))) {
          std::cerr << "WAV import smoke: one undo did not remove the track\n";
          app.exit(1);
          return;
        }
        bridge.save();
        if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
          std::cerr << "WAV import smoke: undo save failed\n";
          app.exit(1);
          return;
        }
        std::cout << "Qt Quick WAV import undone\n";
      }
    }
    app.quit();
  });
  timer->start();
}

} // namespace quick_smoke
