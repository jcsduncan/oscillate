#include "oscillate/native_dsp/convolution.hpp"
#include "oscillate/native_dsp/convolution_runtime.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
#if defined(CONVOLUTION_WRAP_MALLOC)
thread_local std::size_t fail_malloc_at = std::numeric_limits<std::size_t>::max();
thread_local std::size_t malloc_attempt = 0;
#endif
} // namespace

#if defined(CONVOLUTION_WRAP_MALLOC)
// GNU ld's --wrap=malloc contract fixes these symbol names.
extern "C" void* __real_malloc(std::size_t size);  // NOLINT(bugprone-reserved-identifier)
extern "C" void* __wrap_malloc(std::size_t size) { // NOLINT(bugprone-reserved-identifier)
  if (counting_allocations) {
    ++counted_allocations;
  }
  if (malloc_attempt++ == fail_malloc_at) {
    return nullptr;
  }
  return __real_malloc(size);
}
#endif

namespace {
using oscillate::engine::StereoFrame;
using oscillate::native_dsp::Convolution;
using oscillate::native_dsp::ConvolutionRuntime;
namespace engine = oscillate::engine;

static_assert(std::is_base_of_v<engine::ProcessorRuntime, ConvolutionRuntime>);

struct RenderSettings {
  std::uint32_t rate;
  float wet;
  std::size_t callback_size;
};
struct OracleSettings {
  std::uint32_t rate;
  float wet;
  float tolerance;
};

using oscillate::test::require;

std::uint32_t rng_state = 0x164ac19dU;
float noise() {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return static_cast<float>(static_cast<int>(rng_state & 0xffffU) - 32768) / 65536.0F;
}

std::vector<StereoFrame> reference(std::span<const StereoFrame> source,
                                   std::span<const StereoFrame> ir, float wet) {
  std::vector<StereoFrame> result(source.size() + ir.size() - 1);
  for (std::size_t n = 0; n < result.size(); ++n) {
    double left = 0.0;
    double right = 0.0;
    const auto first = n >= ir.size() ? n - ir.size() + 1 : 0;
    const auto last = std::min(n + 1, source.size());
    for (auto input = first; input < last; ++input) {
      const auto tap = n - input;
      left += static_cast<double>(source[input].left) * ir[tap].left;
      right += static_cast<double>(source[input].right) * ir[tap].right;
    }
    const auto dry_left = n < source.size() ? source[n].left : 0.0F;
    const auto dry_right = n < source.size() ? source[n].right : 0.0F;
    result[n] = {static_cast<float>((1.0F - wet) * dry_left + wet * left),
                 static_cast<float>((1.0F - wet) * dry_right + wet * right)};
  }
  return result;
}

std::vector<StereoFrame> render(std::span<const StereoFrame> source,
                                std::span<const StereoFrame> ir, RenderSettings settings) {
  auto prepared = Convolution::create(ir, settings.rate, settings.wet);
  REQUIRE_OK(prepared, "valid IR was rejected");
  auto processor = std::move(prepared).value();
  REQUIRE_EQ(processor->tail_frames(), ir.size() - 1,
             "latency or exact finite tail was misreported");
  REQUIRE_EQ(Convolution::latency_frames(), 0, "latency or exact finite tail was misreported");
  std::vector<StereoFrame> output(source.size() + ir.size() - 1);
  std::ranges::copy(source, output.begin());
  for (std::size_t offset = 0; offset < output.size(); offset += settings.callback_size) {
    processor->process(
        std::span(output).subspan(offset, std::min(settings.callback_size, output.size() - offset)),
        1);
  }
  REQUIRE_EQ(processor->guarded_frames_after_quiesce(), 0,
             "finite oracle signal unexpectedly guarded");
  return output;
}

void compare(std::span<const StereoFrame> source, std::span<const StereoFrame> ir,
             OracleSettings settings) {
  const auto expected = reference(source, ir, settings.wet);
  const auto at_64 = render(source, ir, {settings.rate, settings.wet, 64});
  const auto at_128 = render(source, ir, {settings.rate, settings.wet, 128});
  const auto at_67 = render(source, ir, {settings.rate, settings.wet, 67});
  for (std::size_t index = 0; index < expected.size(); ++index) {
    REQUIRE_EQ(at_64[index].left, at_128[index].left,
               "convolution changed output with callback partitioning");
    REQUIRE_EQ(at_64[index].right, at_128[index].right,
               "convolution changed output with callback partitioning");
    REQUIRE_EQ(at_64[index].left, at_67[index].left,
               "convolution changed output with callback partitioning");
    REQUIRE_EQ(at_64[index].right, at_67[index].right,
               "convolution changed output with callback partitioning");
    for (const auto& actual : {at_64[index], at_128[index], at_67[index]}) {
      if (std::abs(actual.left - expected[index].left) > settings.tolerance ||
          std::abs(actual.right - expected[index].right) > settings.tolerance) {
        throw std::runtime_error("FIR oracle mismatch at frame " + std::to_string(index) +
                                 ": left=" + std::to_string(actual.left) +
                                 ", expected=" + std::to_string(expected[index].left) +
                                 ", right=" + std::to_string(actual.right) +
                                 ", expected=" + std::to_string(expected[index].right));
      }
    }
  }
}

void invalid_inputs() {
  std::array<StereoFrame, 1> ir{{{1.0F, 1.0F}}};
  REQUIRE_TRUE(!Convolution::create({}, 48'000, 1.0F), "invalid settings accepted");
  REQUIRE_TRUE(!Convolution::create(ir, 44'000, 1.0F), "invalid settings accepted");
  REQUIRE_TRUE(!Convolution::create(ir, 96'001, 1.0F), "invalid settings accepted");
  REQUIRE_TRUE(!Convolution::create(ir, 48'000, -0.1F), "invalid settings accepted");
  REQUIRE_TRUE(!Convolution::create(ir, 48'000, 1.1F), "invalid settings accepted");
  REQUIRE_TRUE(!Convolution::create(ir, 48'000, std::numeric_limits<float>::quiet_NaN()),
               "invalid settings accepted");
  ir[0].right = std::numeric_limits<float>::infinity();
  require(!Convolution::create(ir, 48'000, 1.0F), "non-finite IR accepted");
  std::vector<StereoFrame> long_ir(96'001);
  require(!Convolution::create(long_ir, 48'000, 1.0F), "IR beyond 2 seconds accepted");
}

void simple_and_boundary_oracles() {
  std::array<StereoFrame, 1> input{{{1.0F, -0.25F}}};
  std::vector<StereoFrame> source(257);
  for (auto& frame : source) {
    frame = {noise(), noise()};
  }
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    const auto head = rate > 48'000 ? 128U : 64U;
    const auto middle = rate > 48'000 ? 4096U : 2048U;
    const auto last = rate > 48'000 ? 16384U : 8192U;
    for (const auto length : {1U, head - 1, head, head + 1, middle - 1, middle, middle + 1,
                              last - 1, last, last + 1, 2 * rate}) {
      std::vector<StereoFrame> ir(length);
      ir.front() = {0.25F, -0.5F};
      ir.back() = {0.125F, 0.375F};
      compare(input, ir, {rate, 1.0F, 0.0001F});
    }
    for (const auto length : {head + 6, middle + 13, last + 35}) {
      std::vector<StereoFrame> ir(length);
      for (auto& frame : ir) {
        frame = {noise() * 0.02F, noise() * 0.02F};
      }
      compare(source, ir, {rate, 0.7F, 0.0002F});
    }
  }
}

void full_ir_and_faults() {
  std::vector<StereoFrame> source(81);
  for (auto& frame : source) {
    frame = {noise(), noise()};
  }
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    std::vector<StereoFrame> full_ir(2ULL * rate);
    for (auto& frame : full_ir) {
      frame = {noise() * 0.002F, noise() * 0.002F};
    }
    compare(source, full_ir, {rate, 0.8F, 0.0002F});
  }
  std::vector<StereoFrame> ir(96'000);
  for (auto& frame : ir) {
    frame = {noise() * 0.002F, noise() * 0.002F};
  }
  auto prepared = Convolution::create(ir, 48'000, 1.0F);
  REQUIRE_OK(prepared, "full IR rejected");
  auto processor = std::move(prepared).value();
  std::array<StereoFrame, 128> invalid{};
  invalid[0].left = std::numeric_limits<float>::quiet_NaN();
  processor->process(invalid, 1);
  REQUIRE_EQ(processor->guarded_frames_after_quiesce(), 1, "invalid callback input not guarded");
  invalid.fill({1.0F, 1.0F});
  processor->process(invalid, 2);
  require(std::ranges::all_of(
              invalid, [](StereoFrame frame) { return frame.left == 0.0F && frame.right == 0.0F; }),
          "stale transport generation was processed");
  std::array<StereoFrame, 129> oversized{};
  oversized.fill({1.0F, 1.0F});
  processor->process(oversized, 1);
  require(
      std::ranges::all_of(
          oversized, [](StereoFrame frame) { return frame.left == 0.0F && frame.right == 0.0F; }),
      "oversized callback was processed");
  processor->reset_after_quiesce(2);
  invalid.fill({0.0F, 0.0F});
  invalid[0] = {1.0F, -1.0F};
  processor->process(invalid, 2);
  REQUIRE_LT(std::abs(invalid[0].left - ir[0].left), oscillate::test::tolerance::float_round_trip_f,
             "reset did not restart direct head");
  REQUIRE_LT(std::abs(invalid[0].right + ir[0].right),
             oscillate::test::tolerance::float_round_trip_f, "reset did not restart direct head");

  std::array<StereoFrame, 1> huge_ir{
      {{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()}}};
  auto huge = Convolution::create(huge_ir, 48'000, 1.0F);
  REQUIRE_OK(huge, "finite high-amplitude IR rejected");
  std::array<StereoFrame, 1> loud{{{2.0F, 2.0F}}};
  huge.value()->process(loud, 1);
  REQUIRE_EQ(loud[0].left, 0.0F, "non-finite wet result did not bypass with fault count");
  REQUIRE_EQ(loud[0].right, 0.0F, "non-finite wet result did not bypass with fault count");
  REQUIRE_EQ(huge.value()->guarded_frames_after_quiesce(), 2,
             "non-finite wet result did not bypass with fault count");
}

void callback_allocations() {
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    std::vector<StereoFrame> ir(2ULL * rate);
    for (auto& frame : ir) {
      frame = {0.0001F, 0.0001F};
    }
    auto prepared = Convolution::create(ir, rate, 0.75F);
    REQUIRE_OK(prepared, "allocation fixture IR rejected");
    auto processor = std::move(prepared).value();
    std::array<StereoFrame, 128> block{};
    counted_allocations = 0;
    counting_allocations = true;
    for (int repeat = 0; repeat < 128; ++repeat) {
      processor->process(block, 1);
    }
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "callback performed a C or C++ allocation");
  }
}

void graph_block_adapter() {
  std::array<StereoFrame, 130> ir{};
  ir[0] = {0.5F, -0.25F};
  ir[127] = {0.25F, 0.125F};
  ir[129] = {-0.125F, 0.25F};
  auto prepared = Convolution::create(ir, 48'000, 1.0F);
  REQUIRE_OK(prepared, "adapter IR rejected");
  ConvolutionRuntime runtime(std::move(prepared).value());
  REQUIRE_EQ(runtime.tail_frames(), 129, "adapter lost convolution tail or latency");
  REQUIRE_EQ(ConvolutionRuntime::latency_frames(), 0, "adapter lost convolution tail or latency");
  std::array<StereoFrame, 8192> block{};
  block[0] = {1.0F, 2.0F};
  block.back() = {-1.0F, 0.5F};
  counted_allocations = 0;
  counting_allocations = true;
  runtime.process(block, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "8192-frame adapter callback allocated");
  REQUIRE_LT(std::abs(block[0].left - 0.5F), oscillate::test::tolerance::float_processing_f,
             "adapter split changed the FIR result");
  REQUIRE_LT(std::abs(block[0].right + 0.5F), oscillate::test::tolerance::float_processing_f,
             "adapter split changed the FIR result");
  REQUIRE_LT(std::abs(block[127].left - 0.25F), oscillate::test::tolerance::float_processing_f,
             "adapter split changed the FIR result");
  REQUIRE_LT(std::abs(block[129].right - 0.5F), oscillate::test::tolerance::float_processing_f,
             "adapter split changed the FIR result");
  REQUIRE_LT(std::abs(block.back().left + 0.5F), oscillate::test::tolerance::float_processing_f,
             "adapter split changed the FIR result");
  std::array<StereoFrame, 129> tail{};
  runtime.process(tail, {8192, 1});
  REQUIRE_LT(std::abs(tail[126].left + 0.25F), oscillate::test::tolerance::float_processing_f,
             "adapter dropped the finite tail across an 8192-frame boundary");
  REQUIRE_LT(std::abs(tail[126].right - 0.0625F), oscillate::test::tolerance::float_processing_f,
             "adapter dropped the finite tail across an 8192-frame boundary");
  REQUIRE_LT(std::abs(tail[128].left - 0.125F), oscillate::test::tolerance::float_processing_f,
             "adapter dropped the finite tail across an 8192-frame boundary");
  REQUIRE_LT(std::abs(tail[128].right - 0.125F), oscillate::test::tolerance::float_processing_f,
             "adapter dropped the finite tail across an 8192-frame boundary");
  std::array<StereoFrame, 1> stale{{{1.0F, 1.0F}}};
  runtime.process(stale, {0, 2});
  REQUIRE_EQ(stale[0].left, 0.0F, "new generation used stale FFT history");
  REQUIRE_EQ(stale[0].right, 0.0F, "new generation used stale FFT history");
  runtime.reset_after_quiesce(2);
  stale[0] = {1.0F, 1.0F};
  runtime.process(stale, {0, 2});
  REQUIRE_LT(std::abs(stale[0].left - 0.5F), oscillate::test::tolerance::float_processing_f,
             "quiescent reset did not resume new generation");
  REQUIRE_LT(std::abs(stale[0].right + 0.25F), oscillate::test::tolerance::float_processing_f,
             "quiescent reset did not resume new generation");
  std::array<StereoFrame, 8193> oversized{};
  oversized.fill({1.0F, 1.0F});
  runtime.process(oversized, {1, 2});
  require(
      std::ranges::all_of(
          oversized, [](StereoFrame frame) { return frame.left == 1.0F && frame.right == 1.0F; }),
      "adapter walked or modified an out-of-contract graph block");
}

class OneShotSource final : public engine::ProcessorRuntime {
public:
  void process(std::span<StereoFrame> stereo, engine::BlockContext) noexcept override {
    if (!fired_) {
      stereo[0] = {1.0F, 1.0F};
      fired_ = true;
    }
  }

private:
  bool fired_ = false;
};

void graph_reuses_convolution_tail() {
  std::array<StereoFrame, 130> ir{};
  ir[129] = {1.0F, 1.0F};
  const auto requests = std::array<engine::NodeRequest, 2>{
      engine::NodeRequest{1, 1, [] { return std::make_unique<OneShotSource>(); }},
      engine::NodeRequest{2, 1, [ir] {
                            auto prepared = Convolution::create(ir, 48'000, 1.0F);
                            return std::make_unique<ConvolutionRuntime>(
                                std::move(prepared).value());
                          }}};
  engine::GraphPublisher publisher;
  auto first = engine::prepare_graph(1, 8192, requests, nullptr);
  REQUIRE_OK(first, "initial convolution graph failed");
  REQUIRE_EQ(publisher.publish(std::move(first).value()), engine::PublishStatus::accepted,
             "initial convolution graph failed");
  std::array<StereoFrame, 64> block{};
  require(publisher.render_block(block, {0, 1}), "first graph block failed");
  auto next = engine::prepare_graph(2, 8192, requests, publisher.active_plan());
  REQUIRE_OK(next, "topology revision replaced compatible convolution state");
  REQUIRE_EQ(next.value()->transitions()[1].kind, engine::TransitionKind::reused,
             "topology revision replaced compatible convolution state");
  REQUIRE_EQ(publisher.publish(std::move(next).value()), engine::PublishStatus::accepted,
             "topology revision replaced compatible convolution state");
  require(publisher.render_block(block, {64, 1}), "second graph block failed");
  require(publisher.render_block(block, {128, 1}), "third graph block failed");
  REQUIRE_LT(std::abs(block[1].left - 1.0F), oscillate::test::tolerance::float_processing_f,
             "graph publication lost convolution tail");
  REQUIRE_LT(std::abs(block[1].right - 1.0F), oscillate::test::tolerance::float_processing_f,
             "graph publication lost convolution tail");
}

void stale_seek_reset_preserves_tail(std::size_t block_frames) {
  std::vector<StereoFrame> ir(8193);
  ir[0] = {0.5F, -0.25F};
  ir[129] = {0.25F, 0.125F};
  ir[8192] = {0.125F, -0.0625F};
  auto prepared = Convolution::create(ir, 48'000, 1.0F);
  REQUIRE_OK(prepared, "stale-reset IR rejected");
  ConvolutionRuntime runtime(std::move(prepared).value());
  std::array<StereoFrame, 128> block{};
  block.front() = {1.0F, 2.0F};
  counted_allocations = 0;
  counting_allocations = true;
  runtime.process(std::span(block).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "initial callback allocated or missed the first IR tap");
  REQUIRE_LT(std::abs(block.front().left - 0.5F), oscillate::test::tolerance::float_processing_f,
             "initial callback allocated or missed the first IR tap");
  REQUIRE_TRUE(!runtime.reset_after_quiesce(0),
               "zero or duplicate generation erased an in-flight IR tail");
  REQUIRE_TRUE(!runtime.reset_after_quiesce(1),
               "zero or duplicate generation erased an in-flight IR tail");
  for (std::size_t start = block_frames; start <= 8192; start += block_frames) {
    block.fill({});
    counted_allocations = 0;
    counting_allocations = true;
    runtime.process(std::span(block).first(block_frames), {start, 1});
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "tail callback allocated after stale reset");
    if (start <= 129 && 129 < start + block_frames) {
      const auto& delayed = block[129 - start];
      REQUIRE_LT(std::abs(delayed.left - 0.25F), oscillate::test::tolerance::float_processing_f,
                 "stale reset erased the exact stereo IR tail");
      REQUIRE_LT(std::abs(delayed.right - 0.25F), oscillate::test::tolerance::float_processing_f,
                 "stale reset erased the exact stereo IR tail");
    }
    if (start <= 8192 && 8192 < start + block_frames) {
      const auto& delayed = block[8192 - start];
      REQUIRE_LT(std::abs(delayed.left - 0.125F), oscillate::test::tolerance::float_processing_f,
                 "stale reset erased the long-tier IR tail");
      REQUIRE_LT(std::abs(delayed.right + 0.125F), oscillate::test::tolerance::float_processing_f,
                 "stale reset erased the long-tier IR tail");
    }
  }
  require(runtime.reset_after_quiesce(2), "new generation did not reset IR history");
  REQUIRE_TRUE(!runtime.reset_after_quiesce(1),
               "older or repeated reset replaced the new IR history");
  REQUIRE_TRUE(!runtime.reset_after_quiesce(2),
               "older or repeated reset replaced the new IR history");
  block.fill({});
  runtime.process(std::span(block).first(block_frames), {0, 1});
  require(std::ranges::all_of(
              std::span(block).first(block_frames),
              [](StereoFrame frame) { return frame.left == 0.0F && frame.right == 0.0F; }),
          "old generation became eligible after a stale reset");
  block.fill({});
  block.front() = {1.0F, 2.0F};
  runtime.process(std::span(block).first(block_frames), {0, 2});
  REQUIRE_LT(std::abs(block.front().left - 0.5F), oscillate::test::tolerance::float_processing_f,
             "valid seek reset did not start the next generation cleanly");
  REQUIRE_LT(std::abs(block.front().right + 0.5F), oscillate::test::tolerance::float_processing_f,
             "valid seek reset did not start the next generation cleanly");
}

void long_tier_seek_reset() {
  std::vector<StereoFrame> ir(8193);
  ir.front() = {0.5F, -0.5F};
  ir.back() = {0.25F, 0.125F};
  auto prepared = Convolution::create(ir, 48'000, 1.0F);
  REQUIRE_OK(prepared, "long-tier reset IR rejected");
  ConvolutionRuntime runtime(std::move(prepared).value());

  std::array<StereoFrame, 128> block{};
  block.front() = {1.0F, 1.0F};
  runtime.process(block, {0, 1});
  REQUIRE_LT(std::abs(block.front().left - 0.5F), oscillate::test::tolerance::float_processing_f,
             "long-tier impulse head did not play");
  REQUIRE_LT(std::abs(block.front().right + 0.5F), oscillate::test::tolerance::float_processing_f,
             "long-tier impulse head did not play");
  for (std::size_t index = 1; index < 32; ++index) {
    block.fill({});
    runtime.process(block, {index * block.size(), 1});
  }
  std::array<StereoFrame, 37> partial{};
  runtime.process(partial, {4096, 1});

  block.fill({1.0F, 1.0F});
  runtime.process(block, {0, 2});
  require(std::ranges::all_of(
              block, [](StereoFrame frame) { return frame.left == 0.0F && frame.right == 0.0F; }),
          "new generation played before quiescent reset");
  runtime.reset_after_quiesce(2);
  for (std::size_t index = 0; index < 64; ++index) {
    block.fill({});
    runtime.process(block, {index * block.size(), 2});
    require(std::ranges::all_of(block,
                                [](StereoFrame frame) {
                                  return std::abs(frame.left) <
                                             oscillate::test::tolerance::float_processing_f &&
                                         std::abs(frame.right) <
                                             oscillate::test::tolerance::float_processing_f;
                                }),
            "old long-tier tail survived seek reset");
  }

  block.fill({});
  block.front() = {1.0F, 1.0F};
  runtime.process(block, {8192, 2});
  REQUIRE_LT(std::abs(block.front().left - 0.5F), oscillate::test::tolerance::float_processing_f,
             "quiescent reset did not restart the long-tier head");
  for (std::size_t index = 1; index < 64; ++index) {
    block.fill({});
    runtime.process(block, {index * block.size(), 2});
  }
  block.fill({});
  runtime.process(block, {16384, 2});
  REQUIRE_LT(std::abs(block.front().left - 0.25F), oscillate::test::tolerance::float_processing_f,
             "fresh long-tier tail was lost after seek reset");
  REQUIRE_LT(std::abs(block.front().right - 0.125F), oscillate::test::tolerance::float_processing_f,
             "fresh long-tier tail was lost after seek reset");
}

void preparation_failures() {
#if defined(CONVOLUTION_WRAP_MALLOC)
  std::array<StereoFrame, 65> ir{};
  // The pinned local patch handles PFFFT setup-struct and aligned-data OOM.
  for (const auto failure_index : {0U, 1U, 2U, 3U}) {
    malloc_attempt = 0;
    fail_malloc_at = failure_index;
    auto rejected = Convolution::create(ir, 48'000, 1.0F);
    fail_malloc_at = std::numeric_limits<std::size_t>::max();
    REQUIRE_TRUE(!rejected, "injected setup allocation failure was not reported");
    REQUIRE_EQ(rejected.error().code, oscillate::foundation::ErrorCode::resource_exhausted,
               "injected setup allocation failure was not reported");
  }
  REQUIRE_OK(Convolution::create(ir, 48'000, 1.0F),
             "failed preparation left the allocator or processor unusable");
#endif
}

} // namespace

int main() {
  try {
    invalid_inputs();
    simple_and_boundary_oracles();
    full_ir_and_faults();
    callback_allocations();
    graph_block_adapter();
    graph_reuses_convolution_tail();
    stale_seek_reset_preserves_tail(64);
    stale_seek_reset_preserves_tail(128);
    long_tier_seek_reset();
    preparation_failures();
    std::cout << "convolution oracle passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
