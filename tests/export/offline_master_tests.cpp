#include "insert_fixtures.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"
#include "private.hpp"

#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/native_dsp/true_peak.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/routing/project_route_plan.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#if OSCILLATE_HAVE_CODEC
#include <sndfile.h>
#endif

namespace {
using namespace oscillate;
using namespace oscillate::exporting;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-master") {}
};

class ExportGate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {
  public:
    explicit Lease(ExportGate& gate) : gate_(gate) {}
    ~Lease() override { gate_.held = false; }

  private:
    ExportGate& gate_;
  };

  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    if (playing || held) {
      return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::failure(
          {foundation::ErrorCode::conflict, "Transport cannot be stopped for export"});
    }
    held = true;
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>(*this));
  }

  [[nodiscard]] bool start() noexcept {
    if (held) {
      return false;
    }
    playing = true;
    return true;
  }
  void stop() noexcept { playing = false; }

  bool playing = false;
  bool held = false;
};

class EmptyLeaseGate final : public project::TransportGate {
public:
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(nullptr);
  }
};

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  append_u16(bytes, static_cast<std::uint16_t>(value));
  append_u16(bytes, static_cast<std::uint16_t>(value >> 16U));
}
std::vector<std::uint8_t> fixture_wav() {
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  append_u32(bytes, 36 + 4 * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  append_u32(bytes, 16);
  append_u16(bytes, 1);
  append_u16(bytes, 2);
  append_u32(bytes, 48'000);
  append_u32(bytes, 48'000 * 4);
  append_u16(bytes, 4);
  append_u16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  append_u32(bytes, 4 * 4);
  for (int frame = 0; frame < 4; ++frame) {
    append_u16(bytes, 8'192);  // left = 0.25
    append_u16(bytes, 16'384); // right = 0.5
  }
  return bytes;
}
struct SineFixture {
  std::uint32_t rate;
  std::uint32_t frames;
  double frequency;
  double phase_radians = 0.0;
};
std::vector<std::uint8_t> sine_fixture_wav(SineFixture spec) {
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  append_u32(bytes, 36 + spec.frames * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  append_u32(bytes, 16);
  append_u16(bytes, 1);
  append_u16(bytes, 2);
  append_u32(bytes, spec.rate);
  append_u32(bytes, spec.rate * 4);
  append_u16(bytes, 4);
  append_u16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  append_u32(bytes, spec.frames * 4);
  for (std::uint32_t frame = 0; frame < spec.frames; ++frame) {
    const auto value = static_cast<std::int16_t>(
        std::round(12'000.0 * std::sin(2.0 * std::numbers::pi * spec.frequency * frame / spec.rate +
                                       spec.phase_radians)));
    append_u16(bytes, static_cast<std::uint16_t>(value));
    append_u16(bytes, static_cast<std::uint16_t>(value));
  }
  return bytes;
}
void write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "fixture write failed");
}
std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "output open failed");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(bytes.at(offset)) |
                                    (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8U));
}
std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(u16(bytes, offset)) |
         (static_cast<std::uint32_t>(u16(bytes, offset + 2)) << 16U);
}
double sample(const std::vector<std::uint8_t>& bytes, std::size_t offset, WavFormat format);
// The largest float32 sample, either channel, over `frames` frames from `first`.
double tail_peak(const std::vector<std::uint8_t>& bytes, std::uint64_t first,
                 std::uint64_t frames) {
  double peak = 0.0;
  for (std::uint64_t frame = first; frame < first + frames; ++frame)
    for (const std::size_t channel : {0U, 4U})
      peak =
          std::max(peak, std::abs(sample(bytes, 56 + static_cast<std::size_t>(frame) * 8 + channel,
                                         WavFormat::float32)));
  return peak;
}
double sample(const std::vector<std::uint8_t>& bytes, std::size_t offset, WavFormat format) {
  if (format == WavFormat::float32) {
    const auto bits = u32(bytes, offset);
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  if (format == WavFormat::pcm16) {
    return static_cast<double>(static_cast<std::int16_t>(u16(bytes, offset))) / 32'768.0;
  }
  const auto bits = static_cast<std::uint32_t>(bytes.at(offset)) |
                    (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8U) |
                    (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16U);
  const auto signed_bits =
      static_cast<std::int32_t>(bits) - ((bits & 0x80'00'00U) != 0 ? 0x1'00'00'00 : 0);
  return static_cast<double>(signed_bits) / 8'388'608.0;
}

project::Snapshot make_snapshot(project::SourceRecord source) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  constexpr std::int64_t ticks_per_sample = timeline::ticks_per_quarter / 24'000;
  project::Clip first{
      id<project::ClipId>(10),
      source.id,
      {{0}, {4}},
      project::MusicalInterval{{timeline::ticks_per_quarter},
                               {timeline::ticks_per_quarter + 4 * ticks_per_sample}},
      1};
  project::Clip second{id<project::ClipId>(11),
                       source.id,
                       {{0}, {4}},
                       project::ElapsedInterval{{24'002}, {24'006}},
                       1};
  return {id<project::ProjectId>(1),
          7,
          48'000,
          std::move(tempo),
          std::move(meter),
          {{id<project::TrackId>(2), {first}}, {id<project::TrackId>(3), {second}}},
          {std::move(source)}};
}

void clip_fades_use_the_same_sample_boundaries_in_master_export() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "fade-source.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fade fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "fade fixture close failed");
  auto receipt = take(std::move(staged));
  auto state = make_snapshot(receipt.record());
  for (auto& track : state.tracks)
    track.clips.front().fades = {0.5, 0.5, project::FadeCurve::linear, project::FadeCurve::linear};
  REQUIRE_EQ(take(store.save(state, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "faded project save failed");
  state = take(store.load_current()).snapshot;
  const LiveRevisionState active{state.revision, state.revision, RevisionApplication::active, {}};
  const auto output = directory.path() / "faded-master.wav";
  REQUIRE_EQ(take(render_wav_master(state, store, active, {output, WavFormat::float32})),
             PublishOutcome::durable, "faded master did not render");
  const auto bytes = read_bytes(output);
  for (std::size_t index = 0; index < 6; ++index) {
    const auto gain = index == 0 || index == 5 ? 0.0 : 0.5;
    const auto frame = 24'000 + index;
    REQUIRE_LT(std::abs(sample(bytes, 56 + frame * 8, WavFormat::float32) - 0.25 * gain), 1e-7,
               "master fade samples disagree with clip boundaries");
    REQUIRE_LT(std::abs(sample(bytes, 60 + frame * 8, WavFormat::float32) - 0.5 * gain), 1e-7,
               "master fade samples disagree with clip boundaries");
  }
}

void test_mix_formats_and_guards() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture open failed");
  auto receipt_result = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "fixture close failed");
  auto receipt = take(std::move(receipt_result));
  auto snapshot = make_snapshot(receipt.record());
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "project save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto output = directory.path() / "master.wav";
  for (auto format : {WavFormat::float32, WavFormat::pcm16, WavFormat::pcm24}) {
    REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {output, format, 123})),
               PublishOutcome::durable, "render not durable");
    const auto bytes = read_bytes(output);
    const auto width = format == WavFormat::pcm16 ? 2U : format == WavFormat::pcm24 ? 3U : 4U;
    const auto data_offset = format == WavFormat::float32 ? 56U : 44U;
    const auto data_size_offset = data_offset - 4U;
    REQUIRE_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF",
               "independent WAV header parse mismatch");
    REQUIRE_EQ(std::string(bytes.begin() + 8, bytes.begin() + 12), "WAVE",
               "independent WAV header parse mismatch");
    REQUIRE_EQ(u32(bytes, 24), 48'000, "independent WAV header parse mismatch");
    REQUIRE_EQ(u16(bytes, 22), 2, "independent WAV header parse mismatch");
    REQUIRE_EQ(u16(bytes, 20), (format == WavFormat::float32 ? 3 : 1),
               "independent WAV header parse mismatch");
    REQUIRE_EQ(u16(bytes, 34), width * 8, "independent WAV header parse mismatch");
    REQUIRE_EQ(u32(bytes, data_size_offset), 24'006 * 2 * width,
               "independent WAV header parse mismatch");
    REQUIRE_EQ(bytes.size(), data_offset + 24'006 * 2 * width,
               "independent WAV header parse mismatch");
    for (std::size_t frame = 0; frame < 24'006; ++frame) {
      const auto gain = frame < 24'000 ? 0.0 : frame >= 24'002 && frame < 24'004 ? 2.0 : 1.0;
      const auto left = sample(bytes, data_offset + frame * 2 * width, format);
      const auto right = sample(bytes, data_offset + (frame * 2 + 1) * width, format);
      const auto tolerance = format == WavFormat::pcm16 ? 2.0 / 32'768.0 : 2.0 / 8'388'608.0;
      REQUIRE_NEAR(left, 0.25 * gain, tolerance, "clip placement or summed samples differ");
      REQUIRE_NEAR(right, 0.5 * gain, tolerance, "clip placement or summed samples differ");
    }
  }
  const auto previous = read_bytes(output);
#if OSCILLATE_HAVE_CODEC
  for (const auto encoded_format :
       {EncodedMasterFormat::flac16, EncodedMasterFormat::flac24, EncodedMasterFormat::mp3}) {
    const auto encoded_output =
        directory.path() /
        (encoded_format == EncodedMasterFormat::mp3 ? "master.mp3" : "master.flac");
    const EncodedMasterOptions encoded_options{encoded_output, encoded_format, 123, 0.5};
    REQUIRE_EQ(take(render_encoded_master(snapshot, store, active, encoded_options)),
               PublishOutcome::durable, "encoded master render failed");
    const auto original = read_bytes(encoded_output);
    SF_INFO info{};
    auto* decoded = sf_open(encoded_output.c_str(), SFM_READ, &info);
    REQUIRE_NE(decoded, nullptr, "encoded master metadata mismatch");
    REQUIRE_EQ(info.channels, 2, "encoded master metadata mismatch");
    REQUIRE_EQ(info.samplerate, 48'000, "encoded master metadata mismatch");
    REQUIRE_EQ(info.frames, 24'006, "encoded master metadata mismatch");
    REQUIRE_EQ((info.format & SF_FORMAT_TYPEMASK),
               (encoded_format == EncodedMasterFormat::mp3 ? SF_FORMAT_MPEG : SF_FORMAT_FLAC),
               "encoded master metadata mismatch");
    std::vector<float> samples(std::size_t{24'006} * 2);
    REQUIRE_EQ(sf_readf_float(decoded, samples.data(), 24'006), 24'006,
               "encoded master decode failed");
    REQUIRE_EQ(sf_close(decoded), 0, "encoded master decode failed");
    if (encoded_format != EncodedMasterFormat::mp3) {
      const auto tolerance =
          encoded_format == EncodedMasterFormat::flac16 ? 2.0 / 32'768.0 : 2.0 / 8'388'608.0;
      for (std::size_t frame = 0; frame < 24'006; ++frame) {
        const auto gain = frame < 24'000 ? 0.0 : frame >= 24'002 && frame < 24'004 ? 2.0 : 1.0;
        REQUIRE_NEAR(samples[frame * 2], 0.25 * gain, tolerance,
                     "FLAC master arrangement or dither differs");
        REQUIRE_NEAR(samples[frame * 2 + 1], 0.5 * gain, tolerance,
                     "FLAC master arrangement or dither differs");
      }
    }
    REQUIRE_EQ(take(render_encoded_master(snapshot, store, active, encoded_options)),
               PublishOutcome::durable, "encoded repeat render differs");
    REQUIRE_EQ(read_bytes(encoded_output), original, "encoded repeat render differs");
    auto stale_encoded = active;
    --stale_encoded.active_graph_revision;
    REQUIRE_TRUE(!render_encoded_master(snapshot, store, stale_encoded, encoded_options),
                 "stale encoded master replaced target");
    REQUIRE_EQ(read_bytes(encoded_output), original, "stale encoded master replaced target");
    auto missing = snapshot;
    missing.sources[0].id = id<project::SourceId>(99);
    REQUIRE_TRUE(!render_encoded_master(missing, store, active, encoded_options),
                 "unresolved encoded master replaced target");
    REQUIRE_EQ(read_bytes(encoded_output), original, "unresolved encoded master replaced target");
  }
#endif
  for (const auto status : {RevisionApplication::pending, RevisionApplication::failed}) {
    auto guarded = active;
    guarded.application = status;
    require(!render_wav_master(snapshot, store, guarded, {output}), "non-active revision rendered");
    REQUIRE_EQ(read_bytes(output), previous, "guard changed existing target");
  }
  auto stale = active;
  stale.active_graph_revision -= 1;
  require(!render_wav_master(snapshot, store, stale, {output}), "stale graph rendered");
  auto changed = snapshot;
  changed.tracks[0].kind = project::TrackKind::instrument;
  require(!render_wav_master(changed, store, active, {output}), "instrument silently omitted");
  changed = snapshot;
  changed.tracks[0].clips[0].source_range.end.value = 3;
  // A placement longer than its source range is a stretched clip: it renders
  // through the warp path (routed_warp exports are covered in
  // warp_export_tests.cpp) and never replaces another target.
  const auto stretched = directory.path() / "stretched-master.wav";
  REQUIRE_EQ(take(render_wav_master(changed, store, active, {stretched})), PublishOutcome::durable,
             "stretched clip did not render through the warp path");
  REQUIRE_EQ(read_bytes(output), previous, "stretched clip did not render through the warp path");
  changed = snapshot;
  changed.tracks[0].clips[0].source_range = {{1}, {5}};
  require(!render_wav_master(changed, store, active, {output}),
          "source range beyond media rendered");
  changed = snapshot;
  changed.tracks[0].clips[0].source = id<project::SourceId>(99);
  require(!render_wav_master(changed, store, active, {output}), "unresolved source rendered");
  changed = snapshot;
  changed.sources[0].sample_rate = oscillate::test::sample_rate_44k;
  require(!render_wav_master(changed, store, active, {output}), "rate-mismatched source rendered");
  changed = snapshot;
  changed.tracks[1].clips[0].placement = project::ElapsedInterval{{-1}, {3}};
  require(!render_wav_master(changed, store, active, {output}), "negative placement rendered");
  changed = snapshot;
  changed.tracks[0].clips.clear();
  changed.tracks[1].clips.clear();
  require(!render_wav_master(changed, store, active, {output}), "empty master rendered");
  REQUIRE_EQ(read_bytes(output), previous, "unsupported content changed existing target");

  auto convolution = snapshot;
  const auto& ir_source = convolution.sources[0];
  const project::ManagedAudioReference ir{ir_source.id,          ir_source.sha256,
                                          ir_source.byte_length, ir_source.sample_rate,
                                          ir_source.channels,    ir_source.frames};
  convolution.master_effects.push_back(
      {id<project::InsertSlotId>(900), false, project::NativeConvolution{ir, 0.4, 1}});
#if OSCILLATE_HAVE_CONVOLUTION
  require(take(render_wav_master(convolution, store, active, {output, WavFormat::pcm24, 123})) ==
                  PublishOutcome::durable &&
              read_bytes(output) == previous,
          "bypassed convolution changed the master render");
  const auto stems = directory.path() / "convolution-stems";
  REQUIRE_EQ(take(render_wav_stems(convolution, store, active, {stems})), PublishOutcome::durable,
             "bypassed convolution blocked stem export");
#else
  const auto unavailable = render_wav_master(convolution, store, active, {output});
  REQUIRE_TRUE(!unavailable, "bypassed convolution was silently omitted from master export");
  REQUIRE_EQ(unavailable.error().code, foundation::ErrorCode::unsupported,
             "bypassed convolution was silently omitted from master export");
  REQUIRE_NE(unavailable.error().message.find("convolution"), std::string::npos,
             "bypassed convolution was silently omitted from master export");
  REQUIRE_EQ(read_bytes(output), previous,
             "bypassed convolution was silently omitted from master export");
  const auto stems = directory.path() / "convolution-stems";
  REQUIRE_TRUE(!render_wav_stems(convolution, store, active, {stems}),
               "convolution was silently omitted from stem export");
  REQUIRE_TRUE(!fs::exists(stems), "convolution was silently omitted from stem export");
#endif

  const auto asset = directory.path() / "project" / "assets" / (receipt.record().sha256 + ".wav");
  auto with_eq = snapshot;
  test_insert::set_master(
      with_eq, test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}));
  auto interrupted =
      detail::render_wav_master_with_start_hook(with_eq, store, active, {output}, [&asset]() {
        REQUIRE_EQ(::truncate(asset.c_str(), 44), 0, "fault injection truncate failed");
      });
  REQUIRE_TRUE(!interrupted, "source disappearing after writer begin did not fail");
  REQUIRE_EQ(interrupted.error().code, foundation::ErrorCode::io_failure,
             "source disappearing after writer begin did not fail");
  REQUIRE_EQ(read_bytes(output), previous, "late read failure changed existing target");

  fs::remove(asset);
  require(!render_wav_master(snapshot, store, active, {output}), "missing asset rendered");
  REQUIRE_EQ(read_bytes(output), previous, "missing asset changed existing target");
}

#if OSCILLATE_HAVE_CODEC
void test_encoded_master(std::string_view name, project::SourceEncoding encoding) {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto fixture = fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / name;
  const int fd = ::open(fixture.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "encoded fixture open failed");
  auto staging = store.begin_encoded_stage(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "encoded fixture close failed");
  auto task = take(std::move(staging));
  auto staged = take(std::move(task).run());
  auto receipt = take(store.publish_encoded_stage(std::move(staged)));
  REQUIRE_EQ(receipt.record().encoding, encoding, "encoded fixture metadata mismatch");
  REQUIRE_EQ(receipt.record().sample_rate, 48'000, "encoded fixture metadata mismatch");
  REQUIRE_EQ(receipt.record().channels, 1, "encoded fixture metadata mismatch");
  REQUIRE_GE(receipt.record().frames, 960, "encoded fixture metadata mismatch");
  auto snapshot = make_snapshot(receipt.record());
  snapshot.tracks[0].clips[0].source_range.end.value = 960;
  snapshot.tracks[0].clips[0].placement = project::ElapsedInterval{{0}, {960}};
  snapshot.tracks[1].clips[0].source_range.end.value = 960;
  snapshot.tracks[1].clips[0].placement = project::ElapsedInterval{{480}, {1'440}};
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "encoded project save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto output = directory.path() / "encoded-master.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {output})), PublishOutcome::durable,
             "encoded master render failed");
  const auto bytes = read_bytes(output);
  REQUIRE_EQ(u32(bytes, 52), 1'440 * 8, "encoded master length differs from clip arrangement");
  REQUIRE_EQ(bytes.size(), 56 + 1'440 * 8, "encoded master length differs from clip arrangement");
  const int pinned = take(store.open_managed_source(receipt.record()));
  auto source = media::AudioReader::open_fd(pinned, receipt.record());
  REQUIRE_EQ(::close(pinned), 0, "encoded managed descriptor close failed");
  auto reader = take(std::move(source));
  std::array<media::StereoFrame, 960> decoded{};
  REQUIRE_EQ(take(reader.read_at(0, decoded)), decoded.size(), "encoded oracle read failed");
  for (std::size_t frame = 0; frame < 1'440; ++frame) {
    const double first = frame < 960 ? decoded[frame].left : 0.0;
    const double second = frame >= 480 ? decoded[frame - 480].left : 0.0;
    const double expected = first + second;
    REQUIRE_LT(std::abs(sample(bytes, 56 + frame * 8, WavFormat::float32) - expected), 1e-7,
               "encoded master sample, placement or mono duplication mismatch");
    REQUIRE_LT(std::abs(sample(bytes, 60 + frame * 8, WavFormat::float32) - expected), 1e-7,
               "encoded master sample, placement or mono duplication mismatch");
  }
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {output})), PublishOutcome::durable,
             "encoded repeat render differs");
  REQUIRE_EQ(read_bytes(output), bytes, "encoded repeat render differs");
  auto stale = active;
  --stale.active_graph_revision;
  REQUIRE_TRUE(!render_wav_master(snapshot, store, stale, {output}),
               "stale encoded revision replaced output");
  REQUIRE_EQ(read_bytes(output), bytes, "stale encoded revision replaced output");
  const auto extension = name.substr(name.find_last_of('.'));
  const auto asset =
      directory.path() / "project" / "assets" / (receipt.record().sha256 + std::string(extension));
  {
    std::fstream changed(asset, std::ios::binary | std::ios::in | std::ios::out);
    require(changed.good(), "encoded managed asset tamper open failed");
    changed.seekp(100);
    changed.put('\x7f');
    require(changed.good(), "encoded managed asset tamper failed");
  }
  REQUIRE_TRUE(!render_wav_master(snapshot, store, active, {output}),
               "corrupt encoded asset replaced output");
  REQUIRE_EQ(read_bytes(output), bytes, "corrupt encoded asset replaced output");
  require(fs::remove(asset), "encoded managed asset removal failed");
  REQUIRE_TRUE(!render_wav_master(snapshot, store, active, {output}),
               "missing encoded asset replaced output");
  REQUIRE_EQ(read_bytes(output), bytes, "missing encoded asset replaced output");
}
#else
void test_encoded_master_unavailable() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "WAV fixture open failed");
  auto receipt_result = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "WAV fixture close failed");
  auto receipt = take(std::move(receipt_result));
  auto snapshot = make_snapshot(receipt.record());
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "WAV project save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto output = directory.path() / "master.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {output})), PublishOutcome::durable,
             "WAV master render failed without codec adapter");
  const auto bytes = read_bytes(output);
  snapshot.sources[0].encoding = project::SourceEncoding::flac;
  const auto rejected = render_wav_master(snapshot, store, active, {output});
  REQUIRE_TRUE(!rejected, "codec-off encoded source replaced WAV master");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unsupported,
             "codec-off encoded source replaced WAV master");
  REQUIRE_EQ(read_bytes(output), bytes, "codec-off encoded source replaced WAV master");
  const auto encoded_output = directory.path() / "master.flac";
  const auto no_encoder = render_encoded_master(
      snapshot, store, active, {encoded_output, EncodedMasterFormat::flac24, 123, 0.5});
  REQUIRE_TRUE(!no_encoder, "codec-off encoded export unexpectedly published");
  REQUIRE_EQ(no_encoder.error().code, foundation::ErrorCode::unsupported,
             "codec-off encoded export unexpectedly published");
  REQUIRE_TRUE(!fs::exists(encoded_output), "codec-off encoded export unexpectedly published");
}
#endif

void test_stopped_export_authority() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "stopped export fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "stopped export fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "stopped export fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  const auto output = directory.path() / "master.wav";
  ExportGate gate;
  EmptyLeaseGate empty_gate;
  require(!StoppedExportAuthority::acquire(snapshot, empty_gate),
          "transport gate returned an accepted empty lease");

  require(gate.start(), "fixture transport did not start");
  auto while_playing = StoppedExportAuthority::acquire(snapshot, gate);
  REQUIRE_TRUE(!while_playing, "stopped authority acquired during playback");
  REQUIRE_EQ(while_playing.error().code, foundation::ErrorCode::conflict,
             "stopped authority acquired during playback");
  gate.stop();

  auto authority = take(StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_TRUE(gate.held, "stopped lease did not prevent playback start");
  REQUIRE_TRUE(!gate.start(), "stopped lease did not prevent playback start");
  REQUIRE_EQ(take(detail::render_wav_master_with_start_hook(
                 snapshot, store, std::move(authority), {output},
                 [&gate]() {
                   REQUIRE_TRUE(gate.held, "stopped lease expired during render");
                   REQUIRE_TRUE(!gate.start(), "stopped lease expired during render");
                 })),
             PublishOutcome::durable, "stopped project did not export");
  REQUIRE_TRUE(!gate.held, "successful render retained its stopped lease");
  REQUIRE_TRUE(!gate.playing, "successful render retained its stopped lease");
  const auto previous = read_bytes(output);
  REQUIRE_GT(previous.size(), 44, "stopped export did not publish a WAV");
  REQUIRE_EQ(std::string(previous.begin(), previous.begin() + 4), "RIFF",
             "stopped export did not publish a WAV");
  auto changed = snapshot;
  ++changed.revision;
  auto stale = take(StoppedExportAuthority::acquire(snapshot, gate));
  auto stale_result = render_wav_master(changed, store, std::move(stale), {output});
  REQUIRE_TRUE(!stale_result, "stale stopped authority replaced the master");
  REQUIRE_EQ(stale_result.error().code, foundation::ErrorCode::conflict,
             "stale stopped authority replaced the master");
  REQUIRE_TRUE(!gate.held, "stale stopped authority replaced the master");
  REQUIRE_EQ(read_bytes(output), previous, "stale stopped authority replaced the master");

  changed = snapshot;
  changed.id = id<project::ProjectId>(99);
  auto wrong_project = take(StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_TRUE(!render_wav_master(changed, store, std::move(wrong_project), {output}),
               "stopped authority accepted a different project ID");
  REQUIRE_TRUE(!gate.held, "stopped authority accepted a different project ID");
  REQUIRE_EQ(read_bytes(output), previous, "stopped authority accepted a different project ID");

  changed = snapshot;
  changed.tracks[0].kind = project::TrackKind::instrument;
  auto unsupported = take(StoppedExportAuthority::acquire(changed, gate));
  REQUIRE_TRUE(!render_wav_master(changed, store, std::move(unsupported), {output}),
               "stopped authority bypassed unsupported-project preflight");
  REQUIRE_TRUE(!gate.held, "stopped authority bypassed unsupported-project preflight");
  REQUIRE_EQ(read_bytes(output), previous,
             "stopped authority bypassed unsupported-project preflight");

  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  auto active_stale = active;
  --active_stale.active_graph_revision;
  REQUIRE_TRUE(!render_wav_master(snapshot, store, active_stale, {output}),
               "stopped authority weakened the active-graph revision guard");
  REQUIRE_EQ(read_bytes(output), previous,
             "stopped authority weakened the active-graph revision guard");

#if OSCILLATE_HAVE_CODEC
  const auto encoded_output = directory.path() / "stopped.flac";
  const EncodedMasterOptions encoded_options{encoded_output, EncodedMasterFormat::flac24, 123, 0.5};
  auto encoded_authority = take(StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_TRUE(gate.held, "stopped encoded lease did not block playback");
  REQUIRE_TRUE(!gate.start(), "stopped encoded lease did not block playback");
  REQUIRE_EQ(
      take(render_encoded_master(snapshot, store, std::move(encoded_authority), encoded_options)),
      PublishOutcome::durable, "stopped encoded export did not publish");
  require(!gate.held, "stopped encoded export retained transport lease");
  const auto encoded_previous = read_bytes(encoded_output);
  auto encoded_stale = take(StoppedExportAuthority::acquire(snapshot, gate));
  changed = snapshot;
  ++changed.revision;
  auto stale_encoded_result =
      render_encoded_master(changed, store, std::move(encoded_stale), encoded_options);
  require(!stale_encoded_result, "stale stopped encoded export was accepted");
  require(!gate.held, "stale encoded export retained transport lease");
  REQUIRE_EQ(read_bytes(encoded_output), encoded_previous,
             "stale stopped encoded export replaced output");
#endif

  const auto asset = directory.path() / "project" / "assets" / (receipt.record().sha256 + ".wav");
  auto interrupted = take(StoppedExportAuthority::acquire(snapshot, gate));
  auto failed = detail::render_wav_master_with_start_hook(
      snapshot, store, std::move(interrupted), {output}, [&gate, &asset]() {
        REQUIRE_TRUE(gate.held, "stopped lease expired before late I/O failure");
        REQUIRE_TRUE(!gate.start(), "stopped lease expired before late I/O failure");
        REQUIRE_EQ(::truncate(asset.c_str(), 44), 0, "late source truncation failed");
      });
  REQUIRE_TRUE(!failed, "late stopped export failure changed existing master or retained lease");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::io_failure,
             "late stopped export failure changed existing master or retained lease");
  REQUIRE_TRUE(!gate.held, "late stopped export failure changed existing master or retained lease");
  REQUIRE_EQ(read_bytes(output), previous,
             "late stopped export failure changed existing master or retained lease");
}

void test_saved_native_sidechain_master_and_stems() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "sidechain fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "sidechain fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  const auto slot = test_insert::next_slot(snapshot);
  native_dsp::CompressorSettings settings;
  settings.threshold_db = -40.0;
  settings.ratio = 20.0;
  settings.knee_db = 0.0;
  settings.attack_ms = 0.1;
  settings.release_ms = 100.0;
  snapshot.tracks[0].effects.push_back({slot, true, test_insert::press(settings)});
  snapshot.tracks[1].sends.emplace_back(id<project::SendId>(90), snapshot.tracks[0].id, 1.0,
                                        project::SendPurpose::sidechain, slot);
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "sidechain project save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto target = directory.path() / "sidechain-master.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {target})), PublishOutcome::durable,
             "saved sidechain master did not render");
  const auto master = read_bytes(target);
  const auto prepared = take(native_dsp::prepare_compressor(settings, snapshot.sample_rate));
  std::array<engine::StereoFrame, 8> processed{}, detector{};
  for (std::size_t frame = 0; frame < 4; ++frame)
    processed[frame] = {0.25F, 0.5F};
  for (std::size_t frame = 2; frame < 6; ++frame)
    detector[frame] = {0.25F, 0.5F};
  native_dsp::CompressorRuntime oracle(prepared);
  oracle.process_with_sidechain(processed, detector, {24'000, 1});
  for (std::size_t frame = 0; frame < 6; ++frame) {
    const auto offset = 56 + (24'000 + frame) * 8;
    const auto expected_left = processed[frame].left + detector[frame].left;
    const auto expected_right = processed[frame].right + detector[frame].right;
    REQUIRE_LT(std::abs(sample(master, offset, WavFormat::float32) - expected_left), 1e-7,
               "saved sidechain master differs from independent compressor detector oracle");
    REQUIRE_LT(std::abs(sample(master, offset + 4, WavFormat::float32) - expected_right), 1e-7,
               "saved sidechain master differs from independent compressor detector oracle");
  }
  REQUIRE_LT(processed[3].left, 0.25F, "external detector did not compress the target source");
  const auto repeat = directory.path() / "sidechain-repeat.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {repeat})), PublishOutcome::durable,
             "sidechain master changed across repeat renders");
  REQUIRE_EQ(read_bytes(repeat), master, "sidechain master changed across repeat renders");
  const auto stems = directory.path() / "sidechain-stems";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active, {stems})), PublishOutcome::durable,
             "direct external sidechain stems did not render");
  const auto target_stem =
      read_bytes(stems / ("track-" + snapshot.tracks[0].id.to_string() + ".wav"));
  const auto detector_stem =
      read_bytes(stems / ("track-" + snapshot.tracks[1].id.to_string() + ".wav"));
  for (std::size_t frame = 0; frame < 6; ++frame) {
    const auto offset = 56 + (24'000 + frame) * 8;
    REQUIRE_LT(std::abs(sample(target_stem, offset, WavFormat::float32) - processed[frame].left),
               1e-7, "target stem lost its full detector or leaked detector audio");
    REQUIRE_LT(
        std::abs(sample(target_stem, offset + 4, WavFormat::float32) - processed[frame].right),
        1e-7, "target stem lost its full detector or leaked detector audio");
    REQUIRE_LT(std::abs(sample(detector_stem, offset, WavFormat::float32) - detector[frame].left),
               1e-7, "detector stem included the compressed target");
    REQUIRE_LT(
        std::abs(sample(detector_stem, offset + 4, WavFormat::float32) - detector[frame].right),
        1e-7, "detector stem included the compressed target");
  }
  const auto repeated_stems = directory.path() / "sidechain-stems-repeat";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active, {repeated_stems})),
             PublishOutcome::durable, "sidechain stem changed across repeat renders");
  REQUIRE_EQ(read_bytes(repeated_stems / ("track-" + snapshot.tracks[0].id.to_string() + ".wav")),
             target_stem, "sidechain stem changed across repeat renders");
  auto delayed_detector = snapshot;
  ++delayed_detector.revision;
  delayed_detector.tracks[1].effects.push_back(
      {test_insert::next_slot(delayed_detector), true, test_insert::ceiling({0.0, 1.0, 100.0})});
  const LiveRevisionState delayed_active{
      delayed_detector.revision, delayed_detector.revision, RevisionApplication::active, {}};
  const auto delayed_stems = directory.path() / "delayed-sidechain-stems";
  const auto delayed_master_path = directory.path() / "delayed-sidechain-master.wav";
  REQUIRE_EQ(
      take(render_wav_master(delayed_detector, store, delayed_active, {delayed_master_path})),
      PublishOutcome::durable, "detector latency shifted the offline master mix");
  REQUIRE_EQ(read_bytes(delayed_master_path), master,
             "detector latency shifted the offline master mix");
  REQUIRE_EQ(take(render_wav_stems(delayed_detector, store, delayed_active, {delayed_stems})),
             PublishOutcome::durable, "compensated direct detector stem did not render");
  const auto delayed_target =
      read_bytes(delayed_stems / ("track-" + snapshot.tracks[0].id.to_string() + ".wav"));
  REQUIRE_EQ(delayed_target, target_stem,
             "detector limiter latency shifted the sidechain relative to the stem source");
  auto tailed_target = snapshot;
  ++tailed_target.revision;
  tailed_target.tracks[0].effects.push_back(
      {test_insert::next_slot(tailed_target), true, test_insert::expanse(0.2, 1.0)});
  const LiveRevisionState tailed_active{
      tailed_target.revision, tailed_target.revision, RevisionApplication::active, {}};
  const auto tailed_stems = directory.path() / "tailed-sidechain-stems";
  REQUIRE_EQ(take(render_wav_stems(tailed_target, store, tailed_active, {tailed_stems})),
             PublishOutcome::durable, "sidechain target reverb stems did not render");
  const auto tailed_target_bytes =
      read_bytes(tailed_stems / ("track-" + snapshot.tracks[0].id.to_string() + ".wav"));
  const auto tailed_detector_bytes =
      read_bytes(tailed_stems / ("track-" + snapshot.tracks[1].id.to_string() + ".wav"));
  const auto reverb_tail =
      test_insert::tail_frames(test_insert::expanse(0.2, 1.0), snapshot.sample_rate);
  REQUIRE_EQ(u32(tailed_target_bytes, 52) / 8, 24'006 + reverb_tail,
             "sidechain target tail was truncated or leaked into the detector stem");
  REQUIRE_EQ(u32(tailed_detector_bytes, 52) / 8, 24'006 + reverb_tail,
             "sidechain target tail was truncated or leaked into the detector stem");
  REQUIRE_GT(tail_peak(tailed_target_bytes, 24'006, reverb_tail), 0.0,
             "sidechain target tail was truncated or leaked into the detector stem");
  REQUIRE_EQ(tail_peak(tailed_detector_bytes, 24'006, reverb_tail), 0.0,
             "sidechain target tail was truncated or leaked into the detector stem");
  const auto interrupted_stems = directory.path() / "sidechain-stems-interrupted";
  auto failed = detail::render_wav_stems_with_after_stem_hook(
      snapshot, store, active, {interrupted_stems}, [](std::size_t) {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::io_failure, "Injected stem failure"});
      });
  REQUIRE_TRUE(!failed, "failed sidechain stem export published a partial bundle");
  REQUIRE_TRUE(!fs::exists(interrupted_stems),
               "failed sidechain stem export published a partial bundle");
  for (const auto kind : {project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    auto dedicated_bus = snapshot;
    ++dedicated_bus.revision;
    const auto bus = id<project::TrackId>(kind == project::TrackKind::group_bus ? 98 : 99);
    dedicated_bus.tracks.emplace_back(bus, std::vector<project::Clip>{}, kind);
    dedicated_bus.tracks[1].sends.clear();
    dedicated_bus.tracks[1].sends.emplace_back(id<project::SendId>(91), bus, 1.0);
    dedicated_bus.tracks.back().sends.emplace_back(id<project::SendId>(92),
                                                   dedicated_bus.tracks[0].id, 1.0,
                                                   project::SendPurpose::sidechain, slot);
    const LiveRevisionState bus_active{
        dedicated_bus.revision, dedicated_bus.revision, RevisionApplication::active, {}};
    const auto bus_stems =
        directory.path() /
        (kind == project::TrackKind::group_bus ? "group-detector-stems" : "return-detector-stems");
    REQUIRE_EQ(take(render_wav_stems(dedicated_bus, store, bus_active, {bus_stems})),
               PublishOutcome::durable, "dedicated bus detector stems did not render");
    const auto bus_target =
        read_bytes(bus_stems / ("track-" + dedicated_bus.tracks[0].id.to_string() + ".wav"));
    const auto bus_source =
        read_bytes(bus_stems / ("track-" + dedicated_bus.tracks[1].id.to_string() + ".wav"));
    REQUIRE_EQ(bus_target, target_stem,
               "dedicated bus detector changed compression or leaked audible bus input");
    REQUIRE_EQ(bus_source, detector_stem,
               "dedicated bus detector changed compression or leaked audible bus input");
    const auto repeated_bus_stems =
        directory.path() / (kind == project::TrackKind::group_bus ? "group-detector-stems-repeat"
                                                                  : "return-detector-stems-repeat");
    REQUIRE_EQ(take(render_wav_stems(dedicated_bus, store, bus_active, {repeated_bus_stems})),
               PublishOutcome::durable,
               "dedicated bus detector stem changed across repeat renders");
    REQUIRE_EQ(read_bytes(repeated_bus_stems /
                          ("track-" + dedicated_bus.tracks[0].id.to_string() + ".wav")),
               bus_target, "dedicated bus detector stem changed across repeat renders");
    if (kind == project::TrackKind::group_bus) {
      auto delayed_bus = dedicated_bus;
      ++delayed_bus.revision;
      delayed_bus.tracks.back().effects.push_back(
          {test_insert::next_slot(delayed_bus), true, test_insert::ceiling({0.0, 1.0, 100.0})});
      const LiveRevisionState delayed_bus_active{
          delayed_bus.revision, delayed_bus.revision, RevisionApplication::active, {}};
      const auto delayed_bus_stems = directory.path() / "delayed-bus-detector-stems";
      REQUIRE_EQ(
          take(render_wav_stems(delayed_bus, store, delayed_bus_active, {delayed_bus_stems})),
          PublishOutcome::durable,
          "detector bus limiter shifted compression relative to the stem source");
      REQUIRE_EQ(read_bytes(delayed_bus_stems /
                            ("track-" + delayed_bus.tracks[0].id.to_string() + ".wav")),
                 bus_target,
                 "detector bus limiter shifted compression relative to the stem source");
      auto tailed_bus = dedicated_bus;
      ++tailed_bus.revision;
      tailed_bus.tracks[0].effects.push_back(
          {test_insert::next_slot(tailed_bus), true, test_insert::expanse(0.2, 1.0)});
      const LiveRevisionState tailed_bus_active{
          tailed_bus.revision, tailed_bus.revision, RevisionApplication::active, {}};
      const auto tailed_bus_stems = directory.path() / "tailed-bus-detector-stems";
      REQUIRE_EQ(take(render_wav_stems(tailed_bus, store, tailed_bus_active, {tailed_bus_stems})),
                 PublishOutcome::durable, "detector bus with target reverb tails did not render");
      const auto bus_target_tail =
          read_bytes(tailed_bus_stems / ("track-" + tailed_bus.tracks[0].id.to_string() + ".wav"));
      const auto bus_source_tail =
          read_bytes(tailed_bus_stems / ("track-" + tailed_bus.tracks[1].id.to_string() + ".wav"));
      REQUIRE_EQ(bus_target_tail, tailed_target_bytes,
                 "detector bus dropped a target tail or leaked it into a foreign stem");
      REQUIRE_EQ(bus_source_tail, tailed_detector_bytes,
                 "detector bus dropped a target tail or leaked it into a foreign stem");
    }
    const auto failed_bus_stems = directory.path() / (kind == project::TrackKind::group_bus
                                                          ? "group-detector-stems-interrupted"
                                                          : "return-detector-stems-interrupted");
    const auto failed_bus = detail::render_wav_stems_with_after_stem_hook(
        dedicated_bus, store, bus_active, {failed_bus_stems}, [](std::size_t) {
          return foundation::Result<void>::failure(
              {foundation::ErrorCode::io_failure, "Injected bus stem failure"});
        });
    REQUIRE_TRUE(!failed_bus, "failed bus detector stem export published a partial bundle");
    REQUIRE_TRUE(!fs::exists(failed_bus_stems),
                 "failed bus detector stem export published a partial bundle");
  }
  auto bus_detector = snapshot;
  ++bus_detector.revision;
  const auto bus = id<project::TrackId>(98);
  bus_detector.tracks.emplace_back(bus, std::vector<project::Clip>{},
                                   project::TrackKind::group_bus);
  bus_detector.tracks[1].sends.clear();
  bus_detector.tracks[1].main_output = bus;
  bus_detector.tracks.back().sends.emplace_back(id<project::SendId>(91), bus_detector.tracks[0].id,
                                                1.0, project::SendPurpose::sidechain, slot);
  const LiveRevisionState bus_active{
      bus_detector.revision, bus_detector.revision, RevisionApplication::active, {}};
  const auto unsupported_stems = directory.path() / "bus-detector-stems";
  const auto unsupported = render_wav_stems(bus_detector, store, bus_active, {unsupported_stems});
  REQUIRE_TRUE(!unsupported,
               "bus detector was silently isolated without a separate detector graph");
  REQUIRE_EQ(unsupported.error().code, foundation::ErrorCode::unsupported,
             "bus detector was silently isolated without a separate detector graph");
  REQUIRE_TRUE(!fs::exists(unsupported_stems),
               "bus detector was silently isolated without a separate detector graph");
}

void test_saved_audio_track_eq_master_and_stems() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "track EQ fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "track EQ fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  const auto slot = test_insert::next_slot(snapshot);
  snapshot.tracks[0].effects.push_back(
      {slot, true, test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0})});
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "track EQ fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  REQUIRE_EQ(snapshot.tracks[0].effects.size(), 1,
             "track EQ insert identity or settings changed on reopen");
  REQUIRE_EQ(snapshot.tracks[0].effects[0].id, slot,
             "track EQ insert identity or settings changed on reopen");
  REQUIRE_EQ(std::get<project::NativeEffectPatch>(snapshot.tracks[0].effects[0].processor).engine,
             "shape", "track EQ insert identity or settings changed on reopen");
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto target = directory.path() / "track-eq-master.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {target})), PublishOutcome::durable,
             "saved track EQ master failed");
  const auto master = read_bytes(target);
  const std::array<native_dsp::EqBandSettings, 1> settings{
      {{native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}};
  const auto prepared = take(native_dsp::prepare_eq(settings, snapshot.sample_rate));
  const auto tail = take(native_dsp::estimate_eq_tail_frames(prepared));
  REQUIRE_EQ(u32(master, 52) / 8, 24'006 + tail, "track EQ tail was truncated");
  std::array<engine::StereoFrame, 8> expected{};
  for (std::size_t frame = 0; frame < 4; ++frame)
    expected[frame] = {0.25F, 0.5F};
  native_dsp::EqRuntime oracle(prepared);
  oracle.process(expected, {24'000, 1});
  const auto bundle = directory.path() / "track-eq-stems";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active, {bundle})), PublishOutcome::durable,
             "saved track EQ stems failed");
  const auto wet = read_bytes(bundle / ("track-" + snapshot.tracks[0].id.to_string() + ".wav"));
  const auto dry = read_bytes(bundle / ("track-" + snapshot.tracks[1].id.to_string() + ".wav"));
  REQUIRE_EQ(u32(wet, 52) / 8, 24'006 + tail, "track EQ stems lost common extent");
  REQUIRE_EQ(u32(dry, 52) / 8, 24'006 + tail, "track EQ stems lost common extent");
  for (const auto frame : {0U, 2U, 4U, 6U}) {
    const auto offset = 56 + (24'000U + frame) * 8;
    const double dry_sample = frame >= 2 && frame < 6 ? 0.25 : 0.0;
    REQUIRE_LT(std::abs(sample(wet, offset, WavFormat::float32) - expected[frame].left), 1e-7,
               "track EQ oracle, pre-route insert order, or stem isolation differs");
    REQUIRE_LT(std::abs(sample(dry, offset, WavFormat::float32) - dry_sample), 1e-7,
               "track EQ oracle, pre-route insert order, or stem isolation differs");
    REQUIRE_LT(
        std::abs(sample(master, offset, WavFormat::float32) - (expected[frame].left + dry_sample)),
        1e-7, "track EQ oracle, pre-route insert order, or stem isolation differs");
  }
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {target})), PublishOutcome::durable,
             "saved track EQ repeat render differs");
  REQUIRE_EQ(read_bytes(target), master, "saved track EQ repeat render differs");
  auto parallel = snapshot;
  parallel.tracks[1].effects.push_back(
      {test_insert::next_slot(parallel), true,
       test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0})});
  const auto parallel_target = directory.path() / "parallel-native-master.wav";
  REQUIRE_EQ(take(render_wav_master(parallel, store, active, {parallel_target})),
             PublishOutcome::durable, "separate native track chains did not export a master");
  const auto parallel_master = read_bytes(parallel_target);
  REQUIRE_EQ(u32(parallel_master, 52) / 8, 24'006 + tail,
             "parallel native export summed independent tails or truncated an EQ tail");
  const auto parallel_bundle = directory.path() / "parallel-native-stems";
  REQUIRE_EQ(take(render_wav_stems(parallel, store, active, {parallel_bundle})),
             PublishOutcome::durable,
             "separate native track chains did not export processed stems");
  const auto parallel_wet =
      read_bytes(parallel_bundle / ("track-" + parallel.tracks[0].id.to_string() + ".wav"));
  const auto parallel_dry =
      read_bytes(parallel_bundle / ("track-" + parallel.tracks[1].id.to_string() + ".wav"));
  std::array<engine::StereoFrame, 8> wet_input{}, dry_input{};
  for (std::size_t frame = 0; frame < 4; ++frame)
    wet_input[frame] = {0.25F, 0.5F};
  for (std::size_t frame = 2; frame < 6; ++frame)
    dry_input[frame] = {0.25F, 0.5F};
  native_dsp::EqRuntime wet_oracle(prepared);
  native_dsp::EqRuntime dry_oracle(prepared);
  wet_oracle.process(wet_input, {24'000, 1});
  dry_oracle.process(dry_input, {24'000, 1});
  for (const auto frame : {0U, 2U, 4U, 6U}) {
    const auto offset = 56 + (24'000U + frame) * 8;
    REQUIRE_LT(std::abs(sample(parallel_wet, offset, WavFormat::float32) - wet_input[frame].left),
               1e-7, "parallel native master or processed stems omitted a saved chain");
    REQUIRE_LT(std::abs(sample(parallel_dry, offset, WavFormat::float32) - dry_input[frame].left),
               1e-7, "parallel native master or processed stems omitted a saved chain");
    REQUIRE_LT(std::abs(sample(parallel_master, offset, WavFormat::float32) -
                        wet_input[frame].left - dry_input[frame].left),
               1e-7, "parallel native master or processed stems omitted a saved chain");
  }
  REQUIRE_EQ(take(render_wav_master(parallel, store, active, {parallel_target})),
             PublishOutcome::durable, "parallel native master was not repeatable");
  REQUIRE_EQ(read_bytes(parallel_target), parallel_master,
             "parallel native master was not repeatable");
  auto serial = snapshot;
  const auto bus = id<project::TrackId>(44);
  serial.tracks[0].main_output = bus;
  serial.tracks.emplace_back(bus, std::vector<project::Clip>{}, project::TrackKind::group_bus);
  serial.tracks.back().effects.push_back(
      {test_insert::next_slot(serial), true,
       test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0})});
  const auto serial_target = directory.path() / "serial-native-master.wav";
  REQUIRE_EQ(take(render_wav_master(serial, store, active, {serial_target})),
             PublishOutcome::durable, "serial native track and bus chains did not export");
  REQUIRE_EQ(u32(read_bytes(serial_target), 52) / 8, 24'006 + 2 * tail,
             "serial native route did not retain both processor tails");
  auto chained = snapshot;
  native_dsp::CompressorSettings compressor_settings;
  compressor_settings.threshold_db = -30.0;
  compressor_settings.ratio = 20.0;
  compressor_settings.knee_db = 0.0;
  compressor_settings.attack_ms = 0.1;
  compressor_settings.release_ms = 1.0;
  chained.tracks[0].effects.push_back(
      {test_insert::next_slot(chained), true, test_insert::press(compressor_settings)});
  const auto chain_target = directory.path() / "track-chain-master.wav";
  REQUIRE_EQ(take(render_wav_master(chained, store, active, {chain_target})),
             PublishOutcome::durable, "ordered native EQ/compressor route failed");
  const auto chain_master = read_bytes(chain_target);
  REQUIRE_EQ(u32(chain_master, 52) / 8, 24'006 + tail,
             "native compressor changed zero-latency route extent");
  const auto prepared_compressor =
      take(native_dsp::prepare_compressor(compressor_settings, chained.sample_rate));
  native_dsp::CompressorRuntime compressor_oracle(prepared_compressor);
  compressor_oracle.process(expected, {24'000, 1});
  for (const auto frame : {0U, 2U, 4U, 6U}) {
    const auto offset = 56 + (24'000U + frame) * 8;
    const double dry_sample = frame >= 2 && frame < 6 ? 0.25 : 0.0;
    REQUIRE_LT(std::abs(sample(chain_master, offset, WavFormat::float32) -
                        (expected[frame].left + dry_sample)),
               1e-7, "offline native route ignored saved EQ/compressor order");
  }
  REQUIRE_EQ(take(render_wav_master(chained, store, active, {chain_target})),
             PublishOutcome::durable, "native route chain repeat render differs");
  REQUIRE_EQ(read_bytes(chain_target), chain_master, "native route chain repeat render differs");
  const auto chain_bundle = directory.path() / "track-chain-stems";
  REQUIRE_EQ(take(render_wav_stems(chained, store, active, {chain_bundle})),
             PublishOutcome::durable, "native route chain processed stems failed");
  const auto chain_wet =
      read_bytes(chain_bundle / ("track-" + chained.tracks[0].id.to_string() + ".wav"));
  REQUIRE_LT(std::abs(sample(chain_wet, 56 + 24'000U * 8, WavFormat::float32) - expected[0].left),
             1e-7, "native route stem omitted saved processor chain");
  auto bypassed_chain = chained;
  const auto bypass_slot = test_insert::next_slot(bypassed_chain);
  project::Vst3PluginInstance missing_effect{.id = id<project::PluginInstanceId>(98),
                                             .class_id = std::string(32, '0'),
                                             .vendor = "Unavailable vendor",
                                             .name = "Unavailable effect",
                                             .version = "1",
                                             .bundle_path = "/missing/bypassed-chain.vst3",
                                             .binary_sha256 = std::string(64, '0'),
                                             .component_state = {},
                                             .parameters = {},
                                             .missing = true};
  bypassed_chain.tracks[0].effects.insert(bypassed_chain.tracks[0].effects.begin() + 1,
                                          {bypass_slot, false, missing_effect});
  const auto bypass_chain_target = directory.path() / "bypassed-chain-master.wav";
  REQUIRE_EQ(take(render_wav_master(bypassed_chain, store, active, {bypass_chain_target})),
             PublishOutcome::durable,
             "bypassed missing VST3 between native stages changed order or output");
  REQUIRE_EQ(read_bytes(bypass_chain_target), chain_master,
             "bypassed missing VST3 between native stages changed order or output");
  REQUIRE_EQ(bypassed_chain.tracks[0].effects[1].id, bypass_slot,
             "bypassed missing VST3 between native stages changed order or output");
  const auto bypass_chain_bundle = directory.path() / "bypassed-chain-stems";
  REQUIRE_EQ(take(render_wav_stems(bypassed_chain, store, active, {bypass_chain_bundle})),
             PublishOutcome::durable, "bypassed VST3 changed the ordered native stem");
  REQUIRE_EQ(
      read_bytes(bypass_chain_bundle / ("track-" + chained.tracks[0].id.to_string() + ".wav")),
      chain_wet, "bypassed VST3 changed the ordered native stem");
  auto mixed_active = bypassed_chain;
  mixed_active.tracks[0].effects[1].enabled = true;
  std::get<project::Vst3PluginInstance>(mixed_active.tracks[0].effects[1].processor).missing =
      false;
  REQUIRE_TRUE(!render_wav_master(mixed_active, store, active,
                                  {directory.path() / "mixed-active-native-vst3.wav"}),
               "active native/VST3 chain was admitted by the bypass rule");
  REQUIRE_TRUE(!fs::exists(directory.path() / "mixed-active-native-vst3.wav"),
               "active native/VST3 chain was admitted by the bypass rule");
  auto overbound = bypassed_chain;
  for (std::size_t stages = 2; stages <= routing::ProjectRouteIdRegistry::max_stages_per_path;
       ++stages)
    overbound.tracks[0].effects.push_back(
        {test_insert::next_slot(overbound), true, test_insert::flat_shape()});
  REQUIRE_TRUE(!render_wav_master(overbound, store, active,
                                  {directory.path() / "overbound-bypassed-chain.wav"}),
               "bypassed VST3 evaded the chain stage bound");
  REQUIRE_TRUE(!fs::exists(directory.path() / "overbound-bypassed-chain.wav"),
               "bypassed VST3 evaded the chain stage bound");
  auto three_stages = chained;
  three_stages.tracks[0].effects.push_back(
      {test_insert::next_slot(three_stages), true,
       test_insert::shape({native_dsp::EqBandKind::peaking, 2'000.0, 0.707, 6.0})});
  const auto third_target = directory.path() / "three-native-stages.wav";
  REQUIRE_EQ(take(render_wav_master(three_stages, store, active, {third_target})),
             PublishOutcome::durable, "three saved native stages did not export");
  const auto third_master = read_bytes(third_target);
  const std::array third_settings{
      native_dsp::EqBandSettings{native_dsp::EqBandKind::peaking, 2'000.0, 0.707, 6.0}};
  const auto third_eq = take(native_dsp::prepare_eq(third_settings, 48'000));
  const auto third_tail = take(native_dsp::estimate_eq_tail_frames(third_eq));
  REQUIRE_EQ(u32(third_master, 52) / 8, 24'006 + tail + third_tail,
             "third native stage tail was truncated");
  native_dsp::EqRuntime third_oracle(third_eq);
  third_oracle.process(expected, {24'000, 1});
  for (const auto frame : {0U, 2U, 4U, 6U}) {
    const auto offset = 56 + (24'000U + frame) * 8;
    const double dry_sample = frame >= 2 && frame < 6 ? 0.25 : 0.0;
    REQUIRE_LT(std::abs(sample(third_master, offset, WavFormat::float32) -
                        (expected[frame].left + dry_sample)),
               1e-7, "three-stage offline native chain lost saved order");
  }
  auto reversed_chain = chained;
  std::ranges::reverse(reversed_chain.tracks[0].effects);
  const auto reversed_target = directory.path() / "reversed-track-chain.wav";
  REQUIRE_EQ(take(render_wav_master(reversed_chain, store, active, {reversed_target})),
             PublishOutcome::durable, "reordered native route chain failed");
  const auto reversed_bytes = read_bytes(reversed_target);
  REQUIRE_NE(reversed_bytes, chain_master,
             "saved native route order did not change the nonlinear render");
  auto limited_native = chained;
  limited_native.tracks[0].effects[1].processor = test_insert::ceiling({-6.0, 1.0, 100.0});
  const auto limited_target = directory.path() / "track-limiter-master.wav";
  REQUIRE_EQ(take(render_wav_master(limited_native, store, active, {limited_target})),
             PublishOutcome::durable, "ordered native EQ/limiter route failed");
  const auto limited_master = read_bytes(limited_target);
  REQUIRE_EQ(u32(limited_master, 52) / 8, 24'006 + tail,
             "route limiter shifted the published timeline or exceeded its ceiling");
  REQUIRE_LE(std::abs(sample(limited_master, 56 + 24'000U * 8, WavFormat::float32)), 0.502,
             "route limiter shifted the published timeline or exceeded its ceiling");
  REQUIRE_EQ(take(render_wav_master(limited_native, store, active, {limited_target})),
             PublishOutcome::durable, "route limiter repeat render differs");
  REQUIRE_EQ(read_bytes(limited_target), limited_master, "route limiter repeat render differs");
  auto invalid_limiter = limited_native;
  invalid_limiter.tracks[0].effects[1].processor = test_insert::with_raw_value(
      std::get<project::NativeEffectPatch>(invalid_limiter.tracks[0].effects[1].processor),
      "ceiling.release", 0.0);
  bool invalid_writer_opened = false;
  REQUIRE_TRUE(!detail::render_wav_master_with_start_hook(
                   invalid_limiter, store, active, {limited_target},
                   [&invalid_writer_opened]() { invalid_writer_opened = true; }),
               "invalid route limiter opened a writer or replaced durable output");
  REQUIRE_TRUE(!invalid_writer_opened,
               "invalid route limiter opened a writer or replaced durable output");
  REQUIRE_EQ(read_bytes(limited_target), limited_master,
             "invalid route limiter opened a writer or replaced durable output");
  auto rejected = snapshot;
  for (std::size_t index = 1; index <= routing::ProjectRouteIdRegistry::max_stages_per_path;
       ++index)
    rejected.tracks[0].effects.push_back(
        {test_insert::next_slot(rejected), true,
         test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0})});
  bool writer_opened = false;
  auto rejected_chain = detail::render_wav_master_with_start_hook(
      rejected, store, active, {target}, [&writer_opened]() { writer_opened = true; });
  REQUIRE_TRUE(!rejected_chain,
               "stage beyond the chain bound was silently omitted or opened a writer");
  REQUIRE_TRUE(!writer_opened,
               "stage beyond the chain bound was silently omitted or opened a writer");
  REQUIRE_EQ(read_bytes(target), master,
             "stage beyond the chain bound was silently omitted or opened a writer");
  rejected = snapshot;
  rejected.tracks[1].effects.push_back(
      {test_insert::next_slot(rejected), true,
       test_insert::with_raw_value(test_insert::flat_shape(), "shape.freq_1", 0.0)});
  REQUIRE_TRUE(!render_wav_master(rejected, store, active, {target}),
               "invalid second-track EQ settings were silently accepted");
  REQUIRE_EQ(read_bytes(target), master, "invalid second-track EQ settings were silently accepted");
  rejected = snapshot;
  rejected.tracks[0].effects[0].enabled = false;
  const auto bypass_target = directory.path() / "bypassed-track-eq.wav";
  REQUIRE_EQ(take(render_wav_master(rejected, store, active, {bypass_target})),
             PublishOutcome::durable, "bypassed native track EQ failed offline export");
  auto plain = snapshot;
  plain.tracks[0].effects.clear();
  const auto plain_target = directory.path() / "plain-track.wav";
  REQUIRE_EQ(take(render_wav_master(plain, store, active, {plain_target})), PublishOutcome::durable,
             "bypassed native track EQ changed dry output or replaced the prior master");
  REQUIRE_EQ(read_bytes(bypass_target), read_bytes(plain_target),
             "bypassed native track EQ changed dry output or replaced the prior master");
  REQUIRE_EQ(read_bytes(target), master,
             "bypassed native track EQ changed dry output or replaced the prior master");
  rejected = std::move(snapshot);
  rejected.tracks[0].effects[0].processor = test_insert::with_raw_value(
      std::get<project::NativeEffectPatch>(rejected.tracks[0].effects[0].processor), "shape.freq_1",
      0.0);
  REQUIRE_TRUE(!render_wav_master(rejected, store, active, {target}),
               "invalid track EQ replaced durable master");
  REQUIRE_EQ(read_bytes(target), master, "invalid track EQ replaced durable master");
}

void test_saved_group_bus_eq_master_stems_and_bypass() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "bus EQ source open failed");
  auto staged = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "bus EQ source close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  const auto group = id<project::TrackId>(80);
  snapshot.tracks.emplace_back(group, std::vector<project::Clip>{}, project::TrackKind::group_bus);
  snapshot.tracks[0].main_output = group;
  const auto slot = test_insert::next_slot(snapshot);
  snapshot.tracks.back().effects.push_back(
      {slot, true, test_insert::shape({native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0})});
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "bus EQ fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  REQUIRE_EQ(snapshot.tracks.back().effects.front().id, slot,
             "bus EQ route or slot identity changed on reopen");
  REQUIRE_EQ(snapshot.tracks[0].main_output, group,
             "bus EQ route or slot identity changed on reopen");
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto target = directory.path() / "bus-eq-master.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {target})), PublishOutcome::durable,
             "saved group EQ master failed");
  const auto master = read_bytes(target);
  const std::array<native_dsp::EqBandSettings, 1> settings{
      {{native_dsp::EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}};
  const auto prepared = take(native_dsp::prepare_eq(settings, snapshot.sample_rate));
  const auto tail = take(native_dsp::estimate_eq_tail_frames(prepared));
  REQUIRE_EQ(u32(master, 52) / 8, 24'006 + tail, "group EQ tail was truncated");
  std::array<engine::StereoFrame, 8> expected{};
  for (std::size_t frame = 0; frame < 4; ++frame)
    expected[frame] = {0.25F, 0.5F};
  native_dsp::EqRuntime oracle(prepared);
  oracle.process(expected, {24'000, 1});
  const auto bundle = directory.path() / "bus-eq-stems";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active, {bundle})), PublishOutcome::durable,
             "saved group EQ stems failed");
  const auto wet = read_bytes(bundle / ("track-" + snapshot.tracks[0].id.to_string() + ".wav"));
  const auto dry = read_bytes(bundle / ("track-" + snapshot.tracks[1].id.to_string() + ".wav"));
  for (const auto frame : {0U, 2U, 4U, 6U}) {
    const auto offset = 56 + (24'000U + frame) * 8;
    const double dry_sample = frame >= 2 && frame < 6 ? 0.25 : 0.0;
    REQUIRE_LT(std::abs(sample(wet, offset, WavFormat::float32) - expected[frame].left), 1e-7,
               "group EQ master/stem routing disagrees with independent DSP oracle");
    REQUIRE_LT(std::abs(sample(dry, offset, WavFormat::float32) - dry_sample), 1e-7,
               "group EQ master/stem routing disagrees with independent DSP oracle");
    REQUIRE_LT(
        std::abs(sample(master, offset, WavFormat::float32) - (expected[frame].left + dry_sample)),
        1e-7, "group EQ master/stem routing disagrees with independent DSP oracle");
  }
  auto bypassed = snapshot;
  bypassed.tracks.back().effects.front().enabled = false;
  const auto bypass_target = directory.path() / "bus-eq-bypassed.wav";
  REQUIRE_EQ(take(render_wav_master(bypassed, store, active, {bypass_target})),
             PublishOutcome::durable, "bypassed group EQ failed offline export");
  auto plain = snapshot;
  plain.tracks.back().effects.clear();
  const auto plain_target = directory.path() / "bus-eq-plain.wav";
  REQUIRE_EQ(take(render_wav_master(plain, store, active, {plain_target})), PublishOutcome::durable,
             "bypassed group EQ changed the dry route");
  REQUIRE_EQ(read_bytes(bypass_target), read_bytes(plain_target),
             "bypassed group EQ changed the dry route");
}

void test_aligned_atomic_stem_bundle() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "stem fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "stem fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "stem fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto bundle = directory.path() / "stems";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active, {bundle})), PublishOutcome::durable,
             "aligned stems did not publish");
  const auto first_name = "track-" + snapshot.tracks[0].id.to_string() + ".wav";
  const auto second_name = "track-" + snapshot.tracks[1].id.to_string() + ".wav";
  const auto first = read_bytes(bundle / first_name);
  const auto second = read_bytes(bundle / second_name);
  REQUIRE_EQ(u32(first, 52) / 8, 24'006, "stems do not share the project extent");
  REQUIRE_EQ(u32(second, 52) / 8, 24'006, "stems do not share the project extent");
  const auto left = [](const auto& bytes, std::size_t frame) {
    return sample(bytes, 56 + frame * 8, WavFormat::float32);
  };
  REQUIRE_EQ(left(first, 24'000), 0.25, "stem tap leaked another track or changed clip placement");
  REQUIRE_EQ(left(second, 24'000), 0.0, "stem tap leaked another track or changed clip placement");
  REQUIRE_EQ(left(first, 24'002), 0.25, "stem tap leaked another track or changed clip placement");
  REQUIRE_EQ(left(second, 24'002), 0.25, "stem tap leaked another track or changed clip placement");
  REQUIRE_EQ(left(first, 24'004), 0.0, "stem tap leaked another track or changed clip placement");
  REQUIRE_EQ(left(second, 24'004), 0.25, "stem tap leaked another track or changed clip placement");
  const auto master_path = directory.path() / "master.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {master_path})),
             PublishOutcome::durable,
             "independent summed master oracle differs from isolated stems");
  REQUIRE_EQ(left(read_bytes(master_path), 24'002), 0.5,
             "independent summed master oracle differs from isolated stems");
  REQUIRE_TRUE(!render_wav_stems(snapshot, store, active, {bundle}),
               "existing stem bundle was replaced");
  REQUIRE_EQ(read_bytes(bundle / first_name), first, "existing stem bundle was replaced");
  REQUIRE_EQ(read_bytes(bundle / second_name), second, "existing stem bundle was replaced");
  const auto repeated = directory.path() / "stems-repeat";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active, {repeated})), PublishOutcome::durable,
             "repeat stem render differs");
  REQUIRE_EQ(read_bytes(repeated / first_name), first, "repeat stem render differs");
  REQUIRE_EQ(read_bytes(repeated / second_name), second, "repeat stem render differs");
  auto stale = active;
  stale.active_graph_revision = 0;
  REQUIRE_TRUE(!render_wav_stems(snapshot, store, stale, {directory.path() / "stale"}),
               "stale stem revision was published");
  REQUIRE_TRUE(!fs::exists(directory.path() / "stale"), "stale stem revision was published");
  const auto interrupted = directory.path() / "interrupted";
  auto failed = detail::render_wav_stems_with_after_stem_hook(
      snapshot, store, active, {interrupted}, [](std::size_t index) {
        if (index == 0) {
          return foundation::Result<void>::failure(
              {foundation::ErrorCode::io_failure, "Injected stem failure"});
        }
        return foundation::Result<void>::success();
      });
  REQUIRE_TRUE(!failed, "partial stem bundle was published");
  REQUIRE_TRUE(!fs::exists(interrupted), "partial stem bundle was published");
  for (const auto& entry : fs::directory_iterator(directory.path())) {
    require(!entry.path().filename().string().starts_with(".oscillate-stems-"),
            "failed private stem directory was not removed");
  }
  auto unsupported = snapshot;
  test_insert::append_effect(unsupported, unsupported.tracks[0],
                             project::Vst3PluginInstance{id<project::PluginInstanceId>(99),
                                                         std::string(32, '0'),
                                                         "vendor",
                                                         "effect",
                                                         "1",
                                                         "/missing/saved.vst3",
                                                         std::string(64, '0'),
                                                         {},
                                                         {},
                                                         false});
  REQUIRE_TRUE(!render_wav_stems(unsupported, store, active, {directory.path() / "unsupported"}),
               "unrendered VST3 insert was omitted from stems");
  REQUIRE_TRUE(!fs::exists(directory.path() / "unsupported"),
               "unrendered VST3 insert was omitted from stems");
  const auto previous_master = read_bytes(master_path);
  std::get<project::Vst3PluginInstance>(unsupported.tracks[0].effects.front().processor).missing =
      true;
  auto missing_effect = render_wav_master(unsupported, store, active, {master_path});
  REQUIRE_TRUE(!missing_effect,
               "missing saved effect did not fail before master output was replaced");
  REQUIRE_NE(missing_effect.error().message.find("VST3 audio effect is missing"), std::string::npos,
             "missing saved effect did not fail before master output was replaced");
  REQUIRE_EQ(read_bytes(master_path), previous_master,
             "missing saved effect did not fail before master output was replaced");

  auto with_reverb = snapshot;
  test_insert::set_master(with_reverb, test_insert::expanse(0.2, 1.0));
  const auto wet_bundle = directory.path() / "wet-stems";
  REQUIRE_EQ(take(render_wav_stems(with_reverb, store, active, {wet_bundle})),
             PublishOutcome::durable, "wet stems did not publish");
  const auto wet_first = read_bytes(wet_bundle / first_name);
  const auto wet_second = read_bytes(wet_bundle / second_name);
  const auto tail = test_insert::tail_frames(test_insert::expanse(0.2, 1.0), 48'000);
  REQUIRE_EQ(u32(wet_first, 52) / 8, 24'006 + tail,
             "stem effect tails were not aligned or complete");
  REQUIRE_EQ(u32(wet_second, 52) / 8, 24'006 + tail,
             "stem effect tails were not aligned or complete");
  REQUIRE_GT(tail_peak(wet_first, 24'006, tail), 0.0, "a stem's master reverb tail was truncated");
  REQUIRE_GT(tail_peak(wet_second, 24'006, tail), 0.0, "a stem's master reverb tail was truncated");
  for (const auto format : {WavFormat::pcm16, WavFormat::pcm24}) {
    const auto width = format == WavFormat::pcm16 ? 2U : 3U;
    const auto integer_bundle =
        directory.path() / (format == WavFormat::pcm16 ? "wet-stems-16" : "wet-stems-24");
    const StemRenderOptions options{integer_bundle, format, 123};
    REQUIRE_EQ(take(render_wav_stems(with_reverb, store, active, options)), PublishOutcome::durable,
               "integer processed stems did not publish");
    const auto integer_first = read_bytes(integer_bundle / first_name);
    const auto integer_second = read_bytes(integer_bundle / second_name);
    for (const auto& bytes : {std::cref(integer_first), std::cref(integer_second)}) {
      const auto& wav = bytes.get();
      REQUIRE_EQ(u16(wav, 20), 1,
                 "integer processed stem bit depth or tail extent differs from WAV contract");
      REQUIRE_EQ(u16(wav, 22), 2,
                 "integer processed stem bit depth or tail extent differs from WAV contract");
      REQUIRE_EQ(u16(wav, 34), width * 8,
                 "integer processed stem bit depth or tail extent differs from WAV contract");
      REQUIRE_EQ(u32(wav, 40), (24'006 + tail) * 2 * width,
                 "integer processed stem bit depth or tail extent differs from WAV contract");
      REQUIRE_EQ(wav.size(), 44 + (24'006 + tail) * 2 * width,
                 "integer processed stem bit depth or tail extent differs from WAV contract");
    }
    const auto tolerance = format == WavFormat::pcm16 ? 2.0 / 32'768.0 : 2.0 / 8'388'608.0;
    const std::array<std::uint64_t, 6> frames{
        24'000, 24'002, 24'004, 24'006 + tail / 4, 24'006 + tail / 2, 24'006 + tail - 1};
    for (const auto frame : frames) {
      for (const auto channel : {0U, 1U}) {
        const auto offset = 44 + (frame * 2 + channel) * width;
        const auto reference_offset = 56 + (frame * 2 + channel) * 4;
        REQUIRE_NEAR(sample(integer_first, offset, format),
                     sample(wet_first, reference_offset, WavFormat::float32), tolerance,
                     "integer processed stem changed source isolation, placement or effect tail");
        REQUIRE_NEAR(sample(integer_second, offset, format),
                     sample(wet_second, reference_offset, WavFormat::float32), tolerance,
                     "integer processed stem changed source isolation, placement or effect tail");
      }
    }
    const auto repeated_integer =
        directory.path() /
        (format == WavFormat::pcm16 ? "wet-stems-16-repeat" : "wet-stems-24-repeat");
    REQUIRE_EQ(take(render_wav_stems(with_reverb, store, active, {repeated_integer, format, 123})),
               PublishOutcome::durable, "integer processed stem repeat changed dither or samples");
    REQUIRE_EQ(read_bytes(repeated_integer / first_name), integer_first,
               "integer processed stem repeat changed dither or samples");
    REQUIRE_EQ(read_bytes(repeated_integer / second_name), integer_second,
               "integer processed stem repeat changed dither or samples");
  }
  auto limited_stems = with_reverb;
  test_insert::set_master(limited_stems, test_insert::ceiling({-6.0, 5.0, 70.0}));
  const auto limited_bundle = directory.path() / "limited-stems";
  REQUIRE_EQ(take(render_wav_stems(limited_stems, store, active, {limited_bundle})),
             PublishOutcome::durable, "processed limiter stems did not publish");
  const auto limited_first = read_bytes(limited_bundle / first_name);
  const auto limited_second = read_bytes(limited_bundle / second_name);
  REQUIRE_EQ(u32(limited_first, 52) / 8, 24'006 + tail,
             "stem limiter truncated an effect tail or exceeded its ceiling");
  REQUIRE_EQ(u32(limited_second, 52) / 8, 24'006 + tail,
             "stem limiter truncated an effect tail or exceeded its ceiling");
  REQUIRE_GT(tail_peak(limited_first, 24'006, tail), 0.0,
             "stem limiter truncated an effect tail or exceeded its ceiling");
  REQUIRE_GT(tail_peak(limited_second, 24'006, tail), 0.0,
             "stem limiter truncated an effect tail or exceeded its ceiling");
  REQUIRE_LE(tail_peak(limited_first, 0, 24'006 + tail), std::pow(10.0, -6.0 / 20.0) + 1e-6,
             "stem limiter truncated an effect tail or exceeded its ceiling");
  REQUIRE_LE(tail_peak(limited_second, 0, 24'006 + tail), std::pow(10.0, -6.0 / 20.0) + 1e-6,
             "stem limiter truncated an effect tail or exceeded its ceiling");
  auto quieter_stems = limited_stems;
  quieter_stems.master_level = 0.5;
  const auto quieter_bundle = directory.path() / "quieter-stems";
  REQUIRE_EQ(take(render_wav_stems(quieter_stems, store, active, {quieter_bundle})),
             PublishOutcome::durable, "master output level did not render on processed stems");
  const auto quieter_first = read_bytes(quieter_bundle / first_name);
  const auto quieter_second = read_bytes(quieter_bundle / second_name);
  for (const auto frame : {24'000U, 24'002U, 24'006U + 101U, 24'006U + 503U}) {
    REQUIRE_LT(std::abs(left(quieter_first, frame) - 0.5 * left(limited_first, frame)), 1e-7,
               "processed stems did not apply master level after the effect chain");
    REQUIRE_LT(std::abs(left(quieter_second, frame) - 0.5 * left(limited_second, frame)), 1e-7,
               "processed stems did not apply master level after the effect chain");
  }
  auto invalid_limited_stems = limited_stems;
  test_insert::set_master(invalid_limited_stems,
                          test_insert::with_raw_value(test_insert::ceiling({-6.0, 5.0, 70.0}),
                                                      "ceiling.lookahead", 11.0));
  const auto invalid_limited_bundle = directory.path() / "invalid-limited-stems";
  REQUIRE_TRUE(!render_wav_stems(invalid_limited_stems, store, active, {invalid_limited_bundle}),
               "invalid limiter published a partial stem bundle");
  REQUIRE_TRUE(!fs::exists(invalid_limited_bundle),
               "invalid limiter published a partial stem bundle");
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  const auto wet_converted = directory.path() / "wet-stems-44100";
  REQUIRE_EQ(take(render_wav_stems(with_reverb, store, active,
                                   {wet_converted, WavFormat::float32, 0, 44'100})),
             PublishOutcome::durable, "converted wet stems did not publish");
  const auto converted_first = read_bytes(wet_converted / first_name);
  const auto converted_second = read_bytes(wet_converted / second_name);
  const auto converted_tail_frames = ((24'006 + tail) * 44'100 + 24'000) / 48'000;
  REQUIRE_EQ(u32(converted_first, 24), 44'100,
             "converted stems lost alignment or effect-tail duration");
  REQUIRE_EQ(u32(converted_second, 24), 44'100,
             "converted stems lost alignment or effect-tail duration");
  REQUIRE_EQ(u32(converted_first, 52) / 8, converted_tail_frames,
             "converted stems lost alignment or effect-tail duration");
  REQUIRE_EQ(u32(converted_second, 52) / 8, converted_tail_frames,
             "converted stems lost alignment or effect-tail duration");
  REQUIRE_GT(tail_peak(converted_first, 24'006 * 44'100 / 48'000, tail * 44'100 / 48'000), 0.0,
             "rate conversion discarded the reverb tail");
  const auto limited_converted = directory.path() / "limited-stems-44100";
  REQUIRE_EQ(take(render_wav_stems(limited_stems, store, active,
                                   {limited_converted, WavFormat::float32, 0, 44'100})),
             PublishOutcome::durable, "converted limiter stems did not publish");
  const auto limited_converted_first = read_bytes(limited_converted / first_name);
  const auto limited_converted_second = read_bytes(limited_converted / second_name);
  REQUIRE_EQ(u32(limited_converted_first, 52) / 8, converted_tail_frames,
             "converted limiter stems shifted timing or lost effect tail");
  REQUIRE_EQ(u32(limited_converted_second, 52) / 8, converted_tail_frames,
             "converted limiter stems shifted timing or lost effect tail");
  REQUIRE_GT(tail_peak(limited_converted_first, 24'006 * 44'100 / 48'000, tail * 44'100 / 48'000),
             0.0, "converted limiter stems shifted timing or lost effect tail");
#endif
  ExportGate gate;
  auto authority = take(StoppedExportAuthority::acquire(snapshot, gate));
  require(gate.held, "stopped stem authority did not hold transport");
  const auto stopped =
      take(render_wav_stems(snapshot, store, std::move(authority), {directory.path() / "stopped"}));
  REQUIRE_EQ(stopped, PublishOutcome::durable, "stopped stem authority was not held and released");
  REQUIRE_TRUE(!gate.held, "stopped stem authority was not held and released");
}

void test_routed_master_and_source_stems() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  const auto source_path = directory.path() / "input.wav";
  write_bytes(source_path, fixture_wav());
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "routed fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(4), fd);
  REQUIRE_EQ(::close(fd), 0, "routed fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  const auto group = id<project::TrackId>(77);
  const auto returned = id<project::TrackId>(78);
  snapshot.tracks.emplace_back(group, std::vector<project::Clip>{}, project::TrackKind::group_bus);
  snapshot.tracks.emplace_back(returned, std::vector<project::Clip>{},
                               project::TrackKind::return_bus);
  snapshot.tracks[0].main_output = group;
  snapshot.tracks[0].sends.push_back({id<project::SendId>(79), returned, 0.5});
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "routed fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  REQUIRE_EQ(snapshot.tracks[0].main_output, group, "saved routing did not reopen");
  REQUIRE_EQ(snapshot.tracks[0].sends.size(), 1, "saved routing did not reopen");
  REQUIRE_EQ(snapshot.tracks[0].sends[0].gain, 0.5, "saved routing did not reopen");
  ExportGate gate;
  const auto master_path = directory.path() / "routed-master.wav";
  auto authority = take(StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_EQ(take(render_wav_master(snapshot, store, std::move(authority), {master_path})),
             PublishOutcome::durable, "routed master did not publish");
  REQUIRE_TRUE(!gate.held, "routed master did not publish");
  const auto master = read_bytes(master_path);
  const auto left = [](const auto& bytes, std::size_t frame) {
    return sample(bytes, 56 + frame * 8, WavFormat::float32);
  };
  REQUIRE_LT(std::abs(left(master, 24'000) - 0.375), oscillate::test::tolerance::float_round_trip,
             "group/return send gain was not audible in master");
  REQUIRE_LT(std::abs(left(master, 24'002) - 0.625), oscillate::test::tolerance::float_round_trip,
             "group/return send gain was not audible in master");
  // A muted source is skipped, not rendered and silenced, so the right one must
  // be skipped: the master holds exactly what the other source contributes.
  for (const std::size_t muted : {std::size_t{0}, std::size_t{1}}) {
    auto quiet = snapshot;
    quiet.tracks[muted].muted = true;
    const auto quiet_path = directory.path() / ("muted-" + std::to_string(muted) + ".wav");
    authority = take(StoppedExportAuthority::acquire(quiet, gate));
    REQUIRE_EQ(take(render_wav_master(quiet, store, std::move(authority), {quiet_path})),
               PublishOutcome::durable, "master with a muted source did not publish");
    const auto heard = read_bytes(quiet_path);
    const double first = muted == 0 ? 0.0 : 0.375;
    const double later = muted == 0 ? 0.25 : 0.375;
    REQUIRE_LT(std::abs(left(heard, 24'000) - first), oscillate::test::tolerance::float_round_trip,
               "muting one source silenced the other or left itself audible");
    REQUIRE_LT(std::abs(left(heard, 24'002) - later), oscillate::test::tolerance::float_round_trip,
               "muting one source silenced the other or left itself audible");
  }

  const auto bundle = directory.path() / "routed-stems";
  authority = take(StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, std::move(authority), {bundle})),
             PublishOutcome::durable, "routed source stems did not publish");
  const auto first_name = "track-" + snapshot.tracks[0].id.to_string() + ".wav";
  const auto second_name = "track-" + snapshot.tracks[1].id.to_string() + ".wav";
  const auto first = read_bytes(bundle / first_name);
  const auto second = read_bytes(bundle / second_name);
  REQUIRE_LT(std::abs(left(first, 24'000) - 0.375), oscillate::test::tolerance::float_round_trip,
             "routed stems leaked another source or emitted an empty bus stem");
  REQUIRE_LT(std::abs(left(first, 24'002) - 0.375), oscillate::test::tolerance::float_round_trip,
             "routed stems leaked another source or emitted an empty bus stem");
  REQUIRE_LT(std::abs(left(second, 24'000)), oscillate::test::tolerance::float_round_trip,
             "routed stems leaked another source or emitted an empty bus stem");
  REQUIRE_LT(std::abs(left(second, 24'002) - 0.25), oscillate::test::tolerance::float_round_trip,
             "routed stems leaked another source or emitted an empty bus stem");
  REQUIRE_EQ(u32(first, 52), u32(second, 52),
             "routed stems leaked another source or emitted an empty bus stem");
  REQUIRE_TRUE(!fs::exists(bundle / ("track-" + group.to_string() + ".wav")),
               "routed stems leaked another source or emitted an empty bus stem");
  REQUIRE_TRUE(!fs::exists(bundle / ("track-" + returned.to_string() + ".wav")),
               "routed stems leaked another source or emitted an empty bus stem");
  authority = take(StoppedExportAuthority::acquire(snapshot, gate));
  const auto repeat = directory.path() / "routed-stems-repeat";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, std::move(authority), {repeat})),
             PublishOutcome::durable, "routed source stems changed on repeat render");
  REQUIRE_EQ(read_bytes(repeat / first_name), first,
             "routed source stems changed on repeat render");
  REQUIRE_EQ(read_bytes(repeat / second_name), second,
             "routed source stems changed on repeat render");

  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  auto bypassed = snapshot;
  project::Vst3PluginInstance missing_plugin{.id = id<project::PluginInstanceId>(90),
                                             .class_id = std::string(32, '0'),
                                             .vendor = "Unavailable vendor",
                                             .name = "Unavailable effect",
                                             .version = "1",
                                             .bundle_path = "/missing/bypassed.vst3",
                                             .binary_sha256 = std::string(64, '0'),
                                             .component_state = {},
                                             .parameters = {},
                                             .missing = true};
  const auto audio_slot = test_insert::next_slot(bypassed);
  bypassed.tracks[0].effects.push_back({audio_slot, false, missing_plugin});
  bypassed.tracks[0].automation.push_back({id<project::AutomationLaneId>(92),
                                           project::Vst3ParameterTarget{missing_plugin.id, 1},
                                           {project::AutomationRepresentation::vst3_normalized,
                                            "plugin.parameter", "", 0.0, 1.0, 0.5, false, 0},
                                           project::AutomationTimebase::elapsed,
                                           {{id<project::AutomationPointId>(93), 0, 0.5}},
                                           1});
  missing_plugin.id = id<project::PluginInstanceId>(91);
  const auto bus_slot = test_insert::next_slot(bypassed);
  bypassed.tracks[2].effects.push_back({bus_slot, false, missing_plugin});
  const auto bypassed_master_path = directory.path() / "bypassed-vst3-master.wav";
  REQUIRE_EQ(take(render_wav_master(bypassed, store, active, {bypassed_master_path})),
             PublishOutcome::durable,
             "bypassed missing track/bus VST3 changed dry output or saved intent");
  REQUIRE_EQ(read_bytes(bypassed_master_path), master,
             "bypassed missing track/bus VST3 changed dry output or saved intent");
  REQUIRE_EQ(bypassed.tracks[0].effects.front().id, audio_slot,
             "bypassed missing track/bus VST3 changed dry output or saved intent");
  REQUIRE_EQ(bypassed.tracks[2].effects.front().id, bus_slot,
             "bypassed missing track/bus VST3 changed dry output or saved intent");
  REQUIRE_EQ(bypassed.tracks[0].automation.size(), 1,
             "bypassed missing track/bus VST3 changed dry output or saved intent");
  const auto bypassed_bundle = directory.path() / "bypassed-vst3-stems";
  REQUIRE_EQ(take(render_wav_stems(bypassed, store, active, {bypassed_bundle})),
             PublishOutcome::durable,
             "bypassed missing track/bus VST3 changed dry processed stems");
  REQUIRE_EQ(read_bytes(bypassed_bundle / first_name), first,
             "bypassed missing track/bus VST3 changed dry processed stems");
  REQUIRE_EQ(read_bytes(bypassed_bundle / second_name), second,
             "bypassed missing track/bus VST3 changed dry processed stems");
  auto enabled = bypassed;
  enabled.tracks[0].effects.front().enabled = true;
  REQUIRE_TRUE(!render_wav_master(enabled, store, active, {master_path}),
               "enabled missing VST3 effect rendered or replaced a dry output");
  REQUIRE_EQ(read_bytes(master_path), master,
             "enabled missing VST3 effect rendered or replaced a dry output");
  REQUIRE_TRUE(
      !render_wav_stems(enabled, store, active, {directory.path() / "enabled-missing-stems"}),
      "enabled missing VST3 effect rendered or replaced a dry output");
  REQUIRE_TRUE(!fs::exists(directory.path() / "enabled-missing-stems"),
               "enabled missing VST3 effect rendered or replaced a dry output");
  std::get<project::Vst3PluginInstance>(enabled.tracks[0].effects.front().processor).missing =
      false;
  REQUIRE_TRUE(!render_wav_master(enabled, store, active, {master_path}),
               "enabled unbound VST3 effect was admitted to ordinary native export");
  REQUIRE_EQ(read_bytes(master_path), master,
             "enabled unbound VST3 effect was admitted to ordinary native export");

  auto bus_reverb = snapshot;
  bus_reverb.tracks[3].effects.push_back(
      {test_insert::next_slot(bus_reverb), true, test_insert::expanse(0.2, 1.0)});
  const LiveRevisionState bus_active{
      bus_reverb.revision, bus_reverb.revision, RevisionApplication::active, {}};
  const auto bus_tail = test_insert::tail_frames(test_insert::expanse(0.2, 1.0), 48'000);
  const auto bus_path = directory.path() / "routed-return-reverb.wav";
  REQUIRE_EQ(take(render_wav_master(bus_reverb, store, bus_active, {bus_path})),
             PublishOutcome::durable, "saved return-bus reverb master failed");
  const auto bus_master = read_bytes(bus_path);
  REQUIRE_EQ(u32(bus_master, 52) / 8, 24'006 + bus_tail,
             "saved return-bus reverb tail was omitted or truncated");
  REQUIRE_GT(tail_peak(bus_master, 24'006, bus_tail), 0.0,
             "saved return-bus reverb tail was omitted or truncated");
  const auto bus_bundle = directory.path() / "routed-return-reverb-stems";
  REQUIRE_EQ(take(render_wav_stems(bus_reverb, store, bus_active, {bus_bundle})),
             PublishOutcome::durable, "saved return-bus reverb stems failed");
  const auto bus_first = read_bytes(bus_bundle / first_name);
  const auto bus_second = read_bytes(bus_bundle / second_name);
  REQUIRE_EQ(u32(bus_first, 52) / 8, 24'006 + bus_tail,
             "return-bus effect tail leaked across source stems");
  REQUIRE_EQ(u32(bus_second, 52), u32(bus_first, 52),
             "return-bus effect tail leaked across source stems");
  REQUIRE_GT(tail_peak(bus_first, 24'006, bus_tail), 0.0,
             "return-bus effect tail leaked across source stems");
  REQUIRE_LT(tail_peak(bus_second, 24'006, bus_tail), 1e-8,
             "return-bus effect tail leaked across source stems");
  REQUIRE_EQ(take(render_wav_master(bus_reverb, store, bus_active, {bus_path})),
             PublishOutcome::durable, "return-bus reverb repeat render changed samples");
  REQUIRE_EQ(read_bytes(bus_path), bus_master, "return-bus reverb repeat render changed samples");

  auto automated = snapshot;
  ++automated.revision;
  const project::AutomationDomain send_domain{project::AutomationRepresentation::physical,
                                              "mixer.send_gain",
                                              "linear_gain",
                                              0.0,
                                              4.0,
                                              0.5,
                                              false,
                                              0,
                                              project::AutomationScale::decibel};
  automated.tracks[0].automation.push_back({id<project::AutomationLaneId>(180),
                                            project::SendGainTarget{id<project::SendId>(79)},
                                            send_domain,
                                            project::AutomationTimebase::elapsed,
                                            {{id<project::AutomationPointId>(181), 0, 0.25}},
                                            1});
  automated.tracks[0].clips[0].automation.push_back(
      {id<project::AutomationLaneId>(182),
       project::SendGainTarget{id<project::SendId>(79)},
       send_domain,
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(183), 0, 1.0}},
       1});
  REQUIRE_EQ(take(store.save(automated, persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "send automation fixture did not save");
  automated = take(store.load_current()).snapshot;
  authority = take(StoppedExportAuthority::acquire(automated, gate));
  const auto automated_path = directory.path() / "send-automated-master.wav";
  REQUIRE_EQ(take(render_wav_master(automated, store, std::move(authority), {automated_path})),
             PublishOutcome::durable, "saved send automation did not render master");
  const auto automated_master = read_bytes(automated_path);
  REQUIRE_LT(std::abs(left(automated_master, 24'000) - 0.5),
             oscillate::test::tolerance::float_round_trip,
             "clip send envelope did not take precedence over track send automation");
  REQUIRE_LT(std::abs(left(automated_master, 24'002) - 0.75),
             oscillate::test::tolerance::float_round_trip,
             "clip send envelope did not take precedence over track send automation");
  authority = take(StoppedExportAuthority::acquire(automated, gate));
  const auto automated_bundle = directory.path() / "send-automated-stems";
  REQUIRE_EQ(take(render_wav_stems(automated, store, std::move(authority), {automated_bundle})),
             PublishOutcome::durable, "saved send automation did not render processed stems");
  const auto automated_first =
      read_bytes(automated_bundle / ("track-" + automated.tracks[0].id.to_string() + ".wav"));
  const auto automated_second =
      read_bytes(automated_bundle / ("track-" + automated.tracks[1].id.to_string() + ".wav"));
  REQUIRE_LT(std::abs(left(automated_first, 24'000) - 0.5),
             oscillate::test::tolerance::float_round_trip,
             "processed stems omitted send automation or leaked clip precedence");
  REQUIRE_LT(std::abs(left(automated_first, 24'002) - 0.5),
             oscillate::test::tolerance::float_round_trip,
             "processed stems omitted send automation or leaked clip precedence");
  REQUIRE_LT(std::abs(left(automated_second, 24'002) - 0.25),
             oscillate::test::tolerance::float_round_trip,
             "processed stems omitted send automation or leaked clip precedence");
  authority = take(StoppedExportAuthority::acquire(automated, gate));
  const auto repeated_path = directory.path() / "send-automated-repeat.wav";
  REQUIRE_EQ(take(render_wav_master(automated, store, std::move(authority), {repeated_path})),
             PublishOutcome::durable, "repeat send automation render changed samples");
  REQUIRE_EQ(read_bytes(repeated_path), automated_master,
             "repeat send automation render changed samples");

  auto missing_route = snapshot;
  missing_route.tracks[0].main_output = id<project::TrackId>(999);
  authority = take(StoppedExportAuthority::acquire(missing_route, gate));
  const auto failed_route =
      render_wav_master(missing_route, store, std::move(authority), {master_path});
  REQUIRE_TRUE(!failed_route, "missing route destination replaced an existing master");
  REQUIRE_EQ(failed_route.error().code, foundation::ErrorCode::not_found,
             "missing route destination replaced an existing master");
  REQUIRE_EQ(read_bytes(master_path), master,
             "missing route destination replaced an existing master");

  auto wet = snapshot;
  ++wet.revision;
  test_insert::set_master(wet, test_insert::expanse(0.2, 1.0));
  REQUIRE_EQ(take(store.save(wet, persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "routed wet fixture save failed");
  wet = take(store.load_current()).snapshot;
  const auto wet_path = directory.path() / "routed-wet.wav";
  authority = take(StoppedExportAuthority::acquire(wet, gate));
  REQUIRE_EQ(take(render_wav_master(wet, store, std::move(authority), {wet_path})),
             PublishOutcome::durable, "routed wet master did not publish");
  const auto wet_bytes = read_bytes(wet_path);
  const auto tail = test_insert::tail_frames(test_insert::expanse(0.2, 1.0), 48'000);
  REQUIRE_EQ(u32(wet_bytes, 52) / 8, 24'006 + tail, "routed master effect tail was truncated");
  REQUIRE_GT(tail_peak(wet_bytes, 24'006, tail), 0.0, "routed master effect tail was truncated");

  auto limited_bus = snapshot;
  limited_bus.tracks[3].effects.push_back(
      {test_insert::next_slot(limited_bus), true, test_insert::ceiling({-6.0, 1.0, 100.0})});
  const LiveRevisionState limited_bus_active{
      limited_bus.revision, limited_bus.revision, RevisionApplication::active, {}};
  const auto limited_bus_path = directory.path() / "routed-return-limiter.wav";
  REQUIRE_EQ(take(render_wav_master(limited_bus, store, limited_bus_active, {limited_bus_path})),
             PublishOutcome::durable, "return-bus limiter master did not render");
  const auto limited_bus_master = read_bytes(limited_bus_path);
  REQUIRE_EQ(u32(limited_bus_master, 52) / 8, 24'006,
             "return-bus limiter PDC shifted or lost the parallel dry path");
  REQUIRE_LT(std::abs(left(limited_bus_master, 24'000) - 0.375),
             oscillate::test::tolerance::float_round_trip,
             "return-bus limiter PDC shifted or lost the parallel dry path");
  REQUIRE_LT(std::abs(left(limited_bus_master, 24'002) - 0.625),
             oscillate::test::tolerance::float_round_trip,
             "return-bus limiter PDC shifted or lost the parallel dry path");
  const auto limited_bus_bundle = directory.path() / "routed-return-limiter-stems";
  REQUIRE_EQ(take(render_wav_stems(limited_bus, store, limited_bus_active, {limited_bus_bundle})),
             PublishOutcome::durable, "return-bus limiter stems did not render");
  const auto limited_bus_first = read_bytes(limited_bus_bundle / first_name);
  const auto limited_bus_second = read_bytes(limited_bus_bundle / second_name);
  REQUIRE_EQ(u32(limited_bus_first, 52) / 8, 24'006,
             "return-bus limiter stems lost timeline alignment or source isolation");
  REQUIRE_EQ(u32(limited_bus_second, 52), u32(limited_bus_first, 52),
             "return-bus limiter stems lost timeline alignment or source isolation");
  REQUIRE_LT(std::abs(left(limited_bus_first, 24'000) - 0.375),
             oscillate::test::tolerance::float_round_trip,
             "return-bus limiter stems lost timeline alignment or source isolation");
  REQUIRE_LT(std::abs(left(limited_bus_second, 24'002) - 0.25),
             oscillate::test::tolerance::float_round_trip,
             "return-bus limiter stems lost timeline alignment or source isolation");
  REQUIRE_EQ(take(render_wav_master(limited_bus, store, limited_bus_active, {limited_bus_path})),
             PublishOutcome::durable, "return-bus limiter repeat render changed samples");
  REQUIRE_EQ(read_bytes(limited_bus_path), limited_bus_master,
             "return-bus limiter repeat render changed samples");

  auto channel_automated = snapshot;
  channel_automated.revision = wet.revision + 1;
  channel_automated.tracks[0].level = 0.8;
  channel_automated.tracks[0].pan = 0.25;
  channel_automated.tracks[0].automation.push_back(
      {id<project::AutomationLaneId>(184),
       project::TrackControlTarget{channel_automated.tracks[0].id,
                                   project::TrackControlTarget::Key::channel_level},
       {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0,
        0.8, false, 0, project::AutomationScale::decibel},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(185), 0, 0.5}},
       1});
  REQUIRE_EQ(take(store.save(channel_automated, persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "channel automation fixture did not save");
  channel_automated = take(store.load_current()).snapshot;
  authority = take(StoppedExportAuthority::acquire(channel_automated, gate));
  const auto channel_path = directory.path() / "channel-automated-master.wav";
  REQUIRE_EQ(
      take(render_wav_master(channel_automated, store, std::move(authority), {channel_path})),
      PublishOutcome::durable, "saved channel automation did not render master");
  const auto channel_master = read_bytes(channel_path);
  REQUIRE_LT(std::abs(left(channel_master, 24'000) - 0.140625),
             oscillate::test::tolerance::float_round_trip,
             "channel automation did not precede both main and send routing");
  REQUIRE_LT(std::abs(left(channel_master, 24'002) - 0.390625),
             oscillate::test::tolerance::float_round_trip,
             "channel automation did not precede both main and send routing");
  authority = take(StoppedExportAuthority::acquire(channel_automated, gate));
  const auto channel_bundle = directory.path() / "channel-automated-stems";
  REQUIRE_EQ(
      take(render_wav_stems(channel_automated, store, std::move(authority), {channel_bundle})),
      PublishOutcome::durable, "saved channel automation did not render stems");
  const auto channel_first =
      read_bytes(channel_bundle / ("track-" + channel_automated.tracks[0].id.to_string() + ".wav"));
  REQUIRE_LT(std::abs(left(channel_first, 24'000) - 0.140625),
             oscillate::test::tolerance::float_round_trip,
             "processed stem omitted channel automation");

  auto clip_channel = channel_automated;
  ++clip_channel.revision;
  clip_channel.tracks[0].clips.front().automation.push_back(
      {id<project::AutomationLaneId>(186),
       project::TrackControlTarget{clip_channel.tracks[0].id,
                                   project::TrackControlTarget::Key::channel_level},
       {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0,
        0.8, false, 0, project::AutomationScale::decibel},
       project::AutomationTimebase::musical,
       {{id<project::AutomationPointId>(187), 0, 0.25}},
       1});
  REQUIRE_EQ(take(store.save(clip_channel, persistence::SaveKind::explicit_save)).status,
             persistence::SaveStatus::committed, "clip channel envelope fixture did not save");
  clip_channel = take(store.load_current()).snapshot;
  authority = take(StoppedExportAuthority::acquire(clip_channel, gate));
  const auto clip_channel_path = directory.path() / "clip-channel-master.wav";
  REQUIRE_EQ(
      take(render_wav_master(clip_channel, store, std::move(authority), {clip_channel_path})),
      PublishOutcome::durable, "saved clip channel envelope did not render master");
  const auto clip_channel_master = read_bytes(clip_channel_path);
  REQUIRE_LT(std::abs(left(clip_channel_master, 24'000) - left(channel_master, 24'000) * 0.5),
             oscillate::test::tolerance::float_round_trip,
             "clip envelope did not override the track channel curve during export");
  authority = take(StoppedExportAuthority::acquire(clip_channel, gate));
  const auto clip_channel_bundle = directory.path() / "clip-channel-stems";
  REQUIRE_EQ(
      take(render_wav_stems(clip_channel, store, std::move(authority), {clip_channel_bundle})),
      PublishOutcome::durable, "saved clip channel envelope did not render stems");
  const auto clip_channel_first =
      read_bytes(clip_channel_bundle / ("track-" + clip_channel.tracks[0].id.to_string() + ".wav"));
  const auto clip_channel_second =
      read_bytes(clip_channel_bundle / ("track-" + clip_channel.tracks[1].id.to_string() + ".wav"));
  const auto channel_second =
      read_bytes(channel_bundle / ("track-" + channel_automated.tracks[1].id.to_string() + ".wav"));
  REQUIRE_LT(std::abs(left(clip_channel_first, 24'000) - left(channel_first, 24'000) * 0.5),
             oscillate::test::tolerance::float_round_trip,
             "clip envelope changed the wrong processed stem");
  REQUIRE_EQ(clip_channel_second, channel_second, "clip envelope changed the wrong processed stem");
}

void test_output_sample_rates() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  constexpr std::uint32_t source_rate = oscillate::test::sample_rate_48k;
  constexpr std::uint32_t source_frames = 4'800;
  constexpr double frequency = 440.0;
  const auto source_path = directory.path() / "sine.wav";
  write_bytes(source_path, sine_fixture_wav({source_rate, source_frames, frequency}));
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "rate fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(81), fd);
  REQUIRE_EQ(::close(fd), 0, "rate fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  snapshot.tracks.erase(snapshot.tracks.begin() + 1, snapshot.tracks.end());
  snapshot.tracks[0].clips.erase(snapshot.tracks[0].clips.begin() + 1,
                                 snapshot.tracks[0].clips.end());
  snapshot.tracks[0].clips[0].source_range = {{0}, {source_frames}};
  snapshot.tracks[0].clips[0].placement = project::ElapsedInterval{{0}, {source_frames}};
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "rate fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto target = directory.path() / "resampled.wav";
  const auto same_rate = directory.path() / "same-rate.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {same_rate})), PublishOutcome::durable,
             "same-rate baseline failed");
  const auto same_bytes = read_bytes(same_rate);
  REQUIRE_EQ(u32(same_bytes, 24), source_rate, "same-rate baseline metadata differs");
  REQUIRE_EQ(u32(same_bytes, 52) / 8, source_frames, "same-rate baseline metadata differs");
  for (const auto target_rate : {44'100U, 88'200U, 96'000U}) {
    const MasterRenderOptions options{target, WavFormat::float32, 14, target_rate};
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
    REQUIRE_EQ(take(render_wav_master(snapshot, store, active, options)), PublishOutcome::durable,
               "converted WAV did not publish");
    const auto bytes = read_bytes(target);
    const auto expected_frames = (source_frames * target_rate + source_rate / 2) / source_rate;
    REQUIRE_EQ(u32(bytes, 24), target_rate, "converted duration or header is incorrect");
    REQUIRE_EQ(u32(bytes, 52) / 8, expected_frames, "converted duration or header is incorrect");
    REQUIRE_EQ(bytes.size(), 56 + expected_frames * 8, "converted duration or header is incorrect");
    // This sinusoidal oracle depends only on physical frequency and output
    // time, not on the converter's coefficient table or algorithm.
    for (std::uint32_t frame = target_rate / 50; frame < target_rate / 20; frame += 37) {
      const auto expected =
          12'000.0 / 32'768.0 * std::sin(2.0 * std::numbers::pi * frequency * frame / target_rate);
      REQUIRE_LT(std::abs(sample(bytes, 56 + frame * 8, WavFormat::float32) - expected), 0.003,
                 "output rate changed tone pitch or timing");
    }
    REQUIRE_EQ(take(render_wav_master(snapshot, store, active, options)), PublishOutcome::durable,
               "repeat rate conversion is not deterministic");
    REQUIRE_EQ(read_bytes(target), bytes, "repeat rate conversion is not deterministic");
    auto stale = active;
    stale.active_graph_revision = 0;
    REQUIRE_TRUE(!render_wav_master(snapshot, store, stale, options),
                 "stale converted render changed prior output");
    REQUIRE_EQ(read_bytes(target), bytes, "stale converted render changed prior output");
#else
    require(!render_wav_master(snapshot, store, active, options) && !fs::exists(target),
            "conversion without dependency published output");
#endif
  }
  const MasterRenderOptions invalid{target, WavFormat::float32, 14, 22'050};
  require(!render_wav_master(snapshot, store, active, invalid),
          "unsupported output sample rate rendered");
#if OSCILLATE_HAVE_CODEC && OSCILLATE_HAVE_EXPORT_RESAMPLER
  // Decode every supported encoded/rate combination and compare lossless
  // samples against the independently parsed float WAV at the same output
  // rate. The 48 kHz variants are covered by test_mix_formats_and_guards.
  for (const auto rate : {44'100U, 88'200U, 96'000U}) {
    const auto reference_path = directory.path() / "encoded-reference.wav";
    REQUIRE_EQ(take(render_wav_master(snapshot, store, active,
                                      {reference_path, WavFormat::float32, 14, rate})),
               PublishOutcome::durable, "encoded rate reference did not publish");
    const auto reference = read_bytes(reference_path);
    const auto expected_frames = (source_frames * rate + source_rate / 2) / source_rate;
    for (const auto format :
         {EncodedMasterFormat::flac16, EncodedMasterFormat::flac24, EncodedMasterFormat::mp3}) {
      if (format == EncodedMasterFormat::mp3 && rate != 44'100) {
        continue; // MP3 deliberately supports only 44.1 and 48 kHz.
      }
      const auto output_path =
          directory.path() /
          (format == EncodedMasterFormat::mp3 ? "resampled.mp3" : "resampled.flac");
      const EncodedMasterOptions options{output_path, format, 14, 0.5, rate};
      REQUIRE_EQ(take(render_encoded_master(snapshot, store, active, options)),
                 PublishOutcome::durable, "converted encoded master did not publish");
      const auto original = read_bytes(output_path);
      SF_INFO info{};
      auto* decoder = sf_open(output_path.c_str(), SFM_READ, &info);
      const auto expected_type =
          format == EncodedMasterFormat::mp3 ? SF_FORMAT_MPEG : SF_FORMAT_FLAC;
      const auto expected_subtype = format == EncodedMasterFormat::flac16 ? SF_FORMAT_PCM_16
                                    : format == EncodedMasterFormat::flac24
                                        ? SF_FORMAT_PCM_24
                                        : SF_FORMAT_MPEG_LAYER_III;
      REQUIRE_NE(decoder, nullptr, "converted encoded format, bit depth, rate or duration differs");
      REQUIRE_EQ(info.samplerate, static_cast<int>(rate),
                 "converted encoded format, bit depth, rate or duration differs");
      REQUIRE_EQ(info.channels, 2, "converted encoded format, bit depth, rate or duration differs");
      REQUIRE_EQ(info.frames, expected_frames,
                 "converted encoded format, bit depth, rate or duration differs");
      REQUIRE_EQ((info.format & SF_FORMAT_TYPEMASK), expected_type,
                 "converted encoded format, bit depth, rate or duration differs");
      REQUIRE_EQ((info.format & SF_FORMAT_SUBMASK), expected_subtype,
                 "converted encoded format, bit depth, rate or duration differs");
      std::vector<float> decoded(static_cast<std::size_t>(expected_frames) * 2);
      REQUIRE_EQ(sf_readf_float(decoder, decoded.data(), expected_frames), expected_frames,
                 "converted encoded master did not decode completely");
      REQUIRE_EQ(sf_close(decoder), 0, "converted encoded master did not decode completely");
      double squared_error = 0.0;
      for (std::size_t index = 0; index < decoded.size(); ++index) {
        const auto expected = sample(reference, 56 + index * 4, WavFormat::float32);
        const auto delta = static_cast<double>(decoded[index]) - expected;
        squared_error += delta * delta;
        if (format != EncodedMasterFormat::mp3) {
          const auto tolerance =
              format == EncodedMasterFormat::flac16 ? 2.0 / 32'768.0 : 2.0 / 8'388'608.0;
          REQUIRE_LE(std::abs(delta), tolerance,
                     "converted FLAC sample differs from published float reference");
        }
      }
      if (format == EncodedMasterFormat::mp3) {
        REQUIRE_LT(std::sqrt(squared_error / static_cast<double>(decoded.size())), 0.08,
                   "converted MP3 differs from published float reference");
      }
      REQUIRE_EQ(take(render_encoded_master(snapshot, store, active, options)),
                 PublishOutcome::durable, "converted encoded master differs on repeat render");
      REQUIRE_EQ(read_bytes(output_path), original,
                 "converted encoded master differs on repeat render");
    }
  }
  const auto encoded_target = directory.path() / "resampled.flac";
  REQUIRE_EQ(
      take(render_encoded_master(snapshot, store, active,
                                 {encoded_target, EncodedMasterFormat::flac24, 14, 0.5, 44'100})),
      PublishOutcome::durable, "converted FLAC did not publish");
  SF_INFO converted_info{};
  auto* encoded = sf_open(encoded_target.c_str(), SFM_READ, &converted_info);
  REQUIRE_NE(encoded, nullptr, "converted FLAC cannot be opened");
  const bool converted_metadata =
      converted_info.samplerate == 44'100 && converted_info.frames == 4'410;
  REQUIRE_EQ(sf_close(encoded), 0, "converted FLAC metadata or duration differs");
  REQUIRE_TRUE(converted_metadata, "converted FLAC metadata or duration differs");
  const auto prior_encoded = read_bytes(encoded_target);
  auto bypassed_encoded = snapshot;
  test_insert::set_master(bypassed_encoded, test_insert::shape({native_dsp::EqBandKind::low_pass,
                                                                1'000.0, 0.707, 0.0}));
  bypassed_encoded.master_effects.front().enabled = false;
  REQUIRE_EQ(
      take(render_encoded_master(bypassed_encoded, store, active,
                                 {encoded_target, EncodedMasterFormat::flac24, 14, 0.5, 44'100})),
      PublishOutcome::durable, "bypassed native master insert changed encoded output");
  REQUIRE_EQ(read_bytes(encoded_target), prior_encoded,
             "bypassed native master insert changed encoded output");

  auto with_tail = snapshot;
  test_insert::set_master(with_tail, test_insert::expanse(0.2, 1.0));
  const auto tail_reference_path = directory.path() / "encoded-tail-reference.wav";
  REQUIRE_EQ(take(render_wav_master(with_tail, store, active,
                                    {tail_reference_path, WavFormat::float32, 14, 44'100})),
             PublishOutcome::durable, "tail reference did not publish");
  const auto tail_reference = read_bytes(tail_reference_path);
  const auto tail_frames = u32(tail_reference, 52) / 8;
  REQUIRE_GT(tail_frames, 4'410, "native reverb did not extend the encoded tail fixture");
  for (const auto format : {EncodedMasterFormat::flac24, EncodedMasterFormat::mp3}) {
    const auto tail_path =
        directory.path() /
        (format == EncodedMasterFormat::mp3 ? "encoded-tail.mp3" : "encoded-tail.flac");
    const EncodedMasterOptions options{tail_path, format, 14, 0.5, 44'100};
    REQUIRE_EQ(take(render_encoded_master(with_tail, store, active, options)),
               PublishOutcome::durable, "encoded master with native effect tail did not publish");
    const auto original = read_bytes(tail_path);
    SF_INFO info{};
    auto* decoder = sf_open(tail_path.c_str(), SFM_READ, &info);
    REQUIRE_NE(decoder, nullptr, "encoded master lost effect-tail duration");
    REQUIRE_EQ(info.frames, tail_frames, "encoded master lost effect-tail duration");
    REQUIRE_EQ(info.samplerate, 44'100, "encoded master lost effect-tail duration");
    std::vector<float> samples(static_cast<std::size_t>(tail_frames) * 2);
    REQUIRE_EQ(sf_readf_float(decoder, samples.data(), tail_frames), tail_frames,
               "encoded effect tail did not decode completely");
    REQUIRE_EQ(sf_close(decoder), 0, "encoded effect tail did not decode completely");
    double reference_energy = 0.0;
    double decoded_energy = 0.0;
    for (std::uint32_t frame = 4'410; frame < tail_frames; ++frame) {
      const auto reference = sample(tail_reference, 56 + frame * 8, WavFormat::float32);
      const auto decoded = static_cast<double>(samples[frame * 2]);
      reference_energy += reference * reference;
      decoded_energy += decoded * decoded;
      if (format == EncodedMasterFormat::flac24) {
        REQUIRE_NEAR(decoded, reference, 2.0 / 8'388'608.0,
                     "FLAC effect tail differs from published float reference");
      }
    }
    REQUIRE_GT(reference_energy, 1e-5, "encoded master omitted audible reverb tail");
    REQUIRE_GT(decoded_energy, 1e-5, "encoded master omitted audible reverb tail");
    REQUIRE_EQ(take(render_encoded_master(with_tail, store, active, options)),
               PublishOutcome::durable, "encoded effect tail changed on repeat render");
    REQUIRE_EQ(read_bytes(tail_path), original, "encoded effect tail changed on repeat render");
  }
#endif
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  const auto alias_source = directory.path() / "above-output-nyquist.wav";
  write_bytes(alias_source, sine_fixture_wav({source_rate, source_frames, 23'000.0}));
  const int alias_fd = ::open(alias_source.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(alias_fd, 0, "alias fixture open failed");
  auto alias_staged = store.stage_wav(id<project::SourceId>(82), alias_fd);
  REQUIRE_EQ(::close(alias_fd), 0, "alias fixture close failed");
  auto alias_receipt = take(std::move(alias_staged));
  auto alias_snapshot = snapshot;
  ++alias_snapshot.revision;
  alias_snapshot.sources = {alias_receipt.record()};
  alias_snapshot.tracks[0].clips[0].source = alias_receipt.record().id;
  REQUIRE_EQ(take(store.save(alias_snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&alias_receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "alias fixture save failed");
  alias_snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState alias_active{
      alias_snapshot.revision, alias_snapshot.revision, RevisionApplication::active, {}};
  const auto alias_target = directory.path() / "alias-test.wav";
  REQUIRE_EQ(take(render_wav_master(alias_snapshot, store, alias_active,
                                    {alias_target, WavFormat::float32, 0, 44'100})),
             PublishOutcome::durable, "alias test conversion failed");
  const auto alias_bytes = read_bytes(alias_target);
  double power = 0.0;
  for (std::size_t frame = 1'000; frame < 3'000; ++frame) {
    const auto value = sample(alias_bytes, 56 + frame * 8, WavFormat::float32);
    power += value * value;
  }
  REQUIRE_LT(std::sqrt(power / 2'000.0), 0.02,
             "downsampling did not suppress a tone above output Nyquist");

  // A 75-frame 2:1 render rounds to 38 frames. The sinc converter supplies
  // 37; the final sample belongs to the silent region beyond the source.
  auto short_store =
      take(persistence::ProjectStore::open_writer(directory.path() / "short-project", true));
  const auto short_source = directory.path() / "short.wav";
  write_bytes(short_source, sine_fixture_wav({96'000, 75, 1'000.0}));
  const int short_fd = ::open(short_source.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(short_fd, 0, "short fixture open failed");
  auto short_staged = short_store.stage_wav(id<project::SourceId>(83), short_fd);
  REQUIRE_EQ(::close(short_fd), 0, "short fixture close failed");
  auto short_receipt = take(std::move(short_staged));
  auto short_snapshot = make_snapshot(short_receipt.record());
  short_snapshot.sample_rate = oscillate::test::sample_rate_96k;
  short_snapshot.tracks.erase(short_snapshot.tracks.begin() + 1, short_snapshot.tracks.end());
  short_snapshot.tracks[0].clips.erase(short_snapshot.tracks[0].clips.begin() + 1,
                                       short_snapshot.tracks[0].clips.end());
  short_snapshot.tracks[0].clips[0].source_range = {{0}, {75}};
  short_snapshot.tracks[0].clips[0].placement = project::ElapsedInterval{{0}, {75}};
  REQUIRE_EQ(take(short_store.save(short_snapshot, persistence::SaveKind::explicit_save,
                                   std::span<const persistence::SourceReceipt>(&short_receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "short fixture save failed");
  short_snapshot = take(short_store.load_current()).snapshot;
  const LiveRevisionState short_active{
      short_snapshot.revision, short_snapshot.revision, RevisionApplication::active, {}};
  const auto short_target = directory.path() / "short-output.wav";
  REQUIRE_EQ(take(render_wav_master(short_snapshot, short_store, short_active,
                                    {short_target, WavFormat::float32, 0, 48'000})),
             PublishOutcome::durable, "rounded-duration converted export failed");
  const auto short_bytes = read_bytes(short_target);
  REQUIRE_EQ(u32(short_bytes, 52) / 8, 38, "rounded output duration or terminal silence differs");
  REQUIRE_EQ(sample(short_bytes, 56 + 37 * 8, WavFormat::float32), 0.0,
             "rounded output duration or terminal silence differs");
#endif
}

double loudness_or_nan(const auto& report) {
  return report.integrated_lkfs.value_or(std::numeric_limits<double>::quiet_NaN());
}

void check_published_pcm(const fs::path& path, const RenderLoudnessReport& report,
                         std::uint32_t expected_rate) {
  const auto bytes = read_bytes(path);
  REQUIRE_EQ(u32(bytes, 24), expected_rate,
             "reported loudness duration differs from published WAV");
  REQUIRE_EQ(u32(bytes, 52) / 8, report.frames,
             "reported loudness duration differs from published WAV");
  native_dsp::IntegratedLoudnessAnalyzer replay(expected_rate, native_dsp::LoudnessLayout::stereo);
  native_dsp::StereoTruePeakMeter peak_replay;
  require(peak_replay.prepare(expected_rate), "published WAV true-peak rate was rejected");
  std::array<float, 2'048> decoded{};
  std::array<engine::StereoFrame, 1'024> peak_frames{};
  for (std::size_t frame = 0; frame < report.frames; frame += decoded.size() / 2) {
    const auto count = std::min<std::size_t>(decoded.size() / 2, report.frames - frame);
    for (std::size_t index = 0; index < count * 2; ++index) {
      decoded[index] =
          static_cast<float>(sample(bytes, 56 + (frame * 2 + index) * 4, WavFormat::float32));
    }
    REQUIRE_EQ(replay.append(std::span(decoded).first(count * 2)), native_dsp::LoudnessStatus::ok,
               "published WAV could not be analyzed");
    for (std::size_t index = 0; index < count; ++index) {
      peak_frames[index] = {decoded[index * 2], decoded[index * 2 + 1]};
    }
    require(peak_replay.process(std::span(peak_frames).first(count)),
            "published WAV true peak could not be analyzed");
  }
  const auto replayed = replay.finish();
  require(peak_replay.finish(), "published WAV true peak could not finish");
  const auto peaks = peak_replay.reading();
  REQUIRE_OK(replayed.integrated_lkfs, "loudness tap differs from published float PCM samples");
  REQUIRE_LT(std::abs(loudness_or_nan(replayed) - loudness_or_nan(report)),
             oscillate::test::tolerance::double_accumulation,
             "loudness tap differs from published float PCM samples");
  REQUIRE_EQ(replayed.momentary_lkfs, report.final_momentary_lkfs,
             "rolling loudness tap differs from published float PCM samples");
  REQUIRE_EQ(replayed.short_term_lkfs, report.final_short_term_lkfs,
             "rolling loudness tap differs from published float PCM samples");
  REQUIRE_EQ(peaks.frames, report.frames, "true-peak tap differs from published float PCM samples");
  REQUIRE_EQ(peaks.left_sample_peak, report.left_sample_peak,
             "true-peak tap differs from published float PCM samples");
  REQUIRE_EQ(peaks.right_sample_peak, report.right_sample_peak,
             "true-peak tap differs from published float PCM samples");
  REQUIRE_EQ(peaks.left_true_peak, report.left_true_peak,
             "true-peak tap differs from published float PCM samples");
  REQUIRE_EQ(peaks.right_true_peak, report.right_true_peak,
             "true-peak tap differs from published float PCM samples");
  REQUIRE_GE(report.left_true_peak, report.left_sample_peak,
             "true-peak tap differs from published float PCM samples");
  REQUIRE_GE(report.right_true_peak, report.right_sample_peak,
             "true-peak tap differs from published float PCM samples");
}

void test_export_loudness_reports() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  constexpr std::uint32_t frames = 96'000;
  const auto source_path = directory.path() / "tone.wav";
  write_bytes(source_path, sine_fixture_wav({48'000, frames, 1'000.0}));
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "loudness fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(90), fd);
  REQUIRE_EQ(::close(fd), 0, "loudness fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  for (auto& track : snapshot.tracks) {
    track.clips.erase(track.clips.begin() + 1, track.clips.end());
    track.clips[0].source_range = {{0}, {frames}};
    track.clips[0].placement = project::ElapsedInterval{{0}, {frames}};
  }
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "loudness fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};

  const auto master_path = directory.path() / "loudness-master.wav";
  RenderLoudnessReport master_report;
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active,
                                    {master_path, WavFormat::float32, 0, 48'000, &master_report})),
             PublishOutcome::durable, "measured master did not publish");
  REQUIRE_EQ(master_report.frames, frames,
             "master report omitted the published duration or loudness");
  REQUIRE_OK(master_report.integrated_lkfs,
             "master report omitted the published duration or loudness");

  const auto bundle = directory.path() / "loudness-stems";
  std::vector<StemLoudnessReport> stem_reports;
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active,
                                   {bundle, WavFormat::float32, 0, 48'000, &stem_reports})),
             PublishOutcome::durable, "measured stems did not publish");
  REQUIRE_EQ(stem_reports.size(), 2, "stem reports lost source order or published duration");
  REQUIRE_EQ(stem_reports[0].track, snapshot.tracks[0].id,
             "stem reports lost source order or published duration");
  REQUIRE_EQ(stem_reports[1].track, snapshot.tracks[1].id,
             "stem reports lost source order or published duration");
  REQUIRE_EQ(stem_reports[0].loudness.frames, frames,
             "stem reports lost source order or published duration");
  REQUIRE_EQ(stem_reports[1].loudness.frames, frames,
             "stem reports lost source order or published duration");
  REQUIRE_OK(stem_reports[0].loudness.integrated_lkfs,
             "stem reports lost source order or published duration");
  REQUIRE_OK(stem_reports[1].loudness.integrated_lkfs,
             "stem reports lost source order or published duration");
  REQUIRE_OK(master_report.final_momentary_lkfs,
             "stem reports lost source order or published duration");
  REQUIRE_TRUE(!master_report.final_short_term_lkfs.has_value(),
               "stem reports lost source order or published duration");
  const double expected_gain_db = 20.0 * std::log10(2.0);
  REQUIRE_LT(std::abs(loudness_or_nan(master_report) - loudness_or_nan(stem_reports[0].loudness) -
                      expected_gain_db),
             0.02, "master/stem loudness did not follow the independent two-source level oracle");
  REQUIRE_LT(std::abs(loudness_or_nan(stem_reports[0].loudness) -
                      loudness_or_nan(stem_reports[1].loudness)),
             oscillate::test::tolerance::double_accumulation,
             "master/stem loudness did not follow the independent two-source level oracle");
  check_published_pcm(master_path, master_report, 48'000);
  for (std::size_t index = 0; index < stem_reports.size(); ++index) {
    check_published_pcm(bundle / ("track-" + stem_reports[index].track.to_string() + ".wav"),
                        stem_reports[index].loudness, 48'000);
  }

#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  for (const auto output_rate : {44'100U, 88'200U, 96'000U}) {
    RenderLoudnessReport converted_output_report;
    const auto converted_output_path =
        directory.path() / ("converted-" + std::to_string(output_rate) + ".wav");
    REQUIRE_EQ(take(render_wav_master(snapshot, store, active,
                                      {converted_output_path, WavFormat::float32, 0, output_rate,
                                       &converted_output_report})),
               PublishOutcome::durable, "converted output-rate loudness report omitted frames");
    REQUIRE_EQ(converted_output_report.frames, 2ULL * output_rate,
               "converted output-rate loudness report omitted frames");
    REQUIRE_OK(converted_output_report.integrated_lkfs,
               "converted output-rate loudness report omitted frames");
    check_published_pcm(converted_output_path, converted_output_report, output_rate);
    REQUIRE_LT(std::abs(loudness_or_nan(converted_output_report) - loudness_or_nan(master_report)),
               0.05, "output-rate conversion changed steady tone loudness unexpectedly");
  }
  std::vector<StemLoudnessReport> converted_stem_reports;
  const auto converted_stem_bundle = directory.path() / "converted-44100-stems";
  REQUIRE_EQ(take(render_wav_stems(
                 snapshot, store, active,
                 {converted_stem_bundle, WavFormat::float32, 0, 44'100, &converted_stem_reports})),
             PublishOutcome::durable, "converted stem bundle omitted loudness reports");
  REQUIRE_EQ(converted_stem_reports.size(), 2, "converted stem bundle omitted loudness reports");
  for (const auto& stem : converted_stem_reports) {
    check_published_pcm(converted_stem_bundle / ("track-" + stem.track.to_string() + ".wav"),
                        stem.loudness, 44'100);
  }

  const auto converted_source_path = directory.path() / "tone-96k.wav";
  write_bytes(converted_source_path, sine_fixture_wav({96'000, 96'000, 1'000.0}));
  const int converted_fd = ::open(converted_source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(converted_fd, 0, "converted loudness fixture open failed");
  auto converted_receipt = take(store.stage_wav(id<project::SourceId>(91), converted_fd));
  REQUIRE_EQ(::close(converted_fd), 0, "converted loudness fixture close failed");
  auto converted_snapshot = make_snapshot(converted_receipt.record());
  converted_snapshot.sample_rate = oscillate::test::sample_rate_96k;
  converted_snapshot.tracks.erase(converted_snapshot.tracks.begin() + 1,
                                  converted_snapshot.tracks.end());
  converted_snapshot.tracks[0].clips.erase(converted_snapshot.tracks[0].clips.begin() + 1,
                                           converted_snapshot.tracks[0].clips.end());
  converted_snapshot.tracks[0].clips[0].source_range = {{0}, {96'000}};
  converted_snapshot.tracks[0].clips[0].placement = project::ElapsedInterval{{0}, {96'000}};
  REQUIRE_EQ(take(store.save(converted_snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&converted_receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "converted loudness fixture save failed");
  converted_snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState converted_active{
      converted_snapshot.revision, converted_snapshot.revision, RevisionApplication::active, {}};
  RenderLoudnessReport converted_report;
  const auto converted_path = directory.path() / "converted-loudness.wav";
  REQUIRE_EQ(
      take(render_wav_master(converted_snapshot, store, converted_active,
                             {converted_path, WavFormat::float32, 0, 48'000, &converted_report})),
      PublishOutcome::durable, "converted 48 kHz loudness report omitted output frames");
  REQUIRE_EQ(converted_report.frames, 48'000,
             "converted 48 kHz loudness report omitted output frames");
  REQUIRE_OK(converted_report.integrated_lkfs,
             "converted 48 kHz loudness report omitted output frames");
  check_published_pcm(converted_path, converted_report, 48'000);
#endif

  RenderLoudnessReport untouched{7, 1.0, 2.0, 3.0, 4.0F, 5.0F, 6.0F, 7.0F};
  const auto rejected_path = directory.path() / "unsupported-loudness.wav";
  REQUIRE_TRUE(!render_wav_master(snapshot, store, active,
                                  {rejected_path, WavFormat::pcm24, 0, 48'000, &untouched}),
               "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_TRUE(!fs::exists(rejected_path),
               "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.frames, 7,
             "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.integrated_lkfs, 1.0,
             "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.final_momentary_lkfs, 2.0,
             "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.final_short_term_lkfs, 3.0,
             "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.left_sample_peak, 4.0F,
             "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.right_sample_peak, 5.0F,
             "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.left_true_peak, 6.0F,
             "unsupported output published or changed an uncommitted loudness report");
  REQUIRE_EQ(untouched.right_true_peak, 7.0F,
             "unsupported output published or changed an uncommitted loudness report");
  std::vector<StemLoudnessReport> untouched_stems = stem_reports;
  const auto rejected_bundle = directory.path() / "unsupported-loudness-stems";
  REQUIRE_TRUE(
      !render_wav_stems(snapshot, store, active,
                        {rejected_bundle, WavFormat::float32, 0, 22'050, &untouched_stems}),
      "invalid stem rate published or changed reports");
  REQUIRE_TRUE(!fs::exists(rejected_bundle), "invalid stem rate published or changed reports");
  REQUIRE_EQ(untouched_stems.size(), stem_reports.size(),
             "invalid stem rate published or changed reports");
  const auto interrupted_bundle = directory.path() / "interrupted-loudness-stems";
  auto interrupted = detail::render_wav_stems_with_after_stem_hook(
      snapshot, store, active,
      {interrupted_bundle, WavFormat::float32, 0, 48'000, &untouched_stems},
      [](std::size_t) -> foundation::Result<void> {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::io_failure, "Injected stem failure"});
      });
  REQUIRE_TRUE(!interrupted, "failed stem bundle published a partial report");
  REQUIRE_TRUE(!fs::exists(interrupted_bundle), "failed stem bundle published a partial report");
  REQUIRE_EQ(untouched_stems.size(), stem_reports.size(),
             "failed stem bundle published a partial report");
  REQUIRE_EQ(untouched_stems[0].track, stem_reports[0].track,
             "failed stem bundle published a partial report");
  REQUIRE_EQ(untouched_stems[0].loudness.integrated_lkfs, stem_reports[0].loudness.integrated_lkfs,
             "failed stem bundle published a partial report");
}

void test_export_intersample_peak_report() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  constexpr std::uint32_t frames = 9'600;
  const auto source_path = directory.path() / "quarter-rate.wav";
  write_bytes(source_path, sine_fixture_wav({rate, frames, rate / 4.0, std::numbers::pi / 4.0}));
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "intersample source open failed");
  auto staged = store.stage_wav(id<project::SourceId>(90), fd);
  REQUIRE_EQ(::close(fd), 0, "intersample source close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  snapshot.tracks.erase(snapshot.tracks.begin() + 1, snapshot.tracks.end());
  auto& clips = snapshot.tracks.front().clips;
  clips.erase(clips.begin() + 1, clips.end());
  clips.front().source_range = {{0}, {frames}};
  clips.front().placement = project::ElapsedInterval{{0}, {frames}};
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "intersample fixture save failed");
  snapshot = take(store.load_current()).snapshot;
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  RenderLoudnessReport report;
  const auto output = directory.path() / "intersample.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active,
                                    {output, WavFormat::float32, 0, rate, &report})),
             PublishOutcome::durable, "intersample master did not publish");
  // The continuous sine reaches 12,000/32,768 between samples, while its
  // quarter-rate digital samples top out at that amplitude divided by sqrt(2).
  require(report.frames == frames && report.left_sample_peak > 0.25F &&
              report.left_sample_peak < 0.27F && report.left_true_peak > 0.35F &&
              report.left_true_peak < 0.39F &&
              report.left_sample_peak == report.right_sample_peak &&
              report.left_true_peak == report.right_true_peak,
          "published intersample peak did not match the independent sine oracle");
}

void test_multirate_export_loudness_reports() {
  for (const auto rate : {44'100U, 88'200U, 96'000U}) {
    Directory directory;
    auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
    const auto source_path = directory.path() / "tone.wav";
    write_bytes(source_path, sine_fixture_wav({rate, rate, 1'000.0}));
    const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "multirate loudness source open failed");
    auto staged = store.stage_wav(id<project::SourceId>(90), fd);
    REQUIRE_EQ(::close(fd), 0, "multirate loudness source close failed");
    auto receipt = take(std::move(staged));
    auto snapshot = make_snapshot(receipt.record());
    snapshot.sample_rate = rate;
    for (auto& track : snapshot.tracks) {
      track.clips.erase(track.clips.begin() + 1, track.clips.end());
      track.clips[0].source_range = {{0}, {rate}};
      track.clips[0].placement = project::ElapsedInterval{{0}, {rate}};
    }
    REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                               std::span<const persistence::SourceReceipt>(&receipt, 1)))
                   .status,
               persistence::SaveStatus::committed, "multirate loudness fixture save failed");
    snapshot = take(store.load_current()).snapshot;
    const LiveRevisionState active{
        snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
    RenderLoudnessReport master_report;
    const auto master_path = directory.path() / "master.wav";
    REQUIRE_EQ(take(render_wav_master(snapshot, store, active,
                                      {master_path, WavFormat::float32, 0, 0, &master_report})),
               PublishOutcome::durable, "multirate master did not report its published frames");
    REQUIRE_EQ(master_report.frames, rate, "multirate master did not report its published frames");
    REQUIRE_OK(master_report.integrated_lkfs,
               "multirate master did not report its published frames");
    check_published_pcm(master_path, master_report, rate);
    std::vector<StemLoudnessReport> stems;
    const auto bundle = directory.path() / "stems";
    REQUIRE_EQ(
        take(render_wav_stems(snapshot, store, active, {bundle, WavFormat::float32, 0, 0, &stems})),
        PublishOutcome::durable, "multirate stems lost their reports or duration");
    REQUIRE_EQ(stems.size(), 2, "multirate stems lost their reports or duration");
    REQUIRE_EQ(stems[0].loudness.frames, rate, "multirate stems lost their reports or duration");
    REQUIRE_EQ(stems[1].loudness.frames, rate, "multirate stems lost their reports or duration");
    for (const auto& stem : stems) {
      check_published_pcm(bundle / ("track-" + stem.track.to_string() + ".wav"), stem.loudness,
                          rate);
    }
    REQUIRE_LT(std::abs(loudness_or_nan(master_report) - loudness_or_nan(stems[0].loudness) -
                        20.0 * std::log10(2.0)),
               0.02, "multirate master/stem level differs from independent two-source oracle");
    REQUIRE_LT(std::abs(loudness_or_nan(stems[0].loudness) - loudness_or_nan(stems[1].loudness)),
               oscillate::test::tolerance::double_accumulation,
               "multirate master/stem level differs from independent two-source oracle");
  }
}

void test_managed_source_rate_master_and_stem() {
  Directory directory;
  auto store = take(persistence::ProjectStore::open_writer(directory.path() / "project", true));
  constexpr std::uint32_t source_rate = oscillate::test::sample_rate_44k;
  constexpr std::uint32_t project_rate = oscillate::test::sample_rate_48k;
  constexpr std::uint32_t source_frames = 4'410;
  constexpr double frequency = 440.0;
  const auto source_path = directory.path() / "source-rate.wav";
  write_bytes(source_path, sine_fixture_wav({source_rate, source_frames, frequency}));
  const int fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "source-rate fixture open failed");
  auto staged = store.stage_wav(id<project::SourceId>(84), fd);
  REQUIRE_EQ(::close(fd), 0, "source-rate fixture close failed");
  auto receipt = take(std::move(staged));
  auto snapshot = make_snapshot(receipt.record());
  snapshot.tracks.erase(snapshot.tracks.begin() + 1, snapshot.tracks.end());
  snapshot.tracks[0].clips.erase(snapshot.tracks[0].clips.begin() + 1,
                                 snapshot.tracks[0].clips.end());
  snapshot.tracks[0].clips[0].source_range = {{0}, {source_frames}};
  const auto duration =
      take(media::source_rate_frame_count(source_frames, source_rate, project_rate));
  snapshot.tracks[0].clips[0].placement =
      project::ElapsedInterval{{0}, {static_cast<std::int64_t>(duration)}};
  REQUIRE_EQ(take(store.save(snapshot, persistence::SaveKind::explicit_save,
                             std::span<const persistence::SourceReceipt>(&receipt, 1)))
                 .status,
             persistence::SaveStatus::committed, "source-rate project save failed");
  snapshot = take(store.load_current()).snapshot;
  REQUIRE_EQ(snapshot.sources[0].sample_rate, source_rate,
             "source-rate project lost encoded source coordinates on reopen");
  REQUIRE_EQ(snapshot.tracks[0].clips[0].source_range.end.value, source_frames,
             "source-rate project lost encoded source coordinates on reopen");
  const LiveRevisionState active{
      snapshot.revision, snapshot.revision, RevisionApplication::active, {}};
  const auto master_path = directory.path() / "source-rate-master.wav";
  const auto supported = media::source_rate_supported(source_rate, project_rate);
  auto rendered = render_wav_master(snapshot, store, active, {master_path});
  if (!supported) {
    REQUIRE_TRUE(!rendered, "converter-OFF master published a mismatched-rate source");
    REQUIRE_EQ(rendered.error().code, foundation::ErrorCode::unsupported,
               "converter-OFF master published a mismatched-rate source");
    REQUIRE_TRUE(!fs::exists(master_path),
                 "converter-OFF master published a mismatched-rate source");
    return;
  }
  REQUIRE_TRUE(rendered, "source-rate master did not publish");
  REQUIRE_EQ(rendered.value(), PublishOutcome::durable, "source-rate master did not publish");
  const auto master = read_bytes(master_path);
  REQUIRE_EQ(u32(master, 24), project_rate, "source-rate master duration or sample rate changed");
  REQUIRE_EQ(u32(master, 52) / 8, duration, "source-rate master duration or sample rate changed");
  for (std::uint32_t frame = 1'000; frame < 4'000; frame += 47) {
    const auto expected =
        12'000.0 / 32'768.0 * std::sin(2.0 * std::numbers::pi * frequency * frame / project_rate);
    REQUIRE_LT(std::abs(sample(master, 56 + frame * 8, WavFormat::float32) - expected), 0.003,
               "source-rate master changed physical pitch or timing");
  }
  const auto bundle = directory.path() / "source-rate-stems";
  REQUIRE_EQ(take(render_wav_stems(snapshot, store, active, {bundle})), PublishOutcome::durable,
             "source-rate stem did not publish");
  const auto stem = read_bytes(bundle / ("track-" + snapshot.tracks[0].id.to_string() + ".wav"));
  REQUIRE_EQ(stem, master, "source-rate stem differs from single-track master");
  const auto repeat = directory.path() / "source-rate-repeat.wav";
  REQUIRE_EQ(take(render_wav_master(snapshot, store, active, {repeat})), PublishOutcome::durable,
             "source-rate master changed across repeated renders");
  REQUIRE_EQ(read_bytes(repeat), master, "source-rate master changed across repeated renders");
  auto stretched = snapshot;
  stretched.tracks[0].clips[0].placement = project::ElapsedInterval{{0}, {4'799}};
  // Stretched, pitched and reviewed-warp clips render through the shared
  // warp path (exact oracles live in warp_export_tests.cpp).
  const auto stretched_path = directory.path() / "stretched-rate.wav";
  REQUIRE_EQ(take(render_wav_master(stretched, store, active, {stretched_path})),
             PublishOutcome::durable, "converted-rate stretch did not render its planned duration");
  REQUIRE_EQ(u32(read_bytes(stretched_path), 52) / 8, 4'799,
             "converted-rate stretch did not render its planned duration");
  auto pitched = snapshot;
  pitched.tracks[0].clips[0].processing.pitch_semitones = 7.0;
  const auto pitched_path = directory.path() / "pitched.wav";
  REQUIRE_EQ(take(render_wav_master(pitched, store, active, {pitched_path})),
             PublishOutcome::durable, "saved pitch intent was not rendered into the master");
  REQUIRE_NE(read_bytes(pitched_path), master,
             "saved pitch intent was not rendered into the master");
  auto warped = snapshot;
  warped.tracks[0].clips[0].processing.reviewed_warp_markers.push_back(
      {{source_frames / 2}, timeline::SamplePosition{static_cast<std::int64_t>(duration / 3)}});
  const auto warped_path = directory.path() / "reviewed-warp.wav";
  REQUIRE_EQ(take(render_wav_master(warped, store, active, {warped_path})), PublishOutcome::durable,
             "same-duration reviewed warp intent was not rendered into the master");
  REQUIRE_NE(read_bytes(warped_path), master,
             "same-duration reviewed warp intent was not rendered into the master");
  REQUIRE_EQ(read_bytes(warped_path).size(), master.size(),
             "same-duration reviewed warp intent was not rendered into the master");
}

} // namespace

int main() {
  try {
    test_mix_formats_and_guards();
    clip_fades_use_the_same_sample_boundaries_in_master_export();
#if OSCILLATE_HAVE_CODEC
    test_encoded_master("sine.aiff", project::SourceEncoding::aiff);
    test_encoded_master("sine.flac", project::SourceEncoding::flac);
    test_encoded_master("sine.mp3", project::SourceEncoding::mp3);
#else
    test_encoded_master_unavailable();
#endif
    test_stopped_export_authority();
    test_saved_native_sidechain_master_and_stems();
    test_saved_audio_track_eq_master_and_stems();
    test_saved_group_bus_eq_master_stems_and_bypass();
    test_aligned_atomic_stem_bundle();
    test_routed_master_and_source_stems();
    test_output_sample_rates();
    test_export_loudness_reports();
    test_export_intersample_peak_report();
    test_multirate_export_loudness_reports();
    test_managed_source_rate_master_and_stem();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
