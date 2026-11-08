#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/bass_dsp.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace q = oscillate::quick;
using Complex = std::complex<double>;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() {
  return *synth::torque::engine().descriptor;
}

std::size_t index(std::string_view id) {
  return *oscillate::native_dsp::find_parameter(table(), id);
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
  return oscillate::native_dsp::preset_values(
             table(), table().presets[static_cast<std::size_t>(preset_index(name))])
      .value();
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// ---- Visuals ------------------------------------------------------------------

// Independent oracle: ADR-0032's linear diode ladder, normalised to wc = 1,
// solved by general Gaussian elimination of (sI - A) y = e1.
Complex diode_open_loop(Complex s) {
  const std::array<std::array<double, 4>, 4> a{{
      {-1.0, 1.0, 0.0, 0.0},
      {0.5, -1.0, 0.5, 0.0},
      {0.0, 0.5, -1.0, 0.5},
      {0.0, 0.0, 0.5, -1.0},
  }};
  std::array<std::array<Complex, 5>, 4> m{};
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      m[r][c] = (r == c ? s : Complex{}) - a[r][c];
    }
    m[r][4] = r == 0 ? 1.0 : 0.0;
  }
  for (std::size_t p = 0; p < 4; ++p) {
    for (std::size_t r = p + 1; r < 4; ++r) {
      const Complex f = m[r][p] / m[p][p];
      for (std::size_t c = p; c < 5; ++c) {
        m[r][c] -= f * m[p][c];
      }
    }
  }
  std::array<Complex, 4> y{};
  for (std::size_t r = 4; r-- > 0;) {
    Complex sum = m[r][4];
    for (std::size_t c = r + 1; c < 4; ++c) {
      sum -= m[r][c] * y[c];
    }
    y[r] = sum / m[r][r];
  }
  return y[3];
}

double response_db(std::vector<double> values, double hz) {
  return q::visuals_for("torque")->filter_response_db(table(), values, hz);
}

void diode_curve_matches_the_linear_model() {
  auto values = oscillate::native_dsp::default_values(table());
  require(values[index("torque.model")] == 0.0, "Acid is the default model");
  for (double cutoff : {200.0, 800.0, 3'000.0}) {
    for (double resonance : {0.0, 0.5, 0.9}) {
      values[index("torque.cutoff")] = cutoff;
      values[index("torque.resonance")] = resonance;
      // Independent constants: k = 1.02 * 17 * r, makeup 1 + 0.3 k, and the
      // knob is the resonant peak, wc = cutoff * sqrt 2.
      const double k = 1.02 * 17.0 * resonance;
      const double makeup = 1.0 + 0.3 * k;
      const double wc = cutoff * std::numbers::sqrt2;
      for (double hz : {20.0, 90.0, 400.0, 1'500.0, 6'000.0, 18'000.0}) {
        const Complex open = diode_open_loop({0.0, hz / wc});
        // The display floors at -120 dB.
        const double expected =
            20.0 * std::log10(std::max(std::abs(makeup * open / (1.0 + k * open)), 1e-6));
        require(std::fabs(response_db(values, hz) - expected) < 1e-6,
                "diode curve follows the ADR model at " + std::to_string(hz) + " Hz, cutoff " +
                    std::to_string(cutoff) + ", resonance " + std::to_string(resonance) + ": " +
                    std::to_string(response_db(values, hz)) + " vs " + std::to_string(expected));
      }
      // DC: the loop gain is k, so the passband sits at makeup / (1 + k).
      require(std::fabs(response_db(values, 0.01) - 20.0 * std::log10(makeup / (1.0 + k))) < 1e-4,
              "diode passband is makeup / (1 + k)");
    }
  }
  // At high resonance the peak sits at the knob's cutoff.
  values[index("torque.cutoff")] = 1'000.0;
  values[index("torque.resonance")] = 0.95;
  double best_hz = 0.0;
  double best = -1e9;
  for (double hz = 200.0; hz < 5'000.0; hz *= 1.002) {
    if (const double db = response_db(values, hz); db > best) {
      best = db;
      best_hz = hz;
    }
  }
  require(std::fabs(std::log2(best_hz / 1'000.0)) < 0.05 && best > response_db(values, 20.0) + 12.0,
          "high resonance peaks at the cutoff");
}

void other_models_draw_their_filters() {
  auto values = oscillate::native_dsp::default_values(table());
  values[index("torque.cutoff")] = 1'000.0;
  values[index("torque.resonance")] = 0.0;
  values[index("torque.model")] = 3; // Sub: LP 12 at half resonance.
  require(std::fabs(response_db(values, 1'000.0) + 3.0103) < 0.01, "Sub LP 12 is -3 dB at cutoff");
  require(response_db(values, 10'000.0) < -38.0, "Sub LP 12 falls 12 dB/oct");
  values[index("torque.model")] = 1; // Moog ladder: passband 0 dB at r = 0.
  require(std::fabs(response_db(values, 20.0)) < 0.01 && response_db(values, 10'000.0) < -75.0,
          "Moog ladder is a 24 dB low-pass");
  values[index("torque.model")] = 2; // Reese: comb notches at multiples of C2.
  values[index("torque.cutoff")] = 12'000.0;
  values[index("torque.reese.comb")] = 0.0;
  const double plain_notch = response_db(values, 2.0 * q::torque_comb_reference_hz);
  const double plain_peak = response_db(values, 2.5 * q::torque_comb_reference_hz);
  values[index("torque.reese.comb")] = 1.0;
  const double notch = response_db(values, 2.0 * q::torque_comb_reference_hz);
  const double peak = response_db(values, 2.5 * q::torque_comb_reference_hz);
  // g = -0.75: notch gain 0.25 / 1.75 at multiples of the note, peak gain
  // 0.25 / 0.25 = 1 between them.
  require(std::fabs(notch - plain_notch - 20.0 * std::log10(0.25 / 1.75)) < 0.01 &&
              std::fabs(peak - plain_peak) < 0.01,
          "the Reese comb notches the note's harmonics");
  values[index("torque.reese.filter")] = 1;
  values[index("torque.reese.comb")] = 0.0;
  values[index("torque.cutoff")] = 1'000.0;
  // The SVF band output peaks at Q, 1/sqrt 2 with no resonance.
  require(std::fabs(response_db(values, 1'000.0) + 3.0103) < 0.01 &&
              response_db(values, 50.0) < -25.0,
          "Reese BP 12 peaks at its centre");
}

void oscillator_and_drop_sketches_follow_the_kernel() {
  auto values = oscillate::native_dsp::default_values(table());
  const auto saw = q::torque_oscillator_cycle(table(), values, 100);
  require(saw.size() == 100 && std::fabs(saw.front() + 1.0) < 1e-12, "the acid saw starts at -1");
  for (std::size_t i = 1; i < saw.size(); ++i) {
    require(saw[i] > saw[i - 1], "the acid saw rises through the cycle");
  }
  values[index("torque.acid.wave")] = 1;
  values[index("torque.acid.tilt")] = 0.8;
  const auto square = q::torque_oscillator_cycle(table(), values, 100);
  require(square[1] > square[45] && square[45] > 0.0 && square[51] < 0.0 && square[51] < square[95],
          "the acid square's tops sag towards zero");
  values[index("torque.model")] = 3;
  values[index("torque.sub.layer")] = 0;
  const auto sine = q::torque_oscillator_cycle(table(), values, 64);
  for (std::size_t i = 0; i < sine.size(); ++i) {
    require(std::fabs(sine[i] - std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / 64.0)) <
                1e-12,
            "Sub without a layer is a pure sine");
  }
  values[index("torque.model")] = 2;
  const auto stack = q::torque_oscillator_cycle(table(), values, 64);
  double peak = 0.0;
  for (const double v : stack) {
    peak = std::max(peak, std::fabs(v));
  }
  require(stack.size() == 64 && std::fabs(peak - 1.0) < 1e-12, "sketches peak at 1");

  values[index("torque.sub.drop")] = 12.0;
  values[index("torque.sub.drop_time")] = 0.1;
  const auto drop = q::torque_pitch_drop(table(), values, 0.1, 2);
  require(drop.size() == 2 && std::fabs(drop[0] - 12.0) < 1e-12 &&
              std::fabs(drop[1] - 12.0 * std::log2(1.0 + std::exp(-1.0))) < 1e-12,
          "the drop falls in Hz with its time constant");

  const auto& prism = *synth::prism::engine().descriptor;
  const auto prism_values = oscillate::native_dsp::default_values(prism);
  require(q::torque_oscillator_cycle(prism, prism_values, 64).empty() &&
              q::torque_pitch_drop(prism, prism_values, 1.0, 64).empty(),
          "only Torque has bass sketches");
}

void controller_exposes_torque_sketches() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.has_filter_response(), "Torque draws its filter");
  require(controller.filter_response(32).size() == 32, "the response reaches QML");
  require(controller.oscillator_cycle(48).size() == 48 &&
              controller.pitch_drop(0.5, 20).size() == 20,
          "the oscillator and drop sketches reach QML");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter prism_adapter(oscillate::native_dsp::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, prism_adapter);
  require(prism_controller.oscillator_cycle(48).isEmpty() &&
              prism_controller.pitch_drop(0.5, 20).isEmpty(),
          "other engines have no bass sketches");
}

// ---- QML ----------------------------------------------------------------------

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
      import "qrc:/torque"
      TorquePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/TorquePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Torque panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Torque panel was not created: " + component.errorString().toStdString());
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

QPoint at(QQuickItem* target, double fx, double fy) {
  return target->mapToScene(QPointF(target->width() * fx, target->height() * fy)).toPoint();
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

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1240 && panel->root->implicitHeight() >= 740,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"torqueKnob:", "torqueSegment:", "torqueStepper:", "torqueDropdown:",
                             "torqueTabs:", "torqueStepControl:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
}

void model_tabs_switch_colour_labels_and_extras() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* theme = panel->root->property("theme").value<QObject*>();
  require(theme->property("accent").value<QColor>() == QColor(QStringLiteral("#a6ff3d")),
          "Acid glows acid green");
  require(item(*panel, QStringLiteral("torqueModelRow:0"))->isVisible() &&
              !item(*panel, QStringLiteral("torqueModelRow:2"))->isVisible(),
          "Acid shows its own extras");

  click(*panel, centre(item(*panel, QStringLiteral("torqueModel:2"))));
  require(controller.choice(QStringLiteral("torque.model")) == 2, "a tab picks Reese");
  require(theme->property("accent").value<QColor>() == QColor(QStringLiteral("#a97bff")),
          "Reese glows violet");
  require(item(*panel, QStringLiteral("torqueModelRow:2"))->isVisible() &&
              !item(*panel, QStringLiteral("torqueModelRow:0"))->isVisible(),
          "the extras follow the model");
  require(text_of(*panel, QStringLiteral("torqueReadout")).contains(QStringLiteral("MODEL")),
          "the display reads back the model");

  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("torque.model")) == 3, "Right steps to Sub");
  require(
      item(*panel, QStringLiteral("torqueKnob:torque.cutoff"))->property("label").toString() ==
              QStringLiteral("Tone") &&
          item(*panel, QStringLiteral("torqueKnob:torque.drive"))->property("label").toString() ==
              QStringLiteral("Saturate"),
      "Sub relabels its main controls");
  require(item(*panel, QStringLiteral("torqueDropReadout"))->isVisible(), "Sub shows its drop");
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(controller.choice(QStringLiteral("torque.model")) == 1 &&
              item(*panel, QStringLiteral("torqueKnob:torque.resonance"))
                      ->property("label")
                      .toString() == QStringLiteral("Emphasis"),
          "Moog calls resonance emphasis");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("torque.model")) == 0, "Delete restores Acid");

  // Acid's sag only matters for the square.
  require(
      item(*panel, QStringLiteral("torqueKnob:torque.acid.tilt"))->property("inactive").toBool(),
      "sag is dimmed for the saw");
  click(*panel, at(item(*panel, QStringLiteral("torqueSegment:torque.acid.wave")), 0.75, 0.5));
  require(controller.choice(QStringLiteral("torque.acid.wave")) == 1 &&
              !item(*panel, QStringLiteral("torqueKnob:torque.acid.tilt"))
                   ->property("inactive")
                   .toBool(),
          "the square lights the sag");
}

void steps_edit_by_mouse_and_keyboard() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto value = [&](const char* id) { return controller.value(QString::fromLatin1(id)); };

  // Step 2: C, gate on. The gate cycles on -> tie -> rest -> on.
  auto* gate = item(*panel, QStringLiteral("torqueStepControl:torque.seq.s2.gate"));
  click(*panel, centre(gate));
  require(value("torque.seq.s2.gate") == 2.0, "the gate key cycles to tie");
  click(*panel, centre(gate));
  require(value("torque.seq.s2.gate") == 0.0, "then to rest");
  click(*panel, centre(gate));
  require(value("torque.seq.s2.gate") == 1.0, "then back on");

  // The cell has focus: keys edit it.
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(value("torque.seq.s2.note") == 2.0, "Up raises the note");
  QTest::keyClick(panel->window.get(), Qt::Key_Up, Qt::ShiftModifier);
  require(value("torque.seq.s2.octave") == 1.0, "Shift+Up raises the octave");
  QTest::keyClick(panel->window.get(), Qt::Key_A);
  QTest::keyClick(panel->window.get(), Qt::Key_S);
  require(value("torque.seq.s2.accent") == 1.0 && value("torque.seq.s2.slide") == 1.0,
          "A and S toggle accent and slide");
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(value("torque.seq.s2.gate") == 2.0, "Return cycles the gate");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(value("torque.seq.s2.note") == 0.0 && value("torque.seq.s2.octave") == 0.0 &&
              value("torque.seq.s2.accent") == 0.0 && value("torque.seq.s2.slide") == 0.0 &&
              value("torque.seq.s2.gate") == 1.0,
          "Delete resets the whole step");
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(item(*panel, QStringLiteral("torqueStep:3"))->hasActiveFocus(), "Right moves to step 3");

  // Mouse: wheel and drag on the note, octave keys, accent and slide.
  auto* note = item(*panel, QStringLiteral("torqueStepControl:torque.seq.s4.note"));
  wheel(*panel, centre(note), 120);
  require(value("torque.seq.s4.note") == 1.0, "the wheel raises the note");
  const auto start = adapter.submissions().size();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre(note));
  QTest::mouseMove(panel->window.get(), centre(note) - QPoint(0, 20));
  QTest::mouseMove(panel->window.get(), centre(note) - QPoint(0, 40));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                      centre(note) - QPoint(0, 40));
  require(value("torque.seq.s4.note") > 1.0, "dragging up raises the note");
  require(adapter.submissions()[start].gesture == NativeSynthGesture::begin &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "a note drag is one gesture");
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre(note));
  require(value("torque.seq.s4.note") == 0.0, "double-click resets the note");
  click(*panel,
        at(item(*panel, QStringLiteral("torqueStepControl:torque.seq.s4.octave")), 0.1, 0.5));
  require(value("torque.seq.s4.octave") == -1.0, "the minus key drops an octave");
  click(*panel, centre(item(*panel, QStringLiteral("torqueStepControl:torque.seq.s4.accent"))));
  click(*panel, centre(item(*panel, QStringLiteral("torqueStepControl:torque.seq.s4.slide"))));
  require(value("torque.seq.s4.accent") == 1.0 && value("torque.seq.s4.slide") == 1.0,
          "accent and slide toggle");

  // Steps past the length are dimmed, not removed.
  controller.set_value(QStringLiteral("torque.seq.length"), 4);
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("torqueStep:4"))->property("inPattern").toBool() &&
              !item(*panel, QStringLiteral("torqueStep:5"))->property("inPattern").toBool(),
          "steps past the length are out of the pattern");
  auto* on = item(*panel, QStringLiteral("torqueSegment:torque.seq.on"));
  click(*panel, at(on, 0.75, 0.5));
  require(controller.choice(QStringLiteral("torque.seq.on")) == 1 &&
              panel->root->property("sequencerOn").toBool(),
          "the sequencer switches on");
}

void controls_edit_with_gestures_and_keys() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);

  auto* cutoff = item(*panel, QStringLiteral("torqueKnob:torque.cutoff"));
  const auto start = adapter.submissions().size();
  const double before = controller.value(QStringLiteral("torque.cutoff"));
  const QPoint knob = cutoff->mapToScene(QPointF(cutoff->width() / 2, 25)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 15));
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 30));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob - QPoint(0, 30));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a knob drag is begin, updates and commit");
  require(controller.value(QStringLiteral("torque.cutoff")) > before,
          "dragging up opens the filter");
  require(text_of(*panel, QStringLiteral("torqueReadout")) == QStringLiteral("FILTER · CUTOFF") &&
              text_of(*panel, QStringLiteral("torqueReadoutValue")) ==
                  controller.display(QStringLiteral("torque.cutoff")) &&
              text_of(*panel, QStringLiteral("torqueCutoffReadout")) ==
                  controller.display(QStringLiteral("torque.cutoff")),
          "the display reads back the cutoff");

  const double coarse = controller.normalised(QStringLiteral("torque.cutoff"));
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob);
  const QPointF moved = knob - QPoint(0, 30);
  QMouseEvent shift_move(QEvent::MouseMove, moved, moved, panel->window->mapToGlobal(moved),
                         Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
  QCoreApplication::sendEvent(panel->window.get(), &shift_move);
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob - QPoint(0, 30));
  const double fine = controller.normalised(QStringLiteral("torque.cutoff")) - coarse;
  require(fine > 0.0 && fine < 30.0 / 1'000.0, "Shift drags finely");

  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  require(controller.is_default(QStringLiteral("torque.cutoff")), "double-click resets");
  wheel(*panel, knob, 120);
  require(controller.value(QStringLiteral("torque.cutoff")) > before, "the wheel steps up");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(QStringLiteral("torque.cutoff")), "Delete resets");

  // A dropdown opens the shared menu and the keyboard chooses.
  auto* shape = item(*panel, QStringLiteral("torqueDropdown:torque.lfo.shape"));
  auto* menu = item(*panel, QStringLiteral("torqueMenu"));
  click(*panel, centre(shape));
  require(menu->isVisible(), "the LFO menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.choice(QStringLiteral("torque.lfo.shape")) == 2,
          "the keyboard picks Saw");

  // Steppers: arrows, keys and Delete.
  auto* tune = item(*panel, QStringLiteral("torqueStepper:torque.tune"));
  click(*panel, centre(find_item(tune, QStringLiteral("torqueStepperUp"))));
  require(controller.value(QStringLiteral("torque.tune")) == 1.0, "the up arrow tunes up");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(controller.value(QStringLiteral("torque.tune")) == -1.0, "Down tunes down");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.value(QStringLiteral("torque.tune")) == 0.0, "Delete resets tune");

  // The distortion tone dims while the mix is dry.
  controller.set_value(QStringLiteral("torque.fx.dist_mix"), 0.0);
  QCoreApplication::processEvents();
  require(
      item(*panel, QStringLiteral("torqueKnob:torque.fx.dist_tone"))->property("inactive").toBool(),
      "a dry distortion dims its tone");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("torquePresetName")) == QStringLiteral("Acid Classic"),
          "defaults are Acid Classic");
  click(*panel, centre(item(*panel, QStringLiteral("torquePresetNext"))));
  require(controller.preset_name() == QStringLiteral("Acid Squelch"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("torquePresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Acid Classic"), "previous steps back");

  auto* display = item(*panel, QStringLiteral("torquePresetDisplay"));
  auto* menu = item(*panel, QStringLiteral("torqueMenu"));
  click(*panel, centre(display));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");

  controller.apply_preset(preset_index("Neuro Growl"));
  QCoreApplication::processEvents();
  require(panel->root->property("model").toInt() == 2, "presets set the model");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("torqueStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#e0901f")),
          "deferred status is highlighted");
}

std::vector<float> torque_frame(int step, bool gate) {
  namespace t = synth::torque::telemetry;
  std::vector<float> frame(t::size);
  frame[t::running] = 1.0F;
  frame[t::step] = static_cast<float>(step);
  frame[t::key] = 45.0F;
  frame[t::cutoff] = 900.0F;
  frame[t::gate] = gate ? 1.0F : 0.0F;
  return frame;
}

// The sequencer strip follows the running sequencer (ADR-0037): the playing
// step lights, and with no telemetry the strip is the plain editor.
void playhead_follows_live_telemetry() {
  InMemoryNativeSynthAdapter adapter(preset("Seq Acid Line"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto playing = [&](int step) {
    return item(*panel, QStringLiteral("torqueStep:%1").arg(step))->property("playing").toBool();
  };
  QTest::qWait(300);
  for (int step = 1; step <= 16; ++step) {
    require(!playing(step), "no telemetry: no playhead");
  }
  adapter.set_telemetry(torque_frame(4, true));
  require(QTest::qWaitFor([&] { return playing(5); }, 1000), "the playing step lights");
  require(item(*panel, QStringLiteral("torquePlayhead:5"))->isVisible() && !playing(4) &&
              panel->root->property("liveGate").toBool(),
          "only the playing step lights, with its gate");
  adapter.set_telemetry(torque_frame(9, false));
  require(QTest::qWaitFor([&] { return playing(10) && !playing(5); }, 1000),
          "the playhead moves with the sequencer");
  auto stopped = torque_frame(9, false);
  stopped[synth::torque::telemetry::running] = 0.0F;
  adapter.set_telemetry(stopped);
  require(QTest::qWaitFor([&] { return !playing(10); }, 1000),
          "a stopped sequencer shows no playhead");
  adapter.set_telemetry({});
  QTest::qWait(300);
  require(!panel->root->property("liveRunning").toBool(), "telemetry gone: static view");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("TORQUE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"Acid Classic", "torque.cutoff", "/torque_acid_classic.png"},
                          Shot{"Seq Acid Line", "torque.seq.s3.slide", "/torque_seq_acid_line.png"},
                          Shot{"Moog Bass", "torque.resonance", "/torque_moog_bass.png"},
                          Shot{"Neuro Growl", "torque.reese.comb", "/torque_neuro_growl.png"},
                          Shot{"Distorted 808", "torque.sub.drop", "/torque_distorted_808.png"}}) {
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
  InMemoryNativeSynthAdapter adapter(preset("Seq Acid Line"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(torque_frame(5, true));
  QTest::qWait(400);
  const QString path =
      QString::fromLocal8Bit(directory) + QStringLiteral("/torque_live_playhead.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    diode_curve_matches_the_linear_model();
    other_models_draw_their_filters();
    oscillator_and_drop_sketches_follow_the_kernel();
    controller_exposes_torque_sketches();
    panel_covers_every_parameter();
    model_tabs_switch_colour_labels_and_extras();
    steps_edit_by_mouse_and_keyboard();
    controls_edit_with_gestures_and_keys();
    presets_and_status();
    playhead_follows_live_telemetry();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "torque panel contract passed\n";
  return 0;
}
