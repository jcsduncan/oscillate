#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct PFFFT_Setup;

namespace oscillate::native_dsp {

// Dual-mono stereo convolution core. IR preparation/destruction/reset are
// control-side operations after callback quiescence. One callback owns process();
// it performs no allocation, blocking or I/O. The owning graph supplies a
// sample-rate-matched finite IR and at most 128 frames per call.
class Convolution final {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<Convolution>>
  create(std::span<const engine::StereoFrame> ir, std::uint32_t rate, float wet);

  ~Convolution();
  Convolution(const Convolution&) = delete;
  Convolution& operator=(const Convolution&) = delete;

  void process(std::span<engine::StereoFrame> frames, std::uint64_t generation) noexcept;
  // A seek must advance the processing generation. Reject a stale reset so
  // delayed control work cannot erase a newer IR tail or reactivate old audio.
  bool reset_after_quiesce(std::uint64_t generation) noexcept;

  [[nodiscard]] std::uint64_t tail_frames() const noexcept { return ir_length_ - 1; }
  [[nodiscard]] static constexpr std::uint32_t latency_frames() noexcept { return 0; }
  [[nodiscard]] static constexpr std::size_t max_block_frames() noexcept { return 128; }
  [[nodiscard]] std::uint64_t guarded_frames_after_quiesce() const noexcept { return guarded_; }

private:
  struct Aligned {
    explicit Aligned(std::size_t count = 0);
    ~Aligned();
    Aligned(Aligned&& other) noexcept;
    Aligned& operator=(Aligned&& other) noexcept;
    Aligned(const Aligned&) = delete;
    Aligned& operator=(const Aligned&) = delete;
    [[nodiscard]] float* get() noexcept { return data_; }
    [[nodiscard]] const float* get() const noexcept { return data_; }
    void clear() noexcept;

    float* data_ = nullptr;
    std::size_t count_ = 0;
  };

  struct Tier {
    static void destroy_setup(PFFFT_Setup* value) noexcept;
    Tier(std::size_t block_size, std::size_t offset, std::size_t end, std::size_t ir_length,
         std::span<const engine::StereoFrame> ir);
    ~Tier();
    Tier(Tier&& other) noexcept;
    Tier& operator=(Tier&& other) = delete;
    Tier(const Tier&) = delete;
    Tier& operator=(const Tier&) = delete;

    void begin_block() noexcept;
    void push(engine::StereoFrame input, std::size_t block_index) noexcept;
    void reset() noexcept;
    [[nodiscard]] float output(std::size_t channel, std::size_t index) const noexcept {
      return rendered[channel].get()[index];
    }

    std::size_t block = 0;
    std::size_t offset_blocks = 0;
    std::size_t partitions = 0;
    std::size_t ring_blocks = 0;
    std::uint64_t completed_blocks = 0;
    // The long tier can prepare next block's partition sum while rendering
    // this block: its first input is eight tier blocks in the past.
    bool distributed = false;
    std::size_t prepared_parts = 0;
    std::size_t prepare_numerator = 0;
    std::unique_ptr<PFFFT_Setup, void (*)(PFFFT_Setup*)> setup{nullptr, &destroy_setup};
    std::array<Aligned, 2> spectra;
    std::array<Aligned, 2> input_ring;
    std::array<Aligned, 2> input_time;
    std::array<Aligned, 2> spectrum_sum;
    std::array<Aligned, 2> inverse_time;
    std::array<Aligned, 2> overlap;
    std::array<Aligned, 2> rendered;
    Aligned work;
  };

  struct Controls {
    std::uint32_t rate;
    float wet;
  };
  Convolution(std::span<const engine::StereoFrame> ir, Controls controls);
  std::size_t ir_length_ = 0;
  float wet_ = 0.0F;
  std::size_t head_length_ = 0;
  // Interleave the direct FIR channels to share ring indexing and locality.
  std::array<engine::StereoFrame, 128> head_{};
  std::array<engine::StereoFrame, 128> input_head_{};
  std::size_t head_cursor_ = 0;
  std::vector<Tier> tiers_;
  std::uint64_t position_ = 0;
  std::uint64_t generation_ = 1;
  std::uint64_t guarded_ = 0;
};

} // namespace oscillate::native_dsp
