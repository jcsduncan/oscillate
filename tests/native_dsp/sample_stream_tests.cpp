#include "oscillate/native_dsp/synth/sample_stream.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

// The shared sample-streaming core (ADR-0088) on a synthetic mono pack:
// opening and checking a pack, its resident starts, the registry, a stream
// read bit-exact across the resident/streamed join through several ring
// wraps, and the sinc table. Concert's contract covers the stereo path.
namespace {

namespace stream = oscillate::native_dsp::synth::sample_stream;
using oscillate::foundation::ErrorCode;
using oscillate::foundation::Result;

using oscillate::test::require;

constexpr std::uint32_t mono = 1;
constexpr std::uint32_t stereo = 2;
constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr std::string_view manifest_file = "mono.manifest";
constexpr std::string_view pack_file = "mono.pack";
constexpr std::string_view noun = "test library";
constexpr std::uint32_t max_test_libraries = 4;
constexpr std::uint64_t max_resident_bytes = 1ULL << 20U;
// Sample 0 streams through the ring several times; sample 1 stays whole.
constexpr std::uint32_t lead_frames = 7; // Before sample 0: offsets are not zero.
constexpr std::uint32_t streamed_resident = 1'000;
constexpr std::uint32_t streamed_frames =
    streamed_resident + 3 * stream::ring_frames + stream::read_frames / 2 + 123;
constexpr std::uint32_t whole_frames = 500;
constexpr std::uint32_t pack_frames = lead_frames + streamed_frames + whole_frames;
constexpr float sample_scale = 1.0F / 32'768.0F;
// What the callback reads per block.
constexpr std::size_t block_frames = 256;

// A deterministic, non-repeating int16 signal, so a misplaced frame shows.
std::vector<std::int16_t> pack_data() {
  std::vector<std::int16_t> data(pack_frames);
  std::uint32_t state = 0x1234'5678U;
  for (auto& value : data) {
    state = state * 1'664'525U + 1'013'904'223U;
    value = static_cast<std::int16_t>(state >> 16U);
  }
  return data;
}

std::filesystem::path scratch_directory() {
  return std::filesystem::temp_directory_path() /
         ("sample-stream-tests-" + std::to_string(::getpid()));
}

struct Scratch {
  Scratch() {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
  }
  ~Scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path directory = scratch_directory();
};

// The test library: its manifest lists "sample <offset> <frames> <resident>"
// lines, read with the shared manifest helpers.
std::filesystem::path write_library() {
  static Scratch scratch;
  const auto& directory = scratch.directory;
  const auto data = pack_data();
  std::ofstream pack(directory / pack_file, std::ios::binary);
  pack.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size() * sizeof(std::int16_t)));
  std::ofstream manifest(directory / manifest_file);
  manifest << "sample " << lead_frames << ' ' << streamed_frames << ' ' << streamed_resident << '\n'
           << "sample " << lead_frames + streamed_frames << ' ' << whole_frames << ' '
           << whole_frames << '\n';
  return directory;
}

stream::PackSpec spec_for(const std::filesystem::path& directory) {
  stream::PackSpec spec;
  spec.path = directory / pack_file;
  spec.bytes = std::uint64_t{pack_frames} * sizeof(std::int16_t);
  spec.channels = mono;
  spec.rate = rate;
  spec.max_resident_bytes = max_resident_bytes;
  spec.noun = noun;
  return spec;
}

std::vector<stream::Sample> layout() {
  std::vector<stream::Sample> samples(2);
  samples[0].offset = lead_frames;
  samples[0].frames = streamed_frames;
  samples[0].resident = streamed_resident;
  samples[0].scale = sample_scale;
  samples[1].offset = lead_frames + streamed_frames;
  samples[1].frames = whole_frames;
  samples[1].resident = whole_frames;
  samples[1].scale = sample_scale;
  return samples;
}

class MonoLibrary final : public stream::Library {
public:
  static Result<std::unique_ptr<stream::Library>> open(const std::filesystem::path& directory) {
    using R = Result<std::unique_ptr<stream::Library>>;
    std::ifstream input(directory / manifest_file);
    if (!input) {
      return R::failure({ErrorCode::not_found, "no test library"});
    }
    std::vector<stream::Sample> samples;
    std::string line;
    while (std::getline(input, line)) {
      const auto f = stream::fields(line);
      stream::Sample sample;
      if (f.size() != 4 || f[0] != "sample" || !stream::parse_number(f[1], sample.offset) ||
          !stream::parse_number(f[2], sample.frames) ||
          !stream::parse_number(f[3], sample.resident)) {
        return R::failure({ErrorCode::corrupt_data, "bad test manifest"});
      }
      sample.scale = sample_scale;
      samples.push_back(sample);
    }
    auto pack = stream::Pack::open(spec_for(directory), std::move(samples));
    if (!pack) {
      return R::failure(std::move(pack.error()));
    }
    auto library = std::make_unique<MonoLibrary>();
    library->pack_ = std::move(pack).value();
    return R::success(std::move(library));
  }
};

stream::Registry& registry() {
  static auto* instance = new stream::Registry(manifest_file, &MonoLibrary::open, std::string(noun),
                                               max_test_libraries);
  return *instance;
}

const stream::Library& loaded() {
  static const stream::Library* library = [] {
    const auto outcome = registry().load(write_library());
    require(outcome.state == stream::State::ready && outcome.library != nullptr,
            "the test library loads: " + outcome.message);
    return outcome.library;
  }();
  return *library;
}

void pack_opens_and_reads_resident() {
  const auto directory = write_library();
  auto opened = stream::Pack::open(spec_for(directory), layout());
  REQUIRE_OK(opened, "a mono pack opens");
  const auto& pack = opened.value();
  REQUIRE_EQ(pack.channels(), mono, "the pack keeps its layout");
  REQUIRE_EQ(pack.bytes_per_frame(), sizeof(std::int16_t), "the pack keeps its layout");
  REQUIRE_EQ(pack.rate(), rate, "the pack keeps its layout");
  REQUIRE_GE(pack.descriptor(), 0, "the pack keeps its layout");
  REQUIRE_EQ(pack.samples().size(), 2, "the pack keeps its layout");
  REQUIRE_EQ(pack.sample(0).resident, streamed_resident,
             "resident starts are packed one after another");
  REQUIRE_EQ(pack.sample(0).resident_at, 0, "resident starts are packed one after another");
  REQUIRE_EQ(pack.sample(1).resident, whole_frames, "resident starts are packed one after another");
  REQUIRE_EQ(pack.sample(1).resident_at, streamed_resident,
             "resident starts are packed one after another");
  REQUIRE_EQ(pack.resident_bytes(),
             std::uint64_t{streamed_resident + whole_frames} * sizeof(std::int16_t),
             "the resident store holds exactly the resident frames");
  const auto data = pack_data();
  for (std::uint32_t index = 0; index < 2; ++index) {
    const auto& sample = pack.sample(index);
    const auto* resident = pack.resident(sample);
    for (std::uint32_t f = 0; f < sample.resident; ++f) {
      REQUIRE_EQ(resident[f], data[sample.offset + f], "resident frames are the pack's");
    }
  }
  // A request for more resident frames than a sample has is clamped.
  auto samples = layout();
  samples[1].resident = whole_frames * 2;
  auto clamped = stream::Pack::open(spec_for(directory), std::move(samples));
  REQUIRE_OK(clamped, "resident frames are clamped to the sample");
  REQUIRE_EQ(clamped.value().sample(1).resident, whole_frames,
             "resident frames are clamped to the sample");
  // A moved pack keeps the descriptor.
  stream::Pack moved = std::move(clamped.value());
  REQUIRE_GE(moved.descriptor(), 0, "a pack moves");
  REQUIRE_EQ(moved.samples().size(), 2, "a pack moves");
}

void malformed_packs_are_rejected() {
  const auto directory = write_library();
  const auto rejected = [&](const stream::PackSpec& spec, std::vector<stream::Sample> samples,
                            ErrorCode code) {
    auto opened = stream::Pack::open(spec, std::move(samples));
    return !opened.has_value() && opened.error().code == code && !opened.error().message.empty();
  };
  auto truncated = spec_for(directory);
  truncated.bytes += sizeof(std::int16_t);
  require(rejected(truncated, layout(), ErrorCode::corrupt_data),
          "a pack shorter than its manifest says is refused");
  auto outside = layout();
  outside[1].frames += 1;
  require(rejected(spec_for(directory), outside, ErrorCode::corrupt_data),
          "a sample past the end of its pack is refused");
  auto beyond = layout();
  beyond[1].offset = pack_frames + 1;
  beyond[1].frames = 1;
  require(rejected(spec_for(directory), beyond, ErrorCode::corrupt_data),
          "a sample starting past its pack is refused");
  auto huge = layout();
  huge[0].frames = std::numeric_limits<std::uint32_t>::max();
  require(rejected(spec_for(directory), huge, ErrorCode::corrupt_data),
          "a sample whose length overflows the pack is refused");
  auto bounded = spec_for(directory);
  bounded.max_resident_bytes = streamed_resident * sizeof(std::int16_t);
  require(rejected(bounded, layout(), ErrorCode::corrupt_data),
          "resident frames beyond the bound are refused");
  auto channels = spec_for(directory);
  channels.channels = stream::max_channels + 1;
  require(rejected(channels, layout(), ErrorCode::corrupt_data),
          "an unsupported channel count is refused");
  auto missing = spec_for(directory);
  missing.path = directory / "no.pack";
  require(rejected(missing, layout(), ErrorCode::not_found), "a missing pack is not found");
}

void registry_loads_and_keys_by_serial() {
  const auto nowhere = scratch_directory() / "nowhere";
  const auto missing = registry().status(nowhere);
  REQUIRE_EQ(missing.state, stream::State::missing, "a missing library is reported with a reason");
  REQUIRE_EQ(missing.library, nullptr, "a missing library is reported with a reason");
  REQUIRE_TRUE(!missing.message.empty(), "a missing library is reported with a reason");
  REQUIRE_EQ(missing.directory, nowhere, "a missing library is reported with a reason");
  const auto again = registry().status(nowhere);
  REQUIRE_EQ(again.state, stream::State::missing, "an unchanged directory reports the same");
  REQUIRE_EQ(again.message, missing.message, "an unchanged directory reports the same");
  REQUIRE_EQ(registry().current(), nullptr, "nothing is current before a load");
  const auto& library = loaded();
  REQUIRE_EQ(registry().current(), &library, "a loaded library is current and found by its serial");
  REQUIRE_GE(library.serial(), 1, "a loaded library is current and found by its serial");
  REQUIRE_EQ(registry().by_serial(library.serial()), &library,
             "a loaded library is current and found by its serial");
  REQUIRE_EQ(registry().by_serial(0), nullptr, "serial 0 and serials past the budget are nothing");
  REQUIRE_EQ(registry().by_serial(max_test_libraries + 1), nullptr,
             "serial 0 and serials past the budget are nothing");
  const auto ready = registry().status(nowhere);
  REQUIRE_EQ(ready.state, stream::State::ready, "once ready the status is the library");
  REQUIRE_EQ(ready.library, &library, "once ready the status is the library");
  const auto failed = registry().load(nowhere);
  REQUIRE_EQ(failed.state, stream::State::missing, "a failed load keeps the current library");
  REQUIRE_EQ(registry().current(), &library, "a failed load keeps the current library");
}

// Streams sample 0 as a voice would: blocks of block_frames, handing back
// what it has read, servicing between blocks as an offline render does; then
// into rings sized for stereo.
void mono_stream_is_bit_exact_across_the_join() {
  const auto& library = loaded();
  const auto& pack = library.pack();
  const auto& sample = pack.sample(0);
  const auto data = pack_data();
  stream::Streams streams(registry(), mono, 1);
  REQUIRE_EQ(streams.size(), 1, "streams are mono");
  REQUIRE_EQ(streams.channels(), mono, "streams are mono");
  auto& slot = streams.slot(0);
  constexpr std::uint32_t generation = 1;
  slot.request.store(stream::request_word(generation, library.serial(), 0),
                     std::memory_order_release);

  std::array<float, block_frames> left{};
  std::array<float, block_frames> right{};
  // Before any service the streamed part has not arrived.
  require(stream::gather(sample, mono, pack.resident(sample), streams.ring(0), 0,
                         static_cast<std::int64_t>(sample.resident), 1, left.data(),
                         right.data()) &&
              left[0] == 0.0F,
          "a frame not yet streamed starves and plays as silence");

  std::uint32_t streamed = 0;
  std::int64_t position = -static_cast<std::int64_t>(block_frames / 2); // Leading silence.
  bool crossed = false;
  while (position < static_cast<std::int64_t>(sample.frames)) {
    streams.service();
    const auto word = slot.progress.load(std::memory_order_acquire);
    if (stream::generation_of(word) == generation) {
      streamed = stream::frames_of(word);
    }
    const bool starved =
        stream::gather(sample, mono, pack.resident(sample), streams.ring(0), streamed, position,
                       block_frames, left.data(), right.data());
    require(!starved, "serviced between blocks, a stream never starves");
    for (std::size_t j = 0; j < block_frames; ++j) {
      const std::int64_t i = position + static_cast<std::int64_t>(j);
      const float expected =
          i < 0 || i >= static_cast<std::int64_t>(sample.frames)
              ? 0.0F
              : static_cast<float>(data[sample.offset + static_cast<std::uint64_t>(i)]) *
                    sample_scale;
      REQUIRE_EQ(left[j], expected, "every frame is the pack's, bit-exact, on both sides");
      REQUIRE_EQ(right[j], left[j], "every frame is the pack's, bit-exact, on both sides");
      crossed = crossed || i == static_cast<std::int64_t>(sample.resident);
    }
    position += static_cast<std::int64_t>(block_frames);
    const std::int64_t done = position - static_cast<std::int64_t>(sample.resident);
    if (done > 0) {
      slot.consumed.store(stream::tagged(generation, static_cast<std::uint32_t>(
                                                         std::min<std::int64_t>(done, streamed))),
                          std::memory_order_release);
    }
  }
  REQUIRE_TRUE(crossed, "the read crossed the join and streamed the whole sample");
  REQUIRE_EQ(streamed, sample.frames - sample.resident,
             "the read crossed the join and streamed the whole sample");
  REQUIRE_EQ(streams.failures(), 0, "no read failed");

  // Rings sized for more channels stream a pack of fewer in its own
  // layout (a multi-mic kit's rings hold any kit's channels).
  stream::Streams wider(registry(), stereo, 1);
  wider.slot(0).request.store(stream::request_word(generation, library.serial(), 0),
                              std::memory_order_release);
  wider.service();
  REQUIRE_EQ(wider.failures(), 0, "a pack of fewer channels streams");
  const auto filled = stream::frames_of(wider.slot(0).progress.load(std::memory_order_acquire));
  REQUIRE_GT(filled, 0, "a pack of fewer channels streams");
  for (std::uint32_t f = 0; f < filled; ++f) {
    REQUIRE_EQ(wider.ring(0)[f], data[sample.offset + sample.resident + f],
               "frame by frame in the pack's own layout");
  }
}

void request_words_round_trip() {
  constexpr std::uint32_t generation = 0xDEAD'BEEFU;
  constexpr std::uint32_t serial = stream::Registry::max_serials;
  constexpr std::uint32_t sample = 0x12'3456U;
  constexpr auto word = stream::request_word(generation, serial, sample);
  static_assert(stream::generation_of(word) == generation);
  static_assert(stream::serial_of(word) == serial);
  static_assert(stream::sample_of(word) == sample);
  static_assert(stream::frames_of(stream::tagged(generation, sample)) == sample);
  REQUIRE_EQ(stream::sample_of(stream::request_word(1, 1, stream::no_sample)), stream::no_sample,
             "no_sample survives a request word");
}

void sinc_rows_sum_to_one() {
  const stream::SincTable table(stream::SincTable::passband);
  const auto coefficients = table.coefficients();
  REQUIRE_EQ(coefficients.size(), (stream::SincTable::phases + 1) * stream::SincTable::taps,
             "the table has every phase and the closing row");
  constexpr double tolerance = 1e-5;
  for (std::size_t row = 0; row <= stream::SincTable::phases; ++row) {
    double sum = 0.0;
    for (std::size_t t = 0; t < stream::SincTable::taps; ++t) {
      sum += coefficients[row * stream::SincTable::taps + t];
    }
    REQUIRE_LT(std::fabs(sum - 1.0), tolerance, "each row sums to one");
  }
  // A constant reads as itself at any fraction, on both channels alike.
  std::array<float, stream::SincTable::taps> ones{};
  ones.fill(1.0F);
  for (const double fraction : {0.0, 0.25, 0.5, 0.999}) {
    float l = 0.0F;
    float r = 0.0F;
    table.read(ones.data(), ones.data(), fraction, l, r);
    REQUIRE_LT(std::fabs(l - 1.0F), tolerance, "a constant passes unchanged");
    REQUIRE_EQ(l, r, "a constant passes unchanged");
    REQUIRE_EQ(l, table.read(ones.data(), fraction), "a constant passes unchanged");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "sample stream contract passed",
      {
          {"pack_opens_and_reads_resident", [] { pack_opens_and_reads_resident(); }},
          {"malformed_packs_are_rejected", [] { malformed_packs_are_rejected(); }},
          {"registry_loads_and_keys_by_serial", [] { registry_loads_and_keys_by_serial(); }},
          {"mono_stream_is_bit_exact_across_the_join",
           [] { mono_stream_is_bit_exact_across_the_join(); }},
          {"request_words_round_trip", [] { request_words_round_trip(); }},
          {"sinc_rows_sum_to_one", [] { sinc_rows_sum_to_one(); }},
      });
}
