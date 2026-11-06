#include "voice.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::native_dsp::synth::nova {
namespace {

// Headroom for three full oscillators, sub and noise into the filters.
constexpr double voice_gain = 0.5;

[[nodiscard]] std::uint32_t oversampling(double quality) noexcept {
  switch (static_cast<Quality>(std::lround(quality))) {
  case Quality::eco:
    return 1;
  case Quality::high:
    return 2;
  case Quality::ultra:
    return 4;
  }
  return 2;
}

template <typename Enum> [[nodiscard]] Enum as(double value) noexcept {
  return static_cast<Enum>(std::lround(value));
}

// Analogue's Osc Shape tolerance: one note voice's pulse width, position
// and warp amounts, moved together.
void shape_offset(OscSettings& settings, double shape) noexcept {
  constexpr double narrowest = 0.01; // nova.osc.<o>.pw's range.
  settings.pw = std::clamp(settings.pw + shape, narrowest, 1.0 - narrowest);
  settings.position = std::clamp(settings.position + shape, 0.0, 1.0);
  for (auto& amount : settings.warp_amount)
    amount = std::clamp(amount + shape, 0.0, 1.0);
}

} // namespace

LfoSettings lfo_settings(const Modulated& mod, std::size_t l, const VoiceTick& tick,
                         double& song_cycles) noexcept {
  const auto& x = index().lfo[l];
  LfoSettings s;
  s.sync = static_cast<std::size_t>(std::lround(mod.get(x.sync)));
  s.mode = as<LfoMode>(mod.get(x.mode));
  s.rise = mod.get(x.rise);
  s.delay = mod.get(x.delay);
  s.smooth = mod.get(x.smooth);
  s.phase = mod.get(x.phase) / 360.0;
  if (s.sync == 0 || s.sync >= lfo_sync_count) {
    s.rate_hz = mod.get(x.rate);
    song_cycles = s.rate_hz * tick.song_beat / std::max(1e-9, tick.beats_per_second);
  } else {
    const auto& division = lfo_divisions[s.sync];
    const double beats = division.bars ? division.length * tick.beats_per_bar : division.length;
    s.rate_hz = tick.beats_per_second / beats;
    song_cycles = division.bars ? tick.song_bar / division.length : tick.song_beat / beats;
  }
  // The song position is a block's start plus an offset, so the same instant
  // can differ in its last bits with the host's block split; on a fine grid a
  // cycle boundary (a gate's step) lands on the same side for any split.
  song_cycles = std::round(song_cycles * song_cycle_grid) / song_cycle_grid;
  return s;
}

void Voice::prepare(double base_rate, std::size_t slot) {
  base_rate_ = base_rate;
  slot_ = slot;
  dc_pole_ = std::exp(-2.0 * std::numbers::pi * dc_block_hz / base_rate);
  amp_.prepare(base_rate);
  for (auto& env : env_)
    env.prepare(base_rate);
  reset();
}

void Voice::reset() noexcept {
  active_ = false;
  released_ = false;
  kill_ = 0;
  amp_.reset();
  for (auto& env : env_)
    env.reset();
  for (auto& lfo : lfo_)
    lfo.reset();
  for (auto& filter : filter_)
    filter.reset();
  for (auto& down : down_)
    down.reset();
  dc_in_ = dc_out_ = {};
  cross_ = {};
}

OscSettings Voice::osc_settings(std::size_t o) const noexcept {
  const auto& x = index().osc[o];
  const auto get = [&](std::size_t i) { return mod_.get(i); };
  OscSettings s;
  s.on = get(x.on) >= 0.5;
  s.type = as<OscType>(get(x.type));
  s.shape = as<ClassicShape>(get(x.shape));
  s.interpolation = as<FrameInterpolation>(get(x.interpolation));
  s.level = get(x.level);
  s.pan = get(x.pan);
  s.position = get(x.position);
  s.pw = get(x.pw);
  s.sync = get(x.sync);
  s.drift = get(x.drift);
  for (std::size_t w = 0; w < warp_slots; ++w) {
    s.warp_mode[w] = as<WarpMode>(get(x.warp_mode[w]));
    s.warp_amount[w] = get(x.warp_amount[w]);
    s.warp_source[w] = as<WarpSource>(get(x.warp_source[w]));
  }
  s.voices = static_cast<std::uint32_t>(std::lround(get(x.unison_voices)));
  s.detune_cents = get(x.unison_detune);
  s.curve = as<UnisonCurve>(get(x.unison_curve));
  s.blend = get(x.unison_blend);
  s.width = get(x.unison_width);
  s.stack = as<UnisonStack>(get(x.unison_stack));
  s.position_spread = get(x.unison_position_spread);
  s.warp_spread = get(x.unison_warp_spread);
  s.semitones = 12.0 * get(x.octave) + get(x.semitone) + get(x.fine) / 100.0 + get(x.pitch);
  s.global_drift = get(index().drift);
  return s;
}

void Voice::analogue_start(std::span<const double> values, std::uint64_t project_frame,
                           std::uint64_t note_number) noexcept {
  // Units are drawn whether or not Analogue is on, so switching it on while
  // a note holds takes effect at once.
  const auto settings = analogue_settings([&](std::size_t i) { return values[i]; });
  // The note voice's number: with voice cards, the next card round-robin
  // (the note's number since the kernel's reset); without, the slot.
  auto& a = analogue_;
  a.card = settings.cards == 0 ? slot_ : static_cast<std::size_t>(note_number % settings.cards);
  for (std::size_t p = 0; p < analogue_part_count; ++p) {
    const auto part = static_cast<AnaloguePart>(p);
    a.unit[p] = analogue_seeded(settings.seed, AnalogueKind::notes, 0, part, a.card);
    a.reroll[p] =
        analogue_rerolled(settings.seed, AnalogueKind::notes, 0, part, a.card, note_number);
  }
  a.flutter.start(
      analogue_flutter_seed(settings.seed, AnalogueKind::notes, 0, a.card, project_frame));
  // Unison voices are numbered within their stack, as Voice Lanes show them;
  // drift and free-running phase also take the note voice, so two notes'
  // stacks never wander or start in step.
  for (std::size_t o = 0; o < oscillator_count; ++o) {
    for (std::size_t i = 0; i < max_unison; ++i) {
      auto& unit = a.unison_unit[o][i];
      auto& reroll = a.unison_reroll[o][i];
      for (std::size_t p = 0; p < analogue_part_count; ++p) {
        const auto part = static_cast<AnaloguePart>(p);
        const bool per_note = part >= AnaloguePart::drift_phase;
        unit[p] = analogue_seeded(settings.seed, AnalogueKind::unison, o, part,
                                  per_note ? a.card * max_unison + i : i);
        reroll[p] = analogue_rerolled(settings.seed, AnalogueKind::unison, o, part, i, note_number);
      }
      a.unison_flutter[o][i].start(analogue_flutter_seed(settings.seed, AnalogueKind::unison, o,
                                                         a.card * max_unison + i, project_frame));
    }
  }
  // The drift clock reads as if this voice had wandered since the project's
  // start, so a note at a given frame drifts the same in any render.
  const double rate = 1.0 + analogue_drift_rate_spread *
                                analogue_.unit[static_cast<std::size_t>(AnaloguePart::drift_rate)];
  analogue_.drift_cycles =
      settings.drift_hz * rate * (static_cast<double>(project_frame) / base_rate_);
}

void Voice::analogue_tick(const NovaState& state, double seconds) noexcept {
  const auto get = [&](std::size_t i) { return mod_.get(i); };
  auto& a = analogue_;
  a.settings = analogue_settings(get);
  const auto& s = a.settings;
  const auto unit = [&](AnaloguePart part) { return a.unit[static_cast<std::size_t>(part)]; };
  // A painted note-voice value, else the seeded one; plus this note's
  // re-roll.
  const auto note_lane = [&](AnaloguePart part) {
    const auto p = static_cast<std::size_t>(part);
    double value = analogue_scaled(s, part, a.unit[p]);
    if (p < analogue_lane_count) {
      const double edit = state.analogue.notes[p][a.card];
      if (AnalogueEdits::painted(edit))
        value = edit;
    }
    return analogue_with_reroll(s, part, value, a.reroll[p]);
  };
  const auto unipolar = [](double x) { return 0.5 * (x + 1.0); };
  // Mod A and Mod B are sources whether or not Analogue is on.
  sources_.value[static_cast<std::size_t>(Source::voice_mod_a)] =
      unipolar(note_lane(AnaloguePart::mod_a));
  sources_.value[static_cast<std::size_t>(Source::voice_mod_b)] =
      unipolar(note_lane(AnaloguePart::mod_b));
  sources_.value[static_cast<std::size_t>(Source::voice_spread)] = voice_spread_centre;

  const double rate = 1.0 + analogue_drift_rate_spread * unit(AnaloguePart::drift_rate);
  a.drift_cycles += seconds * s.drift_hz * rate;
  a.notes = s.notes();
  a.unison = s.unison();
  // Flutter advances one draw per control tick (the start's zero-length
  // pass draws none), so any block split draws the same sequence.
  const bool flutter = s.flutter_amount > 0.0;
  const auto flutter_filter = flutter ? flutter_tick(s.flutter_hz, seconds) : FlutterTick{};
  const double flutter_cents = s.flutter_amount * analogue_flutter_cents;
  const auto jitter = [&](Flutter& generator) {
    return seconds > 0.0 ? generator.advance(flutter_filter) : generator.now(flutter_filter);
  };
  if (a.notes) {
    const double wander = analogue_wander(a.drift_cycles, unit(AnaloguePart::drift_phase),
                                          unit(AnaloguePart::drift_phase_2));
    const double cutoff_wander = analogue_wander(
        a.drift_cycles, unit(AnaloguePart::drift_cutoff_phase), unit(AnaloguePart::drift_phase));
    a.drift_cents = s.drift_amount * nova::analogue_drift_cents * wander;
    a.cents = note_lane(AnaloguePart::pitch) * analogue_pitch_cents + a.drift_cents;
    if (flutter)
      a.cents += flutter_cents * jitter(a.flutter);
    a.cutoff_octaves =
        note_lane(AnaloguePart::cutoff) * std::log2(1.0 + analogue_cutoff_ratio) +
        s.drift_cutoff * cutoff_wander * std::log2(1.0 + analogue_drift_cutoff_ratio);
    a.resonance = 1.0 + analogue_resonance_ratio * note_lane(AnaloguePart::resonance);
    a.env = std::exp2(note_lane(AnaloguePart::env) * std::log2(1.0 + analogue_env_ratio));
    a.glide = std::exp2(note_lane(AnaloguePart::glide) * std::log2(1.0 + analogue_glide_ratio));
    a.gain = std::pow(10.0, note_lane(AnaloguePart::level) * analogue_level_db / 20.0);
    a.pan = note_lane(AnaloguePart::pan) * analogue_pan;
    a.shape = note_lane(AnaloguePart::shape) * analogue_shape;
  } else {
    a.drift_cents = 0.0;
  }
  if (!a.unison)
    return;
  const auto& x = index();
  for (std::size_t o = 0; o < oscillator_count; ++o) {
    auto& out = unison_analogue_[o];
    const auto count = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::lround(get(x.osc[o].unison_voices))), 1, max_unison);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& u = a.unison_unit[o][i];
      const auto lane = [&](AnaloguePart part) {
        const auto p = static_cast<std::size_t>(part);
        double value = analogue_scaled(s, part, u[p]);
        if (p < analogue_lane_count) {
          const double edit = state.analogue.unison[o][p][i];
          if (AnalogueEdits::painted(edit))
            value = edit;
        }
        return analogue_with_reroll(s, part, value, a.unison_reroll[o][i][p]);
      };
      const auto at = [&](AnaloguePart part) { return u[static_cast<std::size_t>(part)]; };
      const double cycles =
          a.drift_cycles * (1.0 + analogue_drift_rate_spread * at(AnaloguePart::drift_rate));
      const double drift =
          s.drift_amount * nova::analogue_drift_cents *
          analogue_wander(cycles, at(AnaloguePart::drift_phase), at(AnaloguePart::drift_phase_2));
      out.cents[i] = lane(AnaloguePart::pitch) * analogue_pitch_cents + drift;
      if (flutter)
        out.cents[i] += flutter_cents * jitter(a.unison_flutter[o][i]);
      out.gain[i] = std::pow(10.0, lane(AnaloguePart::level) * analogue_level_db / 20.0);
      out.pan[i] = lane(AnaloguePart::pan) * analogue_pan;
      out.shape[i] = lane(AnaloguePart::shape) * analogue_shape;
    }
  }
}

void Voice::analogue_phases(const NovaState& state, std::size_t o, double osc_hz,
                            std::uint64_t project_frame) noexcept {
  const auto& s = analogue_.settings;
  auto& out = unison_analogue_[o];
  out.free_running = s.free_running;
  for (std::size_t i = 0; i < max_unison; ++i) {
    const auto& u = analogue_.unison_unit[o][i];
    if (s.free_running) {
      // Where this oscillator would be had it run since the project's start,
      // from a seeded point: it starts mid-cycle, the same in any render.
      const double cycles = osc_hz / base_rate_ * static_cast<double>(project_frame);
      const double offset = 0.5 * (u[static_cast<std::size_t>(AnaloguePart::free_phase)] + 1.0);
      out.phase[i] = cycles - std::floor(cycles) + offset;
      continue;
    }
    constexpr auto at = static_cast<std::size_t>(AnaloguePart::phase);
    // A painted Phase lane value, else the seeded one, as for the other lanes.
    const auto painted_or = [](double edit, double seeded) {
      return AnalogueEdits::painted(edit) ? edit : seeded;
    };
    double phase = 0.0;
    if (analogue_.notes)
      phase += analogue_phase_cycles *
               analogue_with_reroll(
                   s, AnaloguePart::phase,
                   painted_or(state.analogue.notes[at][analogue_.card],
                              analogue_scaled(s, AnaloguePart::phase, analogue_.unit[at])),
                   analogue_.reroll[at]);
    if (analogue_.unison)
      phase += analogue_phase_cycles *
               analogue_with_reroll(s, AnaloguePart::phase,
                                    painted_or(state.analogue.unison[o][at][i],
                                               analogue_scaled(s, AnaloguePart::phase, u[at])),
                                    analogue_.unison_reroll[o][i][at]);
    out.phase[i] = phase;
  }
}

void Voice::start(const NoteEvent& note, const VoiceTick& tick, double glide_from, bool legato,
                  std::uint64_t order, std::uint64_t project_frame,
                  std::uint64_t note_number) noexcept {
  const auto& x = index();
  const bool continuing = legato && active_ && !released_;
  note_ = note;
  order_ = order;
  released_ = false;
  kill_ = 0;
  velocity_ = note.velocity / 127.0;
  const std::uint64_t seed = (note.note.high * 0x9E3779B97F4A7C15ULL) ^ note.note.low ^
                             (static_cast<std::uint64_t>(note.key) << 48U);
  random_ = hash_unit(seed ^ 0x5151ULL);
  // Portamento: from the previous key, by time or by rate; in seconds, or
  // in note values of the tempo at the note.
  const bool synced = tick.values[x.portamento_sync] >= 0.5;
  const auto division =
      static_cast<std::size_t>(std::clamp<long>(std::lround(tick.values[x.portamento_division]), 0L,
                                                static_cast<long>(portamento_division_count) - 1));
  const double portamento =
      synced ? portamento_division_beats[division] / std::max(1e-9, tick.beats_per_second)
             : tick.values[x.portamento];
  const bool always = tick.values[x.portamento_mode] >= 0.5;
  if (portamento > 0.0 && glide_from >= 0.0 && (always || continuing)) {
    glide_from_ = continuing ? key_ : glide_from;
    glide_elapsed_ = 0.0;
    const bool by_rate = tick.values[x.portamento_rate] >= 0.5;
    glide_seconds_ = by_rate ? portamento * std::fabs(note.key - glide_from_) / 12.0 : portamento;
  } else {
    glide_from_ = -1.0;
    glide_seconds_ = 0.0;
  }
  key_ = glide_from_ >= 0.0 ? glide_from_ : note.key;
  if (!continuing) {
    oversample_ = oversampling(tick.values[x.quality]);
    const double internal = base_rate_ * oversample_;
    for (auto& osc : osc_)
      osc.prepare(internal, oversample_);
    sub_.prepare(internal, oversample_);
    noise_.prepare(internal);
    for (auto& filter : filter_) {
      filter.prepare(internal);
      filter.reset();
    }
    for (auto& down : down_)
      down.reset();
    dc_in_ = dc_out_ = {};
    cross_ = {};
    voice_seconds_ = 0.0;
    for (auto& lfo : lfo_)
      lfo.trigger();
    analogue_start(tick.values, project_frame, note_number);
  }
  active_ = true;
  amp_.gate_on(continuing);
  for (auto& env : env_)
    env.gate_on(continuing);
  // Oscillators start at this note's settings: velocity, note and random
  // are known now; envelopes and LFOs catch up at the tick below.
  sources_.value[static_cast<std::size_t>(Source::velocity)] = velocity_;
  sources_.value[static_cast<std::size_t>(Source::note)] = note.key / 127.0;
  sources_.value[static_cast<std::size_t>(Source::random)] = random_;
  sources_.value[static_cast<std::size_t>(Source::arp_mod)] = arp_mod_;
  sources_.value[static_cast<std::size_t>(Source::arp_accent)] = arp_accent_;
  // A first pass resolves Analogue's knobs (they may be modulated) and so
  // its offsets and voice sources; the second lets routes from those
  // sources reach the note's start. Evaluating twice changes nothing else.
  mod_.evaluate(tick.values, tick.state->routes, sources_);
  analogue_tick(*tick.state, 0.0);
  mod_.evaluate(tick.values, tick.state->routes, sources_);
  if (!continuing) {
    double start_key = key_;
    if (analogue_.notes)
      start_key += analogue_.cents / 100.0;
    const double hz = tick.values[x.a4] * std::exp2((start_key - 69.0) / 12.0);
    for (std::size_t o = 0; o < oscillator_count; ++o) {
      auto settings = osc_settings(o);
      const auto& state = *tick.state;
      const UnisonAnalogue* analogue = nullptr;
      if (analogue_.settings.on) {
        if (analogue_.notes)
          shape_offset(settings, analogue_.shape);
        // Outside the scope, unison voices keep only their start phases.
        if (!analogue_.unison)
          unison_analogue_[o] = {};
        analogue_phases(*tick.state, o, hz * std::exp2(settings.semitones / 12.0), project_frame);
        analogue = &unison_analogue_[o];
      }
      osc_[o].start(settings, hz,
                    settings.type == OscType::wavetable ? state.tables[o].get() : nullptr,
                    mod_.get(x.osc[o].phase) / 360.0, mod_.get(x.osc[o].random_phase),
                    seed + o * 7919, analogue);
    }
    sub_.start(as<ClassicShape>(tick.values[x.sub_shape]));
    noise_.start(tick.state->noise.get(), seed ^ 0xBEEFULL);
  }
  control(tick);
}

void Voice::release() noexcept {
  if (!active_ || released_)
    return;
  released_ = true;
  amp_.gate_off();
  for (auto& env : env_)
    env.gate_off();
}

void Voice::kill() noexcept {
  if (!active_)
    return;
  kill_length_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base_rate_ * 0.003));
  kill_ = kill_length_;
}

void Voice::update_pitch(const VoiceTick& tick) noexcept {
  if (glide_from_ < 0.0) {
    key_ = note_.key;
    return;
  }
  glide_elapsed_ += tick.frames / base_rate_;
  // Analogue: each card's slew is its own.
  const double seconds = glide_seconds_ * (analogue_.notes ? analogue_.glide : 1.0);
  const double t = seconds > 0.0 ? std::min(1.0, glide_elapsed_ / seconds) : 1.0;
  const double shaped = bend(t, -mod_.get(index().portamento_curve));
  key_ = glide_from_ + (note_.key - glide_from_) * shaped;
  if (t >= 1.0)
    glide_from_ = -1.0;
}

void Voice::control(const VoiceTick& tick) noexcept {
  const auto& x = index();
  const auto& state = *tick.state;
  const auto& global = *tick.global;
  const double seconds = tick.frames / base_rate_;
  if (mod_.base().empty())
    mod_.evaluate(tick.values, state.routes, sources_);
  const auto get = [&](std::size_t i) { return mod_.get(i); };
  analogue_tick(state, seconds);

  // Sources first, from last tick's modulated settings.
  for (std::size_t e = 0; e < envelope_count; ++e) {
    const auto& ei = x.env[e];
    EnvelopeSettings s;
    s.attack = get(ei.attack);
    s.hold = get(ei.hold);
    s.decay = get(ei.decay);
    s.sustain = get(ei.sustain);
    s.release = get(ei.release);
    s.attack_curve = get(ei.attack_curve);
    s.decay_curve = get(ei.decay_curve);
    s.release_curve = get(ei.release_curve);
    s.loop = get(ei.loop) >= 0.5;
    s.legato = get(ei.legato) >= 0.5;
    if (analogue_.notes) {
      s.attack *= analogue_.env;
      s.hold *= analogue_.env;
      s.decay *= analogue_.env;
      s.release *= analogue_.env;
    }
    if (e == 0) {
      amp_.set(s);
      sources_.value[0] = amp_.level();
    } else {
      env_[e - 1].set(s);
      // Envelope velocity: scales the source by velocity, 0..100 %.
      const double scale = 1.0 - get(ei.velocity) * (1.0 - velocity_);
      sources_.value[e] = env_[e - 1].level() * scale;
      (void)env_[e - 1].advance(tick.frames);
    }
  }
  voice_seconds_ += seconds;
  for (std::size_t l = 0; l < lfo_count; ++l) {
    const auto source = static_cast<std::size_t>(Source::lfo1) + l;
    if (get(x.lfo[l].scope) >= 0.5) {
      sources_.value[source] = global.lfo[l];
      sources_.gain[source] = global.lfo_gain[l];
      continue;
    }
    double song_cycles = 0.0;
    const auto settings = lfo_settings(mod_, l, tick, song_cycles);
    sources_.value[source] = lfo_[l].tick(settings, state.lfo_shapes[l], seconds, song_cycles);
    sources_.gain[source] = lfo_[l].gain();
  }
  for (std::size_t m = 0; m < macro_count; ++m)
    sources_.value[static_cast<std::size_t>(Source::macro1) + m] = global.macro[m];
  sources_.value[static_cast<std::size_t>(Source::velocity)] = velocity_;
  sources_.value[static_cast<std::size_t>(Source::note)] = note_.key / 127.0;
  sources_.value[static_cast<std::size_t>(Source::modwheel)] = global.modwheel;
  sources_.value[static_cast<std::size_t>(Source::aftertouch)] = global.aftertouch;
  sources_.value[static_cast<std::size_t>(Source::mpe_slide)] = 0.0;
  sources_.value[static_cast<std::size_t>(Source::mpe_pressure)] = 0.0;
  sources_.value[static_cast<std::size_t>(Source::chaos1)] = global.chaos[0];
  sources_.value[static_cast<std::size_t>(Source::chaos2)] = global.chaos[1];
  sources_.value[static_cast<std::size_t>(Source::random)] = random_;
  sources_.value[static_cast<std::size_t>(Source::bend)] = global.bend;
  sources_.value[static_cast<std::size_t>(Source::arp_mod)] = arp_mod_;
  sources_.value[static_cast<std::size_t>(Source::arp_accent)] = arp_accent_;

  mod_.evaluate(tick.values, state.routes, sources_);
  update_pitch(tick);

  const std::uint32_t frames = tick.frames * oversample_;
  const double bend_semitones = (2.0 * global.bend - 1.0) * get(x.bend_range);
  double key = key_ + bend_semitones;
  if (analogue_.notes)
    key += analogue_.cents / 100.0;
  const double hz = get(x.a4) * std::exp2((key - 69.0) / 12.0);
  for (std::size_t o = 0; o < oscillator_count; ++o) {
    auto settings = osc_settings(o);
    if (analogue_.notes)
      shape_offset(settings, analogue_.shape);
    const auto* table = settings.type == OscType::wavetable ? state.tables[o].get() : nullptr;
    osc_[o].set(settings, hz, table, state.remap[o], frames, seconds,
                analogue_.unison ? &unison_analogue_[o] : nullptr);
    route_[o] = as<SourceRoute>(get(x.osc[o].route));
  }
  const double sub_octave = std::round(get(x.sub_octave)) - 2.0;
  sub_.set(hz * std::exp2(sub_octave), get(x.sub_on) >= 0.5 ? get(x.sub_level) : 0.0,
           get(x.sub_pan), as<ClassicShape>(get(x.sub_shape)), frames);
  route_[3] = as<SourceRoute>(get(x.sub_route));
  const double noise_semitones =
      get(x.noise_pitch) + (get(x.noise_keytrack) >= 0.5 ? key - 60.0 : 0.0);
  noise_.set(state.noise.get(), std::exp2(noise_semitones / 12.0),
             get(x.noise_on) >= 0.5 ? get(x.noise_level) : 0.0, get(x.noise_pan), get(x.noise_tone),
             get(x.noise_mode) >= 0.5, frames);
  route_[4] = as<SourceRoute>(get(x.noise_route));
  noise_am_ = as<NoiseAmSource>(get(x.noise_am_source));
  noise_am_amount_ = get(x.noise_am_amount);
  noise_am_exponent_ = std::exp2(get(x.noise_am_curve) * noise_am_curve_octaves);

  for (std::size_t f = 0; f < filter_count; ++f) {
    const auto& fi = x.filter[f];
    filter_on_[f] = get(fi.on) >= 0.5;
    FilterSettings s;
    s.model = as<FilterModel>(get(fi.model));
    s.cutoff_hz = std::clamp(get(fi.cutoff) * std::exp2((key - 60.0) / 12.0 * get(fi.keytrack)),
                             8.0, 22'000.0);
    s.resonance = get(fi.resonance);
    if (analogue_.notes) {
      s.cutoff_hz = std::clamp(s.cutoff_hz * std::exp2(analogue_.cutoff_octaves), 8.0, 22'000.0);
      s.resonance = std::clamp(s.resonance * analogue_.resonance, 0.0, 1.0);
    }
    s.drive_db = get(fi.drive);
    s.fat = get(fi.fat);
    filter_[f].set(s, frames);
    filter_mix_[f] = get(fi.mix);
    filter_pan_[f] = get(fi.pan);
    cutoff_[f] = s.cutoff_hz;
    resonance_[f] = s.resonance;
  }
  routing_ = as<FilterRouting>(get(x.filter_routing));
  const double curved = bend(velocity_, -get(x.velocity_curve));
  amp_velocity_ = 1.0 - get(x.env[0].velocity) * (1.0 - curved);
  if (analogue_.notes)
    amp_velocity_ *= analogue_.gain;
}

void Voice::take_peaks(std::span<float, 5> out) noexcept {
  for (std::size_t o = 0; o < oscillator_count; ++o) {
    out[o] = std::max(out[o], osc_[o].peak());
    osc_[o].decay_peak();
  }
  out[3] = std::max(out[3], sub_.peak());
  sub_.decay_peak();
  out[4] = std::max(out[4], noise_.peak());
  noise_.decay_peak();
}

void Voice::render(std::span<engine::StereoFrame> out, VoiceScratch& scratch) noexcept {
  if (!active_)
    return;
  const auto filtered = [&](std::size_t f, StereoSample in) {
    auto left = static_cast<float>(in.left);
    auto right = static_cast<float>(in.right);
    filter_[f].process(left, right);
    const double mix = filter_mix_[f];
    StereoSample result{in.left + mix * (left - in.left), in.right + mix * (right - in.right)};
    const double pan = filter_pan_[f];
    result.left *= 1.0 - std::max(0.0, pan);
    result.right *= 1.0 + std::min(0.0, pan);
    return result;
  };
  // Oscillators that read each other run sample by sample, each a sample
  // behind the rest; otherwise each renders a chunk on its own. Either way
  // a chunk's sources are rendered before its filters, which never feed
  // back into them.
  const bool crossed = std::ranges::any_of(osc_, [](const auto& osc) { return osc.reads_cross(); });
  // Noise AM: the followed oscillator's signal, sample by sample.
  const bool noise_am = noise_am_ != NoiseAmSource::off;
  constexpr auto first_osc = static_cast<std::size_t>(NoiseAmSource::osc_a);
  const std::size_t followed = noise_am ? static_cast<std::size_t>(noise_am_) - first_osc : 0;
  const std::size_t chunk = max_render_samples / oversample_;
  for (std::size_t done = 0; done < out.size(); done += chunk) {
    const auto frames = out.subspan(done, std::min(chunk, out.size() - done));
    const std::size_t count = frames.size() * oversample_;
    // The sub and noise always run sample by sample (in source order, as
    // before), so their previous samples are there for crossed warps.
    // `follow` is the followed oscillator's signal at this sample.
    const auto sub_and_noise = [&](std::size_t i, CrossInputs& next, double follow) {
      double left = 0.0, right = 0.0;
      next.value[3] = sub_.process(left, right);
      scratch.left[3][i] = left;
      scratch.right[3][i] = right;
      left = right = 0.0;
      const double am =
          noise_am ? noise_am_gain(follow, noise_am_amount_, noise_am_exponent_) : 1.0;
      next.value[4] = noise_.process(left, right, am);
      scratch.left[4][i] = left;
      scratch.right[4][i] = right;
    };
    // Sources that render only zeros this chunk (a dormant oscillator, a
    // silent sub and noise) add nothing to the buses (+0 to sums that are
    // never -0), so the mix skips them.
    std::array<std::size_t, 5> sounding{};
    std::size_t sounding_count = 0;
    for (std::size_t source = 0; source < route_.size(); ++source) {
      const bool zeros = !crossed && (source < oscillator_count ? osc_[source].dormant()
                                                                : sub_.silent() && noise_.silent());
      if (!zeros)
        sounding[sounding_count++] = source;
    }
    if (crossed) {
      for (std::size_t i = 0; i < count; ++i) {
        CrossInputs next{};
        for (std::size_t o = 0; o < oscillator_count; ++o) {
          double left = 0.0, right = 0.0;
          next.value[o] = osc_[o].process(cross_, left, right);
          scratch.left[o][i] = left;
          scratch.right[o][i] = right;
        }
        sub_and_noise(i, next, next.value[followed]);
        cross_ = next;
      }
    } else {
      std::array<float, max_render_samples> mono{};
      std::array<float, max_render_samples> follow{};
      for (std::size_t o = 0; o < oscillator_count; ++o) {
        osc_[o].render(count, scratch.left[o].data(), scratch.right[o].data(), mono.data());
        if (count > 0)
          cross_.value[o] = mono[count - 1];
        if (noise_am && o == followed)
          std::copy_n(mono.data(), count, follow.data());
      }
      // Read by no warp, a silent sub or noise source adds nothing and
      // holds its state.
      if (sub_.silent() && noise_.silent()) {
        for (const std::size_t source : {std::size_t{3}, std::size_t{4}}) {
          std::fill_n(scratch.left[source].data(), count, 0.0);
          std::fill_n(scratch.right[source].data(), count, 0.0);
        }
      } else {
        // Likewise one of them alone: silent, it holds its state and
        // renders the +0 its zero gains would give.
        const bool sub_sounds = !sub_.silent();
        const bool noise_sounds = !noise_.silent();
        for (std::size_t i = 0; i < count; ++i) {
          double left = 0.0, right = 0.0;
          if (sub_sounds) {
            const float value = sub_.process(left, right);
            if (i + 1 == count)
              cross_.value[3] = value;
          }
          scratch.left[3][i] = left;
          scratch.right[3][i] = right;
          left = right = 0.0;
          if (noise_sounds) {
            const double am =
                noise_am ? noise_am_gain(follow[i], noise_am_amount_, noise_am_exponent_) : 1.0;
            const float value = noise_.process(left, right, am);
            if (i + 1 == count)
              cross_.value[4] = value;
          }
          scratch.left[4][i] = left;
          scratch.right[4][i] = right;
        }
      }
    }
    std::size_t i = 0;
    for (auto& frame : frames) {
      for (std::uint32_t k = 0; k < oversample_; ++k, ++i) {
        std::array<StereoSample, 3> bus{}; // Filter 1, filter 2, direct.
        for (std::size_t n = 0; n < sounding_count; ++n) {
          const std::size_t source = sounding[n];
          std::size_t target = 2;
          if (route_[source] != SourceRoute::direct)
            target =
                routing_ == FilterRouting::split && route_[source] == SourceRoute::filter_2 ? 1 : 0;
          bus[target].left += scratch.left[source][i];
          bus[target].right += scratch.right[source][i];
        }
        StereoSample y{};
        switch (routing_) {
        case FilterRouting::serial: {
          StereoSample x = bus[0] + bus[1];
          if (filter_on_[0])
            x = filtered(0, x);
          if (filter_on_[1])
            x = filtered(1, x);
          y = x;
          break;
        }
        case FilterRouting::parallel: {
          const StereoSample x = bus[0] + bus[1];
          if (!filter_on_[0] && !filter_on_[1]) {
            y = x;
          } else {
            if (filter_on_[0])
              y = y + filtered(0, x);
            if (filter_on_[1])
              y = y + filtered(1, x);
          }
          break;
        }
        case FilterRouting::split:
          y = (filter_on_[0] ? filtered(0, bus[0]) : bus[0]) +
              (filter_on_[1] ? filtered(1, bus[1]) : bus[1]);
          break;
        }
        stage_[k] = y + bus[2];
      }
      StereoSample sample = stage_[0];
      if (oversample_ == 2) {
        sample = down_[0].process(stage_[0], stage_[1]);
      } else if (oversample_ == 4) {
        const auto a = down_[0].process(stage_[0], stage_[1]);
        const auto b = down_[0].process(stage_[2], stage_[3]);
        sample = down_[1].process(a, b);
      }
      dc_out_ = sample - dc_in_ + dc_out_ * StereoSample{dc_pole_, dc_pole_};
      dc_in_ = sample;
      sample = dc_out_;
      double gain = amp_.next() * amp_velocity_ * voice_gain;
      if (kill_ > 0) {
        gain *= static_cast<double>(kill_) / kill_length_;
        if (--kill_ == 0) {
          active_ = false;
          gain = 0.0;
        }
      }
      if (analogue_.notes) {
        // Analogue's pan: this card's balance, as the filters' pan.
        sample.left *= 1.0 - std::max(0.0, analogue_.pan);
        sample.right *= 1.0 + std::min(0.0, analogue_.pan);
      }
      frame.left += static_cast<float>(sample.left * gain);
      frame.right += static_cast<float>(sample.right * gain);
      if (!amp_.active()) {
        active_ = false;
        return;
      }
      if (!active_)
        return;
    }
  }
}

} // namespace oscillate::native_dsp::synth::nova
