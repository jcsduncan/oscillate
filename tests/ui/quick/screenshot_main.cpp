// Renders the real Qt Quick shell at its design size and saves the first
// settled frame: how UI work is compared with its mockups. A developer tool
// built beside the UI tests; never part of the shipped application.
#include "bundled_fonts.hpp"
#include "computer_keyboard.hpp"
#include "project_bridge.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr int design_width = 1600;
constexpr int design_height = 1000;
constexpr int default_settle_ms = 1'500;
constexpr int select_clip_delay_ms = 200;
constexpr int dock_tab_delay_ms = 400;

int run(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Oscillate"));
  QCoreApplication::setApplicationName(QStringLiteral("Oscillate"));
  if (!oscillate::quick::register_bundled_fonts())
    std::cerr << "Bundled fonts did not load; using system fonts\n";

  oscillate::quick::ProjectBridge bridge;
  app.installEventFilter(bridge.computer_keyboard());
  constexpr std::string_view usage =
      "Usage: oscillate_quick_screenshot PNG [--view arrange|mixer] [--settle MS]\n"
      "                                  [--select-first-clip] [--visible-bars N]\n"
      "                                  [--first-bar N] [--dock-tab 0|1|2]\n"
      "                                  [--create FOLDER | --open FOLDER]\n";
  if (argc < 2) {
    std::cerr << usage;
    return 2;
  }
  // --view picks the workspace view to render, --settle how long to wait
  // first (background analysis included), --select-first-clip selects the
  // first clip and --dock-tab the device dock's tab: 0 chain, 1 clip, 2 notes.
  const QString screenshot = QString::fromLocal8Bit(argv[1]);
  std::optional<QString> view;
  int settle_ms = default_settle_ms;
  bool select_first_clip = false;
  std::optional<int> dock_tab;
  std::optional<double> visible_bars;
  std::optional<double> first_bar;
  std::string_view project_option;
  std::filesystem::path project_path;
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    const bool has_value = index + 1 < argc;
    if (option == "--view" && has_value) {
      view = QString::fromLocal8Bit(argv[++index]);
    } else if (option == "--settle" && has_value) {
      settle_ms = std::max(0, std::stoi(argv[++index]));
    } else if (option == "--select-first-clip") {
      select_first_clip = true;
    } else if (option == "--dock-tab" && has_value) {
      dock_tab = std::stoi(argv[++index]);
    } else if (option == "--visible-bars" && has_value) {
      visible_bars = std::stod(argv[++index]);
    } else if (option == "--first-bar" && has_value) {
      first_bar = std::stod(argv[++index]);
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
      std::cerr << "Project: " << session.error().message << '\n';
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
    std::cerr << "Qt Quick shell failed to load\n";
    return 1;
  }
  if (view)
    window->setProperty("currentView", *view);
  if (visible_bars)
    window->setProperty("visibleBars", *visible_bars);
  if (first_bar)
    window->setProperty("viewStartBar", *first_bar);
  window->resize(design_width, design_height);
  if (select_first_clip) {
    QTimer::singleShot(select_clip_delay_ms, &app, [&bridge, window] {
      for (const auto& track : bridge.tracks()) {
        const auto clips = track.toMap().value(QStringLiteral("clips")).toList();
        if (!clips.isEmpty()) {
          window->setProperty("selectedClipId",
                              clips.front().toMap().value(QStringLiteral("id")).toString());
          return;
        }
      }
    });
  }
  if (dock_tab) {
    QTimer::singleShot(dock_tab_delay_ms, &app, [window, tab = *dock_tab] {
      if (auto* dock = window->findChild<QObject*>(QStringLiteral("deviceChainDock")))
        dock->setProperty("activeTab", tab);
    });
  }
  QTimer::singleShot(settle_ms, &app, [&app, window, path = screenshot]() {
    if (!window->grabWindow().save(path, "PNG")) {
      std::cerr << "Screenshot: could not write " << path.toStdString() << '\n';
      app.exit(1);
      return;
    }
    app.quit();
  });
  return app.exec();
}

} // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
