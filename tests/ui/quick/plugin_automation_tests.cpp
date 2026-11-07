#include "../../../apps/daw_quick/src/plugin_automation_controller.hpp"
#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "../../../apps/daw_quick/src/track_automation_controller.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/check.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace app = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

// Opening an editor hashes the plugin binary on a worker before it loads.
constexpr int editor_verify_timeout_ms = 10'000;

void await_editor(quick::ProjectBridge& bridge) {
  require(
      QTest::qWaitFor(
          [&] { return !bridge.vst3_editor()->editor().value(QStringLiteral("pending")).toBool(); },
          editor_verify_timeout_ms),
      "VST3 editor did not finish verifying its plugin binary");
}

class Directory {
public:
  Directory() {
    std::string pattern =
        (fs::temp_directory_path() / "oscillate-plugin-automation-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "temporary directory failed");
    root_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  Directory(Directory&&) = delete;
  Directory& operator=(Directory&&) = delete;
  [[nodiscard]] const fs::path& root() const { return root_; }

private:
  fs::path root_;
};

// Renders one block at start so the routed graph acknowledges activation,
// then holds the stream without a device.
class RenderOnStartOutput final : public app::PlaybackOutput {
public:
  RenderOnStartOutput(oscillate::audio_io::StereoRenderSource source,
                      oscillate::audio_io::TransportCursor& cursor)
      : source_(source), cursor_(cursor) {}

  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] oscillate::foundation::Result<void>
  start(std::uint32_t, std::uint32_t requested_frames, std::optional<std::uint64_t>) override {
    std::array<oscillate::engine::StereoFrame, app::max_playback_buffer_frames> frames{};
    std::array<float, 2 * app::max_playback_buffer_frames> samples{};
    if (requested_frames > frames.size() ||
        oscillate::audio_io::render_transport_block(
            source_, cursor_, true, std::span(frames).first(requested_frames),
            std::span(samples).first(static_cast<std::size_t>(requested_frames) * 2)) !=
            oscillate::audio_io::BlockResult::rendered)
      return oscillate::foundation::Result<void>::failure(
          {oscillate::foundation::ErrorCode::unavailable, "test output could not render"});
    state_.store(oscillate::audio_io::OutputState::streaming, std::memory_order_release);
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override {
    state_.store(oscillate::audio_io::OutputState::stopped, std::memory_order_release);
  }
  [[nodiscard]] oscillate::audio_io::OutputStatus status() const noexcept override {
    oscillate::audio_io::OutputStatus result;
    result.state = state_.load(std::memory_order_acquire);
    return result;
  }

private:
  oscillate::audio_io::StereoRenderSource source_;
  oscillate::audio_io::TransportCursor& cursor_;
  std::atomic<oscillate::audio_io::OutputState> state_{oscillate::audio_io::OutputState::stopped};
};

std::unique_ptr<quick::ProjectBridge> held_bridge(const fs::path& root) {
  return std::make_unique<quick::ProjectBridge>(
      root, nullptr,
      [](oscillate::audio_io::StereoRenderSource source,
         oscillate::audio_io::TransportCursor& cursor) {
        return std::make_unique<RenderOnStartOutput>(source, cursor);
      },
      [] {
        return oscillate::foundation::Result<
            std::vector<oscillate::audio_io::PipeWireSink>>::success({});
      });
}

void write_wav(const fs::path& path) {
  constexpr unsigned char wav[]{'R',  'I',  'F', 'F', 40, 0,    0, 0, 'W', 'A',  'V', 'E',
                                'f',  'm',  't', ' ', 16, 0,    0, 0, 1,   0,    1,   0,
                                0x80, 0xbb, 0,   0,   0,  0x77, 1, 0, 2,   0,    16,  0,
                                'd',  'a',  't', 'a', 4,  0,    0, 0, 0,   0x20, 0,   0x20};
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav), sizeof(wav));
  require(output.good(), "WAV fixture write failed");
}

project::TrackId import_audio_track(app::ProjectSession& session, const fs::path& directory) {
  const auto wav = directory / "source.wav";
  write_wav(wav);
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "WAV fixture open failed");
  const auto imported = session.imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "WAV fixture import failed");
  return imported.value();
}

QQuickItem* find_item(QQuickItem* root, const QString& name) {
  if (root->objectName() == name)
    return root;
  for (auto* child : root->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

void collect_items(QQuickItem* root, const QString& name, std::vector<QQuickItem*>& found) {
  if (root->objectName() == name && root->isVisible())
    found.push_back(root);
  for (auto* child : root->childItems())
    collect_items(child, name, found);
}

std::size_t visible_count(QQuickWindow& window, const QString& name) {
  std::vector<QQuickItem*> found;
  collect_items(window.contentItem(), name, found);
  return found.size();
}

QQuickItem* require_item(QQuickWindow& window, const QString& name) {
  QQuickItem* item = nullptr;
  require(QTest::qWaitFor(
              [&] {
                item = find_item(window.contentItem(), name);
                return item != nullptr && item->isVisible();
              },
              5'000),
          "Quick item is not visible: " + name.toStdString());
  return item;
}

void activate(QQuickItem* item) {
  require(item->isEnabled(), "Quick action is disabled: " + item->objectName().toStdString());
  require(QMetaObject::invokeMethod(item, "activate"),
          "Quick action has no activate(): " + item->objectName().toStdString());
  QCoreApplication::processEvents();
}

QVariantMap parameter_entry(const QVariantMap& view, quint32 id) {
  for (const auto& item : view.value(QStringLiteral("parameters")).toList()) {
    const auto map = item.toMap();
    if (map.value(QStringLiteral("id")).toUInt() == id)
      return map;
  }
  return {};
}

QVariantList lane_points(const QVariantMap& view) {
  return view.value(QStringLiteral("lane")).toMap().value(QStringLiteral("points")).toList();
}

QString render_state(const QVariantMap& view) {
  return view.value(QStringLiteral("renderState")).toString();
}

std::unique_ptr<QQmlApplicationEngine> load_shell(quick::ProjectBridge& bridge) {
  auto engine = std::make_unique<QQmlApplicationEngine>();
  engine->rootContext()->setContextProperty("projectBridge", &bridge);
  engine->load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!engine->rootObjects().isEmpty(), "Quick shell failed to load");
  QCoreApplication::processEvents();
  return engine;
}

QQuickWindow& shell_window(QQmlApplicationEngine& engine) {
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  require(window != nullptr, "Quick shell window missing");
  return *window;
}

const project::AutomationLane& only_lane(const project::Snapshot& snapshot) {
  require(snapshot.tracks.size() == 1 && snapshot.tracks.front().automation.size() == 1,
          "expected exactly one saved automation lane");
  return snapshot.tracks.front().automation.front();
}

const project::AutomationLane* instrument_lane(const project::Snapshot& snapshot,
                                               project::TrackId track,
                                               project::PluginInstanceId plugin) {
  const auto owner = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  if (owner == snapshot.tracks.end())
    return nullptr;
  const auto lane = std::ranges::find_if(owner->automation, [plugin](const auto& item) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&item.target);
    return target && target->plugin == plugin && target->parameter_id == 1;
  });
  return lane == owner->automation.end() ? nullptr : &*lane;
}

void effect_parameter_lane_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_CONNECTED_BUNDLE);
  require(scanned.ok(), "connected effect fixture did not scan");
  const auto klass = std::ranges::find_if(scanned.metadata.classes, [](const auto& candidate) {
    return candidate.name == "Connected Gain" && candidate.category == "Audio Module Class";
  });
  require(klass != scanned.metadata.classes.end(), "connected effect class missing");
  const auto root = directory.root() / "effect-song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "effect project creation failed");
  auto session = std::move(created).value();
  const auto track = import_audio_track(*session, directory.root());
  const auto plugin = session->effects().insert_vst3_plugin(
      track, 0,
      {klass->id, klass->vendor, klass->name, klass->version,
       scanned.binary.canonical_bundle.string(), scanned.binary.sha256});
  require(plugin.has_value(), "connected effect insertion failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  const auto saved = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  require(session->effects().set_vst3_editor_state(session->snapshot().revision, track, slot, saved,
                                                   {}, {{101, 0.25}}) &&
              session->saving().save(),
          "saved effect parameter baseline failed");
  const auto track_id = QString::fromStdString(track.to_string());
  const auto slot_id = QString::fromStdString(slot.to_string());

  QString lane_id;
  QString kept_point;
  {
    auto bridge = held_bridge(directory.root());
    bridge->set_project(std::move(session), root);
    bridge->vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_CONNECTED_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge->vst3_browser()->busy(); }, 15'000),
            "connected effect catalogue did not finish scanning");
    auto engine = load_shell(*bridge);
    auto& window = shell_window(*engine);
    auto* controller =
        qobject_cast<quick::PluginAutomationController*>(bridge->plugin_automation());
    require(controller != nullptr, "plugin automation controller missing");
    bridge->vst3_editor()->open_vst3_insert_editor(track_id, slot_id);
    await_editor(*bridge);
    auto view = controller->view();
    const auto gain = parameter_entry(view, 101);
    require(view.value(QStringLiteral("active")).toBool() &&
                view.value(QStringLiteral("role")) == QStringLiteral("effect") &&
                gain.value(QStringLiteral("name")) == QStringLiteral("Gain") &&
                gain.value(QStringLiteral("verified")).toBool() &&
                gain.value(QStringLiteral("automatable")).toBool() &&
                gain.value(QStringLiteral("baseline")).toDouble() == 0.25 &&
                !gain.value(QStringLiteral("lanePresent")).toBool(),
            "verified effect parameter was not offered by name with its saved baseline");

    const auto initial_revision = bridge->revision();
    controller->automate(999);
    controller->set_point(0, 0.5);
    require(bridge->revision() == initial_revision,
            "unknown parameter or unselected lane changed the project");
    activate(require_item(window, QStringLiteral("vst3ParameterAutomate_101")));
    view = controller->view();
    require(bridge->revision() == initial_revision + 1 &&
                view.value(QStringLiteral("parameterSelected")).toBool() &&
                lane_points(view).size() == 1 &&
                lane_points(view).front().toMap().value(QStringLiteral("value")).toDouble() ==
                    0.25 &&
                lane_points(view).front().toMap().value(QStringLiteral("bar")).toInt() == 0 &&
                render_state(view) == QStringLiteral("stopped"),
            "parameter action did not create one lane from the saved baseline");
    auto* status = require_item(window, QStringLiteral("pluginAutomationStatus"));
    require(require_item(window, QStringLiteral("pluginAutomationEditor"))->isVisible() &&
                status->property("text").toString().contains(
                    QStringLiteral("applies when playback starts")),
            "lane editor or stopped render status not visible");

    controller->set_point(1, 1.5);
    require(bridge->revision() == initial_revision + 1, "invalid point value changed the project");
    require_item(window, QStringLiteral("pluginAutomationBarInput"))
        ->setProperty("text", QStringLiteral("3"));
    require_item(window, QStringLiteral("pluginAutomationValueInput"))
        ->setProperty("text", QStringLiteral("0.8"));
    activate(require_item(window, QStringLiteral("pluginAutomationAddPoint")));
    view = controller->view();
    require(lane_points(view).size() == 2 &&
                lane_points(view).back().toMap().value(QStringLiteral("bar")).toInt() == 2 &&
                QTest::qWaitFor(
                    [&] {
                      return visible_count(window, QStringLiteral("pluginAutomationPointRow")) == 2;
                    },
                    5'000),
            "numeric point entry did not add a visible bar-3 point");
    const auto first_point =
        lane_points(view).front().toMap().value(QStringLiteral("id")).toString();
    const auto curve_revision = bridge->revision();
    activate(require_item(window, QStringLiteral("automationShapeNext")));
    auto* curve_controller =
        qobject_cast<quick::TrackAutomationController*>(bridge->track_automation());
    const auto curve_lane_id = controller->view()
                                   .value(QStringLiteral("lane"))
                                   .toMap()
                                   .value(QStringLiteral("laneId"))
                                   .toString();
    require(bridge->revision() == curve_revision + 1 && curve_controller != nullptr &&
                curve_controller->segment_curve(curve_lane_id, first_point, bridge->revision())
                        .value(QStringLiteral("shapeIndex"))
                        .toInt() == 2,
            "plugin automation segment control did not save its shape");
    controller->set_value(first_point, 0.3);
    require(lane_points(controller->view()).front().toMap().value(QStringLiteral("value")) == 0.3,
            "numeric point value edit failed");

    bridge->toggle_playback();
    require(QTest::qWaitFor(
                [&] {
                  return bridge->playback_active() &&
                         render_state(controller->view()) == QStringLiteral("active");
                },
                15'000),
            "verified lane did not report active once the live graph matched: " +
                bridge->playback_status().toStdString() + " / " +
                render_state(controller->view()).toStdString());
    controller->set_value(first_point, 0.4);
    require(QTest::qWaitFor(
                [&] {
                  return render_state(controller->view()) == QStringLiteral("pending") &&
                         status->property("text").toString().contains(
                             QStringLiteral("Stop and Play to apply"));
                },
                5'000),
            "lane edit during playback was presented as live");
    bridge->toggle_playback();
    require(QTest::qWaitFor([&] { return !bridge->playback_active(); }, 15'000) &&
                QTest::qWaitFor(
                    [&] { return render_state(controller->view()) == QStringLiteral("stopped"); },
                    5'000),
            "stopped transport did not return the lane to saved status");

    std::vector<QQuickItem*> removes;
    collect_items(window.contentItem(), QStringLiteral("pluginAutomationRemovePoint"), removes);
    require(removes.size() == 2, "visible point removal actions missing");
    std::ranges::sort(removes, {},
                      [](const QQuickItem* item) { return item->mapToScene({0, 0}).y(); });
    activate(removes.back());
    view = controller->view();
    require(lane_points(view).size() == 1 &&
                lane_points(view).front().toMap().value(QStringLiteral("id")) == first_point,
            "visible point removal did not remove the selected point");
    lane_id = view.value(QStringLiteral("lane")).toMap().value(QStringLiteral("laneId")).toString();
    kept_point = first_point;
    activate(require_item(window, QStringLiteral("pluginAutomationBack")));
    require(!controller->view().value(QStringLiteral("parameterSelected")).toBool(),
            "back action did not return to parameters");
    bridge->save();
    bridge->vst3_editor()->discard_vst3_insert_editor();
  }

  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "effect automation project did not reopen");
  const auto& lane = only_lane(reopened.value()->snapshot());
  const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target);
  require(QString::fromStdString(lane.id.to_string()) == lane_id && target != nullptr &&
              target->plugin == plugin.value() && target->parameter_id == 101 &&
              lane.domain.default_value == 0.25 && lane.points.size() == 1 &&
              lane.points.front().to_next == project::AutomationShape::quadratic &&
              QString::fromStdString(lane.points.front().id.to_string()) == kept_point &&
              lane.points.front().value == 0.4,
          "effect lane identity, baseline or point ID did not survive reopen");
  {
    auto bridge = held_bridge(directory.root());
    bridge->set_project(std::move(reopened).value(), root);
    bridge->vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_CONNECTED_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge->vst3_browser()->busy(); }, 15'000),
            "reopened catalogue did not finish scanning");
    auto engine = load_shell(*bridge);
    auto& window = shell_window(*engine);
    auto* controller =
        qobject_cast<quick::PluginAutomationController*>(bridge->plugin_automation());
    bridge->vst3_editor()->open_vst3_insert_editor(track_id, slot_id);
    await_editor(*bridge);
    auto* action = require_item(window, QStringLiteral("vst3ParameterAutomate_101"));
    require(QTest::qWaitFor(
                [&] {
                  return action->childItems().size() > 0 &&
                         action->childItems().front()->property("text").toString() ==
                             QStringLiteral("LANE 1");
                },
                5'000),
            "reopened lane is not visible on its parameter action");
    const auto revision = bridge->revision();
    activate(action);
    const auto view = controller->view();
    require(bridge->revision() == revision &&
                lane_points(view).front().toMap().value(QStringLiteral("id")).toString() ==
                    kept_point,
            "existing lane was not selected without a project edit");
    activate(require_item(window, QStringLiteral("pluginAutomationRemoveLane")));
    require(!controller->view()
                    .value(QStringLiteral("lane"))
                    .toMap()
                    .value(QStringLiteral("present"))
                    .toBool() &&
                !parameter_entry(controller->view(), 101)
                     .value(QStringLiteral("lanePresent"))
                     .toBool() &&
                bridge->revision() == revision + 1,
            "lane removal was not one project edit");
    bridge->undo();
    require(lane_points(controller->view()).size() == 1, "lane removal did not undo");
    bridge->vst3_editor()->discard_vst3_insert_editor();
  }
}

void instrument_and_missing_reference_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scanned.ok() && !scanned.metadata.classes.empty(), "instrument fixture did not scan");
  const auto& klass = scanned.metadata.classes.front();
  const auto root = directory.root() / "instrument-song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "instrument project creation failed");
  auto session = std::move(created).value();
  const auto track = session->instruments().create_vst3_instrument_track(
      {klass.id, klass.vendor, klass.name, klass.version, scanned.binary.canonical_bundle.string(),
       scanned.binary.sha256});
  require(track.has_value(), "instrument source creation failed");
  const auto source =
      std::get<project::Vst3PluginInstance>(session->snapshot().tracks.front().instrument_source);
  require(session->instruments()
              .set_vst3_instrument_state(session->snapshot().revision, track.value(), source, {},
                                         {{1, 0.65}})
              .has_value(),
          "instrument parameter baseline fixture failed");
  const auto saved_source =
      std::get<project::Vst3PluginInstance>(session->snapshot().tracks.front().instrument_source);
  const auto missing = session->effects().insert_vst3_plugin(
      track.value(), 0,
      {std::string(32, 'e'), "Fixture", "Missing Effect", "1.0",
       (directory.root() / "missing.vst3").string(), std::string(64, 'e')});
  require(missing.has_value(), "missing effect placeholder insertion failed");
  const auto missing_slot = session->snapshot().tracks.front().effects.front().id;
  const auto missing_saved = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  require(session->effects().set_vst3_editor_state(session->snapshot().revision, track.value(),
                                                   missing_slot, missing_saved, {}, {{7, 0.4}}) &&
              session->saving().save(),
          "missing effect reference fixture failed");
  const auto track_id = QString::fromStdString(track.value().to_string());

  QString source_point;
  {
    auto bridge = held_bridge(directory.root());
    bridge->set_project(std::move(session), root);
    auto engine = load_shell(*bridge);
    auto& window = shell_window(*engine);
    auto* controller =
        qobject_cast<quick::PluginAutomationController*>(bridge->plugin_automation());
    auto* chain = find_item(window.contentItem(), QStringLiteral("insertChainPanel"));
    require(chain != nullptr && chain->property("pathId").toString() == track_id,
            "instrument track chain is not selected");
    auto* open_source = require_item(window, QStringLiteral("instrumentSourceParameters"));
    require(QMetaObject::invokeMethod(open_source, "triggered"), "source action failed");
    QCoreApplication::processEvents();
    auto view = controller->view();
    const auto level = parameter_entry(view, 1);
    require(view.value(QStringLiteral("role")) == QStringLiteral("source") &&
                !view.value(QStringLiteral("verified")).toBool() &&
                level.value(QStringLiteral("name")) == QStringLiteral("Parameter 1") &&
                level.value(QStringLiteral("baseline")).toDouble() == 0.65 &&
                level.value(QStringLiteral("canAutomate")).toBool(),
            "instrument source reference was not identified by stable ID");
    const auto revision = bridge->revision();
    activate(require_item(window, QStringLiteral("vst3ParameterAutomate_1")));
    require_item(window, QStringLiteral("pluginAutomationBarInput"))
        ->setProperty("text", QStringLiteral("2"));
    require_item(window, QStringLiteral("pluginAutomationValueInput"))
        ->setProperty("text", QStringLiteral("0.9"));
    activate(require_item(window, QStringLiteral("pluginAutomationAddPoint")));
    view = controller->view();
    require(bridge->revision() == revision + 2 && lane_points(view).size() == 2 &&
                render_state(view) == QStringLiteral("unverified") &&
                require_item(window, QStringLiteral("pluginAutomationStatus"))
                    ->property("text")
                    .toString()
                    .contains(QStringLiteral("not shown as active")),
            "unverified instrument lane was presented as active");
    const auto removed = lane_points(view).front().toMap().value(QStringLiteral("id")).toString();
    controller->remove_point(removed);
    view = controller->view();
    require(lane_points(view).size() == 1, "instrument point removal failed");
    source_point = lane_points(view).front().toMap().value(QStringLiteral("id")).toString();

    bridge->vst3_editor()->open_vst3_insert_editor(
        track_id, QString::fromStdString(missing_slot.to_string()));
    await_editor(*bridge);
    view = controller->view();
    require(!view.value(QStringLiteral("verified")).toBool() &&
                parameter_entry(view, 7).value(QStringLiteral("name")) ==
                    QStringLiteral("Parameter 7"),
            "absent plugin reference was not retained by stable ID");
    activate(require_item(window, QStringLiteral("vst3ParameterAutomate_7")));
    require(render_state(controller->view()) == QStringLiteral("unverified"),
            "absent plugin lane was presented as renderable");
    bridge->save();
    bridge->vst3_editor()->discard_vst3_insert_editor();
  }

  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "instrument automation project did not reopen");
  const auto& saved_track = reopened.value()->snapshot().tracks.front();
  const auto* reopened_source =
      std::get_if<project::Vst3PluginInstance>(&saved_track.instrument_source);
  const auto source_lane = std::ranges::find_if(saved_track.automation, [&](const auto& lane) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target);
    return target && target->plugin == saved_source.id && target->parameter_id == 1;
  });
  const auto missing_lane = std::ranges::find_if(saved_track.automation, [&](const auto& lane) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target);
    return target && target->plugin == missing.value() && target->parameter_id == 7;
  });
  require(
      reopened_source != nullptr && *reopened_source == saved_source &&
          source_lane != saved_track.automation.end() &&
          source_lane->domain.default_value == 0.65 && source_lane->points.size() == 1 &&
          QString::fromStdString(source_lane->points.front().id.to_string()) == source_point &&
          missing_lane != saved_track.automation.end() &&
          missing_lane->domain.default_value == 0.4 &&
          std::get<project::Vst3PluginInstance>(saved_track.effects.front().processor).parameters ==
              std::vector<project::Vst3ParameterReference>{{7, 0.4}},
      "instrument source, missing reference or their lanes did not survive reopen");
}

void verified_instrument_parameter_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scanned.ok() && !scanned.metadata.classes.empty(), "instrument fixture did not scan");
  const auto& klass = scanned.metadata.classes.front();
  const auto root = directory.root() / "verified-instrument";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "verified instrument project creation failed");
  auto session = std::move(created).value();
  const auto track = session->instruments().create_vst3_instrument_track(
      {klass.id, klass.vendor, klass.name, klass.version, scanned.binary.canonical_bundle.string(),
       scanned.binary.sha256});
  require(track.has_value(), "verified instrument track creation failed");
  const auto track_id = QString::fromStdString(track.value().to_string());
  {
    auto bridge = held_bridge(directory.root());
    auto* project_session = session.get();
    bridge->set_project(std::move(session), root);
    bridge->vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_INSTRUMENT_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge->vst3_browser()->busy(); }, 15'000),
            "verified instrument scan did not complete");
    auto engine = load_shell(*bridge);
    auto& window = shell_window(*engine);
    auto* controller =
        qobject_cast<quick::PluginAutomationController*>(bridge->plugin_automation());
    bridge->vst3_editor()->open_vst3_instrument_editor(track_id);
    await_editor(*bridge);
    auto view = controller->view();
    require(view.value(QStringLiteral("verified")).toBool() &&
                parameter_entry(view, 1).value(QStringLiteral("name")) == QStringLiteral("Level") &&
                !parameter_entry(view, 1).value(QStringLiteral("canAutomate")).toBool(),
            "verified instrument exposed an invented parameter baseline");
    bridge->vst3_editor()->set_vst3_insert_parameter(1, 0.65);
    view = controller->view();
    const auto& source = std::get<project::Vst3PluginInstance>(
        project_session->snapshot().tracks.front().instrument_source);
    const auto plugin_id = source.id;
    require(source.parameters == std::vector<project::Vst3ParameterReference>{{1, 0.65}} &&
                !source.component_state.empty() &&
                parameter_entry(view, 1).value(QStringLiteral("baseline")).toDouble() == 0.65 &&
                parameter_entry(view, 1).value(QStringLiteral("canAutomate")).toBool(),
            "instrument value was not captured as a saved automation baseline");
    activate(require_item(window, QStringLiteral("vst3ParameterAutomate_1")));
    const auto* lane = instrument_lane(project_session->snapshot(), track.value(), plugin_id);
    require(render_state(controller->view()) == QStringLiteral("stopped") && lane != nullptr &&
                lane->domain.default_value == 0.65,
            "verified instrument lane did not start at its saved value");
    bridge->save();
    bridge->vst3_editor()->discard_vst3_insert_editor();
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "verified instrument project did not reopen");
  const auto& source = std::get<project::Vst3PluginInstance>(
      reopened.value()->snapshot().tracks.front().instrument_source);
  const auto* lane = instrument_lane(reopened.value()->snapshot(), track.value(), source.id);
  require(source.parameters == std::vector<project::Vst3ParameterReference>{{1, 0.65}} &&
              !source.component_state.empty() && lane != nullptr &&
              lane->domain.default_value == 0.65,
          "verified instrument value or automation lane did not persist");
}

fs::path lane_effect_bundle() { return fs::path(VST3_LANE_EFFECT_BUNDLE); }

fs::path lane_instrument_bundle() { return fs::path(VST3_LANE_INSTRUMENT_BUNDLE); }

// QTest has no QWindow string overload; send printable ASCII one key at a time.
void type_text(QQuickWindow& window, const QString& text) {
  for (const auto character : text)
    QTest::keyClick(&window, character.toLatin1());
}

std::vector<quint32> projected_ids(const QVariantMap& view) {
  std::vector<quint32> ids;
  for (const auto& item : view.value(QStringLiteral("parameters")).toList())
    ids.push_back(item.toMap().value(QStringLiteral("id")).toUInt());
  return ids;
}

const project::Vst3ParameterReference* saved_reference(const project::Vst3PluginInstance& plugin,
                                                       std::uint32_t id) {
  const auto found = std::ranges::find(plugin.parameters, id, &project::Vst3ParameterReference::id);
  return found == plugin.parameters.end() ? nullptr : &*found;
}

// Drives the generic editor's search, overflow status and keyboard path for a
// plugin with more than one page of parameters. `last_id` sits beyond the
// first 64 rows; `saved` reads that plugin back from the current snapshot.
// The lane instrument fixture flushes only parameter 1 while suspended, so
// for it the found row must reach the existing command and be refused
// without a revision; the bridge then closes the editor and `reopen` restores
// it.
void browse_large_plugin(quick::ProjectBridge& bridge, QQuickWindow& window,
                         quick::PluginAutomationController& controller, quint32 total,
                         quint32 last_id,
                         const std::function<const project::Vst3PluginInstance&()>& saved,
                         bool fixture_flushes_found_value, const std::function<void()>& reopen) {
  window.resize(1920, 1080);
  window.show();
  window.requestActivate();
  QCoreApplication::processEvents();
  auto view = controller.view();
  auto* status = require_item(window, QStringLiteral("vst3ParameterResultStatus"));
  require(view.value(QStringLiteral("parameterCount")).toUInt() == total &&
              projected_ids(view).size() == 64 &&
              view.value(QStringLiteral("resultsLimited")).toBool() &&
              parameter_entry(view, last_id).isEmpty() &&
              status->property("text").toString() ==
                  QStringLiteral("Showing first 64 of %1 parameters. Search by name or ID to find "
                                 "the rest")
                      .arg(total),
          "large plugin was truncated without an explicit bounded-result status");

  auto* search = require_item(window, QStringLiteral("vst3ParameterSearch"));
  search->forceActiveFocus();
  type_text(window, QString::number(last_id));
  view = controller.view();
  require(search->property("text").toString() == QString::number(last_id) &&
              projected_ids(view) == std::vector<quint32>{last_id} &&
              view.value(QStringLiteral("matchCount")).toInt() == 1 &&
              !view.value(QStringLiteral("resultsLimited")).toBool() &&
              parameter_entry(view, last_id).value(QStringLiteral("name")) ==
                  QStringLiteral("Lane") &&
              parameter_entry(view, last_id).value(QStringLiteral("verified")).toBool(),
          "ID search did not find the parameter beyond the first page");

  // Down from the search field reaches the row's value; Return saves through
  // the existing parameter command as one revision.
  QTest::keyClick(&window, Qt::Key_Down);
  auto* value = require_item(window, QStringLiteral("vst3ParameterValue_%1").arg(last_id));
  require(value->hasActiveFocus(), "keyboard did not move from search to the found parameter");
  const auto revision = bridge.revision();
  type_text(window, QStringLiteral("0.4"));
  QTest::keyClick(&window, Qt::Key_Return);
  const auto* reference = saved_reference(saved(), last_id);
  view = controller.view();
  if (!fixture_flushes_found_value) {
    require(
        bridge.revision() == revision && reference == nullptr &&
            bridge.property("status").toString().contains(QStringLiteral("could not be applied")) &&
            !view.value(QStringLiteral("active")).toBool(),
        "refused value for the found parameter changed the project or was not reported");
    reopen();
    require(controller.view().value(QStringLiteral("filter")).toString().isEmpty() &&
                require_item(window, QStringLiteral("vst3ParameterSearch"))
                    ->property("text")
                    .toString()
                    .isEmpty(),
            "reopened editor kept a stale parameter search");
  } else {
    require(bridge.revision() == revision + 1 && reference != nullptr &&
                reference->normalized_value == 0.4 &&
                parameter_entry(view, last_id).value(QStringLiteral("baseline")).toDouble() == 0.4,
            "found parameter value was not saved by stable ID");
    // The save republishes rows; keyboard focus follows the stable ID.
    require(QTest::qWaitFor(
                [&] {
                  auto* restored = find_item(window.contentItem(),
                                             QStringLiteral("vst3ParameterValue_%1").arg(last_id));
                  return restored != nullptr && restored->hasActiveFocus();
                },
                2'000),
            "keyboard focus did not return to the saved parameter");
  }

  // Name search is case-insensitive; Escape clears it and restores the page.
  search->forceActiveFocus();
  search->setProperty("text", QStringLiteral("LANE"));
  view = controller.view();
  // Every fixture parameter after the first is named "Lane": 64 matches fit
  // one page, 65 report the limit instead of dropping the last silently.
  const auto lanes = projected_ids(view);
  const bool fits = total - 1 <= 64;
  require(view.value(QStringLiteral("matchCount")).toUInt() == total - 1 &&
              std::ranges::find(lanes, 1U) == lanes.end() &&
              (std::ranges::find(lanes, last_id) != lanes.end()) == fits &&
              view.value(QStringLiteral("resultsLimited")).toBool() == !fits &&
              (fits || status->property("text").toString() ==
                           QStringLiteral("Showing first 64 of %1 matches for \u201cLANE\u201d. "
                                          "Refine the search")
                               .arg(total - 1)),
          "name search did not reach or explicitly limit the matching parameters");
  search->setProperty("text", QStringLiteral("no such control"));
  view = controller.view();
  require(projected_ids(view).empty() &&
              require_item(window, QStringLiteral("vst3ParameterEmpty"))->isVisible() &&
              status->property("text").toString().startsWith(
                  QStringLiteral("No parameter name or ID matches")),
          "empty search result was not described");
  search->forceActiveFocus();
  QTest::keyClick(&window, Qt::Key_Escape);
  view = controller.view();
  require(search->property("text").toString().isEmpty() &&
              view.value(QStringLiteral("filter")).toString().isEmpty() &&
              view.value(QStringLiteral("resultsLimited")).toBool(),
          "Escape did not clear the parameter search");

  // Ctrl+F returns to search; a lane created from a found row keeps its ID
  // and the search survives the trip into the lane editor and back. Filtering
  // recycles delegates, so resolve a row that is on the restored first page.
  require_item(window, QStringLiteral("vst3ParameterValue_1"))->forceActiveFocus();
  QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
  require(search->hasActiveFocus(), "Ctrl+F did not focus the parameter search");
  type_text(window, QString::number(last_id));
  if (!fixture_flushes_found_value)
    return;
  activate(require_item(window, QStringLiteral("vst3ParameterAutomate_%1").arg(last_id)));
  view = controller.view();
  require(
      view.value(QStringLiteral("parameterSelected")).toBool() &&
          view.value(QStringLiteral("parameter")).toMap().value(QStringLiteral("id")).toUInt() ==
              last_id &&
          lane_points(view).size() == 1 &&
          lane_points(view).front().toMap().value(QStringLiteral("value")).toDouble() == 0.4,
      "lane from a searched parameter did not keep its stable ID and baseline");
  activate(require_item(window, QStringLiteral("pluginAutomationBack")));
  view = controller.view();
  require(view.value(QStringLiteral("filter")).toString() == QString::number(last_id) &&
              require_item(window, QStringLiteral("vst3ParameterRow_%1").arg(last_id))->isVisible(),
          "returning from the lane editor lost the parameter search");
}

void large_effect_parameter_browser_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto bundle = lane_effect_bundle();
  const auto scanned = scanner.scan(bundle);
  require(scanned.ok(), "65-parameter effect fixture did not scan");
  const auto klass = std::ranges::find_if(scanned.metadata.classes, [](const auto& candidate) {
    return candidate.name == "Lane Effect" && candidate.category == "Audio Module Class";
  });
  require(klass != scanned.metadata.classes.end(), "65-parameter effect class missing");
  const auto root = directory.root() / "large-effect";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "large effect project creation failed");
  auto session = std::move(created).value();
  const auto track = import_audio_track(*session, directory.root());
  require(
      session->effects()
              .insert_vst3_plugin(track, 0,
                                  {klass->id, klass->vendor, klass->name, klass->version,
                                   scanned.binary.canonical_bundle.string(), scanned.binary.sha256})
              .has_value() &&
          session->saving().save(),
      "large effect insertion failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  auto* project_session = session.get();
  auto bridge = held_bridge(directory.root());
  bridge->set_project(std::move(session), root);
  bridge->vst3_browser()->add_vst3_search_path(QString::fromStdString(bundle.string()));
  require(QTest::qWaitFor([&] { return !bridge->vst3_browser()->busy(); }, 15'000),
          "large effect catalogue did not finish scanning");
  auto engine = load_shell(*bridge);
  auto& window = shell_window(*engine);
  auto* controller = qobject_cast<quick::PluginAutomationController*>(bridge->plugin_automation());
  require(controller != nullptr, "plugin automation controller missing");
  bridge->vst3_editor()->open_vst3_insert_editor(QString::fromStdString(track.to_string()),
                                                 QString::fromStdString(slot.to_string()));
  await_editor(*bridge);
  require(controller->view().value(QStringLiteral("role")) == QStringLiteral("effect"),
          "large effect editor did not select the effect");
  browse_large_plugin(
      *bridge, window, *controller, 65, 65,
      [project_session]() -> const project::Vst3PluginInstance& {
        return std::get<project::Vst3PluginInstance>(
            project_session->snapshot().tracks.front().effects.front().processor);
      },
      true, [] {});
  bridge->vst3_editor()->discard_vst3_insert_editor();
}

void large_instrument_parameter_browser_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto bundle = lane_instrument_bundle();
  const auto scanned = scanner.scan(bundle);
  require(scanned.ok() && !scanned.metadata.classes.empty(),
          "66-parameter instrument fixture did not scan");
  const auto& klass = scanned.metadata.classes.front();
  const auto root = directory.root() / "large-instrument";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "large instrument project creation failed");
  auto session = std::move(created).value();
  const auto track = session->instruments().create_vst3_instrument_track(
      {klass.id, klass.vendor, klass.name, klass.version, scanned.binary.canonical_bundle.string(),
       scanned.binary.sha256});
  require(track.has_value(), "large instrument track creation failed");
  auto* project_session = session.get();
  auto bridge = held_bridge(directory.root());
  bridge->set_project(std::move(session), root);
  bridge->vst3_browser()->add_vst3_search_path(QString::fromStdString(bundle.string()));
  require(QTest::qWaitFor([&] { return !bridge->vst3_browser()->busy(); }, 15'000),
          "large instrument scan did not complete");
  auto engine = load_shell(*bridge);
  auto& window = shell_window(*engine);
  auto* controller = qobject_cast<quick::PluginAutomationController*>(bridge->plugin_automation());
  require(controller != nullptr, "plugin automation controller missing");
  bridge->vst3_editor()->open_vst3_instrument_editor(
      QString::fromStdString(track.value().to_string()));
  await_editor(*bridge);
  require(controller->view().value(QStringLiteral("role")) == QStringLiteral("source"),
          "large instrument editor did not select the source");
  browse_large_plugin(
      *bridge, window, *controller, 66, 66,
      [project_session]() -> const project::Vst3PluginInstance& {
        return std::get<project::Vst3PluginInstance>(
            project_session->snapshot().tracks.front().instrument_source);
      },
      true,
      [&bridge, &track] {
        bridge->vst3_editor()->open_vst3_instrument_editor(
            QString::fromStdString(track.value().to_string()));
        await_editor(*bridge);
      });
  bridge->vst3_editor()->discard_vst3_insert_editor();
}

} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    effect_parameter_lane_contract();
    instrument_and_missing_reference_contract();
    verified_instrument_parameter_contract();
    large_effect_parameter_browser_contract();
    large_instrument_parameter_browser_contract();
    return 0;
  } catch (const std::exception& error) {
    qCritical("%s", error.what());
    return 1;
  }
}
