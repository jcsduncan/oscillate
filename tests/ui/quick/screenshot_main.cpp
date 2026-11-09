// Renders the real Qt Quick shell at its design size and saves the first
// settled frame: how UI work is compared with its mockups. A developer tool
// built beside the UI tests; never part of the shipped application.
#include "bundled_fonts.hpp"
#include "computer_keyboard.hpp"
#include "project_bridge.hpp"
#include "screenshot_demo.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

#include <algorithm>
#include <cmath>
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
// Long enough for the shell to load and the output devices to be listed.
constexpr int play_delay_ms = 600;

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
      "                                  [--select-track N] [--select-first-clip]\n"
      "                                  [--visible-bars N] [--tempo-lane]\n"
      "                                  [--first-bar N] [--dock-tab 0|1|2]\n"
      "                                  [--play-from BAR] [--size WIDTHxHEIGHT]\n"
      "                                  [--create FOLDER | --open FOLDER | --demo FOLDER]\n";
  if (argc < 2) {
    std::cerr << usage;
    return 2;
  }
  // --view picks the workspace view to render, --settle how long to wait
  // first (background analysis included), --select-first-clip selects the
  // first clip (of the selected track, with --select-track N) and
  // --dock-tab the device dock's tab: 0 chain, 1 clip, 2 notes.
  // --size sets the window's logical size (default the design size).
  // --tempo-lane expands the tempo lane under the ruler.
  // --play-from plays from that zero-based bar until the shot, so meters and
  // device displays are live; --demo writes the documentation project into
  // a new folder and opens it.
  const QString screenshot = QString::fromLocal8Bit(argv[1]);
  std::optional<QString> view;
  int settle_ms = default_settle_ms;
  bool select_first_clip = false;
  bool tempo_lane = false;
  std::optional<int> select_track;
  std::optional<int> dock_tab;
  std::optional<double> visible_bars;
  std::optional<double> first_bar;
  std::optional<double> play_from;
  int width = design_width;
  int height = design_height;
  std::string_view project_option;
  std::filesystem::path project_path;
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    const bool has_value = index + 1 < argc;
    if (option == "--view" && has_value) {
      view = QString::fromLocal8Bit(argv[++index]);
    } else if (option == "--settle" && has_value) {
      settle_ms = std::max(0, std::stoi(argv[++index]));
    } else if (option == "--select-track" && has_value && std::stoi(argv[index + 1]) >= 0) {
      select_track = std::stoi(argv[++index]);
    } else if (option == "--select-first-clip") {
      select_first_clip = true;
    } else if (option == "--tempo-lane") {
      tempo_lane = true;
    } else if (option == "--dock-tab" && has_value) {
      dock_tab = std::stoi(argv[++index]);
    } else if (option == "--visible-bars" && has_value) {
      visible_bars = std::stod(argv[++index]);
    } else if (option == "--first-bar" && has_value) {
      first_bar = std::stod(argv[++index]);
    } else if (option == "--size" && has_value) {
      const std::string size(argv[++index]);
      const auto separator = size.find('x');
      if (separator == std::string::npos) {
        std::cerr << usage;
        return 2;
      }
      width = std::stoi(size.substr(0, separator));
      height = std::stoi(size.substr(separator + 1));
      if (width <= 0 || height <= 0) {
        std::cerr << usage;
        return 2;
      }
    } else if (option == "--play-from" && has_value) {
      play_from = std::stod(argv[++index]);
      if (!std::isfinite(*play_from) || *play_from < 0.0) {
        std::cerr << usage;
        return 2;
      }
    } else if ((option == "--create" || option == "--open" || option == "--demo") && has_value &&
               project_option.empty()) {
      project_option = option;
      project_path = argv[++index];
    } else {
      std::cerr << usage;
      return 2;
    }
  }
  if (play_from && settle_ms <= play_delay_ms) {
    std::cerr << "--play-from needs a --settle longer than " << play_delay_ms
              << " ms, when playback starts\n";
    return 2;
  }
  if (project_option == "--demo")
    oscillate::quick::screenshot::create_demo_project(project_path);
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
  window->resize(width, height);
  if (tempo_lane) {
    auto* workspace = window->findChild<QObject*>(QStringLiteral("arrangementWorkspace"));
    if (workspace == nullptr) {
      std::cerr << "--tempo-lane: the shell has no arrangement workspace\n";
      return 1;
    }
    workspace->setProperty("tempoLaneExpanded", true);
  }
  if (select_track) {
    if (*select_track >= bridge.tracks().size()) {
      std::cerr << "--select-track " << *select_track << ": the project has "
                << bridge.tracks().size() << " tracks\n";
      return 1;
    }
    window->setProperty("selectedTrack", *select_track);
  }
  if (select_first_clip) {
    QTimer::singleShot(select_clip_delay_ms, &app, [&bridge, window, select_track] {
      // The selected track's first clip; without --select-track, the first clip anywhere.
      const auto tracks = bridge.tracks();
      const qsizetype first = select_track.value_or(0);
      const qsizetype last =
          select_track ? std::min<qsizetype>(first + 1, tracks.size()) : tracks.size();
      for (qsizetype index = first; index < last; ++index) {
        const auto clips = tracks.at(index).toMap().value(QStringLiteral("clips")).toList();
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
  if (play_from) {
    QTimer::singleShot(play_delay_ms, &app, [&bridge, bar = *play_from] {
      bridge.seek_to_bar(bar);
      bridge.toggle_playback();
    });
  }
  QTimer::singleShot(settle_ms, &app, [&app, &bridge, window, path = screenshot]() {
    const QImage frame = window->grabWindow();
    bridge.stop_playback();
    if (!frame.save(path, "PNG")) {
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
