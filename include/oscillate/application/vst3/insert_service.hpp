#pragma once

#include "oscillate/application/project_session.hpp"
#include "oscillate/plugins/vst3_catalogue.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace oscillate::plugins {

// Produced only by a fresh worker-side verification. It carries the exact
// class and binary receipt used by the control-side project command.
class VerifiedVst3EffectSelection final {
public:
  VerifiedVst3EffectSelection(const VerifiedVst3EffectSelection&) = default;
  VerifiedVst3EffectSelection(VerifiedVst3EffectSelection&&) noexcept = default;
  VerifiedVst3EffectSelection& operator=(const VerifiedVst3EffectSelection&) = default;
  VerifiedVst3EffectSelection& operator=(VerifiedVst3EffectSelection&&) noexcept = default;
  [[nodiscard]] const Vst3Class& effect_class() const noexcept { return effect_class_; }
  [[nodiscard]] const Vst3BinaryIdentity& binary() const noexcept { return binary_; }

private:
  friend foundation::Result<VerifiedVst3EffectSelection>
  verify_vst3_effect_selection(const Vst3CatalogueEntry&, std::string_view, Vst3Scanner&);
  VerifiedVst3EffectSelection(Vst3Class effect_class, Vst3BinaryIdentity binary)
      : effect_class_(std::move(effect_class)), binary_(std::move(binary)) {}
  Vst3Class effect_class_;
  Vst3BinaryIdentity binary_;
};

class VerifiedVst3InstrumentSelection final {
public:
  VerifiedVst3InstrumentSelection(const VerifiedVst3InstrumentSelection&) = default;
  VerifiedVst3InstrumentSelection(VerifiedVst3InstrumentSelection&&) noexcept = default;
  VerifiedVst3InstrumentSelection& operator=(const VerifiedVst3InstrumentSelection&) = default;
  VerifiedVst3InstrumentSelection& operator=(VerifiedVst3InstrumentSelection&&) noexcept = default;
  [[nodiscard]] const Vst3Class& instrument_class() const noexcept { return instrument_class_; }
  [[nodiscard]] const Vst3BinaryIdentity& binary() const noexcept { return binary_; }

private:
  friend foundation::Result<VerifiedVst3InstrumentSelection>
  verify_vst3_instrument_selection(const Vst3CatalogueEntry&, std::string_view, Vst3Scanner&);
  VerifiedVst3InstrumentSelection(Vst3Class instrument_class, Vst3BinaryIdentity binary)
      : instrument_class_(std::move(instrument_class)), binary_(std::move(binary)) {}
  Vst3Class instrument_class_;
  Vst3BinaryIdentity binary_;
};

// Worker-side only: fresh helper scan, binary identity and class comparison.
[[nodiscard]] foundation::Result<VerifiedVst3EffectSelection>
verify_vst3_effect_selection(const Vst3CatalogueEntry& selected, std::string_view class_id,
                             Vst3Scanner& scanner);

[[nodiscard]] foundation::Result<VerifiedVst3InstrumentSelection>
verify_vst3_instrument_selection(const Vst3CatalogueEntry& selected, std::string_view class_id,
                                 Vst3Scanner& scanner);

// UI/control-side only: no scanner or plugin load. ProjectSession validates
// the target/position, applies one undoable command and saves it.
[[nodiscard]] foundation::Result<project::PluginInstanceId>
insert_verified_vst3_effect(application::ProjectSession& session,
                            const VerifiedVst3EffectSelection& verified, project::TrackId track,
                            std::size_t index);

[[nodiscard]] foundation::Result<project::PluginInstanceId>
insert_verified_vst3_instrument(application::ProjectSession& session,
                                const VerifiedVst3InstrumentSelection& verified,
                                project::TrackId track);

[[nodiscard]] foundation::Result<project::TrackId>
create_verified_vst3_instrument_track(application::ProjectSession& session,
                                      const VerifiedVst3InstrumentSelection& verified);

// Control thread only. A selected catalogue receipt is rescanned before the
// project command so missing or changed binaries cannot enter the model.
// This records a default-state effect; graph activation is a separate step.
[[nodiscard]] foundation::Result<project::PluginInstanceId>
insert_verified_vst3_effect(application::ProjectSession& session,
                            const Vst3CatalogueEntry& selected, std::string_view class_id,
                            project::TrackId track, std::size_t index, Vst3Scanner& scanner);

} // namespace oscillate::plugins
