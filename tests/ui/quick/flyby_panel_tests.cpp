#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/flyby.hpp"
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

// The Flyby panel (ADR-0067): every parameter on a control (the path's
// points on the chart), a point dragged as one gesture writing its X and Y,
// the run mode showing and resting the controls that matter to it, and the
// telemetry decode into the source dot and the readings.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace flyby = oscillate::native_dsp::effects::flyby;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *flyby::engine().descriptor; }

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
      import "qrc:/flyby"
      FlybyPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/FlybyPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Flyby panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Flyby panel was not created: " + component.errorString().toStdString());
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
  QCoreApplication::processEvents();
}

void drag(Panel& panel, QPoint from, QPoint by) {
  QTest::mousePress(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  QTest::mouseMove(panel.window.get(), from + by / 2);
  QTest::mouseMove(panel.window.get(), from + by);
  QTest::mouseRelease(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from + by);
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, 1000);
}

std::vector<double> defaults() { return oscillate::native_dsp::default_values(table()); }

// Clicks segment choice `index` of `count`.
void pick(Panel& panel, const QString& id, int index, int count) {
  auto* segment = item(panel, QStringLiteral("flybySegment:") + id);
  click(panel, scene(segment, segment->width() * (index + 0.5) / count, segment->height() / 2));
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  oscillate::test::panel_fit::require_knob_labels_apart(panel->root, "Flyby");
  const int points = item(*panel, QStringLiteral("flybyMap"))->property("pointCount").toInt();
  require(points == 4, "a pass has four points");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    require(id.startsWith(QStringLiteral("flyby.")), "every id is Flyby's: " + id.toStdString());
    bool shown = false;
    for (const char* kind : {"flybyKnob:", "flybySegment:", "flybyDropdown:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    // The path's points are on the chart.
    for (int point = 1; point <= points; ++point) {
      if (id == QStringLiteral("flyby.x%1").arg(point) ||
          id == QStringLiteral("flyby.y%1").arg(point)) {
        shown = find_item(panel->root, QStringLiteral("flybyPoint:%1").arg(point)) != nullptr;
      }
    }
    require(shown, "no control for " + id.toStdString());
  }
  // The tagline ends before the preset browser rather than running under it.
  oscillate::test::panel_fit::require_clear_of(
      *item(*panel, QStringLiteral("flybyTagline")),
      *item(*panel, QStringLiteral("flybyPresetPrevious"))->parentItem(), "the preset browser");
}

void dragging_a_point_writes_its_x_and_y_as_one_gesture() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* map = item(*panel, QStringLiteral("flybyMap"));
  auto* end = item(*panel, QStringLiteral("flybyPoint:4"));
  const double per_metre = map->property("pxPerMetre").toDouble();
  const double x = controller.value(QStringLiteral("flyby.x4"));
  const double y = controller.value(QStringLiteral("flyby.y4"));
  const auto start = adapter.submissions().size();
  drag(*panel, centre(end), QPoint(40, -30));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a point drag is begin, updates and commit");
  require(std::fabs(controller.value(QStringLiteral("flyby.x4")) - (x + 40.0 / per_metre)) < 0.5,
          "the end follows the pointer across");
  require(std::fabs(controller.value(QStringLiteral("flyby.y4")) - (y + 30.0 / per_metre)) < 0.5,
          "and up the chart (forward)");
  // An orbit has no points to drag; its radius is a knob.
  pick(*panel, QStringLiteral("flyby.path"), 1, 2);
  require(controller.choice(QStringLiteral("flyby.path")) == 1, "ORBIT picks Orbit");
  require(
      QTest::qWaitFor(
          [&] { return find_item(panel->root, QStringLiteral("flybyPoint:1")) == nullptr; }, 1000),
      "an orbit hides the points");
  require(!item(*panel, QStringLiteral("flybyKnob:flyby.radius"))->property("inactive").toBool(),
          "and wakes Radius");
}

void run_modes_show_their_controls() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString run = QStringLiteral("flyby.run");
  const auto inactive = [&](const char* id) {
    return item(*panel, QStringLiteral("flybyKnob:") + QString::fromLatin1(id))
        ->property("inactive")
        .toBool();
  };
  require(!inactive("flyby.speed") && inactive("flyby.position"),
          "Loop moves at Speed; Position rests");
  pick(*panel, run, 3, 4);
  require(controller.choice(run) == 3, "POSITION picks Position");
  require(inactive("flyby.speed") && !inactive("flyby.position"),
          "Position wakes Position and rests Speed");
  pick(*panel, run, 2, 4);
  require(item(*panel, QStringLiteral("flybyDropdown:flyby.length"))->isVisible(),
          "Sync shows Length");
  require(!item(*panel, QStringLiteral("flybySegment:flyby.rest"))->isVisible(),
          "and hides Between");
  pick(*panel, run, 1, 4);
  require(item(*panel, QStringLiteral("flybySegment:flyby.rest"))->isVisible(),
          "Once shows Between");
  require(!item(*panel, QStringLiteral("flybyDropdown:flyby.length"))->isVisible(),
          "and hides Length");
  pick(*panel, QStringLiteral("flyby.listen"), 1, 2);
  require(
      text_of(*panel, QStringLiteral("flybyListenHint")).startsWith(QStringLiteral("A constant")),
      "Speakers explains the pan");
}

std::vector<float> frame_at(float x, float y) {
  namespace t = flyby::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::x] = x;
  frame[t::y] = y;
  frame[t::distance] = std::hypot(x, y);
  frame[t::pitch_cents] = 120.0F;
  frame[t::itd_us] = 300.0F;
  frame[t::level_left_db] = -8.0F;
  frame[t::level_right_db] = -6.0F;
  frame[t::room] = 0.4F;
  frame[t::position] = 0.3F;
  frame[t::closest] = 14.0F;
  frame[t::speed] = 25.0F;
  frame[t::input_peak] = -12.0F;
  frame[t::output_peak] = -14.0F;
  frame[t::output_peak + 1] = -13.0F;
  return frame;
}

void telemetry_drives_the_chart_and_readings() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("flybyReadingValue:distance")) == QStringLiteral("—"),
          "a dash with nothing measured");
  adapter.set_telemetry(frame_at(10.0F, 5.0F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("x")).toDouble() == 10.0 &&
              live.value(QStringLiteral("speed")).toDouble() == 25.0,
          "the controller decodes Flyby's frame");
  require(
      wait_for_text(*panel, QStringLiteral("flybyReadingValue:distance"), QStringLiteral("11.2 m")),
      "distance reads the telemetry: " +
          text_of(*panel, QStringLiteral("flybyReadingValue:distance")).toStdString());
  require(text_of(*panel, QStringLiteral("flybyReadingValue:speed")) == QStringLiteral("90 km/h"),
          "speed in km/h");
  require(text_of(*panel, QStringLiteral("flybyReadingValue:pitch_cents")) ==
              QStringLiteral("+120 ct"),
          "Doppler in cents");
  require(text_of(*panel, QStringLiteral("flybyReadingValue:itd_us")) == QStringLiteral("R 300 µs"),
          "the right ear leads");
  auto* map = item(*panel, QStringLiteral("flybyMap"));
  auto* dot = item(*panel, QStringLiteral("flybySource"));
  const double per_metre = map->property("pxPerMetre").toDouble();
  const double cx = dot->x() + dot->width() / 2;
  const double cy = dot->y() + dot->height() / 2;
  require(std::fabs(cx - (map->width() / 2 + 10.0 * per_metre)) < 1.0 &&
              std::fabs(cy - (map->height() / 2 - 5.0 * per_metre)) < 1.0,
          "the source dot sits where the engine hears it");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("flybyReadingValue:distance"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

void presets_step() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(table().presets.size() >= 2, "Flyby has scenes to step through");
  const QString first = controller.preset_name();
  click(*panel, centre(item(*panel, QStringLiteral("flybyPresetNext"))));
  require(controller.preset_name() != first, "next steps the scene");
  require(text_of(*panel, QStringLiteral("flybyPresetName")) == controller.preset_name(),
          "the display names it");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;

// A train-ish source: a low engine buzz, rumble and a two-tone horn.
std::vector<oscillate::engine::StereoFrame> train(std::size_t frames) {
  std::vector<oscillate::engine::StereoFrame> out(frames);
  std::uint32_t seed = 4'242;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    seed = seed * 1'664'525U + 1'013'904'223U;
    const double noise = static_cast<double>(seed >> 8U) / 8'388'608.0 - 1.0;
    const double x = 0.15 * std::sin(2 * std::numbers::pi * 55.0 * t) + 0.08 * noise +
                     0.06 * std::sin(2 * std::numbers::pi * 311.0 * t) +
                     0.06 * std::sin(2 * std::numbers::pi * 370.0 * t);
    out[i] = {static_cast<float>(x), static_cast<float>(x)};
  }
  return out;
}

// The real Flyby over the source; the last telemetry frame.
std::vector<float> real_telemetry(const std::vector<double>& values, double seconds) {
  namespace effects = oscillate::native_dsp::effects;
  auto created = effects::NativeEffectRuntime::create(flyby::engine(), values, rate);
  require(created.has_value(), "a Flyby runtime");
  auto runtime = std::move(created.value());
  const auto input = train(static_cast<std::size_t>(seconds * rate));
  std::vector<oscillate::engine::StereoFrame> block;
  std::vector<float> last;
  constexpr std::size_t block_frames = 160;
  for (std::size_t position = 0; position < input.size(); position += block_frames) {
    const std::size_t size = std::min(block_frames, input.size() - position);
    const auto from = static_cast<std::ptrdiff_t>(position);
    block.assign(input.begin() + from, input.begin() + from + static_cast<std::ptrdiff_t>(size));
    const oscillate::native_dsp::Transport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Flyby renders");
    std::vector<float> frame;
    if (runtime->read_telemetry(frame)) {
      last = frame;
    }
  }
  return last;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("FLYBY_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const std::size_t shots = std::min<std::size_t>(4, table().presets.size());
  for (std::size_t s = 0; s < shots; ++s) {
    const auto& entry = table().presets[s];
    auto resolved = oscillate::native_dsp::preset_values(table(), entry);
    require(resolved.has_value(), "preset resolves");
    const auto values = resolved.value();
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    // A few seconds in, so the source is on its way.
    for (const double seconds : {2.0, 2.2, 2.4, 2.6, 2.8, 3.0}) {
      adapter.set_telemetry(real_telemetry(values, seconds));
      QTest::qWait(60);
    }
    QTest::qWait(150);
    std::string file(entry.name);
    std::ranges::replace(file, ' ', '_');
    const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/flyby_") +
                         QString::fromStdString(file) + QStringLiteral(".png");
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    dragging_a_point_writes_its_x_and_y_as_one_gesture();
    run_modes_show_their_controls();
    telemetry_drives_the_chart_and_readings();
    presets_step();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "flyby panel contract passed\n";
  return 0;
}
