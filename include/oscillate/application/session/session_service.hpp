#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::application {

// The project, store and transport gate one ProjectSession owns; private to
// the session's services (src/application/session/session_state.hpp).
struct SessionState;

// Base of the services a ProjectSession composes. Each service edits the one
// shared project model on the control thread; it owns no project state.
class SessionService {
public:
  SessionService(const SessionService&) = delete;
  SessionService& operator=(const SessionService&) = delete;
  SessionService(SessionService&&) = delete;
  SessionService& operator=(SessionService&&) = delete;

protected:
  explicit SessionService(SessionState& state) noexcept : state_(state) {}
  ~SessionService() = default;

  [[nodiscard]] project::ProjectModel& model() const noexcept;
  [[nodiscard]] const project::Snapshot& snapshot() const noexcept;

  // Session-numbered identities: one past the greatest in use, never below
  // the current revision, so undo or removal never recycles an identity.
  [[nodiscard]] std::uint64_t next_track_number() const;
  [[nodiscard]] std::uint64_t next_clip_number() const;
  [[nodiscard]] std::uint64_t next_note_number() const;
  [[nodiscard]] std::uint64_t next_source_number() const;
  [[nodiscard]] std::uint64_t next_send_number() const;
  [[nodiscard]] std::uint64_t next_plugin_number() const;
  [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> next_automation_numbers() const;

  SessionState& state_;
};

} // namespace oscillate::application
