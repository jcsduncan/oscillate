#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/cast.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The Cast panel (ADR-0115): every parameter on a control; the text fits;
// the subtitle names the IR; the browser picks a bundled IR into the
// document; the telemetry decodes into the response view, its readings and
// the meters; the real Cast's readings reach the panel; presets and status;
// and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace cast = oscillate::native_dsp::effects::cast;
namespace engine = oscillate::engine;

using oscillate::test::require;

constexpr double rate = 48'000.0;
constexpr int wait_ms = 1'000;
// The bundled IRs' routing (cast::telemetry::responses).
constexpr float true_stereo_responses = 4.0F;

const oscillate::native_dsp::EngineDescriptor& table() { return *cast::engine().descriptor; }
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
      import "qrc:/cast"
      CastPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/CastPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Cast panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Cast panel was not created: " + component.errorString().toStdString());
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

void click(Panel& panel, QQuickItem* target) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, centre(target));
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, wait_ms);
}

// Snare-like bursts of noise every half second, decaying fast.
std::vector<engine::StereoFrame> bursts(double level, std::size_t frames) {
  constexpr double decay_rate = 30.0;
  constexpr std::uint32_t seed = 12'345;
  constexpr std::uint32_t multiplier = 1'664'525U;
  constexpr std::uint32_t increment = 1'013'904'223U;
  constexpr unsigned shift = 8U;
  constexpr double scale = 8'388'608.0;
  const auto period = static_cast<std::size_t>(rate / 2.0);
  std::uint32_t state = seed;
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i % period) / rate;
    state = state * multiplier + increment;
    const double hiss = static_cast<double>(state >> shift) / scale - 1.0;
    const auto x = static_cast<float>(level * hiss * std::exp(-decay_rate * t));
    out[i] = {x, x};
  }
  return out;
}

// The real Cast over the input with a preset's IR; its telemetry every 1600
// frames.
std::vector<std::vector<float>> real_telemetry(std::string_view preset_name,
                                               const std::vector<engine::StereoFrame>& input) {
  constexpr std::size_t block = 160;
  constexpr std::size_t every = 1'600;
  constexpr double beats_per_bar = 4.0;
  const auto index = static_cast<std::size_t>(preset_index(preset_name));
  auto created = effects::NativeEffectRuntime::create(cast::engine(), preset(preset_name),
                                                      cast::engine().preset_document(index), rate);
  require(created.has_value(), "a Cast runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> slice;
  for (std::size_t position = 0; position < input.size(); position += block) {
    const std::size_t size = std::min(block, input.size() - position);
    slice.assign(input.begin() + static_cast<std::ptrdiff_t>(position),
                 input.begin() + static_cast<std::ptrdiff_t>(position + size));
    const oscillate::native_dsp::Transport transport{0.0, 0.0, 0.0, beats_per_bar};
    require(runtime->process(slice, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Cast renders");
    if ((position + size) % every == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Cast publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter_and_fits() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool found = false;
    for (const char* kind : {"castKnob:", "castSegmented:", "castToggle:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("castPresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("castSubtitle")) ==
              QStringLiteral("CONVOLUTION · CONCERT HALL"),
          "an empty document plays the hall: " +
              text_of(*panel, QStringLiteral("castSubtitle")).toStdString());
  for (const auto& ir : cast::builtin_irs()) {
    require(find_item(panel->root, QStringLiteral("castIr:") + qs(ir.id)) != nullptr,
            "the browser lists " + std::string(ir.id));
  }
  require(!item(*panel, QStringLiteral("castLoadRight"))->property("active").toBool(),
          "a right input needs a stereo file first");
  oscillate::test::panel_fit::require_text_fits(panel->root, "cast");
}

void the_browser_picks_an_ir() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* list = item(*panel, QStringLiteral("castIrList"));
  QQuickItem* plate = nullptr;
  require(QTest::qWaitFor(
              [&] {
                QMetaObject::invokeMethod(list, "positionViewAtEnd");
                plate = find_item(panel->root, QStringLiteral("castIr:plate"));
                return plate != nullptr && plate->isVisible();
              },
              wait_ms),
          "the plate is in the list");
  click(*panel, plate);
  const auto document = adapter.document();
  require(document.media.size() == 1 && document.media.front().id == cast::ir_slot,
          "one media slot, the IR");
  const auto* builtin =
      std::get_if<oscillate::project::NativeBuiltinMedia>(&document.media.front().media);
  require(builtin != nullptr && builtin->name == "plate", "the plate is in the document");
  require(
      wait_for_text(*panel, QStringLiteral("castSubtitle"), QStringLiteral("CONVOLUTION · PLATE")),
      "the subtitle follows: " + text_of(*panel, QStringLiteral("castSubtitle")).toStdString());
  require(text_of(*panel, QStringLiteral("castDropHint")) ==
              QStringLiteral("Or drop a WAV, FLAC or AIFF on the panel"),
          "the browser says files can be dropped");
}

void telemetry_decodes_into_the_view() {
  namespace t = cast::telemetry;
  constexpr float in_db = -6.2F;
  constexpr float wet_db = -14.5F;
  constexpr float duck_db = 4.5F;
  constexpr float length = 2.5F;
  constexpr float rt60 = 2.05F;
  constexpr float quieter_db = 1.0F;
  constexpr float decay_per_point_db = 0.5F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("castLength")) == QStringLiteral("—"),
          "a dash with nothing measured");
  require(text_of(*panel, QStringLiteral("castPlotChipText")) == QStringLiteral("PREVIEW"),
          "idle, the view previews");
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = in_db;
  frame[t::input_peak + 1] = in_db - quieter_db;
  frame[t::wet_peak] = wet_db;
  frame[t::wet_peak + 1] = wet_db - quieter_db;
  frame[t::duck] = duck_db;
  frame[t::length_seconds] = length;
  frame[t::responses] = true_stereo_responses;
  frame[t::rt60_seconds] = rt60;
  for (std::size_t p = 0; p < cast::overview_points; ++p) {
    frame[t::envelope + p] = -decay_per_point_db * static_cast<float>(p);
    frame[t::decay_curve + p] = -decay_per_point_db * static_cast<float>(p);
  }
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("envelope")).toList().size() ==
                  static_cast<qsizetype>(cast::overview_points),
          "the controller decodes Cast's frame");
  require(wait_for_text(*panel, QStringLiteral("castLength"), QStringLiteral("2.50 s")),
          "length: " + text_of(*panel, QStringLiteral("castLength")).toStdString());
  require(text_of(*panel, QStringLiteral("castRt60")) == QStringLiteral("2.05 s") &&
              text_of(*panel, QStringLiteral("castRouting")) == QStringLiteral("TRUE STEREO"),
          "rt60 and routing");
  require(text_of(*panel, QStringLiteral("castInPeak")) == QStringLiteral("−6.2 dBFS") &&
              text_of(*panel, QStringLiteral("castWetPeak")) == QStringLiteral("−14.5 dBFS") &&
              text_of(*panel, QStringLiteral("castDuck")) == QStringLiteral("−4.5 dB"),
          "the meters: " + text_of(*panel, QStringLiteral("castInPeak")).toStdString());
  require(item(*panel, QStringLiteral("castIrView"))->property("envelope").toList().size() ==
              static_cast<qsizetype>(cast::overview_points),
          "the view draws the IR");
  require(text_of(*panel, QStringLiteral("castPlotChipText")) == QStringLiteral("LIVE"),
          "playing, the view is live");
  oscillate::test::panel_fit::require_text_fits(panel->root, "cast playing");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("castLength"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

// The real Cast: the panel reads the hall's length and routing.
void the_real_cast_reaches_the_panel() {
  namespace t = cast::telemetry;
  constexpr double seconds = 1.0;
  constexpr double level = 0.5;
  const auto frames =
      real_telemetry("Concert Hall", bursts(level, static_cast<std::size_t>(seconds * rate)));
  const auto& last = frames.back();
  require(last[t::length_seconds] > 0.0F && last[t::responses] == true_stereo_responses,
          "the hall plays");
  InMemoryNativeSynthAdapter adapter(preset("Concert Hall"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(last);
  const QString expected =
      QString::number(static_cast<double>(last[t::length_seconds]), 'f', 2) + QStringLiteral(" s");
  require(wait_for_text(*panel, QStringLiteral("castLength"), expected),
          "the panel reads the real length");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, item(*panel, QStringLiteral("castPresetNext")));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, item(*panel, QStringLiteral("castPresetPrevious")));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("castMenu"));
  click(*panel, item(*panel, QStringLiteral("castPresetDisplay")));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("castStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("CAST_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  constexpr double seconds = 1.6;
  constexpr double level = 0.5;
  constexpr int frame_ms = 33;
  constexpr int settle_ms = 150;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  InMemoryNativeSynthAdapter adapter(preset("Vocal Plate"));
  adapter.submit_document(
      cast::engine().preset_document(static_cast<std::size_t>(preset_index("Vocal Plate"))),
      oscillate::quick::NativeSynthGesture::single);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto frames =
      real_telemetry("Vocal Plate", bursts(level, static_cast<std::size_t>(seconds * rate)));
  for (const auto& frame : frames) {
    adapter.set_telemetry(frame);
    QTest::qWait(frame_ms);
  }
  QTest::qWait(settle_ms);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/cast.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter_and_fits();
    the_browser_picks_an_ir();
    telemetry_decodes_into_the_view();
    the_real_cast_reaches_the_panel();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "cast panel contract passed\n";
  return 0;
}
