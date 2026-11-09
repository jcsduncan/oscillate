#pragma once

#include <cstdint>

namespace oscillate::plugins {

// Why a VST3 instance could not be bound, or ready.
enum class Vst3BindingStatus : std::uint8_t {
  ready,
  scan_unavailable,
  identity_mismatch,
  load_failed,
  state_rejected,
  parameter_values_unapplied,
  activation_failed,
  timing_unsupported,
  automation_unavailable,
};

} // namespace oscillate::plugins
