#include "../../../apps/daw_quick/src/managed_media_controller.hpp"
#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <QCoreApplication>

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
    std::string pattern = (fs::temp_directory_path() / "oscillate-quick-repair-XXXXXX").string();
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

void write_fixture(const fs::path& path) {
  constexpr unsigned char wav[]{'R',  'I',  'F', 'F', 40, 0,    0, 0, 'W', 'A',  'V', 'E',
                                'f',  'm',  't', ' ', 16, 0,    0, 0, 1,   0,    1,   0,
                                0x80, 0xbb, 0,   0,   0,  0x77, 1, 0, 2,   0,    16,  0,
                                'd',  'a',  't', 'a', 4,  0,    0, 0, 0,   0x20, 0,   0x20};
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav), sizeof(wav));
  require(output.good(), "fixture write failed");
}

void wait_for_repair(quick::ProjectBridge& bridge) {
  oscillate::test::wait_until(
      [&] {
        QCoreApplication::processEvents();
        return !bridge.managed_media()->busy();
      },
      "the Quick repair worker finished");
  QCoreApplication::processEvents();
  require(!bridge.managed_media()->busy(), "Quick repair worker timed out");
}

void quick_repair_checks() {
  Directory directory;
  const auto root = directory.root() / "song";
  const auto exact = directory.root() / "exact.wav";
  const auto wrong = directory.root() / "wrong.wav";
  write_fixture(exact);
  fs::copy_file(exact, wrong);
  {
    std::fstream changed(wrong, std::ios::binary | std::ios::in | std::ios::out);
    changed.seekp(44);
    changed.put('\x7f');
  }
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "project create failed");
  const auto descriptor = ::open(exact.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "fixture open failed");
  const auto imported = created.value()->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "fixture import failed");
  const auto baseline = created.value()->snapshot();
  const auto source = baseline.sources.front();
  const auto asset = root / "assets" / (source.sha256 + ".wav");
  created.value().reset();
  fs::remove(asset);
  auto opened = app::ProjectSession::open(root);
  require(opened.has_value(), "project with missing media did not open");

  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
      return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
    });
    bridge.set_project(std::move(opened.value()), root);
    require(bridge.managed_media()->issues().size() == 1 &&
                bridge.managed_media()
                        ->issues()
                        .front()
                        .toMap()
                        .value(QStringLiteral("id"))
                        .toString() == QString::fromStdString(source.id.to_string()) &&
                bridge.managed_media()->issues().front().toMap().value(QStringLiteral("status")) ==
                    QStringLiteral("Missing") &&
                bridge.managed_media()->issues().front().toMap().value(
                    QStringLiteral("repairable")) == true,
            "Quick did not expose exact saved missing-media identity");
    const auto before_revision = bridge.revision();
    bridge.managed_media()->repair(QString::fromStdString(source.id.to_string()),
                                   QUrl::fromLocalFile(QString::fromStdString(wrong.string())));
    require(bridge.managed_media()->busy() && !bridge.playback_enabled() &&
                !bridge.managed_media()->repair_enabled(),
            "Quick left replacement or playback actions enabled during repair");
    wait_for_repair(bridge);
    require(bridge.managed_media()->repair_enabled() &&
                bridge.managed_media()->status().contains(QStringLiteral("Repair not confirmed")) &&
                bridge.managed_media()->issues().size() == 1 && !fs::exists(asset) &&
                bridge.revision() == before_revision,
            "Quick wrong-file failure changed project state, hid missing media, or left repair "
            "disabled");

    fs::copy_file(exact, asset);
    fs::resize_file(asset, 0);
    bridge.managed_media()->refresh();
    require(bridge.managed_media()->issues().size() == 1 &&
                bridge.managed_media()->issues().front().toMap().value(QStringLiteral("status")) ==
                    QStringLiteral("Damaged") &&
                !bridge.managed_media()
                     ->issues()
                     .front()
                     .toMap()
                     .value(QStringLiteral("repairable"))
                     .toBool(),
            "Quick did not distinguish damaged managed media from missing media");
    bridge.managed_media()->repair(QString::fromStdString(source.id.to_string()),
                                   QUrl::fromLocalFile(QString::fromStdString(exact.string())));
    require(!bridge.managed_media()->busy() &&
                bridge.managed_media()->status().contains(QStringLiteral("Only missing")) &&
                fs::file_size(asset) == 0,
            "Quick replaced a damaged existing managed asset");
    fs::remove(asset);
    bridge.managed_media()->refresh();

    bridge.managed_media()->repair(QString::fromStdString(source.id.to_string()),
                                   QUrl::fromLocalFile(QString::fromStdString(exact.string())));
    require(bridge.managed_media()->busy(), "Quick did not dispatch exact-file repair");
    wait_for_repair(bridge);
    require(bridge.managed_media()->status().contains(QStringLiteral("restored and verified")) &&
                bridge.managed_media()->issues().empty() && bridge.revision() == before_revision,
            "Quick did not refresh repaired media without an edit");
    bridge.save();
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().sources == baseline.sources &&
              reopened.value()->snapshot().tracks == baseline.tracks &&
              reopened.value()->media_repair().managed_media_issues().value().empty(),
          "Quick repaired saved source did not reopen unchanged");
  reopened.value().reset();

  fs::remove(asset);
  auto close_session = app::ProjectSession::open(root);
  require(close_session.has_value(), "project did not reopen with missing media for close test");
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
      return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
    });
    bridge.set_project(std::move(close_session.value()), root);
    const auto revision = bridge.revision();
    bridge.managed_media()->repair(QString::fromStdString(source.id.to_string()),
                                   QUrl(QStringLiteral("https://example.invalid/replacement.wav")));
    require(!bridge.managed_media()->busy() &&
                bridge.managed_media()->status().contains(QStringLiteral("local replacement")) &&
                bridge.revision() == revision,
            "nonlocal replacement began repair or changed the project");

    bool ready_to_close = false;
    QObject::connect(&bridge, &quick::ProjectBridge::readyToClose, &bridge,
                     [&ready_to_close] { ready_to_close = true; });
    bridge.managed_media()->repair(QString::fromStdString(source.id.to_string()),
                                   QUrl::fromLocalFile(QString::fromStdString(wrong.string())));
    require(bridge.managed_media()->busy(), "close test did not start repair");
    require(!bridge.request_close(), "close accepted while repair still owned a worker");
    require(bridge.managed_media()->status().contains(QStringLiteral("before closing")),
            "close did not expose managed-media cancellation");
    oscillate::test::wait_until(
        [&] {
          QCoreApplication::processEvents();
          return ready_to_close;
        },
        "the repair let the window close");
    require(ready_to_close && !bridge.managed_media()->busy() && bridge.revision() == revision &&
                !fs::exists(asset) && bridge.request_close(),
            "close did not wait for worker retirement or changed the saved source");
  }
}

void unsaved_revision_repair_guard_checks() {
  Directory directory;
  const auto root = directory.root() / "unsaved";
  const auto exact = directory.root() / "source.wav";
  write_fixture(exact);
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "revision-guard project create failed");
  const auto descriptor = ::open(exact.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "revision-guard fixture open failed");
  const auto imported = created.value()->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "revision-guard import failed");
  const auto source = created.value()->snapshot().sources.front();
  created.value().reset();
  fs::remove(root / "assets" / (source.sha256 + ".wav"));

  auto opened = app::ProjectSession::open(root);
  require(opened.has_value(), "revision-guard project reopen failed");
  const auto edited =
      opened.value()->tracks().add_bus_track(oscillate::project::TrackKind::group_bus);
  require(edited.has_value(), "revision-guard model edit failed");
  const auto revision = opened.value()->snapshot().revision;
  bool repair_allowed = false;
  quick::ManagedMediaController controller([&] { return repair_allowed; });
  controller.set_project(opened.value().get());
  controller.refresh();
  require(controller.issues().size() == 1, "revision-guard missing source was not listed");
  controller.repair(QString::fromStdString(source.id.to_string()),
                    QUrl::fromLocalFile(QString::fromStdString(exact.string())));
  require(!controller.busy() && controller.status().contains(QStringLiteral("Stop playback")) &&
              opened.value()->snapshot().revision == revision,
          "media owner bypassed its host repair admission gate");
  repair_allowed = true;
  controller.repair(QString::fromStdString(source.id.to_string()),
                    QUrl::fromLocalFile(QString::fromStdString(exact.string())));
  require(!controller.busy() &&
              controller.status().contains(QStringLiteral("Save current edits")) &&
              opened.value()->snapshot().revision == revision,
          "unsaved revision repair was accepted or lost the model edit");
}

void external_link_is_not_managed_repair() {
  Directory directory;
  const auto root = directory.root() / "linked";
  const auto original = directory.root() / "original.wav";
  const auto replacement = directory.root() / "replacement.wav";
  write_fixture(original);
  fs::copy_file(original, replacement);
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "external-guard project create failed");
  const auto descriptor = ::open(original.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "external-guard fixture open failed");
  const auto imported = created.value()->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  require(imported.has_value(), "external-guard import failed");
  auto state = created.value()->snapshot();
  const auto source_id = state.sources.front().id;
  state.sources.front().external_link = oscillate::project::ExternalSourceLink{original.string()};
  ++state.revision;
  created.value().reset();
  {
    auto writer = oscillate::persistence::ProjectStore::open_writer(root, false);
    require(writer.has_value(), "external-guard writer open failed");
    require(writer.value().save(state, oscillate::persistence::SaveKind::explicit_save).has_value(),
            "external-guard linked save failed");
  }
  fs::remove(original);
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "external-guard project reopen failed");
  quick::ManagedMediaController controller;
  controller.set_project(reopened.value().get());
  controller.refresh();
  require(controller.issues().size() == 1 &&
              controller.issues().front().toMap().value(QStringLiteral("status")) ==
                  QStringLiteral("External link missing") &&
              controller.issues().front().toMap().value(QStringLiteral("external")).toBool() &&
              controller.issues().front().toMap().value(QStringLiteral("lastKnownPath")) ==
                  QString::fromStdString(original.string()) &&
              !controller.issues().front().toMap().value(QStringLiteral("repairable")).toBool(),
          "missing external link was offered as a managed asset repair");
  controller.repair(QString::fromStdString(source_id.to_string()),
                    QUrl::fromLocalFile(QString::fromStdString(replacement.string())));
  require(!controller.busy() &&
              controller.status().contains(QStringLiteral("requires relinking")) &&
              reopened.value()->snapshot().revision == state.revision &&
              reopened.value()->snapshot().sources.front() == state.sources.front(),
          "managed repair accepted an external link or changed saved identity");

  const auto wrong = directory.root() / "wrong-external.wav";
  fs::copy_file(replacement, wrong);
  {
    std::fstream changed(wrong, std::ios::binary | std::ios::in | std::ios::out);
    changed.seekp(44);
    changed.put('\x7f');
  }
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
      return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
    });
    bridge.set_project(std::move(reopened.value()), root);
    const auto before_revision = bridge.revision();
    bridge.managed_media()->relink(QString::fromStdString(source_id.to_string()),
                                   QUrl(QStringLiteral("https://example.invalid/source.wav")));
    require(!bridge.managed_media()->busy() && bridge.revision() == before_revision &&
                bridge.managed_media()->status().contains(QStringLiteral("local audio file")),
            "nonlocal external relink candidate started a worker");
    bridge.managed_media()->relink(QString::fromStdString(source_id.to_string()),
                                   QUrl::fromLocalFile(QString::fromStdString(wrong.string())));
    require(bridge.managed_media()->busy() && !bridge.playback_enabled() &&
                !bridge.imports()->import_enabled() && !bridge.exports()->export_enabled(),
            "external relink did not reserve the media worker boundary");
    wait_for_repair(bridge);
    require(bridge.revision() == before_revision &&
                bridge.managed_media()->status().contains(QStringLiteral("not confirmed")) &&
                bridge.managed_media()->issues().size() == 1,
            "wrong external candidate changed the project or hid the media issue");

    bridge.managed_media()->relink(
        QString::fromStdString(source_id.to_string()),
        QUrl::fromLocalFile(QString::fromStdString(replacement.string())));
    require(bridge.managed_media()->busy(), "exact external relink did not start verification");
    wait_for_repair(bridge);
    require(bridge.revision() == before_revision + 1 &&
                bridge.managed_media()->status().contains(QStringLiteral("relinked and saved")) &&
                bridge.managed_media()->issues().empty(),
            "exact external relink did not publish and refresh the saved project");
  }
  auto linked = app::ProjectSession::open(root);
  require(linked.has_value(), "Quick external relink project did not reopen");
  const auto& saved = linked.value()->snapshot();
  const auto& link = saved.sources.front().external_link;
  if (!link)
    throw std::runtime_error("Quick external relink lost source location");
  require(saved.revision == state.revision + 1 && saved.sources.front().id == source_id &&
              link->last_known_path == replacement.string() &&
              linked.value()->media_repair().managed_media_issues().value().empty(),
          "Quick external relink did not survive project reopen");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OscillateTest"));
  QCoreApplication::setApplicationName(QStringLiteral("QuickManagedMediaRepair"));
  try {
    quick_repair_checks();
    unsaved_revision_repair_guard_checks();
    external_link_is_not_managed_repair();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Quick managed media repair: %s\n", error.what());
    return 1;
  }
}
