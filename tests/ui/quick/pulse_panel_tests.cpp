#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;

constexpr std::array<std::string_view, 12> pad_ids{"kick",    "kick2",    "snare", "rim",
                                                   "clap",    "tom_low",  "chh",   "tom_mid",
                                                   "cowbell", "tom_high", "ohh",   "cymbal"};
constexpr std::size_t kick = 0;
constexpr std::size_t snare = 2;
constexpr std::size_t clap = 4;
constexpr std::size_t tom_low = 5;
constexpr std::size_t chh = 6;
constexpr std::size_t cowbell = 8;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() {
  return *synth::pulse::engine().descriptor;
}

std::size_t index(std::string_view id) {
  return *oscillate::native_dsp::find_parameter(table(), id);
}

int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString pad_parameter(std::size_t pad, std::string_view control) {
  return QStringLiteral("pulse.") + qs(pad_ids[pad]) + QStringLiteral(".") + qs(control);
}

// ---- Visuals ------------------------------------------------------------------

void pad_visuals_follow_the_kernel_laws() {
  namespace q = oscillate::quick;
  auto values = oscillate::native_dsp::default_values(table());

  // Kick: body falls 60 dB at Decay, the 3 ms click is long gone by then.
  const double decay = values[index("pulse.kick.decay")];
  const double click = values[index("pulse.kick.character")];
  const auto envelope = q::pulse_pad_envelope(table(), values, kick, decay, 101);
  require(envelope.size() == 101 && std::fabs(envelope.front() - 1.0) < 1e-12,
          "envelopes start at their peak");
  require(std::fabs(envelope.back() - 1e-3 / (1.0 + click)) < 1e-6, "kick is -60 dB at Decay");
  require(std::is_sorted(envelope.rbegin(), envelope.rend()), "kick envelope only falls");

  // Kick pitch: 50 Hz end, start raised by 6 x Tone, 30 ms sweep; Tune shifts both.
  auto pitch = q::pulse_pad_pitch(table(), values, kick, 1.0, 11);
  const double tone = values[index("pulse.kick.tone")];
  require(std::fabs(pitch.front() - 50.0 * (1.0 + 6.0 * tone)) < 1e-9, "kick sweep starts high");
  require(std::fabs(pitch.back() - 50.0) < 1e-6, "kick settles on 50 Hz");
  values[index("pulse.kick.tune")] = 12.0;
  pitch = q::pulse_pad_pitch(table(), values, kick, 1.0, 11);
  require(std::fabs(pitch.back() - 100.0) < 1e-6, "an octave of tune doubles the body");
  const auto tom = q::pulse_pad_pitch(table(), values, tom_low, 1.0, 3);
  require(tom.size() == 3 && std::fabs(tom.back() - 90.0) < 0.01, "the low tom ends on 90 Hz");
  require(q::pulse_pad_pitch(table(), values, snare, 1.0, 8).empty() &&
              q::pulse_pad_pitch(table(), values, clap, 1.0, 8).empty(),
          "pads without a swept body have no pitch sketch");

  // Clap: four bursts at 0.008 + 0.008 x Spread, each starting at full level.
  const double spacing = 0.008 + 0.008 * values[index("pulse.clap.character")];
  const std::size_t points = 2'001;
  const double window = 6.0 * spacing;
  const auto burst = q::pulse_pad_envelope(table(), values, clap, window, points);
  int peaks = 0;
  for (int b = 0; b < 4; ++b) {
    const auto at = static_cast<std::size_t>(std::lround(b * spacing / window * (points - 1)));
    // The first sample at or after the burst time.
    peaks += std::max(burst[at], burst[at + 1]) > 0.95 ? 1 : 0;
  }
  require(peaks == 4, "the clap shows four bursts");
  require(burst[static_cast<std::size_t>(std::lround(5.0 / 6.0 * (points - 1)))] < 0.61,
          "the clap tail starts at 0.6");

  // Snare: the body falls at half the noise's Decay.
  const auto snare_envelope =
      q::pulse_pad_envelope(table(), values, snare, values[index("pulse.snare.decay")], 2);
  const double c = values[index("pulse.snare.character")];
  require(std::fabs(snare_envelope.back() -
                    ((1.0 - 0.5 * c) * 1e-6 + (0.3 + c) * 1e-3) / (1.3 + 0.5 * c)) < 1e-9,
          "snare body and noise fall at their own rates");

  // Bands: the hat band-pass sits above its 6.5 kHz high-pass; the cowbell is a Q 2 peak.
  const double hat_centre = 7'000.0 * (0.6 + 0.8 * values[index("pulse.chh.tone")]);
  require(q::pulse_pad_band_db(table(), values, chh, hat_centre) > -4.0 &&
              q::pulse_pad_band_db(table(), values, chh, 500.0) < -40.0,
          "hats pass a high band only");
  const double bell = 2'640.0 * (0.8 + 0.4 * values[index("pulse.cowbell.tone")]);
  require(std::fabs(q::pulse_pad_band_db(table(), values, cowbell, bell) - 20.0 * std::log10(2.0)) <
              1e-6,
          "the cowbell band peaks at Q");
  require(q::pulse_pad_band_db(table(), values, kick, 1'000.0) == 0.0, "kicks have no band");

  // Other engines and pads out of range draw nothing.
  const auto& prism = *synth::prism::engine().descriptor;
  const auto prism_values = oscillate::native_dsp::default_values(prism);
  require(q::pulse_pad_envelope(prism, prism_values, kick, 1.0, 8).empty() &&
              q::pulse_pad_pitch(prism, prism_values, kick, 1.0, 8).empty(),
          "only Pulse has pad sketches");
  require(q::pulse_pad_envelope(table(), values, 12, 1.0, 8).empty(), "pad 12 does not exist");
}

void controller_exposes_pad_sketches() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(!controller.has_filter_response(), "Pulse draws no synth filter");
  require(controller.pad_envelope(0, 1.0, 64).size() == 64, "pad envelope reaches QML");
  require(controller.pad_pitch(0, 0.3, 32).size() == 32, "kick pitch reaches QML");
  require(controller.pad_pitch(6, 0.3, 32).isEmpty(), "hats have no pitch");
  require(controller.pad_band(6, 50).size() == 50, "hat band reaches QML");
  require(controller.pad_envelope(-1, 1.0, 64).isEmpty(), "negative pads are rejected");

  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter prism_adapter(oscillate::native_dsp::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, prism_adapter);
  require(prism_controller.pad_envelope(0, 1.0, 64).isEmpty() &&
              prism_controller.pad_band(0, 64).isEmpty(),
          "other engines have no pad sketches");
}

// ---- QML ----------------------------------------------------------------------

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
      import "qrc:/pulse"
      PulsePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/PulsePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Pulse panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Pulse panel was not created: " + component.errorString().toStdString());
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

void wheel(Panel& panel, QPoint at, int delta) {
  QWheelEvent event(at, panel.window->mapToGlobal(at), QPoint(), QPoint(0, delta), Qt::NoButton,
                    Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(panel.window.get(), &event);
}

int selected_pad(const Panel& panel) { return panel.root->property("selectedPad").toInt(); }

void select_pad(Panel& panel, int pad) {
  panel.root->setProperty("selectedPad", pad);
  QCoreApplication::processEvents();
}

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 680,
          "panel has its designed size");

  // Level and pan live on the mixer for every pad, the bus on its own plate,
  // and the rest on the editor once their pad is selected.
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (int pad = 0; pad < 12 && !shown; ++pad) {
      select_pad(*panel, pad);
      shown = find_item(panel->root, QStringLiteral("pulseKnob:") + id) != nullptr ||
              find_item(panel->root, QStringLiteral("pulseFader:") + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
}

void pads_select_the_editor_and_trigger() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QSignalSpy triggered(panel->root, SIGNAL(padTriggered(int)));

  require(selected_pad(*panel) == 0, "the kick is selected first");
  require(find_item(panel->root, QStringLiteral("pulseKnob:pulse.kick.decay")) != nullptr,
          "the editor starts on the kick");

  click(*panel, item(*panel, QStringLiteral("pulsePad:5")));
  require(selected_pad(*panel) == 5, "clicking a pad selects it");
  require(find_item(item(*panel, QStringLiteral("pulsePad:5")), QStringLiteral("pulsePadGlow"))
                  ->opacity() > 0.0,
          "a pressed pad flashes");
  require(triggered.size() == 1 && triggered.front().front().toInt() == 41,
          "a pad click asks root to play its key");
  require(find_item(panel->root, QStringLiteral("pulseKnob:pulse.kick.decay")) == nullptr &&
              find_item(panel->root, QStringLiteral("pulseKnob:pulse.tom_low.decay")) != nullptr,
          "the editor follows the selected pad");
  require(item(*panel, QStringLiteral("pulsePadName"))->property("text").toString() ==
              QStringLiteral("LOW TOM"),
          "the badge names the pad");
  require(item(*panel, QStringLiteral("pulseKnob:pulse.tom_low.character"))
                  ->property("label")
                  .toString() == QStringLiteral("Noise"),
          "toms call Character Noise");
  require(item(*panel, QStringLiteral("pulseShape"))->property("pitched").toBool(),
          "toms show their pitch drop");

  // Editing through the editor reaches the selected pad's parameter.
  auto* decay = item(*panel, QStringLiteral("pulseKnob:pulse.tom_low.decay"));
  decay->forceActiveFocus();
  const double before = controller.value(QStringLiteral("pulse.tom_low.decay"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("pulse.tom_low.decay")) > before,
          "the editor edits the selected pad");
  require(controller.value(QStringLiteral("pulse.kick.decay")) ==
              table().parameters[index("pulse.kick.decay")].default_value,
          "other pads are untouched");

  // Character names follow the pad.
  for (const auto& [pad, name] :
       {std::pair{0, "Click"}, std::pair{1, "Attack"}, std::pair{2, "Snappy"},
        std::pair{4, "Spread"}, std::pair{6, "Noise"}, std::pair{8, "Body"},
        std::pair{11, "Noise"}}) {
    select_pad(*panel, pad);
    require(item(*panel, QStringLiteral("pulseKnob:") +
                             pad_parameter(static_cast<std::size_t>(pad), "character"))
                    ->property("label")
                    .toString() == QString::fromLatin1(name),
            std::string("Character reads ") + name);
  }
  select_pad(*panel, static_cast<int>(clap));
  require(!item(*panel, QStringLiteral("pulseShape"))->property("pitched").toBool(),
          "the clap shows its noise band");

  // Keyboard on the pads: arrows walk, Space plays.
  auto* pad = item(*panel, QStringLiteral("pulsePad:4"));
  pad->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(selected_pad(*panel) == 5, "Right selects the next pad");
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(triggered.size() == 2 && triggered.back().front().toInt() == 41, "Space plays the pad");
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(selected_pad(*panel) == 3, "Left walks back");

  // The instrument strip selects without playing.
  select_pad(*panel, 0);
  require(triggered.size() == 2, "selecting by property does not play");
}

void controls_edit_with_gestures_and_keys() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);

  // A knob drag is one gesture.
  auto* tune = item(*panel, QStringLiteral("pulseKnob:pulse.kick.tune"));
  auto start = adapter.submissions().size();
  const QPoint knob = tune->mapToScene(QPointF(tune->width() / 2, 24)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 15));
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 30));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob - QPoint(0, 30));
  auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a knob drag is begin, updates and commit");
  require(controller.value(QStringLiteral("pulse.kick.tune")) > 0.0, "dragging up tunes up");

  // Shift drags finely.
  const double coarse = controller.normalised(QStringLiteral("pulse.kick.tune"));
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob);
  // QTest moves carry no modifiers, so send the held-Shift move directly.
  const QPointF moved = knob - QPoint(0, 30);
  QMouseEvent shift_move(QEvent::MouseMove, moved, moved, panel->window->mapToGlobal(moved),
                         Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
  QCoreApplication::sendEvent(panel->window.get(), &shift_move);
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::ShiftModifier, knob - QPoint(0, 30));
  const double fine_step = controller.normalised(QStringLiteral("pulse.kick.tune")) - coarse;
  require(fine_step > 0.0 && fine_step < 30.0 / 1'000.0, "Shift drags finely");

  // Double-click resets.
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  require(controller.is_default(QStringLiteral("pulse.kick.tune")), "double-click resets");

  // The wheel steps.
  wheel(*panel, knob, 120);
  require(controller.value(QStringLiteral("pulse.kick.tune")) > 0.0, "the wheel steps up");

  // A mixer fader drags as one gesture and lowers the level.
  auto* level = item(*panel, QStringLiteral("pulseFader:pulse.snare.level"));
  start = adapter.submissions().size();
  const double level_before = controller.value(QStringLiteral("pulse.snare.level"));
  const QPoint grab = centre(level);
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, grab);
  QTest::mouseMove(panel->window.get(), grab + QPoint(0, 10));
  QTest::mouseMove(panel->window.get(), grab + QPoint(0, 20));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, grab + QPoint(0, 20));
  require(adapter.submissions().size() > start + 2 &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "a fader drag is a gesture");
  require(controller.value(QStringLiteral("pulse.snare.level")) < level_before,
          "dragging down lowers the level");
  require(level->activeFocusOnTab(), "faders are keyboard reachable");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(QStringLiteral("pulse.snare.level")), "Delete resets a fader");

  // Keyboard on a bus knob.
  auto* glue = item(*panel, QStringLiteral("pulseKnob:pulse.bus.glue"));
  require(glue->activeFocusOnTab(), "knobs are keyboard reachable");
  glue->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("pulse.bus.glue")) > 0.25, "Up raises Glue");
  QTest::keyClick(panel->window.get(), Qt::Key_Down, Qt::ShiftModifier);
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(QStringLiteral("pulse.bus.glue")), "Delete resets Glue");

  // The glue meter marks the threshold Glue sets.
  controller.set_value(QStringLiteral("pulse.bus.glue"), 0.5);
  QCoreApplication::processEvents();
  auto* meter = item(*panel, QStringLiteral("pulseGlueMeter"));
  require(std::fabs(meter->property("threshold").toDouble() + 12.0) < 1e-9,
          "Glue 50% puts the threshold at -12 dB");
}

void kit_browser_and_status() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("pulseKitName"))->property("text").toString() ==
              QStringLiteral("808 Classic"),
          "defaults are 808 Classic");

  click(*panel, item(*panel, QStringLiteral("pulseKitNext")));
  require(controller.preset_name() == QStringLiteral("808 Boom"), "next steps the kit");
  click(*panel, item(*panel, QStringLiteral("pulseKitPrevious")));
  require(controller.preset_name() == QStringLiteral("808 Classic"), "previous steps back");

  auto* box = item(*panel, QStringLiteral("pulseKitBox"));
  auto* menu = item(*panel, QStringLiteral("pulseMenu"));
  click(*panel, box);
  require(menu->isVisible(), "the kit menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.preset_name() == QStringLiteral("808 Boom"),
          "the keyboard chooses the next kit");
  require(item(*panel, QStringLiteral("pulseKitName"))->property("text").toString() ==
              QStringLiteral("808 Boom"),
          "the header shows the kit");
  require(controller.value(QStringLiteral("pulse.kick.decay")) == 1.8, "the kit is applied");

  // Deferred status reaches the header.
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("pulseStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#e0901f")),
          "deferred status is highlighted");
}

std::vector<float> pulse_frame(float reduction, int pad, float level) {
  namespace t = synth::pulse::telemetry;
  std::vector<float> frame(t::size);
  frame[t::gain_reduction] = reduction;
  frame[t::first_pad + static_cast<std::size_t>(pad)] = level;
  return frame;
}

// The glue meter and the pads follow the running kit (ADR-0037); with no
// telemetry the meter rests and the pads are dark.
void meter_and_pads_follow_live_telemetry() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* meter = item(*panel, QStringLiteral("pulseGlueMeter"));
  const auto lit = [&](int segment) {
    return item(*panel, QStringLiteral("pulseGlueSegment:%1").arg(segment))
        ->property("lit")
        .toBool();
  };
  const auto pad_light = [&](int pad) {
    return item(*panel, QStringLiteral("pulsePadLive:%1").arg(pad))->opacity();
  };
  QTest::qWait(300);
  require(meter->property("reduction").toDouble() == 0.0 && !lit(0),
          "no telemetry: the meter rests");
  require(pad_light(2) == 0.0, "no telemetry: pads are dark");

  adapter.set_telemetry(pulse_frame(7.0F, 2, 1.0F));
  require(QTest::qWaitFor([&] { return meter->property("reduction").toDouble() == 7.0; }, 1000),
          "the meter reads the live reduction");
  require(lit(0) && lit(1) && lit(2) && lit(3) && !lit(4), "7 dB lights four 2 dB segments");
  require(pad_light(2) > 0.5 && pad_light(0) == 0.0, "the sounding pad lights, others stay dark");

  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return meter->property("reduction").toDouble() == 0.0; }, 1000) &&
              pad_light(2) == 0.0,
          "telemetry gone: back to rest");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("PULSE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    int pad;
    const char* file;
  };
  for (const Shot shot : {Shot{"808 Classic", 0, "/pulse_808_classic.png"},
                          Shot{"909 Techno", 0, "/pulse_909_techno.png"},
                          Shot{"909 Techno", 6, "/pulse_909_techno_hat.png"},
                          Shot{"Trap", 4, "/pulse_trap_clap.png"}}) {
    auto values = oscillate::native_dsp::preset_values(
                      table(), table().presets[static_cast<std::size_t>(preset_index(shot.preset))])
                      .value();
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    select_pad(*panel, shot.pad);
    QTest::qWait(120);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto frame = pulse_frame(9.0F, 0, 0.9F);
  frame[synth::pulse::telemetry::first_pad + 6] = 0.5F;
  adapter.set_telemetry(frame);
  QTest::qWait(400);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/pulse_live.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    pad_visuals_follow_the_kernel_laws();
    controller_exposes_pad_sketches();
    panel_covers_every_parameter();
    pads_select_the_editor_and_trigger();
    controls_edit_with_gestures_and_keys();
    kit_browser_and_status();
    meter_and_pads_follow_live_telemetry();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pulse panel contract passed\n";
  return 0;
}
