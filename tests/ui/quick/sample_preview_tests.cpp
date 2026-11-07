#include "../../../apps/daw_quick/src/sample_library.hpp"
#include "../../../apps/daw_quick/src/sample_preview.hpp"

#include "oscillate/export/wav_writer.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace quick = oscillate::quick;
namespace exporting = oscillate::exporting;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-samples-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "fixture directory creation failed");
    path_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

// Writes mono `samples` as a stereo float WAV.
void write_wav(const fs::path& path, std::uint32_t rate, const std::vector<float>& samples) {
  fs::create_directories(path.parent_path());
  auto writer =
      exporting::WavWriter::begin(path, {rate, exporting::WavFormat::float32, samples.size(), 0});
  require(writer.has_value(), "WAV writer failed");
  std::vector<float> interleaved;
  interleaved.reserve(samples.size() * 2);
  for (const float sample : samples) {
    interleaved.push_back(sample);
    interleaved.push_back(sample);
  }
  require(writer.value().write_interleaved_stereo(interleaved).has_value() &&
              writer.value().finish().has_value(),
          "WAV write failed");
}

// The library lists audio files by folder, reads tempos from names and
// skips hidden folders and other files.
void the_library_lists_samples() {
  Directory directory;
  const auto& root = directory.path();
  const std::vector<float> blip(4'800, 0.25F);
  write_wav(root / "Loops" / "Groove 124 BPM.wav", 48'000, blip);
  write_wav(root / "Drums" / "Kick 01.wav", 48'000, blip);
  write_wav(root / ".trash" / "Old.wav", 48'000, blip);
  std::ofstream(root / "notes.txt") << "not audio";
  std::ofstream(root / "Top.flac") << "not decoded without the codec";
  const auto found = quick::scan_sample_folder(root, false);
  require(found.size() == 2, "two samples: hidden folders, text and FLAC are skipped");
  require(found[0].folder == "Drums" && found[0].name == "Kick 01" && !found[0].bpm,
          "sorted by folder; a one-shot has no tempo");
  require(found[1].folder == "Loops" && found[1].bpm == 124.0, "a loop's tempo is in its name");
  require(quick::scan_sample_folder(root, true).size() == 3, "with the codec, FLAC is listed too");
  require(quick::scan_sample_folder(root / "missing", false).empty(), "a missing folder is empty");
}

// A 100 BPM loop previews at the song's 120 BPM: 5/6 as long, its beats
// on the song's beats.
void a_loop_is_stretched_to_the_song() {
  Directory directory;
  constexpr std::uint32_t rate = 48'000;
  constexpr std::size_t beat = rate * 60 / 100; // 28 800 frames.
  std::vector<float> clicks(beat * 4, 0.0F);
  for (std::size_t b = 0; b < 4; ++b)
    for (std::size_t i = 0; i < 96; ++i)
      clicks[b * beat + i] = 0.8F * std::sin(static_cast<float>(i) * 0.6F);
  const auto file = directory.path() / "Clicks 100 BPM.wav";
  write_wav(file, rate, clicks);
  const auto prepared = quick::prepare_sample(file, rate, 120.0);
  require(prepared.has_value(), "the loop prepares");
  const auto& sample = prepared.value();
  require(sample.bpm == 100.0 && sample.warped, "the loop is read as 100 BPM and stretched");
  require(sample.left.size() == clicks.size() * 100 / 120, "5/6 of its length at 120 BPM");
  // Each click lands within 50 ms of the song's beat (24 000 frames).
  for (std::size_t b = 1; b < 4; ++b) {
    const auto at = b * 24'000;
    std::size_t loudest = at - 2'400;
    for (std::size_t i = at - 2'400; i < at + 2'400; ++i)
      if (std::abs(sample.left[i]) > std::abs(sample.left[loudest]))
        loudest = i;
    require(std::abs(sample.left[loudest]) > 0.1F, "the click survives the stretch");
    require(loudest + 2'400 > at && loudest < at + 2'400, "the click is on the song's beat");
  }
}

// A sample at another rate is resampled with its pitch kept; a one-shot at
// the song's rate plays untouched.
void rates_convert_and_one_shots_pass() {
  Directory directory;
  constexpr double hz = 441.0;
  std::vector<float> tone(101'430); // 2.3 s at 44.1 kHz: no bar count fits.
  for (std::size_t i = 0; i < tone.size(); ++i)
    tone[i] = 0.5F * static_cast<float>(
                         std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / 44'100.0));
  const auto file = directory.path() / "Pad long.wav";
  write_wav(file, 44'100, tone);
  const auto prepared = quick::prepare_sample(file, 48'000, 120.0);
  require(prepared.has_value(), "the tone prepares");
  const auto& sample = prepared.value();
  require(!sample.bpm && !sample.warped, "not a loop, not stretched");
  require(sample.left.size() == 110'400, "2.3 s at 48 kHz");
  // One second from the middle crosses zero twice per cycle.
  std::size_t crossings = 0;
  for (std::size_t i = 24'001; i < 72'000; ++i)
    if ((sample.left[i - 1] < 0.0F) != (sample.left[i] < 0.0F))
      ++crossings;
  require(crossings + 4 >= 882 && crossings <= 882 + 4, "the pitch is kept");

  std::vector<float> hit(24'000, 0.0F);
  hit[0] = 1.0F;
  const auto shot = directory.path() / "Snap.wav";
  write_wav(shot, 48'000, hit);
  const auto one_shot = quick::prepare_sample(shot, 48'000, 120.0);
  require(one_shot.has_value() && !one_shot.value().bpm && !one_shot.value().warped &&
              one_shot.value().left == hit,
          "a one-shot at the song's rate is played as it is");
}

} // namespace

int main() {
  the_library_lists_samples();
  a_loop_is_stretched_to_the_song();
  rates_convert_and_one_shots_pass();
}
