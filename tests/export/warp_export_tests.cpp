// Offline export of saved processed (warped/stretched/pitched) clips: master
// and processed stems against an independent direct-stream oracle, marker
// timing, source/project rate differences, pitch and formant semantics,
// routing and tails, repeat renders, raw-clip regression and source
// failures. Synthetic correctness only, not recorded listening approval.
#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"
#include "warp_oracle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
namespace app = oscillate::application;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace processing = oscillate::processing;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-warp-export") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// Mono 16-bit PCM from a sample function in [-1, 1].
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
fs::path make_wav(const fs::path& path, std::uint32_t frames, std::uint32_t rate,
                  const std::function<double(std::uint32_t)>& sample) {
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, rate);
  put32(bytes, rate * 2);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 2);
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    put16(bytes, static_cast<std::uint16_t>(
                     static_cast<std::int16_t>(std::lround(32'767.0 * sample(frame)))));
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::uint32_t le32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[at + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[at + 3]) << 24U);
}

// A float32 stereo export, de-interleaved.
struct Pcm {
  std::uint32_t rate = 0;
  std::vector<float> left;
  std::vector<float> right;
};

Pcm read_float_wav(const fs::path& path) {
  const auto bytes = read_bytes(path);
  REQUIRE_GT(bytes.size(), 12, "export is not a RIFF file");
  REQUIRE_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF", "export is not a RIFF file");
  Pcm pcm;
  for (std::size_t at = 12; at + 8 <= bytes.size();) {
    const std::string id(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                         bytes.begin() + static_cast<std::ptrdiff_t>(at + 4));
    const auto size = le32(bytes, at + 4);
    if (id == "fmt ")
      pcm.rate = le32(bytes, at + 12);
    if (id == "data") {
      for (std::size_t sample = 0; sample + 8 <= size; sample += 8) {
        pcm.left.push_back(std::bit_cast<float>(le32(bytes, at + 8 + sample)));
        pcm.right.push_back(std::bit_cast<float>(le32(bytes, at + 12 + sample)));
      }
      return pcm;
    }
    at += 8 + size + (size & 1U);
  }
  throw std::runtime_error("export has no data chunk");
}

// A 440 Hz tone with a click every quarter second, so transient analysis has
// real onsets and pitch analysis a voiced tone.
double tone_with_clicks(std::uint32_t frame, std::uint32_t rate) {
  const double tone =
      0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / rate);
  return tone + (frame % (rate / 4) == rate / 8 ? 0.6 : 0.0);
}

// A managed project with one clip per imported source, each on its own track.
struct Song {
  Directory directory;
  fs::path root;
  std::unique_ptr<app::ProjectSession> session;
  Gate gate;

  Song() : root(directory.path() / "song"), session(take(app::ProjectSession::create(root))) {}

  project::TrackId import(std::uint32_t frames, std::uint32_t rate,
                          const std::function<double(std::uint32_t)>& sample) {
    const auto wav = make_wav(directory.path() / ("source-" + std::to_string(imports++) + ".wav"),
                              frames, rate, sample);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "WAV fixture open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    REQUIRE_OK(track, "managed WAV import failed");
    REQUIRE_OK(session->saving().save(), "fixture save failed");
    return track.value();
  }

  [[nodiscard]] exporting::PublishOutcome master(const project::Snapshot& snapshot,
                                                 const fs::path& target) {
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    return take(exporting::render_wav_master(snapshot, store, std::move(authority), {target}));
  }
  [[nodiscard]] foundation::Result<exporting::PublishOutcome>
  try_master(const project::Snapshot& snapshot, const fs::path& target) {
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    return exporting::render_wav_master(snapshot, store, std::move(authority), {target});
  }
  [[nodiscard]] exporting::PublishOutcome stems(const project::Snapshot& snapshot,
                                                const fs::path& bundle) {
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
    return take(exporting::render_wav_stems(snapshot, store, std::move(authority), {bundle}));
  }

private:
  int imports = 0;
};

project::Clip& clip_of(project::Snapshot& snapshot, project::TrackId track) {
  for (auto& item : snapshot.tracks)
    if (item.id == track)
      return item.clips.front();
  throw std::runtime_error("track has no clip");
}

project::ClipProcessing with_mode(project::WarpMode mode, double pitch = 0.0) {
  project::ClipProcessing processing;
  processing.pitch_semitones = pitch;
  processing.warp.mode = mode;
  processing.warp.mode_overridden = true;
  return processing;
}

// Places a track's clip at [start, start + length) elapsed frames with the
// given processing, as one saved edit would.
project::Snapshot placed(const project::Snapshot& base, project::TrackId track, std::int64_t start,
                         std::int64_t length, const project::ClipProcessing& processing) {
  auto snapshot = base;
  auto& clip = clip_of(snapshot, track);
  clip.placement = project::ElapsedInterval{{start}, {start + length}};
  clip.processing = processing;
  ++clip.settings_generation;
  return snapshot;
}

std::string mode_name(project::WarpMode mode) {
  return std::string(processing::warp_algorithm_id(mode));
}

void require_matches_oracle(const Pcm& pcm, const warp_oracle::Rendered& expected,
                            std::string_view what) {
  REQUIRE_GE(pcm.left.size(), expected.first + expected.left.size(),
             std::string(what) + " export is shorter than the clip");
  for (std::uint64_t sample = 0; sample < expected.first; ++sample)
    require(pcm.left[sample] == 0.0F && pcm.right[sample] == 0.0F,
            std::string(what) + " sounded before its clip");
  for (std::size_t frame = 0; frame < expected.left.size(); ++frame)
    require(pcm.left[expected.first + frame] == expected.left[frame] &&
                pcm.right[expected.first + frame] == expected.right[frame],
            std::string(what) + " export differs from the direct stream at frame " +
                std::to_string(frame));
  for (auto sample = expected.first + expected.left.size(); sample < pcm.left.size(); ++sample)
    require(pcm.left[sample] == 0.0F && pcm.right[sample] == 0.0F,
            std::string(what) + " sounded past its clip");
}

// --- Every mode, master and processed stem -----------------------------------

void every_mode_master_and_stem_match_the_direct_stream() {
  Song song;
  const auto track = song.import(
      48'000, 48'000, [](std::uint32_t frame) { return tone_with_clicks(frame, 48'000); });
  std::size_t index = 0;
  for (const auto mode :
       {project::WarpMode::repitch, project::WarpMode::beats, project::WarpMode::tones,
        project::WarpMode::texture, project::WarpMode::complex, project::WarpMode::complex_pro}) {
    auto processing = with_mode(mode, 2.0);
    processing.reviewed_warp_markers.push_back({{24'000}, timeline::SamplePosition{20'000}});
    const auto snapshot = placed(song.session->snapshot(), track, 1'000, 60'000, processing);
    // Alternate 64- and 128-frame oracle pulls against the export's blocks.
    const std::size_t pull = index++ % 2 == 0 ? 64 : 128;
    auto copy = snapshot;
    const auto expected = warp_oracle::render(song.root, snapshot, clip_of(copy, track).id, pull);
    require(expected.first == 1'000 && expected.left.size() == 60'000,
            mode_name(mode) + " oracle does not follow the frame plan");
    const auto master_path = song.directory.path() / (mode_name(mode) + "-master.wav");
    REQUIRE_EQ(song.master(snapshot, master_path), exporting::PublishOutcome::durable,
               mode_name(mode) + " master did not publish");
    const auto master = read_float_wav(master_path);
    require(master.rate == 48'000 && master.left.size() == 61'000,
            mode_name(mode) + " master length is not the planned clip end");
    require_matches_oracle(master, expected, mode_name(mode) + " master");
    const auto bundle = song.directory.path() / (mode_name(mode) + "-stems");
    REQUIRE_EQ(song.stems(snapshot, bundle), exporting::PublishOutcome::durable,
               mode_name(mode) + " stems did not publish");
    REQUIRE_EQ(read_bytes(bundle / ("track-" + track.to_string() + ".wav")),
               read_bytes(master_path),
               mode_name(mode) + " processed stem differs from the single-track master");
    const auto repeat = song.directory.path() / (mode_name(mode) + "-repeat.wav");
    require(song.master(snapshot, repeat) == exporting::PublishOutcome::durable &&
                read_bytes(repeat) == read_bytes(master_path),
            mode_name(mode) + " repeat render changed bytes");
  }
}

// --- Marker timing at the same and a different project rate --------------------

std::uint64_t peak(const std::vector<float>& signal) {
  return static_cast<std::uint64_t>(
      std::ranges::max_element(signal, {}, [](float value) { return std::fabs(value); }) -
      signal.begin());
}

void markers_land_exactly_at_the_same_and_another_rate() {
  Song song;
  const auto track =
      song.import(48'000, 48'000, [](std::uint32_t frame) { return frame == 12'000 ? 0.8 : 0.0; });
  for (const auto mode : {project::WarpMode::repitch, project::WarpMode::beats}) {
    auto processing = with_mode(mode);
    processing.reviewed_warp_markers.push_back({{12'000}, timeline::SamplePosition{20'000}});
    const auto snapshot = placed(song.session->snapshot(), track, 480, 60'000, processing);
    const auto path = song.directory.path() / (mode_name(mode) + "-marker.wav");
    REQUIRE_EQ(song.master(snapshot, path), exporting::PublishOutcome::durable,
               "marker master did not publish");
    REQUIRE_EQ(peak(read_float_wav(path).left), 20'480,
               mode_name(mode) + " did not place the marked impulse at project frame 20,480");
  }
  // The same project changed to 44.1 kHz: elapsed placement and marker
  // offsets rescale (480 -> 441, 20,000 -> 18,375) and the 48 kHz source is
  // read through the exact project-rate view, so the marked source frame
  // (12,000 -> 11,025 view frames) lands exactly on 441 + 18,375.
  auto processing = with_mode(project::WarpMode::repitch);
  processing.reviewed_warp_markers.push_back({{12'000}, timeline::SamplePosition{20'000}});
  const auto at_48k = placed(song.session->snapshot(), track, 480, 60'000, processing);
  auto model = take(project::ProjectModel::open(at_48k, song.gate));
  const auto changed = take(model.apply({project::SetSampleRate{44'100}}));
  const auto& at_44k = *changed.snapshot;
  const auto path = song.directory.path() / "rate-marker.wav";
  REQUIRE_EQ(song.master(at_44k, path), exporting::PublishOutcome::durable,
             "different-rate marker master did not publish");
  const auto pcm = read_float_wav(path);
  REQUIRE_EQ(pcm.rate, 44'100, "different-rate export moved the marker or changed duration");
  REQUIRE_EQ(pcm.left.size(), 441 + 55'125,
             "different-rate export moved the marker or changed duration");
  REQUIRE_EQ(peak(pcm.left), 18'816, "different-rate export moved the marker or changed duration");
  auto copy = at_44k;
  require_matches_oracle(pcm, warp_oracle::render(song.root, at_44k, clip_of(copy, track).id, 64),
                         "different-rate Re-Pitch");
}

// --- Pitch and formant semantics ----------------------------------------------

double frequency(const std::vector<float>& signal, std::size_t first, std::size_t last,
                 double rate) {
  double start = -1.0;
  double end = -1.0;
  int crossings = 0;
  for (auto i = first; i + 1 < last; ++i) {
    if (signal[i] <= 0.0F && signal[i + 1] > 0.0F) {
      const double at = static_cast<double>(i) + signal[i] / (signal[i] - signal[i + 1]);
      if (start < 0.0)
        start = at;
      end = at;
      ++crossings;
    }
  }
  REQUIRE_GT(crossings, 10, "too few crossings to measure pitch");
  return (crossings - 1) * rate / (end - start);
}

void pitch_and_formant_intent_render_as_saved() {
  Song song;
  const auto track = song.import(48'000, 48'000, [](std::uint32_t frame) {
    return 0.4 * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 48'000.0);
  });
  const auto render = [&](const project::ClipProcessing& processing, std::string_view name) {
    auto changed = processing;
    // A marker makes every variant processed, including Re-Pitch.
    changed.reviewed_warp_markers.push_back({{24'000}, timeline::SamplePosition{24'000}});
    const auto snapshot = placed(song.session->snapshot(), track, 0, 48'000, changed);
    const auto path = song.directory.path() / (std::string(name) + ".wav");
    REQUIRE_EQ(song.master(snapshot, path), exporting::PublishOutcome::durable,
               std::string(name) + " did not publish");
    return path;
  };
  const auto pitched = read_float_wav(render(with_mode(project::WarpMode::complex, 7.004), "up7"));
  const double cents =
      1'200.0 * std::log2(frequency(pitched.left, 9'600, 38'400, 48'000.0) / 440.0);
  REQUIRE_LT(std::fabs(cents - 700.0), 5.0,
             "exported +7.004 semitones is not +700 cents: " + std::to_string(cents));
  REQUIRE_EQ(read_bytes(render(with_mode(project::WarpMode::repitch, 9.0), "repitch-9")),
             read_bytes(render(with_mode(project::WarpMode::repitch, 0.0), "repitch-0")),
             "inactive Re-Pitch transpose changed the export");
  auto intent_off = with_mode(project::WarpMode::complex_pro, 5.0);
  intent_off.preserve_formants = false;
  auto zero = with_mode(project::WarpMode::complex_pro, 5.0);
  zero.warp.complex_pro.formants = 0.0;
  const auto off_bytes = read_bytes(render(intent_off, "formants-off"));
  REQUIRE_EQ(off_bytes, read_bytes(render(zero, "formants-zero")),
             "Complex Pro formant intent is not applied as its formant amount");
  REQUIRE_NE(off_bytes,
             read_bytes(render(with_mode(project::WarpMode::complex_pro, 5.0), "formants-full")),
             "Complex Pro formant intent is not applied as its formant amount");
}

// --- Routing, tails and raw-clip regression -------------------------------------

void routing_tails_and_raw_clips_render_together() {
  Song song;
  const auto processed = song.import(
      48'000, 48'000, [](std::uint32_t frame) { return tone_with_clicks(frame, 48'000); });
  const auto raw = song.import(24'000, 48'000, [](std::uint32_t frame) {
    return 0.25 * std::sin(2.0 * std::numbers::pi * 220.0 * frame / 48'000.0);
  });
  auto snapshot = placed(song.session->snapshot(), processed, 2'000, 60'000,
                         with_mode(project::WarpMode::complex, -2.0));
  auto copy = snapshot;
  const auto expected = warp_oracle::render(song.root, snapshot, clip_of(copy, processed).id, 128);
  const auto raw_first = static_cast<std::uint64_t>(
      std::get<project::ElapsedInterval>(clip_of(copy, raw).placement).start.value);

  // Without inserts the master is the processed clip plus the raw clip.
  const auto plain_path = song.directory.path() / "plain.wav";
  REQUIRE_EQ(song.master(snapshot, plain_path), exporting::PublishOutcome::durable,
             "two-track master did not publish");
  const auto plain = read_float_wav(plain_path);
  const auto bundle = song.directory.path() / "plain-stems";
  REQUIRE_EQ(song.stems(snapshot, bundle), exporting::PublishOutcome::durable,
             "two-track stems did not publish");
  const auto processed_stem = read_float_wav(bundle / ("track-" + processed.to_string() + ".wav"));
  const auto raw_stem = read_float_wav(bundle / ("track-" + raw.to_string() + ".wav"));
  require_matches_oracle(processed_stem, expected, "processed stem beside a raw clip");
  // The raw clip keeps its unprocessed reader: its stem is the source itself.
  for (std::uint64_t frame = 0; frame < 24'000; ++frame) {
    const auto quantised = std::lround(
        32'767.0 * 0.25 *
        std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(frame) / 48'000.0));
    const double source = static_cast<double>(quantised) / 32'768.0;
    REQUIRE_LT(std::fabs(raw_stem.left[raw_first + frame] - source), tolerance::float_round_trip,
               "raw clip export changed beside a processed clip");
  }
  for (std::size_t frame = 0; frame < plain.left.size(); ++frame)
    REQUIRE_LT(std::fabs(plain.left[frame] - (processed_stem.left[frame] + raw_stem.left[frame])),
               tolerance::float_round_trip, "master is not the sum of its processed and raw stems");

  // Route the processed track through a group bus with a reverb insert: the
  // export keeps the reverb tail after the processed clip ends.
  const auto group = take(song.session->tracks().add_bus_track(project::TrackKind::group_bus));
  REQUIRE_OK(song.session->routing().set_main_output(processed, group), "bus route failed");
  REQUIRE_OK(song.session->effects().add_effect_engine(project::SignalPathId{group}, 0, "expanse"),
             "reverb insert failed");
  auto routed = song.session->snapshot();
  auto& routed_clip = clip_of(routed, processed);
  routed_clip.placement = project::ElapsedInterval{{2'000}, {62'000}};
  routed_clip.processing = with_mode(project::WarpMode::complex, -2.0);
  ++routed_clip.settings_generation;
  const auto tail_path = song.directory.path() / "tail.wav";
  REQUIRE_EQ(song.master(routed, tail_path), exporting::PublishOutcome::durable,
             "routed processed master did not publish");
  const auto tail = read_float_wav(tail_path);
  REQUIRE_GT(tail.left.size(), 62'000 + 4'800, "reverb tail was cut at the processed clip end");
  float after = 0.0F;
  for (std::size_t frame = 62'100; frame < 62'000 + 4'800; ++frame)
    after = std::max(after, std::fabs(tail.left[frame]));
  REQUIRE_GT(after, 1e-4F, "processed clip left no reverb tail after its end");
  const auto repeat = song.directory.path() / "tail-repeat.wav";
  REQUIRE_EQ(song.master(routed, repeat), exporting::PublishOutcome::durable,
             "routed processed master changed on repeat");
  REQUIRE_EQ(read_bytes(repeat), read_bytes(tail_path),
             "routed processed master changed on repeat");
}

// --- Source failures -------------------------------------------------------------

void changed_or_missing_source_publishes_nothing() {
  Song song;
  const auto track = song.import(
      48'000, 48'000, [](std::uint32_t frame) { return tone_with_clicks(frame, 48'000); });
  const auto snapshot =
      placed(song.session->snapshot(), track, 0, 60'000, with_mode(project::WarpMode::complex));
  const auto target = song.directory.path() / "kept.wav";
  {
    std::ofstream sentinel(target, std::ios::binary);
    sentinel << "kept";
  }
  const auto asset = song.root / "assets" / (snapshot.sources.front().sha256 + ".wav");
  const auto original = read_bytes(asset);
  // Same size, different bytes: the verified descriptor refuses it.
  auto changed = original;
  changed[changed.size() / 2] ^= 0x01U;
  {
    std::ofstream file(asset, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(changed.data()),
               static_cast<std::streamsize>(changed.size()));
  }
  const auto kept = [&] {
    const auto bytes = read_bytes(target);
    return std::string(bytes.begin(), bytes.end()) == "kept";
  };
  REQUIRE_TRUE(!song.try_master(snapshot, target), "changed processed source published a render");
  REQUIRE_TRUE(kept(), "changed processed source published a render");
  fs::remove(asset);
  REQUIRE_TRUE(!song.try_master(snapshot, target), "missing processed source published a render");
  REQUIRE_TRUE(kept(), "missing processed source published a render");
}

} // namespace

int main() {
  try {
    every_mode_master_and_stem_match_the_direct_stream();
    markers_land_exactly_at_the_same_and_another_rate();
    pitch_and_formant_intent_render_as_saved();
    routing_tails_and_raw_clips_render_together();
    changed_or_missing_source_publishes_nothing();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
