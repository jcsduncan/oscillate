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

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// A sidechain is chosen on the effect it keys: the effect panel's strip picks
// the source track, which becomes that track's sidechain send to the exact
// effect slot, in one undo step.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

QQuickItem* find_visible(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name && parent->isVisible())
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_visible(child, name))
      return found;
  }
  return nullptr;
}

void click(QQuickWindow& window, QQuickItem* item) {
  require(item != nullptr, "sidechain control is missing");
  const auto centre = item->mapToScene({item->width() / 2, item->height() / 2}).toPoint();
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre);
  QCoreApplication::processEvents();
}

QQuickItem* choice_named(QQuickItem* strip, const QString& name) {
  QList<QQuickItem*> pending{strip};
  while (!pending.isEmpty()) {
    auto* item = pending.takeFirst();
    if (item->objectName() == QStringLiteral("sidechainSourceChoice") && item->isVisible() &&
        item->property("modelData").toMap().value("name").toString() == name)
      return item;
    pending.append(item->childItems());
  }
  return nullptr;
}

QVariantList sends_of(quick::ProjectBridge& bridge, int row) {
  return bridge.tracks()[row].toMap().value("sends").toList();
}

void effect_panel_chooses_its_key() {
  QTemporaryDir directory;
  require(directory.isValid(), "sidechain fixture directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "song";
  auto opened = application::ProjectSession::create(root);
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened).value();
  const auto kick = session->tracks().add_bus_track(project::TrackKind::group_bus);
  const auto snare = session->tracks().add_bus_track(project::TrackKind::group_bus);
  const auto bass = session->tracks().add_bus_track(project::TrackKind::group_bus);
  require(kick && snare && bass, "track creation failed");
  const auto press =
      session->effects().add_effect_engine(project::SignalPathId{bass.value()}, 0, "press");
  require(press.has_value(), "Press insertion failed");
  const application::Vst3InsertIdentity identity{std::string(32, 'a'), "Fixture Vendor",
                                                 "Aux Effect",         "1.0",
                                                 "/opt/aux.vst3",      std::string(64, 'b')};
  require(session->effects().insert_vst3_plugin(snare.value(), 0, identity).has_value(),
          "VST3 effect insertion failed");
  const auto vst3_slot = session->snapshot().tracks[1].effects.front().id;
  const auto bass_id = QString::fromStdString(bass.value().to_string());
  const auto press_id = QString::fromStdString(press.value().to_string());
  const auto kick_id = QString::fromStdString(kick.value().to_string());
  const auto snare_id = QString::fromStdString(snare.value().to_string());

  {
    quick::ProjectBridge bridge(directory.path().toStdString());
    bridge.set_project(std::move(session), root);
    QQmlApplicationEngine qml;
    qml.rootContext()->setContextProperty("projectBridge", &bridge);
    qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!qml.rootObjects().isEmpty(), "DAW shell failed to load");
    auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
    require(window != nullptr, "DAW shell is not a Quick window");
    window->resize(1600, 1000);
    QCoreApplication::processEvents();

    // A VST3 effect's aux input offers the same choice.
    const auto vst3 =
        bridge.sidechain_state(snare_id, QString::fromStdString(vst3_slot.to_string()));
    require(vst3.value("available").toBool() && !vst3.value("keyed").toBool(),
            "a VST3 effect does not offer a sidechain key");
    require(!bridge.sidechain_state(kick_id, press_id).value("available").toBool(),
            "a slot on another track offers a key");

    bridge.open_native_effect_editor(bass_id, press_id);
    QCoreApplication::processEvents();
    auto* overlay =
        find_visible(window->contentItem(), QStringLiteral("nativeEffectEditorOverlay"));
    require(overlay != nullptr && !overlay->property("docked").toBool(),
            "the effect panel does not open floating");
    auto* strip = find_visible(overlay, QStringLiteral("nativeEffectSidechainStrip"));
    require(strip != nullptr && !strip->property("keyed").toBool(),
            "Press's panel has no sidechain strip");
    const auto state = strip->property("state").toMap();
    const auto sources = state.value("sources").toList();
    require(sources.size() == 2, "the strip does not offer the other two tracks");
    const auto kick_name = sources.front().toMap().value("name").toString();
    const auto snare_name = sources.back().toMap().value("name").toString();

    click(*window, find_visible(strip, QStringLiteral("sidechainSourceButton")));
    require(strip->property("menuOpen").toBool(), "the source menu did not open");
    const auto before = bridge.revision();
    click(*window, choice_named(strip, kick_name));
    auto kick_sends = sends_of(bridge, 0);
    require(bridge.revision() == before + 1 && kick_sends.size() == 1 &&
                kick_sends.front().toMap().value("purpose").toString() ==
                    QStringLiteral("sidechain") &&
                kick_sends.front().toMap().value("destinationId").toString() == bass_id &&
                kick_sends.front().toMap().value("targetSlotId").toString() == press_id,
            "choosing a source did not save the kick's sidechain send to Press");
    require(strip->property("keyed").toBool() &&
                bridge.native_effect_context().value("keyed").toBool() &&
                bridge.native_effect_context().value("keyName").toString() == kick_name,
            "the panel does not follow the new key");

    // Another source replaces the key in one undoable step.
    click(*window, find_visible(strip, QStringLiteral("sidechainSourceButton")));
    click(*window, choice_named(strip, snare_name));
    require(sends_of(bridge, 0).isEmpty() && sends_of(bridge, 1).size() == 1 &&
                bridge.revision() == before + 2,
            "a new source did not replace the old key");
    bridge.undo();
    require(sends_of(bridge, 0).size() == 1 && sends_of(bridge, 1).isEmpty(),
            "undo did not restore the previous key in one step");

    // The gain changes in place; None removes the key.
    QMetaObject::invokeMethod(bridge.route_commands(), "set_sidechain_source",
                              Q_ARG(QString, bass_id), Q_ARG(QString, press_id),
                              Q_ARG(QString, kick_id), Q_ARG(double, 0.5));
    kick_sends = sends_of(bridge, 0);
    require(kick_sends.size() == 1 && kick_sends.front().toMap().value("gain").toDouble() == 0.5,
            "the key gain did not change in place");
    click(*window, find_visible(strip, QStringLiteral("sidechainSourceButton")));
    click(*window, choice_named(strip, QStringLiteral("None (self)")));
    require(sends_of(bridge, 0).isEmpty() && !strip->property("keyed").toBool() &&
                !bridge.native_effect_context().value("keyed").toBool(),
            "None did not remove the key");
    bridge.undo();
    bridge.save();
  }
  auto reopened = application::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().tracks.front().sends.size() == 1 &&
              reopened.value()->snapshot().tracks.front().sends.front().target_insert ==
                  press.value(),
          "the key did not survive save and reopen");
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    effect_panel_chooses_its_key();
  } catch (const std::exception& error) {
    qCritical("FAILED: %s", error.what());
    return 1;
  }
  return 0;
}
