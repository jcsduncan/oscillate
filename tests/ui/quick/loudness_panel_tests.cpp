#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/loudness.hpp"
#include "oscillate/native_dsp/synth/ember.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Loudness panel (ADR-0042): every parameter on a table-driven control;
// the telemetry decode into the integrated hero, the readout tiles, the bar
// meter and the history, with readings not yet available shown as "—"; the
// distance to target in traffic-light colour and the over-ceiling alarm;
// the EBU +9 and +18 scales; history kept beyond the meter's own 30 s;
// presets and status; and screenshots on request, live from the real
// Loudness meter over music from the native instruments.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace loudness = oscillate::native_dsp::effects::loudness;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *loudness::engine().descriptor; }
int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

std::vector<double> preset_of(const synth::EngineDescriptor& descriptor, std::string_view name) {
  for (const auto& entry : descriptor.presets) {
    if (entry.name == name) {
      return synth::preset_values(descriptor, entry).value();
    }
  }
  throw std::runtime_error("missing preset " + std::string(name));
}

std::vector<double> preset(std::string_view name) { return preset_of(table(), name); }

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
      import "qrc:/loudness"
      LoudnessPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/LoudnessPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Loudness panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Loudness panel was not created: " + component.errorString().toStdString());
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
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// ---- Frames -----------------------------------------------------------------------

struct Reading {
  float momentary = -200.0F;
  float short_term = -200.0F;
  float integrated = -200.0F;
  float range = 0.0F;
  float momentary_max = -200.0F;
  float short_term_max = -200.0F;
  float true_peak = -200.0F;
  float true_peak_left = -200.0F;
  float true_peak_right = -200.0F;
  float true_peak_recent = -200.0F;
  float sample_peak = -200.0F;
  float psr = 0.0F;
  float plr = 0.0F;
  float duration = 0.0F;
};

// A frame with these readings and histories whose last `filled` values are
// `value` plus their index (so appended values can be recognised).
std::vector<float> frame_of(const Reading& r, std::size_t filled = 0, float value = -15.0F) {
  namespace t = loudness::telemetry;
  std::vector<float> frame(t::size, -200.0F);
  frame[t::momentary] = r.momentary;
  frame[t::short_term] = r.short_term;
  frame[t::integrated] = r.integrated;
  frame[t::range] = r.range;
  frame[t::momentary_max] = r.momentary_max;
  frame[t::short_term_max] = r.short_term_max;
  frame[t::true_peak] = r.true_peak;
  frame[t::true_peak_left] = r.true_peak_left;
  frame[t::true_peak_right] = r.true_peak_right;
  frame[t::true_peak_recent] = r.true_peak_recent;
  frame[t::sample_peak] = r.sample_peak;
  frame[t::psr] = r.psr;
  frame[t::plr] = r.plr;
  frame[t::duration] = r.duration;
  for (std::size_t i = loudness::history - filled; i < loudness::history; ++i) {
    frame[t::momentary_history + i] = value - 1.0F + static_cast<float>(i) * 1e-3F;
    frame[t::short_term_history + i] = value + static_cast<float>(i) * 1e-3F;
  }
  return frame;
}

Reading typical() {
  Reading r;
  r.momentary = -13.4F;
  r.short_term = -14.2F;
  r.integrated = -14.6F;
  r.range = 5.3F;
  r.momentary_max = -10.1F;
  r.short_term_max = -12.0F;
  r.true_peak = -1.6F;
  r.true_peak_left = -1.6F;
  r.true_peak_right = -2.2F;
  r.true_peak_recent = -2.0F;
  r.sample_peak = -1.9F;
  r.psr = 12.2F;
  r.plr = 13.0F;
  r.duration = 252.0F;
  return r;
}

QColor colour_of(Panel& panel, const char* name) {
  return panel.root->property("theme").value<QObject*>()->property(name).value<QColor>();
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Streaming (Spotify, YouTube)"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 720,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"loudnessKnob:", "loudnessSegment:", "loudnessToggle:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("loudnessPresetName")) ==
              QStringLiteral("Streaming (Spotify, YouTube)"),
          "the panel names its delivery target");
}

void readings_decode_and_silence_shows_as_dashes() {
  InMemoryNativeSynthAdapter adapter(preset("Streaming (Spotify, YouTube)"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  // Nothing yet: every reading is a dash.
  for (const char* name :
       {"loudnessIntegrated", "loudnessTargetDelta", "loudnessShortTerm", "loudnessMomentary",
        "loudnessRange", "loudnessTruePeak", "loudnessPsr", "loudnessPlr"}) {
    require(text_of(*panel, QString::fromLatin1(name)) == QStringLiteral("—"),
            std::string(name) + " is a dash with nothing measured");
  }
  // A meter warming up: momentary only.
  Reading warming;
  warming.momentary = -18.0F;
  warming.duration = 0.3F;
  adapter.set_telemetry(frame_of(warming));
  require(QTest::qWaitFor(
              [&] {
                return text_of(*panel, QStringLiteral("loudnessMomentary")) ==
                       QStringLiteral("−18.0");
              },
              1000),
          "momentary arrives first");
  require(text_of(*panel, QStringLiteral("loudnessShortTerm")) == QStringLiteral("—") &&
              text_of(*panel, QStringLiteral("loudnessIntegrated")) == QStringLiteral("—"),
          "short-term and integrated wait for their windows");
  // A full reading.
  adapter.set_telemetry(frame_of(typical(), 300));
  require(
      QTest::qWaitFor(
          [&] {
            return text_of(*panel, QStringLiteral("loudnessIntegrated")) == QStringLiteral("−14.6");
          },
          1000),
      "integrated decodes: " + text_of(*panel, QStringLiteral("loudnessIntegrated")).toStdString());
  require(text_of(*panel, QStringLiteral("loudnessShortTerm")) == QStringLiteral("−14.2") &&
              text_of(*panel, QStringLiteral("loudnessShortTermSub")) ==
                  QStringLiteral("MAX −12.0"),
          "short-term and its maximum");
  require(text_of(*panel, QStringLiteral("loudnessMomentary")) == QStringLiteral("−13.4") &&
              text_of(*panel, QStringLiteral("loudnessMomentarySub")) ==
                  QStringLiteral("MAX −10.1"),
          "momentary and its maximum");
  require(text_of(*panel, QStringLiteral("loudnessRange")) == QStringLiteral("5.3"), "the range");
  require(text_of(*panel, QStringLiteral("loudnessTruePeak")) == QStringLiteral("−1.6") &&
              text_of(*panel, QStringLiteral("loudnessTruePeakSub")) == QStringLiteral("3 S −2.0"),
          "true peak, overall and recent");
  require(text_of(*panel, QStringLiteral("loudnessPsr")) == QStringLiteral("12.2") &&
              text_of(*panel, QStringLiteral("loudnessPlr")) == QStringLiteral("13.0"),
          "PSR and PLR");
  require(text_of(*panel, QStringLiteral("loudnessDuration")) == QStringLiteral("4:12"),
          "the measured time: " +
              text_of(*panel, QStringLiteral("loudnessDuration")).toStdString());
  auto* meter = item(*panel, QStringLiteral("loudnessMeter"));
  // EBU +9 around -14: -14.2 LUFS is -0.2 LU, (−0.2 + 18) / 27 up the scale.
  require(std::fabs(meter->property("shortTermFraction").toDouble() - 17.8 / 27.0) < 1e-6,
          "the meter places short-term on the scale");
  require(std::fabs(meter->property("leftPeakFraction").toDouble() - (30.0 - 1.6) / 33.0) < 1e-6,
          "and true peak on its own");
  adapter.set_telemetry({});
  require(QTest::qWaitFor(
              [&] {
                return text_of(*panel, QStringLiteral("loudnessIntegrated")) == QStringLiteral("—");
              },
              1000),
          "no telemetry: dashes again");
}

void target_distance_and_ceiling_light_up() {
  InMemoryNativeSynthAdapter adapter(preset("Streaming (Spotify, YouTube)"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* lamp = item(*panel, QStringLiteral("loudnessLamp"));
  struct Case {
    float integrated;
    const char* text;
    const char* tone;
  };
  for (const Case c :
       {Case{-14.6F, "−0.6 LU TO TARGET", "onTarget"}, Case{-12.5F, "+1.5 LU TO TARGET", "near"},
        Case{-17.9F, "−3.9 LU TO TARGET", "off"}, Case{-14.0F, "0.0 LU TO TARGET", "onTarget"}}) {
    Reading r = typical();
    r.integrated = c.integrated;
    adapter.set_telemetry(frame_of(r));
    require(QTest::qWaitFor(
                [&] {
                  return text_of(*panel, QStringLiteral("loudnessTargetDelta")) ==
                         QString::fromUtf8(c.text);
                },
                1000),
            std::string("distance to target reads ") + c.text + ", got " +
                text_of(*panel, QStringLiteral("loudnessTargetDelta")).toStdString());
    require(lamp->property("color").value<QColor>() == colour_of(*panel, c.tone),
            std::string("the lamp is ") + c.tone);
  }
  // True peak over the ceiling (-1 dBTP) raises the alarm.
  auto* peak = item(*panel, QStringLiteral("loudnessTruePeak"));
  Reading hot = typical();
  hot.true_peak = 0.8F;
  hot.true_peak_left = 0.8F;
  adapter.set_telemetry(frame_of(hot));
  require(QTest::qWaitFor([&] { return panel->root->property("peakOver").toBool(); }, 1000),
          "a true peak over the ceiling is flagged");
  require(text_of(*panel, QStringLiteral("loudnessTruePeak")) == QStringLiteral("+0.8"),
          "an over reading carries its sign");
  require(peak->property("color").value<QColor>() == colour_of(*panel, "off"), "and shows red");
  require(find_item(item(*panel, QStringLiteral("loudnessMeter")),
                    QStringLiteral("loudnessMeterPeakLeft"))
              ->property("over")
              .toBool(),
          "the left peak bar is over");
  // Raising the ceiling clears it.
  hot.true_peak = -0.4F;
  hot.true_peak_left = -0.4F;
  adapter.set_telemetry(frame_of(hot));
  require(QTest::qWaitFor(
              [&] {
                return text_of(*panel, QStringLiteral("loudnessTruePeak")) ==
                       QStringLiteral("−0.4");
              },
              1000),
          "-0.4 dBTP is still over a -1 ceiling");
  require(panel->root->property("peakOver").toBool(), "and flagged");
  controller.set_value(QStringLiteral("loudness.ceiling"), 0.0);
  QCoreApplication::processEvents();
  require(!panel->root->property("peakOver").toBool(), "under the ceiling, no alarm");
}

void scale_switches_between_ebu_views() {
  InMemoryNativeSynthAdapter adapter(preset("Streaming (Spotify, YouTube)"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(frame_of(typical()));
  auto* meter = item(*panel, QStringLiteral("loudnessMeter"));
  require(
      QTest::qWaitFor([&] { return meter->property("shortTermFraction").toDouble() > 0.0; }, 1000),
      "live");
  require(meter->property("scaleTop").toDouble() == 9.0, "EBU +9 by default");
  auto* scale = item(*panel, QStringLiteral("loudnessSegment:loudness.scale"));
  click(*panel, scene(scale, scale->width() * 0.75, scale->height() / 2));
  require(controller.choice(QStringLiteral("loudness.scale")) == 1, "a tile picks EBU +18");
  require(meter->property("scaleTop").toDouble() == 18.0 &&
              std::fabs(meter->property("shortTermFraction").toDouble() - 35.8 / 54.0) < 1e-6,
          "the meter spans -36 to +18 LU");
  require(text_of(*panel, QStringLiteral("loudnessScaleNote")).contains(QStringLiteral("+18")),
          "and says so");
  require(item(*panel, QStringLiteral("loudnessHistory"))->property("scaleTop").toDouble() == 18.0,
          "the history follows the scale");
}

void history_is_kept_beyond_the_meter() {
  InMemoryNativeSynthAdapter adapter(preset("Streaming (Spotify, YouTube)"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto length = [&] { return panel->root->property("historyLength").toInt(); };
  // The first frame takes the meter's history, less its silent start.
  Reading r = typical();
  r.duration = 3.0F;
  adapter.set_telemetry(frame_of(r, 30));
  require(QTest::qWaitFor([&] { return length() == 30; }, 1000),
          "the first frame brings 30 values: " + std::to_string(length()));
  // Then exactly what the meter's duration says is new.
  for (int step = 1; step <= 40; ++step) {
    r.duration = 3.0F + 1.0F * static_cast<float>(step);
    adapter.set_telemetry(frame_of(r, 300));
    const int expected = 30 + 10 * step;
    require(QTest::qWaitFor([&] { return length() == expected; }, 1000),
            "step " + std::to_string(step) + ": " + std::to_string(length()) +
                " values, expected " + std::to_string(expected));
  }
  require(length() == 430 && length() > static_cast<int>(loudness::history),
          "43 seconds kept, past the meter's 30");
  const auto values = panel->root->property("shortTermHistory").toList();
  require(std::fabs(values.back().toDouble() - (-15.0 + 299e-3)) < 1e-4,
          "the newest value is the meter's newest");
  // Integrate switched: the meter's duration restarts and so does the panel.
  r.duration = 0.4F;
  adapter.set_telemetry(frame_of(r, 4));
  require(QTest::qWaitFor([&] { return length() == 4; }, 1000),
          "a restarted measurement restarts the history: " + std::to_string(length()));
  // Paused (duration still): time carries it at 10 a second.
  QTest::qWait(1'050);
  require(length() >= 12 && length() <= 16,
          "paused, time adds about ten a second: " + std::to_string(length()));
}

void presets_controls_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Streaming (Spotify, YouTube)"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("loudnessPresetNext"))));
  require(controller.preset_name() != QStringLiteral("Streaming (Spotify, YouTube)"),
          "next steps the target");
  click(*panel, centre(item(*panel, QStringLiteral("loudnessPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Streaming (Spotify, YouTube)"),
          "previous steps back");
  auto* menu = item(*panel, QStringLiteral("loudnessMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("loudnessPresetDisplay"))));
  require(menu->isVisible(), "the target menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Club Master"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("loudnessPresetName")) == QStringLiteral("Club Master"),
          "a target shows by name");
  require(std::fabs(panel->root->property("target").toDouble() + 8.0) < 1e-9 &&
              std::fabs(panel->root->property("ceiling").toDouble() + 0.3) < 1e-9,
          "and sets its target and ceiling");

  auto* toggle = item(*panel, QStringLiteral("loudnessToggle:loudness.integrate"));
  require(toggle->property("on").toBool(), "integrating by default");
  click(*panel, centre(toggle));
  require(controller.choice(QStringLiteral("loudness.integrate")) == 0,
          "a click pauses and clears");
  require(text_of(*panel, QStringLiteral("loudnessReadout")) ==
              QStringLiteral("MEASURE · INTEGRATE"),
          "the display reads back the switch: " +
              text_of(*panel, QStringLiteral("loudnessReadout")).toStdString());
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("loudness.integrate")) == 1, "Space starts afresh");

  auto* target = item(*panel, QStringLiteral("loudnessKnob:loudness.target"));
  target->forceActiveFocus();
  const double before = controller.value(QStringLiteral("loudness.target"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("loudness.target")) > before, "Up raises the target");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("loudnessStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

struct Note {
  double beat;
  double length;
  std::uint8_t key;
  std::uint8_t velocity = 105;
};

constexpr double bpm = 122.0;
constexpr double rate = 48'000.0;

std::vector<engine::StereoFrame> render(const synth::SynthEngine& engine,
                                        const std::vector<double>& values,
                                        const std::vector<Note>& notes, std::size_t frames) {
  auto created = synth::NativeSynthRuntime::create(engine, values, rate);
  require(created.has_value(), "a synth runtime");
  auto runtime = std::move(created.value());
  struct Event {
    std::uint64_t frame;
    engine::MidiEventKind kind;
    std::uint8_t key;
    std::uint8_t velocity;
    std::uint64_t id;
  };
  const double frames_per_beat = 60.0 / bpm * rate;
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto& n : notes) {
    const auto start = static_cast<std::uint64_t>(std::llround(n.beat * frames_per_beat));
    const auto stop =
        static_cast<std::uint64_t>(std::llround((n.beat + n.length) * frames_per_beat));
    events.push_back({start, engine::MidiEventKind::note_on, n.key, n.velocity, id});
    events.push_back({stop, engine::MidiEventKind::note_off, n.key, 0, id});
    ++id;
  }
  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
    return a.frame != b.frame ? a.frame < b.frame : a.kind < b.kind;
  });
  std::vector<engine::StereoFrame> out(frames, {0.0F, 0.0F});
  std::vector<engine::StereoFrame> block;
  std::vector<engine::BlockMidiEvent> block_events;
  std::size_t next = 0;
  for (std::size_t position = 0; position < frames; position += 128) {
    const std::size_t size = std::min<std::size_t>(128, frames - position);
    block.assign(size, {0.0F, 0.0F});
    block_events.clear();
    while (next < events.size() && events[next].frame < position + size) {
      const auto& e = events[next++];
      engine::BlockMidiEvent event;
      event.offset = static_cast<std::uint32_t>(e.frame - position);
      event.clip_id_bits = {1, 1};
      event.note_id_bits = {0, e.id};
      event.kind = e.kind;
      event.key = e.key;
      event.velocity = e.velocity;
      block_events.push_back(event);
    }
    const double beats_per_frame = bpm / 60.0 / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const synth::SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, block_events, position, 1, transport, {}).status ==
                synth::SynthProcessStatus::rendered,
            "synth renders");
    std::copy(block.begin(), block.end(), out.begin() + static_cast<std::ptrdiff_t>(position));
  }
  return out;
}

// A deep-house loop in A minor (Am F C G), 122 BPM, with a breakdown: bars
// 1-24 full (to 47 s), 25-30 strings and pluck alone, 31 on full again.
std::vector<engine::StereoFrame> music(std::size_t frames) {
  const double beats = static_cast<double>(frames) / rate * bpm / 60.0;
  const auto bars = static_cast<int>(beats / 4.0) + 1;
  const std::array<std::array<std::uint8_t, 4>, 4> chords{
      {{57, 60, 64, 69}, {53, 57, 60, 65}, {55, 60, 64, 67}, {55, 59, 62, 67}}};
  const std::array<std::uint8_t, 4> roots{33, 29, 36, 31};
  std::vector<Note> drums;
  std::vector<Note> bass;
  std::vector<Note> pads;
  std::vector<Note> pluck;
  for (int bar = 0; bar < bars; ++bar) {
    const bool breakdown = bar >= 24 && bar < 30;
    const auto chord = static_cast<std::size_t>(bar % 4);
    const double b = 4.0 * bar;
    for (const auto key : chords[chord]) {
      pads.push_back({b, 3.95, key, 78});
    }
    for (int step = 0; step < 16; ++step) {
      const auto key =
          static_cast<std::uint8_t>(chords[chord][static_cast<std::size_t>(step % 4)] + 12);
      pluck.push_back(
          {b + step * 0.25, 0.2, key, static_cast<std::uint8_t>(step % 2 == 0 ? 90 : 66)});
    }
    if (breakdown) {
      continue;
    }
    for (int beat = 0; beat < 4; ++beat) {
      drums.push_back({b + beat, 0.25, 36, 112});
      drums.push_back({b + beat + 0.5, 0.2, 46, 88});
      drums.push_back({b + beat + 0.25, 0.1, 42, 58});
      drums.push_back({b + beat + 0.75, 0.1, 42, 64});
      if (beat % 2 == 1) {
        drums.push_back({b + beat, 0.2, 40, 96});
      }
      bass.push_back({b + beat + 0.25, 0.2, roots[chord], 100});
      bass.push_back({b + beat + 0.5, 0.2, static_cast<std::uint8_t>(roots[chord] + 12), 90});
      bass.push_back({b + beat + 0.75, 0.2, roots[chord], 96});
    }
  }
  struct Part {
    const synth::SynthEngine* engine;
    std::string_view patch;
    const std::vector<Note>* notes;
    float gain;
  };
  const std::array<Part, 4> parts{{{&synth::pulse::engine(), "909 House", &drums, 0.9F},
                                   {&synth::torque::engine(), "Moog Bass", &bass, 0.55F},
                                   {&synth::ember::engine(), "Juno Strings", &pads, 0.32F},
                                   {&synth::prism::engine(), "Pluck", &pluck, 0.22F}}};
  std::vector<engine::StereoFrame> mix(frames, {0.0F, 0.0F});
  for (const auto& part : parts) {
    const auto audio =
        render(*part.engine, preset_of(*part.engine->descriptor, part.patch), *part.notes, frames);
    for (std::size_t i = 0; i < frames; ++i) {
      mix[i].left += part.gain * audio[i].left;
      mix[i].right += part.gain * audio[i].right;
    }
  }
  return mix;
}

// The real Loudness meter over `audio` at `gain`, a telemetry frame every
// second (the panel counts what is new from the meter's duration).
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& audio,
                                               float gain, std::size_t stop) {
  auto created = effects::NativeEffectRuntime::create(loudness::engine(), values, rate);
  require(created.has_value(), "a Loudness runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  for (std::size_t position = 0; position < stop; position += 480) {
    const std::size_t size = std::min<std::size_t>(480, stop - position);
    block.assign(audio.begin() + static_cast<std::ptrdiff_t>(position),
                 audio.begin() + static_cast<std::ptrdiff_t>(position + size));
    for (auto& f : block) {
      f.left *= gain;
      f.right *= gain;
    }
    const synth::SynthTransport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Loudness runs");
    if ((position + size) % 48'000 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Loudness publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("LOUDNESS_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const std::size_t stop = static_cast<std::size_t>(66.0 * rate);
  const auto audio = music(stop);
  // Measure the loop once to set each shot's level.
  const auto plain = real_telemetry(preset("EBU R128 Broadcast"), audio, 1.0F, stop);
  const double measured = plain.back()[loudness::telemetry::integrated];
  struct Shot {
    const char* preset;
    double lufs;    // Where to place the integrated loudness.
    double seconds; // How long to play.
    const char* touched;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"Streaming (Spotify, YouTube)", -14.3, 36.0, "loudness.target",
             "/loudness_streaming.png"},
        Shot{"Club Master", -7.6, 36.0, "loudness.ceiling", "/loudness_club_master.png"},
        Shot{"EBU R128 Broadcast", -23.2, 36.0, "loudness.scale", "/loudness_ebu_r128.png"},
        Shot{"Streaming (Spotify, YouTube)", -14.3, 66.0, "loudness.integrate",
             "/loudness_history.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    const auto gain = static_cast<float>(std::pow(10.0, (shot.lufs - measured) / 20.0));
    const auto frames =
        real_telemetry(values, audio, gain, static_cast<std::size_t>(shot.seconds * rate));
    // One frame a second of meter time; the panel takes what each adds.
    for (const auto& frame : frames) {
      adapter.set_telemetry(frame);
      QTest::qWait(120);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << " (integrated "
              << frames.back()[loudness::telemetry::integrated] << " LUFS, true peak "
              << frames.back()[loudness::telemetry::true_peak] << " dBTP)\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    readings_decode_and_silence_shows_as_dashes();
    target_distance_and_ceiling_light_up();
    scale_switches_between_ebu_views();
    history_is_kept_beyond_the_meter();
    presets_controls_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "loudness panel contract passed\n";
  return 0;
}
