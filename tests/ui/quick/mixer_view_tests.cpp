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
// the master; a dB scale beside every fader; mute and solo; a fader drag is
// one undo step; send knobs per return create, set and clear sends; the
// output menu reroutes to a group.
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

// A scale mark and the level it stands for, as the mixer shows them.
struct ScaleMark {
  QString label;
  double db;
};
// The marks every strip's scale shows at least (the issue's minimum), and
// the floor, which reads −∞.
constexpr double unity_db = 0.0;
constexpr double floor_db = -60.0;
constexpr double ceiling_db = 12.0;
const std::vector<ScaleMark>& required_marks() {
  static const std::vector<ScaleMark> marks{{QStringLiteral("0"), unity_db},
                                            {QStringLiteral("−6"), -6.0},
                                            {QStringLiteral("−12"), -12.0},
                                            {QStringLiteral("−24"), -24.0},
                                            {QStringLiteral("−∞"), floor_db}};
  return marks;
}
// A mark and the fader cap or meter reading at its level line up within
// this many pixels.
constexpr double scale_alignment_px = 1.0;

double scene_y(QQuickItem* item, double y) { return item->mapToScene(QPointF(0.0, y)).y(); }

// Every strip, the master's included, draws a dB scale beside its fader:
// each required mark, the unity mark brighter than the rest, and each mark
// level with the fader cap's centre and the meters' reading at that level.
void every_fader_has_a_db_scale(QQuickItem* view, const QStringList& strip_ids) {
  for (const auto& id : strip_ids) {
    auto* scale = find_named(view, QStringLiteral("mixerScale:") + id);
    auto* fader = find_named(view, QStringLiteral("mixerFader:") + id);
    auto* meter = find_named(view, QStringLiteral("mixerMeterL:") + id);
    require(scale != nullptr && scale->isVisible(),
            "strip " + id.toStdString() + " has a dB scale");
    require(fader != nullptr && meter != nullptr, "strip " + id.toStdString() + " has its fader");
    auto* column = find_named(meter, QStringLiteral("mixerMeterColumn"));
    require(column != nullptr, "the meter has its column");
    const double span = ceiling_db - floor_db;
    for (const auto& mark : required_marks()) {
      auto* item = find_named(scale, QStringLiteral("mixerScaleMark:") + mark.label);
      require(item != nullptr && item->isVisible(),
              "strip " + id.toStdString() + "'s scale marks " + mark.label.toStdString());
      const double mark_y = scene_y(item, item->height() / 2.0);
      const double position = (mark.db - floor_db) / span;
      QVariant cap_y;
      QMetaObject::invokeMethod(fader, "centreOf", Q_RETURN_ARG(QVariant, cap_y),
                                Q_ARG(QVariant, position));
      require(std::abs(scene_y(fader, cap_y.toDouble()) - mark_y) <= scale_alignment_px,
              "the " + mark.label.toStdString() + " mark is level with the fader cap there");
      const double reading_y = scene_y(column, column->height() * (1.0 - position));
      require(std::abs(reading_y - mark_y) <= scale_alignment_px,
              "the " + mark.label.toStdString() + " mark is level with the meter reading there");
    }
    auto* unity = find_named(scale, QStringLiteral("mixerScaleMark:0"));
    auto* minus_six = find_named(scale, QStringLiteral("mixerScaleMark:−6"));
    require(unity->childItems().front()->width() > minus_six->childItems().front()->width(),
            "the unity mark stands out from the other marks");
  }
}

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
  QStringList strip_ids;
  for (const auto& name : order)
    strip_ids.push_back(name.mid(QStringLiteral("mixerStrip:").size()));
  every_fader_has_a_db_scale(view, strip_ids);

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
