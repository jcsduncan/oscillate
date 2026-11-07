#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/pump.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
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
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Pump panel (ADR-0058): every parameter on a table-driven control; the
// shape library, Fit to Kick, Invert and Clear writing the shape document; a
// point drag as one document gesture whose ends move alone; the Kick Audio
// controls appearing with that trigger; slot and band tabs drawing their own
// curves (and following until drawn); the pencil, mirror, shift, random and
// clipboard tools; pattern steps; MIDI Learn; the telemetry decode into the
// readout, the reduction meter and the kick; presets; and screenshots on
// request, live from the real Pump.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace pump = oscillate::native_dsp::effects::pump;
namespace engine = oscillate::engine;
namespace project = oscillate::project;

using oscillate::test::require;

const effects::EffectEngine& pump_engine() { return pump::engine(); }
const synth::EngineDescriptor& table() { return *pump_engine().descriptor; }

std::size_t preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return i;
    }
  }
  throw std::runtime_error("missing preset " + std::string(name));
}

std::vector<double> preset(std::string_view name) {
  return synth::preset_values(table(), table().presets[preset_index(name)]).value();
}

project::NativeSynthDocument shape_of(std::string_view preset_name) {
  return pump_engine().preset_document(preset_index(preset_name));
}

std::vector<project::NativeCurvePoint> points_of(const project::NativeSynthDocument& doc) {
  require(doc.curves.size() == 1 && doc.curves.front().id == pump::shape_curve,
          "the document holds the shape curve");
  return doc.curves.front().points;
}

// The points of the document's curve `id`, or none.
std::vector<project::NativeCurvePoint> curve_named(const project::NativeSynthDocument& doc,
                                                   std::string_view id) {
  for (const auto& curve : doc.curves) {
    if (curve.id == id) {
      return curve.points;
    }
  }
  return {};
}

bool has_curve(const project::NativeSynthDocument& doc, std::string_view id) {
  return std::ranges::any_of(doc.curves, [&](const auto& curve) { return curve.id == id; });
}

// The drawn volume at x, for straight and stepped segments.
double gain_at(const std::vector<project::NativeCurvePoint>& points, double x) {
  for (std::size_t i = 1; i < points.size(); ++i) {
    if (x <= points[i].x) {
      const auto& a = points[i - 1];
      const auto& b = points[i];
      require(a.tension == 0.0, "gain_at reads straight segments only");
      if (a.segment == project::NativeCurveSegment::step) {
        return a.y;
      }
      return a.y + (b.y - a.y) * (x - a.x) / std::max(1e-9, b.x - a.x);
    }
  }
  return points.back().y;
}

// An adapter holding a document, as the session does for Pump's slots.
std::unique_ptr<InMemoryNativeSynthAdapter> adapter_with(std::vector<double> values,
                                                         const project::NativeSynthDocument& doc) {
  auto adapter = std::make_unique<InMemoryNativeSynthAdapter>(std::move(values));
  adapter->submit_document(doc, NativeSynthGesture::single);
  return adapter;
}

// ---- QML ------------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller, bool keyed = false) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/pump"
      PumpPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/PumpPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Pump panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)},
       {QStringLiteral("keyed"), keyed},
       {QStringLiteral("keyName"), QStringLiteral("Kick")}});
  require(object != nullptr,
          "Pump panel was not created: " + component.errorString().toStdString());
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

// Where a shape point sits on the editor (PumpCurve's plot: 14 px in from
// each side, 30 px from the top, 24 px from the bottom), at full depth.
QPoint point_on_curve(QQuickItem* curve, double x, double y) {
  const double left = 14.0;
  const double right = curve->width() - 14.0;
  const double top = 30.0;
  const double bottom = curve->height() - 24.0;
  return scene(curve, left + x * (right - left), bottom - y * (bottom - top));
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 700,
          "panel has its designed size");
  for (const auto& parameter : table().parameters) {
    const auto id =
        QString::fromUtf8(parameter.id.data(), static_cast<qsizetype>(parameter.id.size()));
    bool found = false;
    for (const auto* kind :
         {"pumpKnob:", "pumpSegment:", "pumpStepper:", "pumpDropdown:", "pumpToggle:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    // Pattern steps are chips: "pump.step.3" is "pumpStep:3".
    if (id.startsWith(QStringLiteral("pump.step."))) {
      found = find_item(panel->root, QStringLiteral("pumpStep:") + id.mid(10)) != nullptr;
    }
    require(found, "a control for " + id.toStdString());
  }
  for (int i = 0; i < 11; ++i) {
    require(item(*panel, QStringLiteral("pumpShape:%1").arg(i))->isVisible(),
            "the library shows every shape");
  }
  require(item(*panel, QStringLiteral("pumpShape:0"))->property("on").toBool(),
          "Classic Pump is lit as the current shape");
}

void knobs_and_segments_edit_values() {
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  const double depth = controller.value(QStringLiteral("pump.depth"));
  wheel(*panel, centre(item(*panel, QStringLiteral("pumpKnob:pump.depth"))), -120);
  require(controller.value(QStringLiteral("pump.depth")) < depth, "scrolling a knob edits it");
  auto* rate = item(*panel, QStringLiteral("pumpSegment:pump.rate"));
  click(*panel, scene(rate, rate->width() * (0.5 / 6.0), rate->height() / 2));
  require(controller.choice(QStringLiteral("pump.rate")) == 0, "the rate segment picks 1/16");
  require(text_of(*panel, QStringLiteral("pumpReadout")).contains(QStringLiteral("RATE")),
          "the display names the control touched");
  auto* kick_audio = item(*panel, QStringLiteral("pumpKickAudio"));
  require(!kick_audio->isVisible(), "Kick Audio controls hide in Tempo");
  controller.set_choice(QStringLiteral("pump.trigger"), 1);
  QCoreApplication::processEvents();
  require(kick_audio->isVisible(), "Kick Audio shows its threshold and lookahead");
}

void the_library_and_tools_write_the_shape() {
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("pumpShape:3"))));
  const auto gate = points_of(shape_of("Hard Gate"));
  require(adapter->document_submissions().back().gesture == NativeSynthGesture::single,
          "a shape is one edit");
  const auto picked = points_of(adapter->document());
  require(picked.size() == gate.size() && picked[1].x == gate[1].x && picked[2].y == gate[2].y,
          "the Gate tile writes the gate shape");
  require(item(*panel, QStringLiteral("pumpShape:3"))->property("on").toBool(),
          "the picked shape lights");

  click(*panel, centre(item(*panel, QStringLiteral("pumpFlip"))));
  const auto flipped = points_of(adapter->document());
  require(flipped.front().y == 1.0 && flipped.back().y == 0.0, "Invert turns the shape over");

  click(*panel, centre(item(*panel, QStringLiteral("pumpClear"))));
  const auto classic = points_of(shape_of("Classic Pump"));
  require(points_of(adapter->document()) == classic, "Clear returns to Classic Pump");

  // With nothing playing, Fit to Kick fits the preview kick: a duck that
  // recovers within the cycle.
  click(*panel, centre(item(*panel, QStringLiteral("pumpFit"))));
  const auto fitted = points_of(adapter->document());
  require(fitted.size() == 4 && fitted.front().y == 0.0 && fitted[2].x > 0.06 && fitted[2].x < 0.95,
          "Fit to Kick writes a duck that recovers as the kick fades");
}

void a_point_drag_is_one_gesture() {
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  auto* curve = item(*panel, QStringLiteral("pumpCurve"));
  const auto start = adapter->document_submissions().size();
  // The release point (0.42, 0.97) moves later and stays up.
  const QPoint from = point_on_curve(curve, 0.42, 0.97);
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from);
  QTest::mouseMove(panel->window.get(), from + QPoint(30, 0));
  QTest::mouseMove(panel->window.get(), from + QPoint(60, 0));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from + QPoint(60, 0));
  const auto& s = adapter->document_submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a drag is begin, updates and commit");
  for (std::size_t i = start + 1; i + 1 < s.size(); ++i) {
    require(s[i].gesture == NativeSynthGesture::update, "the drag's steps are updates");
  }
  const auto& moved = points_of(adapter->document());
  require(moved[2].x > 0.45, "the point moved later");

  // The first point moves alone: the duck starts higher, the end stays up.
  const QPoint first = point_on_curve(curve, 0.0, 0.0);
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, first);
  QTest::mouseMove(panel->window.get(), first + QPoint(0, -60));
  QTest::mouseMove(panel->window.get(), first + QPoint(0, -120));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, first + QPoint(0, -120));
  const auto& ends = points_of(adapter->document());
  require(ends.front().y > 0.2 && ends.back().y == 1.0, "the ends of the cycle move alone");

  // Escape mid-drag puts the shape back.
  const auto before = adapter->document();
  const QPoint knee = point_on_curve(curve, ends[1].x, ends[1].y);
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knee);
  QTest::mouseMove(panel->window.get(), knee + QPoint(10, -80));
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knee + QPoint(10, -80));
  require(adapter->document() == before, "Escape cancels the drag");
}

QVariant call(Panel& panel, const char* function, QVariant argument = {}) {
  QVariant result;
  const bool done =
      argument.isValid()
          ? QMetaObject::invokeMethod(panel.root, function, Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, argument))
          : QMetaObject::invokeMethod(panel.root, function, Q_RETURN_ARG(QVariant, result));
  require(done, std::string("panel function ") + function);
  return result;
}

void slots_and_bands_draw_their_own_curves() {
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  auto* curve = item(*panel, QStringLiteral("pumpCurve"));
  // B follows A until it is drawn: picking a shape there makes it B's own.
  click(*panel, centre(item(*panel, QStringLiteral("pumpSlot:B"))));
  require(curve->property("curveId").toString() == QStringLiteral("shape.b"), "slot B is edited");
  require(curve->property("follows").toBool(), "B follows A until drawn");
  require(curve->property("points").toList().size() ==
              static_cast<qsizetype>(points_of(shape_of("Classic Pump")).size()),
          "B shows A's shape");
  click(*panel, centre(item(*panel, QStringLiteral("pumpShape:3"))));
  const auto& doc = adapter->document();
  require(curve_named(doc, "shape.b") == points_of(shape_of("Hard Gate")) &&
              curve_named(doc, "shape") == points_of(shape_of("Classic Pump")),
          "the Gate tile draws B and leaves A");
  require(!curve->property("follows").toBool(), "B is its own once drawn");

  // A point drag on a following slot creates it from A.
  click(*panel, centre(item(*panel, QStringLiteral("pumpSlot:C"))));
  const QPoint from = point_on_curve(curve, 0.42, 0.97);
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from);
  QTest::mouseMove(panel->window.get(), from + QPoint(40, 0));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from + QPoint(40, 0));
  const auto c = curve_named(adapter->document(), "shape.c");
  require(c.size() == 4 && c[2].x > 0.45, "dragging C's point draws C from A");

  // Three Band: the mid band of B takes its own curve, and resets.
  require(!item(*panel, QStringLiteral("pumpBandTab:mid"))->isVisible(), "band tabs hide in Full");
  controller.set_choice(QStringLiteral("pump.band"), 2);
  QCoreApplication::processEvents();
  click(*panel, centre(item(*panel, QStringLiteral("pumpSlot:B"))));
  click(*panel, centre(item(*panel, QStringLiteral("pumpBandTab:mid"))));
  require(curve->property("curveId").toString() == QStringLiteral("shape.b.mid"),
          "B's mid band is edited");
  require(curve->property("depthId").toString() == QStringLiteral("pump.depth_mid"),
          "the mid band draws against its own depth");
  click(*panel, centre(item(*panel, QStringLiteral("pumpFlip"))));
  const auto mid = curve_named(adapter->document(), "shape.b.mid");
  require(!mid.empty() && mid.front().y == 1.0, "Invert draws B's mid band from B");
  auto* reset = item(*panel, QStringLiteral("pumpResetCurve"));
  require(reset->isVisible(), "a drawn band can follow its slot again");
  click(*panel, centre(reset));
  require(!has_curve(adapter->document(), "shape.b.mid"), "the reset band follows B again");
}

void drawing_tools_transform_the_shape() {
  auto adapter = adapter_with(preset("Hard Gate"), shape_of("Hard Gate"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  auto* curve = item(*panel, QStringLiteral("pumpCurve"));

  // Mirror plays the gate backwards: open, then the hold at the end.
  call(*panel, "mirrorShape");
  auto shape = points_of(adapter->document());
  require(std::fabs(gain_at(shape, 0.1) - 1.0) < 1e-6 && std::fabs(gain_at(shape, 0.9)) < 1e-6,
          "Mirror reverses the cycle");
  call(*panel, "mirrorShape");
  shape = points_of(adapter->document());
  require(std::fabs(gain_at(shape, 0.1)) < 1e-6 && std::fabs(gain_at(shape, 0.5) - 1.0) < 1e-6,
          "Mirror twice is the gate again");

  // A quarter-note cycle on a 1/16 grid is four steps: shift one later.
  call(*panel, "shiftShape", 1);
  shape = points_of(adapter->document());
  require(std::fabs(gain_at(shape, 0.1) - 1.0) < 1e-6 && std::fabs(gain_at(shape, 0.35)) < 1e-6 &&
              std::fabs(gain_at(shape, 0.6) - 1.0) < 1e-6,
          "Shift moves the hold a step later");
  call(*panel, "shiftShape", -1);
  shape = points_of(adapter->document());
  require(std::fabs(gain_at(shape, 0.1)) < 1e-6 && std::fabs(gain_at(shape, 0.5) - 1.0) < 1e-6,
          "and back");

  // Copy, change, paste: the copy comes back.
  const auto copied = points_of(adapter->document());
  call(*panel, "copyShape");
  call(*panel, "randomShape", 42);
  const auto random = points_of(adapter->document());
  require(random != copied && random.size() >= 2 && random.size() <= 64,
          "Random draws a new shape that fits the document");
  require(call(*panel, "pasteShape").toBool() && points_of(adapter->document()) == copied,
          "Paste brings the copied shape back");
  controller.set_clipboard_text(QStringLiteral("not a shape"));
  require(!call(*panel, "pasteShape").toBool() && points_of(adapter->document()) == copied,
          "Paste ignores what is not a shape");

  // The pencil paints the first two steps at half level, as flat steps.
  click(*panel, centre(item(*panel, QStringLiteral("pumpTool:pencil"))));
  require(curve->property("tool").toString() == QStringLiteral("pencil"), "the pencil is picked");
  const QPoint start = point_on_curve(curve, 0.05, 0.5);
  const QPoint end = point_on_curve(curve, 0.45, 0.5);
  const auto before = adapter->document_submissions().size();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, start);
  QTest::mouseMove(panel->window.get(), (start + end) / 2);
  QTest::mouseMove(panel->window.get(), end);
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, end);
  require(adapter->document_submissions()[before].gesture == NativeSynthGesture::begin &&
              adapter->document_submissions().back().gesture == NativeSynthGesture::commit,
          "a pencil stroke is one gesture");
  shape = points_of(adapter->document());
  require(std::fabs(gain_at(shape, 0.1) - 0.5) < 0.03 &&
              std::fabs(gain_at(shape, 0.3) - 0.5) < 0.03,
          "the painted steps sit at the pointer's level");
  require(std::ranges::any_of(
              shape,
              [](const auto& point) { return point.segment == project::NativeCurveSegment::step; }),
          "painted steps are flat steps");
  require(std::fabs(gain_at(shape, 0.75) - 1.0) < 1e-6, "the steps not painted keep the gate");
}

void pattern_steps_and_learn() {
  auto adapter = adapter_with(preset("Bar Fill"), shape_of("Bar Fill"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("pumpStep:4"))->property("slot").toInt() == 1,
          "Bar Fill plays B on step 4");
  click(*panel, centre(item(*panel, QStringLiteral("pumpStep:2"))));
  require(controller.choice(QStringLiteral("pump.step.2")) == 1,
          "a click moves a step to the next slot");
  QTest::mouseClick(panel->window.get(), Qt::RightButton, Qt::NoModifier,
                    centre(item(*panel, QStringLiteral("pumpStep:2"))));
  require(controller.choice(QStringLiteral("pump.step.2")) == 0, "a right-click moves it back");

  // MIDI Learn takes the next note heard on the keying tracks.
  controller.set_choice(QStringLiteral("pump.trigger"), 2);
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("pumpMidi"))->isVisible(), "MIDI shows its note range");
  auto frame = std::vector<float>(pump::telemetry::size, 0.0F);
  frame[pump::telemetry::phase] = -1.0F;
  frame[pump::telemetry::last_note] = 36.0F;
  adapter->set_telemetry(frame);
  click(*panel, centre(item(*panel, QStringLiteral("pumpLearn"))));
  require(QTest::qWaitFor(
              [&] {
                return controller.value(QStringLiteral("pump.note_low")) == 36.0 &&
                       controller.value(QStringLiteral("pump.note_high")) == 36.0;
              },
              3000),
          "Learn sets the range to the kick's note");
  require(!item(*panel, QStringLiteral("pumpLearn"))->property("on").toBool(), "Learn disarms");
  call(*panel, "learnNote", 38);
  require(controller.value(QStringLiteral("pump.note_low")) == 38.0, "a learnt note is the range");
}

std::vector<float> frame_of(double phase, double gain_db, bool keyed) {
  namespace t = pump::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::phase] = static_cast<float>(phase);
  frame[t::gain_db] = static_cast<float>(gain_db);
  frame[t::cycle_seconds] = 0.5F;
  frame[t::cycle_beats] = 1.0F;
  frame[t::keyed] = keyed ? 1.0F : 0.0F;
  for (std::size_t i = 0; i < pump::view_points; ++i) {
    const double x = static_cast<double>(i) / pump::view_points;
    frame[t::kick + i] = static_cast<float>(0.6 * std::exp(-x * 6.0));
    frame[t::applied + i] = static_cast<float>(std::min(1.0, x * 3.0));
    frame[t::input + i] = 0.5F;
    frame[t::output + i] = static_cast<float>(0.5 * std::min(1.0, x * 3.0));
  }
  frame[t::last_note] = -1.0F;
  return frame;
}

void telemetry_reaches_the_editor() {
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  const auto decoded = [&] {
    adapter->set_telemetry(frame_of(0.25, -6.0, true));
    return controller.telemetry();
  }();
  require(decoded.value(QStringLiteral("live")).toBool() &&
              std::fabs(decoded.value(QStringLiteral("gain_db")).toDouble() + 6.0) < 1e-6 &&
              decoded.value(QStringLiteral("kick")).toList().size() ==
                  static_cast<qsizetype>(pump::view_points) &&
              decoded.value(QStringLiteral("applied")).toList().size() ==
                  static_cast<qsizetype>(pump::view_points) &&
              decoded.value(QStringLiteral("keyed")).toBool() &&
              decoded.value(QStringLiteral("output")).toList().size() ==
                  static_cast<qsizetype>(pump::view_points),
          "the controller decodes Pump's telemetry");
  auto panel = load_panel(controller, true);
  require(QTest::qWaitFor(
              [&] {
                return text_of(*panel, QStringLiteral("pumpGainReadout")) ==
                       QStringLiteral("−6.0 dB");
              },
              1500),
          "the readout shows the gain now");
  auto* curve = item(*panel, QStringLiteral("pumpCurve"));
  require(std::fabs(curve->property("phase").toDouble() - 0.25) < 1e-6, "the playhead follows");
  require(curve->property("cycleSeconds").toDouble() == 0.5, "the cycle's length is shown");
  require(item(*panel, QStringLiteral("pumpReductionMeter"))->width() > 10,
          "the reduction meter moves");
  const auto kick = curve->property("kick").toList();
  require(!kick.isEmpty() && std::fabs(kick.front().toDouble() - 1.0) < 1e-6,
          "the captured kick is scaled to its peak");
  require(curve->property("output").toList().size() == static_cast<qsizetype>(pump::view_points),
          "the cycle's output reaches the editor");
  click(*panel, centre(item(*panel, QStringLiteral("pumpDisplay"))));
  require(curve->property("display").toString() == QStringLiteral("io"),
          "the view switches to input and output");
  adapter->set_telemetry({});
  require(QTest::qWaitFor(
              [&] {
                return text_of(*panel, QStringLiteral("pumpGainReadout")) ==
                       QStringLiteral("−0.0 dB");
              },
              1500),
          "no telemetry: the readout rests");
  require(curve->property("phase").toDouble() < 0, "no playhead when nothing plays");
}

void presets_load_values_and_shape() {
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("pumpPresetNext"))));
  require(controller.preset_name() != QStringLiteral("Classic Pump"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("pumpPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Classic Pump"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("pumpMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("pumpPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;

// The real Pump over a bass note keyed by a kick on every beat at 120 BPM;
// a telemetry frame every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const project::NativeSynthDocument& doc,
                                               std::size_t frames) {
  auto created = effects::NativeEffectRuntime::create(pump_engine(), values, doc, rate);
  require(created.has_value(), "a Pump runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> out;
  std::vector<engine::StereoFrame> block;
  std::vector<engine::StereoFrame> key;
  const double beats_per_frame = 2.0 / rate;
  for (std::size_t position = 0; position < frames; position += 160) {
    block.resize(160);
    key.resize(160);
    for (std::size_t i = 0; i < 160; ++i) {
      const double t = static_cast<double>(position + i) / rate;
      const double since = std::fmod(t, 0.5);
      const double k =
          0.9 * std::exp(-since * 8.5) *
          std::sin(2 * std::numbers::pi * (48.0 * since + 3.6 * (1 - std::exp(-since * 28))));
      const double bass = 0.3 * std::sin(2 * std::numbers::pi * 55 * t);
      block[i] = {static_cast<float>(bass), static_cast<float>(bass)};
      key[i] = {static_cast<float>(k), static_cast<float>(k)};
    }
    const double beat = static_cast<double>(position) * beats_per_frame;
    const synth::SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, key, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Pump renders");
    if ((position + 160) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Pump publishes telemetry");
      out.push_back(frame);
    }
  }
  return out;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("PUMP_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* file;
  };
  for (const Shot& shot :
       {Shot{"Classic Pump", "/pump_classic.png"}, Shot{"Hard Gate", "/pump_gate.png"},
        Shot{"Kick Follower", "/pump_kick.png"}, Shot{"Drum Bus Kick", "/pump_drum_bus.png"},
        Shot{"Three Band", "/pump_three_band.png"}, Shot{"Bar Fill", "/pump_pattern.png"},
        Shot{"Rack Kick", "/pump_midi.png"}}) {
    const auto values = preset(shot.preset);
    const auto doc = shape_of(shot.preset);
    auto adapter = adapter_with(values, doc);
    NativeSynthDeviceController controller(table(), *adapter);
    auto panel = load_panel(controller, true);
    const auto frames = real_telemetry(values, doc, static_cast<std::size_t>(2.3 * rate));
    for (std::size_t i = frames.size() > 20 ? frames.size() - 20 : 0; i < frames.size(); ++i) {
      adapter->set_telemetry(frames[i]);
      QTest::qWait(33);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

// PUMP_PANEL_BENCH: the panel's own cost while playing, 300 telemetry polls
// with a moving playhead, each rendered; prints CPU milliseconds per frame.
void bench_if_requested() {
  if (std::getenv("PUMP_PANEL_BENCH") == nullptr)
    return;
  auto adapter = adapter_with(preset("Classic Pump"), shape_of("Classic Pump"));
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller, true);
  const auto run = [&](int frames) {
    const auto start = std::clock();
    for (int i = 0; i < frames; ++i) {
      adapter->set_telemetry(frame_of(static_cast<double>(i % 100) / 100.0, -3.0, true));
      QMetaObject::invokeMethod(panel->root, "poll");
      (void)panel->window->grabWindow();
    }
    return 1e3 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC / frames;
  };
  (void)run(20);
  std::cout << "pump panel: " << run(300) << " ms CPU per polled frame\n";
  // A drag repaints the shape layer every step.
  auto* curve = item(*panel, QStringLiteral("pumpCurve"));
  const auto start = std::clock();
  for (int i = 0; i < 100; ++i) {
    QMetaObject::invokeMethod(curve, "repaintShape");
    (void)panel->window->grabWindow();
  }
  std::cout << "pump panel: "
            << 1e3 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC / 100
            << " ms CPU per shape repaint\n";
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    knobs_and_segments_edit_values();
    the_library_and_tools_write_the_shape();
    a_point_drag_is_one_gesture();
    slots_and_bands_draw_their_own_curves();
    drawing_tools_transform_the_shape();
    pattern_steps_and_learn();
    telemetry_reaches_the_editor();
    presets_load_values_and_shape();
    screenshots_if_requested();
    bench_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pump panel contract passed\n";
  return 0;
}
