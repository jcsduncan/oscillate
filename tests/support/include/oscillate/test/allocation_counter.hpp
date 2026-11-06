#pragma once

// Counts heap allocations so real-time tests can prove a callback never
// allocates. Including this header replaces the program's global operator
// new and delete with malloc/free versions that count every operator new:
//
//   oscillate::test::counted_allocations = 0;
//   oscillate::test::counting_allocations = true;
//   render_one_block();
//   oscillate::test::counting_allocations = false;
//   REQUIRE_EQ(oscillate::test::counted_allocations, 0, "the callback allocated");
//
// Replacement allocation functions may be defined only once per program, so
// include this header from exactly one translation unit of a test executable.
// The default array, nothrow and sized forms forward to these, so they are
// counted too.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace oscillate::test {

// Per thread: counts only the thread that sets the flag, so workers and the
// test runner never pollute a callback's count.
inline thread_local bool counting_allocations = false;
inline thread_local std::size_t counted_allocations = 0;

// Process-wide: counts every thread while set, for code under test that may
// allocate on a thread the test does not own.
inline std::atomic<bool> counting_process_allocations{false};
inline std::atomic<std::size_t> process_allocations{0};

// Set to make this thread's next operator new throw std::bad_alloc, once.
inline thread_local bool fail_next_allocation = false;

} // namespace oscillate::test

void* operator new(std::size_t size) {
  namespace test = oscillate::test;
  if (test::fail_next_allocation) {
    test::fail_next_allocation = false;
    throw std::bad_alloc();
  }
  if (test::counting_allocations)
    ++test::counted_allocations;
  if (test::counting_process_allocations.load(std::memory_order_relaxed))
    test::process_allocations.fetch_add(1, std::memory_order_relaxed);
  // malloc(0) may return null; operator new must return a unique pointer.
  if (void* memory = std::malloc(size == 0 ? 1 : size))
    return memory;
  throw std::bad_alloc();
}

// GCC 15 inlines the replacement new into callers, sees malloc paired with a
// call to operator delete, and reports -Wmismatched-new-delete at the free
// below. Both sides are malloc/free by construction, so the warning is a false
// positive; it is silenced for these two definitions only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
