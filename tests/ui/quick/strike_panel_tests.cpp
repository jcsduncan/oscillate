#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/strike.hpp"
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

// The Strike panel (ADR-0118): every parameter on a control; the text fits;
// the subtitle follows the band count and the phase; a band column per band,
// the unused crossovers idle; the lookahead, phase and clip show their
// latency; the switches edit their parameters; the telemetry decodes into
// the view, the meters and each band's gain; the real Strike's readings
// reach the panel; presets and status; and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace strike = oscillate::native_dsp::effects::strike;
namespace engine = oscillate::engine;

using oscillate::test::require;

constexpr double rate = 48'000.0;
constexpr int wait_ms = 1'000;
constexpr int three_bands = 2;
constexpr int linear_phase = 1;
constexpr int lookahead_2ms = 3;

const oscillate::native_dsp::EngineDescriptor& table() { return *strike::engine().descriptor; }
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
      import "qrc:/strike"
      StrikePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/StrikePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Strike panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Strike panel was not created: " + component.errorString().toStdString());
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

// The panel's one-decimal reading of a signed value: a true minus sign.
QString signed_text(double value, const QString& unit) {
  const QString number = QString::number(std::fabs(value), 'f', 1);
  return (value < 0.0 ? QStringLiteral("−") : QStringLiteral("+")) + number + unit;
}

// A drum bus: a kick (a falling 60 Hz body) on every beat at 124 BPM, a
// snare (a 200 Hz body over noise) on two and four, over a little room.
std::vector<engine::StereoFrame> drum_bus(std::size_t frames) {
  constexpr double bpm = 124.0;
  constexpr double seconds_per_minute = 60.0;
  constexpr double kick_hz = 60.0;
  constexpr double kick_decay = 0.08;
  constexpr double kick_level = 0.5;
  constexpr double snare_hz = 200.0;
  constexpr double snare_decay = 0.06;
  constexpr double snare_level = 0.3;
  constexpr double room = 0.01;
  constexpr std::uint32_t seed = 12'345;
  constexpr std::uint32_t multiplier = 1'664'525U;
  constexpr std::uint32_t increment = 1'013'904'223U;
  constexpr unsigned shift = 8U;
  constexpr double scale = 8'388'608.0;
  const auto beat = static_cast<std::size_t>(seconds_per_minute / bpm * rate);
  std::uint32_t state = seed;
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    state = state * multiplier + increment;
    const double hiss = static_cast<double>(state >> shift) / scale - 1.0;
    const double t = static_cast<double>(i % beat) / rate;
    const bool backbeat = (i / beat) % 2 == 1;
    double x =
        kick_level * std::exp(-t / kick_decay) * std::sin(2.0 * std::numbers::pi * kick_hz * t) +
        room * hiss;
    if (backbeat) {
      x += snare_level * std::exp(-t / snare_decay) *
           (std::sin(2.0 * std::numbers::pi * snare_hz * t) + hiss);
    }
    out[i] = {static_cast<float>(x), static_cast<float>(x)};
  }
  return out;
}

// The real Strike over the input; its telemetry every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  constexpr std::size_t block = 160;
  constexpr std::size_t every = 1'600;
  constexpr double beats_per_bar = 4.0;
  auto created = effects::NativeEffectRuntime::create(strike::engine(), values, rate);
  require(created.has_value(), "a Strike runtime");
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
            "Strike renders");
    if ((position + size) % every == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Strike publishes telemetry");
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
    for (const char* kind : {"strikeKnob:", "strikeSegmented:", "strikeToggle:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("strikePresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("strikeSubtitle")) ==
              QStringLiteral("TRANSIENTS · SINGLE BAND"),
          "the subtitle names the bands: " +
              text_of(*panel, QStringLiteral("strikeSubtitle")).toStdString());
  require(item(*panel, QStringLiteral("strikeKnob:strike.xover_1"))->property("inactive").toBool(),
          "one band uses no crossover");
  require(item(*panel, QStringLiteral("strikeKnob:strike.ceiling"))->property("inactive").toBool(),
          "the ceiling is idle without Clip");
  require(!item(*panel, QStringLiteral("strikeKnob:strike.band_2.attack"))->isVisible(),
          "one band shows one column");
  oscillate::test::panel_fit::require_text_fits(panel->root, "strike");
}

void controls_edit_their_parameters() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("strikeLatency")) == QStringLiteral("No latency"),
          "no latency by default");
  controller.set_choice(QStringLiteral("strike.lookahead"), lookahead_2ms);
  require(wait_for_text(*panel, QStringLiteral("strikeLatency"),
                        QStringLiteral("Latency 2.0 ms · reported for delay compensation")),
          "the lookahead's latency: " +
              text_of(*panel, QStringLiteral("strikeLatency")).toStdString());

  auto* bands = item(*panel, QStringLiteral("strikeSegmented:strike.bands"));
  bands->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("strike.bands")) == three_bands, "three bands");
  auto* phase = item(*panel, QStringLiteral("strikeSegmented:strike.phase"));
  phase->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("strike.phase")) == linear_phase, "linear phase");
  require(wait_for_text(*panel, QStringLiteral("strikeSubtitle"),
                        QStringLiteral("TRANSIENTS · 3 BANDS · LINEAR PHASE")),
          "the subtitle follows");
  require(
      !item(*panel, QStringLiteral("strikeKnob:strike.xover_2"))->property("inactive").toBool() &&
          item(*panel, QStringLiteral("strikeKnob:strike.xover_3"))->property("inactive").toBool(),
      "three bands use two crossovers");
  require(item(*panel, QStringLiteral("strikeKnob:strike.band_3.attack"))->isVisible() &&
              !item(*panel, QStringLiteral("strikeKnob:strike.band_4.attack"))->isVisible(),
          "three band columns");
  // 2 ms of lookahead (96 frames) and the bank's 1023 + 512 frames at
  // 48 kHz: 1631 frames, 33.98 ms.
  require(wait_for_text(*panel, QStringLiteral("strikeLatency"),
                        QStringLiteral("Latency 34.0 ms · reported for delay compensation")),
          "the bank's latency joins the lookahead's: " +
              text_of(*panel, QStringLiteral("strikeLatency")).toStdString());

  click(*panel, item(*panel, QStringLiteral("strikeToggle:strike.clip")));
  require(controller.choice(QStringLiteral("strike.clip")) == 1, "clip turns on");
  require(QTest::qWaitFor(
              [&] {
                return !item(*panel, QStringLiteral("strikeKnob:strike.ceiling"))
                            ->property("inactive")
                            .toBool();
              },
              wait_ms),
          "the ceiling wakes");
  click(*panel, item(*panel, QStringLiteral("strikeToggle:strike.band_2.solo")));
  require(controller.choice(QStringLiteral("strike.band_2.solo")) == 1, "band 2 solos");
  oscillate::test::panel_fit::require_text_fits(panel->root, "strike three bands");
}

void telemetry_decodes_into_the_view() {
  namespace t = strike::telemetry;
  constexpr float in_db = -6.2F;
  constexpr float out_db = -4.4F;
  constexpr float quieter_db = 1.0F;
  constexpr float band_gain_db = 4.5F;
  constexpr float peak = 0.5F;
  constexpr float lift_db = 6.0F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("strikeInPeak")) == QStringLiteral("—"),
          "a dash with nothing measured");
  require(text_of(*panel, QStringLiteral("strikePlotChipText")) == QStringLiteral("PREVIEW"),
          "idle, the view previews");
  require(text_of(*panel, QStringLiteral("strikeClipped")) == QStringLiteral("OFF"), "clip is off");
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = in_db;
  frame[t::input_peak + 1] = in_db - quieter_db;
  frame[t::output_peak] = out_db;
  frame[t::output_peak + 1] = out_db - quieter_db;
  frame[t::band_count] = 1.0F;
  frame[t::band_gain] = band_gain_db;
  for (std::size_t s = 0; s < strike::history_slices; ++s) {
    frame[t::history_input + s] = peak;
    frame[t::history_output + s] = peak;
    frame[t::history_gain + s] = s % 2 == 0 ? lift_db : -lift_db;
  }
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  const auto gains = live.value(QStringLiteral("history_gain")).toList();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("history_input")).toList().size() ==
                  static_cast<qsizetype>(strike::history_slices) &&
              gains.size() == static_cast<qsizetype>(strike::max_bands) &&
              gains.front().toList().size() == static_cast<qsizetype>(strike::history_slices),
          "the controller decodes Strike's frame");
  require(wait_for_text(*panel, QStringLiteral("strikeInPeak"), QStringLiteral("−6.2 dBFS")),
          "in peak: " + text_of(*panel, QStringLiteral("strikeInPeak")).toStdString());
  require(text_of(*panel, QStringLiteral("strikeOutPeak")) == QStringLiteral("−4.4 dBFS"),
          "out peak: " + text_of(*panel, QStringLiteral("strikeOutPeak")).toStdString());
  require(text_of(*panel, QStringLiteral("strikeBandGain:0")) == QStringLiteral("+4.5 dB"),
          "band gain: " + text_of(*panel, QStringLiteral("strikeBandGain:0")).toStdString());
  require(item(*panel, QStringLiteral("strikeView"))->property("inputs").toList().size() ==
              static_cast<qsizetype>(strike::history_slices),
          "the view draws the slices");
  require(text_of(*panel, QStringLiteral("strikePlotChipText")) == QStringLiteral("LIVE"),
          "playing, the view is live");
  oscillate::test::panel_fit::require_text_fits(panel->root, "strike playing");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("strikeInPeak"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

// The real Strike on a drum bus: the panel reads the band gains it applies.
void the_real_strike_reaches_the_panel() {
  namespace t = strike::telemetry;
  constexpr double seconds = 2.0;
  constexpr float least_lift_db = 1.0F;
  const auto values = preset("Drum Bus Punch");
  const auto frames = real_telemetry(values, drum_bus(static_cast<std::size_t>(seconds * rate)));
  const auto& last = frames.back();
  float most = 0.0F;
  for (std::size_t s = 0; s < strike::history_slices; ++s) {
    most = std::max(most, last[t::history_gain + strike::history_slices + s]);
  }
  require(most > least_lift_db, "the middle band lifts the snare");
  require(last[t::band_count] == static_cast<float>(three_bands + 1), "three bands");

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(last);
  require(wait_for_text(*panel, QStringLiteral("strikeBandGain:1"),
                        signed_text(last[t::band_gain + 1], QStringLiteral(" dB"))),
          "the panel reads the real band gain: " +
              text_of(*panel, QStringLiteral("strikeBandGain:1")).toStdString());
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, item(*panel, QStringLiteral("strikePresetNext")));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, item(*panel, QStringLiteral("strikePresetPrevious")));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("strikeMenu"));
  click(*panel, item(*panel, QStringLiteral("strikePresetDisplay")));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Master Impact"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("strikePresetName")) == QStringLiteral("Master Impact") &&
              text_of(*panel, QStringLiteral("strikeSubtitle")) ==
                  QStringLiteral("TRANSIENTS · 4 BANDS · LINEAR PHASE"),
          "a preset shows by name and bands");
  oscillate::test::panel_fit::require_text_fits(panel->root, "strike four bands");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("strikeStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("STRIKE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  // Long enough to fill the view.
  constexpr double seconds = 2.5;
  constexpr int frame_ms = 33;
  constexpr int settle_ms = 150;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const auto values = preset("Drum Bus Punch");
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto frames = real_telemetry(values, drum_bus(static_cast<std::size_t>(seconds * rate)));
  for (const auto& frame : frames) {
    adapter.set_telemetry(frame);
    QTest::qWait(frame_ms);
  }
  QTest::qWait(settle_ms);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/strike.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter_and_fits();
    controls_edit_their_parameters();
    telemetry_decodes_into_the_view();
    the_real_strike_reaches_the_panel();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "strike panel contract passed\n";
  return 0;
}
