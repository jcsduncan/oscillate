#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/native_dsp/compressor.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace {
using namespace oscillate;

class OneFrameDetectorDelay final : public engine::ProcessorRuntime {
public:
  void process(std::span<engine::StereoFrame> frames, engine::BlockContext) noexcept override {
    for (auto& frame : frames)
      std::swap(frame, previous_);
  }

private:
  engine::StereoFrame previous_{};
};

double measure(std::size_t frames) {
  auto prepared = native_dsp::prepare_compressor({-18.0, 4.0, 6.0, 10.0, 100.0, 0.0, 80.0}, 48'000);
  if (!prepared)
    throw std::runtime_error(prepared.error().message);
  const std::array<engine::RoutedNode, 3> nodes{
      {{{1, 0}, {}},
       {{2, 1}, std::make_shared<OneFrameDetectorDelay>()},
       {{3, 0}, std::make_shared<native_dsp::CompressorRuntime>(std::move(prepared).value())}}};
  const std::array<engine::RoutingEdge, 3> edges{
      {{11, 2, 3, 1.0F, engine::RoutingEdgeKind::sidechain}, {12, 3, 1}, {13, 2, 1, 0.0F}}};
  const engine::RoutedMixerLimits limits{{1, 1},
                                         128,
                                         std::size_t{5} * 128U * sizeof(engine::StereoFrame),
                                         std::size_t{8} * sizeof(engine::StereoFrame)};
  auto result = engine::prepare_routed_mixer(nodes, edges, limits);
  if (!result)
    throw std::runtime_error(result.error().message);
  auto mixer = std::move(result).value();
  std::array<engine::StereoFrame, 128> audio{};
  std::array<engine::StereoFrame, 128> detector{};
  std::array<engine::StereoFrame, 128> output{};
  audio.fill({0.3F, -0.2F});
  detector.fill({0.8F, 0.5F});
  const std::array<std::span<const engine::StereoFrame>, 3> sources{
      std::span(detector).first(frames), std::span(audio).first(frames), {}};
  constexpr std::size_t warmup = 2'000;
  constexpr std::size_t iterations = 20'000;
  for (std::size_t block = 0; block < warmup; ++block) {
    if (!mixer->mix_block(sources, std::span(output).first(frames), {block * frames, 1}))
      throw std::runtime_error("sidechain warmup block rejected");
  }
  double checksum = 0.0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t block = warmup; block < warmup + iterations; ++block) {
    if (!mixer->mix_block(sources, std::span(output).first(frames), {block * frames, 1}))
      throw std::runtime_error("sidechain measured block rejected");
    checksum += output[0].left;
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
  if (!std::isfinite(checksum) || checksum == 0.0)
    throw std::runtime_error("sidechain benchmark returned invalid audio");
  return elapsed.count() * 1'000'000.0 / iterations;
}
} // namespace

int main() {
  try {
    for (const std::size_t frames : {64U, 128U})
      std::cout << "48 kHz latent sidechain compressor with direct target source, frames=" << frames
                << " mean_us_per_block=" << measure(frames) << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
