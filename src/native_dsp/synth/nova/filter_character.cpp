#include "synth/nova/filter_character.hpp"

#include "synth/nova/filter_impl.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace oscillate::native_dsp::synth::nova::filter_detail {
namespace {

// The vowels run A, E, I, O, U at positions 0 .. last_vowel; a position
// sits in the pair from its vowel to the next, the last pair from O.
constexpr double last_vowel = static_cast<double>(vowel_table.size() - 1);
constexpr std::size_t last_vowel_pair = vowel_table.size() - 2;
// The Vowel model's cutoff walks the vowels from vowel_lowest_hz over a
// vowel_span_ratio range (100 Hz to 10 kHz).
constexpr double vowel_lowest_hz = 100.0;
constexpr double vowel_span_ratio = 100.0;
// No band is centred below this.
constexpr double lowest_band_hz = 20.0;
// A formant's level: its sung level scaled by formant_level_compression,
// rising 3 dB an octave from unity at level_reference_hz (also where the
// energy normalisation's saw-like source has unit level).
constexpr double formant_level_compression = 0.4;
constexpr double level_reference_hz = 500.0;
// Formant: FAT walks the vowels; resonance narrows the bands from
// formant_width_max times their sung width by formant_width_ratio. Each
// band's damping stays within formant_k_min .. formant_k_max.
constexpr double formant_width_max = 4.0;
constexpr double formant_width_ratio = 0.15;
constexpr double formant_k_min = 0.005;
constexpr double formant_k_max = 2.0;
// The energy normalisation: a saw-like source's share in each band, over
// formant_energy_scale, made up by at most formant_max_makeup.
constexpr double formant_energy_scale = 700.0;
constexpr double formant_max_makeup = 10.0;
constexpr double energy_floor = 1e-12;
// Vowel: bells narrowed from vowel_width_max times their sung width by
// vowel_width_ratio with resonance (Q within vowel_q_min .. vowel_q_max),
// boosted from vowel_boost_min_db by up to vowel_boost_span_db, each
// formant's share falling over vowel_level_range_db of its sung level, and
// vowel_makeup_share of the boost taken back at the output.
constexpr double vowel_width_max = 2.5;
constexpr double vowel_width_ratio = 0.3;
constexpr double vowel_boost_min_db = 9.0;
constexpr double vowel_boost_span_db = 15.0;
constexpr double vowel_q_min = 0.7;
constexpr double vowel_q_max = 25.0;
constexpr double vowel_level_range_db = 45.0;
constexpr double vowel_makeup_share = 0.3;
// Phaser: feedback up to phaser_max_feedback; stages spread up to
// phaser_spread_octaves either side of the cutoff, none below phaser_lowest_hz.
constexpr double phaser_max_feedback = 0.9;
constexpr double phaser_spread_octaves = 2.0;
constexpr double phaser_lowest_hz = 10.0;
// Dual Peak: the peaks dual_peak_min_octaves apart at FAT 0, and up to
// dual_peak_span_octaves further at FAT 1.
constexpr double dual_peak_min_octaves = 0.5;
constexpr double dual_peak_span_octaves = 2.5;
// Ring: the carrier's squaring drive, and the level a full square loses.
constexpr double ring_square_drive = 7.0;
constexpr double ring_square_loss = 0.25;

// ---- Vowels ----------------------------------------------------------------------------

// position 0..4 runs A, E, I, O, U; between vowels the centres and
// bandwidths move in log frequency and the levels in dB.
[[nodiscard]] Vowel vowel_at(double position) noexcept {
  position = std::clamp(position, 0.0, last_vowel);
  const auto first = std::min<std::size_t>(last_vowel_pair, static_cast<std::size_t>(position));
  const double mix = position - static_cast<double>(first);
  const Vowel& from = vowel_table[first];
  const Vowel& to = vowel_table[first + 1];
  Vowel out{};
  for (std::size_t f = 0; f < formant_bands; ++f) {
    out[f] = {from[f].hz * std::pow(to[f].hz / from[f].hz, mix),
              from[f].bandwidth * std::pow(to[f].bandwidth / from[f].bandwidth, mix),
              from[f].db + mix * (to[f].db - from[f].db)};
  }
  return out;
}

// A formant's level: the sung levels compressed, plus +3 dB per octave, so
// a bright source talks instead of passing only the first formant.
[[nodiscard]] double formant_level(const Formant& formant) noexcept {
  return db_to_gain(formant_level_compression * formant.db) *
         std::sqrt(formant.hz / level_reference_hz);
}

} // namespace
} // namespace oscillate::native_dsp::synth::nova::filter_detail

namespace oscillate::native_dsp::synth::nova {

std::size_t VoiceFilter::Impl::vowel_pair(FilterModel model, const Params& p) noexcept {
  using namespace filter_detail;
  double position = 0.0;
  if (model == FilterModel::formant) {
    position = last_vowel * p.fat;
  } else if (model == FilterModel::vowel) {
    position = vowel_position(p.cutoff_hz);
  } else {
    return 0;
  }
  return std::min<std::size_t>(last_vowel_pair,
                               static_cast<std::size_t>(std::clamp(position, 0.0, last_vowel)));
}

double VoiceFilter::Impl::vowel_position(double cutoff_hz) noexcept {
  using namespace filter_detail;
  return last_vowel *
         std::clamp(std::log2(cutoff_hz / vowel_lowest_hz) / std::log2(vowel_span_ratio), 0.0, 1.0);
}

void VoiceFilter::Impl::derive_character(FilterModel model, const Params& p, double rate,
                                         Coefficients& c) noexcept {
  using namespace filter_detail;
  const double r = p.resonance;
  const double fat = p.fat;
  const double fc = p.cutoff_hz;
  switch (model) {
  case FilterModel::formant: {
    const Vowel vowel = vowel_at(last_vowel * fat);
    const double shift = fc / formant_reference_hz;
    const double width = formant_width_max * std::pow(formant_width_ratio, r);
    double energy = 0.0;
    for (std::size_t f = 0; f < formant_bands; ++f) {
      const double centre = std::clamp(vowel[f].hz * shift, lowest_band_hz, max_normalised * rate);
      const double bandwidth = vowel[f].bandwidth * width * shift;
      const double level = formant_level(vowel[f]);
      auto& band = c.band[f];
      band.g = warp(centre, rate);
      band.k = std::clamp(bandwidth / centre, formant_k_min, formant_k_max);
      band.gain = level * band.k;
      // What the band passes of a saw-like (-6 dB per octave) source.
      energy += level * level * bandwidth * (level_reference_hz / centre) *
                (level_reference_hz / centre) / formant_energy_scale;
    }
    c.output_gain =
        std::clamp(1.0 / std::sqrt(std::max(energy, energy_floor)), 1.0, formant_max_makeup);
    return;
  }
  case FilterModel::vowel: {
    // Cutoff walks A-E-I-O-U from 100 Hz to 10 kHz; FAT raises the voice
    // by up to an octave (a smaller singer).
    const double position = vowel_position(fc);
    const Vowel vowel = vowel_at(position);
    const double shift = std::exp2(fat);
    const double width = vowel_width_max * std::pow(vowel_width_ratio, r);
    const double boost_db = vowel_boost_min_db + vowel_boost_span_db * r;
    for (std::size_t f = 0; f < vowel_bands; ++f) {
      const double centre = std::clamp(vowel[f].hz * shift, lowest_band_hz, max_normalised * rate);
      const double q =
          std::clamp(vowel[f].hz / (vowel[f].bandwidth * width), vowel_q_min, vowel_q_max);
      const double db = boost_db * std::max(0.0, 1.0 + vowel[f].db / vowel_level_range_db);
      const double a = std::pow(10.0, db / bell_db_per_decade);
      auto& band = c.band[f];
      band.g = warp(centre, rate);
      band.k = 1.0 / (q * a);
      band.gain = band.k * (a * a - 1.0);
    }
    c.output_gain = db_to_gain(-vowel_makeup_share * boost_db);
    return;
  }
  case FilterModel::phaser:
    // Eight first-order all-passes, spread up to two octaves either side
    // of the cutoff by FAT, with resonance as feedback.
    c.k = phaser_max_feedback * r;
    for (std::size_t i = 0; i < phaser_stages; ++i) {
      const double place =
          2.0 * static_cast<double>(i) / static_cast<double>(phaser_stages - 1) - 1.0;
      auto& band = c.band[i];
      band.g = warp(std::max(phaser_lowest_hz, fc * std::exp2(phaser_spread_octaves * fat * place)),
                    rate);
      band.k = band.g / (1.0 + band.g);
    }
    return;
  case FilterModel::morph_lbh: {
    c.g = warp(fc, rate);
    c.k = butterworth_k1;
    c.k2 = 1.0 / resonance_q(r, butterworth_q2);
    const double t = std::clamp(fat, 0.0, 1.0);
    c.weight_low = t <= 0.5 ? 1.0 - 2.0 * t : 0.0;
    c.weight_band = t <= 0.5 ? 2.0 * t : 2.0 - 2.0 * t;
    c.weight_high = t <= 0.5 ? 0.0 : 2.0 * t - 1.0;
    return;
  }
  case FilterModel::dual_peak: {
    // Two resonant peaks either side of the cutoff, half an octave apart
    // at FAT 0 and three octaves at FAT 1; between them they cancel.
    const double spread =
        std::sqrt(std::exp2(dual_peak_min_octaves + dual_peak_span_octaves * fat));
    c.k = 1.0 / resonance_q(r, 1.0);
    c.band[0].g = warp(fc / spread, rate);
    c.band[1].g = warp(fc * spread, rate);
    return;
  }
  case FilterModel::ring:
    c.g = std::min(fc, max_normalised * rate) / rate;
    c.k = r;
    c.stage_mix = fat; // Carrier shape: sine towards a rounded square.
    c.stage_drive = ring_square_drive;
    c.output_gain = std::numbers::sqrt2 * (1.0 - ring_square_loss * fat);
    return;
  default:
    return;
  }
}

VoiceFilter::Impl::Complex VoiceFilter::Impl::response_character(FilterModel model,
                                                                 const Coefficients& c,
                                                                 const ResponsePoint& at) noexcept {
  using namespace filter_detail;
  switch (model) {
  case FilterModel::formant: {
    Complex sum = 0.0;
    for (std::size_t f = 0; f < formant_bands; ++f) {
      sum += c.band[f].gain * at.svf_at(c.band[f].g, c.band[f].k).band;
    }
    return sum * c.output_gain;
  }
  case FilterModel::vowel: {
    Complex product = c.output_gain;
    for (std::size_t f = 0; f < vowel_bands; ++f) {
      product *= 1.0 + c.band[f].gain * at.svf_at(c.band[f].g, c.band[f].k).band;
    }
    return product;
  }
  case FilterModel::phaser: {
    Complex all = 1.0;
    for (std::size_t i = 0; i < phaser_stages; ++i) {
      const Complex s = at.s_of(c.band[i].g);
      all *= (1.0 - s) / (1.0 + s);
    }
    return 0.5 * (1.0 + all / (1.0 - c.k * all));
  }
  case FilterModel::morph_lbh: {
    const auto mix = [&](const ResponsePoint::Sections& o, double k) {
      return c.weight_low * o.low + c.weight_band * k * o.band + c.weight_high * o.high;
    };
    return mix(at.svf_at(c.g, c.k), c.k) * mix(at.svf_at(c.g, c.k2), c.k2);
  }
  case FilterModel::dual_peak:
    return c.k * (at.svf_at(c.band[0].g, c.k).band + at.svf_at(c.band[1].g, c.k).band);
  case FilterModel::ring:
    // The unshifted part; the rest moves to the sidebands at f +/- cutoff.
    return 1.0 - c.k;
  default:
    return 1.0;
  }
}

} // namespace oscillate::native_dsp::synth::nova
