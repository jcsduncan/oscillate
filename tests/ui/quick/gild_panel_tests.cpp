#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/gild.hpp"
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

// The Gild panel (ADR-0122): every parameter on a control; the text fits;
// the subtitle follows the band count, the phase and the oversampling; a
// band column per band, the unused crossovers idle; the oversampling and
// phase show their latency; the switches edit their parameters; the
// telemetry decodes into the spectrum, the meters and each band's readings;
// the real Gild's readings reach the panel; presets and status; and
// screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace gild = oscillate::native_dsp::effects::gild;
namespace engine = oscillate::engine;

using oscillate::test::require;

constexpr double rate = 48'000.0;
constexpr int wait_ms = 1'000;
constexpr int two_bands = 1;
constexpr int linear_phase = 1;
constexpr int x1 = 0;
constexpr int x4 = 2;
constexpr double milliseconds = 1'000.0;

const oscillate::native_dsp::EngineDescriptor& table() { return *gild::engine().descriptor; }
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
      import "qrc:/gild"
      GildPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/GildPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Gild panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Gild panel was not created: " + component.errorString().toStdString());
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

QString latency_text(bool linear, int factor) {
  const double ms =
      static_cast<double>(gild::latency_frames(linear, static_cast<std::size_t>(factor), rate)) /
      rate * milliseconds;
  return QStringLiteral("Latency ") + QString::number(ms, 'f', 1) +
         QStringLiteral(" ms · reported for delay compensation");
}

// A mix: a bass line, a chord's partials, a kick every beat at 124 BPM and
// a little hiss.
std::vector<engine::StereoFrame> programme(std::size_t frames) {
  constexpr double bpm = 124.0;
  constexpr double seconds_per_minute = 60.0;
  constexpr std::array partial_hz{55.0, 220.0, 330.0, 440.0, 1'320.0, 2'640.0};
  constexpr std::array partial_level{0.25, 0.08, 0.06, 0.05, 0.02, 0.01};
  constexpr double kick_hz = 60.0;
  constexpr double kick_decay = 0.08;
  constexpr double kick_level = 0.3;
  constexpr double hiss = 0.02;
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
    const double noise = static_cast<double>(state >> shift) / scale - 1.0;
    const double t = static_cast<double>(i) / rate;
    const double since = static_cast<double>(i % beat) / rate;
    double x = hiss * noise + kick_level * std::exp(-since / kick_decay) *
                                  std::sin(2.0 * std::numbers::pi * kick_hz * since);
    for (std::size_t p = 0; p < partial_hz.size(); ++p) {
      x += partial_level[p] * std::sin(2.0 * std::numbers::pi * partial_hz[p] * t);
    }
    out[i] = {static_cast<float>(x), static_cast<float>(x)};
  }
  return out;
}

// The real Gild over the input; its telemetry every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  constexpr std::size_t block = 160;
  constexpr std::size_t every = 1'600;
  constexpr double beats_per_bar = 4.0;
  auto created = effects::NativeEffectRuntime::create(gild::engine(), values, rate);
  require(created.has_value(), "a Gild runtime");
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
            "Gild renders");
    if ((position + size) % every == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Gild publishes telemetry");
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
    for (const char* kind : {"gildKnob:", "gildSegmented:", "gildToggle:", "gildDropdown:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("gildPresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("gildSubtitle")) ==
              QStringLiteral("EXCITER · 4 BANDS · MINIMUM PHASE · 4×"),
          "the subtitle names the bands: " +
              text_of(*panel, QStringLiteral("gildSubtitle")).toStdString());
  require(!item(*panel, QStringLiteral("gildKnob:gild.xover_3"))->property("inactive").toBool(),
          "four bands use every crossover");
  require(item(*panel, QStringLiteral("gildKnob:gild.band_4.drive"))->isVisible(),
          "four band columns");
  oscillate::test::panel_fit::require_text_fits(panel->root, "gild");
}

void controls_edit_their_parameters() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("gildLatency")) == latency_text(false, x4),
          "the oversampler's latency: " +
              text_of(*panel, QStringLiteral("gildLatency")).toStdString());
  auto* oversampling = item(*panel, QStringLiteral("gildSegmented:gild.oversampling"));
  oversampling->forceActiveFocus();
  for (int step = x4; step > x1; --step) {
    QTest::keyClick(panel->window.get(), Qt::Key_Left);
  }
  require(controller.choice(QStringLiteral("gild.oversampling")) == x1, "no oversampling");
  require(wait_for_text(*panel, QStringLiteral("gildLatency"), QStringLiteral("No latency")),
          "1x has no latency: " + text_of(*panel, QStringLiteral("gildLatency")).toStdString());

  auto* bands = item(*panel, QStringLiteral("gildSegmented:gild.bands"));
  bands->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(controller.choice(QStringLiteral("gild.bands")) == two_bands, "two bands");
  auto* phase = item(*panel, QStringLiteral("gildSegmented:gild.phase"));
  phase->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("gild.phase")) == linear_phase, "linear phase");
  require(wait_for_text(*panel, QStringLiteral("gildSubtitle"),
                        QStringLiteral("EXCITER · 2 BANDS · LINEAR PHASE · 1×")),
          "the subtitle follows: " + text_of(*panel, QStringLiteral("gildSubtitle")).toStdString());
  require(!item(*panel, QStringLiteral("gildKnob:gild.xover_1"))->property("inactive").toBool() &&
              item(*panel, QStringLiteral("gildKnob:gild.xover_2"))->property("inactive").toBool(),
          "two bands use one crossover");
  require(item(*panel, QStringLiteral("gildKnob:gild.band_2.drive"))->isVisible() &&
              !item(*panel, QStringLiteral("gildKnob:gild.band_3.drive"))->isVisible(),
          "two band columns");
  require(wait_for_text(*panel, QStringLiteral("gildLatency"), latency_text(true, x1)),
          "the bank's latency: " + text_of(*panel, QStringLiteral("gildLatency")).toStdString());

  click(*panel, item(*panel, QStringLiteral("gildToggle:gild.delta")));
  require(controller.choice(QStringLiteral("gild.delta")) == 1, "delta turns on");
  click(*panel, item(*panel, QStringLiteral("gildToggle:gild.match")));
  require(controller.choice(QStringLiteral("gild.match")) == 0, "level match turns off");
  click(*panel, item(*panel, QStringLiteral("gildToggle:gild.band_2.solo")));
  require(controller.choice(QStringLiteral("gild.band_2.solo")) == 1, "band 2 solos");
  oscillate::test::panel_fit::require_text_fits(panel->root, "gild two bands");
}

void telemetry_decodes_into_the_view() {
  namespace t = gild::telemetry;
  constexpr float in_db = -6.2F;
  constexpr float out_db = -4.4F;
  constexpr float quieter_db = 1.0F;
  constexpr float band_level_db = -12.0F;
  constexpr float band_added_db = -32.5F;
  constexpr float band_drive_db = 18.0F;
  constexpr float spectrum_db = -40.0F;
  constexpr float silent_db = -120.0F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("gildInPeak")) == QStringLiteral("—"),
          "a dash with nothing measured");
  require(text_of(*panel, QStringLiteral("gildPlotChipText")) == QStringLiteral("PREVIEW"),
          "idle, the view previews");
  std::vector<float> frame(t::size, silent_db);
  frame[t::input_peak] = in_db;
  frame[t::input_peak + 1] = in_db - quieter_db;
  frame[t::output_peak] = out_db;
  frame[t::output_peak + 1] = out_db - quieter_db;
  frame[t::latency_ms] = 0.0F;
  frame[t::band_count] = static_cast<float>(gild::max_bands);
  for (std::size_t k = 0; k < gild::max_crossovers; ++k) {
    frame[t::crossovers + k] = 0.0F;
  }
  frame[t::band_level] = band_level_db;
  frame[t::band_added] = band_added_db;
  frame[t::band_drive] = band_drive_db;
  for (std::size_t i = 0; i < gild::spectrum_bins; ++i) {
    frame[t::input_spectrum + i] = spectrum_db;
    frame[t::added_spectrum + i] = spectrum_db;
  }
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("added_spectrum")).toList().size() ==
                  static_cast<qsizetype>(gild::spectrum_bins) &&
              live.value(QStringLiteral("band_added")).toList().size() ==
                  static_cast<qsizetype>(gild::max_bands),
          "the controller decodes Gild's frame");
  require(wait_for_text(*panel, QStringLiteral("gildInPeak"), QStringLiteral("−6.2 dBFS")),
          "in peak: " + text_of(*panel, QStringLiteral("gildInPeak")).toStdString());
  require(text_of(*panel, QStringLiteral("gildOutPeak")) == QStringLiteral("−4.4 dBFS"),
          "out peak: " + text_of(*panel, QStringLiteral("gildOutPeak")).toStdString());
  require(text_of(*panel, QStringLiteral("gildBandAdded:0")) ==
              QStringLiteral("ADDS ") +
                  signed_text(band_added_db - band_level_db, QStringLiteral(" dB")),
          "band 1 adds: " + text_of(*panel, QStringLiteral("gildBandAdded:0")).toStdString());
  require(text_of(*panel, QStringLiteral("gildBandAdded:1")) == QStringLiteral("ADDS —"),
          "band 2 adds nothing");
  require(text_of(*panel, QStringLiteral("gildBandDrive:0")) ==
              QStringLiteral("DRIVE ") + signed_text(band_drive_db, QStringLiteral(" dB")),
          "band 1's drive: " + text_of(*panel, QStringLiteral("gildBandDrive:0")).toStdString());
  require(text_of(*panel, QStringLiteral("gildAdded")) ==
              signed_text(band_added_db, QStringLiteral(" dBFS")),
          "all that is added: " + text_of(*panel, QStringLiteral("gildAdded")).toStdString());
  require(item(*panel, QStringLiteral("gildView"))->property("added").toList().size() ==
              static_cast<qsizetype>(gild::spectrum_bins),
          "the view draws the spectrum");
  require(text_of(*panel, QStringLiteral("gildPlotChipText")) == QStringLiteral("LIVE"),
          "playing, the view is live");
  oscillate::test::panel_fit::require_text_fits(panel->root, "gild playing");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("gildInPeak"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

// The real Gild on a mix: the panel reads what the warm band adds.
void the_real_gild_reaches_the_panel() {
  namespace t = gild::telemetry;
  constexpr double seconds = 2.0;
  constexpr std::size_t warm_band = 1;
  constexpr float audible_db = -80.0F;
  const auto values = preset("Warm Mids");
  const auto frames = real_telemetry(values, programme(static_cast<std::size_t>(seconds * rate)));
  const auto& last = frames.back();
  require(last[t::band_added + warm_band] > audible_db, "the middle band adds");
  require(last[t::band_count] == 3.0F, "three bands");

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(last);
  const QString expected =
      QStringLiteral("ADDS ") +
      signed_text(last[t::band_added + warm_band] - last[t::band_level + warm_band],
                  QStringLiteral(" dB"));
  require(wait_for_text(*panel, QStringLiteral("gildBandAdded:1"), expected),
          "the panel reads what the real band adds: " +
              text_of(*panel, QStringLiteral("gildBandAdded:1")).toStdString());
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, item(*panel, QStringLiteral("gildPresetNext")));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, item(*panel, QStringLiteral("gildPresetPrevious")));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("gildMenu"));
  click(*panel, item(*panel, QStringLiteral("gildPresetDisplay")));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Tape Glue"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("gildPresetName")) == QStringLiteral("Tape Glue") &&
              text_of(*panel, QStringLiteral("gildSubtitle")) ==
                  QStringLiteral("EXCITER · 4 BANDS · LINEAR PHASE · 4×"),
          "a preset shows by name and bands: " +
              text_of(*panel, QStringLiteral("gildSubtitle")).toStdString());
  oscillate::test::panel_fit::require_text_fits(panel->root, "gild four bands");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("gildStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("GILD_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  constexpr double seconds = 2.5;
  constexpr int frame_ms = 33;
  constexpr int settle_ms = 150;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const auto values = preset("Tape Glue");
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto frames = real_telemetry(values, programme(static_cast<std::size_t>(seconds * rate)));
  for (const auto& frame : frames) {
    adapter.set_telemetry(frame);
    QTest::qWait(frame_ms);
  }
  QTest::qWait(settle_ms);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/gild.png");
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
    the_real_gild_reaches_the_panel();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "gild panel contract passed\n";
  return 0;
}
