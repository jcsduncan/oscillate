#pragma once

#include "oscillate/clay_dsp/take.hpp"
#include "oscillate/foundation/error.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace oscillate::clay_dsp {

// Notes fade in and out over this time, centred on their edges, so that
// neighbouring notes cross-fade with complementary gains.
inline constexpr double note_fade_seconds = 0.008;
// Grain length for moving residuals and segments (Hann, 50 % overlap).
inline constexpr double grain_seconds = 0.024;

// Renders a take with edits: output = original - (each edited note as it
// was) + (each edited note as edited), so unedited audio is copied
// untouched. Construction does all the planning; render() is a pure
// function of the frame range, so any block split gives the same samples.
// Offline and worker-side: never on the audio thread.
class Renderer {
public:
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  Renderer(Renderer&&) noexcept;
  Renderer& operator=(Renderer&&) noexcept;
  ~Renderer();

  // The audio must be the analysed audio. `right` may be empty for mono, in
  // which case render() writes only `left`.
  [[nodiscard]] static foundation::Result<Renderer> create(const Take& take,
                                                           std::span<const float> left,
                                                           std::span<const float> right,
                                                           std::span<const NoteEdit> edits);

  // Frames [first, first + left.size()); frames outside the clip read as
  // silence plus whatever edited notes were moved there.
  void render(std::int64_t first, std::span<float> left, std::span<float> right) const;

  // The span edits can reach: the clip plus notes moved past its end.
  [[nodiscard]] std::int64_t length() const noexcept;

private:
  struct State;
  explicit Renderer(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

} // namespace oscillate::clay_dsp
