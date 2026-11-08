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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

// Ctrl-dragging clips in the arrangement (UQ-14): a Ctrl-drag drops copies
// at the snapped drop and leaves the originals, showing a copy cursor, a
// plus on the dragged clip and the original in place while it drags; the
// copies become the selection; a Ctrl-click without a drag still picks.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

constexpr std::int64_t bar = 4 * timeline::ticks_per_quarter;
constexpr std::uint8_t key = 60;
constexpr int window_width = 1600;
constexpr int window_height = 1000;
constexpr double visible_bars = 8.0;
// Where the clip is grabbed, and how far each drag goes: just past a bar
// line, so the drop has to snap back onto it.
constexpr double grab_bar = 0.5;
constexpr double off_grid = 0.03;
constexpr double first_copy_bars = 2.0;
constexpr double group_copy_bars = 4.0;
constexpr int drag_steps = 8;

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name && parent->isVisible())
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

// The visible item named `name` whose scene rectangle holds `point`.
QQuickItem* item_at(QQuickItem* parent, const QString& name, QPointF point) {
  if (parent->objectName() == name && parent->isVisible() &&
      parent->mapRectToScene(QRectF(0, 0, parent->width(), parent->height())).contains(point))
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = item_at(child, name, point))
      return found;
  }
  return nullptr;
}

// Every MIDI clip's start, in ticks.
std::multiset<std::int64_t> starts(const application::ProjectSession& session) {
  std::multiset<std::int64_t> found;
  for (const auto& clip : session.snapshot().tracks.front().midi_clips)
    found.insert(std::get<project::MusicalInterval>(clip.placement).start.value);
  return found;
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
  // The scene point at `at_bar` on the only track's row.
  [[nodiscard]] QPoint lane_point(double at_bar) const {
    auto* arrangement = item(QStringLiteral("arrangementView"));
    auto* row = item(QStringLiteral("arrangementRow"));
    require(arrangement != nullptr && row != nullptr, "arrangement missing");
    const auto x = arrangement->mapToScene({arrangement->width() * at_bar / visible_bars, 0}).x();
    const auto y = row->mapToScene({0, row->height() / 2}).y();
    return QPointF(x, y).toPoint();
  }
  // The clip move handle under `point`.
  [[nodiscard]] QQuickItem* handle_at(QPoint point) const {
    return item_at(window_->contentItem(), QStringLiteral("midiClipMoveHandle"), point);
  }
  void press(QPoint at, Qt::KeyboardModifiers modifiers) const {
    QTest::mousePress(window_, Qt::LeftButton, modifiers, at);
  }
  // Moves with `modifiers` held, as a real pointer reports them.
  void move(QPoint from, QPoint to, Qt::KeyboardModifiers modifiers) const {
    for (int step = 1; step <= drag_steps; ++step)
      QTest::mouseEvent(QTest::MouseMove, window_, Qt::NoButton, modifiers,
                        from + (to - from) * step / drag_steps);
    QCoreApplication::processEvents();
  }
  void release(QPoint at, Qt::KeyboardModifiers modifiers) const {
    QTest::mouseRelease(window_, Qt::LeftButton, modifiers, at);
    QCoreApplication::processEvents();
  }
  void click(QPoint at, Qt::KeyboardModifiers modifiers) const {
    QTest::mouseClick(window_, Qt::LeftButton, modifiers, at);
    QCoreApplication::processEvents();
  }

private:
  QQmlApplicationEngine qml_;
  QQuickWindow* window_ = nullptr;
};

void ctrl_drag_copies_clips() {
  QTemporaryDir directory;
  require(directory.isValid(), "copy fixture directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "song";
  auto created = application::ProjectSession::create(root);
  require(created.has_value(), "project creation failed");
  auto& session = *created.value();
  const auto track = session.instruments().add_synth_track();
  require(track.has_value(), "instrument track was not added");
  require(session.midi_clips().add_midi_clip(track.value(), {0}).has_value(),
          "MIDI clip was not added");
  const auto original = session.snapshot().tracks.front().midi_clips.front().id;
  require(session.notes().add_note(original, {0}, key).has_value(), "the note was not added");
  const auto original_id = QString::fromStdString(original.to_string());

  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  const Shell shell(bridge);

  // While it drags with Ctrl: a copy cursor, a plus on the dragged clip,
  // and the original drawn where it is.
  const auto grab = shell.lane_point(grab_bar);
  const auto drop = shell.lane_point(grab_bar + first_copy_bars + off_grid);
  auto* handle = shell.handle_at(grab);
  require(handle != nullptr, "no MIDI clip under the grab point");
  const auto revision = bridge.revision();
  shell.press(grab, Qt::ControlModifier);
  shell.move(grab, drop, Qt::ControlModifier);
  require(handle->property("cursorShape").toInt() == Qt::DragCopyCursor,
          "a Ctrl-drag does not show the copy cursor");
  require(shell.item(QStringLiteral("clipCopyBadge")) != nullptr,
          "the dragged clip does not show a plus");
  require(shell.item(QStringLiteral("clipCopyOrigin")) != nullptr,
          "the original is not drawn in place while copying");
  shell.release(drop, Qt::ControlModifier);
  require(bridge.revision() == revision + 1, "the copy was not one edit");
  require(starts(session) ==
              std::multiset<std::int64_t>{0, static_cast<std::int64_t>(first_copy_bars * bar)},
          "the copy did not land on the bar line, beside the original");
  require(shell.item(QStringLiteral("clipCopyBadge")) == nullptr, "the plus stayed after the drop");
  const auto selected = bridge.selected_clip_ids();
  require(selected.size() == 1 && selected.front() != original_id,
          "the copy did not become the selection");

  // A Ctrl-click without a drag picks the original too, and copies nothing.
  const auto picked_at = bridge.revision();
  shell.click(grab, Qt::ControlModifier);
  require(bridge.revision() == picked_at, "a Ctrl-click made an edit");
  require(bridge.selected_clip_ids().size() == 2 &&
              bridge.selected_clip_ids().contains(original_id),
          "a Ctrl-click did not add the clip to the selection");

  // A Ctrl-drag of a selected clip copies the whole selection together.
  const auto group_drop = shell.lane_point(grab_bar + group_copy_bars + off_grid);
  const auto grouped_at = bridge.revision();
  shell.press(grab, Qt::ControlModifier);
  shell.move(grab, group_drop, Qt::ControlModifier);
  shell.release(group_drop, Qt::ControlModifier);
  require(bridge.revision() == grouped_at + 1, "the group copy was not one edit");
  const auto group_offset = static_cast<std::int64_t>(group_copy_bars * bar);
  const auto copy_start = static_cast<std::int64_t>(first_copy_bars * bar);
  require(starts(session) ==
              std::multiset<std::int64_t>{0, copy_start, group_offset, copy_start + group_offset},
          "the selection's copies did not keep their spacing");
  require(bridge.selected_clip_ids().size() == 2 &&
              !bridge.selected_clip_ids().contains(original_id),
          "the group's copies did not become the selection");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    ctrl_drag_copies_clips();
  } catch (const std::exception& error) {
    std::cerr << "Arrangement clip copy test: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
