#include "fixtures.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/warp_dsp/rate_view.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <array>
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
#include <utility>
#include <vector>

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {
namespace warp = oscillate::warp_dsp;

constexpr std::array<std::uint32_t, 4> rates{44'100, 48'000, 88'200, 96'000};

using oscillate::test::require;

std::string pair_name(std::uint32_t source, std::uint32_t project) {
  return std::to_string(source) + "->" + std::to_string(project);
}

std::unique_ptr<warp::RateConvertedSourceView>
make_view(warp::SourceView& source, std::uint32_t source_rate, std::uint32_t project_rate) {
  auto view = warp::RateConvertedSourceView::create(source, source_rate, project_rate);
  REQUIRE_OK(view, view ? "" : view.error().message);
  return std::move(view).value();
}

struct Signal {
  std::vector<float> left;
  std::vector<float> right;
};

// Distinct sines per channel so a swapped channel cannot pass.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Signal sines(std::uint32_t rate, std::size_t frames, double left_hz, double right_hz) {
  Signal signal{std::vector<float>(frames), std::vector<float>(frames)};
  for (std::size_t index = 0; index < frames; ++index) {
    const double time = static_cast<double>(index) / rate;
    signal.left[index] =
        static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * left_hz * time));
    signal.right[index] =
        static_cast<float>(0.5 * std::cos(2.0 * std::numbers::pi * right_hz * time));
  }
  return signal;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Signal noise(std::size_t frames, std::uint64_t seed) {
  warp_fixtures::Random random(seed);
  Signal signal{std::vector<float>(frames), std::vector<float>(frames)};
  for (std::size_t index = 0; index < frames; ++index) {
    signal.left[index] = static_cast<float>(0.5 * random.symmetric());
    signal.right[index] = static_cast<float>(0.5 * random.symmetric());
  }
  return signal;
}

Signal read_all(warp::SourceView& view, std::int64_t first, std::size_t frames) {
  Signal out{std::vector<float>(frames), std::vector<float>(frames)};
  require(view.read(first, out.left, out.right), "converted read failed");
  return out;
}

// Frames far enough from both ends that the filter sees only real source.
struct Interior {
  std::int64_t first;
  std::int64_t last;
};
Interior interior(const warp::RateConvertedSourceView& view, std::uint32_t source_rate,
                  std::uint32_t project_rate) {
  const auto margin = static_cast<std::int64_t>(
      std::ceil((view.taps() + 16.0) * project_rate / static_cast<double>(source_rate)));
  return {margin, static_cast<std::int64_t>(view.frames()) - margin};
}

void every_pair_keeps_duration_and_bypasses_equal_rates() {
  const auto source = noise(30'011, 1);
  warp::MemorySourceView memory(source.left, source.right);
  for (const auto source_rate : rates) {
    for (const auto project_rate : rates) {
      auto view = make_view(memory, source_rate, project_rate);
      const auto expected =
          warp::project_rate_frames(source.left.size(), source_rate, project_rate);
      require(expected && view->frames() == expected.value(),
              "converted duration differs for " + pair_name(source_rate, project_rate));
      REQUIRE_EQ(view->bypass(), (source_rate == project_rate),
                 "bypass flag wrong for " + pair_name(source_rate, project_rate));
      REQUIRE_LE(view->phases(), 320, "standard pair exceeds 320 phases");
      if (source_rate == project_rate) {
        const auto out = read_all(*view, -7, source.left.size() + 14);
        bool exact = true;
        for (std::size_t index = 0; index < out.left.size(); ++index) {
          const auto position = static_cast<std::int64_t>(index) - 7;
          const bool inside =
              position >= 0 && static_cast<std::size_t>(position) < source.left.size();
          const float left = inside ? source.left[static_cast<std::size_t>(position)] : 0.0F;
          const float right = inside ? source.right[static_cast<std::size_t>(position)] : 0.0F;
          exact = exact && out.left[index] == left && out.right[index] == right;
        }
        require(exact, "equal-rate view is not bit-exact");
      }
    }
  }
  // One second at every pair is exactly one second at the project rate.
  for (const auto source_rate : rates)
    for (const auto project_rate : rates)
      REQUIRE_EQ(warp::project_rate_frames(source_rate, source_rate, project_rate).value(),
                 project_rate, "one second did not convert to one second");
}

// Passband: tones up to 90% of the lower Nyquist come through at the right
// pitch, phase and level. Each output frame is compared with the analytic
// sine at exactly n / P seconds, so a pitch or timing error cannot pass.
void passband_matches_analytic_sines() {
  for (const auto source_rate : rates) {
    for (const auto project_rate : rates) {
      if (source_rate == project_rate)
        continue;
      const double lower_nyquist = std::min(source_rate, project_rate) / 2.0;
      for (const double fraction : {0.0227, 0.45, 0.9}) {
        const double left_hz = fraction * lower_nyquist;
        const double right_hz = 0.5 * fraction * lower_nyquist + 37.0;
        const auto source = sines(source_rate, source_rate / 2, left_hz, right_hz);
        warp::MemorySourceView memory(source.left, source.right);
        auto view = make_view(memory, source_rate, project_rate);
        const auto out = read_all(*view, 0, view->frames());
        const auto range = interior(*view, source_rate, project_rate);
        double worst = 0.0;
        for (auto frame = range.first; frame < range.last; ++frame) {
          const double time = static_cast<double>(frame) / project_rate;
          const double left = 0.5 * std::sin(2.0 * std::numbers::pi * left_hz * time);
          const double right = 0.5 * std::cos(2.0 * std::numbers::pi * right_hz * time);
          worst = std::max({worst, std::fabs(out.left[static_cast<std::size_t>(frame)] - left),
                            std::fabs(out.right[static_cast<std::size_t>(frame)] - right)});
        }
        // 0.5 amplitude: 1.6e-5 is -90 dB relative to the tone.
        REQUIRE_LT(worst, 1.6e-5,
                   "passband error too large for " + pair_name(source_rate, project_rate) + " at " +
                       std::to_string(left_hz) + " Hz: " + std::to_string(worst));
      }
    }
  }
}

// Stopband: when converting down, content above the project Nyquist must not
// fold back into the audible band.
void stopband_rejects_aliases() {
  for (const auto source_rate : rates) {
    for (const auto project_rate : rates) {
      if (source_rate <= project_rate)
        continue;
      for (const double fraction : {1.02, 1.3, 1.9}) {
        const double hz = fraction * project_rate / 2.0;
        if (hz >= source_rate / 2.0)
          continue;
        const auto source = sines(source_rate, source_rate / 2, hz, hz);
        warp::MemorySourceView memory(source.left, source.right);
        auto view = make_view(memory, source_rate, project_rate);
        const auto out = read_all(*view, 0, view->frames());
        const auto range = interior(*view, source_rate, project_rate);
        double peak = 0.0;
        for (auto frame = range.first; frame < range.last; ++frame)
          peak = std::max(
              {peak, static_cast<double>(std::fabs(out.left[static_cast<std::size_t>(frame)])),
               static_cast<double>(std::fabs(out.right[static_cast<std::size_t>(frame)]))});
        // 0.5 amplitude in; at least 90 dB of rejection.
        REQUIRE_LT(peak, 1.6e-5,
                   "alias leaked for " + pair_name(source_rate, project_rate) + " at " +
                       std::to_string(hz) + " Hz: " + std::to_string(peak));
      }
    }
  }
}

void bank_matches_double_reference() {
  const auto source = noise(9'001, 2);
  warp::MemorySourceView memory(source.left, source.right);
  for (const auto source_rate : rates) {
    for (const auto project_rate : rates) {
      auto view = make_view(memory, source_rate, project_rate);
      // Include both ends so the boundary handling is covered too.
      const auto out = read_all(*view, -5, view->frames() + 10);
      double worst = 0.0;
      for (std::size_t index = 0; index < out.left.size(); ++index) {
        const auto frame = static_cast<std::int64_t>(index) - 5;
        worst = std::max(
            {worst,
             std::fabs(out.left[index] - warp::reference_rate_converted_sample(
                                             source.left, source_rate, project_rate, frame)),
             std::fabs(out.right[index] - warp::reference_rate_converted_sample(
                                              source.right, source_rate, project_rate, frame))});
      }
      REQUIRE_LT(worst, 2e-6,
                 "float bank differs from the double reference for " +
                     pair_name(source_rate, project_rate) + ": " + std::to_string(worst));
    }
  }
}

// The converter against an oracle written here from the published design
// (cutoff, zero crossings, Kaiser beta) with the standard library's Bessel
// function, sharing no code with the bank's design. Noise drives every band,
// the transition band included; the pairs cover downsampling with a
// non-integer half span and upsampling.
double independent_sample(const std::vector<float>& source, std::uint32_t source_rate,
                          std::uint32_t project_rate, std::int64_t output_frame) {
  using View = warp::RateConvertedSourceView;
  // Output frames outside the converted length read as silence.
  const auto frames = warp::project_rate_frames(source.size(), source_rate, project_rate);
  if (!frames || output_frame < 0 || static_cast<std::uint64_t>(output_frame) >= frames.value())
    return 0.0;
  const double lower = std::min(1.0, static_cast<double>(project_rate) / source_rate);
  const double cutoff = View::cutoff * lower;
  const double half_span = View::zero_crossings / lower;
  const double position =
      static_cast<double>(output_frame) * source_rate / static_cast<double>(project_rate);
  const auto first = static_cast<std::int64_t>(std::floor(position - half_span));
  const auto last = static_cast<std::int64_t>(std::ceil(position + half_span));
  const double window_scale = std::cyl_bessel_i(0.0, View::kaiser_beta);
  double sum = 0.0;
  double weight_sum = 0.0;
  for (auto frame = first; frame <= last; ++frame) {
    const double distance = static_cast<double>(frame) - position;
    if (std::fabs(distance) >= half_span)
      continue;
    const double ratio = distance / half_span;
    const double argument = std::numbers::pi * cutoff * distance;
    const double sinc = distance == 0.0 ? 1.0 : std::sin(argument) / argument;
    const double weight =
        cutoff * sinc * std::cyl_bessel_i(0.0, View::kaiser_beta * std::sqrt(1.0 - ratio * ratio)) /
        window_scale;
    weight_sum += weight;
    if (frame >= 0 && static_cast<std::size_t>(frame) < source.size())
      sum += weight * source[static_cast<std::size_t>(frame)];
  }
  return sum / weight_sum;
}

void bank_matches_independent_design() {
  constexpr double bank_tolerance = 2e-6;
  constexpr std::int64_t edge_frames = 5;
  constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 3> pairs{
      {{oscillate::test::sample_rate_48k, oscillate::test::sample_rate_44k},
       {oscillate::test::sample_rate_96k, oscillate::test::sample_rate_44k},
       {oscillate::test::sample_rate_44k, oscillate::test::sample_rate_48k}}};
  const auto source = noise(9'001, 7);
  warp::MemorySourceView memory(source.left, source.right);
  for (const auto& [source_rate, project_rate] : pairs) {
    auto view = make_view(memory, source_rate, project_rate);
    const auto out =
        read_all(*view, -edge_frames, static_cast<std::size_t>(view->frames() + 2 * edge_frames));
    double worst = 0.0;
    for (std::size_t index = 0; index < out.left.size(); ++index) {
      const auto frame = static_cast<std::int64_t>(index) - edge_frames;
      worst = std::max({worst,
                        std::fabs(out.left[index] - independent_sample(source.left, source_rate,
                                                                       project_rate, frame)),
                        std::fabs(out.right[index] - independent_sample(source.right, source_rate,
                                                                        project_rate, frame))});
    }
    REQUIRE_LT(worst, bank_tolerance,
               "bank differs from the independent design for " +
                   pair_name(source_rate, project_rate) + ": " + std::to_string(worst));
  }
}

// Seek and partition: any start frame and any split reproduce one continuous
// read bit for bit, including reads straddling both ends and block edges.
void reads_are_seek_and_partition_exact() {
  const auto source = noise(12'345, 3);
  warp::MemorySourceView memory(source.left, source.right);
  warp_fixtures::Random random(4);
  for (const auto source_rate : rates) {
    for (const auto project_rate : rates) {
      auto view = make_view(memory, source_rate, project_rate);
      const auto total = static_cast<std::int64_t>(view->frames());
      const auto whole = read_all(*view, -300, static_cast<std::size_t>(total + 600));
      for (int trial = 0; trial < 40; ++trial) {
        const auto first =
            static_cast<std::int64_t>(random.uniform() * static_cast<double>(total + 400)) - 300;
        const auto length =
            static_cast<std::size_t>(1 + random.uniform() * (trial % 2 == 0 ? 37.0 : 5'000.0));
        const auto part = read_all(*view, first, length);
        bool exact = true;
        for (std::size_t index = 0; index < length; ++index) {
          const auto at = first + 300 + static_cast<std::int64_t>(index);
          const bool inside = at >= 0 && at < static_cast<std::int64_t>(whole.left.size());
          const float left = inside ? whole.left[static_cast<std::size_t>(at)] : 0.0F;
          const float right = inside ? whole.right[static_cast<std::size_t>(at)] : 0.0F;
          exact = exact && part.left[index] == left && part.right[index] == right;
        }
        require(exact,
                "seek or partition changed samples for " + pair_name(source_rate, project_rate));
      }
      // Outside [0, frames()) reads as silence.
      bool silent = true;
      for (std::int64_t index = 0; index < 300; ++index) {
        silent = silent && whole.left[static_cast<std::size_t>(index)] == 0.0F &&
                 whole.right[static_cast<std::size_t>(index)] == 0.0F &&
                 whole.left[static_cast<std::size_t>(total + 300 + index)] == 0.0F &&
                 whole.right[static_cast<std::size_t>(total + 300 + index)] == 0.0F;
      }
      require(silent, "frames outside the converted range were not silent");
    }
  }
}

// Trim boundaries: a clip that starts and ends inside the source converts its
// first and last frame from real source on both sides, not from silence.
void trimmed_interior_reads_use_neighbouring_source() {
  const auto source = sines(44'100, 44'100, 1'000.0, 1'500.0);
  warp::MemorySourceView memory(source.left, source.right);
  auto view = make_view(memory, 44'100, 48'000);
  // Source frames 4,410 and 39,690 (0.1 s and 0.9 s) are exactly project
  // frames 4,800 and 43,200.
  for (const std::int64_t boundary : {4'800, 43'200}) {
    const auto out = read_all(*view, boundary - 1, 3);
    for (std::int64_t offset = 0; offset < 3; ++offset) {
      const double time = static_cast<double>(boundary - 1 + offset) / 48'000.0;
      const double expected = 0.5 * std::sin(2.0 * std::numbers::pi * 1'000.0 * time);
      REQUIRE_LT(std::fabs(out.left[static_cast<std::size_t>(offset)] - expected), 1.6e-5,
                 "trim boundary sample did not see neighbouring source");
    }
  }
}

class FailingView final : public warp::SourceView {
public:
  [[nodiscard]] std::uint64_t frames() const noexcept override { return 48'000; }
  [[nodiscard]] bool read(std::int64_t, std::span<float> left,
                          std::span<float> right) noexcept override {
    std::ranges::fill(left, 1.0F);
    std::ranges::fill(right, 1.0F);
    return false;
  }
};

void rejects_unsupported_rates_and_reports_failures() {
  const auto source = noise(1'000, 5);
  warp::MemorySourceView memory(source.left, source.right);
  require(!warp::RateConvertedSourceView::create(memory, 48'000, 32'000),
          "unsupported project rate accepted");
  require(!warp::RateConvertedSourceView::create(memory, 7'999, 48'000),
          "unsupported source rate accepted");
  auto period = warp::RateConvertedSourceView::create(memory, 44'101, 48'000);
  REQUIRE_TRUE(!period, "unbounded phase period accepted");
  REQUIRE_EQ(period.error().code, oscillate::foundation::ErrorCode::unsupported,
             "unbounded phase period accepted");
  // Non-standard but bounded source rates are converted too.
  auto low = make_view(memory, 22'050, 48'000);
  REQUIRE_TRUE(!low->bypass(), "bounded non-standard rate was not converted");
  REQUIRE_EQ(low->frames(), warp::project_rate_frames(1'000, 22'050, 48'000).value(),
             "bounded non-standard rate was not converted");
  FailingView failing;
  auto view = make_view(failing, 44'100, 48'000);
  std::vector<float> left(5'000, 1.0F);
  std::vector<float> right(5'000, 1.0F);
  REQUIRE_TRUE(!view->read(0, left, right),
               "wrapped read failure was not reported as silence and false");
  REQUIRE_TRUE(std::ranges::all_of(left, [](float value) { return value == 0.0F; }),
               "wrapped read failure was not reported as silence and false");
  REQUIRE_TRUE(std::ranges::all_of(right, [](float value) { return value == 0.0F; }),
               "wrapped read failure was not reported as silence and false");
  std::vector<float> short_right(4'999);
  require(!view->read(0, left, short_right), "mismatched channel spans accepted");
  // A same-rate view passes reads straight through, failures included.
  auto bypass = make_view(failing, 48'000, 48'000);
  REQUIRE_TRUE(bypass->bypass(), "a same-rate view was not a bypass");
  std::ranges::fill(left, 1.0F);
  std::ranges::fill(right, 1.0F);
  REQUIRE_TRUE(!bypass->read(0, left, right),
               "bypass read failure was not reported as silence and false");
  REQUIRE_TRUE(std::ranges::all_of(left, [](float value) { return value == 0.0F; }),
               "bypass read failure was not reported as silence and false");
  REQUIRE_TRUE(std::ranges::all_of(right, [](float value) { return value == 0.0F; }),
               "bypass read failure was not reported as silence and false");
}

void reads_do_not_allocate() {
  const auto source = noise(96'000, 6);
  warp::MemorySourceView memory(source.left, source.right);
  auto view = make_view(memory, 96'000, 44'100);
  std::vector<float> left(10'000);
  std::vector<float> right(10'000);
  process_allocations = 0;
  counting_process_allocations = true;
  const bool read = view->read(1'234, left, right) && view->read(-50, left, right);
  counting_process_allocations = false;
  REQUIRE_TRUE(read, "converted read allocated");
  REQUIRE_EQ(process_allocations.load(), 0, "converted read allocated");
}

// Converted output feeds a warp stream exactly like a native-rate source.
void stream_reads_converted_source() {
  const auto source = sines(44'100, 44'100, 440.0, 440.0);
  warp::MemorySourceView memory(source.left, source.right);
  auto view = make_view(memory, 44'100, 48'000);
  auto map = warp::WarpTimeMap::create(
      {{0, view->frames(), 0, view->frames() * warp::source_unit, false}});
  REQUIRE_OK(map, "identity map rejected");
  auto stream = warp::create_repitch_stream({48'000, 4'096}, warp::QualityTier::q0);
  REQUIRE_OK(stream, "Re-Pitch stream unavailable");
  REQUIRE_EQ(stream.value()->start(map.value(), *view, 0), warp::StreamStatus::ok,
             "stream rejected converted view");
  std::vector<float> left(4'096);
  std::vector<float> right(4'096);
  std::vector<float> rendered;
  while (stream.value()->next_output_frame() < map.value().output_frames()) {
    const auto count = stream.value()->render(left, right);
    REQUIRE_GT(count, 0, "stream stalled on converted view");
    rendered.insert(rendered.end(), left.begin(),
                    left.begin() + static_cast<std::ptrdiff_t>(count));
  }
  REQUIRE_EQ(rendered.size(), 48'000, "converted identity changed duration");
  // 440 Hz at 48 kHz: the pitch held instead of rising by 48/44.1.
  double worst = 0.0;
  for (std::size_t frame = 2'000; frame < 46'000; ++frame) {
    const double expected =
        0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / 48'000.0);
    worst = std::max(worst, std::fabs(rendered[frame] - expected));
  }
  REQUIRE_LT(worst, 1e-4, "converted identity changed pitch or phase");
}

} // namespace

int main() {
  try {
    every_pair_keeps_duration_and_bypasses_equal_rates();
    passband_matches_analytic_sines();
    stopband_rejects_aliases();
    bank_matches_double_reference();
    bank_matches_independent_design();
    reads_are_seek_and_partition_exact();
    trimmed_interior_reads_use_neighbouring_source();
    rejects_unsupported_rates_and_reports_failures();
    reads_do_not_allocate();
    stream_reads_converted_source();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
