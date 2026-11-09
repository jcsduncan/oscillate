#include "in_memory_native_synth_adapter.hpp"
#include "native_effect_visuals.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/effects/contour.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
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

// The Contour panel (ADR-0040): the response sketch measured from the real
// effect against Butterworth and comb oracles, the controller's telemetry
// decode, and the QML panel driven by the effect's table, live and still.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace contour = oscillate::native_dsp::effects::contour;
namespace q = oscillate::quick;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *contour::engine().descriptor; }
std::size_t index(std::string_view id) {
  const auto found = oscillate::native_dsp::find_parameter(table(), id);
  if (!found) {
    throw std::runtime_error("missing Contour parameter");
  }
  return *found;
}

std::vector<double> preset(std::string_view name) {
  for (const auto& entry : table().presets) {
    if (entry.name == name) {
      auto values = oscillate::native_dsp::preset_values(table(), entry);
      if (!values.has_value()) {
        throw std::runtime_error("invalid Contour preset");
      }
      return std::move(values).value();
    }
  }
  throw std::runtime_error("missing preset");
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// ---- Response sketch ----------------------------------------------------------------

constexpr std::size_t points = 301; // About 30 per octave from 20 Hz.

double sketch_at(const std::vector<double>& response, double hz) {
  const double position = std::log2(hz / 20.0) / std::log2(1'000.0) * (points - 1);
  return response[static_cast<std::size_t>(std::lround(position))];
}

struct FilterFixture {
  int model;
  double cutoff;
  double resonance = 0.0;
};

std::vector<double> filter(FilterFixture fixture) {
  auto values = preset("Init");
  values[index("contour.model")] = fixture.model;
  values[index("contour.cutoff")] = fixture.cutoff;
  values[index("contour.resonance")] = fixture.resonance;
  return values;
}

void the_sketch_is_the_real_filter() {
  // LP 24 at r = 0 is a fourth-order Butterworth, read at each point's
  // frequency between the FFT's bins.
  const auto lp =
      q::contour_response(table(), points, filter({.model = 1, .cutoff = 1'000.0}), 0.0);
  require(lp.size() == points, "one value per point");
  const auto butterworth = [](double hz, double cutoff, int order) {
    return -10.0 * std::log10(1.0 + std::pow(hz / cutoff, 2.0 * order));
  };
  for (const std::size_t p : {100UL, 150UL, 200UL, 220UL}) {
    const double hz = 20.0 * std::pow(1'000.0, static_cast<double>(p) / (points - 1));
    const double expected = butterworth(hz, 1'000.0, 4);
    require(std::fabs(lp[p] - expected) < 0.3, "LP 24 at " + std::to_string(hz) + " Hz draws " +
                                                   std::to_string(lp[p]) + " dB, " +
                                                   std::to_string(expected) + " expected");
  }
  const auto hp =
      q::contour_response(table(), points, filter({.model = 3, .cutoff = 1'000.0}), 0.0);
  require(sketch_at(hp, 100.0) < -38.0 && std::fabs(sketch_at(hp, 10'000.0)) < 0.3,
          "HP 12 falls 12 dB/oct below its cutoff");
  // The notch shows its depth at the centre.
  const auto notch =
      q::contour_response(table(), points, filter({.model = 5, .cutoff = 1'000.0}), 0.0);
  require(sketch_at(notch, 1'000.0) < -25.0 && std::fabs(sketch_at(notch, 100.0)) < 0.3,
          "the notch draws deep at its centre: " + std::to_string(sketch_at(notch, 1'000.0)));
  // Comb +: peaks on the harmonics of the cutoff, troughs between.
  const auto comb = q::contour_response(
      table(), points, filter({.model = 9, .cutoff = 200.0, .resonance = 0.9}), 0.0);
  require(sketch_at(comb, 400.0) - sketch_at(comb, 300.0) > 12.0, "Comb + peaks at harmonics");
  // A live cutoff moves the curve.
  const auto moved =
      q::contour_response(table(), points, filter({.model = 1, .cutoff = 1'000.0}), 4'000.0);
  require(std::fabs(sketch_at(moved, 4'000.0) - sketch_at(lp, 1'000.0)) < 0.5,
          "the live cutoff redraws the same shape higher");
  const auto& prism = *synth::prism::engine().descriptor;
  require(
      q::contour_response(prism, points, oscillate::native_dsp::default_values(prism), 0.0).empty(),
      "only Contour has this sketch");
}

void the_controller_decodes_telemetry() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.contour_response(64, 0.0).size() == 64, "the sketch reaches QML");
  require(!controller.telemetry()[QStringLiteral("live")].toBool(), "nothing playing");
  std::vector<float> frame(contour::telemetry::size);
  frame[contour::telemetry::cutoff_hz] = 2'400.0F;
  frame[contour::telemetry::envelope] = 0.25F;
  frame[contour::telemetry::lfo] = 7.0F;
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live[QStringLiteral("live")].toBool() &&
              live[QStringLiteral("cutoff_hz")].toDouble() == 2'400.0 &&
              live[QStringLiteral("envelope")].toDouble() == 0.25 &&
              live[QStringLiteral("lfo")].toDouble() == 7.0,
          "the live cutoff, envelope and LFO decode");
}

// ---- Panel --------------------------------------------------------------------------

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
      import "qrc:/contour"
      ContourPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/ContourPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Contour panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Contour panel was not created: " + component.errorString().toStdString());
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
  require(panel->root->implicitWidth() >= 1100 && panel->root->implicitHeight() >= 600,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind :
         {"contourKnob:", "contourSegment:", "contourDropdown:", "contourModels:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  for (int model = 0; model < 11; ++model) {
    require(find_item(panel->root, QStringLiteral("contourModel:%1").arg(model)) != nullptr,
            "every model has a key");
  }
}

void the_model_bank_switches_models() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("contourModelCaption")).contains(QStringLiteral("24")),
          "the default LP 24 is captioned");
  click(*panel, centre(item(*panel, QStringLiteral("contourModel:6"))));
  require(controller.choice(QStringLiteral("contour.model")) == 6, "a key picks the ladder");
  require(text_of(*panel, QStringLiteral("contourModelCaption")).contains(QStringLiteral("LADDER")),
          "the screen names the ladder");
  require(item(*panel, QStringLiteral("contourKnob:contour.morph"))->property("inactive").toBool(),
          "morph is dimmed off the SEM");
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(
      controller.choice(QStringLiteral("contour.model")) == 8 &&
          !item(*panel, QStringLiteral("contourKnob:contour.morph"))->property("inactive").toBool(),
      "Right steps to the SEM, which lights morph");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(controller.choice(QStringLiteral("contour.model")) == 10, "Down jumps a row");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("contour.model")) == 1, "Delete restores LP 24");
  require(text_of(*panel, QStringLiteral("contourReadout")).contains(QStringLiteral("MODEL")),
          "the display reads back the model");
}

void controls_edit_with_gestures() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);

  auto* cutoff = item(*panel, QStringLiteral("contourKnob:contour.cutoff"));
  const auto start = adapter.submissions().size();
  const double before = controller.value(QStringLiteral("contour.cutoff"));
  const QPoint knob = cutoff->mapToScene(QPointF(cutoff->width() / 2, 30)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 15));
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 30));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob - QPoint(0, 30));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a knob drag is begin, updates and commit");
  require(controller.value(QStringLiteral("contour.cutoff")) > before, "dragging up opens it");
  require(text_of(*panel, QStringLiteral("contourReadout")) == QStringLiteral("FILTER · CUTOFF") &&
              text_of(*panel, QStringLiteral("contourScreenReadout:contour.cutoff")) ==
                  controller.display(QStringLiteral("contour.cutoff")),
          "the display and the screen read back the cutoff");

  // The screen's handle: drag sideways to set the cutoff, scroll for
  // resonance.
  auto* screen = item(*panel, QStringLiteral("contourCutoffDrag"));
  const auto drag_start = adapter.submissions().size();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, at(screen, 0.3, 0.5));
  QTest::mouseMove(panel->window.get(), at(screen, 0.5, 0.5));
  QTest::mouseMove(panel->window.get(), at(screen, 0.8, 0.5));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, at(screen, 0.8, 0.5));
  const double expected = controller.frequency_at(0.8);
  require(std::fabs(std::log2(controller.value(QStringLiteral("contour.cutoff")) / expected)) <
              0.05,
          "dragging the handle puts the cutoff under the pointer");
  require(adapter.submissions()[drag_start].gesture == NativeSynthGesture::begin &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "a handle drag is one gesture");
  const double resonance = controller.value(QStringLiteral("contour.resonance"));
  wheel(*panel, at(screen, 0.5, 0.5), 120);
  require(controller.value(QStringLiteral("contour.resonance")) > resonance,
          "scrolling the screen raises resonance");

  require(item(*panel, QStringLiteral("contourDropdown:contour.lfo.division"))
              ->property("inactive")
              .toBool(),
          "the division is dimmed while free");
  click(*panel, at(item(*panel, QStringLiteral("contourSegment:contour.lfo.sync")), 0.75, 0.5));
  require(controller.choice(QStringLiteral("contour.lfo.sync")) == 1 &&
              item(*panel, QStringLiteral("contourKnob:contour.lfo.rate"))
                  ->property("inactive")
                  .toBool(),
          "sync dims the free rate");
  auto* division = item(*panel, QStringLiteral("contourDropdown:contour.lfo.division"));
  auto* menu = item(*panel, QStringLiteral("contourMenu"));
  click(*panel, centre(division));
  require(menu->isVisible(), "the division menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.choice(QStringLiteral("contour.lfo.division")) == 9,
          "the keyboard picks the next division");

  click(*panel, at(item(*panel, QStringLiteral("contourSegment:contour.env.source")), 0.75, 0.5));
  require(controller.choice(QStringLiteral("contour.env.source")) == 1,
          "the source switches to the sidechain");
  require(
      item(*panel, QStringLiteral("contourKnob:contour.env.attack"))->property("inactive").toBool(),
      "attack is dimmed with no envelope amount");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("contourPresetName")) == QStringLiteral("Warm Low-Pass"),
          "defaults are Warm Low-Pass");
  click(*panel, centre(item(*panel, QStringLiteral("contourPresetNext"))));
  require(controller.preset_name() == QStringLiteral("Acid Sweep") &&
              controller.choice(QStringLiteral("contour.model")) == 6,
          "next loads Acid Sweep and its ladder");
  click(*panel, centre(item(*panel, QStringLiteral("contourPresetDisplay"))));
  auto* menu = item(*panel, QStringLiteral("contourMenu"));
  require(menu->isVisible(), "the preset browser opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.set_value(QStringLiteral("contour.drive"), 6.0);
  QCoreApplication::processEvents();
  require(controller.preset_modified(), "an edit marks the preset");
  adapter.set_status({.saved_revision = 9,
                      .audible_revision = 9,
                      .editable = false,
                      .message = "Effect unavailable"});
  controller.refresh();
  QCoreApplication::processEvents();
  require(!item(*panel, QStringLiteral("contourKnob:contour.cutoff"))->isEnabled() ||
              item(*panel, QStringLiteral("contourKnob:contour.cutoff"))->opacity() < 0.5,
          "a locked effect dims its controls");
}

struct TelemetryFixture {
  double cutoff;
  double envelope;
  double lfo;
};

std::vector<float> live_frame(TelemetryFixture fixture) {
  std::vector<float> frame(contour::telemetry::size);
  frame[contour::telemetry::cutoff_hz] = static_cast<float>(fixture.cutoff);
  frame[contour::telemetry::envelope] = static_cast<float>(fixture.envelope);
  frame[contour::telemetry::lfo] = static_cast<float>(fixture.lfo);
  return frame;
}

void the_screen_follows_the_running_filter() {
  InMemoryNativeSynthAdapter adapter(preset("Auto-Wah"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  require(!panel->root->property("liveOn").toBool() &&
              !item(*panel, QStringLiteral("contourLiveLegend"))->isVisible(),
          "no telemetry: the set curve alone");
  adapter.set_telemetry(live_frame({.cutoff = 3'200.0, .envelope = 0.4, .lfo = 0.0}));
  require(QTest::qWaitFor([&] { return panel->root->property("liveOn").toBool(); }, 1000),
          "telemetry lights the screen");
  require(QTest::qWaitFor(
              [&] { return panel->root->property("liveResponse").toList().size() == 240; }, 1000),
          "the live curve is measured at the live cutoff");
  require(item(*panel, QStringLiteral("contourLiveLegend"))->isVisible() &&
              text_of(*panel, QStringLiteral("contourLiveCutoff")) ==
                  QStringLiteral("LIVE 3.20 kHz"),
          "the screen labels the live cutoff");
  require(item(*panel, QStringLiteral("contourEnvelopeMeter"))->property("level").toDouble() > 0.8,
          "the envelope meter reads the follower");
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !panel->root->property("liveOn").toBool(); }, 1000),
          "telemetry gone: still view");
  require(panel->root->property("liveResponse").toList().isEmpty(), "and the live curve clears");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("CONTOUR_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"Warm Low-Pass", "contour.cutoff", "/contour_warm_low_pass.png"},
        Shot{"Acid Sweep", "contour.resonance", "/contour_acid_sweep.png"},
        Shot{"Metallic Comb", "contour.model", "/contour_metallic_comb.png"},
        Shot{"Stereo Notch Sweep", "contour.lfo.stereo", "/contour_stereo_notch.png"}}) {
    InMemoryNativeSynthAdapter adapter(preset(shot.preset));
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    auto* theme = panel->root->property("theme").value<QObject*>();
    theme->setProperty("lastTouched", QString::fromLatin1(shot.touched));
    QTest::qWait(200);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  // Live states as the kernel would report them: the acid sweep's LFO
  // 14 st up, and the auto-wah's follower at -9 dB, which opens it by
  // 36 st x (60 - 9) / 60.
  struct LiveShot {
    const char* preset;
    double cutoff;
    double envelope;
    double lfo;
    const char* file;
  };
  const double wah_envelope = 0.35;
  const double wah_semitones = 36.0 * (20.0 * std::log10(wah_envelope) + 60.0) / 60.0;
  for (const LiveShot shot :
       {LiveShot{"Acid Sweep", 500.0 * std::exp2(14.0 / 12.0), 0.0, 14.0, "/contour_live_lfo.png"},
        LiveShot{"Auto-Wah", 400.0 * std::exp2(wah_semitones / 12.0), wah_envelope, 0.0,
                 "/contour_live_envelope.png"}}) {
    InMemoryNativeSynthAdapter adapter(preset(shot.preset));
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    adapter.set_telemetry(
        live_frame({.cutoff = shot.cutoff, .envelope = shot.envelope, .lfo = shot.lfo}));
    QTest::qWait(500);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    the_sketch_is_the_real_filter();
    the_controller_decodes_telemetry();
    panel_covers_every_parameter();
    the_model_bank_switches_models();
    controls_edit_with_gestures();
    presets_and_status();
    the_screen_follows_the_running_filter();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "contour panel contract passed\n";
  return 0;
}
