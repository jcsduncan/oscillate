#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
namespace app = oscillate::application;
namespace foundation = oscillate::foundation;
namespace quick = oscillate::quick;

using oscillate::test::require;

// Comfortably older than the test process, whatever the filesystem's clock.
constexpr auto stale_stage_age = std::chrono::hours(1);

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-project-library-XXXXXX").string();
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

void project_library_recovery_checks() {
  Directory directory;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromStdString((directory.root() / "settings").string()));
  const auto library = directory.root() / ".oscillate" / "projects";
  const auto first = library / "First song";
  const auto damaged = library / "Second song";
  const auto external = directory.root() / "external" / "Outside song";
  const auto discover = [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  };

  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
    require(!bridge.project_open() && bridge.available_projects().empty(),
            "new library did not start at welcome");
    bridge.create_default_project(QStringLiteral(" First song "));
    require(bridge.project_open() && bridge.project_name() == QStringLiteral("First song") &&
                fs::is_regular_file(first / "state" / "CURRENT") &&
                bridge.available_projects().size() == 1,
            "default project was not created and listed");
    const auto revision = bridge.revision();

    bridge.create_default_project(QStringLiteral("../escape"));
    require(bridge.revision() == revision &&
                bridge.status().contains(QStringLiteral("valid project name")) &&
                !fs::exists(directory.root() / ".oscillate" / "escape"),
            "invalid project name changed the open project or escaped the library");
    bridge.create_default_project(QStringLiteral("First song"));
    require(bridge.revision() == revision && bridge.available_projects().size() == 1 &&
                bridge.status().contains(QStringLiteral("already exists")),
            "duplicate create replaced the open project");

    bridge.create_default_project(QStringLiteral("Second song"));
    require(bridge.available_projects().size() == 2,
            "project library did not discover a saved project");
    bridge.open_listed(QString::fromStdString(first.string()));
    require(bridge.project_name() == QStringLiteral("First song"),
            "first project did not reopen before damaged project test");
    std::ofstream(damaged / "state" / "CURRENT", std::ios::trunc) << "invalid revision\n";
    bridge.open_listed(QString::fromStdString(damaged.string()));
    require(bridge.project_open() && bridge.project_name() == QStringLiteral("First song") &&
                bridge.revision() == revision &&
                bridge.recovery_choice().value("active").toBool() &&
                bridge.recovery_choice().value("options").toList().size() == 1,
            "damaged CURRENT did not offer validated recovery while preserving active project");
    bridge.cancel_recovery();

    bridge.open_project(QUrl(QStringLiteral("https://example.invalid/project")));
    require(bridge.project_name() == QStringLiteral("First song") &&
                bridge.status().contains(QStringLiteral("local project folder")),
            "remote project URL displaced the active project");

    fs::create_directories(external.parent_path());
    auto outside = app::ProjectSession::create(external);
    require(outside.has_value(), "external project fixture failed");
    outside.value().reset();
    bridge.open_project(QUrl::fromLocalFile(QString::fromStdString(external.string())));
    require(bridge.project_name() == QStringLiteral("Outside song") &&
                bridge.available_projects().size() == 3,
            "project chooser did not remember an external project");
  }

  fs::remove(damaged / "state" / "CURRENT");
  // Creation stages are never projects. One left by an earlier process is
  // removed at startup; a newer one may belong to a live creation and stays.
  const auto old_stage = library / ".Third song.creating-00112233445566778899aabbccddeeff";
  const auto new_stage = library / ".Fourth song.creating-ffeeddccbbaa99887766554433221100";
  for (const auto& stage : {old_stage, new_stage})
    fs::create_directories(stage / "state" / "generations");
  fs::last_write_time(old_stage, fs::file_time_type::clock::now() - stale_stage_age);
  quick::ProjectBridge reopened(directory.root(), nullptr, {}, discover);
  require(!reopened.project_open() && reopened.available_projects().size() == 3,
          "library did not retain a project with recoverable generations after restart");
  require(!fs::exists(old_stage) && fs::exists(new_stage),
          "startup cleanup did not remove only the stage older than the process");
  reopened.open_listed(QString::fromStdString(damaged.string()));
  require(!reopened.project_open() && reopened.recovery_choice().value("active").toBool(),
          "unpointed project did not offer a recovery choice");
  reopened.cancel_recovery();
  reopened.open_listed(QString::fromStdString(first.string()));
  require(reopened.project_open() && reopened.project_name() == QStringLiteral("First song"),
          "listed project did not reopen after restart");
  reopened.open_listed(QString::fromStdString(external.string()));
  require(reopened.project_open() && reopened.project_name() == QStringLiteral("Outside song"),
          "recent external project was not available after restart");
}

// Close returns to the project screen; a library project can be moved to
// the Trash (never the open one), an outside project only forgotten.
void close_forget_and_trash_projects() {
  Directory directory;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromStdString((directory.root() / "settings").string()));
  // The Trash lives under XDG_DATA_HOME; keep it inside the fixture.
  qputenv("XDG_DATA_HOME", QByteArray::fromStdString((directory.root() / "data").string()));
  fs::create_directories(directory.root() / "data");
  const auto library = directory.root() / ".oscillate" / "projects";
  const auto external = directory.root() / "external" / "Outside song";
  fs::create_directories(external.parent_path());
  auto outside = app::ProjectSession::create(external);
  require(outside.has_value(), "external project fixture failed");
  outside.value().reset();
  const auto discover = [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
  bridge.create_default_project(QStringLiteral("Keep"));
  bridge.create_default_project(QStringLiteral("Bin me"));
  bridge.open_project(QUrl::fromLocalFile(QString::fromStdString(external.string())));
  require(bridge.project_name() == QStringLiteral("Outside song") &&
              bridge.available_projects().size() == 3,
          "close fixture projects were not listed");

  const auto entry = [&bridge](const fs::path& root) {
    for (const auto& item : bridge.available_projects()) {
      const auto map = item.toMap();
      if (map.value("path").toString() == QString::fromStdString(root.string()))
        return map;
    }
    return QVariantMap{};
  };
  require(entry(library / "Keep").value("inLibrary").toBool() &&
              !entry(external).value("inLibrary").toBool() &&
              entry(external).value("open").toBool(),
          "project entries do not say where they live or which is open");

  bridge.trash_project(QString::fromStdString(external.string()));
  require(fs::exists(external) && bridge.status().contains(QStringLiteral("project library")),
          "an outside project was deleted");

  bridge.open_listed(QString::fromStdString((library / "Bin me").string()));
  bridge.trash_project(QString::fromStdString((library / "Bin me").string()));
  require(fs::exists(library / "Bin me") && bridge.status().contains(QStringLiteral("Close")),
          "the open project was deleted");

  bridge.close_project();
  require(!bridge.project_open() && bridge.project_name().isEmpty() &&
              bridge.status() == QStringLiteral("Project closed"),
          "close did not return to the project screen");
  require(!entry(library / "Bin me").value("open").toBool(),
          "a closed project is still marked open");

  bridge.trash_project(QString::fromStdString((library / "Bin me").string()));
  require(!fs::exists(library / "Bin me") &&
              fs::exists(directory.root() / "data" / "Trash" / "files" / "Bin me") &&
              entry(library / "Bin me").isEmpty() && bridge.available_projects().size() == 2,
          "a closed library project was not moved to the Trash and unlisted");

  bridge.forget_project(QString::fromStdString(external.string()));
  require(fs::exists(external) && entry(external).isEmpty() &&
              bridge.available_projects().size() == 1,
          "forgetting an outside project did not just unlist it");

  bridge.rename_project(QString::fromStdString((library / "Keep").string()),
                        QStringLiteral("../escape"));
  require(fs::exists(library / "Keep") && !fs::exists(directory.root() / ".oscillate" / "escape"),
          "an invalid rename moved the project");
  bridge.rename_project(QString::fromStdString((library / "Keep").string()),
                        QStringLiteral(" Kept "));
  require(!fs::exists(library / "Keep") &&
              fs::is_regular_file(library / "Kept" / "state" / "CURRENT") &&
              !entry(library / "Kept").isEmpty() && entry(library / "Keep").isEmpty(),
          "a closed library project was not renamed and relisted");

  bridge.open_listed(QString::fromStdString((library / "Kept").string()));
  require(bridge.project_open() && bridge.project_name() == QStringLiteral("Kept"),
          "a renamed project did not open");
  bridge.rename_project(QString::fromStdString((library / "Kept").string()),
                        QStringLiteral("Open one"));
  require(fs::exists(library / "Kept") && bridge.status().contains(QStringLiteral("Close")),
          "the open project was renamed");
}

void explicit_autosave_choice_and_pointer_repair() {
  Directory directory;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromStdString((directory.root() / "settings").string()));
  const auto root = directory.root() / ".oscillate" / "projects" / "Recovery song";
  fs::create_directories(root.parent_path());
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "recovery project fixture failed");
  const auto baseline = session.value()->snapshot().revision;
  auto added = session.value()->tracks().add_instrument_track();
  require(added.has_value(), "autosave track fixture failed");
  const auto recovered = session.value()->snapshot();
  session.value().reset();
  std::string original_generation;
  std::string autosaved_generation;
  {
    auto writer = oscillate::persistence::ProjectStore::open_writer(root, false);
    require(writer.has_value(), "autosave writer fixture failed");
    auto autosaved = writer.value().save(recovered, oscillate::persistence::SaveKind::autosave);
    require(autosaved.has_value(), "autosave fixture failed");
    auto original = writer.value().load_current();
    require(original.has_value(), "last-save fixture missing");
    original_generation = original.value().generation;
    autosaved_generation = autosaved.value().generation;
  }

  const auto discover = [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  };
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
    bridge.open_listed(QString::fromStdString(root.string()));
    require(bridge.recovery_choice().value("active").toBool(),
            "autosave recovery was not offered before pointer race fixture");
    const auto stale_generation = autosaved_generation;
    {
      auto writer = oscillate::persistence::ProjectStore::open_writer(root, false);
      require(writer.has_value(), "pointer-race writer fixture failed");
      auto advanced = writer.value().save(recovered, oscillate::persistence::SaveKind::autosave);
      require(advanced.has_value(), "pointer-race autosave fixture failed");
      autosaved_generation = advanced.value().generation;
    }
    bridge.choose_recovery(QString::fromStdString(stale_generation), QStringLiteral("autosave"));
    require(!bridge.project_open() && bridge.status().contains(QStringLiteral("pointer changed")),
            "stale autosave choice replaced a newer recovery pointer");
    bridge.cancel_recovery();
  }
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
    bridge.open_listed(QString::fromStdString(root.string()));
    const auto choices = bridge.recovery_choice().value("options").toList();
    require(!bridge.project_open() && choices.size() == 2 &&
                choices[0].toMap().value("kind").toString() == QStringLiteral("saved") &&
                choices[1].toMap().value("kind").toString() == QStringLiteral("autosave"),
            "last saved and autosave were not presented as separate choices");
    bridge.choose_recovery(QString::fromStdString(original_generation), QStringLiteral("saved"));
    require(bridge.project_open() && bridge.revision() == baseline && bridge.tracks().empty() &&
                bridge.recovery_choice().isEmpty(),
            "last saved choice did not open the explicit generation");
  }
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
    bridge.open_listed(QString::fromStdString(root.string()));
    bridge.choose_recovery(QString::fromStdString(autosaved_generation),
                           QStringLiteral("autosave"));
    require(bridge.project_open() && bridge.revision() == recovered.revision &&
                bridge.tracks().size() == 1 &&
                bridge.status().contains(QStringLiteral("unsaved work")),
            "autosave choice did not open recovered unsaved state");
    auto reader = oscillate::persistence::ProjectStore::open_reader(root);
    require(reader.has_value() &&
                reader.value().load_current().value().generation == original_generation,
            "opening autosave silently advanced CURRENT");
    bridge.save();
    auto after = reader.value().load_current();
    require(after.has_value() && after.value().snapshot.revision == recovered.revision &&
                after.value().generation != original_generation,
            "explicit save did not promote recovered work to CURRENT");
  }
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
    bridge.open_listed(QString::fromStdString(root.string()));
    require(bridge.project_open() && bridge.recovery_choice().isEmpty(),
            "autosave at the same revision caused a repeated recovery prompt");
  }
  std::ofstream(root / "state" / "CURRENT", std::ios::trunc) << "bad pointer\n";
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
  bridge.open_listed(QString::fromStdString(root.string()));
  const auto options = bridge.recovery_choice().value("options").toList();
  const auto repaired = std::ranges::find_if(options, [](const auto& item) {
    return item.toMap().value("kind").toString() == QStringLiteral("saved");
  });
  require(!bridge.project_open() && repaired != options.end() &&
              !bridge.recovery_choice().value("hasCurrent").toBool(),
          "corrupt CURRENT did not offer a validated saved-generation repair");
  bridge.choose_recovery(repaired->toMap().value("generation").toString(), QStringLiteral("saved"));
  auto fixed = oscillate::persistence::ProjectStore::open_reader(root);
  require(bridge.project_open() && fixed.has_value() && fixed.value().load_current().has_value(),
          "chosen saved generation did not repair CURRENT and open");
}

void equal_revision_autosave_stays_unsaved() {
  Directory directory;
  const auto root = directory.root() / "equal-revision";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "equal-revision project fixture failed");
  const auto snapshot = session.value()->snapshot();
  session.value().reset();
  std::string autosave_generation;
  {
    auto writer = oscillate::persistence::ProjectStore::open_writer(root, false);
    require(writer.has_value(), "equal-revision writer fixture failed");
    auto saved = writer.value().save(snapshot, oscillate::persistence::SaveKind::autosave);
    require(saved.has_value(), "equal-revision autosave fixture failed");
    autosave_generation = saved.value().generation;
  }
  auto restored = app::ProjectSession::open_recovered(
      root, autosave_generation, oscillate::persistence::SaveKind::autosave, true);
  require(restored.has_value() && restored.value()->saving().has_unsaved_changes(),
          "autosave with same model revision was incorrectly treated as explicitly saved");
  auto committed = restored.value()->saving().save();
  require(committed.has_value() && !restored.value()->saving().has_unsaved_changes(),
          "explicit save did not clear equal-revision recovery state");
}

void project_collection_ui_checks() {
  Directory directory;
  const auto discover = [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
  bridge.collect_project_copy();
  require(!bridge.collection_busy() &&
              bridge.status() ==
                  QStringLiteral("Finish active project work before collecting a copy"),
          "collection without an open project started a worker");
  bridge.create_default_project(QStringLiteral("Starter"));
  require(bridge.project_open(), "collection UI fixture project failed");
  bridge.collect_project_copy();
  require(bridge.collection_busy(), "project collection did not start a worker");
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(&bridge, &quick::ProjectBridge::collectionChanged, &loop, [&] {
    if (!bridge.collection_busy())
      loop.quit();
  });
  timeout.start(5'000);
  loop.exec();
  const auto copy = directory.root() / ".oscillate" / "projects" / "Starter collected";
  require(!bridge.collection_busy() &&
              bridge.collection_status().contains(QStringLiteral("ready")) &&
              bridge.collection_status().contains(QStringLiteral("saved revision")) &&
              fs::is_regular_file(copy / "state" / "CURRENT") &&
              bridge.available_projects().size() == 2,
          "collected copy was not published or listed");
  bridge.collect_project_copy();
  require(bridge.collection_busy(), "second collection did not start a worker");
  timeout.start(5'000);
  loop.exec();
  const auto second_copy = directory.root() / ".oscillate" / "projects" / "Starter collected (2)";
  require(!bridge.collection_busy() && fs::is_regular_file(copy / "state" / "CURRENT") &&
              fs::is_regular_file(second_copy / "state" / "CURRENT") &&
              bridge.available_projects().size() == 3,
          "collected-copy naming replaced an existing project or failed to list the next copy");
  bridge.open_listed(QString::fromStdString(second_copy.string()));
  require(bridge.project_open() && bridge.project_name() == QStringLiteral("Starter collected (2)"),
          "second collected copy did not open from the project library");
}

void project_switch_saves_current_revision() {
  Directory directory;
  const auto discover = [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
  bridge.create_default_project(QStringLiteral("Current"));
  // A default project starts with the A · Reverb and B · Delay returns.
  bridge.add_instrument_track();
  const auto edited_revision = bridge.revision();
  require(bridge.tracks().size() == 3, "fixture edit did not create a track");
  bridge.create_default_project(QStringLiteral("Next"));
  require(bridge.project_name() == QStringLiteral("Next"), "project switch failed");
  bridge.open_listed(
      QString::fromStdString((directory.root() / ".oscillate" / "projects" / "Current").string()));
  require(bridge.project_name() == QStringLiteral("Current") &&
              bridge.revision() == edited_revision && bridge.tracks().size() == 3,
          "project switch did not save and reopen the prior revision");
}

void autosave_worker_preserves_recovery_and_switch_boundary() {
  Directory directory;
  const auto discover = [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, discover);
  bridge.create_default_project(QStringLiteral("Autosave song"));
  const auto baseline = bridge.revision();
  bridge.add_instrument_track();
  const auto edited = bridge.revision();
  require(edited > baseline, "autosave fixture did not become dirty");
  bridge.set_autosave_interval_seconds(30);
  bridge.save_recovery_snapshot();
  bridge.create_default_project(QStringLiteral("Premature switch"));
  require(bridge.project_name() == QStringLiteral("Autosave song") &&
              bridge.status().contains(QStringLiteral("autosave")) &&
              !fs::exists(directory.root() / ".oscillate" / "projects" / "Premature switch"),
          "project switch ran while autosave held the writer");
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(&bridge, &quick::ProjectBridge::autosaveStatusChanged, &loop, [&] {
    if (bridge.autosave_status().contains(QStringLiteral("snapshot saved")) ||
        bridge.autosave_status().contains(QStringLiteral("failed")))
      loop.quit();
  });
  timeout.start(5'000);
  loop.exec();
  require(bridge.autosave_status().contains(QStringLiteral("snapshot saved")),
          "autosave worker did not finish cleanly");
  const auto root = directory.root() / ".oscillate" / "projects" / "Autosave song";
  auto reader = oscillate::persistence::ProjectStore::open_reader(root);
  require(reader.has_value(), "open autosave reader");
  auto current = reader.value().load_current();
  auto autosave = reader.value().load_autosave();
  require(current && autosave && current.value().snapshot.revision == baseline &&
              autosave.value().snapshot.revision == edited,
          "autosave did not preserve explicit save and recovery generation");
  bridge.create_default_project(QStringLiteral("Next song"));
  require(bridge.project_name() == QStringLiteral("Next song") &&
              !bridge.autosave_status().contains(QStringLiteral("snapshot saved")),
          "completed autosave leaked status into the new project");
  bridge.add_instrument_track();
  const auto closing_revision = bridge.revision();
  bridge.save_recovery_snapshot();
  require(!bridge.request_close(), "close did not defer an active autosave");
  bool ready_to_close = false;
  QEventLoop close_loop;
  QTimer close_timeout;
  close_timeout.setSingleShot(true);
  QObject::connect(&close_timeout, &QTimer::timeout, &close_loop, &QEventLoop::quit);
  QObject::connect(&bridge, &quick::ProjectBridge::readyToClose, &close_loop, [&] {
    ready_to_close = true;
    close_loop.quit();
  });
  close_timeout.start(5'000);
  close_loop.exec();
  require(ready_to_close && bridge.request_close(),
          "close did not resume after autosave without losing the latest edit");
  auto closed_reader = oscillate::persistence::ProjectStore::open_reader(
      directory.root() / ".oscillate" / "projects" / "Next song");
  require(closed_reader.has_value(), "open closed project reader");
  auto closed_current = closed_reader.value().load_current();
  require(closed_current && closed_current.value().snapshot.revision == closing_revision,
          "close did not explicitly save the last edit after autosave");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OscillateTest"));
  QCoreApplication::setApplicationName(QStringLiteral("QuickProjectLibraryRecovery"));
  try {
    project_library_recovery_checks();
    close_forget_and_trash_projects();
    explicit_autosave_choice_and_pointer_repair();
    equal_revision_autosave_stays_unsaved();
    project_collection_ui_checks();
    project_switch_saves_current_revision();
    autosave_worker_preserves_recovery_and_switch_boundary();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Quick project library recovery: %s\n", error.what());
    return 1;
  }
  return 0;
}
