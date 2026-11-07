#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
namespace prep = oscillate::automation_project;
namespace automation = oscillate::automation;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t value) { return take(T::from_bits({0, value})); }

project::AutomationLane lane(std::uint64_t lane_id, project::TrackId track,
                             project::AutomationTimebase timebase,
                             std::vector<project::AutomationPoint> points) {
  return {id<project::AutomationLaneId>(lane_id),
          project::TrackControlTarget{track, project::TrackControlTarget::Key::channel_level},
          {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0,
           4.0, 1.0, false, 0},
          timebase,
          std::move(points),
          1};
}

project::AutomationPoint point(std::uint64_t point_id, std::int64_t coordinate, double value,
                               project::AutomationShape shape = project::AutomationShape::hold) {
  return {id<project::AutomationPointId>(point_id), coordinate, value, shape, {}};
}

project::Snapshot snapshot() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 17));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Snapshot result{id<project::ProjectId>(1), 7, 48'000, std::move(tempo),
                           std::move(meter),          {}};
  auto track = id<project::TrackId>(2);
  project::Track instrument{track, {}, project::TrackKind::instrument};
  instrument.level = 0.4;
  result.tracks.push_back(std::move(instrument));
  return result;
}

project::MidiClip midi(std::uint64_t clip_id, project::Placement placement,
                       std::vector<project::AutomationLane> lanes) {
  return {id<project::ClipId>(clip_id),
          placement,
          {timeline::ticks_per_quarter},
          false,
          {},
          1,
          std::move(lanes)};
}

double at(const prep::PreparedTargetAutomation& prepared, std::int64_t sample) {
  double output = -1.0;
  require(prepared.evaluate_block({sample}, std::span{&output, 1}), "evaluate sample");
  return output;
}

void near(double actual, double expected, std::string_view message) {
  REQUIRE_LT(std::abs(actual - expected), tolerance::double_accumulation, message);
}

void clip_handoff_and_shared_ticks() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  track.automation.push_back(
      lane(11, track.id, project::AutomationTimebase::elapsed,
           {point(12, 100, 0.0, project::AutomationShape::linear), point(13, 108, 0.8)}));
  track.midi_clips.push_back(
      midi(20, project::ElapsedInterval{{101}, {103}},
           {lane(21, track.id, project::AutomationTimebase::elapsed, {point(22, 0, 0.9)})}));
  track.midi_clips.push_back(
      midi(23, project::ElapsedInterval{{105}, {107}},
           {lane(24, track.id, project::AutomationTimebase::elapsed, {point(25, 0, 0.2)})}));
  auto prepared = take(prep::prepare_project_automation(state, 7));
  REQUIRE_EQ(prepared.size(), 1, "stable prepared identity");
  REQUIRE_EQ(prepared.front().track(), track.id, "stable prepared identity");
  REQUIRE_EQ(prepared.front().target(), track.automation.front().target,
             "stable prepared identity");
  REQUIRE_EQ(prepared.front().snapshot_revision(), 7, "stable prepared identity");
  REQUIRE_EQ(prepared.front().tempo_revision(), 17, "stable prepared identity");
  std::array<double, 9> direct{};
  std::array<double, 9> shared{};
  std::array<timeline::TickPosition, 9> ticks{};
  for (std::size_t index = 0; index < ticks.size(); ++index) {
    ticks[index] = take(
        state.tempo.sample_to_tick({100 + static_cast<std::int64_t>(index)}, state.sample_rate));
  }
  require(prepared.front().evaluate_block({100}, direct), "direct automation block");
  require(prepared.front().evaluate_block({100}, ticks, shared), "shared tick automation block");
  for (std::size_t index = 0; index < direct.size(); ++index) {
    near(direct[index], shared[index], "shared tick and direct evaluation agree");
  }
  near(direct[0], 0.0, "track before clip");
  near(direct[1], 0.9, "first clip begins");
  near(direct[2], 0.9, "first clip remains active");
  near(direct[3], 0.3, "track resumes at clip end");
  near(direct[5], 0.2, "second clip begins");
  near(direct[7], 0.7, "track resumes at second clip end");
  require(!prepared.front().evaluate_block({100}, std::span(ticks).first(8), direct),
          "wrong shared tick block size rejected");
}

void audio_clip_send_gain_precedence_and_saved_fallback() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  track.kind = project::TrackKind::audio;
  track.instrument_source = std::monostate{};
  const auto send = id<project::SendId>(410);
  track.sends.push_back({send, id<project::TrackId>(411), 0.5});
  const project::AutomationDomain domain{project::AutomationRepresentation::physical,
                                         "mixer.send_gain",
                                         "linear",
                                         0.0,
                                         4.0,
                                         1.0,
                                         false,
                                         0,
                                         project::AutomationScale::decibel};
  const auto send_lane = [&](std::uint64_t lane_id, std::vector<project::AutomationPoint> points) {
    return project::AutomationLane{id<project::AutomationLaneId>(lane_id),
                                   project::SendGainTarget{send},
                                   domain,
                                   project::AutomationTimebase::elapsed,
                                   std::move(points),
                                   1};
  };
  track.clips.push_back({id<project::ClipId>(415),
                         id<project::SourceId>(3),
                         {{0}, {2}},
                         project::ElapsedInterval{{1'001}, {1'003}},
                         1,
                         {send_lane(416, {point(417, 0, 0.9)})}});
  auto clip_only = take(prep::prepare_project_automation(state, state.revision));
  near(at(clip_only.front(), 999), 0.5, "saved send gain must be the no-track-curve fallback");
  track.automation.push_back(send_lane(
      412, {point(413, 1'000, 0.0, project::AutomationShape::linear), point(414, 1'008, 0.8)}));
  track.clips.push_back({id<project::ClipId>(418),
                         id<project::SourceId>(3),
                         {{2}, {4}},
                         project::ElapsedInterval{{1'005}, {1'007}},
                         1,
                         {send_lane(419, {point(420, 0, 0.2)})}});
  auto prepared = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_EQ(prepared.size(), 1, "send gain did not bind to stable route ID");
  REQUIRE_EQ(std::get<project::SendGainTarget>(prepared[0].target()).send, send,
             "send gain did not bind to stable route ID");
  std::array<double, 10> gains{};
  require(prepared[0].evaluate_block({999}, gains), "send gain block failed");
  near(gains[0], 0.0, "track curve extrapolates its first point before the clip");
  near(gains[1], 0.0, "track curve begins");
  near(gains[2], 0.9, "first audio clip envelope takes precedence");
  // A gain lane blends in decibels (schema 1.39), silence at -70 dB: from
  // silence to 0.8 over eight samples, three and seven of the way along.
  const auto fader = [](double u) {
    return std::pow(10.0, (-70.0 + (20.0 * std::log10(0.8) + 70.0) * u) / 20.0);
  };
  near(gains[4], fader(3.0 / 8.0), "track curve resumes at first clip end");
  near(gains[6], 0.2, "second audio clip envelope takes precedence");
  near(gains[8], fader(7.0 / 8.0), "track curve resumes at second clip end");
}

void cross_timebase_and_clip_move() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  const auto quarter = timeline::ticks_per_quarter;
  track.midi_clips.push_back(
      midi(30, project::MusicalInterval{{quarter}, {2 * quarter}},
           {lane(31, track.id, project::AutomationTimebase::elapsed,
                 {point(32, 0, 0.0, project::AutomationShape::linear), point(33, 12'000, 1.0)})}));
  track.midi_clips.push_back(
      midi(34, project::ElapsedInterval{{60'000}, {85'000}},
           {lane(35, track.id, project::AutomationTimebase::musical,
                 {point(36, 0, 0.0, project::AutomationShape::linear), point(37, quarter, 1.0)})}));
  auto prepared = take(prep::prepare_project_automation(state, 7));
  near(at(prepared.front(), 24'000), 0.0, "musical clip with elapsed envelope begins");
  near(at(prepared.front(), 30'000), 0.5, "musical clip elapsed envelope follows clip origin");
  near(at(prepared.front(), 60'000), 0.0,
       "later elapsed clip with musical envelope takes precedence");
  near(at(prepared.front(), 72'000), 0.5, "elapsed clip musical envelope follows tempo");
  near(at(prepared.front(), 84'000), 1.0, "elapsed clip musical envelope endpoint");
  near(at(prepared.front(), 85'000), 0.4, "clip end restores channel level");

  track.midi_clips.front().placement = project::MusicalInterval{{0}, {quarter}};
  state.revision = 8;
  auto moved = take(prep::prepare_project_automation(state, 8));
  near(at(moved.front(), 0), 0.0, "moved clip retains local envelope origin");
  near(at(moved.front(), 6'000), 0.5, "moved clip retains local envelope shape");
}

void tempo_ramp_remaps_clip_local_musical_curve() {
  auto state = snapshot();
  const auto track_id = state.tracks.front().id;
  const auto quarter = timeline::ticks_per_quarter;
  state.tracks.front().midi_clips.push_back(
      midi(38, project::ElapsedInterval{{10'000}, {100'000}},
           {lane(39, track_id, project::AutomationTimebase::musical,
                 {point(51, 0, 0.0, project::AutomationShape::linear), point(52, quarter, 1.0)})}));
  auto before = take(prep::prepare_project_automation(state, 7));
  near(at(before.front(), 22'000), 0.5, "constant tempo midpoint");

  state.tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::linear_bpm},
                                                 {{quarter}, 60.0, timeline::TempoShape::step}},
                                                18));
  state.revision = 8;
  auto after = take(prep::prepare_project_automation(state, 8));
  const auto anchor = take(state.tempo.sample_to_tick({10'000}, state.sample_rate));
  const auto midpoint =
      take(state.tempo.tick_to_sample({anchor.value + quarter / 2}, state.sample_rate));
  // The midpoint is rounded to a whole sample, so the curve may be off by up to
  // one sample of its slope: the 0..1 rise spread over the quarter's samples.
  const auto quarter_end =
      take(state.tempo.tick_to_sample({anchor.value + quarter}, state.sample_rate));
  const auto quarter_start = take(state.tempo.tick_to_sample(anchor, state.sample_rate));
  const double one_sample_of_slope =
      1.0 / static_cast<double>(quarter_end.value - quarter_start.value);
  REQUIRE_LT(std::abs(at(after.front(), midpoint.value) - 0.5), one_sample_of_slope,
             "musical local midpoint follows authoritative ramp map");
  REQUIRE_EQ(after.front().tempo_revision(), 18, "new tempo revision prepared");
}

void tempo_steps_and_clip_precedence_are_block_size_independent() {
  auto state = snapshot();
  const auto track_id = state.tracks.front().id;
  // At 48 kHz, 120 BPM advances 40,000 ticks per sample. The first step
  // occurs at sample 64; 240 BPM then advances 80,000 ticks per sample, so
  // the second step and the clip's exclusive end occur at sample 128.
  constexpr auto first_step = std::int64_t{64} * 40'000;
  constexpr auto second_step = first_step + std::int64_t{64} * 80'000;
  state.tempo =
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step},
                                       {{first_step}, 240.0, timeline::TempoShape::step},
                                       {{second_step}, 120.0, timeline::TempoShape::step}},
                                      18));
  REQUIRE_EQ(take(state.tempo.tick_to_sample({first_step}, state.sample_rate)).value, 64,
             "tempo steps must coincide with the chosen sample boundaries");
  REQUIRE_EQ(take(state.tempo.tick_to_sample({second_step}, state.sample_rate)).value, 128,
             "tempo steps must coincide with the chosen sample boundaries");
  auto& track = state.tracks.front();
  track.automation.push_back(
      lane(60, track_id, project::AutomationTimebase::musical,
           {point(61, 0, 0.1), point(62, first_step, 0.4), point(63, second_step, 0.8)}));
  track.midi_clips.push_back(
      midi(64, project::MusicalInterval{{first_step}, {second_step}},
           {lane(65, track_id, project::AutomationTimebase::musical, {point(66, 0, 0.9)})}));
  const auto prepared = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_EQ(prepared.size(), 1, "tempo-boundary target preparation");

  std::array<double, 192> reference{};
  require(prepared.front().evaluate_block({0}, reference), "continuous reference evaluation");
  near(reference[63], 0.1, "track owns sample before tempo and clip boundary");
  near(reference[64], 0.9, "clip owns first sample at tempo step");
  near(reference[127], 0.9, "clip owns its last sample at second tempo step");
  near(reference[128], 0.8, "track resumes on clip's exclusive end sample");

  // 50-sample blocks start inside the clip and the tempo steps (at 64 and 128).
  for (const auto block_size : {std::size_t{50}, std::size_t{64}, std::size_t{128}}) {
    std::array<double, 192> direct{};
    std::array<double, 192> shared{};
    std::array<timeline::TickPosition, 128> ticks{};
    for (std::size_t start = 0; start < direct.size(); start += block_size) {
      const auto count = std::min(block_size, direct.size() - start);
      const auto first = timeline::SamplePosition{static_cast<std::int64_t>(start)};
      require(prepared.front().evaluate_block(first, std::span(direct).subspan(start, count)),
              "split direct evaluation");
      for (std::size_t sample = 0; sample < count; ++sample) {
        ticks[sample] = take(state.tempo.sample_to_tick({static_cast<std::int64_t>(start + sample)},
                                                        state.sample_rate));
      }
      require(prepared.front().evaluate_block(first, std::span(ticks).first(count),
                                              std::span(shared).subspan(start, count)),
              "split shared-tick evaluation");
    }
    for (std::size_t sample = 0; sample < reference.size(); ++sample) {
      near(direct[sample], reference[sample], "automation depends on callback block size");
      near(shared[sample], reference[sample], "shared-tick timing diverges at tempo boundary");
    }
  }
}

void trim_and_extend_reveal_clip_points() {
  auto state = snapshot();
  const auto track_id = state.tracks.front().id;
  state.tracks.front().midi_clips.push_back(
      midi(53, project::ElapsedInterval{{100}, {104}},
           {lane(54, track_id, project::AutomationTimebase::elapsed,
                 {point(55, 0, 0.2), point(56, 6, 0.8)})}));
  auto trimmed = take(prep::prepare_project_automation(state, 7));
  near(at(trimmed.front(), 103), 0.2, "trimmed interval retains first point");
  near(at(trimmed.front(), 106), 0.4, "outside trimmed interval uses channel level");
  state.tracks.front().midi_clips.front().placement = project::ElapsedInterval{{100}, {107}};
  state.revision = 8;
  auto extended = take(prep::prepare_project_automation(state, 8));
  near(at(extended.front(), 106), 0.8, "extension reveals preserved point");
}

void rejects_unavailable_and_invalid() {
  auto state = snapshot();
  REQUIRE_TRUE(!prep::prepare_project_automation(state, 6), "stale revision rejected");
  REQUIRE_EQ(prep::prepare_project_automation(state, 6).error().code,
             foundation::ErrorCode::conflict, "stale revision rejected");
  auto& track = state.tracks.front();
  track.automation.push_back(lane(40, track.id, project::AutomationTimebase::musical,
                                  {point(41, 0, 0.1), point(42, 1, 0.2)}));
  auto collision = prep::prepare_project_automation(state, 7);
  REQUIRE_TRUE(!collision, "same-sample musical points rejected");
  REQUIRE_EQ(collision.error().code, foundation::ErrorCode::invalid_argument,
             "same-sample musical points rejected");

  track.automation.clear();
  track.midi_clips.push_back(
      midi(43,
           project::ElapsedInterval{{std::numeric_limits<std::int64_t>::max() - 10},
                                    {std::numeric_limits<std::int64_t>::max()}},
           {lane(44, track.id, project::AutomationTimebase::elapsed, {point(45, 20, 0.8)})}));
  auto overflow = prep::prepare_project_automation(state, 7);
  REQUIRE_TRUE(!overflow, "clip local coordinate overflow rejected");
  REQUIRE_EQ(overflow.error().code, foundation::ErrorCode::resource_exhausted,
             "clip local coordinate overflow rejected");

  track.midi_clips.clear();
  auto plugin_id = id<project::PluginInstanceId>(48);
  project::AutomationLane plugin_lane{
      id<project::AutomationLaneId>(49),
      project::Vst3ParameterTarget{plugin_id, 17},
      {project::AutomationRepresentation::vst3_normalized, "cutoff", "", 0.0, 1.0, 0.5, false, 0},
      project::AutomationTimebase::elapsed,
      {point(50, 0, 0.5)},
      1};
  track.automation.push_back(plugin_lane);
  auto missing_plugin = prep::prepare_project_automation(state, 7);
  REQUIRE_TRUE(!missing_plugin, "missing VST3 target rejected");
  REQUIRE_EQ(missing_plugin.error().code, foundation::ErrorCode::unavailable,
             "missing VST3 target rejected");
  project::Vst3PluginInstance plugin{plugin_id, "class", "vendor", "name", "1",
                                     "",        "",      {},       {},     true};
  track.effects.push_back({id<project::InsertSlotId>(20), true, plugin});
  REQUIRE_EQ(take(prep::prepare_project_automation(state, 7)).size(), 1,
             "saved VST3 missing flag blocked semantic preparation before fresh scan");
  std::get<project::Vst3PluginInstance>(track.effects.back().processor).missing = false;
  REQUIRE_EQ(take(prep::prepare_project_automation(state, 7)).size(), 1,
             "present VST3 effect target did not prepare normalized automation");
}

void rejects_conflicts_and_bad_curves() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  require(take(prep::prepare_project_automation(state, 7)).empty(),
          "project without automation has no prepared targets");
  auto first = lane(61, track.id, project::AutomationTimebase::elapsed, {point(62, 0, 0.5)});
  track.automation = {
      first, lane(63, track.id, project::AutomationTimebase::elapsed, {point(64, 1, 0.6)})};
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "duplicate track target rejected");

  track.automation = {first};
  auto different = lane(65, track.id, project::AutomationTimebase::elapsed, {point(66, 0, 0.7)});
  different.domain.default_value = 0.7;
  track.midi_clips.push_back(midi(67, project::ElapsedInterval{{100}, {110}}, {different}));
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument,
             "clip and track descriptor conflict rejected");

  track.automation.clear();
  track.midi_clips.front().automation.front().domain.default_value = 1.0;
  track.midi_clips.push_back(
      midi(68, project::ElapsedInterval{{105}, {115}},
           {lane(69, track.id, project::AutomationTimebase::elapsed, {point(70, 0, 0.5)})}));
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::conflict, "overlapping clip envelopes rejected");

  track.midi_clips.pop_back();
  track.midi_clips.front().automation.front().points.front().coordinate = -1;
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::resource_exhausted, "negative clip local coordinate rejected");
  track.midi_clips.front().automation.front().points.front().coordinate = 0;
  track.midi_clips.front().automation.front().points.front().value = 5.0;
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument,
             "out-of-domain point rejected by shared evaluator");
  track.midi_clips.front().automation.front().points.front().value = 0.7;
  track.midi_clips.front().placement = project::ElapsedInterval{{110}, {100}};
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "invalid clip placement rejected");
  track.midi_clips.clear();
  track.automation = {first};
  state.sample_rate = 1;
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "unsupported sample rate rejected");
}

void rejects_malformed_track_and_musical_intervals() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  auto control = lane(71, track.id, project::AutomationTimebase::elapsed, {point(72, 0, 0.5)});
  track.automation = {control};
  track.automation.front().points.push_back(point(73, 0, 0.6));
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "duplicate track coordinates rejected");
  track.automation.front().points.pop_back();
  track.automation.front().points.front().value = 5.0;
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument,
             "invalid track curve rejected by shared evaluator");
  track.automation.front().points.front().value = 0.5;
  track.automation.front().target = project::TrackControlTarget{
      id<project::TrackId>(99), project::TrackControlTarget::Key::channel_level};
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "cross-track target rejected");
  track.automation.front().target = control.target;
  // Deliberately model a malformed decoded enum that bypassed codec validation.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  track.automation.front().timebase = static_cast<project::AutomationTimebase>(255);
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "unknown timebase rejected");

  track.automation.clear();
  auto clip_lane = lane(74, track.id, project::AutomationTimebase::musical, {point(75, 0, 0.5)});
  track.midi_clips.push_back(midi(76, project::MusicalInterval{{0}, {0}}, {clip_lane}));
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "invalid musical placement rejected");
  track.midi_clips.front().placement = project::MusicalInterval{{0}, {1}};
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::invalid_argument, "sub-sample musical placement rejected");
}

void resolves_distinct_targets_and_rejects_audio_clip_binding() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  track.automation.push_back(
      lane(77, track.id, project::AutomationTimebase::elapsed, {point(78, 0, 0.6)}));
  const auto plugin = id<project::PluginInstanceId>(79);
  const auto vst_lane = [&](std::uint64_t lane_id, std::uint32_t parameter_id) {
    return project::AutomationLane{
        id<project::AutomationLaneId>(lane_id),
        project::Vst3ParameterTarget{plugin, parameter_id},
        {project::AutomationRepresentation::vst3_normalized, "cutoff", "", 0.0, 1.0, 0.5, false, 0},
        project::AutomationTimebase::elapsed,
        {point(lane_id + 100, 0, 0.5)},
        1};
  };
  track.automation.push_back(vst_lane(80, 2));
  track.automation.push_back(vst_lane(81, 1));
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::unavailable,
             "stable VST3 parameter IDs remain distinct and unavailable");
  track.automation.clear();
  track.clips.emplace_back(id<project::ClipId>(82), id<project::SourceId>(83),
                           project::SourceRange{{0}, {10}}, project::ElapsedInterval{{0}, {10}}, 1,
                           std::vector<project::AutomationLane>{vst_lane(84, 3)});
  REQUIRE_EQ(prep::prepare_project_automation(state, 7).error().code,
             foundation::ErrorCode::unavailable,
             "unbacked audio clip lane is not silently ignored");
}

void vst3_effect_clip_precedence_and_saved_baseline() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  track.kind = project::TrackKind::audio;
  track.instrument_source = std::monostate{};
  const auto plugin_id = id<project::PluginInstanceId>(90);
  project::Vst3PluginInstance plugin{plugin_id, "class", "vendor", "gain",        "1",
                                     "",        "",      {},       {{101, 0.35}}, false};
  track.effects.push_back({id<project::InsertSlotId>(91), true, plugin});
  const auto gain_lane = [&](std::uint64_t lane_id, std::vector<project::AutomationPoint> points) {
    return project::AutomationLane{
        id<project::AutomationLaneId>(lane_id),
        project::Vst3ParameterTarget{plugin_id, 101},
        {project::AutomationRepresentation::vst3_normalized, "gain", "", 0.0, 1.0, 0.5, false, 0},
        project::AutomationTimebase::elapsed,
        std::move(points),
        1};
  };
  track.clips.push_back({id<project::ClipId>(92),
                         id<project::SourceId>(93),
                         {{0}, {2}},
                         project::ElapsedInterval{{101}, {103}},
                         1,
                         {gain_lane(94, {point(95, 0, 0.9)})}});
  auto clip_only = take(prep::prepare_project_automation(state, state.revision));
  near(at(clip_only.front(), 100), 0.35, "saved static parameter is baseline before clip");
  near(at(clip_only.front(), 101), 0.9, "audio clip VST3 envelope takes precedence");
  near(at(clip_only.front(), 103), 0.35, "saved parameter resumes after clip");
  track.automation.push_back(
      gain_lane(96, {point(97, 100, 0.0, project::AutomationShape::linear), point(98, 108, 0.8)}));
  auto prepared = take(prep::prepare_project_automation(state, state.revision));
  std::array<double, 5> output{};
  REQUIRE_EQ(prepared.size(), 1, "VST3 effect block did not prepare");
  REQUIRE_TRUE(prepared.front().evaluate_block({100}, output), "VST3 effect block did not prepare");
  near(output[0], 0.0, "VST3 track curve begins");
  near(output[1], 0.9, "VST3 clip envelope wins");
  near(output[2], 0.9, "VST3 clip envelope remains active");
  near(output[3], 0.3, "VST3 track curve resumes at current value");
  const auto semantics_key = prepared.front().semantics_key();
  ++state.revision;
  auto unrelated_revision = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_EQ(unrelated_revision.front().semantics_key(), semantics_key,
             "unrelated project revision changed VST3 automation runtime identity");
  track.automation.front().points.back().value = 0.7;
  ++track.automation.front().settings_generation;
  auto changed_curve = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_NE(changed_curve.front().semantics_key(), semantics_key,
             "changed VST3 curve reused stale automation runtime identity");
  track.effects.front().enabled = false;
  REQUIRE_EQ(prep::prepare_project_automation(state, state.revision).error().code,
             foundation::ErrorCode::unavailable,
             "bypassed VST3 effect did not reject an active automation lane");
}

void native_parameter_baseline_precedence_and_binding() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  track.instrument_source =
      project::NativeInstrumentPatch{"prism", 1, {{"prism.filter.cutoff", 1'500.0}}};
  const auto native_lane = [&](std::uint64_t lane_id, std::string parameter,
                               std::vector<project::AutomationPoint> points) {
    return project::AutomationLane{id<project::AutomationLaneId>(lane_id),
                                   project::NativeParameterTarget{track.id, parameter},
                                   {project::AutomationRepresentation::physical, parameter, "Hz",
                                    20.0, 20'000.0, 2'400.0, false, 0},
                                   project::AutomationTimebase::elapsed,
                                   std::move(points),
                                   1};
  };
  // A MIDI clip lane alone falls back to the saved patch value, and an id the
  // sparse patch omits falls back to the domain (engine) default.
  track.midi_clips.push_back(
      midi(30, project::ElapsedInterval{{2'000}, {2'010}},
           {native_lane(31, "prism.filter.cutoff", {point(32, 0, 300.0)}),
            native_lane(33, "prism.filter.resonance", {point(34, 0, 900.0)})}));
  auto prepared = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_EQ(prepared.size(), 2, "each native parameter is its own target");
  const auto* cutoff = &prepared[0];
  const auto* other = &prepared[1];
  if (std::get<project::NativeParameterTarget>(cutoff->target()).parameter_id !=
      "prism.filter.cutoff") {
    std::swap(cutoff, other);
  }
  near(at(*cutoff, 1'999), 1'500.0, "saved native value is the unautomated baseline");
  near(at(*cutoff, 2'000), 300.0, "clip envelope takes precedence");
  near(at(*other, 1'999), 2'400.0, "omitted parameters use the domain default");

  auto orphan = state;
  orphan.tracks.front().instrument_source = std::monostate{};
  require(!prep::prepare_project_automation(orphan, orphan.revision),
          "a native lane without a built-in instrument is unavailable");
  auto mismatched = state;
  mismatched.tracks.front().midi_clips.front().automation.front().domain.semantic_key = "x";
  require(!prep::prepare_project_automation(mismatched, mismatched.revision),
          "the domain must name its parameter");
  // Stepped native lanes (ADR-0034): a discrete domain of held whole steps
  // evaluates as a staircase; a fractional step is rejected.
  auto stepped = state;
  auto& wave_track = stepped.tracks.front();
  wave_track.automation.push_back(project::AutomationLane{
      id<project::AutomationLaneId>(40),
      project::NativeParameterTarget{wave_track.id, "prism.osc1.wave"},
      {project::AutomationRepresentation::physical, "prism.osc1.wave", "", 0.0, 4.0, 0.0, true, 4},
      project::AutomationTimebase::elapsed,
      {point(41, 100, 1.0), point(42, 104, 3.0)},
      1});
  auto stairs = take(prep::prepare_project_automation(stepped, stepped.revision));
  const auto wave = std::ranges::find_if(stairs, [](const auto& target) {
    return std::get<project::NativeParameterTarget>(target.target()).parameter_id ==
           "prism.osc1.wave";
  });
  REQUIRE_NE(wave, stairs.end(), "a stepped native lane prepares");
  std::array<double, 8> steps{};
  require(wave->evaluate_block({100}, steps), "the stepped lane evaluates");
  REQUIRE_EQ(steps[0], 1.0, "stepped lanes hold whole values between points");
  REQUIRE_EQ(steps[3], 1.0, "stepped lanes hold whole values between points");
  REQUIRE_EQ(steps[4], 3.0, "stepped lanes hold whole values between points");
  REQUIRE_EQ(steps[7], 3.0, "stepped lanes hold whole values between points");
  wave_track.automation.back().points.back().value = 2.5;
  require(!prep::prepare_project_automation(stepped, stepped.revision),
          "a stepped lane rejects fractional values");
  auto out_of_range = state;
  std::get<project::NativeInstrumentPatch>(out_of_range.tracks.front().instrument_source)
      .parameters.front()
      .value = 50'000.0;
  require(!prep::prepare_project_automation(out_of_range, out_of_range.revision),
          "a saved value outside the lane domain is rejected");
}

// Hand-worked macro mappings: a linear blend, an inverted one, rounding to a
// discrete domain's steps (across the domain range) and clamping into it.
void macro_values_blend_round_and_clamp() {
  const auto mapping = [](double minimum, double maximum, double domain_minimum,
                          double domain_maximum, std::uint32_t steps) {
    return project::MacroMapping{project::NativeParameterTarget{snapshot().tracks.front().id, "p"},
                                 {project::AutomationRepresentation::physical, "p", "",
                                  domain_minimum, domain_maximum, domain_minimum, steps != 0,
                                  steps},
                                 minimum,
                                 maximum};
  };
  // 0.25 of the way from 100 to 300.
  near(prep::macro_parameter_value(mapping(100.0, 300.0, 0.0, 1'000.0, 0), 0.25), 150.0,
       "linear macro blend");
  // Inverted: 0.25 of the way from 300 down to 100.
  near(prep::macro_parameter_value(mapping(300.0, 100.0, 0.0, 1'000.0, 0), 0.25), 250.0,
       "inverted macro blend");
  // Domain 1..16 in 15 steps: 0.5 of 1..16 is 8.5, which rounds to the step 9.
  near(prep::macro_parameter_value(mapping(1.0, 16.0, 1.0, 16.0, 15), 0.5), 9.0,
       "discrete macro rounds to a domain step");
  // A macro value outside 0..1 is clamped before blending.
  near(prep::macro_parameter_value(mapping(100.0, 300.0, 0.0, 1'000.0, 0), 2.0), 300.0,
       "macro value clamped to 0..1");
  // A span past the double range is not stepped, so the result stays finite.
  const double huge = std::numeric_limits<double>::max();
  REQUIRE_TRUE(
      std::isfinite(prep::macro_parameter_value(mapping(-huge, huge, -huge, huge, 2), 0.25)),
      "an overflowing discrete span maps to a finite value");
}

// A macro lane on a track with a built-in instrument and a built-in effect:
// two mappings, the instrument's cutoff and the effect's mix.
constexpr double cutoff_minimum = 20.0;
constexpr double cutoff_maximum = 20'000.0;
constexpr double cutoff_default = 2'400.0;
constexpr double macro_low = 100.0;  // The cutoff at macro 0.
constexpr double macro_high = 300.0; // The cutoff at macro 1.
constexpr double macro_lane_value = 0.25;
constexpr std::int64_t macro_point_sample = 0;
constexpr std::int64_t macro_probe_sample = 10;

project::AutomationDomain cutoff_domain() {
  return {project::AutomationRepresentation::physical,
          "prism.filter.cutoff",
          "Hz",
          cutoff_minimum,
          cutoff_maximum,
          cutoff_default,
          false,
          0};
}

project::AutomationDomain mix_domain() {
  return {project::AutomationRepresentation::physical, "mix", "", 0.0, 1.0, 1.0, false, 0};
}

project::AutomationLane macro_lane(project::TrackId track, std::uint32_t index) {
  return {id<project::AutomationLaneId>(120),
          project::MacroTarget{track, index},
          {project::AutomationRepresentation::physical, "macro", "", 0.0, 1.0, 0.0, false, 0},
          project::AutomationTimebase::elapsed,
          {point(121, macro_point_sample, macro_lane_value)},
          1};
}

project::Snapshot macro_snapshot() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  track.instrument_source = project::NativeInstrumentPatch{"prism", 1, {}};
  const auto slot = id<project::InsertSlotId>(122);
  track.effects.push_back({slot, true, project::NativeEffectPatch{"filter", 1, {}}});
  project::Macro macro;
  macro.mappings.push_back({project::NativeParameterTarget{track.id, "prism.filter.cutoff"},
                            cutoff_domain(), macro_low, macro_high});
  macro.mappings.push_back(
      {project::NativeEffectParameterTarget{slot, "mix"}, mix_domain(), 0.0, 1.0});
  track.macros.push_back(std::move(macro));
  track.automation.push_back(macro_lane(track.id, 0));
  return state;
}

const prep::PreparedTargetAutomation&
prepared_for(const std::vector<prep::PreparedTargetAutomation>& prepared,
             const project::AutomationTarget& target) {
  const auto found = std::ranges::find(prepared, target, &prep::PreparedTargetAutomation::target);
  REQUIRE_NE(found, prepared.end(), "a prepared lane drives the target");
  return *found;
}

// A macro lane becomes one prepared lane per mapped parameter, in that
// parameter's units, and never a lane on the macro itself.
void macro_lane_drives_each_mapped_parameter() {
  auto state = macro_snapshot();
  const auto& track = state.tracks.front();
  const project::AutomationTarget cutoff =
      project::NativeParameterTarget{track.id, "prism.filter.cutoff"};
  const project::AutomationTarget mix =
      project::NativeEffectParameterTarget{track.effects.front().id, "mix"};
  auto prepared = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_EQ(prepared.size(), 2, "one prepared lane per mapped parameter");
  REQUIRE_TRUE(std::ranges::none_of(prepared,
                                    [](const prep::PreparedTargetAutomation& lane) {
                                      return std::holds_alternative<project::MacroTarget>(
                                          lane.target());
                                    }),
               "no prepared lane targets the macro");
  // 0.25 of the way from 100 to 300, and 0.25 of the mix's 0..1.
  near(at(prepared_for(prepared, cutoff), macro_probe_sample),
       macro_low + macro_lane_value * (macro_high - macro_low),
       "the macro lane drives the cutoff in its units");
  near(at(prepared_for(prepared, mix), macro_probe_sample), macro_lane_value,
       "the macro lane drives the effect's mix");
  REQUIRE_NE(prepared_for(prepared, cutoff).semantics_key(),
             prepared_for(prepared, mix).semantics_key(),
             "each destination has its own runtime identity");
}

// The runtime identity of a macro-driven lane follows its mapping, and only
// its own mapping.
void macro_mapping_change_changes_semantics_key() {
  auto state = macro_snapshot();
  auto& track = state.tracks.front();
  const project::AutomationTarget cutoff =
      project::NativeParameterTarget{track.id, "prism.filter.cutoff"};
  const project::AutomationTarget mix =
      project::NativeEffectParameterTarget{track.effects.front().id, "mix"};
  const auto before = take(prep::prepare_project_automation(state, state.revision));
  constexpr double wider_high = 500.0;
  track.macros.front().mappings.front().maximum = wider_high;
  const auto after = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_NE(prepared_for(after, cutoff).semantics_key(),
             prepared_for(before, cutoff).semantics_key(),
             "a changed mapping reused a stale runtime identity");
  REQUIRE_EQ(prepared_for(after, mix).semantics_key(), prepared_for(before, mix).semantics_key(),
             "an unchanged mapping lost its runtime identity");
  near(at(prepared_for(after, cutoff), macro_probe_sample),
       macro_low + macro_lane_value * (wider_high - macro_low),
       "the changed mapping drives the cutoff");
  track.macros.front().mappings.front().domain.scale = project::AutomationScale::logarithmic;
  const auto rescaled = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_NE(prepared_for(rescaled, cutoff).semantics_key(),
             prepared_for(after, cutoff).semantics_key(),
             "a changed mapping scale reused a stale runtime identity");
}

void macro_lane_rejects_missing_macro_and_conflicts() {
  auto out_of_range = macro_snapshot();
  auto& macro_track = out_of_range.tracks.front();
  macro_track.automation.front().target =
      project::MacroTarget{macro_track.id, static_cast<std::uint32_t>(macro_track.macros.size())};
  REQUIRE_EQ(prep::prepare_project_automation(out_of_range, out_of_range.revision).error().code,
             foundation::ErrorCode::invalid_argument, "a lane on a missing macro is rejected");

  // The mapped cutoff also has a lane of its own, on the track or in a clip.
  auto track_lane = macro_snapshot();
  auto& track = track_lane.tracks.front();
  const auto cutoff_lane =
      project::AutomationLane{id<project::AutomationLaneId>(123),
                              project::NativeParameterTarget{track.id, "prism.filter.cutoff"},
                              cutoff_domain(),
                              project::AutomationTimebase::elapsed,
                              {point(124, 0, cutoff_default)},
                              1};
  track.automation.push_back(cutoff_lane);
  REQUIRE_EQ(prep::prepare_project_automation(track_lane, track_lane.revision).error().code,
             foundation::ErrorCode::conflict,
             "a macro-mapped parameter with its own track lane is rejected");
  auto clip_lane = macro_snapshot();
  clip_lane.tracks.front().midi_clips.push_back(
      midi(125, project::ElapsedInterval{{0}, {10}}, {cutoff_lane}));
  REQUIRE_EQ(prep::prepare_project_automation(clip_lane, clip_lane.revision).error().code,
             foundation::ErrorCode::conflict,
             "a macro-mapped parameter with its own clip lane is rejected");

  // A second macro lane mapping the same parameter.
  auto two_macros = macro_snapshot();
  auto& doubled = two_macros.tracks.front();
  doubled.macros.push_back(doubled.macros.front());
  auto second = macro_lane(doubled.id, 1);
  second.id = id<project::AutomationLaneId>(126);
  doubled.automation.push_back(second);
  REQUIRE_EQ(prep::prepare_project_automation(two_macros, two_macros.revision).error().code,
             foundation::ErrorCode::conflict, "two macro lanes on one parameter are rejected");

  auto foreign = macro_snapshot();
  foreign.tracks.front().macros.front().mappings.front().destination =
      project::NativeParameterTarget{id<project::TrackId>(127), "prism.filter.cutoff"};
  REQUIRE_EQ(prep::prepare_project_automation(foreign, foreign.revision).error().code,
             foundation::ErrorCode::invalid_argument,
             "a mapping to another track's parameter is rejected");
}

// A built-in effect's parameter lane: the saved value is the unautomated
// baseline, a clip envelope takes precedence, and the lane must match a
// built-in effect on its track.
void native_effect_parameter_baseline_and_binding() {
  constexpr double saved_cutoff = 800.0;
  constexpr double clip_cutoff = 2'000.0;
  constexpr std::int64_t clip_start = 101;
  constexpr std::int64_t clip_end = 103;
  auto state = snapshot();
  auto& track = state.tracks.front();
  track.kind = project::TrackKind::audio;
  track.instrument_source = std::monostate{};
  const auto slot = id<project::InsertSlotId>(130);
  track.effects.push_back(
      {slot, true,
       project::NativeEffectPatch{"filter", 1, {{"prism.filter.cutoff", saved_cutoff}}}});
  const auto effect_lane = [&](std::uint64_t lane_id, std::string parameter) {
    auto domain = cutoff_domain();
    domain.semantic_key = parameter;
    return project::AutomationLane{id<project::AutomationLaneId>(lane_id),
                                   project::NativeEffectParameterTarget{slot, parameter},
                                   std::move(domain),
                                   project::AutomationTimebase::elapsed,
                                   {point(lane_id + 1, 0, clip_cutoff)},
                                   1};
  };
  track.clips.push_back(
      {id<project::ClipId>(131),
       id<project::SourceId>(132),
       {{0}, {clip_end - clip_start}},
       project::ElapsedInterval{{clip_start}, {clip_end}},
       1,
       {effect_lane(133, "prism.filter.cutoff"), effect_lane(135, "prism.filter.resonance")}});
  auto prepared = take(prep::prepare_project_automation(state, state.revision));
  REQUIRE_EQ(prepared.size(), 2, "each effect parameter is its own target");
  const project::AutomationTarget cutoff_target =
      project::NativeEffectParameterTarget{slot, "prism.filter.cutoff"};
  const project::AutomationTarget resonance_target =
      project::NativeEffectParameterTarget{slot, "prism.filter.resonance"};
  const auto& cutoff = prepared_for(prepared, cutoff_target);
  const auto& resonance = prepared_for(prepared, resonance_target);
  near(at(cutoff, clip_start - 1), saved_cutoff, "saved effect value is the baseline");
  near(at(cutoff, clip_start), clip_cutoff, "the clip envelope takes precedence");
  near(at(cutoff, clip_end), saved_cutoff, "the saved value resumes after the clip");
  near(at(resonance, clip_start - 1), cutoff_default,
       "an id the sparse patch omits uses the domain default");

  const auto rejects = [&](auto change, foundation::ErrorCode code, std::string_view message) {
    auto broken = state;
    change(broken.tracks.front());
    const auto result = prep::prepare_project_automation(broken, broken.revision);
    REQUIRE_TRUE(!result, message);
    REQUIRE_EQ(result.error().code, code, message);
  };
  rejects([](project::Track& changed) { changed.effects.clear(); },
          foundation::ErrorCode::unavailable, "a lane on a missing effect slot is rejected");
  rejects(
      [](project::Track& changed) {
        changed.effects.front().processor =
            project::Vst3PluginInstance{id<project::PluginInstanceId>(137),
                                        "class",
                                        "vendor",
                                        "name",
                                        "1",
                                        "",
                                        "",
                                        {},
                                        {},
                                        false};
      },
      foundation::ErrorCode::unavailable, "a lane on a VST3 slot is not a built-in effect lane");
  rejects(
      [](project::Track& changed) {
        changed.clips.front().automation.front().domain.semantic_key = "other";
      },
      foundation::ErrorCode::invalid_argument, "the domain must name its parameter");
  rejects(
      [](project::Track& changed) {
        changed.clips.front().automation.front().domain.representation =
            project::AutomationRepresentation::vst3_normalized;
      },
      foundation::ErrorCode::invalid_argument, "an effect lane must be physical");
  rejects(
      [](project::Track& changed) {
        constexpr double beyond_domain = 50'000.0;
        std::get<project::NativeEffectPatch>(changed.effects.front().processor)
            .parameters.front()
            .value = beyond_domain;
      },
      foundation::ErrorCode::invalid_argument, "a saved value outside the domain is rejected");
}

// A send-gain lane must name a send on its track and carry the send-gain
// domain exactly.
void send_gain_validation_failures() {
  auto state = snapshot();
  auto& track = state.tracks.front();
  const auto send = id<project::SendId>(140);
  track.sends.push_back({send, id<project::TrackId>(141), 0.5});
  track.automation.push_back({id<project::AutomationLaneId>(142),
                              project::SendGainTarget{send},
                              {project::AutomationRepresentation::physical, "mixer.send_gain",
                               "linear", 0.0, 4.0, 1.0, false, 0},
                              project::AutomationTimebase::elapsed,
                              {point(143, 0, 1.0)},
                              1});
  REQUIRE_EQ(take(prep::prepare_project_automation(state, state.revision)).size(), 1,
             "a well-formed send-gain lane prepares");
  const auto rejects = [&](auto change, std::string_view message) {
    auto broken = state;
    change(broken.tracks.front());
    const auto result = prep::prepare_project_automation(broken, broken.revision);
    REQUIRE_TRUE(!result, message);
    REQUIRE_EQ(result.error().code, foundation::ErrorCode::invalid_argument, message);
  };
  rejects([](project::Track& changed) { changed.sends.clear(); },
          "a lane on a missing send is rejected");
  rejects([](project::Track& changed) { changed.automation.front().domain.semantic_key = "gain"; },
          "a send-gain lane must use the send-gain key");
  rejects(
      [](project::Track& changed) {
        changed.automation.front().domain.representation =
            project::AutomationRepresentation::vst3_normalized;
      },
      "a send-gain lane must be physical");
  rejects(
      [](project::Track& changed) {
        constexpr double narrower_maximum = 2.0;
        changed.automation.front().domain.maximum = narrower_maximum;
      },
      "a send-gain lane must span the send-gain range");
  rejects(
      [](project::Track& changed) {
        constexpr std::uint32_t steps = 4;
        changed.automation.front().domain.discrete = true;
        changed.automation.front().domain.step_count = steps;
      },
      "a send-gain lane must be continuous");
}

} // namespace

int main() {
  try {
    clip_handoff_and_shared_ticks();
    audio_clip_send_gain_precedence_and_saved_fallback();
    cross_timebase_and_clip_move();
    tempo_ramp_remaps_clip_local_musical_curve();
    tempo_steps_and_clip_precedence_are_block_size_independent();
    trim_and_extend_reveal_clip_points();
    rejects_unavailable_and_invalid();
    rejects_conflicts_and_bad_curves();
    rejects_malformed_track_and_musical_intervals();
    resolves_distinct_targets_and_rejects_audio_clip_binding();
    vst3_effect_clip_precedence_and_saved_baseline();
    native_parameter_baseline_precedence_and_binding();
    macro_values_blend_round_and_clamp();
    macro_lane_drives_each_mapped_parameter();
    macro_mapping_change_changes_semantics_key();
    macro_lane_rejects_missing_macro_and_conflicts();
    native_effect_parameter_baseline_and_binding();
    send_gain_validation_failures();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
