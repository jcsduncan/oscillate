#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"
#include "panel_text_checks.hpp"
#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ADR-0043 phase 5: every built-in effect is reachable in the Quick app. It
// is listed in the browser (grouped Effects / Utilities), added from there to
// the selected track's chain, projected as a "BUILT-IN" slot, opened in its
// designed panel, edited live during playback (the stage acknowledges the
// exact revision without a rebuild) and fed with live telemetry.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace effects = oscillate::native_dsp::effects;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

// A fake device the test drives block by block on the UI thread, so a live
// edit can be offered, consumed by the callback and acknowledged in order.
struct Pump {
  std::optional<audio::StereoRenderSource> source;
  audio::TransportCursor* cursor = nullptr;
  std::uint32_t frames = 0;
  float peak = 0.0F;

  void blocks(int count) {
    if (!source || cursor == nullptr) {
      throw std::runtime_error("the fake device has not started");
    }
    std::array<engine::StereoFrame, app::max_playback_buffer_frames> scratch{};
    std::array<float, 2 * std::size_t{app::max_playback_buffer_frames}> samples{};
    for (int block = 0; block < count; ++block) {
      require(audio::render_transport_block(*source, *cursor, true,
                                            std::span(scratch).first(frames),
                                            std::span(samples).first(std::size_t{frames} * 2)) ==
                  audio::BlockResult::rendered,
              "a fake device block failed to render");
      for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * 2; ++i)
        peak = std::max(peak, std::fabs(samples[i]));
    }
  }
};

class PumpOutput final : public app::PlaybackOutput {
public:
  PumpOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor,
             std::shared_ptr<Pump> pump)
      : pump_(std::move(pump)) {
    pump_->source = source;
    pump_->cursor = &cursor;
  }
  // The PlaybackOutput interface's rate and block size.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] oscillate::foundation::Result<void> start(std::uint32_t rate, std::uint32_t frames,
                                                          std::optional<std::uint64_t>) override {
    if (frames == 0 || frames > app::max_playback_buffer_frames)
      return oscillate::foundation::Result<void>::failure(
          {oscillate::foundation::ErrorCode::invalid_argument, "Invalid block size"});
    pump_->frames = frames;
    // The graph activates on its first callback.
    pump_->blocks(8);
    status_.state = audio::OutputState::streaming;
    status_.requested_client_rate = rate;
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override { status_.state = audio::OutputState::stopped; }
  [[nodiscard]] audio::OutputStatus status() const noexcept override { return status_; }

private:
  std::shared_ptr<Pump> pump_;
  audio::OutputStatus status_;
};

QQuickItem* find_item(QQuickItem* root, const QString& name) {
  if (root->objectName() == name)
    return root;
  for (auto* child : root->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

void catalogue_lists_every_engine() {
  QTemporaryDir directory;
  quick::ProjectBridge bridge(directory.path().toStdString());
  const auto engines = bridge.native_effect_engines();
  require(engines.size() == static_cast<qsizetype>(effects::engines().size()),
          "the browser catalogue lists every registered effect");
  int utilities = 0;
  for (const auto& entry : engines) {
    const auto map = entry.toMap();
    require(!map.value("name").toString().isEmpty() && !map.value("detail").toString().isEmpty() &&
                map.value("detail").toString() != QStringLiteral("Built-in audio effect"),
            "every effect has a name and its own one-line description");
    utilities += map.value("utility").toBool() ? 1 : 0;
  }
  require(utilities == 6, "Lens, Loudness, Scope, Tune, Trim and Signal are utilities");
}

// The first continuous, automatable, live parameter: a knob.
const synth::ParameterDescriptor& knob(const effects::EffectEngine& effect) {
  for (const auto& parameter : effect.descriptor->parameters) {
    if (parameter.automatable && !synth::is_discrete(parameter) &&
        parameter.timing == synth::ParameterTiming::live)
      return parameter;
  }
  throw std::runtime_error("effect has no knob");
}

void effect_is_inserted_opened_edited_and_live(const effects::EffectEngine& effect) {
  const std::string id(effect.descriptor->id);
  std::cout << id << ": " << std::flush;
  QTemporaryDir directory;
  require(directory.isValid(), "scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / (id + "-app");
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "project creation failed");
  const auto track = session.value()->instruments().add_synth_track();
  require(track.has_value(), "synth track");
  require(session.value()->midi_clips().add_midi_clip(track.value(), {0}).has_value(), "MIDI clip");
  const auto clip = session.value()->snapshot().tracks.front().midi_clips.front().id;
  for (int beat = 0; beat < 4; ++beat) // A new clip is one bar.
    require(session.value()
                ->notes()
                .add_note_with_duration(clip, {beat * oscillate::timeline::ticks_per_quarter},
                                        {oscillate::timeline::ticks_per_quarter},
                                        static_cast<std::uint8_t>(48 + (beat % 3) * 7))
                .has_value(),
            "note");
  require(session.value()->saving().save().has_value(), "project saved");

  auto pump = std::make_shared<Pump>();
  app::PlaybackOutputFactory output = [pump](audio::StereoRenderSource source,
                                             audio::TransportCursor& cursor) {
    return std::make_unique<PumpOutput>(source, cursor, pump);
  };
  quick::ProjectBridge bridge(directory.path().toStdString(), nullptr, output);
  bridge.set_project(std::move(session).value(), root);
  const auto track_id = QString::fromStdString(track.value().to_string());

  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "DAW shell did not load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "DAW shell has no window");
  window->resize(1600, 1000);

  // Browser: BUILT-IN effects for the selected track.
  window->setProperty("selectedTrack", 0);
  window->setProperty("pluginBrowserOpen", true);
  // Let deferred layout work (the arrangement's viewport refresh) settle
  // before choosing the browser's filters.
  QTest::qWait(0);
  QCoreApplication::processEvents();
  auto* browser = find_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
  require(browser != nullptr && browser->isVisible(), "plugin browser did not open");
  browser->setProperty("showInstruments", false);
  browser->setProperty("showBuiltIns", true);
  QCoreApplication::processEvents();
  // Utilities (meters, tuner, generators) have their own tab; effects are
  // listed under Effects.
  bool utility = false;
  for (const auto& engine : bridge.native_effect_engines()) {
    if (engine.toMap().value("id").toString() == QString::fromStdString(id))
      utility = engine.toMap().value("utility").toBool();
  }
  if (utility) {
    auto* catalogue = browser->findChild<QObject*>(QStringLiteral("browserCatalogue"));
    require(catalogue != nullptr, "the browser has its catalogue");
    QMetaObject::invokeMethod(catalogue, "selectCategory",
                              Q_ARG(QVariant, QVariant(QStringLiteral("Utilities"))));
    QCoreApplication::processEvents();
  }
  const auto entries = browser->property("filteredNativeEntries").toList();
  qsizetype index = -1;
  for (qsizetype i = 0; i < entries.size(); ++i) {
    if (entries[i].toMap().value("id").toString() == QString::fromStdString(id))
      index = i;
  }
  std::string listed;
  for (const auto& entry : entries)
    listed += " " + entry.toMap().value("id").toString().toStdString();
  require(index >= 0, "the browser lists " + id + " (category " +
                          browser->property("category").toString().toStdString() +
                          ", lists:" + listed.substr(0, 200) + ")");
  const auto group = entries[index].toMap().value("group").toString();
  require(group == QStringLiteral("Effects") || group == QStringLiteral("Utilities"),
          "the entry is grouped");
  auto* list = find_item(browser, QStringLiteral("nativeBrowserResults"));
  require(list != nullptr, "built-in results list");
  QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, static_cast<int>(index)),
                            Q_ARG(int, 0));
  QCoreApplication::processEvents();
  auto* row = find_item(list, QStringLiteral("nativeBrowserEntry_") + QString::fromStdString(id));
  require(row != nullptr, "the browser row for " + id + " exists");
  auto* action = find_item(row, QStringLiteral("nativeEntryAction"));
  require(action != nullptr && action->isEnabled(), "the add action is enabled");
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                    action->mapToScene({action->width() / 2, action->height() / 2}).toPoint());
  QCoreApplication::processEvents();

  // The chain projects a BUILT-IN effect slot.
  const auto chain = bridge.insert_chains().at(0).toMap().value("effects").toList();
  require(chain.size() == 1, "the effect was added to the track: " + bridge.status().toStdString());
  const auto slot = chain.front().toMap();
  require(slot.value("nativeKind").toString() == QStringLiteral("effect") &&
              slot.value("engine").toString() == QString::fromStdString(id) &&
              slot.value("type").toString() == QStringLiteral("BUILT-IN") &&
              slot.value("name").toString() ==
                  QString::fromStdString(std::string(effect.descriptor->name)),
          "the slot projects as the named built-in effect");
  require(slot.value("readsSidechain").toBool() == effect.sidechain,
          "the slot says whether it reads a sidechain");
  require(!slot.value("accent").toString().isEmpty() &&
              !slot.value("kindLabel").toString().isEmpty(),
          "the slot carries its strip accent and kind");
  window->setProperty("pluginBrowserOpen", false);
  QCoreApplication::processEvents();

  // The horizontal chain shows the effect as a strip with its key knobs, on
  // the engine's own controller.
  const auto strip_parameters =
      bridge.device_strip_info(QString::fromStdString(id)).value("parameters").toList();
  require(!strip_parameters.isEmpty() && strip_parameters.size() <= 4,
          "the strip names up to four key parameters");
  auto* card = find_item(window->contentItem(), QStringLiteral("insertEffectCard"));
  require(card != nullptr, "the chain shows the effect strip");
  for (const auto& parameter : strip_parameters) {
    require(QTest::qWaitFor(
                [&] {
                  QCoreApplication::processEvents();
                  return find_item(card, QStringLiteral("stripKnob:") + parameter.toString()) !=
                         nullptr;
                },
                2'000),
            "the strip has a knob for " + parameter.toString().toStdString());
  }
  oscillate::test::panel_text::require_formatted_values(card, id + " strip");
  // Each knob's label and value keep to the knob's own slot, elided and
  // clipped there, so no caption runs into its neighbour's.
  constexpr double layout_slack_px = 0.5;
  double previous_right = -std::numeric_limits<double>::infinity();
  for (const auto& parameter : strip_parameters) {
    const auto name = parameter.toString();
    auto* slot_item = find_item(card, QStringLiteral("stripKnob:") + name);
    const double left = slot_item->mapToScene({0.0, 0.0}).x();
    const double right = left + slot_item->width();
    require(left + layout_slack_px >= previous_right,
            "strip knobs sit side by side: " + name.toStdString());
    for (const auto& kind :
         {QStringLiteral("stripKnobLabel:"), QStringLiteral("stripKnobValue:")}) {
      auto* caption = find_item(slot_item, kind + name);
      require(caption != nullptr, "the strip knob has its caption " + (kind + name).toStdString());
      const double from = caption->mapToScene({0.0, 0.0}).x();
      // Inside the slot, and nothing drawn past its own width: the text fits
      // (elided when long) or the caption clips it.
      require(from + layout_slack_px >= left &&
                  from + caption->width() <= right + layout_slack_px &&
                  (caption->clip() || caption->property("contentWidth").toDouble() <=
                                          caption->width() + layout_slack_px),
              "the caption keeps to its slot, elided: " + (kind + name).toStdString() + " \"" +
                  caption->property("text").toString().toStdString() + "\" at " +
                  std::to_string(from) + "+" + std::to_string(caption->width()) + " (content " +
                  std::to_string(caption->property("contentWidth").toDouble()) + ") in " +
                  std::to_string(left) + ".." + std::to_string(right));
    }
    previous_right = right;
  }
  auto* strip_controller = qobject_cast<quick::NativeSynthDeviceController*>(
      bridge.device_strip_controller(track_id, slot.value("slotId").toString()));
  require(strip_controller != nullptr &&
              strip_controller->engine_id() == QString::fromStdString(id),
          "the strip's controller drives this effect");
  // Turn the knob well away from where it is: a stepped control lands on
  // its nearest step, a continuous one on the value itself.
  const auto strip_knob = strip_parameters.front().toString();
  const auto before_strip = bridge.revision();
  const auto start = strip_controller->normalised(strip_knob);
  const auto target = start < 0.5 ? 0.95 : 0.05;
  strip_controller->begin_gesture(strip_knob);
  strip_controller->set_normalised(strip_knob, target);
  strip_controller->end_gesture(strip_knob);
  QCoreApplication::processEvents();
  const auto turned = strip_controller->normalised(strip_knob);
  require(bridge.revision() > before_strip && std::fabs(turned - start) > 1e-6 &&
              std::fabs(turned - target) < std::fabs(start - target),
          "turning a strip knob saves the effect");
  require(find_item(window->contentItem(), QStringLiteral("insertEffectCard")) == card,
          "a value edit keeps the strip (no rebuild mid-gesture)");
  bridge.release_device_strip(track_id, slot.value("slotId").toString());

  // Open its designed panel.
  bridge.open_native_effect_editor(track_id, slot.value("slotId").toString());
  QCoreApplication::processEvents();
  auto* controller =
      qobject_cast<quick::NativeSynthDeviceController*>(bridge.native_effect_controller());
  require(controller != nullptr && controller->engine_id() == QString::fromStdString(id),
          "the effect controller opened");
  const auto panel_name = QString::fromStdString(id) + QStringLiteral("Panel");
  require(QTest::qWaitFor([&] { return find_item(window->contentItem(), panel_name) != nullptr; },
                          3'000),
          "the designed panel loaded in the app");
  require(bridge.native_effect_automation() != nullptr, "track effects have an automation drawer");

  // Play, turn a knob, and see the running stage take it live.
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, 10'000),
          "playback did not start: " + bridge.playback_status().toStdString());
  pump->blocks(32);
  QCoreApplication::processEvents();
  const auto& parameter = knob(effect);
  const auto parameter_id = QString::fromStdString(std::string(parameter.id));
  controller->begin_gesture(parameter_id);
  controller->set_normalised(parameter_id, 0.71);
  controller->end_gesture(parameter_id);
  require(controller->editable(), "the effect is editable while playing");
  require(std::fabs(controller->normalised(parameter_id) - 0.71) < 1e-6, "the knob moved");
  // The saved edit is audible once the stage acknowledges this revision;
  // routed_graph_current means the active graph is the saved revision.
  require(QTest::qWaitFor(
              [&] {
                pump->blocks(2);
                QCoreApplication::processEvents();
                controller->refresh();
                return !controller->pending() && bridge.routed_graph_current();
              },
              5'000),
          "the live edit did not reach the stage: " + bridge.playback_status().toStdString());
  require(!bridge.playback_status().contains(QStringLiteral("restart")),
          "the edit applied without a restart");

  // Telemetry arrives while playing.
  require(QTest::qWaitFor(
              [&] {
                pump->blocks(2);
                const auto live = controller->telemetry();
                return !live.value(QStringLiteral("frame")).toList().isEmpty() ||
                       live.value(QStringLiteral("live")).toBool();
              },
              3'000),
          "the panel received no live telemetry");
  require(pump->peak > 0.0F || effect.generator || id == "trim",
          "the track is audible through the effect");
  // Every value the playing panel shows follows the display formatter.
  oscillate::test::panel_text::require_formatted_values(
      find_item(window->contentItem(), panel_name), id);
  // No text in the playing panel is cut short, drawn over other text or
  // clipped.
  oscillate::test::panel_fit::require_text_fits(find_item(window->contentItem(), panel_name), id);

  if (const auto shots = qEnvironmentVariable("EFFECT_APP_SCREENSHOT_DIR"); !shots.isEmpty()) {
    QDir().mkpath(shots);
    QTest::qWait(400);
    window->grabWindow().save(shots + "/" + QString::fromStdString(id) + "_in_app.png");
  }
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 10'000),
          "playback did not stop");
  bridge.close_native_effect_editor();
  std::cout << "inserted, opened, edited live, telemetry\n";
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    oscillate::test::panel_text::rules_catch_known_mistakes();
    catalogue_lists_every_engine();
    const auto only = qEnvironmentVariable("EFFECT_APP_ONLY");
    for (const auto* effect : effects::engines()) {
      if (!only.isEmpty() &&
          only != QString::fromUtf8(effect->descriptor->id.data(),
                                    static_cast<qsizetype>(effect->descriptor->id.size())))
        continue;
      effect_is_inserted_opened_edited_and_live(*effect);
    }
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "native effect app contract passed\n";
  return 0;
}
