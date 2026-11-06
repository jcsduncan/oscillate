// Thread-CPU benchmark for warp streams. Run from an uninstrumented Release
// build; records cost as a percentage of real time per stereo stream.
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/rate_view.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <time.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace warp = oscillate::warp_dsp;

double thread_seconds() {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
}

const char* tier_name(warp::QualityTier tier) {
  switch (tier) {
  case warp::QualityTier::q0:
    return "Q0";
  case warp::QualityTier::q1:
    return "Q1";
  case warp::QualityTier::q2:
    return "Q2";
  }
  return "?";
}

int run() {
  constexpr std::uint32_t rate = 48'000;
  constexpr std::uint64_t output_frames = rate * 30ULL;
  std::vector<float> left(rate * 30ULL * 5);
  std::vector<float> right(left.size());
  std::uint32_t state = 1;
  for (std::size_t index = 0; index < left.size(); ++index) {
    state = state * 1664525U + 1013904223U;
    left[index] = static_cast<float>(state >> 8) / 16777216.0F - 0.5F;
    right[index] = left[index] * 0.7F;
  }
  warp::MemorySourceView source(left, right);
  {
    // 30 s of noise exercises the full analysis cost (dense flux everywhere).
    warp::MemorySourceView window(std::span<const float>(left).first(output_frames),
                                  std::span<const float>(right).first(output_frames));
    const double started = thread_seconds();
    const auto found = warp::analysis::detect_transients(window, {rate, 1.0F, 0.03});
    const double elapsed = thread_seconds() - started;
    if (!found)
      return 1;
    std::printf("transients,48000,%.1fx_realtime\n",
                (static_cast<double>(output_frames) / rate) / elapsed);
    // Pitch track plus marks, as a Tones render runs them, on a voiced tone.
    std::vector<float> tone_left(output_frames);
    std::vector<float> tone_right(output_frames);
    for (std::size_t index = 0; index < tone_left.size(); ++index) {
      tone_left[index] = static_cast<float>(
          0.4 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(index) / rate));
      tone_right[index] = tone_left[index];
    }
    warp::MemorySourceView voiced(tone_left, tone_right);
    const double pitch_started = thread_seconds();
    const auto track = warp::analysis::track_pitch(voiced, {.sample_rate = rate});
    if (!track)
      return 1;
    const auto marks = warp::analysis::place_pitch_marks(voiced, track.value(), rate);
    if (!marks || marks.value().empty())
      return 1;
    const double pitch_elapsed = thread_seconds() - pitch_started;
    std::printf("pitch_marks,48000,%.1fx_realtime\n",
                (static_cast<double>(output_frames) / rate) / pitch_elapsed);
  }
  std::printf("mode,tier,speed,block,percent_realtime\n");
  for (const auto tier : {warp::QualityTier::q0, warp::QualityTier::q1, warp::QualityTier::q2}) {
    for (const double speed : {0.5, 0.9, 1.0, 1.25, 2.0, 4.0}) {
      // Fractional start so unity speed exercises interpolation, not copies.
      const auto begin = static_cast<std::uint64_t>(0.37 * warp::source_unit);
      auto map = warp::WarpTimeMap::create(
          {{0, output_frames, begin,
            begin + static_cast<std::uint64_t>(speed * static_cast<double>(output_frames) *
                                               warp::source_unit),
            false}});
      if (!map)
        return 1;
      for (const std::size_t block : {std::size_t{64}, std::size_t{128}}) {
        auto stream = warp::create_repitch_stream({rate, 8'192}, tier);
        if (!stream)
          return 1;
        std::vector<float> out_left(block);
        std::vector<float> out_right(block);
        if (stream.value()->start(map.value(), source, 0) != warp::StreamStatus::ok)
          return 1;
        const double started = thread_seconds();
        while (stream.value()->next_output_frame() < output_frames) {
          if (stream.value()->render(out_left, out_right) == 0)
            return 1;
        }
        const double elapsed = thread_seconds() - started;
        std::printf("repitch,%s,%.2f,%zu,%.3f\n", tier_name(tier), speed, block,
                    100.0 * elapsed / (static_cast<double>(output_frames) / rate));
      }
    }
  }
  // Beats: slices every 250 ms of source, all loop modes, unity and +7 st.
  std::vector<std::uint64_t> slices;
  for (std::uint64_t frame = rate / 8; frame < output_frames * 4; frame += rate / 4)
    slices.push_back(frame);
  for (const double speed : {0.5, 1.25, 2.0}) {
    auto map = warp::WarpTimeMap::create(
        {{0, output_frames, 0,
          static_cast<std::uint64_t>(speed * static_cast<double>(output_frames)) *
              warp::source_unit,
          false}});
    if (!map)
      return 1;
    for (const auto loop :
         {warp::BeatsLoop::off, warp::BeatsLoop::forward, warp::BeatsLoop::back_and_forth}) {
      for (const std::int32_t cents : {0, 700}) {
        auto stream = warp::create_beats_stream({rate, 8'192}, warp::QualityTier::q0,
                                                {slices, loop, 100.0F, cents});
        if (!stream || stream.value()->start(map.value(), source, 0) != warp::StreamStatus::ok)
          return 1;
        std::vector<float> out_left(128);
        std::vector<float> out_right(128);
        const double started = thread_seconds();
        while (stream.value()->next_output_frame() < output_frames) {
          if (stream.value()->render(out_left, out_right) == 0) {
            std::fprintf(stderr, "beats render stopped at %llu status %d\n",
                         static_cast<unsigned long long>(stream.value()->next_output_frame()),
                         static_cast<int>(stream.value()->status()));
            return 1;
          }
        }
        const double elapsed = thread_seconds() - started;
        std::printf("beats,Q0,%.2f,loop%d,cents%d,%.3f\n", speed, static_cast<int>(loop), cents,
                    100.0 * elapsed / (static_cast<double>(output_frames) / rate));
      }
    }
  }
  // Complex: drum fixture onsets every 250 ms, every tier.
  std::vector<std::uint64_t> onsets;
  for (std::uint64_t frame = rate / 8; frame < output_frames * 4; frame += rate / 4)
    onsets.push_back(frame);
  for (const auto tier : {warp::QualityTier::q0, warp::QualityTier::q1, warp::QualityTier::q2}) {
    for (const double speed : {0.5, 1.0, 2.0}) {
      auto map = warp::WarpTimeMap::create(
          {{0, output_frames, 0,
            static_cast<std::uint64_t>(speed * static_cast<double>(output_frames)) *
                warp::source_unit,
            false}});
      if (!map)
        return 1;
      for (const std::size_t block : {std::size_t{64}, std::size_t{128}}) {
        auto stream = warp::create_complex_stream({rate, 8'192}, tier, {onsets});
        if (!stream || stream.value()->start(map.value(), source, 0) != warp::StreamStatus::ok)
          return 1;
        std::vector<float> out_left(block);
        std::vector<float> out_right(block);
        const double started = thread_seconds();
        while (stream.value()->next_output_frame() < output_frames) {
          if (stream.value()->render(out_left, out_right) == 0)
            return 1;
        }
        const double elapsed = thread_seconds() - started;
        std::printf("complex,%s,%.2f,%zu,%.3f\n", tier_name(tier), speed, block,
                    100.0 * elapsed / (static_cast<double>(output_frames) / rate));
      }
    }
  }
  // Transposed Complex and Complex Pro (formants) at Q0, 0.8x.
  {
    auto map = warp::WarpTimeMap::create(
        {{0, output_frames, 0,
          static_cast<std::uint64_t>(0.8 * static_cast<double>(output_frames)) * warp::source_unit,
          false}});
    if (!map)
      return 1;
    for (const std::int32_t cents : {-700, 700}) {
      for (const bool pro : {false, true}) {
        auto stream = pro ? warp::create_complex_pro_stream({rate, 8'192}, warp::QualityTier::q0,
                                                            {onsets, cents, 100.0F, 128.0F})
                          : warp::create_complex_stream({rate, 8'192}, warp::QualityTier::q0,
                                                        {onsets, cents});
        if (!stream || stream.value()->start(map.value(), source, 0) != warp::StreamStatus::ok)
          return 1;
        std::vector<float> out_left(128);
        std::vector<float> out_right(128);
        const double started = thread_seconds();
        while (stream.value()->next_output_frame() < output_frames) {
          if (stream.value()->render(out_left, out_right) == 0)
            return 1;
        }
        const double elapsed = thread_seconds() - started;
        std::printf("%s,Q0,0.80,cents%d,%.3f\n", pro ? "complex_pro" : "complex", cents,
                    100.0 * elapsed / (static_cast<double>(output_frames) / rate));
      }
    }
  }
  // Tones (pitch marks every 5 ms on noise exercise grain cost) and Texture.
  {
    std::vector<warp::analysis::PitchMark> marks;
    for (std::uint64_t frame = 120; frame < output_frames * 4; frame += 240)
      marks.push_back({frame, 240.0F});
    auto map = warp::WarpTimeMap::create(
        {{0, output_frames, 0,
          static_cast<std::uint64_t>(0.8 * static_cast<double>(output_frames)) * warp::source_unit,
          false}});
    if (!map)
      return 1;
    for (const std::int32_t cents : {0, 700}) {
      for (const bool tonal : {true, false}) {
        auto stream = tonal ? warp::create_tones_stream({rate, 8'192}, warp::QualityTier::q0,
                                                        {marks, 40.0F, cents})
                            : warp::create_texture_stream({rate, 8'192}, warp::QualityTier::q0,
                                                          {100.0F, 30.0F, cents, 1});
        if (!stream || stream.value()->start(map.value(), source, 0) != warp::StreamStatus::ok)
          return 1;
        std::vector<float> out_left(128);
        std::vector<float> out_right(128);
        const double started = thread_seconds();
        while (stream.value()->next_output_frame() < output_frames) {
          if (stream.value()->render(out_left, out_right) == 0)
            return 1;
        }
        const double elapsed = thread_seconds() - started;
        std::printf("%s,Q0,0.80,cents%d,%.3f\n", tonal ? "tones" : "texture", cents,
                    100.0 * elapsed / (static_cast<double>(output_frames) / rate));
      }
    }
  }
  // Different-rate source conversion: 30 s of project frames per pair, read
  // in 64- and 128-frame pulls (worst case: each pull re-reads the filter
  // reach) and in 2,048-frame worker blocks.
  for (const auto [source_rate, project_rate] :
       {std::pair{44'100U, 48'000U}, std::pair{48'000U, 44'100U}, std::pair{44'100U, 96'000U},
        std::pair{96'000U, 44'100U}, std::pair{88'200U, 48'000U}, std::pair{48'000U, 96'000U}}) {
    auto view = warp::RateConvertedSourceView::create(source, source_rate, project_rate);
    if (!view)
      return 1;
    for (const std::size_t pull : {std::size_t{64}, std::size_t{128}, std::size_t{2'048}}) {
      std::vector<float> out_left(pull);
      std::vector<float> out_right(pull);
      const std::uint64_t frames = project_rate * 30ULL;
      const double started = thread_seconds();
      for (std::uint64_t frame = 0; frame < frames; frame += pull) {
        if (!view.value()->read(static_cast<std::int64_t>(frame), out_left, out_right))
          return 1;
      }
      const double elapsed = thread_seconds() - started;
      std::printf("rate_view,%u->%u,pull%zu,taps%u,%.3f\n", source_rate, project_rate, pull,
                  view.value()->taps(), 100.0 * elapsed / 30.0);
    }
  }
  return 0;
}

} // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
