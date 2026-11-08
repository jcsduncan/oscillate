#include "oscillate/native_dsp/effects/flyby.hpp"

#include "effect_dsp.hpp"
#include "flyby_presets.hpp"
#include "flyby_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/expanse.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::effects::flyby {
namespace {

namespace t = flyby_table;
using t::index_of;

namespace p {
constexpr std::size_t path = index_of("flyby.path");
constexpr std::size_t x1 = index_of("flyby.x1");
constexpr std::size_t radius = index_of("flyby.radius");
constexpr std::size_t direction = index_of("flyby.direction");
constexpr std::size_t height = index_of("flyby.height");
constexpr std::size_t run = index_of("flyby.run");
constexpr std::size_t speed = index_of("flyby.speed");
constexpr std::size_t length = index_of("flyby.length");
constexpr std::size_t position = index_of("flyby.position");
constexpr std::size_t doppler = index_of("flyby.doppler");
constexpr std::size_t air = index_of("flyby.air");
constexpr std::size_t rolloff = index_of("flyby.rolloff");
constexpr std::size_t room = index_of("flyby.room");
constexpr std::size_t room_size = index_of("flyby.room_size");
constexpr std::size_t listen = index_of("flyby.listen");
constexpr std::size_t mix = index_of("flyby.mix");
constexpr std::size_t output = index_of("flyby.output");
constexpr std::size_t rest = index_of("flyby.rest");
constexpr std::size_t reference = index_of("flyby.reference");
constexpr std::size_t near = index_of("flyby.near");
} // namespace p
static_assert(t::lengths.size() == length_count, "the header counts Sync's lengths");
static_assert(index_of("flyby.y1") == p::x1 + 1 && index_of("flyby.y4") == p::x1 + 7,
              "the path's points are x1, y1 … x4, y4 in order");

constexpr double pi = std::numbers::pi;
constexpr double kmh = 1.0 / 3.6; // km/h to m/s
// Samples of the path: its arc length (so motion is even) and its closest
// approach.
constexpr std::size_t path_samples = 256;
// Fixed-point steps solving for the emission time; each shrinks the error
// by the source's speed over the speed of sound (at most 0.81 at 1000 km/h).
constexpr int emission_steps = 24;
// Woodworth's interaural time: (a / c)(θ + sin θ); the widest is at 90°.
constexpr double max_itd = head_radius / speed_of_sound * (pi / 2.0 + 1.0);
// Every read keeps half the widest interaural time in hand, so the ear
// nearer the source never needs a negative delay, plus whole frames for the
// interpolator. Rounded up to whole frames, it is the latency, and the dry
// path is delayed to match.
constexpr double margin_seconds = max_itd / 2.0;
constexpr std::uint32_t read_floor_frames = 2;
[[nodiscard]] std::uint32_t margin_frames(double rate) noexcept {
  return static_cast<std::uint32_t>(std::ceil(margin_seconds * rate)) + read_floor_frames;
}
// Brown and Duda's head shadow: α(θ) runs from 2 (facing) to αmin (behind),
// its minimum at θmin.
constexpr double shadow_alpha_min = 0.1;
constexpr double shadow_theta_min_degrees = 150.0;
// Air absorption ≈ k f² dB per metre (ISO 9613-1, 20 °C, 50% RH): about
// 0.15 dB/m at 10 kHz. A one-pole low-pass sits where it reaches 3 dB.
constexpr double air_db_per_metre_hz2 = 1.5e-9;
constexpr double air_corner_db = 3.0;
// No distance makes the source louder than this.
constexpr double max_gain_db = 24.0;
// A jump in the travel time bigger than this within a tick is a wrap or a
// restart, not motion: it crossfades instead of sweeping the pitch.
constexpr double jump_seconds = 0.004;
constexpr double crossfade_seconds = 0.02;
// Once with Between = Silent: the source fades in and out this fast.
constexpr double gate_seconds = 0.02;
// The room: Expanse wet-only, sized by Size, fed at this level.
constexpr double room_feed = 0.5;
constexpr double room_min_size = 0.25;
constexpr double room_size_span = 0.65;
constexpr double room_min_decay = 0.6;
constexpr double room_decay_span = 3.4;
constexpr double meter_hold_seconds = 1.0;

struct Vec {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};
[[nodiscard]] double norm(const Vec& v) noexcept {
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// The path as the parameters draw it, sampled for arc length.
class Path {
public:
  void set(std::span<const double> v) noexcept {
    std::array<double, 12> key{};
    key[0] = v[p::path];
    for (std::size_t i = 0; i < 8; ++i) {
      key[1 + i] = v[p::x1 + i];
    }
    key[9] = v[p::radius];
    key[10] = v[p::direction];
    key[11] = v[p::height];
    if (key == key_ && built_) {
      return;
    }
    key_ = key;
    built_ = true;
    orbit_ = static_cast<int>(v[p::path]) == t::orbit;
    for (std::size_t i = 0; i < t::points; ++i) {
      points_[i] = {v[p::x1 + 2 * i], v[p::x1 + 2 * i + 1], 0.0};
    }
    radius_ = v[p::radius];
    clockwise_ = static_cast<int>(v[p::direction]) == 0;
    height_ = v[p::height];
    arc_[0] = 0.0;
    Vec previous = shape(0.0);
    double closest = norm(at_shape(previous));
    for (std::size_t i = 1; i <= path_samples; ++i) {
      const Vec here = shape(static_cast<double>(i) / path_samples);
      arc_[i] = arc_[i - 1] + std::hypot(here.x - previous.x, here.y - previous.y);
      closest = std::min(closest, norm(at_shape(here)));
      previous = here;
    }
    closest_ = std::max(closest, 1e-3);
  }
  [[nodiscard]] double length() const noexcept { return std::max(arc_[path_samples], 1e-6); }
  [[nodiscard]] double closest() const noexcept { return closest_; }
  [[nodiscard]] bool orbit() const noexcept { return orbit_; }
  // The source at fraction u of the path's length (wrapping for an orbit,
  // held at the ends of a pass).
  [[nodiscard]] Vec at(double u) const noexcept {
    u = orbit_ ? u - std::floor(u) : std::clamp(u, 0.0, 1.0);
    const double target = u * length();
    const auto it = std::ranges::lower_bound(arc_, target);
    const auto hi =
        static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(it - arc_.begin(), 1, path_samples));
    const std::size_t lo = hi - 1;
    const double span = arc_[hi] - arc_[lo];
    const double k = span > 0.0 ? (target - arc_[lo]) / span : 0.0;
    return at_shape(shape((static_cast<double>(lo) + k) / path_samples));
  }

private:
  // The path's own parameter s (0..1) to a point on the ground.
  [[nodiscard]] Vec shape(double s) const noexcept {
    if (orbit_) {
      const double a = 2.0 * pi * s * (clockwise_ ? 1.0 : -1.0);
      return {radius_ * std::sin(a), radius_ * std::cos(a), 0.0};
    }
    const double r = 1.0 - s;
    const auto& [a, b, c, d] = points_;
    return {r * r * r * a.x + 3.0 * r * r * s * b.x + 3.0 * r * s * s * c.x + s * s * s * d.x,
            r * r * r * a.y + 3.0 * r * r * s * b.y + 3.0 * r * s * s * c.y + s * s * s * d.y, 0.0};
  }
  [[nodiscard]] Vec at_shape(Vec v) const noexcept {
    v.z = height_;
    return v;
  }

  std::array<double, 12> key_{};
  bool built_ = false;
  bool orbit_ = false;
  bool clockwise_ = true;
  std::array<Vec, t::points> points_{};
  double radius_ = 1.0;
  double height_ = 0.0;
  std::array<double, path_samples + 1> arc_{};
  double closest_ = 1.0;
};

// Brown and Duda's spherical-head shadow for one ear: H(s) = (αs + β) /
// (s + β), β = 2c / a, through the bilinear transform.
struct Shadow {
  double b0 = 1.0;
  double b1 = 0.0;
  double a1 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  void set(double theta, double rate) noexcept {
    const double alpha =
        (1.0 + shadow_alpha_min / 2.0) +
        (1.0 - shadow_alpha_min / 2.0) * std::cos(theta * 180.0 / shadow_theta_min_degrees);
    const double beta = 2.0 * speed_of_sound / head_radius;
    const double k = 2.0 * rate;
    b0 = (alpha * k + beta) / (k + beta);
    b1 = (beta - alpha * k) / (k + beta);
    a1 = (beta - k) / (k + beta);
  }
  void clear() noexcept { b0 = 1.0, b1 = 0.0, a1 = 0.0; }
  [[nodiscard]] double process(double x) noexcept {
    const double y = b0 * x + b1 * x1 - a1 * y1;
    x1 = x;
    y1 = y;
    return y;
  }
  void reset() noexcept { x1 = y1 = 0.0; }
};

// A value that moves linearly to its target over a number of frames.
struct Ramp {
  double value = 0.0;
  double step = 0.0;
  std::uint32_t left = 0;
  void to(double target, std::uint32_t frames) noexcept {
    if (frames == 0) {
      value = target;
      left = 0;
      return;
    }
    step = (target - value) / frames;
    left = frames;
  }
  double next() noexcept {
    if (left > 0) {
      value += step;
      --left;
    }
    return value;
  }
};

// What each ear hears at one instant.
struct Hearing {
  Vec source;
  double distance = 1.0;
  double delay_left = 0.0; // Seconds.
  double delay_right = 0.0;
  double centre = 0.0;
  double gain_left = 0.0;
  double gain_right = 0.0;
  double air_left = 0.0; // One-pole coefficient (0: open).
  double air_right = 0.0;
  double theta_left = pi / 2.0; // Angle from each ear's axis to the source.
  double theta_right = pi / 2.0;
  double itd = 0.0; // Seconds, positive when the right ear leads.
};

class FlybyKernel final : public EffectKernel {
public:
  FlybyKernel(std::span<const double> values, double rate) : rate_(rate) {
    margin_ = margin_frames(rate);
    const auto frames = static_cast<std::size_t>(std::ceil(max_delay_seconds * rate)) + margin_ + 8;
    line_.prepare(frames);
    dry_left_.prepare(margin_ + 4);
    dry_right_.prepare(margin_ + 4);
    const auto& room = expanse::engine();
    room_values_.resize(room.descriptor->parameters.size());
    for (std::size_t i = 0; i < room_values_.size(); ++i) {
      room_values_[i] = room.descriptor->parameters[i].default_value;
    }
    const auto find = [&](std::string_view id) {
      for (std::size_t i = 0; i < room_values_.size(); ++i) {
        if (room.descriptor->parameters[i].id == id) {
          return i;
        }
      }
      return room_values_.size();
    };
    room_size_index_ = find("expanse.size");
    room_decay_index_ = find("expanse.decay");
    const std::size_t mix = find("expanse.mix");
    const std::size_t predelay = find("expanse.predelay");
    if (mix < room_values_.size()) {
      room_values_[mix] = 1.0;
    }
    if (predelay < room_values_.size()) {
      room_values_[predelay] = 0.0;
    }
    room_scratch_.resize(control_interval_frames);
    configure_room(values);
    room_ = room.create_kernel(room_values_, rate);
    crossfade_frames_ = static_cast<std::uint32_t>(std::lround(crossfade_seconds * rate));
    gate_frames_ = static_cast<std::uint32_t>(std::lround(gate_seconds * rate));
    meter_fall_ = std::exp(-1.0 / (meter_hold_seconds * rate));
    path_.set(values);
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport& transport) noexcept override {
    path_.set(v);
    transport_ = transport;
    run_ = static_cast<int>(v[p::run]);
    listen_ = static_cast<int>(v[p::listen]);
    silent_rest_ = static_cast<int>(v[p::rest]) == t::silent;
    doppler_ = v[p::doppler];
    air_ = v[p::air];
    rolloff_ = v[p::rolloff];
    reference_ = v[p::reference];
    near_ = v[p::near];
    mix_.to(v[p::mix], primed_ ? frames : 0U);
    output_.to(dsp::db_to_gain(v[p::output]), primed_ ? frames : 0U);
    room_gain_.to(room_feed * v[p::room], primed_ ? frames : 0U);
    speed_ = v[p::speed] * kmh;
    length_bars_ = t::length_bars[static_cast<std::size_t>(v[p::length])];
    position_ = v[p::position];

    // Where the source is at the end of this tick, and how fast it moves.
    const double seconds = static_cast<double>(frames) / rate_;
    const double u = position_at_tick_end(frames);
    const double rate_u = motion_rate(seconds, u);
    const Hearing h = hear(u, rate_u);
    const double base = h.centre;
    const bool jump = primed_ && std::fabs(base - last_centre_) > jump_seconds;
    if (jump) {
      // The source leapt (a loop wrapped, a pass restarted): the old reads
      // fade out while the new ones fade in.
      old_left_ = delay_left_.value;
      old_right_ = delay_right_.value;
      fade_.value = 0.0;
      fade_.to(1.0, crossfade_frames_);
    }
    const std::uint32_t glide = !primed_ ? 0U : (jump ? 0U : frames);
    delay_left_.to(h.delay_left * rate_ + margin_, glide);
    delay_right_.to(h.delay_right * rate_ + margin_, glide);
    const std::uint32_t level_glide = !primed_ ? 0U : (jump ? crossfade_frames_ : frames);
    gain_left_.to(h.gain_left, level_glide);
    gain_right_.to(h.gain_right, level_glide);
    air_left_ = h.air_left;
    air_right_ = h.air_right;
    if (listen_ == t::headphones) {
      shadow_left_.set(h.theta_left, rate_);
      shadow_right_.set(h.theta_right, rate_);
    } else {
      shadow_left_.clear();
      shadow_right_.clear();
    }
    const bool sounding = !(silent_rest_ && run_ == t::once && !in_pass_);
    gate_.to(sounding ? 1.0 : 0.0, primed_ ? std::max(frames, gate_frames_) : 0U);

    // Telemetry: a delay growing at `slope` seconds per second plays the
    // input at 1 - slope of its speed.
    const double slope = primed_ && !jump ? (base - last_centre_) / std::max(seconds, 1e-9) : 0.0;
    pitch_cents_ = 1'200.0 * std::log2(std::max(1e-3, 1.0 - slope));
    last_centre_ = base;
    hearing_ = h;
    u_ = u;
    speed_now_ = rate_u * path_.length();

    configure_room(v);
    if (room_) {
      room_->control(room_values_, frames, transport);
    }
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    // A host may run longer than a tick between controls (Nova's rack
    // does); the scratch is a tick long, so run it a tick at a time.
    for (std::size_t done = 0; done < audio.size(); done += control_interval_frames) {
      run(audio.subspan(done, std::min<std::size_t>(control_interval_frames, audio.size() - done)));
    }
  }

  void reset() noexcept override {
    line_.reset();
    dry_left_.reset();
    dry_right_.reset();
    shadow_left_.reset();
    shadow_right_.reset();
    air_state_left_ = air_state_right_ = 0.0;
    fade_ = {};
    free_seconds_ = 0.0;
    primed_ = false;
    in_pass_ = false;
    in_peak_ = out_peak_left_ = out_peak_right_ = 0.0;
    if (room_) {
      room_->reset();
    }
  }

  void locate(std::uint64_t project_frame) noexcept override { frame_ = project_frame; }

  void adopt_key_notes(std::span<const KeyNote> notes) noexcept override { notes_ = notes; }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    const auto db = [](double g) {
      return static_cast<float>(20.0 * std::log10(std::max(g, 1e-6)));
    };
    const auto& h = hearing_;
    out[telemetry::x] = static_cast<float>(h.source.x);
    out[telemetry::y] = static_cast<float>(h.source.y);
    out[telemetry::z] = static_cast<float>(h.source.z);
    out[telemetry::distance] = static_cast<float>(h.distance);
    out[telemetry::pitch_cents] = static_cast<float>(pitch_cents_);
    out[telemetry::itd_us] = static_cast<float>(listen_ == t::headphones ? h.itd * 1e6 : 0.0);
    out[telemetry::level_left_db] = db(h.gain_left);
    out[telemetry::level_right_db] = db(h.gain_right);
    out[telemetry::room] = static_cast<float>(room_gain_.value / room_feed);
    out[telemetry::position] = static_cast<float>(u_);
    out[telemetry::closest] = static_cast<float>(path_.closest());
    out[telemetry::speed] = static_cast<float>(speed_now_);
    out[telemetry::input_peak] = db(in_peak_);
    out[telemetry::output_peak] = db(out_peak_left_);
    out[telemetry::output_peak + 1] = db(out_peak_right_);
    return telemetry::size;
  }

private:
  // One tick's worth (at most) of audio.
  void run(std::span<engine::StereoFrame> audio) noexcept {
    const std::size_t n = audio.size();
    for (std::size_t i = 0; i < n; ++i) {
      const double mono = 0.5 * (static_cast<double>(audio[i].left) + audio[i].right);
      dry_left_.write(audio[i].left);
      dry_right_.write(audio[i].right);
      dry_[i] = {dry_left_.sample(margin_), dry_right_.sample(margin_)};
      in_peak_ = std::max(in_peak_ * meter_fall_, std::fabs(mono));
      line_.write(std::isfinite(mono) ? mono : 0.0);
      const double dl = delay_left_.next();
      const double dr = delay_right_.next();
      double left = line_.read(dl);
      double right = line_.read(dr);
      if (fade_.left > 0) {
        const double w = fade_.next();
        const double was_left = line_.read(old_left_);
        const double was_right = line_.read(old_right_);
        left = was_left + w * (left - was_left);
        right = was_right + w * (right - was_right);
      }
      const double gate = gate_.next();
      // The room hears the source from where it is, with no head between.
      room_scratch_[i] = 0.5 * (left + right) * room_gain_.next() * gate;
      air_state_left_ += (1.0 - air_left_) * (left - air_state_left_);
      air_state_right_ += (1.0 - air_right_) * (right - air_state_right_);
      left = shadow_left_.process(air_state_left_) * gain_left_.next() * gate;
      right = shadow_right_.process(air_state_right_) * gain_right_.next() * gate;
      wet_left_[i] = left;
      wet_right_[i] = right;
    }
    if (room_) {
      std::array<engine::StereoFrame, control_interval_frames> room{};
      for (std::size_t i = 0; i < n; ++i) {
        room[i] = {static_cast<float>(room_scratch_[i]), static_cast<float>(room_scratch_[i])};
      }
      room_->process(std::span<engine::StereoFrame>(room.data(), n), {});
      for (std::size_t i = 0; i < n; ++i) {
        wet_left_[i] += room[i].left;
        wet_right_[i] += room[i].right;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      const double mix = mix_.next();
      const double gain = output_.next();
      const auto [dry_left, dry_right] = dry_[i];
      const double left = (dry_left + mix * (wet_left_[i] - dry_left)) * gain;
      const double right = (dry_right + mix * (wet_right_[i] - dry_right)) * gain;
      audio[i].left = std::isfinite(left) ? static_cast<float>(left) : 0.0F;
      audio[i].right = std::isfinite(right) ? static_cast<float>(right) : 0.0F;
      out_peak_left_ = std::max(out_peak_left_ * meter_fall_, std::fabs(left));
      out_peak_right_ = std::max(out_peak_right_ * meter_fall_, std::fabs(right));
    }
    frame_ += n;
    if (transport_.beats_per_frame <= 0.0) {
      free_seconds_ += static_cast<double>(n) / rate_;
    }
  }

  void configure_room(std::span<const double> v) noexcept {
    const double size = v[p::room_size];
    if (room_size_index_ < room_values_.size()) {
      room_values_[room_size_index_] = room_min_size + room_size_span * size;
    }
    if (room_decay_index_ < room_values_.size()) {
      room_values_[room_decay_index_] = room_min_decay + room_decay_span * size;
    }
  }

  // The path position at the end of a tick `frames` long, by Run.
  [[nodiscard]] double position_at_tick_end(std::uint32_t frames) noexcept {
    const double frame_end = static_cast<double>(frame_ + frames);
    switch (run_) {
    case t::loop: {
      const double seconds = transport_.beats_per_frame > 0.0
                                 ? frame_end / rate_
                                 : free_seconds_ + static_cast<double>(frames) / rate_;
      const double u = seconds * speed_ / path_.length();
      return u - std::floor(u);
    }
    case t::sync: {
      const double beats =
          transport_.beat_at_block_start + transport_.beats_per_frame * static_cast<double>(frames);
      const double span = length_bars_ * std::max(transport_.beats_per_bar, 1.0);
      const double u = beats / span;
      return u - std::floor(u);
    }
    case t::once: {
      const auto end = static_cast<std::int64_t>(frame_ + frames);
      const auto after = std::ranges::upper_bound(notes_, end, {}, &KeyNote::frame);
      if (after == notes_.begin()) {
        in_pass_ = false;
        return 0.0;
      }
      const double since = static_cast<double>(end - std::prev(after)->frame) / rate_;
      const double u = since * speed_ / path_.length();
      in_pass_ = u < 1.0;
      return std::min(u, 1.0);
    }
    default:
      return std::clamp(position_, 0.0, 1.0);
    }
  }

  // How fast the position moves (path fractions per second).
  [[nodiscard]] double motion_rate(double seconds, double u) noexcept {
    double rate = 0.0;
    switch (run_) {
    case t::loop:
      rate = speed_ / path_.length();
      break;
    case t::once:
      rate = in_pass_ ? speed_ / path_.length() : 0.0;
      break;
    case t::sync:
      rate = transport_.beats_per_frame * rate_ /
             (length_bars_ * std::max(transport_.beats_per_bar, 1.0));
      break;
    default: {
      // Position: the automation's own speed, from tick to tick.
      double du = u - last_u_;
      rate = primed_ && seconds > 0.0 ? du / seconds : 0.0;
      break;
    }
    }
    last_u_ = u;
    return rate;
  }

  // What each ear hears of a source at u moving at rate_u. The sample read
  // now entered the line `delay` ago (the travel time from the closest
  // approach, scaled by Doppler, plus the margin), so it was emitted where
  // the source was then: solved together, the pitch is the true Doppler
  // shift, f / (1 + Doppler · v_r / c), with v_r at emission.
  [[nodiscard]] Hearing hear(double u, double rate_u) const noexcept {
    Hearing h;
    const double margin = static_cast<double>(margin_) / rate_;
    const auto travel = [&](const Vec& at) {
      return std::max(0.0, doppler_ * (norm(at) - path_.closest()) / speed_of_sound);
    };
    double delay = travel(path_.at(u)) + margin;
    for (int k = 0; k < emission_steps; ++k) {
      delay = travel(path_.at(u - rate_u * delay)) + margin;
    }
    const Vec s = path_.at(u - rate_u * delay);
    const double d = std::max(norm(s), 1e-3);
    h.source = s;
    h.distance = d;
    // The ears read either side of the centre, within the margin.
    h.centre = travel(s);
    // Lateral angle (right positive) and each ear's angle to the source.
    const double lateral = std::asin(std::clamp(s.x / d, -1.0, 1.0));
    const double woodworth =
        head_radius / speed_of_sound * (std::fabs(lateral) + std::sin(std::fabs(lateral)));
    h.itd = lateral >= 0.0 ? woodworth : -woodworth;
    h.theta_right = std::acos(std::clamp(s.x / d, -1.0, 1.0));
    h.theta_left = std::acos(std::clamp(-s.x / d, -1.0, 1.0));
    const double gain =
        std::min(dsp::db_to_gain(max_gain_db), std::pow(reference_ / std::max(d, near_), rolloff_));
    if (listen_ == t::headphones) {
      h.delay_left = h.centre + h.itd / 2.0;
      h.delay_right = h.centre - h.itd / 2.0;
      h.gain_left = gain;
      h.gain_right = gain;
    } else {
      h.delay_left = h.delay_right = h.centre;
      const double pan = (std::sin(lateral) + 1.0) * pi / 4.0;
      h.gain_left = gain * std::cos(pan) * std::numbers::sqrt2;
      h.gain_right = gain * std::sin(pan) * std::numbers::sqrt2;
    }
    const auto air_pole = [&](double metres) {
      if (air_ <= 0.0) {
        return 0.0;
      }
      const double hz = std::sqrt(air_corner_db / (air_db_per_metre_hz2 * air_ * metres));
      return hz >= native_dsp::dsp::nyquist_guard * rate_ ? 0.0 : std::exp(-2.0 * pi * hz / rate_);
    };
    h.air_left = h.air_right = air_pole(d);
    return h;
  }

  double rate_;
  std::uint32_t margin_ = 0;
  dsp::Delay line_;
  dsp::Delay dry_left_;
  dsp::Delay dry_right_;
  std::array<std::pair<double, double>, control_interval_frames> dry_{};
  Path path_;
  Transport transport_{};
  std::span<const KeyNote> notes_{};
  std::uint64_t frame_ = 0;
  double free_seconds_ = 0.0;
  bool primed_ = false;
  bool in_pass_ = false;
  int run_ = t::loop;
  int listen_ = t::headphones;
  bool silent_rest_ = true;
  double doppler_ = 1.0;
  double air_ = 1.0;
  double rolloff_ = 1.0;
  double reference_ = 5.0;
  double near_ = 1.0;
  double speed_ = 25.0;
  double length_bars_ = 2.0;
  double position_ = 0.0;
  double last_u_ = 0.0;
  double last_centre_ = 0.0;
  double u_ = 0.0;
  double speed_now_ = 0.0;
  double pitch_cents_ = 0.0;
  Hearing hearing_{};
  Ramp delay_left_;
  Ramp delay_right_;
  Ramp gain_left_;
  Ramp gain_right_;
  Ramp fade_;
  Ramp gate_;
  Ramp mix_;
  Ramp output_;
  Ramp room_gain_;
  double old_left_ = 0.0;
  double old_right_ = 0.0;
  double air_left_ = 0.0;
  double air_right_ = 0.0;
  double air_state_left_ = 0.0;
  double air_state_right_ = 0.0;
  Shadow shadow_left_;
  Shadow shadow_right_;
  std::uint32_t crossfade_frames_ = 960;
  std::uint32_t gate_frames_ = 960;
  double meter_fall_ = 0.9999;
  double in_peak_ = 0.0;
  double out_peak_left_ = 0.0;
  double out_peak_right_ = 0.0;
  std::vector<double> room_values_;
  std::size_t room_size_index_ = 0;
  std::size_t room_decay_index_ = 0;
  std::unique_ptr<EffectKernel> room_;
  std::vector<double> room_scratch_;
  std::array<double, control_interval_frames> wet_left_{};
  std::array<double, control_interval_frames> wet_right_{};
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (!std::isfinite(sample_rate) || sample_rate < 8'000.0 || sample_rate > 384'000.0) {
    return nullptr;
  }
  return std::make_unique<FlybyKernel>(values, sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const auto& room = expanse::engine();
  std::vector<double> room_values(room.descriptor->parameters.size());
  for (std::size_t i = 0; i < room_values.size(); ++i) {
    const auto& parameter = room.descriptor->parameters[i];
    room_values[i] = parameter.id == "expanse.decay"
                         ? room_min_decay + room_decay_span * values[p::room_size]
                         : parameter.default_value;
  }
  return static_cast<std::uint64_t>(
             std::ceil((max_delay_seconds + crossfade_seconds) * sample_rate)) +
         room.tail_frames(room_values, sample_rate);
}

std::uint32_t latency_frames(std::span<const double>, double sample_rate) {
  return margin_frames(sample_rate);
}

const synth::EngineDescriptor descriptor{"flyby", "Flyby", 1, t::parameters,
                                         flyby_presets::presets};
// Keyable (a sidechain route names the tracks whose notes start Once), as
// Pump is; the key's audio itself is not read.
const EffectEngine flyby_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true,
                                false,       nullptr,        nullptr,      nullptr,         true};

} // namespace

const EffectEngine& engine() { return flyby_engine; }

} // namespace oscillate::native_dsp::effects::flyby
