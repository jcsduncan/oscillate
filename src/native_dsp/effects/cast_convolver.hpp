#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct PFFFT_Setup;

// Cast's zero-latency partitioned convolution (ADR-0115) on the pinned
// PFFFT real transform. Taps 0 to head_taps - 1 run directly in time; four
// uniformly partitioned overlap-save tiers take the rest. Each tier starts
// at least twice its block after tap zero, except the first, which starts
// one block in and runs at its block's end. The others spread one block's
// transform, partition products and inverse over the next block, so the
// work per callback stays even at any callback size.
namespace oscillate::native_dsp::effects::cast_dsp {

inline constexpr std::size_t head_taps = 64;
struct TierLayout {
  std::size_t block;  // Frames per partition.
  std::size_t offset; // The tier's first tap.
};
inline constexpr std::array<TierLayout, 4> tiers{
    {{64, 64}, {256, 1'024}, {1'024, 4'096}, {4'096, 16'384}}};
inline constexpr std::size_t tier_count = tiers.size();
inline constexpr std::size_t max_responses = 4;
static_assert(tiers.front().offset == head_taps && tiers.front().block == head_taps);

// The partitions tier `tier` holds for an IR of `taps` taps.
[[nodiscard]] std::size_t partitions(std::size_t tier, std::size_t taps) noexcept;

// A float buffer aligned for PFFFT's SIMD.
class Floats {
public:
  Floats() = default;
  explicit Floats(std::size_t count);
  ~Floats();
  Floats(Floats&& other) noexcept;
  Floats& operator=(Floats&& other) noexcept;
  Floats(const Floats&) = delete;
  Floats& operator=(const Floats&) = delete;
  [[nodiscard]] float* data() noexcept { return data_; }
  [[nodiscard]] const float* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  void zero() noexcept;

private:
  float* data_ = nullptr;
  std::size_t size_ = 0;
};

// One real transform size's PFFFT setup. Control creates it; transforms run
// anywhere with their own work buffer.
class Fft {
public:
  explicit Fft(std::size_t size);
  ~Fft();
  Fft(Fft&& other) noexcept;
  Fft& operator=(Fft&& other) = delete;
  Fft(const Fft&) = delete;
  Fft& operator=(const Fft&) = delete;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  // Unordered spectra (PFFFT's internal order), unscaled both ways.
  void forward(const float* in, float* out, float* work) const noexcept;
  void inverse(const float* in, float* out, float* work) const noexcept;
  // Ordered spectra: DC, Nyquist, then the real and imaginary parts of
  // bins 1 to size / 2 - 1. Unscaled both ways.
  void forward_ordered(const float* in, float* out, float* work) const noexcept;
  void inverse_ordered(const float* in, float* out, float* work) const noexcept;
  // out += a * b * scale.
  void multiply_add(const float* a, const float* b, float* out, float scale) const noexcept;

private:
  PFFFT_Setup* setup_ = nullptr;
  std::size_t size_ = 0;
};

// How inputs reach outputs: mono (one response on each channel), stereo
// (one response per channel) or true stereo (left in to left and right out,
// then right in to left and right out).
enum class Routing : std::uint8_t { mono, stereo, true_stereo };
struct Route {
  std::uint8_t input = 0;
  std::uint8_t output = 0;
  std::uint8_t response = 0;
};
[[nodiscard]] std::span<const Route> routes(Routing routing) noexcept;
[[nodiscard]] Routing routing_for(std::size_t responses) noexcept;

// An IR prepared for the convolver: the head's taps and every tier's
// partition spectra. Immutable once built (control only).
class CompiledIr {
public:
  // Equal-length responses (1, 2 or 4), at most `capacity` taps.
  [[nodiscard]] static std::unique_ptr<CompiledIr>
  build(std::span<const std::vector<float>> responses, std::size_t capacity);

  [[nodiscard]] Routing routing() const noexcept { return routing_; }
  [[nodiscard]] std::size_t length() const noexcept { return length_; }
  [[nodiscard]] std::size_t partitions(std::size_t tier) const noexcept {
    return partitions_[tier];
  }
  // The head's taps of `response`, reversed (the newest input's tap last).
  [[nodiscard]] const float* head(std::size_t response) const noexcept {
    return head_[response].data();
  }
  // Partition `part` of tier `tier` for `response`, scaled by 1 / size.
  [[nodiscard]] const float* spectrum(std::size_t tier, std::size_t response,
                                      std::size_t part) const noexcept {
    return spectra_[tier].data() + (response * partitions_[tier] + part) * 2 * tiers[tier].block;
  }

private:
  CompiledIr() = default;
  Routing routing_ = Routing::mono;
  std::size_t length_ = 0;
  std::array<std::array<float, head_taps>, max_responses> head_{};
  std::array<std::size_t, tier_count> partitions_{};
  std::array<Floats, tier_count> spectra_;
};

// The convolution state one kernel owns. Control constructs it for IRs of
// up to `capacity` taps; process() runs on the callback without allocating.
class Convolver {
public:
  explicit Convolver(std::size_t capacity);

  // The IR to play (null: silence). The first IR, and any after reset(),
  // starts at once; a change crossfades over one block of each tier, which
  // keeps reading the previous IR until then (see references()).
  void set_ir(const CompiledIr* ir) noexcept;
  // Clears the history and output; the current IR stays.
  void reset() noexcept;
  // Adds nothing to `in`; writes the wet output. Any frame count.
  void process(std::span<const float> in_left, std::span<const float> in_right,
               std::span<float> out_left, std::span<float> out_right) noexcept;
  // True while the convolver may still read `ir`.
  [[nodiscard]] bool references(const CompiledIr* ir) const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  // One overlap-save tier: the block being collected, the previous one, the
  // input spectra of the last `capacity` blocks and one block's product
  // being computed in steps.
  struct Tier {
    Fft fft;
    std::size_t index = 0;
    std::size_t block = 0;
    std::size_t offset = 0;
    std::size_t capacity = 0; // Partitions of input history.
    std::array<Floats, 2> collect;
    std::array<Floats, 2> previous;
    std::array<Floats, 2> history;             // capacity spectra per input.
    std::array<std::array<Floats, 2>, 2> sums; // [new, old][output]
    Floats time;
    Floats work;
    std::size_t fill = 0;
    std::uint64_t blocks = 0; // Completed blocks.
    std::size_t zero_run = 0; // Completed blocks of silent input in a row.
    // The product in progress: block `computing` (0-based), its steps.
    bool pending = false;
    std::uint64_t computing = 0;
    std::size_t done = 0;
    std::size_t total = 0;
    bool skip = false;
    const CompiledIr* latched_new = nullptr;
    const CompiledIr* latched_old = nullptr;
    const CompiledIr* last = nullptr;

    explicit Tier(std::size_t tier, std::size_t partitions);
  };

  void run_head(std::span<const float> in_left, std::span<const float> in_right,
                std::span<float> out_left, std::span<float> out_right) noexcept;
  void start(Tier& tier) noexcept;
  void advance(Tier& tier, std::size_t target) noexcept;
  void step(Tier& tier) noexcept;

  std::size_t capacity_ = 0;
  std::vector<Tier> tiers_;
  const CompiledIr* target_ = nullptr;
  const CompiledIr* head_ir_ = nullptr;
  const CompiledIr* head_old_ = nullptr;
  std::size_t head_fade_ = 0; // Frames of the head's crossfade left.
  // Each input's last head_taps - 1 frames, then the chunk in progress.
  std::array<std::array<float, head_taps - 1 + head_taps>, 2> head_history_{};
  // Every tier's output, by absolute frame.
  std::array<Floats, 2> ring_;
  std::size_t ring_mask_ = 0;
  std::uint64_t position_ = 0;
};

} // namespace oscillate::native_dsp::effects::cast_dsp
