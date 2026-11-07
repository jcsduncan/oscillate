#include "oscillate/media/source_rate_reader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;

void put16(std::ofstream& file, std::uint16_t value) {
  for (unsigned shift = 0; shift < 16; shift += 8) {
    file.put(static_cast<char>((static_cast<unsigned int>(value) >> shift) & 0xffU));
  }
}
void put32(std::ofstream& file, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    file.put(static_cast<char>((value >> shift) & 0xffU));
  }
}
template <typename T> T take(foundation::Result<T> result) {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result).value();
}
struct Fixture {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("oscillate-source-rate-bench-" + std::to_string(::getpid()) + ".wav");
  Fixture() {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::uint32_t frames = rate * 5;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write("RIFF", 4);
    put32(file, 36 + frames * 8);
    file.write("WAVEfmt ", 8);
    put32(file, 16);
    put16(file, 3);
    put16(file, 2);
    put32(file, rate);
    put32(file, rate * 8);
    put16(file, 8);
    put16(file, 32);
    file.write("data", 4);
    put32(file, frames * 8);
    for (std::uint32_t index = 0; index < frames; ++index) {
      const auto sample =
          static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * index / rate));
      put32(file, std::bit_cast<std::uint32_t>(sample));
      put32(file, std::bit_cast<std::uint32_t>(-sample));
    }
    if (!file) {
      throw std::runtime_error("benchmark fixture write failed");
    }
  }
  ~Fixture() { std::filesystem::remove(path); }
};
} // namespace

int run_benchmark() {
  Fixture fixture;
  auto reader = take(media::SourceRateReader::open(
      media::AudioReader::from_wav(take(media::WavReader::open(fixture.path))), 44'100));
  std::array<media::StereoFrame, media::SourceRateReader::max_read_frames> block{};
  double checksum = 0.0;
  for (const std::size_t block_size : {64U, 128U, 1'024U}) {
    std::vector<double> seconds;
    for (unsigned repetition = 0; repetition < 5; ++repetition) {
      const auto start = std::chrono::steady_clock::now();
      for (std::uint64_t first = 0; first < reader.frame_count();) {
        const auto wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(block_size, reader.frame_count() - first));
        const auto count = take(reader.read_at(first, std::span(block).first(wanted)));
        if (count != wanted) {
          throw std::runtime_error("benchmark read short");
        }
        for (std::size_t index = 0; index < count; ++index) {
          checksum += block[index].left;
        }
        first += count;
      }
      seconds.push_back(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    std::ranges::sort(seconds);
    std::cout << "block_frames=" << block_size << " source_rate_median_seconds=" << seconds[2]
              << " source_seconds=5\n";
  }
  std::ifstream status("/proc/self/status");
  std::string rss_line;
  std::string hwm_line;
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with("VmRSS:")) {
      rss_line = line;
    } else if (line.starts_with("VmHWM:")) {
      hwm_line = line;
    }
  }
  std::cout << hwm_line << " " << rss_line << " checksum=" << checksum << '\n';
  return 0;
}

int main() {
  try {
    return run_benchmark();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
