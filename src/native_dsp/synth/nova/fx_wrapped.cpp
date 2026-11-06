#include "fx_modules.hpp"

#include "oscillate/native_dsp/effects/expanse.hpp"
#include "oscillate/native_dsp/effects/flyby.hpp"
#include "oscillate/native_dsp/effects/orbit.hpp"
#include "oscillate/native_dsp/effects/relay.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Nova modules that run the native effect kernels (NOVA-FX-05): Chorus and
// Phaser on Orbit, Delay on Relay, Reverb on Expanse. Each maps the
// contract's p0..p7 (08a §5) onto its kernel's table and keeps the kernel
// wet-only (its mix at 100%); the rack applies mix and bypass.
namespace oscillate::native_dsp::synth::nova::fx_detail {

// ---- KernelDriver -------------------------------------------------------------------

bool KernelDriver::init(const effects::EffectEngine& engine, double base_rate,
                        std::size_t max_frames) {
  if (engine.descriptor == nullptr || engine.create_kernel == nullptr ||
      engine.tail_frames == nullptr) {
    return false;
  }
  engine_ = &engine;
  base_rate_ = base_rate;
  values_ = synth::default_values(*engine.descriptor);
  kernel_ = engine.create_kernel(values_, base_rate);
  scratch_.assign(std::max<std::size_t>(1, max_frames), engine::StereoFrame{0.0F, 0.0F});
  resampler_.set_factor(1);
  return kernel_ != nullptr && ok_;
}

std::size_t KernelDriver::index(std::string_view id) {
  const auto found = synth::find_parameter(*engine_->descriptor, id);
  if (!found) {
    ok_ = false;
    return std::numeric_limits<std::size_t>::max();
  }
  return *found;
}

void KernelDriver::control(std::uint32_t base_frames, double beats_per_second) noexcept {
  const auto& parameters = engine_->descriptor->parameters;
  for (std::size_t i = 0; i < values_.size(); ++i) {
    values_[i] = synth::sanitise(parameters[i], values_[i]);
  }
  transport_.beats_per_frame = std::isfinite(beats_per_second) && beats_per_second > 0.0
                                   ? beats_per_second / base_rate_
                                   : 2.0 / base_rate_;
  issue(base_frames);
}

void KernelDriver::issue(std::uint32_t frames) noexcept {
  // The kernels' ramps keep their step until the next control, so the
  // driver re-aims them exactly when the frames they were given run out: a
  // jump (0 frames) is one frame, then a hold.
  const std::uint32_t steps = std::max<std::uint32_t>(1, frames);
  // The beat moves on by the frames run since the last tick, at the tempo:
  // Nova's rack knows the tempo, not the song's position, so synced
  // kernels keep time with it from where they started.
  transport_.beat_at_block_start += transport_.beats_per_frame * (issued_ - remaining_);
  transport_.bar_at_block_start = transport_.beat_at_block_start / transport_.beats_per_bar;
  kernel_->control(values_, steps, transport_);
  remaining_ = steps;
  issued_ = steps;
  primed_ = true;
}

void KernelDriver::set_factor(std::uint32_t factor) noexcept {
  if (factor != resampler_.factor()) {
    resampler_.set_factor(factor);
  }
}

void KernelDriver::run(Frames base) noexcept {
  if (!primed_) {
    issue(0);
  }
  std::size_t done = 0;
  while (done < base.size()) {
    if (remaining_ == 0) {
      // The tick's frames ran out: hold every ramp on its target.
      issue(hold_frames);
    }
    const std::size_t count = std::min<std::size_t>(remaining_, base.size() - done);
    kernel_->process(base.subspan(done, count), {});
    remaining_ -= static_cast<std::uint32_t>(count);
    done += count;
  }
}

void KernelDriver::process(Frames io) noexcept {
  const std::uint32_t factor = resampler_.factor();
  if (factor == 1U) {
    run(io);
    return;
  }
  const std::size_t n = std::min(io.size() / factor, scratch_.size());
  resampler_.down(io.data(), n, scratch_.data());
  run(Frames(scratch_.data(), n));
  resampler_.up(scratch_.data(), n, io.data());
  // The rack hands whole base frames; anything past them is silenced.
  std::fill(io.begin() + static_cast<std::ptrdiff_t>(n * factor), io.end(),
            engine::StereoFrame{0.0F, 0.0F});
}

void KernelDriver::reset() noexcept {
  kernel_->reset();
  transport_.beat_at_block_start = 0.0;
  transport_.bar_at_block_start = 0.0;
  issued_ = remaining_;
  resampler_.reset();
}

namespace {

// Base-rate frames for a tick of `frames` at the chain's rate.
[[nodiscard]] std::uint32_t base_frames(std::uint32_t frames, std::uint32_t factor) noexcept {
  return frames == 0 ? 0U : std::max<std::uint32_t>(1, frames / factor);
}

// ---- Chorus and Phaser (Orbit) ----------------------------------------------------

class OrbitModule final : public FxModule {
public:
  OrbitModule(FxKind kind, double base_rate, std::size_t max_frames) : kind_(kind) {
    ok_ = driver_.init(effects::orbit::engine(), base_rate, max_frames);
    if (!ok_) {
      return;
    }
    mode_ = driver_.index("orbit.mode");
    rate_ = driver_.index("orbit.rate");
    sync_ = driver_.index("orbit.sync");
    shape_ = driver_.index("orbit.shape");
    depth_ = driver_.index("orbit.depth");
    stereo_ = driver_.index("orbit.stereo");
    time_ = driver_.index("orbit.time");
    feedback_ = driver_.index("orbit.feedback");
    voices_ = driver_.index("orbit.voices");
    stages_ = driver_.index("orbit.stages");
    centre_ = driver_.index("orbit.centre");
    tone_ = driver_.index("orbit.tone");
    mix_ = driver_.index("orbit.mix");
    output_ = driver_.index("orbit.output");
    ok_ = driver_.ok();
    map(fx_defaults(kind).p, {2.0, base_rate});
  }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] FxKind kind() const noexcept override { return kind_; }
  void reset() noexcept override {
    driver_.reset();
    phase_ = 0.0;
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    map(p, context);
    driver_.control(base_frames(frames, driver_.factor()), context.beats_per_second);
  }

private:
  // The contract's p values onto the kernel's table, at the chain's rate.
  void map(const FxParams& p, const FxContext& context) noexcept {
    const std::uint32_t factor = factor_for(context.sample_rate, driver_.base_rate());
    driver_.set_factor(factor);
    hz_ = log_map(p[0], 0.01, 10.0);
    const bool chorus = kind_ == FxKind::chorus;
    // Orbit's modes: 0 chorus, 4 phaser. Voice choices "1".."4"; stage
    // choices 2, 4, 6, 8, 12.
    driver_.value(mode_, chorus ? 0.0 : 4.0);
    driver_.value(rate_, hz_);
    driver_.value(sync_, 0.0);
    driver_.value(shape_, 0.0);
    driver_.value(depth_, clamp01(p[1]));
    driver_.value(stereo_, chorus ? 90.0 : 75.0);
    driver_.value(feedback_, 0.95 * clamp01(p[2]));
    if (chorus) {
      driver_.value(time_, lerp(p[3], 1.0, 30.0));
      driver_.value(voices_, choice(p[7], 2) == 0 ? 1.0 : 3.0);
      // A gentle bucket-brigade darkening: warm, not dull.
      driver_.value(tone_, -1.5);
    } else {
      constexpr std::array<double, 3> stage_index{1.0, 3.0, 4.0};
      driver_.value(centre_, log_map(p[3], 100.0, 8'000.0));
      driver_.value(stages_, stage_index[choice(p[7], 3)]);
      driver_.value(tone_, 0.0);
    }
    driver_.value(mix_, 1.0);
    driver_.value(output_, 0.0);
  }

public:
  void process(Frames io) noexcept override {
    driver_.process(io);
    const double base = static_cast<double>(io.size() / driver_.factor());
    phase_ += base * hz_ / driver_.base_rate();
    phase_ -= std::floor(phase_);
  }
  [[nodiscard]] float telemetry() const noexcept override { return static_cast<float>(phase_); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return driver_.tail_frames() * driver_.factor();
  }

private:
  FxKind kind_;
  KernelDriver driver_;
  bool ok_ = false;
  std::size_t mode_ = 0, rate_ = 0, sync_ = 0, shape_ = 0, depth_ = 0, stereo_ = 0, time_ = 0,
              feedback_ = 0, voices_ = 0, stages_ = 0, centre_ = 0, tone_ = 0, mix_ = 0,
              output_ = 0;
  double hz_ = 1.0;
  double phase_ = 0.0;
};

// ---- Delay (Relay) ------------------------------------------------------------------

// The contract's divisions (08a §5) in quarter-note beats.
constexpr std::array<double, 15> delay_beats{
    1.0 / 16.0, 1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 3.0, 1.0 / 2.0,
    3.0 / 4.0,  2.0 / 3.0,  1.0,       3.0 / 2.0, 2.0,       3.0,       4.0};

class DelayModule final : public FxModule {
public:
  DelayModule(double base_rate, std::size_t max_frames) {
    ok_ = driver_.init(effects::relay::engine(), base_rate, max_frames);
    if (!ok_) {
      return;
    }
    character_ = driver_.index("relay.character");
    routing_ = driver_.index("relay.routing");
    time_ = driver_.index("relay.time");
    sync_ = driver_.index("relay.sync");
    offset_ = driver_.index("relay.offset");
    feedback_ = driver_.index("relay.feedback");
    low_cut_ = driver_.index("relay.low_cut");
    high_cut_ = driver_.index("relay.high_cut");
    drive_ = driver_.index("relay.drive");
    diffusion_ = driver_.index("relay.diffusion");
    mod_depth_ = driver_.index("relay.mod_depth");
    mod_rate_ = driver_.index("relay.mod_rate");
    ducking_ = driver_.index("relay.ducking");
    width_ = driver_.index("relay.width");
    mix_ = driver_.index("relay.mix");
    output_ = driver_.index("relay.output");
    ok_ = driver_.ok();
    map(fx_defaults(FxKind::delay).p, {2.0, base_rate});
  }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::delay; }
  void reset() noexcept override { driver_.reset(); }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    map(p, context);
    driver_.control(base_frames(frames, driver_.factor()), context.beats_per_second);
  }

private:
  // The contract's p values onto the kernel's table, at the chain's rate.
  void map(const FxParams& p, const FxContext& context) noexcept {
    const std::uint32_t factor = factor_for(context.sample_rate, driver_.base_rate());
    driver_.set_factor(factor);
    double ms = log_map(p[0], 1.0, 2'000.0);
    if (clamp01(p[5]) >= 0.5) {
      const double bps = std::isfinite(context.beats_per_second) && context.beats_per_second > 0.0
                             ? context.beats_per_second
                             : 2.0;
      ms = 1'000.0 * delay_beats[choice(p[0], delay_beats.size())] / bps;
    }
    ms = std::clamp(ms, 1.0, 4'000.0);
    // Feedback filter: -1 closes a low-pass to 1 kHz, +1 opens a high-pass
    // to 2 kHz; the centre leaves the repeats full range.
    const double tilt = 2.0 * clamp01(p[2]) - 1.0;
    const double high_cut = tilt < 0.0 ? 20'000.0 * std::pow(0.05, -tilt) : 20'000.0;
    const double low_cut = tilt > 0.0 ? 20.0 * std::pow(100.0, tilt) : 20.0;
    const std::size_t mode = choice(p[7], 3); // Stereo, Ping-pong, Tape.
    driver_.value(character_, mode == 2 ? 1.0 : 0.0);
    driver_.value(routing_, mode == 1 ? 1.0 : 0.0);
    driver_.value(time_, ms);
    driver_.value(sync_, 0.0);
    driver_.value(offset_, 0.0);
    driver_.value(feedback_, 1.1 * clamp01(p[1]));
    driver_.value(low_cut_, low_cut);
    driver_.value(high_cut_, high_cut);
    // Tape: wow and flutter plus loop saturation.
    driver_.value(drive_, mode == 2 ? 6.0 : 0.0);
    driver_.value(mod_depth_, mode == 2 ? 0.6 : 0.0);
    driver_.value(mod_rate_, 0.7);
    driver_.value(diffusion_, 0.0);
    driver_.value(ducking_, 0.0);
    driver_.value(width_, 1.0);
    driver_.value(mix_, 1.0);
    driver_.value(output_, 0.0);
  }

public:
  void process(Frames io) noexcept override { driver_.process(io); }
  [[nodiscard]] float telemetry() const noexcept override {
    std::array<float, 16> out{};
    return driver_.telemetry(out) > effects::relay::telemetry::time_left_ms
               ? out[effects::relay::telemetry::time_left_ms]
               : 0.0F;
  }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return driver_.tail_frames() * driver_.factor();
  }

private:
  KernelDriver driver_;
  bool ok_ = false;
  std::size_t character_ = 0, routing_ = 0, time_ = 0, sync_ = 0, offset_ = 0, feedback_ = 0,
              low_cut_ = 0, high_cut_ = 0, drive_ = 0, diffusion_ = 0, mod_depth_ = 0,
              mod_rate_ = 0, ducking_ = 0, width_ = 0, mix_ = 0, output_ = 0;
};

// ---- Flyby ------------------------------------------------------------------------

// The contract's preset paths (08a §5) at the closest distance d: straight
// past in front either way, an arc over the front, an orbit, or straight
// overhead from behind to in front. Straight paths reach four times d
// either side (30 m at least, the map's edge at most).
constexpr std::size_t flyby_paths = 5;
enum class FlybyPath : std::uint8_t { left_right, right_left, arc, orbit, overhead };
constexpr std::array<double, 3> flyby_runs{0.0, 2.0, 3.0}; // Loop, Sync, Position.
constexpr double flyby_reach_per_metre = 4.0;
constexpr double flyby_min_reach = 30.0;
constexpr double flyby_max_reach = 150.0;
constexpr double flyby_max_distance = 60.0;
constexpr double flyby_max_speed = 1'000.0;
constexpr double flyby_max_height = 150.0;

class FlybyModule final : public FxModule {
public:
  FlybyModule(double base_rate, std::size_t max_frames) {
    ok_ = driver_.init(effects::flyby::engine(), base_rate, max_frames);
    if (!ok_) {
      return;
    }
    path_ = driver_.index("flyby.path");
    for (std::size_t i = 0; i < 4; ++i) {
      x_[i] = driver_.index("flyby.x" + std::to_string(i + 1));
      y_[i] = driver_.index("flyby.y" + std::to_string(i + 1));
    }
    radius_ = driver_.index("flyby.radius");
    height_ = driver_.index("flyby.height");
    run_ = driver_.index("flyby.run");
    speed_ = driver_.index("flyby.speed");
    length_ = driver_.index("flyby.length");
    position_ = driver_.index("flyby.position");
    doppler_ = driver_.index("flyby.doppler");
    air_ = driver_.index("flyby.air");
    room_ = driver_.index("flyby.room");
    listen_ = driver_.index("flyby.listen");
    mix_ = driver_.index("flyby.mix");
    ok_ = driver_.ok();
    map(fx_defaults(FxKind::flyby).p, {2.0, base_rate});
  }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::flyby; }
  void reset() noexcept override { driver_.reset(); }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    map(p, context);
    driver_.control(base_frames(frames, driver_.factor()), context.beats_per_second);
  }
  void process(Frames io) noexcept override { driver_.process(io); }
  // 08a §6: the source's distance in metres.
  [[nodiscard]] float telemetry() const noexcept override {
    std::array<float, effects::flyby::telemetry::size> out{};
    return driver_.telemetry(out) > effects::flyby::telemetry::distance
               ? out[effects::flyby::telemetry::distance]
               : 0.0F;
  }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return driver_.tail_frames() * driver_.factor();
  }

private:
  void line(double x1, double y1, double x4, double y4) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
      const double k = static_cast<double>(i) / 3.0;
      driver_.value(x_[i], x1 + k * (x4 - x1));
      driver_.value(y_[i], y1 + k * (y4 - y1));
    }
  }
  void map(const FxParams& p, const FxContext& context) noexcept {
    driver_.set_factor(factor_for(context.sample_rate, driver_.base_rate()));
    const double d = log_map(p[1], 1.0, flyby_max_distance);
    const double reach = std::clamp(flyby_reach_per_metre * d, flyby_min_reach, flyby_max_reach);
    double height = flyby_max_height * clamp01(p[10]);
    driver_.value(path_, 0.0);
    switch (static_cast<FlybyPath>(choice(p[0], flyby_paths))) {
    case FlybyPath::left_right:
      line(-reach, d, reach, d);
      break;
    case FlybyPath::right_left:
      line(reach, d, -reach, d);
      break;
    case FlybyPath::arc: {
      // The ends a third of the reach behind; the handles lift the apex to d.
      const double back = -reach / 3.0;
      const double lift = (d - 0.25 * back) / 0.75;
      driver_.value(x_[0], -reach);
      driver_.value(y_[0], back);
      driver_.value(x_[1], -reach / 2.0);
      driver_.value(y_[1], std::min(lift, flyby_max_reach));
      driver_.value(x_[2], reach / 2.0);
      driver_.value(y_[2], std::min(lift, flyby_max_reach));
      driver_.value(x_[3], reach);
      driver_.value(y_[3], back);
      break;
    }
    case FlybyPath::orbit:
      driver_.value(path_, 1.0);
      driver_.value(radius_, d);
      break;
    case FlybyPath::overhead:
      line(0.0, -reach, 0.0, reach);
      height = d;
      break;
    }
    driver_.value(height_, height);
    driver_.value(run_, flyby_runs[choice(p[3], flyby_runs.size())]);
    driver_.value(speed_, log_map(p[2], 1.0, flyby_max_speed));
    driver_.value(length_, static_cast<double>(choice(p[4], effects::flyby::length_count)));
    driver_.value(position_, clamp01(p[5]));
    driver_.value(doppler_, 2.0 * clamp01(p[6]));
    driver_.value(listen_, static_cast<double>(choice(p[7], 2)));
    driver_.value(air_, 4.0 * clamp01(p[8]));
    driver_.value(room_, clamp01(p[9]));
    driver_.value(mix_, 1.0);
  }

  KernelDriver driver_;
  bool ok_ = false;
  std::size_t path_ = 0, radius_ = 0, height_ = 0, run_ = 0, speed_ = 0, length_ = 0, position_ = 0,
              doppler_ = 0, air_ = 0, room_ = 0, listen_ = 0, mix_ = 0;
  std::array<std::size_t, 4> x_{};
  std::array<std::size_t, 4> y_{};
};

// ---- Reverb (Expanse) ---------------------------------------------------------------

class ReverbModule final : public FxModule {
public:
  ReverbModule(double base_rate, std::size_t max_frames) {
    ok_ = driver_.init(effects::expanse::engine(), base_rate, max_frames);
    if (!ok_) {
      return;
    }
    mode_ = driver_.index("expanse.mode");
    size_ = driver_.index("expanse.size");
    decay_ = driver_.index("expanse.decay");
    predelay_ = driver_.index("expanse.predelay");
    high_decay_ = driver_.index("expanse.high_decay");
    high_freq_ = driver_.index("expanse.high_freq");
    low_cut_ = driver_.index("expanse.low_cut");
    shimmer_ = driver_.index("expanse.shimmer");
    shimmer_pitch_ = driver_.index("expanse.shimmer_pitch");
    freeze_ = driver_.index("expanse.freeze");
    mod_depth_ = driver_.index("expanse.mod_depth");
    mix_ = driver_.index("expanse.mix");
    output_ = driver_.index("expanse.output");
    ok_ = driver_.ok();
    meter_.prepare(base_rate);
    map(fx_defaults(FxKind::reverb).p, {2.0, base_rate});
  }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::reverb; }
  void reset() noexcept override {
    driver_.reset();
    meter_.reset();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    map(p, context);
    driver_.control(base_frames(frames, driver_.factor()), context.beats_per_second);
  }

private:
  // The contract's p values onto the kernel's table, at the chain's rate.
  void map(const FxParams& p, const FxContext& context) noexcept {
    const std::uint32_t factor = factor_for(context.sample_rate, driver_.base_rate());
    if (factor != driver_.factor()) {
      meter_.prepare(driver_.base_rate() * factor);
    }
    driver_.set_factor(factor);
    // Expanse spaces: 0 room, 1 hall, 2 plate. Shimmer is the hall with a
    // +12 semitone shifter inside the loop (interval choice 3).
    const std::size_t type = choice(p[7], 4); // Hall, Plate, Room, Shimmer.
    constexpr std::array<double, 4> space{1.0, 2.0, 0.0, 1.0};
    const double damp = clamp01(p[2]);
    driver_.value(mode_, space[type]);
    driver_.value(size_, clamp01(p[0]));
    driver_.value(decay_, log_map(p[1], 0.1, 30.0));
    driver_.value(predelay_, 250.0 * clamp01(p[3]));
    driver_.value(high_decay_, 1.5 * std::pow(0.1 / 1.5, damp));
    driver_.value(high_freq_, 12'000.0 * std::pow(1'500.0 / 12'000.0, damp));
    driver_.value(low_cut_, 60.0);
    driver_.value(shimmer_, type == 3 ? 0.6 : 0.0);
    driver_.value(shimmer_pitch_, 3.0);
    driver_.value(freeze_, 0.0);
    driver_.value(mod_depth_, type == 0 || type == 3 ? 0.45 : 0.35);
    driver_.value(mix_, 1.0);
    driver_.value(output_, 0.0);
  }

public:
  void process(Frames io) noexcept override {
    driver_.process(io);
    meter_.feed(io);
  }
  [[nodiscard]] float telemetry() const noexcept override { return meter_.db(); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return driver_.tail_frames() * driver_.factor();
  }

private:
  KernelDriver driver_;
  PeakMeter meter_;
  bool ok_ = false;
  std::size_t mode_ = 0, size_ = 0, decay_ = 0, predelay_ = 0, high_decay_ = 0, high_freq_ = 0,
              low_cut_ = 0, shimmer_ = 0, shimmer_pitch_ = 0, freeze_ = 0, mod_depth_ = 0, mix_ = 0,
              output_ = 0;
};

template <typename Module, typename... Args>
[[nodiscard]] std::unique_ptr<FxModule> checked(Args&&... args) {
  auto module = std::make_unique<Module>(std::forward<Args>(args)...);
  if (!module->ok()) {
    return nullptr;
  }
  return module;
}

} // namespace

std::unique_ptr<FxModule> make_chorus(double base_rate, std::size_t max_frames) {
  return checked<OrbitModule>(FxKind::chorus, base_rate, max_frames);
}
std::unique_ptr<FxModule> make_phaser(double base_rate, std::size_t max_frames) {
  return checked<OrbitModule>(FxKind::phaser, base_rate, max_frames);
}
std::unique_ptr<FxModule> make_delay(double base_rate, std::size_t max_frames) {
  return checked<DelayModule>(base_rate, max_frames);
}
std::unique_ptr<FxModule> make_reverb(double base_rate, std::size_t max_frames) {
  return checked<ReverbModule>(base_rate, max_frames);
}
std::unique_ptr<FxModule> make_flyby(double base_rate, std::size_t max_frames) {
  return checked<FlybyModule>(base_rate, max_frames);
}

} // namespace oscillate::native_dsp::synth::nova::fx_detail
