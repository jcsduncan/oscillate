#include "oscillate/codec/encoded_reader.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using Clock = std::chrono::steady_clock;
constexpr std::size_t block_frames = media::SourceRateReader::max_read_frames;
constexpr unsigned repetitions = 30;

using oscillate::test::take;

using oscillate::test::require;

[[nodiscard]] project::SourceEncoding source_encoding(codec::EncodedFormat format) {
  switch (format) {
  case codec::EncodedFormat::flac:
    return project::SourceEncoding::flac;
  case codec::EncodedFormat::mp3:
    return project::SourceEncoding::mp3;
  default:
    throw std::runtime_error("Qualification requires FLAC or MP3");
  }
}

[[nodiscard]] media::SourceRateReader open_reader(const std::filesystem::path& path,
                                                  const codec::SourceInfo& info,
                                                  std::uint32_t project_rate) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  REQUIRE_GE(fd, 0, "Cannot open fixture descriptor");
  const project::SourceRecord record{take(project::SourceId::from_bits({0, 1})),
                                     source_encoding(info.format),
                                     std::string(64, '0'),
                                     info.encoded_bytes,
                                     info.sample_rate,
                                     info.channels,
                                     info.frames};
  auto opened = media::AudioReader::open_fd(fd, record);
  ::close(fd);
  return take(media::SourceRateReader::open(take(std::move(opened)), project_rate));
}

[[nodiscard]] std::vector<std::uint64_t> positions(std::uint64_t frames) {
  REQUIRE_GT(frames, block_frames * 4, "Fixture is too short");
  return {0, frames / 4, frames / 2, frames * 3 / 4, frames - block_frames - 1};
}

[[nodiscard]] std::vector<std::vector<codec::StereoFrame>>
sequential_oracle(const std::filesystem::path& path, const codec::SourceInfo& info,
                  const std::vector<std::uint64_t>& starts) {
  auto reader = take(codec::EncodedReader::open(path));
  std::vector<std::vector<codec::StereoFrame>> windows(
      starts.size(), std::vector<codec::StereoFrame>(block_frames));
  std::array<codec::StereoFrame, 4096> chunk{};
  std::uint64_t cursor = 0;
  while (cursor < info.frames) {
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), info.frames - cursor));
    const auto got = take(reader.read_at(cursor, std::span(chunk).first(wanted)));
    REQUIRE_EQ(got, wanted, "Sequential reference decode was short");
    for (std::size_t window = 0; window < starts.size(); ++window) {
      const auto overlap_first = std::max(cursor, starts[window]);
      const auto overlap_last = std::min(cursor + got, starts[window] + block_frames);
      for (auto frame = overlap_first; frame < overlap_last; ++frame) {
        windows[window][frame - starts[window]] = chunk[frame - cursor];
      }
    }
    cursor += got;
  }
  return windows;
}

struct Summary {
  std::vector<double> microseconds;
  void add(Clock::time_point start) {
    microseconds.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
  }
  void print(const char* label) {
    std::ranges::sort(microseconds);
    const auto p95 =
        static_cast<std::size_t>(std::ceil(static_cast<double>(microseconds.size()) * 0.95)) - 1;
    std::cout << label << "_us median=" << microseconds[microseconds.size() / 2]
              << " p95=" << microseconds[p95] << " max=" << microseconds.back()
              << " n=" << microseconds.size() << '\n';
  }
};

int run(const std::filesystem::path& path) {
  const auto info = take(codec::EncodedReader::open(path)).info();
  REQUIRE_EQ(info.sample_rate, 48'000, "Fixture must be 48 kHz");
  REQUIRE_GE(info.frames, 48'000ULL * 600, "Fixture must be at least ten minutes");
  const auto starts = positions(info.frames);
  const auto expected = sequential_oracle(path, info, starts);
  std::array<media::StereoFrame, block_frames> output{};
  auto reader = open_reader(path, info, info.sample_rate);
  Summary decoder_cold_prime;
  Summary random_seek;
  Summary near_end;
  double max_absolute_error = 0.0;
  double squared_error = 0.0;
  std::uint64_t compared = 0;
  for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
    auto start = Clock::now();
    auto fresh = open_reader(path, info, info.sample_rate);
    REQUIRE_EQ(take(fresh.read_at(0, output)), output.size(), "Cold prime was short");
    decoder_cold_prime.add(start);

    for (std::size_t index = 1; index < starts.size(); ++index) {
      const std::size_t shuffled = 1 + ((index - 1 + repetition) % (starts.size() - 1));
      start = Clock::now();
      REQUIRE_EQ(take(reader.read_at(starts[shuffled], output)), output.size(),
                 "Random seek was short");
      if (shuffled == starts.size() - 1) {
        near_end.add(start);
      } else {
        random_seek.add(start);
      }
      for (std::size_t frame = 0; frame < output.size(); ++frame) {
        for (const auto pair : {std::pair{output[frame].left, expected[shuffled][frame].left},
                                std::pair{output[frame].right, expected[shuffled][frame].right}}) {
          const double error = std::abs(static_cast<double>(pair.first) - pair.second);
          require(std::isfinite(error), "Nonfinite decoded frame");
          max_absolute_error = std::max(max_absolute_error, error);
          squared_error += error * error;
          ++compared;
        }
      }
    }
  }
  const double rms_error = std::sqrt(squared_error / static_cast<double>(compared));
  std::cout << "format=" << (info.format == codec::EncodedFormat::flac ? "flac" : "mp3")
            << " source_frames=" << info.frames << " encoded_bytes=" << info.encoded_bytes
            << " alignment_max=" << max_absolute_error << " alignment_rms=" << rms_error << '\n';
  REQUIRE_LT(max_absolute_error, (info.format == codec::EncodedFormat::flac ? 1e-6 : 0.02),
             "Random seek differs from sequential reference");
  decoder_cold_prime.print("decoder_cold_open_and_prime");
  random_seek.print("random_seek_1024");
  near_end.print("near_end_seek_1024");

  std::stop_source cancelled;
  cancelled.request_stop();
  require(!reader.read_at(starts[2], output, cancelled.get_token()),
          "Cancelled read was not rejected");
  REQUIRE_EQ(take(reader.read_at(starts[2], output)), output.size(),
             "Reader failed after cancellation");
  std::array<media::StereoFrame, block_frames + 1> oversized{};
  require(!reader.read_at(starts[2], oversized), "Oversized worker request accepted");
  std::cout << "cancellation=pass max_read_frames=" << block_frames << '\n';

  auto converted = open_reader(path, info, 44'100);
  const auto project_starts = positions(converted.frame_count());
  Summary converted_seek;
  double partition_error = 0.0;
  for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
    for (std::size_t index = 0; index < project_starts.size(); ++index) {
      const auto position = project_starts[(index + repetition) % project_starts.size()];
      const auto start = Clock::now();
      REQUIRE_EQ(take(converted.read_at(position, output)), output.size(),
                 "Converted seek was short");
      converted_seek.add(start);
      if (repetition == 0) {
        std::array<media::StereoFrame, block_frames> partitioned{};
        for (std::size_t offset = 0; offset < block_frames; offset += 64) {
          REQUIRE_EQ(take(converted.read_at(position + offset,
                                            std::span(partitioned).subspan(offset, 64))),
                     64, "Converted partition read was short");
        }
        for (std::size_t frame = 0; frame < block_frames; ++frame) {
          partition_error =
              std::max(partition_error,
                       std::abs(static_cast<double>(output[frame].left) - partitioned[frame].left));
        }
      }
    }
  }
  REQUIRE_LT(partition_error, 3e-5, "Converted seek changed with request partition");
  converted_seek.print("converted_random_seek_1024");
  std::cout << "converted_partition_max=" << partition_error << '\n';
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      throw std::runtime_error("usage: oscillate_compressed_seek_rehearsal FILE.flac|FILE.mp3");
    }
    return run(argv[1]);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
