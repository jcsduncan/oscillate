#include "oscillate/project/clip_fade.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace tolerance = oscillate::test::tolerance;

namespace {
using namespace oscillate;
using namespace oscillate::project;

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

struct Gate final : TransportGate {
  struct Lease final : StoppedTransportLease {};
  foundation::Result<std::unique_ptr<StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

void fade_gains_have_exact_boundaries_and_curves() {
  auto prepared = take(prepare_clip_fades({.in_fraction = 0.5,
                                           .out_fraction = 0.25,
                                           .in_curve = FadeCurve::linear,
                                           .out_curve = FadeCurve::smoothstep},
                                          8));
  REQUIRE_EQ(prepared.in_frames, 4, "fade lengths changed");
  REQUIRE_EQ(prepared.out_frames, 2, "fade lengths changed");
  const double expected[]{0.0, 0.25, 0.5, 0.75, 1.0, 1.0, 0.5, 0.0};
  for (std::uint64_t frame = 0; frame < 8; ++frame)
    REQUIRE_LT(std::abs(clip_fade_gain(prepared, frame) - expected[frame]),
               tolerance::double_rounding,
               "fade sample gain differs from hand-calculated envelope");
  REQUIRE_EQ(clip_fade_gain(prepared, 8), 0.0, "gain outside clip is audible");
  auto rounded = take(prepare_clip_fades({.in_fraction = 0.5, .out_fraction = 0.5}, 3));
  REQUIRE_EQ(rounded.in_frames, 2, "ties-to-even or non-overlap fade rounding changed");
  REQUIRE_EQ(rounded.out_frames, 1, "ties-to-even or non-overlap fade rounding changed");
  REQUIRE_TRUE(!prepare_clip_fades({.in_fraction = 0.1}, 1), "invalid fade settings were prepared");
  REQUIRE_TRUE(!prepare_clip_fades({.in_fraction = 0.0}, 0), "invalid fade settings were prepared");
  REQUIRE_TRUE(!prepare_clip_fades({.in_fraction = std::numeric_limits<double>::quiet_NaN()}, 8),
               "invalid fade settings were prepared");
  REQUIRE_TRUE(!prepare_clip_fades({.in_fraction = 0.6, .out_fraction = 0.5}, 8),
               "invalid fade settings were prepared");
}

void fade_command_is_atomic_undoable_and_scales_with_clip() {
  Gate gate;
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  auto model = take(
      ProjectModel::create(id<ProjectId>(1), 48'000, std::move(tempo), std::move(meter), gate));
  const auto track = id<TrackId>(2);
  const auto clip = id<ClipId>(3);
  REQUIRE_OK(model.apply(
      {AddTrack{track},
       AddClip{track, Clip{clip, id<SourceId>(4), {{0}, {8}}, ElapsedInterval{{0}, {8}}}}}));
  const ClipFades fades{0.5, 0.25, FadeCurve::linear, FadeCurve::smoothstep};
  auto applied = take(model.apply({SetClipFades{clip, fades}}));
  REQUIRE_EQ(applied.snapshot->tracks.front().clips.front().fades, fades,
             "fade edit did not publish clip instance settings");
  REQUIRE_EQ(applied.snapshot->tracks.front().clips.front().settings_generation, 2,
             "fade edit did not publish clip instance settings");
  const auto after = model.snapshot();
  REQUIRE_TRUE(!model.apply({SetClipFades{clip, {.in_fraction = 0.8, .out_fraction = 0.3}}}),
               "invalid fade edit changed project state");
  REQUIRE_EQ(model.snapshot(), after, "invalid fade edit changed project state");
  REQUIRE_TRUE(!model.apply({SetClipPlacement{clip, ElapsedInterval{{0}, {1}}}}),
               "one-frame resize silently retained an invalid fade");
  REQUIRE_EQ(model.snapshot(), after, "one-frame resize silently retained an invalid fade");
  auto reopened = take(ProjectModel::open(*after, gate));
  REQUIRE_EQ(reopened.snapshot()->tracks.front().clips.front().fades, fades,
             "fade settings did not survive model rehydration");
  REQUIRE_EQ(take(model.undo()).snapshot->tracks.front().clips.front().fades, ClipFades{},
             "fade edit was not one reversible operation");
  REQUIRE_EQ(take(model.redo()).snapshot->tracks.front().clips.front().fades, fades,
             "fade edit was not one reversible operation");
}

} // namespace

int main() {
  try {
    fade_gains_have_exact_boundaries_and_curves();
    fade_command_is_atomic_undoable_and_scales_with_clip();
    std::cout << "Clip fade contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
