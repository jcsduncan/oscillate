#include "oscillate/codec/encoded_reader.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using oscillate::codec::EncodedFormat;
using oscillate::codec::EncodedReader;
using oscillate::codec::StereoFrame;

using oscillate::test::require;

std::filesystem::path fixture(const char* name) {
  return std::filesystem::path(OSCILLATE_CODEC_FIXTURE_DIR) / name;
}

void exact_lossless(EncodedFormat format, const char* name) {
  auto opened = EncodedReader::open(fixture(name));
  REQUIRE_OK(opened, "lossless fixture failed to open");
  auto reader = std::move(opened).value();
  REQUIRE_EQ(reader.info().format, format, "lossless fixture metadata changed");
  REQUIRE_EQ(reader.info().channels, 1, "lossless fixture metadata changed");
  REQUIRE_EQ(reader.info().sample_rate, 48'000, "lossless fixture metadata changed");
  REQUIRE_EQ(reader.info().frames, 960, "lossless fixture metadata changed");
  REQUIRE_GT(reader.info().encoded_bytes, 0, "lossless fixture metadata changed");
  std::array<StereoFrame, 16> first{};
  auto count = reader.read_at(0, first);
  REQUIRE_TRUE(count, "lossless fixture read failed");
  REQUIRE_EQ(count.value(), first.size(), "lossless fixture read failed");
  REQUIRE_LT(std::abs(first[12].left - 0.125F), 0.0002F,
             "independent sine oracle or mono duplication failed");
  REQUIRE_EQ(first[12].left, first[12].right, "independent sine oracle or mono duplication failed");
  std::array<StereoFrame, 3> at_end{};
  count = reader.read_at(959, at_end);
  REQUIRE_TRUE(count, "end-of-source read was not bounded");
  REQUIRE_EQ(count.value(), 1, "end-of-source read was not bounded");
  REQUIRE_EQ(reader.read_at(960, at_end).value(), 0, "past-end read was not empty");
  std::array<StereoFrame, 0> empty{};
  REQUIRE_EQ(reader.read_at(0, empty).value(), 0, "empty output read was not empty");
}

void lossy_mp3() {
  auto opened = EncodedReader::open(fixture("sine.mp3"));
  REQUIRE_OK(opened, "MP3 fixture failed to open");
  auto reader = std::move(opened).value();
  REQUIRE_EQ(reader.info().format, EncodedFormat::mp3, "MP3 metadata invalid");
  REQUIRE_EQ(reader.info().channels, 1, "MP3 metadata invalid");
  REQUIRE_EQ(reader.info().sample_rate, 48'000, "MP3 metadata invalid");
  REQUIRE_GE(reader.info().frames, 960, "MP3 metadata invalid");
  std::array<StereoFrame, 960> signal{};
  auto decoded = reader.read_at(0, signal);
  REQUIRE_TRUE(decoded, "MP3 frames unavailable");
  REQUIRE_GE(decoded.value(), 960, "MP3 frames unavailable");
  float peak = 0.0F;
  for (const auto frame : signal) {
    REQUIRE_TRUE(std::isfinite(frame.left),
                 "MP3 mono decoding produced nonfinite/asymmetric audio");
    REQUIRE_EQ(frame.left, frame.right, "MP3 mono decoding produced nonfinite/asymmetric audio");
    peak = std::max(peak, std::abs(frame.left));
  }
  REQUIRE_GT(peak, 0.08F, "MP3 sine peak outside lossy tolerance");
  REQUIRE_LT(peak, 0.20F, "MP3 sine peak outside lossy tolerance");
}

void partition_and_seek_consistency() {
  auto reference_opened = EncodedReader::open(fixture("sine.mp3"));
  auto partitioned_opened = EncodedReader::open(fixture("sine.mp3"));
  REQUIRE_TRUE(reference_opened, "MP3 partition fixture failed to open");
  REQUIRE_TRUE(partitioned_opened, "MP3 partition fixture failed to open");
  auto reference = std::move(reference_opened).value();
  auto partitioned = std::move(partitioned_opened).value();
  std::vector<StereoFrame> whole(960);
  REQUIRE_EQ(reference.read_at(0, whole).value(), whole.size(), "MP3 reference decode failed");
  std::array<StereoFrame, 67> block{};
  for (std::uint64_t first = 0; first < whole.size(); first += block.size()) {
    const auto count = std::min(block.size(), whole.size() - static_cast<std::size_t>(first));
    auto read = partitioned.read_at(first, std::span(block).first(count));
    REQUIRE_TRUE(read, "MP3 partition decode failed");
    REQUIRE_EQ(read.value(), count, "MP3 partition decode failed");
    for (std::size_t index = 0; index < count; ++index) {
      REQUIRE_LT(std::abs(block[index].left - whole[first + index].left), 0.0002F,
                 "MP3 output changed with block partition");
    }
  }
  REQUIRE_EQ(partitioned.read_at(300, block).value(), block.size(),
             "MP3 backward seek failed after sequential decode");
  for (std::size_t index = 0; index < block.size(); ++index) {
    REQUIRE_LT(std::abs(block[index].left - whole[300 + index].left), 0.0002F,
               "MP3 backward seek changed decoded audio");
  }
  auto moved = std::move(partitioned);
  REQUIRE_EQ(moved.read_at(367, block).value(), block.size(),
             "MP3 sequential read failed after reader move");
  for (std::size_t index = 0; index < block.size(); ++index) {
    REQUIRE_LT(std::abs(block[index].left - whole[367 + index].left), 0.0002F,
               "MP3 move changed decoded audio");
  }
}

void stereo_wav_and_unsupported_container() {
  auto opened = EncodedReader::open(fixture("stereo.wav"));
  REQUIRE_TRUE(opened, "stereo WAV was not classified");
  REQUIRE_EQ(opened.value().info().format, EncodedFormat::wav, "stereo WAV was not classified");
  REQUIRE_EQ(opened.value().info().channels, 2, "stereo WAV was not classified");
  auto reader = std::move(opened).value();
  std::array<StereoFrame, 32> frames{};
  REQUIRE_EQ(reader.read_at(200, frames).value(), frames.size(), "stereo WAV read failed");
  for (const auto frame : frames) {
    REQUIRE_LT(std::abs(frame.left - 0.25F), oscillate::test::tolerance::float_round_trip_f,
               "stereo channel order or float values changed");
    REQUIRE_LT(std::abs(frame.right - 0.5F), oscillate::test::tolerance::float_round_trip_f,
               "stereo channel order or float values changed");
  }
  require(!EncodedReader::open(fixture("unsupported.ogg")),
          "Ogg container was accepted outside the v1 import format set");
}

void pinned_descriptor_and_invalid_inputs() {
  const auto path = fixture("sine.flac");
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "could not open pinned fixture descriptor");
  REQUIRE_EQ(::lseek(fd, 7, SEEK_SET), 7, "could not set caller descriptor offset");
  auto opened = EncodedReader::open_fd(fd);
  REQUIRE_OK(opened, "pinned descriptor rejected");
  auto reader = std::move(opened).value();
  std::array<StereoFrame, 16> audio{};
  REQUIRE_EQ(reader.read_at(480, audio).value(), 16, "random seeks failed");
  REQUIRE_EQ(reader.read_at(0, audio).value(), 16, "random seeks failed");
  REQUIRE_EQ(::lseek(fd, 0, SEEK_CUR), 7, "decoder moved caller descriptor offset");
  ::close(fd);
  REQUIRE_EQ(reader.read_at(12, audio).value(), 16,
             "decoder did not retain its own pinned descriptor");
  require(!EncodedReader::open_fd(-1), "invalid descriptor accepted");
  require(!EncodedReader::open(std::filesystem::temp_directory_path()),
          "directory accepted as audio");

  const auto root =
      std::filesystem::temp_directory_path() / ("oscillate-codec-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  // A write-only regular file and a pipe are both refused.
  const auto write_only_path = root / "write-only.flac";
  std::filesystem::copy_file(path, write_only_path);
  const int write_only = ::open(write_only_path.c_str(), O_WRONLY | O_CLOEXEC);
  REQUIRE_GE(write_only, 0, "could not open a write-only descriptor");
  require(!EncodedReader::open_fd(write_only), "write-only descriptor accepted");
  ::close(write_only);
  std::array<int, 2> pipe_ends{};
  REQUIRE_EQ(::pipe2(pipe_ends.data(), O_CLOEXEC), 0, "could not create a pipe");
  require(!EncodedReader::open_fd(pipe_ends[0]), "pipe accepted as audio");
  ::close(pipe_ends[0]);
  ::close(pipe_ends[1]);
  const auto link = root / "linked.flac";
  std::filesystem::create_symlink(path, link);
  require(!EncodedReader::open(link), "symlink path followed");
  const auto garbage = root / "garbage.flac";
  {
    std::ofstream file(garbage, std::ios::binary);
    file << "not an encoded audio file";
  }
  require(!EncodedReader::open(garbage), "garbage accepted as audio");
  std::filesystem::remove_all(root);
}
} // namespace

int main() {
  try {
    exact_lossless(EncodedFormat::aiff, "sine.aiff");
    exact_lossless(EncodedFormat::flac, "sine.flac");
    lossy_mp3();
    partition_and_seek_consistency();
    stereo_wav_and_unsupported_container();
    pinned_descriptor_and_invalid_inputs();
    std::cout << "encoded reader fixtures passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
