#include "allocation_probe.hpp"

#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/test/audio_constants.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace engine = oscillate::engine;
namespace dsp = oscillate::native_dsp;

constexpr std::size_t track_count = 32;
constexpr std::uint32_t sample_rate = oscillate::test::sample_rate_48k;
constexpr engine::RoutingNodeId first_group = 33;
constexpr engine::RoutingNodeId first_return = 35;
constexpr engine::RoutingNodeId master = 37;

class MasterChain final : public engine::ProcessorRuntime {
public:
  MasterChain(dsp::PreparedEq eq, dsp::PreparedCompressor compressor, dsp::PreparedLimiter limiter)
      : eq_(eq), compressor_(compressor), limiter_(limiter) {}

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override {
    eq_.process(stereo, context);
    compressor_.process(stereo, context);
    limiter_.process(stereo, context);
  }

private:
  dsp::EqRuntime eq_;
  dsp::CompressorRuntime compressor_;
  dsp::LimiterRuntime limiter_;
};

struct Options {
  std::size_t frames = 64;
  std::size_t repetitions = 3;
  std::size_t measured_blocks = 20'000;
  std::size_t warmup_blocks = 2'000;
};

std::size_t positive_number(std::string_view text) {
  std::size_t value = 0;
  if (text.empty())
    throw std::runtime_error("an option needs a positive integer");
  for (const char digit : text) {
    if (digit < '0' || digit > '9' ||
        value > (1'000'000U - static_cast<std::size_t>(digit - '0')) / 10U)
      throw std::runtime_error("option must be an integer from 1 to 1000000");
    value = value * 10 + static_cast<std::size_t>(digit - '0');
  }
  if (value == 0)
    throw std::runtime_error("option must be positive");
  return value;
}

Options parse(int argc, char** argv) {
  Options options;
  if (argc % 2 == 0)
    throw std::runtime_error("usage: rehearsal [--frames 64|128] [--repetitions N] "
                             "[--blocks N] [--warmup N]");
  for (int index = 1; index < argc; index += 2) {
    const std::string_view key(argv[index]);
    const auto value = positive_number(argv[index + 1]);
    if (key == "--frames")
      options.frames = value;
    else if (key == "--repetitions")
      options.repetitions = value;
    else if (key == "--blocks")
      options.measured_blocks = value;
    else if (key == "--warmup")
      options.warmup_blocks = value;
    else
      throw std::runtime_error("unknown rehearsal option");
  }
  if (options.frames != 64 && options.frames != 128)
    throw std::runtime_error("frames must be 64 or 128");
  return options;
}

std::vector<engine::RoutedNode> make_nodes(std::uint32_t limiter_latency,
                                           std::shared_ptr<MasterChain> chain) {
  std::vector<engine::RoutedNode> nodes;
  nodes.reserve(master);
  for (engine::RoutingNodeId id = 1; id < master; ++id)
    nodes.push_back({{id, 0}, {}});
  nodes.push_back({{master, limiter_latency}, std::move(chain)});
  return nodes;
}

std::vector<engine::RoutingEdge> make_edges() {
  std::vector<engine::RoutingEdge> edges;
  edges.reserve(track_count * 2 + 4);
  engine::RoutingEdgeId edge_id = 1;
  for (std::size_t index = 0; index < track_count; ++index) {
    const auto source = static_cast<engine::RoutingNodeId>(index + 1);
    edges.push_back({edge_id++, source, first_group + index / 16, 0.125F});
    edges.push_back({edge_id++, source, first_return + index / 16, 0.03125F});
  }
  for (engine::RoutingNodeId bus = first_group; bus < master; ++bus)
    edges.push_back({edge_id++, bus, master, 1.0F});
  return edges;
}

using SourceBlock = std::array<engine::StereoFrame, 128>;
std::array<SourceBlock, track_count> make_sources() {
  std::array<SourceBlock, track_count> sources{};
  for (std::size_t track = 0; track < track_count; ++track) {
    for (std::size_t frame = 0; frame < 128; ++frame) {
      const auto phase = static_cast<float>((frame * (track + 3)) % 127) / 127.0F;
      const float amplitude = [track, frame, phase] {
        switch (track % 4) {
        case 0:
          return 0.15F * std::sin(6.283185307179586F * phase);
        case 1:
          return frame % 32 < 4 ? 0.20F : 0.0F;
        case 2:
          return 0.08F * (2.0F * phase - 1.0F);
        default:
          return 0.06F;
        }
      }();
      sources[track][frame] = {amplitude, -amplitude * 0.75F};
    }
  }
  return sources;
}

std::shared_ptr<MasterChain> make_master(std::uint32_t& limiter_latency) {
  const std::array<dsp::EqBandSettings, 1> bands{{{dsp::EqBandKind::peaking, 1'000.0, 1.0, 3.0}}};
  auto eq = dsp::prepare_eq(bands, sample_rate);
  auto compressor = dsp::prepare_compressor({}, sample_rate);
  auto limiter = dsp::prepare_limiter({}, sample_rate);
  if (!eq || !compressor || !limiter)
    throw std::runtime_error("native master preparation failed");
  limiter_latency = limiter.value().latency_frames();
  return std::make_shared<MasterChain>(eq.value(), compressor.value(), limiter.value());
}

double percentile(std::span<const double> sorted, std::size_t numerator, std::size_t denominator) {
  const auto count = sorted.size();
  return sorted[(count * numerator + denominator - 1) / denominator - 1];
}

void run(const Options& options) {
  std::uint32_t limiter_latency = 0;
  auto chain = make_master(limiter_latency);
  const auto nodes = make_nodes(limiter_latency, std::move(chain));
  const auto edges = make_edges();
  const auto audio = make_sources();
  const auto period_us = static_cast<double>(options.frames) * 1'000'000.0 / sample_rate;
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "workload=synthetic-32-routed rate=oscillate::test::sample_rate_48k frames="
            << options.frames << " source_tracks=32 group_buses=2 return_buses=2 sends=32"
            << " master=eq+compressor+limiter limiter_latency_frames=" << limiter_latency
            << " warmup_blocks=" << options.warmup_blocks
            << " measured_blocks=" << options.measured_blocks
            << " repetitions=" << options.repetitions << " period_us=" << period_us << '\n';
  for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
    const engine::RoutedMixerLimits limits{{master, 4'096},
                                           options.frames,
                                           (nodes.size() + 1) * options.frames *
                                               sizeof(engine::StereoFrame),
                                           edges.size() * 4'097 * sizeof(engine::StereoFrame)};
    auto prepared = engine::prepare_routed_mixer(nodes, edges, limits);
    if (!prepared)
      throw std::runtime_error(prepared.error().message);
    auto mixer = std::move(prepared).value();
    std::vector<std::span<const engine::StereoFrame>> inputs(nodes.size());
    for (std::size_t slot = 0; slot < mixer->plan().topological_order.size(); ++slot) {
      const auto id = mixer->plan().topological_order[slot];
      if (id <= track_count)
        inputs[slot] = std::span(audio[id - 1]).first(options.frames);
    }
    std::array<engine::StereoFrame, 128> output{};
    std::vector<double> block_us(options.measured_blocks);
    std::uint64_t render_failures = 0;
    std::size_t over_period = 0;
    double total_us = 0.0;
    std::uint64_t position = 0;
    const auto generation = repetition + 1;
    for (std::size_t block = 0; block < options.warmup_blocks + options.measured_blocks; ++block) {
      const bool measured = block >= options.warmup_blocks;
      const auto start = std::chrono::steady_clock::now();
      begin_allocation_probe();
      const bool rendered =
          mixer->mix_block(inputs, std::span(output).first(options.frames), {position, generation});
      end_allocation_probe();
      const auto elapsed =
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
              .count();
      if (!rendered)
        ++render_failures;
      if (measured) {
        block_us[block - options.warmup_blocks] = elapsed;
        total_us += elapsed;
        over_period += elapsed > period_us ? 1U : 0U;
      }
      position += options.frames;
    }
    std::ranges::sort(block_us);
    std::cout << "run=" << repetition + 1
              << " mean_us=" << total_us / static_cast<double>(options.measured_blocks)
              << " p50_us=" << percentile(block_us, 50, 100)
              << " p95_us=" << percentile(block_us, 95, 100)
              << " p99_us=" << percentile(block_us, 99, 100) << " max_us=" << block_us.back()
              << " over_period=" << over_period
              << " callback_allocations=" << allocation_probe_count()
              << " render_failures=" << render_failures << " output_left=" << output[0].left
              << '\n';
    if (render_failures != 0 || allocation_probe_count() != 0 || !std::isfinite(output[0].left) ||
        !std::isfinite(output[0].right))
      throw std::runtime_error("rehearsal graph failed an invariant");
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    run(parse(argc, argv));
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "32-track routed rehearsal: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
