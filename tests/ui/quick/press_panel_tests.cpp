#include "device_strip_view.hpp"
#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
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
#include <utility>
#include <vector>

// The Press panel (ADR-0047): every parameter on a table-driven control;
// the transfer curve from the compressor's own gain computer, against a
// hand-calculated soft knee; dragging the screen for the threshold and
// scrolling for the ratio; the telemetry decode into the gain-reduction
// meter, the operating point and the peak meters; the detector high-pass
// and the keyed indicator; the real Press over a steady tone landing where
// the curve says; presets and status; and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace press = oscillate::native_dsp::effects::press;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *press::engine().descriptor; }
int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

std::vector<double> preset(std::string_view name) {
  for (const auto& entry : table().presets) {
    if (entry.name == name) {
      return synth::preset_values(table(), entry).value();
    }
  }
  throw std::runtime_error("missing preset " + std::string(name));
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::vector<double> numbers(const QVariantList& list) {
  std::vector<double> out;
  for (const auto& value : list) {
    out.push_back(value.toDouble());
  }
  return out;
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
      import "qrc:/press"
      PressPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/PressPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Press panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Press panel was not created: " + component.errorString().toStdString());
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

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, 1000);
}

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<float> frame_of(float in_l, float in_r, float out_l, float out_r, float reduction) {
  namespace t = press::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = in_l;
  frame[t::input_peak + 1] = in_r;
  frame[t::output_peak] = out_l;
  frame[t::output_peak + 1] = out_r;
  frame[t::reduction] = reduction;
  return frame;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 660,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"pressKnob:", "pressToggle:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("pressPresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(!item(*panel, QStringLiteral("pressKeyed"))->isVisible(), "not keyed by default");
}

// The classic soft knee (compressor.hpp), by hand: -18 dB, 4:1, 6 dB knee.
void the_curve_is_the_gain_computer() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  const auto curve = numbers(controller.press_transfer(-60.0, 61));
  require(curve.size() == 61, "one point a dB");
  require(std::fabs(curve[0] + 60.0) < 1e-9, "far under the knee: unity");
  require(std::fabs(curve[36] + 24.0) < 1e-9, "-24 dB is under the knee (-21): unity");
  // At the threshold, half way through the knee: 0.75 * 3^2 / 12.
  require(std::fabs(curve[42] + 18.5625) < 1e-9,
          "the knee at the threshold: " + std::to_string(curve[42]));
  // Above the knee: 18 dB over, at 4:1, comes out 4.5 dB over.
  require(std::fabs(curve[60] + 13.5) < 1e-9, "full scale at 4:1");
  require(controller.press_transfer(0.0, 61).isEmpty(), "a floor must be under 0 dBFS");
  controller.set_value(QStringLiteral("press.makeup"), 3.0);
  require(std::fabs(numbers(controller.press_transfer(-60.0, 61))[60] + 10.5) < 1e-9,
          "makeup lifts the curve");

  auto panel = load_panel(controller);
  auto* view = item(*panel, QStringLiteral("pressTransfer"));
  auto* handle = item(*panel, QStringLiteral("pressThresholdHandle"));
  QVariant y_of;
  require(QMetaObject::invokeMethod(view, "yOf", Q_RETURN_ARG(QVariant, y_of),
                                    Q_ARG(QVariant, -18.5625 + 3.0)),
          "yOf() runs");
  require(std::fabs(handle->property("hy").toDouble() - y_of.toDouble()) < 0.05,
          "the threshold handle sits on the curve");
  require(
      item(*panel, QStringLiteral("pressScreenReadout:press.ratio"))->property("text").toString() ==
          controller.display(QStringLiteral("press.ratio")),
      "the screen reads the ratio");
}

void the_screen_and_dials_edit() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* drag = item(*panel, QStringLiteral("pressTransferDrag"));
  const auto start = adapter.submissions().size();
  const auto at = [&](double fx) { return scene(drag, drag->width() * fx, drag->height() * 0.5); };
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, at(0.3));
  QTest::mouseMove(panel->window.get(), at(0.4));
  QTest::mouseMove(panel->window.get(), at(0.5));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, at(0.5));
  require(adapter.submissions()[start].gesture == NativeSynthGesture::begin &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "a screen drag is one gesture");
  require(std::fabs(controller.value(QStringLiteral("press.threshold")) + 30.0) < 0.5,
          "the threshold follows the pointer: " +
              std::to_string(controller.value(QStringLiteral("press.threshold"))));
  require(text_of(*panel, QStringLiteral("pressReadout")) == QStringLiteral("DYNAMICS · THRESHOLD"),
          "the display reads back the threshold");
  const double ratio = controller.value(QStringLiteral("press.ratio"));
  wheel(*panel, at(0.5), 120);
  require(controller.value(QStringLiteral("press.ratio")) > ratio, "scrolling raises the ratio");

  // The detector's frequency waits for its high-pass.
  auto* freq = item(*panel, QStringLiteral("pressKnob:press.detector_freq"));
  require(freq->property("inactive").toBool(), "the HP frequency is inactive while it is off");
  click(*panel, centre(item(*panel, QStringLiteral("pressToggle:press.detector_hp"))));
  require(controller.choice(QStringLiteral("press.detector_hp")) == 1, "the switch turns it on");
  require(!freq->property("inactive").toBool(), "and the frequency wakes");

  // A sidechain keys it.
  panel->root->setProperty("keyed", true);
  panel->root->setProperty("keyName", QStringLiteral("Kick"));
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("pressKeyed"))->isVisible() &&
              text_of(*panel, QStringLiteral("pressKeyedText")) == QStringLiteral("KEYED · KICK"),
          "the key is shown by name");
}

void telemetry_decodes_into_the_meters() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  require(text_of(*panel, QStringLiteral("pressReduction")) == QStringLiteral("—"),
          "a dash with nothing measured");
  auto* meter = item(*panel, QStringLiteral("pressReductionMeter"));
  require(meter->property("reductionAt").toDouble() == 0.0, "an idle meter is empty");

  adapter.set_telemetry(frame_of(-6.0F, -8.0F, -15.0F, -17.0F, 9.0F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("reduction")).toDouble() == 9.0 &&
              live.value(QStringLiteral("input_peak")).toList().size() == 2,
          "the controller decodes Press's frame");
  require(wait_for_text(*panel, QStringLiteral("pressReduction"), QStringLiteral("−9.0")),
          "the reduction reads: " +
              text_of(*panel, QStringLiteral("pressReduction")).toStdString());
  require(std::fabs(meter->property("reductionAt").toDouble() - 9.0 / 24.0) < 1e-6,
          "the bar hangs 9 dB down a 24 dB scale");
  auto* fill = find_item(meter, QStringLiteral("pressReductionFill"));
  require(fill != nullptr && fill->height() > 0, "and fills");
  auto* view = item(*panel, QStringLiteral("pressTransfer"));
  require(view->property("pointShown").toBool() &&
              std::fabs(view->property("pointOut").toDouble() + 15.0) < 1e-6,
          "the operating point: the louder input less the reduction");
  require(text_of(*panel, QStringLiteral("pressInPeakL")) == QStringLiteral("−6.0") &&
              text_of(*panel, QStringLiteral("pressOutPeakR")) == QStringLiteral("−17.0"),
          "the peak meters");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("pressReduction"), QStringLiteral("—")),
          "no telemetry: a dash again");
  require(!view->property("pointShown").toBool(), "and no operating point");
}

// ---- The real compressor --------------------------------------------------------

constexpr double rate = 48'000.0;

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<engine::StereoFrame> sine(double amplitude, std::size_t frames) {
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const auto s = static_cast<float>(
        amplitude * std::sin(2 * std::numbers::pi * 1'000.0 * static_cast<double>(i) / rate));
    out[i] = {s, s};
  }
  return out;
}

// The real Press over the input; a telemetry frame every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  auto created = effects::NativeEffectRuntime::create(press::engine(), values, rate);
  require(created.has_value(), "a Press runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  for (std::size_t position = 0; position < input.size(); position += 160) {
    const std::size_t size = std::min<std::size_t>(160, input.size() - position);
    block.assign(input.begin() + static_cast<std::ptrdiff_t>(position),
                 input.begin() + static_cast<std::ptrdiff_t>(position + size));
    const synth::SynthTransport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Press renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Press publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

// A steady -6 dBFS tone into -18 dB at 4:1: 12 dB over, 9 dB of reduction,
// out at -15 dBFS, where the curve says.
void the_real_press_lands_on_its_curve() {
  const auto values = preset("Default");
  const auto frames = real_telemetry(values, sine(0.5, static_cast<std::size_t>(2.0 * rate)));
  const auto& last = frames.back();
  namespace t = press::telemetry;
  require(std::fabs(last[t::input_peak] + 6.02) < 0.1, "the tone's peak");
  require(std::fabs(last[t::reduction] - 9.0) < 1.0,
          "the reduction the curve predicts: " + std::to_string(last[t::reduction]));
  require(std::fabs(last[t::output_peak] - (last[t::input_peak] - last[t::reduction])) < 1.0,
          "the output is the input less the reduction");

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(last);
  QString expected =
      QStringLiteral("−") + QString::number(static_cast<double>(last[t::reduction]), 'f', 1);
  require(wait_for_text(*panel, QStringLiteral("pressReduction"), expected),
          "the panel reads the real reduction");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("pressPresetNext"))));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("pressPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("pressMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("pressPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Vocal"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("pressPresetName")) == QStringLiteral("Vocal"),
          "a preset shows by name");
  require(item(*panel, QStringLiteral("pressToggle:press.detector_hp"))->property("on").toBool(),
          "and lights its detector high-pass");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("pressStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("PRESS_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    double amplitude;
    bool keyed;
    const char* file;
  };
  for (const Shot& shot : {Shot{"Default", 0.5, false, "/press_default.png"},
                           Shot{"Squash", 0.7, false, "/press_squash.png"},
                           Shot{"Vocal", 0.3, true, "/press_vocal_keyed.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->setProperty("keyed", shot.keyed);
    panel->root->setProperty("keyName", QStringLiteral("Kick"));
    const auto frames =
        real_telemetry(values, sine(shot.amplitude, static_cast<std::size_t>(2.0 * rate)));
    for (std::size_t i = frames.size() > 20 ? frames.size() - 20 : 0; i < frames.size(); ++i) {
      adapter.set_telemetry(frames[i]);
      QTest::qWait(33);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

// The device-chain strip (device_strip_view.hpp): Press's reduction view on
// its panel's range, with a cymbal's band leading.
void telemetry_gives_the_strip_the_reduction_and_its_band() {
  namespace t = press::telemetry;
  namespace strip_view = oscillate::quick::strip_view;
  constexpr float peak_db = -6.0F;
  constexpr float reduction_db = 9.0F;
  constexpr std::size_t top_band = effects::reduction_bands::count - 1;
  constexpr float top_share = 0.8F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto frame = frame_of(peak_db, peak_db, peak_db, peak_db, reduction_db);
  frame[t::reduction_bands + top_band] = top_share;
  adapter.set_telemetry(frame);
  const auto strip = controller.telemetry().value(QStringLiteral("strip")).toMap();
  require(strip.value(QStringLiteral("kind")).toString() == QStringLiteral("reduction") &&
              strip.value(QStringLiteral("range_db")).toDouble() == strip_view::press_range_db,
          "Press's strip is a reduction view on its panel's range");
  require(strip.value(QStringLiteral("text")).toString() == QStringLiteral("−9.0 dB · max −9.0"),
          "now and the largest: " + strip.value(QStringLiteral("text")).toString().toStdString());
  require(
      strip.value(QStringLiteral("lead")).toInt() == static_cast<int>(top_band) &&
          strip.value(QStringLiteral("lead_text")).toString() == QStringLiteral("8.43–20.0 kHz"),
      "the top band leads: " + strip.value(QStringLiteral("lead_text")).toString().toStdString());
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    the_curve_is_the_gain_computer();
    the_screen_and_dials_edit();
    telemetry_decodes_into_the_meters();
    the_real_press_lands_on_its_curve();
    presets_and_status();
    telemetry_gives_the_strip_the_reduction_and_its_band();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "press panel contract passed\n";
  return 0;
}
