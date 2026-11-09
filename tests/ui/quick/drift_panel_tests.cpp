#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/drift_engine.hpp"
#include "oscillate/native_dsp/synth/grain_dsp.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJSValue>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
constexpr double pi = std::numbers::pi;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() {
  return *synth::drift::engine().descriptor;
}

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

double response_db(std::vector<double> values, double hz) {
  return q::visuals_for("drift")->filter_response_db(table(), values, hz);
}

void filter_curve_follows_the_filter_types() {
  auto values = oscillate::native_dsp::default_values(table());
  values[index("drift.filter.cutoff")] = 1'000.0;
  values[index("drift.filter.resonance")] = 0.0;
  values[index("drift.filter.type")] = 0; // LP 12, Q = 1/sqrt 2.
  require(std::fabs(response_db(values, 1'000.0) + 3.0103) < 0.01 &&
              std::fabs(response_db(values, 20.0)) < 0.01,
          "LP 12 is flat below and -3 dB at the cutoff");
  require(response_db(values, 10'000.0) < -38.0, "LP 12 falls 12 dB per octave");
  values[index("drift.filter.type")] = 1; // LP 24: a Butterworth pair at r = 0.
  require(std::fabs(response_db(values, 1'000.0) + 3.0103) < 0.01 &&
              response_db(values, 10'000.0) < -78.0,
          "LP 24 is Butterworth: -3 dB at cutoff, 24 dB per octave");
  values[index("drift.filter.type")] = 2; // BP 12: the band output peaks at Q.
  require(std::fabs(response_db(values, 1'000.0) + 3.0103) < 0.01 &&
              response_db(values, 50.0) < -25.0,
          "BP 12 peaks at its centre");
  values[index("drift.filter.type")] = 3; // HP 12.
  require(std::fabs(response_db(values, 20'000.0)) < 0.02 && response_db(values, 100.0) < -38.0,
          "HP 12 passes the top and cuts the bottom");
}

void grain_windows_follow_the_kernel() {
  for (int shape = 0; shape < 4; ++shape) {
    const auto window = q::drift_grain_window(shape, 101);
    require(window.size() == 101, "a window has the requested points");
    for (std::size_t i = 0; i < window.size(); ++i) {
      const double x = static_cast<double>(i) / 100.0;
      require(window[i] == synth::GrainCloud::window(static_cast<synth::GrainShape>(shape), x),
              "the sketch is the kernel's window");
    }
  }
  // Independent shapes: Hann is the raised cosine; Swell mirrors Decay.
  const auto hann = q::drift_grain_window(0, 101);
  const auto decay = q::drift_grain_window(2, 101);
  const auto swell = q::drift_grain_window(3, 101);
  for (std::size_t i = 0; i < hann.size(); ++i) {
    const double x = static_cast<double>(i) / 100.0;
    require(std::fabs(hann[i] - (0.5 - 0.5 * std::cos(2.0 * pi * x))) < 1e-12,
            "Hann is a raised cosine");
    require(std::fabs(swell[i] - decay[100 - i]) < 1e-12, "Swell is Decay reversed");
  }
  require(q::drift_grain_window(4, 10).empty() && q::drift_grain_window(-1, 10).empty(),
          "unknown shapes draw nothing");
}

void partial_spectrum_follows_the_stiff_string_law() {
  auto values = oscillate::native_dsp::default_values(table());
  values[index("drift.partials.tilt")] = 1.5;
  values[index("drift.partials.odd")] = 0.5;
  values[index("drift.partials.stretch")] = 0.001;
  const auto lines = q::drift_partial_spectrum(table(), values);
  require(lines.size() == synth::PartialBank::count, "sixteen partial lines");
  for (std::size_t n = 1; n <= lines.size(); ++n) {
    const double k = static_cast<double>(n);
    require(std::fabs(lines[n - 1].ratio - k * std::sqrt(1.0 + 0.001 * k * k)) < 1e-12,
            "partial n sits at n sqrt(1 + B n^2)");
  }
  // n^-tilt with odd partials raised and even ones lowered by the balance.
  const double expected = (std::pow(3.0, -1.5) * 1.5) / (std::pow(2.0, -1.5) * 0.5);
  require(std::fabs(lines[2].level / lines[1].level - expected) < 1e-12,
          "the odd/even balance weights the partials");
  require(std::fabs(lines[0].level - 1.0) < 1e-12, "the fundamental leads at 1");
}

// The preview against the real scheduler. A GrainCloud with the first note's
// seed reads a frozen buffer holding a ramp (each sample is its own index);
// in each isolated grain's flat middle, the output's value reveals where the
// grain centres, the sign of its slope whether it is reversed, and its size
// the playback rate. The preview must name the same grains.
void cloud_preview_matches_the_scheduler() {
  constexpr double rate = 48'000.0;
  auto values = oscillate::native_dsp::default_values(table());
  values[index("drift.grain.size")] = 0.05;
  values[index("drift.grain.density")] = 4.0;
  values[index("drift.grain.chaos")] = 0.0;
  values[index("drift.blur")] = 0.0;
  values[index("drift.grain.position")] = 1.0;
  values[index("drift.grain.scan")] = 0.0;
  values[index("drift.grain.spray")] = 0.3;
  values[index("drift.grain.pitch")] = 0.0;
  values[index("drift.grain.intervals")] = 1; // Octaves.
  values[index("drift.grain.interval_amount")] = 0.5;
  values[index("drift.grain.jitter")] = 10.0;
  values[index("drift.grain.reverse")] = 0.5;
  values[index("drift.grain.spread")] = 0.0;
  values[index("drift.grain.shape")] = 1; // Tukey: a flat middle.
  values[index("drift.freeze")] = 1;
  const auto preview = q::drift_grain_cloud(table(), values, 2.0, 100);
  require(preview.size() == 8, "four grains a second for two seconds");
  const auto again = q::drift_grain_cloud(table(), values, 2.0, 100);
  for (std::size_t k = 0; k < preview.size(); ++k) {
    require(again[k].time == preview[k].time && again[k].position == preview[k].position &&
                again[k].semitones == preview[k].semitones &&
                again[k].reverse == preview[k].reverse,
            "the preview is deterministic");
  }

  synth::GrainBuffer buffer;
  buffer.prepare(static_cast<std::size_t>(synth::drift::buffer_seconds * rate));
  for (std::size_t i = 0; i < 3 * 48'000; ++i) {
    buffer.write(static_cast<double>(i) / 131'072.0);
  }
  synth::GrainCloud cloud;
  cloud.reset(104'729U + 1U); // Slot 0, age 1: the first note.
  synth::GrainSettings s;
  s.size_frames = 0.05 * rate;
  s.interval_frames = rate / 4.0;
  s.shape = synth::GrainShape::tukey;
  s.lag_frames = rate;
  s.scan = 0.0;
  s.spray_frames = 0.3 * rate;
  s.intervals = synth::GrainIntervals::octaves;
  s.interval_probability = 0.5;
  s.jitter_cents = 10.0;
  s.reverse_probability = 0.5;
  cloud.set(s);
  std::vector<double> left(96'000);
  std::vector<double> right(96'000);
  for (std::size_t start = 0; start < left.size(); start += 32) {
    cloud.render(buffer, true, buffer.count(), left.data() + start, right.data() + start, 32);
  }
  int reversed = 0;
  for (const auto& grain : preview) {
    const auto spawn = static_cast<std::size_t>(std::round(grain.time * rate));
    const std::size_t middle = spawn + static_cast<std::size_t>(s.size_frames / 2.0);
    const double slope = (left[middle + 1] - left[middle]) * 131'072.0;
    const double behind = (static_cast<double>(buffer.count()) - left[middle] * 131'072.0) / rate;
    require((slope < 0.0) == grain.reverse, "the preview names reversed grains");
    require(std::fabs(12.0 * std::log2(std::fabs(slope)) - grain.semitones) < 0.01,
            "the preview pitches grains as the scheduler plays them");
    require(std::fabs(behind - grain.position) < 2e-3,
            "the preview places grains where the scheduler reads them");
    reversed += grain.reverse ? 1 : 0;
  }
  require(reversed > 0 && reversed < 8, "the check sees both directions");

  // Spray, reverse and intervals change the preview as specified.
  values[index("drift.grain.spray")] = 0.0;
  values[index("drift.grain.jitter")] = 0.0;
  values[index("drift.grain.reverse")] = 0.0;
  values[index("drift.grain.interval_amount")] = 0.0;
  for (const auto& grain : q::drift_grain_cloud(table(), values, 2.0, 100)) {
    require(grain.position == 1.0 && grain.semitones == 0.0 && !grain.reverse,
            "without spray, jitter, reverse or harmony every grain sits at the scan centre");
  }
  values[index("drift.grain.reverse")] = 1.0;
  values[index("drift.grain.interval_amount")] = 1.0;
  for (const auto& grain : q::drift_grain_cloud(table(), values, 2.0, 100)) {
    require(grain.reverse &&
                (grain.semitones == -12.0 || grain.semitones == 12.0 || grain.semitones == 24.0),
            "full reverse and harmony reverse every grain onto octaves");
  }
  values[index("drift.sync")] = 1;
  values[index("drift.sync.division")] = 2; // 1/16 at 120 BPM: 0.125 s.
  const auto synced = q::drift_grain_cloud(table(), values, 1.0, 100);
  require(synced.size() == 8, "sixteenths fill a second with eight grains");
  for (std::size_t k = 0; k < synced.size(); ++k) {
    require(std::fabs(synced[k].time - 0.125 * static_cast<double>(k)) < 1e-12,
            "synced grains sit on the grid");
  }
  const auto& prism = *synth::prism::engine().descriptor;
  const auto prism_values = oscillate::native_dsp::default_values(prism);
  require(q::drift_grain_cloud(prism, prism_values, 2.0, 100).empty() &&
              q::drift_partial_spectrum(prism, prism_values).empty() &&
              q::drift_amp_envelope(prism, prism_values, 64).points.empty(),
          "only Drift has grain sketches");
}

void amp_envelope_runs_the_kernel_envelope() {
  auto values = oscillate::native_dsp::default_values(table());
  values[index("drift.amp.attack")] = 0.5;
  values[index("drift.amp.decay")] = 1.0;
  values[index("drift.amp.sustain")] = 0.5;
  values[index("drift.amp.release")] = 1.0;
  const auto sketch = q::drift_amp_envelope(table(), values, 400);
  require(sketch.points.size() == 400 && sketch.points.front() == 0.0, "the sketch starts silent");
  const double peak = *std::max_element(sketch.points.begin(), sketch.points.end());
  require(std::fabs(peak - 1.0) < 0.02, "the attack reaches full level");
  const auto at = [&](double seconds) {
    return sketch.points[static_cast<std::size_t>(seconds / sketch.seconds * 399.0)];
  };
  require(std::fabs(at(sketch.release_at - 0.01) - 0.5) < 0.02, "it holds at the sustain level");
  require(sketch.points.back() < 0.01, "the release falls to silence");
}

void controller_exposes_drift_sketches() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.has_filter_response() && controller.filter_response(32).size() == 32,
          "Drift draws its filter");
  require(!controller.grain_cloud(2.0, 64).isEmpty() &&
              controller.partial_spectrum().size() == 16 &&
              controller.grain_window(0, 24).size() == 24 &&
              controller.amp_envelope(64).value(QStringLiteral("points")).toList().size() == 64,
          "the grain sketches reach QML");
  const auto first = controller.grain_cloud(2.0, 64).first().toMap();
  require(first.contains(QStringLiteral("position")) && first.contains(QStringLiteral("reverse")),
          "grains carry their position and direction");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter prism_adapter(oscillate::native_dsp::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, prism_adapter);
  require(prism_controller.grain_cloud(2.0, 64).isEmpty() &&
              prism_controller.partial_spectrum().isEmpty() &&
              prism_controller.grain_window(0, 24).isEmpty() &&
              prism_controller.amp_envelope(64).isEmpty(),
          "other engines have no grain sketches");
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
      import "qrc:/drift"
      DriftPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/DriftPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Drift panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Drift panel was not created: " + component.errorString().toStdString());
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
  require(panel->root->implicitWidth() >= 1240 && panel->root->implicitHeight() >= 780,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind :
         {"driftKnob:", "driftSegment:", "driftStepper:", "driftDropdown:", "driftToggle:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("driftCloudCaption")).contains(QStringLiteral("NOT LIVE")),
          "the cloud says it is a preview");
  require(
      item(*panel, QStringLiteral("driftKnob:drift.blur"))->property("diameter").toInt() >
          item(*panel, QStringLiteral("driftKnob:drift.grain.size"))->property("diameter").toInt(),
      "Blur is the prominent macro");
  const auto glyph = item(*panel, QStringLiteral("driftSegment:drift.grain.shape"))
                         ->property("glyph")
                         .value<QJSValue>();
  require(glyph.isCallable() && controller.grain_window(0, 24).size() == 24,
          "the shape keys draw their windows");
}

void freeze_and_sync_switch() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* freeze = item(*panel, QStringLiteral("driftToggle:drift.freeze"));
  auto* frost = item(*panel, QStringLiteral("driftFrost"));
  require(!frost->isVisible(), "no frost while live");
  click(*panel, centre(freeze));
  require(controller.choice(QStringLiteral("drift.freeze")) == 1 && frost->isVisible() &&
              panel->root->property("frozen").toBool(),
          "Freeze frosts the cloud");
  require(text_of(*panel, QStringLiteral("driftReadout")) == QStringLiteral("GRAINS · FREEZE"),
          "the display reads back Freeze");
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("drift.freeze")) == 0, "Space thaws it");
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("drift.freeze")) == 0, "Delete resets Freeze");

  auto* sync = item(*panel, QStringLiteral("driftToggle:drift.sync"));
  auto* cloud = item(*panel, QStringLiteral("driftCloudView"));
  click(*panel, centre(sync));
  require(controller.choice(QStringLiteral("drift.sync")) == 1 &&
              cloud->property("synced").toBool(),
          "Sync puts the cloud on the grid");
  require(
      item(*panel, QStringLiteral("driftKnob:drift.grain.chaos"))->property("inactive").toBool(),
      "synced spawning dims chaos");
  // The cloud follows the settings.
  const auto before = cloud->property("grains").toList().size();
  controller.set_value(QStringLiteral("drift.sync.division"), 0); // 1/32: twice as many.
  QCoreApplication::processEvents();
  require(cloud->property("grains").toList().size() > before, "a finer division adds grains");
}

void controls_edit_with_gestures_and_keys() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);

  auto* size = item(*panel, QStringLiteral("driftKnob:drift.grain.size"));
  const auto start = adapter.submissions().size();
  const double before = controller.value(QStringLiteral("drift.grain.size"));
  const QPoint knob = size->mapToScene(QPointF(size->width() / 2, 22)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 15));
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 30));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob - QPoint(0, 30));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a knob drag is begin, updates and commit");
  require(controller.value(QStringLiteral("drift.grain.size")) > before,
          "dragging up grows grains");
  require(text_of(*panel, QStringLiteral("driftReadout")) == QStringLiteral("GRAINS · SIZE") &&
              text_of(*panel, QStringLiteral("driftReadoutValue")) ==
                  controller.display(QStringLiteral("drift.grain.size")),
          "the display reads back the size");

  const double coarse = controller.normalised(QStringLiteral("drift.grain.size"));
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob);
  const QPointF moved = knob - QPoint(0, 30);
  QMouseEvent shift_move(QEvent::MouseMove, moved, moved, panel->window->mapToGlobal(moved),
                         Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
  QCoreApplication::sendEvent(panel->window.get(), &shift_move);
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob - QPoint(0, 30));
  const double fine = controller.normalised(QStringLiteral("drift.grain.size")) - coarse;
  require(fine > 0.0 && fine < 30.0 / 1'000.0, "Shift drags finely");

  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  require(controller.is_default(QStringLiteral("drift.grain.size")), "double-click resets");
  wheel(*panel, knob, 120);
  require(controller.value(QStringLiteral("drift.grain.size")) > before, "the wheel steps up");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(QStringLiteral("drift.grain.size")), "Delete resets");
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("drift.grain.size")) > before, "Up steps the knob");

  // The shape keys pick a window; Left and Right step.
  auto* shape = item(*panel, QStringLiteral("driftSegment:drift.grain.shape"));
  click(*panel, at(shape, 0.62, 0.5));
  require(controller.choice(QStringLiteral("drift.grain.shape")) == 2, "a key picks Decay");
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("drift.grain.shape")) == 3, "Right steps to Swell");

  // Harmony dims while intervals are off.
  controller.set_choice(QStringLiteral("drift.grain.intervals"), 0);
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("driftKnob:drift.grain.interval_amount"))
              ->property("inactive")
              .toBool(),
          "harmony is dimmed without intervals");

  // A dropdown opens the shared menu and the keyboard chooses.
  auto* lfo_shape = item(*panel, QStringLiteral("driftDropdown:drift.lfo1.shape"));
  auto* menu = item(*panel, QStringLiteral("driftMenu"));
  click(*panel, centre(lfo_shape));
  require(menu->isVisible(), "the LFO menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.choice(QStringLiteral("drift.lfo1.shape")) == 2,
          "the keyboard picks Saw");

  // Steppers: arrows, keys and Delete.
  auto* pitch = item(*panel, QStringLiteral("driftStepper:drift.grain.pitch"));
  click(*panel, centre(find_item(pitch, QStringLiteral("driftStepperUp"))));
  require(controller.value(QStringLiteral("drift.grain.pitch")) == 1.0,
          "the up arrow raises pitch");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(controller.value(QStringLiteral("drift.grain.pitch")) == -1.0, "Down lowers it");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.value(QStringLiteral("drift.grain.pitch")) == 0.0, "Delete resets pitch");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("driftPresetName")) == QStringLiteral("Nebula"),
          "defaults are Nebula");
  click(*panel, centre(item(*panel, QStringLiteral("driftPresetNext"))));
  require(controller.preset_name() == QStringLiteral("Glass Rain"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("driftPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Nebula"), "previous steps back");

  auto* display = item(*panel, QStringLiteral("driftPresetDisplay"));
  auto* menu = item(*panel, QStringLiteral("driftMenu"));
  click(*panel, centre(display));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");

  controller.apply_preset(preset_index("Frozen Choir"));
  QCoreApplication::processEvents();
  require(panel->root->property("frozen").toBool(), "a frozen preset frosts the cloud");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("driftStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#e0901f")),
          "deferred status is highlighted");
}

// A frame of `count` grains spread back from the write head, every third
// reversed, all frozen or none.
std::vector<float> drift_frame(std::size_t count, bool frozen) {
  namespace d = synth::drift::telemetry;
  std::vector<float> frame(d::first_grain + count * d::stride);
  frame[d::count] = static_cast<float>(count);
  for (std::size_t g = 0; g < count; ++g) {
    const std::size_t base = d::first_grain + g * d::stride;
    frame[base + d::key] = 60.0F;
    frame[base + d::behind] = 0.2F + 0.12F * static_cast<float>(g);
    frame[base + d::semitones] = static_cast<float>(g % 3 == 0 ? 12 : g % 3 == 1 ? 0 : 7);
    frame[base + d::reverse] = g % 3 == 0 ? 1.0F : 0.0F;
    frame[base + d::phase] = static_cast<float>(g % 5) / 5.0F + 0.1F;
    frame[base + d::frozen] = frozen ? 1.0F : 0.0F;
  }
  return frame;
}

// The cloud draws the grains actually sounding while the synth runs
// (ADR-0037), and the settings preview otherwise.
void cloud_shows_live_grains() {
  InMemoryNativeSynthAdapter adapter(preset("Nebula"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* cloud = item(*panel, QStringLiteral("driftCloudView"));
  QTest::qWait(300);
  require(
      !cloud->property("live").toBool() &&
          text_of(*panel, QStringLiteral("driftCloudCaption")).contains(QStringLiteral("NOT LIVE")),
      "no telemetry: the settings preview");
  adapter.set_telemetry(drift_frame(5, false));
  require(QTest::qWaitFor([&] { return cloud->property("live").toBool(); }, 1000),
          "live grains arrive");
  require(cloud->property("grains").toList().size() == 5 &&
              text_of(*panel, QStringLiteral("driftCloudCaption")) ==
                  QStringLiteral("LIVE · 5 GRAINS"),
          "the cloud draws the five live grains");
  const auto first = cloud->property("grains").toList().front().toMap();
  require(std::fabs(first.value(QStringLiteral("position")).toDouble() - 0.2) < 1e-6 &&
              first.value(QStringLiteral("semitones")).toDouble() == 12.0 &&
              first.value(QStringLiteral("reverse")).toBool(),
          "a grain sits behind the head at its pitch, reversed");
  require(!item(*panel, QStringLiteral("driftFrost"))->isVisible(),
          "a playing cloud is not frozen");
  adapter.set_telemetry(drift_frame(3, true));
  require(QTest::qWaitFor([&] { return item(*panel, QStringLiteral("driftFrost"))->isVisible(); },
                          1000),
          "a frozen voice frosts the live cloud");
  adapter.set_telemetry({});
  require(
      QTest::qWaitFor([&] { return !cloud->property("live").toBool(); }, 1000) &&
          text_of(*panel, QStringLiteral("driftCloudCaption")).contains(QStringLiteral("NOT LIVE")),
      "telemetry gone: back to the preview");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("DRIFT_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"Nebula", "drift.grain.size", "/drift_nebula.png"},
                          Shot{"Frozen Choir", "drift.freeze", "/drift_frozen_choir.png"},
                          Shot{"Stutter Clock", "drift.sync.division", "/drift_stutter_clock.png"},
                          Shot{"Spectral Smear", "drift.blur", "/drift_spectral_smear.png"},
                          Shot{"Glass Rain", "drift.grain.intervals", "/drift_glass_rain.png"}}) {
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
  InMemoryNativeSynthAdapter adapter(preset("Nebula"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(drift_frame(24, false));
  QTest::qWait(400);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/drift_live.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    filter_curve_follows_the_filter_types();
    grain_windows_follow_the_kernel();
    partial_spectrum_follows_the_stiff_string_law();
    cloud_preview_matches_the_scheduler();
    amp_envelope_runs_the_kernel_envelope();
    controller_exposes_drift_sketches();
    panel_covers_every_parameter();
    freeze_and_sync_switch();
    controls_edit_with_gestures_and_keys();
    presets_and_status();
    cloud_shows_live_grains();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "drift panel contract passed\n";
  return 0;
}
