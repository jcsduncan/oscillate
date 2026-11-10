#pragma once

#include "oscillate/application/session/save_service.hpp"
#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

// An already verified effect identity. No component state or parameter values
// are invented for a newly inserted instance.
struct Vst3InsertIdentity {
  std::string class_id;
  std::string vendor;
  std::string name;
  std::string version;
  std::string canonical_bundle_path;
  std::string binary_sha256;
};

// How a VST3 editor or instrument state edit is saved: before returning,
// or by a later background save.
enum class EditorSavePolicy : std::uint8_t { immediate, background };

// One built-in effect edit for apply_effect_edits: the named values on an
// existing slot (the rest kept), or a new instance of `engine` at `index`
// (the chain's end when none) at the engine defaults with the named values.
struct EffectEdit {
  project::SignalPathId path;
  std::optional<project::InsertSlotId> slot;
  std::optional<std::size_t> index;
  std::string engine;
  std::vector<std::pair<std::string, double>> values;
};

class EffectService : public SessionService {
public:
  EffectService(SessionState& state, SaveService& saving) noexcept
      : SessionService(state), saving_(saving) {}

  // Low-level, control-side project command. The caller must verify the VST3
  // scan receipt and binary identity before supplying this initial instance.
  // Commits one undo step and saves it before returning success.
  [[nodiscard]] foundation::Result<project::PluginInstanceId>
  insert_vst3_plugin(project::TrackId track, std::size_t index, Vst3InsertIdentity identity);
  // Rechecks the original revision/slot after any caller plugin/Qt work, then
  // commits captured state as one undoable project edit.
  [[nodiscard]] foundation::Result<void>
  set_vst3_editor_state(std::uint64_t opened_revision, project::TrackId track,
                        project::InsertSlotId slot, const project::Vst3PluginInstance& original,
                        std::vector<std::uint8_t> state,
                        std::vector<project::Vst3ParameterReference> parameters,
                        EditorSavePolicy save_policy = EditorSavePolicy::immediate);
  // The controller chunk is captured with the component chunk from one
  // suspended instance. Nullopt and an empty chunk have distinct meanings.
  [[nodiscard]] foundation::Result<void>
  set_vst3_editor_state(std::uint64_t opened_revision, project::TrackId track,
                        project::InsertSlotId slot, const project::Vst3PluginInstance& original,
                        std::vector<std::uint8_t> component_state,
                        std::optional<std::vector<std::uint8_t>> controller_state,
                        std::vector<project::Vst3ParameterReference> parameters,
                        EditorSavePolicy save_policy = EditorSavePolicy::immediate);
  // A fresh scanner receipt may relocate only the same class and exact binary.
  // The original slot/source, component state and parameter references survive
  // one undoable edit; a stale revision or incompatible receipt changes nothing.
  [[nodiscard]] foundation::Result<void>
  relink_vst3_plugin(std::uint64_t opened_revision, project::TrackId track,
                     std::optional<project::InsertSlotId> slot,
                     const project::Vst3PluginInstance& original, Vst3InsertIdentity verified);
  // Saved insert-chain edits are one undo step each. Callers must separately
  // qualify live playback/export before describing a revision as active.
  [[nodiscard]] foundation::Result<project::InsertSlotId>
  add_native_effect(project::SignalPathId path, std::size_t index,
                    project::NativeEffectPatch processor);
  [[nodiscard]] foundation::Result<void>
  set_native_effect(project::SignalPathId path, project::InsertSlotId slot,
                    project::NativeEffectPatch processor,
                    std::optional<std::uint64_t> gesture = {});
  // A built-in effect (ADR-0040) at a factory preset or the engine defaults.
  [[nodiscard]] foundation::Result<project::InsertSlotId>
  add_effect_engine(project::SignalPathId path, std::size_t index, std::string_view engine,
                    std::optional<std::size_t> preset = std::nullopt);
  // Dense values in the engine's table order, for device panels.
  [[nodiscard]] foundation::Result<std::vector<double>>
  native_effect_values(project::SignalPathId path, project::InsertSlotId slot) const;
  // The engine validates every value; the model stores only non-defaults and
  // coalesces steps with the same gesture into one undo step.
  [[nodiscard]] foundation::Result<void>
  set_native_effect_values(project::SignalPathId path, project::InsertSlotId slot,
                           std::span<const double> values,
                           std::optional<std::uint64_t> gesture = {});
  // A factory preset's values and document on an existing built-in effect.
  [[nodiscard]] foundation::Result<void>
  set_native_effect_preset(project::SignalPathId path, project::InsertSlotId slot,
                           std::size_t preset, std::optional<std::uint64_t> gesture = {});
  // A built-in effect's structured state (ADR-0058): its drawn curves.
  [[nodiscard]] foundation::Result<project::NativeSynthDocument>
  native_effect_document(project::SignalPathId path, project::InsertSlotId slot) const;
  // Replaces the whole document, keeping the values. The model checks
  // structure, the engine its meaning; steps with the same gesture coalesce
  // into one undo step.
  [[nodiscard]] foundation::Result<void>
  set_native_effect_document(project::SignalPathId path, project::InsertSlotId slot,
                             project::NativeSynthDocument document,
                             std::optional<std::uint64_t> gesture = {});
  // Values and document together in one undo step (a user preset); the
  // document reaches the effect's linked group.
  [[nodiscard]] foundation::Result<void>
  set_native_effect_state(project::SignalPathId path, project::InsertSlotId slot,
                          std::span<const double> values, project::NativeSynthDocument document,
                          std::optional<std::uint64_t> gesture = {});
  // Linked built-in effects share their document (ADR-0058): a document or
  // preset edit on one reaches the whole group in the same undo step; values
  // stay per instance. Links these slots (two or more, of one engine) into a
  // new group, the first one's document copied to all, in one undo step.
  [[nodiscard]] foundation::Result<void>
  link_native_effects(std::span<const project::InsertSlotId> effect_slots);
  // Leaves the group, keeping the current document; the last one left in a
  // group is unlinked with it.
  [[nodiscard]] foundation::Result<void> unlink_native_effect(project::SignalPathId path,
                                                              project::InsertSlotId slot);
  // How many effects share this one's document: 1 when unlinked, 0 when the
  // slot is not a built-in effect.
  [[nodiscard]] std::size_t native_effect_link_count(project::InsertSlotId slot) const;
  // Duck tracks to a key in one undo step: on each target (not the key), its
  // first Pump, or a new one at the end of its chain, is keyed from `source`
  // alone and linked with the rest, sharing the first reused Pump's shape.
  // Returns how many tracks now duck.
  [[nodiscard]] foundation::Result<std::size_t>
  duck_tracks_to(project::TrackId source, std::span<const project::TrackId> targets);
  [[nodiscard]] foundation::Result<void>
  move_effect_insert(project::SignalPathId path, project::InsertSlotId slot, std::size_t index);
  [[nodiscard]] foundation::Result<void>
  set_effect_insert_enabled(project::SignalPathId path, project::InsertSlotId slot, bool enabled);
  [[nodiscard]] foundation::Result<void> remove_effect_insert(project::SignalPathId path,
                                                              project::InsertSlotId slot);
  // Several built-in effect edits as one undo step, validated whole before
  // anything changes. Returns the slots edited or added, in order.
  [[nodiscard]] foundation::Result<std::vector<project::InsertSlotId>>
  apply_effect_edits(std::span<const EffectEdit> edits);
  // The master assistant's proposal (ADR-0124) as one undo step: built-in
  // effects added, set or switched on on the master, and the rebalance.
  // Any other operation is refused before anything changes.
  [[nodiscard]] foundation::Result<void>
  apply_master_operations(std::vector<project::Operation> operations);

private:
  SaveService& saving_;
};

} // namespace oscillate::application
