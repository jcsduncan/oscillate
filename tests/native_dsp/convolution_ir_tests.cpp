#include "oscillate/native_dsp/convolution_ir.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <openssl/evp.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using oscillate::engine::StereoFrame;
using oscillate::native_dsp::ConvolutionIrBudget;
using oscillate::native_dsp::ConvolutionIrIdentity;
using oscillate::native_dsp::prepare_convolution_ir;

using oscillate::test::require;

void u16(std::vector<unsigned char>& out, std::uint16_t value) {
  out.push_back(static_cast<unsigned char>(value));
  out.push_back(static_cast<unsigned char>(value >> 8U));
}
void u32(std::vector<unsigned char>& out, std::uint32_t value) {
  u16(out, static_cast<std::uint16_t>(value));
  u16(out, static_cast<std::uint16_t>(value >> 16U));
}

std::vector<unsigned char> wav(std::span<const std::int16_t> samples) {
  std::vector<unsigned char> out;
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  u32(out, static_cast<std::uint32_t>(36 + samples.size() * 2));
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(out, 16);
  u16(out, 1);
  u16(out, 2);
  u32(out, 48'000);
  u32(out, 48'000 * 4);
  u16(out, 4);
  u16(out, 16);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  u32(out, static_cast<std::uint32_t>(samples.size() * 2));
  for (auto sample : samples) {
    u16(out, static_cast<std::uint16_t>(sample));
  }
  return out;
}

std::string sha256(std::span<const unsigned char> bytes) {
  std::array<unsigned char, 32> digest{};
  unsigned size = 0;
  REQUIRE_EQ(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr), 1,
             "test digest failed");
  REQUIRE_EQ(size, digest.size(), "test digest failed");
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (auto value : digest) {
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

struct Fixture {
  explicit Fixture(std::span<const unsigned char> bytes) {
    static std::uint32_t serial = 0;
    path = std::filesystem::temp_directory_path() /
           ("oscillate-convolution-ir-" + std::to_string(::getpid()) + "-" +
            std::to_string(serial++) + ".wav");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "cannot create IR fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "cannot open IR fixture");
  }
  ~Fixture() {
    ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  std::filesystem::path path;
  int fd = -1;
};

void admission_contract() {
  constexpr std::array<std::int16_t, 6> samples{16384, -16384, 8192, 4096, 0, 0};
  auto bytes = wav(samples);
  Fixture fixture(bytes);
  const ConvolutionIrIdentity identity{sha256(bytes), bytes.size(), 48'000, 2, 3};
  const ConvolutionIrBudget budget{6ULL * 1024 * 1024, 300,    100, 0, identity.sha256,
                                   identity.frames,    48'000, 64};
  auto prepared = prepare_convolution_ir(fixture.fd, identity, budget, 48'000, 1.0F);
  REQUIRE_OK(prepared, "verified IR rejected");
  REQUIRE_EQ(prepared.value().processor->tail_frames(), 2, "IR tail was not retained");
  std::array<StereoFrame, 4> output{{{1.0F, 1.0F}, {}, {}, {}}};
  prepared.value().processor->process(output, 1);
  REQUIRE_LT(std::abs(output[0].left - 0.5F), 0.0001F,
             "loaded IR did not render the expected stereo impulse");
  REQUIRE_LT(std::abs(output[0].right + 0.5F), 0.0001F,
             "loaded IR did not render the expected stereo impulse");
  REQUIRE_LT(std::abs(output[1].left - 0.25F), 0.0001F,
             "loaded IR did not render the expected stereo impulse");
  REQUIRE_LT(std::abs(output[1].right - 0.125F), 0.0001F,
             "loaded IR did not render the expected stereo impulse");

  auto wrong_hash = identity;
  wrong_hash.sha256[0] = wrong_hash.sha256[0] == 'a' ? 'b' : 'a';
  auto matching_wrong_hash_measurement = budget;
  matching_wrong_hash_measurement.measured_ir_sha256 = wrong_hash.sha256;
  require(!prepare_convolution_ir(fixture.fd, wrong_hash, matching_wrong_hash_measurement, 48'000,
                                  1.0F),
          "different encoded media was admitted");
  auto wrong_meta = identity;
  wrong_meta.frames = 2;
  auto matching_wrong_meta_measurement = budget;
  matching_wrong_meta_measurement.measured_ir_frames = wrong_meta.frames;
  require(!prepare_convolution_ir(fixture.fd, wrong_meta, matching_wrong_meta_measurement, 48'000,
                                  1.0F),
          "different decoded metadata was admitted");
  require(!prepare_convolution_ir(fixture.fd, identity, budget, 44'100, 1.0F),
          "wrong project rate was admitted");
  auto wrong_measurement = budget;
  wrong_measurement.measured_ir_sha256[0] =
      wrong_measurement.measured_ir_sha256[0] == 'a' ? 'b' : 'a';
  require(!prepare_convolution_ir(fixture.fd, identity, wrong_measurement, 48'000, 1.0F),
          "another IR's callback cost was admitted");
  wrong_measurement = budget;
  wrong_measurement.measured_ir_frames = 2;
  require(!prepare_convolution_ir(fixture.fd, identity, wrong_measurement, 48'000, 1.0F),
          "another IR length's callback cost was admitted");
  wrong_measurement = budget;
  wrong_measurement.measured_sample_rate = 96'000;
  require(!prepare_convolution_ir(fixture.fd, identity, wrong_measurement, 48'000, 1.0F),
          "another sample rate's callback cost was admitted");
  wrong_measurement = budget;
  wrong_measurement.measured_block_frames = 256;
  require(!prepare_convolution_ir(fixture.fd, identity, wrong_measurement, 48'000, 1.0F),
          "unsupported callback block measurement was admitted");
  auto no_cpu = budget;
  no_cpu.measured_worst_callback_us = 0;
  require(!prepare_convolution_ir(fixture.fd, identity, no_cpu, 48'000, 1.0F),
          "unknown callback cost was admitted");
  auto over_cpu = budget;
  over_cpu.measured_worst_callback_us = 151;
  require(!prepare_convolution_ir(fixture.fd, identity, over_cpu, 48'000, 1.0F),
          "over-budget callback cost was admitted");
  auto replacement = budget;
  replacement.active_instances = 1;
  require(!prepare_convolution_ir(fixture.fd, identity, replacement, 48'000, 1.0F),
          "unbudgeted overlapping runtime was admitted");
  auto no_memory = budget;
  no_memory.available_preparation_bytes = 5ULL * 1024 * 1024;
  require(!prepare_convolution_ir(fixture.fd, identity, no_memory, 48'000, 1.0F),
          "under-budget preparation memory was admitted");
  auto high_rate_identity = identity;
  high_rate_identity.sample_rate = oscillate::test::sample_rate_96k;
  require(!prepare_convolution_ir(fixture.fd, high_rate_identity, budget, 96'000, 1.0F),
          "96 kHz IR was admitted against a 48 kHz memory reserve");

  auto malformed = wav(samples);
  malformed[20] = 99;
  Fixture malformed_file(malformed);
  auto malformed_identity = identity;
  malformed_identity.sha256 = sha256(malformed);
  auto matching_malformed_measurement = budget;
  matching_malformed_measurement.measured_ir_sha256 = malformed_identity.sha256;
  require(!prepare_convolution_ir(malformed_file.fd, malformed_identity,
                                  matching_malformed_measurement, 48'000, 1.0F),
          "malformed IR media was admitted despite matching recorded hash");

  bytes.back() ^= 1U;
  std::ofstream changed(fixture.path, std::ios::binary | std::ios::trunc);
  changed.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
  changed.close();
  require(!prepare_convolution_ir(fixture.fd, identity, budget, 48'000, 1.0F),
          "modified source bytes were admitted");
}
} // namespace

int main() {
  try {
    admission_contract();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
