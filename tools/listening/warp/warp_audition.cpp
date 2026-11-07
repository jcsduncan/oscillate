// Opt-in, worker-only audition harness for the warp engines. Reads a local
// 48 kHz stereo WAV through the verified media reader, renders every warp mode
// for a fixed set of cases and writes 32-bit float WAVs plus a manifest. It is
// a listening and measurement aid, not a CTest workload; see
// docs/performance/warp-recorded-review.md for the review protocol.
#include "oscillate/media/prefetch.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <openssl/evp.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace warp = oscillate::warp_dsp;
namespace analysis = oscillate::warp_dsp::analysis;
namespace media = oscillate::media;

constexpr std::uint32_t required_rate = 48'000;
constexpr std::uint16_t required_channels = 2;
constexpr std::uint64_t max_input_frames = std::uint64_t{required_rate} * 600;
constexpr std::uint64_t max_output_frames = std::uint64_t{required_rate} * 600;
constexpr std::size_t pull_frames = 128;
constexpr std::size_t read_chunk = std::size_t{1} << 16;

const char* const usage =
    "Usage:\n"
    "  oscillate_warp_audition --input IN.wav --output-dir DIR [--tier q0|q1|q2]\n"
    "                          [--extreme]\n"
    "  oscillate_warp_audition --self-test DIR\n"
    "\n"
    "Renders Re-Pitch, Beats, Complex, Complex Pro, Tones and Texture for the\n"
    "identity, 0.5x, 2x and marker rate-change cases, plus +/-7 semitones for\n"
    "modes with transpose, as 32-bit float WAVs in DIR with DIR/manifest.tsv.\n"
    "--extreme adds 0.25x, 4x and +/-12 semitone cases.\n"
    "Input must be a 48 kHz stereo WAV. Every case must fit within 10 minutes\n"
    "of output (at most 5 minutes of input, or 2.5 with --extreme). It is never\n"
    "resampled or channel-converted. Choose DIR inside a build tree, e.g.:\n"
    "\n"
    "  build/warp-release/tools/listening/warp/oscillate_warp_audition \\\n"
    "    --input ~/loops/drums.wav --output-dir build/warp-release/audition/drums\n";

class Failure final : public std::exception {
public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
  std::string message_;
};

double thread_seconds() {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
}

std::string sha256_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw Failure("cannot read input for digest: " + path.string());
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                  &EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw Failure("SHA-256 unavailable");
  std::vector<char> buffer(read_chunk);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(), count) != 1)
      throw Failure("SHA-256 update failed");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
    throw Failure("SHA-256 finalisation failed");
  std::string hex;
  for (unsigned int index = 0; index < length; ++index) {
    static constexpr std::string_view digits = "0123456789abcdef";
    hex.push_back(digits[digest[index] >> 4U]);
    hex.push_back(digits[digest[index] & 0x0FU]);
  }
  return hex;
}

// Whole input decoded once through the verified WAV reader.
struct Input {
  std::vector<float> left;
  std::vector<float> right;
  std::string digest;
};

Input load(const fs::path& path) {
  auto opened = media::WavReader::open(path);
  if (!opened)
    throw Failure("unsupported or unreadable WAV: " + opened.error().message);
  auto& reader = opened.value();
  if (reader.sample_rate() != required_rate)
    throw Failure("input is " + std::to_string(reader.sample_rate()) +
                  " Hz; the harness requires 48000 Hz and does not resample");
  if (reader.channels() != required_channels)
    throw Failure("input has " + std::to_string(reader.channels()) +
                  " channels; the harness requires stereo and does not convert channels");
  if (reader.frame_count() == 0 || reader.frame_count() > max_input_frames)
    throw Failure("input must hold 1 frame to 10 minutes of audio");
  Input input;
  const auto frames = static_cast<std::size_t>(reader.frame_count());
  input.left.resize(frames);
  input.right.resize(frames);
  std::vector<media::StereoFrame> chunk(read_chunk);
  std::size_t done = 0;
  while (done < frames) {
    const auto want = std::min(read_chunk, frames - done);
    auto read = reader.read_at(done, std::span(chunk).first(want));
    if (!read || read.value() == 0)
      throw Failure("input read failed at frame " + std::to_string(done));
    for (std::size_t index = 0; index < read.value(); ++index) {
      input.left[done + index] = chunk[index].left;
      input.right[done + index] = chunk[index].right;
    }
    done += read.value();
  }
  input.digest = sha256_file(path);
  return input;
}

void write_float_wav(const fs::path& path, const std::vector<float>& left,
                     const std::vector<float>& right) {
  const auto frames = static_cast<std::uint64_t>(left.size());
  const std::uint64_t data_bytes = frames * 2 * sizeof(float);
  if (data_bytes > 0xFFFF'FFFFULL - 36)
    throw Failure("render too long for a WAV file: " + path.string());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    throw Failure("cannot write " + path.string());
  const auto put16 = [&output](std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                    static_cast<char>(value >> 8U)};
    output.write(bytes.data(), 2);
  };
  const auto put32 = [&output](std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>(value >> 24U)};
    output.write(bytes.data(), 4);
  };
  output.write("RIFF", 4);
  put32(static_cast<std::uint32_t>(36 + data_bytes));
  output.write("WAVEfmt ", 8);
  put32(16);
  put16(3); // IEEE float
  put16(2);
  put32(required_rate);
  put32(required_rate * 2 * sizeof(float));
  put16(2 * sizeof(float));
  put16(32);
  output.write("data", 4);
  put32(static_cast<std::uint32_t>(data_bytes));
  std::array<float, 8192 * 2> interleaved{};
  for (std::size_t first = 0; first < left.size();) {
    const auto count = std::min<std::size_t>(8192, left.size() - first);
    for (std::size_t index = 0; index < count; ++index) {
      interleaved[2 * index] = left[first + index];
      interleaved[2 * index + 1] = right[first + index];
    }
    output.write(reinterpret_cast<const char*>(interleaved.data()),
                 static_cast<std::streamsize>(count * 2 * sizeof(float)));
    first += count;
  }
  if (!output)
    throw Failure("write failed: " + path.string());
}

// One render case: a time map (as segments over the input) and a transpose.
struct Case {
  std::string name;
  std::string description;
  std::vector<warp::TimeMapSegment> segments;
  std::int32_t transpose_cents = 0;
};

std::uint64_t q16(std::uint64_t frame) { return frame << warp::source_fraction_bits; }

std::vector<Case> cases_for(std::uint64_t frames, bool extreme) {
  const auto stretched = [frames](double speed) {
    const auto output =
        static_cast<std::uint64_t>(std::llround(static_cast<double>(frames) / speed));
    return std::vector<warp::TimeMapSegment>{{0, output, 0, q16(frames), false}};
  };
  // Markers at one and two thirds of the source: unity, then 0.6x, then 1.5x.
  const auto third = frames / 3;
  const auto first_end = third;
  const auto second_end =
      first_end + static_cast<std::uint64_t>(std::llround(static_cast<double>(third) / 0.6));
  const auto rest = frames - 2 * third;
  const auto third_end =
      second_end + static_cast<std::uint64_t>(std::llround(static_cast<double>(rest) / 1.5));
  std::vector<warp::TimeMapSegment> markers{
      {0, first_end, 0, q16(third), false},
      {first_end, second_end, q16(third), q16(2 * third), false},
      {second_end, std::max(third_end, second_end + 1), q16(2 * third), q16(frames), false}};
  std::vector<Case> cases{{"identity", "unity map", stretched(1.0), 0},
                          {"half", "0.5x speed (twice as long)", stretched(0.5), 0},
                          {"double", "2x speed (half as long)", stretched(2.0), 0},
                          {"markers", "markers at 1/3 and 2/3: 1x, 0.6x, 1.5x", markers, 0},
                          {"up7", "unity map, +700 cents", stretched(1.0), 700},
                          {"down7", "unity map, -700 cents", stretched(1.0), -700}};
  if (extreme) {
    cases.push_back({"quarter", "0.25x speed (four times as long)", stretched(0.25), 0});
    cases.push_back({"quadruple", "4x speed (a quarter as long)", stretched(4.0), 0});
    cases.push_back({"up12", "unity map, +1200 cents", stretched(1.0), 1'200});
    cases.push_back({"down12", "unity map, -1200 cents", stretched(1.0), -1'200});
  }
  return cases;
}

struct Analysis {
  std::vector<std::uint64_t> onsets;
  std::vector<analysis::PitchMark> marks;
};

Analysis analyse(warp::SourceView& view) {
  auto transients = analysis::detect_transients(view, {required_rate, 1.0F, 0.030});
  if (!transients)
    throw Failure("transient analysis failed: " + transients.error().message);
  Analysis result;
  for (const auto& transient : transients.value())
    result.onsets.push_back(transient.frame);
  auto track = analysis::track_pitch(view, {required_rate, 60.0F, 1'000.0F, 0.15F});
  if (!track)
    throw Failure("pitch tracking failed: " + track.error().message);
  auto marks = analysis::place_pitch_marks(view, track.value(), required_rate);
  if (!marks)
    throw Failure("pitch marking failed: " + marks.error().message);
  result.marks = std::move(marks).value();
  return result;
}

struct Mode {
  std::string name;
  bool transposes;
  std::function<oscillate::foundation::Result<std::unique_ptr<warp::WarpStream>>(warp::QualityTier,
                                                                                 std::int32_t)>
      create;
};

std::vector<Mode> modes_for(const Analysis& found) {
  const warp::StreamLimits limits{required_rate, 8'192};
  return {{"repitch", false,
           [limits](warp::QualityTier tier, std::int32_t) {
             return warp::create_repitch_stream(limits, tier);
           }},
          {"beats", true,
           [limits, &found](warp::QualityTier tier, std::int32_t cents) {
             return warp::create_beats_stream(
                 limits, tier, {found.onsets, warp::BeatsLoop::forward, 100.0F, cents});
           }},
          {"complex", true,
           [limits, &found](warp::QualityTier tier, std::int32_t cents) {
             return warp::create_complex_stream(limits, tier, {found.onsets, cents});
           }},
          {"complex_pro", true,
           [limits, &found](warp::QualityTier tier, std::int32_t cents) {
             return warp::create_complex_pro_stream(limits, tier,
                                                    {found.onsets, cents, 100.0F, 128.0F});
           }},
          {"tones", true,
           [limits, &found](warp::QualityTier tier, std::int32_t cents) {
             return warp::create_tones_stream(limits, tier, {found.marks, 40.0F, cents});
           }},
          {"texture", true, [limits](warp::QualityTier tier, std::int32_t cents) {
             return warp::create_texture_stream(limits, tier, {100.0F, 0.0F, cents, 1});
           }}};
}

const char* status_name(warp::StreamStatus status) {
  switch (status) {
  case warp::StreamStatus::ok:
    return "ok";
  case warp::StreamStatus::not_started:
    return "not_started";
  case warp::StreamStatus::unsupported_map:
    return "unsupported_map";
  case warp::StreamStatus::out_of_range:
    return "out_of_range";
  case warp::StreamStatus::source_failed:
    return "source_failed";
  case warp::StreamStatus::unprepared:
    return "unprepared";
  }
  return "unknown";
}

const char* tier_name(warp::QualityTier tier) {
  switch (tier) {
  case warp::QualityTier::q0:
    return "q0";
  case warp::QualityTier::q1:
    return "q1";
  case warp::QualityTier::q2:
    return "q2";
  }
  return "?";
}

struct Row {
  std::string file;
  std::string mode;
  std::string case_name;
  std::string description;
  std::int32_t cents = 0;
  std::uint64_t expected_frames = 0;
  std::uint64_t frames = 0;
  double peak = 0.0;
  std::uint64_t clipped = 0;
  double cpu_seconds = 0.0;
  std::string status;
};

Row render_case(const Mode& mode, const Case& render_case, warp::QualityTier tier,
                warp::SourceView& view, const fs::path& directory) {
  Row row;
  row.mode = mode.name;
  row.case_name = render_case.name;
  row.description = render_case.description;
  row.cents = render_case.transpose_cents;
  row.file = mode.name + "_" + render_case.name + ".wav";
  auto map = warp::WarpTimeMap::create(render_case.segments);
  if (!map)
    throw Failure("case map rejected: " + map.error().message);
  row.expected_frames = map.value().output_frames();
  auto stream = mode.create(tier, render_case.transpose_cents);
  if (!stream) {
    row.status = "create_failed: " + stream.error().message;
    return row;
  }
  std::vector<float> left(static_cast<std::size_t>(row.expected_frames), 0.0F);
  std::vector<float> right(left.size(), 0.0F);
  const double started = thread_seconds();
  auto status = stream.value()->start(map.value(), view, 0);
  std::size_t done = 0;
  while (status == warp::StreamStatus::ok && done < left.size()) {
    const auto want = std::min(pull_frames, left.size() - done);
    const auto written = stream.value()->render(std::span(left).subspan(done, want),
                                                std::span(right).subspan(done, want));
    done += written;
    if (written < want) {
      status = stream.value()->status();
      if (status == warp::StreamStatus::ok) {
        row.status = "short_render_without_status";
        break;
      }
    }
  }
  row.cpu_seconds = thread_seconds() - started;
  row.frames = done;
  if (row.status.empty())
    row.status = status_name(status);
  left.resize(done);
  right.resize(done);
  for (std::size_t index = 0; index < done; ++index) {
    const double magnitude = std::max(std::fabs(left[index]), std::fabs(right[index]));
    row.peak = std::max(row.peak, magnitude);
    row.clipped +=
        (std::fabs(left[index]) >= 1.0F ? 1U : 0U) + (std::fabs(right[index]) >= 1.0F ? 1U : 0U);
  }
  write_float_wav(directory / row.file, left, right);
  return row;
}

std::vector<Row> run(const fs::path& input_path, const fs::path& directory, warp::QualityTier tier,
                     bool extreme, const std::string& command) {
  const auto input = load(input_path);
  const auto cases = cases_for(input.left.size(), extreme);
  if (std::ranges::any_of(cases, [](const Case& item) {
        return item.segments.back().output_end > max_output_frames;
      }))
    throw Failure("audition output exceeds 10 minutes; choose a shorter input clip");
  warp::MemorySourceView view(input.left, input.right);
  fs::create_directories(directory);
  const auto found = analyse(view);
  std::vector<Row> rows;
  for (const auto& mode : modes_for(found)) {
    for (const auto& item : cases) {
      if (item.transpose_cents != 0 && !mode.transposes)
        continue;
      rows.push_back(render_case(mode, item, tier, view, directory));
      std::printf("%-12s %-9s %-7s frames %llu/%llu cpu %.3f s\n", rows.back().mode.c_str(),
                  rows.back().case_name.c_str(), rows.back().status.c_str(),
                  static_cast<unsigned long long>(rows.back().frames),
                  static_cast<unsigned long long>(rows.back().expected_frames),
                  rows.back().cpu_seconds);
    }
  }
  std::ofstream manifest(directory / "manifest.tsv", std::ios::trunc);
  if (!manifest)
    throw Failure("cannot write manifest");
  manifest << "# oscillate_warp_audition manifest\n"
           << "# command\t" << command << "\n"
           << "# input\t" << input_path.string() << "\n"
           << "# input_sha256\t" << input.digest << "\n"
           << "# input_frames\t" << input.left.size() << "\n"
           << "# sample_rate\t" << required_rate << "\n"
           << "# tier\t" << tier_name(tier) << "\n"
           << "# extreme_cases\t" << (extreme ? "yes" : "no") << "\n"
           << "# transients\t" << found.onsets.size() << "\n"
           << "# pitch_marks\t" << found.marks.size() << "\n"
           << "# render_pull_frames\t" << pull_frames << "\n"
           << "# audio is deterministic for a given input and command; cpu_seconds is not\n"
           << "file\tmode\tcase\tdescription\ttranspose_cents\texpected_frames\toutput_frames\t"
              "duration_seconds\tpeak\tclipped_samples\tcpu_seconds\tstatus\n";
  for (const auto& row : rows) {
    std::ostringstream line;
    line.precision(6);
    line << row.file << '\t' << row.mode << '\t' << row.case_name << '\t' << row.description << '\t'
         << row.cents << '\t' << row.expected_frames << '\t' << row.frames << '\t' << std::fixed
         << static_cast<double>(row.frames) / required_rate << '\t' << row.peak << '\t'
         << row.clipped << '\t' << row.cpu_seconds << '\t' << row.status << '\n';
    manifest << line.str();
  }
  if (!manifest)
    throw Failure("manifest write failed");
  return rows;
}

// --- self test ------------------------------------------------------------

void write_pcm16(const fs::path& path, std::uint32_t rate, std::uint16_t channels,
                 const std::vector<float>& samples) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
  const auto put16 = [&output](std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                    static_cast<char>(value >> 8U)};
    output.write(bytes.data(), 2);
  };
  const auto put32 = [&output](std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>(value >> 24U)};
    output.write(bytes.data(), 4);
  };
  output.write("RIFF", 4);
  put32(36 + data_bytes);
  output.write("WAVEfmt ", 8);
  put32(16);
  put16(1);
  put16(channels);
  put32(rate);
  put32(rate * channels * 2U);
  put16(static_cast<std::uint16_t>(channels * 2U));
  put16(16);
  output.write("data", 4);
  put32(data_bytes);
  for (const float sample : samples)
    put16(static_cast<std::uint16_t>(
        static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0F, 1.0F) * 32'000.0F))));
  if (!output)
    throw Failure("self-test fixture write failed");
}

// Drum hits over a sustained harmonic tone: transients and pitch together.
std::vector<float> fixture(std::uint32_t rate, std::uint16_t channels, double seconds) {
  const auto frames = static_cast<std::size_t>(seconds * rate);
  std::vector<float> samples(frames * channels);
  std::uint64_t noise = 1;
  for (std::size_t index = 0; index < frames; ++index) {
    const double t = static_cast<double>(index) / rate;
    double tone = 0.0;
    for (int harmonic = 1; harmonic <= 6; ++harmonic)
      tone += std::sin(2.0 * std::numbers::pi * 165.0 * harmonic * t) / harmonic;
    const double beat = std::fmod(t, 0.25);
    noise = noise * 6364136223846793005ULL + 1442695040888963407ULL;
    const double hit =
        (static_cast<double>(noise >> 11) / 4503599627370496.0 - 1.0) * std::exp(-beat / 0.03);
    const double value = 0.12 * tone + 0.35 * hit;
    for (std::uint16_t channel = 0; channel < channels; ++channel)
      samples[index * channels + channel] = static_cast<float>(channel == 0 ? value : 0.9 * value);
  }
  return samples;
}

struct WavHeader {
  std::uint16_t format = 0;
  std::uint16_t channels = 0;
  std::uint32_t rate = 0;
  std::uint16_t bits = 0;
  std::uint64_t frames = 0;
};

WavHeader read_header(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<unsigned char, 44> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVEfmt ", 8) != 0 ||
      std::memcmp(bytes.data() + 36, "data", 4) != 0)
    throw Failure("bad WAV header: " + path.string());
  const auto u16 = [&bytes](std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8U));
  };
  const auto u32 = [&bytes](std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24U);
  };
  WavHeader header{u16(20), u16(22), u32(24), u16(34), 0};
  const auto data = u32(40);
  if (header.channels == 0 || header.bits == 0)
    throw Failure("degenerate WAV header: " + path.string());
  header.frames = data / (header.channels * (header.bits / 8U));
  if (fs::file_size(path) != 44 + static_cast<std::uintmax_t>(data))
    throw Failure("WAV size disagrees with its header: " + path.string());
  return header;
}

void check(bool condition, const std::string& message) {
  if (!condition)
    throw Failure("self-test: " + message);
}

int self_test(const fs::path& directory) {
  fs::create_directories(directory);
  const auto input = directory / "selftest_input.wav";
  write_pcm16(input, required_rate, 2, fixture(required_rate, 2, 2.0));
  const auto renders = directory / "renders";
  const auto rows = run(input, renders, warp::QualityTier::q0, false, "--self-test");
  // Re-Pitch has 4 cases (no transpose); the other five modes have 6.
  check(rows.size() == 4 + 5 * 6, "expected 34 renders, got " + std::to_string(rows.size()));
  for (const auto& row : rows) {
    check(row.status == "ok", row.file + " status " + row.status);
    check(row.frames == row.expected_frames, row.file + " frame count");
    const auto header = read_header(renders / row.file);
    check(header.format == 3 && header.channels == 2 && header.rate == required_rate &&
              header.bits == 32 && header.frames == row.frames,
          row.file + " header");
    check(row.peak > 0.0, row.file + " is silent");
  }
  std::ifstream manifest(renders / "manifest.tsv");
  std::size_t data_lines = 0;
  std::string line;
  bool digest = false;
  while (std::getline(manifest, line)) {
    if (line.rfind("# input_sha256\t", 0) == 0)
      digest = line.size() == std::string_view("# input_sha256\t").size() + 64;
    else if (!line.empty() && line[0] != '#')
      ++data_lines;
  }
  check(digest, "manifest digest");
  check(data_lines == rows.size() + 1, "manifest rows");
  // Deterministic audio: a second run writes identical bytes.
  const auto again = directory / "renders_again";
  (void)run(input, again, warp::QualityTier::q0, false, "--self-test");
  for (const auto& row : rows) {
    std::ifstream first(renders / row.file, std::ios::binary);
    std::ifstream second(again / row.file, std::ios::binary);
    const std::string a((std::istreambuf_iterator<char>(first)), {});
    const std::string b((std::istreambuf_iterator<char>(second)), {});
    check(a == b, row.file + " is not deterministic");
  }
  // Extreme cases render with exact lengths too (Re-Pitch has no transpose).
  const auto extreme = run(input, directory / "renders_extreme", warp::QualityTier::q0, true,
                           "--self-test --extreme");
  check(extreme.size() == 6 + 5 * 10, "expected 56 extreme-set renders");
  for (const auto& row : extreme) {
    check(row.status == "ok", row.file + " extreme status " + row.status);
    check(row.frames == row.expected_frames, row.file + " extreme frame count");
  }
  // Unsupported inputs fail clearly and render nothing.
  const auto rejected = [&directory](const std::string& name, std::uint32_t rate,
                                     std::uint16_t channels, const std::string& expect) {
    const auto path = directory / name;
    write_pcm16(path, rate, channels, fixture(rate, channels, 0.5));
    const auto output = directory / (name + "_out");
    try {
      (void)run(path, output, warp::QualityTier::q0, false, "--self-test");
    } catch (const Failure& failure) {
      check(std::string(failure.what()).find(expect) != std::string::npos,
            name + " rejected with an unclear message: " + failure.what());
      check(!fs::exists(output / "manifest.tsv"), name + " wrote a manifest");
      return;
    }
    throw Failure("self-test: " + name + " was accepted");
  };
  rejected("mono.wav", required_rate, 1, "requires stereo");
  rejected("rate44k.wav", 44'100, 2, "requires 48000 Hz");
  {
    const auto path = directory / "not_a_wav.wav";
    std::ofstream(path) << "not audio";
    bool failed = false;
    try {
      (void)run(path, directory / "not_a_wav_out", warp::QualityTier::q0, false, "--self-test");
    } catch (const Failure& failure) {
      failed =
          std::string(failure.what()).find("unsupported or unreadable WAV") != std::string::npos;
    }
    check(failed, "a non-WAV input was not rejected clearly");
  }
  std::printf("self-test passed: %zu renders verified in %s\n", rows.size(),
              renders.string().c_str());
  return 0;
}

std::optional<warp::QualityTier> parse_tier(std::string_view text) {
  if (text == "q0")
    return warp::QualityTier::q0;
  if (text == "q1")
    return warp::QualityTier::q1;
  if (text == "q2")
    return warp::QualityTier::q2;
  return std::nullopt;
}

int main_with(const std::vector<std::string>& arguments) {
  std::string command = "oscillate_warp_audition";
  for (const auto& argument : arguments)
    command += " " + argument;
  if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
    std::fputs(usage, arguments.empty() ? stderr : stdout);
    return arguments.empty() ? 2 : 0;
  }
  if (arguments[0] == "--self-test") {
    if (arguments.size() != 2)
      throw Failure("--self-test takes one output directory");
    return self_test(arguments[1]);
  }
  std::optional<fs::path> input;
  std::optional<fs::path> output;
  auto tier = warp::QualityTier::q0;
  bool extreme = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& option = arguments[index];
    if (option == "--extreme") {
      extreme = true;
      continue;
    }
    if (index + 1 >= arguments.size())
      throw Failure("missing value for " + option);
    const auto& value = arguments[++index];
    if (option == "--input")
      input = value;
    else if (option == "--output-dir")
      output = value;
    else if (option == "--tier") {
      const auto parsed = parse_tier(value);
      if (!parsed)
        throw Failure("--tier must be q0, q1 or q2");
      tier = *parsed;
    } else
      throw Failure("unknown option " + option);
  }
  if (!input || !output)
    throw Failure("--input and --output-dir are required");
  (void)run(*input, *output, tier, extreme, command);
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  try {
    return main_with(std::vector<std::string>(argv + 1, argv + argc));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "oscillate_warp_audition: %s\n", error.what());
    return 1;
  }
}
