#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "relay_panel_model.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/relay.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
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
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Relay panel (ADR-0040): the echo train it draws against the Relay
// kernel itself, and the QML tape-echo machine driven by the engine table,
// its sync swap, character bay, presets, status and live meters.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::RelayPanelModel;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace relay = effects::relay;
namespace q = oscillate::quick;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *relay::engine().descriptor; }
std::size_t index(std::string_view id) { return *synth::find_parameter(table(), id); }

int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

std::vector<double> preset(std::string_view name) {
  return synth::preset_values(table(),
                              table().presets[static_cast<std::size_t>(preset_index(name))])
      .value();
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// ---- The echo train against the kernel ------------------------------------

constexpr double rate = 48'000.0;

// A 10 ms Hann-windowed 1 kHz burst, on both inputs.
std::vector<double> burst() {
  std::vector<double> b(480);
  for (std::size_t i = 0; i < b.size(); ++i) {
    const double t = static_cast<double>(i);
    b[i] = 0.5 * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * t / 480.0)) *
           std::sin(2.0 * std::numbers::pi * 1'000.0 * t / rate);
  }
  return b;
}

struct Rendered {
  std::vector<double> left;
  std::vector<double> right;
};

Rendered render(const std::vector<double>& values, std::size_t frames, double bpm) {
  auto created = effects::NativeEffectRuntime::create(relay::engine(), values, rate);
  require(created.has_value(), "relay runtime is created");
  auto runtime = std::move(created.value());
  const auto input = burst();
  Rendered out{std::vector<double>(frames), std::vector<double>(frames)};
  std::vector<engine::StereoFrame> buffer(128);
  for (std::size_t position = 0; position < frames; position += 128) {
    const std::size_t size = std::min<std::size_t>(128, frames - position);
    buffer.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      const double x = position + i < input.size() ? input[position + i] : 0.0;
      buffer[i] = {static_cast<float>(x), static_cast<float>(x)};
    }
    const double bpf = bpm / 60.0 / rate;
    const double beat = static_cast<double>(position) * bpf;
    const synth::SynthTransport transport{beat, bpf, beat / 4.0, 4.0};
    require(runtime->process(buffer, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "relay renders");
    for (std::size_t i = 0; i < size; ++i) {
      out.left[position + i] = buffer[i].left;
      out.right[position + i] = buffer[i].right;
    }
  }
  return out;
}

// The burst's level at exactly `lag`: its correlation with a quadrature
// pair of templates over their energy, so the loop filters' small phase
// shift per pass does not read as a level change.
double gain_at(const std::vector<double>& y, std::size_t lag) {
  double energy = 0.0;
  double in_phase = 0.0;
  double quadrature = 0.0;
  for (std::size_t i = 0; i < 480; ++i) {
    const double t = static_cast<double>(i);
    const double window = 0.5 * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * t / 480.0));
    const double s = window * std::sin(2.0 * std::numbers::pi * 1'000.0 * t / rate);
    const double c = window * std::cos(2.0 * std::numbers::pi * 1'000.0 * t / rate);
    energy += s * s;
    in_phase += s * y[lag + i];
    quadrature += c * y[lag + i];
  }
  return std::hypot(in_phase, quadrature) / energy;
}

double db(double gain) { return 20.0 * std::log10(std::fabs(gain) + 1e-30); }

// Every repeat the model draws lands where the kernel puts it, at the
// level the kernel gives it, on the side it comes out of.
void echo_train_matches_the_kernel() {
  struct Case {
    const char* name;
    double routing;
    double feedback;
    double sync;
    double bpm;
  };
  for (const Case c : {Case{"stereo", 0, 0.5, 0, 120}, Case{"ping-pong", 1, 0.7, 0, 120},
                       Case{"mono", 2, 0.6, 0, 120}, Case{"synced", 0, 0.4, 1, 100}}) {
    auto values = preset("Init");
    values[index("relay.routing")] = c.routing;
    values[index("relay.feedback")] = c.feedback;
    values[index("relay.time")] = 250.0;
    values[index("relay.offset")] = c.routing == 2 ? 0.0 : 0.2;
    values[index("relay.sync")] = c.sync;
    values[index("relay.division")] = 5.0; // 1/8
    values[index("relay.mix")] = 1.0;
    const auto train = q::relay_echo_train(table(), values, c.bpm, 4.0, 1.6, 64);
    require(train.size() >= 4, std::string(c.name) + ": the train has repeats");
    const auto out = render(values, static_cast<std::size_t>(1.8 * rate), c.bpm);
    for (const auto& e : train) {
      const auto lag = static_cast<std::size_t>(std::lround(e.seconds * rate));
      const auto& heard = e.channel == 1 ? out.right : out.left;
      const double measured = gain_at(heard, lag);
      require(std::fabs(db(measured) - db(e.level)) < 0.1,
              std::string(c.name) + ": repeat at " + std::to_string(e.seconds) + " s is " +
                  std::to_string(db(measured)) + " dB, drawn " + std::to_string(db(e.level)));
      if (c.routing == 1) {
        const auto& other = e.channel == 1 ? out.left : out.right;
        require(std::fabs(gain_at(other, lag)) < 1e-4,
                std::string(c.name) + ": a ping-pong repeat is on one side only");
      }
      if (e.channel == 2) {
        require(std::fabs(gain_at(out.right, lag) - measured) < 1e-6, "mono repeats on both sides");
      }
    }
  }
  // Synced: an eighth at 100 BPM is 0.3 s, the right line 20% longer.
  auto values = preset("Quarter Echo");
  values[index("relay.division")] = 5.0;
  values[index("relay.offset")] = 0.2;
  const auto times = q::relay_times(table(), values, 100.0, 4.0);
  require(std::fabs(times[0] - 0.3) < 1e-12 && std::fabs(times[1] - 0.36) < 1e-12,
          "synced times follow the tempo");
  // Bars follow the meter.
  values[index("relay.division")] = 13.0; // 1 Bar
  require(std::fabs(q::relay_times(table(), values, 120.0, 3.0)[0] - 1.5) < 1e-12,
          "a bar in 3/4 at 120 BPM is 1.5 s");
}

// The loop's cuts and character darken every pass.
void repeats_darken_and_diffusion_smears() {
  auto values = preset("Analog Bucket");
  values[index("relay.diffusion")] = 0.5;
  const auto train = q::relay_echo_train(table(), values, 120.0, 4.0, 6.0, 64);
  require(train.size() >= 3, "analog repeats");
  for (std::size_t i = 1; i < train.size(); ++i) {
    if (train[i].channel == train[i - 1].channel) {
      require(train[i].brightness < train[i - 1].brightness, "each pass is darker");
      require(train[i].spread_ms > train[i - 1].spread_ms, "and more diffuse");
    }
  }
  values[index("relay.feedback")] = 0.0;
  require(q::relay_echo_train(table(), values, 120.0, 4.0, 6.0, 64).size() == 2,
          "no feedback: one repeat per side");
}

// ---- The panel --------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller, RelayPanelModel& model) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/relay"
      RelayPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/RelayPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Relay panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)},
       {QStringLiteral("relay"), QVariant::fromValue<QObject*>(&model)}});
  require(object != nullptr,
          "Relay panel was not created: " + component.errorString().toStdString());
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
}

// Visible on screen, through every ancestor.
bool shown(QQuickItem* target) { return target->isVisible(); }

struct Fixture {
  InMemoryNativeSynthAdapter adapter;
  NativeSynthDeviceController controller;
  RelayPanelModel model;
  std::unique_ptr<Panel> panel;
  explicit Fixture(std::vector<double> values)
      : adapter(std::move(values)), controller(table(), adapter), model(controller, adapter),
        panel(load_panel(controller, model)) {}
};

void panel_covers_every_parameter() {
  Fixture f(synth::default_values(table()));
  require(f.panel->root->implicitWidth() >= 1200 && f.panel->root->implicitHeight() >= 660,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool found = false;
    for (const char* kind : {"relayKnob:", "relaySegment:", "relayDropdown:"}) {
      found = found || find_item(f.panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(found, "no control for " + id.toStdString());
  }
}

void sync_swaps_time_for_division() {
  Fixture f(synth::default_values(table()));
  auto& panel = *f.panel;
  auto* time = item(panel, QStringLiteral("relayKnob:relay.time"));
  auto* division = item(panel, QStringLiteral("relayDropdown:relay.division"));
  require(!shown(time) && shown(division), "Quarter Echo is synced: the division shows");
  require(item(panel, QStringLiteral("relayTimeReadout"))->property("text").toString() ==
              QStringLiteral("500 ms"),
          "a quarter at 120 BPM reads 500 ms");
  f.model.set_tempo(100.0);
  QCoreApplication::processEvents();
  require(item(panel, QStringLiteral("relayTimeReadout"))->property("text").toString() ==
              QStringLiteral("600 ms"),
          "the readout follows the tempo");
  auto* sync = item(panel, QStringLiteral("relaySegment:relay.sync"));
  click(panel, sync->mapToScene(QPointF(sync->width() * 0.25, sync->height() / 2)).toPoint());
  QCoreApplication::processEvents();
  require(f.controller.choice(QStringLiteral("relay.sync")) == 0, "clicking Off unsyncs");
  require(shown(time) && !shown(division), "free time shows the time knob");
  require(item(panel, QStringLiteral("relayTimeReadout"))->property("text").toString() ==
              QStringLiteral("375 ms"),
          "and the free time");
}

void character_bay_and_echo_view_follow_the_values() {
  Fixture f(synth::default_values(table()));
  auto& panel = *f.panel;
  require(shown(item(panel, QStringLiteral("relayDigital"))) &&
              !shown(item(panel, QStringLiteral("relayReels"))),
          "digital shows the DSP readout");
  auto* character = item(panel, QStringLiteral("relaySegment:relay.character"));
  click(
      panel,
      character->mapToScene(QPointF(character->width() * 0.5, character->height() / 2)).toPoint());
  QCoreApplication::processEvents();
  require(f.controller.choice(QStringLiteral("relay.character")) == 1, "the middle key is Tape");
  auto* reels = item(panel, QStringLiteral("relayReels"));
  require(shown(reels) && reels->property("spinning").toBool(), "tape reels turn");
  require(f.panel->root->property("theme").value<QObject*>()->property("accent").value<QColor>() ==
              QColor(QStringLiteral("#53c3d0")),
          "the accent follows the character");
  f.controller.set_choice(QStringLiteral("relay.character"), 2);
  QCoreApplication::processEvents();
  require(shown(item(panel, QStringLiteral("relayChip"))) && !shown(reels),
          "analog shows the bucket-brigade chip");

  auto* view = item(panel, QStringLiteral("relayEchoView"));
  const int before = view->property("echoCount").toInt();
  // The screen shows about six repeats of the line; less feedback falls
  // below -60 dB sooner.
  f.controller.set_value(QStringLiteral("relay.feedback"), 0.1);
  QCoreApplication::processEvents();
  require(view->property("echoCount").toInt() < before, "less feedback draws fewer repeats");
  f.controller.set_value(QStringLiteral("relay.feedback"), 0.0);
  QCoreApplication::processEvents();
  require(view->property("echoCount").toInt() == 2, "no feedback: one repeat per side");
  f.controller.set_choice(QStringLiteral("relay.routing"), 2);
  QCoreApplication::processEvents();
  require(view->property("echoCount").toInt() == 1, "mono: one repeat for both sides");
  require(item(panel, QStringLiteral("relayKnob:relay.width"))->property("inactive").toBool(),
          "width does nothing in mono");
}

void presets_and_status() {
  Fixture f(synth::default_values(table()));
  auto& panel = *f.panel;
  require(item(panel, QStringLiteral("relayPresetName"))->property("text").toString() ==
              QStringLiteral("Quarter Echo"),
          "defaults are Quarter Echo");
  click(panel, centre(item(panel, QStringLiteral("relayPresetNext"))));
  require(f.controller.preset_name() == QStringLiteral("Dotted Eighth"), "next steps the preset");
  click(panel, centre(item(panel, QStringLiteral("relayPresetPrevious"))));
  require(f.controller.preset_name() == QStringLiteral("Quarter Echo"), "previous steps back");
  auto* menu = item(panel, QStringLiteral("relayMenu"));
  click(panel, centre(item(panel, QStringLiteral("relayPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel.window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  f.controller.apply_preset(preset_index("Tape Echo"));
  QCoreApplication::processEvents();
  require(f.panel->root->property("character").toInt() == 1, "presets set the character");
  require(shown(item(panel, QStringLiteral("relayKnob:relay.time"))), "Tape Echo is free time");

  f.adapter.set_status(
      {.saved_revision = 9, .audible_revision = 9, .deferred_changes = true, .message = {}});
  f.controller.refresh();
  QCoreApplication::processEvents();
  require(item(panel, QStringLiteral("relayStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#c76a12")),
          "deferred status is highlighted");
}

std::vector<float> relay_frame(float wet_rms, float duck_db) {
  namespace t = relay::telemetry;
  std::vector<float> frame(t::size);
  frame[t::time_left_ms] = 500.0F;
  frame[t::time_right_ms] = 500.0F;
  frame[t::duck_db] = duck_db;
  frame[t::wet_rms] = wet_rms;
  return frame;
}

// The meter follows live telemetry: 0 VU at -18 dBFS, and the duck strip.
void meters_follow_live_telemetry() {
  Fixture f(preset("Ducked Vocal"));
  auto& panel = *f.panel;
  auto* vu = item(panel, QStringLiteral("relayVu"));
  QTest::qWait(300);
  require(!f.panel->root->property("liveRunning").toBool(), "no telemetry: idle");
  f.adapter.set_telemetry(relay_frame(static_cast<float>(std::pow(10.0, -18.0 / 20.0)), -12.0F));
  require(QTest::qWaitFor([&] { return f.panel->root->property("liveRunning").toBool(); }, 1000),
          "telemetry makes the panel live");
  require(std::fabs(vu->property("vu").toDouble()) < 0.05, "-18 dBFS reads 0 VU");
  require(
      std::fabs(item(panel, QStringLiteral("relayDuckMeter"))->property("reduction").toDouble() -
                12.0) < 1e-3,
      "the duck strip shows 12 dB");
  require(shown(item(panel, QStringLiteral("relayLiveLamp"))), "the screen's live lamp lights");
  f.adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !f.panel->root->property("liveRunning").toBool(); }, 1500),
          "telemetry gone: idle again");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("RELAY_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"Quarter Echo", "relay.feedback", "/relay_quarter_echo.png"},
        Shot{"Tape Echo", "relay.mod_depth", "/relay_tape_echo.png"},
        Shot{"Ping-Pong Eighths", "relay.routing", "/relay_ping_pong_eighths.png"},
        Shot{"Diffuse Ambience", "relay.diffusion", "/relay_diffuse_ambience.png"}}) {
    Fixture f(preset(shot.preset));
    auto* theme = f.panel->root->property("theme").value<QObject*>();
    theme->setProperty("lastTouched", QString::fromLatin1(shot.touched));
    QTest::qWait(250);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(f.panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  Fixture f(preset("Ducked Vocal"));
  f.adapter.set_telemetry(relay_frame(static_cast<float>(std::pow(10.0, -16.0 / 20.0)), -9.5F));
  QTest::qWait(700);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/relay_live_meters.png");
  require(f.panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    echo_train_matches_the_kernel();
    repeats_darken_and_diffusion_smears();
    panel_covers_every_parameter();
    sync_swaps_time_for_division();
    character_bay_and_echo_view_follow_the_values();
    presets_and_status();
    meters_follow_live_telemetry();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "relay panel contract passed\n";
  return 0;
}
