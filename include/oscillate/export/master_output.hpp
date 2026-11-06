#pragma once

#include "oscillate/export/offline_master.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace oscillate::exporting::detail {

// Rounded output-rate duration of a project-rate render.
[[nodiscard]] foundation::Result<std::uint64_t>
converted_frame_count(std::uint64_t frames, std::uint32_t input_rate, std::uint32_t output_rate);

// Worker-only master output stage shared by native and VST3 offline exports.
// It accepts exactly `project_frames` interleaved stereo project-rate samples,
// converts them to the requested output rate when needed and hands them to a
// WAV or encoded writer. Like those writers, an unfinished instance removes
// its private temporary file and never changes the requested target.
class MasterOutput {
public:
  // Called with each output-rate span after it has been written. Returning
  // false aborts the render with resource_exhausted.
  using Observer = std::function<bool(std::span<const float>)>;

  [[nodiscard]] static foundation::Result<MasterOutput> begin(const MasterOutputOptions& options,
                                                              std::uint32_t project_rate,
                                                              std::uint64_t project_frames,
                                                              Observer observer = {});

  MasterOutput(const MasterOutput&) = delete;
  MasterOutput& operator=(const MasterOutput&) = delete;
  MasterOutput(MasterOutput&& other) noexcept;
  MasterOutput& operator=(MasterOutput&& other) = delete;
  ~MasterOutput();

  [[nodiscard]] std::uint32_t output_rate() const noexcept;
  [[nodiscard]] std::uint64_t output_frames() const noexcept;

  [[nodiscard]] foundation::Result<void> write_interleaved_stereo(std::span<const float> samples);
  // Ends the output at `project_frames`: more than written so far (the last
  // write still ends the conversion), no more than begun with.
  [[nodiscard]] foundation::Result<void> shorten(std::uint64_t project_frames);
  [[nodiscard]] foundation::Result<PublishOutcome> finish();

private:
  struct State;
  explicit MasterOutput(std::unique_ptr<State> state) noexcept;

  std::unique_ptr<State> state_;
};

} // namespace oscillate::exporting::detail
