#include "arrangement_automation_controller.hpp"
#include "project_bridge.hpp"
#include "rebalance_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/render_ahead.hpp"
#include "oscillate/test/check.hpp"

#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Edits made while playing with render-ahead on (ADR-0128): each is heard
// within the window and one block, and the device plays on without a gap or
// a jump, and without stopping.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

constexpr std::size_t block = 256;
constexpr std::size_t stereo = 2;
// The device plays this many windows before the first edit.
constexpr std::size_t warm_windows = 2;
constexpr std::uint32_t sample_rate = 48'000;
constexpr std::size_t microseconds_per_second = 1'000'000;
constexpr auto period = std::chrono::microseconds{block * microseconds_per_second / sample_rate};
constexpr int render_ahead_ms = 100;
constexpr std::size_t most_blocks = 1 << 16;
constexpr int start_wait_ms = 15'000;
constexpr int edit_wait_ms = 10'000;
constexpr float silent_peak = 1e-4F;
constexpr float audible_peak = 1e-3F;
// A level lowered by the edit: under this share of what played before.
constexpr float lowered_share = 0.5F;
// Trim's "Gain -6 dB" preset (trim_presets.hpp), and how far a -6 dB stage
// lowers and then, removed, raises the level, with room for the synth's own
// movement between blocks.
constexpr int trim_minus_six_preset = 5;
constexpr float trimmed_share = 0.7F;
constexpr float untrimmed_ratio = 1.4F;
constexpr int bars = 64;
constexpr int beats_per_bar = 4;
constexpr std::uint8_t note_key = 48;
// Blocks a gain change takes to settle once heard: the mixer's 128-frame
// ramp, and a synth's or effect's own short glide.
constexpr std::size_t settle_blocks = 4;

// A device that plays through render-ahead in real time on its own thread,
// with the producer running beside it as an output runs it, and records
// each block's peak and the position it played.
class AheadOutput final : public app::PlaybackOutput {
public:
  AheadOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor)
      : source_(source), cursor_(cursor) {
    peaks_.resize(most_blocks);
    positions_.resize(most_blocks);
  }
  ~AheadOutput() override { stop(); }

  foundation::Result<void> start(std::uint32_t, std::uint32_t,
                                 std::optional<std::uint64_t>) override {
    if (source_.ahead != nullptr)
      source_.ahead->start_owned_thread();
    running_.store(true, std::memory_order_release);
    state_.store(audio::OutputState::streaming, std::memory_order_release);
    thread_ = std::thread([this] {
      std::array<engine::StereoFrame, block> frames{};
      std::array<float, block * stereo> samples{};
      auto deadline = std::chrono::steady_clock::now();
      while (running_.load(std::memory_order_acquire)) {
        (void)audio::render_device_block(source_, cursor_, true, frames, samples);
        float peak = 0.0F;
        for (const float sample : samples)
          peak = std::max(peak, std::abs(sample));
        const auto index = blocks_.load(std::memory_order_relaxed);
        if (index < most_blocks) {
          peaks_[index] = peak;
          positions_[index] = source_.ahead != nullptr
                                  ? source_.ahead->audible_position().next_sample
                                  : cursor_.completed_snapshot().next_sample;
          blocks_.store(index + 1, std::memory_order_release);
        }
        deadline += period;
        std::this_thread::sleep_until(deadline);
      }
    });
    return foundation::Result<void>::success();
  }
  void stop() noexcept override {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
      thread_.join();
    if (source_.ahead != nullptr)
      source_.ahead->stop_owned_thread();
    state_.store(audio::OutputState::stopped, std::memory_order_release);
  }
  [[nodiscard]] audio::OutputStatus status() const noexcept override {
    audio::OutputStatus result;
    result.state = state_.load(std::memory_order_acquire);
    return result;
  }

  static inline AheadOutput* current = nullptr;
  [[nodiscard]] std::size_t blocks() const noexcept {
    return blocks_.load(std::memory_order_acquire);
  }
  [[nodiscard]] float peak(std::size_t index) const noexcept { return peaks_[index]; }
  [[nodiscard]] std::uint64_t position(std::size_t index) const noexcept {
    return positions_[index];
  }
  [[nodiscard]] std::string describe() const {
    if (source_.ahead == nullptr)
      return "no render-ahead";
    const auto status = source_.ahead->status();
    return "window " + std::to_string(status.window_blocks) + ", ahead " +
           std::to_string(status.ahead_blocks) + ", just in time " +
           std::to_string(status.just_in_time_blocks) + ", empty " +
           std::to_string(status.empty_window_blocks) + ", renders ahead " +
           std::to_string(source_.ahead->renders_ahead()) + ", blocks " + std::to_string(blocks());
  }
  [[nodiscard]] std::uint64_t ahead_blocks() const noexcept {
    return source_.ahead == nullptr ? 0 : source_.ahead->status().ahead_blocks;
  }
  [[nodiscard]] std::size_t worst_edit_blocks() const noexcept {
    return source_.ahead == nullptr
               ? 1
               : (source_.ahead->worst_edit_delay_frames() + block - 1) / block;
  }

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  std::atomic<bool> running_{false};
  std::atomic<audio::OutputState> state_{audio::OutputState::stopped};
  std::vector<float> peaks_;
  std::vector<std::uint64_t> positions_;
  std::atomic<std::size_t> blocks_{0};
  std::thread thread_;
};

QString track_id_of(QVariant track) { return track.toMap().value("id").toString(); }

void edits_are_heard_within_the_window() {
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
  // A held note on every beat, looped for the whole run: the track always
  // sounds.
  for (int beat = 0; beat < beats_per_bar; ++beat)
    require(session->notes()
                .add_note_with_duration(clip, {beat * timeline::ticks_per_quarter},
                                        {timeline::ticks_per_quarter}, note_key)
                .has_value(),
            "note");
  require(session->midi_clips().set_midi_clip_loop(clip, true).has_value(), "loop");
  require(session->midi_clips()
              .resize_midi_clip(clip, {bars * beats_per_bar * timeline::ticks_per_quarter})
              .has_value(),
          "the clip did not lengthen");
  const auto track_id = QString::fromStdString(track.value().to_string());

  quick::ProjectBridge bridge(
      directory.path().toStdString(), nullptr,
      [](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
        auto output = std::make_unique<AheadOutput>(source, cursor);
        AheadOutput::current = output.get();
        return output;
      },
      [] { return foundation::Result<std::vector<audio::PipeWireSink>>::success({}); });
  bridge.audio_output()->select_render_ahead(render_ahead_ms);
  bridge.set_project(std::move(session), root);
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return bridge.playback_active() && bridge.routed_graph_current(); },
                          start_wait_ms),
          "playback did not start: " + bridge.playback_status().toStdString());
  auto& output = *AheadOutput::current;
  require(
      QTest::qWaitFor([&] { return output.blocks() > output.worst_edit_blocks() * warm_windows; },
                      edit_wait_ms),
      "the device did not play");

  // The edit reaches the graph; from the first block after that the device
  // must hear it within the window and a block, plus the gain ramp.
  const auto heard_within = [&](std::string_view what, const std::function<void()>& edit,
                                const std::function<bool(float, float)>& heard) {
    const auto before = output.blocks() - 1;
    const auto level = output.peak(before);
    const auto revision = bridge.revision();
    edit();
    require(bridge.revision() > revision,
            std::string(what) + " was refused: " + bridge.status().toStdString());
    require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, edit_wait_ms),
            std::string(what) +
                " never reached the graph: " + bridge.playback_status().toStdString());
    const auto applied = output.blocks();
    const auto bound = applied + output.worst_edit_blocks() + settle_blocks;
    require(QTest::qWaitFor([&] { return output.blocks() > bound; }, edit_wait_ms),
            std::string(what) + ": the device stopped");
    // A routing or clip edit changes no level it can be told by: it must
    // apply while playing, and the playhead check below holds it to no gap.
    if (!heard)
      return;
    std::size_t at = applied;
    while (at <= bound && !heard(level, output.peak(at)))
      ++at;
    if (at > bound) {
      std::string trace;
      for (auto index = before; index < std::min(output.blocks(), bound + bound - before); ++index)
        trace += std::to_string(index) + ":" + std::to_string(output.peak(index)) + " ";
      require(false, std::string(what) + " was not heard within the window and a block (edit " +
                         std::to_string(before) + ", applied " + std::to_string(applied) +
                         ", bound " + std::to_string(bound) + "): " + trace);
    }
    require(bridge.playback_active(), std::string(what) + " stopped playback");
  };
  const auto silent = [](float, float peak) { return peak < silent_peak; };
  const auto audible = [](float, float peak) { return peak > audible_peak; };
  const auto lowered = [](float level, float peak) { return peak < level * lowered_share; };

  // A parameter: the master fader, down and back.
  heard_within("the master fader down", [&] { bridge.set_master_level(0.0); }, silent);
  heard_within("the master fader up", [&] { bridge.set_master_level(1.0); }, audible);
  // A mute.
  heard_within("muting the track", [&] { bridge.set_track_muted(track_id, true); }, silent);
  heard_within("unmuting the track", [&] { bridge.set_track_muted(track_id, false); }, audible);
  // Routing: a send to a return adds the track again, and removing it
  // takes it away.
  const auto tracks_before = bridge.tracks();
  bridge.add_bus_track(QStringLiteral("return"));
  require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, edit_wait_ms),
          "adding a return stopped the graph");
  QString return_id;
  for (const auto& entry : bridge.tracks())
    if (!tracks_before.contains(entry) && track_id_of(entry) != track_id)
      return_id = track_id_of(entry);
  require(!return_id.isEmpty(), "the return did not appear");
  heard_within("a send to the return", [&] { bridge.add_send_route(track_id, return_id, 1.0); },
               {});
  QString send_id;
  for (const auto& entry : bridge.tracks())
    if (track_id_of(entry) == track_id)
      send_id = entry.toMap().value("sends").toList().front().toMap().value("id").toString();
  heard_within("removing the send", [&] { bridge.remove_send(send_id); }, {});
  // Rebalance: the track's role turned down.
  auto* rebalance = qobject_cast<quick::RebalanceController*>(bridge.rebalance());
  require(rebalance != nullptr && !rebalance->roles().isEmpty(), "no rebalance roles");
  auto* role = qobject_cast<quick::RebalanceRole*>(rebalance->roles().front());
  rebalance->setTrackRole(track_id, role->id());
  require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, edit_wait_ms),
          "choosing a role stopped the graph");
  const auto lowest_gain = role->parameter(QStringLiteral("gain")).value("minimum").toDouble();
  heard_within(
      "the role turned down", [&] { role->set_value(QStringLiteral("gain"), lowest_gain); },
      lowered);
  heard_within(
      "the role restored", [&] { role->reset_to_default(QStringLiteral("gain")); },
      [](float, float peak) { return peak > audible_peak; });
  // Automation: the track's level drawn to nothing.
  auto* lanes =
      qobject_cast<quick::ArrangementAutomationController*>(bridge.arrangement_automation());
  require(lanes != nullptr, "no arrangement automation");
  heard_within(
      "level automation",
      [&] { (void)lanes->add_point(track_id, QStringLiteral("level"), 1.0, 0.0); }, silent);
  heard_within("level automation removed", [&] { bridge.undo(); }, audible);
  // A device added and removed: a -6 dB gain stage.
  heard_within(
      "adding a gain stage",
      [&] { bridge.add_effect_preset(track_id, 0, QStringLiteral("trim"), trim_minus_six_preset); },
      [](float level, float peak) { return peak < level * trimmed_share; });
  const auto trim_slot = bridge.insert_chains()
                             .front()
                             .toMap()
                             .value("effects")
                             .toList()
                             .front()
                             .toMap()
                             .value("slotId")
                             .toString();
  heard_within(
      "removing the gain stage", [&] { bridge.remove_effect_insert(track_id, trim_slot); },
      [](float level, float peak) { return peak > level * untrimmed_ratio; });
  // Clip and note edits take effect at their notes; the clip is removed
  // while playing and the device plays on.
  const auto clip_id = QString::fromStdString(clip.to_string());
  heard_within("adding a note", [&] { bridge.add_note(clip_id, 1, note_key); }, {});
  heard_within("deleting the clip", [&] { bridge.delete_midi_clip(clip_id); }, {});

  require(output.ahead_blocks() > output.blocks() / 2,
          "playback did not render ahead: " + output.describe());
  // The device played every block in order: no gap, jump or repeat, and
  // never stopped.
  const auto played = output.blocks();
  for (std::size_t index = 1; index < played; ++index)
    require(output.position(index) == output.position(index - 1) + block,
            "the playhead jumped at block " + std::to_string(index));
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, start_wait_ms), "stop failed");
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    edits_are_heard_within_the_window();
  } catch (const std::exception& error) {
    qCritical("FAILED: %s", error.what());
    return 1;
  }
  return 0;
}
