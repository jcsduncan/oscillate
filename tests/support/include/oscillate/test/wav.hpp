#pragma once

// WAV fixtures: canonical 44-byte-header RIFF/WAVE files written from samples
// and read back for checking. The writers produce exactly the bytes the
// hand-rolled fixture writers did (RIFF, a 16-byte fmt chunk, data), so
// fixtures that hash or size their files keep their values.
//
//   oscillate::test::write_wav_pcm16(path, sample_rate_48k, mono_channels, samples);
//   const auto rendered = oscillate::test::read_float_wav(output);

#include "oscillate/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::test {

// The WAV format tag is a 16-bit field, so the enum keeps its width.
enum class WavEncoding : std::uint16_t { pcm = 1, ieee_float = 3 }; // NOLINT(performance-enum-size)

struct WavLayout {
  std::uint32_t sample_rate;
  std::uint16_t channels;
  std::uint16_t bits_per_sample;
  WavEncoding encoding;

  [[nodiscard]] constexpr std::uint16_t block_align() const noexcept {
    return static_cast<std::uint16_t>(channels * (bits_per_sample / bits_per_byte));
  }
  [[nodiscard]] constexpr std::uint32_t byte_rate() const noexcept {
    return sample_rate * block_align();
  }

  static constexpr std::uint16_t bits_per_byte = 8;
};

inline constexpr std::uint16_t pcm8_bits = 8;
inline constexpr std::uint16_t pcm16_bits = 16;
inline constexpr std::uint16_t pcm24_bits = 24;
inline constexpr std::uint16_t float32_bits = 32;

// Sizes of the canonical layout.
inline constexpr std::uint32_t wav_fmt_chunk_bytes = 16;
inline constexpr std::uint32_t wav_header_bytes = 44;
// The RIFF size counts everything after its own 8-byte header: "WAVE", the
// fmt chunk and the data chunk header come to 36 bytes before the samples.
inline constexpr std::uint32_t wav_riff_overhead = wav_header_bytes - 8;
// Offsets into the canonical header.
inline constexpr std::size_t wav_format_offset = 20;
inline constexpr std::size_t wav_channels_offset = 22;
inline constexpr std::size_t wav_rate_offset = 24;
inline constexpr std::size_t wav_bits_offset = 34;
inline constexpr std::size_t wav_data_size_offset = 40;

namespace detail {
inline constexpr unsigned byte_bits = 8;
inline constexpr unsigned byte_mask = 0xffU;
} // namespace detail

inline void append_le16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & detail::byte_mask));
  bytes.push_back(static_cast<std::uint8_t>((value >> detail::byte_bits) & detail::byte_mask));
}

inline void append_le32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  constexpr unsigned half_bits = 16;
  constexpr std::uint32_t half_mask = 0xffffU;
  append_le16(bytes, static_cast<std::uint16_t>(value & half_mask));
  append_le16(bytes, static_cast<std::uint16_t>(value >> half_bits));
}

inline void append_tag(std::vector<std::uint8_t>& bytes, std::string_view tag) {
  bytes.insert(bytes.end(), tag.begin(), tag.end());
}

[[nodiscard]] inline std::uint16_t read_le16(std::span<const std::uint8_t> bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << detail::byte_bits));
}

[[nodiscard]] inline std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t at) {
  constexpr unsigned half_bits = 16;
  return static_cast<std::uint32_t>(read_le16(bytes, at)) |
         (static_cast<std::uint32_t>(read_le16(bytes, at + 2)) << half_bits);
}

// The 44-byte header for `data_bytes` of samples.
[[nodiscard]] inline std::vector<std::uint8_t> wav_header(const WavLayout& layout,
                                                          std::uint32_t data_bytes) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(wav_header_bytes);
  append_tag(bytes, "RIFF");
  append_le32(bytes, wav_riff_overhead + data_bytes);
  append_tag(bytes, "WAVEfmt ");
  append_le32(bytes, wav_fmt_chunk_bytes);
  append_le16(bytes, static_cast<std::uint16_t>(layout.encoding));
  append_le16(bytes, layout.channels);
  append_le32(bytes, layout.sample_rate);
  append_le32(bytes, layout.byte_rate());
  append_le16(bytes, layout.block_align());
  append_le16(bytes, layout.bits_per_sample);
  append_tag(bytes, "data");
  append_le32(bytes, data_bytes);
  return bytes;
}

// A whole file of 16-bit PCM from interleaved samples.
[[nodiscard]] inline std::vector<std::uint8_t>
wav_bytes_pcm16(std::uint32_t sample_rate, std::uint16_t channels,
                std::span<const std::int16_t> interleaved) {
  const auto data_bytes = static_cast<std::uint32_t>(interleaved.size() * sizeof(std::int16_t));
  auto bytes = wav_header({sample_rate, channels, pcm16_bits, WavEncoding::pcm}, data_bytes);
  for (const auto sample : interleaved) {
    append_le16(bytes, static_cast<std::uint16_t>(sample));
  }
  return bytes;
}

// A whole file of 32-bit float from interleaved samples.
[[nodiscard]] inline std::vector<std::uint8_t> wav_bytes_float(std::uint32_t sample_rate,
                                                               std::uint16_t channels,
                                                               std::span<const float> interleaved) {
  const auto data_bytes = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
  auto bytes =
      wav_header({sample_rate, channels, float32_bits, WavEncoding::ieee_float}, data_bytes);
  for (const auto sample : interleaved) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &sample, sizeof bits);
    append_le32(bytes, bits);
  }
  return bytes;
}

inline void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  require(out.good(), "fixture write failed: " + path.string());
}

[[nodiscard]] inline std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "fixture read failed: " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_wav_pcm16(const std::filesystem::path& path, std::uint32_t sample_rate,
                            std::uint16_t channels, std::span<const std::int16_t> interleaved) {
  write_bytes(path, wav_bytes_pcm16(sample_rate, channels, interleaved));
}

inline void write_wav_float(const std::filesystem::path& path, std::uint32_t sample_rate,
                            std::uint16_t channels, std::span<const float> interleaved) {
  write_bytes(path, wav_bytes_float(sample_rate, channels, interleaved));
}

// A parsed WAV: its format chunk and the data chunk's raw bytes. Chunks other
// than fmt and data are skipped (RIFF pads odd-sized chunks to even).
struct WavFile {
  WavLayout layout{};
  std::vector<std::uint8_t> data;
};

[[nodiscard]] inline WavFile parse_wav(std::span<const std::uint8_t> bytes) {
  constexpr std::size_t riff_header_bytes = 12;
  constexpr std::size_t chunk_header_bytes = 8;
  constexpr std::size_t tag_bytes = 4;
  require(bytes.size() >= riff_header_bytes && std::memcmp(bytes.data(), "RIFF", tag_bytes) == 0 &&
              std::memcmp(bytes.data() + 2 * tag_bytes, "WAVE", tag_bytes) == 0,
          "not a RIFF/WAVE file");
  WavFile file;
  bool have_format = false;
  std::size_t position = riff_header_bytes;
  while (position + chunk_header_bytes <= bytes.size()) {
    const auto size = read_le32(bytes, position + tag_bytes);
    const auto body = position + chunk_header_bytes;
    require(body + size <= bytes.size(), "truncated WAV chunk");
    if (std::memcmp(bytes.data() + position, "fmt ", tag_bytes) == 0) {
      constexpr std::size_t channels_at = 2;
      constexpr std::size_t rate_at = 4;
      constexpr std::size_t bits_at = 14;
      require(size >= wav_fmt_chunk_bytes, "WAV fmt chunk too short");
      file.layout = {read_le32(bytes, body + rate_at), read_le16(bytes, body + channels_at),
                     read_le16(bytes, body + bits_at),
                     static_cast<WavEncoding>(read_le16(bytes, body))};
      have_format = true;
    } else if (std::memcmp(bytes.data() + position, "data", tag_bytes) == 0) {
      require(have_format, "WAV data chunk before its fmt chunk");
      file.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body),
                       bytes.begin() + static_cast<std::ptrdiff_t>(body + size));
      return file;
    }
    position = body + size + (size & 1U);
  }
  throw TestFailure("WAV has no data chunk");
}

[[nodiscard]] inline WavFile read_wav(const std::filesystem::path& path) {
  return parse_wav(read_bytes(path));
}

// The data chunk of a WAV file read as interleaved 32-bit floats, whatever
// its header says (the export tests' oracle reads the bytes it wrote).
[[nodiscard]] inline std::vector<float> read_float_wav(const std::filesystem::path& path) {
  const auto file = read_wav(path);
  std::vector<float> samples(file.data.size() / sizeof(float));
  std::memcpy(samples.data(), file.data.data(), samples.size() * sizeof(float));
  return samples;
}

} // namespace oscillate::test
