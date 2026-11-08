#include "oscillate/native_dsp/synth/modal_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace oscillate::native_dsp::synth::modal {
namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;
// Modes are held just below Nyquist when retuned upward.
constexpr double max_retuned_fraction = 0.49;

// (1/q) B(1/q, 1/2), q = p + 1: the contact integral of F = K x^p,
// so that a rigid contact lasts 2 (x_max / v) times this.
[[nodiscard]] double contact_integral(double exponent) noexcept {
  const double a = 1.0 / (exponent + 1.0);
  const double beta = std::tgamma(a) * std::tgamma(0.5) / std::tgamma(a + 0.5);
  return a * beta;
}

} // namespace

void Striker::launch(double surface, double impact_speed) noexcept {
  position = surface;
  speed = impact_speed;
  law.reset();
  active = true;
  contact_seconds = 0.0;
  peak_force = 0.0;
}

double Striker::push(double surface, double dt) noexcept {
  if (!active) {
    return 0.0;
  }
  const double x = position - surface;
  const double force = law.force(x, dt);
  speed -= force / mass * dt;
  position += speed * dt;
  peak_force = std::max(peak_force, force);
  const bool touching = x > 0.0;
  if (touching) {
    contact_seconds += dt;
  }
  // Free of the surface and moving away: the blow is over.
  if ((!touching && speed < 0.0) || contact_seconds > max_contact_seconds) {
    active = false;
  }
  return force;
}

double rigid_contact_stiffness(RigidStrike strike, double seconds) noexcept {
  const double mass = strike.mass;
  const double exponent = strike.exponent;
  const double speed = strike.speed;
  const double q = exponent + 1.0;
  const double x_max = seconds * speed / (2.0 * contact_integral(exponent));
  return q * mass * speed * speed / (2.0 * std::pow(x_max, q));
}

double rigid_contact_seconds(RigidStrike strike, double stiffness) noexcept {
  const double mass = strike.mass;
  const double exponent = strike.exponent;
  const double speed = strike.speed;
  const double q = exponent + 1.0;
  const double x_max = std::pow(q * mass * speed * speed / (2.0 * stiffness), 1.0 / q);
  return 2.0 * x_max / speed * contact_integral(exponent);
}

// ---- ModalBank -----------------------------------------------------------------

void ModalBank::prepare(std::size_t capacity, double sample_rate, int substeps) {
  rate_ = sample_rate;
  dt_ = 1.0 / sample_rate;
  substeps_ = std::max(1, substeps);
  for (auto* v : {&q_, &p_, &c_, &s_, &cs_, &ss_, &phi_, &h_, &h1_, &k_, &g_, &gl_, &gr_}) {
    v->assign(capacity, 0.0F);
  }
  for (auto* v : {&omega_, &sigma_, &mass_, &r_, &r_sub_}) {
    v->assign(capacity, 0.0);
  }
  clear();
}

void ModalBank::clear() noexcept {
  std::ranges::fill(q_, 0.0F);
  std::ranges::fill(p_, 0.0F);
  count_ = 0;
  factor_ = 1.0;
}

void ModalBank::rotate(std::size_t i, double omega) noexcept {
  const double dt_sub = dt_ / static_cast<double>(substeps_);
  const double r = r_[i];
  const double r_sub = r_sub_[i];
  c_[i] = static_cast<float>(r * std::cos(omega * dt_));
  s_[i] = static_cast<float>(r * std::sin(omega * dt_));
  cs_[i] = static_cast<float>(r_sub * std::cos(omega * dt_sub));
  ss_[i] = static_cast<float>(r_sub * std::sin(omega * dt_sub));
  // A force F for dt changes the velocity by shape F dt / mass; p is
  // velocity / omega, at the designed omega so that a retuned (bent) mode
  // keeps the level it was designed for.
  const double push = static_cast<double>(phi_[i]) / (mass_[i] * omega_[i]);
  h_[i] = static_cast<float>(push * dt_sub);
  h1_[i] = static_cast<float>(push * dt_);
}

bool ModalBank::add(const ModeDesign& mode) noexcept {
  if (count_ >= q_.size() || !(mode.hz > 0.0) || !(mode.hz < 0.5 * rate_) || !(mode.t60 > 0.0) ||
      !(mode.mass > 0.0)) {
    return false;
  }
  const std::size_t i = count_++;
  omega_[i] = two_pi * mode.hz;
  sigma_[i] = ln1000 / mode.t60;
  mass_[i] = mode.mass;
  phi_[i] = static_cast<float>(mode.shape);
  g_[i] = static_cast<float>(mode.gain);
  gl_[i] = g_[i];
  gr_[i] = g_[i];
  // A steady input u cos(omega t) settles at |q| = k u / (2 (1 - r)): unity
  // output at resonance times `drive`.
  const double r = std::exp(-sigma_[i] * dt_);
  r_[i] = r;
  r_sub_[i] = std::exp(-sigma_[i] * dt_ / static_cast<double>(substeps_));
  k_[i] = mode.gain != 0.0 ? static_cast<float>(2.0 * (1.0 - r) * mode.drive / mode.gain) : 0.0F;
  q_[i] = 0.0F;
  p_[i] = 0.0F;
  rotate(i, omega_[i] * factor_);
  return true;
}

void ModalBank::retune(double factor) noexcept {
  factor_ = factor;
  const double ceiling = two_pi * max_retuned_fraction * rate_;
  for (std::size_t i = 0; i < count_; ++i) {
    rotate(i, std::min(omega_[i] * factor, ceiling));
  }
}

float ModalBank::step(double force, double drive) noexcept {
  const auto f = static_cast<float>(force);
  const auto u = static_cast<float>(drive);
  float out = 0.0F;
  for (std::size_t i = 0; i < count_; ++i) {
    const float q = q_[i];
    const float p = p_[i];
    const float qn = c_[i] * q + s_[i] * p;
    q_[i] = qn;
    p_[i] = -s_[i] * q + c_[i] * p + h1_[i] * f + k_[i] * u;
    out += g_[i] * qn;
  }
  return out;
}

float ModalBank::step_contact(Striker& striker, double force, double drive) noexcept {
  const double dt_sub = dt_ / static_cast<double>(substeps_);
  const auto u = static_cast<float>(drive / static_cast<double>(substeps_));
  for (int sub = 0; sub < substeps_; ++sub) {
    double total = force;
    if (striker.active) {
      total += striker.push(displacement(), dt_sub);
    }
    const auto f = static_cast<float>(total);
    for (std::size_t i = 0; i < count_; ++i) {
      const float q = q_[i];
      const float p = p_[i];
      q_[i] = cs_[i] * q + ss_[i] * p;
      p_[i] = -ss_[i] * q + cs_[i] * p + h_[i] * f + k_[i] * u;
    }
  }
  return output();
}

float ModalBank::output() const noexcept {
  float out = 0.0F;
  for (std::size_t i = 0; i < count_; ++i) {
    out += g_[i] * q_[i];
  }
  return out;
}

void ModalBank::place(std::size_t mode, double left, double right) noexcept {
  if (mode >= count_) {
    return;
  }
  gl_[mode] = static_cast<float>(static_cast<double>(g_[mode]) * left);
  gr_[mode] = static_cast<float>(static_cast<double>(g_[mode]) * right);
}

void ModalBank::stereo_output(float& left, float& right) const noexcept {
  float l = 0.0F;
  float r = 0.0F;
  for (std::size_t i = 0; i < count_; ++i) {
    l += gl_[i] * q_[i];
    r += gr_[i] * q_[i];
  }
  left = l;
  right = r;
}

double ModalBank::displacement() const noexcept {
  double y = 0.0;
  for (std::size_t i = 0; i < count_; ++i) {
    y += static_cast<double>(phi_[i]) * static_cast<double>(q_[i]);
  }
  return y;
}

double ModalBank::energy() const noexcept {
  double energy = 0.0;
  for (std::size_t i = 0; i < count_; ++i) {
    const double q = q_[i];
    const double p = p_[i];
    const double omega = omega_[i] * factor_;
    energy += 0.5 * mass_[i] * omega * omega * (q * q + p * p);
  }
  return energy;
}

double ModalBank::amplitude() const noexcept {
  double total = 0.0;
  for (std::size_t i = 0; i < count_; ++i) {
    const double q = q_[i];
    const double p = p_[i];
    total += std::fabs(static_cast<double>(g_[i])) * std::sqrt(q * q + p * p);
  }
  return total;
}

void ModalBank::silence() noexcept {
  std::fill_n(q_.begin(), count_, 0.0F);
  std::fill_n(p_.begin(), count_, 0.0F);
}

bool ModalBank::retire_below(double threshold) noexcept {
  if (amplitude() < threshold) {
    silence();
    return true;
  }
  return false;
}

double ModalBank::hz(std::size_t mode) const noexcept { return omega_[mode] / two_pi; }

double ModalBank::t60(std::size_t mode) const noexcept { return ln1000 / sigma_[mode]; }

} // namespace oscillate::native_dsp::synth::modal
