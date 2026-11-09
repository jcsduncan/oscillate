#include "device_strip_view.hpp"
#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/scope.hpp"
#include "oscillate/native_dsp/parameters.hpp"
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

// The Scope panel (ADR-0042): every parameter on a table-driven control;
// the telemetry decode into the sweep (track and sidechain columns, the
// write column) and the alignment meter (delay in ms and samples, polarity,
// correlation); the hint's wording for each sign of the delay, an inverted
// polarity and an uncertain or missing reading; the window's grid, synced
// and free; presets and status; and screenshots on request, live from the
// real Scope over a Pulse kick and a Prism Sub Bass 2 ms late.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace scope = oscillate::native_dsp::effects::scope;
namespace engine = oscillate::engine;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *scope::engine().descriptor; }
int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

std::vector<double> preset_of(const oscillate::native_dsp::EngineDescriptor& descriptor,
                              std::string_view name) {
  for (const auto& entry : descriptor.presets) {
    if (entry.name == name) {
      return oscillate::native_dsp::preset_values(descriptor, entry).value();
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
      import "qrc:/scope"
      ScopePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/ScopePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Scope panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Scope panel was not created: " + component.errorString().toStdString());
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

// A property holding a JavaScript value, as a variant.
QVariant js(QObject* object, const char* name) {
  QVariant value = object->property(name);
  if (value.canConvert<QJSValue>()) {
    return value.value<QJSValue>().toVariant();
  }
  return value;
}

QColor colour_of(Panel& panel, const char* name) {
  return panel.root->property("theme").value<QObject*>()->property(name).value<QColor>();
}

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, 1000);
}

// ---- Frames -----------------------------------------------------------------------

struct Alignment {
  float delay_ms = 0.0F;
  float polarity = 1.0F;
  float correlation = 0.9F;
  float window_ms = 500.0F;
  float write_column = 123.0F;
  bool key = true;
};

// A frame whose track column c spans +-(c / 480) and whose sidechain spans
// half that, so columns can be recognised.
std::vector<float> frame_of(const Alignment& a) {
  namespace t = scope::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  for (std::size_t c = 0; c < scope::columns; ++c) {
    const float v = static_cast<float>(c) / static_cast<float>(scope::columns);
    frame[t::track + 2 * c] = -v;
    frame[t::track + 2 * c + 1] = v;
    frame[t::sidechain + 2 * c] = a.key ? -0.5F * v : 0.0F;
    frame[t::sidechain + 2 * c + 1] = a.key ? 0.5F * v : 0.0F;
  }
  frame[t::write_column] = a.write_column;
  frame[t::delay_ms] = a.delay_ms;
  frame[t::polarity] = a.polarity;
  frame[t::correlation] = a.correlation;
  frame[t::window_ms] = a.window_ms;
  return frame;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Kick and Bass"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 720,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"scopeKnob:", "scopeSegment:", "scopeToggle:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("scopePresetName")) == QStringLiteral("Kick and Bass"),
          "the panel names its preset");
}

void telemetry_decodes_into_the_sweep_and_meter() {
  InMemoryNativeSynthAdapter adapter(preset("Kick and Bass"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  for (const char* name : {"scopeDelay", "scopeCorrelation", "scopePolarityText"}) {
    require(text_of(*panel, QString::fromLatin1(name)) == QStringLiteral("—"),
            std::string(name) + " is a dash with nothing measured");
  }
  auto* screen = item(*panel, QStringLiteral("scopeScreen"));
  require(!screen->property("live").toBool(), "the screen waits for a signal");

  Alignment a;
  a.delay_ms = 1.6F;
  adapter.set_telemetry(frame_of(a));
  require(wait_for_text(*panel, QStringLiteral("scopeDelay"), QStringLiteral("+1.60")),
          "the delay decodes: " + text_of(*panel, QStringLiteral("scopeDelay")).toStdString());
  require(text_of(*panel, QStringLiteral("scopeDelaySamples")) ==
              QStringLiteral("+76.8 samples @ 48.0 kHz"),
          "and in samples: " + text_of(*panel, QStringLiteral("scopeDelaySamples")).toStdString());
  require(text_of(*panel, QStringLiteral("scopeCorrelation")) == QStringLiteral("0.90"),
          "the correlation");
  require(text_of(*panel, QStringLiteral("scopePolarityText")) == QStringLiteral("IN PHASE") &&
              !item(*panel, QStringLiteral("scopeInvertHint"))->isVisible(),
          "in phase, no invert hint");
  require(screen->property("live").toBool() && screen->property("writeColumn").toInt() == 123,
          "the write column");
  const auto track = js(screen, "track").toList();
  const auto key = js(screen, "key").toList();
  require(track.size() == 960 && key.size() == 960, "480 (min, max) columns each");
  require(std::fabs(track[qsizetype{2} * 240 + 1].toDouble() - 0.5) < 1e-6 &&
              std::fabs(track[qsizetype{2} * 240].toDouble() + 0.5) < 1e-6,
          "track column 240 spans +-0.5");
  require(std::fabs(key[qsizetype{2} * 240 + 1].toDouble() - 0.25) < 1e-6,
          "sidechain column 240 to 0.25");
  auto* needle = item(*panel, QStringLiteral("scopeNeedle"));
  require(std::fabs(needle->property("delayMs").toDouble() - 1.6) < 1e-6 &&
              needle->property("position").toDouble() > 0.0,
          "the needle leans late");
  auto* beam = item(*panel, QStringLiteral("scopeBeam"));
  const double plot_x = screen->property("plotX").toDouble();
  const double plot_w = screen->property("plotW").toDouble();
  require(std::fabs(beam->x() - (plot_x + plot_w * 123.5 / 480.0)) < 1e-6,
          "the beam stands at the write column");
  auto* bar = item(*panel, QStringLiteral("scopeConfidenceBar"));
  require(std::fabs(bar->width() - (bar->parentItem()->width() - 2) * 0.9) < 1e-3,
          "the confidence bar fills to the correlation");

  // Early, and the needle leans the other way; the scale clamps at 20 ms.
  a.delay_ms = -0.42F;
  adapter.set_telemetry(frame_of(a));
  require(wait_for_text(*panel, QStringLiteral("scopeDelay"), QStringLiteral("−0.42")),
          "an early track reads negative");
  require(needle->property("position").toDouble() < 0.0, "the needle leans early");
  a.delay_ms = 25.0F;
  adapter.set_telemetry(frame_of(a));
  require(QTest::qWaitFor([&] { return needle->property("position").toDouble() == 1.0; }, 1000),
          "the needle stops at the end of the search");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("scopeDelay"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

void the_hint_says_which_way_and_how_far() {
  InMemoryNativeSynthAdapter adapter(preset("Kick and Bass"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString hint = QStringLiteral("scopeHint");
  struct Case {
    Alignment a;
    const char* text;
    const char* tone; // The delay's colour; empty when not measuring.
  };
  // The readings are named in each case below.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  const auto with = [](float delay, float polarity, float correlation, bool key = true) {
    Alignment a;
    a.delay_ms = delay;
    a.polarity = polarity;
    a.correlation = correlation;
    a.key = key;
    return a;
  };
  for (const Case& c : {
           Case{with(1.6F, 1.0F, 0.9F),
                "The track is 1.60 ms late against the sidechain: nudge it earlier.", "off"},
           Case{with(-0.8F, 1.0F, 0.9F),
                "The track is 0.80 ms early against the sidechain: nudge it later.", "near"},
           Case{with(0.04F, 1.0F, 0.9F), "The track is aligned with the sidechain.", "aligned"},
           Case{with(0.02F, -1.0F, 0.9F), "Polarity is inverted: flip the track's polarity.",
                "off"},
           Case{with(2.5F, -1.0F, 0.9F),
                "Polarity is inverted, and the track is 2.50 ms late: flip it, then nudge it "
                "earlier.",
                "off"},
           Case{with(-19.8F, 1.0F, 0.8F),
                "The track is 19.8 ms early against the sidechain: nudge it later (at the edge of "
                "the ±20 ms search).",
                "off"},
           Case{with(3.0F, 1.0F, 0.12F),
                "The track and the sidechain have little in common here: the reading is "
                "uncertain.",
                "inkDim"},
           Case{with(0.0F, 1.0F, 0.0F, false),
                "Nothing on the sidechain: route the kick (or any reference) to it to align "
                "against.",
                ""},
       }) {
    adapter.set_telemetry(frame_of(c.a));
    require(wait_for_text(*panel, hint, QString::fromUtf8(c.text)),
            std::string("the hint reads \"") + c.text + "\", got \"" +
                text_of(*panel, hint).toStdString() + "\"");
    if (*c.tone != '\0') {
      require(item(*panel, QStringLiteral("scopeNeedle"))->property("tone").value<QColor>() ==
                  colour_of(*panel, c.tone),
              std::string("the needle is ") + c.tone + " for " + c.text);
    }
  }
  // Inverted: the polarity tile says so.
  adapter.set_telemetry(frame_of(with(0.02F, -1.0F, 0.9F)));
  require(wait_for_text(*panel, QStringLiteral("scopePolarityText"), QStringLiteral("INVERTED")),
          "an inverted polarity reads INVERTED");
  require(item(*panel, QStringLiteral("scopeInvertHint"))->isVisible() &&
              text_of(*panel, QStringLiteral("scopeInvertHint")) ==
                  QStringLiteral("INVERT THE TRACK"),
          "with an invert hint");
  // A host may name the two signals.
  panel->root->setProperty("trackName", QStringLiteral("Bass"));
  panel->root->setProperty("keyName", QStringLiteral("the kick"));
  adapter.set_telemetry(frame_of(with(1.6F, 1.0F, 0.9F)));
  require(wait_for_text(*panel, hint,
                        QStringLiteral("Bass is 1.60 ms late against the kick: nudge it earlier.")),
          "named: " + text_of(*panel, hint).toStdString());
  // Align off.
  controller.set_choice(QStringLiteral("scope.align"), 0);
  QCoreApplication::processEvents();
  require(text_of(*panel, hint) ==
              QStringLiteral("Align is off: switch it on to measure the delay against the kick."),
          "align off: " + text_of(*panel, hint).toStdString());
  require(text_of(*panel, QStringLiteral("scopeDelay")) == QStringLiteral("—"),
          "and no delay shown");
}

QStringList label_texts(Panel& panel) {
  QStringList texts;
  for (const auto& label : js(panel.root, "grid").toMap()[QStringLiteral("labels")].toList()) {
    texts << label.toMap()[QStringLiteral("text")].toString();
  }
  return texts;
}

void the_grid_follows_the_window() {
  InMemoryNativeSynthAdapter adapter(preset("Kick and Bass"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  Alignment a;
  adapter.set_telemetry(frame_of(a));
  require(wait_for_text(*panel, QStringLiteral("scopeDelay"), QStringLiteral("0.00")), "live");
  require(label_texts(*panel) == QStringList({QStringLiteral("1"), QStringLiteral("e"),
                                              QStringLiteral("&"), QStringLiteral("a")}),
          "a beat counts its sixteenths: " + label_texts(*panel).join(' ').toStdString());
  controller.apply_preset(preset_index("Bar View"));
  QCoreApplication::processEvents();
  require(label_texts(*panel) == QStringList({QStringLiteral("1"), QStringLiteral("1.2"),
                                              QStringLiteral("1.3"), QStringLiteral("1.4")}),
          "a bar counts its beats: " + label_texts(*panel).join(' ').toStdString());
  int bars = 0;
  for (const auto& line : js(panel->root, "grid").toMap()[QStringLiteral("lines")].toList()) {
    bars += line.toMap()[QStringLiteral("weight")].toInt() == 2 ? 1 : 0;
  }
  require(bars == 2, "bar lines at both ends of a bar");
  controller.apply_preset(preset_index("Waveform"));
  a.window_ms = 20.0F;
  adapter.set_telemetry(frame_of(a));
  require(QTest::qWaitFor(
              [&] {
                return label_texts(*panel) ==
                       QStringList({QStringLiteral("0"), QStringLiteral("4.00"),
                                    QStringLiteral("8.00"), QStringLiteral("12.0"),
                                    QStringLiteral("16.0")});
              },
              1000),
          "free time divides the window: " + label_texts(*panel).join(' ').toStdString());
  require(!item(*panel, QStringLiteral("scopeSegment:scope.length"))->isVisible() &&
              item(*panel, QStringLiteral("scopeKnob:scope.time"))->isVisible(),
          "free time shows Time, not Length");
}

void presets_controls_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Kick and Bass"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("scopePresetNext"))));
  require(controller.preset_name() != QStringLiteral("Kick and Bass"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("scopePresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Kick and Bass"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("scopeMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("scopePresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Side Channel"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("scopePresetName")) == QStringLiteral("Side Channel"),
          "a preset shows by name");
  require(controller.choice(QStringLiteral("scope.channel")) == 1 &&
              !panel->root->property("overlayOn").toBool(),
          "and sets the side channel without the overlay");
  require(!item(*panel, QStringLiteral("scopeLegendKey"))->property("shown").toBool(),
          "the legend dims the hidden sidechain");
  controller.apply_preset(preset_index("Kick and Bass"));
  QCoreApplication::processEvents();

  auto* length = item(*panel, QStringLiteral("scopeSegment:scope.length"));
  click(*panel, scene(length, length->width() * 4.5 / 7.0, length->height() / 2));
  require(controller.choice(QStringLiteral("scope.length")) == 4, "a tile picks a bar");
  require(text_of(*panel, QStringLiteral("scopeReadout")) == QStringLiteral("WINDOW · LENGTH"),
          "the display reads back the tile: " +
              text_of(*panel, QStringLiteral("scopeReadout")).toStdString());

  auto* overlay = item(*panel, QStringLiteral("scopeToggle:scope.overlay"));
  require(overlay->property("on").toBool(), "the sidechain is overlaid by default");
  click(*panel, centre(overlay));
  require(controller.choice(QStringLiteral("scope.overlay")) == 0, "a click hides it");
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("scope.overlay")) == 1, "Space shows it again");

  auto* band = item(*panel, QStringLiteral("scopeSegment:scope.align_band"));
  click(*panel, scene(band, band->width() * 0.25, band->height() / 2));
  require(controller.choice(QStringLiteral("scope.align_band")) == 0, "a tile picks full band");

  auto* sync = item(*panel, QStringLiteral("scopeToggle:scope.sync"));
  click(*panel, centre(sync));
  require(controller.choice(QStringLiteral("scope.sync")) == 0, "sync off");
  auto* time = item(*panel, QStringLiteral("scopeKnob:scope.time"));
  require(time->isVisible(), "the Time knob appears");
  time->forceActiveFocus();
  const double before = controller.value(QStringLiteral("scope.time"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("scope.time")) > before, "Up lengthens the window");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("scopeStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double bpm = 120.0;
constexpr double rate = 48'000.0;
constexpr std::uint64_t beat_frames = 24'000;

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
  std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
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
    const double beats_per_frame = bpm / 60.0 / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const oscillate::native_dsp::Transport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, block_events, position, 1, transport, {}).status ==
                synth::SynthProcessStatus::rendered,
            "synth renders");
    std::ranges::copy(block, out.begin() + static_cast<std::ptrdiff_t>(position));
  }
  return out;
}

// A Pulse kick on every beat (the sidechain) and a Prism Sub Bass line
// under it (the track), `delay` frames late and scaled by `gain`. With
// `layered`, the bass carries its own copy of the kick, as an 808-style
// bass print does: that shared kick is what the alignment meter can time.
// A bass alone shares no waveform with the kick, so the meter reads it as
// uncertain.
struct Material {
  std::vector<engine::StereoFrame> track;
  std::vector<engine::StereoFrame> key;
};

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Material kick_and_bass(std::size_t frames, std::ptrdiff_t delay, float gain, bool layered) {
  std::vector<Note> kicks;
  std::vector<Note> bass;
  const std::uint8_t keys[4] = {33, 33, 36, 31};
  for (std::uint64_t beat = 0; beat * beat_frames < frames; ++beat) {
    kicks.push_back({beat * beat_frames, 2'400, 36});
    bass.push_back({beat * beat_frames, 20'000, keys[(beat / 4) % 4]});
  }
  Material m;
  m.key = render(synth::pulse::engine(), preset_of(*synth::pulse::engine().descriptor, "909 House"),
                 kicks, frames);
  auto dry = render(synth::prism::engine(),
                    preset_of(*synth::prism::engine().descriptor, "Sub Bass"), bass, frames);
  if (layered) {
    for (std::size_t i = 0; i < frames; ++i) {
      dry[i].left = 0.7F * dry[i].left + 0.6F * m.key[i].left;
      dry[i].right = 0.7F * dry[i].right + 0.6F * m.key[i].right;
    }
  }
  m.track.assign(frames, {0.0F, 0.0F});
  for (std::size_t i = 0; i < frames; ++i) {
    const auto j = static_cast<std::ptrdiff_t>(i) - delay;
    if (j >= 0) {
      m.track[i] = {gain * dry[static_cast<std::size_t>(j)].left,
                    gain * dry[static_cast<std::size_t>(j)].right};
    }
  }
  return m;
}

// The real Scope over the material, synced to 120 BPM; the telemetry
// frames every 1600 frames (a 30 Hz poll).
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values, const Material& m,
                                               std::size_t stop) {
  auto created = effects::NativeEffectRuntime::create(scope::engine(), values, rate);
  require(created.has_value(), "a Scope runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  std::vector<engine::StereoFrame> side;
  for (std::size_t position = 0; position < stop; position += 160) {
    const std::size_t size = std::min<std::size_t>(160, stop - position);
    block.assign(m.track.begin() + static_cast<std::ptrdiff_t>(position),
                 m.track.begin() + static_cast<std::ptrdiff_t>(position + size));
    side.assign(m.key.begin() + static_cast<std::ptrdiff_t>(position),
                m.key.begin() + static_cast<std::ptrdiff_t>(position + size));
    const double beats_per_frame = bpm / 60.0 / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const oscillate::native_dsp::Transport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, side, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Scope renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Scope publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("SCOPE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    std::ptrdiff_t delay; // Frames the bass is late.
    float gain;           // Negative inverts it.
    bool layered;         // The bass carries the kick too.
    double seconds;       // Where to stop.
    const char* touched;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"Kick and Bass", 96, 1.0F, true, 4.36, "scope.align_band", "/scope_kick_and_bass.png"},
        Shot{"Bar View", 96, 1.0F, true, 6.62, "scope.length", "/scope_bar_view.png"},
        Shot{"Kick and Bass", 0, -1.0F, true, 4.36, "scope.align", "/scope_inverted.png"},
        Shot{"Kick and Bass", 96, 1.0F, false, 4.36, "scope.align_band", "/scope_uncertain.png"},
        Shot{"Waveform", 96, 1.0F, true, 4.36, "scope.time", "/scope_waveform.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->setProperty("trackName", QStringLiteral("Bass"));
    panel->root->setProperty("keyName", QStringLiteral("the kick"));
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    const auto stop = static_cast<std::size_t>(shot.seconds * rate);
    const auto material = kick_and_bass(stop, shot.delay, shot.gain, shot.layered);
    const auto frames = real_telemetry(values, material, stop);
    // The last second of polls, in real time.
    for (std::size_t i = frames.size() > 30 ? frames.size() - 30 : 0; i < frames.size(); ++i) {
      adapter.set_telemetry(frames[i]);
      QTest::qWait(33);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    const auto& last = frames.back();
    std::cout << "wrote " << path.toStdString() << " (delay " << last[scope::telemetry::delay_ms]
              << " ms, polarity " << last[scope::telemetry::polarity] << ", correlation "
              << last[scope::telemetry::correlation] << ")\n";
  }
}

// The device-chain strip (device_strip_view.hpp): Scope's sweep at every
// fourth column (the outer min and max of each four), the beam, the key
// overlaid when there is one, and the correlation.
void telemetry_gives_the_strip_the_sweep() {
  namespace strip_view = oscillate::quick::strip_view;
  constexpr std::size_t per = strip_view::scope_columns_per_column;
  constexpr std::size_t columns = scope::columns / per;
  InMemoryNativeSynthAdapter adapter(preset("Kick and Bass"));
  NativeSynthDeviceController controller(table(), adapter);
  const Alignment keyed;
  adapter.set_telemetry(frame_of(keyed));
  auto strip = controller.telemetry().value(QStringLiteral("strip")).toMap();
  require(strip.value(QStringLiteral("kind")).toString() == QStringLiteral("scope") &&
              strip.value(QStringLiteral("columns")).toInt() == static_cast<int>(columns),
          "Scope's strip is a sweep of fewer columns");
  // frame_of: track column c spans +-(c / columns), the key half that; each
  // strip column's outermost is its last.
  const auto track = strip.value(QStringLiteral("track")).toList();
  const auto key = strip.value(QStringLiteral("key")).toList();
  require(track.size() == 2 * static_cast<qsizetype>(columns) && key.size() == track.size(),
          "(min, max) per column for the track and the key");
  constexpr float key_scale = 0.5F;
  for (std::size_t c = 0; c < columns; ++c) {
    const float outer = static_cast<float>(c * per + per - 1) / static_cast<float>(scope::columns);
    const auto at = 2 * static_cast<qsizetype>(c);
    require(track.at(at).toDouble() == static_cast<double>(-outer) &&
                track.at(at + 1).toDouble() == static_cast<double>(outer) &&
                key.at(at + 1).toDouble() == static_cast<double>(key_scale * outer),
            "column " + std::to_string(c) + " spans its four");
  }
  require(strip.value(QStringLiteral("write_column")).toInt() ==
              static_cast<int>(static_cast<std::size_t>(keyed.write_column) / per),
          "the beam stands at its column");
  require(strip.value(QStringLiteral("keyed")).toBool() &&
              strip.value(QStringLiteral("text")).toString() == QStringLiteral("corr 0.90"),
          "the correlation: " + strip.value(QStringLiteral("text")).toString().toStdString());
  Alignment unkeyed;
  unkeyed.key = false;
  adapter.set_telemetry(frame_of(unkeyed));
  strip = controller.telemetry().value(QStringLiteral("strip")).toMap();
  require(!strip.value(QStringLiteral("keyed")).toBool() &&
              strip.value(QStringLiteral("key")).toList().isEmpty() &&
              strip.value(QStringLiteral("text")).toString() == QStringLiteral("no key"),
          "no key, no overlay");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    telemetry_decodes_into_the_sweep_and_meter();
    the_hint_says_which_way_and_how_far();
    the_grid_follows_the_window();
    presets_controls_and_status();
    telemetry_gives_the_strip_the_sweep();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "scope panel contract passed\n";
  return 0;
}
