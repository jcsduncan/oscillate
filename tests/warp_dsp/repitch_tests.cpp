#include "core/bandlimited_kernel.hpp"
#include "core/bandlimited_read.hpp"
#include "core/source_window.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {
namespace warp = oscillate::warp_dsp;
constexpr double rate = oscillate::test::sample_rate_48k_hz;

using oscillate::test::require;

struct Signal {
  std::vector<float> left;
  std::vector<float> right;
};

warp::WarpTimeMap make_map(std::vector<warp::TimeMapSegment> segments) {
  auto map = warp::WarpTimeMap::create(std::move(segments));
  REQUIRE_OK(map, map ? "" : map.error().message);
  return std::move(map).value();
}

warp::TimeMapSegment linear(std::uint64_t output_begin, std::uint64_t output_end,
                            double source_begin, double source_end, bool splice = false) {
  return {output_begin, output_end,
          static_cast<std::uint64_t>(std::llround(source_begin * warp::source_unit)),
          static_cast<std::uint64_t>(std::llround(source_end * warp::source_unit)), splice};
}

std::unique_ptr<warp::WarpStream> stream(warp::QualityTier tier = warp::QualityTier::q0) {
  auto created = warp::create_repitch_stream({48'000, 8'192}, tier);
  REQUIRE_OK(created, "Re-Pitch stream creation failed");
  return std::move(created).value();
}

void invalid_tier_is_rejected() {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): an invalid tier on purpose.
  constexpr auto invalid_tier = static_cast<warp::QualityTier>(255);
  require(!warp::create_repitch_stream({48'000, 8'192}, invalid_tier),
          "invalid Re-Pitch quality tier accepted");
  require(!warp::detail::BandlimitedKernel::for_tier(invalid_tier),
          "invalid kernel quality tier returned a kernel");
}

Signal noise(std::size_t frames);

// Custom designs whose taps are not whole lane blocks still read every tap;
// a design with no zero crossings is refused.
void custom_kernel_lengths_read_every_tap() {
  constexpr std::size_t frames = 4'096;
  constexpr std::uint64_t centre = std::uint64_t{frames / 2} << 16;
  constexpr std::uint64_t half_frame = std::uint64_t{1} << 15;
  constexpr double kaiser_beta = 6.0;
  constexpr double unity_speed = 1.0;
  constexpr float read_tolerance = 2e-6F;
  const auto source = noise(frames);
  warp::MemorySourceView view(source.left, source.right);
  warp::detail::SourceWindow window(frames);
  require(window.ensure(view, 0, frames), "custom kernel window fill failed");
  for (const std::uint32_t crossings : {1U, 3U, 5U, 6U}) {
    const warp::detail::BandlimitedKernel kernel({crossings, kaiser_beta});
    const auto fast =
        warp::detail::bandlimited_read(window, kernel, {centre + half_frame, unity_speed});
    const auto slow = warp::detail::reference_bandlimited_read(window, kernel,
                                                               {centre + half_frame, unity_speed});
    REQUIRE_LT(std::fabs(fast.left - slow.left), read_tolerance,
               "a custom kernel length dropped taps");
    REQUIRE_LT(std::fabs(fast.right - slow.right), read_tolerance,
               "a custom kernel length dropped taps");
    REQUIRE_GT(std::fabs(fast.left), 0.0F, "a custom kernel length dropped taps");
  }
  bool refused = false;
  try {
    const warp::detail::BandlimitedKernel empty({0, kaiser_beta});
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "a kernel with no zero crossings was built");
}

// Renders [first, map end) in blocks of `block` frames (0 means random sizes).
Signal render(warp::WarpStream& engine, const warp::WarpTimeMap& map, warp::SourceView& source,
              std::uint64_t first, std::size_t block) {
  REQUIRE_EQ(engine.start(map, source, first), warp::StreamStatus::ok, "stream start failed");
  Signal out;
  const auto total = static_cast<std::size_t>(map.output_frames() - first);
  out.left.resize(total);
  out.right.resize(total);
  std::size_t done = 0;
  std::uint64_t random = 7;
  while (done < total) {
    random = random * 6364136223846793005ULL + 1;
    const auto size = std::min(total - done, block != 0 ? block : 1 + (random >> 33) % 777);
    const auto written = engine.render(std::span(out.left).subspan(done, size),
                                       std::span(out.right).subspan(done, size));
    REQUIRE_EQ(written, size,
               "stream rendered fewer frames than requested at " + std::to_string(done) +
                   " status " + std::to_string(static_cast<int>(engine.status())));
    done += written;
  }
  REQUIRE_EQ(engine.next_output_frame(), map.output_frames(), "stream clock drifted");
  return out;
}

Signal noise(std::size_t frames) {
  Signal signal{std::vector<float>(frames), std::vector<float>(frames)};
  std::uint32_t state = 1;
  for (std::size_t index = 0; index < frames; ++index) {
    state = state * 1664525U + 1013904223U;
    signal.left[index] = static_cast<float>(state >> 8) / 16777216.0F - 0.5F;
    signal.right[index] = -signal.left[index] * 0.5F;
  }
  return signal;
}

struct Tone {
  std::size_t frames;
  double frequency;
};

Signal sine(const Tone& tone) {
  const auto frames = tone.frames;
  const auto frequency = tone.frequency;
  Signal signal{std::vector<float>(frames), std::vector<float>(frames)};
  for (std::size_t index = 0; index < frames; ++index) {
    const double phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate;
    signal.left[index] = static_cast<float>(0.5 * std::sin(phase));
    signal.right[index] = static_cast<float>(0.5 * std::cos(phase));
  }
  return signal;
}

void identity_is_bit_exact() {
  const auto source = noise(20'000);
  warp::MemorySourceView view(source.left, source.right);
  for (const auto tier : {warp::QualityTier::q0, warp::QualityTier::q1, warp::QualityTier::q2}) {
    auto engine = stream(tier);
    const auto map = make_map({linear(0, 12'000, 100.0, 12'100.0)});
    const auto out = render(*engine, map, view, 0, 128);
    for (std::size_t index = 0; index < out.left.size(); ++index)
      require(out.left[index] == source.left[index + 100] &&
                  out.right[index] == source.right[index + 100],
              "unity-speed read is not bit-exact");
  }
}

// Error of a resampled sine against its analytic value, in dB re amplitude.
struct ReadCase {
  double speed;
  double frequency;
  double offset;
  warp::QualityTier tier;
};

double sine_error_db(const ReadCase& read) {
  const auto [speed, frequency, offset, tier] = read;
  const std::size_t frames = 16'384;
  const auto source = sine({static_cast<std::size_t>(frames * speed) + 4'096, frequency});
  warp::MemorySourceView view(source.left, source.right);
  auto engine = stream(tier);
  const auto map = make_map({linear(0, frames, offset, offset + frames * speed)});
  const auto out = render(*engine, map, view, 0, 64);
  double worst = 0.0;
  const auto& segment = map.segments().front();
  for (std::size_t index = 2'048; index < frames - 2'048; ++index) {
    const double position =
        static_cast<double>(warp::WarpTimeMap::source_at(segment, index)) / warp::source_unit;
    const double phase = 2.0 * std::numbers::pi * frequency * position / rate;
    worst = std::max({worst, std::fabs(out.left[index] - 0.5 * std::sin(phase)),
                      std::fabs(out.right[index] - 0.5 * std::cos(phase))});
  }
  return 20.0 * std::log10(std::max(worst, 1e-12) / 0.5);
}

void optimised_read_matches_reference() {
  const auto source = noise(70'000);
  warp::MemorySourceView view(source.left, source.right);
  warp::detail::SourceWindow window(70'000);
  require(window.ensure(view, 0, 70'000), "oracle window fill failed");
  std::uint64_t random = 3;
  for (const auto tier : {warp::QualityTier::q0, warp::QualityTier::q1, warp::QualityTier::q2}) {
    const auto kernel = warp::detail::BandlimitedKernel::for_tier(tier);
    for (int probe = 0; probe < 20'000; ++probe) {
      random = random * 6364136223846793005ULL + 1442695040888963407ULL;
      const double speed = 0.1 + static_cast<double>(random >> 40) / (1ULL << 24) * 15.8;
      const std::uint64_t position = (20'000ULL << 16) + (random >> 31) % (30'000ULL << 16);
      const auto fast = warp::detail::bandlimited_read(window, *kernel, {position, speed});
      const auto planned = warp::detail::bandlimited_read(
          window, *kernel, position, warp::detail::make_bandlimited_read_plan(speed));
      const auto slow =
          warp::detail::reference_bandlimited_read(window, *kernel, {position, speed});
      REQUIRE_EQ(fast.left, planned.left, "planned read differs from per-sample read");
      REQUIRE_EQ(fast.right, planned.right, "planned read differs from per-sample read");
      REQUIRE_LT(std::fabs(fast.left - slow.left), 2e-6F,
                 "optimised read differs from reference read");
      REQUIRE_LT(std::fabs(fast.right - slow.right), 2e-6F,
                 "optimised read differs from reference read");
    }
  }
}

void fixed_rate_reader_matches_reference() {
  const auto source = noise(70'000);
  warp::MemorySourceView view(source.left, source.right);
  warp::detail::SourceWindow window(70'000);
  require(window.ensure(view, 0, 70'000), "oracle window fill failed");
  std::uint64_t random = 5;
  for (const double speed : {0.3, 0.8, 1.0, 1.0595, 1.5, 2.0, 3.7, 8.0}) {
    for (const auto tier : {warp::QualityTier::q0, warp::QualityTier::q2}) {
      const auto kernel = warp::detail::BandlimitedKernel::for_tier(tier);
      const warp::detail::FixedRateReader reader(kernel->design(), speed);
      for (int probe = 0; probe < 3'000; ++probe) {
        random = random * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t position = (20'000ULL << 16) + (random >> 31) % (30'000ULL << 16);
        const auto bank = reader.read(window, position);
        const auto slow =
            warp::detail::reference_bandlimited_read(window, *kernel, {position, speed});
        REQUIRE_LT(std::fabs(bank.left - slow.left), tolerance::float_processing_f,
                   "fixed-rate reader differs from reference read");
        REQUIRE_LT(std::fabs(bank.right - slow.right), tolerance::float_processing_f,
                   "fixed-rate reader differs from reference read");
      }
      // Whole-frame unity reads stay bit-exact.
      if (speed <= 1.0)
        REQUIRE_EQ(reader.read(window, 25'000ULL << 16).left, source.left[25'000],
                   "fixed-rate whole-frame read is not exact");
    }
  }
}

void band_limited_reads_are_accurate() {
  for (const double speed : {0.25, 0.37, 0.5, 0.75, 1.0, 1.5, 2.0, 3.3, 7.9}) {
    const double error = sine_error_db({speed, 1'000.0, 17.3, warp::QualityTier::q0});
    if (error > -80.0) {
      std::fprintf(stderr, "speed %.2f: %.1f dB\n", speed, error);
      require(false, "Q0 band-limited read error above -80 dB");
    }
  }
  REQUIRE_LT(sine_error_db({0.8, 10'000.0, 3.7, warp::QualityTier::q0}), -70.0,
             "Q0 high-frequency interpolation error above -70 dB");
  REQUIRE_LT(sine_error_db({0.8, 1'000.0, 3.7, warp::QualityTier::q2}), -45.0,
             "Q2 interpolation error above -45 dB");
}

void fast_reads_do_not_alias() {
  // At 4x speed, an 8 kHz source tone would land at 32 kHz and alias; the
  // band-limited read must suppress it.
  const std::size_t frames = 8'192;
  const auto source = sine({frames * 4 + 4'096, 8'000.0});
  warp::MemorySourceView view(source.left, source.right);
  auto engine = stream();
  const auto map = make_map({linear(0, frames, 2'048.0, 2'048.0 + frames * 4.0)});
  const auto out = render(*engine, map, view, 0, 128);
  double energy = 0.0;
  for (std::size_t index = 1'024; index < frames - 1'024; ++index)
    energy += static_cast<double>(out.left[index]) * out.left[index];
  const double rms = std::sqrt(energy / static_cast<double>(frames - 2'048));
  REQUIRE_LT(20.0 * std::log10(rms / (0.5 / std::numbers::sqrt2)), -70.0,
             "aliased tone not suppressed by 70 dB");
}

void markers_land_exactly() {
  // Impulses at the marker (source 1500 -> output 1000) and inside each
  // segment must peak exactly where the map sends them.
  std::vector<float> left(4'000, 0.0F);
  left[1'500] = 1.0F;
  left[600] = 1.0F;   // output 400 at speed 1.5
  left[2'000] = 1.0F; // output 2000 at speed 0.5
  std::vector<float> right = left;
  warp::MemorySourceView view(left, right);
  auto engine = stream();
  const auto map =
      make_map({linear(0, 1'000, 0.0, 1'500.0), linear(1'000, 3'000, 1'500.0, 2'500.0)});
  const auto out = render(*engine, map, view, 0, 100);
  const auto peak = [&out](std::size_t begin, std::size_t end) {
    std::size_t best = begin;
    for (auto index = begin; index < end; ++index) {
      if (std::fabs(out.left[index]) > std::fabs(out.left[best]))
        best = index;
    }
    return best;
  };
  REQUIRE_EQ(peak(900, 1'100), 1'000, "marker impulse misplaced");
  REQUIRE_EQ(out.left[1'000], 1.0F, "marker impulse misplaced");
  REQUIRE_EQ(peak(300, 500), 400, "fast-segment impulse misplaced");
  REQUIRE_EQ(peak(1'900, 2'100), 2'000, "slow-segment impulse misplaced");
  REQUIRE_EQ(out.left[2'000], 1.0F, "slow-segment impulse misplaced");
}

void partition_and_seek_are_invariant() {
  const auto source = noise(40'000);
  warp::MemorySourceView view(source.left, source.right);
  auto engine = stream();
  const auto map = make_map(
      {linear(0, 5'000, 10.25, 7'510.25), linear(5'000, 9'000, 7'510.25, 9'000.0),
       linear(9'000, 9'003, 20'000.0, 20'030.0, true), linear(9'003, 14'000, 20'030.0, 26'000.0)});
  const auto reference = render(*engine, map, view, 0, 8'192);
  for (const std::size_t block :
       {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{128}, std::size_t{0}}) {
    const auto pieces = render(*engine, map, view, 0, block);
    REQUIRE_EQ(pieces.left, reference.left, "output depends on render partition");
    REQUIRE_EQ(pieces.right, reference.right, "output depends on render partition");
  }
  for (const std::uint64_t seek : {1ULL, 4'999ULL, 5'000ULL, 9'001ULL, 13'999ULL}) {
    const auto tail = render(*engine, map, view, seek, 0);
    require(std::equal(tail.left.begin(), tail.left.end(),
                       reference.left.begin() + static_cast<std::ptrdiff_t>(seek)),
            "output after seek differs from continuous render");
  }
}

void splices_crossfade_without_steps() {
  // Constant input: an equal-gain crossfade must hold the level exactly.
  std::vector<float> ones(20'000, 1.0F);
  warp::MemorySourceView constant(ones, ones);
  auto engine = stream();
  const auto map =
      make_map({linear(0, 1'000, 1'000.0, 2'000.0), linear(1'000, 2'000, 9'000.0, 10'500.0, true)});
  const auto out = render(*engine, map, constant, 0, 128);
  for (std::size_t index = 100; index < 1'900; ++index)
    REQUIRE_LT(std::fabs(out.left[index] - 1.0F), tolerance::float_processing_f,
               "splice crossfade changed level");
  // After the crossfade the output equals a direct read of the new segment.
  const auto source = noise(20'000);
  warp::MemorySourceView view(source.left, source.right);
  const auto spliced = render(*engine, map, view, 0, 0);
  const auto direct = make_map({linear(0, 1'000, 9'000.0, 10'500.0)});
  const auto expected = render(*engine, direct, view, 0, 0);
  for (std::size_t index = warp::splice_crossfade_frames; index < 1'000; ++index)
    REQUIRE_EQ(spliced.left[1'000 + index], expected.left[index],
               "post-splice output differs from direct read");
}

void rejects_unsupported_and_reports_failure() {
  const auto source = noise(1'000);
  warp::MemorySourceView view(source.left, source.right);
  auto engine = stream();
  const auto fast = make_map({linear(0, 10, 0.0, 170.0)});
  REQUIRE_EQ(engine->start(fast, view, 0), warp::StreamStatus::unsupported_map,
             "speed above Re-Pitch range accepted");
  const auto normal = make_map({linear(0, 10, 0.0, 10.0)});
  REQUIRE_EQ(engine->start(normal, view, 10), warp::StreamStatus::out_of_range,
             "start past the map accepted");
  class FailingView final : public warp::SourceView {
  public:
    std::uint64_t frames() const noexcept override { return 100; }
    bool read(std::int64_t, std::span<float>, std::span<float>) noexcept override { return false; }
  } failing;
  std::vector<float> left(10);
  std::vector<float> right(10);
  REQUIRE_EQ(engine->start(normal, failing, 0), warp::StreamStatus::ok,
             "source failure not reported");
  REQUIRE_EQ(engine->render(left, right), 0, "source failure not reported");
  REQUIRE_EQ(engine->status(), warp::StreamStatus::source_failed, "source failure not reported");
}

void steady_state_does_not_allocate() {
  const auto source = noise(100'000);
  warp::MemorySourceView view(source.left, source.right);
  auto engine = stream();
  const auto map =
      make_map({linear(0, 20'000, 0.0, 30'000.0), linear(20'000, 30'000, 50'000.0, 60'000.0, true),
                linear(30'000, 40'000, 60'000.0, 62'500.0)});
  std::vector<float> left(128);
  std::vector<float> right(128);
  counting_process_allocations = true;
  const bool started = engine->start(map, view, 0) == warp::StreamStatus::ok;
  std::size_t total = 0;
  while (started && engine->next_output_frame() < map.output_frames())
    total += engine->render(left, right);
  counting_process_allocations = false;
  REQUIRE_TRUE(started, "allocation probe render incomplete");
  REQUIRE_EQ(total, 40'000, "allocation probe render incomplete");
  REQUIRE_EQ(process_allocations.load(), 0, "start() or render() allocated");
}

} // namespace

int main() {
  try {
    invalid_tier_is_rejected();
    custom_kernel_lengths_read_every_tap();
    identity_is_bit_exact();
    optimised_read_matches_reference();
    fixed_rate_reader_matches_reference();
    band_limited_reads_are_accurate();
    fast_reads_do_not_alias();
    markers_land_exactly();
    partition_and_seek_are_invariant();
    splices_crossfade_without_steps();
    rejects_unsupported_and_reports_failure();
    steady_state_does_not_allocate();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
