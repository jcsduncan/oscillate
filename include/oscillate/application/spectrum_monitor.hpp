#pragma once

#include "oscillate/application/spectrum_tap.hpp"
#include "oscillate/native_dsp/spectrum.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

namespace oscillate::application {

struct SpectrumReading {
  native_dsp::StereoSpectrum spectrum;
  std::uint64_t end_sample = 0;
  std::uint64_t transport_generation = 0;
  std::uint64_t epoch = 0;
};

// The callback only copies into a bounded SPSC tap. A dedicated worker owns
// FFT analysis; the UI copies the latest complete reading under a short mutex.
class SpectrumMonitor final {
public:
  SpectrumMonitor();
  SpectrumMonitor(const SpectrumMonitor&) = delete;
  SpectrumMonitor& operator=(const SpectrumMonitor&) = delete;
  ~SpectrumMonitor() = default;

  // The device has joined its callback before either control method runs.
  void set_sample_rate_after_quiesce(std::uint32_t sample_rate) noexcept;
  void invalidate_after_quiesce() noexcept;
  void observe(std::span<const engine::StereoFrame> frames, engine::BlockContext context) noexcept;
  [[nodiscard]] std::optional<SpectrumReading> latest() const;
  [[nodiscard]] std::uint64_t dropped_windows() const noexcept { return tap_.dropped_windows(); }

private:
  void run(const std::stop_token& stop) noexcept;

  StereoSpectrumTap tap_;
  std::atomic<std::uint32_t> sample_rate_{0};
  mutable std::mutex latest_mutex_;
  std::optional<SpectrumReading> latest_;
  // Destroyed first, so the worker joins before the tap and reading state.
  std::jthread worker_;
};

} // namespace oscillate::application
