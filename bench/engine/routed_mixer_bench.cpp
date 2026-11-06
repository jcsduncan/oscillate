#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/limiter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
class MasterBenchRuntime final : public oscillate::engine::ProcessorRuntime {
public:
  MasterBenchRuntime(oscillate::native_dsp::PreparedEq eq,
                     oscillate::native_dsp::PreparedCompressor compressor)
      : eq_(eq), compressor_(compressor) {}

  void process(std::span<oscillate::engine::StereoFrame> stereo,
               oscillate::engine::BlockContext context) noexcept override {
    eq_.process(stereo, context);
    compressor_.process(stereo, context);
  }

private:
  oscillate::native_dsp::EqRuntime eq_;
  oscillate::native_dsp::CompressorRuntime compressor_;
};

class NativeTrackBenchRuntime final : public oscillate::engine::ProcessorRuntime {
public:
  NativeTrackBenchRuntime(oscillate::native_dsp::PreparedEq eq,
                          oscillate::native_dsp::PreparedCompressor compressor)
      : eq_(eq), compressor_(compressor) {}
  void process(std::span<oscillate::engine::StereoFrame> stereo,
               oscillate::engine::BlockContext context) noexcept override {
    eq_.process(stereo, context);
    compressor_.process(stereo, context);
  }

private:
  oscillate::native_dsp::EqRuntime eq_;
  oscillate::native_dsp::CompressorRuntime compressor_;
};

class NativeTrackLimiterBenchRuntime final : public oscillate::engine::ProcessorRuntime {
public:
  NativeTrackLimiterBenchRuntime(oscillate::native_dsp::PreparedEq eq,
                                 oscillate::native_dsp::PreparedLimiter limiter)
      : eq_(eq), limiter_(limiter) {}
  void process(std::span<oscillate::engine::StereoFrame> stereo,
               oscillate::engine::BlockContext context) noexcept override {
    eq_.process(stereo, context);
    limiter_.process(stereo, context);
  }

private:
  oscillate::native_dsp::EqRuntime eq_;
  oscillate::native_dsp::LimiterRuntime limiter_;
};

// Measures the routed serial dispatch/PDC cost without claiming to measure a
// third-party VST3 host. The actual plugin callback cost belongs to G7.
class SyntheticSevenFrameGainRuntime final : public oscillate::engine::ProcessorRuntime {
public:
  void process(std::span<oscillate::engine::StereoFrame> stereo,
               oscillate::engine::BlockContext) noexcept override {
    for (auto& frame : stereo) {
      const auto input = frame;
      frame = history_[cursor_];
      history_[cursor_] = {input.left * 0.5F, input.right * 0.5F};
      cursor_ = (cursor_ + 1) % history_.size();
    }
  }

private:
  std::array<oscillate::engine::StereoFrame, 7> history_{};
  std::size_t cursor_ = 0;
};

class SyntheticMixedBenchRuntime final : public oscillate::engine::ProcessorRuntime {
public:
  SyntheticMixedBenchRuntime(std::shared_ptr<oscillate::engine::ProcessorRuntime> native,
                             bool native_first) {
    auto synthetic = std::make_shared<SyntheticSevenFrameGainRuntime>();
    if (native_first) {
      first_ = std::move(native);
      second_ = std::move(synthetic);
    } else {
      first_ = std::move(synthetic);
      second_ = std::move(native);
    }
  }
  void process(std::span<oscillate::engine::StereoFrame> stereo,
               oscillate::engine::BlockContext context) noexcept override {
    first_->process(stereo, context);
    second_->process(stereo, context);
  }

private:
  std::shared_ptr<oscillate::engine::ProcessorRuntime> first_;
  std::shared_ptr<oscillate::engine::ProcessorRuntime> second_;
};

int run_channel_gain_bench() {
  using namespace oscillate::engine;
  constexpr std::size_t tracks = 32;
  constexpr std::size_t iterations = 20'000;
  constexpr std::size_t warmup = 2'000;
  std::array<std::array<StereoFrame, 128>, tracks> audio{};
  for (auto& source : audio)
    source.fill({0.01F, -0.01F});
  for (const std::size_t frames : {64U, 128U}) {
    for (const int gain_mode : {0, 1, 2}) {
      std::vector<RoutedNode> nodes;
      std::vector<RoutingEdge> edges;
      std::vector<std::span<const StereoFrame>> sources;
      nodes.reserve(tracks + 1);
      edges.reserve(tracks);
      sources.reserve(tracks + 1);
      for (std::size_t index = 0; index < tracks; ++index) {
        nodes.push_back({{index + 1, 0},
                         {},
                         {},
                         false,
                         gain_mode == 1 ? StereoFrame{0.8F, 0.6F} : StereoFrame{1.0F, 1.0F}});
        edges.push_back({index + 1, index + 1, tracks + 1});
        sources.emplace_back(std::span(audio[index]).first(frames));
      }
      nodes.push_back({{tracks + 1, 0},
                       {},
                       {},
                       false,
                       gain_mode == 2 ? StereoFrame{0.75F, 0.75F} : StereoFrame{1.0F, 1.0F}});
      sources.emplace_back();
      auto prepared = prepare_routed_mixer(nodes, edges,
                                           {{tracks + 1, 0},
                                            frames,
                                            (tracks + 2) * frames * sizeof(StereoFrame),
                                            tracks * sizeof(StereoFrame)});
      if (!prepared)
        throw std::runtime_error(prepared.error().message);
      auto mixer = std::move(prepared).value();
      std::array<StereoFrame, 128> output{};
      for (std::size_t block = 0; block < warmup; ++block)
        if (!mixer->mix_block(sources, std::span(output).first(frames), {block * frames, 1}))
          return EXIT_FAILURE;
      std::vector<double> micros(iterations);
      for (std::size_t block = warmup; block < warmup + iterations; ++block) {
        const auto start = std::chrono::steady_clock::now();
        if (!mixer->mix_block(sources, std::span(output).first(frames), {block * frames, 1}))
          return EXIT_FAILURE;
        micros[block - warmup] =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
      }
      std::ranges::sort(micros);
      const char* label = gain_mode == 0   ? "unity"
                          : gain_mode == 1 ? "0.8L/0.6R per track"
                                           : "0.75 master";
      std::cout << "48 kHz " << frames << " frames, 32 tracks, channel gain " << label
                << ": median " << micros[iterations / 2] << " us, p99 "
                << micros[iterations * 99 / 100 - 1] << " us, checksum " << output[0].left << '/'
                << output[0].right << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_gain_ramp_bench() {
  using namespace oscillate::engine;
  constexpr std::size_t tracks = 32;
  constexpr std::size_t iterations = 2'000;
  std::array<std::array<StereoFrame, 128>, tracks> audio{};
  for (auto& source : audio)
    source.fill({0.01F, -0.01F});
  for (const std::size_t frames : {64U, 128U}) {
    std::vector<RoutedNode> nodes;
    std::vector<RoutingEdge> edges;
    std::vector<std::span<const StereoFrame>> sources;
    nodes.reserve(tracks + 1);
    edges.reserve(tracks);
    sources.reserve(tracks + 1);
    for (std::size_t index = 0; index < tracks; ++index) {
      nodes.push_back({{index + 1, 0}, {}});
      edges.push_back({index + 1, index + 1, tracks + 1});
      sources.emplace_back(std::span(audio[index]).first(frames));
    }
    nodes.push_back({{tracks + 1, 0}, {}});
    sources.emplace_back();
    const RoutedMixerLimits limits{{tracks + 1, 0},
                                   frames,
                                   (tracks + 2) * frames * sizeof(StereoFrame),
                                   tracks * sizeof(StereoFrame),
                                   0,
                                   128};
    auto prepared = prepare_routed_mixer(nodes, edges, limits);
    if (!prepared)
      throw std::runtime_error(prepared.error().message);
    auto mixer = std::move(prepared).value();
    std::array<StereoFrame, 128> output{};
    std::vector<double> micros(iterations);
    if (!mixer->mix_block(sources, std::span(output).first(frames), {0, 1}))
      return EXIT_FAILURE;
    for (std::size_t block = 1; block <= iterations; ++block) {
      const auto position = static_cast<std::uint64_t>(block * frames);
      for (std::size_t track = 0; track < tracks; ++track)
        nodes[track].output_gain =
            block % 2 == 1 ? StereoFrame{0.8F, 0.6F} : StereoFrame{1.0F, 1.0F};
      prepared = prepare_routed_mixer(nodes, edges, limits, mixer.get(), {position, 1});
      if (!prepared)
        throw std::runtime_error(prepared.error().message);
      mixer = std::move(prepared).value();
      const auto start = std::chrono::steady_clock::now();
      if (!mixer->mix_block(sources, std::span(output).first(frames), {position, 1}))
        return EXIT_FAILURE;
      micros[block - 1] =
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
              .count();
    }
    std::ranges::sort(micros);
    std::cout << "48 kHz " << frames << " frames, 32 changing track gains, 128-sample ramp: median "
              << micros[iterations / 2] << " us, p99 " << micros[iterations * 99 / 100 - 1]
              << " us, checksum " << output[0].left << '/' << output[0].right << '\n';
  }
  return EXIT_SUCCESS;
}

int run_bus_bench() {
  using namespace oscillate::engine;
  constexpr std::size_t tracks = 32;
  constexpr std::size_t iterations = 20'000;
  constexpr std::size_t warmup = 2'000;
  const auto limiter = oscillate::native_dsp::prepare_limiter({-1.0, 1.0, 100.0}, 48'000);
  if (!limiter)
    throw std::runtime_error("mixed bus benchmark limiter preparation failed");
  std::array<std::array<StereoFrame, 128>, tracks> audio{};
  for (auto& track : audio)
    track.fill({0.001F, -0.001F});
  for (const auto block_frames : {64U, 128U}) {
    for (const auto mode : {0, 1, 2}) {
      std::vector<RoutedNode> nodes;
      std::vector<RoutingEdge> edges;
      nodes.reserve(tracks + 2);
      edges.reserve(tracks + 2);
      for (std::size_t index = 0; index < tracks; ++index) {
        nodes.push_back({{index + 1, 0}, {}});
        edges.push_back({index + 1, index + 1, tracks + 2, index % 2 == 0 ? 1.0F : 0.5F});
      }
      std::shared_ptr<ProcessorRuntime> bus_processor;
      if (mode != 0)
        bus_processor = std::make_shared<SyntheticMixedBenchRuntime>(
            std::make_shared<oscillate::native_dsp::LimiterRuntime>(limiter.value()), mode == 1);
      nodes.push_back({{tracks + 1, mode == 0 ? 0U : limiter.value().latency_frames() + 7U},
                       std::move(bus_processor)});
      nodes.push_back({{tracks + 2, 0}, {}});
      edges.push_back({tracks + 1, 1, tracks + 1, 1.0F});
      edges.push_back({tracks + 2, tracks + 1, tracks + 2, 1.0F});
      const RoutedMixerLimits limits{{tracks + 2, 64},
                                     block_frames,
                                     (tracks + 3) * block_frames * sizeof(StereoFrame),
                                     (tracks + 2) * 65 * sizeof(StereoFrame),
                                     0};
      auto prepared = prepare_routed_mixer(nodes, edges, limits);
      if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return EXIT_FAILURE;
      }
      auto mixer = std::move(prepared).value();
      std::vector<std::span<const StereoFrame>> sources;
      sources.reserve(tracks + 2);
      for (const auto& track : audio)
        sources.emplace_back(std::span(track).first(block_frames));
      sources.emplace_back();
      sources.emplace_back();
      std::array<StereoFrame, 128> output{};
      std::vector<double> micros(iterations);
      for (std::size_t block = 0; block < warmup; ++block)
        if (!mixer->mix_block(sources, std::span(output).first(block_frames),
                              {block * block_frames, 1}))
          return EXIT_FAILURE;
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t block = warmup; block < warmup + iterations; ++block) {
        const auto block_start = std::chrono::steady_clock::now();
        if (!mixer->mix_block(sources, std::span(output).first(block_frames),
                              {block * block_frames, 1}))
          return EXIT_FAILURE;
        micros[block - warmup] = std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - block_start)
                                     .count();
      }
      const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
      const auto deadline = static_cast<double>(block_frames) * 1'000'000.0 / 48'000.0;
      const auto misses =
          std::ranges::count_if(micros, [deadline](double value) { return value > deadline; });
      std::ranges::sort(micros);
      std::cout << "48 kHz " << block_frames << " frames, 32 sources, one parallel mixed bus "
                << (mode == 0   ? "off"
                    : mode == 1 ? "limiter+synthetic 7-frame gain"
                                : "synthetic 7-frame gain+limiter")
                << ": " << wall.count() * 1'000'000.0 / iterations << " us/block wall mean, median "
                << micros[iterations / 2] << " us, p99 " << micros[(iterations * 99) / 100 - 1]
                << " us, worst " << micros.back() << " us, observed deadline misses " << misses
                << "/" << iterations << ", deadline " << deadline << " us, checksum "
                << output[0].left << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_bench() {
  using namespace oscillate::engine;
  constexpr std::size_t tracks = 32;
  constexpr std::size_t iterations = 20'000;
  constexpr std::size_t warmup = 2'000;
  std::vector<RoutedNode> nodes;
  std::vector<RoutingEdge> edges;
  for (std::size_t index = 0; index < tracks; ++index) {
    nodes.push_back({{index + 1, index + 1 == tracks ? 64U : 0U}, {}});
    edges.push_back({index + 1, index + 1, tracks + 1, index % 2 == 0 ? 1.0F : 0.5F});
  }
  nodes.push_back({{tracks + 1, 0}, {}});
  std::array<std::array<StereoFrame, 128>, tracks> audio{};
  for (auto& track : audio) {
    track.fill({0.001F, -0.001F});
  }
  const std::array<oscillate::native_dsp::EqBandSettings, 1> bands{
      {{oscillate::native_dsp::EqBandKind::peaking, 1'000.0, 1.0, 3.0}}};
  auto eq = oscillate::native_dsp::prepare_eq(bands, 48'000);
  auto compressor = oscillate::native_dsp::prepare_compressor({}, 48'000);
  auto limiter = oscillate::native_dsp::prepare_limiter({-1.0, 1.0, 100.0}, 48'000);
  if (!eq || !compressor || !limiter)
    throw std::runtime_error("master benchmark preparation failed");

  for (const auto with_master_chain : {false, true}) {
    for (const auto block_frames : {64U, 128U}) {
      for (const auto with_source_history : {false, true}) {
        for (const auto track_mode : {0, 1, 3, 4, 5, 6, 7, 10, 11}) {
          if (track_mode >= 4 && (with_master_chain || with_source_history))
            continue;
          if (track_mode == 1)
            nodes.front().processor =
                std::make_shared<NativeTrackBenchRuntime>(eq.value(), compressor.value());
          else if (track_mode == 3)
            nodes.front().processor =
                std::make_shared<NativeTrackLimiterBenchRuntime>(eq.value(), limiter.value());
          else if (track_mode == 4 || track_mode == 5)
            nodes.front().processor = std::make_shared<SyntheticMixedBenchRuntime>(
                std::make_shared<oscillate::native_dsp::EqRuntime>(eq.value()), track_mode == 4);
          else if (track_mode == 6 || track_mode == 7)
            nodes.front().processor = std::make_shared<SyntheticMixedBenchRuntime>(
                std::make_shared<oscillate::native_dsp::CompressorRuntime>(compressor.value()),
                track_mode == 6);
          else if (track_mode == 10 || track_mode == 11)
            nodes.front().processor = std::make_shared<SyntheticMixedBenchRuntime>(
                std::make_shared<oscillate::native_dsp::LimiterRuntime>(limiter.value()),
                track_mode == 10);
          else
            nodes.front().processor.reset();
          nodes.front().routing.processor_latency_frames =
              track_mode == 3    ? limiter.value().latency_frames()
              : track_mode >= 10 ? limiter.value().latency_frames() + 7U
              : track_mode >= 4  ? 7U
                                 : 0U;
          nodes.back().processor =
              with_master_chain
                  ? std::make_shared<MasterBenchRuntime>(eq.value(), compressor.value())
                  : nullptr;
          const RoutedMixerLimits limits{
              {tracks + 1, 64},
              block_frames,
              (tracks + 2) * block_frames * sizeof(StereoFrame),
              tracks * 65 * sizeof(StereoFrame),
              with_source_history ? (tracks + 1) * 65 * sizeof(StereoFrame) : 0};
          auto prepared = prepare_routed_mixer(nodes, edges, limits);
          if (!prepared) {
            std::cerr << prepared.error().message << '\n';
            return EXIT_FAILURE;
          }
          auto mixer = std::move(prepared).value();
          std::vector<std::span<const StereoFrame>> sources;
          sources.reserve(tracks + 1);
          for (const auto& track : audio) {
            sources.emplace_back(std::span(track).first(block_frames));
          }
          sources.emplace_back();
          std::array<StereoFrame, 128> output{};
          std::vector<double> block_micros(iterations);
          for (std::size_t block = 0; block < warmup; ++block) {
            if (!mixer->mix_block(sources, std::span(output).first(block_frames),
                                  {block * block_frames, 1}))
              return EXIT_FAILURE;
          }
          const auto start = std::chrono::steady_clock::now();
          for (std::size_t block = warmup; block < warmup + iterations; ++block) {
            const auto block_start = std::chrono::steady_clock::now();
            if (!mixer->mix_block(sources, std::span(output).first(block_frames),
                                  {block * block_frames, 1})) {
              return EXIT_FAILURE;
            }
            block_micros[block - warmup] = std::chrono::duration<double, std::micro>(
                                               std::chrono::steady_clock::now() - block_start)
                                               .count();
          }
          const auto elapsed =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
          const auto micros_per_block = elapsed.count() * 1'000'000.0 / iterations;
          const auto deadline = static_cast<double>(block_frames) * 1'000'000.0 / 48'000.0;
          const auto misses = std::ranges::count_if(
              block_micros, [deadline](double duration) { return duration > deadline; });
          std::ranges::sort(block_micros);
          std::cout << "48 kHz " << block_frames << " frames, " << tracks
                    << " sources (alternating unity/half-gain), native master chain "
                    << (with_master_chain ? "on" : "off") << ", native track chain "
                    << (track_mode == 1    ? "EQ+compressor"
                        : track_mode == 3  ? "EQ+limiter"
                        : track_mode == 4  ? "EQ+synthetic 7-frame gain"
                        : track_mode == 5  ? "synthetic 7-frame gain+EQ"
                        : track_mode == 6  ? "compressor+synthetic 7-frame gain"
                        : track_mode == 7  ? "synthetic 7-frame gain+compressor"
                        : track_mode == 10 ? "limiter+synthetic 7-frame gain"
                        : track_mode == 11 ? "synthetic 7-frame gain+limiter"
                                           : "off")
                    << ", source history " << (with_source_history ? "on" : "off") << ": "
                    << micros_per_block << " us/block wall mean, median "
                    << block_micros[iterations / 2] << " us, p99 "
                    << block_micros[(iterations * 99) / 100 - 1] << " us, worst "
                    << block_micros.back() << " us, observed deadline misses " << misses << "/"
                    << iterations << ", deadline " << deadline << " us, checksum " << output[0].left
                    << '\n';
        }
      }
    }
  }
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--channel-gain")
      return run_channel_gain_bench();
    if (argc == 2 && std::string_view(argv[1]) == "--gain-ramp")
      return run_gain_ramp_bench();
    if (run_bench() != EXIT_SUCCESS)
      return EXIT_FAILURE;
    return run_bus_bench();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
