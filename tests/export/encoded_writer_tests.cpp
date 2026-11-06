#include "encoded_writer.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>

#include <sndfile.h>

namespace {
using namespace oscillate::exporting;
using oscillate::exporting::detail::EncodedWriter;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-encoded-writer") {}
};

std::vector<std::uint8_t> bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "output open failed");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<float> signal() {
  std::vector<float> samples(std::size_t{48'000} * 2);
  for (std::size_t frame = 0; frame < samples.size() / 2; ++frame) {
    const auto value = static_cast<float>(
        0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / 48'000.0));
    samples[frame * 2] = value;
    samples[frame * 2 + 1] = -value;
  }
  return samples;
}

void test_format(EncodedMasterFormat format, std::string_view extension) {
  Directory directory;
  const auto target = directory.path() / (std::string("master") + std::string(extension));
  const auto samples = signal();
  EncodedMasterOptions options{target, format, 41, 0.5};
  auto render = [&]() {
    auto writer = take(EncodedWriter::begin(options, 48'000, samples.size() / 2));
    for (std::size_t first = 0; first < samples.size(); first += 2'048) {
      auto written = writer.write_interleaved_stereo(
          std::span(samples).subspan(first, std::min<std::size_t>(2'048, samples.size() - first)));
      require(static_cast<bool>(written), "encoded write failed");
    }
    REQUIRE_EQ(take(writer.finish()), PublishOutcome::durable, "encoded publication not durable");
  };
  render();
  auto original = bytes(target);
  REQUIRE_GT(original.size(), 1'000, "encoded output suspiciously small");
  SF_INFO info{};
  auto* decoded = sf_open(target.c_str(), SFM_READ, &info);
  REQUIRE_NE(decoded, nullptr, "encoded output could not be decoded");
  const auto expected_container =
      format == EncodedMasterFormat::mp3 ? SF_FORMAT_MPEG : SF_FORMAT_FLAC;
  REQUIRE_EQ((info.format & SF_FORMAT_TYPEMASK), expected_container,
             "encoded output metadata differs");
  REQUIRE_EQ(info.channels, 2, "encoded output metadata differs");
  REQUIRE_EQ(info.samplerate, 48'000, "encoded output metadata differs");
  REQUIRE_EQ(info.frames, 48'000, "encoded output metadata differs");
  std::vector<float> decoded_samples(samples.size());
  REQUIRE_EQ(sf_readf_float(decoded, decoded_samples.data(), 48'000), 48'000,
             "encoded output frames differ");
  REQUIRE_EQ(sf_close(decoded), 0, "decoded output close failed");
  double squared_error = 0.0;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto delta = static_cast<double>(decoded_samples[index] - samples[index]);
    squared_error += delta * delta;
  }
  const auto rmse = std::sqrt(squared_error / static_cast<double>(samples.size()));
  REQUIRE_LT(rmse, (format == EncodedMasterFormat::mp3 ? 0.08 : 0.0001),
             "decoded master quality differs");
  render();
  REQUIRE_EQ(bytes(target), original, "repeat encoded output differs");

  {
    auto writer = take(EncodedWriter::begin(options, 48'000, samples.size() / 2));
    require(static_cast<bool>(writer.write_interleaved_stereo(std::span(samples).first(2'048))),
            "partial encoded write failed");
    require(!writer.finish(), "truncated encoded render published");
  }
  REQUIRE_EQ(bytes(target), original, "truncated render replaced previous output");
  {
    auto writer = take(EncodedWriter::begin(options, 48'000, 1));
    require(!writer.write_interleaved_stereo(std::span(samples).first(1)),
            "odd stereo sample count accepted");
    REQUIRE_TRUE(!writer.write_interleaved_stereo(std::span(samples).first(2)),
                 "failed encoded writer became writable");
    REQUIRE_TRUE(!writer.finish(), "failed encoded writer became writable");
  }
  {
    auto writer = take(EncodedWriter::begin(options, 48'000, 1));
    require(!writer.write_interleaved_stereo(std::span(samples).first(4)),
            "excess encoded frames accepted");
  }
  {
    auto writer = take(EncodedWriter::begin(options, 48'000, 1));
    std::array<float, 2> invalid{std::numeric_limits<float>::quiet_NaN(), 0.0F};
    require(!writer.write_interleaved_stereo(invalid), "nonfinite encoded sample accepted");
  }
  {
    const auto collision = directory.path() / (std::string("occupied") + std::string(extension));
    require(fs::create_directory(collision), "collision directory creation failed");
    auto colliding_options = options;
    colliding_options.target = collision;
    auto writer = take(EncodedWriter::begin(colliding_options, 48'000, samples.size() / 2));
    require(static_cast<bool>(writer.write_interleaved_stereo(samples)),
            "collision fixture write failed");
    require(!writer.finish(), "encoded output replaced a directory");
    require(fs::is_directory(collision), "collision target changed");
  }
  if (format == EncodedMasterFormat::flac16) {
    auto writer = take(EncodedWriter::begin(options, 48'000, samples.size() / 2));
    require(static_cast<bool>(writer.write_interleaved_stereo(samples)),
            "tamper fixture write failed");
    fs::path temporary;
    for (const auto& entry : fs::directory_iterator(directory.path())) {
      if (entry.path().extension() == ".tmp") {
        temporary = entry.path();
      }
    }
    REQUIRE_TRUE(!temporary.empty(), "temporary-file tamper failed");
    REQUIRE_EQ(::truncate(temporary.c_str(), 0), 0, "temporary-file tamper failed");
    require(!writer.finish(), "tampered encoded output published");
    REQUIRE_EQ(bytes(target), original, "tampered render replaced previous output");

    {
      auto writer = take(EncodedWriter::begin(options, 48'000, samples.size() / 2));
      require(static_cast<bool>(writer.write_interleaved_stereo(samples)),
              "post-close corruption fixture write failed");
      auto corrupt = writer.finish([](int fd) {
        if (::ftruncate(fd, 0) != 0) {
          throw std::runtime_error("post-close corruption failed");
        }
      });
      require(!corrupt, "corrupt finalized encoder file published");
    }
    REQUIRE_EQ(bytes(target), original, "corrupt finalized output replaced previous output");

    {
      auto writer = take(EncodedWriter::begin(options, 48'000, samples.size() / 2));
      require(static_cast<bool>(writer.write_interleaved_stereo(samples)),
              "short finalized file fixture write failed");
      auto short_output = writer.finish([](int fd) {
        REQUIRE_EQ(::ftruncate(fd, 0), 0, "short finalized file reset failed");
        REQUIRE_EQ(::lseek(fd, 0, SEEK_SET), 0, "short finalized file reset failed");
        SF_INFO info{};
        info.samplerate = 48'000;
        info.channels = 2;
        info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
        auto* shorter = sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE);
        REQUIRE_NE(shorter, nullptr, "short finalized encoder open failed");
        std::array<short, 2> one_frame{};
        REQUIRE_EQ(sf_writef_short(shorter, one_frame.data(), 1), 1,
                   "short finalized encoder write failed");
        REQUIRE_EQ(sf_close(shorter), 0, "short finalized encoder write failed");
      });
      require(!short_output, "short finalized encoder file published");
    }
    REQUIRE_EQ(bytes(target), original, "short finalized output replaced previous output");

    fs::permissions(directory.path(), fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);
    const auto cannot_create = EncodedWriter::begin(options, 48'000, samples.size() / 2);
    fs::permissions(directory.path(), fs::perms::owner_all, fs::perm_options::replace);
    REQUIRE_TRUE(!cannot_create, "unwritable directory changed encoded output");
    REQUIRE_EQ(bytes(target), original, "unwritable directory changed encoded output");

    const auto child = ::fork();
    REQUIRE_GE(child, 0, "fault-injection fork failed");
    if (child == 0) {
      (void)::signal(SIGXFSZ, SIG_IGN);
      rlimit limit{};
      limit.rlim_cur = 4'096;
      limit.rlim_max = 4'096;
      if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        ::_exit(2);
      }
      bool failed = false;
      {
        auto limited = EncodedWriter::begin(options, 48'000, samples.size() / 2);
        if (!limited) {
          ::_exit(3);
        }
        failed = !limited.value().write_interleaved_stereo(samples);
      }
      ::_exit(failed ? 0 : 4);
    }
    int status = 0;
    REQUIRE_EQ(::waitpid(child, &status, 0), child, "file-size limit did not stop encoded write");
    REQUIRE_TRUE(WIFEXITED(status), "file-size limit did not stop encoded write");
    REQUIRE_EQ(WEXITSTATUS(status), 0, "file-size limit did not stop encoded write");
    REQUIRE_EQ(bytes(target), original, "failed encoded write replaced previous output");
  }
  for (const auto& entry : fs::directory_iterator(directory.path())) {
    require(entry.path().filename() == target.filename() || fs::is_directory(entry.path()),
            "temporary encoded file leaked");
  }
  options.target = directory.path() / "wrong.wav";
  require(!EncodedWriter::begin(options, 48'000, samples.size() / 2),
          "mismatched extension accepted");
  options.target = target;
  options.compression_level = 2.0;
  require(!EncodedWriter::begin(options, 48'000, samples.size() / 2), "invalid quality accepted");
  options.compression_level = 0.5;
  options.format = std::bit_cast<EncodedMasterFormat>(std::uint8_t{255});
  require(!EncodedWriter::begin(options, 48'000, samples.size() / 2),
          "unknown encoded format accepted");
  options.format = format;
  require(!EncodedWriter::begin(options, 48'000, 0), "zero-frame output accepted");
  require(!EncodedWriter::begin(
              options, 48'000,
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1),
          "unbounded frame count accepted");
  options.target = directory.path() / "";
  require(!EncodedWriter::begin(options, 48'000, samples.size() / 2), "unnamed output accepted");
  options.target = directory.path() / "missing" / (std::string("master") + std::string(extension));
  require(!EncodedWriter::begin(options, 48'000, samples.size() / 2),
          "missing output directory accepted");
  options.target = target;
  require(!EncodedWriter::begin(options, 96'001, samples.size() / 2), "unsupported rate accepted");
  if (format == EncodedMasterFormat::mp3) {
    require(!EncodedWriter::begin(options, 96'000, samples.size() / 2),
            "unsupported MP3 rate accepted");
  }
  REQUIRE_EQ(bytes(target), original, "rejected format changed previous output");
}

void test_integer_dither(EncodedMasterFormat format) {
  Directory directory;
  constexpr std::size_t frames = 4'096;
  const std::vector<float> silence(frames * 2, 0.0F);
  auto render = [&](std::uint64_t seed, std::size_t chunk, std::string_view name) {
    const auto target = directory.path() / std::string(name);
    auto writer = take(EncodedWriter::begin({target, format, seed, 0.5}, 48'000, frames));
    for (std::size_t first = 0; first < silence.size(); first += chunk) {
      require(static_cast<bool>(
                  writer.write_interleaved_stereo(std::span(silence).subspan(first, chunk))),
              "dither fixture write failed");
    }
    REQUIRE_EQ(take(writer.finish()), PublishOutcome::durable, "dither fixture did not publish");
    SF_INFO info{};
    auto* decoder = sf_open(target.c_str(), SFM_READ, &info);
    REQUIRE_NE(decoder, nullptr, "dither fixture bit depth differs");
    REQUIRE_EQ(info.frames, frames, "dither fixture bit depth differs");
    REQUIRE_EQ(info.channels, 2, "dither fixture bit depth differs");
    REQUIRE_EQ((info.format & SF_FORMAT_SUBMASK),
               (format == EncodedMasterFormat::flac16 ? SF_FORMAT_PCM_16 : SF_FORMAT_PCM_24),
               "dither fixture bit depth differs");
    std::vector<float> decoded(silence.size());
    REQUIRE_EQ(sf_readf_float(decoder, decoded.data(), frames), frames,
               "dither fixture did not decode");
    REQUIRE_EQ(sf_close(decoder), 0, "dither fixture did not decode");
    return decoded;
  };
  const auto extension = format == EncodedMasterFormat::flac16 ? "16" : "24";
  const auto first = render(42, silence.size(), std::string(extension) + "-one.flac");
  const auto chunked = render(42, 128, std::string(extension) + "-chunked.flac");
  const auto changed = render(43, 128, std::string(extension) + "-changed.flac");
  REQUIRE_EQ(first, chunked, "FLAC dither seed or chunking changed samples");
  REQUIRE_NE(first, changed, "FLAC dither seed or chunking changed samples");
  const auto lsb = format == EncodedMasterFormat::flac16 ? 1.0 / 32'768.0 : 1.0 / 8'388'608.0;
  std::size_t positive = 0;
  std::size_t negative = 0;
  for (const auto value : first) {
    REQUIRE_LE(std::abs(value), lsb, "FLAC silence dither exceeded one LSB");
    positive += value > 0.0F;
    negative += value < 0.0F;
  }
  REQUIRE_GT(positive, 100, "FLAC integer output lacks bipolar dither");
  REQUIRE_GT(negative, 100, "FLAC integer output lacks bipolar dither");
}

} // namespace

int main() {
  try {
    test_format(EncodedMasterFormat::flac16, ".flac");
    test_format(EncodedMasterFormat::flac24, ".flac");
    test_format(EncodedMasterFormat::mp3, ".mp3");
    test_integer_dither(EncodedMasterFormat::flac16);
    test_integer_dither(EncodedMasterFormat::flac24);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
