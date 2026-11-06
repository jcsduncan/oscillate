#pragma once

#include "oscillate/application/control_transport_gate.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace oscillate::application {

// A managed-media repair in flight (media_repair_service.cpp). While one
// runs, every service refuses project edits that would publish or save.
struct ManagedRepairState;

// What one ProjectSession owns and its services share: the project model,
// its durable store and transport gate, and the save bookkeeping. Control
// thread only.
struct SessionState {
  SessionState(std::filesystem::path project_root, persistence::ProjectStore project_store)
      : root(std::move(project_root)), store(std::move(project_store)) {}

  std::filesystem::path root;
  std::shared_ptr<ControlTransportGate> gate = std::make_shared<ControlTransportGate>();
  persistence::ProjectStore store;
  std::unique_ptr<project::ProjectModel> model;
  std::uint64_t saved_revision = 0;
  bool recovered_autosave = false;
  std::vector<persistence::SourceReceipt> pending_receipts;
  std::vector<project::SourceRecord> saved_external_sources;
  std::optional<persistence::VerifiedExternalSource> pending_external_proof;
  std::optional<foundation::Error> save_maintenance_warning;
  std::shared_ptr<ManagedRepairState> repair_state;
};

} // namespace oscillate::application
