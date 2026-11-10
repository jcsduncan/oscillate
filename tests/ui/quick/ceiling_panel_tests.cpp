#include "device_strip_view.hpp"
#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/test/audio_constants.hpp"
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
#include <array>
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

// The Ceiling panel (ADR-0117): every parameter on a table-driven control;
// the ceiling readout and its line across the output meters; the latency
// in the header; the telemetry decode into the reduction now, its history
// and its largest, the peak meters, the loudness, what the clipper and the
// guard took; Learn moving Gain to the target; the real Ceiling over a
// full-scale tone holding the output at the ceiling; presets and status;
// and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace ceiling = oscillate::native_dsp::effects::ceiling;
namespace engine = oscillate::engine;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *ceiling::engine().descriptor; }
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
      import "qrc:/ceiling"
      CeilingPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/CeilingPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Ceiling panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Ceiling panel was not created: " + component.errorString().toStdString());
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

// The loudness readings a test frame carries unless a test sets its own.
constexpr float frame_integrated = -9.0F;
constexpr float frame_short_term = -8.5F;
constexpr float frame_momentary = -8.0F;
constexpr float frame_clipped = 1.2F;
constexpr float frame_guarded = 0.3F;

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<float> frame_of(float in_l, float in_r, float out_l, float out_r, float reduction) {
  namespace t = ceiling::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::integrated] = frame_integrated;
  frame[t::short_term] = frame_short_term;
  frame[t::momentary] = frame_momentary;
  frame[t::clipped] = frame_clipped;
  frame[t::guarded] = frame_guarded;
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
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 620,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    require(find_item(panel->root, QStringLiteral("ceilingKnob:") + id) != nullptr,
            "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("ceilingPresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("ceilingValue")) == QStringLiteral("−1.0"),
          "the ceiling reads in large");
  constexpr double nominal_rate = oscillate::test::sample_rate_48k_hz;
  const double latency_ms = controller.ceiling_latency_ms(nominal_rate);
  require(latency_ms > 0.0, "the controller reads Ceiling's latency");
  require(text_of(*panel, QStringLiteral("ceilingLatency")) ==
              QStringLiteral("TRUE-PEAK LIMITER · ") + QString::number(latency_ms, 'f', 1) +
                  QStringLiteral(" ms LATENCY"),
          "the header names the latency: " +
              text_of(*panel, QStringLiteral("ceilingLatency")).toStdString());
  // It ends before the preset browser rather than running under it.
  oscillate::test::panel_fit::require_clear_of(
      *item(*panel, QStringLiteral("ceilingLatency")),
      *item(*panel, QStringLiteral("ceilingPresetPrevious"))->parentItem(), "the preset browser");
}

void controls_edit_and_the_ceiling_line_follows() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* knob = item(*panel, QStringLiteral("ceilingKnob:ceiling.ceiling"));
  knob->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(controller.value(QStringLiteral("ceiling.ceiling")) < -1.0, "Down lowers the ceiling");
  require(text_of(*panel, QStringLiteral("ceilingReadout")) == QStringLiteral("LIMIT · CEILING"),
          "the display reads back the ceiling");
  controller.set_value(QStringLiteral("ceiling.ceiling"), -6.0);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("ceilingValue")) == QStringLiteral("−6.0"), "−6.0 reads");
  // On the -60..+6 dB meters, -6 dB stands at 54/66 of the way up.
  auto* line = item(*panel, QStringLiteral("ceilingOutCeilingLine"));
  auto* meter = item(*panel, QStringLiteral("ceilingOutMeterL"));
  require(line->isVisible() && !item(*panel, QStringLiteral("ceilingInCeilingLine"))->isVisible(),
          "the ceiling is drawn on the output only");
  const double top = meter->mapToItem(line->parentItem(), QPointF(0, 0)).y();
  const double expected = top + 2 + (meter->height() - 4) * (1 - 54.0 / 66.0) - 1;
  require(std::fabs(line->y() - expected) < 0.5, "the line stands at the ceiling");
  require(
      !item(*panel, QStringLiteral("ceilingKnob:ceiling.lookahead"))->property("inactive").toBool(),
      "lookahead is a live control");
}

void telemetry_decodes_into_the_history() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  require(text_of(*panel, QStringLiteral("ceilingReduction")) == QStringLiteral("—") &&
              text_of(*panel, QStringLiteral("ceilingReductionLargest")) == QStringLiteral("—"),
          "dashes with nothing measured");

  adapter.set_telemetry(frame_of(0.0F, -2.0F, -1.0F, -3.0F, 3.5F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("reduction")).toDouble() == 3.5,
          "the controller decodes Ceiling's frame");
  require(wait_for_text(*panel, QStringLiteral("ceilingReduction"), QStringLiteral("−3.5")),
          "the reduction now: " +
              text_of(*panel, QStringLiteral("ceilingReduction")).toStdString());
  require(
      QTest::qWaitFor([&] { return panel->root->property("samples").toList().size() >= 3; }, 1'000),
      "the history gathers while playing");
  adapter.set_telemetry(frame_of(0.0F, -2.0F, -1.0F, -3.0F, 1.0F));
  require(wait_for_text(*panel, QStringLiteral("ceilingReduction"), QStringLiteral("−1.0")),
          "the reduction follows");
  require(text_of(*panel, QStringLiteral("ceilingReductionLargest")) == QStringLiteral("−3.5"),
          "the largest is kept");
  require(text_of(*panel, QStringLiteral("ceilingOutPeakL")) == QStringLiteral("−1.0") &&
              text_of(*panel, QStringLiteral("ceilingInPeakR")) == QStringLiteral("−2.0"),
          "the peak meters");
  require(text_of(*panel, QStringLiteral("ceilingIntegrated")) == QStringLiteral("−9.0") &&
              text_of(*panel, QStringLiteral("ceilingShortTerm")) == QStringLiteral("−8.5") &&
              text_of(*panel, QStringLiteral("ceilingMomentary")) == QStringLiteral("−8.0"),
          "the loudness readings");
  require(text_of(*panel, QStringLiteral("ceilingToTarget")) == QStringLiteral("+5.0 LU to target"),
          "the distance to the -14 LUFS target: " +
              text_of(*panel, QStringLiteral("ceilingToTarget")).toStdString());
  require(text_of(*panel, QStringLiteral("ceilingClipped")) == QStringLiteral("−1.2 dB") &&
              text_of(*panel, QStringLiteral("ceilingGuarded")) == QStringLiteral("−0.3 dB"),
          "what the clipper and the guard took");

  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("ceilingReduction"), QStringLiteral("—")),
          "no telemetry: a dash again");
  const auto kept = panel->root->property("samples").toList().size();
  QTest::qWait(300);
  require(panel->root->property("samples").toList().size() == kept,
          "the history waits while stopped");
  click(*panel, centre(item(*panel, QStringLiteral("ceilingClearHistory"))));
  require(panel->root->property("samples").toList().isEmpty() &&
              text_of(*panel, QStringLiteral("ceilingReductionLargest")) == QStringLiteral("—"),
          "clear empties it");
}

// ---- The real limiter -----------------------------------------------------------

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

// The real Ceiling over the input; a telemetry frame every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  auto created = effects::NativeEffectRuntime::create(ceiling::engine(), values, rate);
  require(created.has_value(), "a Ceiling runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  for (std::size_t position = 0; position < input.size(); position += 160) {
    const std::size_t size = std::min<std::size_t>(160, input.size() - position);
    block.assign(input.begin() + static_cast<std::ptrdiff_t>(position),
                 input.begin() + static_cast<std::ptrdiff_t>(position + size));
    const oscillate::native_dsp::Transport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Ceiling renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Ceiling publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

// A full-scale tone under a -1 dB ceiling: a little over 1 dB of reduction
// (sustained material sits just under the clipper's knee), and the output's
// true peak held at the ceiling.
constexpr float least_reduction_db = 0.8F;
constexpr float most_reduction_db = 2.2F;
constexpr float held_at_ceiling_db = -0.95F;
constexpr float under_ceiling_db = -2.0F;
void the_real_ceiling_holds_the_output() {
  const auto values = preset("Default");
  const auto frames = real_telemetry(values, sine(1.0, static_cast<std::size_t>(2.0 * rate)));
  const auto& last = frames.back();
  namespace t = ceiling::telemetry;
  require(std::fabs(last[t::input_peak]) < 0.05, "a full-scale input");
  require(last[t::reduction] > least_reduction_db && last[t::reduction] < most_reduction_db,
          "about 1 dB of reduction: " + std::to_string(last[t::reduction]));
  require(last[t::output_peak] <= held_at_ceiling_db && last[t::output_peak] > under_ceiling_db,
          "the output held at the ceiling: " + std::to_string(last[t::output_peak]));

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(last);
  const QString expected =
      QStringLiteral("−") + QString::number(static_cast<double>(last[t::reduction]), 'f', 1);
  require(wait_for_text(*panel, QStringLiteral("ceilingReduction"), expected),
          "the panel reads the real reduction");
}

// Gain drives a quiet mix into the limiter: a -12 dBFS tone with 12 dB of
// gain comes out at the ceiling, about 11 dB louder, the IN meter still
// reading the tone before the gain.
void gain_drives_the_mix_into_the_ceiling() {
  auto values = preset("Default");
  const auto gain = std::ranges::find(table().parameters, std::string_view{"ceiling.gain"},
                                      &oscillate::native_dsp::ParameterDescriptor::id);
  require(gain != table().parameters.end(), "Ceiling has a gain");
  values[static_cast<std::size_t>(gain - table().parameters.begin())] = 12.0;
  const double quiet = std::pow(10.0, -12.0 / 20.0);
  const auto frames = real_telemetry(values, sine(quiet, static_cast<std::size_t>(2.0 * rate)));
  const auto& last = frames.back();
  namespace t = ceiling::telemetry;
  require(std::fabs(last[t::input_peak] + 12.0F) < 0.1F,
          "IN reads the mix before the gain: " + std::to_string(last[t::input_peak]));
  require(last[t::output_peak] <= held_at_ceiling_db && last[t::output_peak] > under_ceiling_db,
          "the gained tone is held at the ceiling: " + std::to_string(last[t::output_peak]));
  require(last[t::reduction] > least_reduction_db && last[t::reduction] < most_reduction_db,
          "the gain pushes about 1 dB into the limiter: " + std::to_string(last[t::reduction]));

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("gainValue")) == QStringLiteral("+12.0"),
          "the gain reads in large");
}

// Learn moves Gain by the distance from the integrated loudness to the
// target; with nothing measured it does nothing.
void learn_moves_gain_to_the_target() {
  constexpr float quiet_integrated = -18.0F;
  constexpr double target = -14.0;
  constexpr double learned_gain = 4.0;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* learn = item(*panel, QStringLiteral("ceilingLearn"));
  click(*panel, centre(learn));
  require(controller.value(QStringLiteral("ceiling.gain")) == 0.0, "nothing measured, no change");
  require(controller.value(QStringLiteral("ceiling.target")) == target, "the default target");
  auto frame = frame_of(-6.0F, -6.0F, -1.0F, -1.0F, 2.0F);
  frame[ceiling::telemetry::integrated] = quiet_integrated;
  adapter.set_telemetry(frame);
  require(QTest::qWaitFor([&] { return learn->property("active").toBool(); }, 1'000),
          "Learn wakes with a reading");
  click(*panel, centre(learn));
  require(std::fabs(controller.value(QStringLiteral("ceiling.gain")) - learned_gain) < 1e-9,
          "Gain moves 4 dB toward the target");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("ceilingPresetNext"))));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("ceilingPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("ceilingMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("ceilingPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Loud Club"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("ceilingPresetName")) == QStringLiteral("Loud Club") &&
              text_of(*panel, QStringLiteral("ceilingValue")) == QStringLiteral("−0.3"),
          "a preset shows by name and ceiling");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("ceilingStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("CEILING_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* file;
  };
  // Six seconds of a kick on every beat at 124 BPM under a held chord,
  // swelling from -12 dBFS to +3 dBFS.
  constexpr double seconds = 6.0;
  constexpr double bpm = 124.0;
  constexpr double seconds_per_minute = 60.0;
  constexpr double kick_hz = 55.0;
  constexpr double kick_decay = 0.12;
  constexpr double chord_level = 0.25;
  constexpr std::array chord_hz{220.0, 277.18, 329.63};
  constexpr double floor_db = -12.0;
  constexpr double swell_db = 15.0;
  constexpr double decibels_per_decade = 20.0;
  std::vector<engine::StereoFrame> input(static_cast<std::size_t>(seconds * rate));
  const double beat = seconds_per_minute / bpm;
  for (std::size_t i = 0; i < input.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    const double since = std::fmod(t, beat);
    const double db = floor_db + swell_db * std::sin(std::numbers::pi * t / (2.0 * seconds));
    double x = std::sin(2 * std::numbers::pi * kick_hz * since) * std::exp(-since / kick_decay);
    for (const double hz : chord_hz) {
      x += chord_level * std::sin(2 * std::numbers::pi * hz * t) /
           static_cast<double>(chord_hz.size());
    }
    const auto s = static_cast<float>(std::pow(10.0, db / decibels_per_decade) * x);
    input[i] = {s, 0.9F * s};
  }
  for (const Shot& shot :
       {Shot{"Default", "/ceiling_default.png"}, Shot{"Loud Club", "/ceiling_loud.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    for (const auto& frame : real_telemetry(values, input)) {
      adapter.set_telemetry(frame);
      QTest::qWait(33);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

// The device-chain strip (device_strip_view.hpp): Ceiling's reduction per
// poll as a history on the panel's range, now and its largest as text, and
// the band that leads the detector, until playback stops.
void telemetry_gives_the_strip_the_reduction_and_its_band() {
  namespace t = ceiling::telemetry;
  namespace strip_view = oscillate::quick::strip_view;
  constexpr float peak_db = -6.0F;
  constexpr std::array<float, 3> polls{1.0F, 4.1F, 2.3F};
  // A kick: band 1 leads; the others sit under least_leading_share.
  constexpr std::size_t kick_band = 1;
  constexpr float kick_share = 0.6F;
  constexpr float other_share = 0.04F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  const auto with_reduction = [&](float db) {
    auto frame = frame_of(peak_db, peak_db, peak_db, peak_db, db);
    for (std::size_t band = 0; band < effects::reduction_bands::count; ++band)
      frame[t::reduction_bands + band] = band == kick_band ? kick_share : other_share;
    return frame;
  };
  const auto poll = [&] { return controller.telemetry().value(QStringLiteral("strip")).toMap(); };
  QVariantMap strip;
  for (const float db : polls) {
    adapter.set_telemetry(with_reduction(db));
    strip = poll();
  }
  require(strip.value(QStringLiteral("kind")).toString() == QStringLiteral("reduction"),
          "Ceiling's strip is a reduction view");
  const auto history = strip.value(QStringLiteral("history")).toList();
  require(history.size() == static_cast<qsizetype>(polls.size()), "one history point per poll");
  for (std::size_t i = 0; i < polls.size(); ++i)
    require(history.at(static_cast<qsizetype>(i)).toDouble() == static_cast<double>(polls[i]),
            "the history is the reduction per poll, oldest first");
  require(strip.value(QStringLiteral("range_db")).toDouble() == strip_view::ceiling_range_db,
          "on the Ceiling panel's range");
  require(strip.value(QStringLiteral("text")).toString() == QStringLiteral("−2.3 dB · max −4.1"),
          "now and the largest: " + strip.value(QStringLiteral("text")).toString().toStdString());
  require(strip.value(QStringLiteral("lead")).toInt() == static_cast<int>(kick_band) &&
              strip.value(QStringLiteral("lead_text")).toString() == QStringLiteral("47.4–112 Hz"),
          "the kick's band leads: " +
              strip.value(QStringLiteral("lead_text")).toString().toStdString());
  require(strip.value(QStringLiteral("bands")).toList().size() ==
              static_cast<qsizetype>(effects::reduction_bands::count),
          "every band's share");
  for (std::size_t i = 0; i < strip_view::history_points; ++i)
    strip = poll();
  require(strip.value(QStringLiteral("history")).toList().size() ==
              static_cast<qsizetype>(strip_view::history_points),
          "the history keeps its last few seconds");
  adapter.set_telemetry({});
  require(!controller.telemetry().contains(QStringLiteral("strip")), "nothing plays, no strip");
  adapter.set_telemetry(with_reduction(polls.front()));
  require(poll().value(QStringLiteral("history")).toList().size() == 1,
          "a new run starts a new history");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    controls_edit_and_the_ceiling_line_follows();
    telemetry_decodes_into_the_history();
    the_real_ceiling_holds_the_output();
    gain_drives_the_mix_into_the_ceiling();
    learn_moves_gain_to_the_target();
    presets_and_status();
    telemetry_gives_the_strip_the_reduction_and_its_band();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "ceiling panel contract passed\n";
  return 0;
}
