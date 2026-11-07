#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/tune.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJSValue>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Tune panel (ADR-0042): every parameter on a table-driven control;
// the telemetry decode into the note, the cents, the frequency, clarity and
// level, a held note through a gap, and the history; the note maths (the
// nearest note and its cents, in sharps or flats, across octaves); Kick
// mode's trajectory, settled pitch, hits and hint; presets and status; and
// screenshots on request, live from the real Tune over a Prism bass and a
// Pulse 808 kick.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace tune = oscillate::native_dsp::effects::tune;
namespace engine = oscillate::engine;

using oscillate::test::require;
// How long a UI wait may take before it counts as a hang (not a timing claim).
constexpr int ui_wait_ms = 5'000;

const synth::EngineDescriptor& table() { return *tune::engine().descriptor; }
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

std::size_t parameter_index(std::string_view id) {
  for (std::size_t i = 0; i < table().parameters.size(); ++i) {
    if (table().parameters[i].id == id) {
      return i;
    }
  }
  throw std::runtime_error("missing parameter " + std::string(id));
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
      import "qrc:/tune"
      TunePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/TunePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Tune panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Tune panel was not created: " + component.errorString().toStdString());
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

QVariant js(QObject* object, const char* name) {
  QVariant value = object->property(name);
  if (value.canConvert<QJSValue>()) {
    return value.value<QJSValue>().toVariant();
  }
  return value;
}

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, 1000);
}

// The panel's own note maths: noteLabel(note, flats).
QString label_of(Panel& panel, double note, bool flats) {
  QVariant result;
  require(QMetaObject::invokeMethod(panel.root, "noteLabel", Q_RETURN_ARG(QVariant, result),
                                    Q_ARG(QVariant, note), Q_ARG(QVariant, flats)),
          "noteLabel is callable");
  return result.toString();
}

// ---- Frames -----------------------------------------------------------------------

struct Reading {
  float hz = 0.0F;
  float note = 0.0F;
  float cents = 0.0F;
  float clarity = 0.0F;
  float level_db = -200.0F;
  float settled_hz = 0.0F;
  float settled_note = 0.0F;
  float settled_cents = 0.0F;
  float hits = 0.0F;
};

double hz_of(double note, double reference = 440.0) {
  return reference * std::pow(2.0, (note - 69.0) / 12.0);
}

Reading pitch(double note) {
  Reading r;
  r.hz = static_cast<float>(hz_of(note));
  r.note = static_cast<float>(note);
  r.cents = static_cast<float>(100.0 * (note - std::round(note)));
  r.clarity = 0.97F;
  r.level_db = -12.0F;
  return r;
}

// A frame with these readings, a kick glide from 160 Hz down to the
// settled pitch over the first `glide` points, and a history whose last
// `filled` notes are `history_note`.
std::vector<float> frame_of(const Reading& r, std::size_t glide = 0, std::size_t filled = 0,
                            float history_note = 0.0F) {
  namespace t = tune::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::hz] = r.hz;
  frame[t::note] = r.note;
  frame[t::cents] = r.cents;
  frame[t::clarity] = r.clarity;
  frame[t::level_db] = r.level_db;
  frame[t::settled_hz] = r.settled_hz;
  frame[t::settled_note] = r.settled_note;
  frame[t::settled_cents] = r.settled_cents;
  frame[t::hits] = r.hits;
  for (std::size_t k = 0; k < glide; ++k) {
    const double f = static_cast<double>(k) / static_cast<double>(glide);
    frame[t::trajectory + k] =
        static_cast<float>(r.settled_hz + (160.0 - r.settled_hz) * std::exp(-6.0 * f));
  }
  for (std::size_t k = tune::history - filled; k < tune::history; ++k) {
    frame[t::notes + k] = history_note;
  }
  return frame;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Instrument"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 720,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"tuneKnob:", "tuneSegment:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("tunePresetName")) == QStringLiteral("Instrument"),
          "the panel names its preset");
}

void the_note_maths() {
  InMemoryNativeSynthAdapter adapter(preset("Instrument"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  struct Case {
    double note;
    bool flats;
    const char* label;
  };
  for (const Case c :
       {Case{69.25, false, "A4 +25c"}, Case{69.25, true, "A4 +25c"}, Case{69.6, true, "B♭4 −40c"},
        Case{69.6, false, "A♯4 −40c"}, Case{70.6, true, "B4 −40c"}, Case{70.4, true, "B♭4 +40c"},
        Case{60.0, false, "C4 0c"}, Case{59.51, false, "C4 −49c"}, Case{59.49, false, "B3 +49c"},
        Case{29.0 - 0.12, true, "F1 −12c"}, Case{22.0, true, "B♭0 0c"},
        Case{11.9, false, "C0 −10c"}, Case{106.0, false, "A♯7 0c"}}) {
    const QString label = label_of(*panel, c.note, c.flats);
    require(label == QString::fromUtf8(c.label), "note " + std::to_string(c.note) + " in " +
                                                     (c.flats ? "flats" : "sharps") + " reads " +
                                                     c.label + ", got " + label.toStdString());
  }
}

void readings_decode_and_hold() {
  InMemoryNativeSynthAdapter adapter(preset("Instrument"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  require(text_of(*panel, QStringLiteral("tuneCents")) == QStringLiteral("—") &&
              text_of(*panel, QStringLiteral("tuneHz")) == QStringLiteral("— Hz") &&
              text_of(*panel, QStringLiteral("tuneClarity")) == QStringLiteral("—"),
          "nothing yet: dashes");
  require(text_of(*panel, QStringLiteral("tuneHint")) ==
              QStringLiteral("Play a single note into the tuner."),
          "and an invitation");

  adapter.set_telemetry(frame_of(pitch(69.25), 0, 120, 69.25F));
  require(wait_for_text(*panel, QStringLiteral("tuneCents"), QStringLiteral("+25.0")),
          "the cents decode: " + text_of(*panel, QStringLiteral("tuneCents")).toStdString());
  require(text_of(*panel, QStringLiteral("tuneNoteName")) == QStringLiteral("A") &&
              text_of(*panel, QStringLiteral("tuneNoteAccidental")).isEmpty() &&
              text_of(*panel, QStringLiteral("tuneNoteOctave")) == QStringLiteral("4"),
          "A4");
  require(text_of(*panel, QStringLiteral("tuneHz")) == QStringLiteral("446.40 Hz"),
          "the frequency: " + text_of(*panel, QStringLiteral("tuneHz")).toStdString());
  require(text_of(*panel, QStringLiteral("tuneClarity")) == QStringLiteral("0.97") &&
              text_of(*panel, QStringLiteral("tuneLevel")) == QStringLiteral("−12.0 dBFS"),
          "clarity and level");
  require(text_of(*panel, QStringLiteral("tuneHint")) ==
              QStringLiteral("A4 is 25 cents sharp: tune down."),
          "the hint: " + text_of(*panel, QStringLiteral("tuneHint")).toStdString());
  auto* strobe = item(*panel, QStringLiteral("tuneStrobe"));
  require(strobe->property("live").toBool() &&
              std::fabs(strobe->property("cents").toDouble() - 25.0) < 1e-4,
          "the strobe drifts with the error");
  const double before = strobe->property("offset").toDouble();
  require(
      QTest::qWaitFor([&] { return strobe->property("offset").toDouble() > before; }, ui_wait_ms),
      "sharp drifts right");
  auto* needle = item(*panel, QStringLiteral("tuneCentsNeedle"));
  require(std::fabs(needle->property("position").toDouble() - 0.5) < 1e-4,
          "the needle half way to +50");
  auto* history = item(*panel, QStringLiteral("tuneHistory"));
  require(js(history, "notes").toList().size() == 200 && history->property("centre").toInt() == 69,
          "the history centres on the notes played");

  // A gap: the note is held, dimmed.
  Reading gap = pitch(69.25);
  gap.hz = 0.0F;
  gap.note = 0.0F;
  gap.cents = 0.0F;
  gap.clarity = 0.2F;
  adapter.set_telemetry(frame_of(gap));
  require(wait_for_text(*panel, QStringLiteral("tuneHint"),
                        QStringLiteral("Held: A4 +25c. Play the next note.")),
          "held: " + text_of(*panel, QStringLiteral("tuneHint")).toStdString());
  require(text_of(*panel, QStringLiteral("tuneNoteName")) == QStringLiteral("A") &&
              panel->root->property("held").toBool(),
          "the last note stays up");
  require(!strobe->property("live").toBool(), "the strobe stops");

  // In tune, flat, and in flats.
  adapter.set_telemetry(frame_of(pitch(64.01)));
  require(wait_for_text(*panel, QStringLiteral("tuneHint"), QStringLiteral("E4 is in tune.")),
          "in tune: " + text_of(*panel, QStringLiteral("tuneHint")).toStdString());
  controller.set_choice(QStringLiteral("tune.notation"), 1);
  adapter.set_telemetry(frame_of(pitch(69.6)));
  require(wait_for_text(*panel, QStringLiteral("tuneHint"),
                        QStringLiteral("B♭4 is 40 cents flat: tune up.")),
          "flat, in flats: " + text_of(*panel, QStringLiteral("tuneHint")).toStdString());
  require(text_of(*panel, QStringLiteral("tuneNoteName")) == QStringLiteral("B") &&
              text_of(*panel, QStringLiteral("tuneNoteAccidental")) == QStringLiteral("♭") &&
              text_of(*panel, QStringLiteral("tuneCents")) == QStringLiteral("−40.0"),
          "B♭ with its accidental");
  controller.set_choice(QStringLiteral("tune.notation"), 0);
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("tuneNoteName")) == QStringLiteral("A") &&
              text_of(*panel, QStringLiteral("tuneNoteAccidental")) == QStringLiteral("♯"),
          "sharps names it A♯");

  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("tuneCents"), QStringLiteral("—")),
          "no telemetry: dashes again, nothing held");
  require(!panel->root->property("held").toBool(), "and nothing held");
}

void kick_mode_reads_the_settled_pitch() {
  InMemoryNativeSynthAdapter adapter(preset("Kick"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("tuneKickView"))->isVisible() &&
              !item(*panel, QStringLiteral("tuneInstrumentView"))->isVisible(),
          "Kick mode shows the kick view");
  require(
      text_of(*panel, QStringLiteral("tuneHint")) ==
          QStringLiteral("Play the kick: Tune follows its pitch glide and reads where it settles."),
      "waiting for a kick");
  Reading r;
  r.settled_note = static_cast<float>(29.0 - 0.12);
  r.settled_hz = static_cast<float>(hz_of(r.settled_note));
  r.settled_cents = -12.0F;
  r.hits = 7.0F;
  r.clarity = 0.0F;
  r.level_db = -9.0F;
  adapter.set_telemetry(frame_of(r, 60));
  require(wait_for_text(*panel, QStringLiteral("tuneHint"),
                        QStringLiteral("Kick settles at F1 −12c: tune it +0.12 st to land on F1.")),
          "the kick hint: " + text_of(*panel, QStringLiteral("tuneHint")).toStdString());
  require(text_of(*panel, QStringLiteral("tuneSettledName")) == QStringLiteral("F") &&
              text_of(*panel, QStringLiteral("tuneSettledOctave")) == QStringLiteral("1") &&
              text_of(*panel, QStringLiteral("tuneSettledCents")) == QStringLiteral("−12.0") &&
              text_of(*panel, QStringLiteral("tuneHits")) == QStringLiteral("7"),
          "F1, -12 cents, seven hits");
  require(text_of(*panel, QStringLiteral("tuneSettledHz")) ==
              QString::number(r.settled_hz, 'f', 2) + QStringLiteral(" Hz"),
          "the settled frequency");
  auto* trajectory = item(*panel, QStringLiteral("tuneTrajectory"));
  const auto points = js(trajectory, "points").toList();
  require(points.size() == 80 && std::fabs(points[0].toDouble() - 160.0) < 1e-3 &&
              points[60].toDouble() == 0.0,
          "the trajectory: 80 points, 0 past the end");
  require(trajectory->property("hasHit").toBool() &&
              item(*panel, QStringLiteral("tuneSettledLine"))->isVisible(),
          "the settled pitch is marked");
  r.settled_note = 29.02F;
  r.settled_cents = 2.0F;
  r.settled_hz = static_cast<float>(hz_of(r.settled_note));
  adapter.set_telemetry(frame_of(r, 60));
  require(wait_for_text(*panel, QStringLiteral("tuneHint"),
                        QStringLiteral("Kick settles at F1 +2c: in tune on F1.")),
          "an in-tune kick: " + text_of(*panel, QStringLiteral("tuneHint")).toStdString());
  r.settled_note = 34.3F;
  r.settled_cents = 30.0F;
  controller.set_choice(QStringLiteral("tune.notation"), 1);
  adapter.set_telemetry(frame_of(r, 60));
  require(
      wait_for_text(*panel, QStringLiteral("tuneHint"),
                    QStringLiteral("Kick settles at B♭1 +30c: tune it −0.30 st to land on B♭1.")),
      "a sharp kick, in flats: " + text_of(*panel, QStringLiteral("tuneHint")).toStdString());
}

void presets_controls_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Instrument"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("tunePresetNext"))));
  require(controller.preset_name() != QStringLiteral("Instrument"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("tunePresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Instrument"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("tuneMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("tunePresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Baroque (A = 415 Hz)"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("tunePresetName")) ==
              QStringLiteral("Baroque (A = 415 Hz)"),
          "a preset shows by name");
  require(text_of(*panel, QStringLiteral("tuneReference")) == QStringLiteral("A4 = 415.0"),
          "and sets the reference: " +
              text_of(*panel, QStringLiteral("tuneReference")).toStdString());

  auto* mode = item(*panel, QStringLiteral("tuneSegment:tune.mode"));
  click(*panel, scene(mode, mode->width() * 2.5 / 3.0, mode->height() / 2));
  require(controller.choice(QStringLiteral("tune.mode")) == 2, "a tile picks Kick");
  require(item(*panel, QStringLiteral("tuneKickView"))->isVisible(), "and the kick view shows");
  require(text_of(*panel, QStringLiteral("tuneReadout")) == QStringLiteral("TUNER · MODE"),
          "the display reads back the tile: " +
              text_of(*panel, QStringLiteral("tuneReadout")).toStdString());

  auto* notation = item(*panel, QStringLiteral("tuneSegment:tune.notation"));
  click(*panel, scene(notation, notation->width() * 0.75, notation->height() / 2));
  require(controller.choice(QStringLiteral("tune.notation")) == 1 &&
              panel->root->property("flats").toBool(),
          "a tile picks flats");

  auto* reference = item(*panel, QStringLiteral("tuneKnob:tune.reference"));
  reference->forceActiveFocus();
  const double before = controller.value(QStringLiteral("tune.reference"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("tune.reference")) > before, "Up raises A4");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("tuneStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;
constexpr std::uint64_t beat_frames = 24'000; // 120 BPM.

struct Note {
  std::uint64_t frame;
  std::uint64_t length;
  std::uint8_t key;
};

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
    std::uint64_t id;
  };
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto& n : notes) {
    events.push_back({n.frame, engine::MidiEventKind::note_on, n.key, id});
    events.push_back({n.frame + n.length, engine::MidiEventKind::note_off, n.key, id});
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
      event.velocity = 110;
      block_events.push_back(event);
    }
    const double beats_per_frame = 2.0 / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const synth::SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, block_events, position, 1, transport, {}).status ==
                synth::SynthProcessStatus::rendered,
            "synth renders");
    std::copy(block.begin(), block.end(), out.begin() + static_cast<std::ptrdiff_t>(position));
  }
  return out;
}

// The real Tune over `audio`, a telemetry frame every 1600 frames (a 30 Hz
// poll).
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& audio) {
  auto created = effects::NativeEffectRuntime::create(tune::engine(), values, rate);
  require(created.has_value(), "a Tune runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  for (std::size_t position = 0; position < audio.size(); position += 160) {
    const std::size_t size = std::min<std::size_t>(160, audio.size() - position);
    block.assign(audio.begin() + static_cast<std::ptrdiff_t>(position),
                 audio.begin() + static_cast<std::ptrdiff_t>(position + size));
    const synth::SynthTransport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Tune renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Tune publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("TUNE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    bool flats; // Switched from the preset's sharps on the panel.
    const char* touched;
    const char* file;
  };
  const std::size_t frames = 4 * 2 * beat_frames + 30'000;
  // A Prism Sub Bass walking A1, C2, G1 and settling on E1 (or B♭1): its
  // sub oscillator sounds an octave under the keys played.
  const auto bass = [&](std::uint8_t last) {
    std::vector<Note> notes{{0, 2 * beat_frames - 2'000, 45},
                            {2 * beat_frames, 2 * beat_frames - 2'000, 48},
                            {4 * beat_frames, 2 * beat_frames - 2'000, 43},
                            {6 * beat_frames, frames, last}};
    return render(synth::prism::engine(), preset_of(*synth::prism::engine().descriptor, "Sub Bass"),
                  notes, frames);
  };
  // A Pulse 808 kick every other beat.
  const auto kick = [&] {
    std::vector<Note> notes;
    for (std::uint64_t at = 6'000; at + 6'000 < frames; at += 2 * beat_frames) {
      notes.push_back({at, 2'400, 36});
    }
    return render(synth::pulse::engine(), preset_of(*synth::pulse::engine().descriptor, "808 Boom"),
                  notes, frames - 10'000);
  };
  for (const Shot shot : {Shot{"Bass", false, "tune.mode", "/tune_bass.png"},
                          Shot{"A = 432 Hz", true, "tune.notation", "/tune_432_flats.png"},
                          Shot{"Kick", false, "tune.mode", "/tune_kick_808.png"}}) {
    auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    if (shot.flats) {
      controller.set_choice(QStringLiteral("tune.notation"), 1);
      values[parameter_index("tune.notation")] = 1.0;
    }
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    const bool kick_mode = values[parameter_index("tune.mode")] == 2.0;
    const auto audio = kick_mode ? kick() : bass(shot.flats ? 46 : 40);
    const auto telemetry = real_telemetry(values, audio);
    // The last four seconds of polls, in real time.
    for (std::size_t i = telemetry.size() > 120 ? telemetry.size() - 120 : 0; i < telemetry.size();
         ++i) {
      adapter.set_telemetry(telemetry[i]);
      QTest::qWait(33);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    const auto& last = telemetry.back();
    std::cout << "wrote " << path.toStdString() << " (" << last[tune::telemetry::hz] << " Hz, note "
              << last[tune::telemetry::note] << ", settled " << last[tune::telemetry::settled_hz]
              << " Hz, " << last[tune::telemetry::settled_cents] << " c, hits "
              << last[tune::telemetry::hits] << ")\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    the_note_maths();
    readings_decode_and_hold();
    kick_mode_reads_the_settled_pitch();
    presets_controls_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "tune panel contract passed\n";
  return 0;
}
