#include "fixtures.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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
namespace analysis = oscillate::warp_dsp::analysis;
constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;

using oscillate::test::require;

struct Rendered {
  std::vector<float> left;
  std::vector<float> right;
};

class FailingAlignSource final : public warp::SourceView {
public:
  FailingAlignSource(std::span<const float> left, std::span<const float> right,
                     std::size_t failing_span)
      : backing_(left, right), failing_span_(failing_span) {}
  [[nodiscard]] std::uint64_t frames() const noexcept override { return backing_.frames(); }
  [[nodiscard]] bool read(std::int64_t first, std::span<float> left,
                          std::span<float> right) noexcept override {
    if (left.size() == failing_span_)
      return false;
    return backing_.read(first, left, right);
  }

private:
  warp::MemorySourceView backing_;
  std::size_t failing_span_;
};

warp::WarpTimeMap stretch_map(std::uint64_t source_frames, double speed) {
  const auto output =
      static_cast<std::uint64_t>(std::llround(static_cast<double>(source_frames) / speed));
  auto map = warp::WarpTimeMap::create({{0, output, 0, source_frames << 16, false}});
  REQUIRE_OK(map, "stretch map rejected");
  return std::move(map).value();
}

Rendered render(warp::WarpStream& stream, const warp::WarpTimeMap& map, warp::SourceView& source,
                std::uint64_t first = 0, std::size_t block = 128) {
  REQUIRE_EQ(stream.start(map, source, first), warp::StreamStatus::ok, "start failed");
  Rendered out;
  const auto total = static_cast<std::size_t>(map.output_frames() - first);
  out.left.resize(total);
  out.right.resize(total);
  std::size_t done = 0;
  std::uint64_t random = 17;
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

warp_fixtures::Stereo harmonic(double f0, double seconds, bool shaped) {
  auto signal = warp_fixtures::silence(static_cast<std::size_t>(seconds * rate));
  const auto envelope = [shaped](double frequency) {
    if (!shaped)
      return 1.0 / (frequency / 150.0);
    const auto resonance = [frequency](double centre, double width) {
      return 1.0 / (1.0 + std::pow((frequency - centre) / width, 2.0));
    };
    return resonance(700.0, 160.0) + 0.6 * resonance(1'200.0, 200.0) + 0.02;
  };
  for (int harmonic = 1; f0 * harmonic < 6'000.0; ++harmonic) {
    const double frequency = f0 * harmonic;
    const double amplitude = 0.08 * envelope(frequency);
    for (std::size_t index = 0; index < signal.left.size(); ++index) {
      const auto sample =
          static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                      static_cast<double>(index) / rate +
                                                  0.4 * harmonic));
      signal.left[index] += sample;
      signal.right[index] += sample;
    }
  }
  return signal;
}

// Fundamental from the autocorrelation peak within a fifth of `expected`,
// robust to harmonic-rich waveforms with a weak fundamental.
double measured_frequency(const std::vector<float>& signal, std::size_t begin, std::size_t end,
                          double expected) {
  double best = 0.0;
  std::size_t best_lag = 0;
  const auto shortest = static_cast<std::size_t>(rate / (expected * 1.5));
  const auto longest = static_cast<std::size_t>(rate / (expected / 1.5));
  for (std::size_t lag = shortest; lag < longest; ++lag) {
    double sum = 0.0;
    for (auto index = begin; index + lag < end; index += 3)
      sum += static_cast<double>(signal[index]) * signal[index + lag];
    if (sum > best) {
      best = sum;
      best_lag = lag;
    }
  }
  // Parabolic refinement.
  const auto correlation = [&](std::size_t lag) {
    double sum = 0.0;
    for (auto index = begin; index + lag < end; index += 3)
      sum += static_cast<double>(signal[index]) * signal[index + lag];
    return sum;
  };
  const double before = correlation(best_lag - 1);
  const double after = correlation(best_lag + 1);
  const double curvature = before - 2.0 * best + after;
  const double offset = curvature < 0.0 ? 0.5 * (before - after) / curvature : 0.0;
  return rate / (static_cast<double>(best_lag) + offset);
}

// Strongest spectral line within a fifth of `expected` (Hann-windowed DFT
// probes every 0.25 Hz): granular output carries a grain-rate modulation that
// defeats time-domain pitch estimates, but its spectral lines stay put.
double spectral_peak(const std::vector<float>& signal, std::size_t begin, std::size_t end,
                     double expected) {
  const std::size_t length = std::min<std::size_t>(end - begin, 65'536);
  const auto span = static_cast<double>(length);
  double best = -1.0;
  double best_frequency = 0.0;
  // The sweep grid is the test input.
  // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
  for (double frequency = expected / 1.5; frequency < expected * 1.5; frequency += 0.25) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t index = 0; index < length; index += 2) {
      const double window =
          0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) / span);
      const double angle = 2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate;
      re += window * signal[begin + index] * std::cos(angle);
      im -= window * signal[begin + index] * std::sin(angle);
    }
    const double magnitude = re * re + im * im;
    if (magnitude > best) {
      best = magnitude;
      best_frequency = frequency;
    }
  }
  return best_frequency;
}

using oscillate::test::rms;

std::vector<analysis::PitchMark> marks_for(const warp_fixtures::Stereo& signal) {
  warp::MemorySourceView view(signal.left, signal.right);
  const auto track = analysis::track_pitch(view, {rate, 60.0F, 1'000.0F, 0.15F});
  REQUIRE_OK(track, "pitch tracking failed");
  auto marks = analysis::place_pitch_marks(view, track.value(), rate);
  REQUIRE_OK(marks, "pitch marking failed");
  return std::move(marks).value();
}

std::unique_ptr<warp::WarpStream> tones(std::vector<analysis::PitchMark> marks,
                                        std::int32_t cents = 0) {
  auto created = warp::create_tones_stream({rate, 8'192}, warp::QualityTier::q0,
                                           {std::move(marks), 40.0F, cents});
  REQUIRE_OK(created, created ? "" : created.error().message);
  return std::move(created).value();
}

std::unique_ptr<warp::WarpStream> texture(float flux, std::int32_t cents = 0,
                                          std::uint64_t seed = 1) {
  auto created = warp::create_texture_stream({rate, 8'192}, warp::QualityTier::q0,
                                             {100.0F, flux, cents, seed});
  REQUIRE_OK(created, created ? "" : created.error().message);
  return std::move(created).value();
}

void tones_keep_pitch_and_level() {
  const auto source = harmonic(150.0, 3.0, false);
  const auto marks = marks_for(source);
  warp::MemorySourceView view(source.left, source.right);
  const double reference = rms(source.left, rate / 2, source.left.size() - rate / 2);
  for (const double speed : {0.5, 0.8, 1.25, 2.0}) {
    const auto map = stretch_map(source.left.size(), speed);
    auto stream = tones(marks);
    const auto out = render(*stream, map, view);
    const auto margin = static_cast<std::size_t>(0.2 * rate);
    const double frequency = measured_frequency(out.left, margin, out.left.size() - margin, 150.0);
    const double level =
        20.0 * std::log10(rms(out.left, margin, out.left.size() - margin) / reference);
    std::printf("tones %.2fx: %.3f Hz (%.2f cents), level %+.2f dB\n", speed, frequency,
                1200.0 * std::log2(frequency / 150.0), level);
    REQUIRE_LT(std::fabs(1200.0 * std::log2(frequency / 150.0)), 1.0, "Tones changed pitch");
    REQUIRE_LT(std::fabs(level), 1.0, "Tones changed level");
  }
}

void tones_transpose_with_formants() {
  const auto source = harmonic(150.0, 3.0, true);
  const auto marks = marks_for(source);
  warp::MemorySourceView view(source.left, source.right);
  const auto map = stretch_map(source.left.size(), 0.9);
  for (const std::int32_t cents : {-700, -300, 300, 700}) {
    auto stream = tones(marks, cents);
    const auto out = render(*stream, map, view);
    const auto margin = static_cast<std::size_t>(0.2 * rate);
    const double expected = 150.0 * std::exp2(cents / 1200.0);
    const double frequency =
        measured_frequency(out.left, margin, out.left.size() - margin, expected);
    const double error = 1200.0 * std::log2(frequency / expected);
    std::printf("tones %+d cents: %.3f Hz (%.2f cents off)\n", cents, frequency, error);
    REQUIRE_LT(std::fabs(error), 5.0, "Tones transpose is off pitch");
  }
}

void texture_keeps_level_and_transposes() {
  auto pad = warp_fixtures::vibrato_pad(rate, 4.0);
  warp::MemorySourceView view(pad.left, pad.right);
  const double reference = rms(pad.left, std::size_t{2} * rate, std::size_t{3} * rate);
  for (const double speed : {0.25, 0.5, 2.0}) {
    const auto map = stretch_map(pad.left.size(), speed);
    auto stream = texture(30.0F);
    const auto out = render(*stream, map, view);
    // Compare the plateau after the source's 1.5 s fade-in.
    const auto from = static_cast<std::size_t>(1.8 / speed * rate);
    const auto to = std::min(out.left.size() - rate / 10, from + rate);
    const double level = 20.0 * std::log10(rms(out.left, from, to) / reference);
    std::printf("texture %.2fx: level %+.2f dB\n", speed, level);
    REQUIRE_LT(std::fabs(level), 1.5, "Texture changed level");
  }
  const auto source = harmonic(150.0, 3.0, false);
  warp::MemorySourceView tone_view(source.left, source.right);
  // Stretching alone must not detune a sustained tone by the grain rate.
  for (const double speed : {0.5, 0.8, 2.0}) {
    const auto stretched = stretch_map(source.left.size(), speed);
    auto stream = texture(0.0F);
    const auto out = render(*stream, stretched, tone_view);
    const auto margin = static_cast<std::size_t>(0.2 * rate);
    const double frequency = spectral_peak(out.left, margin, out.left.size() - margin, 150.0);
    std::printf("texture %.2fx: spectral peak %.2f Hz (%.2f cents off)\n", speed, frequency,
                1200.0 * std::log2(frequency / 150.0));
    REQUIRE_LT(std::fabs(1200.0 * std::log2(frequency / 150.0)), 10.0,
               "Texture stretch detuned a sustained tone");
  }
  const auto map = stretch_map(source.left.size(), 0.8);
  for (const std::int32_t cents : {-1'200, 700}) {
    auto stream = texture(0.0F, cents);
    const auto out = render(*stream, map, tone_view);
    const auto margin = static_cast<std::size_t>(0.2 * rate);
    const double expected = 150.0 * std::exp2(cents / 1200.0);
    const double frequency = spectral_peak(out.left, margin, out.left.size() - margin, expected);
    std::printf("texture %+d cents: spectral peak %.2f Hz (%.2f cents off)\n", cents, frequency,
                1200.0 * std::log2(frequency / expected));
    REQUIRE_LT(std::fabs(1200.0 * std::log2(frequency / expected)), 10.0,
               "Texture transpose is off pitch");
  }
}

void grains_are_deterministic() {
  const auto source = harmonic(150.0, 3.0, true);
  const auto marks = marks_for(source);
  warp::MemorySourceView view(source.left, source.right);
  auto map = warp::WarpTimeMap::create({{0, 60'000, 0, 40'000ULL << 16, false},
                                        {60'000, 90'000, 40'000ULL << 16, 100'000ULL << 16, false},
                                        {90'000, 150'000, 30'000ULL << 16, 70'000ULL << 16, true}});
  REQUIRE_OK(map, "grain map rejected");
  std::vector<std::unique_ptr<warp::WarpStream>> streams;
  streams.push_back(tones(marks, 300));
  streams.push_back(texture(60.0F, -200, 7));
  for (auto& stream : streams) {
    const auto reference = render(*stream, map.value(), view, 0, 8'192);
    for (const std::size_t block : {std::size_t{1}, std::size_t{64}, std::size_t{0}}) {
      const auto pieces = render(*stream, map.value(), view, 0, block);
      REQUIRE_EQ(pieces.left, reference.left, "grain output depends on partition");
    }
    for (const std::uint64_t seek : {1ULL, 30'000ULL, 89'990ULL, 90'030ULL, 140'000ULL}) {
      const auto tail = render(*stream, map.value(), view, seek, 0);
      require(std::equal(tail.left.begin(), tail.left.end(),
                         reference.left.begin() + static_cast<std::ptrdiff_t>(seek)),
              "grain output after seek differs");
    }
  }
  // Fluctuation and seed change the texture.
  auto calm = texture(0.0F);
  auto busy = texture(80.0F, 0, 1);
  auto other = texture(80.0F, 0, 2);
  const auto a = render(*calm, map.value(), view);
  const auto b = render(*busy, map.value(), view);
  const auto c = render(*other, map.value(), view);
  REQUIRE_NE(a.left, b.left, "flux or seed had no effect");
  REQUIRE_NE(b.left, c.left, "flux or seed had no effect");
}

void grains_do_not_allocate() {
  const auto source = harmonic(150.0, 2.0, true);
  const auto marks = marks_for(source);
  warp::MemorySourceView view(source.left, source.right);
  const auto map = stretch_map(source.left.size(), 0.7);
  std::vector<std::unique_ptr<warp::WarpStream>> streams;
  streams.push_back(tones(marks, -500));
  streams.push_back(texture(50.0F, 400));
  std::vector<float> left(128);
  std::vector<float> right(128);
  for (auto& stream : streams) {
    counting_process_allocations = true;
    const bool started = stream->start(map, view, 0) == warp::StreamStatus::ok;
    while (started && stream->next_output_frame() < map.output_frames())
      (void)stream->render(left, right);
    counting_process_allocations = false;
    require(started, "grain allocation probe failed to start");
  }
  REQUIRE_EQ(process_allocations.load(), 0, "grain start()/render() allocated");
}

void texture_reports_alignment_read_failure() {
  const auto source = harmonic(150.0, 1.0, false);
  const auto map = stretch_map(source.left.size(), 1.0);
  // At 48 kHz with the 100 ms grain used by texture(), alignment reads a
  // 480-frame model and a 1440-frame search window. Neither read may turn a
  // broken source into a plausible but incorrect unaligned grain.
  for (const std::size_t failing_span : {std::size_t{480}, std::size_t{1'440}}) {
    FailingAlignSource view(source.left, source.right, failing_span);
    auto stream = texture(30.0F);
    REQUIRE_EQ(stream->start(map, view, 0), warp::StreamStatus::ok,
               "texture failed before its alignment read");
    std::vector<float> left(8'192);
    std::vector<float> right(left.size());
    (void)stream->render(left, right);
    REQUIRE_EQ(stream->status(), warp::StreamStatus::source_failed,
               "texture alignment source failure was ignored");
  }
}

void dense_segment_map_matches_single_run() {
  const auto source = harmonic(150.0, 2.0, false);
  warp::MemorySourceView view(source.left, source.right);
  constexpr std::uint64_t frames = 65'536;
  auto single = warp::WarpTimeMap::create({{0, frames, 0, frames << 16, false}});
  REQUIRE_OK(single, "single-run map rejected");
  std::vector<warp::TimeMapSegment> segments;
  segments.reserve(frames / 16);
  for (std::uint64_t begin = 0; begin < frames; begin += 16)
    segments.push_back({begin, begin + 16, begin << 16, (begin + 16) << 16, false});
  auto dense = warp::WarpTimeMap::create(std::move(segments));
  REQUIRE_OK(dense, "dense-run map rejected");
  for (auto stream_factory : {0, 1}) {
    auto sparse_stream = stream_factory == 0 ? tones({}) : texture(30.0F);
    auto dense_stream = stream_factory == 0 ? tones({}) : texture(30.0F);
    const auto sparse_audio = render(*sparse_stream, single.value(), view);
    const auto dense_audio = render(*dense_stream, dense.value(), view);
    REQUIRE_EQ(sparse_audio.left, dense_audio.left,
               "dense continuous segments changed grain output");
  }
}

void grains_reject_invalid_quality() {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): an invalid tier on purpose.
  const auto invalid = static_cast<warp::QualityTier>(255);
  require(!warp::create_tones_stream({rate, 128}, invalid, {{}, 40.0F, 0}),
          "Tones accepted an invalid quality tier");
  require(!warp::create_texture_stream({rate, 128}, invalid, {100.0F, 0.0F, 0, 1}),
          "Texture accepted an invalid quality tier");
  const auto outside = warp::max_source_position / warp::source_unit;
  require(!warp::create_tones_stream({rate, 128}, warp::QualityTier::q0,
                                     {{{outside, 300.0F}}, 40.0F, 0}),
          "Tones accepted a pitch mark outside mapped source bounds");
}

void tones_indexed_distant_seeks_are_exact() {
  constexpr std::uint64_t frames = 18ULL * rate;
  auto source = warp_fixtures::silence(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    source.left[frame] = static_cast<float>(
        0.25 * std::sin(2.0 * std::numbers::pi * 150.0 * static_cast<double>(frame) / rate));
    source.right[frame] = source.left[frame] * 0.8F;
  }
  warp::MemorySourceView view(source.left, source.right);
  std::vector<warp::TimeMapSegment> segments;
  constexpr std::uint64_t first_end = 10ULL * rate;
  for (std::uint64_t frame = 0; frame < first_end; frame += 256)
    segments.push_back({frame, std::min(frame + 256, first_end), frame << 16,
                        std::min(frame + 256, first_end) << 16, false});
  segments.push_back({10ULL * rate, 14ULL * rate, 0, (4ULL * rate) << 16, true});
  segments.push_back({14ULL * rate, frames, (8ULL * rate) << 16, (12ULL * rate) << 16, true});
  auto map = warp::WarpTimeMap::create(std::move(segments));
  REQUIRE_OK(map, "long Tones map rejected");
  std::vector<analysis::PitchMark> marks;
  for (std::uint64_t frame = 160; frame < frames; frame += 320) {
    if (frame < 4ULL * rate || frame > 7ULL * rate)
      marks.push_back({frame, 320.0F});
  }
  warp::TonesSettings settings{marks, 40.0F, 300};
  const auto preparation_start = std::chrono::steady_clock::now();
  auto prepared =
      warp::prepare_tones_seek_index(map.value(), {rate, 8'192}, warp::QualityTier::q0, settings);
  const auto preparation_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - preparation_start)
                                  .count();
  REQUIRE_OK(prepared, "Tones index preparation failed");
  const auto index_stats = prepared.value()->statistics();
  REQUIRE_EQ(index_stats.runs, 3, "Tones checkpoint index is unexpectedly large");
  REQUIRE_GT(index_stats.grains, 1'000, "Tones checkpoint index is unexpectedly large");
  REQUIRE_LT(index_stats.checkpoints, index_stats.grains / 128 + index_stats.runs + 1,
             "Tones checkpoint index is unexpectedly large");
  auto created =
      warp::create_tones_stream({rate, 8'192}, warp::QualityTier::q0, settings, prepared.value());
  REQUIRE_OK(created, "indexed Tones stream rejected");
  const auto reference = render(*created.value(), map.value(), view);
  double max_seek_us = 0.0;
  for (const std::uint64_t seek : {1ULL, 3ULL * rate, 6ULL * rate, 9ULL * rate, 10ULL * rate + 12,
                                   13ULL * rate, 14ULL * rate + 20, 17ULL * rate}) {
    const auto started = std::chrono::steady_clock::now();
    counting_process_allocations = true;
    const auto status = created.value()->start(map.value(), view, seek);
    counting_process_allocations = false;
    const auto seek_us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count();
    max_seek_us = std::max(max_seek_us, seek_us);
    REQUIRE_EQ(status, warp::StreamStatus::ok, "indexed Tones distant start failed");
    constexpr std::size_t count = 4'096;
    std::vector<float> left(count);
    std::vector<float> right(count);
    REQUIRE_EQ(created.value()->render(left, right), count, "indexed Tones seek render short");
    require(std::equal(left.begin(), left.end(),
                       reference.left.begin() + static_cast<std::ptrdiff_t>(seek)),
            "indexed Tones distant seek changed left audio");
    require(std::equal(right.begin(), right.end(),
                       reference.right.begin() + static_cast<std::ptrdiff_t>(seek)),
            "indexed Tones distant seek changed right audio");
  }
  REQUIRE_EQ(process_allocations.load(), 0, "indexed Tones start allocated");
  auto unindexed = tones(marks, 300);
  const auto measure_start = [&](warp::WarpStream& stream) {
    constexpr int repetitions = 8;
    const auto started = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < repetitions; ++repeat)
      REQUIRE_EQ(stream.start(map.value(), view, 17ULL * rate), warp::StreamStatus::ok,
                 "Tones seek cost probe failed");
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
               .count() /
           repetitions;
  };
  const double unindexed_seek_us = measure_start(*unindexed);
  const double indexed_seek_us = measure_start(*created.value());
  std::printf("indexed Tones preparation %.3f ms; max distant start %.3f us\n", preparation_ms,
              max_seek_us);
  std::printf("Tones index: %zu grains, %zu checkpoints, %zu retained bytes\n", index_stats.grains,
              index_stats.checkpoints, index_stats.estimated_bytes);
  std::printf("Tones 17 s start: unindexed %.3f us, indexed %.3f us (8 calls each)\n",
              unindexed_seek_us, indexed_seek_us);

  auto copied_map = map.value();
  REQUIRE_EQ(created.value()->start(copied_map, view, 0), warp::StreamStatus::ok,
             "copied map lost its prepared identity");
  auto recreated_map = warp::WarpTimeMap::create(std::vector<warp::TimeMapSegment>(
      map.value().segments().begin(), map.value().segments().end()));
  REQUIRE_OK(recreated_map, "recreated Tones map rejected");
  REQUIRE_EQ(created.value()->start(recreated_map.value(), view, 0), warp::StreamStatus::unprepared,
             "indexed Tones accepted a separately created equal map");
  map.value() = std::move(recreated_map).value();
  REQUIRE_EQ(created.value()->start(map.value(), view, 0), warp::StreamStatus::unprepared,
             "indexed Tones accepted an in-place replaced map");
  auto identical_map = warp::WarpTimeMap::create({{0, frames, 0, frames << 16, false}});
  REQUIRE_OK(identical_map, "alternate Tones map rejected");
  REQUIRE_EQ(created.value()->start(identical_map.value(), view, 0), warp::StreamStatus::unprepared,
             "indexed Tones accepted an unprepared map");
  auto changed = settings;
  changed.transpose_cents += 100;
  require(
      !warp::create_tones_stream({rate, 8'192}, warp::QualityTier::q0, changed, prepared.value()),
      "indexed Tones accepted stale transpose settings");
  require(
      !warp::create_tones_stream({rate, 8'192}, warp::QualityTier::q1, settings, prepared.value()),
      "indexed Tones accepted stale quality tier");
  require(!warp::create_tones_stream({44'100, 8'192}, warp::QualityTier::q0, settings,
                                     prepared.value()),
          "indexed Tones accepted stale sample rate");
  require(!warp::create_tones_stream({rate, 8'192}, warp::QualityTier::q0, settings, {}),
          "indexed Tones accepted missing preparation");
}

void tones_long_dense_seek_cost() {
  constexpr std::uint64_t segment_frames = 64;
  constexpr std::uint64_t segment_count = 200'000;
  std::vector<warp::TimeMapSegment> segments;
  segments.reserve(segment_count);
  for (std::uint64_t index = 0; index < segment_count; ++index) {
    const auto first = index * segment_frames;
    segments.push_back(
        {first, first + segment_frames, first << 16, (first + segment_frames) << 16, false});
  }
  auto map = warp::WarpTimeMap::create(std::move(segments));
  REQUIRE_OK(map, "long dense Tones map rejected");
  const warp::TonesSettings settings{{}, 40.0F, 0};
  const auto prepared_started = std::chrono::steady_clock::now();
  auto prepared =
      warp::prepare_tones_seek_index(map.value(), {rate, 128}, warp::QualityTier::q0, settings);
  const auto prepare_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepared_started)
          .count();
  REQUIRE_OK(prepared, "long dense Tones preparation failed");
  auto indexed =
      warp::create_tones_stream({rate, 128}, warp::QualityTier::q0, settings, prepared.value());
  auto unindexed = warp::create_tones_stream({rate, 128}, warp::QualityTier::q0, settings);
  REQUIRE_OK(indexed, "long dense Tones factory failed");
  REQUIRE_OK(unindexed, "long dense Tones factory failed");
  auto source = warp_fixtures::silence(128);
  warp::MemorySourceView view(source.left, source.right);
  const auto average_start_us = [&](warp::WarpStream& stream) {
    constexpr int repetitions = 8;
    const auto started = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < repetitions; ++repeat)
      REQUIRE_EQ(stream.start(map.value(), view, 180ULL * rate), warp::StreamStatus::ok,
                 "long dense Tones start failed");
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
               .count() /
           repetitions;
  };
  const auto unindexed_us = average_start_us(*unindexed.value());
  const auto indexed_us = average_start_us(*indexed.value());
  const auto stats = prepared.value()->statistics();
  std::printf("Tones dense 266 s map: prep %.3f ms, unindexed seek %.3f us, indexed seek %.3f us, "
              "%zu checkpoints, %zu retained bytes\n",
              prepare_ms, unindexed_us, indexed_us, stats.checkpoints, stats.estimated_bytes);
}

} // namespace

int main() {
  try {
    tones_keep_pitch_and_level();
    tones_transpose_with_formants();
    texture_keeps_level_and_transposes();
    grains_are_deterministic();
    grains_do_not_allocate();
    texture_reports_alignment_read_failure();
    dense_segment_map_matches_single_run();
    grains_reject_invalid_quality();
    tones_indexed_distant_seeks_are_exact();
    tones_long_dense_seek_cost();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
