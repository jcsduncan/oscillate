#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// A trimmed master export ends where the sound after the music falls
// quiet, not at every effect's worst-case tail: one note through Dust's
// noise bed and the seeded reverb and delay returns.
namespace {
namespace app = oscillate::application;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;
namespace effects = oscillate::native_dsp::effects;

using oscillate::test::require;

using oscillate::test::take;

void accepted(const foundation::Result<void>& result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-tail-trim") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

std::vector<float> read_float_wav(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<char> bytes{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
  REQUIRE_GT(bytes.size(), 12, "export is not RIFF");
  REQUIRE_EQ(std::memcmp(bytes.data(), "RIFF", 4), 0, "export is not RIFF");
  std::uint32_t riff = 0;
  std::memcpy(&riff, bytes.data() + 4, 4);
  REQUIRE_EQ(riff + 8, bytes.size(), "the RIFF size matches the file");
  std::size_t position = 12;
  while (position + 8 <= bytes.size()) {
    std::uint32_t size = 0;
    std::memcpy(&size, bytes.data() + position + 4, 4);
    if (std::memcmp(bytes.data() + position, "fact", 4) == 0) {
      REQUIRE_LE(position + 12, bytes.size(), "truncated fact chunk");
      std::uint32_t frames = 0;
      std::memcpy(&frames, bytes.data() + position + 8, 4);
      constexpr std::uint64_t stereo_float_bytes = 2 * sizeof(float);
      REQUIRE_GE(static_cast<std::uint64_t>(bytes.size()),
                 static_cast<std::uint64_t>(frames) * stereo_float_bytes,
                 "fact frames fit the file");
    }
    if (std::memcmp(bytes.data() + position, "data", 4) == 0) {
      REQUIRE_EQ(position + 8 + size, bytes.size(), "the data chunk ends the file");
      std::vector<float> samples(size / sizeof(float));
      std::memcpy(samples.data(), bytes.data() + position + 8, samples.size() * sizeof(float));
      return samples;
    }
    position += 8 + size + (size & 1U);
  }
  throw std::runtime_error("WAV has no data chunk");
}

std::vector<float> render(const fs::path& root, const project::Snapshot& saved,
                          const fs::path& output, bool trim, std::uint32_t rate = 0) {
  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(root));
  auto authority = take(exporting::StoppedExportAuthority::acquire(saved, gate));
  exporting::MasterRenderOptions options{output, exporting::WavFormat::float32};
  options.sample_rate = rate;
  options.trim_silent_tail = trim;
  (void)take(exporting::render_wav_master(saved, store, std::move(authority), options));
  return read_float_wav(output);
}

double rms(const std::vector<float>& samples, std::size_t first_frame, std::size_t last_frame) {
  double sum = 0.0;
  for (std::size_t index = first_frame * 2; index < last_frame * 2; ++index)
    sum += static_cast<double>(samples[index]) * samples[index];
  return last_frame > first_frame
             ? std::sqrt(sum / static_cast<double>((last_frame - first_frame) * 2))
             : 0.0;
}

// Dust's bed level for the test, well under the note.
constexpr double bed_level_db = -60.0;

void trimmed_tail_ends_with_the_sound() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto synth =
      take(session->instruments().add_native_instrument_track("ember", std::nullopt));
  accepted(session->midi_clips().add_midi_clip(synth, {0}));
  const auto clip =
      session->snapshot().tracks[take(session->tracks().track_index(synth))].midi_clips.front().id;
  accepted(session->notes().add_note_with_duration(clip, {0}, {timeline::ticks_per_quarter}, 60));
  // A quiet bed, as in a mix: it plays on after the note, far below it.
  const auto dust = take(session->effects().add_effect_engine(synth, 0, "dust"));
  {
    const auto& table = *effects::find_engine("dust")->descriptor;
    auto values = take(session->effects().native_effect_values(synth, dust));
    const auto bed = oscillate::native_dsp::synth::find_parameter(table, "dust.mix.bed");
    REQUIRE_TRUE(bed, "Dust has a bed level");
    values[*bed] = bed_level_db;
    accepted(session->effects().set_native_effect_values(synth, dust, values));
  }
  for (const auto& track : session->snapshot().tracks)
    if (track.kind == project::TrackKind::return_bus)
      (void)take(session->routing().add_send(synth, track.id, 0.6));
  accepted(session->saving().save());
  const auto saved = session->snapshot();
  const auto rate = static_cast<std::size_t>(saved.sample_rate);

  const auto full = render(root, saved, directory.path() / "full.wav", false);
  const auto trimmed = render(root, saved, directory.path() / "trimmed.wav", true);
  const auto full_frames = full.size() / 2;
  const auto kept = trimmed.size() / 2;
  std::cout << "full " << full_frames << " frames, trimmed " << kept << '\n';
  REQUIRE_LT(kept, full_frames, "the trimmed export is shorter");
  // The note ends a quarter in (120 BPM): the music is all there.
  REQUIRE_GT(kept, rate / 2, "the music and its audible tail are kept");

  // Identical up to the fade, then faded to nothing.
  const auto fade =
      static_cast<std::size_t>(static_cast<double>(rate) * exporting::tail_fade_seconds);
  require(std::equal(trimmed.begin(),
                     trimmed.begin() + static_cast<std::ptrdiff_t>((kept - fade) * 2),
                     full.begin()),
          "the trimmed export is the full one up to the fade");
  REQUIRE_LT(std::abs(trimmed[kept * 2 - 1]), tolerance::float_round_trip_f,
             "the last frame is faded out");
  REQUIRE_LT(std::abs(trimmed[kept * 2 - 2]), tolerance::float_round_trip_f,
             "the last frame is faded out");

  // What was cut is quiet against the music.
  const auto window =
      static_cast<std::size_t>(static_cast<double>(rate) * exporting::tail_window_seconds);
  double loudest = 0.0;
  for (std::size_t first = 0; first + window <= rate / 2; first += window)
    loudest = std::max(loudest, rms(full, first, first + window));
  const double quiet =
      std::max(std::pow(10.0, exporting::tail_floor_dbfs / 20.0),
               loudest * std::pow(10.0, -exporting::tail_quiet_below_music_db / 20.0));
  for (std::size_t first = kept; first < full_frames; first += window)
    REQUIRE_LT(rms(full, first, std::min(full_frames, first + window)), quiet,
               "only quiet sound is cut");

  // Off-rate output ends early too, with a consistent header. Rate
  // conversion exists only in the libsamplerate export build.
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  const auto converted = render(root, saved, directory.path() / "converted.wav", true, 44'100);
  const auto expected = static_cast<double>(kept) * 44'100.0 / static_cast<double>(rate);
  const std::size_t converted_frames = converted.size() / 2; // Interleaved stereo.
  REQUIRE_NEAR(static_cast<double>(converted_frames), expected, 1.0,
               "the converted export is trimmed to the same time");
#endif
}

} // namespace

int main() {
  try {
    trimmed_tail_ends_with_the_sound();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "export tail trim contract passed\n";
  return 0;
}
