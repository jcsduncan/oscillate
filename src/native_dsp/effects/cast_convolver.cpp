#include "cast_convolver.hpp"

#include <pffft/pffft.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace oscillate::native_dsp::effects::cast_dsp {
namespace {

// The first tier runs at its block's end; the others start their product
// then and must finish it within the next block, so each starts at least
// two of its blocks in.
constexpr bool tiers_meet_their_deadlines() {
  for (std::size_t tier = 1; tier < tier_count; ++tier) {
    if (tiers[tier].offset < 2 * tiers[tier].block ||
        (tiers[tier].offset - tiers[tier - 1].offset) % tiers[tier - 1].block != 0 ||
        tiers[tier].block % tiers.front().block != 0) {
      return false;
    }
  }
  return true;
}
static_assert(tiers_meet_their_deadlines());

constexpr std::array<Route, 2> mono_routes{{{0, 0, 0}, {1, 1, 0}}};
constexpr std::array<Route, 2> stereo_routes{{{0, 0, 0}, {1, 1, 1}}};
constexpr std::array<Route, 4> true_stereo_routes{{{0, 0, 0}, {0, 1, 1}, {1, 0, 2}, {1, 1, 3}}};
constexpr std::size_t mono_responses = 1;
constexpr std::size_t stereo_responses = 2;

// The tap after the tier's last.
[[nodiscard]] std::size_t tier_end(std::size_t tier, std::size_t taps) noexcept {
  return tier + 1 < tier_count ? std::min(taps, tiers[tier + 1].offset) : taps;
}

[[nodiscard]] std::size_t ring_size() noexcept {
  std::size_t size = 1;
  const std::size_t needed = tiers.back().offset + 2 * tiers.back().block;
  while (size < needed) {
    size *= 2;
  }
  return size;
}

} // namespace

std::size_t partitions(std::size_t tier, std::size_t taps) noexcept {
  const std::size_t begin = tiers[tier].offset;
  const std::size_t end = tier_end(tier, taps);
  if (end <= begin) {
    return 0;
  }
  return (end - begin + tiers[tier].block - 1) / tiers[tier].block;
}

Floats::Floats(std::size_t count) : size_(count) {
  if (count == 0) {
    return;
  }
  data_ = static_cast<float*>(pffft_aligned_malloc(count * sizeof(float)));
  if (data_ == nullptr) {
    throw std::bad_alloc();
  }
  zero();
}

Floats::~Floats() { pffft_aligned_free(data_); }

Floats::Floats(Floats&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

Floats& Floats::operator=(Floats&& other) noexcept {
  if (this != &other) {
    pffft_aligned_free(data_);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

void Floats::zero() noexcept {
  if (data_ != nullptr) {
    std::memset(data_, 0, size_ * sizeof(float));
  }
}

Fft::Fft(std::size_t size)
    : setup_(pffft_new_setup(static_cast<int>(size), PFFFT_REAL)), size_(size) {
  if (setup_ == nullptr) {
    throw std::bad_alloc();
  }
}

Fft::~Fft() {
  if (setup_ != nullptr) {
    pffft_destroy_setup(setup_);
  }
}

Fft::Fft(Fft&& other) noexcept
    : setup_(std::exchange(other.setup_, nullptr)), size_(std::exchange(other.size_, 0)) {}

void Fft::forward(const float* in, float* out, float* work) const noexcept {
  pffft_transform(setup_, in, out, work, PFFFT_FORWARD);
}

void Fft::inverse(const float* in, float* out, float* work) const noexcept {
  pffft_transform(setup_, in, out, work, PFFFT_BACKWARD);
}

void Fft::forward_ordered(const float* in, float* out, float* work) const noexcept {
  pffft_transform_ordered(setup_, in, out, work, PFFFT_FORWARD);
}

void Fft::inverse_ordered(const float* in, float* out, float* work) const noexcept {
  pffft_transform_ordered(setup_, in, out, work, PFFFT_BACKWARD);
}

void Fft::multiply_add(const float* a, const float* b, float* out, float scale) const noexcept {
  pffft_zconvolve_accumulate(setup_, a, b, out, scale);
}

std::span<const Route> routes(Routing routing) noexcept {
  switch (routing) {
  case Routing::mono:
    return mono_routes;
  case Routing::stereo:
    return stereo_routes;
  case Routing::true_stereo:
    return true_stereo_routes;
  }
  return mono_routes;
}

Routing routing_for(std::size_t responses) noexcept {
  if (responses <= mono_responses) {
    return Routing::mono;
  }
  return responses == stereo_responses ? Routing::stereo : Routing::true_stereo;
}

std::unique_ptr<CompiledIr> CompiledIr::build(std::span<const std::vector<float>> responses,
                                              std::size_t capacity) {
  auto ir = std::unique_ptr<CompiledIr>(new CompiledIr());
  ir->routing_ = routing_for(responses.size());
  const std::size_t count = std::min(responses.size(), max_responses);
  std::size_t length = 0;
  for (std::size_t r = 0; r < count; ++r) {
    length = std::max(length, responses[r].size());
  }
  ir->length_ = std::min(length, capacity);
  const auto tap = [&](std::size_t r, std::size_t index) {
    return index < responses[r].size() && index < ir->length_ ? responses[r][index] : 0.0F;
  };
  for (std::size_t r = 0; r < count; ++r) {
    for (std::size_t m = 0; m < head_taps; ++m) {
      ir->head_[r][head_taps - 1 - m] = tap(r, m);
    }
  }
  for (std::size_t tier = 0; tier < tier_count; ++tier) {
    const std::size_t block = tiers[tier].block;
    const std::size_t size = 2 * block;
    const std::size_t parts = cast_dsp::partitions(tier, ir->length_);
    ir->partitions_[tier] = parts;
    if (parts == 0) {
      continue;
    }
    const Fft fft(size);
    Floats time(size);
    Floats work(size);
    ir->spectra_[tier] = Floats(count * parts * size);
    const std::size_t end = tier_end(tier, ir->length_);
    const float scale = 1.0F / static_cast<float>(size);
    for (std::size_t r = 0; r < count; ++r) {
      for (std::size_t part = 0; part < parts; ++part) {
        time.zero();
        const std::size_t first = tiers[tier].offset + part * block;
        for (std::size_t m = 0; m < block && first + m < end; ++m) {
          time.data()[m] = tap(r, first + m) * scale;
        }
        fft.forward(time.data(), ir->spectra_[tier].data() + (r * parts + part) * size,
                    work.data());
      }
    }
  }
  return ir;
}

Convolver::Tier::Tier(std::size_t tier, std::size_t partitions)
    : fft(2 * tiers[tier].block), index(tier), block(tiers[tier].block), offset(tiers[tier].offset),
      capacity(std::max<std::size_t>(1, partitions)), time(2 * block), work(2 * block) {
  for (std::size_t c = 0; c < 2; ++c) {
    collect[c] = Floats(block);
    previous[c] = Floats(block);
    history[c] = Floats(capacity * 2 * block);
    sums[0][c] = Floats(2 * block);
    sums[1][c] = Floats(2 * block);
  }
}

Convolver::Convolver(std::size_t capacity) : capacity_(capacity) {
  tiers_.reserve(tier_count);
  for (std::size_t tier = 0; tier < tier_count; ++tier) {
    tiers_.emplace_back(tier, partitions(tier, capacity));
  }
  const std::size_t size = ring_size();
  ring_mask_ = size - 1;
  ring_[0] = Floats(size);
  ring_[1] = Floats(size);
}

void Convolver::set_ir(const CompiledIr* ir) noexcept {
  if (ir == target_) {
    return;
  }
  const bool first = target_ == nullptr && head_ir_ == nullptr;
  target_ = ir;
  if (first) {
    head_ir_ = ir;
    for (auto& tier : tiers_) {
      tier.last = ir;
    }
    return;
  }
  // The head fades from what it plays now (or was fading from).
  head_old_ = head_fade_ > 0 ? head_old_ : head_ir_;
  head_ir_ = ir;
  head_fade_ = head_taps;
}

void Convolver::reset() noexcept {
  for (auto& tier : tiers_) {
    for (std::size_t c = 0; c < 2; ++c) {
      tier.collect[c].zero();
      tier.previous[c].zero();
      tier.history[c].zero();
    }
    tier.fill = 0;
    tier.blocks = 0;
    tier.zero_run = 0;
    tier.pending = false;
    tier.latched_new = nullptr;
    tier.latched_old = nullptr;
    tier.last = target_;
  }
  head_ir_ = target_;
  head_old_ = nullptr;
  head_fade_ = 0;
  for (auto& history : head_history_) {
    history.fill(0.0F);
  }
  ring_[0].zero();
  ring_[1].zero();
  position_ = 0;
}

bool Convolver::references(const CompiledIr* ir) const noexcept {
  if (ir == nullptr) {
    return false;
  }
  if (ir == target_ || ir == head_ir_ || (head_fade_ > 0 && ir == head_old_)) {
    return true;
  }
  return std::ranges::any_of(tiers_, [ir](const Tier& tier) {
    return tier.last == ir || (tier.pending && (tier.latched_new == ir || tier.latched_old == ir));
  });
}

void Convolver::run_head(std::span<const float> in_left, std::span<const float> in_right,
                         std::span<float> out_left, std::span<float> out_right) noexcept {
  constexpr std::size_t kept = head_taps - 1;
  const std::size_t frames = in_left.size();
  std::copy(in_left.begin(), in_left.end(), head_history_[0].begin() + kept);
  std::copy(in_right.begin(), in_right.end(), head_history_[1].begin() + kept);
  const std::array<float*, 2> out{out_left.data(), out_right.data()};
  const auto render = [&](const CompiledIr& ir, std::size_t i, std::array<float, 2>& y) {
    for (const Route& route : routes(ir.routing())) {
      const float* taps = ir.head(route.response);
      const float* x = head_history_[route.input].data() + i;
      float sum = 0.0F;
      for (std::size_t m = 0; m < head_taps; ++m) {
        sum += taps[m] * x[m];
      }
      y[route.output] += sum;
    }
  };
  for (std::size_t i = 0; i < frames; ++i) {
    std::array<float, 2> y{};
    if (head_ir_ != nullptr) {
      render(*head_ir_, i, y);
    }
    if (head_fade_ > 0) {
      std::array<float, 2> old{};
      if (head_old_ != nullptr) {
        render(*head_old_, i, old);
      }
      const float weight = static_cast<float>(head_fade_) / static_cast<float>(head_taps + 1);
      for (std::size_t c = 0; c < 2; ++c) {
        y[c] = y[c] + weight * (old[c] - y[c]);
      }
      if (--head_fade_ == 0) {
        head_old_ = nullptr;
      }
    }
    for (std::size_t c = 0; c < 2; ++c) {
      const std::size_t at = (position_ + i) & ring_mask_;
      out[c][i] = y[c] + ring_[c].data()[at];
      ring_[c].data()[at] = 0.0F;
    }
  }
  for (auto& history : head_history_) {
    std::copy(history.begin() + static_cast<std::ptrdiff_t>(frames),
              history.begin() + static_cast<std::ptrdiff_t>(frames + kept), history.begin());
  }
}

void Convolver::start(Tier& tier) noexcept {
  const std::size_t size = 2 * tier.block;
  const std::uint64_t k = tier.blocks;
  ++tier.blocks;
  bool silent = true;
  for (std::size_t c = 0; c < 2 && silent; ++c) {
    const float* x = tier.collect[c].data();
    silent = std::all_of(x, x + tier.block, [](float v) { return v == 0.0F; });
  }
  tier.zero_run = silent ? tier.zero_run + 1 : 0;
  // The frame [previous, current] of each input, transformed into the
  // history slot of this block (silence when both blocks were silent).
  const std::size_t slot = static_cast<std::size_t>(k % tier.capacity);
  for (std::size_t c = 0; c < 2; ++c) {
    float* spectrum = tier.history[c].data() + slot * size;
    if (tier.zero_run >= 2) {
      std::memset(spectrum, 0, size * sizeof(float));
    } else {
      std::memcpy(tier.time.data(), tier.previous[c].data(), tier.block * sizeof(float));
      std::memcpy(tier.time.data() + tier.block, tier.collect[c].data(),
                  tier.block * sizeof(float));
      tier.fft.forward(tier.time.data(), spectrum, tier.work.data());
    }
    std::memcpy(tier.previous[c].data(), tier.collect[c].data(), tier.block * sizeof(float));
  }
  tier.latched_new = target_;
  tier.latched_old = tier.last != target_ ? tier.last : nullptr;
  tier.last = target_;
  const std::size_t parts =
      std::max(tier.latched_new != nullptr ? tier.latched_new->partitions(tier.index) : 0,
               tier.latched_old != nullptr ? tier.latched_old->partitions(tier.index) : 0);
  tier.computing = k;
  tier.done = 0;
  tier.total = std::min(parts, tier.capacity) + 1;
  // Every partition reads silence and nothing fades: no output.
  tier.skip = parts == 0 || (tier.latched_old == nullptr && tier.zero_run > tier.capacity);
  tier.pending = true;
  for (auto& version : tier.sums) {
    for (auto& sum : version) {
      sum.zero();
    }
  }
}

void Convolver::step(Tier& tier) noexcept {
  const std::size_t index = tier.index;
  const std::size_t size = 2 * tier.block;
  const std::size_t parts = tier.total - 1;
  if (tier.skip) {
    tier.done = tier.total;
    tier.pending = false;
    return;
  }
  if (tier.done < parts) {
    // Partition `part` multiplies the input `part` blocks back.
    const std::size_t part = tier.done;
    if (part <= tier.computing) {
      const std::size_t slot = static_cast<std::size_t>((tier.computing - part) % tier.capacity);
      const std::array<const CompiledIr*, 2> versions{tier.latched_new, tier.latched_old};
      for (std::size_t v = 0; v < versions.size(); ++v) {
        const CompiledIr* ir = versions[v];
        if (ir == nullptr || part >= ir->partitions(index)) {
          continue;
        }
        for (const Route& route : routes(ir->routing())) {
          tier.fft.multiply_add(tier.history[route.input].data() + slot * size,
                                ir->spectrum(index, route.response, part),
                                tier.sums[v][route.output].data(), 1.0F);
        }
      }
    }
    ++tier.done;
    return;
  }
  // The inverse: the second half of each product is this block's output,
  // due `offset` frames after the block began.
  const std::uint64_t base = tier.computing * tier.block + tier.offset;
  const bool fading = tier.latched_old != nullptr;
  for (std::size_t c = 0; c < 2; ++c) {
    float* ring = ring_[c].data();
    tier.fft.inverse(tier.sums[0][c].data(), tier.time.data(), tier.work.data());
    const float* now = tier.time.data() + tier.block;
    if (!fading) {
      for (std::size_t n = 0; n < tier.block; ++n) {
        ring[(base + n) & ring_mask_] += now[n];
      }
      continue;
    }
    // The old IR's product, transformed in place.
    tier.fft.inverse(tier.sums[1][c].data(), tier.sums[1][c].data(), tier.work.data());
    const float* old = tier.sums[1][c].data() + tier.block;
    const float span = static_cast<float>(tier.block + 1);
    for (std::size_t n = 0; n < tier.block; ++n) {
      const float weight = static_cast<float>(n + 1) / span;
      ring[(base + n) & ring_mask_] += old[n] + weight * (now[n] - old[n]);
    }
  }
  tier.done = tier.total;
  tier.pending = false;
}

void Convolver::advance(Tier& tier, std::size_t target) noexcept {
  while (tier.pending && tier.done < std::min(target, tier.total)) {
    step(tier);
  }
}

void Convolver::process(std::span<const float> in_left, std::span<const float> in_right,
                        std::span<float> out_left, std::span<float> out_right) noexcept {
  const std::size_t unit = tiers.front().block;
  std::size_t at = 0;
  while (at < in_left.size()) {
    const std::size_t frames =
        std::min(in_left.size() - at, unit - static_cast<std::size_t>(position_ % unit));
    run_head(in_left.subspan(at, frames), in_right.subspan(at, frames),
             out_left.subspan(at, frames), out_right.subspan(at, frames));
    for (auto& tier : tiers_) {
      std::memcpy(tier.collect[0].data() + tier.fill, in_left.data() + at, frames * sizeof(float));
      std::memcpy(tier.collect[1].data() + tier.fill, in_right.data() + at, frames * sizeof(float));
      tier.fill += frames;
    }
    position_ += frames;
    at += frames;
    for (std::size_t index = 0; index < tiers_.size(); ++index) {
      Tier& tier = tiers_[index];
      if (index == 0) {
        if (tier.fill == tier.block) {
          start(tier);
          advance(tier, tier.total);
          tier.fill = 0;
        }
        continue;
      }
      // Steps in proportion to the block collected since the product began.
      if (tier.pending) {
        advance(tier, (tier.total * tier.fill + tier.block - 1) / tier.block);
      }
      if (tier.fill == tier.block) {
        advance(tier, tier.total);
        start(tier);
        tier.fill = 0;
      }
    }
  }
}

} // namespace oscillate::native_dsp::effects::cast_dsp
