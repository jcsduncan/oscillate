#include "project_bridge.hpp"
#include "screenshot_demo.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QList>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

// The shell over the documentation's "Harbour Lights" project, at the size
// its screenshots use: every ruler label, chord name, toolbar hint, search
// hint and strip knob label shows whole, with nothing drawn over it.
namespace {
namespace application = oscillate::application;
namespace quick = oscillate::quick;

using oscillate::test::require;

// The documentation's shell size, with every one of the song's 32 bars shown.
constexpr int window_width = 1760;
constexpr int window_height = 1000;
constexpr double visible_bars = 32.0;
// Demo tracks, top down: the Chords track carries Ember, Shape and Press;
// the Arp track's first clip opens in the piano roll.
constexpr int chords_track = 3;
constexpr int arp_track = 4;
constexpr int chain_tab = 0;
constexpr int notes_tab = 2;
// Halfway into bar 99, past the song's end: a three-digit position readout.
constexpr double far_bar = 98.5;
// Rounding in scene coordinates.
constexpr double layout_slack_px = 0.5;
constexpr int settle_timeout_ms = 2'000;

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name && parent->isVisible())
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

void find_items(QQuickItem* parent, const QString& name, QList<QQuickItem*>& found) {
  if (!parent->isVisible())
    return;
  if (parent->objectName() == name)
    found.push_back(parent);
  for (auto* child : parent->childItems())
    find_items(child, name, found);
}

void find_prefixed(QQuickItem* parent, const QString& prefix, QList<QQuickItem*>& found) {
  if (!parent->isVisible())
    return;
  if (parent->objectName().startsWith(prefix))
    found.push_back(parent);
  for (auto* child : parent->childItems())
    find_prefixed(child, prefix, found);
}

QRectF scene_rect(const QQuickItem* item, double width, double height) {
  return {item->mapToScene({0.0, 0.0}), QSizeF(width, height)};
}

QRectF scene_rect(const QQuickItem* item) {
  return scene_rect(item, item->width(), item->height());
}

bool overlaps(const QRectF& first, const QRectF& second) {
  return first.adjusted(layout_slack_px, layout_slack_px, -layout_slack_px, -layout_slack_px)
      .intersects(second);
}

std::string text_of(const QQuickItem* item) {
  return item->property("text").toString().toStdString();
}

void require_whole(const QQuickItem* text, const std::string& what) {
  require(!text->property("truncated").toBool(), what + " is cut short: \"" + text_of(text) + "\"");
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
    window_->show();
    require(QTest::qWaitForWindowExposed(window_), "DAW shell window never showed");
    QCoreApplication::processEvents();
  }

  [[nodiscard]] QQuickItem* root() const { return window_->contentItem(); }
  [[nodiscard]] QQuickItem* wait_for(const QString& name) const {
    QQuickItem* found = nullptr;
    const bool shown = QTest::qWaitFor(
        [&] {
          QCoreApplication::processEvents();
          found = find_item(root(), name);
          return found != nullptr;
        },
        settle_timeout_ms);
    require(shown && found != nullptr, "the shell shows " + name.toStdString());
    return found;
  }
  void select_track(int track) const {
    window_->setProperty("selectedTrack", track);
    QCoreApplication::processEvents();
  }
  void select_clip(const QString& id) const {
    window_->setProperty("selectedClipId", id);
    QCoreApplication::processEvents();
  }
  void show_view(const QString& view) const {
    window_->setProperty("currentView", view);
    QCoreApplication::processEvents();
  }
  void open_dock_tab(int tab) const {
    auto* dock = window_->findChild<QObject*>(QStringLiteral("deviceChainDock"));
    require(dock != nullptr, "the shell has its device dock");
    dock->setProperty("activeTab", tab);
    QCoreApplication::processEvents();
  }

private:
  QQmlApplicationEngine qml_;
  QQuickWindow* window_ = nullptr;
};

// No bar number sits under a marker flag or the ruler's tools.
void ruler_labels_clear_flags_and_tools(const Shell& shell) {
  auto* tools = shell.wait_for(QStringLiteral("arrangementTools"));
  QList<QQuickItem*> markers;
  find_items(shell.root(), QStringLiteral("arrangementMarker"), markers);
  require(!markers.isEmpty(), "the demo's markers show on the ruler");
  QList<QQuickItem*> labels;
  find_items(shell.root(), QStringLiteral("arrangementRulerLabel"), labels);
  require(!labels.isEmpty(), "the ruler shows bar numbers");
  for (const auto* label : labels) {
    const auto area = scene_rect(label, label->property("implicitWidth").toDouble(),
                                 label->property("implicitHeight").toDouble());
    require(!overlaps(area, scene_rect(tools)),
            "ruler label " + text_of(label) + " shows under the arrangement tools");
    for (const auto* marker : markers) {
      require(!overlaps(area, scene_rect(marker)),
              "ruler label " + text_of(label) + " shows under a marker flag");
    }
  }
}

// A chord clip names its chords along its foot, below every note.
void chord_names_clear_notes(const Shell& shell) {
  const auto* first_chord = shell.wait_for(QStringLiteral("arrangementChordName"));
  QList<QQuickItem*> chords;
  find_items(shell.root(), QStringLiteral("arrangementChordName"), chords);
  require(chords.contains(first_chord), "the chord names are found where they show");
  for (const auto* chord : chords) {
    const auto top = chord->mapToScene({0.0, 0.0}).y();
    QList<QQuickItem*> notes;
    find_items(chord->parentItem(), QStringLiteral("arrangementClipNote"), notes);
    require(!notes.isEmpty(), "the chord clip draws its notes");
    for (const auto* note : notes) {
      require(note->mapToScene({0.0, note->height()}).y() <= top + layout_slack_px,
              "a note draws over the chord name " +
                  chord->childItems().back()->property("text").toString().toStdString());
    }
  }
}

// The Chords chain's strip knobs show their labels whole.
void strip_labels_whole(const Shell& shell) {
  auto* card = shell.wait_for(QStringLiteral("insertEffectCard"));
  QList<QQuickItem*> labels;
  find_prefixed(shell.root(), QStringLiteral("stripKnobLabel:"), labels);
  require(card->isVisible(), "the chain shows its effect strips");
  require(!labels.isEmpty(), "the chain shows strip knobs");
  for (const auto* label : labels)
    require_whole(label, label->objectName().toStdString());
}

// The piano roll's hint follows its last header button, whole.
void piano_roll_hint_after_buttons(const Shell& shell) {
  auto* chords = shell.wait_for(QStringLiteral("pianoChordsButton"));
  auto* hint = shell.wait_for(QStringLiteral("pianoRollHint"));
  require(hint->mapToScene({0.0, 0.0}).x() + layout_slack_px >=
              chords->mapToScene({chords->width(), 0.0}).x(),
          "the piano roll's hint shows under its Chords button: \"" + text_of(hint) + "\"");
  require_whole(hint, "the piano roll's hint");
}

// Seeks to a zero-based bar and waits for the position readout to show the
// bar number; false if it never does.
bool seek_and_show(quick::ProjectBridge& bridge, QQuickItem* value, double bar,
                   const QString& shown) {
  bridge.seek_to_bar(bar);
  return QTest::qWaitFor(
      [&] {
        QCoreApplication::processEvents();
        return value->property("text").toString().startsWith(shown);
      },
      settle_timeout_ms);
}

// The toolbar's position readout holds a three-digit bar whole, and the key
// readout beside it stays put as the bar number grows.
void position_readout_holds_three_digits(const Shell& shell, quick::ProjectBridge& bridge) {
  auto* value = shell.wait_for(QStringLiteral("positionValue"));
  auto* key = shell.wait_for(QStringLiteral("keyReadout"));
  require(seek_and_show(bridge, value, 0.0, QStringLiteral("1 ")),
          "the position readout follows a seek to bar 1: \"" + text_of(value) + "\"");
  const auto key_x = key->mapToScene({0.0, 0.0}).x();
  require(seek_and_show(bridge, value, far_bar, QStringLiteral("99 ")),
          "the position readout follows a seek to bar 99: \"" + text_of(value) + "\"");
  const auto far_key_x = key->mapToScene({0.0, 0.0}).x();
  require(std::abs(far_key_x - key_x) <= layout_slack_px,
          "the key readout shifts when the position changes");
  auto* readout = shell.wait_for(QStringLiteral("positionReadout"));
  require(value->mapToScene({value->property("implicitWidth").toDouble(), 0.0}).x() <=
              readout->mapToScene({readout->width(), 0.0}).x() + layout_slack_px,
          "the position readout clips \"" + text_of(value) + "\"");
}

// The mixer's master section fits its loudness readings across its width.
void master_readings_fit(const Shell& shell) {
  shell.show_view(QStringLiteral("mixer"));
  auto* readings = shell.wait_for(QStringLiteral("masterReadings"));
  auto* column = readings->parentItem();
  require(readings->mapToScene({readings->property("implicitWidth").toDouble(), 0.0}).x() <=
              column->mapToScene({column->width(), 0.0}).x() + layout_slack_px,
          "the master section's readings run past its edge");
}

void shell_text_shows_whole() {
  QTemporaryDir directory;
  require(directory.isValid(), "fixture directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "Harbour Lights";
  quick::screenshot::create_demo_project(root);
  auto session = application::ProjectSession::open(root);
  require(session.has_value(), "the demo project did not open");
  quick::ProjectBridge bridge;
  bridge.set_project(std::move(session.value()), root);

  Shell shell(bridge);
  require_whole(shell.wait_for(QStringLiteral("dockedBrowserSearchHint")),
                "the browser's search hint");
  shell.select_track(chords_track);
  shell.open_dock_tab(chain_tab);
  ruler_labels_clear_flags_and_tools(shell);
  chord_names_clear_notes(shell);
  strip_labels_whole(shell);

  shell.select_track(arp_track);
  const auto clips = bridge.tracks().at(arp_track).toMap().value(QStringLiteral("clips")).toList();
  require(!clips.isEmpty(), "the Arp track has clips");
  shell.select_clip(clips.front().toMap().value(QStringLiteral("id")).toString());
  shell.open_dock_tab(notes_tab);
  piano_roll_hint_after_buttons(shell);
  position_readout_holds_three_digits(shell, bridge);
  master_readings_fit(shell);
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    shell_text_shows_whole();
  } catch (const std::exception& error) {
    std::cerr << "Shell text fit test: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
