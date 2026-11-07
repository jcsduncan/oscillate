#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The mixer view (redesign): strips in arrangement order, then returns, then
// the master; mute and solo; a fader drag is one undo step; send knobs per
// return create, set and clear sends; the output menu reroutes to a group.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

QQuickItem* find_named(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name)
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_named(child, name))
      return found;
  }
  return nullptr;
}

QVariantMap track(quick::ProjectBridge& bridge, const QString& id) {
  for (const auto& item : bridge.tracks()) {
    if (item.toMap().value("id").toString() == id)
      return item.toMap();
  }
  throw std::runtime_error("track missing from the bridge");
}

QString text(const auto& id) { return QString::fromStdString(id.to_string()); }

void mixer_strips_edit_the_project() {
  QTemporaryDir directory;
  require(directory.isValid(), "fixture directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "song";
  auto session = std::move(application::ProjectSession::create(root)).value();
  const auto synth = session->instruments().add_synth_track();
  const auto group = session->tracks().add_bus_track(project::TrackKind::group_bus);
  const auto reverb = session->tracks().add_bus_track(project::TrackKind::return_bus);
  const auto delay = session->tracks().add_bus_track(project::TrackKind::return_bus);
  require(synth && group && reverb && delay, "fixture tracks");
  const auto send = session->routing().add_send(synth.value(), reverb.value(), 0.5);
  require(send.has_value(), "fixture send");

  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(session), root);
  QQmlEngine engine;
  QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/MixerView.qml")));
  std::unique_ptr<QObject> object(component.createWithInitialProperties(
      {{QStringLiteral("bridge"), QVariant::fromValue(static_cast<QObject*>(&bridge))}}));
  require(object != nullptr, ("mixer view did not load: " + component.errorString()).toStdString());
  QQuickWindow window;
  auto* view = qobject_cast<QQuickItem*>(object.get());
  require(view != nullptr, "mixer view is not an item");
  window.resize(1600, 900);
  view->setParentItem(window.contentItem());
  view->setSize(QSizeF(1600, 900));
  window.show();
  require(QTest::qWaitForWindowExposed(&window), "mixer window exposed");
  QCoreApplication::processEvents();

  // Strip order: tracks (with their groups) in arrangement order, returns, master.
  const QStringList expected{QStringLiteral("mixerStrip:") + text(synth.value()),
                             QStringLiteral("mixerStrip:") + text(group.value()),
                             QStringLiteral("mixerStrip:") + text(reverb.value()),
                             QStringLiteral("mixerStrip:") + text(delay.value()),
                             QStringLiteral("mixerStrip:master")};
  auto* strips = find_named(view, QStringLiteral("mixerStrips"));
  require(strips != nullptr, "strip area");
  QStringList order;
  std::vector<QQuickItem*> stack{strips};
  while (!stack.empty()) {
    auto* item = stack.back();
    stack.pop_back();
    const auto children = item->childItems();
    for (auto it = children.rbegin(); it != children.rend(); ++it)
      stack.push_back(*it);
    if (item->objectName().startsWith(QStringLiteral("mixerStrip:")))
      order.push_back(item->objectName());
  }
  require(order == expected, "strips are tracks, then returns, then the master");

  // Sends: one knob per return, lettered; the existing send shows its level.
  const auto synth_id = text(synth.value());
  auto* send_a = find_named(view, QStringLiteral("mixerSend:") + synth_id + QStringLiteral(":A"));
  auto* send_b = find_named(view, QStringLiteral("mixerSend:") + synth_id + QStringLiteral(":B"));
  require(send_a && send_b, "a send knob per return");
  require(send_a->property("valueText").toString().contains(QStringLiteral("6.0")),
          "send A shows the existing −6 dB send");
  require(send_b->property("valueText").toString() == QStringLiteral("off"), "send B is off");
  require(find_named(view, QStringLiteral("mixerSends:") + text(reverb.value())) == nullptr ||
              !find_named(view, QStringLiteral("mixerSends:") + text(reverb.value()))->isVisible(),
          "returns have no send section");

  // Turning B up creates its send; resetting A clears its gain.
  send_b->forceActiveFocus();
  QTest::keyClick(&window, Qt::Key_Up);
  QCoreApplication::processEvents();
  auto sends = track(bridge, synth_id).value("sends").toList();
  require(sends.size() == 2, "turning an off send up creates it");
  send_a->forceActiveFocus();
  QTest::keyClick(&window, Qt::Key_Delete);
  QCoreApplication::processEvents();
  for (const auto& item : track(bridge, synth_id).value("sends").toList()) {
    const auto map = item.toMap();
    if (map.value("destinationId").toString() == text(reverb.value()))
      require(map.value("gain").toDouble() == 0.0, "resetting a send turns it off");
  }

  // Mute and solo.
  auto* mute = find_named(view, QStringLiteral("mixerMute:") + synth_id);
  auto* solo = find_named(view, QStringLiteral("mixerSolo:") + synth_id);
  require(mute && solo, "M and S buttons");
  QMetaObject::invokeMethod(mute, "activate");
  QMetaObject::invokeMethod(solo, "activate");
  QCoreApplication::processEvents();
  require(track(bridge, synth_id).value("muted").toBool() &&
              track(bridge, synth_id).value("soloed").toBool(),
          "M and S edit the track");

  // A fader drag is heard live and saves one edit on release: one undo step.
  auto* fader = find_named(view, QStringLiteral("mixerFader:") + synth_id);
  require(fader != nullptr, "fader");
  const auto before = track(bridge, synth_id).value("level").toDouble();
  const auto revision = bridge.revision();
  QMetaObject::invokeMethod(fader, "begun");
  QMetaObject::invokeMethod(fader, "moved", Q_ARG(double, 0.7));
  QTest::qWait(60);
  QMetaObject::invokeMethod(fader, "moved", Q_ARG(double, 0.6));
  QCoreApplication::processEvents();
  require(bridge.revision() == revision, "a fader drag saved an edit before its release");
  QMetaObject::invokeMethod(fader, "committed", Q_ARG(double, 0.5));
  QCoreApplication::processEvents();
  require(std::abs(track(bridge, synth_id).value("level").toDouble() - 0.5) < 1e-9 &&
              bridge.revision() == revision + 1,
          "the fader's release sets the level in one edit");
  bridge.undo();
  QCoreApplication::processEvents();
  require(std::abs(track(bridge, synth_id).value("level").toDouble() - before) < 1e-9,
          "one undo reverts the whole drag");

  // Keyboard: Up raises half a decibel.
  fader->forceActiveFocus();
  QTest::keyClick(&window, Qt::Key_Up);
  QCoreApplication::processEvents();
  require(std::abs(20 * std::log10(track(bridge, synth_id).value("level").toDouble()) -
                   (20 * std::log10(before) + 0.5)) < 1e-6,
          "Up raises the fader 0.5 dB");

  // Output menu: route the synth into the group.
  auto* strip = find_named(view, QStringLiteral("mixerStrip:") + synth_id);
  strip->setProperty("routeMenuOpen", true);
  QCoreApplication::processEvents();
  QQuickItem* choice = nullptr;
  for (const auto& item : bridge.route_targets()) {
    const auto map = item.toMap();
    if (map.value("id").toString() == text(group.value()))
      choice =
          find_named(strip, QStringLiteral("mixerRouteChoice:") + map.value("name").toString());
  }
  require(choice != nullptr, "the group is an output choice");
  QTest::mouseClick(
      &window, Qt::LeftButton, Qt::NoModifier,
      choice->mapToScene(QPointF(choice->width() / 2, choice->height() / 2)).toPoint());
  QCoreApplication::processEvents();
  require(track(bridge, synth_id).value("mainDestinationId").toString() == text(group.value()),
          "the output menu routes the track into the group");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    mixer_strips_edit_the_project();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "mixer view contract passed\n";
  return 0;
}
