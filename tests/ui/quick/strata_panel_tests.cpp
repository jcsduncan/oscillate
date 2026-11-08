#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/strata.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
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
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Strata panel (ADR-0082): a control for every parameter (globals,
// crossovers, every band's controls when it is selected); the transfer curve
// from the engine's own gain computer; adding a band by double-clicking the
// spectrum and removing one; OTT taken over by the first touch of a band,
// without changing the sound; the telemetry decode and Learn writing its
// suggestions as thresholds; presets; and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace strata = oscillate::native_dsp::effects::strata;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *strata::engine().descriptor; }

std::size_t index(std::string_view id) {
  const auto found = synth::find_parameter(table(), id);
  if (!found) {
    throw std::runtime_error("unknown parameter " + std::string(id));
  }
  return *found;
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

QString band_id(int b, const char* field) {
  return QStringLiteral("strata.band_%1.%2").arg(b + 1).arg(QString::fromLatin1(field));
}

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
      import "qrc:/strata"
      StrataPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/StrataPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Strata panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Strata panel was not created: " + component.errorString().toStdString());
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
  if (parent->objectName() == name && parent->isVisible()) {
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

void click(Panel& panel, QPoint point) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, point);
  QCoreApplication::processEvents();
}

bool shown(Panel& panel, const QString& id) {
  for (const char* kind :
       {"strataKnob:", "strataToggle:", "strataSegment:", "strataCrossover:", "strataDropdown:"}) {
    if (find_item(panel.root, QString::fromLatin1(kind) + id) != nullptr) {
      return true;
    }
  }
  return false;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  auto values = preset("Init");
  values[index("strata.bands")] = 5.0; // six bands
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1300 && panel->root->implicitHeight() >= 780,
          "panel has its designed size");
  for (int b = 0; b < static_cast<int>(strata::max_bands); ++b) {
    panel->root->setProperty("selected", b);
    QCoreApplication::processEvents();
    for (const auto& p : table().parameters) {
      const QString id = qs(p.id);
      if (id.startsWith(QStringLiteral("strata.band_")) &&
          !id.startsWith(QStringLiteral("strata.band_%1.").arg(b + 1))) {
        continue;
      }
      require(shown(*panel, id), "no control for " + id.toStdString());
    }
  }
}

// The curve is the engine's: Init's band, -18 dB at 2:1, 6 dB knee.
void the_curve_is_the_gain_computer() {
  InMemoryNativeSynthAdapter adapter(preset("Init"));
  NativeSynthDeviceController controller(table(), adapter);
  const auto curve = controller.strata_transfer(0, -60.0, 61);
  require(curve.size() == 61, "one point a dB");
  const strata::Curve init{};
  for (int i = 0; i < 61; ++i) {
    const double input = -60.0 + i;
    require(std::fabs(curve[i].toDouble() - strata::output_db(input, init)) < 1e-9,
            "the curve at " + std::to_string(input));
  }
  require(std::fabs(curve[60].toDouble() + 9.0) < 1e-9, "full scale at 2:1 comes out at -9 dB");
  require(controller.strata_crossovers().size() == 3, "four bands, three crossovers");
  require(controller.strata_band_shapes(32).size() == 4, "a shape per band");
}

void bands_are_added_and_removed() {
  InMemoryNativeSynthAdapter adapter(preset("Init"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* spectrum = item(*panel, QStringLiteral("strataSpectrum"));
  QVariant x;
  require(QMetaObject::invokeMethod(spectrum, "xOf", Q_RETURN_ARG(QVariant, x),
                                    Q_ARG(QVariant, 3'000.0)),
          "xOf runs");
  const QPoint at = spectrum->mapToScene(QPointF(x.toDouble(), spectrum->height() * 0.8)).toPoint();
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, at);
  QCoreApplication::processEvents();
  require(controller.choice(QStringLiteral("strata.bands")) == 4, "five bands now");
  const auto crossovers = controller.strata_crossovers();
  require(crossovers.size() == 4 && std::fabs(crossovers[2].toDouble() - 3'000.0) < 30.0,
          "a crossover where the double-click landed");
  require(panel->root->property("selected").toInt() == 3, "the new band is selected");
  click(*panel, centre(item(*panel, QStringLiteral("strataRemoveBand:3"))));
  require(controller.choice(QStringLiteral("strata.bands")) == 3, "four bands again");
  require(std::fabs(controller.strata_crossovers()[2].toDouble() - 6'000.0) < 1.0,
          "the crossovers close up");
}

// In OTT mode the first touch of a band writes OTT out as bands.
void ott_is_taken_over_by_a_touch() {
  auto values = preset("OTT 30%");
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto before = controller.strata_transfer(1, -60.0, 61);
  const auto written = controller.strata_ott_values();
  click(*panel, centre(item(*panel, QStringLiteral("strataTakeOver"))));
  require(controller.choice(QStringLiteral("strata.mode")) == 0, "Bands mode now");
  require(controller.choice(QStringLiteral("strata.bands")) == 2, "three bands");
  require(std::fabs(controller.value(band_id(1, "threshold")) -
                    written.value(band_id(1, "threshold")).toDouble()) < 1e-9,
          "OTT's thresholds are the bands'");
  const auto after = controller.strata_transfer(1, -60.0, 61);
  for (int i = 0; i < 61; ++i) {
    require(std::fabs(after[i].toDouble() - before[i].toDouble()) < 1e-6,
            "the curve did not change");
  }
  click(*panel, centre(item(*panel, QStringLiteral("strataEngageOtt"))));
  require(controller.choice(QStringLiteral("strata.mode")) == 1, "the OTT card engages OTT");
}

std::vector<float> frame_with(float learn_progress) {
  namespace t = strata::telemetry;
  std::vector<float> frame(t::size, strata::silent_db);
  frame[t::input_peak] = frame[t::input_peak + 1] = -6.0F;
  frame[t::output_peak] = frame[t::output_peak + 1] = -9.0F;
  frame[t::keyed] = 0.0F;
  frame[t::latency_ms] = 0.0F;
  frame[t::auto_gain_db] = 0.0F;
  frame[t::learn_progress] = learn_progress;
  frame[t::band_count] = 4.0F;
  for (std::size_t b = 0; b < strata::max_bands; ++b) {
    frame[t::band_gain + b] = -3.0F;
    frame[t::band_level + b] = -20.0F;
    frame[t::learned + b] = b < 4 ? -30.0F + static_cast<float>(b) : strata::silent_db;
  }
  for (std::size_t k = 0; k < strata::max_crossovers; ++k) {
    frame[t::crossovers + k] = 0.0F;
  }
  for (std::size_t i = 0; i < strata::display_bands; ++i) {
    frame[t::input_spectrum + i] = -40.0F;
    frame[t::output_spectrum + i] = -42.0F;
  }
  return frame;
}

void telemetry_and_learn() {
  auto values = preset("Init");
  values[index("strata.learn")] = 1.0;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  adapter.set_telemetry(frame_with(0.5F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("band_gain")).toList().size() == 6 &&
              live.value(QStringLiteral("input_spectrum")).toList().size() ==
                  static_cast<qsizetype>(strata::display_bands),
          "the controller decodes Strata's frame");
  auto panel = load_panel(controller);
  require(QTest::qWaitFor(
              [&] {
                return item(*panel, QStringLiteral("strataLearnProgress"))
                           ->property("text")
                           .toString() == QStringLiteral("LISTENING 50 %");
              },
              1'000),
          "Learn's progress shows");
  adapter.set_telemetry(frame_with(1.0F));
  require(QTest::qWaitFor([&] { return controller.choice(QStringLiteral("strata.learn")) == 0; },
                          1'000),
          "Learn finishes by itself");
  for (int b = 0; b < 4; ++b) {
    require(std::fabs(controller.value(band_id(b, "threshold")) - (-30.0 + b)) < 1e-6,
            "the suggestion became band " + std::to_string(b + 1) + "'s threshold");
  }
}

void presets_step() {
  InMemoryNativeSynthAdapter adapter(preset("Init"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("strataPresetNext"))));
  require(controller.preset_name() == QStringLiteral("Glue the drums"), "next steps the preset");
  require(item(*panel, QStringLiteral("strataPresetName"))->property("text").toString() ==
              QStringLiteral("Glue the drums"),
          "the panel names its preset");
  require(controller.strata_crossovers().size() == 2, "three bands from the preset");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;

// A kick, hats and a bass at 126 BPM, for the screenshots.
std::vector<engine::StereoFrame> groove(std::size_t frames) {
  std::vector<engine::StereoFrame> out(frames);
  const double beat = 60.0 / 126.0 * rate;
  std::uint32_t seed = 1;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i);
    const double in_beat = std::fmod(t, beat);
    const double kick =
        std::exp(-in_beat / (0.12 * rate)) *
        std::sin(2.0 * std::numbers::pi *
                 (50.0 * in_beat / rate + 3.0 * std::exp(-in_beat / (0.02 * rate))));
    seed = seed * 1'664'525U + 1'013'904'223U;
    const double white = static_cast<double>(seed >> 8U) / 8'388'608.0 - 1.0;
    const double off = std::fmod(t + beat / 2.0, beat);
    const double hat = white * std::exp(-off / (0.02 * rate)) * 0.3;
    const double bass = 0.25 * std::sin(2.0 * std::numbers::pi * 55.0 * t / rate) *
                        (1.0 - std::exp(-off / (0.01 * rate)));
    const auto x = static_cast<float>(0.8 * kick + hat + bass);
    out[i] = {x, x};
  }
  return out;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("STRATA_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  for (const auto* shot : {"Glue the drums", "OTT 30%", "Master polish"}) {
    const auto values = preset(shot);
    auto created = effects::NativeEffectRuntime::create(strata::engine(), values, rate);
    require(created.has_value(), "a Strata runtime");
    auto runtime = std::move(created.value());
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    auto audio = groove(static_cast<std::size_t>(3.0 * rate));
    std::vector<float> frame;
    for (std::size_t position = 0; position < audio.size(); position += 256) {
      std::span<engine::StereoFrame> block(audio.data() + position,
                                           std::min<std::size_t>(256, audio.size() - position));
      const oscillate::native_dsp::Transport transport{0.0, 0.0, 0.0, 4.0};
      (void)runtime->process(block, {}, position, 1, transport, {});
      if (runtime->read_telemetry(frame)) {
        adapter.set_telemetry(frame);
      }
      if (position % 4'096 == 0) {
        QTest::qWait(1);
      }
    }
    QTest::qWait(300);
    QString file = QString::fromLatin1(shot).toLower().replace(' ', '_').remove('%');
    const QString path = QString::fromLocal8Bit(directory) + "/strata_" + file + ".png";
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    the_curve_is_the_gain_computer();
    bands_are_added_and_removed();
    ott_is_taken_over_by_a_touch();
    telemetry_and_learn();
    presets_step();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "strata panel contract passed\n";
  return 0;
}
