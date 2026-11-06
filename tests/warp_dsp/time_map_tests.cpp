#include "oscillate/test/check.hpp"
#include "oscillate/warp_dsp/time_map.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace warp = oscillate::warp_dsp;

using oscillate::test::require;

// Independent oracle: floor(a * b / c) by 32-bit limb multiplication and
// bitwise long division over the full 128-bit product.
std::uint64_t mul_div_floor(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
  const std::uint64_t mask = 0xffff'ffffULL;
  const std::uint64_t a0 = a & mask;
  const std::uint64_t a1 = a >> 32;
  const std::uint64_t b0 = b & mask;
  const std::uint64_t b1 = b >> 32;
  const std::uint64_t p00 = a0 * b0;
  const std::uint64_t p01 = a0 * b1;
  const std::uint64_t p10 = a1 * b0;
  const std::uint64_t p11 = a1 * b1;
  const std::uint64_t middle = (p00 >> 32) + (p01 & mask) + (p10 & mask);
  std::uint64_t low = (p00 & mask) | (middle << 32);
  std::uint64_t high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
  std::uint64_t remainder = 0;
  std::uint64_t quotient = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const std::uint64_t next = bit >= 64 ? (high >> (bit - 64)) & 1U : (low >> bit) & 1U;
    remainder = (remainder << 1) | next;
    if (remainder >= c) {
      remainder -= c;
      if (bit < 64)
        quotient |= std::uint64_t{1} << bit;
      else
        require(false, "oracle quotient overflow");
    }
  }
  return quotient;
}

std::uint64_t oracle(const warp::TimeMapSegment& segment, std::uint64_t frame) {
  return segment.source_begin + mul_div_floor(frame - segment.output_begin,
                                              segment.source_end - segment.source_begin,
                                              segment.output_end - segment.output_begin);
}

struct Lcg {
  std::uint64_t state;
  std::uint64_t next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 11;
  }
};

void rejects_invalid_maps() {
  using S = warp::TimeMapSegment;
  const auto rejected = [](std::vector<S> segments) {
    return !warp::WarpTimeMap::create(std::move(segments));
  };
  require(rejected({}), "empty map accepted");
  require(rejected({{1, 10, 0, 10 << 16, false}}), "map not starting at 0 accepted");
  require(rejected({{0, 0, 0, 0, false}}), "zero-length segment accepted");
  require(rejected({{0, 10, 0, 10 << 16, true}}), "leading splice accepted");
  require(rejected({{0, 10, 20 << 16, 10 << 16, false}}), "reversing source accepted");
  require(rejected({{0, 10, 0, 10 << 16, false}, {11, 20, 10 << 16, 20 << 16, false}}),
          "output gap accepted");
  require(rejected({{0, 10, 0, 10 << 16, false}, {10, 20, 30 << 16, 40 << 16, false}}),
          "source jump without splice accepted");
  require(rejected({{0, 1, 0, std::uint64_t{1025} << 16, false}}), "speed above 1024 accepted");
  require(rejected({{0, (std::uint64_t{1} << 31) + 1, 0, 1, false}}),
          "segment longer than 2^31 accepted");
  require(!rejected({{0, 10, 0, 10 << 16, false}, {10, 20, 50 << 16, 60 << 16, true}}),
          "valid splice rejected");
  require(!rejected({{0, 10, 5 << 16, 5 << 16, false}}), "frozen (zero-speed) segment rejected");
}

void evaluation_matches_exact_oracle() {
  Lcg random{12345};
  for (int trial = 0; trial < 2000; ++trial) {
    const std::uint64_t length = 1 + random.next() % (std::uint64_t{1} << 31);
    const std::uint64_t speed_limit = length << 26;
    const std::uint64_t span =
        random.next() % std::min<std::uint64_t>(speed_limit, std::uint64_t{1} << 59);
    const std::uint64_t source = random.next() % (std::uint64_t{1} << 59);
    const warp::TimeMapSegment segment{0, length, source, source + span, false};
    for (int probe = 0; probe < 8; ++probe) {
      const auto frame = probe == 0 ? 0 : probe == 1 ? length - 1 : random.next() % length;
      REQUIRE_EQ(warp::WarpTimeMap::source_at(segment, frame), oracle(segment, frame),
                 "integer evaluation differs from exact oracle");
    }
    const auto beyond = length + random.next() % 65;
    REQUIRE_EQ(warp::WarpTimeMap::extrapolate(segment, beyond), oracle(segment, beyond),
               "extrapolation differs from exact oracle");
  }
}

void endpoints_and_markers_are_exact() {
  // A marker at output 1000 maps to exactly source frame 1500.
  auto map = warp::WarpTimeMap::create(
      {{0, 1000, 0, 1500ULL << 16, false}, {1000, 3000, 1500ULL << 16, 2500ULL << 16, false}});
  REQUIRE_OK(map, "two-segment map rejected");
  const auto segments = map.value().segments();
  REQUIRE_EQ(warp::WarpTimeMap::source_at(segments[1], 1000), 1500ULL << 16,
             "marker or interior position inexact");
  REQUIRE_EQ(warp::WarpTimeMap::source_at(segments[0], 999), (1500ULL << 16) * 999 / 1000,
             "marker or interior position inexact");
  REQUIRE_EQ(warp::WarpTimeMap::speed(segments[0]), 1.5, "segment speed incorrect");
  REQUIRE_EQ(warp::WarpTimeMap::speed(segments[1]), 0.5, "segment speed incorrect");
}

void stepper_matches_direct_evaluation() {
  Lcg random{4242};
  for (int trial = 0; trial < 300; ++trial) {
    const std::uint64_t length = 1 + random.next() % 5'000;
    const std::uint64_t span = random.next() % (length << 22);
    const std::uint64_t source = random.next() % (std::uint64_t{1} << 50);
    const warp::TimeMapSegment segment{1'000, 1'000 + length, source, source + span, false};
    const std::uint64_t start = 1'000 + random.next() % length;
    warp::SegmentStepper stepper(segment, start);
    for (auto frame = start; frame < segment.output_end; ++frame) {
      REQUIRE_EQ(stepper.position(), warp::WarpTimeMap::source_at(segment, frame),
                 "stepper diverged from direct evaluation");
      stepper.advance();
    }
  }
}

void inverse_search_is_exact() {
  Lcg random{777};
  for (int trial = 0; trial < 500; ++trial) {
    const std::uint64_t length = 1 + random.next() % 20'000;
    const std::uint64_t span = random.next() % (length << 20);
    const std::uint64_t source = random.next() % (std::uint64_t{1} << 40);
    const warp::TimeMapSegment segment{50, 50 + length, source, source + span, false};
    for (int probe = 0; probe < 20; ++probe) {
      const auto target = source + random.next() % (span + 2);
      const auto found = warp::WarpTimeMap::first_output_reaching(segment, target);
      REQUIRE_GE(found, segment.output_begin, "inverse search left the segment");
      REQUIRE_LE(found, segment.output_end, "inverse search left the segment");
      if (found < segment.output_end)
        REQUIRE_GE(warp::WarpTimeMap::source_at(segment, found), target,
                   "inverse search result does not reach the source");
      if (found > segment.output_begin)
        REQUIRE_LT(warp::WarpTimeMap::source_at(segment, found - 1), target,
                   "inverse search result is not the first reaching frame");
    }
  }
}

void cursor_agrees_with_search() {
  std::vector<warp::TimeMapSegment> segments;
  std::uint64_t output = 0;
  std::uint64_t source = 0;
  Lcg random{99};
  for (int index = 0; index < 500; ++index) {
    const auto length = 1 + random.next() % 300;
    const auto span = random.next() % (length << 18);
    const bool splice = index > 0 && random.next() % 5 == 0;
    if (splice)
      source = random.next() % (std::uint64_t{1} << 40);
    segments.push_back({output, output + length, source, source + span, splice});
    output += length;
    source += span;
  }
  auto map = warp::WarpTimeMap::create(segments);
  REQUIRE_OK(map, "random map rejected");
  warp::TimeMapCursor cursor(map.value());
  for (std::uint64_t frame = 0; frame < output; frame += 1 + random.next() % 7) {
    require(cursor.locate(frame), "cursor stopped early");
    REQUIRE_EQ(cursor.index(), map.value().find_segment(frame), "cursor disagrees with search");
  }
  for (int jump = 0; jump < 1000; ++jump) {
    const auto frame = random.next() % output;
    REQUIRE_TRUE(cursor.locate(frame), "cursor random jump disagrees with search");
    REQUIRE_EQ(cursor.index(), map.value().find_segment(frame),
               "cursor random jump disagrees with search");
  }
  require(!cursor.locate(output), "cursor located past the end");
}

} // namespace

int main() {
  try {
    rejects_invalid_maps();
    evaluation_matches_exact_oracle();
    endpoints_and_markers_are_exact();
    stepper_matches_direct_evaluation();
    inverse_search_is_exact();
    cursor_agrees_with_search();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
