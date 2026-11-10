#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/keel.hpp"
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

// The Keel panel (ADR-0119): every parameter on a control (the curve's on
// the balance view); the text fits; the subtitle follows the target; the
// view edits the drawn curve by pointer and keys; Flatten and Learn; the
// telemetry decodes into the view, the history and the meters; the real
// Keel's readings reach the panel; presets and status; and screenshots on
// request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace keel = oscillate::native_dsp::effects::keel;
namespace engine = oscillate::engine;

using oscillate::test::require;

constexpr double rate = 48'000.0;
constexpr int wait_ms = 1'000;
constexpr int target_curve = 1;
constexpr double tolerance_db = 1e-6;

const oscillate::native_dsp::EngineDescriptor& table() { return *keel::engine().descriptor; }
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

QString curve_id(std::size_t band) {
  constexpr int digits = 2;
  return QStringLiteral("keel.curve_%1").arg(static_cast<int>(band + 1), digits, 10, QChar('0'));
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
      import "qrc:/keel"
      KeelPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/KeelPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Keel panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Keel panel was not created: " + component.errorString().toStdString());
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

QPoint at_fraction(QQuickItem* target, double x, double y) {
  return target->mapToScene(QPointF(target->width() * x, target->height() * y)).toPoint();
}

QPoint centre(QQuickItem* target) {
  constexpr double middle = 0.5;
  return at_fraction(target, middle, middle);
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

// A mix-like programme: noise falling 6 dB an octave above 2 kHz over a
// 55 Hz bass.
std::vector<engine::StereoFrame> programme(std::size_t frames) {
  constexpr double corner_hz = 2'000.0;
  constexpr double bass_hz = 55.0;
  constexpr double bass_level = 0.2;
  constexpr double noise_level = 0.3;
  constexpr std::uint32_t seed = 12'345;
  constexpr std::uint32_t multiplier = 1'664'525U;
  constexpr std::uint32_t increment = 1'013'904'223U;
  constexpr unsigned shift = 8U;
  constexpr double scale = 8'388'608.0;
  const double pole = std::exp(-2.0 * std::numbers::pi * corner_hz / rate);
  std::uint32_t state = seed;
  double smooth = 0.0;
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    state = state * multiplier + increment;
    const double hiss = static_cast<double>(state >> shift) / scale - 1.0;
    smooth = hiss + pole * (smooth - hiss);
    const double t = static_cast<double>(i) / rate;
    const double x =
        noise_level * smooth + bass_level * std::sin(2.0 * std::numbers::pi * bass_hz * t);
    out[i] = {static_cast<float>(x), static_cast<float>(x)};
  }
  return out;
}

// The real Keel over the input; its telemetry every `every` frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input,
                                               std::size_t every) {
  constexpr std::size_t block = 128;
  constexpr double beats_per_bar = 4.0;
  auto created = effects::NativeEffectRuntime::create(keel::engine(), values, rate);
  require(created.has_value(), "a Keel runtime");
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
            "Keel renders");
    if ((position + size) % every == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Keel publishes telemetry");
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
  const auto curve_ids = item(*panel, QStringLiteral("keelView"))->property("curveIds").toList();
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool found = std::ranges::any_of(curve_ids, [&](const QVariant& v) { return v == id; });
    for (const char* kind : {"keelKnob:", "keelSegmented:", "keelToggle:"}) {
      found = found || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("keelPresetName")) == QStringLiteral("Default"),
          "the panel names its preset");
  require(text_of(*panel, QStringLiteral("keelSubtitle")) ==
              QStringLiteral("TONAL BALANCE · TILT −2.5 dB/OCT"),
          "the subtitle names the target: " +
              text_of(*panel, QStringLiteral("keelSubtitle")).toStdString());
  require(!item(*panel, QStringLiteral("keelView"))->property("editable").toBool(),
          "the tilt target draws no handles");
  require(!item(*panel, QStringLiteral("keelLearn"))->property("active").toBool(),
          "nothing tracked: nothing to learn");
  oscillate::test::panel_fit::require_text_fits(panel->root, "keel");
}

// On Curve the view edits the drawn curve: a drag moves the nearest band,
// the arrows choose and nudge one; Flatten sets every band to 0 dB.
void the_view_edits_the_curve() {
  constexpr double nudge_db = 0.5;
  constexpr double press_x = 0.5;
  constexpr double press_y = 0.5;
  constexpr double drag_y = 0.25;
  // The pointer lands on whole pixels.
  constexpr double drag_tolerance_db = 1.0;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* target = item(*panel, QStringLiteral("keelSegmented:keel.target"));
  target->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.choice(QStringLiteral("keel.target")) == target_curve, "the curve target");
  require(wait_for_text(*panel, QStringLiteral("keelSubtitle"),
                        QStringLiteral("TONAL BALANCE · CURVE")),
          "the subtitle follows");
  auto* view = item(*panel, QStringLiteral("keelView"));
  require(QTest::qWaitFor([&] { return view->property("editable").toBool(); }, wait_ms),
          "the view edits the curve");
  auto* area = item(*panel, QStringLiteral("keelCurveArea"));
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                    at_fraction(area, press_x, press_y));
  QTest::mouseMove(panel->window.get(), at_fraction(area, press_x, drag_y));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                      at_fraction(area, press_x, drag_y));
  const auto chosen = static_cast<std::size_t>(view->property("chosen").toInt());
  const double range = view->property("rangeDb").toDouble();
  const double lifted = controller.value(curve_id(chosen));
  require(std::fabs(lifted - (press_y - drag_y) * 2.0 * range) < drag_tolerance_db,
          "a quarter of the view up lifts the band by a quarter of the scale: " +
              std::to_string(lifted));
  view->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(std::fabs(controller.value(curve_id(chosen + 1)) - nudge_db) < tolerance_db,
          "Right chooses the next band and Up lifts it half a decibel");
  click(*panel, item(*panel, QStringLiteral("keelFlatten")));
  for (std::size_t b = 0; b < keel::bands; ++b) {
    require(controller.value(curve_id(b)) == 0.0, "Flatten sets every band to 0 dB");
  }
  oscillate::test::panel_fit::require_text_fits(panel->root, "keel curve");
}

void telemetry_decodes_into_the_view() {
  namespace t = keel::telemetry;
  constexpr float in_db = -6.2F;
  constexpr float out_db = -4.4F;
  constexpr float quieter_db = 1.0F;
  constexpr float match_db = -1.3F;
  constexpr float lift_db = 2.0F;
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("keelInPeak")) == QStringLiteral("—"),
          "a dash with nothing measured");
  require(text_of(*panel, QStringLiteral("keelPlotChipText")) == QStringLiteral("PREVIEW"),
          "idle, the view previews");
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = in_db;
  frame[t::input_peak + 1] = in_db - quieter_db;
  frame[t::output_peak] = out_db;
  frame[t::output_peak + 1] = out_db - quieter_db;
  frame[t::match_db] = match_db;
  frame[t::tracked] = 1.0F;
  for (std::size_t b = 0; b < keel::bands; ++b) {
    frame[t::correction + b] = b % 2 == 0 ? lift_db : -lift_db;
  }
  for (std::size_t s = 0; s < keel::history_slices; ++s) {
    frame[t::history + s * keel::bands] = lift_db;
  }
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("balance")).toList().size() ==
                  static_cast<qsizetype>(keel::bands) &&
              live.value(QStringLiteral("history")).toList().size() ==
                  static_cast<qsizetype>(keel::history_slices),
          "the controller decodes Keel's frame");
  require(wait_for_text(*panel, QStringLiteral("keelInPeak"), QStringLiteral("−6.2 dBFS")),
          "in peak: " + text_of(*panel, QStringLiteral("keelInPeak")).toStdString());
  require(text_of(*panel, QStringLiteral("keelOutPeak")) == QStringLiteral("−4.4 dBFS"),
          "out peak: " + text_of(*panel, QStringLiteral("keelOutPeak")).toStdString());
  require(text_of(*panel, QStringLiteral("keelMatch")) == QStringLiteral("−1.3 dB"),
          "match: " + text_of(*panel, QStringLiteral("keelMatch")).toStdString());
  require(item(*panel, QStringLiteral("keelView"))->property("correction").toList().size() ==
              static_cast<qsizetype>(keel::bands),
          "the view draws the correction");
  require(item(*panel, QStringLiteral("keelHistory"))->property("slices").toList().size() ==
              static_cast<qsizetype>(keel::history_slices),
          "the history draws its slices");
  require(text_of(*panel, QStringLiteral("keelPlotChipText")) == QStringLiteral("LIVE"),
          "playing, the view is live");
  require(item(*panel, QStringLiteral("keelLearn"))->property("active").toBool(),
          "a tracked balance can be learned");
  oscillate::test::panel_fit::require_text_fits(panel->root, "keel playing");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("keelInPeak"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

// The real Keel on a mix-like programme: the panel reads its correction,
// and Learn takes the balance it tracked as the target curve.
void the_real_keel_reaches_the_panel_and_learns() {
  namespace t = keel::telemetry;
  constexpr double seconds = 3.0;
  constexpr std::size_t every = 4'096;
  constexpr float least_db = 0.5F;
  const auto values = preset("Default");
  const auto frames =
      real_telemetry(values, programme(static_cast<std::size_t>(seconds * rate)), every);
  const auto& last = frames.back();
  require(last[t::tracked] == 1.0F, "Keel tracked the programme");
  float most = 0.0F;
  for (std::size_t b = 0; b < keel::bands; ++b) {
    most = std::max(most, std::fabs(last[t::correction + b]));
  }
  require(most > least_db, "Keel corrects the programme");

  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(last);
  auto* learn = item(*panel, QStringLiteral("keelLearn"));
  require(QTest::qWaitFor([&] { return learn->property("active").toBool(); }, wait_ms),
          "Learn wakes with a reading");
  click(*panel, learn);
  require(controller.choice(QStringLiteral("keel.target")) == target_curve,
          "Learn turns the target to the curve");
  const auto learned = keel::learned_curve(last, values);
  for (std::size_t b = 0; b < keel::bands; ++b) {
    const auto index = oscillate::native_dsp::find_parameter(table(), curve_id(b).toStdString());
    if (!index) {
      throw std::runtime_error("no parameter " + curve_id(b).toStdString());
    }
    const auto& parameter = table().parameters[*index];
    require(std::fabs(controller.value(curve_id(b)) -
                      oscillate::native_dsp::sanitise(parameter, learned[b])) < tolerance_db,
            "the curve is the learned balance at band " + std::to_string(b));
  }
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Default"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, item(*panel, QStringLiteral("keelPresetNext")));
  require(controller.preset_name() != QStringLiteral("Default"), "next steps the preset");
  click(*panel, item(*panel, QStringLiteral("keelPresetPrevious")));
  require(controller.preset_name() == QStringLiteral("Default"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("keelMenu"));
  click(*panel, item(*panel, QStringLiteral("keelPresetDisplay")));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Warm It Up"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("keelPresetName")) == QStringLiteral("Warm It Up") &&
              text_of(*panel, QStringLiteral("keelSubtitle")) ==
                  QStringLiteral("TONAL BALANCE · TILT −3.0 dB/OCT"),
          "a preset shows by name and target");
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("keelStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

void screenshots_if_requested() {
  const char* directory = std::getenv("KEEL_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  // Long enough to fill the history.
  constexpr double seconds = 26.0;
  constexpr std::size_t every = 48'000;
  constexpr int frame_ms = 20;
  constexpr int settle_ms = 150;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const auto values = preset("Streaming Balance");
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto frames =
      real_telemetry(values, programme(static_cast<std::size_t>(seconds * rate)), every);
  for (const auto& frame : frames) {
    adapter.set_telemetry(frame);
    QTest::qWait(frame_ms);
  }
  QTest::qWait(settle_ms);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/keel.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter_and_fits();
    the_view_edits_the_curve();
    telemetry_decodes_into_the_view();
    the_real_keel_reaches_the_panel_and_learns();
    presets_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "keel panel contract passed\n";
  return 0;
}
