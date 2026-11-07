#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Modal building blocks for the physical models (ADR-0068): the Stulov
// contact law Knock's strikers use, a mass that strikes through it, a
// two-pole resonator for body modes, and a bank of damped modes in rotation
// form.
namespace oscillate::native_dsp::synth::modal {

inline constexpr double ln1000 = 6.907755278982137; // T60 = ln(1000) / sigma.
inline constexpr int default_substeps = 8;          // Contact sub-steps per sample.
// A strike that has not let go after this long is ended: real contacts
// last a few milliseconds.
inline constexpr double max_contact_seconds = 0.05;

// The Stulov law, F = K (x^p + alpha d(x^p)/dt) for compression x > 0, with
// its own state, mass-free.
struct ContactLaw {
  double k = 1.0;       // Stiffness, N / m^p.
  double p = 1.5;       // Exponent (1.5 is Hertz; felt is 2.2-3.5).
  double alpha = 0.0;   // Hysteresis (s).
  double last_xp = 0.0; // The previous step's x^p.

  void reset() noexcept { last_xp = 0.0; }
  // The force at compression x after a step of dt seconds, the stiffness
  // scaled by `share` (a hammer pressing several strings at once). A felt
  // never pulls.
  [[nodiscard]] double force(double x, double dt, double share = 1.0) noexcept {
    const double xp = x > 0.0 ? std::pow(x, p) : 0.0;
    const double rate = (xp - last_xp) / dt;
    last_xp = xp;
    return std::max(0.0, share * k * (xp + alpha * rate));
  }
};

// A mass thrown at a surface through a contact law. Positions are metres,
// positive toward the surface; the surface's displacement is positive away
// from the striker.
struct Striker {
  double mass = 0.01;
  ContactLaw law{};
  double position = 0.0;
  double speed = 0.0;
  bool active = false;
  double contact_seconds = 0.0;
  double peak_force = 0.0;

  // Starts a blow at the surface, moving toward it at impact_speed m/s.
  void launch(double surface, double impact_speed) noexcept;
  // One step of dt seconds against the surface's displacement: returns the
  // force on the surface and moves the striker. It ends once it is free of
  // the surface and moving away, or after max_contact_seconds.
  [[nodiscard]] double push(double surface, double dt) noexcept;
};

// The stiffness K for which a mass at `speed` against a rigid surface,
// F = K x^p, stays in contact for `seconds`; and the inverse. For p = 1 the
// contact is half a period of the mass on the spring.
struct RigidStrike {
  double mass = 0.0;     // kg.
  double exponent = 1.0; // The contact law's p.
  double speed = 0.0;    // m/s at impact.
};
[[nodiscard]] double rigid_contact_stiffness(RigidStrike strike, double seconds) noexcept;
[[nodiscard]] double rigid_contact_seconds(RigidStrike strike, double stiffness) noexcept;

// One mode of a bank as designed.
struct ModeDesign {
  double hz = 0.0;
  double t60 = 1.0;   // Seconds to fall 60 dB.
  double shape = 1.0; // The mode shape at the strike point (force in, displacement out).
  double mass = 1.0;  // Modal mass (kg).
  double gain = 1.0;  // Output per metre of modal displacement.
  // Driven input: the output's amplitude at resonance per unit amplitude of
  // a steady input at the mode's frequency (0 for struck-only modes).
  double drive = 0.0;
};

// Damped modes in rotation form, structure of arrays, single precision
// state. Each mode keeps (q, p = velocity / omega); a sample rotates the
// pair by omega dt and scales it by exp(-sigma dt), so its frequency is
// exact at any rate. Capacity is fixed in prepare(); nothing else
// allocates.
class ModalBank {
public:
  void prepare(std::size_t capacity, double sample_rate, int substeps = default_substeps);
  // No modes, no motion.
  void clear() noexcept;
  // False, and nothing added, when the bank is full or the mode is not
  // below Nyquist with a positive T60 and mass.
  bool add(const ModeDesign& mode) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return q_.size(); }

  // Every mode to `factor` times its designed frequency (held below
  // Nyquist), keeping its motion, decay rate and force coupling, so a bent
  // mode keeps its level.
  void retune(double factor) noexcept;
  [[nodiscard]] double factor() const noexcept { return factor_; }

  // One sample: `force` (N) at the strike point held over the sample, plus
  // the driven input. Returns the output, the sum of gain times
  // displacement.
  [[nodiscard]] float step(double force, double drive) noexcept;
  // One sample in sub-steps while the striker is in contact: each sub-step
  // adds the striker's force against the displacement at the strike point
  // to `force`.
  [[nodiscard]] float step_contact(Striker& striker, double force, double drive) noexcept;

  [[nodiscard]] float output() const noexcept;
  // Stereo: a mode's share of the output in each ear, (1, 1) when added.
  // Out-of-range modes are ignored.
  void place(std::size_t mode, double left, double right) noexcept;
  // The output in each ear: output() with each mode weighted by its share.
  void stereo_output(float& left, float& right) const noexcept;
  // Displacement at the strike point (m).
  [[nodiscard]] double displacement() const noexcept;
  // Kinetic plus potential energy (J).
  [[nodiscard]] double energy() const noexcept;
  // An upper bound on the output's amplitude from the current motion.
  [[nodiscard]] double amplitude() const noexcept;
  // Stops all motion, keeping the modes.
  void silence() noexcept;
  // Silences the bank once amplitude() is below threshold; true when silent.
  bool retire_below(double threshold) noexcept;

  // The designed values.
  [[nodiscard]] double hz(std::size_t mode) const noexcept;
  [[nodiscard]] double t60(std::size_t mode) const noexcept;

private:
  void rotate(std::size_t mode, double omega) noexcept;

  double rate_ = 48'000.0;
  double dt_ = 1.0 / 48'000.0;
  int substeps_ = default_substeps;
  std::size_t count_ = 0;
  double factor_ = 1.0;
  // Per mode: state, the per-sample and per-sub-step rotation with decay,
  // the strike shape, the force-to-p coefficients per sub-step (h) and per
  // sample (h1), the driven input's coefficient (k), the output gain and
  // that gain times the mode's share of each ear (gl, gr).
  std::vector<float> q_, p_, c_, s_, cs_, ss_, phi_, h_, h1_, k_, g_, gl_, gr_;
  // Designed omega, decay rate and mass; the per-sample and per-sub-step
  // decay factors.
  std::vector<double> omega_, sigma_, mass_, r_, r_sub_;
};

} // namespace oscillate::native_dsp::synth::modal
