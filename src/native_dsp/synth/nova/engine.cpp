#include "oscillate/native_dsp/synth/nova.hpp"

#include "document.hpp"
#include "low_end.hpp"
#include "nova_presets.hpp"
#include "voice.hpp"

#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>

// Nova (ADR-0057): the kernel. Notes pass through the note generator (arp
// and clip sequencer) to up to 32 voices; global sources and the global
// path of the matrix run once per tick; the voice sum goes through the
// effect rack and the master level. Telemetry follows contract 08a §6.
namespace oscillate::native_dsp::synth::nova {
namespace {

// 32 playing plus room for stolen fades; Analogue numbers note voices by slot.
constexpr std::size_t voice_capacity = analogue_note_voices;
constexpr std::size_t max_pending_notes = 256;
// Voices go to the audio helpers (ADR-0085) once at least this many sound:
// one alone gains nothing from a hand-off.
constexpr std::size_t min_parallel_voices = 2;
// A voice rendered apart starts from -0, the additive identity, so its
// buffer holds exactly what it would have added to the shared one.
constexpr float voice_sum_identity = -0.0F;
// 08a §6: the FX block starts at 720 with the modules' own values, then the
// slot meters, details and spectra.
constexpr std::size_t telemetry_fx = 720;
constexpr std::size_t telemetry_slot_in = telemetry_fx + fx_slot_uids;
constexpr std::size_t telemetry_slot_out = telemetry_slot_in + fx_slot_uids;
constexpr std::size_t telemetry_slot_detail = telemetry_slot_out + fx_slot_uids;
constexpr std::size_t telemetry_slot_spectrum =
    telemetry_slot_detail + fx_slot_uids * fx_detail_values;
constexpr std::size_t telemetry_fx_end = telemetry_slot_spectrum + fx_slot_uids * fx_spectrum_bands;
// Analogue (ADR-0073): the voice sources, then for the first note voices
// (the voice cards) the pitch drift in cents and the key, or -1.
constexpr std::size_t telemetry_voice_sources = telemetry_fx_end;
constexpr std::size_t telemetry_voice_source_count = 3;
constexpr std::size_t telemetry_cards = 8;
constexpr std::size_t telemetry_card_drift = telemetry_voice_sources + telemetry_voice_source_count;
constexpr std::size_t telemetry_card_key = telemetry_card_drift + telemetry_cards;
// The arp (ADR-0078): arp.mod and arp.accent as s, and whether the step
// playing (offset 79) is on a variation bar.
constexpr std::size_t telemetry_arp = telemetry_card_key + telemetry_cards;
constexpr std::size_t telemetry_arp_count = 3;
constexpr std::size_t telemetry_size = telemetry_arp + telemetry_arp_count;
static_assert(telemetry_size <= max_telemetry_values);

template <typename Enum> [[nodiscard]] Enum as(double value) noexcept {
  return static_cast<Enum>(std::lround(value));
}

class NovaKernel final : public SynthKernel {
public:
  explicit NovaKernel(double sample_rate)
      : rate_(sample_rate), global_mod_(descriptor().parameters.size()),
        voices_(std::make_unique<std::array<Voice, voice_capacity>>()) {
    for (std::size_t slot = 0; slot < voices_->size(); ++slot)
      (*voices_)[slot].prepare(rate_, slot);
    rack_.prepare(rate_, control_interval_frames);
    lock_.prepare(rate_);
    generator_.prepare(rate_);
    chaos_.reset();
    for (auto& lfo : lfo_)
      lfo.reset();
  }

  void adopt_document(const SynthDocumentState& state) noexcept override {
    state_ = static_cast<const NovaState*>(&state);
    rack_.adopt(*state_->fx);
    generator_.adopt(state_->clips);
  }

  void adopt_lock(const LockState& state) noexcept override { lock_.adopt(state); }
  void use_workers(engine::AudioWorkerPool* workers) noexcept override { workers_ = workers; }
  void locate(std::uint64_t project_frame) noexcept override { frame_ = project_frame; }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    values_ = values;
    transport_ = transport;
    since_tick_ = 0;
    tick_frames_ = frames_to_next_tick;
    if (state_ == nullptr)
      return;
    const auto& x = index();
    const double seconds = frames_to_next_tick / rate_;
    const double beats_per_second =
        transport.beats_per_frame > 0.0 ? transport.beats_per_frame * rate_ : 2.0;
    if (transport.beats_per_frame <= 0.0)
      free_seconds_ += seconds;
    const double song_beat = transport.beats_per_frame > 0.0 ? transport.beat_at_block_start
                                                             : free_seconds_ * beats_per_second;
    const double song_bar =
        transport.beats_per_frame > 0.0 ? transport.bar_at_block_start : song_beat / 4.0;

    // Global sources, from last tick's global modulation.
    if (global_mod_.base().empty()) {
      SourceValues none;
      global_mod_.evaluate(values, state_->routes, none);
    }
    const auto get = [&](std::size_t i) { return global_mod_.get(i); };
    for (std::size_t m = 0; m < macro_count; ++m)
      global_.macro[m] = get(x.macro[m]);
    for (std::size_t c = 0; c < chaos_count; ++c)
      global_.chaos[c] = chaos_.tick(c, get(x.chaos_rate[c]), get(x.chaos_smooth[c]), seconds);
    global_.modwheel = get(x.perf_modwheel);
    global_.aftertouch = get(x.perf_aftertouch);
    global_.bend = 0.5 + 0.5 * get(x.perf_bend);
    tick_ = {values,           state_,
             &global_,         frames_to_next_tick,
             beats_per_second, transport.beats_per_bar,
             song_beat,        song_bar};
    for (std::size_t l = 0; l < lfo_count; ++l) {
      double song_cycles = 0.0;
      const auto settings = lfo_settings(global_mod_, l, tick_, song_cycles);
      global_.lfo[l] = lfo_[l].tick(settings, state_->lfo_shapes[l], seconds, song_cycles);
      global_.lfo_gain[l] = lfo_[l].gain();
      global_.lfo_phase[l] = lfo_[l].phase();
    }

    // Voices, then the global path with the newest voice's sources.
    const Voice* newest = nullptr;
    for (auto& voice : *voices_) {
      if (!voice.active())
        continue;
      voice.control(tick_);
      if (!voice.dying() && (newest == nullptr || voice.order() > newest->order()))
        newest = &voice;
    }
    newest_ = newest;
    SourceValues sources;
    if (newest != nullptr) {
      sources = newest->sources();
    } else {
      for (std::size_t l = 0; l < lfo_count; ++l) {
        sources.value[static_cast<std::size_t>(Source::lfo1) + l] = global_.lfo[l];
        sources.gain[static_cast<std::size_t>(Source::lfo1) + l] = global_.lfo_gain[l];
      }
      for (std::size_t m = 0; m < macro_count; ++m)
        sources.value[static_cast<std::size_t>(Source::macro1) + m] = global_.macro[m];
      sources.value[static_cast<std::size_t>(Source::chaos1)] = global_.chaos[0];
      sources.value[static_cast<std::size_t>(Source::chaos2)] = global_.chaos[1];
      sources.value[static_cast<std::size_t>(Source::modwheel)] = global_.modwheel;
      sources.value[static_cast<std::size_t>(Source::aftertouch)] = global_.aftertouch;
      sources.value[static_cast<std::size_t>(Source::bend)] = global_.bend;
      for (const auto voice : {Source::voice_mod_a, Source::voice_mod_b, Source::voice_spread})
        sources.value[static_cast<std::size_t>(voice)] = voice_spread_centre;
    }
    global_mod_.evaluate(values, state_->routes, sources);

    // Effects and master.
    FxRackSettings fx;
    for (std::size_t u = 0; u < fx_slot_uids; ++u) {
      const auto& slot = x.fx_slot[u];
      for (std::size_t k = 0; k < fx_slot_params; ++k)
        fx.slots[u].p[k] = get(slot.p[k]);
      fx.slots[u].mix = get(slot.mix);
      fx.slots[u].on = get(slot.on) >= 0.5;
    }
    const auto factor = [](double choice) {
      return std::uint32_t{1} << static_cast<std::uint32_t>(std::lround(choice));
    };
    fx.oversample = {factor(get(x.fx_main_oversample)), factor(get(x.fx_bus_oversample[0])),
                     factor(get(x.fx_bus_oversample[1]))};
    fx.split = get(x.fx_split) >= 0.5;
    fx.split_hz = get(x.fx_split_frequency);
    for (std::size_t b = 0; b < 2; ++b) {
      fx.bus_send[b] = get(x.fx_bus_send[b]);
      fx.bus_send_post[b] = get(x.fx_bus_send_point[b]) >= 0.5;
      fx.bus_return_db[b] = get(x.fx_bus_return[b]);
    }
    rack_.set(fx, {beats_per_second, rate_}, frames_to_next_tick);
    const double target = oscillate::native_dsp::dsp::db_to_gain(get(x.master));
    master_step_ = (target - master_) / std::max<std::uint32_t>(1, frames_to_next_tick);
    // Duck and crossover are global modulation destinations like the rest.
    lock_.set(get(x.lock_on) >= 0.5, get(x.lock_duck), get(x.lock_crossover),
              get(x.lock_mono) >= 0.5);

    // The note generator's settings.
    ArpSettings arp;
    arp.on = get(x.arp_on) >= 0.5;
    arp.mode = as<ArpMode>(get(x.arp_mode));
    arp.rate = static_cast<std::size_t>(std::lround(get(x.arp_rate)));
    arp.gate = get(x.arp_gate);
    arp.swing = get(x.arp_swing);
    arp.octaves = static_cast<std::uint32_t>(std::lround(get(x.arp_octaves)));
    arp.velocity = as<ArpVelocity>(get(x.arp_velocity));
    arp.retrigger = get(x.arp_retrigger) >= 0.5;
    arp.latch = get(x.arp_latch) >= 0.5;
    const auto whole = [&](std::size_t i) {
      return static_cast<std::uint32_t>(std::max(0L, std::lround(get(i))));
    };
    arp.note_mode = as<ArpNoteMode>(get(x.arp_note_mode));
    arp.scale = static_cast<std::size_t>(std::lround(get(x.arp_scale)));
    arp.root = whole(x.arp_root);
    arp.passing = get(x.arp_passing) >= 0.5;
    arp.strum = get(x.arp_strum);
    arp.euclid_hits = whole(x.arp_euclid_hits);
    arp.euclid_steps = whole(x.arp_euclid_steps);
    arp.euclid_rotate = whole(x.arp_euclid_rotate);
    arp.fill = get(x.arp_fill) >= 0.5;
    arp.variation = arp_variation_bars[std::min<std::size_t>(
        static_cast<std::size_t>(whole(x.arp_variation)), arp_variation_bars.size() - 1)];
    arp.seed = whole(x.arp_seed);
    arp.groove.amount = get(x.arp_groove_amount);
    arp.groove.steps = whole(x.arp_groove_length);
    arp.groove.grid = static_cast<std::size_t>(whole(x.arp_groove_grid));
    for (std::size_t n = 0; n < arp_groove_steps; ++n) {
      arp.groove.timing[n] = get(x.arp_groove_timing[n]);
      arp.groove.velocity[n] = get(x.arp_groove_velocity[n]);
    }
    for (std::size_t l = 0; l < arp_lane_count; ++l) {
      const auto& lane = x.arp_lane[l];
      arp.lanes[l].length = whole(lane.length);
      for (std::size_t n = 0; n < arp_max_steps; ++n)
        arp.lanes[l].values[n] = get(lane.step[n]);
    }
    SequencerSettings sequencer;
    sequencer.on = get(x.seq_on) >= 0.5;
    sequencer.clip = static_cast<std::size_t>(std::lround(get(x.seq_clip)));
    sequencer.trigger = as<SequencerTrigger>(get(x.seq_trigger));
    sequencer.root = static_cast<std::uint8_t>(std::lround(get(x.seq_root)));
    sequencer.follow = get(x.seq_follow) >= 0.5;
    generator_.set(arp, sequencer);
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (pending_count_ < max_pending_notes)
      pending_[pending_count_++] = {0, true, event};
  }
  void note_off(const NoteEvent& event) noexcept override {
    if (pending_count_ < max_pending_notes)
      pending_[pending_count_++] = {0, false, event};
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    if (state_ == nullptr)
      return;
    const auto started = std::chrono::steady_clock::now();
    // The block's transport, advanced from the last tick.
    SynthTransport transport = transport_;
    const double beats = transport_.beats_per_frame * since_tick_;
    transport.beat_at_block_start += beats;
    transport.bar_at_block_start += beats / std::max(1e-9, transport_.beats_per_bar);
    const auto frames = static_cast<std::uint32_t>(output.size());
    const auto count = generator_.process(std::span(pending_.data(), pending_count_), transport,
                                          frames, generated_);
    pending_count_ = 0;
    std::uint32_t position = 0;
    for (std::size_t i = 0; i <= count; ++i) {
      const std::uint32_t until = i < count ? std::min(generated_[i].offset, frames) : frames;
      if (until > position) {
        render_voices(output.subspan(position, until - position));
        position = until;
      }
      if (i < count) {
        note_frame_ = frame_ + position;
        if (generated_[i].on)
          start_note(generated_[i]);
        else
          stop_note(generated_[i].note);
      }
    }
    rack_.process(output);
    for (auto& frame : output) {
      master_ += master_step_;
      frame.left = static_cast<float>(frame.left * master_);
      frame.right = static_cast<float>(frame.right * master_);
    }
    lock_.process(output, frame_);
    for (const auto& frame : output) {
      out_peak_[0] = std::max(out_peak_[0], std::fabs(frame.left));
      out_peak_[1] = std::max(out_peak_[1], std::fabs(frame.right));
    }
    since_tick_ += frames;
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    cpu_ = elapsed / (frames / rate_);
  }

  void reset() noexcept override {
    for (auto& voice : *voices_)
      voice.reset();
    generator_.reset();
    rack_.reset();
    lock_.reset();
    for (auto& lfo : lfo_)
      lfo.reset();
    chaos_.reset();
    held_count_ = 0;
    pending_count_ = 0;
    last_key_ = -1.0;
    // Voice cards start again at the first (ADR-0073): the runtime resets
    // the kernel on every new transport generation (seek, play from a
    // position, export), so a render from a position deals cards alike.
    notes_started_ = 0;
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    std::uint32_t active = 0;
    for (const auto& voice : *voices_)
      active += voice.active() ? 1U : 0U;
    return {active, steals_, false};
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry_size || state_ == nullptr)
      return 0;
    std::fill_n(out.begin(), telemetry_size, 0.0F);
    const auto& x = index();
    const Voice* v = newest_;
    const auto status_now = status();
    out[0] = 1.0F;
    out[1] = static_cast<float>(status_now.active_voices);
    out[2] = static_cast<float>(std::lround(values_.empty() ? 16.0 : values_[x.polyphony]));
    out[3] = static_cast<float>(cpu_);
    out[4] = v != nullptr ? static_cast<float>(v->note().key) : -1.0F;
    out[5] = transport_.beats_per_frame > 0.0 ? 1.0F : 0.0F;
    const auto get = [&](std::size_t i) {
      return v != nullptr ? v->modulated().get(i) : global_mod_.get(i);
    };
    for (std::size_t o = 0; o < oscillator_count; ++o) {
      out[8 + o] = static_cast<float>(get(x.osc[o].position));
      out[11 + o] = v != nullptr ? static_cast<float>(v->oscillator(o).sounding()) : 0.0F;
    }
    for (std::size_t p = 0; p < 5; ++p)
      out[14 + p] = peaks_[p];
    for (std::size_t f = 0; f < filter_count; ++f) {
      out[19 + f] =
          static_cast<float>(v != nullptr ? v->filter_cutoff(f) : get(x.filter[f].cutoff));
      out[21 + f] =
          static_cast<float>(v != nullptr ? v->filter_resonance(f) : get(x.filter[f].resonance));
    }
    for (std::size_t e = 0; e < envelope_count; ++e) {
      if (v == nullptr)
        continue;
      const auto& env = v->envelope(e);
      out[23 + e] = static_cast<float>(env.level());
      out[27 + e] = static_cast<float>(static_cast<int>(env.stage()));
      out[31 + e] = static_cast<float>(env.progress());
    }
    const auto& sources = v != nullptr ? v->sources().value : idle_sources_.value;
    for (std::size_t l = 0; l < lfo_count; ++l) {
      const bool global = get(x.lfo[l].scope) >= 0.5;
      out[35 + l] = static_cast<float>(
          v != nullptr ? sources[static_cast<std::size_t>(Source::lfo1) + l] : global_.lfo[l]);
      out[43 + l] =
          static_cast<float>(global || v == nullptr ? global_.lfo_phase[l] : v->lfo_phase(l));
    }
    for (std::size_t m = 0; m < macro_count; ++m)
      out[51 + m] = static_cast<float>(global_.macro[m]);
    const std::array<Source, 10> rest{Source::velocity,   Source::note,      Source::modwheel,
                                      Source::aftertouch, Source::mpe_slide, Source::mpe_pressure,
                                      Source::chaos1,     Source::chaos2,    Source::random,
                                      Source::bend};
    for (std::size_t i = 0; i < rest.size(); ++i) {
      const auto s = static_cast<std::size_t>(rest[i]);
      out[59 + i] =
          static_cast<float>(v != nullptr ? sources[s]
                                          : (rest[i] == Source::chaos1   ? global_.chaos[0]
                                             : rest[i] == Source::chaos2 ? global_.chaos[1]
                                             : rest[i] == Source::bend   ? global_.bend
                                                                         : 0.0));
    }
    FxRack::Telemetry fx{};
    rack_.telemetry(fx);
    for (std::size_t i = 0; i < fx.meters.size(); ++i)
      out[69 + i] = fx.meters[i];
    const auto db = [](float peak) {
      return peak > 1e-6F ? std::max(-120.0F, 20.0F * std::log10(peak)) : -120.0F;
    };
    out[75] = db(out_peak_[0]);
    out[76] = db(out_peak_[1]);
    out[77] = static_cast<float>(generator_.playing_clip());
    out[78] = static_cast<float>(generator_.clip_playhead_beats());
    out[79] = static_cast<float>(generator_.arp_step());
    for (std::size_t s = 0; s < route_slots; ++s)
      out[80 + s] = static_cast<float>(v != nullptr ? v->modulated().contribution(s)
                                                    : global_mod_.contribution(s));
    for (std::size_t o = 0; o < oscillator_count; ++o) {
      if (v != nullptr)
        v->oscillator(o).snapshot(std::span<float, 128>(out.data() + 336 + 128 * o, 128));
    }
    const auto put = [&](std::size_t at, std::span<const float> values) {
      std::copy(values.begin(), values.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
    };
    put(telemetry_fx, fx.live);
    put(telemetry_slot_in, fx.input);
    put(telemetry_slot_out, fx.output);
    put(telemetry_slot_detail, fx.detail);
    put(telemetry_slot_spectrum, fx.spectrum);
    for (std::size_t i = 0; i < telemetry_voice_source_count; ++i) {
      const auto s = static_cast<std::size_t>(Source::voice_mod_a) + i;
      out[telemetry_voice_sources + i] =
          static_cast<float>(v != nullptr ? sources[s] : voice_spread_centre);
    }
    // By voice card (the slot without cards); the newest note on a card
    // shows.
    std::array<const Voice*, telemetry_cards> on_card{};
    for (const auto& voice : *voices_) {
      if (!voice.active() || voice.dying() || voice.card() >= telemetry_cards)
        continue;
      auto& shown = on_card[voice.card()];
      if (shown == nullptr || voice.order() > shown->order())
        shown = &voice;
    }
    for (std::size_t card = 0; card < telemetry_cards; ++card) {
      const Voice* voice = on_card[card];
      out[telemetry_card_drift + card] =
          voice != nullptr ? static_cast<float>(voice->analogue_drift_cents()) : 0.0F;
      out[telemetry_card_key + card] =
          voice != nullptr ? static_cast<float>(voice->note().key) : -1.0F;
    }
    out[telemetry_arp] =
        static_cast<float>(v != nullptr ? sources[static_cast<std::size_t>(Source::arp_mod)] : 0.0);
    out[telemetry_arp + 1] = static_cast<float>(
        v != nullptr ? sources[static_cast<std::size_t>(Source::arp_accent)] : 0.0);
    out[telemetry_arp + 2] = generator_.arp_variation() ? 1.0F : 0.0F;
    // Peaks are since the last frame.
    peaks_.fill(0.0F);
    out_peak_ = {};
    return telemetry_size;
  }

private:
  void render_voices(std::span<engine::StereoFrame> out) noexcept {
    std::size_t sounding = 0;
    for (std::size_t v = 0; v < voices_->size(); ++v) {
      if ((*voices_)[v].active())
        (*parallel_)[sounding++].voice = v;
    }
    if (workers_ != nullptr && sounding >= min_parallel_voices &&
        out.size() <= control_interval_frames && workers_->attached_workers() != 0) {
      render_voices_apart(out, sounding);
    } else {
      for (auto& voice : *voices_) {
        if (voice.active())
          voice.render(out, (*scratch_)[0]);
      }
    }
    for (auto& voice : *voices_) {
      if (voice.active())
        voice.take_peaks(peaks_);
    }
  }

  // Each sounding voice renders into its own buffer on whichever helper
  // takes it, with that helper's scratch; then the buffers add into `out`
  // in voice order. Every frame therefore sums the same values in the same
  // order as rendering the voices one after another into `out`.
  void render_voices_apart(std::span<engine::StereoFrame> out, std::size_t sounding) noexcept {
    struct Job {
      NovaKernel& kernel;
      std::size_t frames;
    } job{*this, out.size()};
    workers_->run_nested(
        sounding,
        [](void* context, std::size_t task, std::size_t lane) noexcept {
          auto& voice_job = *static_cast<Job*>(context);
          auto& kernel = voice_job.kernel;
          auto& apart = (*kernel.parallel_)[task];
          const auto buffer = std::span(apart.output).first(voice_job.frames);
          std::ranges::fill(buffer, engine::StereoFrame{voice_sum_identity, voice_sum_identity});
          (*kernel.voices_)[apart.voice].render(buffer, (*kernel.scratch_)[lane]);
        },
        &job);
    for (std::size_t n = 0; n < sounding; ++n) {
      const auto& output = (*parallel_)[n].output;
      for (std::size_t f = 0; f < out.size(); ++f) {
        out[f].left += output[f].left;
        out[f].right += output[f].right;
      }
    }
  }

  void start_note(const GeneratedNote& generated) noexcept {
    const NoteEvent& note = generated.note;
    const auto& x = index();
    const auto voicing = as<Voicing>(values_[x.voicing]);
    if (voicing != Voicing::poly) {
      if (held_count_ < held_.size())
        held_[held_count_++] = note;
      Voice* mono = nullptr;
      for (auto& voice : *voices_) {
        if (voice.active() && !voice.dying())
          mono = mono == nullptr || voice.order() > mono->order() ? &voice : mono;
      }
      const bool legato = voicing == Voicing::legato && mono != nullptr && !mono->released();
      if (mono != nullptr && !legato && !mono->released()) {
        // Mono: the same voice retriggers, carrying its phases.
        mono->set_arp(generated.mod, generated.accent);
        mono->start(note, tick_, last_key_, true, ++clock_, note_frame_, notes_started_);
        last_key_ = note.key;
        return;
      }
      if (mono != nullptr && legato) {
        mono->set_arp(generated.mod, generated.accent);
        mono->start(note, tick_, last_key_, true, ++clock_, note_frame_, notes_started_);
        last_key_ = note.key;
        return;
      }
    }
    const auto limit = static_cast<std::size_t>(std::lround(values_[x.polyphony]));
    std::size_t playing = 0;
    for (const auto& voice : *voices_)
      playing += voice.active() && !voice.dying() ? 1U : 0U;
    if (playing >= limit) {
      // Steal: the oldest released voice, else the quietest.
      Voice* victim = nullptr;
      for (auto& voice : *voices_) {
        if (!voice.active() || voice.dying())
          continue;
        if (victim == nullptr)
          victim = &voice;
        else if (voice.released() != victim->released())
          victim = voice.released() ? &voice : victim;
        else if (voice.released() ? voice.order() < victim->order()
                                  : voice.loudness() < victim->loudness())
          victim = &voice;
      }
      if (victim != nullptr) {
        victim->kill();
        ++steals_;
      }
    }
    Voice* free = nullptr;
    for (auto& voice : *voices_) {
      if (!voice.active()) {
        free = &voice;
        break;
      }
    }
    if (free == nullptr)
      return;
    // A fresh voice: the note's number picks its voice card (ADR-0073).
    free->set_arp(generated.mod, generated.accent);
    free->start(note, tick_, last_key_, false, ++clock_, note_frame_, notes_started_++);
    last_key_ = note.key;
  }

  void stop_note(const NoteEvent& note) noexcept {
    const auto& x = index();
    const auto voicing = as<Voicing>(values_[x.voicing]);
    if (voicing != Voicing::poly) {
      // Last-note priority: fall back to the newest key still held.
      std::size_t kept = 0;
      for (std::size_t i = 0; i < held_count_; ++i) {
        if (held_[i].note != note.note)
          held_[kept++] = held_[i];
      }
      held_count_ = kept;
      for (auto& voice : *voices_) {
        if (!voice.active() || voice.dying() || voice.note().note != note.note)
          continue;
        if (held_count_ > 0) {
          voice.start(held_[held_count_ - 1], tick_, last_key_, true, ++clock_, note_frame_,
                      notes_started_);
          last_key_ = held_[held_count_ - 1].key;
        } else {
          voice.release();
        }
      }
      return;
    }
    for (auto& voice : *voices_) {
      if (voice.active() && !voice.released() && voice.note().note == note.note &&
          voice.note().clip == note.clip)
        voice.release();
    }
  }

  double rate_;
  const NovaState* state_ = nullptr;
  std::span<const double> values_{};
  SynthTransport transport_{};
  std::uint32_t since_tick_ = 0;
  std::uint32_t tick_frames_ = control_interval_frames;
  double free_seconds_ = 0.0;
  Modulated global_mod_;
  GlobalSources global_{};
  VoiceTick tick_{};
  std::array<Lfo, lfo_count> lfo_{};
  Chaos chaos_;
  std::unique_ptr<std::array<Voice, voice_capacity>> voices_;
  // One per thread that can render voices at once (lanes of a nested batch).
  std::unique_ptr<std::array<VoiceScratch, engine::AudioWorkerPool::max_lanes>> scratch_ =
      std::make_unique<std::array<VoiceScratch, engine::AudioWorkerPool::max_lanes>>();
  // A sounding voice rendered apart: its slot, and its share of one render.
  struct VoiceApart {
    std::size_t voice = 0;
    std::array<engine::StereoFrame, control_interval_frames> output{};
  };
  std::unique_ptr<std::array<VoiceApart, voice_capacity>> parallel_ =
      std::make_unique<std::array<VoiceApart, voice_capacity>>();
  engine::AudioWorkerPool* workers_ = nullptr;
  const Voice* newest_ = nullptr;
  SourceValues idle_sources_{};
  FxRack rack_;
  NoteGenerator generator_;
  std::array<GeneratedNote, max_pending_notes> pending_{};
  std::size_t pending_count_ = 0;
  std::array<GeneratedNote, max_generated_notes> generated_{};
  std::array<NoteEvent, 128> held_{};
  std::size_t held_count_ = 0;
  double last_key_ = -1.0;
  std::uint64_t clock_ = 0;
  // Notes that took a fresh voice since the last reset: the voice card
  // counter (nova.analogue.cards) and each note's re-roll seed.
  std::uint64_t notes_started_ = 0;
  std::uint32_t steals_ = 0;
  double master_ = 1.0, master_step_ = 0.0;
  LowEndLock lock_;
  std::uint64_t frame_ = 0;
  std::uint64_t note_frame_ = 0; // The project frame of the note being started.
  // Written by render, read and cleared by telemetry (both on the callback).
  mutable std::array<float, 5> peaks_{};
  mutable std::array<float, 2> out_peak_{};
  double cpu_ = 0.0;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<NovaKernel>(sample_rate);
}

// The amp release and room for effect tails, within export's ten-second
// bound for a native synth's tail.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const double seconds = std::min(10.0, 1.15 * values[index().env[0].release] + 6.0);
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

bool lock_enabled(std::span<const double> values) {
  return values.size() == descriptor().parameters.size() && values[index().lock_on] >= 0.5;
}

} // namespace

const SynthEngine& engine() {
  // Lock follows Anvil's hits (the kick).
  static const SynthEngine nova_engine{
      &descriptor(),     &create_kernel,   &tail_frames,     false,        &validate_document,
      &prepare_document, &preset_document, lock_kick_engine, &lock_enabled};
  return nova_engine;
}

} // namespace oscillate::native_dsp::synth::nova
