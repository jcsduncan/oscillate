#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/chop.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
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

// The Chop panel (ADR-0075): every parameter on a control (the lanes'
// cells for the steps), a stroke across a lane writing its steps as one
// gesture with the cells between filled, a stepped lane drawing whole
// values, the pattern chips applying their presets, Re-seed stepping the
// seed the same way every time, and the telemetry lighting the step that
// plays; and screenshots on request, live from the real Chop.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace chop = oscillate::native_dsp::effects::chop;
namespace effects = oscillate::native_dsp::effects;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *chop::engine().descriptor; }

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString step_id(int step, std::string_view field) {
  return QStringLiteral("chop.step.%1.%2").arg(step + 1).arg(qs(field));
}

std::vector<double> defaults() { return synth::default_values(table()); }

std::size_t index_of(std::string_view id) {
  const auto found = synth::find_parameter(table(), id);
  require(found.has_value(), "Chop has " + std::string(id));
  return *found;
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
      import "qrc:/chop"
      ChopPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/ChopPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Chop panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Chop panel was not created: " + component.errorString().toStdString());
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

void click(Panel& panel, QPoint point) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, point);
  QCoreApplication::processEvents();
}

// A point in a step's cell at `value` (0 bottom .. 1 top) of its height.
QPoint in_cell(Panel& panel, const QString& id, double value) {
  auto* cell = item(panel, QStringLiteral("chopStep:") + id);
  return cell->mapToScene(QPointF(cell->width() / 2, cell->height() * (1.0 - value))).toPoint();
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  auto values = defaults();
  values[index_of("chop.steps")] = 1.0; // 32 steps: every step's cells show.
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    require(id.startsWith(QStringLiteral("chop.")), "every id is Chop's: " + id.toStdString());
    bool shown = false;
    for (const char* kind : {"chopKnob:", "chopSegment:", "chopStep:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  // At 16 steps only the first sixteen are drawn.
  controller.set_choice(QStringLiteral("chop.steps"), 0);
  require(QTest::qWaitFor(
              [&] {
                return find_item(panel->root, QStringLiteral("chopStep:") + step_id(16, "level")) ==
                       nullptr;
              },
              1000),
          "16 steps draws sixteen cells");
}

void a_stroke_is_one_gesture() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto start = adapter.submissions().size();
  // Low on step 1, across to high on step 5, skipping cells on the way.
  const QPoint from = in_cell(*panel, step_id(0, "level"), 0.2);
  const QPoint to = in_cell(*panel, step_id(4, "level"), 0.8);
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, from);
  QTest::mouseMove(panel->window.get(), in_cell(*panel, step_id(2, "level"), 0.5));
  QTest::mouseMove(panel->window.get(), to);
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, to);
  QCoreApplication::processEvents();
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a stroke is begin, updates and one commit");
  for (std::size_t i = start + 1; i + 1 < s.size(); ++i)
    require(s[i].gesture == NativeSynthGesture::update, "nothing else lands mid-stroke");
  std::vector<double> levels;
  for (int step = 0; step < 6; ++step)
    levels.push_back(controller.value(step_id(step, "level")));
  require(std::fabs(levels[0] - 0.2) < 0.05 && std::fabs(levels[4] - 0.8) < 0.05,
          "the stroke's ends are where it was drawn");
  for (int step = 1; step < 5; ++step)
    require(levels[static_cast<std::size_t>(step)] > levels[static_cast<std::size_t>(step - 1)],
            "the cells between follow the stroke");
  require(levels[5] == 1.0, "steps past the stroke keep their level");
  require(panel->root->property("theme").value<QObject*>()->property("lastTouched").toString() ==
              step_id(4, "level"),
          "the readout follows the stroke");
}

void stepped_lanes_draw_whole_values() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto start = adapter.submissions().size();
  click(*panel, in_cell(*panel, step_id(2, "ratchet"), 0.98));
  require(controller.value(step_id(2, "ratchet")) == 4.0, "the top of the ratchet lane is 4 hits");
  require(adapter.submissions().size() >= start + 2 &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "a click is a gesture too");
  click(*panel, in_cell(*panel, step_id(3, "repeat"), 0.6));
  require(controller.value(step_id(3, "repeat")) == 2.0, "repeat draws in whole steps (4x)");
  click(*panel, in_cell(*panel, step_id(3, "repeat"), 0.02));
  require(controller.value(step_id(3, "repeat")) == 0.0, "the bottom is off");
}

void pattern_chips_apply_presets() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  for (const auto& entry : table().presets) {
    auto* chip = item(*panel, QStringLiteral("chopPreset:") + qs(entry.name));
    click(*panel, centre(chip));
    require(controller.preset_name() == qs(entry.name),
            "the chip applies " + std::string(entry.name));
    require(chip->property("on").toBool(), "and lights");
    const auto resolved = synth::preset_values(table(), entry);
    require(resolved.has_value(), "the preset resolves");
    for (std::size_t i = 0; i < table().parameters.size(); ++i) {
      require(std::fabs(controller.value(qs(table().parameters[i].id)) - resolved.value()[i]) <
                  1e-9,
              "the preset sets " + std::string(table().parameters[i].id));
    }
  }
}

void reseed_steps_the_seed() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString seed = QStringLiteral("chop.seed");
  const auto& p = table().parameters[index_of("chop.seed")];
  auto* reseed = item(*panel, QStringLiteral("chopReseed"));
  const double first = controller.value(seed);
  click(*panel, centre(reseed));
  const double next = controller.value(seed);
  require(next != first && next >= p.minimum && next <= p.maximum && next == std::round(next),
          "Re-seed picks another whole seed in range");
  controller.set_value(seed, first);
  click(*panel, centre(reseed));
  require(controller.value(seed) == next, "the same seed always re-seeds to the same next one");
  click(*panel, centre(reseed));
  require(controller.value(seed) != next, "and on again");
}

void telemetry_lights_the_playing_step() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("chopHeroHint"))->isVisible(), "stopped, the hero says so");
  namespace t = chop::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::phase] = 0.3F;
  frame[t::step] = 4.0F;
  frame[t::slot] = 4.0F;
  frame[t::pattern_beats] = 4.0F;
  for (std::size_t s = 0; s < 16; ++s)
    frame[t::played + s] = s == 6 ? 0.0F : 1.0F;
  for (std::size_t i = 0; i < chop::view_points; ++i) {
    frame[t::input + i] = 0.5F;
    frame[t::output + i] = 0.25F;
    frame[t::gain + i] = 0.5F;
  }
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("step")).toInt() == 4 &&
              live.value(QStringLiteral("played")).toList().size() ==
                  static_cast<qsizetype>(chop::max_steps) &&
              !live.value(QStringLiteral("played")).toList().at(6).toBool() &&
              live.value(QStringLiteral("input")).toList().size() ==
                  static_cast<qsizetype>(chop::view_points),
          "the controller decodes Chop's frame");
  require(QTest::qWaitFor([&] { return panel->root->property("playingStep").toInt() == 4; }, 1000),
          "the panel follows the playing step");
  require(!item(*panel, QStringLiteral("chopHeroHint"))->isVisible(), "playing, the hint goes");
}

void the_mode_segment_switches_modes() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* segment = item(*panel, QStringLiteral("chopSegment:chop.mode"));
  click(*panel,
        segment->mapToScene(QPointF(segment->width() * 5 / 6, segment->height() / 2)).toPoint());
  require(controller.choice(QStringLiteral("chop.mode")) == 2, "Gate + repeat is picked");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;
constexpr std::size_t block_frames = 160;
// The panel polls at about 30 Hz while playing.
constexpr std::size_t poll_frames = 1'600;
constexpr int poll_ms = 33;
constexpr int settle_ms = 150;
constexpr double tempo_beats_per_second = 2.0;
constexpr double beats_per_bar = 4.0;

std::vector<double> preset(std::string_view name) {
  for (const auto& entry : table().presets) {
    if (entry.name == name) {
      return synth::preset_values(table(), entry).value();
    }
  }
  throw std::runtime_error("missing preset " + std::string(name));
}

// A held pad to chop: a 110 Hz saw-like stack with a slow swell.
engine::StereoFrame pad_frame(std::size_t frame) {
  constexpr double root_hz = 110.0;
  constexpr int partials = 6;
  constexpr double level = 0.25;
  constexpr double swell_hz = 0.5;
  constexpr double swell_depth = 0.3;
  const double seconds = static_cast<double>(frame) / rate;
  double sum = 0.0;
  for (int k = 1; k <= partials; ++k) {
    sum += std::sin(2.0 * std::numbers::pi * root_hz * k * seconds) / k;
  }
  const double swell =
      1.0 - swell_depth * (0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * swell_hz * seconds));
  const auto sample = static_cast<float>(level * swell * sum);
  return {sample, sample};
}

// The real Chop over the pad until `stop`; the telemetry every poll.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               std::size_t stop) {
  auto created = effects::NativeEffectRuntime::create(chop::engine(), values, rate);
  require(created.has_value(), "a Chop runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  const double beats_per_frame = tempo_beats_per_second / rate;
  for (std::size_t position = 0; position < stop; position += block_frames) {
    const std::size_t size = std::min(block_frames, stop - position);
    block.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      block[i] = pad_frame(position + i);
    }
    const double beat = static_cast<double>(position) * beats_per_frame;
    const synth::SynthTransport transport{beat, beats_per_frame, beat / beats_per_bar,
                                          beats_per_bar};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Chop renders");
    if ((position + size) % poll_frames == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Chop publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("CHOP_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  constexpr std::size_t shown_polls = 10;
  struct Shot {
    const char* preset;
    double seconds;
    const char* file;
  };
  for (const Shot shot : {Shot{"Off-beat chop", 2.3, "/chop_off_beat.png"},
                          Shot{"Glitch fill", 2.6, "/chop_glitch_fill.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    const auto frames = real_telemetry(values, static_cast<std::size_t>(shot.seconds * rate));
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
    a_stroke_is_one_gesture();
    stepped_lanes_draw_whole_values();
    pattern_chips_apply_presets();
    reseed_steps_the_seed();
    telemetry_lights_the_playing_step();
    the_mode_segment_switches_modes();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "chop panel contract passed\n";
  return 0;
}
