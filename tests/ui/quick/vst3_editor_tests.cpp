#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "../../../apps/daw_quick/src/vst3_editor_commit.hpp"
#include "oscillate/audio_io/render_ahead.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/check.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
    std::string pattern = (fs::temp_directory_path() / "oscillate-vst3-editor-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "temporary directory failed");
    root_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  [[nodiscard]] const fs::path& root() const { return root_; }

private:
  fs::path root_;
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

class HeldOutput final : public app::PlaybackOutput {
public:
  [[nodiscard]] oscillate::foundation::Result<void> start(std::uint32_t, std::uint32_t,
                                                          std::optional<std::uint64_t>) override {
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
  std::atomic<oscillate::audio_io::OutputState> state_{oscillate::audio_io::OutputState::stopped};
};

void write_wav(const fs::path& path) {
  constexpr unsigned char wav[]{'R',  'I',  'F', 'F', 40, 0,    0, 0, 'W', 'A',  'V', 'E',
                                'f',  'm',  't', ' ', 16, 0,    0, 0, 1,   0,    1,   0,
                                0x80, 0xbb, 0,   0,   0,  0x77, 1, 0, 2,   0,    16,  0,
                                'd',  'a',  't', 'a', 4,  0,    0, 0, 0,   0x20, 0,   0x20};
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav), sizeof(wav));
  require(output.good(), "WAV fixture write failed");
}

void run_checks() {
  Directory directory;
  const auto root = directory.root() / "song";
  const auto wav = directory.root() / "source.wav";
  write_wav(wav);
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "project create failed");
  auto session = std::move(created).value();
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "fixture open failed");
  auto imported = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), imported ? "WAV import failed" : imported.error().message);
  const auto track = imported.value();
  auto plugin = session->effects().insert_vst3_plugin(
      track, 0,
      {std::string(32, 'a'), "Fixture", "Missing VST3", "1.0",
       (directory.root() / "missing.vst3").string(), std::string(64, 'a')});
  require(plugin.has_value(), "saved VST3 placeholder insertion failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  const auto initial_plugin = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  const auto opened_revision = session->snapshot().revision;
  require(!session->effects().set_vst3_editor_state(opened_revision - 1, track, slot,
                                                    initial_plugin, {9}, {{7, 0.9}}) &&
              session->snapshot().revision == opened_revision,
          "stale editor commit changed the project");
  require(session->effects()
              .set_vst3_editor_state(opened_revision, track, slot, initial_plugin, {1, 2, 3},
                                     {{7, 0.25}})
              .has_value(),
          "atomic editor state save failed");
  require(!session->effects().set_vst3_editor_state(opened_revision, track, slot, initial_plugin,
                                                    {9}, {{7, 0.9}}),
          "editor accepted a second commit from a stale revision");
  const auto before = session->snapshot().revision;
  require(session->history().undo().has_value(), "editor state undo failed");
  const auto* undone = std::get_if<project::Vst3PluginInstance>(
      &session->snapshot().tracks.front().effects.front().processor);
  require(undone && undone->component_state.empty() && undone->parameters.empty(),
          "editor state and parameter references did not undo atomically");
  require(session->history().redo().has_value(), "editor state redo failed");
  require(session->snapshot().revision > before, "redo did not advance project revision");
  auto commit_candidate = session->snapshot();
  const auto original = std::get<project::Vst3PluginInstance>(
      commit_candidate.tracks.front().effects.front().processor);
  require(quick::can_commit_vst3_editor_state(commit_candidate, track, slot, original),
          "unchanged editor slot was rejected");
  auto unrelated_edit = commit_candidate;
  ++unrelated_edit.revision;
  unrelated_edit.tracks.front().level = 0.7;
  require(quick::can_commit_vst3_editor_state(unrelated_edit, track, slot, original),
          "unrelated project edit invalidated unchanged editor target");
  auto changed_identity = commit_candidate;
  std::get<project::Vst3PluginInstance>(changed_identity.tracks.front().effects.front().processor)
      .binary_sha256 = std::string(64, 'b');
  require(!quick::can_commit_vst3_editor_state(changed_identity, track, slot, original),
          "editor accepted a changed saved plugin identity");
  auto removed_slot = commit_candidate;
  removed_slot.tracks.front().effects.clear();
  require(!quick::can_commit_vst3_editor_state(removed_slot, track, slot, original),
          "editor accepted a removed insert slot");
  session.reset();
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "saved editor project did not reopen");
  const auto* retained = std::get_if<project::Vst3PluginInstance>(
      &reopened.value()->snapshot().tracks.front().effects.front().processor);
  require(retained && retained->component_state == std::vector<std::uint8_t>({1, 2, 3}) &&
              retained->parameters == std::vector<project::Vst3ParameterReference>({{7, 0.25}}),
          "saved editor state did not survive reopen");

  quick::ProjectBridge bridge(directory.root());
  bridge.set_project(std::move(reopened.value()), root);
  const auto revision = bridge.revision();
  bridge.vst3_editor()->open_vst3_insert_editor(QString::fromStdString(track.to_string()),
                                                QString::fromStdString(slot.to_string()));
  await_editor(bridge);
  const auto editor = bridge.vst3_editor()->editor();
  require(editor.value(QStringLiteral("open")).toBool() &&
              !editor.value(QStringLiteral("native")).toBool() &&
              editor.value(QStringLiteral("parameters")).toList().size() == 1,
          "missing native editor did not expose saved generic parameter fallback");
  bridge.vst3_editor()->apply_vst3_insert_editor();
  require(bridge.revision() == revision, "read-only generic fallback mutated project");
  bridge.vst3_editor()->discard_vst3_insert_editor();
  require(bridge.vst3_editor()->editor().isEmpty() && bridge.revision() == revision,
          "discard changed project or retained editor state");
  bridge.vst3_editor()->open_vst3_insert_editor(QString::fromStdString(track.to_string()),
                                                QString::fromStdString(slot.to_string()));
  await_editor(bridge);
  const auto other_root = directory.root() / "other-song";
  auto replacement = app::ProjectSession::create(other_root);
  require(replacement.has_value(), "replacement project did not create");
  bridge.set_project(std::move(replacement.value()), other_root);
  require(bridge.vst3_editor()->editor().isEmpty(), "project switch retained editor state");
}

void relink_session_contract() {
  Directory directory;
  const auto root = directory.root() / "relink-song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "relink project creation failed");
  auto session = std::move(created).value();
  const auto wav = directory.root() / "source.wav";
  write_wav(wav);
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "relink WAV open failed");
  const auto imported = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "relink audio import failed");
  const app::Vst3InsertIdentity identity{std::string(32, 'a'),
                                         "Fixture",
                                         "Saved Gain",
                                         "1.0",
                                         (directory.root() / "missing.vst3").string(),
                                         std::string(64, 'a')};
  require(session->effects().insert_vst3_plugin(imported.value(), 0, identity).has_value(),
          "relink effect insert failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  const auto initial = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  require(session->effects()
              .set_vst3_editor_state(session->snapshot().revision, imported.value(), slot, initial,
                                     {4, 5, 6}, {{7, 0.25}})
              .has_value(),
          "relink fixture state save failed");
  const auto original = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  const auto before = session->snapshot().revision;
  auto verified = identity;
  verified.canonical_bundle_path = (directory.root() / "relocated.vst3").string();
  auto wrong_binary = verified;
  wrong_binary.binary_sha256 = std::string(64, 'b');
  auto wrong_class = verified;
  wrong_class.class_id = std::string(32, 'b');
  auto absent_identity = verified;
  absent_identity.binary_sha256.clear();
  require(!session->effects().relink_vst3_plugin(before, imported.value(), slot, original,
                                                 wrong_binary) &&
              !session->effects().relink_vst3_plugin(before, imported.value(), slot, original,
                                                     wrong_class) &&
              !session->effects().relink_vst3_plugin(before, imported.value(), slot, original,
                                                     absent_identity) &&
              !session->effects().relink_vst3_plugin(before - 1, imported.value(), slot, original,
                                                     verified) &&
              session->snapshot().revision == before,
          "failed or stale relink changed the saved plugin");
  require(
      session->effects().relink_vst3_plugin(before, imported.value(), slot, original, verified) &&
          session->snapshot().revision == before + 1,
      "verified relocation did not commit one revision");
  const auto& effect = session->snapshot().tracks.front().effects.front();
  const auto* recovered = std::get_if<project::Vst3PluginInstance>(&effect.processor);
  require(effect.id == slot && recovered && recovered->id == original.id &&
              recovered->bundle_path == verified.canonical_bundle_path &&
              recovered->component_state == original.component_state &&
              recovered->parameters == original.parameters &&
              !session->effects().relink_vst3_plugin(before, imported.value(), slot, original,
                                                     verified),
          "relink lost slot, state or stale-call protection");
  require(session->history().undo() &&
              std::get<project::Vst3PluginInstance>(
                  session->snapshot().tracks.front().effects.front().processor) == original &&
              session->history().redo() && session->saving().save(),
          "relink did not undo and redo atomically");
  session.reset();
  auto reopened = app::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().tracks.front().effects.front().id == slot &&
              std::get<project::Vst3PluginInstance>(
                  reopened.value()->snapshot().tracks.front().effects.front().processor)
                      .bundle_path == verified.canonical_bundle_path,
          "relocated effect did not reopen with original slot");

  const auto instrument_root = directory.root() / "instrument-song";
  auto instrument = app::ProjectSession::create(instrument_root);
  require(instrument.has_value(), "instrument relink project creation failed");
  const auto track = instrument.value()->instruments().create_vst3_instrument_track(identity);
  require(track.has_value(), "relink instrument source creation failed");
  const auto source = std::get<project::Vst3PluginInstance>(
      instrument.value()->snapshot().tracks.front().instrument_source);
  require(instrument.value()->effects().relink_vst3_plugin(instrument.value()->snapshot().revision,
                                                           track.value(), std::nullopt, source,
                                                           verified) &&
              instrument.value()->history().undo() &&
              std::get<project::Vst3PluginInstance>(
                  instrument.value()->snapshot().tracks.front().instrument_source) == source &&
              instrument.value()->history().redo() && instrument.value()->saving().save(),
          "instrument source relink lost identity or undo history");
  instrument.value().reset();
  auto source_reopened = app::ProjectSession::open(instrument_root);
  require(source_reopened &&
              std::get<project::Vst3PluginInstance>(
                  source_reopened.value()->snapshot().tracks.front().instrument_source)
                      .bundle_path == verified.canonical_bundle_path,
          "instrument source relocation did not reopen");
}

// Renders one device block per permit on its own thread, so a test decides
// exactly when the callback consumes a pending manual event.
struct BlockPermits {
  std::mutex mutex;
  std::condition_variable wake;
  std::size_t permits = 0;
  bool stopping = false;
  void permit() {
    {
      std::lock_guard lock(mutex);
      ++permits;
    }
    wake.notify_all();
  }
};

class PermittedOutput final : public app::PlaybackOutput {
public:
  PermittedOutput(oscillate::audio_io::StereoRenderSource source,
                  oscillate::audio_io::TransportCursor& cursor,
                  std::shared_ptr<BlockPermits> permits)
      : source_(source), cursor_(cursor), permits_(std::move(permits)) {}
  ~PermittedOutput() override { stop(); }

  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] oscillate::foundation::Result<void>
  start(std::uint32_t, std::uint32_t requested_frames, std::optional<std::uint64_t>) override {
    {
      std::lock_guard lock(permits_->mutex);
      permits_->stopping = false;
    }
    state_.store(oscillate::audio_io::OutputState::streaming, std::memory_order_release);
    // Scratch for the largest block this fake device was asked for, made
    // here, off the render loop.
    std::vector<oscillate::engine::StereoFrame> frames(requested_frames);
    std::vector<float> samples(static_cast<std::size_t>(requested_frames) * 2);
    worker_ = std::thread([this, requested_frames, frames = std::move(frames),
                           samples = std::move(samples)]() mutable {
      for (;;) {
        {
          std::unique_lock lock(permits_->mutex);
          permits_->wake.wait(lock, [&] { return permits_->stopping || permits_->permits != 0; });
          if (permits_->stopping)
            return;
          --permits_->permits;
        }
        (void)oscillate::audio_io::render_device_block(source_, cursor_, true, std::span(frames),
                                                       std::span(samples));
      }
    });
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override {
    {
      std::lock_guard lock(permits_->mutex);
      permits_->stopping = true;
    }
    permits_->wake.notify_all();
    if (worker_.joinable())
      worker_.join();
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
  std::shared_ptr<BlockPermits> permits_;
  std::thread worker_;
  std::atomic<oscillate::audio_io::OutputState> state_{oscillate::audio_io::OutputState::stopped};
};

// A synthetic plugin with test-only fixed-latency evidence. Each playback
// gesture saves one revision, shows pending until its exact receipt, then
// live. A gesture over an unacknowledged value is refused unsaved.
void qualified_live_parameter_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_CONNECTED_BUNDLE);
  require(scanned.ok(), "connected parameter fixture did not scan");
  const auto klass = std::ranges::find_if(scanned.metadata.classes, [](const auto& candidate) {
    return candidate.name == "Connected Gain" && candidate.category == "Audio Module Class";
  });
  require(klass != scanned.metadata.classes.end(), "connected effect class missing");
  const auto root = directory.root() / "live-parameter-song";
  auto session = std::move(app::ProjectSession::create(root)).value();
  const auto wav = directory.root() / "source.wav";
  write_wav(wav);
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "live parameter WAV open failed");
  const auto imported = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "live parameter WAV import failed");
  require(session->effects()
              .insert_vst3_plugin(imported.value(), 0,
                                  {klass->id, klass->vendor, klass->name, klass->version,
                                   scanned.binary.canonical_bundle.string(), scanned.binary.sha256})
              .has_value(),
          "connected effect insertion failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  const auto original = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  require(session->effects()
              .set_vst3_editor_state(session->snapshot().revision, imported.value(), slot, original,
                                     {}, std::vector<std::uint8_t>{0x4d}, {{101, 0.25}})
              .has_value(),
          "initial static parameter did not save");
  auto permits = std::make_shared<BlockPermits>();
  {
    quick::ProjectBridge bridge(
        directory.root(), nullptr,
        [permits](oscillate::audio_io::StereoRenderSource source,
                  oscillate::audio_io::TransportCursor& cursor) {
          return std::make_unique<PermittedOutput>(source, cursor, permits);
        },
        [] {
          return oscillate::foundation::Result<
              std::vector<oscillate::audio_io::PipeWireSink>>::success({});
        },
        {}, {},
        quick::ProjectBridge::Dependencies{.fixed_latency_vst3_binaries =
                                               std::vector{scanned.binary.sha256}});
    bridge.set_project(std::move(session), root);
    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_CONNECTED_BUNDLE));
    require(bridge.vst3_browser()->busy(), "connected parameter scan did not start");
    bridge.vst3_editor()->open_vst3_insert_editor(
        QString::fromStdString(imported.value().to_string()),
        QString::fromStdString(slot.to_string()));
    await_editor(bridge);
    const auto scanning_editor = bridge.vst3_editor()->editor();
    require(scanning_editor.value(QStringLiteral("open")).toBool() &&
                !scanning_editor.value(QStringLiteral("editable")).toBool() &&
                scanning_editor.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Scanning saved plugin")),
            "catalogue scan prevented the saved parameter fallback from opening");
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "connected parameter catalogue did not finish scanning");
    bridge.vst3_editor()->open_vst3_insert_editor(
        QString::fromStdString(imported.value().to_string()),
        QString::fromStdString(slot.to_string()));
    await_editor(bridge);
    require(bridge.vst3_editor()->editor().value(QStringLiteral("editable")).toBool(),
            "qualified fixture did not open its generic editor");
    const auto live_status = [&] {
      return bridge.vst3_editor()->editor().value(QStringLiteral("liveStatus")).toString();
    };
    double value = 0.3;
    for (const int frames : {64, 128}) {
      bridge.audio_output()->select_buffer_frames(frames);
      permits->permit(); // The first block activates the graph.
      bridge.toggle_playback();
      const auto playing = bridge.revision();
      require(QTest::qWaitFor(
                  [&] {
                    return bridge.playback_status().startsWith(
                        QStringLiteral("Playing routed revision %1").arg(playing));
                  },
                  15'000),
              "qualified effect did not reach the active routed graph: " +
                  bridge.playback_status().toStdString());
      bridge.vst3_editor()->open_vst3_insert_editor(
          QString::fromStdString(imported.value().to_string()),
          QString::fromStdString(slot.to_string()));
      await_editor(bridge);
      require(bridge.vst3_editor()->editor().value(QStringLiteral("editable")).toBool(),
              "VST3 editor could not reopen during playback");
      for (unsigned gesture = 1; gesture <= 2; ++gesture) {
        value += 0.1;
        bridge.vst3_editor()->set_vst3_insert_parameter(101, value);
        const auto saved = playing + gesture;
        require(bridge.revision() == saved &&
                    live_status() ==
                        QStringLiteral("Revision %1 queued to save; waiting for the audio "
                                       "callback to apply it")
                            .arg(saved),
                "live gesture was not saved as pending: " + live_status().toStdString());
        bridge.vst3_editor()->set_vst3_insert_parameter(101, 0.95);
        require(bridge.revision() == saved &&
                    live_status().contains(QStringLiteral("no callback receipt yet")),
                "gesture over an unacknowledged value was saved");
        permits->permit();
        require(QTest::qWaitFor(
                    [&] {
                      return live_status().startsWith(
                                 QStringLiteral("Revision %1 live from sample ").arg(saved)) &&
                             bridge.playback_status().startsWith(
                                 QStringLiteral("Playing routed revision %1").arg(saved));
                    },
                    5'000),
                "receipt did not make the saved revision live: " + live_status().toStdString() +
                    " / " + bridge.playback_status().toStdString());
      }
      bridge.toggle_playback();
      require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 15'000) &&
                  !bridge.vst3_editor()->editor().contains(QStringLiteral("liveStatus")) &&
                  bridge.vst3_editor()->editor().value(QStringLiteral("open")).toBool(),
              "stopped editor kept run status or closed");
    }
    const auto stopped = bridge.revision();
    bridge.vst3_editor()->set_vst3_insert_parameter(101, 0.2);
    require(bridge.revision() == stopped + 1 &&
                !bridge.vst3_editor()->editor().contains(QStringLiteral("liveStatus")),
            "stopped edit regressed after live edits");
    bridge.vst3_editor()->discard_vst3_insert_editor();
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "live parameter project did not reopen");
  const auto persisted = std::get<project::Vst3PluginInstance>(
      reopened.value()->snapshot().tracks.front().effects.front().processor);
  require(persisted.parameters == std::vector<project::Vst3ParameterReference>{{101, 0.2}},
          "live and stopped parameter intent did not reopen");
}

void generic_parameter_editor_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_CONNECTED_BUNDLE);
  require(scanned.ok(), "connected parameter fixture did not scan");
  const auto klass = std::ranges::find_if(scanned.metadata.classes, [](const auto& candidate) {
    return candidate.name == "Connected Gain" && candidate.category == "Audio Module Class";
  });
  require(klass != scanned.metadata.classes.end(), "connected effect class missing");
  const auto root = directory.root() / "generic-parameter-song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "generic parameter project creation failed");
  auto session = std::move(created).value();
  const auto wav = directory.root() / "source.wav";
  write_wav(wav);
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "generic parameter WAV open failed");
  const auto imported = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "generic parameter WAV import failed");
  const app::Vst3InsertIdentity identity{klass->id,
                                         klass->vendor,
                                         klass->name,
                                         klass->version,
                                         scanned.binary.canonical_bundle.string(),
                                         scanned.binary.sha256};
  require(session->effects().insert_vst3_plugin(imported.value(), 0, identity).has_value(),
          "connected effect insertion failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  const auto original = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  require(session->effects()
              .set_vst3_editor_state(session->snapshot().revision, imported.value(), slot, original,
                                     {}, std::vector<std::uint8_t>{0x4d}, {{101, 0.25}})
              .has_value(),
          "initial static parameter did not save");
  auto permits = std::make_shared<BlockPermits>();
  {
    quick::ProjectBridge bridge(
        directory.root(), nullptr,
        [permits](oscillate::audio_io::StereoRenderSource source,
                  oscillate::audio_io::TransportCursor& cursor) {
          return std::make_unique<PermittedOutput>(source, cursor, permits);
        },
        [] {
          return oscillate::foundation::Result<
              std::vector<oscillate::audio_io::PipeWireSink>>::success({});
        });
    bridge.set_project(std::move(session), root);
    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_CONNECTED_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "connected parameter catalogue did not finish scanning");
    bridge.vst3_editor()->open_vst3_insert_editor(
        QString::fromStdString(imported.value().to_string()),
        QString::fromStdString(slot.to_string()));
    await_editor(bridge);
    const auto editor = bridge.vst3_editor()->editor();
    require(editor.value(QStringLiteral("open")).toBool() &&
                editor.value(QStringLiteral("editable")).toBool() &&
                !editor.value(QStringLiteral("native")).toBool() &&
                editor.value(QStringLiteral("parameters")).toList().size() == 1,
            "saved static parameter did not open in editable generic fallback");
    const auto initial_revision = bridge.revision();
    const auto shown_value = [&] {
      return bridge.vst3_editor()
          ->editor()
          .value(QStringLiteral("parameters"))
          .toList()
          .front()
          .toMap()
          .value(QStringLiteral("value"))
          .toString();
    };
    // The unqualified fixture saves parameter intent during playback but
    // never presents the editor clone's value as audible in the active graph.
    for (const int frames : {64, 128}) {
      bridge.audio_output()->select_buffer_frames(frames);
      const auto playing_revision = bridge.revision();
      bridge.toggle_playback();
      const bool graph_ready = QTest::qWaitFor(
          [&] {
            permits->permit();
            return bridge.playback_active() &&
                   bridge.playback_status().startsWith(
                       QStringLiteral("Playing routed revision %1").arg(playing_revision));
          },
          15'000);
      require(graph_ready, "connected effect did not reach the active routed graph at " +
                               std::to_string(frames) +
                               " frames: " + bridge.playback_status().toStdString());
      const double requested = frames == 64 ? 0.6 : 0.65;
      bridge.vst3_editor()->set_vst3_insert_parameter(101, requested);
      const auto live =
          bridge.vst3_editor()->editor().value(QStringLiteral("liveStatus")).toString();
      require(
          bridge.revision() == playing_revision + 1 &&
              shown_value() == QString::number(requested, 'f', 3) &&
              bridge.vst3_editor()->editor().value(QStringLiteral("open")).toBool() &&
              live.contains(QStringLiteral("not yet audible")) &&
              live.contains(QStringLiteral("no verified live control")) && bridge.status() == live,
          "unqualified playback gesture was not saved with honest status: " + live.toStdString());
      bridge.vst3_editor()->set_vst3_insert_parameter(101, requested + 0.01);
      require(bridge.revision() == playing_revision + 2 &&
                  shown_value() == QString::number(requested + 0.01, 'f', 3) &&
                  bridge.vst3_editor()
                      ->editor()
                      .value(QStringLiteral("liveStatus"))
                      .toString()
                      .contains(QStringLiteral("not yet audible")),
              "second playback-time edit did not update the saved intent");
      bridge.toggle_playback();
      require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 15'000),
              "routed playback did not stop after saved-only gesture");
    }
    bridge.vst3_editor()->set_vst3_insert_parameter(999, 0.5);
    bridge.vst3_editor()->set_vst3_insert_parameter(101, 1.5);
    require(bridge.revision() == initial_revision + 4,
            "invalid generic parameter changed the project");
    bridge.vst3_editor()->set_vst3_insert_parameter(101, 0.5);
    require(bridge.revision() == initial_revision + 5 &&
                bridge.vst3_editor()->editor().value(QStringLiteral("open")).toBool() &&
                !bridge.vst3_editor()->editor().contains(QStringLiteral("liveStatus")),
            "generic parameter did not save one undo step and stay open");
    bridge.vst3_editor()->set_vst3_insert_parameter(101, 0.75);
    require(bridge.revision() == initial_revision + 6,
            "second generic parameter edit did not save one undo step");
    bridge.set_track_level_pan(QString::fromStdString(imported.value().to_string()), 0.7, 0.0);
    const auto after_level = bridge.revision();
    require(after_level == initial_revision + 7,
            "unrelated track level change did not advance project revision");
    bridge.vst3_editor()->set_vst3_insert_parameter(101, 0.8);
    require(bridge.revision() == after_level + 1 &&
                bridge.vst3_editor()->editor().value(QStringLiteral("open")).toBool(),
            "unchanged VST3 target could not commit after unrelated edit");
    bridge.vst3_editor()->discard_vst3_insert_editor();
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "generic parameter project did not reopen");
  const auto persisted = std::get<project::Vst3PluginInstance>(
      reopened.value()->snapshot().tracks.front().effects.front().processor);
  require(persisted.parameters == std::vector<project::Vst3ParameterReference>{{101, 0.8}} &&
              !persisted.component_state.empty() && persisted.controller_state &&
              *persisted.controller_state == std::vector<std::uint8_t>{0x4d},
          "generic parameter, component and controller state did not survive reopen");
}

void generic_instrument_editor_saves_while_playing() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scanned.ok() && !scanned.metadata.classes.empty(), "instrument fixture did not scan");
  const auto& klass = scanned.metadata.classes.front();
  const auto root = directory.root() / "instrument-edit-song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "instrument project creation failed");
  const app::Vst3InsertIdentity identity{klass.id,
                                         klass.vendor,
                                         klass.name,
                                         klass.version,
                                         scanned.binary.canonical_bundle.string(),
                                         scanned.binary.sha256};
  const auto track = created.value()->instruments().create_vst3_instrument_track(identity);
  require(track.has_value(), "instrument track creation failed");
  auto permits = std::make_shared<BlockPermits>();
  {
    quick::ProjectBridge bridge(
        directory.root(), nullptr,
        [permits](oscillate::audio_io::StereoRenderSource source,
                  oscillate::audio_io::TransportCursor& cursor) {
          return std::make_unique<PermittedOutput>(source, cursor, permits);
        },
        [] {
          return oscillate::foundation::Result<
              std::vector<oscillate::audio_io::PipeWireSink>>::success({});
        });
    bridge.set_project(std::move(created).value(), root);
    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_INSTRUMENT_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "instrument catalogue did not finish scanning");
    bridge.vst3_editor()->open_vst3_instrument_editor(
        QString::fromStdString(track.value().to_string()));
    await_editor(bridge);
    require(bridge.vst3_editor()->editor().value(QStringLiteral("editable")).toBool(),
            "instrument generic editor is unavailable");
    const auto running = bridge.revision();
    bridge.toggle_playback();
    require(QTest::qWaitFor(
                [&] {
                  permits->permit();
                  return bridge.playback_status().startsWith(
                      QStringLiteral("Playing routed revision %1").arg(running));
                },
                15'000),
            "instrument playback did not start");
    bridge.vst3_editor()->set_vst3_insert_parameter(1, 0.4);
    const auto status =
        bridge.vst3_editor()->editor().value(QStringLiteral("liveStatus")).toString();
    require(bridge.revision() == running + 1 &&
                status.contains(QStringLiteral("not yet audible")) && bridge.playback_active(),
            "instrument parameter did not save safely during playback");
    bridge.toggle_playback();
    require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 15'000),
            "instrument playback did not stop");
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "instrument edit did not reopen");
  const auto& source = std::get<project::Vst3PluginInstance>(
      reopened.value()->snapshot().tracks.front().instrument_source);
  require(source.parameters == std::vector<project::Vst3ParameterReference>{{1, 0.4}} &&
              !source.component_state.empty() && source.controller_state &&
              *source.controller_state == source.component_state,
          "instrument parameter and both state chunks were not saved together");
}

void controller_state_quick_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_CONNECTED_BUNDLE);
  require(scanned.ok(), "controller-state effect fixture did not scan");
  const auto klass = std::ranges::find_if(scanned.metadata.classes, [](const auto& candidate) {
    return candidate.name == "Connected Gain" && candidate.category == "Audio Module Class";
  });
  require(klass != scanned.metadata.classes.end(), "controller-state effect class missing");
  const auto root = directory.root() / "controller-state-song";
  auto session = std::move(app::ProjectSession::create(root)).value();
  const auto wav = directory.root() / "source.wav";
  write_wav(wav);
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "controller-state WAV open failed");
  const auto track = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(track.has_value(), "controller-state WAV import failed");
  const app::Vst3InsertIdentity identity{klass->id,
                                         klass->vendor,
                                         klass->name,
                                         klass->version,
                                         scanned.binary.canonical_bundle.string(),
                                         scanned.binary.sha256};
  require(session->effects().insert_vst3_plugin(track.value(), 0, identity).has_value(),
          "controller-state effect insertion failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  const auto initial = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  float gain = 2.0F;
  std::vector<std::uint8_t> component(sizeof(gain));
  std::memcpy(component.data(), &gain, sizeof(gain));
  require(session->effects()
              .set_vst3_editor_state(session->snapshot().revision, track.value(), slot, initial,
                                     component, std::vector<std::uint8_t>{0x4d}, {{101, 0.25}})
              .has_value(),
          "controller-state initial pair did not save");
  const auto read_saved_effect = [&] {
    auto reader = oscillate::persistence::ProjectStore::open_reader(root);
    require(reader.has_value(), "controller-state reader could not open");
    const auto generation = reader.value().load_current();
    require(generation.has_value(), "controller-state generation could not load");
    return std::get<project::Vst3PluginInstance>(
        generation.value().snapshot.tracks.front().effects.front().processor);
  };
  auto permits = std::make_shared<BlockPermits>();
  {
    quick::ProjectBridge bridge(
        directory.root(), nullptr,
        [permits](oscillate::audio_io::StereoRenderSource source,
                  oscillate::audio_io::TransportCursor& cursor) {
          return std::make_unique<PermittedOutput>(source, cursor, permits);
        },
        [] {
          return oscillate::foundation::Result<
              std::vector<oscillate::audio_io::PipeWireSink>>::success({});
        },
        {}, {},
        quick::ProjectBridge::Dependencies{.fixed_latency_vst3_binaries =
                                               std::vector{scanned.binary.sha256}});
    bridge.set_project(std::move(session), root);
    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_CONNECTED_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "controller-state catalogue did not finish scanning");
    const auto open_editor = [&] {
      bridge.vst3_editor()->open_vst3_insert_editor(
          QString::fromStdString(track.value().to_string()),
          QString::fromStdString(slot.to_string()));
      await_editor(bridge);
      require(bridge.vst3_editor()->editor().value(QStringLiteral("editable")).toBool(),
              "controller-state generic editor did not open");
    };
    open_editor();
    const auto initial_revision = bridge.revision();
    bridge.toggle_playback();
    require(QTest::qWaitFor(
                [&] {
                  permits->permit();
                  return bridge.playback_status().startsWith(
                      QStringLiteral("Playing routed revision %1").arg(initial_revision));
                },
                15'000),
            "controller-state playback did not start");
    require(setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", "controller-private-edit", 1) == 0,
            "controller-private edit fixture could not start");
    bridge.vst3_editor()->set_vst3_insert_parameter(101, 0.5);
    require(unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL") == 0,
            "controller-private edit fixture could not stop");
    const auto status =
        bridge.vst3_editor()->editor().value(QStringLiteral("liveStatus")).toString();
    require(bridge.revision() == initial_revision + 1 &&
                status.contains(QStringLiteral("controller state changed")) &&
                status.contains(QStringLiteral("not yet audible")) && bridge.playback_active(),
            "controller-only change was falsely reported live: " + status.toStdString());
    const auto committed_pair = [&] {
      const auto saved = read_saved_effect();
      return saved.component_state == component && saved.controller_state &&
             *saved.controller_state == std::vector<std::uint8_t>{0x7b} &&
             saved.parameters == std::vector<project::Vst3ParameterReference>{{101, 0.5}};
    };
    require(QTest::qWaitFor(committed_pair, 5'000),
            "controller-only bytes, component and parameter were not saved atomically");
    const auto saved = read_saved_effect();
    // The saved pair loads into a new instance that switches in live
    // (ADR-0050).
    require(QTest::qWaitFor(
                [&] {
                  permits->permit();
                  return bridge.routed_graph_current() && bridge.playback_active();
                },
                5'000),
            "a saved controller pair did not switch in live");
    bridge.toggle_playback();
    require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 15'000),
            "controller-state playback did not stop");
    bridge.undo();
    require(bridge.status().contains(QStringLiteral("Undo applied")),
            "controller-state Quick undo failed");
    bridge.save();
    const auto undone = read_saved_effect();
    require(undone.component_state == component && undone.controller_state &&
                *undone.controller_state == std::vector<std::uint8_t>{0x4d} &&
                undone.parameters == std::vector<project::Vst3ParameterReference>{{101, 0.25}},
            "one undo did not restore both VST3 chunks and parameter intent");
    for (const char* mode : {"controller-private-save", "controller-private-oversized"}) {
      open_editor();
      const auto revision = bridge.revision();
      require(setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", mode, 1) == 0,
              "controller capture failure fixture could not start");
      bridge.vst3_editor()->set_vst3_insert_parameter(101, 0.75);
      require(unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL") == 0,
              "controller capture failure fixture could not stop");
      require(bridge.revision() == revision &&
                  bridge.status().contains(QStringLiteral("state could not be saved")) &&
                  read_saved_effect() == undone,
              "failed controller capture partially saved a Quick editor transaction");
    }
    bridge.redo();
    const auto redo_status = bridge.status();
    bridge.save();
    const auto redone = read_saved_effect();
    require(redo_status.contains(QStringLiteral("Redo applied")) && redone == saved,
            "controller-state redo did not restore the complete pair: " +
                redo_status.toStdString() + " / " +
                std::to_string(
                    redone.parameters.empty() ? -1.0 : redone.parameters.front().normalized_value));
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), reopened ? "controller-state project did not reopen after edit"
                                         : reopened.error().message);
  const auto& saved = std::get<project::Vst3PluginInstance>(
      reopened.value()->snapshot().tracks.front().effects.front().processor);
  require(saved.component_state == component && saved.controller_state &&
              *saved.controller_state == std::vector<std::uint8_t>{0x7b} &&
              saved.parameters == std::vector<project::Vst3ParameterReference>{{101, 0.5}},
          "controller-only bytes, component and parameter were not saved atomically");
}

// Holds the editor's worker hash until the test opens it, after the binary
// was read, and records the thread the hash ran on.
struct HashGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool open = true;
  int started = 0;
  int finished = 0;
  std::thread::id worker;
};
// Longest a held hash waits before it lets go, so a broken test cannot hang.
constexpr auto hash_gate_limit = std::chrono::seconds(30);
// A catalogue scan of the copied fixture.
constexpr int catalogue_scan_timeout_ms = 15'000;

void editor_hashes_binary_off_the_gui_thread() {
  Directory directory;
  const auto bundle = directory.root() / "plugins" / fs::path(VST3_CONNECTED_BUNDLE).filename();
  fs::create_directories(bundle.parent_path());
  fs::copy(VST3_CONNECTED_BUNDLE, bundle, fs::copy_options::recursive);
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(bundle);
  require(scanned.ok(), "copied connected fixture did not scan");
  const auto klass = std::ranges::find_if(scanned.metadata.classes, [](const auto& candidate) {
    return candidate.name == "Connected Gain" && candidate.category == "Audio Module Class";
  });
  require(klass != scanned.metadata.classes.end(), "connected effect class missing");
  const auto root = directory.root() / "held-hash-song";
  auto session = std::move(app::ProjectSession::create(root)).value();
  const auto wav = directory.root() / "source.wav";
  write_wav(wav);
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "held hash WAV open failed");
  const auto imported = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "held hash WAV import failed");
  const app::Vst3InsertIdentity identity{klass->id,
                                         klass->vendor,
                                         klass->name,
                                         klass->version,
                                         scanned.binary.canonical_bundle.string(),
                                         scanned.binary.sha256};
  for (std::size_t index = 0; index < 2; ++index)
    require(session->effects().insert_vst3_plugin(imported.value(), index, identity).has_value(),
            "connected effect insertion failed");
  const auto track_id = QString::fromStdString(imported.value().to_string());
  const auto& effects = session->snapshot().tracks.front().effects;
  const auto kept_slot = QString::fromStdString(effects.front().id.to_string());
  const auto removed_slot = QString::fromStdString(effects.back().id.to_string());

  auto gate = std::make_shared<HashGate>();
  quick::ProjectBridge::Dependencies dependencies;
  dependencies.vst3_binary_hasher = [gate](const fs::path& path) {
    auto hashed = oscillate::plugins::hash_vst3_binary(path);
    std::unique_lock lock(gate->mutex);
    gate->worker = std::this_thread::get_id();
    ++gate->started;
    gate->changed.notify_all();
    gate->changed.wait_for(lock, hash_gate_limit, [&] { return gate->open; });
    ++gate->finished;
    return hashed;
  };
  const auto hold = [&](bool held) {
    {
      std::lock_guard lock(gate->mutex);
      gate->open = !held;
    }
    gate->changed.notify_all();
  };
  const auto await_count = [&](const int& counter, int count, const std::string& what) {
    require(QTest::qWaitFor(
                [&] {
                  std::lock_guard lock(gate->mutex);
                  return counter >= count;
                },
                editor_verify_timeout_ms),
            what);
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, {}, {}, {}, std::move(dependencies));
  bridge.set_project(std::move(session), root);
  bridge.vst3_browser()->add_vst3_search_path(QString::fromStdString(bundle.string()));
  require(
      QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, catalogue_scan_timeout_ms),
      "copied connected catalogue did not finish scanning");
  const auto editor = [&] { return bridge.vst3_editor()->editor(); };

  // The open returns at once with a pending editor; the hash runs on a
  // worker while the GUI thread keeps handling events.
  hold(true);
  bridge.vst3_editor()->open_vst3_insert_editor(track_id, kept_slot);
  require(editor().value(QStringLiteral("open")).toBool() &&
              editor().value(QStringLiteral("pending")).toBool() &&
              !editor().value(QStringLiteral("editable")).toBool() &&
              editor()
                  .value(QStringLiteral("message"))
                  .toString()
                  .contains(QStringLiteral("Verifying")),
          "editor did not show a pending state while its binary was hashed");
  await_count(gate->started, 1, "editor hash did not start");
  {
    std::lock_guard lock(gate->mutex);
    require(gate->worker != std::this_thread::get_id(), "editor hashed on the GUI thread");
  }
  bool ticked = false;
  QTimer::singleShot(0, [&ticked] { ticked = true; });
  require(QTest::qWaitFor([&] { return ticked; }, editor_verify_timeout_ms) &&
              editor().value(QStringLiteral("pending")).toBool(),
          "GUI event loop stalled while the editor hash was held");
  hold(false);
  await_editor(bridge);
  require(editor().value(QStringLiteral("editable")).toBool(),
          "verified editor did not load the plugin: " +
              editor().value(QStringLiteral("message")).toString().toStdString());
  bridge.vst3_editor()->discard_vst3_insert_editor();

  // Same bytes under a new inode after the hash: the load is refused.
  hold(true);
  bridge.vst3_editor()->open_vst3_insert_editor(track_id, kept_slot);
  await_count(gate->started, 2, "second editor hash did not start");
  const auto binary = scanned.binary.canonical_binary;
  const auto replacement = binary.parent_path() / "replacement.so";
  fs::copy_file(binary, replacement);
  fs::rename(replacement, binary);
  hold(false);
  await_editor(bridge);
  require(editor().value(QStringLiteral("open")).toBool() &&
              !editor().value(QStringLiteral("editable")).toBool() &&
              editor()
                  .value(QStringLiteral("message"))
                  .toString()
                  .contains(QStringLiteral("changed since it was verified")),
          "binary replaced after hashing was not refused: " +
              editor().value(QStringLiteral("message")).toString().toStdString());
  bridge.vst3_editor()->discard_vst3_insert_editor();

  // An effect removed while its binary was hashed closes the editor.
  hold(true);
  bridge.vst3_editor()->open_vst3_insert_editor(track_id, removed_slot);
  await_count(gate->started, 3, "third editor hash did not start");
  const auto before_removal = bridge.revision();
  bridge.remove_effect_insert(track_id, removed_slot);
  require(bridge.revision() == before_removal + 1, "pending editor blocked effect removal");
  hold(false);
  require(QTest::qWaitFor([&] { return editor().isEmpty(); }, editor_verify_timeout_ms),
          "editor stayed open for a removed effect");

  // A replaced session drops the pending result.
  hold(true);
  bridge.vst3_editor()->open_vst3_insert_editor(track_id, kept_slot);
  await_count(gate->started, 4, "fourth editor hash did not start");
  const auto other_root = directory.root() / "replacement-song";
  auto replacement_session = app::ProjectSession::create(other_root);
  require(replacement_session.has_value(), "replacement project did not create");
  bridge.set_project(std::move(replacement_session.value()), other_root);
  require(editor().isEmpty(), "session replacement kept the pending editor");
  QSignalSpy editor_changes(bridge.vst3_editor(), &quick::Vst3EditorController::editorChanged);
  hold(false);
  await_count(gate->finished, 4, "held hash did not finish");
  QCoreApplication::processEvents();
  require(editor().isEmpty() && editor_changes.isEmpty(),
          "a replaced session's hash result reached the editor");
}

void relink_bridge_contract() {
  Directory directory;
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_GAIN_BUNDLE);
  require(scanned.ok() && scanned.metadata.classes.size() == 1,
          "relink scanner fixture unavailable");
  const auto& klass = scanned.metadata.classes.front();
  const auto root = directory.root() / "bridge-relink-song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "bridge relink project creation failed");
  auto session = std::move(created).value();
  const auto wav = directory.root() / "source.wav";
  write_wav(wav);
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "bridge relink WAV open failed");
  const auto imported = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "bridge relink WAV import failed");
  const app::Vst3InsertIdentity absent{klass.id,
                                       klass.vendor,
                                       klass.name,
                                       klass.version,
                                       (directory.root() / "absent.vst3").string(),
                                       scanned.binary.sha256};
  require(session->effects().insert_vst3_plugin(imported.value(), 0, absent).has_value(),
          "bridge relink placeholder insert failed");
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  const auto original = std::get<project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  require(session->effects()
              .set_vst3_editor_state(session->snapshot().revision, imported.value(), slot, original,
                                     {7, 8, 9}, {{11, 0.35}})
              .has_value(),
          "bridge relink saved state failed");
  session.reset();
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "bridge relink placeholder did not reopen");
  quick::ProjectBridge bridge(
      directory.root(), nullptr,
      [](oscillate::audio_io::StereoRenderSource, oscillate::audio_io::TransportCursor&) {
        return std::make_unique<HeldOutput>();
      },
      [] {
        return oscillate::foundation::Result<
            std::vector<oscillate::audio_io::PipeWireSink>>::success({});
      });
  bridge.set_project(std::move(reopened).value(), root);
  const auto track_id = QString::fromStdString(imported.value().to_string());
  const auto slot_id = QString::fromStdString(slot.to_string());
  const auto chain = bridge.insert_chains().front().toMap();
  const auto saved = chain.value(QStringLiteral("effects")).toList().front().toMap();
  require(saved.value(QStringLiteral("slotId")) == slot_id &&
              saved.value(QStringLiteral("sha256")) ==
                  QString::fromStdString(scanned.binary.sha256) &&
              saved.value(QStringLiteral("stateBytes")).toInt() == 3 &&
              saved.value(QStringLiteral("parameterCount")).toInt() == 1,
          "bridge lost saved placeholder identity or state projection");

  const auto before = bridge.revision();
  bridge.vst3_browser()->relink_vst3_plugin(track_id, slot_id, -1);
  require(bridge.revision() == before &&
              bridge.vst3_browser()->status().contains(QStringLiteral("Select a saved")),
          "invalid relink changed the project");
  bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_GAIN_BUNDLE));
  const auto candidate = [&bridge, &klass]() -> QVariantMap {
    for (const auto& item : bridge.vst3_browser()->entries()) {
      auto map = item.toMap();
      if (map.value(QStringLiteral("classId")) == QString::fromStdString(klass.id) &&
          map.value(QStringLiteral("state")) == QStringLiteral("ready"))
        return map;
    }
    return {};
  };
  require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy() && !candidate().isEmpty(); },
                          15'000),
          "relink candidate did not scan");
  bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_INSTRUMENT_BUNDLE));
  const auto incompatible = [&bridge]() -> QVariantMap {
    for (const auto& item : bridge.vst3_browser()->entries()) {
      auto map = item.toMap();
      if (map.value(QStringLiteral("kind")) == QStringLiteral("instrument") &&
          map.value(QStringLiteral("state")) == QStringLiteral("ready"))
        return map;
    }
    return {};
  };
  require(QTest::qWaitFor(
              [&] { return !bridge.vst3_browser()->busy() && !incompatible().isEmpty(); }, 15'000),
          "incompatible scanner candidate did not appear");
  bridge.vst3_browser()->relink_vst3_plugin(track_id, slot_id,
                                            incompatible().value(QStringLiteral("index")).toInt());
  require(bridge.revision() == before &&
              bridge.vst3_browser()->status().contains(QStringLiteral("exact saved VST3 class")) &&
              bridge.insert_chains()
                      .front()
                      .toMap()
                      .value(QStringLiteral("effects"))
                      .toList()
                      .front()
                      .toMap()
                      .value(QStringLiteral("bundle")) ==
                  QString::fromStdString(absent.canonical_bundle_path),
          "incompatible class damaged saved placeholder");
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("projectBridge", &bridge);
  engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!engine.rootObjects().isEmpty(), "relink Quick shell failed to load");
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  require(window != nullptr, "relink Quick window missing");
  auto* chain_panel = find_item(window->contentItem(), QStringLiteral("insertChainPanel"));
  require(chain_panel != nullptr, "saved insert chain panel missing");
  chain_panel->setProperty("selectedRecoverySlotId", slot_id);
  QCoreApplication::processEvents();
  auto* recovery = find_item(window->contentItem(), QStringLiteral("vst3RecoveryCandidate"));
  require(recovery && recovery->isVisible() && recovery->isEnabled(),
          "exact saved VST3 candidate not offered in recovery UI");
  bridge.vst3_browser()->relink_vst3_plugin(track_id, slot_id,
                                            candidate().value(QStringLiteral("index")).toInt());
  require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000) &&
              bridge.revision() == before + 1 &&
              bridge.vst3_browser()->status().contains(QStringLiteral("relinked and saved")) &&
              bridge.playback_status().contains(QStringLiteral("not active")),
          "verified bridge relink was not saved and marked inactive");
  const auto relinked = bridge.insert_chains()
                            .front()
                            .toMap()
                            .value(QStringLiteral("effects"))
                            .toList()
                            .front()
                            .toMap();
  require(relinked.value(QStringLiteral("slotId")) == slot_id &&
              relinked.value(QStringLiteral("bundle")) ==
                  QString::fromStdString(scanned.binary.canonical_bundle.string()) &&
              relinked.value(QStringLiteral("stateBytes")).toInt() == 3 &&
              relinked.value(QStringLiteral("parameterCount")).toInt() == 1,
          "bridge relink discarded saved chain state");
  const auto active_revision = bridge.revision();
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return bridge.playback_active(); }, 10'000),
          "verified VST3 graph did not start on held output");
  bridge.vst3_browser()->relink_vst3_plugin(track_id, slot_id,
                                            candidate().value(QStringLiteral("index")).toInt());
  require(bridge.revision() == active_revision &&
              bridge.vst3_browser()->status().contains(QStringLiteral("Stop playback")),
          "active playback allowed VST3 relink");
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 10'000),
          "held playback did not stop for relink undo");
  bridge.undo();
  require(bridge.insert_chains()
                  .front()
                  .toMap()
                  .value(QStringLiteral("effects"))
                  .toList()
                  .front()
                  .toMap()
                  .value(QStringLiteral("bundle")) ==
              QString::fromStdString(absent.canonical_bundle_path),
          "one bridge undo did not restore saved placeholder");
}
} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    run_checks();
    editor_hashes_binary_off_the_gui_thread();
    generic_parameter_editor_contract();
    generic_instrument_editor_saves_while_playing();
    controller_state_quick_contract();
    qualified_live_parameter_contract();
    relink_session_contract();
    relink_bridge_contract();
    return 0;
  } catch (const std::exception& error) {
    qCritical("%s", error.what());
    return 1;
  }
}
