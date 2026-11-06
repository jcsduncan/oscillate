#pragma once

// Independent decoders for published export files. WAV parsing reads the
// canonical headers by hand; encoded files are decoded with libsndfile.

#include "oscillate/export/offline_master.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if OSCILLATE_HAVE_CODEC
#include <sndfile.h>
#endif

namespace format_oracle {

struct Decoded {
  std::uint32_t sample_rate = 0;
  std::uint16_t format_tag = 0;
  std::uint16_t bits = 0;
  std::uint64_t frames = 0;
  std::vector<float> samples; // Interleaved stereo.
};

inline std::vector<std::uint8_t> file_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.good())
    throw std::runtime_error("export output did not open: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::uint32_t le(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                        std::size_t width) {
  if (offset + width > bytes.size())
    throw std::runtime_error("WAV header is truncated");
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < width; ++index)
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8U * index);
  return value;
}

// Accepts the 56-byte float32 (fmt+fact) and 44-byte PCM16/24 layouts.
inline Decoded decode_wav(const std::filesystem::path& path) {
  const auto bytes = file_bytes(path);
  Decoded decoded;
  decoded.format_tag = static_cast<std::uint16_t>(le(bytes, 20, 2));
  if (le(bytes, 22, 2) != 2)
    throw std::runtime_error("export WAV is not stereo");
  decoded.sample_rate = le(bytes, 24, 4);
  decoded.bits = static_cast<std::uint16_t>(le(bytes, 34, 2));
  const bool floating = decoded.format_tag == 3;
  const std::size_t data = floating ? 56 : 44;
  const std::size_t sample_bytes = decoded.bits / 8U;
  if (sample_bytes == 0 || std::memcmp(bytes.data() + data - 8, "data", 4) != 0)
    throw std::runtime_error("export WAV data chunk is not canonical");
  const auto data_bytes = le(bytes, data - 4, 4);
  if (bytes.size() != data + data_bytes || data_bytes % (sample_bytes * 2) != 0)
    throw std::runtime_error("export WAV data size differs from file size");
  decoded.frames = data_bytes / (sample_bytes * 2);
  decoded.samples.resize(static_cast<std::size_t>(decoded.frames) * 2);
  for (std::size_t index = 0; index < decoded.samples.size(); ++index) {
    const auto offset = data + index * sample_bytes;
    if (floating && decoded.bits == 32) {
      std::memcpy(&decoded.samples[index], bytes.data() + offset, sizeof(float));
    } else if (decoded.format_tag == 1 && (decoded.bits == 16 || decoded.bits == 24)) {
      const auto raw = le(bytes, offset, sample_bytes);
      const auto shift = 32U - decoded.bits;
      const auto value = static_cast<std::int32_t>(raw << shift) >> shift;
      decoded.samples[index] =
          static_cast<float>(value / static_cast<double>(std::uint32_t{1} << (decoded.bits - 1U)));
    } else {
      throw std::runtime_error("export WAV sample format is unexpected");
    }
  }
  return decoded;
}

#if OSCILLATE_HAVE_CODEC
// format_tag carries the libsndfile major type and bits its subtype mask.
inline Decoded decode_encoded(const std::filesystem::path& path) {
  SF_INFO info{};
  auto* file = sf_open(path.c_str(), SFM_READ, &info);
  if (file == nullptr || info.channels != 2 || info.frames <= 0) {
    if (file != nullptr)
      (void)sf_close(file);
    throw std::runtime_error("encoded export did not open as stereo");
  }
  Decoded decoded;
  decoded.sample_rate = static_cast<std::uint32_t>(info.samplerate);
  decoded.format_tag = static_cast<std::uint16_t>((info.format & SF_FORMAT_TYPEMASK) >> 16U);
  decoded.bits = static_cast<std::uint16_t>(info.format & SF_FORMAT_SUBMASK);
  decoded.frames = static_cast<std::uint64_t>(info.frames);
  decoded.samples.resize(static_cast<std::size_t>(info.frames) * 2);
  const auto read = sf_readf_float(file, decoded.samples.data(), info.frames);
  if (sf_close(file) != 0 || read != info.frames)
    throw std::runtime_error("encoded export did not decode completely");
  return decoded;
}
#endif

inline std::uint64_t converted_frames(std::uint64_t frames, std::uint32_t from, std::uint32_t to) {
  return (frames * to + from / 2) / from;
}

// No writer temporary may survive a published or rejected render.
inline bool has_private_temporaries(const std::filesystem::path& directory) {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with(".") && name.ends_with(".tmp"))
      return true;
  }
  return false;
}

} // namespace format_oracle
