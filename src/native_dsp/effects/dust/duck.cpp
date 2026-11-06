#include "effects/dust/duck.hpp"

#include <cmath>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::dust::detail {

void Duck::prepare(double rate) {
  rate_ = rate;
  attack_frames_ = static_cast<std::int64_t>(std::lround(duck_attack_seconds * rate));
  sidechain_attack_ = 1.0 - std::exp(-duck_attack_taus / (duck_attack_seconds * rate));
}

void Duck::reset() noexcept {
  key_envelope_ = 0.0;
  attack_left_ = 0;
  attack_step_ = 0.0;
  sidechain_envelope_ = 0.0;
  cursor_dirty_ = true;
}

void Duck::set(std::span<const double> v, double steps, bool jump) noexcept {
  amount_.to(v[p::duck_amount], steps, jump);
  const double release_s = v[p::duck_release] * seconds_per_ms / (duck_release_decades * ln10);
  release_ = std::exp(-1.0 / (release_s * rate_));
  keyed_by_notes_ = static_cast<int>(v[p::duck_source]) == t::key_notes;
}

} // namespace oscillate::native_dsp::effects::dust::detail
