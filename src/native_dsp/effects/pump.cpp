#include "oscillate/native_dsp/effects/pump.hpp"

#include "dsp/ramp.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "pump_dsp.hpp"
#include "pump_presets.hpp"
#include "pump_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::native_dsp::effects {
namespace pump {

namespace {

using foundation::ErrorCode;
using foundation::Result;
using pump_table::index_of;

namespace p {
constexpr std::size_t trigger = index_of("pump.trigger");
constexpr std::size_t rate = index_of("pump.rate");
constexpr std::size_t feel = index_of("pump.feel");
constexpr std::size_t threshold = index_of("pump.threshold");
constexpr std::size_t lookahead = index_of("pump.lookahead");
constexpr std::size_t detect_low = index_of("pump.detect_low");
constexpr std::size_t detect_high = index_of("pump.detect_high");
constexpr std::size_t listen = index_of("pump.listen");
constexpr std::size_t note_low = index_of("pump.note_low");
constexpr std::size_t note_high = index_of("pump.note_high");
constexpr std::size_t velocity = index_of("pump.velocity");
constexpr std::size_t follow = index_of("pump.follow");
constexpr std::size_t depth = index_of("pump.depth");
constexpr std::size_t swing = index_of("pump.swing");
constexpr std::size_t slot = index_of("pump.slot");
constexpr std::size_t morph = index_of("pump.morph");
constexpr std::size_t pattern = index_of("pump.pattern");
constexpr std::size_t step = index_of("pump.step.1");
static_assert(index_of("pump.step.8") == step + pump_table::max_pattern - 1);
constexpr std::size_t nudge = index_of("pump.nudge");
constexpr std::size_t anticlick = index_of("pump.anticlick");
constexpr std::size_t band = index_of("pump.band");
constexpr std::size_t crossover = index_of("pump.crossover");
constexpr std::size_t crossover_high = index_of("pump.crossover_high");
constexpr std::size_t depth_mid = index_of("pump.depth_mid");
constexpr std::size_t depth_high = index_of("pump.depth_high");
constexpr std::size_t stereo = index_of("pump.stereo");
constexpr std::size_t mix = index_of("pump.mix");
constexpr std::size_t output = index_of("pump.output");
} // namespace p

// With no tempo (Kick Audio off the timeline), cycles are timed at 120 BPM.
constexpr double fallback_beats_per_second = 2.0;
// Band, stereo and listen switches crossfade over this long.
constexpr double band_fade_seconds = native_dsp::dsp::switch_fade_seconds;
// Kick Audio: a band-limited kick confirms a full-band onset this recent,
// and full-band onsets re-arm this soon (a hat just before a kick does not
// hide the kick's own onset).
constexpr double confirm_seconds = 0.03;
constexpr double onset_hold_seconds = 0.015;
// The applied-curve view simulates the smoother at this many steps a cycle.
constexpr std::size_t view_steps = 4 * view_points;

[[nodiscard]] std::size_t choice(double value) noexcept {
  return static_cast<std::size_t>(std::max(0.0, value));
}

// The one bend every drawn curve uses (as Nova's, spec 08a §3.2).
[[nodiscard]] double bend(double t, double tension) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  if (tension > 0.0)
    return std::pow(t, 1.0 + 4.0 * tension);
  if (tension < 0.0)
    return 1.0 - std::pow(1.0 - t, 1.0 - 4.0 * tension);
  return t;
}

[[nodiscard]] double wrap(double x) noexcept { return x - std::floor(x); }

// The compiled document: every slot and band resolved to a shape, missing
// ones following their slot's (and missing slots following A's).
struct PumpState final : DocumentState {
  std::vector<Shape> drawn;
  std::array<const Shape*, shape_slots * shape_bands> lookup{};
  [[nodiscard]] const Shape& at(std::size_t slot, std::size_t band) const noexcept {
    return *lookup[slot * shape_bands + band];
  }
};

// The curve's place in PumpState::lookup, or nullopt for a stranger.
[[nodiscard]] std::optional<std::size_t> curve_index(std::string_view id) {
  for (std::size_t slot = 0; slot < shape_slots; ++slot)
    for (std::size_t band = 0; band < shape_bands; ++band)
      if (id == curve_id(slot, band))
        return slot * shape_bands + band;
  return std::nullopt;
}

void resolve(PumpState& state, const std::array<const Shape*, shape_slots * shape_bands>& found) {
  const Shape* a = found[0] != nullptr ? found[0] : &default_shape();
  for (std::size_t slot = 0; slot < shape_slots; ++slot) {
    const Shape* main = found[slot * shape_bands] != nullptr ? found[slot * shape_bands] : a;
    for (std::size_t band = 0; band < shape_bands; ++band) {
      const auto i = slot * shape_bands + band;
      state.lookup[i] = found[i] != nullptr ? found[i] : main;
    }
  }
}

[[nodiscard]] const PumpState& default_state() {
  static const auto state = [] {
    auto made = std::make_unique<PumpState>();
    resolve(*made, {});
    return made;
  }();
  return *state;
}

[[nodiscard]] project::NativeSynthDocument
shape_document(std::span<const pump_presets::Point> points) {
  project::NativeCurve curve{std::string(shape_curve), {}};
  for (const auto& point : points)
    curve.points.push_back({point.x, point.y, point.tension});
  project::NativeSynthDocument document;
  document.curves.push_back(std::move(curve));
  return document;
}

Result<void> validate_document(const project::NativeSynthDocument& document) {
  using R = Result<void>;
  if (!document.routes.empty() || !document.chains.empty() || !document.sequences.empty() ||
      !document.media.empty() || !document.labels.empty() || !document.voice_lanes.empty()) {
    return R::failure({ErrorCode::invalid_argument, "Pump's document holds only shape curves"});
  }
  std::array<bool, shape_slots * shape_bands> seen{};
  for (const auto& curve : document.curves) {
    const auto index = curve_index(curve.id);
    if (!index || seen[*index])
      return R::failure({ErrorCode::invalid_argument,
                         "Pump's curves are its shape_slots' and shape_bands' shapes, once"});
    if (curve.points.empty())
      return R::failure({ErrorCode::invalid_argument, "A Pump shape needs a point"});
    seen[*index] = true;
  }
  return R::success();
}

Result<std::shared_ptr<const DocumentState>>
prepare_document(const project::NativeSynthDocument& document, const DocumentInputs&) {
  using R = Result<std::shared_ptr<const DocumentState>>;
  if (auto valid = validate_document(document); !valid)
    return R::failure(valid.error());
  auto state = std::make_shared<PumpState>();
  // Reserved, so the pointers into it stay put.
  state->drawn.reserve(document.curves.size());
  std::array<const Shape*, shape_slots * shape_bands> found{};
  for (const auto& curve : document.curves) {
    state->drawn.push_back(sample_shape(curve.points));
    if (const auto index = curve_index(curve.id))
      found[*index] = &state->drawn.back();
  }
  resolve(*state, found);
  return R::success(std::shared_ptr<const DocumentState>(std::move(state)));
}

project::NativeSynthDocument preset_document(std::optional<std::size_t> preset) {
  const std::size_t index = preset.value_or(pump_presets::presets.size());
  const std::size_t shape =
      index < pump_presets::preset_shapes.size() ? pump_presets::preset_shapes[index] : 0;
  auto document = shape_document(pump_presets::shapes[shape].points);
  if (index < pump_presets::preset_b_shapes.size() && pump_presets::preset_b_shapes[index] >= 0) {
    const auto b = static_cast<std::size_t>(pump_presets::preset_b_shapes[index]);
    auto second = shape_document(pump_presets::shapes[b].points);
    second.curves.front().id = curve_id(1, 0);
    document.curves.push_back(std::move(second.curves.front()));
  }
  return document;
}

[[nodiscard]] std::size_t lookahead_frames(std::span<const double> v, double sample_rate) {
  const auto index = std::min(choice(v[p::lookahead]), pump_table::lookahead_ms.size() - 1);
  return static_cast<std::size_t>(
      std::lround(pump_table::lookahead_ms[index] * 1e-3 * sample_rate));
}

[[nodiscard]] std::int64_t modulo(std::int64_t a, std::int64_t n) noexcept {
  return ((a % n) + n) % n;
}

class PumpKernel final : public EffectKernel {
public:
  PumpKernel(std::span<const double> values, double sample_rate)
      : rate_(sample_rate), lookahead_(lookahead_frames(values, sample_rate)),
        band_steps_(band_fade_seconds * sample_rate) {
    for (auto& line : delay_)
      line.prepare(lookahead_ + 1);
    for (auto& line : listen_delay_)
      line.prepare(lookahead_ + 1);
    for (auto& smoother : smoothers_) {
      smoother.prepare(sample_rate);
      smoother.set(values[p::anticlick] * 1e-3);
    }
    detector_.prepare(sample_rate);
    onset_detector_.prepare(sample_rate, onset_hold_seconds);
    confirm_frames_ = confirm_seconds * sample_rate;
    set_filters(values);
  }

  void adopt_document(const DocumentState& state) noexcept override {
    state_ = &static_cast<const PumpState&>(state);
    view_dirty_ = true;
  }

  void adopt_key_notes(std::span<const KeyNote> notes) noexcept override {
    notes_ = notes;
    cursors_dirty_ = true;
  }

  void locate(std::uint64_t project_frame) noexcept override {
    const auto frame = static_cast<std::int64_t>(project_frame);
    if (frame != frame_)
      cursors_dirty_ = true;
    frame_ = frame;
  }

  void control(std::span<const double> v, std::uint32_t frames_to_next_tick,
               const Transport& transport) noexcept override {
    const auto steps = static_cast<double>(frames_to_next_tick);
    trigger_ = static_cast<int>(choice(v[p::trigger]));
    detector_.set_threshold_db(v[p::threshold]);
    onset_detector_.set_threshold_db(v[p::threshold]);
    for (auto& smoother : smoothers_)
      smoother.set(v[p::anticlick] * 1e-3);
    set_filters(v);
    depth_.to(v[p::depth], steps, !primed_);
    depth_mid_.to(v[p::depth_mid], steps, !primed_);
    depth_high_.to(v[p::depth_high], steps, !primed_);
    morph_.to(v[p::morph], steps, !primed_);
    mix_.to(v[p::mix], steps, !primed_);
    output_.to(std::pow(10.0, v[p::output] / 20.0), steps, !primed_);
    // Switches fade, re-aimed every tick over at least the tick.
    const auto fade = [&](native_dsp::dsp::Ramp& ramp, double target) {
      ramp.settle(target, std::max(steps, band_steps_ * std::fabs(target - ramp.value)), !primed_);
    };
    const auto band = static_cast<int>(choice(v[p::band]));
    fade(band_weight_[0], band == pump_table::full ? 1.0 : 0.0);
    fade(band_weight_[1], band == pump_table::lows ? 1.0 : 0.0);
    fade(band_weight_[2], band == pump_table::three ? 1.0 : 0.0);
    const auto stereo = static_cast<int>(choice(v[p::stereo]));
    fade(apply_mid_, stereo == pump_table::side ? 0.0 : 1.0);
    fade(apply_side_, stereo == pump_table::mid ? 0.0 : 1.0);
    fade(listen_, trigger_ == pump_table::audio && choice(v[p::listen]) == 1 ? 1.0 : 0.0);

    const int low = static_cast<int>(v[p::note_low]);
    const int high = static_cast<int>(v[p::note_high]);
    if (low != note_low_ || high != note_high_) {
      note_low_ = low;
      note_high_ = high;
      cursors_dirty_ = true;
    }
    velocity_amount_ = v[p::velocity];
    follow_ = choice(v[p::follow]) == pump_table::kick_spacing;
    swing_ = 2.0 * (std::clamp(v[p::swing], 0.5, 0.75) - 0.5);
    slot_mode_ = static_cast<int>(std::min(choice(v[p::slot]), pump_table::slot_modes.size() - 1));
    pattern_length_ = std::clamp<std::int64_t>(static_cast<std::int64_t>(v[p::pattern]), 1,
                                               pump_table::max_pattern);
    for (std::size_t i = 0; i < pump_table::max_pattern; ++i)
      steps_[i] = std::min(choice(v[p::step + i]), shape_slots - 1);

    // The cycle in its own units: bars for the bar rates, else beats.
    const auto rate = std::min(choice(v[p::rate]), pump_table::rates.size() - 1);
    const auto feel = pump_table::feel_scale[std::min(choice(v[p::feel]), std::size_t{2})];
    const bool in_bars = pump_table::rate_bars[rate] > 0.0;
    cycle_units_ = (in_bars ? pump_table::rate_bars[rate] : pump_table::rate_beats[rate]) * feel;
    const double beats_per_bar = transport.beats_per_bar > 0.0 ? transport.beats_per_bar : 4.0;
    cycle_beats_ = in_bars ? cycle_units_ * beats_per_bar : cycle_units_;
    tempo_known_ = transport.beats_per_frame > 0.0;
    const double beats_per_frame =
        tempo_known_ ? transport.beats_per_frame : fallback_beats_per_second / rate_;
    units_per_frame_ = in_bars ? beats_per_frame / beats_per_bar : beats_per_frame;
    position_ = in_bars ? transport.bar_at_block_start : transport.beat_at_block_start;
    cycle_frames_ = cycle_units_ / units_per_frame_;
    nudge_frames_ = v[p::nudge] * 1e-3 * rate_;
    preroll_frames_ = smoothers_[0].delay_seconds() * rate_;

    // The last note on the keying tracks, any key, for Learn.
    const auto after = std::ranges::upper_bound(notes_, frame_, {}, &KeyNote::frame);
    last_note_ = after == notes_.begin() ? -1 : std::prev(after)->key;

    const double depth = v[p::depth];
    const double mix = v[p::mix];
    const double tau = smoothers_[0].tau_seconds();
    const double morph = slot_mode_ == pump_table::morph ? v[p::morph] : 0.0;
    if (view_dirty_ || depth != view_depth_ || mix != view_mix_ || tau != view_tau_ ||
        playing_slot_ != view_slot_ || std::fabs(morph - view_morph_) > 0.005 ||
        std::fabs(cycle_frames_ - view_cycle_frames_) > 1e-3 * cycle_frames_) {
      view_depth_ = depth;
      view_mix_ = mix;
      view_tau_ = tau;
      view_slot_ = playing_slot_;
      view_morph_ = morph;
      view_cycle_frames_ = cycle_frames_;
      update_view();
    }
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    keyed_ = !sidechain.empty();
    const auto lookahead = static_cast<double>(lookahead_);
    if (cursors_dirty_ && trigger_ == pump_table::midi) {
      const double out = static_cast<double>(frame_) - lookahead;
      for (auto* cursor : {&look_, &heard_cursor_, &capture_cursor_})
        cursor->set_range(note_low_, note_high_);
      look_.rebuild(notes_, out - nudge_frames_ + preroll_frames_);
      heard_cursor_.rebuild(notes_, out - nudge_frames_);
      capture_cursor_.rebuild(notes_, static_cast<double>(frame_) - nudge_frames_);
      cursors_dirty_ = false;
    }
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const auto in = audio[i];
      const auto key = keyed_ ? sidechain[i] : in;
      const auto band_key = detect_.next(key);
      delay_[0].write(in.left);
      delay_[1].write(in.right);
      const double left = delay_[0].sample(lookahead_);
      const double right = delay_[1].sample(lookahead_);
      // Listen hears the key on the output's clock; the detector, below,
      // still hears it now. The line runs whether or not Listen is on.
      listen_delay_[0].write(band_key.left);
      listen_delay_[1].write(band_key.right);
      const double listen_l = listen_delay_[0].sample(lookahead_);
      const double listen_r = listen_delay_[1].sample(lookahead_);

      // Where the shape is read for the output frame, read early by the
      // smoothers' delay so the smoothed gain lands where it was drawn.
      Read read;
      double heard = -1.0;
      if (trigger_ == pump_table::tempo) {
        // The output is `lookahead` frames behind the input and the key.
        const double out_units = position_ - lookahead * units_per_frame_;
        const double nudge_units = nudge_frames_ * units_per_frame_;
        const double preroll_units = preroll_frames_ * units_per_frame_;
        if (tempo_known_) {
          const auto look = tempo_cycle(out_units - nudge_units + preroll_units);
          read = {look.phase, look.index, 1.0, true};
          heard = tempo_cycle(out_units - nudge_units).phase;
          capture_.add(tempo_cycle(position_ - nudge_units).phase, key);
        }
        position_ += units_per_frame_;
      } else if (trigger_ == pump_table::midi) {
        // The notes are known ahead, so the shape is read early by the
        // smoothers' delay and the nudge with no lookahead needed.
        const double out = static_cast<double>(frame_) - lookahead;
        const double look = out - nudge_frames_ + preroll_frames_;
        look_.advance(look);
        if (const auto* note = look_.last()) {
          read = {std::min((look - static_cast<double>(note->frame)) / note_cycle(look_), 1.0),
                  look_.count() - 1, velocity_scale(note->velocity), true};
        } else {
          read = {1.0, 0, 1.0, true};
        }
        const double heard_time = out - nudge_frames_;
        heard_cursor_.advance(heard_time);
        if (const auto* note = heard_cursor_.last()) {
          const double length = note_cycle(heard_cursor_);
          if (const double t = heard_time - static_cast<double>(note->frame); t < length)
            heard = t / length;
        }
        const double in_time = static_cast<double>(frame_) - nudge_frames_;
        capture_cursor_.advance(in_time);
        if (const auto* note = capture_cursor_.last()) {
          const double length = note_cycle(capture_cursor_);
          if (const double t = in_time - static_cast<double>(note->frame); t < length)
            capture_.add(t / length, key);
        }
      } else {
        // The full band times the onset; the kick's band, later by its
        // filters' delay, says it was a kick. The cycle starts at the onset.
        if (onset_detector_.next(key))
          since_onset_ = 0.0;
        if (detector_.next(band_key))
          on_kick(since_onset_ <= confirm_frames_ ? since_onset_ : 0.0);
        since_onset_ = std::min(since_onset_ + 1.0, idle_frames);
        const double now = kick_.elapsed - lookahead - nudge_frames_;
        double time = now + preroll_frames_;
        const Kick* kick = &kick_;
        if (time < 0.0) {
          kick = &previous_kick_;
          time = previous_kick_.elapsed - lookahead - nudge_frames_ + preroll_frames_;
        }
        read = {std::min(time / kick_cycle(*kick), 1.0), kick->index, 1.0, true};
        const double length = kick_cycle(kick_);
        if (now >= 0.0 && now < length)
          heard = now / length;
        if (const double captured = (kick_.elapsed - nudge_frames_) / length;
            captured >= 0.0 && captured < 1.0)
          capture_.add(captured, key);
        kick_.elapsed = std::min(kick_.elapsed + 1.0, idle_frames);
        previous_kick_.elapsed = std::min(previous_kick_.elapsed + 1.0, idle_frames);
      }
      heard_ = heard;
      ++frame_;

      // Each band's gain: its shape through its depth, the smoother and
      // the mix.
      const std::array<double, shape_bands> depths{depth_.next(), depth_mid_.next(),
                                                   depth_high_.next()};
      const double morph = morph_.next();
      const double mix = mix_.next();
      std::array<double, shape_bands> gain{};
      if (read.active)
        playing_slot_ =
            slot_mode_ == pump_table::morph ? -1 : static_cast<int>(slot_for(read.index));
      for (std::size_t b = 0; b < shape_bands; ++b) {
        const double shape = read.active ? shape_at(read, b, morph) : 1.0;
        const double target = 1.0 - depths[b] * read.scale * (1.0 - shape);
        gain[b] = 1.0 - mix * (1.0 - smoothers_[b].next(target));
      }
      gain_ = gain;
      if (read.active)
        step_ = static_cast<int>(modulo(read.index, pattern_length_));

      // The mid and side each take the gain as far as Stereo says. On a
      // pair of channels that is a 2x2 matrix per band; with both applied
      // it is the plain gain, exactly.
      const double to_mid = apply_mid_.next();
      const double to_side = apply_side_.next();
      std::array<double, shape_bands> same{}, cross{};
      const auto applied = [](double amount, double g) {
        return amount == 1.0 ? g : 1.0 - amount * (1.0 - g);
      };
      for (std::size_t b = 0; b < shape_bands; ++b) {
        const double mid = applied(to_mid, gain[b]);
        const double side = applied(to_side, gain[b]);
        same[b] = 0.5 * (mid + side);
        cross[b] = 0.5 * (mid - side);
      }
      const auto pumped = [&](std::size_t b, double l, double r, double& out_l, double& out_r) {
        out_l += same[b] * l + cross[b] * r;
        out_r += same[b] * r + cross[b] * l;
      };

      const double w_full = band_weight_[0].next();
      const double w_lows = band_weight_[1].next();
      const double w_three = band_weight_[2].next();
      double full_l = 0.0, full_r = 0.0;
      pumped(0, left, right, full_l, full_r);
      double l = w_full * full_l;
      double r = w_full * full_r;
      // The splits always run, so switching band only fades.
      double low_l = 0.0, high_l = 0.0, low_r = 0.0, high_r = 0.0;
      crossover_.split(0, left, low_l, high_l);
      crossover_.split(1, right, low_r, high_r);
      double mid_l = 0.0, top_l = 0.0, mid_r = 0.0, top_r = 0.0;
      high_split_.split(0, high_l, mid_l, top_l);
      high_split_.split(1, high_r, mid_r, top_r);
      // The lows through the high crossover's allpass, in phase with the
      // mids and highs.
      double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
      low_allpass_.split(0, low_l, a, b);
      low_allpass_.split(1, low_r, c, d);
      if (w_lows > 0.0) {
        double lows_l = high_l, lows_r = high_r;
        pumped(0, low_l, low_r, lows_l, lows_r);
        l += w_lows * lows_l;
        r += w_lows * lows_r;
      }
      if (w_three > 0.0) {
        double three_l = 0.0, three_r = 0.0;
        pumped(0, a + b, c + d, three_l, three_r);
        pumped(1, mid_l, mid_r, three_l, three_r);
        pumped(2, top_l, top_r, three_l, three_r);
        l += w_three * three_l;
        r += w_three * three_r;
      }
      // Listen: what the detector hears, in place of the output.
      const double listen = listen_.next();
      if (listen > 0.0) {
        l += listen * (listen_l - l);
        r += listen * (listen_r - r);
      }
      const double out = output_.next();
      audio[i] = {static_cast<float>(l * out), static_cast<float>(r * out)};
      if (heard >= 0.0) {
        input_.add(heard, {static_cast<float>(left), static_cast<float>(right)});
        output_capture_.add(heard, audio[i]);
      }
    }
  }

  void reset() noexcept override {
    for (auto& line : delay_)
      line.reset();
    for (auto& line : listen_delay_)
      line.reset();
    crossover_.reset();
    high_split_.reset();
    low_allpass_.reset();
    detect_.reset();
    detector_.reset();
    onset_detector_.reset();
    since_onset_ = idle_frames;
    capture_.reset();
    input_.reset();
    output_capture_.reset();
    kick_ = previous_kick_ = {};
    cursors_dirty_ = true;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size)
      return 0;
    const auto db = [](double gain) {
      return static_cast<float>(20.0 * std::log10(std::max(gain, 1e-6)));
    };
    out[telemetry::phase] = static_cast<float>(heard_);
    out[telemetry::gain_db] = db(gain_[0]);
    out[telemetry::cycle_seconds] = trigger_ == pump_table::tempo && !tempo_known_
                                        ? 0.0F
                                        : static_cast<float>(cycle_frames_ / rate_);
    out[telemetry::keyed] = keyed_ ? 1.0F : 0.0F;
    std::ranges::copy(capture_.shown(), out.begin() + telemetry::kick);
    std::ranges::copy(view_, out.begin() + telemetry::applied);
    out[telemetry::cycle_beats] = static_cast<float>(cycle_beats_);
    std::ranges::copy(input_.shown(), out.begin() + telemetry::input);
    std::ranges::copy(output_capture_.shown(), out.begin() + telemetry::output);
    out[telemetry::last_note] = static_cast<float>(last_note_);
    out[telemetry::step] = static_cast<float>(step_);
    out[telemetry::slot] = static_cast<float>(playing_slot_);
    out[telemetry::gain_mid_db] = db(gain_[1]);
    out[telemetry::gain_high_db] = db(gain_[2]);
    return telemetry::size;
  }

private:
  // Frames since a kick saturate here: long past any cycle's end.
  static constexpr double idle_frames = 1e12;

  // Where a frame reads the shapes: the phase in its cycle, the cycle's
  // count (the pattern step), and its velocity's depth scale.
  struct Read {
    double phase = 0.0;
    std::int64_t index = 0;
    double scale = 1.0;
    bool active = false;
  };
  struct Position {
    double phase;
    std::int64_t index;
  };
  // Kick Audio: frames since a kick, the gap before it (0 when it did not
  // look like the beat) and its count.
  struct Kick {
    double elapsed = idle_frames;
    double spacing = 0.0;
    std::int64_t index = 0;
  };

  void set_filters(std::span<const double> v) noexcept {
    if (v[p::crossover] != crossover_hz_) {
      crossover_hz_ = v[p::crossover];
      crossover_.set(crossover_hz_, rate_);
    }
    if (v[p::crossover_high] != crossover_high_hz_) {
      crossover_high_hz_ = v[p::crossover_high];
      high_split_.set(crossover_high_hz_, rate_);
      low_allpass_.set(crossover_high_hz_, rate_);
    }
    if (v[p::detect_low] != detect_low_hz_ || v[p::detect_high] != detect_high_hz_) {
      detect_low_hz_ = v[p::detect_low];
      detect_high_hz_ = v[p::detect_high];
      detect_.set(detect_low_hz_, detect_high_hz_, rate_);
    }
  }

  // Tempo: the cycle at a song position, the second of each pair late by
  // the swing.
  [[nodiscard]] Position tempo_cycle(double units) const noexcept {
    if (swing_ <= 0.0) {
      const double cycles = units / cycle_units_;
      return {wrap(cycles), static_cast<std::int64_t>(std::floor(cycles))};
    }
    const double pair = 2.0 * cycle_units_;
    const double k = std::floor(units / pair);
    const double place = (units - k * pair) / cycle_units_;
    const double first = 1.0 + swing_;
    const auto base = 2 * static_cast<std::int64_t>(k);
    if (place < first)
      return {place / first, base};
    return {std::min((place - first) / (1.0 - swing_), 1.0), base + 1};
  }

  [[nodiscard]] std::size_t slot_for(std::int64_t index) const noexcept {
    if (slot_mode_ == pump_table::pattern)
      return steps_[static_cast<std::size_t>(modulo(index, pattern_length_))];
    if (slot_mode_ == pump_table::morph)
      return 0;
    return static_cast<std::size_t>(slot_mode_ - pump_table::slot_a);
  }

  [[nodiscard]] double shape_at(const Read& read, std::size_t band, double morph) const noexcept {
    if (slot_mode_ == pump_table::morph) {
      const double a = state_->at(0, band).at(read.phase);
      return a + morph * (state_->at(1, band).at(read.phase) - a);
    }
    return state_->at(slot_for(read.index), band).at(read.phase);
  }

  [[nodiscard]] double velocity_scale(std::uint8_t velocity) const noexcept {
    return 1.0 - velocity_amount_ * (1.0 - static_cast<double>(velocity) / 127.0);
  }

  // MIDI: the cycle after the cursor's note, stretched to the next note
  // when following the kick's spacing (not past four cycles: a break).
  [[nodiscard]] double note_cycle(const NoteCursor& cursor) const noexcept {
    if (follow_) {
      if (const auto* next = cursor.next(); next != nullptr) {
        const auto gap = static_cast<double>(next->frame - cursor.last()->frame);
        if (gap > 0.0 && gap <= 4.0 * cycle_frames_)
          return gap;
      }
    }
    return cycle_frames_;
  }

  [[nodiscard]] double kick_cycle(const Kick& kick) const noexcept {
    return follow_ && kick.spacing > 0.0 ? kick.spacing : cycle_frames_;
  }

  // Kick Audio, heard `since` frames after its onset: the gap since the
  // last kick predicts this one's cycle when it is within a quarter and
  // four times the rate's.
  void on_kick(double since) noexcept {
    const double gap = kick_.elapsed - since;
    previous_kick_ = kick_;
    kick_.elapsed = since;
    kick_.index = previous_kick_.index + 1;
    kick_.spacing = gap >= 0.25 * cycle_frames_ && gap <= 4.0 * cycle_frames_ ? gap : 0.0;
  }

  // The applied curve: two cycles of the playing slot's (or the morph's)
  // low band through depth, mix and the smoother (with its preroll), the
  // second kept, as the panel's "what you hear" line.
  void update_view() noexcept {
    view_dirty_ = false;
    const double step_seconds = cycle_frames_ / rate_ / static_cast<double>(view_steps);
    const double c = 1.0 - std::exp(-step_seconds / std::max(view_tau_, 1e-9));
    const double preroll = smoothers_[0].delay_seconds() * rate_ / cycle_frames_;
    const std::size_t slot = view_slot_ < 0 ? 0 : static_cast<std::size_t>(view_slot_);
    const auto shape = [&](double phase) {
      const double a = state_->at(slot, 0).at(phase);
      return view_slot_ < 0 ? a + view_morph_ * (state_->at(1, 0).at(phase) - a) : a;
    };
    double a = 1.0, b = 1.0;
    for (std::size_t pass = 0; pass < 2; ++pass) {
      for (std::size_t k = 0; k < view_steps; ++k) {
        const double phase = static_cast<double>(k) / static_cast<double>(view_steps);
        const double target =
            1.0 - view_depth_ *
                      (1.0 - shape(trigger_ == pump_table::tempo ? wrap(phase + preroll)
                                                                 : std::min(phase + preroll, 1.0)));
        a += c * (target - a);
        b += c * (a - b);
        if (pass == 1 && k % (view_steps / view_points) == 0)
          view_[k / (view_steps / view_points)] = static_cast<float>(1.0 - view_mix_ * (1.0 - b));
      }
    }
  }

  double rate_;
  std::size_t lookahead_;
  double band_steps_;
  const PumpState* state_ = &default_state();
  std::array<dsp::Delay, 2> delay_;
  std::array<dsp::Delay, 2> listen_delay_;
  std::array<GainSmoother, shape_bands> smoothers_;
  KickDetector detector_;
  KickDetector onset_detector_;
  double since_onset_ = idle_frames;
  double confirm_frames_ = 0.0;
  DetectFilter detect_;
  Crossover crossover_;
  Crossover high_split_;
  Crossover low_allpass_;
  double crossover_hz_ = 0.0;
  double crossover_high_hz_ = 0.0;
  double detect_low_hz_ = 0.0;
  double detect_high_hz_ = 0.0;
  KickCapture capture_;
  KickCapture input_;
  KickCapture output_capture_;
  native_dsp::dsp::Ramp depth_;
  native_dsp::dsp::Ramp depth_mid_;
  native_dsp::dsp::Ramp depth_high_;
  native_dsp::dsp::Ramp morph_;
  native_dsp::dsp::Ramp mix_;
  native_dsp::dsp::Ramp output_;
  std::array<native_dsp::dsp::Ramp, 3> band_weight_;
  native_dsp::dsp::Ramp apply_mid_;
  native_dsp::dsp::Ramp apply_side_;
  native_dsp::dsp::Ramp listen_;
  int trigger_ = pump_table::tempo;
  bool tempo_known_ = false;
  double cycle_units_ = 1.0;
  double cycle_beats_ = 1.0;
  double units_per_frame_ = 0.0;
  double position_ = 0.0;
  double cycle_frames_ = 24'000.0;
  double nudge_frames_ = 0.0;
  double preroll_frames_ = 0.0;
  double swing_ = 0.0;
  double velocity_amount_ = 0.0;
  bool follow_ = false;
  int slot_mode_ = pump_table::pattern;
  std::int64_t pattern_length_ = 1;
  std::array<std::size_t, pump_table::max_pattern> steps_{};
  Kick kick_;
  Kick previous_kick_;
  std::span<const KeyNote> notes_;
  NoteCursor look_;
  NoteCursor heard_cursor_;
  NoteCursor capture_cursor_;
  int note_low_ = 0;
  int note_high_ = 127;
  bool cursors_dirty_ = true;
  int last_note_ = -1;
  double heard_ = -1.0;
  std::int64_t frame_ = 0;
  std::array<double, shape_bands> gain_{1.0, 1.0, 1.0};
  int step_ = 0;
  int playing_slot_ = 0;
  bool keyed_ = false;
  bool primed_ = false;
  bool view_dirty_ = true;
  double view_depth_ = -1.0;
  double view_mix_ = -1.0;
  double view_tau_ = -1.0;
  int view_slot_ = 0;
  double view_morph_ = 0.0;
  double view_cycle_frames_ = -1.0;
  std::array<float, view_points> view_{};
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  return std::make_unique<PumpKernel>(values, sample_rate);
}

// The lookahead line empties after the input stops, and the crossover's
// ringing (Lows Only, down to 40 Hz) dies within 100 ms; the gain is a level.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  return lookahead_frames(values, sample_rate) +
         static_cast<std::uint64_t>(std::ceil(0.1 * sample_rate));
}

std::uint32_t latency_frames(std::span<const double> values, double sample_rate) {
  return static_cast<std::uint32_t>(lookahead_frames(values, sample_rate));
}

const EngineDescriptor descriptor{"pump", "Pump", 1, pump_table::parameters, pump_presets::presets};
const EffectEngine pump_engine{
    &descriptor, &create_kernel,     &tail_frames,      &latency_frames,  true,
    false,       &validate_document, &prepare_document, &preset_document, true};

} // namespace

Shape sample_shape(std::span<const project::NativeCurvePoint> points) {
  Shape shape;
  std::size_t segment = 0;
  for (std::size_t i = 0; i <= shape_resolution; ++i) {
    const double x = static_cast<double>(i) / shape_resolution;
    while (segment + 2 < points.size() && x >= points[segment + 1].x)
      ++segment;
    const auto& a = points[segment];
    const auto& b = points[std::min(segment + 1, points.size() - 1)];
    double y = a.y;
    if (a.segment == project::NativeCurveSegment::curve && b.x > a.x) {
      const double t = std::clamp((x - a.x) / (b.x - a.x), 0.0, 1.0);
      y = a.y + (b.y - a.y) * bend(t, a.tension);
    }
    shape.gain[i] = static_cast<float>(std::clamp(y, 0.0, 1.0));
  }
  return shape;
}

std::string curve_id(std::size_t slot, std::size_t band) {
  std::string id(shape_curve);
  if (slot > 0) {
    id += '.';
    id += static_cast<char>('a' + slot);
  }
  if (band == 1)
    id += ".mid";
  else if (band == 2)
    id += ".high";
  return id;
}

const Shape& default_shape() {
  static const Shape shape = [] {
    const auto document = shape_document(pump_presets::shapes.front().points);
    return sample_shape(document.curves.front().points);
  }();
  return shape;
}

const EffectEngine& engine() { return pump_engine; }

} // namespace pump
} // namespace oscillate::native_dsp::effects
