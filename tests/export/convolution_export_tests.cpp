#include "insert_fixtures.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

struct Directory : public oscillate::test::TempDir {
  Directory() : TempDir("oscillate-convolution-export") {}
  const std::filesystem::path root{path()};
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  u16(bytes, static_cast<std::uint16_t>(value));
  u16(bytes, static_cast<std::uint16_t>(value >> 16U));
}
void write_wav(const fs::path& path, std::span<const std::array<std::int16_t, 2>> frames) {
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  u32(bytes, static_cast<std::uint32_t>(36 + frames.size() * 4));
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(bytes, 16);
  u16(bytes, 1);
  u16(bytes, 2);
  u32(bytes, 48'000);
  u32(bytes, 48'000 * 4);
  u16(bytes, 4);
  u16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  u32(bytes, static_cast<std::uint32_t>(frames.size() * 4));
  for (const auto& frame : frames) {
    u16(bytes, static_cast<std::uint16_t>(frame[0]));
    u16(bytes, static_cast<std::uint16_t>(frame[1]));
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  require(out.good(), "fixture write failed");
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "render file missing");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes.at(offset)) |
         (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8U) |
         (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16U) |
         (static_cast<std::uint32_t>(bytes.at(offset + 3)) << 24U);
}
float frame_sample(const std::vector<std::uint8_t>& bytes, std::size_t frame, std::size_t channel) {
  const auto bits = u32(bytes, 56 + (frame * 2 + channel) * 4);
  float sample = 0;
  std::memcpy(&sample, &bits, sizeof(sample));
  return sample;
}

void check_impulse(const std::vector<std::uint8_t>& bytes) {
  REQUIRE_EQ(u32(bytes, 52), 5 * 2 * 4, "convolution tail frame was truncated");
  REQUIRE_EQ(bytes.size(), 56 + 5 * 2 * 4, "convolution tail frame was truncated");
  constexpr std::array expected_left{0.25F, 0.125F, 0.0F, 0.0F, 0.0F};
  constexpr std::array expected_right{0.125F, 0.0625F, 0.0F, 0.0F, 0.0F};
  for (std::size_t frame = 0; frame < expected_left.size(); ++frame) {
    REQUIRE_LT(std::abs(frame_sample(bytes, frame, 0) - expected_left[frame]),
               tolerance::near_silence_f,
               "convolution master/stem disagrees with independent two-tap oracle");
    REQUIRE_LT(std::abs(frame_sample(bytes, frame, 1) - expected_right[frame]),
               tolerance::near_silence_f,
               "convolution master/stem disagrees with independent two-tap oracle");
  }
}

void offline_master_track_stem_and_failures() {
  Directory dir;
  auto store = take(persistence::ProjectStore::open_writer(dir.root / "project", true));
  const auto source_path = dir.root / "source.wav";
  const auto ir_path = dir.root / "ir.wav";
  constexpr std::array<std::array<std::int16_t, 2>, 4> source{
      {{16'384, 8'192}, {0, 0}, {0, 0}, {0, 0}}};
  constexpr std::array<std::array<std::int16_t, 2>, 2> impulse{{{16'384, 16'384}, {8'192, 8'192}}};
  write_wav(source_path, source);
  write_wav(ir_path, impulse);
  const int source_fd = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
  const int ir_fd = ::open(ir_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(source_fd, 0, "fixture open failed");
  REQUIRE_GE(ir_fd, 0, "fixture open failed");
  auto source_receipt = store.stage_wav(id<project::SourceId>(10), source_fd);
  auto ir_receipt = store.stage_wav(id<project::SourceId>(11), ir_fd);
  REQUIRE_EQ(::close(source_fd), 0, "fixture close failed");
  REQUIRE_EQ(::close(ir_fd), 0, "fixture close failed");
  auto receipts = std::array<persistence::SourceReceipt, 2>{take(std::move(source_receipt)),
                                                            take(std::move(ir_receipt))};
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Clip clip{id<project::ClipId>(20),
                     receipts[0].record().id,
                     {{0}, {4}},
                     project::ElapsedInterval{{0}, {4}},
                     1};
  project::Snapshot saved{id<project::ProjectId>(1),
                          7,
                          48'000,
                          std::move(tempo),
                          std::move(meter),
                          {{id<project::TrackId>(2), {clip}}},
                          {receipts[0].record(), receipts[1].record()}};
  REQUIRE_OK(store.save(saved, persistence::SaveKind::explicit_save, receipts));
  saved = take(store.load_current()).snapshot;
  const auto& ir = saved.sources[1];
  const project::ManagedAudioReference reference{ir.id,          ir.sha256,   ir.byte_length,
                                                 ir.sample_rate, ir.channels, ir.frames};
  const project::EffectInsert slot{id<project::InsertSlotId>(30), true,
                                   project::NativeConvolution{reference, 1.0, 1}};
  Gate gate;
  const auto master_target = dir.root / "master.wav";
  auto master = saved;
  master.master_effects.push_back(slot);
  auto render_master = [&](const project::Snapshot& snapshot, const fs::path& target) {
    return exporting::render_wav_master(
        snapshot, store, take(exporting::StoppedExportAuthority::acquire(snapshot, gate)),
        {target, exporting::WavFormat::float32});
  };
  REQUIRE_EQ(take(render_master(master, master_target)), exporting::PublishOutcome::durable,
             "master convolution render failed");
  const auto first = read_bytes(master_target);
  check_impulse(first);
  REQUIRE_EQ(take(render_master(master, master_target)), exporting::PublishOutcome::durable,
             "repeat convolution render differed");
  REQUIRE_EQ(read_bytes(master_target), first, "repeat convolution render differed");
  const project::EffectInsert limiter{id<project::InsertSlotId>(32), true,
                                      test_insert::ceiling({-18.0, 1.0, 100.0})};
  auto limiter_before = master;
  limiter_before.master_effects.insert(limiter_before.master_effects.begin(), limiter);
  auto limiter_after = master;
  limiter_after.master_effects.push_back(limiter);
  const auto before_target = dir.root / "limiter-before.wav";
  const auto after_target = dir.root / "limiter-after.wav";
  REQUIRE_EQ(take(render_master(limiter_before, before_target)), exporting::PublishOutcome::durable,
             "saved convolution/limiter insert order was not observed");
  REQUIRE_EQ(take(render_master(limiter_after, after_target)), exporting::PublishOutcome::durable,
             "saved convolution/limiter insert order was not observed");
  REQUIRE_NE(read_bytes(before_target), read_bytes(after_target),
             "saved convolution/limiter insert order was not observed");
  auto track = saved;
  track.tracks[0].effects.push_back(slot);
  const auto track_target = dir.root / "track.wav";
  REQUIRE_EQ(take(render_master(track, track_target)), exporting::PublishOutcome::durable,
             "track convolution render failed");
  check_impulse(read_bytes(track_target));
  const auto stems = dir.root / "stems";
  REQUIRE_EQ(
      take(exporting::render_wav_stems(
          track, store, take(exporting::StoppedExportAuthority::acquire(track, gate)), {stems})),
      exporting::PublishOutcome::durable, "processed convolution stem failed");
  check_impulse(read_bytes(stems / ("track-" + track.tracks[0].id.to_string() + ".wav")));
  track.master_effects.push_back(
      {id<project::InsertSlotId>(31), true, project::NativeConvolution{reference, 0.5, 1}});
  const auto duplicate = render_master(track, master_target);
  REQUIRE_TRUE(!duplicate, "second active convolution replaced durable output");
  REQUIRE_EQ(duplicate.error().code, foundation::ErrorCode::resource_exhausted,
             "second active convolution replaced durable output");
  REQUIRE_EQ(read_bytes(master_target), first, "second active convolution replaced durable output");
  const auto ir_asset = dir.root / "project" / "assets" / (ir.sha256 + ".wav");
  {
    std::fstream altered(ir_asset, std::ios::in | std::ios::out | std::ios::binary);
    require(altered.good(), "managed IR fixture missing");
    altered.seekp(44);
    altered.put('\x7f');
    require(altered.good(), "managed IR fixture mutation failed");
  }
  const auto damaged = render_master(master, master_target);
  REQUIRE_TRUE(!damaged, "same-size changed IR replaced durable output");
  REQUIRE_EQ(read_bytes(master_target), first, "same-size changed IR replaced durable output");
  fs::remove(ir_asset);
  const auto missing = render_master(master, master_target);
  REQUIRE_TRUE(!missing, "missing managed IR replaced durable output");
  REQUIRE_EQ(read_bytes(master_target), first, "missing managed IR replaced durable output");
  const auto missing_stems = dir.root / "missing-ir-stems";
  REQUIRE_TRUE(!exporting::render_wav_stems(
                   track, store, take(exporting::StoppedExportAuthority::acquire(track, gate)),
                   {missing_stems}),
               "missing IR published a processed stem bundle");
  REQUIRE_TRUE(!fs::exists(missing_stems), "missing IR published a processed stem bundle");
}
} // namespace

int main() {
  try {
    offline_master_track_stem_and_failures();
    std::cout << "Offline convolution export contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
