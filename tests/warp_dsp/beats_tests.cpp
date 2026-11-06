#include "fixtures.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {
namespace warp = oscillate::warp_dsp;
constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;

using oscillate::test::require;

struct Rendered {
  std::vector<float> left;
  std::vector<float> right;
};

warp::WarpTimeMap stretch_map(std::uint64_t source_frames, double speed) {
  const auto output =
      static_cast<std::uint64_t>(std::llround(static_cast<double>(source_frames) / speed));
  auto map = warp::WarpTimeMap::create({{0, output, 0, source_frames << 16, false}});
  REQUIRE_OK(map, "stretch map rejected");
  return std::move(map).value();
}

std::unique_ptr<warp::WarpStream> beats(warp::BeatsSettings settings) {
  auto created =
      warp::create_beats_stream({rate, 8'192}, warp::QualityTier::q0, std::move(settings));
  REQUIRE_OK(created, created ? "" : created.error().message);
  return std::move(created).value();
}

Rendered render(warp::WarpStream& stream, const warp::WarpTimeMap& map, warp::SourceView& source,
                std::uint64_t first = 0, std::size_t block = 128) {
  REQUIRE_EQ(stream.start(map, source, first), warp::StreamStatus::ok, "start failed");
  Rendered out;
  const auto total = static_cast<std::size_t>(map.output_frames() - first);
  out.left.resize(total);
  out.right.resize(total);
  std::size_t done = 0;
  std::uint64_t random = 11;
  while (done < total) {
    random = random * 6364136223846793005ULL + 1;
    const auto size = std::min(total - done, block != 0 ? block : 1 + (random >> 33) % 999);
    const auto written = stream.render(std::span(out.left).subspan(done, size),
                                       std::span(out.right).subspan(done, size));
    REQUIRE_EQ(written, size, "render returned short");
    done += written;
  }
  return out;
}

std::uint64_t attack_frame(const warp::WarpTimeMap& map, std::uint64_t source_frame) {
  return warp::WarpTimeMap::first_output_reaching(map.segments().front(), source_frame << 16);
}

void attacks_land_exactly_at_every_ratio() {
  auto drums = warp_fixtures::drum_hits(rate, 6.0, 0.25, 9);
  warp::MemorySourceView view(drums.left, drums.right);
  for (const double speed : {0.5, 0.8, 1.25, 2.0, 3.0}) {
    const auto map = stretch_map(drums.left.size(), speed);
    // Loop off keeps gaps clean so the independent detector judges placement
    // alone; looped gaps are covered by gaps_follow_loop_mode.
    warp::BeatsSettings settings;
    settings.slice_starts = drums.onsets;
    settings.loop = warp::BeatsLoop::off;
    auto stream = beats(settings);
    const auto out = render(*stream, map, view);
    for (std::size_t index = 0; index + 1 < drums.onsets.size(); ++index) {
      const auto onset = drums.onsets[index];
      const auto at = attack_frame(map, onset);
      const auto next = attack_frame(map, drums.onsets[index + 1]);
      // After the predecessor's 2 ms overlap fade, the slice is an exact copy
      // of the source right up to its successor's attack.
      constexpr std::uint64_t overlap = (rate + 250) / 500;
      // With loop off, a slice that runs out before its successor fades to
      // silence over its last 2 ms.
      const auto natural = drums.onsets[index + 1] - onset;
      const auto copy = next - at > natural ? natural - overlap : next - at;
      for (std::uint64_t offset = index == 0 ? 0 : overlap; offset < copy; ++offset)
        REQUIRE_EQ(out.left[at + offset], drums.left[onset + offset],
                   "Beats slice is not an exact copy from its mapped attack frame");
    }
    // Above 2x the drums are 83 ms apart and masked kicks fall below the
    // detector's adaptive threshold; placement there rests on the exact copy.
    if (speed > 2.0)
      continue;
    // An independent detector finds the attacks where the map put them. The
    // exact-copy check above proves sample placement; this cross-check uses a
    // 5 ms tolerance because a low kick under a loud snare tail is perceived
    // a few milliseconds after its first sample.
    warp::MemorySourceView rendered(out.left, out.right);
    const auto found = warp::analysis::detect_transients(rendered, {rate, 0.5F, 0.03});
    REQUIRE_OK(found, "detector failed on Beats output");
    std::size_t matched = 0;
    std::size_t within_half_ms = 0;
    for (const auto onset : drums.onsets) {
      const auto expected = static_cast<double>(attack_frame(map, onset));
      double nearest = 1e9;
      for (const auto& transient : found.value()) {
        const double offset = static_cast<double>(transient.frame) - expected;
        if (std::fabs(offset) < std::fabs(nearest))
          nearest = offset;
      }
      matched += std::fabs(nearest) <= 0.005 * rate;
      within_half_ms += std::fabs(nearest) <= 0.0005 * rate;
    }
    std::printf("beats %.2fx: %zu/%zu attacks detected within 5 ms (%zu within 0.5 ms)\n", speed,
                matched, drums.onsets.size(), within_half_ms);
    REQUIRE_GE(matched, drums.onsets.size() - 1, "Beats attacks not detected at mapped frames");
  }
}

void unity_map_reproduces_source() {
  auto plucks = warp_fixtures::plucks(rate, 5.0, 0.21, 3);
  warp::MemorySourceView view(plucks.left, plucks.right);
  const auto map = stretch_map(plucks.left.size(), 1.0);
  warp::BeatsSettings settings;
  settings.slice_starts = plucks.onsets;
  auto stream = beats(settings);
  const auto out = render(*stream, map, view, 0, 0);
  for (std::size_t index = 0; index < out.left.size(); ++index) {
    if (out.left[index] != plucks.left[index] || out.right[index] != plucks.right[index]) {
      std::printf("unity mismatch at %zu: %.6f vs %.6f\n", index,
                  static_cast<double>(out.left[index]), static_cast<double>(plucks.left[index]));
      require(false, "unity Beats is not a bit-exact copy");
    }
  }
}

double rms(const std::vector<float>& signal, std::uint64_t begin, std::uint64_t end) {
  double energy = 0.0;
  for (auto index = begin; index < end; ++index)
    energy += signal[index] * signal[index];
  return std::sqrt(energy / static_cast<double>(end - begin));
}

void gaps_follow_loop_mode() {
  // Plucks slowed to half speed leave each slice half its output duration.
  auto plucks = warp_fixtures::plucks(rate, 4.0, 0.25, 5);
  warp::MemorySourceView view(plucks.left, plucks.right);
  const auto map = stretch_map(plucks.left.size(), 0.5);
  for (const auto loop :
       {warp::BeatsLoop::off, warp::BeatsLoop::forward, warp::BeatsLoop::back_and_forth}) {
    warp::BeatsSettings settings;
    settings.slice_starts = plucks.onsets;
    settings.loop = loop;
    auto stream = beats(settings);
    const auto out = render(*stream, map, view);
    double worst_step = 0.0;
    double gap_level = 0.0;
    for (std::size_t index = 0; index + 1 < plucks.onsets.size(); ++index) {
      const auto at = attack_frame(map, plucks.onsets[index]);
      const auto next = attack_frame(map, plucks.onsets[index + 1]);
      const auto natural = plucks.onsets[index + 1] - plucks.onsets[index];
      // The gap region after the slice's natural content.
      gap_level += rms(out.left, at + natural + 200, next - 200);
      for (auto frame = at + 1; frame + 200 < next; ++frame)
        worst_step = std::max(
            worst_step, static_cast<double>(std::fabs(out.left[frame] - out.left[frame - 1])));
    }
    std::printf("loop %d: mean gap rms %.5f, worst step %.4f\n", static_cast<int>(loop),
                gap_level / static_cast<double>(plucks.onsets.size() - 1), worst_step);
    if (loop == warp::BeatsLoop::off)
      REQUIRE_EQ(gap_level, 0.0, "loop off did not leave silence in the gap");
    else
      REQUIRE_GT(gap_level, 0.01 * static_cast<double>(plucks.onsets.size() - 1),
                 "looping modes left the gap silent");
    // A loop seam or fade must not step harder than the source ever does.
    double source_step = 0.0;
    for (std::size_t frame = 1; frame < plucks.left.size(); ++frame)
      source_step = std::max(
          source_step, static_cast<double>(std::fabs(plucks.left[frame] - plucks.left[frame - 1])));
    REQUIRE_LE(worst_step, source_step * 1.02, "Beats gap handling produced a discontinuity");
  }
}

void every_mode_renders_dense_slices_completely() {
  // Dense slices, slow and fast maps, every loop mode and pitch: overlap
  // tails cross loop wraps, which must never exhaust the source windows.
  const auto noise = warp_fixtures::drum_hits(rate, 12.0, 0.05, 21);
  warp::MemorySourceView view(noise.left, noise.right);
  std::vector<std::uint64_t> slices;
  for (std::uint64_t frame = rate / 16; frame < noise.left.size(); frame += rate / 9)
    slices.push_back(frame);
  // Includes the geometry where a forward-loop wrap falls inside the 2 ms
  // overlap fade at a slice boundary (slices every 250 ms, 0.5x, +7 st).
  std::vector<std::uint64_t> quarter_slices;
  for (std::uint64_t frame = rate / 8; frame < noise.left.size(); frame += rate / 4)
    quarter_slices.push_back(frame);
  for (const auto& slicing : {quarter_slices, slices}) {
    for (const double speed : {0.37, 0.5, 1.6}) {
      const auto map = stretch_map(noise.left.size() / 2, speed);
      for (const auto loop :
           {warp::BeatsLoop::off, warp::BeatsLoop::forward, warp::BeatsLoop::back_and_forth}) {
        for (const std::int32_t cents : {-1'900, 0, 700, 2'400}) {
          auto stream = beats({slicing, loop, 100.0F, cents});
          (void)render(*stream, map, view, 0, 128);
          REQUIRE_EQ(stream->status(), warp::StreamStatus::ok, "dense Beats render failed");
        }
      }
    }
  }
}

void envelope_decays_each_slice() {
  auto drums = warp_fixtures::drum_hits(rate, 4.0, 0.25, 7);
  warp::MemorySourceView view(drums.left, drums.right);
  const auto map = stretch_map(drums.left.size(), 0.8);
  warp::BeatsSettings settings;
  settings.slice_starts = drums.onsets;
  settings.envelope = 40.0F;
  auto stream = beats(settings);
  const auto out = render(*stream, map, view);
  for (std::size_t index = 0; index + 1 < drums.onsets.size(); ++index) {
    const auto at = attack_frame(map, drums.onsets[index]);
    const auto next = attack_frame(map, drums.onsets[index + 1]);
    const auto decay = static_cast<std::uint64_t>(std::ceil(0.4 * static_cast<double>(next - at)));
    for (auto frame = at + decay + 1; frame < next; ++frame)
      REQUIRE_EQ(out.left[frame], 0.0F, "envelope did not silence the slice after 40%");
  }
}

void transpose_shifts_pitch() {
  // One slice of a 440 Hz tone transposed up an octave must read at 880 Hz.
  std::vector<float> tone(rate * 2);
  for (std::size_t index = 0; index < tone.size(); ++index)
    tone[index] = static_cast<float>(
        0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(index) / rate));
  warp::MemorySourceView view(tone, tone);
  const auto map = stretch_map(rate, 1.0);
  warp::BeatsSettings settings;
  settings.slice_starts = {0};
  settings.transpose_cents = 1'200;
  auto stream = beats(settings);
  const auto out = render(*stream, map, view);
  std::size_t crossings = 0;
  for (std::size_t index = 2'000; index < 26'000; ++index)
    crossings += (out.left[index - 1] < 0.0F) != (out.left[index] < 0.0F);
  const double frequency = static_cast<double>(crossings) / 2.0 / (24'000.0 / rate);
  std::printf("transpose +1200 cents: %.2f Hz\n", frequency);
  REQUIRE_LT(std::fabs(frequency - 880.0), 2.0, "transpose did not double the frequency");
}

void partition_and_seek_are_invariant() {
  auto plucks = warp_fixtures::plucks(rate, 4.0, 0.21, 8);
  warp::MemorySourceView view(plucks.left, plucks.right);
  const auto source_frames = static_cast<std::uint64_t>(plucks.left.size());
  // Slow segment, fast segment, then a splice back into the middle of a slice.
  auto map =
      warp::WarpTimeMap::create({{0, 60'000, 0, 40'000ULL << 16, false},
                                 {60'000, 90'000, 40'000ULL << 16, 100'000ULL << 16, false},
                                 {90'000, 150'000, 61'234ULL << 16, 101'234ULL << 16, true},
                                 {150'000, 160'000, 101'234ULL << 16,
                                  std::min<std::uint64_t>(source_frames, 120'000) << 16, false}});
  REQUIRE_OK(map, "invariance map rejected");
  for (const auto loop :
       {warp::BeatsLoop::off, warp::BeatsLoop::forward, warp::BeatsLoop::back_and_forth}) {
    warp::BeatsSettings settings;
    settings.slice_starts = plucks.onsets;
    settings.loop = loop;
    settings.envelope = 70.0F;
    settings.transpose_cents = 300;
    auto stream = beats(settings);
    const auto reference = render(*stream, map.value(), view, 0, 8'192);
    for (const std::size_t block : {std::size_t{1}, std::size_t{64}, std::size_t{0}}) {
      const auto pieces = render(*stream, map.value(), view, 0, block);
      REQUIRE_EQ(pieces.left, reference.left, "Beats output depends on partition");
    }
    for (const std::uint64_t seek : {1ULL, 59'999ULL, 90'000ULL, 90'005ULL, 155'555ULL}) {
      const auto tail = render(*stream, map.value(), view, seek, 0);
      require(std::equal(tail.left.begin(), tail.left.end(),
                         reference.left.begin() + static_cast<std::ptrdiff_t>(seek)),
              "Beats output after seek differs");
    }
    // Entering mid-slice at the splice fades in rather than clicking.
    require(std::fabs(reference.left[90'000]) <= std::fabs(reference.left[90'040]) + 1e-6F ||
                std::fabs(reference.left[90'000]) < 0.01F,
            "splice into mid-slice did not fade in");
  }
}

void rejects_invalid_settings_and_does_not_allocate() {
  require(!warp::create_beats_stream({rate, 8'192}, static_cast<warp::QualityTier>(255), {}),
          "invalid Beats quality tier accepted");
  const auto invalid = [](warp::BeatsSettings settings) {
    return !warp::create_beats_stream({rate, 8'192}, warp::QualityTier::q0, std::move(settings));
  };
  require(invalid({{5, 5}, warp::BeatsLoop::off, 100.0F, 0}), "duplicate slices accepted");
  require(invalid({{9, 5}, warp::BeatsLoop::off, 100.0F, 0}), "unsorted slices accepted");
  require(invalid({{}, warp::BeatsLoop::off, 101.0F, 0}), "envelope above 100 accepted");
  require(invalid({{}, warp::BeatsLoop::off, 100.0F, 5'000}), "transpose beyond range accepted");

  auto drums = warp_fixtures::drum_hits(rate, 5.0, 0.2, 2);
  warp::MemorySourceView view(drums.left, drums.right);
  const auto map = stretch_map(drums.left.size(), 0.7);
  warp::BeatsSettings settings;
  settings.slice_starts = drums.onsets;
  settings.transpose_cents = -500;
  auto stream = beats(settings);
  std::vector<float> left(128);
  std::vector<float> right(128);
  counting_process_allocations = true;
  const bool started = stream->start(map, view, 0) == warp::StreamStatus::ok;
  while (started && stream->next_output_frame() < map.output_frames())
    (void)stream->render(left, right);
  counting_process_allocations = false;
  REQUIRE_TRUE(started, "Beats start()/render() allocated");
  REQUIRE_EQ(process_allocations.load(), 0, "Beats start()/render() allocated");
}

} // namespace

int main() {
  try {
    attacks_land_exactly_at_every_ratio();
    unity_map_reproduces_source();
    gaps_follow_loop_mode();
    every_mode_renders_dense_slices_completely();
    envelope_decays_each_slice();
    transpose_shifts_pitch();
    partition_and_seek_are_invariant();
    rejects_invalid_settings_and_does_not_allocate();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
