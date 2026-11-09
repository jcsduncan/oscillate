#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/dx_envelope.hpp"
#include "oscillate/native_dsp/synth/fm.hpp"
#include "oscillate/native_dsp/synth/halo.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
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

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
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

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *synth::halo::engine().descriptor; }

std::size_t index(std::string_view id) {
  return oscillate::test::take(oscillate::native_dsp::find_parameter(table(), id));
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

void algorithm_layouts_follow_the_engine_routing() {
  const auto& data = synth::fm_algorithm_table();
  for (std::size_t a = 0; a < data.size(); ++a) {
    const auto layout = q::fm_algorithm_layout(a);
    const std::string name = "algorithm " + std::to_string(a + 1);
    require(layout.valid, name + " lays out");
    require(layout.modulates == data[a].modulates && layout.carriers == data[a].carriers &&
                layout.feedback == data[a].feedback,
            name + " draws the engine's routing");
    for (std::size_t op = 0; op < 6; ++op) {
      const bool heard = ((layout.carriers >> op) & 1U) != 0U;
      require(heard == (layout.row[op] == 0), name + ": carriers sit on the bottom row");
      for (std::size_t to = 0; to < op; ++to) {
        if (((layout.modulates[op] >> to) & 1U) != 0U) {
          require(layout.row[op] > layout.row[to], name + ": modulators sit above targets");
        }
      }
      for (std::size_t other = 0; other < op; ++other) {
        require(layout.row[op] != layout.row[other] ||
                    std::fabs(layout.column[op] - layout.column[other]) >= 0.75,
                name + ": operators never overlap");
      }
      require(layout.column[op] >= 0.0 && layout.column[op] + 1.0 <= layout.columns + 1e-9 &&
                  layout.row[op] < layout.rows,
              name + ": operators are inside the grid");
    }
  }
  const auto five = q::fm_algorithm_layout(4);
  require(five.carriers == 0b010101, "algorithm 5 hears operators 1, 3 and 5");
  require(five.row[1] == 1 && five.row[3] == 1 && five.row[5] == 1 && five.rows == 2,
          "algorithm 5 is three two-operator stacks");
  const auto one = q::fm_algorithm_layout(0);
  require(one.row[5] == 3 && one.rows == 4 && one.feedback == 5,
          "algorithm 1 stacks 6>5>4>3 with feedback on 6");
  require(q::fm_algorithm_layout(1).feedback == 1, "algorithm 2 feeds back on operator 2");
  const auto all = q::fm_algorithm_layout(31);
  require(std::popcount(all.carriers) == 6 && all.rows == 1 && all.columns == 6.0,
          "algorithm 32 is six carriers in a row");
  require(!q::fm_algorithm_layout(32).valid, "there are 32 algorithms");
}

void operator_envelopes_follow_the_kernel() {
  auto values = preset("Organ");
  const auto organ = q::halo_operator_envelope(table(), values, 0, 401);
  require(organ.points.size() == 401 && organ.points.front() == 0.0, "envelopes start at zero");
  using synth::DxEnvelope;
  const double release = DxEnvelope::segment_seconds(
      DxEnvelope::level_units(values[index("halo.op1.l3")]),
      DxEnvelope::level_units(values[index("halo.op1.l4")]), values[index("halo.op1.r4")], 0, 60);
  const double attack = DxEnvelope::segment_seconds(DxEnvelope::floor_units, DxEnvelope::full_units,
                                                    values[index("halo.op1.r1")], 0, 60);
  require(std::fabs(organ.seconds - organ.release_at - release) < 1e-12 &&
              organ.release_at > attack,
          "the sketch holds after its decays, then releases at R4");
  const auto held = static_cast<std::size_t>(organ.release_at / organ.seconds * 400.0) - 1;
  require(std::fabs(organ.points[held] - 1.0) < 1e-3, "Organ sustains at full level");
  require(organ.points.back() < 1e-3, "the release reaches the floor");
  // Decays are straight lines in decibels: equal steps down the log scale.
  values = preset("Init");
  const auto init = q::halo_operator_envelope(table(), values, 0, 401);
  const auto start = static_cast<std::size_t>(init.release_at / init.seconds * 400.0) + 2;
  const double first = init.points[start] - init.points[start + 10];
  const double second = init.points[start + 10] - init.points[start + 20];
  require(first > 0.0 && std::fabs(first - second) < 1e-3 * first,
          "a release draws as a straight line");

  values = preset("E-Piano");
  const auto piano = q::halo_operator_envelope(table(), values, 3, 200);
  double peak = 0.0;
  for (const double v : piano.points) {
    peak = std::max(peak, v);
  }
  require(peak > 0.99 && peak <= 1.0 + 1e-9, "the attack reaches full level");
  const auto at_release = static_cast<std::size_t>(piano.release_at / piano.seconds * 199.0) - 1;
  require(piano.points[at_release] < 1e-3, "the tine decays to its zero sustain");

  const auto& prism = *synth::prism::engine().descriptor;
  require(q::halo_operator_envelope(prism, oscillate::native_dsp::default_values(prism), 0, 64)
              .points.empty(),
          "only Halo has operator sketches");
  require(q::halo_operator_envelope(table(), values, 6, 64).points.empty(),
          "there are six operators");
}

void controller_exposes_halo_sketches() {
  auto values = oscillate::native_dsp::default_values(table());
  values[index("halo.filter.resonance")] = 0.0;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.has_filter_response(), "Halo draws its filter");
  for (const auto& db : controller.filter_response(16)) {
    require(db.toDouble() == 0.0, "filter Off is flat");
  }
  controller.set_choice(QStringLiteral("halo.filter.type"), 2);
  controller.set_value(QStringLiteral("halo.filter.cutoff"), 1'000.0);
  const double at_cutoff = controller.filter_response(3).at(1).toDouble(); // 20 Hz, ~632 Hz, 20 kHz
  require(at_cutoff > -3.0 && at_cutoff < 0.5, "LP 24 passes below its cutoff");
  require(controller.filter_response(3).back().toDouble() < -60.0, "LP 24 falls 24 dB/oct");

  const auto five = controller.fm_algorithm(4);
  const auto carriers = five.value(QStringLiteral("carriers")).toList();
  require(carriers.size() == 6 && carriers[0].toBool() && !carriers[1].toBool() &&
              carriers[2].toBool() && carriers[4].toBool() && !carriers[5].toBool(),
          "algorithm 5 reaches QML with carriers 1, 3, 5");
  require(five.value(QStringLiteral("edges")).toList().size() == 3 &&
              five.value(QStringLiteral("feedback")).toInt() == 5,
          "algorithm 5 has three edges and feedback on 6");
  require(controller.fm_algorithm(32).isEmpty() && controller.fm_algorithm(-1).isEmpty(),
          "algorithm indices are checked");
  const auto envelope = controller.operator_envelope(0, 64);
  require(envelope.value(QStringLiteral("points")).toList().size() == 64 &&
              envelope.value(QStringLiteral("seconds")).toDouble() > 0.0,
          "operator envelopes reach QML");

  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter prism_adapter(oscillate::native_dsp::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, prism_adapter);
  require(prism_controller.fm_algorithm(0).isEmpty() &&
              prism_controller.operator_envelope(0, 64).isEmpty(),
          "other engines have no FM sketches");
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
      import "qrc:/halo"
      HaloPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/HaloPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Halo panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Halo panel was not created: " + component.errorString().toStdString());
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
  require(find_item(panel->root, QStringLiteral("haloAlgorithmGrid")) != nullptr,
          "the algorithm has its membrane keys");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    if (id == QStringLiteral("halo.algorithm")) {
      continue;
    }
    bool shown = false;
    for (const char* kind : {"haloKnob:", "haloSegment:", "haloStepper:", "haloDropdown:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
}

void algorithm_keys_drive_the_diagram_and_roles() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);

  // E-Piano is algorithm 5: three carrier/modulator pairs, feedback on 6.
  require(text_of(*panel, QStringLiteral("haloAlgorithmNumber")) == QStringLiteral("ALG 05"),
          "the algorithm plate shows the algorithm");
  for (int op = 1; op <= 6; ++op) {
    const QString role =
        item(*panel, QStringLiteral("haloOperatorRole:%1").arg(op))->property("role").toString();
    require(role == (op % 2 == 1 ? QStringLiteral("CARRIER") : QStringLiteral("MOD")),
            "odd operators are carriers in algorithm 5");
  }
  require(item(*panel, QStringLiteral("haloOperator:6"))->property("feedback").toBool() &&
              !item(*panel, QStringLiteral("haloOperator:2"))->property("feedback").toBool(),
          "operator 6 carries the feedback");

  click(*panel, centre(item(*panel, QStringLiteral("haloAlgorithm:32"))));
  require(controller.choice(QStringLiteral("halo.algorithm")) == 31, "a key picks algorithm 32");
  for (int op = 1; op <= 6; ++op) {
    require(item(*panel, QStringLiteral("haloOperatorRole:%1").arg(op))->property("role") ==
                QStringLiteral("CARRIER"),
            "algorithm 32 makes every operator a carrier");
  }
  const auto layout = item(*panel, QStringLiteral("haloAlgorithmView"))->property("layout").toMap();
  require(layout.value(QStringLiteral("rows")).toInt() == 1, "the diagram redraws");
  require(text_of(*panel, QStringLiteral("haloReadout")).contains(QStringLiteral("ALGORITHM")),
          "the LCD reads back the algorithm");

  // Keyboard on the grid: arrows walk, Up/Down jump a row, Delete resets.
  auto* grid = item(*panel, QStringLiteral("haloAlgorithmGrid"));
  require(grid->activeFocusOnTab() && grid->hasActiveFocus(), "the grid takes focus");
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(controller.choice(QStringLiteral("halo.algorithm")) == 30, "Left steps down");
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.choice(QStringLiteral("halo.algorithm")) == 22, "Up jumps a row");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("halo.algorithm")) == 4, "Delete restores 5");
}

void controls_edit_with_gestures_and_keys() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);

  // A knob drag is one gesture, and the LCD reads it back.
  auto* level = item(*panel, QStringLiteral("haloKnob:halo.op2.level"));
  auto start = adapter.submissions().size();
  const double before = controller.value(QStringLiteral("halo.op2.level"));
  const QPoint knob = level->mapToScene(QPointF(level->width() / 2, 20)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 15));
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 30));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob - QPoint(0, 30));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a knob drag is begin, updates and commit");
  require(controller.value(QStringLiteral("halo.op2.level")) > before, "dragging up raises level");
  require(text_of(*panel, QStringLiteral("haloReadout")) == QStringLiteral("OP 2 · LEVEL") &&
              text_of(*panel, QStringLiteral("haloReadoutValue")) ==
                  controller.display(QStringLiteral("halo.op2.level")),
          "the LCD names the last control touched and its value");

  // Shift drags finely.
  const double coarse = controller.normalised(QStringLiteral("halo.op2.level"));
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob);
  const QPointF moved = knob - QPoint(0, 30);
  QMouseEvent shift_move(QEvent::MouseMove, moved, moved, panel->window->mapToGlobal(moved),
                         Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
  QCoreApplication::sendEvent(panel->window.get(), &shift_move);
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob - QPoint(0, 30));
  const double fine = controller.normalised(QStringLiteral("halo.op2.level")) - coarse;
  require(fine > 0.0 && fine < 30.0 / 1'000.0, "Shift drags finely");

  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  require(controller.is_default(QStringLiteral("halo.op2.level")), "double-click resets");
  wheel(*panel, knob, 120);
  require(controller.value(QStringLiteral("halo.op2.level")) > before, "the wheel steps up");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(QStringLiteral("halo.op2.level")), "Delete resets");

  // Waveform keys: the third segment is Abs.
  auto* wave = item(*panel, QStringLiteral("haloSegment:halo.op3.wave"));
  click(*panel, at(wave, 0.62, 0.5));
  require(controller.choice(QStringLiteral("halo.op3.wave")) == 2, "a wave key picks Abs");
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("halo.op3.wave")) == 3, "Right steps the wave");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("halo.op3.wave")) == 0, "Delete restores sine");

  // The ratio stepper and the frequency readout.
  auto* ratio = item(*panel, QStringLiteral("haloStepper:halo.op4.ratio"));
  require(text_of(*panel, QStringLiteral("haloFrequency:4")) == QStringLiteral("×14.00"),
          "the tine runs at 14x");
  click(*panel, centre(find_item(ratio, QStringLiteral("haloStepperUp"))));
  require(controller.choice(QStringLiteral("halo.op4.ratio")) == 15, "the up arrow raises ratio");
  require(text_of(*panel, QStringLiteral("haloFrequency:4")) == QStringLiteral("×15.00"),
          "the readout follows the ratio");
  wheel(*panel, centre(ratio), -120);
  require(controller.choice(QStringLiteral("halo.op4.ratio")) == 14, "the wheel steps the ratio");
  controller.set_value(QStringLiteral("halo.op4.fine"), 0.5);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("haloFrequency:4")) == QStringLiteral("×21.00"),
          "fine multiplies the ratio");

  // Fixed mode swaps which tuning knob is live.
  auto* mode = item(*panel, QStringLiteral("haloSegment:halo.op4.mode"));
  click(*panel, at(mode, 0.75, 0.5));
  require(controller.choice(QStringLiteral("halo.op4.mode")) == 1, "the mode key picks Fixed");
  require(text_of(*panel, QStringLiteral("haloFrequency:4")).contains(QStringLiteral("Hz")),
          "fixed operators read in Hz");
  require(
      item(*panel, QStringLiteral("haloKnob:halo.op4.fine"))->property("inactive").toBool() &&
          !item(*panel, QStringLiteral("haloKnob:halo.op4.fixed"))->property("inactive").toBool(),
      "fixed mode dims Fine and lights Fixed");
  require(!item(*panel, QStringLiteral("haloKnob:halo.op4.fine"))
               ->property("inactiveReason")
               .toString()
               .isEmpty(),
          "an inactive knob says why");

  // A dropdown opens the shared menu and the keyboard chooses.
  // An Off filter collapses to its title bar and an OFF chip.
  require(controller.choice(QStringLiteral("halo.filter.type")) == 0 &&
              item(*panel, QStringLiteral("haloFilterOff"))->isVisible() &&
              !item(*panel, QStringLiteral("haloKnob:halo.filter.cutoff"))->isVisible(),
          "an Off filter collapses");
  auto* type = item(*panel, QStringLiteral("haloDropdown:halo.filter.type"));
  auto* menu = item(*panel, QStringLiteral("haloMenu"));
  click(*panel, centre(type));
  require(menu->isVisible(), "the filter menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.choice(QStringLiteral("halo.filter.type")) == 2,
          "the keyboard picks LP 24");
  require(item(*panel, QStringLiteral("haloFilterView"))->property("active").toBool(),
          "the filter view wakes up");
  require(item(*panel, QStringLiteral("haloKnob:halo.filter.cutoff"))->isVisible() &&
              !item(*panel, QStringLiteral("haloFilterOff"))->isVisible(),
          "the filter's controls return with the filter on");

  // Unison stepper keys.
  auto* unison = item(*panel, QStringLiteral("haloStepper:halo.voice.unison"));
  unison->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("halo.voice.unison")) == 2.0, "Up adds a unison voice");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.value(QStringLiteral("halo.voice.unison")) == 1.0, "Delete resets unison");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("haloPresetName")) == QStringLiteral("E-Piano"),
          "defaults are the E-Piano");
  click(*panel, centre(item(*panel, QStringLiteral("haloPresetNext"))));
  require(controller.preset_name() == QStringLiteral("Bright EP"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("haloPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("E-Piano"), "previous steps back");

  auto* display = item(*panel, QStringLiteral("haloPresetDisplay"));
  auto* menu = item(*panel, QStringLiteral("haloMenu"));
  click(*panel, centre(display));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");

  controller.apply_preset(preset_index("Organ"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("haloAlgorithmNumber")) == QStringLiteral("ALG 32"),
          "presets set the algorithm");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("haloStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#e0901f")),
          "deferred status is highlighted");
}

// Algorithms 4 and 6 close loops across operators: the layout reports the
// loop's source, the diagram and caption show it, and the cards at both
// ends are badged. Self-feedback algorithms keep the plain badge.
void feedback_loops_are_drawn_and_badged() {
  require(q::fm_algorithm_layout(3).feedback == 5 && q::fm_algorithm_layout(3).feedback_from == 3 &&
              q::fm_algorithm_layout(5).feedback_from == 4 &&
              q::fm_algorithm_layout(4).feedback_from == q::fm_algorithm_layout(4).feedback,
          "the layout reports each loop's source");
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.fm_algorithm(3).value(QStringLiteral("feedback_from")).toInt() == 3,
          "the controller reports the loop's source");
  auto panel = load_panel(controller);
  auto* view = item(*panel, QStringLiteral("haloAlgorithmView"));
  const auto badge = [&](int op) {
    auto* b = item(*panel, QStringLiteral("haloFeedbackBadge:%1").arg(op));
    return b->isVisible() ? b->childItems().front()->property("text").toString() : QString();
  };
  require(!view->property("loop").toBool() &&
              text_of(*panel, QStringLiteral("haloFeedbackCaption")) ==
                  QStringLiteral("FEEDBACK ON OP 6") &&
              badge(6) == QStringLiteral("FB") && badge(4).isEmpty(),
          "algorithm 5 feeds op 6 back on itself");
  controller.set_choice(QStringLiteral("halo.algorithm"), 3);
  QCoreApplication::processEvents();
  require(view->property("loop").toBool() &&
              text_of(*panel, QStringLiteral("haloFeedbackCaption")) ==
                  QStringLiteral("FEEDBACK 4 → 6") &&
              badge(6) == QStringLiteral("FB ← 4") && badge(4) == QStringLiteral("FB → 6") &&
              badge(5).isEmpty(),
          "algorithm 4 loops op 4 into op 6");
  controller.set_choice(QStringLiteral("halo.algorithm"), 5);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("haloFeedbackCaption")) ==
                  QStringLiteral("FEEDBACK 5 → 6") &&
              badge(5) == QStringLiteral("FB → 6") && badge(4).isEmpty(),
          "algorithm 6 loops op 5 into op 6");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("HALO_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"E-Piano", "halo.op4.level", "/halo_e_piano.png"},
                          Shot{"Bells", "halo.op2.fine", "/halo_bells.png"},
                          Shot{"Brass", "halo.algorithm", "/halo_brass.png"},
                          Shot{"Metallic Lead", "halo.filter.cutoff", "/halo_metallic_lead.png"},
                          Shot{"Glass Pad", "halo.fx.delay", "/halo_glass_pad.png"}}) {
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
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  controller.set_choice(QStringLiteral("halo.algorithm"), 3);
  auto panel = load_panel(controller);
  QTest::qWait(150);
  const QString path =
      QString::fromLocal8Bit(directory) + QStringLiteral("/halo_algorithm_4_loop.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    algorithm_layouts_follow_the_engine_routing();
    operator_envelopes_follow_the_kernel();
    controller_exposes_halo_sketches();
    panel_covers_every_parameter();
    algorithm_keys_drive_the_diagram_and_roles();
    feedback_loops_are_drawn_and_badged();
    controls_edit_with_gestures_and_keys();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "halo panel contract passed\n";
  return 0;
}
