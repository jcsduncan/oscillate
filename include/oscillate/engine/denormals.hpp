#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace oscillate::engine {

// Flushes denormal numbers to zero for its lifetime, then restores the
// previous mode: FTZ and DAZ on x86, FZ on AArch64. A decaying tail (a
// reverb, a filter, a resampler ringing out) otherwise reaches values below
// 1e-308 (1e-38 for floats) that the CPU computes in microcode, tens of times
// slower: a voice that has gone silent could cost more than one playing.
// Every render entry holds one (the device callback, audio workers, offline
// export, and the native synth and effect runtimes), so live, export and
// tests compute alike. Two register writes; callback safe.
class DenormalsFlushed {
public:
  DenormalsFlushed() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    saved_ = _mm_getcsr();
    _mm_setcsr(saved_ | flush_bits);
#elif defined(__aarch64__)
    saved_ = __builtin_aarch64_get_fpcr64();
    __builtin_aarch64_set_fpcr64(saved_ | flush_bits);
#endif
  }
  ~DenormalsFlushed() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(saved_);
#elif defined(__aarch64__)
    __builtin_aarch64_set_fpcr64(saved_);
#endif
  }
  DenormalsFlushed(const DenormalsFlushed&) = delete;
  DenormalsFlushed& operator=(const DenormalsFlushed&) = delete;

private:
#if defined(__x86_64__) || defined(__i386__)
  static constexpr unsigned flush_bits = 0x8040U; // FTZ (bit 15) and DAZ (bit 6).
  unsigned saved_ = 0;
#elif defined(__aarch64__)
  static constexpr std::uint64_t flush_bits = std::uint64_t{1} << 24U; // FZ.
  std::uint64_t saved_ = 0;
#endif
};

} // namespace oscillate::engine
