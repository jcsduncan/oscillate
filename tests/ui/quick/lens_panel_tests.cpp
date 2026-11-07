#include "device_strip_view.hpp"
#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/lens.hpp"
#include "oscillate/native_dsp/synth/ember.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QMetaObject>
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

// The Lens panel (ADR-0042): every parameter on a table-driven control, the
// telemetry decode into the spectrum (curves, hold, overlay, resonances)
// and the resonance table, note names against A4 = 440 Hz with cents, the
// cursor readout, the difference curve, the spectrogram growing once per
// analysis and pausing while frozen, presets and the drawing range, and
// screenshots on request: live ones from the real Lens over a deep-house
// loop from the native drums and synths.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace lens = oscillate::native_dsp::effects::lens;
namespace engine = oscillate::engine;

using oscillate::test::require;
// How long a UI wait may take before it counts as a hang (not a timing claim).
constexpr int ui_wait_ms = 5'000;

const synth::EngineDescriptor& table() { return *lens::engine().descriptor; }
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
      import "qrc:/lens"
      LensPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/LensPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Lens panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Lens panel was not created: " + component.errorString().toStdString());
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

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

std::vector<double> list_of(QQuickItem* target, const char* property) {
  std::vector<double> out;
  for (const QVariant& value : target->property(property).toList()) {
    out.push_back(value.toDouble());
  }
  return out;
}

QVariantMap note_of(QQuickItem* spectrum, double hz) {
  QVariant result;
  require(QMetaObject::invokeMethod(spectrum, "note", Q_RETURN_ARG(QVariant, result),
                                    Q_ARG(QVariant, hz)),
          "the spectrum names notes");
  return result.toMap();
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Mix"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1280 && panel->root->implicitHeight() >= 760,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"lensKnob:", "lensSegment:", "lensToggle:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("lensPresetName")) == QStringLiteral("Mix"),
          "the panel names its preset");
}

// A frame with known values: A rises 0.05 dB a point from -40, B sits at
// -50, the overlay at -45 (silent from point 300), A's hold 3 dB over A;
// two resonances; `hops` analyses.
// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<float> test_frame(float hops, float a_offset = 0.0F) {
  namespace t = lens::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  for (std::size_t j = 0; j < lens::points; ++j) {
    const float a = -40.0F + 0.05F * static_cast<float>(j) + a_offset;
    frame[t::curve_a + j] = a;
    frame[t::curve_b + j] = -50.0F;
    frame[t::overlay + j] = j < 300 ? -45.0F : -200.0F;
    frame[t::hold + j] = a + 3.0F;
  }
  frame[t::peaks + 0] = 55.0F;
  frame[t::peaks + 1] = -8.1F;
  frame[t::peaks + 2] = 466.16F;
  frame[t::peaks + 3] = -20.0F;
  frame[t::hops] = hops;
  return frame;
}

void telemetry_reaches_the_spectrum_and_table() {
  InMemoryNativeSynthAdapter adapter(preset("Mix"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* spectrum = item(*panel, QStringLiteral("lensSpectrum"));
  QTest::qWait(300);
  require(!spectrum->property("live").toBool(), "no telemetry: the spectrum previews");
  require(list_of(spectrum, "curveA").size() == 400, "the preview fills the spectrum");
  adapter.set_telemetry(test_frame(10.0F));
  require(QTest::qWaitFor([&] { return spectrum->property("live").toBool(); }, 1000),
          "live telemetry arrives");
  QTest::qWait(60);
  const auto a = list_of(spectrum, "curveA");
  const auto hold = list_of(spectrum, "hold");
  require(a.size() == 400 && std::fabs(a[10] + 39.5) < 1e-4 && std::fabs(a[399] + 20.05) < 1e-4,
          "curve A decodes");
  require(hold.size() == 400 && std::fabs(hold[10] + 36.5) < 1e-4, "the hold decodes");
  require(list_of(spectrum, "curveB").empty(), "a single view shows no second curve");
  require(list_of(spectrum, "overlay").empty(), "the overlay is off");
  // Peaks and the table.
  const auto peaks = spectrum->property("peaks").toList();
  require(peaks.size() == 2, "two resonances decode");
  require(std::fabs(peaks[0].toMap()[QStringLiteral("hz")].toDouble() - 55.0) < 1e-4 &&
              std::fabs(peaks[0].toMap()[QStringLiteral("db")].toDouble() + 8.1) < 1e-4,
          "the first resonance decodes");
  const QString first =
      item(*panel, QStringLiteral("lensResonance:0"))->property("text").toString();
  require(first == QStringLiteral("A1 0 ct 55.0 Hz −8.1 dB"),
          "the table names the resonance: " + first.toStdString());
  const QString second =
      item(*panel, QStringLiteral("lensResonance:1"))->property("text").toString();
  require(second == QStringLiteral("A♯4 0 ct 466 Hz −20.0 dB"),
          "and the second: " + second.toStdString());
  require(item(*panel, QStringLiteral("lensResonance:2"))->property("text").toString().isEmpty(),
          "absent resonances leave their rows empty");
  require(text_of(*panel, QStringLiteral("lensHops")) == QStringLiteral("10"),
          "the analyses count");
  // A pair view and the overlay.
  controller.set_choice(QStringLiteral("lens.view"), 4);
  controller.set_choice(QStringLiteral("lens.overlay"), 1);
  QCoreApplication::processEvents();
  QTest::qWait(60);
  const auto b = list_of(spectrum, "curveB");
  require(b.size() == 400 && std::fabs(b[0] + 50.0) < 1e-6, "Mid / Side shows the second curve");
  require(list_of(spectrum, "overlay").size() == 400, "the overlay shows");
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !spectrum->property("live").toBool(); }, 1000),
          "telemetry gone: back to the preview");
}

void notes_and_the_cursor_read_true() {
  InMemoryNativeSynthAdapter adapter(preset("Mix"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* spectrum = item(*panel, QStringLiteral("lensSpectrum"));
  struct Case {
    double hz;
    const char* name;
    int cents;
  };
  // A4 = 440 Hz; cents by 1200 log2(f / note).
  for (const Case c : {Case{440.0, "A4", 0}, Case{466.1638, "A♯4", 0}, Case{261.6256, "C4", 0},
                       Case{55.0, "A1", 0}, Case{27.5, "A0", 0}, Case{446.0, "A4", 23},
                       Case{432.0, "A4", -32}, Case{4186.009, "C8", 0}, Case{60.0, "B1", -49}}) {
    const auto n = note_of(spectrum, c.hz);
    require(n[QStringLiteral("name")].toString() == QString::fromUtf8(c.name) &&
                n[QStringLiteral("cents")].toInt() == c.cents,
            std::to_string(c.hz) + " Hz reads " +
                n[QStringLiteral("name")].toString().toStdString() + " " +
                std::to_string(n[QStringLiteral("cents")].toInt()) + " ct");
  }
  // Hovering 440 Hz on live data reads its note, frequency and level.
  adapter.set_telemetry(test_frame(1.0F));
  require(QTest::qWaitFor([&] { return spectrum->property("live").toBool(); }, 1000), "live");
  const double px = spectrum->property("px").toDouble();
  const double pw = spectrum->property("pw").toDouble();
  const double py = spectrum->property("py").toDouble();
  const double ph = spectrum->property("ph").toDouble();
  const double x = px + pw * std::log(440.0 / 20.0) / std::log(1000.0);
  QTest::mouseMove(panel->window.get(), scene(spectrum, x - 20, py + ph / 2));
  QTest::mouseMove(panel->window.get(), scene(spectrum, x, py + ph / 2));
  QCoreApplication::processEvents();
  QTest::qWait(30);
  const QString readout = text_of(*panel, QStringLiteral("lensCursorReadout"));
  require(readout.contains(QStringLiteral("A4")) && readout.contains(QStringLiteral("Hz")),
          "the cursor reads its note and frequency: " + readout.toStdString());
  const int point = spectrum->property("hoverPoint").toInt();
  const double level = -40.0 + 0.05 * point;
  require(readout.contains(QStringLiteral("−") + QString::number(std::fabs(level), 'f', 1) +
                           QStringLiteral(" dB")),
          "and the level under it: " + readout.toStdString());
}

void the_difference_is_mix_minus_reference() {
  InMemoryNativeSynthAdapter adapter(preset("Reference Match"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* spectrum = item(*panel, QStringLiteral("lensSpectrum"));
  adapter.set_telemetry(test_frame(1.0F));
  require(QTest::qWaitFor([&] { return spectrum->property("live").toBool(); }, 1000), "live");
  require(
      QTest::qWaitFor([&] { return spectrum->property("showDifference").toBool(); }, ui_wait_ms),
      "Reference Match shows the difference");
  const auto difference = list_of(spectrum, "difference");
  require(difference.size() == 400, "a difference per point");
  for (const std::size_t j : {0UL, 100UL, 299UL}) {
    const double expected = (-40.0 + 0.05 * static_cast<double>(j)) - -45.0;
    require(std::fabs(difference[j] - expected) < 1e-4, "difference is A minus the reference");
  }
  require(std::isnan(difference[300]) && std::isnan(difference[399]),
          "and undefined where the reference is silent");
  click(*panel, centre(item(*panel, QStringLiteral("lensDifferenceToggle"))));
  require(!spectrum->property("showDifference").toBool(), "the difference can be hidden");
  click(*panel, centre(item(*panel, QStringLiteral("lensDifferenceToggle"))));
  require(spectrum->property("showDifference").toBool(), "and shown again");
}

void the_spectrogram_grows_per_analysis_and_freeze_holds() {
  InMemoryNativeSynthAdapter adapter(preset("Mix"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* gram = item(*panel, QStringLiteral("lensSpectrogram"));
  require(gram->property("rows").toInt() == 0, "the spectrogram starts empty");
  // Each new analysis adds a row; the same analysis polled again adds none.
  // (Idle, the panel polls slowly: wait for the first frame to be seen.)
  adapter.set_telemetry(test_frame(1.0F, 1.0F));
  require(QTest::qWaitFor([&] { return gram->property("rows").toInt() == 1; }, 1000),
          "the first analysis paints a row");
  for (int hop = 2; hop <= 6; ++hop) {
    adapter.set_telemetry(test_frame(static_cast<float>(hop), static_cast<float>(hop)));
    QTest::qWait(90);
  }
  const int grown = gram->property("rows").toInt();
  require(grown == 6, "one row per analysis: " + std::to_string(grown));
  QTest::qWait(200);
  require(gram->property("rows").toInt() == grown, "the same analysis adds no rows");
  require(text_of(*panel, QStringLiteral("lensHistory")) == QStringLiteral("6"), "the rows count");
  // Freeze: the picture holds, frost shows, the spectrogram pauses (a frozen
  // Lens publishes the same analysis).
  click(*panel, centre(item(*panel, QStringLiteral("lensToggle:lens.freeze"))));
  require(controller.choice(QStringLiteral("lens.freeze")) == 1, "Freeze turns on");
  require(item(*panel, QStringLiteral("lensFrost"))->isVisible(), "a frozen picture is frosted");
  QTest::qWait(300);
  require(gram->property("rows").toInt() == grown, "and the spectrogram pauses");
  click(*panel, centre(item(*panel, QStringLiteral("lensToggle:lens.freeze"))));
  require(!item(*panel, QStringLiteral("lensFrost"))->isVisible(), "thawed, the frost clears");
  adapter.set_telemetry(test_frame(7.0F));
  require(QTest::qWaitFor([&] { return gram->property("rows").toInt() > grown; }, ui_wait_ms) &&
              gram->property("rows").toInt() == grown + 1,
          "and new analyses paint again");
}

void presets_range_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Mix"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("lensPresetNext"))));
  require(controller.preset_name() == QStringLiteral("Mastering"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("lensPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Mix"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("lensMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("lensPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Stereo Field"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("lensPresetName")) == QStringLiteral("Stereo Field"),
          "a preset shows by name");
  auto* spectrum = item(*panel, QStringLiteral("lensSpectrum"));
  require(spectrum->property("showB").toBool(), "Stereo Field draws its second curve");

  // The segments pick their choices.
  auto* resolution = item(*panel, QStringLiteral("lensSegment:lens.resolution"));
  click(*panel, scene(resolution, resolution->width() * 2.5 / 3.0, resolution->height() / 2));
  require(controller.choice(QStringLiteral("lens.resolution")) == 2, "a tile picks Precise");
  require(text_of(*panel, QStringLiteral("lensReadout")) == QStringLiteral("ANALYSIS · RESOLUTION"),
          "the display reads back the control: " +
              text_of(*panel, QStringLiteral("lensReadout")).toStdString());

  // The drawing range and ceiling are the panel's own.
  click(*panel, centre(item(*panel, QStringLiteral("lensRange:60"))));
  require(spectrum->property("rangeDb").toDouble() == 60.0, "60 dB range");
  click(*panel, centre(item(*panel, QStringLiteral("lensRange:120"))));
  require(spectrum->property("rangeDb").toDouble() == 120.0, "120 dB range");
  click(*panel, centre(item(*panel, QStringLiteral("lensCeilingUp"))));
  require(spectrum->property("topDb").toDouble() == 6.0 &&
              text_of(*panel, QStringLiteral("lensCeiling")) == QStringLiteral("TOP +6"),
          "the ceiling rises 6 dB");
  click(*panel, centre(item(*panel, QStringLiteral("lensCeilingDown"))));
  click(*panel, centre(item(*panel, QStringLiteral("lensCeilingDown"))));
  require(spectrum->property("topDb").toDouble() == -6.0, "and falls");

  auto* decay = item(*panel, QStringLiteral("lensKnob:lens.hold_decay"));
  decay->forceActiveFocus();
  const double before = controller.value(QStringLiteral("lens.hold_decay"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("lens.hold_decay")) > before, "Up raises the hold decay");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("lensStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

struct Note {
  std::uint64_t frame;
  std::uint64_t length;
  std::uint8_t key;
  std::uint8_t velocity = 105;
};

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<engine::StereoFrame> render(const synth::SynthEngine& engine,
                                        const std::vector<double>& values,
                                        const std::vector<Note>& notes, std::size_t frames,
                                        float gain) {
  auto created = synth::NativeSynthRuntime::create(engine, values, 48'000.0);
  require(created.has_value(), "a synth runtime");
  auto runtime = std::move(created.value());
  struct Event {
    std::uint64_t frame;
    engine::MidiEventKind kind;
    std::uint8_t key;
    std::uint8_t velocity;
    std::uint64_t id;
  };
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto& n : notes) {
    events.push_back({n.frame, engine::MidiEventKind::note_on, n.key, n.velocity, id});
    events.push_back({n.frame + n.length, engine::MidiEventKind::note_off, n.key, 0, id});
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
      event.velocity = e.velocity;
      block_events.push_back(event);
    }
    const double beats_per_frame = 122.0 / 60.0 / 48'000.0;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const synth::SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, block_events, position, 1, transport, {}).status ==
                synth::SynthProcessStatus::rendered,
            "synth renders");
    for (std::size_t i = 0; i < size; ++i) {
      out[position + i].left += block[i].left * gain;
      out[position + i].right += block[i].right * gain;
    }
  }
  return out;
}

void add_into(std::vector<engine::StereoFrame>& into,
              const std::vector<engine::StereoFrame>& part) {
  for (std::size_t i = 0; i < into.size(); ++i) {
    into[i].left += part[i].left;
    into[i].right += part[i].right;
  }
}

// A deep-house loop at 122 BPM in A minor: Pulse 909 House kick, claps and
// hats; a rolling Prism Sub Bass; Ember Juno Strings chords; a Prism pluck.
std::vector<engine::StereoFrame> deep_house(std::size_t frames) {
  const double beat = 60.0 / 122.0 * 48'000.0;
  const auto at = [&](double b) { return static_cast<std::uint64_t>(std::lround(b * beat)); };
  std::vector<Note> drums;
  std::vector<Note> bass;
  std::vector<Note> pads;
  std::vector<Note> pluck;
  const std::uint8_t roots[4] = {33, 29, 36, 31};
  const std::uint8_t chords[4][4] = {
      {57, 60, 64, 69}, {53, 57, 60, 65}, {55, 60, 64, 67}, {55, 59, 62, 67}};
  for (int b = 0; static_cast<double>(at(b)) < static_cast<double>(frames); ++b) {
    const int bar = (b / 4) % 4;
    drums.push_back({at(b), 2'000, 36, 112});
    drums.push_back({at(b + 0.5), 1'500, 46, 90});
    drums.push_back({at(b + 0.25), 600, 42, 56});
    drums.push_back({at(b + 0.75), 600, 42, 64});
    if (b % 2 == 1) {
      drums.push_back({at(b), 2'000, 40, 96});
    }
    bass.push_back({at(b + 0.25), at(0.2), roots[bar], 100});
    bass.push_back({at(b + 0.5), at(0.2), static_cast<std::uint8_t>(roots[bar] + 12), 90});
    bass.push_back({at(b + 0.75), at(0.2), roots[bar], 96});
    if (b % 4 == 0) {
      for (const auto key : chords[bar]) {
        pads.push_back({at(b), at(3.9), key, 80});
      }
    }
    for (int s = 0; s < 4; ++s) {
      pluck.push_back({at(b + 0.25 * s), at(0.2), static_cast<std::uint8_t>(chords[bar][s] + 12),
                       static_cast<std::uint8_t>(s % 2 == 0 ? 92 : 70)});
    }
  }
  auto mix =
      render(synth::pulse::engine(), preset_of(*synth::pulse::engine().descriptor, "909 House"),
             drums, frames, 0.8F);
  add_into(mix,
           render(synth::prism::engine(), preset_of(*synth::prism::engine().descriptor, "Sub Bass"),
                  bass, frames, 0.55F));
  add_into(mix, render(synth::ember::engine(),
                       preset_of(*synth::ember::engine().descriptor, "Juno Strings"), pads, frames,
                       0.3F));
  add_into(mix,
           render(synth::prism::engine(), preset_of(*synth::prism::engine().descriptor, "Pluck"),
                  pluck, frames, 0.2F));
  return mix;
}

// Pink-ish noise as a reference: white noise through a cascade of one-pole
// low-passes (Paul Kellet's refined filter).
std::vector<engine::StereoFrame> pink_reference(std::size_t frames) {
  std::vector<engine::StereoFrame> out(frames);
  std::uint32_t state = 12'345;
  double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
  for (auto& frame : out) {
    state = state * 1'664'525U + 1'013'904'223U;
    const double white = static_cast<double>(state >> 8U) / 8'388'608.0 - 1.0;
    b0 = 0.99886 * b0 + white * 0.0555179;
    b1 = 0.99332 * b1 + white * 0.0750759;
    b2 = 0.96900 * b2 + white * 0.1538520;
    b3 = 0.86650 * b3 + white * 0.3104856;
    b4 = 0.55000 * b4 + white * 0.5329522;
    b5 = -0.7616 * b5 - white * 0.0168980;
    const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
    b6 = white * 0.115926;
    frame.left = static_cast<float>(pink * 0.06);
    frame.right = frame.left;
  }
  return out;
}

// The real Lens over `audio`, a telemetry frame every 1600 frames (30 Hz).
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& audio,
                                               const std::vector<engine::StereoFrame>* side) {
  auto created = effects::NativeEffectRuntime::create(lens::engine(), values, 48'000.0);
  require(created.has_value(), "a Lens runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  std::vector<engine::StereoFrame> side_block;
  for (std::size_t position = 0; position + 160 <= audio.size(); position += 160) {
    block.assign(audio.begin() + static_cast<std::ptrdiff_t>(position),
                 audio.begin() + static_cast<std::ptrdiff_t>(position + 160));
    side_block.clear();
    if (side != nullptr) {
      side_block.assign(side->begin() + static_cast<std::ptrdiff_t>(position),
                        side->begin() + static_cast<std::ptrdiff_t>(position + 160));
    }
    const synth::SynthTransport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, side_block, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Lens renders");
    if ((position + 160) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Lens publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("LENS_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const std::size_t frames = std::size_t{48'000} * 6;
  const auto loop = deep_house(frames);
  const auto reference = pink_reference(frames);
  struct Shot {
    const char* preset;
    int view;
    bool side;
    double hover_hz;
    const char* touched;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"Mix", -1, false, 55.0, "lens.smoothing", "/lens_mix.png"},
        Shot{"Stereo Field", -1, false, 2'400.0, "lens.view", "/lens_mid_side.png"},
        Shot{"Reference Match", -1, true, 900.0, "lens.overlay", "/lens_reference_match.png"},
        Shot{"Low End", -1, false, 110.0, "lens.resolution", "/lens_low_end.png"},
        Shot{"Transients", -1, false, 0.0, "lens.average", "/lens_spectrogram.png"}}) {
    auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    if (std::string_view(shot.preset) == "Low End") {
      panel->root->setProperty("rangeDb", 60);
    }
    const auto polled = real_telemetry(values, loop, shot.side ? &reference : nullptr);
    // Play the polls back in real time so the spectrogram paints as it would.
    for (const auto& frame : polled) {
      adapter.set_telemetry(frame);
      QTest::qWait(33);
    }
    QTest::qWait(80);
    if (shot.hover_hz > 0.0) {
      auto* spectrum = item(*panel, QStringLiteral("lensSpectrum"));
      const double x = spectrum->property("px").toDouble() + spectrum->property("pw").toDouble() *
                                                                 std::log(shot.hover_hz / 20.0) /
                                                                 std::log(1000.0);
      const double y =
          spectrum->property("py").toDouble() + spectrum->property("ph").toDouble() * 0.45;
      QTest::mouseMove(panel->window.get(), scene(spectrum, x - 25, y));
      QTest::mouseMove(panel->window.get(), scene(spectrum, x, y));
      QTest::qWait(60);
    }
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

// The device-chain strip (device_strip_view.hpp): Lens's first curve at
// every second point (the louder of each pair) in its default dB window, and
// the strongest resonance.
void telemetry_gives_the_strip_the_curve() {
  namespace t = lens::telemetry;
  namespace strip_view = oscillate::quick::strip_view;
  constexpr float hops = 10.0F;
  InMemoryNativeSynthAdapter adapter(preset("Mix"));
  NativeSynthDeviceController controller(table(), adapter);
  const auto frame = test_frame(hops);
  adapter.set_telemetry(frame);
  const auto strip = controller.telemetry().value(QStringLiteral("strip")).toMap();
  require(strip.value(QStringLiteral("kind")).toString() == QStringLiteral("curve"),
          "Lens's strip is a curve");
  const auto values = strip.value(QStringLiteral("values")).toList();
  constexpr std::size_t per = strip_view::lens_points_per_point;
  require(values.size() == static_cast<qsizetype>(lens::points / per), "every second point");
  for (std::size_t i = 0; i < lens::points / per; ++i) {
    const float louder = std::max(frame[t::curve_a + per * i], frame[t::curve_a + per * i + 1]);
    require(values.at(static_cast<qsizetype>(i)).toDouble() == static_cast<double>(louder),
            "point " + std::to_string(i) + " is the louder of its pair");
  }
  require(strip.value(QStringLiteral("floor_db")).toDouble() == strip_view::spectrum_floor_db &&
              strip.value(QStringLiteral("ceiling_db")).toDouble() ==
                  strip_view::spectrum_ceiling_db,
          "in the Lens panel's default window");
  require(strip.value(QStringLiteral("text")).toString() == QStringLiteral("peak 55.0 Hz"),
          "the strongest resonance: " +
              strip.value(QStringLiteral("text")).toString().toStdString());
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    telemetry_reaches_the_spectrum_and_table();
    notes_and_the_cursor_read_true();
    the_difference_is_mix_minus_reference();
    the_spectrogram_grows_per_analysis_and_freeze_holds();
    presets_range_and_status();
    telemetry_gives_the_strip_the_curve();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "lens panel contract passed\n";
  return 0;
}
