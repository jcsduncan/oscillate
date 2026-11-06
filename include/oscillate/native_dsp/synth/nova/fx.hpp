#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Nova's effect rack (spec 08 §7, contract 08a §2.9, §2.10, §3.3 and §5):
// Main plus two buses, up to eight modules each, from a pool of 24 slot
// uids. Module instances belong to the compiled document so a module keeps
// its sound (tails, LFO phases) when unrelated parts of the document change.
namespace oscillate::native_dsp::synth::nova {

// Contract module ids, in this order.
enum class FxKind : std::uint8_t {
  distortion,
  filter,
  chorus,
  flanger,
  phaser,
  delay,
  reverb,
  compressor,
  multiband,
  eq,
  dimension,
  hyper,
  utility,
  convolve,
  shifter,
  flyby,
};
inline constexpr std::size_t fx_kind_count = 16;
inline constexpr std::size_t fx_slot_uids = 24;  // uids 1..24
inline constexpr std::size_t fx_chain_count = 3; // fx.main, fx.bus.1, fx.bus.2
inline constexpr std::size_t fx_max_modules_per_chain = 8;
using FxParams = std::array<double, fx_slot_params>;
// The per-slot spectrum in the telemetry frame (08a §6): log-spaced bands
// from 20 Hz to 20 kHz, band i centred at 20 * 1000^((i + 0.5) / n).
inline constexpr std::size_t fx_spectrum_bands = 48;
// Per-slot detail values (08a §6): Multiband's three band gain changes.
inline constexpr std::size_t fx_detail_values = 3;
[[nodiscard]] std::string_view fx_kind_id(FxKind kind) noexcept;
[[nodiscard]] std::optional<FxKind> fx_kind_from_id(std::string_view id) noexcept;
// The EQ (08a §5): band b uses p(4b) type, p(4b + 1) frequency (log
// 20..20000 Hz), p(4b + 2) gain (-24..24 dB), p(4b + 3) Q (log 0.25..16).
inline constexpr std::size_t eq_bands = 4;
inline constexpr std::size_t eq_band_params = 4;
inline constexpr double eq_min_hz = 20.0;
inline constexpr double eq_max_hz = 20'000.0;
inline constexpr double eq_max_db = 24.0;
inline constexpr double eq_min_q = 0.25;
inline constexpr double eq_max_q = 16.0;
enum class EqType : std::uint8_t {
  off,
  bell,
  low_shelf,
  high_shelf,
  low_cut_12,
  low_cut_24,
  high_cut_12,
  high_cut_24,
  notch,
  band_pass,
};
inline constexpr std::size_t eq_type_count = 10;
// The p value that selects `type`.
[[nodiscard]] constexpr double eq_type_value(EqType type) noexcept {
  return static_cast<double>(type) / static_cast<double>(eq_type_count - 1);
}
// The Shifter (08a §5): p0 is a shift of shifter_max_hz (2p - 1)^3 Hz; p1
// feedback up to shifter_max_feedback.
inline constexpr double shifter_max_hz = 5'000.0;
inline constexpr double shifter_max_feedback = 0.9;

// The contract's defaults for p0..p15 and mix (08a §5).
struct FxDefaults {
  FxParams p{};
  double mix = 1.0;
};
[[nodiscard]] FxDefaults fx_defaults(FxKind kind) noexcept;

// The musical context at a control tick.
struct FxContext {
  double beats_per_second = 2.0; // Tempo, for synced delays.
  // The rate the module runs at now: the engine's rate times its chain's
  // oversampling (1, 2 or 4). It changes when the user changes the chain's
  // oversampling; the module recomputes its coefficients and keeps its
  // buffers (sized at creation for 4x the engine's rate).
  double sample_rate = 48'000.0;
};

// One module. Created on control for an engine rate, with room to run at up
// to 4x it; all other calls are callback safe and allocation-free.
class FxModule {
public:
  virtual ~FxModule() = default;
  [[nodiscard]] virtual FxKind kind() const noexcept = 0;
  virtual void reset() noexcept = 0;
  // At a control tick: p0..p15 in 0..1 as stored (08a §5 maps them),
  // reached smoothly over `frames` samples.
  virtual void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept = 0;
  // In place, wet only: the rack applies mix and bypass.
  virtual void process(std::span<engine::StereoFrame> io) noexcept = 0;
  // 08a §6 offset 720: the module's live telemetry value.
  [[nodiscard]] virtual float telemetry() const noexcept { return 0.0F; }
  // 08a §6 offset 792: module-specific detail (Multiband's band gain
  // changes in dB); zeros for the rest.
  virtual void detail(std::span<float, fx_detail_values> out) const noexcept {
    std::fill(out.begin(), out.end(), 0.0F);
  }
  // Frames the module can keep sounding after silence, at its rate.
  [[nodiscard]] virtual std::uint64_t tail_frames() const noexcept { return 0; }
};

// Control only. `sample_rate` is the engine's rate; `max_frames` the largest
// block at that rate (the module may see up to 4x as many when its chain is
// oversampled). `ir` is the convolve module's decoded impulse (media slot
// fx.<uid>.ir), null for every other kind or when absent.
[[nodiscard]] foundation::Result<std::unique_ptr<FxModule>>
make_fx_module(FxKind kind, double sample_rate, std::size_t max_frames,
               std::shared_ptr<const SynthMediaAsset> ir);

// One placed module in a compiled layout.
struct FxSlot {
  std::uint32_t uid = 0; // 1..24
  FxKind kind = FxKind::distortion;
  std::shared_ptr<FxModule> module;
};

// The compiled rack layout: which module sits where, with its instance.
// Built on control from the document. Instances whose uid and kind are
// unchanged since `previous` are shared with it (the same object keeps
// running); new or changed ones are freshly made. `retiring` holds the
// instances `previous` had that this layout dropped or replaced, so the
// rack can fade them out instead of cutting them.
struct FxLayout {
  std::array<std::vector<FxSlot>, fx_chain_count> chains;
  std::vector<FxSlot> retiring;
};

// `media` holds decoded fx.<uid>.ir slots; a convolve whose IR changed is a
// fresh instance.
[[nodiscard]] foundation::Result<std::shared_ptr<const FxLayout>>
build_fx_layout(const project::NativeSynthDocument& document, double sample_rate,
                std::size_t max_frames, const SynthMedia& media, const FxLayout* previous);

// Per-tick rack settings from the flat parameters (08a §2.9 and §2.10),
// indexed by uid - 1 for the slots.
struct FxSlotSettings {
  FxParams p{};
  double mix = 1.0;
  bool on = true;
};
struct FxRackSettings {
  std::array<FxSlotSettings, fx_slot_uids> slots{};
  // nova.fx.main.oversample, nova.fx.bus.<b>.oversample: 1, 2 or 4. A change
  // resets that chain's oversamplers under a 5 ms duck.
  std::array<std::uint32_t, fx_chain_count> oversample{2, 1, 1};
  bool split = false;
  double split_hz = 150.0;
  std::array<double, 2> bus_send{0.0, 0.0};      // 0..1
  std::array<bool, 2> bus_send_post{true, true}; // After Main's modules.
  std::array<double, 2> bus_return_db{0.0, 0.0};
};

// The kernel's rack: owns the bus buffers, oversamplers and crossover;
// reads module instances from the adopted layout. prepare() on control,
// everything else callback safe and allocation-free.
class FxRack {
public:
  // Defined in fx.cpp, where State is complete.
  FxRack();
  ~FxRack();
  FxRack(const FxRack&) = delete;
  FxRack& operator=(const FxRack&) = delete;
  void prepare(double sample_rate, std::size_t max_frames);
  void reset() noexcept;
  // At a block start, when the kernel adopts a new document. A slot whose
  // module changed or left crossfades over 20 ms (its old instance is in
  // layout.retiring); a reorder ducks the affected chain for 5 ms each way,
  // so no edit clicks. The layout stays alive until the next adoption.
  void adopt(const FxLayout& layout) noexcept;
  // At a control tick, reached smoothly over `frames` samples.
  void set(const FxRackSettings& settings, const FxContext& context, std::uint32_t frames) noexcept;
  // In place on the voice sum at the engine's rate: Main (after the
  // optional low/high split), then the buses fed from Main's send points,
  // returned into the output.
  void process(std::span<engine::StereoFrame> io) noexcept;
  // 08a §6, all dBFS with a -120 floor where they are levels.
  struct Telemetry {
    std::array<float, fx_slot_uids> live{};   // Each module's own value.
    std::array<float, 6> meters{};            // L and R of Main, Bus 1, Bus 2.
    std::array<float, fx_slot_uids> input{};  // Peak into each slot.
    std::array<float, fx_slot_uids> output{}; // Peak out of it, after its mix.
    std::array<float, fx_slot_uids * fx_detail_values> detail{};
    std::array<float, fx_slot_uids * fx_spectrum_bands> spectrum{}; // Out of each slot.
  };
  void telemetry(Telemetry& out) const noexcept;
  // The longest tail of the adopted modules, at the engine's rate.
  [[nodiscard]] std::uint64_t tail_frames() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace oscillate::native_dsp::synth::nova
