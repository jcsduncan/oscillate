#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/pocket.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
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
#include <vector>

// The Pocket panel (ADR-0041): every parameter on a table-driven control,
// Margin only in Unmask, the telemetry decode into the masking map, meters
// and waveforms, held peaks that hold and then fall, focus handles that
// never cross, a preview from the laws with nothing playing, presets and
// status, and screenshots on request (live ones from the real Pocket over a
// Pulse kick and a Prism bass).
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace pocket = oscillate::native_dsp::effects::pocket;
namespace engine = oscillate::engine;

using oscillate::test::require;
// How long a UI wait may take before it counts as a hang (not a timing claim).
constexpr int ui_wait_ms = 5'000;

const oscillate::native_dsp::EngineDescriptor& table() { return *pocket::engine().descriptor; }
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
      import "qrc:/pocket"
      PocketPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/PocketPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Pocket panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Pocket panel was not created: " + component.errorString().toStdString());
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

void drag(Panel& panel, QPoint from, QPoint to) {
  QTest::mousePress(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  for (int step = 1; step <= 8; ++step) {
    const QPoint at = from + (to - from) * step / 8;
    QTest::mouseMove(panel.window.get(), at);
  }
  QTest::mouseRelease(panel.window.get(), Qt::LeftButton, Qt::NoModifier, to);
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

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1280 && panel->root->implicitHeight() >= 700,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind : {"pocketKnob:", "pocketSegment:", "pocketToggle:", "pocketFocus:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("pocketPresetName")) == QStringLiteral("Clean Low End"),
          "the panel names its preset");
}

void margin_shows_only_for_unmask() {
  InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* margin = item(*panel, QStringLiteral("pocketKnob:pocket.margin"));
  require(margin->isVisible(), "Unmask shows its margin");
  auto* mode = item(*panel, QStringLiteral("pocketSegment:pocket.mode"));
  const auto tile = [&](int index) {
    return scene(mode, mode->width() / 2, mode->height() * (index + 0.5) / 3.0);
  };
  const QString descriptions[3] = {QStringLiteral("hides the kick"),
                                   QStringLiteral("kick's energy"), QStringLiteral("pump")};
  for (int m : {1, 2, 0}) {
    click(*panel, tile(m));
    require(controller.choice(QStringLiteral("pocket.mode")) == m, "a tile picks the mode");
    require(margin->isVisible() == (m == 0),
            "Margin shows only in Unmask (mode " + std::to_string(m) + ")");
    require(text_of(*panel, QStringLiteral("pocketModeDescription"))
                .contains(descriptions[m], Qt::CaseInsensitive),
            "the mode is described: " +
                text_of(*panel, QStringLiteral("pocketModeDescription")).toStdString());
  }
}

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<float> test_frame(float kick_band5, float overlap_after) {
  namespace t = pocket::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  for (std::size_t b = 0; b < pocket::bands; ++b) {
    frame[t::kick_db + b] = -40.0F + static_cast<float>(b);
    frame[t::bass_db + b] = -20.0F - static_cast<float>(b);
    frame[t::reduction_db + b] = b == 3 ? -6.5F : b == 4 ? -2.0F : 0.0F;
  }
  frame[t::kick_db + 5] = kick_band5;
  frame[t::overlap_before_db] = -20.0F;
  frame[t::overlap_after_db] = overlap_after;
  for (std::size_t i = 0; i < pocket::wave_points; ++i) {
    const float k = 0.8F * std::exp(-static_cast<float>(i % 64) / 12.0F);
    frame[t::wave_kick + 2 * i] = -k;
    frame[t::wave_kick + 2 * i + 1] = k;
    frame[t::wave_bass_in + 2 * i] = -0.4F;
    frame[t::wave_bass_in + 2 * i + 1] = 0.4F;
    frame[t::wave_bass_out + 2 * i] = -0.3F;
    frame[t::wave_bass_out + 2 * i + 1] = 0.3F;
  }
  return frame;
}

void telemetry_reaches_the_map_meters_and_waves() {
  InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* map = item(*panel, QStringLiteral("pocketMap"));
  auto* waves = item(*panel, QStringLiteral("pocketWaves"));
  QTest::qWait(300);
  require(!map->property("live").toBool(), "no telemetry: the map previews");
  adapter.set_telemetry(test_frame(-12.0F, -26.0F));
  require(QTest::qWaitFor([&] { return map->property("live").toBool(); }, 1000),
          "live telemetry arrives");
  QTest::qWait(80);
  const auto kick = list_of(map, "kick");
  const auto bass = list_of(map, "bass");
  const auto cut = list_of(map, "cut");
  require(kick.size() == 24 && bass.size() == 24 && cut.size() == 24, "24 bands each");
  require(std::fabs(kick[2] + 38.0) < 1e-6 && std::fabs(kick[5] + 12.0) < 1e-6 &&
              std::fabs(bass[2] + 22.0) < 1e-6,
          "the spectra decode");
  require(std::fabs(cut[3] - 6.5) < 1e-6 && std::fabs(cut[4] - 2.0) < 1e-6 && cut[0] == 0.0,
          "cuts decode as positive dB");
  require(text_of(*panel, QStringLiteral("pocketCleared")) == QStringLiteral("6.0"),
          "space cleared is overlap in minus out: " +
              text_of(*panel, QStringLiteral("pocketCleared")).toStdString());
  require(text_of(*panel, QStringLiteral("pocketOverlapBefore")) == QStringLiteral("−20.0 dB") &&
              text_of(*panel, QStringLiteral("pocketOverlapAfter")) == QStringLiteral("−26.0 dB"),
          "the overlap bars read out");
  require(text_of(*panel, QStringLiteral("pocketDeepestCut"))
              .startsWith(QStringLiteral("−6.5 dB @ 62")),
          "the deepest cut and its band: " +
              text_of(*panel, QStringLiteral("pocketDeepestCut")).toStdString());
  require(waves->property("columns").toInt() == 256 && list_of(waves, "bassOut").size() == 512,
          "the waveforms carry 256 columns");
  require(std::fabs(list_of(waves, "kick")[1] - 0.8) < 1e-6, "and their values");
  // Hover reads one band out.
  auto* hover = item(*panel, QStringLiteral("pocketHover"));
  const double x = map->property("px").toDouble() + map->property("pw").toDouble() *
                                                        std::log(62.2 / 25.0) /
                                                        std::log(10000.0 / 25.0);
  QTest::mouseMove(panel->window.get(), scene(map, x, hover->y() + hover->height() / 2));
  QCoreApplication::processEvents();
  require(
      text_of(*panel, QStringLiteral("pocketBandReadout")).contains(QStringLiteral("CUT −6.5 dB")),
      "hovering a band reads its cut: " +
          text_of(*panel, QStringLiteral("pocketBandReadout")).toStdString());
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !map->property("live").toBool(); }, 1000),
          "telemetry gone: back to the preview");
}

void held_peaks_hold_then_fall() {
  InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* map = item(*panel, QStringLiteral("pocketMap"));
  adapter.set_telemetry(test_frame(-6.0F, -26.0F));
  require(QTest::qWaitFor([&] { return map->property("live").toBool(); }, 1000), "live");
  require(QTest::qWaitFor([&] { return std::fabs(list_of(map, "heldKick")[5] + 6.0) < 1e-6; },
                          ui_wait_ms),
          "a peak is held");
  // The kick falls away: the held line stays for the hold time...
  adapter.set_telemetry(test_frame(-30.0F, -26.0F));
  require(
      QTest::qWaitFor([&] { return std::fabs(list_of(map, "kick")[5] + 30.0) < 1e-6; }, ui_wait_ms),
      "the spectrum follows at once");
  require(std::fabs(list_of(map, "heldKick")[5] + 6.0) < 1e-6, "but the peak holds");
  // ...then falls slowly (14 dB/s), never below the signal.
  QTest::qWait(900);
  const double fallen = list_of(map, "heldKick")[5];
  require(fallen < -6.5 && fallen > -20.0, "then falls slowly: " + std::to_string(fallen));
  QTest::qWait(2'000);
  require(std::fabs(list_of(map, "heldKick")[5] + 30.0) < 1e-6, "and settles on the signal");
  // A louder hit lifts it at once.
  adapter.set_telemetry(test_frame(-3.0F, -26.0F));
  require(QTest::qWaitFor([&] { return std::fabs(list_of(map, "heldKick")[5] + 3.0) < 1e-6; },
                          ui_wait_ms),
          "a new peak lifts the line");
}

void focus_handles_never_cross() {
  InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* map = item(*panel, QStringLiteral("pocketMap"));
  const auto grip_of = [&](const char* id) {
    auto* handle = item(*panel, QStringLiteral("pocketFocus:") + QString::fromLatin1(id));
    return scene(handle, handle->width() / 2, map->property("py").toDouble() - 12);
  };
  const auto low = [&] { return controller.value(QStringLiteral("pocket.focus_low")); };
  const auto high = [&] { return controller.value(QStringLiteral("pocket.focus_high")); };
  const double spacing = 1.26;
  // Low dragged far right stops a third of an octave under high.
  drag(*panel, grip_of("pocket.focus_low"), scene(map, map->width() - 20, 30));
  require(low() <= high() / spacing + 1e-6, "low cannot pass high: " + std::to_string(low()));
  require(low() > 150.0, "but moves as far as it may");
  // High dragged far left stops above low.
  drag(*panel, grip_of("pocket.focus_high"), scene(map, 45, 30));
  require(high() >= low() * spacing - 1e-6, "high cannot pass low: " + std::to_string(high()));
  // Back out wide: high to the right edge, low to the left.
  drag(*panel, grip_of("pocket.focus_high"), scene(map, map->width() - 16, 30));
  drag(*panel, grip_of("pocket.focus_low"), scene(map, 42, 30));
  require(low() < 40.0 && high() > 1'500.0 && low() < high(),
          "the edges open wide again: " + std::to_string(low()) + " - " + std::to_string(high()));
  // The keyboard nudges a focused handle.
  item(*panel, QStringLiteral("pocketFocus:pocket.focus_high"))->forceActiveFocus();
  const double before = high();
  QTest::keyClick(panel->window.get(), Qt::Key_Left);
  require(high() < before, "Left lowers the high edge");
}

void the_preview_follows_the_laws() {
  InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* map = item(*panel, QStringLiteral("pocketMap"));
  QTest::qWait(300);
  const auto cut = list_of(map, "cut");
  require(list_of(map, "kick").size() == 24 && cut.size() == 24, "the preview fills the map");
  double deepest = 0.0;
  double outside = 0.0;
  for (std::size_t b = 0; b < 24; ++b) {
    const double hz = 30.0 * std::pow(8000.0 / 30.0, static_cast<double>(b) / 23.0);
    deepest = std::max(deepest, cut[b]);
    if (hz > 250.0 * std::sqrt(2.0)) {
      outside = std::max(outside, cut[b]);
    }
  }
  require(deepest > 1.0 && deepest <= 9.0 + 1e-9, "Unmask cuts up to its depth in the preview");
  require(outside == 0.0, "and nothing past half an octave outside the focus");
  require(text_of(*panel, QStringLiteral("pocketCleared")).toDouble() > 0.5,
          "the preview clears some space");
  // Pump cuts the whole focus by its depth.
  controller.set_choice(QStringLiteral("pocket.mode"), 2);
  QCoreApplication::processEvents();
  const auto pump = list_of(map, "cut");
  require(std::fabs(pump[5] - 9.0) < 1e-9 && std::fabs(pump[2] - 9.0) < 1e-9,
          "Pump's preview is the depth across the focus");
  // Depth zero cuts nothing.
  controller.set_value(QStringLiteral("pocket.depth"), 0.0);
  QCoreApplication::processEvents();
  const auto none = list_of(map, "cut");
  require(*std::ranges::max_element(none) == 0.0, "no depth, no cut");
}

void presets_status_and_switches() {
  InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("pocketMonoShade"))->isVisible(),
          "Clean Low End's mono sub is hatched on the map");
  click(*panel, centre(item(*panel, QStringLiteral("pocketPresetNext"))));
  require(controller.preset_name() == QStringLiteral("Sub Only"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("pocketPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Clean Low End"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("pocketMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("pocketPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Gentle Rock"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("pocketPresetName")) == QStringLiteral("Gentle Rock"),
          "a preset shows by name");
  require(!item(*panel, QStringLiteral("pocketMonoShade"))->isVisible(),
          "Gentle Rock keeps a full-range side");

  auto* toggle = item(*panel, QStringLiteral("pocketToggle:pocket.mono_sub"));
  click(*panel, centre(toggle));
  require(controller.choice(QStringLiteral("pocket.mono_sub")) == 1, "a click turns mono sub on");
  require(text_of(*panel, QStringLiteral("pocketReadout")) == QStringLiteral("SUB · MONO SUB"),
          "the display reads back the switch: " +
              text_of(*panel, QStringLiteral("pocketReadout")).toStdString());
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("pocket.mono_sub")) == 0, "Space turns it off");

  auto* lookahead = item(*panel, QStringLiteral("pocketSegment:pocket.lookahead"));
  click(*panel, scene(lookahead, lookahead->width() * 4.5 / 5.0, lookahead->height() / 2));
  require(controller.choice(QStringLiteral("pocket.lookahead")) == 4, "a tile picks 40 ms");
  require(text_of(*panel, QStringLiteral("pocketLatency")) == QStringLiteral("40 ms"),
          "the latency reads out: " +
              text_of(*panel, QStringLiteral("pocketLatency")).toStdString());

  auto* depth = item(*panel, QStringLiteral("pocketKnob:pocket.depth"));
  depth->forceActiveFocus();
  const double before = controller.value(QStringLiteral("pocket.depth"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("pocket.depth")) > before, "Up deepens the cut");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("pocketStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

// A synth part: note events at absolute frames, rendered in 128-frame blocks.
struct Note {
  std::uint64_t frame;
  std::uint64_t length;
  std::uint8_t key;
};

std::vector<engine::StereoFrame> render(const synth::SynthEngine& engine,
                                        const std::vector<double>& values,
                                        const std::vector<Note>& notes, std::size_t frames) {
  auto created = synth::NativeSynthRuntime::create(engine, values, 48'000.0);
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
    const double beats_per_frame = 120.0 / 60.0 / 48'000.0;
    const double beat = static_cast<double>(position) * beats_per_frame;
    const oscillate::native_dsp::Transport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    require(runtime->process(block, block_events, position, 1, transport, {}).status ==
                synth::SynthProcessStatus::rendered,
            "synth renders");
    std::ranges::copy(block, out.begin() + static_cast<std::ptrdiff_t>(position));
  }
  return out;
}

// The real Pocket over a Pulse kick (four on the floor, 120 BPM) and a Prism
// Sub Bass line, with a telemetry frame every 1600 frames (a 30 Hz poll).
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               std::string_view kit, std::size_t stop) {
  const std::size_t frames = stop;
  std::vector<Note> kicks;
  for (std::uint64_t frame = 2'400; frame < frames; frame += 24'000) {
    kicks.push_back({frame, 2'400, 36});
  }
  // A1 off-beat eighths and a held C2, so the bass sits under the kicks.
  std::vector<Note> bass;
  const std::uint8_t keys[4] = {33, 33, 36, 31};
  for (std::uint64_t beat = 0; beat * 24'000 < frames; ++beat) {
    bass.push_back({beat * 24'000, 20'000, keys[beat % 4]});
  }
  const auto kick = render(synth::pulse::engine(),
                           preset_of(*synth::pulse::engine().descriptor, kit), kicks, frames);
  auto bass_audio = render(synth::prism::engine(),
                           preset_of(*synth::prism::engine().descriptor, "Sub Bass"), bass, frames);
  for (auto& frame : bass_audio) {
    frame.left *= 0.7F;
    frame.right *= 0.7F;
  }
  auto created = effects::NativeEffectRuntime::create(pocket::engine(), values, 48'000.0);
  require(created.has_value(), "a Pocket runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames_out;
  std::vector<engine::StereoFrame> block;
  std::vector<engine::StereoFrame> side;
  for (std::size_t position = 0; position < frames; position += 160) {
    const std::size_t size = std::min<std::size_t>(160, frames - position);
    block.assign(bass_audio.begin() + static_cast<std::ptrdiff_t>(position),
                 bass_audio.begin() + static_cast<std::ptrdiff_t>(position + size));
    side.assign(kick.begin() + static_cast<std::ptrdiff_t>(position),
                kick.begin() + static_cast<std::ptrdiff_t>(position + size));
    const oscillate::native_dsp::Transport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, side, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Pocket renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Pocket publishes telemetry");
      frames_out.push_back(frame);
    }
  }
  return frames_out;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("POCKET_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  {
    InMemoryNativeSynthAdapter adapter(preset("Clean Low End"));
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty("lastTouched",
                                                                  QStringLiteral("pocket.depth"));
    QTest::qWait(400);
    const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/pocket_preview.png");
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  struct Live {
    const char* preset;
    const char* kit;
    const char* touched;
    const char* file;
  };
  // Stop a few hundredths of a second after a kick lands, mid-cut.
  for (const Live shot :
       {Live{"Clean Low End", "909 House", "pocket.depth", "/pocket_clean_low_end.png"},
        Live{"808 And Kick", "808 Boom", "pocket.margin", "/pocket_808_and_kick.png"},
        Live{"Classic Pump", "909 House", "pocket.release", "/pocket_classic_pump.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    const auto frames = real_telemetry(values, shot.kit, 4 * 24'000 + 2'400 + 3'200);
    // Play the polls back in real time so the held peaks build as they would.
    for (const auto& frame : frames) {
      adapter.set_telemetry(frame);
      QTest::qWait(33);
    }
    QTest::qWait(60);
    // Hover the band nearest 62 Hz so the readout shows.
    auto* map = item(*panel, QStringLiteral("pocketMap"));
    const double x = map->property("px").toDouble() +
                     map->property("pw").toDouble() * std::log(62.2 / 25.0) / std::log(400.0);
    // Two moves: the same point twice in a row sends no event.
    QTest::mouseMove(panel->window.get(), scene(map, x - 30, map->height() * 0.55));
    QTest::mouseMove(panel->window.get(), scene(map, x, map->height() * 0.6));
    QTest::qWait(60);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    margin_shows_only_for_unmask();
    telemetry_reaches_the_map_meters_and_waves();
    held_peaks_hold_then_fall();
    focus_handles_never_cross();
    the_preview_follows_the_laws();
    presets_status_and_switches();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pocket panel contract passed\n";
  return 0;
}
