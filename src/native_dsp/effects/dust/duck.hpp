#pragma once

#include "effects/dust/parts.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// Dust's duck (ADR-0069): the bed dips under a sidechain or the notes of a
// keying track, so it breathes with the kick. The input is never ducked.
namespace oscillate::native_dsp::effects::dust::detail {

// The duck reaches its depth over this long: from a sidechain (heard
// latency_seconds ahead, as the output is compensated) or from a key note
// (started this long before the note, so the bed is down on the hit).
inline constexpr double duck_attack_seconds = 0.003;
// A sidechain's attack settles over this many time constants of it.
inline constexpr double duck_attack_taus = 3.0;
// Release is the time to recover nine tenths.
inline constexpr double duck_release_decades = 1.0;
inline constexpr double seconds_per_ms = 1e-3;
inline constexpr double velocity_full = 127.0;

class Duck {
public:
  // Control only.
  void prepare(double rate);
  // The keying track's note-ons, sorted by frame.
  void adopt(std::span<const KeyNote> notes) noexcept {
    notes_ = notes;
    cursor_dirty_ = true;
  }
  // The transport jumped: find the next note again.
  void moved() noexcept { cursor_dirty_ = true; }
  void reset() noexcept;
  void set(std::span<const double> v, double steps, bool jump) noexcept;

  // `clock` is the bed's frame: the project frame less the latency.
  void seek(std::int64_t clock) noexcept {
    if (cursor_dirty_) {
      const std::int64_t look = clock + attack_frames_;
      cursor_ = static_cast<std::size_t>(
          std::ranges::lower_bound(notes_, look, {}, &KeyNote::frame) - notes_.begin());
      cursor_dirty_ = false;
    }
  }

  // One frame: the bed's gain (1 for none). `clock` as for seek; `key` is
  // the sidechain's frame, null without one.
  [[nodiscard]] double next(std::int64_t clock, const engine::StereoFrame* key) noexcept {
    const std::int64_t look = clock + attack_frames_;
    while (cursor_ < notes_.size() && notes_[cursor_].frame <= look) {
      if (keyed_by_notes_) {
        const double peak =
            std::max(key_envelope_, static_cast<double>(notes_[cursor_].velocity) / velocity_full);
        attack_left_ = attack_frames_;
        attack_step_ =
            (peak - key_envelope_) / static_cast<double>(std::max<std::int64_t>(1, attack_frames_));
      }
      ++cursor_;
    }
    if (attack_left_ > 0) {
      key_envelope_ += attack_step_;
      --attack_left_;
    } else {
      key_envelope_ *= release_;
    }
    double level = 0.0;
    if (keyed_by_notes_) {
      level = key_envelope_;
    } else if (key != nullptr) {
      const double e = std::max(std::fabs(static_cast<double>(key->left)),
                                std::fabs(static_cast<double>(key->right)));
      sidechain_envelope_ =
          e > sidechain_envelope_
              ? sidechain_envelope_ + sidechain_attack_ * (e - sidechain_envelope_)
              : e + (sidechain_envelope_ - e) * release_;
      const double db = db_per_decade * std::log10(sidechain_envelope_ + log_floor);
      level = std::clamp((db - duck_floor_dbfs) / (duck_full_dbfs - duck_floor_dbfs), 0.0, 1.0);
    } else {
      sidechain_envelope_ *= release_;
    }
    return 1.0 - std::clamp(amount_.next() * level, 0.0, 1.0);
  }

private:
  double rate_ = nominal_rate;
  std::span<const KeyNote> notes_;
  std::size_t cursor_ = 0;
  bool cursor_dirty_ = true;
  bool keyed_by_notes_ = false;
  std::int64_t attack_frames_ = 1;
  std::int64_t attack_left_ = 0;
  double attack_step_ = 0.0;
  double key_envelope_ = 0.0;
  double sidechain_envelope_ = 0.0;
  double sidechain_attack_ = 1.0;
  double release_ = 0.0;
  dsp::Ramp amount_;
};

} // namespace oscillate::native_dsp::effects::dust::detail
