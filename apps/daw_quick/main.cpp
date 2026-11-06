#include "app_logging.hpp"
#include "bundled_fonts.hpp"
#include "computer_keyboard.hpp"
#include "project_bridge.hpp"

#include "oscillate/foundation/log.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
namespace log = oscillate::foundation::log;

int run(int argc, char** argv) {
  oscillate::quick::start_logging();
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Oscillate"));
  QCoreApplication::setApplicationName(QStringLiteral("Oscillate"));
  if (!oscillate::quick::register_bundled_fonts())
    log::warning(log::Category::application, "Bundled fonts did not load; using system fonts");

  oscillate::quick::ProjectBridge bridge;
  app.installEventFilter(bridge.computer_keyboard());
  constexpr std::string_view usage =
      "Usage: oscillate [--check-startup] [--create FOLDER | --open FOLDER]\n";
  bool check_startup = false;
  std::string_view project_option;
  std::filesystem::path project_path;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    const bool has_value = index + 1 < argc;
    if (option == "--check-startup") {
      check_startup = true;
    } else if ((option == "--create" || option == "--open") && has_value &&
               project_option.empty()) {
      project_option = option;
      project_path = argv[++index];
    } else {
      std::cerr << usage;
      return 2;
    }
  }
  if (!project_option.empty()) {
    auto session = project_option == "--create"
                       ? oscillate::application::ProjectSession::create(project_path)
                       : oscillate::application::ProjectSession::open(project_path);
    if (!session) {
      log::error(log::Category::project, "Project: {} ({})", session.error().message,
                 project_path.string());
      return 1;
    }
    bridge.set_project(std::move(session.value()), project_path);
  }

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("projectBridge", &bridge);
  engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  auto* window = engine.rootObjects().isEmpty()
                     ? nullptr
                     : qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  if (window == nullptr) {
    log::error(log::Category::application, "Qt Quick shell failed to load");
    return 1;
  }
  bridge.performance_trace()->attach(window);
  if (check_startup) {
    QTimer::singleShot(0, &app, [&app, &bridge, window]() {
      const QString expected = bridge.project_open()
                                   ? QStringLiteral("OSCILLATE — ") + bridge.project_name()
                                   : QStringLiteral("OSCILLATE — Welcome");
      if (!window->isVisible() || window->title() != expected) {
        log::error(log::Category::application,
                   "Qt Quick startup state did not match the opened project");
        app.exit(1);
        return;
      }
      std::cout << (bridge.project_open() ? "Project ready: " : "Welcome ready: ")
                << window->title().toStdString() << '\n';
      app.quit();
    });
  }
  const int code = app.exec();
  log::info(log::Category::application, "Oscillate exiting with code {}", code);
  log::flush();
  return code;
}

} // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    log::error(log::Category::application, "Unhandled exception: {}", error.what());
    log::flush();
    return 1;
  }
}
