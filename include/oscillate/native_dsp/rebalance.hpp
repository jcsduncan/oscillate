#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/eq.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// The master rebalance stage (ADR-0123): one role's gain and three-band EQ
// (low shelf, bell, high shelf) on a track's output, after its fader. The EQ
// is the classic cascade Shape runs (prepare_eq, EqRuntime).
namespace oscillate::native_dsp {

// One role's settings; gains in dB. Neutral settings leave audio untouched.
struct RoleTone {
  double gain_db = 0.0;
  double low_db = 0.0;
  double low_hz = 0.0;
  double mid_db = 0.0;
  double mid_hz = 0.0;
  double mid_q = 0.0;
  double high_db = 0.0;
  double high_hz = 0.0;
  bool operator==(const RoleTone&) const = default;
};

// The shelves' slope, as Shape's shelf shape: 1 is the steepest without
// overshoot.
inline constexpr double rebalance_shelf_slope = 1.0;
inline constexpr std::size_t rebalance_bands = 3;

// The three bands in cascade order, as Shape's EqBandSettings.
[[nodiscard]] std::array<EqBandSettings, rebalance_bands> rebalance_bands_for(const RoleTone& tone,
                                                                              double sample_rate);

// What a stage moves to: its EQ, the same EQ at 0 dB (where a stage that
// was bypassed starts its glide), the linear gain, whether every band is at
// 0 dB and whether the gain is unity too. Control side only.
struct RoleTarget {
  PreparedEq eq;
  PreparedEq flat;
  float gain = 1.0F;
  bool flat_eq = true;
  bool neutral = true;
  // Frames a flat EQ keeps running after a glide to it, so the filter state
  // the earlier bands left rings out before the stage stops running it.
  std::uint64_t settle_frames = 0;
  // Frames the EQ sounds after its input stops; zero while flat.
  std::uint64_t tail_frames = 0;
};
[[nodiscard]] foundation::Result<RoleTarget> role_target(const RoleTone& tone,
                                                         std::uint32_t sample_rate);
// A stage that does nothing (a track with no role, or the rebalance off).
[[nodiscard]] foundation::Result<RoleTarget> neutral_target(std::uint32_t sample_rate);

// A role's gain glides to a new value over this many frames.
inline constexpr std::uint32_t rebalance_gain_glide_frames = 512;

// One track's stage. It starts at its first target with no glide; later
// targets (offer) glide there: the EQ over EqRuntime's transition, the gain
// over rebalance_gain_glide_frames. While neutral and settled the stage
// leaves its block untouched, bit for bit. Construct and offer on the
// control side; process on the one callback that owns it. Never allocates.
class RoleStage final : public engine::ProcessorRuntime {
public:
  explicit RoleStage(const RoleTarget& initial) noexcept;

  // Control only, one producer: the newest offer wins.
  void offer(const RoleTarget& target) noexcept;

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override;
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override { return true; }

  // Callback side; tests read it after processing.
  [[nodiscard]] bool bypassed() const noexcept { return !eq_ && gain_ == 1.0F && !gliding(); }

private:
  [[nodiscard]] bool gliding() const noexcept { return glide_done_ < rebalance_gain_glide_frames; }
  void adopt() noexcept;

  // A triple buffer: the control side writes back_, publishes it as the
  // middle with the fresh bit, and the callback swaps it in as front_.
  static constexpr std::uint8_t fresh_bit = 4;
  static constexpr std::uint8_t index_mask = 3;
  std::array<RoleTarget, 3> buffers_;
  std::atomic<std::uint8_t> middle_{1};
  std::uint8_t back_ = 2;  // control only
  std::uint8_t front_ = 0; // callback only
  static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

  // Callback state.
  std::optional<EqRuntime> eq_;
  std::uint64_t offered_ = 0;
  bool pending_ = false;
  std::uint64_t settled_ = 0;
  float gain_ = 1.0F;
  float gain_from_ = 1.0F;
  std::uint32_t glide_done_ = rebalance_gain_glide_frames;
};

} // namespace oscillate::native_dsp
