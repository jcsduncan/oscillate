#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/test/check.hpp"

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
using oscillate::quick::NativeSynthPanelStatus;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *synth::prism::engine().descriptor; }

std::size_t index(std::string_view id) { return *synth::find_parameter(table(), id); }

int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

// ---- Controller ---------------------------------------------------------------

void model_is_driven_by_the_engine_table() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  const auto cutoff = controller.parameter(QStringLiteral("prism.filter.cutoff"));
  require(cutoff.value(QStringLiteral("name")).toString() == QStringLiteral("Cutoff") &&
              cutoff.value(QStringLiteral("group")).toString() == QStringLiteral("Filter") &&
              cutoff.value(QStringLiteral("scale")).toString() == QStringLiteral("log"),
          "cutoff facts come from the descriptor");
  const auto unison = controller.parameter(QStringLiteral("prism.osc1.unison"));
  const auto phase = controller.parameter(QStringLiteral("prism.osc1.phase"));
  require(!unison.value(QStringLiteral("next_note")).toBool() &&
              phase.value(QStringLiteral("next_note")).toBool() &&
              unison.value(QStringLiteral("automatable")).toBool() &&
              phase.value(QStringLiteral("automatable")).toBool(),
          "next-note timing is exposed, and stepped automation is allowed");
  require(controller.parameter(QStringLiteral("prism.filter.mode"))
                  .value(QStringLiteral("choices"))
                  .toStringList()
                  .size() == 9,
          "filter choices are exposed");
  require(controller.parameter(QStringLiteral("prism.mod1.amount"))
              .value(QStringLiteral("bipolar"))
              .toBool(),
          "bipolar ranges are flagged");
  require(controller.parameter_ids(QStringLiteral("Osc 1")).size() == 10,
          "parameters group by section");
  require(controller.parameter(QStringLiteral("prism.nope")).isEmpty(),
          "unknown ids are empty, not invented");
  require(controller.preset_name() == QStringLiteral("Prism Default") &&
              !controller.preset_modified(),
          "default values are recognised as the default preset");
}

void edits_round_trip_through_the_adapter() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  const QString cutoff = QStringLiteral("prism.filter.cutoff");
  const int revision = controller.revision();
  controller.set_normalised(cutoff, 0.5);
  const double expected = 20.0 * std::sqrt(1000.0);
  require(std::fabs(controller.value(cutoff) - expected) < 1e-6, "log knob midpoint");
  require(std::fabs(controller.normalised(cutoff) - 0.5) < 1e-9, "normalised round trip");
  require(controller.display(cutoff) == QStringLiteral("632 Hz"), "value is formatted");
  require(controller.display_at(cutoff, 1.0) == QStringLiteral("20.00 kHz"),
          "previews format any position");
  require(controller.revision() > revision, "revision advances");
  require(adapter.submissions().size() == 1 &&
              adapter.submissions().back().gesture == NativeSynthGesture::single &&
              adapter.values()[index("prism.filter.cutoff")] == controller.value(cutoff),
          "a click edit is one complete submission");
  require(controller.preset_name() == QStringLiteral("Prism Default") &&
              controller.preset_modified(),
          "edits mark the preset as modified");

  controller.set_choice(QStringLiteral("prism.filter.mode"), 8);
  require(controller.choice(QStringLiteral("prism.filter.mode")) == 8 &&
              controller.display(QStringLiteral("prism.filter.mode")) == QStringLiteral("Ladder"),
          "choices set by index");
  controller.step(QStringLiteral("prism.osc1.unison"), 1, false);
  require(controller.value(QStringLiteral("prism.osc1.unison")) == 2.0, "stepped +1");
  controller.step(QStringLiteral("prism.osc1.unison"), -5, false);
  require(controller.value(QStringLiteral("prism.osc1.unison")) == 1.0, "steps clamp");
  controller.reset_to_default(cutoff);
  require(controller.is_default(cutoff), "reset restores the default");
  const auto count = adapter.submissions().size();
  controller.set_value(cutoff, 1e9);
  require(controller.value(cutoff) == 20'000.0, "out-of-range input is clamped");
  controller.set_value(cutoff, 20'000.0);
  require(adapter.submissions().size() == count + 1, "unchanged values are not resubmitted");
}

void drags_are_coalesced_gestures() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  const QString resonance = QStringLiteral("prism.filter.resonance");
  controller.begin_gesture(resonance);
  require(controller.gesture_parameter() == resonance, "gesture is tracked");
  controller.set_normalised(resonance, 0.3);
  controller.set_normalised(resonance, 0.6);
  controller.end_gesture(resonance);
  const auto& s = adapter.submissions();
  require(s.size() == 4 && s[0].gesture == NativeSynthGesture::begin &&
              s[1].gesture == NativeSynthGesture::update &&
              s[2].gesture == NativeSynthGesture::update &&
              s[3].gesture == NativeSynthGesture::commit,
          "begin, updates and one commit");
  require(controller.gesture_parameter().isEmpty(), "gesture ends");

  controller.begin_gesture(resonance);
  controller.set_normalised(resonance, 0.9);
  controller.cancel_gesture(resonance);
  require(std::fabs(controller.value(resonance) - 0.6) < 1e-9,
          "a cancelled drag restores the committed value");
}

void presets_apply_and_cycle() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  const int acid = preset_index("Acid");
  controller.apply_preset(acid);
  const auto expected =
      synth::preset_values(table(), table().presets[static_cast<std::size_t>(acid)]).value();
  require(adapter.values() == expected, "preset values reach the project");
  require(controller.preset_name() == QStringLiteral("Acid") &&
              controller.preset_category() == QStringLiteral("Bass") &&
              !controller.preset_modified(),
          "the applied preset is named");
  controller.step_preset(1);
  require(controller.preset_index() == acid + 1, "next preset");
  controller.apply_preset(0);
  controller.step_preset(-1);
  require(controller.preset_index() == static_cast<int>(table().presets.size()) - 1,
          "previous wraps around");
  require(controller.presets().size() == 16 && controller.preset_categories().contains("Pad"),
          "presets and categories are listed");

  // A change from outside the panel, such as undo, is re-identified.
  InMemoryNativeSynthAdapter external(expected);
  NativeSynthDeviceController other(table(), external);
  require(other.preset_name() == QStringLiteral("Acid"), "loaded values are identified");
  auto custom = expected;
  custom[index("prism.filter.cutoff")] = 1'234.0;
  external.submit(custom, NativeSynthGesture::single);
  other.refresh();
  require(other.preset_index() == -1 && other.preset_name() == QStringLiteral("Custom"),
          "external edits become a custom patch");
}

void status_reports_deferral_and_failures() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.status_text() == QStringLiteral("Live"), "live by default");
  adapter.set_status({.saved_revision = 3, .audible_revision = 3, .deferred_changes = true});
  controller.refresh();
  require(controller.deferred() &&
              controller.status_text() == QStringLiteral("Applies to new notes"),
          "deferred next-note changes are explained");
  adapter.set_status({.saved_revision = 4, .audible_revision = 3});
  controller.refresh();
  require(controller.pending() && controller.status_text() == QStringLiteral("Updating"),
          "a saved but not yet audible revision is pending");
  adapter.set_status({.saved_revision = 4,
                      .audible_revision = 4,
                      .editable = false,
                      .message = "Track is frozen"});
  controller.refresh();
  require(!controller.editable() && controller.status_text() == QStringLiteral("Read only") &&
              controller.status_detail() == QStringLiteral("Track is frozen"),
          "read-only state carries its reason");
  const auto before = adapter.submissions().size();
  controller.set_normalised(QStringLiteral("prism.filter.cutoff"), 0.1);
  controller.apply_preset(0);
  require(adapter.submissions().size() == before, "read-only panels submit nothing");
}

void filter_response_matches_the_prototypes() {
  auto values = synth::default_values(table());
  values[index("prism.filter.resonance")] = 0.0;
  values[index("prism.filter.cutoff")] = 1'000.0;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  // 1 kHz sits at fraction log(50)/log(1000) of the 20 Hz..20 kHz axis.
  require(std::fabs(controller.fraction_of(1'000.0) - std::log(50.0) / std::log(1000.0)) < 1e-12,
          "log frequency axis");
  require(std::fabs(controller.frequency_at(controller.fraction_of(1'000.0)) - 1'000.0) < 1e-6,
          "axis round trip");
  // 1001 points put 1 kHz exactly on a sample: 1000 * fraction is integral? Use
  // the curve's own axis to pick the nearest point and its exact frequency.
  const int points = 1001;
  const auto response = controller.filter_response(points);
  int nearest = 0;
  for (int i = 0; i < points; ++i) {
    if (std::fabs(controller.frequency_at(i / 1000.0) - 1'000.0) <
        std::fabs(controller.frequency_at(nearest / 1000.0) - 1'000.0)) {
      nearest = i;
    }
  }
  const double w = controller.frequency_at(nearest / 1000.0) / 1'000.0;
  const double butterworth = -10.0 * std::log10(1.0 + std::pow(w, 8.0));
  require(std::fabs(response[nearest].toDouble() - butterworth) < 0.01,
          "LP24 at zero resonance is a 4th-order Butterworth");
  require(response.front().toDouble() > -0.01 && response.back().toDouble() < -100.0,
          "low-pass passes lows and cuts highs");
  controller.set_choice(QStringLiteral("prism.filter.mode"), 8);
  controller.set_value(QStringLiteral("prism.filter.resonance"), 1.0);
  const auto ladder = controller.filter_response(points);
  require(ladder[nearest].toDouble() > 10.0, "a full-resonance ladder peaks at cutoff");
}

// ---- QML ----------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller, bool light) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(light ? R"(
      import QtQuick
      import "qrc:/prism"
      PrismPanel {
        theme: PrismTheme {
          canvas: "#eef1f5"; panel: "#ffffff"; raised: "#f2f4f8"; well: "#f6f8fb"
          line: "#d5dbe4"; text: "#1b2230"; dim: "#5e6a7c"; faint: "#8c97a8"
          osc1: "#11a37f"; osc2: "#1f86d1"; mixer: "#6c5ce7"; filter: "#d9822b"
          amp: "#d6336c"; env2: "#c2459e"; lfo1: "#5c9e1f"; lfo2: "#b3961b"
          mod: "#7d5fd6"; voice: "#2a8fa8"; fx: "#c8623a"; live: "#11a37f"
          warning: "#b5761f"
        }
      }
    )"
                          : R"(
      import QtQuick
      import "qrc:/prism"
      PrismPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/PrismPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Prism panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Prism panel was not created: " + component.errorString().toStdString());
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

// Repeater delegates are visual children only, so walk the item tree.
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

void panel_loads_and_edits_through_the_controller() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller, false);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() > 700,
          "panel has its designed size");

  // Every table parameter has a control in the panel.
  for (const auto& p : table().parameters) {
    const QString id = QString::fromUtf8(p.id.data(), static_cast<qsizetype>(p.id.size()));
    bool shown = false;
    for (const char* kind : {"prismKnob:", "prismSegmented:", "prismDropdown:", "prismToggle:",
                             "prismAmount:", "prismWheel:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    const bool drawn_in_view = id.endsWith(QStringLiteral(".division"));
    require(shown || drawn_in_view, "no control for " + id.toStdString());
  }

  // Keyboard: arrows step, Delete resets, Tab reaches knobs.
  auto* cutoff = item(*panel, QStringLiteral("prismKnob:prism.filter.cutoff"));
  require(cutoff->activeFocusOnTab(), "knobs are keyboard reachable");
  cutoff->forceActiveFocus();
  const double before = controller.value(QStringLiteral("prism.filter.cutoff"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("prism.filter.cutoff")) > before, "arrow raises");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(QStringLiteral("prism.filter.cutoff")), "Delete resets");

  // Mouse drag on a knob is one gesture.
  const auto drag_count = adapter.submissions().size();
  const QPoint centre = cutoff->mapToScene(QPointF(cutoff->width() / 2, 24)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre);
  QTest::mouseMove(panel->window.get(), centre - QPoint(0, 30));
  QTest::mouseMove(panel->window.get(), centre - QPoint(0, 60));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre - QPoint(0, 60));
  const auto& s = adapter.submissions();
  require(s.size() > drag_count + 2 && s[drag_count].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a knob drag is begin, updates and commit");
  require(controller.value(QStringLiteral("prism.filter.cutoff")) > before,
          "dragging up raises the cutoff");

  // Segmented choice by click.
  auto* wave = item(*panel, QStringLiteral("prismSegmented:prism.osc1.wave"));
  const QPoint sine =
      wave->mapToScene(QPointF(wave->width() * 7 / 8, wave->height() / 2)).toPoint();
  QTest::mouseClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, sine);
  require(controller.choice(QStringLiteral("prism.osc1.wave")) == 3, "click picks sine");

  // Preset menu: open, then choose with the keyboard.
  auto* box = item(*panel, QStringLiteral("prismPresetBox"));
  auto* menu = item(*panel, QStringLiteral("prismMenu"));
  QTest::mouseClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                    box->mapToScene(QPointF(box->width() / 2, box->height() / 2)).toPoint());
  require(menu->isVisible(), "preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.preset_index() >= 0, "keyboard chooses a preset");
  require(item(*panel, QStringLiteral("prismPresetName"))->property("text").toString() ==
              controller.preset_name(),
          "the header shows the preset");

  // Deferred status reaches the header.
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("prismStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#e0ae75")),
          "deferred status is highlighted");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("PRISM_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  for (const bool light : {false, true}) {
    auto values = synth::preset_values(
                      table(), table().presets[static_cast<std::size_t>(preset_index("Supersaw"))])
                      .value();
    values[index("prism.mod1.source")] = 2;
    values[index("prism.mod1.target")] = 7;
    values[index("prism.mod1.amount")] = 0.35;
    values[index("prism.lfo1.vibrato")] = 12;
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller, light);
    QTest::qWait(50);
    const QString path =
        QString::fromLocal8Bit(directory) +
        (light ? QStringLiteral("/prism_light.png") : QStringLiteral("/prism_dark.png"));
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  // The patch a new track starts with.
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller, false);
  QTest::qWait(50);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/prism_default.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    model_is_driven_by_the_engine_table();
    edits_round_trip_through_the_adapter();
    drags_are_coalesced_gestures();
    presets_apply_and_cycle();
    status_reports_deferral_and_failures();
    filter_response_matches_the_prototypes();
    panel_loads_and_edits_through_the_controller();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "prism panel contract passed\n";
  return 0;
}
