#include "smoke_export_scenario.hpp"

#include "project_bridge.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <iostream>

namespace quick_smoke {

bool start_master_export(const ExportOptions& options, QApplication& app,
                         QQmlApplicationEngine& engine, oscillate::quick::ProjectBridge& bridge,
                         VisibleItemCount count_visible_items) {
  if (options.master == MasterExportScenario::none) {
    return true;
  }
  bridge.exports()->set_export_target({});
  bridge.exports()->export_master();
  if (bridge.exports()->export_busy() ||
      !bridge.exports()->export_status().contains(QStringLiteral("Choose an output file"))) {
    std::cerr << "WAV export smoke: empty target was accepted\n";
    return false;
  }
  if (options.master == MasterExportScenario::close) {
    QObject::connect(
        &bridge, &oscillate::quick::ProjectBridge::readyToClose, &app, [&app, &bridge]() {
          if (bridge.exports()->export_busy() ||
              !bridge.exports()->export_status().startsWith(QStringLiteral("WAV exported"))) {
            std::cerr << "WAV export close smoke: completion was not durable\n";
            app.exit(1);
            return;
          }
          std::cout << "Qt Quick WAV master finished before close\n";
          app.quit();
        });
  }
  bridge.exports()->set_export_target(QString::fromStdString(options.target.string()));
  bridge.exports()->set_export_format_index(options.format);
  if (bridge.exports()->export_rate_index() != 0 ||
      bridge.exports()->output_rate_conversion_available() !=
          (OSCILLATE_HAVE_EXPORT_RESAMPLER != 0) ||
      !bridge.exports()->export_rate_available(0) ||
      bridge.exports()->export_rate_availability().size() != 5) {
    std::cerr << "Export project-rate default or conversion availability is wrong\n";
    return false;
  }
  bridge.exports()->set_export_rate_index(3);
  if (bridge.exports()->export_rate_index() != (OSCILLATE_HAVE_EXPORT_RESAMPLER ? 3 : 0)) {
    std::cerr << "Export rate selection ignored build availability\n";
    return false;
  }
  if (OSCILLATE_HAVE_CODEC && OSCILLATE_HAVE_EXPORT_RESAMPLER) {
    bridge.exports()->set_export_format_index(5);
    if (bridge.exports()->master_export_rate_valid() ||
        !bridge.exports()->export_rate_availability()[3].toBool() ||
        !bridge.exports()->export_rate_status().contains(QStringLiteral("48 kHz"))) {
      std::cerr << "MP3 accepted an unsupported high output rate\n";
      return false;
    }
  }
  bridge.exports()->set_export_rate_index(0);
  bridge.exports()->set_export_format_index(options.format);
  auto* export_window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  // The toolbar's Export opens the dialog; nothing renders until then.
  if (!export_window ||
      count_visible_items(export_window->contentItem(), QStringLiteral("exportDialog")) != 0) {
    std::cerr << "Export dialog was visible before it was opened\n";
    return false;
  }
  export_window->setProperty("exportDialogOpen", true);
  QCoreApplication::processEvents();
  if (count_visible_items(export_window->contentItem(), QStringLiteral("exportDialog")) != 1 ||
      count_visible_items(export_window->contentItem(), QStringLiteral("exportWavButton")) != 1 ||
      count_visible_items(export_window->contentItem(), QStringLiteral("exportFileName")) != 1 ||
      count_visible_items(export_window->contentItem(), QStringLiteral("exportFolder")) != 1 ||
      count_visible_items(export_window->contentItem(), QStringLiteral("exportTrimTailCheck")) !=
          1 ||
      !bridge.exports()->export_trim_tail()) {
    std::cerr << "Export dialog did not render its settings\n";
    return false;
  }
  engine.rootObjects().front()->setProperty("exportFormatMenuOpen", true);
  QCoreApplication::processEvents();
  if (count_visible_items(export_window->contentItem(), QStringLiteral("masterFormatMenu")) != 1 ||
      count_visible_items(export_window->contentItem(), QStringLiteral("masterFormatOption")) !=
          (OSCILLATE_HAVE_CODEC ? 6 : 3)) {
    std::cerr << "Master export format menu did not render its options\n";
    return false;
  }
  engine.rootObjects().front()->setProperty("exportFormatMenuOpen", false);
  engine.rootObjects().front()->setProperty("exportRateMenuOpen", true);
  QCoreApplication::processEvents();
  if (count_visible_items(export_window->contentItem(), QStringLiteral("exportRateMenu")) != 1 ||
      count_visible_items(export_window->contentItem(), QStringLiteral("exportRateOption")) != 5) {
    std::cerr << "Export rate menu did not render its options\n";
    return false;
  }
  engine.rootObjects().front()->setProperty("exportRateMenuOpen", false);
  export_window->setProperty("exportDialogOpen", false);
  if (options.output_rate != 0) {
    bridge.exports()->set_export_rate_index(options.output_rate);
    if (bridge.exports()->export_rate_index() != options.output_rate ||
        !bridge.exports()->master_export_rate_valid()) {
      std::cerr << "Requested export rate is unavailable\n";
      return false;
    }
  }
  bridge.exports()->export_master();
  if (!bridge.exports()->export_busy() || bridge.exports()->export_enabled() ||
      bridge.playback_enabled()) {
    std::cerr << "WAV export smoke: worker or transport guard did not start\n";
    return false;
  }
  const auto revision = bridge.revision();
  bridge.open_project(QUrl::fromLocalFile(QString::fromStdString(options.project_root.string())));
  if (bridge.revision() != revision || !bridge.status().contains(QStringLiteral("Finish export"))) {
    std::cerr << "WAV export smoke: project switch was allowed\n";
    return false;
  }
  bridge.toggle_playback();
  if (bridge.playback_active()) {
    std::cerr << "WAV export smoke: playback began during export\n";
    return false;
  }
  if (options.master == MasterExportScenario::close && bridge.request_close()) {
    std::cerr << "WAV export close smoke: close did not wait for worker\n";
    return false;
  }
  return true;
}

void observe_master_export(const ExportOptions& options, QApplication& app,
                           oscillate::quick::ProjectBridge& bridge) {
  if (options.master == MasterExportScenario::none) {
    return;
  }
  QTimer::singleShot(10'000, &app, [&app, &bridge]() {
    std::cerr << "WAV export smoke: timed out: " << bridge.exports()->export_status().toStdString()
              << '\n';
    app.exit(1);
  });
  if (options.master == MasterExportScenario::close) {
    return;
  }
  auto* timer = new QTimer(&app);
  timer->setInterval(20);
  QObject::connect(timer, &QTimer::timeout, &app,
                   [&app, &bridge, failed = options.master == MasterExportScenario::failure,
                    format = options.format, timer]() {
                     if (bridge.exports()->export_busy()) {
                       return;
                     }
                     timer->stop();
                     const auto format_name = format < 3    ? QStringLiteral("WAV")
                                              : format == 5 ? QStringLiteral("MP3")
                                                            : QStringLiteral("FLAC");
                     if (!bridge.exports()->export_enabled() || !bridge.playback_enabled() ||
                         bridge.exports()->export_status().contains(
                             format_name + QStringLiteral(" export failed")) != failed) {
                       std::cerr << "WAV export smoke: unexpected completion: "
                                 << bridge.exports()->export_status().toStdString() << '\n';
                       app.exit(1);
                       return;
                     }
                     std::cout << "Qt Quick " << format_name.toStdString() << " master "
                               << (failed ? "failure preserved" : "exported") << '\n';
                     app.quit();
                   });
  timer->start();
}

bool start_stem_export(const ExportOptions& options, QApplication& app,
                       QQmlApplicationEngine& engine, oscillate::quick::ProjectBridge& bridge,
                       VisibleItemCount count_visible_items) {
  if (!options.stems) {
    return true;
  }
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  if (window) {
    window->setProperty("exportDialogOpen", true);
    window->setProperty("exportMoreOpen", true);
    QCoreApplication::processEvents();
  }
  if (!window ||
      count_visible_items(window->contentItem(), QStringLiteral("exportStemButton")) != 1) {
    std::cerr << "Stem export button not rendered\n";
    return false;
  }
  const auto target = options.project_root / ("stems-rev-" + std::to_string(bridge.revision()));
  if (options.output_rate != 0) {
    bridge.exports()->set_export_rate_index(options.output_rate);
    if (bridge.exports()->export_rate_index() != options.output_rate) {
      std::cerr << "Requested stem output rate is unavailable\n";
      return false;
    }
  }
  bridge.exports()->export_stems();
  if (!bridge.exports()->export_busy() || bridge.exports()->export_enabled() ||
      bridge.playback_enabled()) {
    std::cerr << "Stem export did not reserve stopped transport\n";
    return false;
  }
  auto* timer = new QTimer(&app);
  timer->setInterval(20);
  QElapsedTimer elapsed;
  elapsed.start();
  QObject::connect(timer, &QTimer::timeout, &app, [&app, &bridge, target, timer, elapsed] {
    if (elapsed.elapsed() > 10'000) {
      std::cerr << "Stem export timed out: " << bridge.exports()->export_status().toStdString()
                << '\n';
      app.exit(1);
      return;
    }
    if (bridge.exports()->export_busy()) {
      return;
    }
    timer->stop();
    std::size_t count = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator it(target, error), end; !error && it != end;
         it.increment(error)) {
      if (it->path().extension() == ".wav" && std::filesystem::file_size(it->path(), error) > 44) {
        ++count;
      }
    }
    // One stem per source track; group and return buses have none.
    std::size_t sources = 0;
    for (const auto& track : bridge.tracks()) {
      const auto kind = track.toMap().value(QStringLiteral("kind")).toString();
      if (kind != QStringLiteral("group") && kind != QStringLiteral("return"))
        ++sources;
    }
    if (error || count != sources ||
        !bridge.exports()->export_status().startsWith(QStringLiteral("WAV stems exported")) ||
        !bridge.playback_enabled()) {
      std::cerr << "Stem export did not publish aligned bundle: "
                << bridge.exports()->export_status().toStdString() << '\n';
      app.exit(1);
      return;
    }
    std::cout << "Qt Quick WAV stem bundle exported\n";
    app.quit();
  });
  timer->start();
  return true;
}

} // namespace quick_smoke
