#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
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
    std::string pattern = (fs::temp_directory_path() / "oscillate-quick-named-XXXXXX").string();
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

void named_revision_bridge_checks() {
  Directory directory;
  const auto root = directory.root() / "song";
  auto opened = app::ProjectSession::create(root);
  require(opened.has_value(), "project create failed");
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
      return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
    });
    bridge.refresh_named_revisions();
    bridge.create_named_revision(QStringLiteral("No project"));
    bridge.restore_named_revision(QStringLiteral("No project"));
    require(!bridge.named_revision_enabled() && bridge.named_revisions().empty() &&
                bridge.status().contains(QStringLiteral("Stop playback")),
            "Quick enabled named revisions without an open project");
    bridge.set_project(std::move(opened.value()), root);
    bridge.add_instrument_track();
    bridge.save();
    bridge.create_named_revision(QStringLiteral("First mix"));
    require(bridge.named_revisions().size() == 1 &&
                bridge.named_revisions().front().toMap().value(QStringLiteral("name")) ==
                    QStringLiteral("First mix") &&
                bridge.status().contains(QStringLiteral("saved")),
            "Quick did not show saved named revision");
    std::ofstream(root / "state" / "revisions" / std::string(32, 'e')) << "not json";
    bridge.refresh_named_revisions();
    require(bridge.named_revision_issues().size() == 1 &&
                bridge.named_revision_issues().front().toMap().value(QStringLiteral("reason")) ==
                    QStringLiteral("Damaged entry") &&
                bridge.named_revision_issues().front().toMap().value(QStringLiteral("name")) ==
                    QStringLiteral("Entry ") + QString(32, 'e'),
            "Quick did not expose a damaged revision entry");
    bridge.create_named_revision(QStringLiteral("First mix"));
    require(bridge.named_revisions().size() == 1 &&
                bridge.status().contains(QStringLiteral("already exists")),
            "Quick duplicate name was not rejected");
    bridge.add_instrument_track();
    const auto unsaved = bridge.revision();
    bridge.restore_named_revision(QStringLiteral("First mix"));
    require(bridge.revision() == unsaved && bridge.tracks().size() == 2 &&
                bridge.status().contains(QStringLiteral("Save current")),
            "Quick restore silently discarded unsaved edit");
    bridge.save();
    bridge.restore_named_revision(QStringLiteral("First mix"));
    require(bridge.revision() == unsaved + 1 && bridge.tracks().size() == 1 &&
                bridge.status().contains(QStringLiteral("Restored")) &&
                bridge.playback_status().contains(QStringLiteral("next Play")),
            "Quick restore did not show model and graph status separately");
    bridge.undo();
    require(bridge.tracks().size() == 2 &&
                bridge.playback_status().contains(
                    QStringLiteral("model revision %1").arg(bridge.revision())),
            "Quick restore undo left stale graph status");
    bridge.redo();
    require(bridge.tracks().size() == 1 &&
                bridge.playback_status().contains(
                    QStringLiteral("model revision %1").arg(bridge.revision())),
            "Quick restore redo left stale graph status");
    bridge.save();
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().tracks.size() == 1 &&
              reopened.value()->saving().list_named_revisions().value().revisions.size() == 1,
          "Quick restored CURRENT or named pointer did not reopen");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OscillateTest"));
  QCoreApplication::setApplicationName(QStringLiteral("QuickNamedRevisions"));
  try {
    named_revision_bridge_checks();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Quick named revision: %s\n", error.what());
    return 1;
  }
}
