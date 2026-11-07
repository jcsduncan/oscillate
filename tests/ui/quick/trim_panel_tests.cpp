#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/trim.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
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
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Trim panel (ADR-0042): every parameter on a table-driven control
// (the two polarity switches over one four-way choice); the telemetry
// decode into the input and output meters (peak text, RMS fill, held peak)
// and the gain change between them; the gain readout and unity; pan as
// L/C/R; the stereo image following pan, width, mono, swap, polarity and
// solo; the summary line; presets and status; and screenshots on request,
// live from the real Trim over a Prism Juno Chorus Pad chord.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace trim = oscillate::native_dsp::effects::trim;
namespace engine = oscillate::engine;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *trim::engine().descriptor; }
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
      import "qrc:/trim"
      TrimPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/TrimPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Trim panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Trim panel was not created: " + component.errorString().toStdString());
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

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, 1000);
}

// ---- Frames -----------------------------------------------------------------------

struct Levels {
  float in_peak[2] = {-6.0F, -12.0F};
  float out_peak[2] = {-9.0F, -15.0F};
  float in_rms[2] = {-18.0F, -24.0F};
  float out_rms[2] = {-21.5F, -27.5F};
};

std::vector<float> frame_of(const Levels& l) {
  namespace t = trim::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  for (std::size_t c = 0; c < 2; ++c) {
    frame[t::input_peak + c] = l.in_peak[c];
    frame[t::output_peak + c] = l.out_peak[c];
    frame[t::input_rms + c] = l.in_rms[c];
    frame[t::output_rms + c] = l.out_rms[c];
  }
  return frame;
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Unity"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 720,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"trimKnob:", "trimSegment:", "trimToggle:", "trimPolarity:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("trimPresetName")) == QStringLiteral("Unity"),
          "the panel names its preset");
  require(panel->root->property("atUnity").toBool(), "Unity is at rest");
}

void telemetry_decodes_into_the_meters() {
  InMemoryNativeSynthAdapter adapter(preset("Unity"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  for (const char* name : {"trimInPeakL", "trimOutPeakR", "trimChange"}) {
    require(text_of(*panel, QString::fromLatin1(name)) == QStringLiteral("—"),
            std::string(name) + " is a dash with nothing measured");
  }
  require(item(*panel, QStringLiteral("trimInMeterL"))->property("rmsAt").toDouble() == 0.0,
          "an idle meter is empty");

  adapter.set_telemetry(frame_of(Levels{}));
  require(wait_for_text(*panel, QStringLiteral("trimInPeakL"), QStringLiteral("−6.0")),
          "the input peak decodes: " +
              text_of(*panel, QStringLiteral("trimInPeakL")).toStdString());
  require(text_of(*panel, QStringLiteral("trimInPeakR")) == QStringLiteral("−12.0") &&
              text_of(*panel, QStringLiteral("trimOutPeakL")) == QStringLiteral("−9.0") &&
              text_of(*panel, QStringLiteral("trimOutPeakR")) == QStringLiteral("−15.0"),
          "each channel's peak");
  // Fill to the RMS and hold at the peak, on a -60..+6 dB scale.
  auto* meter = item(*panel, QStringLiteral("trimInMeterL"));
  require(std::fabs(meter->property("rmsAt").toDouble() - 42.0 / 66.0) < 1e-6,
          "the RMS fills to its place on the scale");
  require(std::fabs(meter->property("peakAt").toDouble() - 54.0 / 66.0) < 1e-6,
          "the peak holds at its place");
  auto* fill = find_item(meter, QStringLiteral("trimMeterFill"));
  require(fill != nullptr && std::fabs(fill->height() - (meter->height() - 4) * 42.0 / 66.0) < 1e-3,
          "the fill's height follows");
  // The change between the louder channels' RMS: -21.5 against -18.
  require(text_of(*panel, QStringLiteral("trimChange")) == QStringLiteral("−3.5"),
          "the gain change: " + text_of(*panel, QStringLiteral("trimChange")).toStdString());

  // Silence on the input reads minus infinity and no change.
  Levels quiet;
  quiet.in_peak[0] = quiet.in_peak[1] = -200.0F;
  quiet.in_rms[0] = quiet.in_rms[1] = -200.0F;
  adapter.set_telemetry(frame_of(quiet));
  require(wait_for_text(*panel, QStringLiteral("trimInPeakL"), QStringLiteral("−∞")),
          "silence reads minus infinity");
  require(text_of(*panel, QStringLiteral("trimChange")) == QStringLiteral("—"),
          "no change without an input");
  require(meter->property("rmsAt").toDouble() == 0.0 && meter->property("peakAt").toDouble() == 0.0,
          "silence is below the scale");
  // Over -3 dBFS the peak turns red.
  Levels hot;
  hot.out_peak[0] = -1.0F;
  adapter.set_telemetry(frame_of(hot));
  require(wait_for_text(*panel, QStringLiteral("trimOutPeakL"), QStringLiteral("−1.0")), "hot");
  require(
      item(*panel, QStringLiteral("trimOutPeakL"))->property("color").value<QColor>() ==
          panel->root->property("theme").value<QObject*>()->property("meterHigh").value<QColor>(),
      "a hot peak is red");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("trimInPeakL"), QStringLiteral("—")),
          "no telemetry: a dash again");
}

void controls_drive_the_readouts_and_image() {
  InMemoryNativeSynthAdapter adapter(preset("Unity"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("trimGainValue")) == QStringLiteral("0.0"),
          "unity gain reads 0.0");
  auto* gain = item(*panel, QStringLiteral("trimKnob:trim.gain"));
  require(std::fabs(gain->property("origin").toDouble() - 0.6) < 1e-9,
          "the gain arc starts at unity");
  gain->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(controller.value(QStringLiteral("trim.gain")) < 0.0, "Down lowers the gain");
  require(text_of(*panel, QStringLiteral("trimGainValue")).startsWith(QStringLiteral("−")),
          "and it reads negative");
  require(!panel->root->property("atUnity").toBool(), "no longer at unity");
  click(*panel, centre(item(*panel, QStringLiteral("trimUnity"))));
  require(controller.value(QStringLiteral("trim.gain")) == 0.0, "the unity button resets gain");

  controller.set_value(QStringLiteral("trim.pan"), -0.4);
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("trimKnob:trim.pan"))->property("valueText").toString() ==
              QStringLiteral("L 40"),
          "pan reads L 40");
  controller.set_value(QStringLiteral("trim.pan"), 1.0);
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("trimKnob:trim.pan"))->property("valueText").toString() ==
              QStringLiteral("R 100"),
          "and R 100");
  controller.set_value(QStringLiteral("trim.pan"), 0.0);
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("trimKnob:trim.pan"))->property("valueText").toString() ==
              QStringLiteral("C"),
          "and C");

  // The two polarity switches set bits of one choice.
  click(*panel, centre(item(*panel, QStringLiteral("trimPolarity:L"))));
  require(controller.choice(QStringLiteral("trim.invert")) == 1, "Ø L inverts the left");
  click(*panel, centre(item(*panel, QStringLiteral("trimPolarity:R"))));
  require(controller.choice(QStringLiteral("trim.invert")) == 3, "Ø R too: both");
  click(*panel, centre(item(*panel, QStringLiteral("trimPolarity:L"))));
  require(controller.choice(QStringLiteral("trim.invert")) == 2, "Ø L off: the right alone");
  require(item(*panel, QStringLiteral("trimPolarity:R"))->property("on").toBool() &&
              !item(*panel, QStringLiteral("trimPolarity:L"))->property("on").toBool(),
          "the switches show it");
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("trim.invert")) == 3,
          "Space on the focused Ø L flips it");
  controller.set_choice(QStringLiteral("trim.invert"), 0);

  // The stereo image follows.
  auto* stage = item(*panel, QStringLiteral("trimStage"));
  controller.set_value(QStringLiteral("trim.width"), 1.5);
  controller.set_value(QStringLiteral("trim.pan"), -0.25);
  QCoreApplication::processEvents();
  require(std::fabs(stage->property("leftEdge").toDouble() + 1.6) < 1e-9 &&
              std::fabs(stage->property("rightEdge").toDouble() - 1.25) < 1e-9,
          "width spreads the band past the speakers, pan moves it");
  click(*panel, centre(item(*panel, QStringLiteral("trimToggle:trim.mono"))));
  require(controller.choice(QStringLiteral("trim.mono")) == 1, "mono on");
  require(stage->property("spread").toDouble() == 0.0, "mono collapses the image");
  require(item(*panel, QStringLiteral("trimKnob:trim.width"))->property("inactive").toBool(),
          "width is inactive in mono");
  click(*panel, centre(item(*panel, QStringLiteral("trimToggle:trim.mono"))));
  click(*panel, centre(item(*panel, QStringLiteral("trimToggle:trim.swap"))));
  require(stage->property("swap").toBool(), "swap mirrors the image");

  auto* solo = item(*panel, QStringLiteral("trimSegment:trim.solo"));
  click(*panel, scene(solo, solo->width() * 4.5 / 5.0, solo->height() / 2));
  require(controller.choice(QStringLiteral("trim.solo")) == 4, "a tile solos the side");
  require(stage->property("solo").toInt() == 4 && stage->property("spread").toDouble() == 0.0,
          "a solo plays on both speakers: a point");
  require(item(*panel, QStringLiteral("trimReadout"))->property("text").toString() ==
              QStringLiteral("MONITOR · SOLO"),
          "the display reads back the tile");
}

QString summary_of(Panel& panel) {
  QVariant result;
  require(QMetaObject::invokeMethod(panel.root, "summary", Q_RETURN_ARG(QVariant, result)),
          "summary() runs");
  return result.toString();
}

void the_summary_says_what_trim_does() {
  InMemoryNativeSynthAdapter adapter(preset("Unity"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(summary_of(*panel) == QStringLiteral("UNITY · BIT-EXACT PASS-THROUGH"),
          "unity says so: " + summary_of(*panel).toStdString());
  controller.set_value(QStringLiteral("trim.gain"), -4.5);
  controller.set_value(QStringLiteral("trim.pan"), 0.3);
  controller.set_value(QStringLiteral("trim.width"), 1.25);
  controller.set_choice(QStringLiteral("trim.invert"), 2);
  controller.set_choice(QStringLiteral("trim.rotate"), 1);
  QCoreApplication::processEvents();
  require(summary_of(*panel) == QStringLiteral("−4.5 DB · PAN R 30 · WIDTH 125% · Ø R · ROTATED"),
          "moves are listed: " + summary_of(*panel).toStdString());
}

void presets_controls_and_status() {
  InMemoryNativeSynthAdapter adapter(preset("Unity"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("trimPresetNext"))));
  require(controller.preset_name() != QStringLiteral("Unity"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("trimPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Unity"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("trimMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("trimPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Headroom Repair"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("trimPresetName")) == QStringLiteral("Headroom Repair"),
          "a preset shows by name");
  require(item(*panel, QStringLiteral("trimToggle:trim.dc"))->property("on").toBool() &&
              item(*panel, QStringLiteral("trimToggle:trim.rotate"))->property("on").toBool(),
          "and lights its switches");

  auto* law = item(*panel, QStringLiteral("trimSegment:trim.law"));
  click(*panel, scene(law, law->width() * 3.5 / 4.0, law->height() / 2));
  require(controller.choice(QStringLiteral("trim.law")) == 3, "a tile picks the -6 dB law");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("trimStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double bpm = 120.0;
constexpr double rate = 48'000.0;

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
      event.velocity = 100;
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

// A held A minor ninth on the Juno Chorus Pad: wide, stereo, steady.
std::vector<engine::StereoFrame> pad(std::size_t frames) {
  std::vector<Note> chord;
  for (const int key : {45, 52, 55, 60, 64, 71}) {
    chord.push_back({0, frames, static_cast<std::uint8_t>(key)});
  }
  return render(synth::prism::engine(),
                preset_of(*synth::prism::engine().descriptor, "Juno Chorus Pad"), chord, frames);
}

// The real Trim over the pad; the telemetry frames every 1600 frames (a
// 30 Hz poll).
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const std::vector<engine::StereoFrame>& input) {
  auto created = effects::NativeEffectRuntime::create(trim::engine(), values, rate);
  require(created.has_value(), "a Trim runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<engine::StereoFrame> block;
  for (std::size_t position = 0; position < input.size(); position += 160) {
    const std::size_t size = std::min<std::size_t>(160, input.size() - position);
    block.assign(input.begin() + static_cast<std::ptrdiff_t>(position),
                 input.begin() + static_cast<std::ptrdiff_t>(position + size));
    const double beats_per_frame = bpm / 60.0 / rate;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const synth::SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Trim renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Trim publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("TRIM_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const auto input = pad(static_cast<std::size_t>(3.0 * rate));
  struct Shot {
    const char* preset;
    std::vector<std::pair<const char*, double>> moves;
    const char* touched;
    const char* file;
  };
  const std::vector<Shot> shots{
      {"Unity", {}, "", "/trim_unity.png"},
      {"Unity",
       {{"trim.gain", -4.5},
        {"trim.pan", -0.3},
        {"trim.law", 1},
        {"trim.width", 1.6},
        {"trim.invert", 2},
        {"trim.rotate", 1}},
       "trim.width",
       "/trim_moves.png"},
      {"Side Solo", {}, "trim.solo", "/trim_side_solo.png"},
      {"Mono Check", {{"trim.gain", 3.0}}, "trim.mono", "/trim_mono_check.png"},
  };
  for (const Shot& shot : shots) {
    auto values = preset(shot.preset);
    for (const auto& [id, value] : shot.moves) {
      for (std::size_t i = 0; i < table().parameters.size(); ++i) {
        if (table().parameters[i].id == id) {
          values[i] = value;
        }
      }
    }
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    const auto frames = real_telemetry(values, input);
    for (std::size_t i = frames.size() > 30 ? frames.size() - 30 : 0; i < frames.size(); ++i) {
      adapter.set_telemetry(frames[i]);
      QTest::qWait(33);
    }
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    const auto& last = frames.back();
    std::cout << "wrote " << path.toStdString() << " (in RMS " << last[trim::telemetry::input_rms]
              << " / " << last[trim::telemetry::input_rms + 1] << ", out RMS "
              << last[trim::telemetry::output_rms] << " / " << last[trim::telemetry::output_rms + 1]
              << " dBFS)\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    telemetry_decodes_into_the_meters();
    controls_drive_the_readouts_and_image();
    the_summary_says_what_trim_does();
    presets_controls_and_status();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "trim panel contract passed\n";
  return 0;
}
