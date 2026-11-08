#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "oscillate/persistence/store.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace oscillate::persistence {

struct StartedCreation;

// Crash-safe creation of a new project folder (ADR-0004). The project is built
// in a hidden sibling stage named `.<name>.creating-<random>`, and only a stage
// whose first explicit save is durable is renamed to its final name. A process
// killed at any point leaves either no project at the final name or a complete
// one; an abandoned stage is removed by the next creation or library scan.
// Control/worker threads only: every call performs blocking I/O.
class ProjectCreation {
public:
  ProjectCreation(ProjectCreation&& other) noexcept;
  ProjectCreation& operator=(ProjectCreation&&) = delete;
  ProjectCreation(const ProjectCreation&) = delete;
  ProjectCreation& operator=(const ProjectCreation&) = delete;
  // Removes the stage unless publish() renamed it into place.
  ~ProjectCreation();

  // Refuses a `root` that exists and is not an empty directory, removes what
  // it can of the abandoned stages beside it, then opens the sole writer on a
  // fresh stage.
  // The fault hook also reaches the writer's saves.
  [[nodiscard]] static foundation::Result<StartedCreation> begin(const std::filesystem::path& root,
                                                                 FaultHook fault = {});

  // Requires a committed CURRENT in the stage, flushes it, renames it to the
  // final name without replacing a folder that appeared meanwhile, then
  // flushes the parent. The writer's descriptors stay valid across the rename.
  [[nodiscard]] foundation::Result<void> publish();

  // Removes stages in `parent` that a process older than this one abandoned:
  // only directories matching the stage pattern, holding only a new project's
  // layout and no CURRENT, and not locked by a live creation. Best effort and bounded: a stage that
  // cannot be removed is skipped, and a scan stops after a fixed number of entries. Returns how
  // many were removed; fails only if `parent` cannot be scanned.
  [[nodiscard]] static foundation::Result<std::size_t>
  clean_abandoned(const std::filesystem::path& parent);

  // True for a folder name this class uses as a creation stage.
  [[nodiscard]] static bool is_stage_name(std::string_view name) noexcept;

private:
  ProjectCreation(std::filesystem::path parent, foundation::UniqueFd parent_fd,
                  std::string final_name, foundation::PublishMode mode, FaultHook fault);

  std::filesystem::path parent_;
  foundation::UniqueFd parent_fd_;
  std::string stage_name_;
  // Held with flock from the moment the stage exists until this is destroyed.
  foundation::UniqueFd stage_fd_;
  std::string final_name_;
  foundation::PublishMode mode_;
  FaultHook fault_;
  bool published_ = false;
};

struct StartedCreation {
  ProjectCreation creation;
  ProjectStore writer;
};

} // namespace oscillate::persistence
