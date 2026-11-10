#include "oscillate/native_dsp/master_assistant.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/keel.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/match_eq.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// The master assistant's proposal and its offline chain (ADR-0124).
namespace oscillate::native_dsp::assistant {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using Values = std::vector<std::pair<std::string, double>>;

// ---- The rules' settings, per character (clean, punchy, warm) -------------------------

struct CeilingStyle {
  double transients;
  double clip_db;
  double shape;
  double release_ms;
  double character;
  double transient_link;
};
// Clean is the Transparent preset's limiter; Punchy lets every onset
// through to a hard knee; Warm rounds peaks in a softer clip.
constexpr std::array<CeilingStyle, character_count> ceiling_styles{{
    {0.3, 0.0, 0.1, 150.0, 0.8, 1.0},
    {1.0, 0.5, 0.0, 100.0, 0.6, 0.5},
    {0.5, 1.0, 0.4, 120.0, 0.7, 1.0},
}};
// Louder than this, the clipper takes a little more of the peaks.
constexpr double loud_master_lufs = -8.0;
constexpr double loud_clip_extra_db = 0.5;

struct DynamicsStyle {
  double ratio;
  double attack_ms;
  double release_ms;
};
// Every attack lets a kick's first 20 ms through (Punchy more); Clean is
// gentle, Warm leans in and lets go slowly.
constexpr std::array<DynamicsStyle, character_count> dynamics_styles{{
    {1.5, 30.0, 150.0},
    {2.0, 40.0, 100.0},
    {2.5, 20.0, 250.0},
}};
constexpr double compressor_knee_db = 6.0;
// The detector ignores the sub, so the kick does not pump the master.
constexpr double compressor_detector_hz = 60.0;
// Strata's range: twice the reduction asked of a band, at least the minimum.
constexpr double strata_range_factor = 2.0;
constexpr double strata_min_range_db = 3.0;
// Three bands at the regions' splits (choice indices are zero-based), and
// the linear-phase banks, whose bands sum to the input delayed: no phase
// turn smears the kick.
constexpr double three_bands = 2.0;
constexpr std::string_view linear_phase = "Linear";

// Gild: Air Lift scaled from its least to its preset drive as the top's
// deficit grows from the threshold to twice it; Warm Mids at a light drive.
constexpr double gild_air_min_drive = 0.4;
constexpr double gild_air_max_drive = 0.65;
constexpr double gild_warm_drive = 0.3;
// Strike's lift on the lows' and the mids' attack.
constexpr double strike_low_attack_db = 2.0;
constexpr double strike_mid_attack_db = 1.0;

constexpr double ceiling_gain_min_db = 0.0;
constexpr double ceiling_gain_max_db = 24.0;
constexpr double ceiling_target_min = -30.0;
constexpr double ceiling_target_max = -4.0;
constexpr double ceiling_min_dbtp = -30.0;

// The offline chain's nominal tempo (no device here follows it).
constexpr double run_bpm = 120.0;
constexpr double beats_per_bar = 4.0;
constexpr double seconds_per_minute = 60.0;
// A secant step needs at least this much change in loudness.
constexpr double secant_min_lu = 1e-3;

std::string fixed(double value, int digits) {
  std::array<char, 32> text{};
  (void)std::snprintf(text.data(), text.size(), "%.*f", digits, value);
  return text.data();
}
std::string signed_db(double value, int digits = 1) {
  return (value >= 0.0 ? "+" : "") + fixed(value, digits) + " dB";
}
std::string hz_text(double hz) {
  constexpr double kilo = 1'000.0;
  return hz >= kilo ? fixed(hz / kilo, hz >= 10.0 * kilo ? 0 : 1) + " kHz" : fixed(hz, 0) + " Hz";
}

const EngineDescriptor& table(std::string_view engine) {
  return *effects::find_engine(engine)->descriptor;
}

// The engine's defaults with `set` applied, every parameter named.
Values full_values(std::string_view engine, std::span<const double> values) {
  const auto& t = table(engine);
  Values out;
  out.reserve(t.parameters.size());
  for (std::size_t i = 0; i < t.parameters.size(); ++i)
    out.emplace_back(std::string(t.parameters[i].id), sanitise(t.parameters[i], values[i]));
  return out;
}
std::vector<double> defaults(std::string_view engine) { return default_values(table(engine)); }
void set(std::vector<double>& values, std::string_view engine, std::string_view id, double value) {
  const auto index = find_parameter(table(engine), id);
  if (index)
    values[*index] = value;
}
double choice_of(std::string_view engine, std::string_view id, std::string_view label) {
  const auto& t = table(engine);
  const auto index = find_parameter(t, id);
  if (!index)
    return 0.0;
  const auto& choices = t.parameters[*index].choices;
  const auto found = std::ranges::find(choices, label);
  return found == choices.end() ? 0.0 : static_cast<double>(found - choices.begin());
}
std::vector<double> preset(std::string_view engine, std::string_view name) {
  const auto& t = table(engine);
  for (const auto& p : t.presets) {
    if (p.name == name) {
      auto values = preset_values(t, p);
      if (values)
        return std::move(values).value();
    }
  }
  return default_values(t);
}

std::size_t region_of(std::size_t band) noexcept {
  const double hz = tonal::band_hz(band);
  return hz < low_split_hz ? 0 : (hz < high_split_hz ? 1 : 2);
}

// The centred tonal difference's mean per region, over the judged bands.
std::array<double, regions> region_deviation(const tonal::Curve& diff) {
  std::array<double, regions> sum{};
  std::array<std::size_t, regions> count{};
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    const double hz = tonal::band_hz(b);
    if (hz < tonal_judged_low_hz || hz > tonal_judged_high_hz)
      continue;
    sum[region_of(b)] += diff[b];
    ++count[region_of(b)];
  }
  for (std::size_t r = 0; r < regions; ++r)
    sum[r] = count[r] == 0 ? 0.0 : sum[r] / static_cast<double>(count[r]);
  return sum;
}

double top_deviation(const tonal::Curve& diff) {
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    const double hz = tonal::band_hz(b);
    if (hz >= gild_top_hz && hz <= tonal_judged_high_hz) {
      sum += diff[b];
      ++count;
    }
  }
  return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

// ---- The rules -----------------------------------------------------------------------

std::vector<Nudge> nudges_for(const tonal::Curve& diff, const RolePowers& roles) {
  std::vector<Nudge> out;
  const auto deviation = region_deviation(diff);
  std::array<double, role_count> wanted{};
  std::array<double, role_count> weight{};
  std::array<std::string, role_count> because{};
  for (std::size_t r = 0; r < regions; ++r) {
    if (std::fabs(deviation[r]) < nudge_region_db)
      continue;
    double total = 0.0;
    for (const auto& role : roles)
      if (role)
        total += (*role)[r];
    if (total <= 0.0)
      continue;
    for (std::size_t k = 0; k < role_count; ++k) {
      if (!roles[k])
        continue;
      const double share = (*roles[k])[r] / total;
      if (share < nudge_share || std::fabs(deviation[r]) <= weight[k])
        continue;
      weight[k] = std::fabs(deviation[r]);
      wanted[k] = std::clamp(-deviation[r] * share, -nudge_max_db, nudge_max_db);
      because[k] = std::string("the ") + std::string(region_names[r]) + " sit " +
                   signed_db(deviation[r]) + " from the reference and " + fixed(share * 100.0, 0) +
                   "% of that is " + std::string(role_names[k]);
    }
  }
  for (std::size_t k = 0; k < role_count; ++k) {
    const double step = std::round(wanted[k] / nudge_step_db) * nudge_step_db;
    if (std::fabs(step) < nudge_step_db)
      continue;
    out.push_back({k, step,
                   "Rebalance " + std::string(role_names[k]) + " " + signed_db(step) + ": " +
                       because[k] + "."});
  }
  return out;
}

// Corrective EQ: Shape bands fitted to the match, or Keel when the balance
// moves too much over the song for one static curve.
void propose_eq(Proposal& out, const Analysis& mix, const Reference& reference,
                const TonalDistance& distance) {
  const std::string where = hz_text(tonal::band_hz(distance.worst_band));
  const std::string measured = "the mix sits " + fixed(distance.rms_db, 1) + " dB RMS from " +
                               reference.name + ", most at " + where + " (" +
                               signed_db(distance.worst_db) + ")";
  if (distance.rms_db < eq_min_distance_db) {
    out.notes.push_back("No corrective EQ: " + measured + ".");
    return;
  }
  if (mix.tonal_spread_db >= keel_spread_db) {
    auto values = defaults("keel");
    set(values, "keel", "keel.amount", keel_amount);
    set(values, "keel", "keel.max_boost", keel_limit_db);
    set(values, "keel", "keel.max_cut", keel_limit_db);
    set(values, "keel", "keel.speed", keel_speed_seconds);
    set(values, "keel", "keel.tilt", 0.0);
    set(values, "keel", "keel.target", choice_of("keel", "keel.target", "Curve"));
    effects::keel::set_curve(values, effects::keel::curve_from(reference.target.median));
    out.devices.push_back({"keel", "Keel", full_values("keel", values),
                           "Keel toward " + reference.name + ": " + measured +
                               ", and the balance moves " + fixed(mix.tonal_spread_db, 1) +
                               " dB through the song, so it follows over " +
                               fixed(keel_speed_seconds, 0) + " s."});
    return;
  }
  const match_eq::Settings settings{eq_amount,  eq_smoothing_octaves, eq_low_hz,
                                    eq_high_hz, eq_max_boost_db,      eq_max_cut_db};
  const auto correction = match_eq::correction(tonal::fine_levels(mix.fine_power),
                                               match_eq::target_levels(reference.target), settings);
  const auto fit = match_eq::fit(correction, settings);
  if (fit.size == 0) {
    out.notes.push_back("No corrective EQ: " + measured + ", and no band is worth adding.");
    return;
  }
  auto values = defaults("shape");
  for (const auto& [id, value] : effects::shape::band_values(std::span(fit.band).first(fit.size)))
    set(values, "shape", id, value);
  out.devices.push_back({"shape", "Shape", full_values("shape", values),
                         "Shape, " + std::to_string(fit.size) + " band" +
                             (fit.size == 1 ? "" : "s") + " toward " + reference.name + ": " +
                             measured + "."});
}

// Dynamics before the limiter, when the limiter alone was measured to cost
// more loudness than the character's comfortable share.
void propose_dynamics(Proposal& out, const Analysis& mix, const Choices& choices) {
  const auto c = static_cast<std::size_t>(out.goals.character);
  const double share = limiter_share_db[c];
  const double loss = choices.limiter_loss_db;
  if (choices.dynamics == Dynamics::none)
    return;
  const double compress = std::clamp(loss - share, dynamics_min_db, dynamics_max_db);
  const auto& style = dynamics_styles[c];
  const double slope = 1.0 - 1.0 / style.ratio;
  double lowest = HUGE_VAL;
  double highest = -HUGE_VAL;
  double mean = 0.0;
  for (const auto& region : mix.region) {
    lowest = std::min(lowest, region.crest_db);
    highest = std::max(highest, region.crest_db);
    mean += region.crest_db / static_cast<double>(regions);
  }
  const std::string need = "the limiter alone loses " + fixed(loss, 1) +
                           " LU to limiting, more than " + std::string(character_names[c]) + "'s " +
                           fixed(share, 1);
  if (choices.dynamics == Dynamics::press) {
    auto values = defaults("press");
    set(values, "press", "press.ratio", style.ratio);
    set(values, "press", "press.attack", style.attack_ms);
    set(values, "press", "press.release", style.release_ms);
    set(values, "press", "press.knee", compressor_knee_db);
    set(values, "press", "press.threshold", std::clamp(mix.loud_db - compress / slope, -60.0, 0.0));
    set(values, "press", "press.makeup", compress);
    set(values, "press", "press.detector_hp", 1.0);
    set(values, "press", "press.detector_freq", compressor_detector_hz);
    out.devices.push_back({"press", "Press", full_values("press", values),
                           "Press, " + fixed(compress, 1) + " dB at " + fixed(style.ratio, 1) +
                               ":1 on the loud parts: " + need + "."});
    return;
  }
  auto values = defaults("strata");
  set(values, "strata", "strata.bands", three_bands);
  set(values, "strata", "strata.xover_1", low_split_hz);
  set(values, "strata", "strata.xover_2", high_split_hz);
  set(values, "strata", "strata.output", compress);
  set(values, "strata", "strata.phase", choice_of("strata", "strata.phase", linear_phase));
  for (std::size_t r = 0; r < regions; ++r) {
    const auto& region = mix.region[r];
    const double amount = std::clamp(compress + region.crest_db - mean, 0.0, 2.0 * compress);
    const std::string band = "strata.band_" + std::to_string(r + 1) + ".";
    set(values, "strata", band + "above", amount > 0.0 ? style.ratio : 1.0);
    set(values, "strata", band + "threshold",
        std::clamp(region.loud_db - amount / slope, -60.0, 0.0));
    set(values, "strata", band + "knee", compressor_knee_db);
    set(values, "strata", band + "range",
        std::max(strata_min_range_db, strata_range_factor * amount));
    set(values, "strata", band + "attack", style.attack_ms);
    set(values, "strata", band + "release", style.release_ms);
  }
  const auto loudest = static_cast<std::size_t>(
      std::ranges::max_element(mix.region, {}, &RegionMeasure::crest_db) - mix.region.begin());
  out.devices.push_back({"strata", "Strata", full_values("strata", values),
                         "Strata, three bands: " + need + ", and the " +
                             std::string(region_names[loudest]) + " peak " +
                             fixed(highest - lowest, 1) + " dB harder than the rest (crest " +
                             fixed(highest, 1) + " against " + fixed(lowest, 1) + " dB)."});
}

void propose_colour(Proposal& out, const Analysis& mix, const tonal::Curve& diff,
                    const Choices& choices) {
  const auto character = out.goals.character;
  if (character == Character::clean) {
    out.notes.push_back("No exciter or transient shaper: Clean adds no colour.");
    return;
  }
  const double top = top_deviation(diff);
  if (character == Character::warm) {
    auto values = preset("gild", "Warm Mids");
    set(values, "gild", "gild.band_2.drive", gild_warm_drive);
    out.devices.push_back({"gild", "Gild", full_values("gild", values),
                           "Gild, tube harmonics from 250 Hz to 2.5 kHz for Warm; the top sits " +
                               signed_db(top) + " from the reference."});
  } else if (-top >= gild_top_deficit_db) {
    const double over = std::clamp((-top - gild_top_deficit_db) / gild_top_deficit_db, 0.0, 1.0);
    auto values = preset("gild", "Air Lift");
    set(values, "gild", "gild.band_2.drive",
        gild_air_min_drive + over * (gild_air_max_drive - gild_air_min_drive));
    out.devices.push_back({"gild", "Gild", full_values("gild", values),
                           "Gild, air over 8 kHz: the top sits " + signed_db(top) +
                               " under the reference, and harmonics lift it without hiss."});
  }
  const auto& lows = mix.region[0];
  if (choices.strike) {
    auto values = defaults("strike");
    set(values, "strike", "strike.bands", three_bands);
    set(values, "strike", "strike.phase", choice_of("strike", "strike.phase", linear_phase));
    set(values, "strike", "strike.xover_1", low_split_hz);
    set(values, "strike", "strike.xover_2", high_split_hz);
    set(values, "strike", "strike.band_1.attack", strike_low_attack_db);
    set(values, "strike", "strike.band_2.attack", strike_mid_attack_db);
    out.devices.push_back(
        {"strike", "Strike", full_values("strike", values),
         "Strike, " + signed_db(strike_low_attack_db) + " on the lows' attack: they hit " +
             fixed(lows.onsets_per_second, 1) + " times a second and the limiter costs " +
             fixed(choices.limiter_loss_db, 1) + " LU."});
  }
}

// Trim, first: the gain down when the mix is already past the target, and
// the wideband width toward a reference track's.
void propose_trim(Proposal& out, const Analysis& mix, const Reference& reference, double trim_db) {
  auto values = defaults("trim");
  std::vector<std::string> because;
  if (trim_db < 0.0) {
    set(values, "trim", "trim.gain", trim_db);
    because.push_back("gain " + signed_db(trim_db) + ": the mix is already past the target");
  }
  if (reference.traits) {
    const double mids = reference.traits->width_db[1] - mix.region[1].width_db;
    const double top = reference.traits->width_db[2] - mix.region[2].width_db;
    const double delta = 0.5 * (mids + top);
    if (std::fabs(delta) >= width_min_change_db) {
      const double width = std::clamp(dsp::db_to_gain(delta), width_min, width_max);
      set(values, "trim", "trim.width", width);
      because.push_back("width " + fixed(width * 100.0, 0) + "%: the side sits " +
                        fixed(-mix.region[1].width_db, 1) + " and " +
                        fixed(-mix.region[2].width_db, 1) +
                        " dB under the mid in the mids and highs, against " +
                        fixed(-reference.traits->width_db[1], 1) + " and " +
                        fixed(-reference.traits->width_db[2], 1) + " in " + reference.name);
    }
  }
  if (because.empty())
    return;
  std::string reason = "Trim, ";
  for (std::size_t i = 0; i < because.size(); ++i)
    reason += (i == 0 ? "" : "; ") + because[i];
  out.devices.push_back({"trim", "Trim", full_values("trim", values), reason + "."});
}

// Vista's mono bass, when the lows carry too much side for a club system.
void propose_mono_bass(Proposal& out, const Analysis& mix) {
  if (mix.low_side_db <= vista_low_side_db) {
    out.notes.push_back("No mono bass: under " + hz_text(mono_bass_hz) + " the side sits " +
                        fixed(-mix.low_side_db, 1) + " dB under the mid already.");
    return;
  }
  auto values = defaults("vista");
  set(values, "vista", "vista.mono_bass", 1.0);
  set(values, "vista", "vista.mono_freq", mono_bass_hz);
  out.devices.push_back({"vista", "Vista", full_values("vista", values),
                         "Vista, mono under " + hz_text(mono_bass_hz) +
                             ": the side there sits only " + fixed(-mix.low_side_db, 1) +
                             " dB under the mid, and a club system sums the lows."});
}

} // namespace

std::string ceiling_reason(const Proposal& proposal, double gain_db, double trim_db) {
  const auto& goals = proposal.goals;
  std::string text = "Ceiling, " + signed_db(gain_db + trim_db) + " to " + fixed(goals.lufs, 1) +
                     " LUFS under " + fixed(goals.ceiling_dbtp, 1) + " dBTP: the mix reads " +
                     fixed(proposal.mix_lufs, 1) + " LUFS with peaks at " +
                     fixed(proposal.mix_true_peak_dbtp, 1) + " dBTP; " +
                     std::string(character_names[static_cast<std::size_t>(goals.character)]) +
                     " sets the transients and clip.";
  return text;
}

std::vector<Choices> candidates(const Analysis& mix, const Goals& goals, double limiter_loss_db) {
  std::vector<Choices> out{{limiter_loss_db, Dynamics::none, false}};
  const double share = limiter_share_db[static_cast<std::size_t>(goals.character)];
  if (limiter_loss_db - share >= dynamics_min_db) {
    out.push_back({limiter_loss_db, Dynamics::press, false});
    double lowest = HUGE_VAL;
    double highest = -HUGE_VAL;
    for (const auto& region : mix.region) {
      lowest = std::min(lowest, region.crest_db);
      highest = std::max(highest, region.crest_db);
    }
    if (highest - lowest >= strata_crest_spread_db)
      out.push_back({limiter_loss_db, Dynamics::strata, false});
  }
  if (goals.character == Character::punchy &&
      mix.region[0].onsets_per_second >= strike_min_onsets &&
      limiter_loss_db >= strike_reduction_db)
    out.push_back({limiter_loss_db, Dynamics::none, true});
  return out;
}

Proposal derive(const Analysis& mix, const Reference& reference, const Goals& goals,
                const RolePowers& roles, const Choices& choices) {
  Proposal out;
  out.goals = goals;
  out.mix_lufs = mix.lufs();
  out.mix_true_peak_dbtp = mix.true_peak_dbtp;
  if (!mix.integrated_lufs) {
    out.notes.push_back("Nothing to propose: the mix is silent or under 400 ms.");
    return out;
  }
  const auto diff = tonal_difference(mix.tonal, reference.target.median);
  const auto distance = tonal_distance(mix.tonal, reference.target.median);
  out.nudges = nudges_for(diff, roles);

  // What the chain must do to the level.
  const double chain_lufs = goals.lufs - goals.after_chain_db;
  const double chain_ceiling =
      std::clamp(goals.ceiling_dbtp - goals.after_chain_db, ceiling_min_dbtp, 0.0);
  const double gain = chain_lufs - mix.lufs();

  propose_trim(out, mix, reference, std::min(gain, 0.0));
  propose_eq(out, mix, reference, distance);
  propose_dynamics(out, mix, choices);
  propose_colour(out, mix, diff, choices);
  propose_mono_bass(out, mix);

  const auto c = static_cast<std::size_t>(goals.character);
  const auto& style = ceiling_styles[c];
  auto values = defaults("ceiling");
  const double estimate = std::clamp(gain, ceiling_gain_min_db, ceiling_gain_max_db);
  set(values, "ceiling", "ceiling.gain", estimate);
  set(values, "ceiling", "ceiling.ceiling", chain_ceiling);
  set(values, "ceiling", "ceiling.transients", style.transients);
  set(values, "ceiling", "ceiling.clip",
      style.clip_db + (goals.lufs > loud_master_lufs ? loud_clip_extra_db : 0.0));
  set(values, "ceiling", "ceiling.shape", style.shape);
  set(values, "ceiling", "ceiling.release", style.release_ms);
  set(values, "ceiling", "ceiling.character", style.character);
  set(values, "ceiling", "ceiling.transient_link", style.transient_link);
  set(values, "ceiling", "ceiling.target",
      std::clamp(goals.lufs, ceiling_target_min, ceiling_target_max));
  out.devices.push_back({"ceiling", "Ceiling", full_values("ceiling", values),
                         ceiling_reason(out, estimate, std::min(gain, 0.0))});
  return out;
}

// ---- Stores --------------------------------------------------------------------------

MemoryStore::MemoryStore(std::uint32_t sample_rate, std::vector<float> left,
                         std::vector<float> right)
    : sample_rate_(sample_rate), left_(std::move(left)), right_(std::move(right)) {
  right_.resize(left_.size());
}

Result<std::size_t> MemoryStore::read(std::uint64_t at, std::span<float> left,
                                      std::span<float> right) {
  if (at >= left_.size())
    return Result<std::size_t>::success(0);
  const auto n = static_cast<std::size_t>(
      std::min<std::uint64_t>(std::min(left.size(), right.size()), left_.size() - at));
  std::copy_n(left_.begin() + static_cast<std::ptrdiff_t>(at), n, left.begin());
  std::copy_n(right_.begin() + static_cast<std::ptrdiff_t>(at), n, right.begin());
  return Result<std::size_t>::success(n);
}

Result<void> MemoryStore::append(std::span<const float> left, std::span<const float> right) {
  if (left.size() != right.size())
    return Result<void>::failure({ErrorCode::invalid_argument, "Channels differ in length"});
  left_.insert(left_.end(), left.begin(), left.end());
  right_.insert(right_.end(), right.begin(), right.end());
  return Result<void>::success();
}

// ---- Running a chain -----------------------------------------------------------------

Result<std::vector<ChainDevice>> chain_from(std::span<const Device> devices) {
  using R = Result<std::vector<ChainDevice>>;
  std::vector<ChainDevice> chain;
  for (const auto& device : devices) {
    const auto* engine = effects::find_engine(device.engine);
    if (engine == nullptr)
      return R::failure({ErrorCode::not_found, "No built-in effect " + device.engine});
    ChainDevice link{engine, default_values(*engine->descriptor), std::nullopt};
    for (const auto& [id, value] : device.values) {
      const auto index = find_parameter(*engine->descriptor, id);
      if (!index)
        return R::failure({ErrorCode::not_found, "No parameter " + id + " on " + device.engine});
      link.values[*index] = sanitise(engine->descriptor->parameters[*index], value);
    }
    chain.push_back(std::move(link));
  }
  return R::success(std::move(chain));
}

Result<Analysis> run_chain(Source& source, std::span<const ChainDevice> chain,
                           const RunOptions& options) {
  using R = Result<Analysis>;
  const auto rate = source.sample_rate();
  std::vector<std::unique_ptr<effects::NativeEffectRuntime>> runtimes;
  std::uint64_t latency = 0;
  for (const auto& device : chain) {
    auto created =
        device.document
            ? effects::NativeEffectRuntime::create(*device.engine, device.values, *device.document,
                                                   static_cast<double>(rate))
            : effects::NativeEffectRuntime::create(*device.engine, device.values,
                                                   static_cast<double>(rate));
    if (!created)
      return R::failure(created.error());
    latency += created.value()->latency_frames();
    runtimes.push_back(std::move(created).value());
  }
  const std::uint64_t frames = source.frames();
  const std::uint64_t total = frames + latency;
  std::vector<float> left(run_block_frames);
  std::vector<float> right(run_block_frames);
  std::vector<engine::StereoFrame> block(run_block_frames);
  std::vector<float> out_left;
  std::vector<float> out_right;
  out_left.reserve(run_block_frames);
  out_right.reserve(run_block_frames);
  Analyser analyser(rate);
  if (options.capture != nullptr) {
    options.capture->left.clear();
    options.capture->right.clear();
  }
  const double beats_per_frame = run_bpm / seconds_per_minute / static_cast<double>(rate);
  std::uint64_t read_at = 0;
  for (std::uint64_t at = 0; at < total; at += run_block_frames) {
    if (options.cancelled && options.cancelled())
      return R::failure({ErrorCode::cancelled, "Cancelled"});
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(run_block_frames, total - at));
    std::size_t got = 0;
    if (read_at < frames) {
      auto read = source.read(read_at, std::span(left).first(n), std::span(right).first(n));
      if (!read)
        return R::failure(read.error());
      got = read.value();
      read_at += got;
    }
    block.resize(n);
    for (std::size_t i = 0; i < n; ++i)
      block[i] = i < got ? engine::StereoFrame{left[i], right[i]} : engine::StereoFrame{0.0F, 0.0F};
    const double beat = static_cast<double>(at) * beats_per_frame;
    const Transport transport{beat, beats_per_frame, beat / beats_per_bar, beats_per_bar};
    for (auto& runtime : runtimes) {
      if (runtime->process(block, {}, at, 1, transport, {}) !=
          effects::EffectProcessStatus::rendered)
        return R::failure({ErrorCode::internal, "A device refused a block"});
    }
    out_left.clear();
    out_right.clear();
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t k = at + i;
      if (k < latency || k - latency >= frames)
        continue;
      const std::uint64_t frame = k - latency;
      const auto l = static_cast<float>(block[i].left * options.output_gain);
      const auto r = static_cast<float>(block[i].right * options.output_gain);
      out_left.push_back(l);
      out_right.push_back(r);
      if (options.capture != nullptr && options.window && frame >= options.window->first &&
          frame < options.window->first + options.window->frames) {
        options.capture->left.push_back(l);
        options.capture->right.push_back(r);
      }
    }
    if (out_left.empty())
      continue;
    analyser.push(out_left, out_right);
    if (options.sink != nullptr) {
      auto appended = options.sink->append(out_left, out_right);
      if (!appended)
        return R::failure(appended.error());
    }
  }
  return R::success(std::move(analyser).finish());
}

Result<Learned> learn_gain(Source& source, std::vector<ChainDevice> chain, const GainSearch& search,
                           const StoreFactory& stores, std::optional<Window> window,
                           const Cancelled& cancelled) {
  using R = Result<Learned>;
  if (search.limiter.device >= chain.size())
    return R::failure({ErrorCode::invalid_argument, "No limiter in the chain"});
  const auto limiter = search.limiter.device;
  const auto& limiter_table = *chain[limiter].engine->descriptor;
  const double gain_max = limiter_table.parameters[search.limiter.parameter].maximum;
  double floor_x = 0.0;
  if (search.trim && search.trim->device < limiter) {
    const auto& trim_table = *chain[search.trim->device].engine->descriptor;
    floor_x = trim_table.parameters[search.trim->parameter].minimum;
  }
  const std::span<const ChainDevice> all(chain);
  const auto before = all.first(limiter);
  const auto after = all.subspan(limiter);

  // The devices before the limiter, at a trim, into a store.
  std::unique_ptr<Store> pre;
  std::optional<double> pre_trim;
  double pre_lufs = floor_db;
  const auto run_pre = [&](double trim) -> Result<void> {
    if (pre && pre_trim && *pre_trim == trim)
      return Result<void>::success();
    if (search.trim && search.trim->device < limiter)
      chain[search.trim->device].values[search.trim->parameter] = trim;
    auto made = stores(source.sample_rate());
    if (!made)
      return Result<void>::failure(made.error());
    pre = std::move(made).value();
    RunOptions options;
    options.sink = pre.get();
    options.cancelled = cancelled;
    auto ran = run_chain(source, before, options);
    if (!ran)
      return Result<void>::failure(ran.error());
    pre_lufs = ran.value().lufs();
    pre_trim = trim;
    return Result<void>::success();
  };

  const double output_db = dsp::gain_to_db(search.output_gain, floor_db);
  Learned best;
  double best_error = HUGE_VAL;
  std::size_t passes = 0;
  // One pass at x: the limiter's gain above 0, the trim below.
  const auto evaluate = [&](double x) -> Result<double> {
    const double gain = std::max(x, 0.0);
    const double trim = std::min(x, 0.0);
    auto ready = before.empty() ? Result<void>::success() : run_pre(trim);
    if (!ready)
      return Result<double>::failure(ready.error());
    chain[limiter].values[search.limiter.parameter] = gain;
    Source& input = before.empty() ? source : *pre;
    Capture capture;
    RunOptions options;
    options.output_gain = search.output_gain;
    options.window = window;
    options.capture = window ? &capture : nullptr;
    options.cancelled = cancelled;
    auto ran = run_chain(input, after, options);
    if (!ran)
      return Result<double>::failure(ran.error());
    ++passes;
    const double lufs = ran.value().lufs();
    if (std::fabs(lufs - search.target_lufs) < best_error) {
      best_error = std::fabs(lufs - search.target_lufs);
      best.gain_db = gain;
      best.trim_db = trim;
      best.limiter_loss_db = pre_lufs + gain + output_db - lufs;
      best.at_most = gain >= gain_max;
      best.output = std::move(ran).value();
      best.window = std::move(capture);
    }
    return Result<double>::success(lufs);
  };

  if (!before.empty()) {
    auto ready = run_pre(0.0);
    if (!ready)
      return R::failure(ready.error());
  } else {
    auto measured = run_chain(source, {}, {});
    if (!measured)
      return R::failure(measured.error());
    pre_lufs = measured.value().lufs();
  }
  double x = std::clamp(search.target_lufs - (pre_lufs + output_db), floor_x, gain_max);
  auto level = evaluate(x);
  if (!level)
    return R::failure(level.error());
  double previous_x = x;
  double previous_level = level.value();
  double error = search.target_lufs - previous_level;
  // The first step assumes a unit slope; later steps use the secant.
  double next = std::clamp(x + error, floor_x, gain_max);
  while (std::fabs(error) > learn_tolerance_lu && passes < learn_max_passes && next != x) {
    x = next;
    level = evaluate(x);
    if (!level)
      return R::failure(level.error());
    error = search.target_lufs - level.value();
    const double change = level.value() - previous_level;
    const double slope = std::fabs(change) > secant_min_lu ? change / (x - previous_x) : 1.0;
    previous_x = x;
    previous_level = level.value();
    next = std::clamp(x + error / (slope > 0.0 ? slope : 1.0), floor_x, gain_max);
  }
  best.passes = passes;
  return R::success(std::move(best));
}

void apply_learned(Proposal& proposal, const Learned& learned) {
  for (auto& device : proposal.devices) {
    if (device.engine == "ceiling") {
      for (auto& [id, value] : device.values)
        if (id == "ceiling.gain")
          value = learned.gain_db;
      device.reason = ceiling_reason(proposal, learned.gain_db, learned.trim_db);
    } else if (device.engine == "trim") {
      for (auto& [id, value] : device.values)
        if (id == "trim.gain")
          value = learned.trim_db;
    }
  }
}

namespace {

// The limiter's and the trim's knobs in a proposal's chain.
GainSearch search_for(const Proposal& proposal) {
  GainSearch search;
  search.target_lufs = proposal.goals.lufs;
  search.output_gain = dsp::db_to_gain(proposal.goals.after_chain_db);
  for (std::size_t i = 0; i < proposal.devices.size(); ++i) {
    const auto& device = proposal.devices[i];
    if (device.engine == "ceiling")
      search.limiter = {i, *find_parameter(table("ceiling"), "ceiling.gain")};
    if (device.engine == "trim")
      search.trim = Knob{i, *find_parameter(table("trim"), "trim.gain")};
  }
  return search;
}

Result<Learned> learn_for(Source& source, const Proposal& proposal, const StoreFactory& stores,
                          std::optional<Window> window, const Cancelled& cancelled) {
  auto chain = chain_from(proposal.devices);
  if (!chain)
    return Result<Learned>::failure(chain.error());
  return learn_gain(source, std::move(chain).value(), search_for(proposal), stores, window,
                    cancelled);
}

double* value_of(Proposal& proposal, std::string_view engine, std::string_view id) {
  for (auto& device : proposal.devices)
    if (device.engine == engine)
      for (auto& [name, value] : device.values)
        if (name == id)
          return &value;
  return nullptr;
}

} // namespace

Result<Planned> plan(Source& source, const Analysis& mix, const Reference& reference,
                     const Goals& goals, const StoreFactory& stores, const RolePowers& roles,
                     std::optional<Window> window, const Cancelled& cancelled) {
  using R = Result<Planned>;
  Planned out;
  out.proposal = derive(mix, reference, goals, roles);
  if (out.proposal.devices.empty())
    return R::failure({ErrorCode::invalid_argument, out.proposal.notes.front()});
  auto learned = learn_for(source, out.proposal, stores, window, cancelled);
  if (!learned)
    return R::failure(learned.error());
  // With the limiter's cost known, each candidate is learned; the one
  // whose output keeps the most crest at the target wins.
  const auto c = static_cast<std::size_t>(goals.character);
  const double loss = learned.value().limiter_loss_db;
  const auto variants = candidates(mix, goals, loss);
  const double alone_crest = learned.value().output.crest_db;
  // A variant on the target beats one off it; then the most crest wins.
  const auto reaches = [&](const Learned& l) {
    return std::fabs(l.output.lufs() - goals.lufs) <= loudness_tolerance_lu;
  };
  double best_crest = alone_crest;
  bool best_reaches = reaches(learned.value());
  double best_miss = std::fabs(learned.value().output.lufs() - goals.lufs);
  std::string kept = "the limiter alone";
  std::vector<std::string> weighed;
  for (std::size_t v = 1; v < variants.size(); ++v) {
    auto variant = derive(mix, reference, goals, roles, variants[v]);
    auto tried = learn_for(source, variant, stores, window, cancelled);
    if (!tried)
      return R::failure(tried.error());
    const double crest = tried.value().output.crest_db;
    const bool on_target = reaches(tried.value());
    const double miss = std::fabs(tried.value().output.lufs() - goals.lufs);
    const std::string name = variants[v].strike                        ? "Strike"
                             : variants[v].dynamics == Dynamics::press ? "Press"
                                                                       : "Strata";
    weighed.push_back(name + " " + fixed(crest, 1) + " dB");
    const bool glue = goals.character == Character::warm && variants[v].dynamics != Dynamics::none;
    const double margin = glue ? -dynamics_crest_margin_db : dynamics_crest_margin_db;
    const bool better = on_target != best_reaches ? on_target
                        : on_target               ? crest > best_crest + margin
                                                  : miss < best_miss;
    if (better) {
      best_crest = crest;
      best_reaches = on_target;
      best_miss = miss;
      kept = name;
      out.proposal = std::move(variant);
      learned = std::move(tried);
    }
  }
  if (!weighed.empty()) {
    std::string text;
    for (const auto& w : weighed)
      text += (text.empty() ? "" : ", ") + w;
    out.proposal.notes.push_back("The limiter alone costs " + fixed(loss, 1) +
                                 " LU at the target, more than " + std::string(character_names[c]) +
                                 "'s " + fixed(limiter_share_db[c], 1) +
                                 ": weighed by the crest each keeps (limiter alone " +
                                 fixed(alone_crest, 1) + " dB; " + text + "), kept " + kept + ".");
  } else {
    out.proposal.notes.push_back("No compressor: the limiter alone costs " + fixed(loss, 1) +
                                 " LU at the target, within " + std::string(character_names[c]) +
                                 "'s " + fixed(limiter_share_db[c], 1) + ".");
  }
  // Short at the top of the gain: more clip room, until Ceiling's range.
  for (std::size_t widen = 0; widen < clip_widen_steps; ++widen) {
    const double short_by = goals.lufs - learned.value().output.lufs();
    if (!learned.value().at_most || short_by <= learn_tolerance_lu)
      break;
    auto* clip = value_of(out.proposal, "ceiling", "ceiling.clip");
    const double wider = clip == nullptr ? 0.0 : std::min(*clip + short_by, clip_max_db);
    if (clip != nullptr && wider > *clip) {
      out.proposal.notes.push_back("Ceiling's clip room grows from " + fixed(*clip, 1) + " to " +
                                   fixed(wider, 1) + " dB: its gain tops out " +
                                   fixed(short_by, 1) + " LU short of the target.");
      *clip = wider;
      auto relearned = learn_for(source, out.proposal, stores, window, cancelled);
      if (!relearned)
        return R::failure(relearned.error());
      learned = std::move(relearned);
    } else {
      break;
    }
  }
  out.learned = std::move(learned).value();
  apply_learned(out.proposal, out.learned);
  return R::success(std::move(out));
}

} // namespace oscillate::native_dsp::assistant
