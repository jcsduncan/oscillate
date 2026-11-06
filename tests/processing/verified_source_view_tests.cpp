#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/verified_source_view.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace processing = oscillate::processing;
namespace media = oscillate::media;
namespace project = oscillate::project;
namespace persistence = oscillate::persistence;
namespace timeline = oscillate::timeline;
using oscillate::foundation::ErrorCode;

using oscillate::test::require;

using oscillate::test::take;

void put16(std::ofstream& out, std::uint16_t value) {
  for (unsigned shift = 0; shift < 16; shift += 8)
    out.put(static_cast<char>(value >> shift));
}
void put32(std::ofstream& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.put(static_cast<char>(value >> shift));
}

void write_wav(const std::filesystem::path& path, std::span<const media::StereoFrame> frames,
               std::uint32_t rate = oscillate::test::sample_rate_48k) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  const auto bytes = static_cast<std::uint32_t>(frames.size() * 8U);
  out.write("RIFF", 4);
  put32(out, 36U + bytes);
  out.write("WAVEfmt ", 8);
  put32(out, 16);
  put16(out, 3);
  put16(out, 2);
  put32(out, rate);
  put32(out, rate * 8U);
  put16(out, 8);
  put16(out, 32);
  out.write("data", 4);
  put32(out, bytes);
  for (const auto frame : frames) {
    put32(out, std::bit_cast<std::uint32_t>(frame.left));
    put32(out, std::bit_cast<std::uint32_t>(frame.right));
  }
  require(static_cast<bool>(out), "WAV fixture write failed");
}

struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("oscillate-verified-source-view-" + std::to_string(::getpid()));
  Fixture() {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }
  ~Fixture() { std::filesystem::remove_all(root); }
};

void bounded_random_reads_and_silence() {
  std::vector<std::size_t> calls;
  auto view = take(processing::WorkerSourceView::bind(
      2'500, [&calls](std::uint64_t first, std::span<media::StereoFrame> output) {
        calls.push_back(output.size());
        for (std::size_t index = 0; index < output.size(); ++index) {
          output[index] = {static_cast<float>(first + index), -static_cast<float>(first + index)};
        }
        return oscillate::foundation::Result<std::size_t>::success(output.size());
      }));
  std::vector<float> left(2'520, -10.0F), right(left.size(), -10.0F);
  require(view->read(-10, left, right), "bounded read failed");
  REQUIRE_EQ(left[0], 0.0F, "read edge silence or source positions changed");
  REQUIRE_EQ(right[9], 0.0F, "read edge silence or source positions changed");
  REQUIRE_EQ(left[10], 0.0F, "read edge silence or source positions changed");
  REQUIRE_EQ(left[2'509], 2'499.0F, "read edge silence or source positions changed");
  REQUIRE_EQ(right[2'509], -2'499.0F, "read edge silence or source positions changed");
  REQUIRE_EQ(left[2'519], 0.0F, "read edge silence or source positions changed");
  REQUIRE_EQ(calls.size(), 3, "underlying random reads exceeded fixed chunk bound");
  REQUIRE_EQ(calls[0], 1'024, "underlying random reads exceeded fixed chunk bound");
  REQUIRE_EQ(calls[1], 1'024, "underlying random reads exceeded fixed chunk bound");
  REQUIRE_EQ(calls[2], 452, "underlying random reads exceeded fixed chunk bound");
  calls.clear();
  std::array<float, 2> near_limit_left{1.0F, 1.0F}, near_limit_right{1.0F, 1.0F};
  REQUIRE_TRUE(
      view->read(std::numeric_limits<std::int64_t>::max(), near_limit_left, near_limit_right),
      "extreme out-of-range position attempted I/O");
  REQUIRE_EQ(near_limit_left, (std::array<float, 2>{0.0F, 0.0F}),
             "extreme out-of-range position attempted I/O");
  REQUIRE_TRUE(calls.empty(), "extreme out-of-range position attempted I/O");
}

void failures_are_reported_and_output_is_silent() {
  auto short_view = take(processing::WorkerSourceView::bind(
      2'000, [](std::uint64_t, std::span<media::StereoFrame> output) {
        std::ranges::fill(output, media::StereoFrame{1.0F, 2.0F});
        return oscillate::foundation::Result<std::size_t>::success(output.size() - 1U);
      }));
  std::array<float, 16> left{}, right{};
  REQUIRE_TRUE(!short_view->read(0, left, right), "short in-range read was hidden");
  REQUIRE_EQ(short_view->status(), processing::SourceReadStatus::short_read,
             "short in-range read was hidden");
  REQUIRE_EQ(short_view->error_code(), ErrorCode::io_failure, "short in-range read was hidden");
  REQUIRE_EQ(left[0], 0.0F, "short in-range read was hidden");
  REQUIRE_EQ(right[15], 0.0F, "short in-range read was hidden");

  auto error_view =
      take(processing::WorkerSourceView::bind(16, [](std::uint64_t, std::span<media::StereoFrame>) {
        return oscillate::foundation::Result<std::size_t>::failure(
            {ErrorCode::corrupt_data, "fixture decoder failed"});
      }));
  REQUIRE_TRUE(!error_view->read(0, left, right), "decoder error was hidden");
  REQUIRE_EQ(error_view->status(), processing::SourceReadStatus::decoder_error,
             "decoder error was hidden");
  REQUIRE_EQ(error_view->error_code(), ErrorCode::corrupt_data, "decoder error was hidden");

  auto nan_view = take(processing::WorkerSourceView::bind(
      16, [](std::uint64_t, std::span<media::StereoFrame> output) {
        std::ranges::fill(output, media::StereoFrame{0.5F, 0.25F});
        output.back().right = std::numeric_limits<float>::quiet_NaN();
        return oscillate::foundation::Result<std::size_t>::success(output.size());
      }));
  REQUIRE_TRUE(!nan_view->read(0, left, right),
               "nonfinite decoded sample escaped or left partial output");
  REQUIRE_EQ(nan_view->status(), processing::SourceReadStatus::nonfinite_sample,
             "nonfinite decoded sample escaped or left partial output");
  REQUIRE_EQ(nan_view->error_code(), ErrorCode::corrupt_data,
             "nonfinite decoded sample escaped or left partial output");
  REQUIRE_EQ(left[0], 0.0F, "nonfinite decoded sample escaped or left partial output");
  REQUIRE_EQ(right[15], 0.0F, "nonfinite decoded sample escaped or left partial output");

  auto throwing_view = take(processing::WorkerSourceView::bind(
      16,
      [](std::uint64_t,
         std::span<media::StereoFrame>) -> oscillate::foundation::Result<std::size_t> {
        throw std::runtime_error("decoder exception");
      }));
  REQUIRE_TRUE(!throwing_view->read(0, left, right),
               "exception crossed noexcept SourceView boundary");
  REQUIRE_EQ(throwing_view->status(), processing::SourceReadStatus::decoder_error,
             "exception crossed noexcept SourceView boundary");
}

void cancellation_stops_between_chunks() {
  std::stop_source stop;
  std::size_t calls = 0;
  auto view = take(processing::WorkerSourceView::bind(
      2'500,
      [&stop, &calls](std::uint64_t, std::span<media::StereoFrame> output) {
        ++calls;
        std::ranges::fill(output, media::StereoFrame{1.0F, 1.0F});
        stop.request_stop();
        return oscillate::foundation::Result<std::size_t>::success(output.size());
      },
      stop.get_token()));
  std::vector<float> left(2'500, 1.0F), right(2'500, 1.0F);
  REQUIRE_TRUE(!view->read(0, left, right),
               "worker cancellation decoded another chunk or kept partial output");
  REQUIRE_EQ(calls, 1, "worker cancellation decoded another chunk or kept partial output");
  REQUIRE_EQ(view->status(), processing::SourceReadStatus::cancelled,
             "worker cancellation decoded another chunk or kept partial output");
  REQUIRE_TRUE(std::ranges::all_of(left, [](float value) { return value == 0.0F; }),
               "worker cancellation decoded another chunk or kept partial output");
  REQUIRE_TRUE(std::ranges::all_of(right, [](float value) { return value == 0.0F; }),
               "worker cancellation decoded another chunk or kept partial output");
}

void exact_managed_identity_and_rate_are_required() {
  Fixture fixture;
  constexpr std::array samples{media::StereoFrame{0.25F, -0.25F}, media::StereoFrame{0.5F, -0.5F}};
  const auto wav = fixture.root / "source.wav";
  write_wav(wav, samples);
  auto store = take(persistence::ProjectStore::open_writer(fixture.root / "project", true));
  const auto source_id = take(project::SourceId::from_bits({0, 1}));
  const auto track_id = take(project::TrackId::from_bits({0, 2}));
  const auto clip_id = take(project::ClipId::from_bits({0, 3}));
  const auto project_id = take(project::ProjectId::from_bits({0, 4}));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture descriptor open failed");
  auto receipt = store.stage_wav(source_id, fd);
  (void)::close(fd);
  REQUIRE_OK(receipt, "managed fixture staging failed");
  auto record = receipt.value().record();
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Snapshot snapshot{
      project_id,
      1,
      48'000,
      std::move(tempo),
      std::move(meter),
      {{track_id, {{clip_id, source_id, {{0}, {2}}, project::ElapsedInterval{{0}, {2}}}}}},
      {record}};
  std::array<persistence::SourceReceipt, 1> receipts{std::move(receipt).value()};
  REQUIRE_OK(store.save(snapshot, persistence::SaveKind::explicit_save, receipts),
             "managed fixture project save failed");
  auto opened =
      processing::open_verified_managed_source_view(fixture.root / "project", record, 48'000);
  REQUIRE_OK(opened, "exact managed source failed verification");
  REQUIRE_EQ(opened.value()->frames(), samples.size(), "exact managed source failed verification");
  std::array<float, 4> left{}, right{};
  REQUIRE_TRUE(opened.value()->read(-1, left, right),
               "verified decoder changed stereo data or boundary silence");
  REQUIRE_EQ(left[0], 0.0F, "verified decoder changed stereo data or boundary silence");
  REQUIRE_EQ(left[1], 0.25F, "verified decoder changed stereo data or boundary silence");
  REQUIRE_EQ(right[2], -0.5F, "verified decoder changed stereo data or boundary silence");
  REQUIRE_EQ(left[3], 0.0F, "verified decoder changed stereo data or boundary silence");
  require(!processing::open_verified_managed_source_view(fixture.root / "project", record, 44'100),
          "different source/project rates bypassed conversion gate");
  auto damaged = record;
  damaged.sha256.front() = damaged.sha256.front() == 'a' ? 'b' : 'a';
  require(!processing::open_verified_managed_source_view(fixture.root / "project", damaged, 48'000),
          "altered saved source identity passed verification");
  std::stop_source stop;
  stop.request_stop();
  require(!processing::open_verified_managed_source_view(fixture.root / "project", record, 48'000,
                                                         stop.get_token()),
          "cancelled source open continued");
  std::filesystem::resize_file(fixture.root / "project" / "assets" / (record.sha256 + ".wav"), 44);
  REQUIRE_TRUE(!opened.value()->read(0, left, right),
               "pinned decoder hid a source truncated after verification");
  REQUIRE_EQ(opened.value()->status(), processing::SourceReadStatus::decoder_error,
             "pinned decoder hid a source truncated after verification");
  REQUIRE_EQ(opened.value()->error_code(), ErrorCode::io_failure,
             "pinned decoder hid a source truncated after verification");
  REQUIRE_TRUE(std::ranges::all_of(left, [](float value) { return value == 0.0F; }),
               "pinned decoder hid a source truncated after verification");
}

// A saved 44.1 kHz managed source opens as a verified 48 kHz view whose
// samples match the double-precision converter oracle; at its own rate the
// same source passes through bit-exactly. Rate limits fail before any I/O.
// A view bound to the store's verified descriptor reads exactly the verified
// bytes: moving or replacing the managed path afterwards does not redirect
// it, a truncation of the pinned file is a reported read failure (never
// silence), and mismatched metadata or an external link is refused.
void descriptor_view_reads_only_the_verified_bytes() {
  Fixture fixture;
  std::vector<media::StereoFrame> samples(4'800);
  for (std::size_t index = 0; index < samples.size(); ++index)
    samples[index] = {static_cast<float>(index % 97) / 97.0F, -0.5F};
  const auto wav = fixture.root / "pinned.wav";
  write_wav(wav, samples);
  auto store = take(persistence::ProjectStore::open_writer(fixture.root / "project", true));
  const auto source_id = take(project::SourceId::from_bits({0, 1}));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture descriptor open failed");
  auto receipt = store.stage_wav(source_id, fd);
  (void)::close(fd);
  REQUIRE_OK(receipt, "managed fixture staging failed");
  const auto record = receipt.value().record();
  project::Snapshot snapshot{
      take(project::ProjectId::from_bits({0, 4})),
      1,
      48'000,
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1)),
      take(timeline::MeterMap::create({{{0}, 4, 4}}, 1)),
      {{take(project::TrackId::from_bits({0, 2})),
        {{take(project::ClipId::from_bits({0, 3})),
          source_id,
          {{0}, {4'800}},
          project::ElapsedInterval{{0}, {4'800}}}}}},
      {record}};
  std::array<persistence::SourceReceipt, 1> receipts{std::move(receipt).value()};
  REQUIRE_OK(store.save(snapshot, persistence::SaveKind::explicit_save, receipts),
             "managed fixture project save failed");

  const auto asset = fixture.root / "project" / "assets" / (record.sha256 + ".wav");
  const int verified = take(store.open_managed_source(record));
  auto view = processing::ProjectRateSourceView::from_descriptor(verified, record, 48'000);
  auto mismatched = record;
  mismatched.frames += 1;
  auto refused = processing::ProjectRateSourceView::from_descriptor(verified, mismatched, 48'000);
  auto linked = record;
  linked.external_link = project::ExternalSourceLink{"/elsewhere/pinned.wav"};
  auto external = processing::ProjectRateSourceView::from_descriptor(verified, linked, 48'000);
  (void)::close(verified);
  REQUIRE_OK(view, "verified descriptor did not bind a same-rate view");
  REQUIRE_TRUE(!view.value()->converts(), "verified descriptor did not bind a same-rate view");
  REQUIRE_EQ(view.value()->frames(), 4'800, "verified descriptor did not bind a same-rate view");
  REQUIRE_TRUE(!refused, "descriptor with mismatched metadata was accepted");
  REQUIRE_EQ(refused.error().code, ErrorCode::corrupt_data,
             "descriptor with mismatched metadata was accepted");
  REQUIRE_TRUE(!external, "external link was accepted through a descriptor");
  REQUIRE_EQ(external.error().code, ErrorCode::unsupported,
             "external link was accepted through a descriptor");

  // Move the managed file away and put different bytes at its path.
  const auto moved = fixture.root / "moved.wav";
  std::filesystem::rename(asset, moved);
  std::vector<media::StereoFrame> other(4'800, media::StereoFrame{0.75F, 0.75F});
  write_wav(asset, other);
  std::vector<float> left(4'800), right(4'800);
  require(view.value()->read(0, left, right), "pinned view could not read after a move");
  bool same = true;
  for (std::size_t index = 0; index < samples.size(); ++index)
    same = same && left[index] == samples[index].left && right[index] == samples[index].right;
  require(same, "pinned view followed the replaced path instead of the verified bytes");

  // Truncating the pinned file itself is a reported failure with silence.
  std::filesystem::resize_file(moved, 44);
  std::ranges::fill(left, 1.0F);
  REQUIRE_TRUE(!view.value()->read(0, left, right), "truncated pinned source was not reported");
  REQUIRE_EQ(view.value()->status(), processing::SourceReadStatus::decoder_error,
             "truncated pinned source was not reported");
  REQUIRE_TRUE(std::ranges::all_of(left, [](float value) { return value == 0.0F; }),
               "truncated pinned source was not reported");
}

void project_rate_view_converts_verified_source() {
  Fixture fixture;
  std::vector<media::StereoFrame> samples(4'410);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const double time = static_cast<double>(index) / 44'100.0;
    samples[index] = {static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 997.0 * time)),
                      static_cast<float>(0.25 * std::cos(2.0 * std::numbers::pi * 311.0 * time))};
  }
  const auto wav = fixture.root / "source.wav";
  write_wav(wav, samples, 44'100);
  auto store = take(persistence::ProjectStore::open_writer(fixture.root / "project", true));
  const auto source_id = take(project::SourceId::from_bits({0, 1}));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture descriptor open failed");
  auto receipt = store.stage_wav(source_id, fd);
  (void)::close(fd);
  REQUIRE_OK(receipt, "managed fixture staging failed");
  auto record = receipt.value().record();
  REQUIRE_EQ(record.sample_rate, 44'100, "fixture was not staged at 44.1 kHz");
  project::Snapshot snapshot{
      take(project::ProjectId::from_bits({0, 4})),
      1,
      48'000,
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1)),
      take(timeline::MeterMap::create({{{0}, 4, 4}}, 1)),
      {{take(project::TrackId::from_bits({0, 2})),
        {{take(project::ClipId::from_bits({0, 3})),
          source_id,
          {{0}, {4'410}},
          project::ElapsedInterval{{0}, {4'800}}}}}},
      {record}};
  std::array<persistence::SourceReceipt, 1> receipts{std::move(receipt).value()};
  REQUIRE_OK(store.save(snapshot, persistence::SaveKind::explicit_save, receipts),
             "managed fixture project save failed");

  auto converted =
      take(processing::ProjectRateSourceView::open(fixture.root / "project", record, 48'000));
  REQUIRE_TRUE(converted->converts(), "44.1 kHz source did not open as a 48 kHz view");
  REQUIRE_EQ(converted->frames(), 4'800, "44.1 kHz source did not open as a 48 kHz view");
  std::vector<float> source_left(samples.size());
  std::vector<float> source_right(samples.size());
  for (std::size_t index = 0; index < samples.size(); ++index) {
    source_left[index] = samples[index].left;
    source_right[index] = samples[index].right;
  }
  std::vector<float> left(4'820), right(4'820);
  require(converted->read(-10, left, right), "verified converted read failed");
  double worst = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto frame = static_cast<std::int64_t>(index) - 10;
    worst = std::max({worst,
                      std::fabs(left[index] - oscillate::warp_dsp::reference_rate_converted_sample(
                                                  source_left, 44'100, 48'000, frame)),
                      std::fabs(right[index] - oscillate::warp_dsp::reference_rate_converted_sample(
                                                   source_right, 44'100, 48'000, frame))});
  }
  REQUIRE_LT(worst, 2e-6, "verified converted samples differ from the oracle");

  auto native =
      take(processing::ProjectRateSourceView::open(fixture.root / "project", record, 44'100));
  std::vector<float> native_left(samples.size()), native_right(samples.size());
  REQUIRE_TRUE(!native->converts(), "equal-rate verified view was not bit-exact");
  REQUIRE_EQ(native->frames(), samples.size(), "equal-rate verified view was not bit-exact");
  REQUIRE_TRUE(native->read(0, native_left, native_right),
               "equal-rate verified view was not bit-exact");
  REQUIRE_EQ(native_left, source_left, "equal-rate verified view was not bit-exact");
  REQUIRE_EQ(native_right, source_right, "equal-rate verified view was not bit-exact");

  require(!processing::ProjectRateSourceView::open(fixture.root / "project", record, 32'000),
          "unsupported project rate opened");
  auto unbounded = record;
  unbounded.sample_rate = 44'101;
  auto refused =
      processing::ProjectRateSourceView::open(fixture.root / "project", unbounded, 48'000);
  REQUIRE_TRUE(!refused, "unbounded rate ratio was not refused before verification");
  REQUIRE_EQ(refused.error().code, ErrorCode::unsupported,
             "unbounded rate ratio was not refused before verification");

  // A decoder failure under conversion silences the request and is reported.
  std::filesystem::resize_file(fixture.root / "project" / "assets" / (record.sha256 + ".wav"), 44);
  std::ranges::fill(left, 1.0F);
  REQUIRE_TRUE(!converted->read(0, left, right),
               "converted view hid a source truncated after verification");
  REQUIRE_EQ(converted->status(), processing::SourceReadStatus::decoder_error,
             "converted view hid a source truncated after verification");
  REQUIRE_TRUE(std::ranges::all_of(left, [](float value) { return value == 0.0F; }),
               "converted view hid a source truncated after verification");
}

} // namespace

int main() {
  try {
    bounded_random_reads_and_silence();
    failures_are_reported_and_output_is_silent();
    cancellation_stops_between_chunks();
    exact_managed_identity_and_rate_are_required();
    project_rate_view_converts_verified_source();
    descriptor_view_reads_only_the_verified_bytes();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
