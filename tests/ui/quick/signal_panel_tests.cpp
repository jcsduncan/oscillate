#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/signal.hpp"
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
#include <vector>

// The Signal panel (ADR-0042): every parameter on a table-driven control
// (the waves as pictured tiles, the power switch); the display's digits,
// note, level and its kind (peak or RMS) for each wave; the telemetry
// decode into the live sweep (frequency, note, progress); the timing knob
// the wave uses, and only that; the routing diagram for each output;
// presets and status; and screenshots on request, live from the real
// Signal.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace signal = oscillate::native_dsp::effects::signal;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *signal::engine().descriptor; }
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
      import "qrc:/signal"
      SignalPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/SignalPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Signal panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Signal panel was not created: " + component.errorString().toStdString());
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

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, 1000);
}

std::vector<float> frame_of(float hz, float progress) {
  std::vector<float> frame(signal::telemetry::size, 0.0F);
  frame[signal::telemetry::frequency] = hz;
  frame[signal::telemetry::progress] = progress;
  return frame;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("1 kHz Reference"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 720,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"signalKnob:", "signalSegment:", "signalToggle:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("signalPresetName")) == QStringLiteral("1 kHz Reference"),
          "the panel names its preset");
}

void the_display_reads_each_wave() {
  InMemoryNativeSynthAdapter adapter(preset("1 kHz Reference"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  // A tone: its frequency in four figures, the note against A4 = 440 Hz,
  // the level as a peak.
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("1.000") &&
              text_of(*panel, QStringLiteral("signalBigUnit")) == QStringLiteral("kHz"),
          "1 kHz: " + text_of(*panel, QStringLiteral("signalBig")).toStdString());
  require(text_of(*panel, QStringLiteral("signalSub")) == QStringLiteral("B5 +21 ct"),
          "1 kHz is B5 +21 cents: " + text_of(*panel, QStringLiteral("signalSub")).toStdString());
  require(text_of(*panel, QStringLiteral("signalLevel")) == QStringLiteral("−18.0") &&
              text_of(*panel, QStringLiteral("signalLevelKind")) == QStringLiteral("dBFS PEAK"),
          "the level, as a peak");
  controller.set_value(QStringLiteral("signal.frequency"), 440.0);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("440.0") &&
              text_of(*panel, QStringLiteral("signalSub")) == QStringLiteral("A4") &&
              text_of(*panel, QStringLiteral("signalBigUnit")) == QStringLiteral("Hz"),
          "440 Hz is A4 exactly");
  controller.set_value(QStringLiteral("signal.frequency"), 40.0);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("40.00") &&
              text_of(*panel, QStringLiteral("signalSub")) == QStringLiteral("D♯1 +49 ct"),
          "40 Hz: " + text_of(*panel, QStringLiteral("signalSub")).toStdString());

  // Noise: its colour, and the level as RMS.
  controller.apply_preset(preset_index("Pink Noise"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("PINK") &&
              text_of(*panel, QStringLiteral("signalLevelKind")) == QStringLiteral("dBFS RMS"),
          "pink noise, by RMS");
  require(text_of(*panel, QStringLiteral("signalNote")) == QStringLiteral("SINE ONLY") &&
              item(*panel, QStringLiteral("signalKnob:signal.frequency"))
                  ->property("inactive")
                  .toBool(),
          "the frequency knob rests for noise");
  controller.apply_preset(preset_index("White Noise"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("WHITE"), "white");

  // Impulses: the rate and the period.
  controller.apply_preset(preset_index("Impulse Train"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("1.00") &&
              text_of(*panel, QStringLiteral("signalSub")) ==
                  QStringLiteral("ONE IMPULSE EVERY 1.00 s"),
          "impulses: " + text_of(*panel, QStringLiteral("signalSub")).toStdString());
  require(text_of(*panel, QStringLiteral("signalLevelKind")) == QStringLiteral("dBFS PEAK"),
          "impulses by peak");

  // Off: the input passes.
  click(*panel, centre(item(*panel, QStringLiteral("signalToggle:signal.on"))));
  require(controller.choice(QStringLiteral("signal.on")) == 0, "the power switch turns it off");
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("OFF") &&
              text_of(*panel, QStringLiteral("signalSub")) ==
                  QStringLiteral("THE INPUT PASSES UNTOUCHED"),
          "off says so");
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("signal.on")) == 1, "Space turns it on again");
}

void the_sweep_follows_the_telemetry() {
  InMemoryNativeSynthAdapter adapter(preset("Log Sweep"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  require(text_of(*panel, QStringLiteral("signalBig")) == QStringLiteral("20 → 20k") &&
              text_of(*panel, QStringLiteral("signalSub")) ==
                  QStringLiteral("EXPONENTIAL · 10.0 s A PASS"),
          "before it runs, the sweep's range: " +
              text_of(*panel, QStringLiteral("signalSub")).toStdString());
  adapter.set_telemetry(frame_of(1234.0F, 0.25F));
  require(wait_for_text(*panel, QStringLiteral("signalBig"), QStringLiteral("1.234")),
          "the live frequency: " + text_of(*panel, QStringLiteral("signalBig")).toStdString());
  require(text_of(*panel, QStringLiteral("signalSub")) ==
              QStringLiteral("D♯6 −15 ct  ·  25 % OF 10.0 s"),
          "its note and progress: " + text_of(*panel, QStringLiteral("signalSub")).toStdString());
  auto* bar = item(*panel, QStringLiteral("signalProgress"));
  require(std::fabs(bar->width() - (bar->parentItem()->width() - 2) * 0.25) < 1e-3,
          "the progress bar fills a quarter");
  auto* axis = item(*panel, QStringLiteral("signalAxis"));
  require(axis->property("live").toBool() &&
              std::fabs(axis->property("hz").toDouble() - 1234.0) < 1e-3,
          "the axis marks the live frequency");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("signalBig"), QStringLiteral("20 → 20k")),
          "no telemetry: the range again");
}

void timing_shows_what_the_wave_uses() {
  InMemoryNativeSynthAdapter adapter(preset("1 kHz Reference"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* sweep = item(*panel, QStringLiteral("signalKnob:signal.sweep_seconds"));
  auto* rate = item(*panel, QStringLiteral("signalKnob:signal.impulse_rate"));
  require(!sweep->isVisible() && !rate->isVisible(), "a tone has no timing control");
  // The wave tiles pick the wave.
  click(*panel, centre(item(*panel, QStringLiteral("signalWave:1"))));
  require(controller.choice(QStringLiteral("signal.wave")) == 1, "the sweep tile picks it");
  require(sweep->isVisible() && !rate->isVisible(), "the sweep shows its time");
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("signal.wave")) == 4, "Right steps the tiles");
  require(!sweep->isVisible() && rate->isVisible(), "impulses show their rate");
  require(text_of(*panel, QStringLiteral("signalReadout")) == QStringLiteral("SIGNAL · WAVE"),
          "the display reads back the wave: " +
              text_of(*panel, QStringLiteral("signalReadout")).toStdString());
}

void routing_is_drawn() {
  InMemoryNativeSynthAdapter adapter(preset("1 kHz Reference"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* left = item(*panel, QStringLiteral("signalSpeakerL"));
  auto* right = item(*panel, QStringLiteral("signalSpeakerR"));
  require(left->property("plays").toBool() && right->property("plays").toBool(), "both play");
  auto* output = item(*panel, QStringLiteral("signalSegment:signal.output"));
  click(*panel, scene(output, output->width() * 1.5 / 5.0, output->height() / 2));
  require(controller.choice(QStringLiteral("signal.output")) == 1, "a tile picks Left");
  require(left->property("plays").toBool() && !right->property("plays").toBool() &&
              right->property("tag").toString() == QStringLiteral("SILENT"),
          "Left: the right speaker is silent");
  controller.apply_preset(preset_index("Polarity Check"));
  QCoreApplication::processEvents();
  require(right->property("inverted").toBool() &&
              right->property("tag").toString() == QStringLiteral("INVERTED") &&
              !left->property("inverted").toBool(),
          "Right Inverted marks the right speaker");
  controller.apply_preset(preset_index("Channel Check"));
  QCoreApplication::processEvents();
  require(left->property("tag").toString() == QStringLiteral("SECONDS 1, 3…") &&
              right->property("tag").toString() == QStringLiteral("SECONDS 2, 4…"),
          "Alternate names the turns");
  auto* mode = item(*panel, QStringLiteral("signalSegment:signal.mode"));
  click(*panel, scene(mode, mode->width() * 0.75, mode->height() / 2));
  require(controller.choice(QStringLiteral("signal.mode")) == 1, "a tile picks Add");
  require(text_of(*panel, QStringLiteral("signalOutputName")) == QStringLiteral("ADD · ALTERNATE"),
          "the display names the routing");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("1 kHz Reference"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("signalPresetNext"))));
  require(controller.preset_name() != QStringLiteral("1 kHz Reference"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("signalPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("1 kHz Reference"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("signalMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("signalPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  auto* level = item(*panel, QStringLiteral("signalKnob:signal.level"));
  level->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("signal.level")) > -18.0, "Up raises the level");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("signalStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;

// The real Signal over silence until `stop`; the telemetry frames every
// 1600 frames (a 30 Hz poll).
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               std::size_t stop) {
  auto created = effects::NativeEffectRuntime::create(signal::engine(), values, rate);
  require(created.has_value(), "a Signal runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  for (std::size_t position = 0; position < stop; position += 160) {
    const std::size_t size = std::min<std::size_t>(160, stop - position);
    block.assign(size, {0.0F, 0.0F});
    const double beats_per_frame = 2.0 / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const synth::SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Signal renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Signal publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("SIGNAL_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    double seconds;
    const char* touched;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"1 kHz Reference", 1.0, "signal.frequency", "/signal_sine_1k.png"},
        Shot{"Log Sweep", 6.2, "signal.sweep_seconds", "/signal_sweep.png"},
        Shot{"Pink Noise", 1.0, "signal.wave", "/signal_pink.png"},
        Shot{"Polarity Check", 1.0, "signal.output", "/signal_polarity.png"},
        Shot{"Impulse Train", 1.0, "signal.impulse_rate", "/signal_impulses.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    const auto frames = real_telemetry(values, static_cast<std::size_t>(shot.seconds * rate));
    for (std::size_t i = frames.size() > 10 ? frames.size() - 10 : 0; i < frames.size(); ++i) {
      adapter.set_telemetry(frames[i]);
      QTest::qWait(33);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    const auto& last = frames.back();
    std::cout << "wrote " << path.toStdString() << " (" << last[signal::telemetry::frequency]
              << " Hz, progress " << last[signal::telemetry::progress] << ")\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    the_display_reads_each_wave();
    the_sweep_follows_the_telemetry();
    timing_shows_what_the_wave_uses();
    routing_is_drawn();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "signal panel contract passed\n";
  return 0;
}
