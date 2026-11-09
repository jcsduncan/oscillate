#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/edge.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

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

// The Edge panel (ADR-0108, ADR-0113): every parameter on a control; the
// text fits; the transfer's ceiling line, its label and the meters' marks
// follow the ceiling;
// the subtitle and latency follow the oversampling; the switches and the
// level-matched compare edit their parameters; the transfer curve comes
// from the engine; the telemetry decodes into the meters, the dot and the
// beat history, with the plots' chips saying LIVE or PREVIEW; the real
// Edge's readings reach the panel; presets and status; and screenshots on
// request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace edge = oscillate::native_dsp::effects::edge;
namespace engine = oscillate::engine;

using oscillate::test::require;

constexpr double rate = 48'000.0;
constexpr int wait_ms = 1'000;

const oscillate::native_dsp::EngineDescriptor& table() { return *edge::engine().descriptor; }
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

std::size_t at(std::string_view id) {
  const auto found =
      std::ranges::find(table().parameters, id, &oscillate::native_dsp::ParameterDescriptor::id);
  require(found != table().parameters.end(), "Edge has " + std::string(id));
  return static_cast<std::size_t>(found - table().parameters.begin());
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
      import "qrc:/edge"
      EdgePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/EdgePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Edge panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Edge panel was not created: " + component.errorString().toStdString());
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

// A drum loop: a kick every half second (a pitch sweep with a click) and
// noise hats between, peaking at `level`.
std::vector<engine::StereoFrame> drums(double level, std::size_t frames) {
  constexpr double body_hz = 50.0;
  constexpr double sweep_hz = 200.0;
  constexpr double sweep_rate = 40.0;
  constexpr double decay_rate = 8.0;
  constexpr double hat_frames = 300.0;
  constexpr double hat_level = 0.4;
  constexpr std::uint32_t seed = 12'345;
  const auto kick_period = static_cast<std::size_t>(rate / 2.0);
  const auto hat_period = kick_period / 2;
  std::uint32_t state = seed;
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i % kick_period) / rate;
    const double phase = body_hz * t + sweep_hz * (1.0 - std::exp(-sweep_rate * t)) / sweep_rate;
    state = state * 1'664'525U + 1'013'904'223U;
    const double hiss = static_cast<double>(state >> 8U) / 8'388'608.0 - 1.0;
    const double x = std::sin(2.0 * std::numbers::pi * phase) * std::exp(-decay_rate * t) +
                     hat_level * hiss * std::exp(-static_cast<double>(i % hat_period) / hat_frames);
    out[i] = {static_cast<float>(level * x), static_cast<float>(level * x)};
  }
  return out;
}

// The real Edge over the input, playing at 120 BPM; its telemetry every
// 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  constexpr std::size_t block = 160;
  constexpr std::size_t every = 1'600;
  constexpr double bpm = 120.0;
  constexpr double seconds_per_minute = 60.0;
  auto created = effects::NativeEffectRuntime::create(edge::engine(), values, rate);
  require(created.has_value(), "an Edge runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> slice;
  const double beats_per_frame = bpm / seconds_per_minute / rate;
  for (std::size_t position = 0; position < input.size(); position += block) {
    const std::size_t size = std::min(block, input.size() - position);
    slice.assign(input.begin() + static_cast<std::ptrdiff_t>(position),
                 input.begin() + static_cast<std::ptrdiff_t>(position + size));
    const double beat = static_cast<double>(position) * beats_per_frame;
    const oscillate::native_dsp::Transport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(slice, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Edge renders");
    if ((position + size) % every == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Edge publishes telemetry");
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
    if (id == QStringLiteral("edge.compare")) {
      continue; // The header's A/B button.
    }
    bool found = false;
    for (const char* kind : {"edgeKnob:", "edgeSegmented:", "edgeToggle:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "no control for " + id.toStdString());
  }
  require(find_item(panel->root, QStringLiteral("edgeCompare")) != nullptr, "the A/B compare");
  require(text_of(*panel, QStringLiteral("edgePresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("edgeSubtitle")) ==
              QStringLiteral("CLIPPER · 8× OVERSAMPLED"),
          "the subtitle names the oversampling");
  oscillate::test::panel_fit::require_text_fits(panel->root, "edge");
}

void controls_edit_their_parameters() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString latency = text_of(*panel, QStringLiteral("edgeLatency"));
  require(latency.startsWith(QStringLiteral("Latency ")) && latency.contains(QStringLiteral("ms")),
          "the latency reads: " + latency.toStdString());

  auto* segments = item(*panel, QStringLiteral("edgeSegmented:edge.oversampling"));
  segments->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(controller.choice(QStringLiteral("edge.oversampling")) == 1, "Left steps down to 2x");
  require(wait_for_text(*panel, QStringLiteral("edgeSubtitle"),
                        QStringLiteral("CLIPPER · 2× OVERSAMPLED")),
          "the subtitle follows");
  require(text_of(*panel, QStringLiteral("edgeLatency")) != latency, "and the latency");

  click(*panel, item(*panel, QStringLiteral("edgeToggle:edge.delta")));
  require(controller.choice(QStringLiteral("edge.delta")) == 1, "the delta switch turns on");
  click(*panel, item(*panel, QStringLiteral("edgeToggle:edge.link")));
  require(controller.choice(QStringLiteral("edge.link")) == 0, "stereo link turns off");
  click(*panel, item(*panel, QStringLiteral("edgeToggle:edge.filters")));
  require(controller.choice(QStringLiteral("edge.filters")) == 1, "the light filters");

  auto* compare = item(*panel, QStringLiteral("edgeCompare"));
  require(compare->property("label").toString() == QStringLiteral("A/B matched"),
          "the compare says it is matched");
  click(*panel, compare);
  require(controller.choice(QStringLiteral("edge.compare")) == 1 &&
              compare->property("on").toBool(),
          "A/B compares");
  click(*panel, item(*panel, QStringLiteral("edgeToggle:edge.match")));
  require(
      QTest::qWaitFor(
          [&] { return compare->property("label").toString() == QStringLiteral("A/B"); }, wait_ms),
      "unmatched, the button says so");
}

// The drawn curve is the engine's: linear to the knee, then held under the
// ceiling.
void the_curve_is_the_engines() {
  constexpr int points = 37;
  constexpr double from_db = -24.0;
  constexpr double to_db = 12.0;
  constexpr double lowered_db = -6.0;
  auto values = preset("Default");
  values[at("edge.shape")] = 0.0;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  const auto hard_clip_at = [&](double ceiling_db) {
    const auto curve = controller.edge_transfer(from_db, to_db, points);
    require(curve.size() == points, "a point per input");
    for (int i = 0; i < points; ++i) {
      const double in = from_db + (to_db - from_db) * i / (points - 1);
      const double expected = std::min(in, ceiling_db);
      require(std::fabs(curve.at(i).toDouble() - expected) <
                  oscillate::test::tolerance::double_accumulation,
              "a hard clip at shape 0, point " + std::to_string(i));
    }
  };
  hard_clip_at(0.0);
  controller.set_value(QStringLiteral("edge.ceiling"), lowered_db);
  hard_clip_at(lowered_db);
  controller.set_value(QStringLiteral("edge.ceiling"), 0.0);
  // Default is 8x linear phase: 92 frames of filters and 55 of guard, 147
  // frames at 48 kHz.
  constexpr double default_latency_ms = 147.0 / 48.0;
  require(std::fabs(controller.edge_latency_ms(rate) - default_latency_ms) <
              oscillate::test::tolerance::double_rounding,
          "the latency in milliseconds: " + std::to_string(controller.edge_latency_ms(rate)));
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("edgeTransfer"))->property("curve").toList().size() > 1,
          "the panel draws it");
  require(text_of(*panel, QStringLiteral("edgeCeilingLabel")) == QStringLiteral("CEILING 0 dBFS"),
          "the ceiling's label");
  auto* label = item(*panel, QStringLiteral("edgeCeilingLabel"));
  auto* in_mark = item(*panel, QStringLiteral("edgeInPeakCeiling"));
  auto* out_mark = item(*panel, QStringLiteral("edgeOutPeakCeiling"));
  const double label_y = label->y();
  const double mark_x = out_mark->x();
  require(in_mark->isVisible() && out_mark->isVisible(), "the level bars mark the ceiling");
  controller.set_value(QStringLiteral("edge.ceiling"), lowered_db);
  require(
      wait_for_text(*panel, QStringLiteral("edgeCeilingLabel"), QStringLiteral("CEILING −6 dBFS")),
      "the label follows: " + text_of(*panel, QStringLiteral("edgeCeilingLabel")).toStdString());
  require(label->y() > label_y, "the ceiling line drops");
  require(out_mark->x() < mark_x && in_mark->x() == out_mark->x(), "and the meters' marks");
}

void telemetry_decodes_into_the_meters() {
  namespace t = edge::telemetry;
  constexpr float in_db = 4.2F;
  constexpr float out_db = -0.3F;
  constexpr float clipped_db = 3.1F;
  constexpr float latency_ms = 3.1F;
  constexpr float factor = 8.0F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("edgeClipped")) == QStringLiteral("—"),
          "a dash with nothing measured");
  require(text_of(*panel, QStringLiteral("edgePlotChipText")) == QStringLiteral("PREVIEW"),
          "idle, the plots preview the settings");
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = in_db;
  constexpr float quieter_db = 1.0F;
  frame[t::input_peak + 1] = in_db - quieter_db;
  frame[t::output_peak] = out_db;
  frame[t::output_peak + 1] = out_db - quieter_db;
  frame[t::clipped] = clipped_db;
  frame[t::dot_input] = in_db;
  frame[t::dot_output] = out_db;
  frame[t::latency_ms] = latency_ms;
  frame[t::factor] = factor;
  constexpr std::size_t beats_per_bar = 4;
  for (std::size_t b = 0; b < edge::history_beats; ++b) {
    frame[t::history + b] = static_cast<float>(b % beats_per_bar);
  }
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("history")).toList().size() ==
                  static_cast<qsizetype>(edge::history_beats),
          "the controller decodes Edge's frame");
  require(wait_for_text(*panel, QStringLiteral("edgeClipped"), QStringLiteral("−3.1 dB")),
          "clipped: " + text_of(*panel, QStringLiteral("edgeClipped")).toStdString());
  require(text_of(*panel, QStringLiteral("edgeInPeak")) == QStringLiteral("+4.2 dBFS") &&
              text_of(*panel, QStringLiteral("edgeOutPeak")) == QStringLiteral("−0.3 dBTP"),
          "the peaks: " + text_of(*panel, QStringLiteral("edgeInPeak")).toStdString());
  require(
      text_of(*panel, QStringLiteral("edgeLatency")).startsWith(QStringLiteral("Latency 3.1 ms ·")),
      "the running latency");
  require(item(*panel, QStringLiteral("edgeDot"))->isVisible(), "the dot shows");
  require(text_of(*panel, QStringLiteral("edgePlotChipText")) == QStringLiteral("LIVE"),
          "playing, the plots are live");
  oscillate::test::panel_fit::require_text_fits(panel->root, "edge playing");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("edgeClipped"), QStringLiteral("—")),
          "no telemetry: a dash again");
  require(!item(*panel, QStringLiteral("edgeDot"))->isVisible(), "and no dot");
}

// The real Edge driving drums 6 dB into the ceiling: the panel reads its
// clipping and an output true peak at or under 0 dBTP.
void the_real_edge_reaches_the_panel() {
  namespace t = edge::telemetry;
  constexpr double drive_db = 6.0;
  constexpr double seconds = 3.0;
  constexpr float least_clipped_db = 3.0F;
  auto values = preset("Default");
  values[at("edge.input")] = drive_db;
  const auto frames = real_telemetry(values, drums(1.0, static_cast<std::size_t>(seconds * rate)));
  const auto& last = frames.back();
  float most = 0.0F;
  for (const auto& frame : frames) {
    most = std::max(most, frame[t::clipped]);
    require(frame[t::output_peak] <= 0.0F && frame[t::output_peak + 1] <= 0.0F,
            "the output stays under 0 dBTP");
  }
  require(most > least_clipped_db, "it clips: " + std::to_string(most));

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(last);
  const QString expected = QStringLiteral("−") +
                           QString::number(static_cast<double>(last[t::clipped]), 'f', 1) +
                           QStringLiteral(" dB");
  require(wait_for_text(*panel, QStringLiteral("edgeClipped"), expected),
          "the panel reads the real clipping");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, item(*panel, QStringLiteral("edgePresetNext")));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, item(*panel, QStringLiteral("edgePresetPrevious")));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("edgeMenu"));
  click(*panel, item(*panel, QStringLiteral("edgePresetDisplay")));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Master"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("edgePresetName")) == QStringLiteral("Master") &&
              text_of(*panel, QStringLiteral("edgeSubtitle")) ==
                  QStringLiteral("CLIPPER · 16× OVERSAMPLED"),
          "a preset shows by name and oversampling");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("edgeStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("EDGE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  // Seven kicks, ending just after the last one lands, so the dot and the
  // meters show it.
  constexpr double seconds = 3.56;
  constexpr double source_peak = 0.891; // -1 dBFS
  constexpr int frame_ms = 33;
  constexpr int settle_ms = 150;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  // Under a ceiling 3 dB down, as on a master that feeds a limiter.
  constexpr double ceiling_db = -3.0;
  auto values = preset("Kick Punch");
  values[at("edge.ceiling")] = ceiling_db;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto frames =
      real_telemetry(values, drums(source_peak, static_cast<std::size_t>(seconds * rate)));
  for (const auto& frame : frames) {
    adapter.set_telemetry(frame);
    QTest::qWait(frame_ms);
  }
  QTest::qWait(settle_ms);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/edge.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter_and_fits();
    controls_edit_their_parameters();
    the_curve_is_the_engines();
    telemetry_decodes_into_the_meters();
    the_real_edge_reaches_the_panel();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "edge panel contract passed\n";
  return 0;
}
