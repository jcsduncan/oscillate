#include "stick_chain.hpp"

#include "oscillate/native_dsp/effects/forge.hpp"
#include "oscillate/native_dsp/effects/hush.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/effects/strike.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "stick_table.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace oscillate::native_dsp::synth::stick::detail {
namespace {

namespace st = stick_table;

// A stage switches in or out over this long.
constexpr double crossfade_seconds = 0.01;
// Levels at the bottom of their range are off.
constexpr double send_off_db = st::off_db;
constexpr double decibels = 20.0;
// A cut at the end of its range is off; within this of it, too.
constexpr double cut_margin = 1e-6;
constexpr double flat_db = 1e-6;

// The EQ's fixed corners: its low shelf, its bell's Q and its high shelf.
constexpr double low_shelf_hz = 100.0;
constexpr double mid_q = 1.0;
constexpr double high_shelf_hz = 8'000.0;
// The gate shuts fully, as a drum gate does.
constexpr double gate_range_db = -80.0;
// Forge's curves for each SatModel (by Forge's names); its Clip for the kit bus.
constexpr std::array<std::string_view, sat_model_count> forge_models{"Tape", "Tube", "Transistor",
                                                                     "Clip"};
constexpr std::string_view forge_clip = "Clip";
constexpr double on = 1.0;
constexpr double off = 0.0;
constexpr std::size_t missing = std::numeric_limits<std::size_t>::max();

[[nodiscard]] double gain_of(double db) noexcept {
  return db <= send_off_db ? 0.0 : std::pow(10.0, db / decibels);
}

[[nodiscard]] std::size_t at(const effects::EffectEngine& engine, std::string_view id) {
  const auto found = find_parameter(*engine.descriptor, id);
  return found ? *found : missing;
}

// A choice parameter's value for the choice named `name`.
[[nodiscard]] double choice_of(const effects::EffectEngine& engine, std::string_view id,
                               std::string_view name) {
  const auto index = at(engine, id);
  if (index == missing) {
    return -1.0;
  }
  const auto choices = engine.descriptor->parameters[index].choices;
  const auto found = std::ranges::find(choices, name);
  return found == choices.end() ? -1.0 : static_cast<double>(found - choices.begin());
}

// Each effect's parameters the chain sets, looked up once.
struct Ids {
  struct {
    std::size_t threshold, range, release, mode, key;
    double gate, input; // The Gate mode, keyed from the Input.
  } hush;
  struct {
    std::size_t on, kind, freq, q, gain;
    double kind_value;
  } shape[5]; // High-pass, low shelf, bell, high shelf, low-pass.
  struct {
    std::size_t threshold, ratio, attack, release, makeup;
  } press;
  struct {
    std::size_t bands, attack, sustain;
    double one_band;
  } strike;
  struct {
    std::size_t model, drive, mix, auto_gain;
    std::array<double, sat_model_count> models;
    double clip;
  } forge;
  bool ok = true;
};

// Shape's bands the EQ uses, in the order above, and the kind each plays.
constexpr std::array<int, 5> shape_bands{1, 2, 3, 7, 8};
constexpr std::array<std::string_view, 5> shape_kinds{"High Pass", "Low Shelf", "Peak",
                                                      "High Shelf", "Low Pass"};
enum ShapeBand : std::size_t { band_cut_low, band_low, band_mid, band_high, band_cut_high };

const Ids& ids() {
  static const Ids found = [] {
    Ids i{};
    const auto& hush = effects::hush::engine();
    const auto& shape = effects::shape::engine();
    const auto& press = effects::press::engine();
    const auto& strike = effects::strike::engine();
    const auto& forge = effects::forge::engine();
    i.hush = {at(hush, "hush.threshold"),
              at(hush, "hush.range"),
              at(hush, "hush.release"),
              at(hush, "hush.mode"),
              at(hush, "hush.key"),
              choice_of(hush, "hush.mode", "Gate"),
              choice_of(hush, "hush.key", "Input")};
    for (std::size_t b = 0; b < shape_bands.size(); ++b) {
      const auto n = std::to_string(shape_bands[b]);
      i.shape[b] = {
          at(shape, "shape.on_" + n),   at(shape, "shape.kind_" + n),
          at(shape, "shape.freq_" + n), at(shape, "shape.q_" + n),
          at(shape, "shape.gain_" + n), choice_of(shape, "shape.kind_" + n, shape_kinds[b])};
    }
    i.press = {at(press, "press.threshold"), at(press, "press.ratio"), at(press, "press.attack"),
               at(press, "press.release"), at(press, "press.makeup")};
    i.strike = {at(strike, "strike.bands"), at(strike, "strike.band_1.attack"),
                at(strike, "strike.band_1.sustain"), choice_of(strike, "strike.bands", "1")};
    i.forge = {at(forge, "forge.model"),
               at(forge, "forge.drive"),
               at(forge, "forge.mix"),
               at(forge, "forge.auto_gain"),
               {},
               choice_of(forge, "forge.model", forge_clip)};
    for (std::size_t m = 0; m < sat_model_count; ++m) {
      i.forge.models[m] = choice_of(forge, "forge.model", forge_models[m]);
    }
    const auto valid = [&](std::initializer_list<std::size_t> list) {
      return std::ranges::none_of(list, [&](std::size_t x) { return x == missing; });
    };
    const auto chosen = [&](std::initializer_list<double> list) {
      return std::ranges::none_of(list, [&](double x) { return x < 0.0; });
    };
    i.ok = chosen({i.hush.gate, i.hush.input, i.strike.one_band, i.forge.clip}) &&
           std::ranges::all_of(i.forge.models, [](double x) { return x >= 0.0; });
    i.ok = i.ok && valid({i.hush.threshold, i.hush.range, i.hush.release, i.hush.mode, i.hush.key,
                          i.press.threshold, i.press.ratio, i.press.attack, i.press.release,
                          i.press.makeup, i.strike.bands, i.strike.attack, i.strike.sustain,
                          i.forge.model, i.forge.drive, i.forge.mix, i.forge.auto_gain});
    for (const auto& band : i.shape) {
      i.ok = i.ok && valid({band.on, band.kind, band.freq, band.q, band.gain}) &&
             band.kind_value >= 0.0;
    }
    return i;
  }();
  return found;
}

// ---- Stick's values onto each effect's ---------------------------------------------------------

void map_gate(std::span<const double> v, std::size_t base, std::span<double> out) noexcept {
  const auto& i = ids().hush;
  out[i.threshold] = v[base + st::mic::gate_threshold];
  out[i.release] = v[base + st::mic::gate_release];
  out[i.range] = gate_range_db;
  out[i.mode] = i.gate;
  out[i.key] = i.input;
}

// An EQ's controls from `base` (its switch) on.
void map_eq(std::span<const double> v, std::size_t base, std::span<double> out) noexcept {
  const auto& bands = ids().shape;
  const auto band = [&](ShapeBand b, bool enabled, double hz, double q, double db) {
    out[bands[b].on] = enabled ? on : off;
    out[bands[b].kind] = bands[b].kind_value;
    out[bands[b].freq] = hz;
    out[bands[b].q] = q;
    out[bands[b].gain] = db;
  };
  constexpr double butterworth_q = 0.707;
  const double low_cut = v[base + st::eq::low_cut];
  const double high_cut = v[base + st::eq::high_cut];
  const double low = v[base + st::eq::low];
  const double mid = v[base + st::eq::mid];
  const double high = v[base + st::eq::high];
  band(band_cut_low, low_cut > st::low_cut_min_hz * (1.0 + cut_margin), low_cut, butterworth_q,
       0.0);
  band(band_low, std::fabs(low) > flat_db, low_shelf_hz, mid_q, low);
  band(band_mid, std::fabs(mid) > flat_db, v[base + st::eq::mid_freq], mid_q, mid);
  band(band_high, std::fabs(high) > flat_db, high_shelf_hz, mid_q, high);
  band(band_cut_high, high_cut < st::high_cut_max_hz * (1.0 - cut_margin), high_cut, butterworth_q,
       0.0);
}

// A compressor's controls from `base` (its threshold) on.
void map_comp(std::span<const double> v, std::size_t base, std::span<double> out) noexcept {
  const auto& i = ids().press;
  out[i.threshold] = v[base];
  out[i.ratio] = v[base + 1];
  out[i.attack] = v[base + 2];
  out[i.release] = v[base + 3];
  out[i.makeup] = v[base + 4];
}

void map_shaper(std::span<const double> v, std::size_t base, std::span<double> out) noexcept {
  const auto& i = ids().strike;
  out[i.bands] = i.one_band; // Minimum phase, no lookahead (the defaults): no latency.
  out[i.attack] = v[base + st::mic::attack];
  out[i.sustain] = v[base + st::mic::sustain];
}

void map_sat(std::span<const double> v, std::size_t base, std::span<double> out) noexcept {
  const auto& i = ids().forge;
  const auto model = static_cast<std::size_t>(std::clamp(
      std::lround(v[base + st::mic::sat_model]), 0L, static_cast<long>(sat_model_count) - 1));
  out[i.model] = i.models[model];
  out[i.drive] = v[base + st::mic::sat_drive];
  out[i.mix] = v[base + st::mic::sat_mix];
  out[i.auto_gain] = on;
}

void map_clip(std::span<const double> v, std::span<double> out) noexcept {
  const auto& i = ids().forge;
  out[i.model] = i.clip;
  out[i.drive] = v[st::index_of("stick.bus.clip_drive")];
  out[i.mix] = on;
  out[i.auto_gain] = on;
}

[[nodiscard]] bool switched(std::span<const double> v, std::size_t index) noexcept {
  return v[index] >= 0.5;
}

constexpr std::size_t parallel_threshold = st::index_of("stick.parallel.threshold");
constexpr std::size_t parallel_level = st::index_of("stick.parallel.level");
constexpr std::size_t bus_comp = st::index_of("stick.bus.comp");
constexpr std::size_t bus_comp_threshold = st::index_of("stick.bus.comp_threshold");
constexpr std::size_t bus_eq = st::index_of("stick.bus.eq");
constexpr std::size_t bus_clip = st::index_of("stick.bus.clip");

// Each stage's switch, from its group's first control.
constexpr std::array<std::size_t, stage_count> stage_switches{
    st::mic::gate, st::mic::eq, st::mic::comp, st::mic::shaper, st::mic::sat};

// Whether the group's stage `s` is switched on.
[[nodiscard]] bool stage_on(std::span<const double> v, MicGroup group, Stage s) noexcept {
  return switched(v, st::mic_index(group, stage_switches[static_cast<std::size_t>(s)]));
}

// The group's stage `s` mapped into `out`.
void map_stage(std::span<const double> v, MicGroup group, Stage s, std::span<double> out) noexcept {
  const auto base = st::mic_index(group, 0);
  switch (s) {
  case Stage::gate:
    map_gate(v, base, out);
    return;
  case Stage::eq:
    map_eq(v, base + st::mic::eq, out);
    return;
  case Stage::comp:
    map_comp(v, base + st::mic::comp_threshold, out);
    return;
  case Stage::shaper:
    map_shaper(v, base, out);
    return;
  case Stage::sat:
    map_sat(v, base, out);
    return;
  }
}

[[nodiscard]] const effects::EffectEngine& stage_engine(Stage s) noexcept {
  switch (s) {
  case Stage::gate:
    return effects::hush::engine();
  case Stage::eq:
    return effects::shape::engine();
  case Stage::comp:
    return effects::press::engine();
  case Stage::shaper:
    return effects::strike::engine();
  case Stage::sat:
    break;
  }
  return effects::forge::engine();
}

} // namespace

// ---- Insert ------------------------------------------------------------------------------------

bool Insert::init(const effects::EffectEngine& engine, double sample_rate, std::size_t max_frames) {
  engine_ = &engine;
  values_ = default_values(*engine.descriptor);
  if (engine.latency_frames != nullptr && engine.latency_frames(values_, sample_rate) != 0) {
    return false;
  }
  kernel_ = engine.create_kernel(values_, sample_rate);
  dry_.assign(std::max<std::size_t>(1, max_frames), engine::StereoFrame{0.0F, 0.0F});
  fade_frames_ = std::max(1.0, crossfade_seconds * sample_rate);
  return kernel_ != nullptr;
}

void Insert::control(bool on, std::uint32_t frames, const Transport& transport) noexcept {
  wet_from_ = wet_to_;
  const double step = static_cast<double>(std::max<std::uint32_t>(1, frames)) / fade_frames_;
  wet_to_ = on ? std::min(1.0, wet_from_ + step) : std::max(0.0, wet_from_ - step);
  if (!primed_) {
    // The first tick takes the stage as it is set, as an export starts.
    wet_from_ = wet_to_ = on ? 1.0 : 0.0;
    primed_ = true;
  }
  span_ = static_cast<double>(std::max<std::uint32_t>(1, frames));
  if (!sounding()) {
    return;
  }
  if (wet_from_ == 0.0) {
    kernel_->reset(); // Coming in: no history from before it was off.
  }
  kernel_->control(values_, std::max<std::uint32_t>(1, frames), transport);
}

void Insert::process(Frames io, std::size_t since) noexcept {
  if (!sounding()) {
    return;
  }
  const bool settled = wet_from_ == 1.0 && wet_to_ == 1.0;
  const std::size_t n = std::min(io.size(), dry_.size());
  if (!settled) {
    std::copy_n(io.begin(), n, dry_.begin());
  }
  kernel_->process(io.first(n), {});
  if (settled) {
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const double u = std::clamp((static_cast<double>(since + i) + 1.0) / span_, 0.0, 1.0);
    const auto w = static_cast<float>(wet_from_ + u * (wet_to_ - wet_from_));
    io[i].left = dry_[i].left + w * (io[i].left - dry_[i].left);
    io[i].right = dry_[i].right + w * (io[i].right - dry_[i].right);
  }
}

void Insert::reset() noexcept {
  if (kernel_ != nullptr) {
    kernel_->reset();
  }
}

// ---- Chain -------------------------------------------------------------------------------------

bool Chain::init(double sample_rate, std::size_t max_frames) {
  if (!ids().ok) {
    return false;
  }
  bool ok = true;
  for (auto& group : groups_) {
    for (std::size_t s = 0; s < stage_count; ++s) {
      ok = group.stages[s].init(stage_engine(static_cast<Stage>(s)), sample_rate, max_frames) && ok;
    }
  }
  ok = parallel_.init(effects::press::engine(), sample_rate, max_frames) && ok;
  ok = glue_.init(effects::press::engine(), sample_rate, max_frames) && ok;
  ok = kit_eq_.init(effects::shape::engine(), sample_rate, max_frames) && ok;
  ok = clip_.init(effects::forge::engine(), sample_rate, max_frames) && ok;
  return ok;
}

void Chain::control(std::span<const double> values, std::uint32_t frames,
                    const Transport& transport) noexcept {
  parallel_open_ = false;
  for (std::size_t g = 0; g < mic_group_count; ++g) {
    auto& group = groups_[g];
    const auto as = static_cast<MicGroup>(g);
    for (std::size_t s = 0; s < stage_count; ++s) {
      auto& insert = group.stages[s];
      const auto stage = static_cast<Stage>(s);
      const bool switched_on = stage_on(values, as, stage);
      // A stage off and faded out is not mapped, nor run.
      if (switched_on || insert.sounding()) {
        map_stage(values, as, stage, insert.values());
      }
      insert.control(switched_on, frames, transport);
    }
    const double send = gain_of(values[st::mic_index(as, st::mic::parallel)]);
    group.send_from = primed_ ? group.send_to : send;
    group.send_to = send;
    parallel_open_ = parallel_open_ || group.send_from > 0.0 || group.send_to > 0.0;
  }
  if (parallel_open_ || parallel_.sounding()) {
    map_comp(values, parallel_threshold, parallel_.values());
  }
  parallel_.control(parallel_open_, frames, transport);
  parallel_open_ = parallel_open_ || parallel_.sounding();
  const double back = gain_of(values[parallel_level]);
  return_from_ = primed_ ? return_to_ : back;
  return_to_ = back;
  const bool glue = switched(values, bus_comp);
  if (glue || glue_.sounding()) {
    map_comp(values, bus_comp_threshold, glue_.values());
  }
  glue_.control(glue, frames, transport);
  const bool eq = switched(values, bus_eq);
  if (eq || kit_eq_.sounding()) {
    map_eq(values, bus_eq, kit_eq_.values());
  }
  kit_eq_.control(eq, frames, transport);
  const bool clip = switched(values, bus_clip);
  if (clip || clip_.sounding()) {
    map_clip(values, clip_.values());
  }
  clip_.control(clip, frames, transport);
  primed_ = true;
}

bool Chain::routed(MicGroup group) const noexcept {
  const auto& g = groups_[static_cast<std::size_t>(group)];
  return g.send_from > 0.0 || g.send_to > 0.0 || std::ranges::any_of(g.stages, &Insert::sounding);
}

void Chain::process_group(MicGroup group, Frames io, std::size_t since) noexcept {
  for (auto& stage : groups_[static_cast<std::size_t>(group)].stages) {
    stage.process(io, since);
  }
}

double Chain::send(MicGroup group, double u) const noexcept {
  const auto& g = groups_[static_cast<std::size_t>(group)];
  return g.send_from + u * (g.send_to - g.send_from);
}

void Chain::process_parallel(Frames io, std::size_t since) noexcept {
  parallel_.process(io, since);
}

double Chain::parallel_return(double u) const noexcept {
  return return_from_ + u * (return_to_ - return_from_);
}

bool Chain::kit_sounding() const noexcept {
  return glue_.sounding() || kit_eq_.sounding() || clip_.sounding();
}

void Chain::process_kit(Frames io, std::size_t since) noexcept {
  glue_.process(io, since);
  kit_eq_.process(io, since);
  clip_.process(io, since);
}

void Chain::reset() noexcept {
  for (auto& group : groups_) {
    for (auto& stage : group.stages) {
      stage.reset();
    }
  }
  parallel_.reset();
  glue_.reset();
  kit_eq_.reset();
  clip_.reset();
}

std::uint64_t Chain::tail_frames(std::span<const double> values, double sample_rate) {
  if (values.size() != st::parameters.size() || !ids().ok) {
    return 0;
  }
  // A stage's own tail, when it is switched on.
  const auto tail = [&](const effects::EffectEngine& engine, auto&& map) -> std::uint64_t {
    auto mapped = default_values(*engine.descriptor);
    if (!map(std::span<double>(mapped))) {
      return 0;
    }
    return engine.tail_frames(mapped, sample_rate);
  };
  std::uint64_t groups = 0;
  bool parallel = false;
  for (std::size_t g = 0; g < mic_group_count; ++g) {
    const auto group = static_cast<MicGroup>(g);
    std::uint64_t chain = 0;
    for (std::size_t s = 0; s < stage_count; ++s) {
      const auto stage = static_cast<Stage>(s);
      chain += tail(stage_engine(stage), [&](std::span<double> out) {
        map_stage(values, group, stage, out);
        return stage_on(values, group, stage);
      });
    }
    groups = std::max(groups, chain);
    parallel = parallel || gain_of(values[st::mic_index(group, st::mic::parallel)]) > 0.0;
  }
  std::uint64_t buses = 0;
  if (parallel) {
    buses += tail(effects::press::engine(), [&](std::span<double> out) {
      map_comp(values, parallel_threshold, out);
      return true;
    });
  }
  buses += tail(effects::press::engine(), [&](std::span<double> out) {
    map_comp(values, bus_comp_threshold, out);
    return switched(values, bus_comp);
  });
  buses += tail(effects::shape::engine(), [&](std::span<double> out) {
    map_eq(values, bus_eq, out);
    return switched(values, bus_eq);
  });
  buses += tail(effects::forge::engine(), [&](std::span<double> out) {
    map_clip(values, out);
    return switched(values, bus_clip);
  });
  return groups + buses;
}

} // namespace oscillate::native_dsp::synth::stick::detail
