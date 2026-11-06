#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::automation {

// Curve coordinates are absolute project ticks or project samples, never clip-local.
enum class Timebase : std::uint8_t { musical, elapsed };
enum class Shape : std::uint8_t { hold, linear, quadratic, exponential, ease_in_out, bezier };

struct BezierHandles {
  // Normalized control points relative to the two endpoints. X is monotone.
  double x1{1.0 / 3.0};
  double y1{1.0 / 3.0};
  double x2{2.0 / 3.0};
  double y2{2.0 / 3.0};
};

struct Point {
  std::int64_t coordinate{};
  double value{}; // Physical parameter domain, not a normalized value.
  Shape to_next{Shape::hold};
  BezierHandles handles{};
};

struct Curve {
  Timebase timebase{Timebase::musical};
  std::vector<Point> points;
};

// The space values blend in between points: a segment's shape runs along
// a straight line there. Decibel blends gains in dB, silence at the floor;
// logarithmic needs a positive minimum.
enum class Warp : std::uint8_t { linear, logarithmic, decibel };
inline constexpr double decibel_floor = -70.0;

struct ParameterDescriptor {
  double minimum{};
  double maximum{1.0};
  double default_value{};
  bool discrete{};
  Warp warp{Warp::linear};
};

// A segment shape's progress (0..1) at `u` (0..1) of the way along it.
[[nodiscard]] double shape_progress(Shape shape, double u, const BezierHandles& handles) noexcept;
// `from` to `to` at `progress` (0..1) along a straight line in `warp`'s space.
[[nodiscard]] double blend(Warp warp, double from, double to, double progress) noexcept;

struct ClipEnvelope {
  Curve curve;
  timeline::SamplePosition active_begin{};
  timeline::SamplePosition active_end{}; // Exclusive, supplied by clip placement.
};

// Construction and validation are control-thread work. The immutable instance
// and its TempoMap must be published/reclaimed using the engine graph protocol.
class PreparedAutomation {
public:
  [[nodiscard]] static foundation::Result<PreparedAutomation>
  create(ParameterDescriptor descriptor, std::optional<Curve> track,
         std::optional<ClipEnvelope> clip, std::shared_ptr<const timeline::TempoMap> tempo,
         std::uint32_t sample_rate);

  [[nodiscard]] std::uint64_t tempo_revision() const noexcept { return tempo_->revision(); }
  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }

  // For a valid nonnegative block, writes one physical-domain value per sample.
  // The three-argument form shares an authoritative tick block among many lanes:
  // caller must use fill_musical_ticks on this same prepared revision and sample
  // interval before publication to the audio thread. No allocation/locks/I/O.
  [[nodiscard]] bool fill_musical_ticks(timeline::SamplePosition first,
                                        std::span<timeline::TickPosition> output) const;
  [[nodiscard]] bool evaluate_block(timeline::SamplePosition first,
                                    std::span<const timeline::TickPosition> ticks,
                                    std::span<double> output) const;

  // Convenience path for offline/single-lane use. Converts through TempoMap
  // inside the per-sample loop; multi-lane live graphs should use shared ticks.
  // Returns false on invalid/overflowing coordinates or failed conversion.
  [[nodiscard]] bool evaluate_block(timeline::SamplePosition first, std::span<double> output) const;

private:
  PreparedAutomation(ParameterDescriptor descriptor, std::optional<Curve> track,
                     std::optional<ClipEnvelope> clip,
                     std::shared_ptr<const timeline::TempoMap> tempo, std::uint32_t sample_rate)
      : descriptor_(descriptor), track_(std::move(track)), clip_(std::move(clip)),
        tempo_(std::move(tempo)), sample_rate_(sample_rate) {}

  [[nodiscard]] double evaluate_curve(const Curve& curve, std::int64_t coordinate) const;

  ParameterDescriptor descriptor_;
  std::optional<Curve> track_;
  std::optional<ClipEnvelope> clip_;
  std::shared_ptr<const timeline::TempoMap> tempo_;
  std::uint32_t sample_rate_;
};

} // namespace oscillate::automation
