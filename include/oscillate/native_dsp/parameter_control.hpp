#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// The control half shared by every table-driven native processor: synths
// (ADR-0027) and effects (ADR-0040). One serialized control producer offers
// whole value sets; the one callback consumer adopts the newest at a block
// start, glides panel jumps over each parameter's glide time on the control
// grid, and applies per-frame automation lanes on top.
namespace oscillate::native_dsp {

// Control values are evaluated on a grid aligned to absolute project frames,
// so any block split renders bit-identical audio.
inline constexpr std::uint32_t control_interval_frames = 32;

// One automated parameter: absolute plain values, one per output frame.
struct ParameterLane {
  std::uint32_t parameter = 0;
  std::span<const double> values;
};

class ParameterControl {
public:
  ParameterControl(const EngineDescriptor& descriptor, std::span<const double> values,
                   double sample_rate);

  // Whole value sets in table order: finite, in range, discrete ones whole.
  [[nodiscard]] static bool valid_values(const EngineDescriptor& descriptor,
                                         std::span<const double> values) noexcept;
  // Control only. The latest accepted offer is adopted at the next block.
  // Fails when the mailbox is full, the revision does not increase or a
  // value is invalid.
  [[nodiscard]] bool offer(std::span<const double> values, std::uint64_t revision) noexcept;
  // Control only: a panel control mid-drag, heard like an offer but with no
  // saved revision, so applied_revision() keeps naming the last saved one.
  // Fails when the mailbox is full or a value is invalid.
  [[nodiscard]] bool offer_live(std::span<const double> values) noexcept;
  [[nodiscard]] std::uint64_t applied_revision() const noexcept;

  // Callback only, at a block start: take the newest offer.
  void adopt() noexcept;
  // Lanes name live, automatable parameters with one finite in-range value
  // per frame; discrete lanes are stepped (ADR-0034).
  [[nodiscard]] bool valid_lanes(std::span<const ParameterLane> lanes,
                                 std::size_t frames) const noexcept;
  // Callback only, at a control tick: advance glides one tick and return
  // the values in force at `block_offset`, automation applied.
  [[nodiscard]] std::span<const double> tick(std::span<const ParameterLane> lanes,
                                             std::size_t block_offset) noexcept;

private:
  [[nodiscard]] bool push(std::span<const double> values, std::uint64_t revision) noexcept;

  struct Mailbox {
    struct Entry {
      std::array<double, max_engine_parameters> values{};
      std::uint64_t revision = 0; // Zero: a live value, no saved revision.
    };
    static constexpr std::size_t capacity = 4;
    std::array<Entry, capacity> entries{};
    std::atomic<std::uint64_t> write{0};
    std::atomic<std::uint64_t> read{0};
    std::atomic<std::uint64_t> applied{0};
    std::uint64_t offered = 0; // Serialized control producer only.
  };

  const EngineDescriptor* descriptor_;
  std::size_t count_;
  double sample_rate_;
  std::vector<double> target_;     // Latest panel values.
  std::vector<double> current_;    // Panel values gliding toward target_.
  std::vector<double> glide_step_; // Per-tick glide in normalised units.
  std::vector<double> effective_;  // current_ with automation applied.
  std::unique_ptr<Mailbox> mailbox_ = std::make_unique<Mailbox>();
};

} // namespace oscillate::native_dsp
