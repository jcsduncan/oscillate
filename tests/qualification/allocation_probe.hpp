#pragma once

#include <cstdint>

// Intercepts ordinary scalar allocations on this rehearsal's callback thread.
// Aligned and direct C allocation are outside this count.
void begin_allocation_probe() noexcept;
void end_allocation_probe() noexcept;
[[nodiscard]] std::uint64_t allocation_probe_count() noexcept;
