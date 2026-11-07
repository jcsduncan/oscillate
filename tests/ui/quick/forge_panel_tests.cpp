#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/effects/forge.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Forge panel (ADR-0040): the controller's curve sketch against the
// engine, its telemetry decode, and the QML panel driven by the engine
// table, its forge view live and as a preview.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace forge = oscillate::native_dsp::effects::forge;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *forge::engine().descriptor; }
std::size_t index(std::string_view id) {
  const auto found = synth::find_parameter(table(), id);
  if (!found) {
    throw std::runtime_error("unknown parameter " + std::string(id));
  }
  return *found;
}

int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

std::vector<double> preset(std::string_view name) {
  return synth::preset_values(table(),
                              table().presets[static_cast<std::size_t>(preset_index(name))])
      .value();
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// ---- Sketches -------------------------------------------------------------------

void the_curve_sketch_is_the_engines() {
  auto values = preset("Hard Clipper");
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  const auto curve = controller.forge_curve(65);
  std::vector<float> expected(65);
  forge::transfer_curve(values, expected);
  require(curve.size() == 65, "65 points");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    require(curve[static_cast<qsizetype>(i)].toDouble() == static_cast<double>(expected[i]),
            "the sketch is the engine's own curve");
  }
  require(curve[32].toDouble() == 0.0, "the curve passes through the origin");
  require(curve[64].toDouble() < 1.01 && curve[0].toDouble() > -1.01,
          "a hard clipper with makeup stays inside full scale");
  require(controller.forge_makeup_db() < -1.0, "driving the clipper cuts its makeup");
  values[index("forge.auto_gain")] = 0.0;
  adapter.submit(values, oscillate::quick::NativeSynthGesture::single);
  controller.refresh();
  require(controller.forge_makeup_db() == 0.0, "no auto gain, no makeup");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter other(synth::default_values(prism));
  NativeSynthDeviceController not_forge(prism, other);
  require(not_forge.forge_curve(65).isEmpty() && not_forge.forge_makeup_db() == 0.0,
          "only Forge has a transfer curve");
}

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<float> forge_frame(float input, float output, float makeup) {
  namespace t = forge::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_rms] = input;
  frame[t::output_rms] = output;
  frame[t::makeup_db] = makeup;
  for (std::size_t i = 0; i < forge::curve_points; ++i) {
    const double x = -1.0 + 2.0 * static_cast<double>(i) / (forge::curve_points - 1);
    frame[t::curve + i] = static_cast<float>(std::tanh(3.0 * x) * 0.5);
  }
  return frame;
}

void controller_decodes_forge_telemetry() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(!controller.telemetry().value(QStringLiteral("live")).toBool(), "nothing plays");
  adapter.set_telemetry(forge_frame(0.25F, 0.2F, -6.5F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool(), "live");
  require(std::fabs(live.value(QStringLiteral("input_rms")).toDouble() - 0.25) < 1e-6 &&
              std::fabs(live.value(QStringLiteral("output_rms")).toDouble() - 0.2) < 1e-6 &&
              std::fabs(live.value(QStringLiteral("makeup_db")).toDouble() + 6.5) < 1e-6,
          "levels and makeup decode");
  const auto curve = live.value(QStringLiteral("curve")).toList();
  require(curve.size() == static_cast<qsizetype>(forge::curve_points) &&
              std::fabs(curve[64].toDouble() - std::tanh(3.0) * 0.5) < 1e-6,
          "the live curve decodes");
}

// ---- QML ------------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/forge"
      ForgePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/ForgePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Forge panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Forge panel was not created: " + component.errorString().toStdString());
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

QPoint centre(QQuickItem* target) {
  return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
}

void click(Panel& panel, QPoint point) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, point);
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1100 && panel->root->implicitHeight() >= 540,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    const bool shown = find_item(panel->root, QStringLiteral("forgeKnob:") + id) != nullptr ||
                       find_item(panel->root, QStringLiteral("forgeSegment:") + id) != nullptr;
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("forgePresetName")) == QStringLiteral("Warm Tape"),
          "defaults are the Warm Tape");
  auto* view = item(*panel, QStringLiteral("forgeCurveView"));
  require(view->property("points").toList().size() == 129, "the forge previews the engine's curve");
  // The harmonics readout: symmetric tape has no even harmonics; the
  // rectifier's output leads with the octave.
  const auto tape = view->property("harmonics").toList();
  require(tape.size() == 9 && tape[1].toDouble() < -90.0 && tape[2].toDouble() > -60.0,
          "tape shows odd harmonics only");
  controller.set_choice(QStringLiteral("forge.model"), 7);
  QCoreApplication::processEvents();
  const auto rectified = view->property("harmonics").toList();
  require(rectified[1].toDouble() > rectified[0].toDouble() + 10.0,
          "the rectifier's harmonics lead with the octave");
}

void curves_and_presets() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* view = item(*panel, QStringLiteral("forgeCurveView"));
  const auto tape = view->property("points").toList();
  auto* curves = item(*panel, QStringLiteral("forgeSegment:forge.model"));
  // The sixth tile of eight is the triangle fold.
  const QPoint fold =
      curves->mapToScene(QPointF(curves->width() / 2, curves->height() * 5.5 / 8.0)).toPoint();
  click(*panel, fold);
  require(controller.choice(QStringLiteral("forge.model")) == 5, "a tile picks the curve");
  QCoreApplication::processEvents();
  require(view->property("points").toList() != tape, "the forge redraws the new curve");
  require(view->property("modelName").toString() == QStringLiteral("Fold"), "and names it");
  require(text_of(*panel, QStringLiteral("forgeReadout")) == QStringLiteral("SHAPE · MODEL"),
          "the display reads back the curve");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(controller.choice(QStringLiteral("forge.model")) == 6, "Down steps to Sine Fold");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("forge.model")) == 0, "Delete resets the curve");

  click(*panel, centre(item(*panel, QStringLiteral("forgePresetNext"))));
  require(controller.preset_name() == QStringLiteral("Tube Warmth"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("forgePresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Warm Tape"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("forgeMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("forgePresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");

  const double heat = panel->root->property("heat").toDouble();
  auto* drive = item(*panel, QStringLiteral("forgeKnob:forge.drive"));
  drive->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("forge.drive")) > 9.0, "Up raises the drive");
  require(panel->root->property("heat").toDouble() > heat, "and the forge runs hotter");

  const QString makeup = text_of(*panel, QStringLiteral("forgeMakeup"));
  require(makeup.endsWith(QStringLiteral(" dB")), "auto gain shows its makeup");
  controller.set_choice(QStringLiteral("forge.auto_gain"), 0);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("forgeMakeup")) == QStringLiteral("OFF"),
          "auto gain off reads OFF");

  adapter.set_status({.saved_revision = 9,
                      .audible_revision = 9,
                      .deferred_changes = true,
                      .editable = true,
                      .message = {}});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("forgeStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#ff9d2e")),
          "deferred status is highlighted");
}

// The forge previews the curve until audio runs through, then draws the
// running curve with the input's peak riding it and the meters lit.
void the_forge_goes_live() {
  InMemoryNativeSynthAdapter adapter(preset("Warm Tape"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* view = item(*panel, QStringLiteral("forgeCurveView"));
  QTest::qWait(300);
  require(!view->property("live").toBool() && text_of(*panel, QStringLiteral("forgeCurveCaption"))
                                                  .startsWith(QStringLiteral("TRANSFER CURVE")),
          "no telemetry: the preview");
  require(text_of(*panel, QStringLiteral("forgeMeterValue:in")) == QStringLiteral("—"),
          "meters are dark");
  adapter.set_telemetry(forge_frame(0.25F, 0.2F, -6.5F));
  require(QTest::qWaitFor([&] { return view->property("live").toBool(); }, 1000),
          "live telemetry arrives");
  require(view->property("points").toList().size() == static_cast<qsizetype>(forge::curve_points),
          "the running curve is drawn");
  require(std::fabs(view->property("inputPeak").toDouble() - 0.25 * std::sqrt(2.0)) < 1e-6,
          "the input's peak rides the curve");
  require(text_of(*panel, QStringLiteral("forgeCurveCaption")).startsWith(QStringLiteral("LIVE")),
          "the caption says live");
  require(text_of(*panel, QStringLiteral("forgeMeterValue:in")) == QStringLiteral("−12.0") &&
              text_of(*panel, QStringLiteral("forgeMeterValue:out")) == QStringLiteral("−14.0"),
          "the meters read in dBFS");
  require(text_of(*panel, QStringLiteral("forgeMakeup")) == QStringLiteral("−6.5 dB"),
          "the running makeup shows");
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !view->property("live").toBool(); }, 1000),
          "telemetry gone: back to the preview");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("FORGE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"Warm Tape", "forge.drive", "/forge_warm_tape.png"},
                          Shot{"West Coast Fold", "forge.model", "/forge_west_coast_fold.png"},
                          Shot{"Hard Clipper", "forge.drive", "/forge_hard_clipper.png"}}) {
    InMemoryNativeSynthAdapter adapter(preset(shot.preset));
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    auto* theme = panel->root->property("theme").value<QObject*>();
    theme->setProperty("lastTouched", QString::fromLatin1(shot.touched));
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  // Live: Hot Tube with a hot input, its own curve from the engine.
  auto values = preset("Hot Tube");
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  namespace t = forge::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_rms] = 0.42F;
  frame[t::output_rms] = 0.31F;
  frame[t::makeup_db] = static_cast<float>(forge::makeup_db(values));
  forge::transfer_curve(values, std::span(frame).subspan(t::curve, forge::curve_points));
  adapter.set_telemetry(frame);
  QTest::qWait(400);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/forge_live.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    the_curve_sketch_is_the_engines();
    controller_decodes_forge_telemetry();
    panel_covers_every_parameter();
    curves_and_presets();
    the_forge_goes_live();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "forge panel contract passed\n";
  return 0;
}
