#include "../../../apps/daw_quick/src/groove_capture_controller.hpp"

#include "oscillate/export/wav_writer.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

// ADR-0079: the Groove from a record panel's controller takes a groove from
// a library file on a worker, publishes its fingerprint and A/B files, and
// keeps it in the pool under the name in the panel.
namespace {
namespace application = oscillate::application;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

constexpr std::uint32_t rate = 48'000;
constexpr double bpm = 124.0;
constexpr double quarter = 60.0 / bpm * rate;
constexpr std::size_t bars = 10;
constexpr int wait_ms = 60'000;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern =
        (fs::temp_directory_path() / "oscillate-groove-capture-ui-XXXXXX").string();
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

// Kicks on the beat, 16th hats with the off-16ths a quarter of a step late.
void write_record(const fs::path& file) {
  const auto frames = static_cast<std::size_t>(quarter * 4.0 * static_cast<double>(bars + 1));
  std::vector<float> stereo(frames * 2, 0.0F);
  const auto add = [&](double at, std::size_t i, double value) {
    const auto n = static_cast<std::size_t>(std::llround(at)) + i;
    if (n < frames) {
      stereo[n * 2] += static_cast<float>(value);
      stereo[n * 2 + 1] += static_cast<float>(value);
    }
  };
  for (std::size_t beat = 0; beat < bars * 4; ++beat) {
    double phase = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.25 * rate); ++i) {
      const double t = static_cast<double>(i) / rate;
      phase += 2.0 * std::numbers::pi * (50.0 + 80.0 * std::exp(-t / 0.02)) / rate;
      add(quarter * static_cast<double>(beat), i,
          (beat % 4 == 0 ? 0.8 : 0.6) * std::sin(phase) * std::exp(-t / 0.12));
    }
  }
  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  for (std::size_t step = 0; step < bars * 16; ++step) {
    const double at = quarter / 4.0 * (static_cast<double>(step) + (step % 2 == 1 ? 0.25 : 0.0));
    double previous = 0.0;
    double earlier = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.04 * rate); ++i) {
      state ^= state << 13U;
      state ^= state >> 7U;
      state ^= state << 17U;
      const double x =
          static_cast<double>(state >> 11U) / static_cast<double>(1ULL << 53U) * 2.0 - 1.0;
      const double y = (x - 2.0 * previous + earlier) * 0.25;
      earlier = previous;
      previous = x;
      add(at, i, 0.5 * y * std::exp(-static_cast<double>(i) / rate / 0.012));
    }
  }
  auto writer = oscillate::exporting::WavWriter::begin(
      file, {rate, oscillate::exporting::WavFormat::float32, frames, 0});
  require(static_cast<bool>(writer), "the record is written");
  require(static_cast<bool>(writer.value().write_interleaved_stereo(stereo)), "its samples");
  require(static_cast<bool>(writer.value().finish()), "and finished");
}

void a_record_gives_a_groove_for_the_pool() {
  Directory directory;
  const auto record = directory.path() / "Swung hats.wav";
  write_record(record);
  std::optional<oscillate::persistence::GrooveTemplate> kept;
  QString reported;
  quick::GrooveCaptureController controller(
      [] { return static_cast<application::ProjectSession*>(nullptr); },
      [&](oscillate::persistence::GrooveTemplate groove) {
        kept = std::move(groove);
        return true;
      },
      [&](QString message) { reported = std::move(message); });

  controller.take();
  require(!controller.running(), "nothing to listen to before a reference");
  require(!controller.save(), "nothing to keep before a take");

  controller.use_file(QString::fromStdString(record.string()));
  require(controller.source_is_file() && controller.source_name() == QStringLiteral("Swung hats"),
          "the record is the reference");
  require(controller.property("name").toString() == QStringLiteral("Swung hats groove"),
          "the groove is named after it");
  controller.setProperty("band", static_cast<int>(application::groove_capture::Band::hats));
  controller.setProperty("bars", 8);
  controller.take();
  require(controller.running(), "it listens on a worker");
  require(controller.wait_for_take(wait_ms), "and finishes: " + reported.toStdString());
  require(controller.has_result(), "a groove: " + controller.status().toStdString());

  const auto result = controller.result();
  require(std::abs(result.value(QStringLiteral("bpm")).toDouble() - bpm) < 0.05, "its tempo");
  require(std::abs(result.value(QStringLiteral("swing")).toDouble() - 62.5) < 2.0,
          "its swing: " + std::to_string(result.value(QStringLiteral("swing")).toDouble()));
  const auto steps = result.value(QStringLiteral("steps")).toList();
  require(steps.size() == 16, "a step a 16th");
  require(std::abs(steps[1].toMap().value(QStringLiteral("timing")).toDouble() - 0.25) < 0.03,
          "the off-16ths drag");
  require(result.value(QStringLiteral("barPeaks")).toList().size() ==
              static_cast<qsizetype>(application::groove_capture::bar_peak_points),
          "a bar of waveform");
  require(!result.value(QStringLiteral("summary")).toString().isEmpty(), "plain words");
  require(QFileInfo(controller.audition_straight()).size() > 0 &&
              QFileInfo(controller.audition_grooved()).size() > 0,
          "the A/B files are written");

  controller.setProperty("name", QStringLiteral("Hats from a record"));
  require(controller.save() && kept && kept->name == "Hats from a record" &&
              kept->steps.size() == 16,
          "it is kept under the panel's name");
}

} // namespace

int main(int argc, char** argv) {
  QStandardPaths::setTestModeEnabled(true);
  QCoreApplication app(argc, argv);
  try {
    a_record_gives_a_groove_for_the_pool();
  } catch (const std::exception& error) {
    std::cerr << "groove capture controller: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
