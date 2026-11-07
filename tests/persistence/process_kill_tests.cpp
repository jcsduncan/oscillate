#include "oscillate/persistence/project_creation.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::persistence;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-save-kill") {}
};

// Exit codes a child uses when it did not die where the test asked.
constexpr int fresh_exec_failed = 124;
constexpr int checkpoint_not_reached = 125;
constexpr int kill_failed = 126;
constexpr int child_setup_failed = 127;

project::Snapshot snapshot(std::uint64_t revision) {
  auto tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::linear_bpm},
       {{timeline::ticks_per_quarter * 4}, 90.0, timeline::TempoShape::step}},
      2));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 3));
  project::Clip clip{id<project::ClipId>(3),
                     id<project::SourceId>(4),
                     {{0}, {48000}},
                     project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
                     7};
  return {id<project::ProjectId>(1), revision,         48000,
          std::move(tempo),          std::move(meter), {{id<project::TrackId>(2), {clip}}}};
}

void kill_at_stage(const fs::path& root, SaveKind kind, SaveStage stage) {
  const pid_t child = ::fork();
  REQUIRE_GE(child, 0, "writer process creation failed");
  if (child == 0) {
    auto writer = ProjectStore::open_writer(root, false, [stage](SaveStage reached) {
      if (reached == stage) {
        ::kill(::getpid(), SIGKILL);
        ::_exit(kill_failed); // The parent requires SIGKILL termination.
      }
      return false;
    });
    if (!writer) {
      ::_exit(child_setup_failed);
    }
    (void)writer.value().save(snapshot(3), kind);
    ::_exit(checkpoint_not_reached);
  }

  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  REQUIRE_EQ(waited, child, "writer did not die at the requested save checkpoint");
  REQUIRE_TRUE(WIFSIGNALED(status), "writer did not die at the requested save checkpoint");
  REQUIRE_EQ(WTERMSIG(status), SIGKILL, "writer did not die at the requested save checkpoint");
}

void verify_reopen_after_kill(SaveKind kind, SaveStage stage) {
  Directory directory;
  const auto root = directory.path() / "project";
  {
    auto writer = take(ProjectStore::open_writer(root, true));
    REQUIRE_EQ(take(writer.save(snapshot(1), SaveKind::explicit_save)).status,
               SaveStatus::committed, "baseline explicit save failed");
    REQUIRE_EQ(take(writer.save(snapshot(2), SaveKind::autosave)).status, SaveStatus::committed,
               "baseline autosave failed");
  }

  kill_at_stage(root, kind, stage);

  // Opening a new writer also performs the abandoned-stage cleanup. Assert
  // through the public reopen API, rather than inspecting temporary names.
  auto reopened = take(ProjectStore::open_writer(root, false));
  const bool pointer_advanced =
      stage == SaveStage::pointer_published || stage == SaveStage::state_flush_failed;
  const auto current = take(reopened.load_current());
  const auto autosave = take(reopened.load_autosave());
  const std::uint64_t expected_current =
      kind == SaveKind::explicit_save && pointer_advanced ? 3 : 1;
  const std::uint64_t expected_autosave = kind == SaveKind::autosave && pointer_advanced ? 3 : 2;
  REQUIRE_EQ(current.snapshot.revision, expected_current, "CURRENT changed across killed save");
  REQUIRE_EQ(autosave.snapshot.revision, expected_autosave, "AUTOSAVE changed across killed save");
  REQUIRE_EQ(current.snapshot.id, autosave.snapshot.id, "reopened pointers cross project identity");

  const bool generation_published = stage == SaveStage::generation_published ||
                                    stage == SaveStage::generations_flushed ||
                                    stage == SaveStage::pointer_written || pointer_advanced;
  const auto candidates = take(reopened.recovery_candidates());
  const bool new_generation_recoverable =
      std::ranges::any_of(candidates, [](const LoadedGeneration& candidate) {
        return candidate.snapshot.revision == 3;
      });
  REQUIRE_EQ(new_generation_recoverable, generation_published,
             "recovery did not match the published generation boundary");
  require(std::ranges::any_of(
              candidates,
              [](const LoadedGeneration& candidate) { return candidate.snapshot.revision == 1; }),
          "killed save removed last durable explicit revision");
}

// Creation kill points: before any stage exists, while the stage is empty,
// with a generation but no CURRENT, with a committed CURRENT but not renamed,
// renamed but the parent not flushed, and after creation completed.
struct CreationKill {
  std::optional<SaveStage> stage; // nullopt: killed after publish returned.
  bool published;
  bool committed_stage_kept; // A stage holding CURRENT is never removed.
};

constexpr std::array creation_kills{
    CreationKill{SaveStage::creation_began, false, false},
    CreationKill{SaveStage::root_parent_flushed, false, false},
    CreationKill{SaveStage::generation_published, false, false},
    CreationKill{SaveStage::pointer_published, false, true},
    CreationKill{SaveStage::creation_published, true, false},
    CreationKill{std::nullopt, true, false},
};

constexpr std::string_view verify_creation_mode = "--verify-creation";
constexpr std::string_view clean_mode = "--clean";
constexpr int fresh_check_failed = 1;
constexpr std::uint64_t created_revision = 1;
constexpr std::uint64_t recreated_revision = 2;
// Comfortably older than the test process, whatever the filesystem's clock.
constexpr auto stale_stage_age = std::chrono::hours(1);

void wait_for_sigkill(pid_t child, std::string_view what) {
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  REQUIRE_EQ(waited, child, what);
  REQUIRE_TRUE(WIFSIGNALED(status), what);
  REQUIRE_EQ(WTERMSIG(status), SIGKILL, what);
}

[[noreturn]] void kill_self() {
  ::kill(::getpid(), SIGKILL);
  ::_exit(kill_failed); // The parent requires SIGKILL termination.
}

void kill_creation(const fs::path& root, std::optional<SaveStage> stage) {
  const pid_t child = ::fork();
  REQUIRE_GE(child, 0, "creator process creation failed");
  if (child == 0) {
    auto started = ProjectCreation::begin(root, [stage](SaveStage reached) {
      if (stage && reached == *stage)
        kill_self();
      return false;
    });
    if (!started)
      ::_exit(child_setup_failed);
    auto& [creation, writer] = started.value();
    if (!writer.save(snapshot(created_revision), SaveKind::explicit_save))
      ::_exit(child_setup_failed);
    if (!creation.publish(writer))
      ::_exit(child_setup_failed);
    if (!stage)
      kill_self();
    ::_exit(checkpoint_not_reached);
  }
  wait_for_sigkill(child, "creator did not die at the requested checkpoint");
}

// Staleness is judged against the process start, so the checks that a killed
// creation is recoverable run in a freshly executed copy of this binary.
int run_fresh(const std::vector<std::string>& arguments) {
  const pid_t child = ::fork();
  REQUIRE_GE(child, 0, "fresh process creation failed");
  if (child == 0) {
    std::vector<std::string> owned{"/proc/self/exe"};
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& argument : owned)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv("/proc/self/exe", argv.data());
    ::_exit(fresh_exec_failed);
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  REQUIRE_EQ(waited, child, "fresh process was lost");
  REQUIRE_TRUE(WIFEXITED(status), "fresh process did not exit normally");
  return WEXITSTATUS(status);
}

std::vector<std::string> stage_names(const fs::path& parent) {
  std::vector<std::string> stages;
  for (const auto& entry : fs::directory_iterator(parent))
    if (ProjectCreation::is_stage_name(entry.path().filename().string()))
      stages.push_back(entry.path().filename().string());
  return stages;
}

// What the project library lists: every folder that is not a creation stage.
std::vector<std::string> listed_names(const fs::path& parent) {
  std::vector<std::string> listed;
  for (const auto& entry : fs::directory_iterator(parent))
    if (!ProjectCreation::is_stage_name(entry.path().filename().string()))
      listed.push_back(entry.path().filename().string());
  return listed;
}

std::uint64_t create_project(const fs::path& root, std::uint64_t revision) {
  auto started = take(ProjectCreation::begin(root));
  REQUIRE_EQ(take(started.writer.save(snapshot(revision), SaveKind::explicit_save)).status,
             SaveStatus::committed, "new project save failed");
  take(started.creation.publish(started.writer));
  return revision;
}

// Runs in a fresh process after a creation was killed.
void verify_creation(const fs::path& root, bool published, bool committed_stage_kept) {
  const auto parent = root.parent_path();
  std::uint64_t expected = created_revision;
  if (published) {
    const auto again = ProjectCreation::begin(root);
    REQUIRE_FALSE(again.has_value(), "a committed project was created over");
    REQUIRE_EQ(again.error().code, foundation::ErrorCode::conflict,
               "recreating a committed project was not a conflict");
    (void)take(ProjectCreation::clean_abandoned(parent));
  } else {
    REQUIRE_FALSE(fs::exists(root), "an uncommitted creation is visible at the final name");
    expected = create_project(root, recreated_revision);
  }
  auto reopened = take(ProjectStore::open_writer(root, false));
  REQUIRE_EQ(take(reopened.load_current()).snapshot.revision, expected,
             "project does not open after a killed creation");
  REQUIRE_EQ(listed_names(parent), std::vector<std::string>{root.filename().string()},
             "the library lists something besides the project");
  const auto stages = stage_names(parent);
  REQUIRE_EQ(stages.size(), committed_stage_kept ? 1U : 0U, "abandoned stage cleanup was wrong");
  for (const auto& stage : stages)
    REQUIRE_TRUE(fs::is_regular_file(parent / stage / "state" / "CURRENT"),
                 "an uncommitted stage survived cleanup");
}

void verify_killed_creation(const CreationKill& kill) {
  Directory directory;
  const auto root = directory.path() / "project";
  kill_creation(root, kill.stage);
  REQUIRE_EQ(fs::exists(root), kill.published, "final name does not match the rename boundary");
  // The stage is newer than this process, so it may belong to a live
  // creation: cleanup here must keep it.
  const auto stages_before = stage_names(directory.path());
  REQUIRE_EQ(take(ProjectCreation::clean_abandoned(directory.path())), 0U,
             "cleanup removed a stage newer than the process");
  REQUIRE_EQ(stage_names(directory.path()), stages_before, "cleanup changed a newer stage");
  REQUIRE_EQ(run_fresh({std::string(verify_creation_mode), root.string(),
                        kill.published ? "1" : "0", kill.committed_stage_kept ? "1" : "0"}),
             0, "a fresh process could not recover the killed creation");
}

// A stage whose writer lock is still held belongs to a live creation, even
// when it is older than the process looking at it.
void verify_live_stage_kept() {
  Directory directory;
  const auto root = directory.path() / "project";
  std::array<int, 2> ready{};
  REQUIRE_EQ(::pipe(ready.data()), 0, "pipe creation failed");
  const pid_t child = ::fork();
  REQUIRE_GE(child, 0, "creator process creation failed");
  if (child == 0) {
    ::close(ready[0]);
    auto started = ProjectCreation::begin(root);
    const char signal_byte = started ? 1 : 0;
    if (::write(ready[1], &signal_byte, 1) != 1)
      ::_exit(child_setup_failed);
    for (;;)
      ::pause();
  }
  ::close(ready[1]);
  char signal_byte = 0;
  REQUIRE_EQ(::read(ready[0], &signal_byte, 1), 1, "creator did not report its stage");
  ::close(ready[0]);
  REQUIRE_EQ(signal_byte, 1, "creator could not begin");
  REQUIRE_EQ(stage_names(directory.path()).size(), 1U, "live creation has no stage");
  const auto clean = std::vector<std::string>{std::string(clean_mode), directory.path().string()};
  auto keep = clean;
  keep.emplace_back("0");
  REQUIRE_EQ(run_fresh(keep), 0, "cleanup removed a stage with a held writer lock");
  ::kill(child, SIGKILL);
  wait_for_sigkill(child, "live creator did not die");
  auto remove = clean;
  remove.emplace_back("1");
  REQUIRE_EQ(run_fresh(remove), 0, "cleanup kept an abandoned stage");
  REQUIRE_TRUE(stage_names(directory.path()).empty(), "abandoned stage survived cleanup");
}

// A stage that cannot be removed is skipped: it blocks neither the cleanup
// of other stages nor a new creation beside it.
void verify_stuck_stage_skipped() {
  Directory directory;
  const auto stuck = directory.path() / ".Stuck.creating-00112233445566778899aabbccddeeff";
  const auto loose = directory.path() / ".Loose.creating-ffeeddccbbaa99887766554433221100";
  const auto sealed = stuck / "state" / "generations";
  fs::create_directories(sealed / "inner");
  fs::create_directories(loose / "state" / "generations");
  for (const auto& stage : {stuck, loose})
    fs::last_write_time(stage, fs::file_time_type::clock::now() - stale_stage_age);
  fs::permissions(sealed, fs::perms::owner_read | fs::perms::owner_exec);
  const auto created = ProjectCreation::begin(directory.path() / "project");
  fs::permissions(sealed, fs::perms::owner_all);
  REQUIRE_TRUE(created.has_value(), "an undeletable stage blocked a new creation");
  REQUIRE_TRUE(fs::exists(stuck), "the undeletable stage fixture was removed");
  REQUIRE_FALSE(fs::exists(loose), "an undeletable stage stopped the cleanup of another");
}

// Only folders a creation made are stages: a stage-shaped folder holding
// anything else is kept, and a project cannot take a stage-shaped name.
void verify_foreign_stage_kept() {
  Directory directory;
  const auto foreign = directory.path() / ".Notes.creating-0123456789abcdef0123456789abcdef";
  fs::create_directories(foreign / "state");
  std::ofstream(foreign / "notes.txt") << "not a project\n";
  fs::last_write_time(foreign, fs::file_time_type::clock::now() - stale_stage_age);
  REQUIRE_EQ(take(ProjectCreation::clean_abandoned(directory.path())), 0U,
             "cleanup removed a stage-shaped folder it did not make");
  REQUIRE_TRUE(fs::exists(foreign / "notes.txt"), "a foreign folder lost its contents");
  const auto reserved = ProjectCreation::begin(directory.path() / foreign.filename());
  REQUIRE_FALSE(reserved.has_value(), "a project took a stage-shaped name");
  REQUIRE_EQ(reserved.error().code, foundation::ErrorCode::invalid_argument,
             "a stage-shaped project name was not rejected as invalid");
}

int fresh_main(const std::vector<std::string_view>& arguments) {
  try {
    if (arguments.size() == 4 && arguments[0] == verify_creation_mode) {
      verify_creation(fs::path(arguments[1]), arguments[2] == "1", arguments[3] == "1");
      return 0;
    }
    if (arguments.size() == 3 && arguments[0] == clean_mode) {
      const auto removed = take(ProjectCreation::clean_abandoned(fs::path(arguments[1])));
      REQUIRE_EQ(removed, arguments[2] == "1" ? 1U : 0U, "cleanup count");
      return 0;
    }
    std::cerr << "unknown fresh-process mode\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  return fresh_check_failed;
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1)
    return fresh_main(std::vector<std::string_view>(argv + 1, argv + argc));
  try {
    for (const auto& kill : creation_kills)
      verify_killed_creation(kill);
    verify_live_stage_kept();
    verify_stuck_stage_skipped();
    verify_foreign_stage_kept();
    constexpr std::array stages{SaveStage::manifest_written,     SaveStage::generation_flushed,
                                SaveStage::generation_published, SaveStage::generations_flushed,
                                SaveStage::pointer_written,      SaveStage::pointer_published,
                                SaveStage::state_flush_failed};
    for (const auto kind : {SaveKind::explicit_save, SaveKind::autosave}) {
      for (const auto stage : stages) {
        verify_reopen_after_kill(kind, stage);
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
