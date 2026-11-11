#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/render_ahead.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/test/check.hpp"

#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Built-in effects are added, removed, reordered and switched while playing:
// the routed graph switches at a held block boundary and playback carries on.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;

using oscillate::test::require;
// How long a UI wait may take before it counts as a hang (not a timing claim).
constexpr int ui_wait_ms = 5'000;

// A device that keeps rendering 128-frame blocks on its own thread, a little
// faster than real time, until stopped.
class ThreadedOutput final : public app::PlaybackOutput {
public:
  ThreadedOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor)
      : source_(source), cursor_(cursor) {}
  ~ThreadedOutput() override { stop(); }

  foundation::Result<void> start(std::uint32_t, std::uint32_t,
                                 std::optional<std::uint64_t>) override {
    running_.store(true, std::memory_order_release);
    state_.store(audio::OutputState::streaming, std::memory_order_release);
    thread_ = std::thread([this] {
      std::array<engine::StereoFrame, 128> frames{};
      std::array<float, 256> samples{};
      while (running_.load(std::memory_order_acquire)) {
        if (audio::render_device_block(source_, cursor_, true, frames, samples) ==
            audio::BlockResult::rendered) {
          float peak = 0.0F;
          for (const float sample : samples)
            peak = std::max(peak, std::abs(sample));
          peak_.store(peak, std::memory_order_relaxed);
          if (peak == 0.0F)
            silent_.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
    return foundation::Result<void>::success();
  }
  void stop() noexcept override {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
      thread_.join();
    state_.store(audio::OutputState::stopped, std::memory_order_release);
  }
  [[nodiscard]] audio::OutputStatus status() const noexcept override {
    audio::OutputStatus result;
    result.state = state_.load(std::memory_order_acquire);
    return result;
  }
  static inline std::atomic<float> peak_{0.0F};
  // Rendered blocks with no signal: a held graph switch shows up here.
  static inline std::atomic<int> silent_{0};

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  std::atomic<bool> running_{false};
  std::atomic<audio::OutputState> state_{audio::OutputState::stopped};
  std::thread thread_;
};

QVariantList effects_of(quick::ProjectBridge& bridge) {
  return bridge.insert_chains().front().toMap().value("effects").toList();
}

void built_in_chain_edits_apply_while_playing() {
  QTemporaryDir directory;
  require(directory.isValid(), "fixture directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "project creation failed");
  auto session = std::move(created).value();
  const auto track = session->instruments().add_synth_track();
  require(track.has_value(), "synth track");
  require(session->midi_clips().add_midi_clip(track.value(), {0}).has_value(), "MIDI clip");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  for (int beat = 0; beat < 4; ++beat)
    require(session->notes().add_note(clip, {beat * timeline::ticks_per_quarter}, 48).has_value(),
            "note");
  require(session->effects().add_effect_engine(track.value(), 0, "contour").has_value(), "Contour");
  const auto track_id = QString::fromStdString(track.value().to_string());
  // A second track with no effects: its first and last effect come and go
  // while the first track's chain keeps playing.
  const auto bass = session->instruments().add_synth_track();
  require(bass.has_value(), "bass track");
  const auto bass_id = QString::fromStdString(bass.value().to_string());

  quick::ProjectBridge bridge(
      directory.path().toStdString(), nullptr,
      [](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
        return std::make_unique<ThreadedOutput>(source, cursor);
      },
      [] { return foundation::Result<std::vector<audio::PipeWireSink>>::success({}); });
  bridge.set_project(std::move(session), root);
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return bridge.playback_active() && bridge.routed_graph_current(); },
                          15'000),
          "playback did not start: " + bridge.playback_status().toStdString());

  const auto applied = [&](std::string_view what) {
    ThreadedOutput::silent_.store(0);
    const auto revision = bridge.revision();
    require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, 10'000),
            std::string(what) + " never became audible: " + bridge.playback_status().toStdString() +
                " / " + bridge.status().toStdString());
    require(bridge.playback_active() && bridge.revision() == revision,
            std::string(what) + " stopped playback");
    // The graph switches at a block boundary without a hold: never a dropout.
    require(ThreadedOutput::silent_.load() == 0,
            std::string(what) + " dropped " + std::to_string(ThreadedOutput::silent_.load()) +
                " silent blocks into playback");
  };

  require(!bridge.insert_chain_edit_enabled() && bridge.chain_edit_enabled(track_id) &&
              bridge.chain_edit_enabled(bass_id),
          "the panel would grey out a built-in chain's controls while playing");
  const auto before = bridge.revision();
  bridge.add_effect_engine(track_id, 1, QStringLiteral("press"));
  require(bridge.revision() == before + 1 && effects_of(bridge).size() == 2,
          "adding Press while playing was refused: " + bridge.status().toStdString());
  applied("adding Press");

  // A second edit to a chain that is already running.
  bridge.add_effect_engine(track_id, 2, QStringLiteral("expanse"));
  require(effects_of(bridge).size() == 3,
          "a second add while playing was refused: " + bridge.status().toStdString());
  applied("adding Expanse");
  const auto press = effects_of(bridge)[1].toMap().value("slotId").toString();
  bridge.move_effect_insert(track_id, press, 0);
  require(effects_of(bridge).front().toMap().value("slotId").toString() == press,
          "reordering while playing was refused: " + bridge.status().toStdString());
  applied("reordering");

  bridge.set_effect_insert_enabled(track_id, press, false);
  require(!effects_of(bridge).front().toMap().value("enabled").toBool(),
          "switching off while playing was refused: " + bridge.status().toStdString());
  applied("switching off");

  bridge.remove_effect_insert(track_id, press);
  require(effects_of(bridge).size() == 2,
          "removing while playing was refused: " + bridge.status().toStdString());
  applied("removing Press");
  // Removing the reverb drains its tail after the switch.
  const auto expanse = effects_of(bridge).back().toMap().value("slotId").toString();
  bridge.remove_effect_insert(track_id, expanse);
  require(effects_of(bridge).size() == 1,
          "removing Expanse while playing was refused: " + bridge.status().toStdString());
  applied("removing Expanse");
  require(QTest::qWaitFor([] { return ThreadedOutput::peak_.load() > 1e-3F; }, ui_wait_ms),
          "playback went silent after the edits");

  bridge.add_effect_engine(bass_id, 0, QStringLiteral("pocket"));
  const auto bass_effects = [&] {
    return bridge.insert_chains()[1].toMap().value("effects").toList();
  };
  require(bass_effects().size() == 1,
          "a first effect on another track was refused: " + bridge.status().toStdString());
  applied("adding Pocket to the bass");
  bridge.remove_effect_insert(bass_id, bass_effects().front().toMap().value("slotId").toString());
  require(bass_effects().isEmpty(),
          "removing the bass's only effect was refused: " + bridge.status().toStdString());
  applied("removing the bass's only effect");

  // Latency changes switch live too (ADR-0049). Ceiling's lookahead on the
  // first track sets a new output latency: the switch fades across one block
  // each way. The same lookahead on the bass leaves the output latency as it
  // is, so adding and removing it keeps time exactly.
  bridge.add_effect_engine(track_id, 1, QStringLiteral("ceiling"));
  require(effects_of(bridge).size() == 2,
          "adding Ceiling while playing was refused: " + bridge.status().toStdString());
  applied("adding Ceiling");
  bridge.add_effect_engine(bass_id, 0, QStringLiteral("ceiling"));
  require(bass_effects().size() == 1,
          "adding Ceiling to the bass was refused: " + bridge.status().toStdString());
  applied("adding Ceiling to the bass");
  bridge.remove_effect_insert(bass_id, bass_effects().front().toMap().value("slotId").toString());
  require(bass_effects().isEmpty(),
          "removing the bass's Ceiling was refused: " + bridge.status().toStdString());
  applied("removing the bass's Ceiling");
  require(ThreadedOutput::peak_.load() > 1e-3F, "playback went silent after the latency edits");

  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 15'000), "stop failed");
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    built_in_chain_edits_apply_while_playing();
  } catch (const std::exception& error) {
    qCritical("FAILED: %s", error.what());
    return 1;
  }
  return 0;
}
