#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/native_dsp/effects/dust.hpp"
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

// The Dust panel (ADR-0069): every parameter on a control, the shelf in the
// engine's layer order, a layer's switch writing its dust.<layer>.on, the
// dice drawing a new seed, the telemetry turning the record, sparking at
// its pops and filling the bed meter, the duck's key named, and presets.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace dust = oscillate::native_dsp::effects::dust;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *dust::engine().descriptor; }

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
      import "qrc:/dust"
      DustPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/DustPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Dust panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Dust panel was not created: " + component.errorString().toStdString());
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

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

std::vector<double> defaults() { return oscillate::native_dsp::default_values(table()); }

const oscillate::native_dsp::ParameterDescriptor& parameter(std::string_view id) {
  const auto found =
      std::ranges::find(table().parameters, id, &oscillate::native_dsp::ParameterDescriptor::id);
  require(found != table().parameters.end(), "Dust has " + std::string(id));
  return *found;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  oscillate::test::panel_fit::require_knob_labels_apart(panel->root, "Dust");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    require(id.startsWith(QStringLiteral("dust.")), "every id is Dust's: " + id.toStdString());
    bool shown = false;
    for (const char* kind :
         {"dustKnob:", "dustSegment:", "dustToggle:", "dustStepper:", "dustFader:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
}

void the_shelf_is_in_the_engines_layer_order() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QStringList keys = panel->root->property("layerKeys").toStringList();
  require(static_cast<std::size_t>(keys.size()) == dust::layer_count,
          "one card per layer of dust::Layer");
  // Telemetry slot i is layer i: the card for Layer::rain sits where the
  // engine's enum puts it.
  require(keys.at(static_cast<qsizetype>(dust::Layer::rain)) == QStringLiteral("rain") &&
              keys.at(static_cast<qsizetype>(dust::Layer::machine)) == QStringLiteral("machine") &&
              keys.at(static_cast<qsizetype>(dust::Layer::vinyl)) == QStringLiteral("vinyl"),
          "the shelf follows dust::Layer");
  for (const QString& key : keys) {
    static_cast<void>(parameter(QStringLiteral("dust.%1.on").arg(key).toStdString()));
    require(find_item(panel->root, QStringLiteral("dustLayer:") + key) != nullptr,
            "a card for " + key.toStdString());
  }
}

void a_layers_switch_turns_it_on_and_off() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString rain = QStringLiteral("dust.rain.on");
  require(controller.choice(rain) == 0, "rain starts off in the default");
  click(*panel, centre(item(*panel, QStringLiteral("dustToggle:") + rain)));
  require(controller.choice(rain) == 1, "clicking RAIN turns it on");
  require(text_of(*panel, QStringLiteral("dustReadoutValue")) == controller.display(rain),
          "the readout shows the touched control");
  click(*panel, centre(item(*panel, QStringLiteral("dustToggle:") + rain)));
  require(controller.choice(rain) == 0, "and off again");
}

void the_dice_draws_a_new_seed() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString seed = QStringLiteral("dust.mix.seed");
  const auto& p = parameter("dust.mix.seed");
  auto* dice = item(*panel, QStringLiteral("dustSeedDice"));
  for (int roll = 0; roll < 5; ++roll) {
    const double before = controller.value(seed);
    click(*panel, centre(dice));
    const double after = controller.value(seed);
    require(after != before, "the dice always draws a different seed");
    require(after >= p.minimum && after <= p.maximum && after == std::round(after),
            "a whole seed in range");
  }
}

std::vector<float> frame_with(float rotation, float pops) {
  namespace t = dust::telemetry;
  std::vector<float> frame(t::size, -120.0F);
  frame[t::first_layer + static_cast<std::size_t>(dust::Layer::vinyl)] = -12.0F;
  frame[t::first_layer + static_cast<std::size_t>(dust::Layer::tape)] = -30.0F;
  frame[t::duck_db] = 6.0F;
  frame[t::rotation] = rotation;
  frame[t::pops] = pops;
  frame[t::output_peak] = -10.0F;
  frame[t::output_peak + 1] = -11.0F;
  return frame;
}

void telemetry_turns_the_record_and_fills_the_meters() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("dustDuckValue")) == QStringLiteral("—"),
          "a dash with nothing measured");
  adapter.set_telemetry(frame_with(0.25F, 10.0F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("rotation")).toDouble() == 0.25 &&
              live.value(QStringLiteral("layers")).toList().size() ==
                  static_cast<qsizetype>(dust::layer_count) &&
              live.value(QStringLiteral("duck_db")).toDouble() == 6.0,
          "the controller decodes Dust's frame");
  auto* disc = item(*panel, QStringLiteral("dustDisc"));
  require(QTest::qWaitFor([&] { return std::fabs(disc->rotation() - 90.0) < 0.01; }, 1000),
          "a quarter turn turns the record 90 degrees: " + std::to_string(disc->rotation()));
  auto* meter = item(*panel, QStringLiteral("dustBedMeter"));
  const double floor = meter->property("floorDb").toDouble();
  const double ceil = meter->property("ceilDb").toDouble();
  const auto expected = [&](double db) { return (db - floor) / (ceil - floor); };
  auto* vinyl = item(*panel, QStringLiteral("dustLayerMeter:vinyl"));
  auto* tape = item(*panel, QStringLiteral("dustLayerMeter:tape"));
  auto* rain = item(*panel, QStringLiteral("dustLayerMeter:rain"));
  require(std::fabs(vinyl->property("fraction").toDouble() - expected(-12.0)) < 1e-6 &&
              std::fabs(tape->property("fraction").toDouble() - expected(-30.0)) < 1e-6,
          "each layer's bar stands at its level");
  require(rain->property("fraction").toDouble() == 0.0, "a silent layer shows nothing");
  require(item(*panel, QStringLiteral("dustDuckMeter"))->property("reductionDb").toDouble() == 6.0,
          "the duck's bar reads its reduction");
  require(text_of(*panel, QStringLiteral("dustDuckValue")) == QStringLiteral("−6.0"),
          "and its reading: " + text_of(*panel, QStringLiteral("dustDuckValue")).toStdString());

  // Pops counted by the engine flash as sparks at the needle.
  auto* sparks = item(*panel, QStringLiteral("dustSparks"));
  const int before = sparks->property("total").toInt();
  adapter.set_telemetry(frame_with(0.5F, 25.0F));
  require(QTest::qWaitFor([&] { return sparks->property("total").toInt() == before + 15; }, 1000),
          "fifteen new pops spark: " + std::to_string(sparks->property("total").toInt()));
  require(QTest::qWaitFor([&] { return std::fabs(disc->rotation() - 180.0) < 0.01; }, 1000),
          "half a turn");
  // The count wraps: a smaller count is a fresh run, never a negative one.
  adapter.set_telemetry(frame_with(0.6F, 3.0F));
  require(QTest::qWaitFor([&] { return sparks->property("total").toInt() == before + 18; }, 1000),
          "a wrapped count still sparks");

  adapter.set_telemetry({});
  require(
      QTest::qWaitFor(
          [&] { return text_of(*panel, QStringLiteral("dustDuckValue")) == QStringLiteral("—"); },
          1000),
      "no telemetry: a dash again");
  require(disc->rotation() == 0.0, "and the record at rest");
}

void the_duck_names_its_key() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("dustDuckKey")).startsWith(QStringLiteral("No key")),
          "unkeyed, it says how to key it");
  panel->root->setProperty("keyed", true);
  panel->root->setProperty("keyName", QStringLiteral("Kick"));
  require(text_of(*panel, QStringLiteral("dustDuckKey")).contains(QStringLiteral("Kick")),
          "keyed, it names the key");
  const QString source = QStringLiteral("dust.duck.source");
  auto* segment = item(*panel, QStringLiteral("dustSegment:") + source);
  const auto count = static_cast<double>(parameter("dust.duck.source").choices.size());
  click(*panel, segment->mapToScene(QPointF(segment->width() * 1.5 / count, segment->height() / 2))
                    .toPoint());
  require(controller.choice(source) == 1, "KEY NOTES picks the keying notes");
  require(text_of(*panel, QStringLiteral("dustDuckKey")).contains(QStringLiteral("note")),
          "and says it ducks on notes");
}

void presets_apply() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(table().presets.size() >= 2, "Dust has presets to step through");
  const QString first = controller.preset_name();
  click(*panel, centre(item(*panel, QStringLiteral("dustPresetNext"))));
  require(controller.preset_name() != first, "next steps the preset");
  require(text_of(*panel, QStringLiteral("dustPresetName")) == controller.preset_name(),
          "the display names it");
  const int index = controller.preset_index();
  require(index >= 0 && static_cast<std::size_t>(index) < table().presets.size(), "a preset index");
  const auto resolved = oscillate::native_dsp::preset_values(
      table(), table().presets[static_cast<std::size_t>(index)]);
  require(resolved.has_value(), "the preset resolves");
  const auto& values = resolved.value();
  for (std::size_t i = 0; i < table().parameters.size(); ++i) {
    const QString id = qs(table().parameters[i].id);
    require(std::fabs(controller.value(id) - values[i]) < 1e-9,
            "the preset sets " + id.toStdString());
  }
  // Every on/off switch on the shelf shows what the preset set.
  for (const QString& key : panel->root->property("layerKeys").toStringList()) {
    const QString on = QStringLiteral("dust.%1.on").arg(key);
    require(item(*panel, QStringLiteral("dustToggle:") + on)->property("on").toBool() ==
                (controller.choice(on) == 1),
            "the switch shows " + on.toStdString());
  }
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;
constexpr double tempo_bpm = 120.0;
constexpr double seconds_per_minute = 60.0;

// A sparse four-on-the-floor kick with a soft chord over it, so the bed has
// something to sit under and the duck something to duck from.
std::vector<oscillate::engine::StereoFrame> music(std::size_t frames) {
  std::vector<oscillate::engine::StereoFrame> out(frames);
  const double beat_seconds = seconds_per_minute / tempo_bpm;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double since = std::fmod(t, beat_seconds);
    const double kick = 0.6 * std::exp(-since * 18.0) *
                        std::sin(2 * std::numbers::pi *
                                 (50.0 * since + 60.0 * (1 - std::exp(-since * 30.0)) / 30.0));
    const double chord = 0.05 * (std::sin(2 * std::numbers::pi * 220.0 * t) +
                                 std::sin(2 * std::numbers::pi * 277.2 * t) +
                                 std::sin(2 * std::numbers::pi * 329.6 * t));
    const auto x = static_cast<float>(kick + chord);
    out[i] = {x, x};
  }
  return out;
}

// The real Dust over the music; the telemetry frames, one per block.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values, double seconds) {
  namespace effects = oscillate::native_dsp::effects;
  auto created = effects::NativeEffectRuntime::create(dust::engine(), values, rate);
  require(created.has_value(), "a Dust runtime");
  auto runtime = std::move(created.value());
  const auto input = music(static_cast<std::size_t>(seconds * rate));
  std::vector<oscillate::engine::StereoFrame> block;
  std::vector<std::vector<float>> frames;
  constexpr std::size_t block_frames = 1'600;
  const double beats_per_frame = tempo_bpm / seconds_per_minute / rate;
  for (std::size_t position = 0; position < input.size(); position += block_frames) {
    const std::size_t size = std::min(block_frames, input.size() - position);
    const auto from = static_cast<std::ptrdiff_t>(position);
    block.assign(input.begin() + from, input.begin() + from + static_cast<std::ptrdiff_t>(size));
    const double beat = static_cast<double>(position) * beats_per_frame;
    const oscillate::native_dsp::Transport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Dust renders");
    std::vector<float> frame;
    if (runtime->read_telemetry(frame)) {
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("DUST_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  for (const auto& entry : table().presets) {
    auto resolved = oscillate::native_dsp::preset_values(table(), entry);
    require(resolved.has_value(), "preset resolves");
    const auto values = resolved.value();
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    // Two seconds in, fed a frame at a time so the sparks count pops.
    const auto frames = real_telemetry(values, 2.0);
    const std::size_t tail = std::min<std::size_t>(frames.size(), 8);
    for (std::size_t i = frames.size() - tail; i < frames.size(); ++i) {
      adapter.set_telemetry(frames[i]);
      QTest::qWait(40);
    }
    QTest::qWait(60);
    std::string file(entry.name);
    std::ranges::replace(file, ' ', '_');
    const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/dust_") +
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
    the_shelf_is_in_the_engines_layer_order();
    a_layers_switch_turns_it_on_and_off();
    the_dice_draws_a_new_seed();
    telemetry_turns_the_record_and_fills_the_meters();
    the_duck_names_its_key();
    presets_apply();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "dust panel contract passed\n";
  return 0;
}
