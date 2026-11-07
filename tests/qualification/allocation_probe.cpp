#include "allocation_probe.hpp"

#include <cstdlib>
#include <new>

namespace {
thread_local bool watching = false;
thread_local std::uint64_t allocations = 0;
} // namespace

void begin_allocation_probe() noexcept { watching = true; }
void end_allocation_probe() noexcept { watching = false; }
std::uint64_t allocation_probe_count() noexcept { return allocations; }

void* operator new(std::size_t bytes) {
  if (watching)
    ++allocations;
  if (void* memory = std::malloc(bytes == 0 ? 1 : bytes))
    return memory;
  throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
