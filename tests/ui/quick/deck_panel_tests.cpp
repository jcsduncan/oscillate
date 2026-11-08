#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/deck.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/test/check.hpp"

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
#include <vector>

// The Deck panel (ADR-0055): every parameter on a control, each band's
// state (its gain, or KILLED), the telemetry decoded into the band level
// bars and the output peak meters, and screenshots on request, live from
// the real Deck running a drum-and-bass test loop.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace deck = oscillate::native_dsp::effects::deck;
namespace engine = oscillate::engine;

using oscillate::test::require;

constexpr double rate = 48'000.0;
constexpr std::size_t block_frames = 160;
// The panel polls at about 30 Hz while playing.
constexpr std::size_t poll_frames = 1'600;
constexpr int poll_ms = 33;
constexpr int settle_ms = 150;
constexpr int wait_ms = 1'000;
constexpr double tempo_beats_per_second = 2.0;
constexpr double beats_per_bar = 4.0;
constexpr double loop_seconds = 2.0;
// A test level on the -60..0 dBFS bars: -15 dB fills three quarters.
constexpr float band_level_db = -15.0F;
constexpr double band_level_fill = 0.75;
constexpr double fill_tolerance = 1e-6;
constexpr float output_peak_db = -6.0F;

const oscillate::native_dsp::EngineDescriptor& table() { return *deck::engine().descriptor; }

std::vector<double> preset(std::string_view name) {
  for (const auto& entry : table().presets) {
    if (entry.name == name) {
      return oscillate::native_dsp::preset_values(table(), entry).value();
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
      import "qrc:/deck"
      DeckPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/DeckPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Deck panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Deck panel was not created: " + component.errorString().toStdString());
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

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// The filled share of a band's level bar.
double band_fill(Panel& panel, const QString& band) {
  auto* bar = item(panel, QStringLiteral("deckBandLevel:") + band);
  const auto children = bar->childItems();
  require(!children.isEmpty(), "the level bar has its fill");
  return children.front()->width() / bar->width();
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Flat"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    const bool shown = find_item(panel->root, QStringLiteral("deckKnob:") + id) != nullptr ||
                       find_item(panel->root, QStringLiteral("deckToggle:") + id) != nullptr;
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("deckPresetName")) == QStringLiteral("Flat"),
          "the panel names its preset");
}

void each_band_shows_its_state() {
  InMemoryNativeSynthAdapter adapter(preset("Warm"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("deckBandState:low")) == QStringLiteral("+2.0 dB"),
          "Warm lifts the low band: " +
              text_of(*panel, QStringLiteral("deckBandState:low")).toStdString());
  require(text_of(*panel, QStringLiteral("deckBandState:high")) == QStringLiteral("−3.0 dB"),
          "and cuts the high band");
  require(text_of(*panel, QStringLiteral("deckBandState:mid")) == QStringLiteral("0.0 dB"),
          "the mid band stays flat");
  controller.set_value(QStringLiteral("deck.low_kill"), 1.0);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("deckBandState:low")) == QStringLiteral("KILLED"),
          "a killed band says so");
}

void the_telemetry_drives_the_meters() {
  InMemoryNativeSynthAdapter adapter(preset("Flat"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(band_fill(*panel, QStringLiteral("low")) == 0.0, "stopped, the bars are empty");
  std::vector<float> frame(deck::telemetry::size, band_level_db);
  frame[deck::telemetry::output_peak] = output_peak_db;
  frame[deck::telemetry::output_peak + 1] = output_peak_db;
  adapter.set_telemetry(frame);
  require(QTest::qWaitFor([&] { return panel->root->property("playing").toBool(); }, wait_ms),
          "telemetry: playing");
  for (const char* band : {"low", "mid", "high"}) {
    require(std::fabs(band_fill(*panel, QString::fromLatin1(band)) - band_level_fill) <
                fill_tolerance,
            std::string("the ") + band + " bar reads its level");
  }
  for (const char* side : {"deckOutputPeakL", "deckOutputPeakR"}) {
    auto* meter = item(*panel, QString::fromLatin1(side));
    require(meter->property("live").toBool() &&
                std::fabs(meter->property("peakDb").toDouble() - output_peak_db) < fill_tolerance,
            std::string(side) + " reads the output peak");
  }
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !panel->root->property("playing").toBool(); }, wait_ms),
          "telemetry gone: still");
  require(band_fill(*panel, QStringLiteral("mid")) == 0.0, "and the bars empty");
}

// ---- Screenshots ----------------------------------------------------------------

// A test loop at 120 BPM: a decaying 55 Hz kick on each beat, a 440 Hz
// pad and a quiet hiss, so each band carries something.
engine::StereoFrame loop_frame(std::size_t frame, std::uint32_t& noise) {
  constexpr double kick_hz = 55.0;
  constexpr double kick_decay_per_second = 12.0;
  constexpr double kick_level = 0.6;
  constexpr double pad_hz = 440.0;
  constexpr double pad_level = 0.15;
  constexpr double hiss_level = 0.08;
  constexpr std::uint32_t lcg_multiplier = 1'664'525U;
  constexpr std::uint32_t lcg_increment = 1'013'904'223U;
  constexpr double lcg_scale = 4'294'967'296.0;
  const double seconds = static_cast<double>(frame) / rate;
  const double beat_seconds =
      std::fmod(seconds * tempo_beats_per_second, 1.0) / tempo_beats_per_second;
  const double kick = kick_level * std::exp(-kick_decay_per_second * beat_seconds) *
                      std::sin(2.0 * std::numbers::pi * kick_hz * beat_seconds);
  const double pad = pad_level * std::sin(2.0 * std::numbers::pi * pad_hz * seconds);
  noise = noise * lcg_multiplier + lcg_increment;
  const double hiss = hiss_level * (2.0 * (static_cast<double>(noise) / lcg_scale) - 1.0);
  const auto sample = static_cast<float>(kick + pad + hiss);
  return {sample, sample};
}

// The real Deck over the loop; the telemetry every poll.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               std::size_t stop) {
  auto created = effects::NativeEffectRuntime::create(deck::engine(), values, rate);
  require(created.has_value(), "a Deck runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  std::uint32_t noise = 1;
  const double beats_per_frame = tempo_beats_per_second / rate;
  for (std::size_t position = 0; position < stop; position += block_frames) {
    const std::size_t size = std::min(block_frames, stop - position);
    block.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      block[i] = loop_frame(position + i, noise);
    }
    const double beat = static_cast<double>(position) * beats_per_frame;
    const oscillate::native_dsp::Transport transport{beat, beats_per_frame, beat / beats_per_bar,
                                                     beats_per_bar};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Deck renders");
    if ((position + size) % poll_frames == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Deck publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("DECK_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  constexpr std::size_t shown_polls = 10;
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"Warm", "deck.low", "/deck_warm.png"},
                          Shot{"Bass Swap (Low Killed)", "deck.low_kill", "/deck_bass_swap.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    const auto frames = real_telemetry(values, static_cast<std::size_t>(loop_seconds * rate));
    for (std::size_t i = frames.size() > shown_polls ? frames.size() - shown_polls : 0;
         i < frames.size(); ++i) {
      adapter.set_telemetry(frames[i]);
      QTest::qWait(poll_ms);
    }
    QTest::qWait(settle_ms);
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
    each_band_shows_its_state();
    the_telemetry_drives_the_meters();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "deck panel contract passed\n";
  return 0;
}
