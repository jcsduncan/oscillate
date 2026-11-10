#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// Stick's production chain (ADR-0121): each mic group's inserts (gate, EQ,
// compressor, transient shaper, saturation), the parallel compression bus
// and the kit bus (glue, EQ, clip), each an embedded native effect kernel
// (Hush, Shape, Press, Strike, Forge) run at zero latency. A stage switches
// in and out over a short crossfade; off and faded out it is not processed.
// Private to the engine.
namespace oscillate::native_dsp::synth::stick::detail {

using Frames = std::span<engine::StereoFrame>;

// One stage: an effect's kernel, its values in the effect's table order,
// and the crossfade that switches it in.
class Insert {
public:
  // Control: the kernel at the effect's defaults (each effect the chain
  // uses runs at zero latency there); false when it cannot run.
  bool init(const effects::EffectEngine& engine, double sample_rate, std::size_t max_frames);
  [[nodiscard]] std::span<double> values() noexcept { return values_; }
  [[nodiscard]] const effects::EffectEngine& engine() const noexcept { return *engine_; }

  // At a control tick: towards on or off over the crossfade, and the
  // values to the kernel while it sounds. A stage coming in starts clean.
  void control(bool on, std::uint32_t frames, const Transport& transport) noexcept;
  // In place: `since` frames after the tick, within it.
  void process(Frames io, std::size_t since) noexcept;
  // Whether it changes the signal in this tick.
  [[nodiscard]] bool sounding() const noexcept { return wet_from_ > 0.0 || wet_to_ > 0.0; }
  void reset() noexcept;

private:
  const effects::EffectEngine* engine_ = nullptr;
  std::unique_ptr<effects::EffectKernel> kernel_;
  std::vector<double> values_;
  std::vector<engine::StereoFrame> dry_;
  double fade_frames_ = 1.0;
  double wet_from_ = 0.0;
  double wet_to_ = 0.0;
  double span_ = 1.0;
  bool primed_ = false;
};

// Every stage of the chain, mapped from Stick's values.
class Chain {
public:
  // Control: every kernel; false when one cannot run.
  bool init(double sample_rate, std::size_t max_frames);

  // At a control tick, from Stick's values.
  void control(std::span<const double> values, std::uint32_t frames,
               const Transport& transport) noexcept;
  // Whether a group needs its own bus this tick: a stage sounding or its
  // Parallel send open.
  [[nodiscard]] bool routed(MicGroup group) const noexcept;
  // The group's inserts, in place.
  void process_group(MicGroup group, Frames io, std::size_t since) noexcept;
  // A group's Parallel send at `u` (0..1) through the tick.
  [[nodiscard]] double send(MicGroup group, double u) const noexcept;
  [[nodiscard]] bool parallel_sounding() const noexcept { return parallel_open_; }
  // The parallel bus's compressor, in place, and its return gain at `u`.
  void process_parallel(Frames io, std::size_t since) noexcept;
  [[nodiscard]] double parallel_return(double u) const noexcept;
  [[nodiscard]] bool kit_sounding() const noexcept;
  // The kit bus: glue, EQ, clip, in place.
  void process_kit(Frames io, std::size_t since) noexcept;
  void reset() noexcept;

  // Control: what the chain adds to Stick's tail for these values.
  [[nodiscard]] static std::uint64_t tail_frames(std::span<const double> values,
                                                 double sample_rate);

private:
  struct Group {
    std::array<Insert, stage_count> stages;
    double send_from = 0.0;
    double send_to = 0.0;
  };
  std::array<Group, mic_group_count> groups_{};
  Insert parallel_;
  Insert glue_;
  Insert kit_eq_;
  Insert clip_;
  double return_from_ = 0.0;
  double return_to_ = 0.0;
  bool parallel_open_ = false;
  bool primed_ = false;
};

} // namespace oscillate::native_dsp::synth::stick::detail
