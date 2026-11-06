#include "oscillate/application/project_session.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <atomic>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace oscillate;

using oscillate::test::require;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-autosave") {}
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return path(); }
};

void background_snapshot_and_explicit_save_race() {
  Directory directory;
  const auto root = directory.root() / "song";
  auto created = application::ProjectSession::create(root);
  REQUIRE_OK(created, "create song");
  auto clean = created.value()->saving().begin_autosave();
  REQUIRE_TRUE(!clean, "clean project scheduled an autosave");
  REQUIRE_EQ(clean.error().code, foundation::ErrorCode::not_found,
             "clean project scheduled an autosave");
  const auto explicit_revision = created.value()->snapshot().revision;
  created.value().reset();

  std::promise<void> entered;
  std::promise<void> release;
  std::atomic<bool> paused{false};
  auto resumed = release.get_future().share();
  auto session = application::ProjectSession::open(root, [&](persistence::SaveStage stage) {
    if (stage == persistence::SaveStage::manifest_written && !paused.exchange(true)) {
      entered.set_value();
      resumed.wait();
    }
    return false;
  });
  REQUIRE_OK(session, "open fault-injected song");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "edit before autosave");
  const auto captured_revision = session.value()->snapshot().revision;
  auto task = session.value()->saving().begin_autosave();
  REQUIRE_OK(task, "capture stable autosave revision");
  REQUIRE_EQ(task.value().revision(), captured_revision, "capture stable autosave revision");
  auto result = std::async(std::launch::async, [task = std::move(task.value())]() mutable {
    return std::move(task).run();
  });
  entered.get_future().wait();
  auto competing = session.value()->saving().save();
  const bool conflict = !competing && competing.error().code == foundation::ErrorCode::conflict;
  const bool edited = session.value()->tracks().add_instrument_track().has_value();
  const auto newer_revision = session.value()->snapshot().revision;
  release.set_value();
  auto outcome = result.get();
  require(conflict, "explicit save did not reject concurrent autosave without blocking");
  require(edited, "model edit was blocked by background autosave");
  REQUIRE_TRUE(outcome, "background autosave did not commit");
  REQUIRE_EQ(outcome.value().status, persistence::SaveStatus::committed,
             "background autosave did not commit");
  auto reader = persistence::ProjectStore::open_reader(root);
  REQUIRE_OK(reader, "open reader");
  auto current = reader.value().load_current();
  auto autosave = reader.value().load_autosave();
  REQUIRE_TRUE(current, "autosave changed CURRENT or captured a later edit");
  REQUIRE_TRUE(autosave, "autosave changed CURRENT or captured a later edit");
  REQUIRE_EQ(current.value().snapshot.revision, explicit_revision,
             "autosave changed CURRENT or captured a later edit");
  REQUIRE_EQ(autosave.value().snapshot.revision, captured_revision,
             "autosave changed CURRENT or captured a later edit");
  REQUIRE_EQ(session.value()->snapshot().revision, newer_revision,
             "autosave changed CURRENT or captured a later edit");
  auto stale_task = session.value()->saving().begin_autosave();
  REQUIRE_TRUE(stale_task, "capture second unsaved revision");
  REQUIRE_EQ(stale_task.value().revision(), newer_revision, "capture second unsaved revision");
  REQUIRE_OK(session.value()->tracks().add_instrument_track(), "edit after stale capture");
  const auto newest_revision = session.value()->snapshot().revision;
  REQUIRE_OK(session.value()->saving().save(), "explicit save after autosave");
  auto stale = std::move(stale_task.value()).run();
  REQUIRE_TRUE(!stale, "stale autosave replaced a newer explicit save");
  REQUIRE_EQ(stale.error().code, foundation::ErrorCode::conflict,
             "stale autosave replaced a newer explicit save");
  current = reader.value().load_current();
  autosave = reader.value().load_autosave();
  REQUIRE_TRUE(current, "explicit save did not persist newer edit");
  REQUIRE_TRUE(autosave, "explicit save did not persist newer edit");
  REQUIRE_EQ(current.value().snapshot.revision, newest_revision,
             "explicit save did not persist newer edit");
  REQUIRE_EQ(autosave.value().snapshot.revision, captured_revision,
             "explicit save did not persist newer edit");
}

void interrupted_autosave_keeps_current() {
  Directory directory;
  const auto root = directory.root() / "song";
  auto created = application::ProjectSession::create(root);
  REQUIRE_OK(created, "create interrupted fixture");
  const auto revision = created.value()->snapshot().revision;
  created.value().reset();
  auto session = application::ProjectSession::open(root, [](persistence::SaveStage stage) {
    return stage == persistence::SaveStage::pointer_written;
  });
  REQUIRE_TRUE(session, "edit interrupted fixture");
  REQUIRE_TRUE(session.value()->tracks().add_instrument_track(), "edit interrupted fixture");
  auto task = session.value()->saving().begin_autosave();
  REQUIRE_OK(task, "begin interrupted autosave");
  auto failed = std::move(task.value()).run();
  require(!failed, "injected interruption did not fail autosave");
  auto reader = persistence::ProjectStore::open_reader(root);
  REQUIRE_OK(reader, "open interrupted reader");
  auto current = reader.value().load_current();
  auto autosave = reader.value().load_autosave();
  REQUIRE_TRUE(current, "failed autosave advanced a pointer");
  REQUIRE_EQ(current.value().snapshot.revision, revision, "failed autosave advanced a pointer");
  REQUIRE_TRUE(!autosave, "failed autosave advanced a pointer");
  REQUIRE_EQ(autosave.error().code, foundation::ErrorCode::not_found,
             "failed autosave advanced a pointer");
}
} // namespace

int main() {
  background_snapshot_and_explicit_save_race();
  interrupted_autosave_keeps_current();
}
