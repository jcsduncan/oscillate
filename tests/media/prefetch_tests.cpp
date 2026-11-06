#include "oscillate/media/prefetch.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using oscillate::test::process_allocations;

namespace {

using oscillate::test::require;
template <typename T>
void require(const oscillate::foundation::Result<T>& result, const char* message) {
  require(static_cast<bool>(result), message);
}

void put16(std::vector<unsigned char>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<unsigned char>(value));
  bytes.push_back(static_cast<unsigned char>(value >> 8U));
}
void put32(std::vector<unsigned char>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes.push_back(static_cast<unsigned char>(value >> shift));
  }
}
void set32(std::vector<unsigned char>& bytes, std::size_t at, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes[at++] = static_cast<unsigned char>(value >> shift);
  }
}
void put_tag(std::vector<unsigned char>& bytes, const char* tag) {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes.push_back(static_cast<unsigned char>(tag[i]));
  }
}

std::vector<unsigned char> make_wav(std::uint16_t format, std::uint16_t channels,
                                    std::uint16_t bits, std::vector<unsigned char> payload) {
  std::vector<unsigned char> bytes;
  put_tag(bytes, "RIFF");
  put32(bytes, 0);
  put_tag(bytes, "WAVE");
  put_tag(bytes, "fmt ");
  put32(bytes, 16);
  put16(bytes, format);
  put16(bytes, channels);
  put32(bytes, 48000);
  const auto align = static_cast<std::uint16_t>(channels * bits / 8);
  put32(bytes, 48000U * align);
  put16(bytes, align);
  put16(bytes, bits);
  put_tag(bytes, "JUNK");
  put32(bytes, 1);
  bytes.push_back(0x7F);
  bytes.push_back(0);
  put_tag(bytes, "data");
  put32(bytes, static_cast<std::uint32_t>(payload.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  if (payload.size() % 2 != 0) {
    bytes.push_back(0);
  }
  set32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
  return bytes;
}

struct Fixture {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("oscillate-media-" + std::to_string(std::rand()) + ".wav");
  explicit Fixture(const std::vector<unsigned char>& bytes) { write(bytes); }
  ~Fixture() { std::filesystem::remove(path); }
  void write(const std::vector<unsigned char>& bytes) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(file), "fixture write failed");
  }
};

oscillate::media::ClipId clip(std::uint64_t low) {
  auto result = oscillate::media::ClipId::from_bits({0, low});
  require(static_cast<bool>(result), "clip ID failed");
  return result.value();
}

void test_wav_formats() {
  using oscillate::media::WavReader;
  {
    Fixture fixture(make_wav(1, 1, 8, {0, 128, 255}));
    auto reader = WavReader::open(fixture.path);
    REQUIRE_TRUE(reader, "mono WAV metadata");
    REQUIRE_EQ(reader.value().frame_count(), 3, "mono WAV metadata");
    REQUIRE_EQ(reader.value().channels(), 1, "mono WAV metadata");
    REQUIRE_EQ(reader.value().sample_rate(), 48000, "mono WAV metadata");
    std::array<oscillate::media::StereoFrame, 3> frames{};
    auto count = reader.value().read_at(1, frames);
    REQUIRE_TRUE(count, "mono seek and EOF");
    REQUIRE_EQ(count.value(), 2, "mono seek and EOF");
    REQUIRE_EQ(frames[0].left, 0, "mono seek and EOF");
    REQUIRE_EQ(frames[0].right, 0, "mono seek and EOF");
    REQUIRE_LT(std::abs(frames[1].left - 127.0F / 128.0F), 0.00001F, "mono seek and EOF");
    REQUIRE_EQ(reader.value().read_at(4, frames).value(), 0, "past EOF");
  }
  {
    Fixture fixture(make_wav(1, 2, 16, {0x00, 0x80, 0xFF, 0x7F}));
    auto reader = WavReader::open(fixture.path);
    require(reader, "stereo PCM16 open");
    std::array<oscillate::media::StereoFrame, 1> frames{};
    REQUIRE_EQ(reader.value().read_at(0, frames).value(), 1, "stereo PCM16 decode");
    REQUIRE_EQ(frames[0].left, -1.0F, "stereo PCM16 decode");
    REQUIRE_GT(frames[0].right, 0.999F, "stereo PCM16 decode");
  }
  {
    Fixture fixture(make_wav(1, 1, 24, {0x00, 0x00, 0x80}));
    auto reader = WavReader::open(fixture.path);
    require(reader, "PCM24 open");
    std::array<oscillate::media::StereoFrame, 1> frames{};
    REQUIRE_EQ(reader.value().read_at(0, frames).value(), 1, "PCM24 sign extension");
    REQUIRE_EQ(frames[0].left, -1.0F, "PCM24 sign extension");
  }
  {
    Fixture fixture(make_wav(1, 1, 32, {0, 0, 0, 0x80}));
    auto reader = WavReader::open(fixture.path);
    require(reader, "PCM32 open");
    std::array<oscillate::media::StereoFrame, 1> frames{};
    REQUIRE_EQ(reader.value().read_at(0, frames).value(), 1, "PCM32 decode");
    REQUIRE_EQ(frames[0].left, -1.0F, "PCM32 decode");
  }
  {
    Fixture fixture(make_wav(3, 1, 32, {0, 0, 0, 0x3F}));
    auto reader = WavReader::open(fixture.path);
    require(reader, "float32 open");
    std::array<oscillate::media::StereoFrame, 1> frames{};
    REQUIRE_EQ(reader.value().read_at(0, frames).value(), 1, "float32 decode");
    REQUIRE_EQ(frames[0].left, 0.5F, "float32 decode");
    fixture.write(make_wav(3, 1, 32, {0, 0, 0xC0, 0x7F}));
    require(!reader.value().read_at(0, frames), "changed source invalid value rejected");
  }
  {
    std::vector<unsigned char> payload(10000, 0);
    payload[0] = 0;
    payload[1] = 0x80;
    payload[9998] = 0xFF;
    payload[9999] = 0x7F;
    Fixture fixture(make_wav(1, 1, 16, std::move(payload)));
    auto reader = WavReader::open(fixture.path);
    REQUIRE_TRUE(reader, "large WAV opens");
    REQUIRE_EQ(reader.value().frame_count(), 5000, "large WAV opens");
    std::vector<oscillate::media::StereoFrame> frames(5000);
    const auto result = reader.value().read_at(0, frames);
    REQUIRE_TRUE(result, "chunked reader preserves samples across read boundary");
    REQUIRE_EQ(result.value(), frames.size(),
               "chunked reader preserves samples across read boundary");
    REQUIRE_EQ(frames.front().left, -1.0F, "chunked reader preserves samples across read boundary");
    REQUIRE_GT(frames.back().left, 0.999F, "chunked reader preserves samples across read boundary");
  }
}

void test_wav_rejections() {
  using oscillate::media::WavReader;
  require(!WavReader::open("/missing/oscillate.wav"), "missing file rejected");
  {
    Fixture fixture({});
    require(!WavReader::open(fixture.path), "empty file rejected");
  }
  auto valid = make_wav(1, 1, 16, {0, 0});
  {
    auto bad = valid;
    bad[0] = 'X';
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "RIFF tag rejected");
  }
  {
    auto bad = valid;
    set32(bad, 4, 0xFFFFFFFFU);
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "oversized RIFF rejected");
  }
  {
    auto bad = valid;
    set32(bad, 16, 0xFFFFFFFFU);
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "oversized chunk rejected");
  }
  {
    auto bad = valid;
    bad[20] = 6; // Unsupported compression.
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "unsupported format rejected");
  }
  {
    auto bad = valid;
    bad[34] = 20; // Unsupported bit depth.
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "unsupported depth rejected");
  }
  {
    auto bad = valid;
    bad[32] = 3; // Invalid block alignment.
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "misaligned format rejected");
  }
  {
    Fixture fixture(make_wav(1, 1, 16, {0}));
    require(!WavReader::open(fixture.path), "partial frame rejected");
  }
  {
    auto bad = valid;
    bad[12] = 'X';
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "missing format rejected");
  }
  {
    auto bad = valid;
    bad[48] = 'X'; // data chunk starts after JUNK padding.
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "missing data rejected");
  }
  {
    auto bad = valid;
    put_tag(bad, "data");
    put32(bad, 0);
    set32(bad, 4, static_cast<std::uint32_t>(bad.size() - 8));
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "duplicate data rejected");
  }
  {
    auto bad = valid;
    put_tag(bad, "fmt ");
    put32(bad, 16);
    bad.insert(bad.end(), valid.begin() + 20, valid.begin() + 36);
    set32(bad, 4, static_cast<std::uint32_t>(bad.size() - 8));
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "duplicate format rejected");
  }
  {
    auto bad = make_wav(1, 1, 8, {128});
    bad.pop_back(); // Odd data chunk now lacks required padding.
    set32(bad, 4, static_cast<std::uint32_t>(bad.size() - 8));
    Fixture fixture(bad);
    require(!WavReader::open(fixture.path), "missing odd-byte padding rejected");
  }
  {
    Fixture fixture(valid);
    auto reader = WavReader::open(fixture.path);
    require(reader, "source opens before truncation");
    fixture.write({});
    std::array<oscillate::media::StereoFrame, 1> output{};
    require(!reader.value().read_at(0, output), "truncated source read fails");
  }
}

void test_pinned_wav_descriptor() {
  using oscillate::media::StereoFrame;
  using oscillate::media::WavReader;
  const auto original = make_wav(1, 1, 8, {0, 128});
  Fixture fixture(original);
  const int fd = ::open(fixture.path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture descriptor opens");
  auto reader = WavReader::open_fd(fd);
  ::close(fd);
  REQUIRE_TRUE(reader, "descriptor metadata matches WAV");
  REQUIRE_EQ(reader.value().sample_rate(), 48000, "descriptor metadata matches WAV");
  REQUIRE_EQ(reader.value().channels(), 1, "descriptor metadata matches WAV");
  REQUIRE_EQ(reader.value().frame_count(), 2, "descriptor metadata matches WAV");

  const auto old_path = fixture.path.string() + ".old";
  std::filesystem::rename(fixture.path, old_path);
  fixture.write(make_wav(1, 1, 8, {255, 255, 255}));
  std::array<StereoFrame, 2> frames{};
  const auto count = reader.value().read_at(0, frames);
  REQUIRE_TRUE(count, "reader remains pinned after path replacement and caller close");
  REQUIRE_EQ(count.value(), 2, "reader remains pinned after path replacement and caller close");
  REQUIRE_EQ(frames[0].left, -1.0F,
             "reader remains pinned after path replacement and caller close");
  REQUIRE_EQ(frames[1].left, 0.0F, "reader remains pinned after path replacement and caller close");
  std::filesystem::remove(old_path);

  const auto current_fd = ::open(fixture.path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(current_fd, 0, "replacement descriptor opens");
  auto replacement = WavReader::open_fd(current_fd);
  ::close(current_fd);
  REQUIRE_TRUE(replacement, "new descriptor sees replacement metadata");
  REQUIRE_EQ(replacement.value().frame_count(), 3, "new descriptor sees replacement metadata");

  require(!WavReader::open_fd(-1), "negative descriptor rejected");
  require(!WavReader::open_fd(fd), "closed descriptor rejected");
  const int write_only = ::open(fixture.path.c_str(), O_WRONLY | O_CLOEXEC);
  REQUIRE_GE(write_only, 0, "write-only descriptor opens");
  require(!WavReader::open_fd(write_only), "write-only descriptor rejected");
  ::close(write_only);

  const auto link_path = fixture.path.string() + ".link";
  std::filesystem::create_symlink(fixture.path, link_path);
  require(!WavReader::open(link_path), "symlink path rejected");
  const int link_fd = ::open(link_path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  REQUIRE_GE(link_fd, 0, "symlink descriptor rejected");
  REQUIRE_TRUE(!WavReader::open_fd(link_fd), "symlink descriptor rejected");
  ::close(link_fd);
  std::filesystem::remove(link_path);

  const auto fifo_path = fixture.path.string() + ".fifo";
  REQUIRE_EQ(::mkfifo(fifo_path.c_str(), 0600), 0, "FIFO fixture created");
  require(!WavReader::open(fifo_path), "FIFO path rejected without blocking");
  const int fifo_fd = ::open(fifo_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  REQUIRE_GE(fifo_fd, 0, "FIFO descriptor rejected");
  REQUIRE_TRUE(!WavReader::open_fd(fifo_fd), "FIFO descriptor rejected");
  ::close(fifo_fd);
  std::filesystem::remove(fifo_path);

  const int bad_fd = ::open(fixture.path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(bad_fd, 0, "malformed descriptor opens");
  fixture.write({});
  require(!WavReader::open_fd(bad_fd), "malformed pinned WAV rejected");
  ::close(bad_fd);
}

void test_prefetch() {
  using namespace oscillate::media;
  Fixture fixture(make_wav(1, 1, 8, {0, 128, 255, 128}));
  auto reader = WavReader::open(fixture.path);
  require(reader, "source opens");
  PrefetchRing ring(clip(1), 3);
  bool rejected_zero_capacity = false;
  try {
    PrefetchRing invalid(clip(1), 0);
  } catch (const std::invalid_argument&) {
    rejected_zero_capacity = true;
  }
  require(rejected_zero_capacity, "zero-capacity ring accepted");
  std::array<StereoFrame, 4> scratch{};
  REQUIRE_EQ(fill_from_wav(reader.value(), 48000, ring, {5, 0}, scratch).value(), 3,
             "worker fills bounded ring");
  REQUIRE_EQ(ring.occupancy(), 3, "worker fills bounded ring");
  REQUIRE_EQ(ring.free_slots(), 0, "worker fills bounded ring");
  REQUIRE_EQ(fill_from_wav(reader.value(), 48000, ring, {5, 3}, scratch).value(), 0,
             "full ring bounded");
  std::array<StereoFrame, 2> output{};
  const auto before = process_allocations.load();
  auto first = ring.consume(5, 0, output, false);
  REQUIRE_EQ(process_allocations.load(), before,
             "priming consumption no allocation or counted underrun");
  REQUIRE_EQ(first.copied, 2, "priming consumption no allocation or counted underrun");
  REQUIRE_EQ(first.silent, 0, "priming consumption no allocation or counted underrun");
  REQUIRE_EQ(ring.underrun_count(), 0, "priming consumption no allocation or counted underrun");
  REQUIRE_EQ(output[0].left, -1.0F, "priming consumption no allocation or counted underrun");
  REQUIRE_EQ(output[1].left, 0, "priming consumption no allocation or counted underrun");
  REQUIRE_EQ(fill_from_wav(reader.value(), 48000, ring, {5, 3}, scratch).value(), 1,
             "worker refill");
  auto steady = ring.consume(5, 2, std::span(output), true);
  REQUIRE_EQ(steady.copied, 2, "steady frames ready");
  REQUIRE_EQ(steady.silent, 0, "steady frames ready");
  REQUIRE_EQ(ring.underrun_count(), 0, "steady frames ready");
  const auto before_shortfall = process_allocations.load();
  steady = ring.consume(5, 4, std::span(output), true);
  REQUIRE_EQ(process_allocations.load(), before_shortfall,
             "worker stall emits bounded silence and exact event");
  REQUIRE_EQ(steady.copied, 0, "worker stall emits bounded silence and exact event");
  REQUIRE_EQ(steady.silent, 2, "worker stall emits bounded silence and exact event");
  REQUIRE_TRUE(steady.underruns[0], "worker stall emits bounded silence and exact event");
  REQUIRE_EQ(ring.underrun_count(), 1, "worker stall emits bounded silence and exact event");
  REQUIRE_EQ(steady.underruns[0].value_or(UnderrunEvent{clip(1)}).first_source_frame, 4,
             "worker stall emits bounded silence and exact event");
  REQUIRE_EQ(steady.underruns[0].value_or(UnderrunEvent{clip(1)}).missing_frames, 2,
             "worker stall emits bounded silence and exact event");
  REQUIRE_EQ(output[0].left, 0, "worker stall emits bounded silence and exact event");
  REQUIRE_EQ(output[1].right, 0, "worker stall emits bounded silence and exact event");
  const auto reported = ring.pop_underrun();
  REQUIRE_TRUE(reported, "control thread did not receive the clip and interval of the underrun");
  REQUIRE_EQ(reported->clip, clip(1),
             "control thread did not receive the clip and interval of the underrun");
  REQUIRE_EQ(reported->generation, 5,
             "control thread did not receive the clip and interval of the underrun");
  REQUIRE_EQ(reported->first_source_frame, 4,
             "control thread did not receive the clip and interval of the underrun");
  REQUIRE_EQ(reported->missing_frames, 2,
             "control thread did not receive the clip and interval of the underrun");
  REQUIRE_TRUE(!ring.pop_underrun(),
               "control thread did not receive the clip and interval of the underrun");
  require(!fill_from_wav(reader.value(), 44100, ring, {1, 0}, scratch), "rate mismatch rejected");
  require(!fill_from_wav(reader.value(), 48000, ring, {1, UINT64_MAX}, scratch),
          "source position overflow rejected");
}

void test_generations_and_loop() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(2), 5);
  require(!ring.try_push({clip(3), 1, 0, {1, 1}}), "cross-clip frame rejected");
  REQUIRE_TRUE(ring.try_push({clip(2), 1, 7, {0.25F, 0.25F}}), "loop generations queue together");
  REQUIRE_TRUE(ring.try_push({clip(2), 2, 0, {0.5F, 0.5F}}), "loop generations queue together");
  std::array<StereoFrame, 1> output{};
  auto result = ring.consume(1, 7, output, true);
  REQUIRE_EQ(result.copied, 1, "current loop frame");
  REQUIRE_EQ(output[0].left, 0.25F, "current loop frame");
  REQUIRE_EQ(ring.occupancy(), 1, "current loop frame");
  result = ring.consume(1, 8, output, false);
  REQUIRE_EQ(result.silent, 1, "future loop tag retained during priming");
  REQUIRE_EQ(ring.occupancy(), 1, "future loop tag retained during priming");
  REQUIRE_EQ(ring.underrun_count(), 0, "future loop tag retained during priming");
  result = ring.consume(2, 0, output, true);
  REQUIRE_EQ(result.copied, 1, "next loop frame used");
  REQUIRE_EQ(output[0].left, 0.5F, "next loop frame used");
  REQUIRE_TRUE(ring.try_push({clip(2), 2, 1, {0.1F, 0.1F}}), "old/new seek frames queued");
  REQUIRE_TRUE(ring.try_push({clip(2), 3, 50, {0.3F, 0.3F}}), "old/new seek frames queued");
  result = ring.consume(3, 50, output, false);
  REQUIRE_EQ(result.stale_dropped, 1, "unanticipated seek drops stale frame without underrun");
  REQUIRE_EQ(result.copied, 1, "unanticipated seek drops stale frame without underrun");
  REQUIRE_EQ(output[0].left, 0.3F, "unanticipated seek drops stale frame without underrun");
  REQUIRE_EQ(ring.underrun_count(), 0, "unanticipated seek drops stale frame without underrun");
  require(ring.try_push({clip(2), 4, 100, {0.4F, 0.4F}}), "future source queued");
  result = ring.consume(4, 99, output, false);
  REQUIRE_EQ(result.silent, 1, "future source frame retained");
  REQUIRE_EQ(ring.occupancy(), 1, "future source frame retained");
  result = ring.consume(4, 100, output, false);
  REQUIRE_EQ(result.copied, 1, "future source frame consumed");
  REQUIRE_EQ(output[0].left, 0.4F, "future source frame consumed");
  require(!ring.try_push({clip(2), 4, 102, {1, 1}}), "producer gap rejected");
  require(ring.try_push({clip(2), 5, 0, {1, 1}}), "pending frame queued");
  ring.discard_pending();
  REQUIRE_EQ(ring.occupancy(), 0, "seek discard clears pending frames");

  PrefetchRing future(clip(2), 4);
  REQUIRE_TRUE(future.try_push({clip(2), 9, 2, {0.2F, 0.2F}}), "future in-block frames queued");
  REQUIRE_TRUE(future.try_push({clip(2), 9, 3, {0.3F, 0.3F}}), "future in-block frames queued");
  std::array<StereoFrame, 4> block{};
  const auto with_gap = future.consume(9, 0, block, true);
  REQUIRE_EQ(with_gap.silent, 2, "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(with_gap.copied, 2, "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(block[0].left, 0, "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(block[1].left, 0, "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(block[2].left, 0.2F, "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(block[3].left, 0.3F, "future in-block frames retained and copied after exact gap");
  REQUIRE_TRUE(with_gap.underruns[0], "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(with_gap.underruns[0].value_or(UnderrunEvent{clip(2)}).first_source_frame, 0,
             "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(with_gap.underruns[0].value_or(UnderrunEvent{clip(2)}).missing_frames, 2,
             "future in-block frames retained and copied after exact gap");
  REQUIRE_TRUE(!with_gap.underruns[1],
               "future in-block frames retained and copied after exact gap");
  REQUIRE_EQ(future.underrun_count(), 1,
             "future in-block frames retained and copied after exact gap");

  PrefetchRing both_gaps(clip(2), 2);
  require(both_gaps.try_push({clip(2), 10, 2, {0.2F, 0.2F}}), "middle frame queued");
  const auto separated = both_gaps.consume(10, 0, block, true);
  REQUIRE_EQ(separated.silent, 3, "separate leading and trailing underrun intervals");
  REQUIRE_EQ(separated.copied, 1, "separate leading and trailing underrun intervals");
  REQUIRE_TRUE(separated.underruns[0], "separate leading and trailing underrun intervals");
  REQUIRE_EQ(separated.underruns[0].value_or(UnderrunEvent{clip(2)}).first_source_frame, 0,
             "separate leading and trailing underrun intervals");
  REQUIRE_EQ(separated.underruns[0].value_or(UnderrunEvent{clip(2)}).missing_frames, 2,
             "separate leading and trailing underrun intervals");
  REQUIRE_TRUE(separated.underruns[1], "separate leading and trailing underrun intervals");
  REQUIRE_EQ(separated.underruns[1].value_or(UnderrunEvent{clip(2)}).first_source_frame, 3,
             "separate leading and trailing underrun intervals");
  REQUIRE_EQ(separated.underruns[1].value_or(UnderrunEvent{clip(2)}).missing_frames, 1,
             "separate leading and trailing underrun intervals");
  REQUIRE_EQ(both_gaps.underrun_count(), 2, "separate leading and trailing underrun intervals");
  const auto overflowing = both_gaps.consume(10, UINT64_MAX - 1, block, true);
  REQUIRE_EQ(overflowing.silent, block.size(),
             "invalid overflowing callback request is silent and cannot overflow event tag");
  REQUIRE_TRUE(!overflowing.underruns[0],
               "invalid overflowing callback request is silent and cannot overflow event tag");
}

void test_bounded_underrun_diagnostics() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(3), 1);
  std::array<StereoFrame, 1> output{};
  const auto before = process_allocations.load();
  for (std::uint64_t frame = 0; frame < 65; ++frame)
    REQUIRE_EQ(ring.consume(7, frame, output, true).silent, 1,
               "steady missing frame was not silenced");
  REQUIRE_EQ(process_allocations.load(), before,
             "bounded callback diagnostic queue allocated or lost its overflow count");
  REQUIRE_EQ(ring.underrun_count(), 65,
             "bounded callback diagnostic queue allocated or lost its overflow count");
  REQUIRE_EQ(ring.dropped_underrun_events(), 1,
             "bounded callback diagnostic queue allocated or lost its overflow count");
  for (std::uint64_t frame = 0; frame < 64; ++frame) {
    const auto event = ring.pop_underrun();
    REQUIRE_TRUE(event, "diagnostic queue lost FIFO clip intervals");
    REQUIRE_EQ(event->clip, clip(3), "diagnostic queue lost FIFO clip intervals");
    REQUIRE_EQ(event->generation, 7, "diagnostic queue lost FIFO clip intervals");
    REQUIRE_EQ(event->first_source_frame, frame, "diagnostic queue lost FIFO clip intervals");
    REQUIRE_EQ(event->missing_frames, 1, "diagnostic queue lost FIFO clip intervals");
  }
  require(!ring.pop_underrun(), "diagnostic queue returned an event after draining");
}

void test_concurrent_underrun_diagnostics() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(4), 1);
  std::atomic<bool> done{false};
  std::thread callback([&] {
    std::array<StereoFrame, 1> output{};
    for (std::uint64_t frame = 0; frame < 1'000; ++frame)
      (void)ring.consume(9, frame, output, true);
    done.store(true, std::memory_order_release);
  });
  std::uint64_t seen = 0;
  std::uint64_t previous = 0;
  bool ordered = true;
  for (;;) {
    // Observe producer completion before asking whether the queue is empty.
    // An empty pop followed by completion could miss an event published in
    // between those observations, even though the SPSC queue is correct.
    const bool completed = done.load(std::memory_order_acquire);
    const auto event = ring.pop_underrun();
    if (event) {
      ordered &= event->clip == clip(4) && event->generation == 9 &&
                 event->first_source_frame >= previous && event->missing_frames == 1;
      previous = event->first_source_frame + 1;
      ++seen;
    } else if (completed) {
      break;
    }
  }
  callback.join();
  REQUIRE_TRUE(ordered, "concurrent diagnostic queue lost an event without accounting for a drop");
  REQUIRE_EQ(seen + ring.dropped_underrun_events(), 1'000,
             "concurrent diagnostic queue lost an event without accounting for a drop");
  REQUIRE_EQ(ring.underrun_count(), 1'000,
             "concurrent diagnostic queue lost an event without accounting for a drop");
}

void test_underrun_completion_after_empty_observation() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(4), 1);
  std::atomic<bool> publish{false};
  std::atomic<bool> done{false};
  std::thread callback([&] {
    while (!publish.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::array<StereoFrame, 1> output{};
    (void)ring.consume(10, 42, output, true);
    done.store(true, std::memory_order_release);
  });
  // This empty observation is deliberately before the producer's last event.
  const bool initially_empty = !ring.pop_underrun();
  publish.store(true, std::memory_order_release);
  while (!done.load(std::memory_order_acquire))
    std::this_thread::yield();
  const auto final_event = ring.pop_underrun();
  callback.join();
  REQUIRE_TRUE(initially_empty, "completed producer's final diagnostic event was not drained");
  REQUIRE_TRUE(final_event, "completed producer's final diagnostic event was not drained");
  REQUIRE_EQ(final_event->first_source_frame, 42,
             "completed producer's final diagnostic event was not drained");
  REQUIRE_EQ(final_event->missing_frames, 1,
             "completed producer's final diagnostic event was not drained");
  REQUIRE_EQ(ring.underrun_count(), 1,
             "completed producer's final diagnostic event was not drained");
  REQUIRE_EQ(ring.dropped_underrun_events(), 0,
             "completed producer's final diagnostic event was not drained");
  REQUIRE_TRUE(!ring.pop_underrun(), "completed producer's final diagnostic event was not drained");
}

void test_spsc_order() {
  using namespace oscillate::media;
  constexpr std::uint64_t frames = 100000;
  PrefetchRing ring(clip(4), 17);
  std::atomic<bool> done{false};
  std::atomic<bool> bounded{true};
  std::jthread observer([&] {
    while (!done.load(std::memory_order_acquire)) {
      if (ring.occupancy() > ring.capacity() || ring.free_slots() > ring.capacity()) {
        bounded.store(false, std::memory_order_relaxed);
      }
    }
  });
  std::jthread producer([&] {
    for (std::uint64_t i = 0; i < frames; ++i) {
      const TaggedFrame frame{clip(4), 1, i, {static_cast<float>(i), static_cast<float>(i)}};
      while (!ring.try_push(frame)) {
        std::this_thread::yield();
      }
      if (ring.occupancy() > ring.capacity() || ring.free_slots() > ring.capacity()) {
        bounded.store(false, std::memory_order_relaxed);
      }
    }
  });
  bool ordered = true;
  for (std::uint64_t i = 0; i < frames; ++i) {
    while (ring.occupancy() == 0) {
      std::this_thread::yield();
    }
    std::array<StereoFrame, 1> output{};
    const auto read = ring.consume(1, i, output, false);
    ordered = ordered && read.copied == 1 && output[0].left == static_cast<float>(i);
  }
  producer.join();
  done.store(true, std::memory_order_release);
  observer.join();
  REQUIRE_TRUE(ordered, "SPSC order and bounded occupancy/free-space snapshots under concurrency");
  REQUIRE_TRUE(bounded.load(std::memory_order_relaxed),
               "SPSC order and bounded occupancy/free-space snapshots under concurrency");
  REQUIRE_EQ(ring.occupancy(), 0,
             "SPSC order and bounded occupancy/free-space snapshots under concurrency");
}

void test_stale_scan_budget() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(5), 512);
  for (std::uint64_t i = 0; i < 100; ++i) {
    require(ring.try_push({clip(5), 1, i, {1, 1}}), "stale fixture fill");
  }
  std::array<StereoFrame, 1> output{};
  const auto first = ring.consume(2, 0, output, false);
  REQUIRE_EQ(first.stale_dropped, PrefetchRing::max_stale_scan_per_block,
             "large stale ring respects fixed callback scan budget");
  REQUIRE_EQ(first.silent, 1, "large stale ring respects fixed callback scan budget");
  REQUIRE_EQ(ring.occupancy(), 100 - PrefetchRing::max_stale_scan_per_block,
             "large stale ring respects fixed callback scan budget");
  REQUIRE_EQ(ring.underrun_count(), 0, "large stale ring respects fixed callback scan budget");
  ring.discard_pending();
  REQUIRE_EQ(ring.occupancy(), 0, "control-side stale reset allows subsequent priming");
}

// A producer that fell behind declares one forward jump; the consumer sees
// the skipped interval as a reported gap and resumes at the new tags.
void test_forward_skip_and_consumer_position() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(6), 64);
  require(!ring.skip_forward(1, 10), "skip before any push was accepted");
  for (std::uint64_t i = 0; i < 4; ++i)
    require(ring.try_push({clip(6), 1, i, {1, 1}}), "skip fixture fill");
  require(!ring.try_push({clip(6), 1, 9, {2, 2}}), "implicit forward jump was accepted");
  REQUIRE_TRUE(!ring.skip_forward(2, 9),
               "skip to another generation, the successor or backwards was accepted");
  REQUIRE_TRUE(!ring.skip_forward(1, 4),
               "skip to another generation, the successor or backwards was accepted");
  REQUIRE_TRUE(!ring.skip_forward(1, 2),
               "skip to another generation, the successor or backwards was accepted");
  REQUIRE_TRUE(ring.skip_forward(1, 9),
               "declared forward skip did not resume contiguous pushes at the new tag");
  REQUIRE_TRUE(ring.try_push({clip(6), 1, 9, {2, 2}}),
               "declared forward skip did not resume contiguous pushes at the new tag");
  REQUIRE_TRUE(!ring.try_push({clip(6), 1, 11, {3, 3}}),
               "declared forward skip did not resume contiguous pushes at the new tag");
  REQUIRE_TRUE(ring.try_push({clip(6), 1, 10, {3, 3}}),
               "declared forward skip did not resume contiguous pushes at the new tag");
  REQUIRE_EQ(ring.consumer_position().generation, 0, "consumer position set before any consume");
  std::array<StereoFrame, 11> output{};
  const auto result = ring.consume(1, 0, output, true);
  REQUIRE_EQ(result.copied, 6, "consumer did not report the skipped interval as one underrun gap");
  REQUIRE_EQ(result.silent, 5, "consumer did not report the skipped interval as one underrun gap");
  REQUIRE_EQ(output[3].left, 1.0F,
             "consumer did not report the skipped interval as one underrun gap");
  REQUIRE_EQ(output[4].left, 0.0F,
             "consumer did not report the skipped interval as one underrun gap");
  REQUIRE_EQ(output[9].left, 2.0F,
             "consumer did not report the skipped interval as one underrun gap");
  REQUIRE_EQ(output[10].left, 3.0F,
             "consumer did not report the skipped interval as one underrun gap");
  REQUIRE_EQ(ring.underrun_count(), 1,
             "consumer did not report the skipped interval as one underrun gap");
  const auto position = ring.consumer_position();
  REQUIRE_EQ(position.generation, 1, "consumer position does not record the last requested end");
  REQUIRE_EQ(position.next_frame, 11, "consumer position does not record the last requested end");
}

void test_exact_handoff_window() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(6), 128);
  REQUIRE_EQ(ring.prime_handoff_window(1, 50, 64), HandoffWindowStatus::waiting,
             "handoff accepted empty or invalid interval");
  REQUIRE_EQ(ring.prime_handoff_window(0, 50, 64), HandoffWindowStatus::invalid,
             "handoff accepted empty or invalid interval");
  REQUIRE_EQ(ring.prime_handoff_window(1, UINT64_MAX, 2), HandoffWindowStatus::invalid,
             "handoff accepted empty or invalid interval");
  REQUIRE_EQ(ring.prime_handoff_window(1, 0, 129), HandoffWindowStatus::invalid,
             "handoff accepted empty or invalid interval");
  for (std::uint64_t frame = 0; frame <= 100; ++frame)
    require(ring.try_push({clip(6), 1, frame, {static_cast<float>(frame), 0.0F}}),
            "handoff fixture push failed");
  const auto allocations_before = process_allocations.load(std::memory_order_relaxed);
  REQUIRE_EQ(ring.prime_handoff_window(1, 50, 64), HandoffWindowStatus::waiting,
             "handoff did not free stale prefix while waiting for an exact future block");
  REQUIRE_EQ(ring.occupancy(), 51,
             "handoff did not free stale prefix while waiting for an exact future block");
  REQUIRE_EQ(ring.free_slots(), 77,
             "handoff did not free stale prefix while waiting for an exact future block");
  for (std::uint64_t frame = 101; frame < 114; ++frame)
    require(ring.try_push({clip(6), 1, frame, {static_cast<float>(frame), 0.0F}}),
            "handoff continuation push failed");
  REQUIRE_EQ(ring.prime_handoff_window(1, 50, 64), HandoffWindowStatus::ready,
             "ready handoff did not prove its exact generation/window without allocation");
  REQUIRE_EQ(process_allocations.load(std::memory_order_relaxed), allocations_before,
             "ready handoff did not prove its exact generation/window without allocation");
  std::array<StereoFrame, 64> output{};
  const auto consumed = ring.consume(1, 50, output, true);
  REQUIRE_EQ(consumed.copied, output.size(),
             "first callback after ready handoff scanned stale tags or lost frames");
  REQUIRE_EQ(consumed.silent, 0,
             "first callback after ready handoff scanned stale tags or lost frames");
  REQUIRE_EQ(consumed.stale_dropped, 0,
             "first callback after ready handoff scanned stale tags or lost frames");
  REQUIRE_EQ(output.front().left, 50.0F,
             "first callback after ready handoff scanned stale tags or lost frames");
  REQUIRE_EQ(output.back().left, 113.0F,
             "first callback after ready handoff scanned stale tags or lost frames");
  REQUIRE_EQ(ring.underrun_count(), 0,
             "first callback after ready handoff scanned stale tags or lost frames");
  PrefetchRing late(clip(7), 8);
  REQUIRE_TRUE(late.try_push({clip(7), 4, 80, {1.0F, 1.0F}}),
               "missed source position/generation was treated as ready or destroyed valid frames");
  REQUIRE_EQ(late.prime_handoff_window(4, 79, 1), HandoffWindowStatus::missed,
             "missed source position/generation was treated as ready or destroyed valid frames");
  REQUIRE_EQ(late.prime_handoff_window(3, 80, 1), HandoffWindowStatus::missed,
             "missed source position/generation was treated as ready or destroyed valid frames");
  REQUIRE_EQ(late.prime_handoff_window(4, 80, 1), HandoffWindowStatus::ready,
             "missed source position/generation was treated as ready or destroyed valid frames");
}

void test_concurrent_handoff_window_and_wraparound() {
  using namespace oscillate::media;
  PrefetchRing ring(clip(8), 32);
  std::atomic<bool> cancel{false};
  std::atomic<bool> published_all{false};
  std::jthread producer([&] {
    for (std::uint64_t frame = 0; frame < 200 && !cancel.load(std::memory_order_acquire); ++frame) {
      const TaggedFrame tagged{clip(8), 2, frame, {static_cast<float>(frame), 0.0F}};
      while (!ring.try_push(tagged) && !cancel.load(std::memory_order_acquire))
        std::this_thread::yield();
    }
    published_all.store(true, std::memory_order_release);
  });
  HandoffWindowStatus status = HandoffWindowStatus::waiting;
  for (std::size_t attempt = 0; attempt < 1'000'000 && status == HandoffWindowStatus::waiting;
       ++attempt) {
    status = ring.prime_handoff_window(2, 192, 8);
    std::this_thread::yield();
  }
  cancel.store(true, std::memory_order_release);
  producer.join();
  REQUIRE_EQ(
      status, HandoffWindowStatus::ready,
      "concurrent producer could not refill a wrapped handoff ring to the exact future window");
  REQUIRE_TRUE(
      published_all.load(std::memory_order_acquire),
      "concurrent producer could not refill a wrapped handoff ring to the exact future window");
  std::array<StereoFrame, 8> output{};
  const auto consumed = ring.consume(2, 192, output, true);
  REQUIRE_EQ(consumed.copied, output.size(),
             "wrapped handoff lost or misordered the final source interval");
  REQUIRE_EQ(consumed.silent, 0, "wrapped handoff lost or misordered the final source interval");
  REQUIRE_EQ(consumed.stale_dropped, 0,
             "wrapped handoff lost or misordered the final source interval");
  REQUIRE_EQ(output.front().left, 192.0F,
             "wrapped handoff lost or misordered the final source interval");
  REQUIRE_EQ(output.back().left, 199.0F,
             "wrapped handoff lost or misordered the final source interval");
}

} // namespace

int main() {
  // Count every thread for the whole run; tests compare before and after.
  oscillate::test::counting_process_allocations = true;
  try {
    test_wav_formats();
    test_wav_rejections();
    test_pinned_wav_descriptor();
    test_prefetch();
    test_generations_and_loop();
    test_bounded_underrun_diagnostics();
    test_concurrent_underrun_diagnostics();
    test_underrun_completion_after_empty_observation();
    test_spsc_order();
    test_stale_scan_budget();
    test_forward_skip_and_consumer_position();
    test_exact_handoff_window();
    test_concurrent_handoff_window_and_wraparound();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
