#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

// Channel pan and mute automation (schema 1.34) drive the routed mixer node:
// export equals live playback exactly, pan moves the image across the
// lane, and mute silences its span after a 5 ms glide without a click.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-channel-automation") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

engine::RoutedMixerLimits limits(std::size_t max_frames) {
  return {{1, 64},
          max_frames,
          6ULL * max_frames * sizeof(engine::StereoFrame),
          5ULL * 65 * sizeof(engine::StereoFrame)};
}

using oscillate::test::read_float_wav;

struct Rendered {
  std::vector<float> exported;
};

Rendered export_nulls_against_live(const fs::path& root, const project::Snapshot& saved,
                                   const fs::path& output) {
  {
    Gate gate;
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(saved, gate));
    REQUIRE_EQ(take(exporting::render_wav_master(saved, store, std::move(authority),
                                                 {output, exporting::WavFormat::float32})),
               exporting::PublishOutcome::durable, "master export published");
  }
  auto exported = read_float_wav(output);
  for (const std::size_t block_frames : {64U, 128U}) {
    Gate gate;
    auto model = take(project::ProjectModel::open(saved, gate));
    app::RoutedPlaybackControl control(model, limits(block_frames));
    auto submitted = control.submit_current();
    REQUIRE_OK(submitted, submitted ? "" : "live graph prepared: " + submitted.error().message);
    auto source =
        take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, block_frames));
    audio::TransportCursor transport;
    std::vector<engine::StereoFrame> stereo(block_frames);
    std::vector<float> samples(2 * block_frames);
    const auto device = source->render_source();
    std::vector<float> live;
    while (live.size() < exported.size()) {
      REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
                 audio::BlockResult::rendered, "live block rendered");
      live.insert(live.end(), samples.begin(), samples.end());
    }
    double largest = 0.0;
    for (std::size_t i = 0; i < exported.size(); ++i)
      largest = std::max(largest, std::fabs(static_cast<double>(exported[i]) - live[i]));
    std::cout << block_frames << " frames: largest difference " << largest << "\n";
    REQUIRE_EQ(largest, 0.0, "offline export nulls against live playback");
  }
  return {std::move(exported)};
}

double power(const std::vector<float>& interleaved, std::size_t channel, std::size_t begin,
             std::size_t end) {
  double sum = 0.0;
  for (std::size_t frame = begin; frame < end; ++frame) {
    const double sample = interleaved[2 * frame + channel];
    sum += sample * sample;
  }
  return sum;
}

void pan_and_mute_lanes_render_like_live() {
  Directory directory;
  const auto root = directory.path() / "song";
  using Key = project::TrackControlTarget::Key;
  const auto saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    const auto track = take(session->instruments().add_synth_track());
    // Four one-bar clips, each holding a note for most of its bar (120 BPM:
    // 2 s a bar).
    for (int bar = 0; bar < 4; ++bar) {
      REQUIRE_OK(session->midi_clips().add_midi_clip(
                     track, {static_cast<std::int64_t>(bar) * 4 * timeline::ticks_per_quarter}),
                 "MIDI clip");
      const auto clip = session->snapshot().tracks.front().midi_clips.back().id;
      require(session->notes()
                  .add_note_with_duration(clip, {0}, {7 * timeline::ticks_per_quarter / 2}, 60)
                  .has_value(),
              "note");
    }
    // Pan hard left at bar 1 to hard right at bar 3; mute bar 2.
    (void)take(session->parameter_automation().set_channel_control_point(track, Key::channel_pan, 0,
                                                                         -1.0));
    (void)take(
        session->parameter_automation().set_channel_control_point(track, Key::channel_pan, 2, 1.0));
    (void)take(session->parameter_automation().set_channel_control_point(track, Key::channel_mute,
                                                                         0, 0.0));
    (void)take(session->parameter_automation().set_channel_control_point(track, Key::channel_mute,
                                                                         1, 1.0));
    (void)take(session->parameter_automation().set_channel_control_point(track, Key::channel_mute,
                                                                         2, 0.0));
    require(!session->parameter_automation().set_channel_control_point(track, Key::channel_mute, 3,
                                                                       0.5),
            "mute points are whole steps");
    require(
        !session->parameter_automation().set_channel_control_point(track, Key::channel_pan, 3, 1.5),
        "pan stays within -1..1");
    REQUIRE_OK(session->saving().save(), "project saved");
    return session->snapshot();
  }();
  const auto rendered = export_nulls_against_live(root, saved, directory.path() / "master.wav");
  const auto& audio = rendered.exported;
  const std::size_t bar = 96'000;
  REQUIRE_GT(audio.size() / 2, 3 * bar, "the export covers the lanes");
  // Bar 1 starts hard left and glides right: the right channel is exactly
  // silent on the first frame and still 30 dB down over the first 2000.
  require(audio[1] == 0.0F && power(audio, 0, 0, 2'000) > 0.0 &&
              power(audio, 1, 0, 2'000) < 1e-3 * power(audio, 0, 0, 2'000),
          "pan starts hard left");
  // Bar 2 is muted after the 240-frame glide.
  require(power(audio, 0, bar + 240, 2 * bar) == 0.0 && power(audio, 1, bar + 240, 2 * bar) == 0.0,
          "the muted bar is silent");
  // The glide: no step larger than a 5 ms ramp of the signal.
  double largest_step = 0.0;
  for (std::size_t frame = bar - 4; frame < bar + 260; ++frame)
    largest_step = std::max(
        largest_step, std::fabs(static_cast<double>(audio[2 * (frame + 1)]) - audio[2 * frame]));
  REQUIRE_LT(largest_step, 0.05, "muting glides without a click");
  // Bar 3 (unmuted, pan past centre towards the right).
  REQUIRE_GT(power(audio, 1, 2 * bar + 4'800, 2 * bar + 48'000),
             power(audio, 0, 2 * bar + 4'800, 2 * bar + 48'000), "pan has moved right by bar 3");
}

void a_mute_lane_overrides_the_switch() {
  Directory directory;
  const auto root = directory.path() / "song";
  using Key = project::TrackControlTarget::Key;
  const auto saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    const auto track = take(session->instruments().add_synth_track());
    REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
    const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
    REQUIRE_OK(session->notes().add_note(clip, {0}, 60), "note");
    REQUIRE_OK(session->tracks().set_track_muted(track, true), "muted switch");
    (void)take(session->parameter_automation().set_channel_control_point(track, Key::channel_mute,
                                                                         0, 0.0));
    REQUIRE_OK(session->saving().save(), "project saved");
    return session->snapshot();
  }();
  const auto rendered = export_nulls_against_live(root, saved, directory.path() / "master.wav");
  REQUIRE_GT(power(rendered.exported, 0, 0, 24'000), 0.0,
             "an unmuting lane plays the switched-off channel");
}

// The arrangement's lanes (ADR-0051): any target, any tick, one undo step per
// gesture, and the result renders like live playback.
void arrangement_lanes_edit_at_any_tick() {
  Directory directory;
  const auto root = directory.path() / "song";
  using Key = project::TrackControlTarget::Key;
  constexpr auto quarter = timeline::ticks_per_quarter;
  const auto saved = [&] {
    auto session = take(app::ProjectSession::create(root));
    const auto track = take(session->instruments().add_synth_track());
    for (int bar = 0; bar < 4; ++bar) {
      REQUIRE_OK(session->midi_clips().add_midi_clip(
                     track, {static_cast<std::int64_t>(bar) * 4 * quarter}),
                 "MIDI clip");
      const auto clip = session->snapshot().tracks.front().midi_clips.back().id;
      REQUIRE_OK(session->notes().add_note_with_duration(clip, {0}, {7 * quarter / 2}, 60), "note");
    }
    const project::AutomationTarget level = project::TrackControlTarget{track, Key::channel_level};
    const auto domain = take(session->automation().automation_domain(track, level));
    REQUIRE_EQ(domain.unit, "linear_gain", "the level lane's domain is the channel's");
    REQUIRE_EQ(domain.minimum, 0.0, "the level lane's domain is the channel's");
    REQUIRE_EQ(domain.maximum, 4.0, "the level lane's domain is the channel's");
    REQUIRE_EQ(domain.default_value, session->snapshot().tracks.front().level,
               "the level lane's domain is the channel's");
    // A fade out across the middle of bar 2, off the bar lines.
    const auto first =
        take(session->automation().add_automation_point(track, level, 5 * quarter + 120, 1.0));
    const auto second =
        take(session->automation().add_automation_point(track, level, 7 * quarter + 360, 0.0));
    const auto& lane = session->snapshot().tracks.front().automation.front();
    REQUIRE_EQ(session->snapshot().tracks.front().automation.size(), 1,
               "points sit at their exact ticks in one lane");
    REQUIRE_EQ(lane.points.size(), 2, "points sit at their exact ticks in one lane");
    REQUIRE_EQ(lane.points[0].coordinate, 5 * quarter + 120,
               "points sit at their exact ticks in one lane");
    REQUIRE_EQ(lane.points[1].coordinate, 7 * quarter + 360,
               "points sit at their exact ticks in one lane");
    REQUIRE_TRUE(!session->automation().add_automation_point(track, level, -1, 0.5),
                 "a negative tick or an out-of-range value is refused");
    REQUIRE_TRUE(!session->automation().add_automation_point(track, level, 0, 4.5),
                 "a negative tick or an out-of-range value is refused");
    // Both points move as one undoable gesture.
    const auto lane_id = lane.id;
    const auto revision = session->snapshot().revision;
    const std::array<app::AutomationService::AutomationPointMove, 2> moves{
        {{first, 4 * quarter, 1.0}, {second, 8 * quarter, 0.0}}};
    REQUIRE_OK(session->automation().move_automation_points(lane_id, moves),
               "a two-point move was not one revision");
    REQUIRE_EQ(session->snapshot().revision, revision + 1, "a two-point move was not one revision");
    const std::array<app::AutomationService::AutomationPointMove, 1> collide{
        {{first, 8 * quarter, 1.0}}};
    require(!session->automation().move_automation_points(lane_id, collide),
            "two points were allowed to share a position");
    // Drawing replaces what lay in its range.
    const std::array<app::AutomationService::AutomationDrawPoint, 3> drawn{
        {{4 * quarter, 1.0}, {6 * quarter, 0.5}, {8 * quarter, 0.25}}};
    REQUIRE_OK(session->automation().draw_automation(track, level, 4 * quarter, 8 * quarter, drawn),
               "drawing failed");
    const auto& redrawn = session->snapshot().tracks.front().automation.front();
    REQUIRE_EQ(redrawn.points.size(), 3, "drawing did not replace the range");
    REQUIRE_EQ(redrawn.points[1].value, 0.5, "drawing did not replace the range");
    REQUIRE_EQ(redrawn.points[2].coordinate, 8 * quarter, "drawing did not replace the range");
    // Removing every point removes the lane; undo brings it back.
    std::vector<project::AutomationPointId> all;
    all.reserve(redrawn.points.size());
    for (const auto& point : redrawn.points)
      all.push_back(point.id);
    REQUIRE_OK(session->automation().remove_automation_points(lane_id, all),
               "removing every point did not remove the lane");
    REQUIRE_TRUE(session->snapshot().tracks.front().automation.empty(),
                 "removing every point did not remove the lane");
    REQUIRE_OK(session->history().undo(), "undo did not restore the drawn lane");
    REQUIRE_EQ(session->snapshot().tracks.front().automation.front().points.size(), 3,
               "undo did not restore the drawn lane");
    // Pan on the same track in its own lane.
    const project::AutomationTarget pan = project::TrackControlTarget{track, Key::channel_pan};
    (void)take(session->automation().add_automation_point(track, pan, 0, -1.0));
    (void)take(session->automation().add_automation_point(track, pan, 12 * quarter, 1.0));
    REQUIRE_EQ(session->snapshot().tracks.front().automation.size(), 2, "pan lane not added");
    REQUIRE_OK(session->saving().save(), "project saved");
    return session->snapshot();
  }();
  const auto rendered = export_nulls_against_live(root, saved, directory.path() / "master.wav");
  const auto& audio = rendered.exported;
  const std::size_t bar = 96'000;
  // Full level before the drawn fade, a quarter of it at its end (bar 3).
  REQUIRE_GT(power(audio, 0, 0, bar / 2), 4.0 * power(audio, 0, 2 * bar + 9'600, 2 * bar + 19'200),
             "the drawn fade did not lower the level");
}

} // namespace

int main() {
  try {
    pan_and_mute_lanes_render_like_live();
    a_mute_lane_overrides_the_switch();
    arrangement_lanes_edit_at_any_tick();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "channel automation contract passed\n";
  return 0;
}
