#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

// Keyboard access in the shell (issue #112): every custom control the issue
// names takes Tab focus, shows the accent focus ring while focused, and the
// keyboard operates it. Return or Space toggles or opens; a menu, picker or
// popover opens with focus inside, the arrows move, Return chooses and
// Escape closes with focus back on the control that opened it; the shortcuts
// list scrolls with Up, Down, Page Up, Page Down, Home and End.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

constexpr int window_width = 1600;
constexpr int window_height = 1000;
// Short enough that the shortcuts list overflows its card and must scroll.
constexpr int short_window_height = 560;
// The sidechain menu's rows: None (self) first, then the source tracks.
constexpr int first_source_row = 1;
// The parameter chooser's first row is "+ Show another lane below"; two
// steps down (over the first group heading) reach a parameter.
constexpr int chooser_steps_to_parameter = 2;
const QString kick_name = QStringLiteral("Kick");

struct Shell {
  quick::ProjectBridge& bridge;
  QQuickWindow& window;
  QColor accent;
};

QQuickItem* find_visible(QQuickItem* parent, const QString& name) {
  QList<QQuickItem*> pending{parent};
  while (!pending.isEmpty()) {
    auto* item = pending.takeFirst();
    if (item->objectName() == name && item->isVisible())
      return item;
    pending.append(item->childItems());
  }
  return nullptr;
}

QQuickItem* require_visible(QQuickItem* parent, const QString& name) {
  auto* item = find_visible(parent, name);
  require(item != nullptr, name.toStdString() + " is not shown");
  return item;
}

QQuickItem* focused(const Shell& shell) { return shell.window.activeFocusItem(); }

QString focused_name(const Shell& shell) {
  auto* item = focused(shell);
  return item != nullptr ? item->objectName() : QString();
}

void press(const Shell& shell, Qt::Key key) {
  QTest::keyClick(&shell.window, key);
  QCoreApplication::processEvents();
}

QObject* bridge_object(const Shell& shell, const char* name) {
  auto* object = shell.bridge.property(name).value<QObject*>();
  require(object != nullptr, std::string("the bridge has no ") + name);
  return object;
}

// A focus ring is shown: a visible FocusRing child (or a sibling FocusRing
// targeting the control), or the control's own edge turned to the ring colour.
bool shows_focus_ring(QQuickItem* item, const QColor& ring) {
  for (auto* child : item->childItems()) {
    if (child->objectName() == QStringLiteral("focusRing") && child->isVisible())
      return true;
  }
  if (auto* parent = item->parentItem()) {
    for (auto* sibling : parent->childItems()) {
      if (sibling->objectName() == QStringLiteral("focusRing") && sibling->isVisible() &&
          sibling->property("target").value<QQuickItem*>() == item)
        return true;
    }
  }
  auto* border = item->property("border").value<QObject*>();
  return border != nullptr && border->property("color").value<QColor>() == ring;
}

// Tab from the control before it in the focus chain lands on it, and it
// shows its focus ring there.
void tab_reaches(const Shell& shell, QQuickItem* item, const QColor& ring) {
  const auto name = item->objectName().toStdString();
  require(item->activeFocusOnTab(), name + " does not take Tab focus");
  auto* before = item->nextItemInFocusChain(false);
  require(before != nullptr && before != item, name + " has nothing before it to Tab from");
  before->forceActiveFocus(Qt::TabFocusReason);
  QCoreApplication::processEvents();
  press(shell, Qt::Key_Tab);
  require(item->hasActiveFocus(), "Tab does not reach " + name);
  require(shows_focus_ring(item, ring), name + " shows no focus ring while focused");
}

void toolbar_toggles(const Shell& shell) {
  auto* root = shell.window.contentItem();
  auto* recording = bridge_object(shell, "recording");
  for (const auto& [name, key] : {std::pair{QStringLiteral("metronomeToggle"), "click"},
                                  std::pair{QStringLiteral("countInToggle"), "countIn"}}) {
    auto* toggle = require_visible(root, name);
    tab_reaches(shell, toggle, shell.accent);
    const bool before = recording->property(key).toBool();
    press(shell, Qt::Key_Return);
    require(recording->property(key).toBool() != before,
            "Return does not toggle " + name.toStdString());
    press(shell, Qt::Key_Space);
    require(recording->property(key).toBool() == before,
            "Space does not toggle " + name.toStdString());
  }

  auto* loop = require_visible(root, QStringLiteral("loopControls"));
  auto* transport = loop->property("transport").value<QObject*>();
  require(transport != nullptr, "the Loop button has no transport");
  tab_reaches(shell, loop, shell.accent);
  const bool looping = transport->property("enabled").toBool();
  press(shell, Qt::Key_Return);
  require(transport->property("enabled").toBool() != looping, "Return does not toggle Loop");
  press(shell, Qt::Key_Space);
  require(transport->property("enabled").toBool() == looping, "Space does not toggle Loop");

  auto* keyboard = bridge_object(shell, "computerKeyboard");
  auto* keys = require_visible(root, QStringLiteral("computerKeyboardToggle"));
  tab_reaches(shell, keys, shell.accent);
  press(shell, Qt::Key_Return);
  require(keyboard->property("enabled").toBool(), "Return does not turn the computer keyboard on");
  auto* in_key = require_visible(root, QStringLiteral("computerKeyboardInKey"));
  tab_reaches(shell, in_key, shell.accent);
  const bool was_in_key = keyboard->property("inKey").toBool();
  press(shell, Qt::Key_Return);
  require(keyboard->property("inKey").toBool() != was_in_key, "Return does not toggle In key");
  press(shell, Qt::Key_Space);
  require(keyboard->property("inKey").toBool() == was_in_key, "Space does not toggle In key");
  keys->forceActiveFocus(Qt::TabFocusReason);
  press(shell, Qt::Key_Space);
  require(!keyboard->property("enabled").toBool(), "Space does not turn the computer keyboard off");
}

void toolbar_view_and_actions(const Shell& shell) {
  auto* root = shell.window.contentItem();
  auto* mixer = require_visible(root, QStringLiteral("viewMixerButton"));
  tab_reaches(shell, mixer, shell.accent);
  press(shell, Qt::Key_Return);
  require(shell.window.property("currentView").toString() == QStringLiteral("mixer"),
          "Return on Mixer does not switch the view");
  auto* arrange = require_visible(root, QStringLiteral("viewArrangeButton"));
  tab_reaches(shell, arrange, shell.accent);
  press(shell, Qt::Key_Space);
  require(shell.window.property("currentView").toString() == QStringLiteral("arrange"),
          "Space on Arrange does not switch the view");

  auto* mix_check = require_visible(root, QStringLiteral("mixCheckToolbarButton"));
  tab_reaches(shell, mix_check, shell.accent);
  press(shell, Qt::Key_Return);
  require(shell.window.property("currentView").toString() == QStringLiteral("mixcheck"),
          "Return on Mix Check does not show it");
  press(shell, Qt::Key_Space);
  require(shell.window.property("currentView").toString() == QStringLiteral("arrange"),
          "Space on Mix Check does not leave it");

  auto* export_button = require_visible(root, QStringLiteral("toolbarExportButton"));
  tab_reaches(shell, export_button, shell.accent);
  press(shell, Qt::Key_Return);
  require(shell.window.property("exportDialogOpen").toBool(), "Return on Export does not open it");
  shell.window.setProperty("exportDialogOpen", false);
  QCoreApplication::processEvents();

  auto* more = require_visible(root, QStringLiteral("toolbarMoreButton"));
  tab_reaches(shell, more, shell.accent);
  press(shell, Qt::Key_Return);
  require_visible(root, QStringLiteral("toolbarMoreMenu"));
  require(focused_name(shell) == QStringLiteral("editNavButton"),
          "the More menu does not open with its first entry focused");
  require(shows_focus_ring(focused(shell), shell.accent), "the focused menu entry shows no ring");
  press(shell, Qt::Key_Down);
  require(focused_name(shell) == QStringLiteral("fxNavButton"),
          "Down does not move in the More menu");
  press(shell, Qt::Key_Up);
  require(focused_name(shell) == QStringLiteral("editNavButton"),
          "Up does not move in the More menu");
  press(shell, Qt::Key_Escape);
  require(find_visible(root, QStringLiteral("toolbarMoreMenu")) == nullptr,
          "Escape does not close the More menu");
  require(more->hasActiveFocus(), "Escape does not return focus to More");
}

void toolbar_key_picker(const Shell& shell) {
  auto* root = shell.window.contentItem();
  auto* readout = require_visible(root, QStringLiteral("keyReadout"));
  tab_reaches(shell, readout, shell.accent);
  press(shell, Qt::Key_Space);
  require_visible(root, QStringLiteral("keyPicker"));
  const auto tonic_prefix = QStringLiteral("keyPickerTonic:");
  require(focused_name(shell).startsWith(tonic_prefix),
          "the key picker does not open with a tonic focused");
  require(shows_focus_ring(focused(shell), shell.accent), "the focused tonic shows no ring");
  press(shell, Qt::Key_Escape);
  require(find_visible(root, QStringLiteral("keyPicker")) == nullptr,
          "Escape does not close the key picker");
  require(readout->hasActiveFocus(), "Escape does not return focus to the key readout");

  press(shell, Qt::Key_Return);
  const auto first = focused_name(shell);
  press(shell, Qt::Key_Right);
  const auto right = focused_name(shell);
  require(right.startsWith(tonic_prefix) && right != first,
          "Right does not move to the next tonic");
  press(shell, Qt::Key_Down);
  const auto below = focused_name(shell);
  require(below.startsWith(tonic_prefix) && below != right, "Down does not move a row of tonics");
  const auto chosen = focused(shell)->property("label").toString();
  press(shell, Qt::Key_Return);
  require(shell.bridge.property("projectKey").toString() == chosen + QStringLiteral(" maj"),
          "Return does not choose the focused tonic");
  require(find_visible(root, QStringLiteral("keyPicker")) == nullptr,
          "choosing a key does not close the picker");
  require(readout->hasActiveFocus(), "choosing a key does not return focus to the readout");
}

void track_colour(const Shell& shell) {
  auto* root = shell.window.contentItem();
  auto* swatch = require_visible(root, QStringLiteral("trackColourSwatch"));
  tab_reaches(shell, swatch, shell.accent);
  press(shell, Qt::Key_Return);
  auto* popover = require_visible(root, QStringLiteral("trackColourPopover"));
  require(popover->hasActiveFocus(), "the colour palette does not open with focus");
  press(shell, Qt::Key_Escape);
  require(!popover->isVisible(), "Escape does not close the colour palette");
  require(swatch->hasActiveFocus(), "Escape does not return focus to the swatch");

  press(shell, Qt::Key_Space);
  require(popover->isVisible() && popover->hasActiveFocus(), "Space does not open the palette");
  const int count = popover->property("colourCount").toInt();
  const int start = popover->property("current").toInt();
  // Move away from the edge the palette opened on, so each arrow moves.
  const bool from_end = start == count - 1;
  press(shell, from_end ? Qt::Key_Left : Qt::Key_Right);
  const int moved = popover->property("current").toInt();
  require(moved == (from_end ? start - 1 : start + 1), "an arrow does not move round the palette");
  auto* keyed =
      require_visible(popover, QStringLiteral("trackPaletteSwatch:") + QString::number(moved));
  require(shows_focus_ring(keyed, shell.accent), "the keyboard's colour shows no ring");
  press(shell, Qt::Key_Return);
  const auto* theme = popover->parentItem()->property("theme").value<QObject*>();
  require(theme != nullptr, "the track list has no theme");
  const auto colours = theme->property("trackPalette").toList();
  const auto colour = shell.bridge.tracks().front().toMap().value("color").toString();
  require(colour.compare(colours.at(moved).toString(), Qt::CaseInsensitive) == 0,
          "Return does not set the track to the keyboard's colour");
  require(!popover->isVisible(), "choosing a colour does not close the palette");
  require(focused_name(shell) == QStringLiteral("trackColourSwatch"),
          "choosing a colour does not return focus to the swatch");
}

void track_automation_chooser(const Shell& shell) {
  auto* root = shell.window.contentItem();
  auto* button = require_visible(root, QStringLiteral("trackAutomationButton"));
  button->forceActiveFocus(Qt::TabFocusReason);
  press(shell, Qt::Key_Return);
  auto* opener = require_visible(root, QStringLiteral("trackAutomationChooser"));
  tab_reaches(shell, opener, shell.accent);
  press(shell, Qt::Key_Return);
  auto* chooser = require_visible(root, QStringLiteral("automationTargetChooser"));
  require(chooser->hasActiveFocus(), "the parameter list does not open with focus");
  press(shell, Qt::Key_Escape);
  require(!chooser->isVisible(), "Escape does not close the parameter list");
  require(opener->hasActiveFocus(), "Escape does not return focus to the chooser");

  press(shell, Qt::Key_Space);
  require(chooser->isVisible() && chooser->hasActiveFocus(),
          "Space does not open the parameter list");
  const int first = chooser->property("current").toInt();
  for (int step = 0; step < chooser_steps_to_parameter; ++step)
    press(shell, Qt::Key_Down);
  const int current = chooser->property("current").toInt();
  require(current > first, "Down does not move in the parameter list");
  const auto row = chooser->property("targets").toList().at(current).toMap();
  require(!row.value("heading").toBool() && !row.value("action").toBool(),
          "Down stops on a heading instead of a parameter");
  press(shell, Qt::Key_Return);
  require(!chooser->isVisible(), "Return does not choose a parameter");
  require(opener->hasActiveFocus(), "choosing a parameter does not return focus to the chooser");
  const auto shown = opener->parentItem()->property("automationShown").toMap();
  require(shown.value("overlay").toString() == row.value("key").toString(),
          "Return does not show the chosen parameter over the clips");
  button->forceActiveFocus(Qt::TabFocusReason);
  press(shell, Qt::Key_Return);
}

void shortcuts_scroll(const Shell& shell) {
  auto* root = shell.window.contentItem();
  shell.window.resize(window_width, short_window_height);
  shell.window.setProperty("shortcutsOpen", true);
  QCoreApplication::processEvents();
  auto* list = require_visible(root, QStringLiteral("shortcutsList"));
  require(list->hasActiveFocus(), "the shortcuts list does not take focus when opened");
  require(list->activeFocusOnTab(), "the shortcuts list does not take Tab focus");
  require(shows_focus_ring(list, shell.accent), "the shortcuts list shows no focus ring");
  const double reach = list->property("maxContentY").toDouble();
  require(reach > 0, "the shortcuts list does not overflow, so it cannot be scrolled");
  auto* overlay = require_visible(root, QStringLiteral("shortcutsOverlay"));
  const double step = overlay->property("scrollStep").toDouble();
  const auto top = [list] { return list->property("contentY").toDouble(); };

  press(shell, Qt::Key_Down);
  require(qFuzzyCompare(top(), std::min(step, reach)), "Down does not scroll the list a row");
  press(shell, Qt::Key_Up);
  require(qFuzzyIsNull(top()), "Up does not scroll the list back");
  press(shell, Qt::Key_PageDown);
  require(qFuzzyCompare(top(), std::min(list->height(), reach)),
          "Page Down does not scroll a page");
  press(shell, Qt::Key_PageUp);
  require(qFuzzyIsNull(top()), "Page Up does not scroll a page back");
  press(shell, Qt::Key_End);
  require(qFuzzyCompare(top(), reach), "End does not scroll to the bottom");
  press(shell, Qt::Key_Home);
  require(qFuzzyIsNull(top()), "Home does not scroll to the top");
  press(shell, Qt::Key_Escape);
  require(!shell.window.property("shortcutsOpen").toBool(), "Escape does not close the shortcuts");
  shell.window.resize(window_width, window_height);
  QCoreApplication::processEvents();
}

void sidechain_and_frame(const Shell& shell, const QString& bus_id, const QString& press_id) {
  shell.bridge.open_native_effect_editor(bus_id, press_id);
  QCoreApplication::processEvents();
  auto* root = shell.window.contentItem();
  auto* overlay = require_visible(root, QStringLiteral("nativeEffectEditorOverlay"));
  auto* strip = require_visible(overlay, QStringLiteral("nativeEffectSidechainStrip"));
  auto* button = require_visible(strip, QStringLiteral("sidechainSourceButton"));
  tab_reaches(shell, button, strip->property("accentColor").value<QColor>());
  press(shell, Qt::Key_Return);
  require(strip->property("menuOpen").toBool(), "Return does not open the sidechain menu");
  auto* menu = require_visible(strip, QStringLiteral("sidechainSourceMenu"));
  auto* inside = focused(shell);
  while (inside != nullptr && inside != menu)
    inside = inside->parentItem();
  require(inside == menu, "the sidechain menu does not open with focus inside");
  press(shell, Qt::Key_Escape);
  require(!strip->property("menuOpen").toBool(), "Escape does not close the sidechain menu");
  require(button->hasActiveFocus(), "Escape does not return focus to the source button");

  press(shell, Qt::Key_Space);
  require(strip->property("menuOpen").toBool(), "Space does not open the sidechain menu");
  const auto before = shell.bridge.revision();
  for (int row = 0; row < first_source_row; ++row)
    press(shell, Qt::Key_Down);
  press(shell, Qt::Key_Return);
  require(!strip->property("menuOpen").toBool(), "Return does not choose a sidechain source");
  require(strip->property("keyed").toBool() && shell.bridge.revision() == before + 1,
          "Return does not key the effect from the chosen source");
  require(button->hasActiveFocus(), "choosing a source does not return focus to the button");
  shell.bridge.undo();
  QCoreApplication::processEvents();

  // The editor overlay is a FloatingDeviceFrame: its title bar's CLOSE.
  auto* close = require_visible(overlay, QStringLiteral("floatingDeviceClose"));
  tab_reaches(shell, close, overlay->property("accentColor").value<QColor>());
  press(shell, Qt::Key_Return);
  require(find_visible(root, QStringLiteral("nativeEffectEditorOverlay")) == nullptr,
          "Return on CLOSE does not close the editor");
}

void track_duck(const Shell& shell) {
  auto* root = shell.window.contentItem();
  auto* hint = require_visible(root, QStringLiteral("trackDuckHint"));
  tab_reaches(shell, hint, shell.accent);
  press(shell, Qt::Key_Return);
  auto* popover = require_visible(root, QStringLiteral("trackDuckPopover"));
  require(popover->hasActiveFocus(), "the duck list does not open with focus");
  press(shell, Qt::Key_Escape);
  require(!popover->isVisible(), "Escape does not close the duck list");
  require(hint->hasActiveFocus(), "Escape does not return focus to Duck the rest");

  press(shell, Qt::Key_Space);
  require(popover->isVisible() && popover->hasActiveFocus(), "Space does not open the duck list");
  const int ticked = popover->property("count").toInt();
  require(ticked > 0, "the duck list does not start with the other tracks ticked");
  press(shell, Qt::Key_Space);
  require(popover->property("count").toInt() == ticked - 1,
          "Space does not untick the keyboard's row");
  press(shell, Qt::Key_Space);
  require(popover->property("count").toInt() == ticked, "Space does not tick the keyboard's row");
  press(shell, Qt::Key_Down);
  press(shell, Qt::Key_Up);
  require(popover->property("current").toInt() == 0, "Up and Down do not move in the duck list");
  const auto before = shell.bridge.revision();
  press(shell, Qt::Key_Return);
  require(!popover->isVisible(), "Return does not apply the duck list");
  require(shell.bridge.revision() > before, "Return does not duck the ticked tracks");
}

void keyboard_drives_the_shell() {
  QTemporaryDir directory;
  require(directory.isValid(), "fixture directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "song";
  auto opened = application::ProjectSession::create(root);
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened).value();
  const auto kick = session->instruments().add_synth_track();
  const auto bus = session->tracks().add_bus_track(project::TrackKind::group_bus);
  require(kick && bus, "fixture tracks");
  const auto effect =
      session->effects().add_effect_engine(project::SignalPathId{bus.value()}, 0, "press");
  require(effect.has_value(), "Press insertion failed");
  const auto kick_id = QString::fromStdString(kick.value().to_string());
  const auto bus_id = QString::fromStdString(bus.value().to_string());
  const auto press_id = QString::fromStdString(effect.value().to_string());

  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(session), root);
  // Named as a kick, the synth offers "Duck the rest".
  bridge.set_track_name(kick_id, kick_name);
  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "DAW shell failed to load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "DAW shell is not a Quick window");
  window->resize(window_width, window_height);
  window->show();
  window->requestActivate();
  require(QTest::qWaitForWindowActive(window), "the shell window did not become active");
  QCoreApplication::processEvents();

  const Shell shell{bridge, *window, window->property("accent").value<QColor>()};
  toolbar_toggles(shell);
  toolbar_view_and_actions(shell);
  toolbar_key_picker(shell);
  track_colour(shell);
  track_automation_chooser(shell);
  shortcuts_scroll(shell);
  sidechain_and_frame(shell, bus_id, press_id);
  track_duck(shell);
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    keyboard_drives_the_shell();
  } catch (const std::exception& error) {
    qCritical("FAILED: %s", error.what());
    return 1;
  }
  return 0;
}
