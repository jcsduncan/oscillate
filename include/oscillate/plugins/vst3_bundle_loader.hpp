#pragma once

#include "oscillate/plugins/vst3_effect_host.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace oscillate::plugins {

struct VerifiedVst3Effect {
  Vst3Class identity;
  Vst3BinaryIdentity binary;
};

// What the file system reports for a binary. Any difference in a later stat
// means the file was rewritten or replaced.
struct Vst3BinaryStamp {
  std::uint64_t device{0};
  std::uint64_t inode{0};
  std::uint64_t byte_size{0};
  std::int64_t modified_seconds{0};
  std::int64_t modified_nanoseconds{0};
  // Status change time: any write or metadata change sets it, and it cannot
  // be set back.
  std::int64_t changed_seconds{0};
  std::int64_t changed_nanoseconds{0};
  friend bool operator==(const Vst3BinaryStamp&, const Vst3BinaryStamp&) = default;
};

// A binary's digest with the stamp it had while it was read.
struct Vst3HashedBinary {
  Vst3BinaryIdentity identity;
  Vst3BinaryStamp stamp;
};

// Reads and hashes the bundle's whole binary: worker side only, never the GUI
// thread or the audio callback. Nothing when the bundle is invalid or the
// file changed while it was read.
[[nodiscard]] std::optional<Vst3HashedBinary> hash_vst3_binary(const std::filesystem::path& bundle);

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

// Loads from a binary hashed earlier (hash_vst3_binary, on a worker) without
// reading it again: the file's stamp must match the hashed one before and
// after the module loads. For callers that must load on the GUI thread.
[[nodiscard]] std::unique_ptr<LoadedVst3Effect>
load_hashed_vst3_effect(const Vst3Class& identity, const Vst3HashedBinary& hashed,
                        double sample_rate, std::uint32_t max_block_frames, std::string& error,
                        Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                        std::uint32_t max_parameter_events = default_vst3_parameter_events,
                        bool routed_sidechain = false);

[[nodiscard]] std::unique_ptr<LoadedVst3Effect>
load_hashed_vst3_instrument(const Vst3Class& identity, const Vst3HashedBinary& hashed,
                            double sample_rate, std::uint32_t max_block_frames, std::string& error,
                            Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                            std::uint32_t max_parameter_events = default_vst3_parameter_events);

} // namespace oscillate::plugins
