#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/effects/orbit.hpp"
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
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Orbit panel (ADR-0040): the controller's telemetry decode, and the QML
// card driven by the effect's table: every parameter has a control, each
// mode shows only the controls it uses, presets and status, the live
// orrery, and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace orbit = oscillate::native_dsp::effects::orbit;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *orbit::engine().descriptor; }

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

std::vector<float> frame(float left, float right, float sweep_left, float sweep_right) {
  namespace t = orbit::telemetry;
  std::vector<float> out(t::size);
  out[t::lfo_left] = left;
  out[t::lfo_right] = right;
  out[t::sweep_left] = sweep_left;
  out[t::sweep_right] = sweep_right;
  return out;
}

void controller_decodes_orbit_telemetry() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.engine_id() == QStringLiteral("orbit"), "the controller is Orbit's");
  require(!controller.telemetry().value(QStringLiteral("live")).toBool(), "nothing playing");
  adapter.set_telemetry(frame(0.5F, -0.25F, 12.5F, 13.25F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool(), "a frame is live");
  require(live.value(QStringLiteral("lfo_left")).toDouble() == 0.5 &&
              live.value(QStringLiteral("lfo_right")).toDouble() == -0.25 &&
              live.value(QStringLiteral("sweep_left")).toDouble() == 12.5 &&
              live.value(QStringLiteral("sweep_right")).toDouble() == 13.25,
          "the layout decodes in orbit.hpp's order");
  adapter.set_telemetry({0.5F, 0.5F});
  require(!controller.telemetry().value(QStringLiteral("live")).toBool(),
          "a short frame is not decoded");
}

// ---- QML ----------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller, bool animate = true) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/orbit"
      OrbitPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/OrbitPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Orbit panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)},
       {QStringLiteral("animate"), animate}});
  require(object != nullptr,
          "Orbit panel was not created: " + component.errorString().toStdString());
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

void wheel(Panel& panel, QPoint point, int delta) {
  QWheelEvent event(point, panel.window->mapToGlobal(point), QPoint(), QPoint(0, delta),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(panel.window.get(), &event);
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1100 && panel->root->implicitHeight() >= 680,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"orbitKnob:", "orbitSegment:", "orbitDropdown:", "orbitTabs:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
}

// Helper sentences are set in sentence case, never in capitals.
void helper_sentences_are_in_sentence_case() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  for (const char* name : {"orbitToneNote", "orbitVoiceNote"}) {
    const QString text = text_of(*panel, QString::fromLatin1(name));
    // A capital first letter and lower-case letters after it.
    const auto first = std::ranges::find_if(text, [](QChar c) { return c.isLetter(); });
    require(first != text.end() && first->isUpper() &&
                std::ranges::any_of(text, [](QChar c) { return c.isLower(); }),
            std::string(name) + " is in sentence case: " + text.toStdString());
  }
}

// Each mode shows only what it uses: time for the delay modes, feedback for
// chorus, flanger and phaser, voices for chorus, stages and centre for the
// phaser; ensemble and tremolo explain themselves instead.
void each_mode_shows_its_own_controls() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* theme = panel->root->property("theme").value<QObject*>();
  struct Expect {
    bool time, feedback, voices, phaser, note;
    const char* colour;
  };
  const Expect expected[7] = {
      {true, true, true, false, false, "#b9a2ff"},   // Chorus
      {false, false, false, false, true, "#ffc56e"}, // Ensemble
      {true, false, false, false, false, "#ff90d9"}, // Dimension
      {true, true, false, false, false, "#7fd8ff"},  // Flanger
      {false, true, false, true, false, "#6dffd3"},  // Phaser
      {true, false, false, false, false, "#ffa07c"}, // Vibrato
      {false, false, false, false, true, "#93a9ff"}, // Tremolo
  };
  const auto shown = [&](const char* name) {
    return item(*panel, QString::fromLatin1(name))->isVisible();
  };
  for (int mode = 0; mode < 7; ++mode) {
    click(*panel, centre(item(*panel, QStringLiteral("orbitMode:%1").arg(mode))));
    require(controller.choice(QStringLiteral("orbit.mode")) == mode, "a tab picks its mode");
    const auto& e = expected[mode];
    const std::string where = " in mode " + std::to_string(mode);
    require(shown("orbitCell:orbit.time") == e.time, "time" + where);
    require(shown("orbitCell:orbit.feedback") == e.feedback, "feedback" + where);
    require(shown("orbitSegment:orbit.voices") == e.voices, "voices" + where);
    require(shown("orbitSegment:orbit.stages") == e.phaser &&
                shown("orbitCell:orbit.centre") == e.phaser,
            "phaser controls" + where);
    require(shown("orbitVoiceNote") == e.note, "the voice note" + where);
    require(theme->property("accent").value<QColor>() == QColor(QString::fromLatin1(e.colour)),
            "the mode's colour" + where);
  }
  require(text_of(*panel, QStringLiteral("orbitReadout")).contains(QStringLiteral("MODE")),
          "the display reads back the mode");
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(controller.choice(QStringLiteral("orbit.mode")) == 5, "Left steps back to vibrato");
  require(item(*panel, QStringLiteral("orbitKnob:orbit.time"))->property("label").toString() ==
              QStringLiteral("Delay"),
          "time reads as delay");
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(controller.choice(QStringLiteral("orbit.mode")) == 3 &&
              item(*panel, QStringLiteral("orbitKnob:orbit.time"))->property("label").toString() ==
                  QStringLiteral("Max Delay"),
          "the flanger's time is its maximum delay");
}

void controls_edit_through_the_controller() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const double depth = controller.value(QStringLiteral("orbit.depth"));
  wheel(*panel, centre(item(*panel, QStringLiteral("orbitKnob:orbit.depth"))), 120);
  require(controller.value(QStringLiteral("orbit.depth")) > depth, "the wheel raises depth");
  require(text_of(*panel, QStringLiteral("orbitReadout")).contains(QStringLiteral("DEPTH")),
          "the display follows the touched control");
  require(item(*panel, QStringLiteral("orbitKnob:orbit.rate"))->property("inactive").toBool() ==
              false,
          "rate is live while free running");
  auto* sync = item(*panel, QStringLiteral("orbitSegment:orbit.sync"));
  click(*panel, sync->mapToScene(QPointF(sync->width() * 0.75, sync->height() / 2)).toPoint());
  require(controller.choice(QStringLiteral("orbit.sync")) == 1, "the sync switch turns on");
  auto* rate = item(*panel, QStringLiteral("orbitKnob:orbit.rate"));
  require(rate->property("inactive").toBool() &&
              !rate->property("inactiveReason").toString().isEmpty(),
          "a synced LFO dims its free rate and says why");
}

void presets_and_status() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("orbitPresetName")) == QStringLiteral("Classic Chorus"),
          "the default preset is named");
  controller.apply_preset(preset_index("Jet Flanger"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("orbitPresetName")) == QStringLiteral("Jet Flanger") &&
              controller.choice(QStringLiteral("orbit.mode")) == 3,
          "a preset sets its mode");
  click(*panel, centre(item(*panel, QStringLiteral("orbitPresetNext"))));
  require(text_of(*panel, QStringLiteral("orbitPresetName")) == QStringLiteral("Negative Flanger"),
          "the arrows step presets");
  adapter.set_status({.saved_revision = 9,
                      .audible_revision = 9,
                      .deferred_changes = true,
                      .editable = true,
                      .message = {}});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("orbitStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#ffb24a")),
          "a deferred change shows as a warning");
}

void the_orrery_follows_live_telemetry() {
  InMemoryNativeSynthAdapter adapter(preset("Classic Chorus"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller, false);
  QTest::qWait(300);
  require(!panel->root->property("isLive").toBool(), "no telemetry: preview");
  require(text_of(*panel, QStringLiteral("orbitLiveCaption")).contains(QStringLiteral("PREVIEW")),
          "the sky says it is a preview");
  adapter.set_telemetry(frame(0.6F, -0.3F, 13.75F, 10.5F));
  require(QTest::qWaitFor([&] { return panel->root->property("isLive").toBool(); }, 1000),
          "telemetry goes live");
  require(text_of(*panel, QStringLiteral("orbitSweepLeft")) == QStringLiteral("13.8 ms") &&
              text_of(*panel, QStringLiteral("orbitSweepRight")) == QStringLiteral("10.5 ms"),
          "the sweep readout shows the running delays");
  require(text_of(*panel, QStringLiteral("orbitLiveCaption")) == QStringLiteral("PLAYING"),
          "the sky says it is playing");
  const double angle = panel->root->property("liveAngleLeft").toDouble();
  require(std::fabs(std::cos(angle) - 0.6) < 1e-6, "the left body sits at its LFO value");
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !panel->root->property("isLive").toBool(); }, 1000),
          "telemetry gone: preview again");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("ORBIT_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"Lush Chorus", "orbit.depth", "/orbit_lush_chorus.png"},
                          Shot{"Jet Flanger", "orbit.feedback", "/orbit_jet_flanger.png"},
                          Shot{"Deep Phaser", "orbit.centre", "/orbit_deep_phaser.png"},
                          Shot{"Auto-Pan", "orbit.stereo", "/orbit_auto_pan.png"},
                          Shot{"String Ensemble", "orbit.mode", "/orbit_string_ensemble.png"}}) {
    InMemoryNativeSynthAdapter adapter(preset(shot.preset));
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller, false);
    auto* theme = panel->root->property("theme").value<QObject*>();
    theme->setProperty("lastTouched", QString::fromLatin1(shot.touched));
    panel->root->setProperty("previewPhase", 0.11);
    QTest::qWait(200);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  InMemoryNativeSynthAdapter adapter(preset("Classic Chorus"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller, false);
  adapter.set_telemetry(frame(0.2F, 0.9F, 13.0F, 14.6F));
  QTest::qWait(150);
  adapter.set_telemetry(frame(0.35F, 0.8F, 13.3F, 14.4F));
  QTest::qWait(400);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/orbit_live.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    controller_decodes_orbit_telemetry();
    panel_covers_every_parameter();
    helper_sentences_are_in_sentence_case();
    each_mode_shows_its_own_controls();
    controls_edit_through_the_controller();
    presets_and_status();
    the_orrery_follows_live_telemetry();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "orbit panel contract passed\n";
  return 0;
}
