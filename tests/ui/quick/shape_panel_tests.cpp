#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
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

// The Shape panel (ADR-0047): every band's on, kind, frequency, Q and gain
// on a table-driven control (the strip follows the selected band); the
// response curve from the EQ's own biquads (a bell peaks at its gain, a
// low cut falls 12 dB an octave, bands sum in dB); the handles (drag for
// frequency and gain as one gesture, scroll for Q, double-click the screen
// to add a band); the telemetry decode into the peak meters; presets and
// status; and screenshots on request, live from the real Shape.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace shape = oscillate::native_dsp::effects::shape;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *shape::engine().descriptor; }
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

std::vector<double> with(std::vector<double> values,
                         std::initializer_list<std::pair<std::string_view, double>> moves) {
  for (const auto& [id, value] : moves) {
    const auto index = synth::find_parameter(table(), id);
    require(index.has_value(), "unknown parameter " + std::string(id));
    values[*index] = value;
  }
  return values;
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString band_id(const char* name, int band) {
  return QStringLiteral("shape.%1_%2").arg(QString::fromLatin1(name)).arg(band);
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
      import "qrc:/shape"
      ShapePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/ShapePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Shape panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Shape panel was not created: " + component.errorString().toStdString());
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

std::vector<float> frame_of(float in_l, float in_r, float out_l, float out_r) {
  namespace t = shape::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = in_l;
  frame[t::input_peak + 1] = in_r;
  frame[t::output_peak] = out_l;
  frame[t::output_peak + 1] = out_r;
  return frame;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Flat"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 700,
          "panel has its designed size");
  require(text_of(*panel, QStringLiteral("shapePresetName")) == QStringLiteral("Flat"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("shapeCurveCaption")).startsWith(QStringLiteral("FLAT")),
          "a flat EQ says so");
  // The strip follows the selected band, so every band's controls appear
  // once it is selected; every band's switch is always on its tile.
  for (int band = 1; band <= 8; ++band) {
    click(*panel, centre(item(*panel, QStringLiteral("shapeBand:%1").arg(band))));
    require(panel->root->property("selected").toInt() == band - 1, "a tile selects its band");
    require(find_item(panel->root, QStringLiteral("shapeToggle:") + band_id("on", band)) != nullptr,
            "band switch");
    require(find_item(panel->root, QStringLiteral("shapeSegment:") + band_id("kind", band)) !=
                nullptr,
            "band kind");
    for (const char* name : {"freq", "q", "gain"}) {
      require(find_item(panel->root, QStringLiteral("shapeKnob:") + band_id(name, band)) != nullptr,
              "band knob " + band_id(name, band).toStdString());
    }
    require(find_item(panel->root, QStringLiteral("shapeSegment:") + band_id("slope", band)) !=
                nullptr,
            "band slope");
  }
  // Every parameter in the table is one of those six per band.
  require(table().parameters.size() == 8 * 6, "eight bands of six");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    require(id.startsWith(QStringLiteral("shape.on_")) ||
                id.startsWith(QStringLiteral("shape.kind_")) ||
                id.startsWith(QStringLiteral("shape.freq_")) ||
                id.startsWith(QStringLiteral("shape.q_")) ||
                id.startsWith(QStringLiteral("shape.gain_")) ||
                id.startsWith(QStringLiteral("shape.slope_")),
            "a parameter outside the band strip: " + id.toStdString());
  }
}

void the_curve_is_the_eq() {
  // Flat: 0 dB everywhere, and no band curves.
  {
    InMemoryNativeSynthAdapter adapter(preset("Flat"));
    NativeSynthDeviceController controller(table(), adapter);
    const auto flat = numbers(controller.shape_response(64, -1));
    require(flat.size() == 64, "the response has the points asked for");
    for (const double db : flat) {
      require(std::fabs(db) < 1e-9, "every band off is flat");
    }
    require(controller.shape_response(64, 0).isEmpty(), "a band that is off draws nothing");
    require(controller.shape_response(64, 8).isEmpty(), "there is no ninth band");
  }
  // A bell at 1 kHz, +6 dB, Q 1 on band 4, and the low cut at 80 Hz on band 1.
  const auto values = with(preset("Low Cut"), {{"shape.on_4", 1},
                                               {"shape.kind_4", 4},
                                               {"shape.freq_4", 1'000},
                                               {"shape.q_4", 1},
                                               {"shape.gain_4", 6}});
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  const auto bell = numbers(controller.shape_response(1'024, 3));
  require(bell.size() == 1'024, "the bell's own curve");
  const auto peak = std::ranges::max_element(bell);
  require(std::fabs(*peak - 6.0) < 0.05, "a bell peaks at its gain: " + std::to_string(*peak));
  const auto at = static_cast<double>(peak - bell.begin()) / 1'023.0;
  require(std::fabs(at - controller.fraction_of(1'000.0)) < 2.0 / 1'023.0, "at its frequency");
  require(std::fabs(bell.front()) < 0.05 && std::fabs(bell.back()) < 0.3,
          "and returns to 0 dB far from it");
  // A second-order high-pass at 80 Hz: about -24 dB two octaves down.
  const auto cut = numbers(controller.shape_response(1'024, 0));
  require(cut.front() < -20.0 && cut.front() > -28.0,
          "the low cut falls 12 dB an octave: " + std::to_string(cut.front()));
  // The sum is the bands in dB.
  const auto sum = numbers(controller.shape_response(1'024, -1));
  for (std::size_t i = 0; i < sum.size(); ++i) {
    require(std::fabs(sum[i] - (bell[i] + cut[i])) < 1e-6, "the curve sums the bands in dB");
  }

  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("shapeHandle:4"))->isVisible() &&
              item(*panel, QStringLiteral("shapeHandle:1"))->isVisible(),
          "the bands that are on have handles");
  require(!item(*panel, QStringLiteral("shapeHandle:2"))->isVisible(),
          "a band that is off has none");
  require(panel->root->property("selected").toInt() == 0, "the first band that is on is selected");
  require(
      text_of(*panel, QStringLiteral("shapeCurveCaption")).startsWith(QStringLiteral("2 BANDS")),
      "the caption counts them: " +
          text_of(*panel, QStringLiteral("shapeCurveCaption")).toStdString());
  require(text_of(*panel, QStringLiteral("shapeBandFreq:4")) == QStringLiteral("1.00k"),
          "the tile reads the frequency");
  // A pass filter has no gain to turn.
  click(*panel, centre(item(*panel, QStringLiteral("shapeBand:1"))));
  require(item(*panel, QStringLiteral("shapeKnob:shape.gain_1"))->property("inactive").toBool(),
          "a pass band's gain is inactive");
  require(item(*panel, QStringLiteral("shapeSegment:shape.slope_1"))->isVisible(),
          "a pass band shows its slope");
  click(*panel, centre(item(*panel, QStringLiteral("shapeBand:2"))));
  require(item(*panel, QStringLiteral("shapeKnob:shape.q_2"))->property("label").toString() ==
              QStringLiteral("Slope"),
          "a shelf's Q is its slope");
  require(!item(*panel, QStringLiteral("shapeSegment:shape.slope_2"))->isVisible(),
          "a shelf has no pass slope");
}

// The slope steepens the drawn curve as it does the sound: two octaves
// under an 80 Hz cut is about -24 dB per 12 dB of slope, and the tile says
// how steep.
void the_slope_steepens_the_curve() {
  const std::array<double, 4> expected{-24.1, -48.2, -96.3, -192.6};
  for (int slope = 0; slope < 4; ++slope) {
    InMemoryNativeSynthAdapter adapter(with(preset("Low Cut"), {{"shape.slope_1", slope}}));
    NativeSynthDeviceController controller(table(), adapter);
    const auto cut = numbers(controller.shape_response(1'024, 0));
    // 20 Hz is two octaves under 80; the display floors far below the grid.
    require(std::fabs(cut.front() - expected[static_cast<std::size_t>(slope)]) < 1.0,
            "the curve falls by the slope: " + std::to_string(cut.front()));
  }
  InMemoryNativeSynthAdapter adapter(preset("Clean Ends"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("shapeBandKind:1")) == QStringLiteral("HP 48"),
          "the tile names a pass band's slope: " +
              text_of(*panel, QStringLiteral("shapeBandKind:1")).toStdString());
}

void handles_drag_scroll_and_add() {
  const auto values = with(preset("Flat"), {{"shape.on_4", 1}, {"shape.freq_4", 1'000}});
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* curve = item(*panel, QStringLiteral("shapeCurve"));
  auto* handle = item(*panel, QStringLiteral("shapeHandle:4"));
  const double plot_x = curve->property("plotX").toDouble();
  const double plot_w = curve->property("plotW").toDouble();
  const double plot_h = curve->property("plotH").toDouble();
  const double hx = handle->property("hx").toDouble();

  // Drag 60 px right and 40 px up: one gesture moving frequency and gain.
  const auto start = adapter.submissions().size();
  const QPoint from = centre(handle);
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from);
  QTest::mouseMove(panel->window.get(), from + QPoint(30, -20));
  QTest::mouseMove(panel->window.get(), from + QPoint(60, -40));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from + QPoint(60, -40));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a handle drag is begin, updates and commit");
  const double hz = controller.frequency_at((hx + 60.0 - plot_x) / plot_w);
  require(std::fabs(std::log2(controller.value(QStringLiteral("shape.freq_4")) / hz)) < 0.02,
          "the band follows the pointer across");
  const double db = 40.0 / plot_h * 48.0;
  require(std::fabs(controller.value(QStringLiteral("shape.gain_4")) - db) < 0.3,
          "and up: " + std::to_string(controller.value(QStringLiteral("shape.gain_4"))));
  require(panel->root->property("selected").toInt() == 3, "grabbing a handle selects its band");
  require(text_of(*panel, QStringLiteral("shapeReadout")) == QStringLiteral("BAND 4 · FREQUENCY"),
          "the display reads back the band");

  // Scroll over the handle for Q.
  const double q = controller.value(QStringLiteral("shape.q_4"));
  wheel(*panel, centre(handle), 120);
  require(controller.value(QStringLiteral("shape.q_4")) > q, "scrolling raises the Q");
  // Double-click the handle: gain back to 0 dB.
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre(handle));
  require(controller.value(QStringLiteral("shape.gain_4")) == 0.0,
          "double-clicking a handle flattens its gain");

  // Double-click the empty screen: the first free bell (band 3) comes on
  // there, in one gesture.
  auto* area = item(*panel, QStringLiteral("shapeCurveArea"));
  const QPoint spot = scene(area, area->width() * 0.25, area->height() * 0.75);
  const auto added = adapter.submissions().size();
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, spot);
  require(controller.choice(QStringLiteral("shape.on_3")) == 1, "a free bell switches on");
  require(std::fabs(std::log2(controller.value(QStringLiteral("shape.freq_3")) /
                              controller.frequency_at(0.25))) < 0.02,
          "where the screen was clicked");
  require(controller.value(QStringLiteral("shape.gain_3")) < -11.0 &&
              controller.value(QStringLiteral("shape.gain_3")) > -13.0,
          "at the level clicked");
  require(adapter.submissions()[added].gesture == NativeSynthGesture::begin &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "adding a band is one undo step");
  require(panel->root->property("selected").toInt() == 2, "and selects it");

  // The tile's switch turns a band off; its handle goes.
  click(*panel, centre(item(*panel, QStringLiteral("shapeToggle:shape.on_4"))));
  require(controller.choice(QStringLiteral("shape.on_4")) == 0, "the switch turns band 4 off");
  QCoreApplication::processEvents();
  require(!handle->isVisible(), "and its handle goes");

  // The strip's kind tiles.
  click(*panel, centre(item(*panel, QStringLiteral("shapeBand:3"))));
  auto* kind = item(*panel, QStringLiteral("shapeSegment:shape.kind_3"));
  click(*panel, scene(kind, kind->width() * 2.5 / 5.0, kind->height() / 2));
  require(controller.choice(QStringLiteral("shape.kind_3")) == 2,
          "a tile makes band 3 a low shelf");
}

// The device-chain strip draws only the output spectrum, never the packed
// frame (peaks, then the input spectrum, then the output spectrum).
void telemetry_gives_the_strip_the_output_spectrum() {
  namespace t = shape::telemetry;
  constexpr float input_band_db = -70.0F;
  constexpr float output_base_db = -60.0F;
  constexpr float output_step_db = 0.25F;
  InMemoryNativeSynthAdapter adapter(preset("Flat"));
  NativeSynthDeviceController controller(table(), adapter);
  auto frame = frame_of(-6.0F, -12.0F, -1.0F, -200.0F);
  for (std::size_t band = 0; band < shape::spectrum_bands; ++band) {
    frame[t::input_spectrum + band] = input_band_db;
    frame[t::output_spectrum + band] = output_base_db + output_step_db * static_cast<float>(band);
  }
  adapter.set_telemetry(frame);
  const auto spectrum = controller.telemetry().value(QStringLiteral("spectrum")).toList();
  require(spectrum.size() == static_cast<qsizetype>(shape::spectrum_bands),
          "one value per output band: " + std::to_string(spectrum.size()));
  for (std::size_t band = 0; band < shape::spectrum_bands; ++band) {
    const double expected = output_base_db + output_step_db * static_cast<float>(band);
    require(spectrum.at(static_cast<qsizetype>(band)).toDouble() == expected,
            "band " + std::to_string(band) + " is the output spectrum's");
  }
}

void telemetry_decodes_into_the_meters() {
  InMemoryNativeSynthAdapter adapter(preset("Flat"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  require(text_of(*panel, QStringLiteral("shapeInPeakL")) == QStringLiteral("—"),
          "a dash with nothing measured");
  require(item(*panel, QStringLiteral("shapeInMeterL"))->property("peakAt").toDouble() == 0.0,
          "an idle meter is empty");

  adapter.set_telemetry(frame_of(-6.0F, -12.0F, -1.0F, -200.0F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("input_peak")).toList().size() == 2 &&
              live.value(QStringLiteral("output_peak")).toList().at(0).toDouble() == -1.0,
          "the controller decodes Shape's frame");
  require(wait_for_text(*panel, QStringLiteral("shapeInPeakL"), QStringLiteral("−6.0")),
          "the input peak decodes: " +
              text_of(*panel, QStringLiteral("shapeInPeakL")).toStdString());
  require(text_of(*panel, QStringLiteral("shapeInPeakR")) == QStringLiteral("−12.0") &&
              text_of(*panel, QStringLiteral("shapeOutPeakL")) == QStringLiteral("−1.0") &&
              text_of(*panel, QStringLiteral("shapeOutPeakR")) == QStringLiteral("−∞"),
          "each channel's peak");
  auto* meter = item(*panel, QStringLiteral("shapeInMeterL"));
  require(std::fabs(meter->property("peakAt").toDouble() - 54.0 / 66.0) < 1e-6,
          "the bar stands at its place on a -60..+6 dB scale");
  auto* fill = find_item(meter, QStringLiteral("shapePeakMeterFill"));
  require(fill != nullptr && std::fabs(fill->height() - (meter->height() - 4) * 54.0 / 66.0) < 1e-3,
          "the fill's height follows");
  require(
      item(*panel, QStringLiteral("shapeOutPeakL"))->property("color").value<QColor>() ==
          panel->root->property("theme").value<QObject*>()->property("meterOver").value<QColor>(),
      "a hot peak is red");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("shapeInPeakL"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Flat"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("shapePresetNext"))));
  require(controller.preset_name() != QStringLiteral("Flat"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("shapePresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Flat"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("shapeMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("shapePresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Vocal"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("shapePresetName")) == QStringLiteral("Vocal"),
          "a preset shows by name");
  for (const int band : {1, 3, 5, 7}) {
    require(item(*panel, QStringLiteral("shapeHandle:%1").arg(band))->isVisible(),
            "its bands have handles");
  }
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("shapeStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;

// A bright, steady test signal: three partials, the right a little quieter.
std::vector<engine::StereoFrame> tones(std::size_t frames) {
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double s = 0.25 * std::sin(2 * std::numbers::pi * 110 * t) +
                     0.12 * std::sin(2 * std::numbers::pi * 880 * t) +
                     0.06 * std::sin(2 * std::numbers::pi * 5'000 * t);
    out[i] = {static_cast<float>(s), static_cast<float>(0.8 * s)};
  }
  return out;
}

// The real Shape over the signal; a telemetry frame every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  auto created = effects::NativeEffectRuntime::create(shape::engine(), values, rate);
  require(created.has_value(), "a Shape runtime");
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
            "Shape renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Shape publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("SHAPE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const auto input = tones(static_cast<std::size_t>(2.0 * rate));
  struct Shot {
    const char* preset;
    int selected;
    const char* file;
  };
  for (const Shot& shot :
       {Shot{"Flat", 0, "/shape_flat.png"}, Shot{"Vocal", 2, "/shape_vocal.png"},
        Shot{"Warmth", 1, "/shape_warmth.png"}, Shot{"Clean Ends", 0, "/shape_clean_ends.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->setProperty("selected", shot.selected);
    const auto frames = real_telemetry(values, input);
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

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    the_curve_is_the_eq();
    the_slope_steepens_the_curve();
    handles_drag_scroll_and_add();
    telemetry_decodes_into_the_meters();
    telemetry_gives_the_strip_the_output_spectrum();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "shape panel contract passed\n";
  return 0;
}
