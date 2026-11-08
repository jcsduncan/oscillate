#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <variant>

// A MIDI clip's left edge in the arrangement (ADR-0096): dragging its
// handle moves the clip's start onto the grid while the notes stay on the
// timeline, so the notes before the new start are hidden; dragging back
// shows them again. Each trim is one undo step.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * beat;
constexpr std::int64_t clip_end = 2 * bar;
constexpr std::int64_t late_note = bar + beat;
constexpr std::uint8_t early_key = 60;
constexpr std::uint8_t late_key = 64;
constexpr int window_width = 1600;
constexpr int window_height = 1000;
constexpr double visible_bars = 8.0;
// How far the edge is dragged: between the two notes and off every grid
// line, so the drop has to snap.
constexpr double trim_drag_bars = 0.9;
constexpr int drag_steps = 8;
constexpr int both_notes = 2;
constexpr int one_note = 1;

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name && parent->isVisible())
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

int count_items(QQuickItem* parent, const QString& name) {
  int count = parent->objectName() == name && parent->isVisible() ? 1 : 0;
  for (auto* child : parent->childItems())
    count += count_items(child, name);
  return count;
}

std::int64_t clip_start(const application::ProjectSession& session) {
  const auto& clip = session.snapshot().tracks.front().midi_clips.front();
  const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement);
  require(musical != nullptr, "the clip left musical time");
  return musical->start.value;
}

class Shell {
public:
  explicit Shell(quick::ProjectBridge& bridge) {
    qml_.rootContext()->setContextProperty("projectBridge", &bridge);
    qml_.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!qml_.rootObjects().isEmpty(), "DAW shell failed to load");
    window_ = qobject_cast<QQuickWindow*>(qml_.rootObjects().front());
    require(window_ != nullptr, "DAW shell is not a Quick window");
    window_->resize(window_width, window_height);
    window_->setProperty("visibleBars", visible_bars);
    window_->setProperty("viewStartBar", 0.0);
    window_->requestActivate();
    require(QTest::qWaitForWindowActive(window_), "DAW shell window never became active");
    QCoreApplication::processEvents();
  }

  [[nodiscard]] QQuickItem* item(const QString& name) const {
    return find_item(window_->contentItem(), name);
  }
  [[nodiscard]] int count(const QString& name) const {
    return count_items(window_->contentItem(), name);
  }
  [[nodiscard]] double bar_width() const {
    auto* arrangement = item(QStringLiteral("arrangementView"));
    require(arrangement != nullptr, "arrangement missing");
    return arrangement->width() / visible_bars;
  }
  [[nodiscard]] int grid_division() const {
    auto* arrangement = item(QStringLiteral("arrangementView"));
    require(arrangement != nullptr, "arrangement missing");
    return arrangement->property("gridDivision").toInt();
  }
  // Drags the left edge handle's centre by `bars`.
  void drag_left_edge(double bars) const {
    auto* handle = item(QStringLiteral("midiClipLeftTrimHandle"));
    require(handle != nullptr, "the MIDI clip has no left edge handle");
    const auto from = handle->mapToScene({handle->width() / 2, handle->height() / 2}).toPoint();
    const auto to = from + QPointF(bars * bar_width(), 0).toPoint();
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= drag_steps; ++step)
      QTest::mouseMove(window_, from + (to - from) * step / drag_steps);
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, to);
    QCoreApplication::processEvents();
  }

private:
  QQmlApplicationEngine qml_;
  QQuickWindow* window_ = nullptr;
};

void left_edge_trims_on_the_grid() {
  QTemporaryDir directory;
  require(directory.isValid(), "trim fixture directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "song";
  auto created = application::ProjectSession::create(root);
  require(created.has_value(), "project creation failed");
  auto& session = *created.value();
  const auto track = session.instruments().add_synth_track();
  require(track.has_value(), "instrument track was not added");
  require(session.midi_clips().add_midi_clip(track.value(), {0}).has_value(),
          "MIDI clip was not added");
  const auto clip = session.snapshot().tracks.front().midi_clips.front().id;
  require(session.midi_clips().resize_midi_clip(clip, {clip_end}).has_value() &&
              session.notes().add_note(clip, {0}, early_key).has_value() &&
              session.notes().add_note(clip, {late_note}, late_key).has_value(),
          "the clip's notes were not added");

  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  const Shell shell(bridge);
  require(shell.count(QStringLiteral("arrangementClipNote")) == both_notes,
          "the untrimmed clip does not draw both notes");

  // The start lands on the grid line nearest the drop, between the notes.
  const auto division = shell.grid_division();
  require(division > 0, "the arrangement has no grid");
  const auto snapped_bars = std::round(trim_drag_bars * division) / division;
  require(snapped_bars != trim_drag_bars, "the drag would land on a grid line unsnapped");
  const auto snapped = static_cast<std::int64_t>(std::llround(snapped_bars * bar));
  require(snapped > 0 && snapped < late_note, "the snapped start is not between the notes");
  const auto revision = bridge.revision();
  shell.drag_left_edge(trim_drag_bars);
  require(bridge.revision() == revision + 1, "the trim was not one edit");
  require(clip_start(session) == snapped, "the left edge did not snap the start to the grid");
  require(shell.count(QStringLiteral("arrangementClipNote")) == one_note,
          "the note before the new start is still drawn");

  // Dragging the edge back to bar 1 shows the hidden note again.
  shell.drag_left_edge(-snapped_bars);
  require(clip_start(session) == 0, "dragging the edge back did not restore the start");
  require(shell.count(QStringLiteral("arrangementClipNote")) == both_notes,
          "dragging the edge back did not reveal the note");

  // One undo takes back one trim.
  bridge.undo();
  QCoreApplication::processEvents();
  require(clip_start(session) == snapped, "undo did not restore the trimmed start");
  require(shell.count(QStringLiteral("arrangementClipNote")) == one_note,
          "undo did not hide the note again");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    left_edge_trims_on_the_grid();
  } catch (const std::exception& error) {
    std::cerr << "Arrangement clip trim test: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
