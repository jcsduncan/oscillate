#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/hush.hpp"
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
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Hush panel (ADR-0116): every parameter on a control; the text fits;
// the subtitle follows the mode, flip and key; the threshold card names
// both thresholds; the range reads minus infinity at its minimum; ratio is
// inactive for a gate; the lookahead shows its latency; the switches edit
// their parameters; the telemetry decodes into the view, the meters and
// the state; the real Hush's readings reach the panel; presets and status;
// and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace hush = oscillate::native_dsp::effects::hush;
namespace engine = oscillate::engine;

using oscillate::test::require;

constexpr double rate = 48'000.0;
constexpr int wait_ms = 1'000;

const oscillate::native_dsp::EngineDescriptor& table() { return *hush::engine().descriptor; }
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
      import "qrc:/hush"
      HushPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/HushPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Hush panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Hush panel was not created: " + component.errorString().toStdString());
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

// A drum mic: a hit every half second (a decaying 180 Hz body), with the
// rest of the kit bleeding in under it at `bleed`.
std::vector<engine::StereoFrame> drum_mic(double level, double bleed, std::size_t frames) {
  constexpr double body_hz = 180.0;
  constexpr double decay_rate = 14.0;
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
    const double x =
        level * std::sin(2.0 * std::numbers::pi * body_hz * t) * std::exp(-decay_rate * t) +
        bleed * hiss;
    out[i] = {static_cast<float>(x), static_cast<float>(x)};
  }
  return out;
}

// The real Hush over the input; its telemetry every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  constexpr std::size_t block = 160;
  constexpr std::size_t every = 1'600;
  constexpr double beats_per_bar = 4.0;
  auto created = effects::NativeEffectRuntime::create(hush::engine(), values, rate);
  require(created.has_value(), "a Hush runtime");
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
            "Hush renders");
    if ((position + size) % every == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Hush publishes telemetry");
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
    for (const char* kind : {"hushKnob:", "hushSegmented:", "hushToggle:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("hushPresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("hushSubtitle")) == QStringLiteral("GATE · SELF-KEYED"),
          "the subtitle names the mode: " +
              text_of(*panel, QStringLiteral("hushSubtitle")).toStdString());
  require(text_of(*panel, QStringLiteral("hushKnobValue:hush.range")) == QStringLiteral("−∞ dB"),
          "the minimum range reads minus infinity: " +
              text_of(*panel, QStringLiteral("hushKnobValue:hush.range")).toStdString());
  require(item(*panel, QStringLiteral("hushKnob:hush.ratio"))->property("inactive").toBool(),
          "ratio is idle for a gate");
  oscillate::test::panel_fit::require_text_fits(panel->root, "hush");
}

void controls_edit_their_parameters() {
  constexpr int five_ms = 4;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("hushLatency")) ==
              QStringLiteral("No lookahead · no latency"),
          "no lookahead by default");
  controller.set_choice(QStringLiteral("hush.lookahead"), five_ms);
  require(wait_for_text(*panel, QStringLiteral("hushLatency"),
                        QStringLiteral("Opens 5.0 ms ahead · reported for delay compensation")),
          "the lookahead's latency: " +
              text_of(*panel, QStringLiteral("hushLatency")).toStdString());

  auto* mode = item(*panel, QStringLiteral("hushSegmented:hush.mode"));
  mode->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("hush.mode")) == 1, "Right picks the expander");
  require(wait_for_text(*panel, QStringLiteral("hushSubtitle"),
                        QStringLiteral("EXPANDER · SELF-KEYED")),
          "the subtitle follows");
  require(!item(*panel, QStringLiteral("hushKnob:hush.ratio"))->property("inactive").toBool(),
          "ratio shapes the expander");

  click(*panel, item(*panel, QStringLiteral("hushToggle:hush.flip")));
  require(controller.choice(QStringLiteral("hush.flip")) == 1, "flip turns on");
  require(
      wait_for_text(*panel, QStringLiteral("hushSubtitle"), QStringLiteral("DUCKER · SELF-KEYED")),
      "a ducker");
  click(*panel, item(*panel, QStringLiteral("hushToggle:hush.listen")));
  require(controller.choice(QStringLiteral("hush.listen")) == 1, "listen turns on");

  require(!item(*panel, QStringLiteral("hushKeyed"))->isVisible(), "not keyed by default");
  panel->root->setProperty("keyed", true);
  require(QTest::qWaitFor([&] { return item(*panel, QStringLiteral("hushKeyed"))->isVisible(); },
                          wait_ms),
          "a routed key shows");
  auto* key = item(*panel, QStringLiteral("hushSegmented:hush.key"));
  key->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(controller.choice(QStringLiteral("hush.key")) == 0, "Left keys from the input");
  require(QTest::qWaitFor([&] { return !item(*panel, QStringLiteral("hushKeyed"))->isVisible(); },
                          wait_ms),
          "and the badge goes");
}

void telemetry_decodes_into_the_meters() {
  namespace t = hush::telemetry;
  constexpr float in_db = -6.2F;
  constexpr float out_db = -6.4F;
  constexpr float reduction_db = 12.5F;
  constexpr float quieter_db = 1.0F;
  constexpr float open_threshold_db = -40.0F;
  constexpr float close_threshold_db = -46.0F;
  constexpr float peak = 0.5F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("hushReduction")) == QStringLiteral("—"),
          "a dash with nothing measured");
  require(text_of(*panel, QStringLiteral("hushPlotChipText")) == QStringLiteral("PREVIEW"),
          "idle, the view previews");
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = in_db;
  frame[t::input_peak + 1] = in_db - quieter_db;
  frame[t::output_peak] = out_db;
  frame[t::output_peak + 1] = out_db - quieter_db;
  frame[t::reduction] = reduction_db;
  frame[t::open] = 1.0F;
  frame[t::open_threshold] = open_threshold_db;
  frame[t::close_threshold] = close_threshold_db;
  for (std::size_t s = 0; s < hush::history_slices; ++s) {
    frame[t::history_peak + s] = peak;
    frame[t::history_gain + s] = s % 2 == 0 ? 1.0F : 0.0F;
  }
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("history_peak")).toList().size() ==
                  static_cast<qsizetype>(hush::history_slices) &&
              live.value(QStringLiteral("history_gain")).toList().size() ==
                  static_cast<qsizetype>(hush::history_slices),
          "the controller decodes Hush's frame");
  require(wait_for_text(*panel, QStringLiteral("hushReduction"), QStringLiteral("−12.5 dB")),
          "reduction: " + text_of(*panel, QStringLiteral("hushReduction")).toStdString());
  require(text_of(*panel, QStringLiteral("hushInPeak")) == QStringLiteral("−6.2 dBFS") &&
              text_of(*panel, QStringLiteral("hushOutPeak")) == QStringLiteral("−6.4 dBFS"),
          "the peaks: " + text_of(*panel, QStringLiteral("hushInPeak")).toStdString());
  require(item(*panel, QStringLiteral("hushScope"))->property("peaks").toList().size() ==
              static_cast<qsizetype>(hush::history_slices),
          "the view draws the slices");
  require(text_of(*panel, QStringLiteral("hushPlotChipText")) == QStringLiteral("LIVE"),
          "playing, the view is live");
  frame[t::reduction] = static_cast<float>(hush::reduction_floor_db);
  adapter.set_telemetry(frame);
  require(wait_for_text(*panel, QStringLiteral("hushReduction"), QStringLiteral("−∞ dB")),
          "a mute reads minus infinity");
  oscillate::test::panel_fit::require_text_fits(panel->root, "hush playing");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("hushReduction"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

// The real Hush on a snare mic: the panel reads the bleed held back.
void the_real_hush_reaches_the_panel() {
  namespace t = hush::telemetry;
  constexpr double seconds = 3.0;
  constexpr double level = 0.5;
  constexpr double bleed = 0.003;
  constexpr float least_reduction_db = 20.0F;
  const auto values = preset("Snare Bleed");
  const auto frames =
      real_telemetry(values, drum_mic(level, bleed, static_cast<std::size_t>(seconds * rate)));
  float most = 0.0F;
  bool opened = false;
  for (const auto& frame : frames) {
    most = std::max(most, frame[t::reduction]);
    opened = opened || frame[t::open] > 0.0F;
  }
  require(opened && most > least_reduction_db, "it opens and holds the bleed back");

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto& last = frames.back();
  adapter.set_telemetry(last);
  const QString expected = QStringLiteral("−") +
                           QString::number(static_cast<double>(last[t::reduction]), 'f', 1) +
                           QStringLiteral(" dB");
  require(wait_for_text(*panel, QStringLiteral("hushReduction"), expected),
          "the panel reads the real reduction");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, item(*panel, QStringLiteral("hushPresetNext")));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, item(*panel, QStringLiteral("hushPresetPrevious")));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("hushMenu"));
  click(*panel, item(*panel, QStringLiteral("hushPresetDisplay")));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Soft Expander"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("hushPresetName")) == QStringLiteral("Soft Expander") &&
              text_of(*panel, QStringLiteral("hushSubtitle")) ==
                  QStringLiteral("EXPANDER · SELF-KEYED"),
          "a preset shows by name and mode");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("hushStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("HUSH_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  // Long enough to fill the view, ending just after a hit lands.
  constexpr double seconds = 3.52;
  constexpr double level = 0.5;
  constexpr double bleed = 0.01;
  constexpr int frame_ms = 33;
  constexpr int settle_ms = 150;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const auto values = preset("Snare Bleed");
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto frames =
      real_telemetry(values, drum_mic(level, bleed, static_cast<std::size_t>(seconds * rate)));
  for (const auto& frame : frames) {
    adapter.set_telemetry(frame);
    QTest::qWait(frame_ms);
  }
  QTest::qWait(settle_ms);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/hush.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter_and_fits();
    controls_edit_their_parameters();
    telemetry_decodes_into_the_meters();
    the_real_hush_reaches_the_panel();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "hush panel contract passed\n";
  return 0;
}
