#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QList>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Bus nesting in the track list (issue #263): a group bus heads its members,
// indented on its rail; folding it hides them and shows their clips as one
// outline on its lane; the returns sit in their own section with their
// feeders named. Dragging a header into a bus's bracket routes it there, out
// of every bracket routes it to the master, each drop one undo step.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

constexpr int window_width = 1600;
constexpr int window_height = 1000;
constexpr std::int64_t bar = 4 * oscillate::timeline::ticks_per_quarter;
constexpr std::int64_t chords_clip_bar = 2;
constexpr double space_send_gain = 0.5;
// Where a drag grips a header (across) and lands in a row (down): its
// upper or lower part, clear of the bus header's and last member's edges.
constexpr double grip_across = 0.4;
constexpr double upper_part = 0.35;
constexpr double lower_part = 0.65;
constexpr double middle = 0.5;
// Pointer steps for a drag, so it passes the drag threshold, then moves
// that settle it on its target row.
constexpr int drag_steps = 8;
constexpr int settle_moves = 3;

void find_all(QQuickItem* parent, const QString& name, QList<QQuickItem*>& found) {
  if (parent->objectName() == name)
    found.push_back(parent);
  for (auto* child : parent->childItems())
    find_all(child, name, found);
}

QQuickItem* find_visible(QQuickItem* parent, const QString& name) {
  if (!parent->isVisible())
    return nullptr;
  if (parent->objectName() == name)
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_visible(child, name))
      return found;
  }
  return nullptr;
}

QString text_of(const QQuickItem* item) { return item->property("text").toString(); }

class Shell {
public:
  explicit Shell(quick::ProjectBridge& bridge) : bridge_(bridge) {
    qml_.rootContext()->setContextProperty("projectBridge", &bridge);
    qml_.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!qml_.rootObjects().isEmpty(), "DAW shell failed to load");
    window_ = qobject_cast<QQuickWindow*>(qml_.rootObjects().front());
    require(window_ != nullptr, "DAW shell is not a Quick window");
    window_->resize(window_width, window_height);
    window_->requestActivate();
    require(QTest::qWaitForWindowActive(window_), "DAW shell window never became active");
    QCoreApplication::processEvents();
  }

  // The track list's row (or arrangement lane) for a track, shown or not.
  [[nodiscard]] QQuickItem* row(const QString& id, const QString& kind = "trackRow") const {
    QList<QQuickItem*> rows;
    find_all(window_->contentItem(), kind, rows);
    for (auto* candidate : rows) {
      if (candidate->property("modelData").toMap().value("id").toString() == id)
        return candidate;
    }
    throw std::runtime_error("no " + kind.toStdString() + " for track " + id.toStdString());
  }
  [[nodiscard]] double top(const QString& id) const { return row(id)->mapToScene({0.0, 0.0}).y(); }
  [[nodiscard]] QQuickItem* in_row(const QString& id, const QString& name) const {
    return find_visible(row(id), name);
  }
  [[nodiscard]] QQuickItem* item(const QString& name) const {
    return find_visible(window_->contentItem(), name);
  }
  [[nodiscard]] QPoint at(const QString& id, double down) const {
    auto* header = row(id);
    return header->mapToScene({header->width() * grip_across, header->height() * down}).toPoint();
  }

  void click(QQuickItem* target, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const {
    require(target != nullptr, "nothing to click");
    const auto centre = target->mapToScene({target->width() / 2, target->height() / 2});
    QTest::mouseClick(window_, Qt::LeftButton, modifiers, centre.toPoint());
    QCoreApplication::processEvents();
  }
  // Presses on a header and moves onto part of another row, holding the
  // button. The rows shift as the drop slot opens, so each step aims at
  // where that row is now, and the last ones settle on it.
  void drag_onto(QPoint from, const QString& id, double down) {
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= drag_steps; ++step) {
      QTest::mouseMove(window_, from + (at(id, down) - from) * step / drag_steps);
      QCoreApplication::processEvents();
    }
    for (int settle = 0; settle < settle_moves; ++settle) {
      QTest::mouseMove(window_, at(id, down));
      QCoreApplication::processEvents();
    }
    held_ = at(id, down);
  }
  void release() const {
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, held_);
    QCoreApplication::processEvents();
  }
  [[nodiscard]] QString destination(const QString& id) const {
    for (const auto& track : bridge_.tracks()) {
      const auto map = track.toMap();
      if (map.value("id").toString() == id)
        return map.value("mainDestinationId").toString();
    }
    throw std::runtime_error("no track " + id.toStdString());
  }

private:
  quick::ProjectBridge& bridge_;
  QQmlApplicationEngine qml_;
  QQuickWindow* window_ = nullptr;
  QPoint held_;
};

QString text_id(project::TrackId id) { return QString::fromStdString(id.to_string()); }

project::TrackId synth(application::ProjectSession& session, std::string name) {
  const auto id = session.instruments().add_synth_track();
  require(id.has_value(), "instrument track creation failed");
  require(session.tracks().set_track_name(id.value(), std::move(name)).has_value(),
          "track naming failed");
  return id.value();
}

void buses_nest_fold_and_take_drops() {
  QTemporaryDir directory;
  require(directory.isValid(), "fixture directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "song";
  auto created = application::ProjectSession::create(root);
  require(created.has_value(), "project creation failed");
  auto& session = *created.value();

  // Saved order: Kick, Bass, Pad, Music (a group bus made for the Chords),
  // Chords, Space (a return). The Kick feeds Music too, from the top of
  // the saved order; the Chords also send to Space.
  const auto kick = synth(session, "Kick");
  const auto bass = synth(session, "Bass");
  const auto pad = synth(session, "Pad");
  const auto chords = synth(session, "Chords");
  const auto music = session.routing().route_tracks_to_bus(std::vector{chords});
  require(music.has_value(), "the Music bus was not made");
  require(session.tracks().set_track_name(music.value(), "Music").has_value() &&
              session.routing().set_main_output(kick, music.value()).has_value(),
          "the Kick did not join the Music bus");
  const auto space = session.tracks().add_bus_track(project::TrackKind::return_bus);
  require(space.has_value() &&
              session.tracks().set_track_name(space.value(), "Space").has_value() &&
              session.routing().set_send_level(chords, space.value(), space_send_gain).has_value(),
          "the Space return was not made");
  require(session.midi_clips().add_midi_clip(kick, {0}).has_value() &&
              session.midi_clips().add_midi_clip(chords, {chords_clip_bar * bar}).has_value(),
          "the members' clips were not added");
  const auto settings_key = QStringLiteral("collapsedBuses/%1")
                                .arg(QString::fromStdString(session.snapshot().id.to_string()));
  const auto kick_id = text_id(kick);
  const auto bass_id = text_id(bass);
  const auto pad_id = text_id(pad);
  const auto chords_id = text_id(chords);
  const auto music_id = text_id(music.value());
  const auto space_id = text_id(space.value());

  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  Shell shell(bridge);

  // Members sit right under their bus, in saved order, and the return in
  // its own section at the bottom.
  require(shell.top(bass_id) < shell.top(pad_id) && shell.top(pad_id) < shell.top(music_id) &&
              shell.top(music_id) < shell.top(kick_id) &&
              shell.top(kick_id) < shell.top(chords_id) &&
              shell.top(chords_id) < shell.top(space_id),
          "the list does not show Bass, Pad, Music, Kick, Chords, then Space");
  auto* returns = shell.item(QStringLiteral("trackReturnsLabel"));
  require(returns != nullptr && returns->mapToScene({0.0, 0.0}).y() < shell.top(space_id) &&
              returns->mapToScene({0.0, 0.0}).y() >= shell.top(chords_id),
          "the returns section does not head the Space return");
  const auto name_x = [&](const QString& id) {
    return shell.in_row(id, QStringLiteral("trackName"))->mapToScene({0.0, 0.0}).x();
  };
  require(name_x(music_id) > name_x(bass_id) && name_x(kick_id) > name_x(music_id) &&
              name_x(chords_id) == name_x(kick_id),
          "members are not indented under their bus");
  auto* kick_rail = shell.in_row(kick_id, QStringLiteral("trackBusRail"));
  auto* chords_rail = shell.in_row(chords_id, QStringLiteral("trackBusRail"));
  require(kick_rail != nullptr && chords_rail != nullptr &&
              shell.in_row(bass_id, QStringLiteral("trackBusRail")) == nullptr,
          "only members show the bus's rail");
  require(kick_rail->height() == shell.row(kick_id)->height() &&
              chords_rail->height() < shell.row(chords_id)->height(),
          "the rail does not run down to just short of the last member's bottom");
  require(text_of(shell.in_row(music_id, QStringLiteral("trackBusBadge"))) == "2 tracks",
          "the bus does not count its members");
  require(text_of(shell.in_row(space_id, QStringLiteral("trackSummary"))) ==
              "Return · fed by Chords",
          "the return does not name the tracks feeding it");
  auto* pill = shell.in_row(chords_id, QStringLiteral("trackSendPill"));
  require(pill != nullptr && text_of(pill) == "↗ Space" &&
              shell.in_row(pad_id, QStringLiteral("trackSendPill")) == nullptr,
          "only a track that sends shows a pill for its return");

  // Folding the bus hides its members and shows their clips on its lane.
  const auto space_top = shell.top(space_id);
  shell.click(shell.in_row(music_id, QStringLiteral("trackBusChevron")));
  require(bridge.collapsed_buses() == QStringList{music_id} &&
              QSettings{}.value(settings_key).toStringList() == QStringList{music_id},
          "folding the bus is not kept with the project's view");
  require(!shell.row(kick_id)->isVisible() && !shell.row(chords_id)->isVisible() &&
              !shell.row(kick_id, QStringLiteral("arrangementRow"))->isVisible() &&
              shell.top(space_id) < space_top,
          "a folded bus still shows its members");
  auto* sum = find_visible(shell.row(music_id, QStringLiteral("arrangementRow")),
                           QStringLiteral("arrangementBusSum"));
  require(sum != nullptr && !sum->property("segments").toList().isEmpty(),
          "a folded bus's lane does not outline its members' clips");
  shell.click(shell.in_row(music_id, QStringLiteral("trackBusChevron")));
  require(bridge.collapsed_buses().isEmpty() && shell.row(kick_id)->isVisible() &&
              find_visible(shell.row(music_id, QStringLiteral("arrangementRow")),
                           QStringLiteral("arrangementBusSum")) == nullptr,
          "unfolding the bus does not show its members again");

  // Into the bracket: a slot opens below the Kick, the rail lights and the
  // chip names the bus; the drop routes the Pad there in one undo step.
  auto revision = bridge.revision();
  shell.drag_onto(shell.at(pad_id, middle), kick_id, lower_part);
  require(shell.item(QStringLiteral("trackDropSlot")) != nullptr &&
              shell.in_row(kick_id, QStringLiteral("trackBusRailGlow")) != nullptr,
          "a drag into the bracket does not open a slot and light the rail");
  auto* chip = shell.item(QStringLiteral("trackDragChipDestination"));
  require(chip != nullptr && text_of(chip) == "Music bus", "the drag chip does not name the bus");
  shell.release();
  require(shell.destination(pad_id) == music_id && bridge.revision() == revision + 1,
          "a drop into the bracket does not route the track to the bus in one step");
  require(shell.top(kick_id) < shell.top(pad_id) && shell.top(pad_id) < shell.top(chords_id) &&
              shell.item(QStringLiteral("trackDropSlot")) == nullptr,
          "the dropped track does not sit where the slot was");
  bridge.undo();
  require(shell.destination(pad_id).isEmpty() && shell.top(pad_id) < shell.top(music_id),
          "one undo does not restore the routing and the order");

  // Out of every bracket: the Chords go back to the master.
  revision = bridge.revision();
  shell.drag_onto(shell.at(chords_id, middle), bass_id, upper_part);
  chip = shell.item(QStringLiteral("trackDragChipDestination"));
  require(chip != nullptr && text_of(chip) == "Master", "the drag chip does not name the master");
  shell.release();
  require(shell.destination(chords_id).isEmpty() && bridge.revision() == revision + 1 &&
              shell.top(chords_id) < shell.top(bass_id),
          "a drop outside every bracket does not route the track to the master");
  bridge.undo();
  require(shell.destination(chords_id) == music_id, "one undo does not put the track back");

  // A selection moves together: both onto the bus header, as its last members.
  shell.click(shell.row(bass_id));
  shell.click(shell.row(pad_id), Qt::ControlModifier);
  revision = bridge.revision();
  shell.drag_onto(shell.at(pad_id, middle), music_id, lower_part);
  shell.release();
  require(shell.destination(bass_id) == music_id && shell.destination(pad_id) == music_id &&
              bridge.revision() == revision + 1 && shell.top(chords_id) < shell.top(bass_id),
          "a selection dropped on the bus header does not join it as its last members");
  bridge.undo();
  require(shell.destination(bass_id).isEmpty() && shell.destination(pad_id).isEmpty(),
          "one undo does not take the whole selection back out");

  // A bus never goes inside a bus: the chip says so and the drop is refused.
  revision = bridge.revision();
  shell.drag_onto(shell.at(music_id, middle), kick_id, upper_part);
  chip = shell.item(QStringLiteral("trackDragChipDestination"));
  require(chip != nullptr && text_of(chip) == "Buses stay on the master",
          "dragging a bus into a bracket does not warn");
  shell.release();
  require(bridge.revision() == revision && shell.destination(music_id).isEmpty() &&
              bridge.status().contains(QStringLiteral("can't go inside")),
          "a bus dropped into a bracket is not refused with a reason");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OscillateTests"));
  QCoreApplication::setApplicationName(QStringLiteral("BusNesting"));
  try {
    buses_nest_fold_and_take_drops();
  } catch (const std::exception& error) {
    std::cerr << "Bus nesting test: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
