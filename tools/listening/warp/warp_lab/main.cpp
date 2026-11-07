// Warp Lab: a standalone sandbox for auditioning the warp engines on your own
// audio. It is not part of the DAW; see README.md beside this file.
#include "warp_lab_controller.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
  try {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Oscillate"));
    QCoreApplication::setApplicationName(QStringLiteral("WarpLab"));
    oscillate::warp_lab::WarpLabController lab;
    QQmlApplicationEngine engine;
#ifdef OSCILLATE_WARP_LAB_QML_DIR
    engine.addImportPath(QStringLiteral(OSCILLATE_WARP_LAB_QML_DIR));
#endif
    engine.rootContext()->setContextProperty(QStringLiteral("lab"), &lab);
    engine.load(QUrl(QStringLiteral("qrc:/WarpLab.qml")));
    if (engine.rootObjects().isEmpty())
      return 1;
    // Usage: oscillate_warp_lab [FILE] [--screenshot PNG]
    QString screenshot;
    for (int index = 1; index < argc; ++index) {
      const auto argument = QString::fromLocal8Bit(argv[index]);
      if (argument == QStringLiteral("--screenshot") && index + 1 < argc)
        screenshot = QString::fromLocal8Bit(argv[++index]);
      else
        lab.load(QUrl::fromLocalFile(argument));
    }
    if (!screenshot.isEmpty()) {
      // Layout check without a display: grab after load and render, then quit.
      auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
      QTimer::singleShot(3'000, &app, [window, screenshot] {
        window->grabWindow().save(screenshot);
        QCoreApplication::quit();
      });
    }
    return QGuiApplication::exec();
  } catch (const std::exception& error) {
    std::cerr << "oscillate_warp_lab: " << error.what() << '\n';
    return 1;
  }
}
