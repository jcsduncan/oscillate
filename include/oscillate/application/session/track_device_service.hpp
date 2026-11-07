#pragma once

#include "oscillate/application/session/session_service.hpp"
#include "oscillate/persistence/rack_file.hpp"

namespace oscillate::application {

class TrackDeviceService : public SessionService {
public:
  explicit TrackDeviceService(SessionState& state) noexcept : SessionService(state) {}

  // Racks (ADR-0059): the track's built-in
  // devices and macros as a rack, with destinations renamed to the rack's own
  // slot ids. VST3 and convolution devices, and a VST3 instrument, are left
  // out and counted in `skipped`.
  struct CapturedRack {
    persistence::TrackRack rack;
    std::size_t skipped = 0;
  };

  // Note effects (ADR-0059): MIDI effects before an instrument track's source, in order.
  // Each edit is one undo step; parameter steps with the same gesture
  // coalesce. Parameters are sparse plain values by the engine's stable ids;
  // the model checks structure only.
  [[nodiscard]] foundation::Result<project::InsertSlotId>
  add_note_effect(project::TrackId track, std::size_t index, std::string_view engine,
                  std::vector<project::NativeInstrumentParameter> parameters = {},
                  std::uint32_t engine_version = 1);
  [[nodiscard]] foundation::Result<void> remove_note_effect(project::TrackId track,
                                                            project::InsertSlotId slot);
  [[nodiscard]] foundation::Result<void>
  move_note_effect(project::TrackId track, project::InsertSlotId slot, std::size_t index);
  [[nodiscard]] foundation::Result<void>
  set_note_effect_enabled(project::TrackId track, project::InsertSlotId slot, bool enabled);
  [[nodiscard]] foundation::Result<void>
  set_note_effect_parameters(project::TrackId track, project::InsertSlotId slot,
                             std::vector<project::NativeInstrumentParameter> parameters,
                             std::optional<std::uint64_t> gesture = {});
  // The same, also switching the effect on: one undo step, and nothing
  // changes when either half is refused.
  [[nodiscard]] foundation::Result<void>
  enable_note_effect_with_parameters(project::TrackId track, project::InsertSlotId slot,
                                     std::vector<project::NativeInstrumentParameter> parameters,
                                     std::optional<std::uint64_t> gesture = {});
  // ADR-0083: Comp pattern `index`
  // (comp::patterns()) on Comp `slot`, keeping its other settings: one undo
  // step.
  [[nodiscard]] foundation::Result<void>
  apply_comp_pattern(project::TrackId track, project::InsertSlotId slot, std::size_t index);
  // Macros: up to eight knobs per track (indices 0..7; touching a higher
  // index than the track has adds macros up to it). Setting a value writes
  // every mapped parameter in the same command. Adding a mapping copies the
  // parameter's automation domain and sets the parameter from the macro at
  // once; a parameter with its own automation lane cannot be mapped.
  // Removing a mapping leaves the parameter where it is.
  [[nodiscard]] foundation::Result<void> set_macro_value(project::TrackId track,
                                                         std::uint32_t index, double value,
                                                         std::optional<std::uint64_t> gesture = {});
  [[nodiscard]] foundation::Result<void> set_macro_name(project::TrackId track, std::uint32_t index,
                                                        std::string name);
  [[nodiscard]] foundation::Result<void> add_macro_mapping(project::TrackId track,
                                                           std::uint32_t index,
                                                           project::MacroDestination destination,
                                                           double minimum, double maximum);
  [[nodiscard]] foundation::Result<void>
  remove_macro_mapping(project::TrackId track, std::uint32_t index,
                       const project::MacroDestination& destination);
  // A mapping's range, in the parameter's plain units (minimum may exceed
  // maximum). The parameter moves to the new range at once; steps with the
  // same gesture coalesce into one undo step.
  [[nodiscard]] foundation::Result<void>
  set_macro_mapping_range(project::TrackId track, std::uint32_t index,
                          const project::MacroDestination& destination, double minimum,
                          double maximum, std::optional<std::uint64_t> gesture = {});
  [[nodiscard]] foundation::Result<CapturedRack> capture_track_rack(project::TrackId track) const;
  // Loads a rack onto an instrument (or, without instrument or note effects,
  // any) track in one undo step: the track's note effects, built-in effects
  // and macros are replaced, the rack's instrument replaces a built-in or
  // empty one, and VST3 and convolution effects stay after the rack's. New
  // slot ids are allocated and the macros retargeted to them.
  [[nodiscard]] foundation::Result<void> load_track_rack(project::TrackId track,
                                                         const persistence::TrackRack& rack);

private:
  [[nodiscard]] foundation::Result<void>
  apply_note_effect_settings(project::TrackId track, project::InsertSlotId slot,
                             std::vector<project::NativeInstrumentParameter> parameters,
                             bool enable, std::optional<std::uint64_t> gesture);
};

} // namespace oscillate::application
