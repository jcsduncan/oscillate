#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace app = oscillate::application;
namespace foundation = oscillate::foundation;
namespace quick = oscillate::quick;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-quick-export-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "temporary directory failed");
    root_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  [[nodiscard]] const fs::path& root() const { return root_; }

private:
  fs::path root_;
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

fs::path source_wav(const fs::path& root) {
  constexpr std::uint32_t frames = 256;
  std::vector<std::uint8_t> wav{'R', 'I', 'F', 'F'};
  put32(wav, 36 + frames * 2);
  wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(wav, 16);
  put16(wav, 1);
  put16(wav, 1);
  put32(wav, 48'000);
  put32(wav, 96'000);
  put16(wav, 2);
  put16(wav, 16);
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  put32(wav, frames * 2);
  for (std::uint32_t index = 0; index < frames; ++index)
    put16(wav, 8192);
  const auto path = root / "source.wav";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  require(output.good(), "source WAV write failed");
  return path;
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "rendered WAV did not open");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::path only_wav(const fs::path& folder) {
  fs::path found;
  for (const auto& item : fs::directory_iterator(folder)) {
    if (item.path().extension() == ".wav") {
      require(found.empty(), "stem bundle contained an unexpected second WAV");
      found = item.path();
    }
  }
  require(!found.empty(), "stem bundle had no WAV");
  return found;
}

void wait_export(quick::ProjectBridge& bridge) {
  if (bridge.exports()->export_busy()) {
    require(QTest::qWaitFor([&bridge] { return !bridge.exports()->export_busy(); }, 15'000),
            "Quick export worker did not finish");
  }
}

void export_recovery_checks() {
  Directory directory;
  const auto project_root = directory.root() / "song";
  auto session = app::ProjectSession::create(project_root);
  require(session.has_value(), "project create failed");
  const auto source = source_wav(directory.root());
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "source WAV open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value() && session.value()->saving().save().has_value(),
          "audio project import or save failed");
  session.value().reset();
  auto reopened = app::ProjectSession::open(project_root);
  require(reopened.has_value(), "saved audio project did not reopen");

  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(reopened.value()), project_root);
  const auto revision = bridge.revision();
  const auto first_folder = project_root / ("stems-rev-" + std::to_string(revision));
  const auto second_folder = fs::path(first_folder.string() + "-2");

  bridge.exports()->export_stems();
  require(bridge.exports()->export_busy(), "first stem render did not start");
  wait_export(bridge);
  require(bridge.exports()->export_status().startsWith(
              QStringLiteral("WAV stems exported from revision")) &&
              fs::is_directory(first_folder),
          "first stem render did not publish");
  const auto first_stem = only_wav(first_folder);
  const auto first_bytes = read_bytes(first_stem);
  require(first_bytes.size() > 56 && first_bytes[0] == 'R' && first_bytes[1] == 'I' &&
              std::ranges::any_of(first_bytes.begin() + 56, first_bytes.end(),
                                  [](std::uint8_t sample_byte) { return sample_byte != 0; }),
          "first stem was not a non-silent WAV");

  bridge.exports()->export_stems();
  require(bridge.exports()->export_busy(), "second stem render did not start");
  wait_export(bridge);
  require(bridge.exports()->export_status().startsWith(
              QStringLiteral("WAV stems exported from revision")) &&
              fs::is_directory(second_folder) && read_bytes(first_stem) == first_bytes &&
              read_bytes(only_wav(second_folder)) == first_bytes,
          "repeated stem render replaced the earlier bundle or changed its audio");

  const auto blocked_master = directory.root() / "blocked-master.wav";
  require(fs::create_directory(blocked_master), "blocked target fixture failed");
  bridge.exports()->set_export_target(QString::fromStdString(blocked_master.string()));
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("export failed")) &&
              !bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::is_directory(blocked_master) && read_bytes(first_stem) == first_bytes &&
              bridge.revision() == revision,
          "failed master publish reported success or changed the earlier stem/project");

  fs::remove(blocked_master);
  bridge.exports()->export_master();
  wait_export(bridge);
  require(
      bridge.exports()->export_status().startsWith(QStringLiteral("WAV exported from revision")) &&
          fs::is_regular_file(blocked_master) && read_bytes(blocked_master) == first_bytes &&
          read_bytes(first_stem) == first_bytes && bridge.revision() == revision,
      "master retry did not publish while preserving the earlier stem and project revision");
}

// Opens the saved one-clip project at `project_root` in a fresh bridge.
void open_audio_project(quick::ProjectBridge& bridge, const fs::path& root,
                        const fs::path& project_root) {
  auto session = app::ProjectSession::create(project_root);
  require(session.has_value(), "project create failed");
  const auto source = source_wav(root);
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "source WAV open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value() && session.value()->saving().save().has_value(),
          "audio project import or save failed");
  session.value().reset();
  auto reopened = app::ProjectSession::open(project_root);
  require(reopened.has_value(), "saved audio project did not reopen");
  bridge.set_project(std::move(reopened.value()), project_root);
}

// The proposed target is <Music>/oscillate/<project>.<ext>, its folder is
// made on export, and a proposed name never replaces an existing file.
void default_target_checks() {
  Directory directory;
  // A private home: the Music folder falls back to $HOME/Music.
  require(::setenv("HOME", directory.root().c_str(), 1) == 0 &&
              ::setenv("XDG_CONFIG_HOME", (directory.root() / "config").c_str(), 1) == 0,
          "home fixture failed");
  const auto folder = directory.root() / "Music" / "oscillate";
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  open_audio_project(bridge, directory.root(), directory.root() / "Night Drive");
  require(fs::path(bridge.exports()->export_target().toStdString()) == folder / "Night Drive.wav" &&
              bridge.exports()->export_file_name() == QStringLiteral("Night Drive") &&
              bridge.exports()->export_extension() == QStringLiteral(".wav") &&
              !fs::exists(folder) && bridge.exports()->export_trim_tail() &&
              bridge.exports()->export_saved_path().isEmpty(),
          "default export target was not the Music folder proposal");

  bridge.exports()->export_master();
  wait_export(bridge);
  require(
      bridge.exports()->export_status().startsWith(QStringLiteral("WAV exported from revision")) &&
          fs::is_regular_file(folder / "Night Drive.wav") &&
          fs::path(bridge.exports()->export_saved_path().toStdString()) ==
              folder / "Night Drive.wav",
      "default export did not create its folder and publish there");
  require(fs::path(bridge.exports()->export_target().toStdString()) ==
                  folder / "Night Drive 2.wav" &&
              !bridge.exports()->export_replaces_file(),
          "the next proposal would replace the file just exported");
  const auto first_bytes = read_bytes(folder / "Night Drive.wav");
  bridge.exports()->export_master();
  wait_export(bridge);
  require(fs::is_regular_file(folder / "Night Drive 2.wav") &&
              read_bytes(folder / "Night Drive.wav") == first_bytes,
          "a second default export replaced the first");

  // A file appearing under the proposed name moves the proposal on.
  std::ofstream(folder / "Night Drive 3.wav") << "taken";
  bridge.exports()->refresh_export_target();
  require(fs::path(bridge.exports()->export_target().toStdString()) == folder / "Night Drive 4.wav",
          "a taken proposal was not renumbered");

  // The user's own name is kept, renamed with the format, and flagged when
  // it would replace a file.
  bridge.exports()->set_export_file_name(QStringLiteral("Night Drive"));
  require(fs::path(bridge.exports()->export_target().toStdString()) == folder / "Night Drive.wav" &&
              bridge.exports()->export_replaces_file(),
          "a typed existing name was renumbered or not flagged");
  bridge.exports()->refresh_export_target();
  require(fs::path(bridge.exports()->export_target().toStdString()) == folder / "Night Drive.wav",
          "refresh replaced the user's chosen name");
  const auto elsewhere = directory.root() / "renders";
  bridge.exports()->set_export_folder(QString::fromStdString(elsewhere.string()));
  require(fs::path(bridge.exports()->export_target().toStdString()) ==
                  elsewhere / "Night Drive.wav" &&
              !bridge.exports()->export_replaces_file(),
          "folder edit did not keep the file name");
  bridge.exports()->set_export_trim_tail(false);
  require(!bridge.exports()->export_trim_tail(), "trim-tail option did not store");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OscillateTest"));
  QCoreApplication::setApplicationName(QStringLiteral("QuickExportRecovery"));
  try {
    export_recovery_checks();
    default_target_checks();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Quick export recovery: %s\n", error.what());
    return 1;
  }
  return 0;
}
