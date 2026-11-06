#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

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
        ::_exit(126); // kill(2) failed; parent requires SIGKILL termination.
      }
      return false;
    });
    if (!writer) {
      ::_exit(127);
    }
    (void)writer.value().save(snapshot(3), kind);
    ::_exit(125); // The requested checkpoint was never reached.
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

} // namespace

int main() {
  try {
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
