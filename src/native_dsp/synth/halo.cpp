#include "oscillate/native_dsp/synth/halo.hpp"

#include "dsp/ramp.hpp"
#include "halo_presets.hpp"
#include "halo_table.hpp"
#include "oscillate/native_dsp/synth/chorus.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/synth/dx_envelope.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/fm.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace oscillate::native_dsp::synth::halo {
namespace {

using halo_table::index_of;
using halo_table::operator_base;

namespace p {
// Offsets inside one operator block.
constexpr std::size_t mode = 0;
constexpr std::size_t ratio = 1;
constexpr std::size_t fine = 2;
constexpr std::size_t detune = 3;
constexpr std::size_t fixed = 4;
constexpr std::size_t level = 5;
constexpr std::size_t velocity = 6;
constexpr std::size_t key = 7;
constexpr std::size_t wave = 8;
constexpr std::size_t rate1 = 9; // Rates 1-4, then levels 1-4.
constexpr std::size_t level1 = 13;
constexpr std::size_t rate_scale = 17;
static_assert(index_of("halo.op1.r4") == operator_base(0) + rate1 + 3);
static_assert(index_of("halo.op1.l1") == operator_base(0) + level1);
static_assert(index_of("halo.op1.rate_scale") == operator_base(0) + rate_scale);
static_assert(index_of("halo.op6.mode") == operator_base(5) + mode);

constexpr std::size_t algorithm = index_of("halo.algorithm");
constexpr std::size_t feedback = index_of("halo.feedback");
constexpr std::size_t brightness = index_of("halo.brightness");
constexpr std::size_t filter_type = index_of("halo.filter.type");
constexpr std::size_t cutoff = index_of("halo.filter.cutoff");
constexpr std::size_t resonance = index_of("halo.filter.resonance");
constexpr std::size_t keytrack = index_of("halo.filter.keytrack");
constexpr std::size_t filter_velocity = index_of("halo.filter.velocity");
constexpr std::size_t lfo_shape = index_of("halo.lfo.shape");
constexpr std::size_t lfo_rate = index_of("halo.lfo.rate");
constexpr std::size_t lfo_sync = index_of("halo.lfo.sync");
constexpr std::size_t lfo_division = index_of("halo.lfo.division");
constexpr std::size_t lfo_delay = index_of("halo.lfo.delay");
constexpr std::size_t lfo_pitch = index_of("halo.lfo.pitch");
constexpr std::size_t lfo_amp = index_of("halo.lfo.amp");
constexpr std::size_t voice_mode = index_of("halo.voice.mode");
constexpr std::size_t unison = index_of("halo.voice.unison");
constexpr std::size_t unison_detune = index_of("halo.voice.detune");
constexpr std::size_t spread = index_of("halo.voice.spread");
constexpr std::size_t glide = index_of("halo.voice.glide");
constexpr std::size_t bend_range = index_of("halo.voice.bend");
constexpr std::size_t wheel = index_of("halo.perf.wheel");
constexpr std::size_t bend = index_of("halo.perf.bend");
constexpr std::size_t pressure = index_of("halo.perf.pressure");
constexpr std::size_t chorus = index_of("halo.fx.chorus");
constexpr std::size_t ensemble = index_of("halo.fx.ensemble");
constexpr std::size_t delay = index_of("halo.fx.delay");
constexpr std::size_t delay_time = index_of("halo.fx.delay_time");
constexpr std::size_t delay_feedback = index_of("halo.fx.delay_feedback");
constexpr std::size_t delay_sync = index_of("halo.fx.delay_sync");
constexpr std::size_t delay_division = index_of("halo.fx.delay_division");
constexpr std::size_t out_level = index_of("halo.out.level");
} // namespace p

enum class VoiceMode : std::uint8_t { poly, mono, legato };

constexpr std::size_t ops = fm_operators;
constexpr std::size_t max_frames = control_interval_frames;
constexpr std::size_t max_fast = 2 * max_frames; // Frames at the oversampled rate.
constexpr double voice_gain = 0.3;
constexpr double modulation_cycles = 2.0; // Phase shift of a full-level modulator.
constexpr double max_feedback_cycles = 0.45;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
[[nodiscard]] double clamp(double value, double low, double high) noexcept {
  return value < low ? low : (value > high ? high : value);
}
[[nodiscard]] double hz(double semitones) noexcept {
  return 440.0 * std::exp2((semitones - 69.0) / 12.0);
}

// Operator n's envelope from the parameter values.
[[nodiscard]] DxEnvelopeSettings envelope_settings(std::span<const double> v,
                                                   std::size_t n) noexcept {
  DxEnvelopeSettings e;
  const std::size_t base = operator_base(n);
  for (std::size_t i = 0; i < 4; ++i) {
    e.rates[i] = v[base + p::rate1 + i];
    e.levels[i] = v[base + p::level1 + i];
  }
  e.rate_scaling = as_int(v[base + p::rate_scale]);
  return e;
}

using Ramp = dsp::Ramp;

// One unison copy of the six operators; envelopes are shared per note.
struct Lane {
  std::array<FmOperator, ops> op{};
  std::array<Ramp, ops> increment{}; // Cycles per oversampled frame.
  double left = 1.0;
  double right = 1.0;
};

using AmpBlock = std::array<std::array<double, max_fast>, ops>;
using FastBlock = std::array<double, max_fast>;
using MixBlock = std::array<StereoSample, max_fast>;
using Waves = std::array<FmWave, ops>;
struct Routing;
// The operator section for one routing and unison count.
using OperatorKernel = void (*)(Routing&, const Waves&, const AmpBlock&, const FastBlock&,
                                MixBlock&, std::size_t) noexcept;

// One operator routing: an algorithm and unison count with its kernel, each
// operator's role (carrier normalisation or modulation index scale) and its
// own operator lanes. A voice keeps a second, fading routing while an
// algorithm or unison change crossfades on a sounding note (ADR-0038).
struct Routing {
  std::uint8_t algorithm = 0;
  std::uint32_t unison = 1;
  OperatorKernel kernel = nullptr;
  std::uint8_t carriers = 0;
  std::size_t feedback_op = 5;
  std::size_t feedback_source = 5; // The feedback operator itself, or a loop's source.
  double carrier_norm = 1.0;
  std::array<Ramp, ops> role{};
  Ramp out_scale{}; // 1 / sqrt(unison).
  std::array<Lane, unison_limit> lanes{};
};

struct Voice {
  bool active = false;
  bool held = false;
  NoteEvent note{};
  std::uint8_t key = 60;
  double velocity = 1.0;
  std::uint64_t age = 0;
  std::uint32_t slot = 0;

  Routing routing{};
  Routing fading{}; // The previous routing while routing_fade runs.
  SwitchFade routing_fade{};
  std::uint32_t fresh_lanes_from = unison_limit; // New lanes jump their ramps once.
  bool fresh_routing = false;                    // A new routing jumps its roles once.
  int filter_type = 0;
  int fading_filter_type = 0;
  StereoMultimodeFilter fading_filter{};
  FilterCoefficients fading_coefficients{};
  SwitchFade filter_fade{};

  double pitch = 60.0;
  double glide_target = 60.0;
  double glide_rate = 0.0;
  double seconds = 0.0;

  std::array<DxEnvelope, ops> env{};
  std::array<double, ops> env_previous{};
  std::array<Ramp, ops> gain{}; // Level x velocity x key scale; the role is per routing.
  Waves wave{};
  Ramp feedback{};
  Ramp out_gain{};

  Ramp g, k1, k2, ladder;
  StereoMultimodeFilter filter{};
  StereoSample last{};
};

[[nodiscard]] FilterMode filter_mode(int type) noexcept {
  switch (type) {
  case 1:
    return FilterMode::lowpass_12;
  case 3:
    return FilterMode::bandpass_12;
  case 4:
    return FilterMode::highpass_12;
  case 5:
    return FilterMode::ladder_24;
  default:
    return FilterMode::lowpass_24;
  }
}

// Unison lanes and both oversampled frames run interleaved: their operator
// chains are independent (only the feedback operator links one frame to the
// next), so the core overlaps them instead of waiting on one serial chain.
// The routing is a template too: operators unroll at compile time, only real
// connections are summed and the accumulators stay in registers.
template <std::size_t Algorithm, std::uint32_t Count>
void render_operators(Routing& routing, const Waves& waves, const AmpBlock& amp,
                      const FastBlock& feedback, MixBlock& mix, std::size_t frames) noexcept {
  constexpr FmAlgorithm algorithm = fm_algorithm_data[Algorithm];
  auto& lanes = routing.lanes;
  // Waves change at control rate; each operator crossfades a change itself.
  for (std::uint32_t u = 0; u < Count; ++u) {
    for (std::size_t n = 0; n < ops; ++n) {
      lanes[u].op[n].set_wave(waves[n]);
    }
  }
  for (std::size_t i = 0; i < frames; ++i) {
    std::array<std::array<std::uint32_t, ops>, Count> step;
    for (std::uint32_t u = 0; u < Count; ++u) {
      for (std::size_t n = 0; n < ops; ++n) {
        step[u][n] = FmWaveTables::to_fixed(lanes[u].increment[n].next());
      }
    }
    std::array<std::array<std::array<double, ops>, Count>, 2> in{};
    std::array<std::array<double, Count>, 2> sum{};
    const auto evaluate_half = [&]<std::size_t N>(std::size_t half) {
      constexpr std::uint8_t targets = algorithm.modulates[N];
      constexpr bool heard = ((algorithm.carriers >> N) & 1U) != 0U;
      const std::size_t s = 2 * i + half;
      const double level = amp[N][s];
      for (std::uint32_t u = 0; u < Count; ++u) {
        auto& op = lanes[u].op[N];
        double raw = 0.0;
        if constexpr (algorithm.loop() && N == algorithm.feedback) {
          // A loop: the chain's last operator, one frame late.
          const double loop = feedback[s] * lanes[u].op[algorithm.feedback_from].feedback_signal();
          raw = op.next(step[u][N], in[half][u][N] + loop);
        } else if constexpr (N == algorithm.feedback) {
          raw = op.next(step[u][N], in[half][u][N], feedback[s]);
        } else {
          raw = op.next(step[u][N], in[half][u][N]);
        }
        const double y = raw * level;
        if constexpr (heard) {
          sum[half][u] += y;
        }
        [&]<std::size_t... To>(std::index_sequence<To...>) {
          ((((targets >> To) & 1U) != 0U ? void(in[half][u][To] += y) : void()), ...);
        }(std::make_index_sequence<N>{});
      }
    };
    if constexpr (algorithm.loop()) {
      // A loop links operators across frames, so the halves run in time
      // order: the source's last output is always the previous frame.
      for (std::size_t half = 0; half < 2; ++half) {
        [&]<std::size_t... K>(std::index_sequence<K...>) {
          (evaluate_half.template operator()<ops - 1 - K>(half), ...);
        }(std::make_index_sequence<ops>{});
      }
    } else {
      // Six down to one: modulators always outnumber their targets. Both
      // halves of each operator together, so independent chains overlap.
      const auto evaluate = [&]<std::size_t N>() {
        evaluate_half.template operator()<N>(0);
        evaluate_half.template operator()<N>(1);
      };
      [&]<std::size_t... K>(std::index_sequence<K...>) {
        (evaluate.template operator()<ops - 1 - K>(), ...);
      }(std::make_index_sequence<ops>{});
    }
    for (std::size_t half = 0; half < 2; ++half) {
      for (std::uint32_t u = 0; u < Count; ++u) {
        mix[2 * i + half].left += sum[half][u] * lanes[u].left;
        mix[2 * i + half].right += sum[half][u] * lanes[u].right;
      }
    }
  }
}

template <std::uint32_t Count, std::size_t... Algorithm>
constexpr std::array<OperatorKernel, fm_algorithms>
kernel_row(std::index_sequence<Algorithm...>) noexcept {
  return {&render_operators<Algorithm, Count>...};
}

constexpr std::array<std::array<OperatorKernel, fm_algorithms>, unison_limit> operator_kernels{
    kernel_row<1>(std::make_index_sequence<fm_algorithms>{}),
    kernel_row<2>(std::make_index_sequence<fm_algorithms>{}),
    kernel_row<3>(std::make_index_sequence<fm_algorithms>{}),
    kernel_row<4>(std::make_index_sequence<fm_algorithms>{}),
};

class HaloKernel final : public SynthKernel {
public:
  explicit HaloKernel(double sample_rate)
      : rate_(sample_rate), control_rate_(sample_rate / control_interval_frames) {
    voices_ = std::make_unique<std::array<Voice, voice_limit>>();
    for (std::uint32_t i = 0; i < voice_limit; ++i) {
      auto& voice = (*voices_)[i];
      voice.slot = i;
      for (auto& env : voice.env) {
        env.prepare(rate_);
      }
    }
    lfo_.prepare(control_rate_);
    ensemble_.prepare(rate_);
    chorus_.prepare(rate_);
    delay_.prepare(rate_);
    dc_[0].prepare(rate_);
    dc_[1].prepare(rate_);
    reset_voices();
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    values_ = values;
    frames_ = frames_to_next_tick;
    beats_per_second_ = transport.beats_per_frame > 0.0 ? transport.beats_per_frame * rate_ : 2.0;
    const bool synced = values[p::lfo_sync] != 0.0;
    const auto division = static_cast<std::size_t>(values[p::lfo_division]);
    lfo_.set_shape(lfo_shape());
    if (synced && transport.beats_per_frame > 0.0) {
      lfo_value_ = lfo_.next_at(sync_cycles(division, transport));
    } else {
      const double rate = synced ? sync_hz(division, transport, rate_) : values[p::lfo_rate];
      lfo_value_ = lfo_.next(rate / control_rate_);
    }
    for (auto& voice : *voices_) {
      if (voice.active) {
        set_envelopes(voice);
        control_voice(voice, false);
      }
    }
    const auto ramp = frames_;
    ensemble_.set_amount(values[p::ensemble], ramp);
    chorus_.set(static_cast<ChorusMode>(as_int(values[p::chorus])), 1.0);
    const double delay_seconds =
        values[p::delay_sync] != 0.0
            ? sync_seconds(static_cast<std::size_t>(values[p::delay_division]), transport, rate_)
            : values[p::delay_time];
    delay_.set(delay_seconds, values[p::delay_feedback], values[p::delay], true, ramp);
    const double level =
        values[p::out_level] <= -59.9 ? 0.0 : std::pow(10.0, values[p::out_level] / 20.0);
    out_gain_.to(level, ramp, !primed_);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty()) {
      return;
    }
    const auto mode = static_cast<VoiceMode>(as_int(values_[p::voice_mode]));
    if (mode == VoiceMode::poly) {
      start(allocate(), event, true);
      last_pitch_ = event.key;
      return;
    }
    if (stack_size_ < stack_.size()) {
      stack_[stack_size_++] = event;
    }
    auto& voice = (*voices_)[0];
    const bool legato = mono_active_ && voice.active && voice.held;
    mono_active_ = true;
    start(voice, event, !(legato && mode == VoiceMode::legato));
    last_pitch_ = event.key;
  }

  void note_off(const NoteEvent& event) noexcept override {
    if (mono_active_) {
      mono_off(event);
      return;
    }
    for (auto& voice : *voices_) {
      if (voice.active && voice.held && voice.note.clip == event.clip &&
          voice.note.note == event.note) {
        release(voice);
      }
    }
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    const std::size_t frames = output.size();
    std::array<StereoSample, max_fast> bus{};
    // Voices retire only on the control grid, so the moment a filter tail is
    // cut never depends on how the host split its blocks.
    const bool ends_tick = frames >= frames_;
    for (auto& voice : *voices_) {
      if (voice.active) {
        render_voice(voice, bus, frames, ends_tick);
      }
    }
    for (std::size_t i = 0; i < frames; ++i) {
      for (std::size_t s = 2 * i; s < 2 * i + 2; ++s) {
        declick_.add(bus[s]);
      }
      const StereoSample x = down_.process(bus[2 * i], bus[2 * i + 1]);
      const auto ensembled = ensemble_.process(x.left, x.right);
      const auto chorused = chorus_.process(ensembled.left, ensembled.right);
      const auto delayed = delay_.process(chorused.left, chorused.right);
      const double gain = out_gain_.next();
      output[i].left += static_cast<float>(safety(dc_[0].process(delayed.left) * gain));
      output[i].right += static_cast<float>(safety(dc_[1].process(delayed.right) * gain));
    }
    frames_ -= static_cast<std::uint32_t>(std::min<std::size_t>(frames, frames_));
  }

  void reset() noexcept override {
    for (const auto& voice : *voices_) {
      if (voice.active) {
        capture_declick(voice);
      }
    }
    reset_voices();
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    status.voice_steals = steals_;
    for (const auto& voice : *voices_) {
      if (voice.active) {
        ++status.active_voices;
      }
    }
    return status;
  }

private:
  [[nodiscard]] LfoShape lfo_shape() const noexcept {
    switch (as_int(values_[p::lfo_shape])) {
    case 0:
      return LfoShape::sine;
    case 2:
      return LfoShape::saw_up;
    case 3:
      return LfoShape::square;
    case 4:
      return LfoShape::sample_hold;
    default:
      return LfoShape::triangle;
    }
  }

  [[nodiscard]] double op(std::size_t n, std::size_t offset) const noexcept {
    return values_[operator_base(n) + offset];
  }

  void reset_voices() noexcept {
    for (auto& voice : *voices_) {
      voice.active = false;
      voice.held = false;
    }
    lfo_.reset(0.0, 0x4A10U);
    down_.reset();
    next_slot_ = 0;
    clock_ = 0;
    mono_active_ = false;
    stack_size_ = 0;
    last_pitch_ = -1.0;
  }

  // ---- Allocation ------------------------------------------------------------

  Voice& allocate() noexcept {
    return allocate_voice(*voices_, next_slot_, [this](const Voice& voice) {
      capture_declick(voice);
      ++steals_;
    });
  }

  // The declick fades a stolen voice's last oversampled output, before the
  // shared decimator, so it is band-limited like everything else.
  void capture_declick(const Voice& voice) noexcept { declick_.capture(voice.last); }

  void release(Voice& voice) noexcept {
    voice.held = false;
    for (auto& env : voice.env) {
      env.gate_off();
    }
  }

  void mono_off(const NoteEvent& event) noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < stack_size_; ++i) {
      if (!(stack_[i].clip == event.clip && stack_[i].note == event.note)) {
        stack_[kept++] = stack_[i];
      }
    }
    stack_size_ = kept;
    auto& voice = (*voices_)[0];
    if (!(voice.note.clip == event.clip && voice.note.note == event.note)) {
      return;
    }
    if (stack_size_ > 0) {
      // Fall back to the newest held note without retriggering.
      start(voice, stack_[stack_size_ - 1], false);
      return;
    }
    release(voice);
    mono_active_ = false;
  }

  void set_envelopes(Voice& voice) noexcept {
    for (std::size_t n = 0; n < ops; ++n) {
      voice.env[n].set(envelope_settings(values_, n), voice.key);
    }
  }

  static void build_routing(Routing& routing, std::uint8_t algorithm_index,
                            std::uint32_t unison) noexcept {
    const auto& algorithm = fm_algorithm_table()[algorithm_index];
    routing.algorithm = algorithm_index;
    routing.unison = unison;
    routing.carriers = algorithm.carriers;
    routing.feedback_op = algorithm.feedback;
    routing.feedback_source = algorithm.feedback_from;
    routing.carrier_norm = 1.0 / std::sqrt(static_cast<double>(std::popcount(algorithm.carriers)));
    routing.kernel = operator_kernels[unison - 1][algorithm_index];
  }

  void start(Voice& voice, const NoteEvent& event, bool retrigger) noexcept {
    const auto v = values_;
    const bool continuing = voice.active;
    voice.active = true;
    voice.held = true;
    voice.note = event;
    voice.key = event.key;
    voice.velocity = event.velocity / 127.0;
    voice.age = ++clock_;
    voice.glide_target = event.key;
    const double glide_time = v[p::glide];
    if (glide_time > 0.0 && (continuing || last_pitch_ >= 0.0)) {
      if (!continuing) {
        voice.pitch = last_pitch_;
      }
      voice.glide_rate =
          std::fabs(voice.glide_target - voice.pitch) / std::max(1.0, glide_time * control_rate_);
    } else {
      voice.pitch = event.key;
      voice.glide_rate = 0.0;
    }
    if (!continuing) {
      voice.filter_type = as_int(v[p::filter_type]);
      build_routing(voice.routing, static_cast<std::uint8_t>(v[p::algorithm]),
                    static_cast<std::uint32_t>(v[p::unison]));
      voice.routing_fade.stop();
      voice.filter_fade.stop();
      voice.fresh_lanes_from = unison_limit;
      // Key sync: every operator starts at phase zero, so attacks repeat.
      for (auto& lane : voice.routing.lanes) {
        for (auto& op : lane.op) {
          op.reset();
        }
      }
      for (std::size_t n = 0; n < ops; ++n) {
        voice.env[n].reset();
        voice.env_previous[n] = 0.0;
      }
      voice.filter.reset();
      voice.last = {};
    }
    set_envelopes(voice);
    if (retrigger || !continuing) {
      for (auto& env : voice.env) {
        env.gate_on();
      }
      voice.seconds = 0.0;
    }
    control_voice(voice, !continuing);
  }

  // ---- Control rate ----------------------------------------------------------

  void control_voice(Voice& voice, bool jump) noexcept {
    const auto v = values_;
    const std::uint32_t frames = frames_;
    if (voice.glide_rate > 0.0) {
      const double distance = voice.glide_target - voice.pitch;
      if (std::fabs(distance) <= voice.glide_rate) {
        voice.pitch = voice.glide_target;
        voice.glide_rate = 0.0;
      } else {
        voice.pitch += distance > 0.0 ? voice.glide_rate : -voice.glide_rate;
      }
    }
    voice.seconds += 1.0 / control_rate_;
    const double delay = v[p::lfo_delay];
    const double lfo = lfo_value_ * (delay <= 0.0 ? 1.0 : std::min(1.0, voice.seconds / delay));

    const double vibrato = lfo * (v[p::lfo_pitch] + 0.5 * v[p::wheel]) * 2.0;
    const double pitch = voice.pitch + v[p::bend] * v[p::bend_range] + vibrato;
    const double note_hz = hz(pitch);
    // Brightness and pressure scale every modulator, so one control opens
    // or closes the whole patch's timbre.
    const double bright = std::exp2(1.5 * clamp(v[p::brightness] + 0.5 * v[p::pressure], -1, 1.5));

    // Structural changes reach sounding notes (ADR-0038): the old routing
    // keeps rendering, warm, and crossfades out. A change that arrives while
    // a fade runs waits for it, so no fade is ever cut short.
    const auto algorithm = static_cast<std::uint8_t>(v[p::algorithm]);
    const auto unison = static_cast<std::uint32_t>(v[p::unison]);
    if (!jump && !voice.routing_fade.active() &&
        (algorithm != voice.routing.algorithm || unison != voice.routing.unison)) {
      voice.fading = voice.routing;
      const std::uint32_t had = voice.routing.unison;
      build_routing(voice.routing, algorithm, unison);
      for (std::uint32_t u = had; u < unison; ++u) {
        for (auto& op : voice.routing.lanes[u].op) {
          op.reset();
        }
      }
      voice.fresh_lanes_from = had;
      voice.fresh_routing = true;
      voice.routing_fade.start();
    }

    std::array<double, ops> op_hz{};
    for (std::size_t n = 0; n < ops; ++n) {
      const double ratio_choice = op(n, p::ratio);
      const double ratio = (ratio_choice == 0.0 ? 0.5 : ratio_choice) * (1.0 + op(n, p::fine));
      const double base = op(n, p::mode) != 0.0 ? op(n, p::fixed) : note_hz * ratio;
      op_hz[n] = base * std::exp2(op(n, p::detune) / 1'200.0);

      const double sensitivity = op(n, p::velocity);
      const double velocity_gain =
          1.0 - sensitivity + sensitivity * voice.velocity * voice.velocity;
      // Key scale: +1 is -6 dB per octave above C4 (+6 dB below, capped).
      const double key_gain =
          clamp(std::exp2(-op(n, p::key) * (voice.key - 60.0) / 12.0), 0.0, 4.0);
      voice.gain[n].to(op(n, p::level) * velocity_gain * key_gain, frames, jump);
      voice.wave[n] = static_cast<FmWave>(as_int(op(n, p::wave)));
    }
    voice.feedback.to(v[p::feedback] * max_feedback_cycles * FmWaveTables::fixed_scale, frames,
                      jump);
    const double index_scale = modulation_cycles * FmWaveTables::fixed_scale * bright;
    // A new routing's roles and scale jump to their values (an operator that
    // was a modulator must not ramp down from an index to a carrier level);
    // the crossfade already brings the routing in smoothly.
    control_routing(voice.routing, op_hz, index_scale, frames, jump || voice.fresh_routing,
                    voice.fresh_lanes_from);
    voice.fresh_lanes_from = unison_limit;
    voice.fresh_routing = false;
    if (voice.routing_fade.active()) {
      control_routing(voice.fading, op_hz, index_scale, frames, false, unison_limit);
    }

    // Filter: a type change crossfades from a warm copy of the old filter.
    const int type = as_int(v[p::filter_type]);
    if (!jump && !voice.filter_fade.active() && type != voice.filter_type) {
      voice.fading_filter = voice.filter;
      voice.fading_filter_type = voice.filter_type;
      voice.filter_type = type;
      voice.filter_fade.start();
    }
    const double semitones =
        v[p::keytrack] * (voice.key - 60.0) + 36.0 * v[p::filter_velocity] * voice.velocity;
    const double cutoff_hz = clamp(v[p::cutoff] * std::exp2(semitones / 12.0), 20.0, 20'000.0);
    if (voice.filter_type != 0) {
      const auto c = make_filter_coefficients(filter_mode(voice.filter_type),
                                              cutoff_hz / (2.0 * rate_), v[p::resonance], 1.0);
      voice.g.to(c.g, frames, jump);
      voice.k1.to(c.k1, frames, jump);
      voice.k2.to(c.k2, frames, jump);
      voice.ladder.to(c.feedback, frames, jump);
    }
    if (voice.filter_fade.active() && voice.fading_filter_type != 0) {
      voice.fading_coefficients = make_filter_coefficients(
          filter_mode(voice.fading_filter_type), cutoff_hz / (2.0 * rate_), v[p::resonance], 1.0);
    }

    const double tremolo = 1.0 - v[p::lfo_amp] * 0.5 * (1.0 + lfo);
    voice.out_gain.to(voice_gain * tremolo, frames, jump);
  }

  // Roles, lane increments and stereo spread for one routing.
  void control_routing(Routing& routing, const std::array<double, ops>& op_hz, double index_scale,
                       std::uint32_t frames, bool jump, std::uint32_t fresh_from) noexcept {
    const auto& v = values_;
    for (std::size_t n = 0; n < ops; ++n) {
      // Carriers share the mix; a modulator's level is its index, in fixed
      // phase units.
      const bool carrier = ((static_cast<unsigned>(routing.carriers) >> n) & 1U) != 0U;
      routing.role[n].to(carrier ? routing.carrier_norm : index_scale, frames, jump);
    }
    // Unison never exceeds the lanes a routing owns.
    const std::uint32_t count = std::min<std::uint32_t>(routing.unison, unison_limit);
    routing.out_scale.to(1.0 / std::sqrt(static_cast<double>(count)), frames, jump);
    // Unison lanes spread in pitch and across the stereo field.
    for (std::uint32_t u = 0; u < count; ++u) {
      auto& lane = routing.lanes[u];
      const double position = count == 1 ? 0.0 : -1.0 + 2.0 * u / static_cast<double>(count - 1);
      const double factor = std::exp2(position * v[p::unison_detune] / 2.0 / 1'200.0);
      for (std::size_t n = 0; n < ops; ++n) {
        lane.increment[n].to(op_hz[n] * factor / (2.0 * rate_), frames, jump || u >= fresh_from);
      }
      const double pan = v[p::spread] * position * (u % 2 == 0 ? 1.0 : -1.0);
      lane.left = pan > 0.0 ? 1.0 - pan : 1.0;
      lane.right = pan < 0.0 ? 1.0 + pan : 1.0;
    }
  }

  // ---- Audio rate ------------------------------------------------------------

  void render_voice(Voice& voice, std::array<StereoSample, max_fast>& bus, std::size_t frames,
                    bool ends_tick) noexcept {
    // Envelope x base gain per operator at the oversampled rate (envelopes
    // run at the base rate and are interpolated linearly between frames),
    // the raw envelopes per frame for feedback, and the feedback amount.
    AmpBlock base;
    std::array<std::array<double, max_frames>, ops> envs;
    std::array<double, max_frames> feedback_amount;
    for (std::size_t n = 0; n < ops; ++n) {
      double previous = voice.env_previous[n];
      for (std::size_t i = 0; i < frames; ++i) {
        const double env = voice.env[n].next();
        const double now = env * voice.gain[n].next();
        base[n][2 * i] = 0.5 * (previous + now);
        base[n][2 * i + 1] = now;
        envs[n][i] = env;
        previous = now;
      }
      voice.env_previous[n] = previous;
    }
    for (std::size_t i = 0; i < frames; ++i) {
      feedback_amount[i] = voice.feedback.next();
    }

    MixBlock mix{};
    render_routing(voice.routing, voice.wave, base, envs, feedback_amount, mix, frames);
    if (voice.routing_fade.active()) {
      MixBlock old{};
      render_routing(voice.fading, voice.wave, base, envs, feedback_amount, old, frames);
      for (std::size_t i = 0; i < frames; ++i) {
        const double weight = voice.routing_fade.next();
        for (std::size_t s = 2 * i; s < 2 * i + 2; ++s) {
          mix[s] = mix[s] + (old[s] - mix[s]) * weight;
        }
      }
    }

    const bool filtered = voice.filter_type != 0;
    const bool fading = voice.filter_fade.active();
    if (filtered || fading) {
      const auto mode = filter_mode(voice.filter_type);
      for (std::size_t i = 0; i < frames; ++i) {
        FilterCoefficients c;
        c.mode = mode;
        c.g = voice.g.next();
        c.k1 = voice.k1.next();
        c.k2 = voice.k2.next();
        c.feedback = voice.ladder.next();
        c.finalise();
        const double weight = fading ? voice.filter_fade.next() : 0.0;
        for (std::size_t s = 2 * i; s < 2 * i + 2; ++s) {
          const StereoSample in = mix[s];
          const StereoSample now = filtered ? voice.filter.process(in, c) : in;
          if (fading) {
            const StereoSample was =
                voice.fading_filter_type != 0
                    ? voice.fading_filter.process(in, voice.fading_coefficients)
                    : in;
            mix[s] = now + (was - now) * weight;
          } else {
            mix[s] = now;
          }
        }
      }
    }

    for (std::size_t i = 0; i < frames; ++i) {
      const double gain = voice.out_gain.next();
      for (std::size_t s = 2 * i; s < 2 * i + 2; ++s) {
        voice.last = mix[s] * gain;
        bus[s] = bus[s] + voice.last;
      }
    }

    if (!ends_tick) {
      return;
    }
    bool sounding = false;
    for (std::size_t n = 0; n < ops; ++n) {
      sounding = sounding || (((static_cast<unsigned>(voice.routing.carriers) >> n) & 1U) != 0U &&
                              voice.env[n].active());
    }
    // A resonant filter rings on after its input stops.
    sounding = sounding || ((filtered || fading) && (std::fabs(voice.last.left) > 1e-6 ||
                                                     std::fabs(voice.last.right) > 1e-6));
    if (!sounding) {
      voice.active = false;
      voice.held = false;
      voice.last = {};
    }
  }

  // One routing's operators: its roles turn the shared envelopes into
  // carrier levels and modulation indices, and the envelope of the operator
  // whose output is fed back (the feedback operator, or a loop's source)
  // sets the feedback depth, as on the classic chips.
  static void render_routing(Routing& routing, const Waves& waves, const AmpBlock& base,
                             const std::array<std::array<double, max_frames>, ops>& envs,
                             const std::array<double, max_frames>& feedback_amount, MixBlock& out,
                             std::size_t frames) noexcept {
    AmpBlock amp{};
    FastBlock feedback{};
    for (std::size_t n = 0; n < ops; ++n) {
      for (std::size_t i = 0; i < frames; ++i) {
        const double role = routing.role[n].next();
        amp[n][2 * i] = base[n][2 * i] * role;
        amp[n][2 * i + 1] = base[n][2 * i + 1] * role;
      }
    }
    const auto& fb_env = envs[routing.feedback_source];
    for (std::size_t i = 0; i < frames; ++i) {
      const double depth = feedback_amount[i] * fb_env[i];
      feedback[2 * i] = depth;
      feedback[2 * i + 1] = depth;
    }
    routing.kernel(routing, waves, amp, feedback, out, frames);
    for (std::size_t i = 0; i < frames; ++i) {
      const double scale = routing.out_scale.next();
      out[2 * i] = out[2 * i] * scale;
      out[2 * i + 1] = out[2 * i + 1] * scale;
    }
  }

  [[nodiscard]] static double safety(double x) noexcept {
    constexpr double knee = 0.9;
    const double magnitude = std::fabs(x);
    if (magnitude <= knee) {
      return x;
    }
    const double shaped = knee + (1.0 - knee) * soft_clip((magnitude - knee) / (1.0 - knee) * 3.0);
    return x < 0.0 ? -shaped : shaped;
  }

  double rate_;
  double control_rate_;
  std::unique_ptr<std::array<Voice, voice_limit>> voices_;
  std::span<const double> values_{};
  std::uint32_t frames_ = 0;
  double beats_per_second_ = 2.0;
  Lfo lfo_{};
  double lfo_value_ = 0.0;
  std::uint32_t next_slot_ = 0;
  std::uint64_t clock_ = 0;
  std::uint32_t steals_ = 0;
  bool mono_active_ = false;
  std::array<NoteEvent, 16> stack_{};
  std::size_t stack_size_ = 0;
  double last_pitch_ = -1.0;
  Declick declick_{2 * declick_frames};
  StereoDownsampler2x down_{};
  Ensemble ensemble_{};
  Chorus chorus_{};
  BbdDelay delay_{};
  Ramp out_gain_{};
  bool primed_ = false;
  std::array<DcBlocker, 2> dc_{};
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<HaloKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  // The slowest release: from full level to L4 at R4, on the lowest key
  // (rate scaling only ever speeds a segment up). A sounding L4 holds after
  // that, as on the hardware, so the bound covers the fall.
  double release = 0.0;
  for (std::size_t n = 0; n < ops; ++n) {
    const auto e = envelope_settings(values, n);
    release = std::max(release, DxEnvelope::segment_seconds(DxEnvelope::full_units,
                                                            DxEnvelope::level_units(e.levels[3]),
                                                            e.rates[3], 0, 0));
  }
  const double delay_seconds =
      values[p::delay] > 0.0
          ? BbdDelay::tail_seconds(BbdDelay::max_seconds, values[p::delay_feedback])
          : 0.0;
  const double seconds =
      release + delay_seconds + Ensemble::tail_seconds + Chorus::tail_seconds + 0.05;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

const EngineDescriptor descriptor{"halo", "Halo", 1, halo_table::parameters, halo_presets::presets};
const SynthEngine halo_engine{&descriptor, &create_kernel, &tail_frames};

} // namespace

const SynthEngine& engine() { return halo_engine; }

} // namespace oscillate::native_dsp::synth::halo
