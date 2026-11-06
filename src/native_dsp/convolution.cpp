#include "oscillate/native_dsp/convolution.hpp"

#include <pffft/pffft.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace oscillate::native_dsp {
namespace {
[[nodiscard]] std::size_t block_at_rate(std::uint32_t rate, std::size_t block_at_48k) noexcept {
  return rate > 48'000 ? 2 * block_at_48k : block_at_48k;
}

struct BoundarySpec {
  std::size_t at_48k;
  std::size_t block;
};
[[nodiscard]] std::size_t aligned_boundary(std::uint32_t rate, BoundarySpec spec) noexcept {
  const auto scaled = (spec.at_48k * rate + 47'999U) / 48'000U;
  return ((scaled + spec.block - 1) / spec.block) * spec.block;
}
} // namespace

Convolution::Aligned::Aligned(std::size_t count) : count_(count) {
  if (count != 0) {
    data_ = static_cast<float*>(pffft_aligned_malloc(count * sizeof(float)));
    if (data_ == nullptr) {
      throw std::bad_alloc();
    }
    clear();
  }
}

Convolution::Aligned::~Aligned() { pffft_aligned_free(data_); }

Convolution::Aligned::Aligned(Aligned&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), count_(std::exchange(other.count_, 0)) {}

Convolution::Aligned& Convolution::Aligned::operator=(Aligned&& other) noexcept {
  if (this != &other) {
    pffft_aligned_free(data_);
    data_ = std::exchange(other.data_, nullptr);
    count_ = std::exchange(other.count_, 0);
  }
  return *this;
}

void Convolution::Aligned::clear() noexcept {
  if (data_ != nullptr) {
    std::fill_n(data_, count_, 0.0F);
  }
}

void Convolution::Tier::destroy_setup(PFFFT_Setup* value) noexcept { pffft_destroy_setup(value); }

Convolution::Tier::Tier(std::size_t block_size, std::size_t absolute_offset,
                        std::size_t absolute_end, std::size_t ir_length,
                        std::span<const engine::StereoFrame> ir)
    : block(block_size), offset_blocks(absolute_offset / block_size),
      partitions((std::min(ir_length, absolute_end) - absolute_offset + block_size - 1) /
                 block_size),
      ring_blocks(offset_blocks + partitions + 1),
      distributed(block_size >= 1024 && offset_blocks >= 2 && partitions <= block_size),
      setup(pffft_new_setup(static_cast<int>(2 * block_size), PFFFT_REAL), &destroy_setup),
      work(2 * block_size) {
  if (!setup) {
    throw std::bad_alloc();
  }
  const std::size_t fft_size = 2 * block;
  for (std::size_t channel = 0; channel < 2; ++channel) {
    spectra[channel] = Aligned(partitions * fft_size);
    input_ring[channel] = Aligned(ring_blocks * fft_size);
    input_time[channel] = Aligned(fft_size);
    spectrum_sum[channel] = Aligned(fft_size);
    inverse_time[channel] = Aligned(fft_size);
    overlap[channel] = Aligned(block);
    rendered[channel] = Aligned(block);
    for (std::size_t part = 0; part < partitions; ++part) {
      input_time[channel].clear();
      for (std::size_t tap = 0; tap < block; ++tap) {
        const auto absolute_tap = absolute_offset + part * block + tap;
        if (absolute_tap >= ir_length) {
          break;
        }
        input_time[channel].get()[tap] =
            channel == 0 ? ir[absolute_tap].left : ir[absolute_tap].right;
      }
      pffft_transform(setup.get(), input_time[channel].get(),
                      spectra[channel].get() + part * fft_size, work.get(), PFFFT_FORWARD);
    }
    input_time[channel].clear();
  }
}

Convolution::Tier::~Tier() = default;

Convolution::Tier::Tier(Tier&& other) noexcept
    : block(other.block), offset_blocks(other.offset_blocks), partitions(other.partitions),
      ring_blocks(other.ring_blocks), completed_blocks(other.completed_blocks),
      distributed(other.distributed), prepared_parts(other.prepared_parts),
      prepare_numerator(other.prepare_numerator), setup(std::move(other.setup)),
      spectra(std::move(other.spectra)), input_ring(std::move(other.input_ring)),
      input_time(std::move(other.input_time)), spectrum_sum(std::move(other.spectrum_sum)),
      inverse_time(std::move(other.inverse_time)), overlap(std::move(other.overlap)),
      rendered(std::move(other.rendered)), work(std::move(other.work)) {}

void Convolution::Tier::begin_block() noexcept {
  const std::uint64_t output_block = completed_blocks;
  const auto fft_size = 2 * block;
  for (std::size_t channel = 0; channel < 2; ++channel) {
    if (!distributed) {
      spectrum_sum[channel].clear();
      for (std::size_t part = 0; part < partitions; ++part) {
        const std::uint64_t delay = offset_blocks + part;
        if (output_block < delay) {
          continue;
        }
        const auto input_block = output_block - delay;
        const auto* past = input_ring[channel].get() + (input_block % ring_blocks) * fft_size;
        const auto* filter = spectra[channel].get() + part * fft_size;
        pffft_zconvolve_accumulate(setup.get(), past, filter, spectrum_sum[channel].get(),
                                   1.0F / static_cast<float>(fft_size));
      }
    }
    pffft_transform(setup.get(), spectrum_sum[channel].get(), inverse_time[channel].get(),
                    work.get(), PFFFT_BACKWARD);
    for (std::size_t frame = 0; frame < block; ++frame) {
      rendered[channel].get()[frame] =
          inverse_time[channel].get()[frame] + overlap[channel].get()[frame];
      overlap[channel].get()[frame] = inverse_time[channel].get()[frame + block];
    }
    if (distributed) {
      spectrum_sum[channel].clear();
    }
  }
  prepared_parts = 0;
  prepare_numerator = 0;
}

void Convolution::Tier::push(engine::StereoFrame input, std::size_t block_index) noexcept {
  if (distributed) {
    // Every tier sample advances a bounded fraction of the next output
    // block's partition sum. At the final sample all partitions are ready;
    // current input is FFT'd only after the old ring slots have been read.
    prepare_numerator += partitions;
    if (prepare_numerator >= block) {
      prepare_numerator -= block;
      const auto part = prepared_parts++;
      const auto output_block = completed_blocks + 1;
      const auto delay = offset_blocks + part;
      if (output_block >= delay) {
        const auto input_block = output_block - delay;
        const auto fft_size = 2 * block;
        for (std::size_t channel = 0; channel < 2; ++channel) {
          const auto* past = input_ring[channel].get() + (input_block % ring_blocks) * fft_size;
          const auto* filter = spectra[channel].get() + part * fft_size;
          pffft_zconvolve_accumulate(setup.get(), past, filter, spectrum_sum[channel].get(),
                                     1.0F / static_cast<float>(fft_size));
        }
      }
    }
  }
  input_time[0].get()[block_index] = input.left;
  input_time[1].get()[block_index] = input.right;
  if (block_index + 1 != block) {
    return;
  }
  const auto fft_size = 2 * block;
  const auto slot = completed_blocks % ring_blocks;
  for (std::size_t channel = 0; channel < 2; ++channel) {
    pffft_transform(setup.get(), input_time[channel].get(),
                    input_ring[channel].get() + slot * fft_size, work.get(), PFFFT_FORWARD);
  }
  ++completed_blocks;
}

void Convolution::Tier::reset() noexcept {
  completed_blocks = 0;
  prepared_parts = 0;
  prepare_numerator = 0;
  for (std::size_t channel = 0; channel < 2; ++channel) {
    input_ring[channel].clear();
    input_time[channel].clear();
    spectrum_sum[channel].clear();
    inverse_time[channel].clear();
    overlap[channel].clear();
    rendered[channel].clear();
  }
}

Convolution::Convolution(std::span<const engine::StereoFrame> ir, Controls controls)
    : ir_length_(ir.size()), wet_(controls.wet), head_length_(block_at_rate(controls.rate, 64)) {
  for (std::size_t tap = 0; tap < std::min(ir.size(), head_length_); ++tap) {
    head_[tap] = ir[tap];
  }
  const auto middle_block = block_at_rate(controls.rate, 256);
  auto long_block = block_at_rate(controls.rate, 1024);
#if defined(CONVOLUTION_HALF_LONG_TIER)
  long_block /= 2;
#endif
  const auto middle_start = aligned_boundary(controls.rate, {2048, middle_block});
  const auto long_start = aligned_boundary(controls.rate, {8192, long_block});
  const std::array<std::array<std::size_t, 3>, 3> schedule{{
      {head_length_, head_length_, middle_start},
      {middle_block, middle_start, long_start},
      {long_block, long_start, ir.size()},
  }};
  for (const auto& [block, offset, end] : schedule) {
    if (ir.size() > offset) {
      tiers_.emplace_back(block, offset, end, ir.size(), ir);
    }
  }
}

Convolution::~Convolution() = default;

foundation::Result<std::unique_ptr<Convolution>>
Convolution::create(std::span<const engine::StereoFrame> ir, std::uint32_t rate, float wet) {
  using foundation::Error;
  using foundation::ErrorCode;
  const bool supported_rate = rate == 44'100 || rate == 48'000 || rate == 88'200 || rate == 96'000;
  if (!supported_rate || ir.empty() || ir.size() > 2ULL * rate || !std::isfinite(wet) ||
      wet < 0.0F || wet > 1.0F) {
    return foundation::Result<std::unique_ptr<Convolution>>::failure(Error{
        ErrorCode::invalid_argument, "convolution requires supported rate, <=2 s IR and wet 0..1"});
  }
  for (const auto& frame : ir) {
    if (!std::isfinite(frame.left) || !std::isfinite(frame.right)) {
      return foundation::Result<std::unique_ptr<Convolution>>::failure(
          Error{ErrorCode::invalid_argument, "IR contains a non-finite sample"});
    }
  }
  try {
    return foundation::Result<std::unique_ptr<Convolution>>::success(
        std::unique_ptr<Convolution>(new Convolution(ir, {rate, wet})));
  } catch (const std::bad_alloc&) {
    return foundation::Result<std::unique_ptr<Convolution>>::failure(
        Error{ErrorCode::resource_exhausted, "convolution FFT preparation failed"});
  }
}

void Convolution::process(std::span<engine::StereoFrame> frames,
                          std::uint64_t generation) noexcept {
  if (generation != generation_ || frames.size() > max_block_frames()) {
    std::ranges::fill(frames, engine::StereoFrame{});
    guarded_ += std::min<std::uint64_t>(frames.size(),
                                        std::numeric_limits<std::uint64_t>::max() - guarded_);
    return;
  }
  for (auto& frame : frames) {
    float input[2]{frame.left, frame.right};
    if (!std::isfinite(input[0]) || !std::isfinite(input[1])) {
      input[0] = 0.0F;
      input[1] = 0.0F;
      if (guarded_ != std::numeric_limits<std::uint64_t>::max()) {
        ++guarded_;
      }
    }
    float wet[2]{};
    // All selected partition/head sizes are powers of two; masks keep the
    // per-sample direct head and tier indexing bounded without division.
    input_head_[head_cursor_] = {input[0], input[1]};
    for (std::size_t tap = 0; tap < std::min(ir_length_, head_length_); ++tap) {
      const auto& filter = head_[tap];
      const auto& past = input_head_[(head_cursor_ + head_length_ - tap) & (head_length_ - 1)];
      wet[0] += filter.left * past.left;
      wet[1] += filter.right * past.right;
    }
    for (auto& tier : tiers_) {
      const auto index = static_cast<std::size_t>(position_ & (tier.block - 1));
      if (index == 0) {
        tier.begin_block();
      }
      wet[0] += tier.output(0, index);
      wet[1] += tier.output(1, index);
      tier.push({input[0], input[1]}, index);
    }
    for (std::size_t channel = 0; channel < 2; ++channel) {
      float value = (1.0F - wet_) * input[channel] + wet_ * wet[channel];
      if (!std::isfinite(value)) {
        value = (1.0F - wet_) * input[channel];
        if (guarded_ != std::numeric_limits<std::uint64_t>::max()) {
          ++guarded_;
        }
      }
      if (channel == 0) {
        frame.left = value;
      } else {
        frame.right = value;
      }
    }
    head_cursor_ = (head_cursor_ + 1) & (head_length_ - 1);
    ++position_;
  }
}

bool Convolution::reset_after_quiesce(std::uint64_t generation) noexcept {
  if (generation == 0 || generation <= generation_)
    return false;
  generation_ = generation;
  position_ = 0;
  head_cursor_ = 0;
  guarded_ = 0;
  input_head_.fill({});
  for (auto& tier : tiers_) {
    tier.reset();
  }
  return true;
}

} // namespace oscillate::native_dsp
