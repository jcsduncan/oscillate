#pragma once

#include "oscillate/application/session/effect_service.hpp"
#include "oscillate/application/session/save_service.hpp"
#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

// Kick & Bass Lock's one click (ADR-0072), as `track`'s menu shows it:
// offered on a Nova track while the project has an Anvil track; locked when
// that track's Lock and every unmuted Anvil track's Lock are on.
struct KickBassLockState {
  bool offered = false;
  bool locked = false;
};
[[nodiscard]] KickBassLockState kick_bass_lock_state(const project::Snapshot& snapshot,
                                                     project::TrackId track);

class InstrumentService : public SessionService {
public:
  InstrumentService(SessionState& state, SaveService& saving) noexcept
      : SessionService(state), saving_(saving) {}

  [[nodiscard]] foundation::Result<void>
  set_vst3_instrument_state(std::uint64_t opened_revision, project::TrackId track,
                            const project::Vst3PluginInstance& original,
                            std::vector<std::uint8_t> state,
                            std::vector<project::Vst3ParameterReference> parameters,
                            EditorSavePolicy save_policy = EditorSavePolicy::immediate);
  [[nodiscard]] foundation::Result<void>
  set_vst3_instrument_state(std::uint64_t opened_revision, project::TrackId track,
                            const project::Vst3PluginInstance& original,
                            std::vector<std::uint8_t> component_state,
                            std::optional<std::vector<std::uint8_t>> controller_state,
                            std::vector<project::Vst3ParameterReference> parameters,
                            EditorSavePolicy save_policy = EditorSavePolicy::immediate);
  // Creates the target track and verified instrument insert in one project command.
  [[nodiscard]] foundation::Result<project::TrackId>
  create_vst3_instrument_track(Vst3InsertIdentity identity);
  // Built-in synths (native_dsp/synth/engines.hpp). Adds an instrument track
  // playing the default engine's default patch, as one undo step.
  [[nodiscard]] foundation::Result<project::TrackId>
  add_synth_track(std::optional<std::size_t> insert_index = std::nullopt);
  // An instrument track playing `engine` at `preset` (its default patch when
  // none), as one undo step.
  [[nodiscard]] foundation::Result<project::TrackId>
  add_native_instrument_track(std::string_view engine, std::optional<std::size_t> preset,
                              std::optional<std::size_t> insert_index = std::nullopt);
  // Replaces an empty or built-in source with `engine`, at a factory
  // preset or the engine defaults. A VST3 instrument must be removed first.
  [[nodiscard]] foundation::Result<void>
  select_native_instrument(project::TrackId track, std::string_view engine,
                           std::optional<std::size_t> preset = std::nullopt);
  // Dense values in the engine's table order, for device panels.
  [[nodiscard]] foundation::Result<std::vector<double>>
  native_instrument_values(project::TrackId track) const;
  // The engine validates every value; the model stores only non-defaults and
  // coalesces steps with the same gesture into one undo step.
  [[nodiscard]] foundation::Result<void>
  set_native_instrument_values(project::TrackId track, std::span<const double> values,
                               std::optional<std::uint64_t> gesture = {});
  // The structured state of a built-in synth (ADR-0057): routes, curves,
  // module chains, sequences, media slots and labels.
  [[nodiscard]] foundation::Result<project::NativeSynthDocument>
  native_synth_document(project::TrackId track) const;
  // Replaces the whole document, keeping the values. The model checks
  // structure and media identity, the engine its meaning; steps with the
  // same gesture coalesce into one undo step.
  [[nodiscard]] foundation::Result<void>
  set_native_synth_document(project::TrackId track, project::NativeSynthDocument document,
                            std::optional<std::uint64_t> gesture = {});
  // The patch's values and document together, as one undo step (a re-chop:
  // Crate's regions and their per-chop parameters, ADR-0063). `values` are
  // the engine's sparse parameters; ids missing from them take the engine
  // defaults. The engine validates both before anything changes; steps with
  // the same gesture coalesce.
  [[nodiscard]] foundation::Result<void> set_native_instrument_state(
      project::TrackId track, std::vector<project::NativeInstrumentParameter> values,
      project::NativeSynthDocument document, std::optional<std::uint64_t> gesture = {});
  // Kick & Bass Lock (ADR-0072) in one click: `bass`'s Nova Lock and the
  // Lock of every unmuted Anvil track switch to `on` together, as one undo
  // step. An Anvil track turned on takes Keep tone (the pair forms, its
  // drawn tail is kept); one already on Bass note stays there. Fails without
  // changing anything when `bass` is not a Nova track; succeeds unchanged
  // when every switch already reads `on`.
  [[nodiscard]] foundation::Result<void> lock_kick_and_bass(project::TrackId bass, bool on);

private:
  SaveService& saving_;
};

} // namespace oscillate::application
