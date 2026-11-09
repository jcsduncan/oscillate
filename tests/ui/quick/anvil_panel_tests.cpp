#include "anvil_panel_model.hpp"
#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Anvil panel (ADR-0060): a control for every parameter; the preview
// model drawing the engine's own kick with nothing playing; a curve point
// drag as one document gesture; layers and curve tabs; Tune to key landing
// the tail on the song's key; the playhead from telemetry; and screenshots
// on request.
namespace {
using oscillate::quick::AnvilPanelModel;
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace anvil = oscillate::native_dsp::synth::anvil;
namespace project = oscillate::project;

using oscillate::test::require;

const synth::SynthEngine& engine() {
  const auto* found = synth::find_engine("anvil");
  require(found != nullptr && found->descriptor != nullptr, "Anvil is registered");
  return *found;
}
const oscillate::native_dsp::EngineDescriptor& table() { return *engine().descriptor; }

std::unique_ptr<InMemoryNativeSynthAdapter> adapter_for(std::size_t preset) {
  auto values = oscillate::native_dsp::preset_values(table(), table().presets[preset]).value();
  auto adapter = std::make_unique<InMemoryNativeSynthAdapter>(std::move(values));
  if (engine().preset_document != nullptr)
    adapter->submit_document(engine().preset_document(preset), NativeSynthGesture::single);
  return adapter;
}

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller, AnvilPanelModel* model,
                                  const QVariantList& scale = {}) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/anvil"
      AnvilPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/AnvilPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Anvil panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)},
       {QStringLiteral("anvil"), QVariant::fromValue<QObject*>(model)},
       {QStringLiteral("songScale"), scale}});
  require(object != nullptr,
          "Anvil panel was not created: " + component.errorString().toStdString());
  panel->root = qobject_cast<QQuickItem*>(object);
  require(panel->root != nullptr, "panel is a Quick item");
  panel->root->setWidth(panel->root->implicitWidth());
  panel->root->setHeight(panel->root->implicitHeight());
  panel->window->resize(static_cast<int>(panel->root->width()),
                        static_cast<int>(panel->root->height()));
  panel->root->setParentItem(panel->window->contentItem());
  panel->window->show();
  require(QTest::qWaitForWindowExposed(panel->window.get()), "panel window is exposed");
  QCoreApplication::processEvents();
  return panel;
}

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name) {
    return parent;
  }
  for (QQuickItem* child : parent->childItems()) {
    if (QQuickItem* found = find_item(child, name)) {
      return found;
    }
  }
  return nullptr;
}

QQuickItem* item(Panel& panel, const QString& name) {
  auto* found = find_item(panel.root, name);
  require(found != nullptr, "missing item " + name.toStdString());
  return found;
}

QPoint scene(QQuickItem* target, double x, double y) {
  return target->mapToScene(QPointF(x, y)).toPoint();
}
QPoint centre(QQuickItem* target) {
  return scene(target, target->width() / 2, target->height() / 2);
}

void click(Panel& panel, QPoint point) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, point);
}

void wheel(Panel& panel, QPoint point, int delta) {
  QWheelEvent event(point, panel.window->mapToGlobal(point), QPoint(), QPoint(0, delta),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(panel.window.get(), &event);
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// Where a curve point sits on the editor (AnvilCurve's plot: 44 px from the
// left, 12 from the right, 14 from the top, 22 from the bottom).
// A point on the curve editor's plot, from its own plot edges.
QPoint point_on_curve(QQuickItem* curve, double x, double y) {
  const double left = curve->property("plotLeft").toDouble();
  const double right = curve->property("plotRight").toDouble();
  const double top = curve->property("plotTop").toDouble();
  const double bottom = curve->property("plotBottom").toDouble();
  return scene(curve, left + x * (right - left), bottom - y * (bottom - top));
}

std::vector<project::NativeCurvePoint> curve_named(const project::NativeSynthDocument& doc,
                                                   std::string_view id) {
  for (const auto& curve : doc.curves) {
    if (curve.id == id) {
      return curve.points;
    }
  }
  return {};
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  auto adapter = adapter_for(0);
  NativeSynthDeviceController controller(table(), *adapter);
  AnvilPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  require(panel->root->implicitWidth() >= 1400 && panel->root->implicitHeight() >= 780,
          "panel has its designed size");
  for (const auto& parameter : table().parameters) {
    const auto id =
        QString::fromUtf8(parameter.id.data(), static_cast<qsizetype>(parameter.id.size()));
    bool found = false;
    for (const auto* kind : {"anvilKnob:", "anvilSegment:", "anvilDropdown:", "anvilToggle:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "a control for " + id.toStdString());
  }
  for (const auto* layer : {"body", "click", "sub", "rumble"}) {
    require(item(*panel, QStringLiteral("anvilLayer:") + QString::fromLatin1(layer))->isVisible(),
            "every layer is listed");
  }
}

void the_preview_draws_the_kick() {
  auto adapter = adapter_for(0);
  NativeSynthDeviceController controller(table(), *adapter);
  AnvilPanelModel model(controller);
  require(model.kick().size() == AnvilPanelModel::kick_columns &&
              model.beat().size() == AnvilPanelModel::beat_columns,
          "the preview has its columns");
  double loudest = 0.0;
  for (const auto& peak : model.kick())
    loudest = std::max(loudest, peak.toDouble());
  require(loudest > 0.1, "the preview kick is audible");
  require(model.tail_hz() > 20.0 && model.tail_hz() < 200.0, "the tail sits in the kick range");
  require(model.kick_fraction() > 0.0 && model.kick_fraction() <= 1.0, "the kick fits the beat");
  // A longer kick renders longer, from the controller's own values.
  const auto before = model.kick_seconds();
  controller.set_value(QStringLiteral("anvil.length"),
                       controller.value(QStringLiteral("anvil.length")) * 1.5);
  model.render();
  require(model.kick_seconds() > before * 1.4, "a value edit re-renders the preview");
  auto panel = load_panel(controller, &model);
  require(text_of(*panel, QStringLiteral("anvilTailNote")) != QStringLiteral("—"),
          "the header names the tail note");
}

void layers_tabs_and_knobs() {
  auto adapter = adapter_for(0);
  NativeSynthDeviceController controller(table(), *adapter);
  AnvilPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  click(*panel, centre(item(*panel, QStringLiteral("anvilLayer:rumble"))));
  require(panel->root->property("selectedLayer").toString() == QStringLiteral("rumble"),
          "a layer card shows its controls");
  auto* amount = item(*panel, QStringLiteral("anvilKnob:anvil.rumble.amount"));
  require(amount->isVisible(), "the rumble's knobs show");
  const double before = controller.value(QStringLiteral("anvil.rumble.amount"));
  wheel(*panel, centre(amount), -120);
  require(controller.value(QStringLiteral("anvil.rumble.amount")) < before,
          "scrolling a knob edits it");
  click(*panel, centre(item(*panel, QStringLiteral("anvilTab:amp"))));
  require(panel->root->property("curveId").toString() == QStringLiteral("amp"),
          "a tab picks the curve drawn");
  require(item(*panel, QStringLiteral("anvilCurve"))->property("curveId").toString() ==
              QStringLiteral("amp"),
          "the editor draws it");
}

void a_point_drag_is_one_gesture() {
  auto adapter = adapter_for(0);
  NativeSynthDeviceController controller(table(), *adapter);
  AnvilPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  click(*panel, centre(item(*panel, QStringLiteral("anvilTab:amp"))));
  auto* curve = item(*panel, QStringLiteral("anvilCurve"));
  const auto points = curve->property("points").toList();
  require(points.size() >= 3, "the amp curve has an inner point");
  const auto inner = points.at(1).toMap();
  const double x = inner.value("x").toDouble(), y = inner.value("y").toDouble();
  const auto submissions = adapter->document_submissions().size();
  const auto from = point_on_curve(curve, x, y);
  const auto to = point_on_curve(curve, x, std::max(0.1, y - 0.3));
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from);
  for (int step = 1; step <= 4; ++step) {
    QTest::mouseMove(panel->window.get(),
                     QPoint(from.x(), from.y() + (to.y() - from.y()) * step / 4));
  }
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, to);
  QCoreApplication::processEvents();
  const auto& sent = adapter->document_submissions();
  require(sent.size() > submissions + 1, "the drag edits the document");
  require(sent[submissions].gesture == NativeSynthGesture::begin &&
              sent.back().gesture == NativeSynthGesture::commit,
          "the drag is one gesture");
  const auto moved = curve_named(adapter->document(), "amp");
  require(std::cmp_equal(moved.size(), points.size()) && moved[1].y < y - 0.15,
          "the point moved down");
  require(std::abs(moved.front().y - points.front().toMap().value("y").toDouble()) < 1e-9,
          "the first point stays");
}

void tune_to_key_lands_in_the_song_key() {
  auto adapter = adapter_for(0);
  NativeSynthDeviceController controller(table(), *adapter);
  AnvilPanelModel model(controller);
  // F minor: F G A♭ B♭ C D♭ E♭.
  const QVariantList f_minor{5, 7, 8, 10, 0, 1, 3};
  auto panel = load_panel(controller, &model, f_minor);
  for (int attempt = 0; attempt < 2; ++attempt) {
    click(*panel, centre(item(*panel, QStringLiteral("anvilTuneToKey"))));
    model.render();
    QCoreApplication::processEvents();
  }
  const double m = 69.0 + 12.0 * std::log2(model.tail_hz() / 440.0);
  const int nearest = static_cast<int>(std::lround(m));
  const int pitch_class = ((nearest % 12) + 12) % 12;
  require(f_minor.contains(pitch_class), "the tail is a note of F minor");
  require(std::abs(m - nearest) * 100.0 < 6.0, "the tail is within 6 cents of it");
  require(text_of(*panel, QStringLiteral("anvilTailKey")).contains(QStringLiteral("in key")),
          "the header says so");
}

void telemetry_moves_the_playhead() {
  auto adapter = adapter_for(0);
  NativeSynthDeviceController controller(table(), *adapter);
  AnvilPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  auto* playhead = item(*panel, QStringLiteral("anvilPlayhead"));
  require(!playhead->isVisible(), "no playhead while idle");
  std::vector<float> frame(anvil::telemetry::size, 0.0F);
  frame[0] = 0.5F;
  frame[1] = 36.0F;
  frame[2] = 110.0F;
  frame[3] = -3.0F;
  adapter->set_telemetry(frame);
  QMetaObject::invokeMethod(panel->root, "poll");
  QCoreApplication::processEvents();
  require(playhead->isVisible(), "the playhead shows while a hit plays");
  require(text_of(*panel, QStringLiteral("anvilPeak")).startsWith(QStringLiteral("−3.0")),
          "the live peak reads");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("ANVIL_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const QVariantList a_minor{9, 11, 0, 2, 4, 5, 7};
  const std::vector<std::pair<const char*, const char*>> shots{
      {"body", "pitch"}, {"click", "amp"}, {"rumble", "drive"}, {"sub", "pitch"}};
  for (std::size_t preset = 0; preset < table().presets.size(); ++preset) {
    auto adapter = adapter_for(preset);
    NativeSynthDeviceController controller(table(), *adapter);
    AnvilPanelModel model(controller);
    auto panel = load_panel(controller, &model, a_minor);
    const auto& [layer, curve] = shots[preset % shots.size()];
    panel->root->setProperty("selectedLayer", QString::fromLatin1(layer));
    panel->root->setProperty("curveId", QString::fromLatin1(curve));
    QTest::qWait(150);
    QString name = QString::fromUtf8(table().presets[preset].name.data(),
                                     static_cast<qsizetype>(table().presets[preset].name.size()))
                       .toLower()
                       .replace(QLatin1Char(' '), QLatin1Char('_'));
    const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/anvil_") + name +
                         QStringLiteral(".png");
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    the_preview_draws_the_kick();
    layers_tabs_and_knobs();
    a_point_drag_is_one_gesture();
    tune_to_key_lands_in_the_song_key();
    telemetry_moves_the_playhead();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "anvil panel contract passed\n";
  return 0;
}
