#include "native_synth_automation_controller.hpp"

#include <algorithm>
#include <variant>

namespace oscillate::quick {
namespace {
QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace

NativeSynthAutomationController::NativeSynthAutomationController(
    const application::ProjectSession& session, project::TrackId track,
    const native_dsp::synth::SynthEngine& engine, QObject* parent)
    : QObject(parent), session_(session), track_(track), descriptor_(*engine.descriptor) {}

NativeSynthAutomationController::NativeSynthAutomationController(
    const application::ProjectSession& session, project::TrackId track, project::InsertSlotId slot,
    const native_dsp::effects::EffectEngine& effect, QObject* parent)
    : QObject(parent), session_(session), track_(track), slot_(slot),
      descriptor_(*effect.descriptor) {}

QString NativeSynthAutomationController::track_id() const {
  return QString::fromStdString(track_.to_string());
}

// The arrangement's lane keys (ArrangementAutomationController).
QString NativeSynthAutomationController::key_of(std::string_view parameter) const {
  return slot_ ? QStringLiteral("fx:") + QString::fromStdString(slot_->to_string()) +
                     QStringLiteral(":") + text(parameter)
               : QStringLiteral("synth:") + text(parameter);
}

QString NativeSynthAutomationController::lane_key() const {
  const auto& tracks = session_.snapshot().tracks;
  const auto track = std::ranges::find(tracks, track_, &project::Track::id);
  if (track != tracks.end()) {
    for (const auto& lane : track->automation) {
      if (slot_) {
        const auto* effect = std::get_if<project::NativeEffectParameterTarget>(&lane.target);
        if (effect && effect->slot == *slot_)
          return key_of(effect->parameter_id);
      } else if (const auto* native = std::get_if<project::NativeParameterTarget>(&lane.target);
                 native && native->track == track_) {
        return key_of(native->parameter_id);
      }
    }
  }
  const auto first = std::ranges::find_if(
      descriptor_.parameters, [](const auto& parameter) { return parameter.automatable; });
  return first == descriptor_.parameters.end() ? QString() : key_of(first->id);
}

} // namespace oscillate::quick
