#pragma once

#include <cstdint>
#include <span>

namespace oscillate::warp_dsp {

// Worker-side random access to verified, project-rate stereo source audio.
// Positions outside [0, frames()) read as silence. Decoder-backed views may
// perform I/O or allocate in read(), so neither this interface nor any
// WarpStream using it may run in the audio callback. A false return reports
// a failed read, invalid request or cancellation.
class SourceView {
public:
  SourceView() = default;
  SourceView(const SourceView&) = delete;
  SourceView& operator=(const SourceView&) = delete;
  SourceView(SourceView&&) = delete;
  SourceView& operator=(SourceView&&) = delete;
  virtual ~SourceView() = default;

  [[nodiscard]] virtual std::uint64_t frames() const noexcept = 0;
  // Fills left/right (equal sizes) with frames [first, first + left.size()).
  [[nodiscard]] virtual bool read(std::int64_t first, std::span<float> left,
                                  std::span<float> right) noexcept = 0;
};

// In-memory planar view; the spans must outlive the view.
class MemorySourceView final : public SourceView {
public:
  // Channels are positional (left, right) like every planar stereo API here.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  MemorySourceView(std::span<const float> left, std::span<const float> right) noexcept
      : left_(left), right_(right) {}
  [[nodiscard]] std::uint64_t frames() const noexcept override { return left_.size(); }
  [[nodiscard]] bool read(std::int64_t first, std::span<float> left,
                          std::span<float> right) noexcept override;

private:
  std::span<const float> left_;
  std::span<const float> right_;
};

} // namespace oscillate::warp_dsp
