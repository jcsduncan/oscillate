#pragma once

#include "oscillate/plugins/vst3_effect_host.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace oscillate::plugins {

struct VerifiedVst3Effect {
  Vst3Class identity;
  Vst3BinaryIdentity binary;
};

// Owns the loaded module for longer than the processor instance. Creation,
// destruction and host control methods belong off the audio callback.
class LoadedVst3Effect final {
public:
  ~LoadedVst3Effect();
  LoadedVst3Effect(const LoadedVst3Effect&) = delete;
  LoadedVst3Effect& operator=(const LoadedVst3Effect&) = delete;
  LoadedVst3Effect(LoadedVst3Effect&&) = delete;
  LoadedVst3Effect& operator=(LoadedVst3Effect&&) = delete;

  [[nodiscard]] Vst3EffectHost& host() noexcept;
  [[nodiscard]] const Vst3EffectHost& host() const noexcept;

  // Internal lifetime constructor; use load_verified_vst3_effect.
  LoadedVst3Effect(void* module, bool (*module_exit)(), std::unique_ptr<Vst3EffectHost> host);

private:
  void* module_{nullptr};
  bool (*module_exit_)() = nullptr;
  std::unique_ptr<Vst3EffectHost> host_;
};

// max_parameter_events sizes the host's preallocated parameter queues; values
// outside 1..max_vst3_parameter_events fail to load.
[[nodiscard]] std::unique_ptr<LoadedVst3Effect>
load_verified_vst3_effect(const VerifiedVst3Effect& verified, double sample_rate,
                          std::uint32_t max_block_frames, std::string& error,
                          Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                          std::uint32_t max_parameter_events = default_vst3_parameter_events,
                          bool routed_sidechain = false);

// Instrument mode accepts silent, unconnected stereo auxiliary audio inputs,
// one primary stereo output, up to seven inactive secondary stereo outputs,
// and exactly one main event input bus. Only the primary output is exposed.
// The returned lifetime owner also owns the module.
[[nodiscard]] std::unique_ptr<LoadedVst3Effect>
load_verified_vst3_instrument(const VerifiedVst3Effect& verified, double sample_rate,
                              std::uint32_t max_block_frames, std::string& error,
                              Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                              std::uint32_t max_parameter_events = default_vst3_parameter_events);

} // namespace oscillate::plugins
