#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/knock_striker.hpp"
#include "oscillate/native_dsp/synth/modal_dsp.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Knock's engine contract (ADR-0068): physically struck pads that are
// deterministic under any block split and reset, sound like a thing at Tone
// 0 and a note at Tone 1, bend both ways, gate, lift, crush and couple as
// designed, strike for as long as their hardness says, brighten with
// velocity, shorten with the global Decay, sit in a sane level window and
// ring at the modes the panel draws. Width spreads each pad's modes with the
// lows kept centred, moves its hits (Ping-pong, Scatter, Orbit) and delays
// the far ear (Haas) without changing its calibrated level; Swing plays
// off-beat sixteenths late on the kernel's own clock, whatever the blocks.
// A pad following the key tunes to the nearest note of the project's key.
namespace {
using namespace synth_test;
namespace knock = oscillate::native_dsp::synth::knock;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double pi = 3.14159265358979323846;

const SynthEngine& knock_engine() { return knock::engine(); }
const EngineDescriptor& table() { return *knock_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::size_t pad_at(std::size_t pad, std::string_view control) {
  return at("knock.pad" + std::to_string(pad + 1) + "." + std::string(control));
}

std::uint8_t key(std::size_t pad) { return static_cast<std::uint8_t>(knock::first_key + pad); }

// Pad 1 alone, dry and plain: a stick on wood, no click, no extras' grit
// beyond the Tone's, no coupling, body, finish, hand, room, width or swing.
std::vector<double> plain(std::string_view material = "Wood", double pitch = 400.0,
                          double tone = 1.0, double decay = 0.2) {
  auto v = preset(table(), "Minimal");
  const auto& materials = table().parameters[pad_at(0, "res.material")].choices;
  const auto found = std::ranges::find(materials, material);
  require(found != materials.end(), "material exists: " + std::string(material));
  const auto material_index = static_cast<double>(std::distance(materials.begin(), found));
  v[pad_at(0, "striker.type")] = 1.0; // Stick.
  v[pad_at(0, "striker.hardness")] = 0.5;
  v[pad_at(0, "striker.position")] = 0.3;
  v[pad_at(0, "striker.click")] = 0.0;
  v[pad_at(0, "res.material")] = material_index;
  v[pad_at(0, "res.pitch")] = pitch;
  v[pad_at(0, "res.tone")] = tone;
  v[pad_at(0, "res.decay")] = decay;
  v[pad_at(0, "res.damping")] = 0.5;
  v[pad_at(0, "res.bend")] = 0.0;
  v[pad_at(0, "couple.amount")] = 0.0;
  v[pad_at(0, "body.hollow")] = 0.0;
  v[pad_at(0, "finish.snap")] = 0.0;
  v[pad_at(0, "finish.length")] = 2.0;
  v[pad_at(0, "finish.lift")] = 20.0;
  v[pad_at(0, "finish.crush")] = 0.0;
  v[pad_at(0, "hand")] = 0.0;
  v[pad_at(0, "level")] = 0.0;
  v[pad_at(0, "pan")] = 0.0;
  v[pad_at(0, "room")] = 0.0;
  v[pad_at(0, "width.spread")] = 0.0;
  v[pad_at(0, "width.motion")] = 0.0; // Fixed.
  v[pad_at(0, "width.depth")] = 0.0;
  v[pad_at(0, "width.haas")] = 0.0;
  v[pad_at(0, "swing.follow")] = 1.0; // On.
  v[pad_at(0, "key.follow")] = 0.0;   // Off.
  v[at("knock.room.level")] = -60.0;
  v[at("knock.out.level")] = 0.0;
  v[at("knock.decay")] = 100.0;
  v[at("knock.velocity")] = 1.0;
  v[at("knock.swing")] = 0.0;
  return v;
}

std::vector<double> mono(const Rendered& r) {
  std::vector<double> out(r.left.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = 0.5 * (r.left[i] + r.right[i]);
  }
  return out;
}

std::vector<double> hit(const std::vector<double>& values, std::size_t frames,
                        std::uint8_t velocity = knock::reference_velocity, std::size_t pad = 0) {
  auto runtime = make_runtime(knock_engine(), values);
  return mono(render(*runtime, note(0, 100, key(pad), 1, velocity), frames));
}

double magnitude(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < length && from + i < x.size(); ++i) {
    const double window =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(length - 1));
    const double t = 2.0 * pi * hz * static_cast<double>(i) / rate;
    re += x[from + i] * window * std::cos(t);
    im += x[from + i] * window * std::sin(t);
  }
  return std::sqrt(re * re + im * im);
}

// The power spectrum of a Hann window of 4096 frames.
std::vector<double> power_spectrum(const std::vector<double>& x, std::size_t from) {
  constexpr std::size_t n = 4'096;
  std::vector<std::complex<double>> data(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double hann =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(n));
    data[i] = from + i < x.size() ? x[from + i] * hann : 0.0;
  }
  fft(data);
  std::vector<double> power(n / 2);
  for (std::size_t bin = 0; bin < n / 2; ++bin) {
    power[bin] = std::norm(data[bin]) + 1e-30;
  }
  return power;
}

double bin_hz(std::size_t bin) { return static_cast<double>(bin) * rate / 4'096.0; }

// Geometric over arithmetic mean of the power from 200 Hz to 16 kHz.
double flatness(const std::vector<double>& x, std::size_t from) {
  const auto power = power_spectrum(x, from);
  double logs = 0.0;
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t bin = 0; bin < power.size(); ++bin) {
    if (bin_hz(bin) < 200.0 || bin_hz(bin) > 16'000.0) {
      continue;
    }
    logs += std::log(power[bin]);
    sum += power[bin];
    ++count;
  }
  return std::exp(logs / static_cast<double>(count)) / (sum / static_cast<double>(count));
}

double centroid(const std::vector<double>& x, std::size_t from) {
  const auto power = power_spectrum(x, from);
  double weighted = 0.0;
  double total = 0.0;
  for (std::size_t bin = 1; bin < power.size(); ++bin) {
    const double m = std::sqrt(power[bin]);
    weighted += m * bin_hz(bin);
    total += m;
  }
  return weighted / total;
}

// The strongest frequency between lo and hi Hz in a window.
// strongest_hz searches in steps of this many hertz.
constexpr double strongest_hz_step = 2.0;

double strongest_hz(const std::vector<double>& x, std::size_t from, std::size_t length, double lo,
                    double hi) {
  double best = lo;
  double best_magnitude = -1.0;
  const auto points = static_cast<std::size_t>(std::floor((hi - lo) / strongest_hz_step));
  for (std::size_t point = 0; point <= points; ++point) {
    const double hz = lo + static_cast<double>(point) * strongest_hz_step;
    const double m = magnitude(x, from, length, hz);
    if (m > best_magnitude) {
      best_magnitude = m;
      best = hz;
    }
  }
  return best;
}

double db(double x) { return oscillate::test::gain_to_db(x, oscillate::test::silence_floor); }

std::size_t frames_of(double seconds) { return static_cast<std::size_t>(seconds * rate); }

void descriptor_registry_and_presets() {
  REQUIRE_EQ(find_engine("knock"), &knock_engine(), "Knock is registered");
  REQUIRE_NE(std::ranges::find(engines(), &knock_engine()), engines().end(), "Knock is registered");
  REQUIRE_OK(validate_engine(table()), "the table validates");
  REQUIRE_EQ(table().parameters.size(), knock::pad_count * 29 + 7, "29 controls a pad, 7 globals");
  REQUIRE_EQ(table().presets.size(), 12, "twelve kits, Minimal first and the defaults");
  REQUIRE_EQ(table().presets.front().name, "Minimal",
             "twelve kits, Minimal first and the defaults");
  REQUIRE_TRUE(table().presets.front().values.empty(),
               "twelve kits, Minimal first and the defaults");
  for (const auto& entry : table().presets) {
    REQUIRE_OK(preset_values(table(), entry), std::string(entry.name) + " resolves");
    require(!entry.tags.empty(), std::string(entry.name) + " is tagged");
    for (const auto tag : entry.tags) {
      require(std::ranges::none_of(tag, [](char c) { return c >= 'A' && c <= 'Z'; }),
              "tags are lower case");
    }
  }
  for (const auto& parameter : table().parameters) {
    REQUIRE_EQ(parameter.timing, ParameterTiming::live, std::string(parameter.id) + " is live");
    REQUIRE_EQ(parameter.automatable, !is_discrete(parameter),
               std::string(parameter.id) + ": continuous controls automate, choices do not");
  }
  // The ids the panel binds to.
  for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
    for (const auto* control : {"striker.type",
                                "striker.hardness",
                                "striker.position",
                                "striker.click",
                                "res.material",
                                "res.pitch",
                                "res.tone",
                                "res.decay",
                                "res.damping",
                                "res.bend",
                                "couple.material",
                                "couple.tuning",
                                "couple.amount",
                                "body.size",
                                "body.hollow",
                                "finish.snap",
                                "finish.length",
                                "finish.lift",
                                "finish.crush",
                                "hand",
                                "level",
                                "pan",
                                "room",
                                "width.spread",
                                "width.motion",
                                "width.depth",
                                "width.haas",
                                "swing.follow",
                                "key.follow"}) {
      (void)pad_at(pad, control);
    }
  }
  for (const auto* id : {"knock.decay", "knock.room.size", "knock.room.level", "knock.velocity",
                         "knock.out.level", "knock.swing", "knock.orbit"}) {
    (void)at(id);
  }
  require(knock_engine().models_pedals, "percussion ignores the pedals");
}

std::vector<Event> pattern() {
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (std::uint64_t step = 0; step < 16; ++step) {
    for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
      if ((step + pad) % 3 == 0) {
        const auto n = note(step * 3'001 + pad * 17, 50, key(pad), id++,
                            static_cast<std::uint8_t>(60 + (step * 7 + pad * 11) % 67));
        events.insert(events.end(), n.begin(), n.end());
      }
    }
  }
  return events;
}

void any_block_split_renders_identically() {
  for (const auto* name : {"Minimal", "Dub Techno", "Glitch Crush", "Afro Percussion"}) {
    const auto values = preset(table(), name);
    auto a = make_runtime(knock_engine(), values);
    auto b = make_runtime(knock_engine(), values);
    auto c = make_runtime(knock_engine(), values);
    const auto x = render(*a, pattern(), 72'000, 128);
    const auto y = render(*b, pattern(), 72'000, 0);
    const auto z = render(*c, pattern(), 72'000, 1'024);
    require(x.left == y.left && x.right == y.right && x.left == z.left && x.right == z.right,
            std::string(name) + ": block splits render identically");
  }
}

// A reset (seek or loop) restarts the hand: the same hits play the same.
void reset_is_deterministic() {
  auto values = preset(table(), "Minimal");
  values[at("knock.room.level")] = -60.0;
  auto runtime = make_runtime(knock_engine(), values);
  const auto pass = [&](std::uint64_t generation) {
    std::vector<double> out;
    std::vector<engine::StereoFrame> buffer(128);
    for (std::uint64_t block = 0; block < 375; ++block) {
      std::vector<engine::BlockMidiEvent> events;
      if (block % 25 == 0) {
        for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
          engine::BlockMidiEvent event;
          event.offset = static_cast<std::uint32_t>(pad * 9);
          event.clip_id_bits = {1, 1};
          event.note_id_bits = {0, block * 8 + pad + 1};
          event.kind = engine::MidiEventKind::note_on;
          event.key = key(pad);
          event.velocity = 100;
          events.push_back(event);
        }
      }
      std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
      const auto report = runtime->process(buffer, events, block * 128, generation, {}, {});
      REQUIRE_EQ(report.status, SynthProcessStatus::rendered, "block renders");
      for (const auto& frame : buffer) {
        out.push_back(frame.left);
        out.push_back(frame.right);
      }
    }
    // Let every gate close before the next pass.
    std::vector<engine::BlockMidiEvent> none;
    for (std::uint64_t block = 375; block < 750; ++block) {
      std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
      (void)runtime->process(buffer, none, block * 128, generation, {}, {});
    }
    return out;
  };
  const auto first = pass(1);
  const auto second = pass(2);
  REQUIRE_EQ(first, second, "the same hits after a reset play the same samples");
  // Interleaved stereo frames: one hit period apart.
  constexpr std::size_t hit_period_samples = std::size_t{2} * 3'200;
  bool varied = false;
  for (std::size_t i = 0; i + hit_period_samples < first.size() && !varied; ++i) {
    varied = first[i] != first[i + hit_period_samples];
  }
  require(varied, "the hand varies repeated hits");
}

void every_preset_is_finite_and_bounded() {
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(knock_engine(), preset(table(), entry.name));
    std::vector<Event> events;
    std::uint64_t id = 1;
    for (std::uint64_t i = 0; i < 24; ++i) {
      const auto n = note(i * 1'201, 10, key(i % knock::pad_count), id++, 127);
      events.insert(events.end(), n.begin(), n.end());
    }
    const auto r = render(*runtime, events, 48'000);
    bool finite = true;
    for (std::size_t i = 0; i < r.left.size(); ++i) {
      finite = finite && std::isfinite(r.left[i]) && std::isfinite(r.right[i]);
    }
    require(finite, std::string(entry.name) + " renders finite audio");
    require(peak(r.left) <= 1.0 && peak(r.right) <= 1.0 && peak(r.left) > 1e-3,
            std::string(entry.name) + " is audible and under full scale");
  }
}

void tone_zero_is_a_thing_tone_one_a_note() {
  const auto thing = hit(plain("Wood", 400.0, 0.0), 8'192);
  const auto note_like = hit(plain("Wood", 400.0, 1.0), 8'192);
  const double a = flatness(thing, 48);
  const double b = flatness(note_like, 48);
  REQUIRE_GT(a, 2.0 * b, "Tone 0 is spectrally flatter than Tone 1");
}

void bend_falls_and_rises() {
  const auto early_late = [](double bend) {
    auto v = plain("Tube", 400.0, 1.0, 0.4);
    v[pad_at(0, "res.bend")] = bend;
    const auto x = hit(v, frames_of(0.4));
    return std::pair{strongest_hz(x, frames_of(0.004), frames_of(0.02), 100.0, 1'000.0),
                     strongest_hz(x, frames_of(0.25), frames_of(0.1), 100.0, 1'000.0)};
  };
  const auto [flat_early, flat_late] = early_late(0.0);
  REQUIRE_LT(std::fabs(flat_early - 400.0), 40.0, "no bend holds the pitch");
  REQUIRE_LT(std::fabs(flat_late - 400.0), 6.0, "no bend holds the pitch");
  const auto [fall_early, fall_late] = early_late(0.5);
  REQUIRE_GT(fall_early, 1.2 * fall_late, "a positive bend falls to the pitch");
  REQUIRE_LT(std::fabs(fall_late - 400.0), 10.0, "a positive bend falls to the pitch");
  const auto [rise_early, rise_late] = early_late(-0.5);
  REQUIRE_LT(rise_early, 0.8 * rise_late, "a negative bend rises to the pitch: a plop");
  REQUIRE_LT(std::fabs(rise_late - 400.0), 10.0, "a negative bend rises to the pitch: a plop");
}

void the_gate_ends_the_hit() {
  for (const double decay : {100.0, 50.0}) {
    auto v = plain("Metal", 900.0, 0.5, 1.0);
    v[pad_at(0, "finish.length")] = 0.09;
    v[at("knock.decay")] = decay;
    const auto x = hit(v, frames_of(0.5));
    const double gate = 0.09 * std::min(1.0, decay / 100.0);
    const auto end = static_cast<std::size_t>(std::ceil((gate + 0.003) * rate)) + 1;
    REQUIRE_GT(peak(x, 0, frames_of(gate * 0.9)), 1e-2, "it sounds before the gate");
    REQUIRE_EQ(peak(x, end), 0.0, "silent after length + 3 ms");
  }
}

void lift_attenuates_below_its_cutoff() {
  const auto ratio = [](double lift) {
    auto v = plain("Wood", 200.0, 1.0, 0.3);
    v[pad_at(0, "striker.hardness")] = 1.0;
    v[pad_at(0, "finish.lift")] = lift;
    const auto x = hit(v, frames_of(0.1));
    // The fundamental against the fifth mode (2.67 kHz).
    return magnitude(x, 0, frames_of(0.05), 200.0) /
           magnitude(x, 0, frames_of(0.05), 200.0 * 13.344);
  };
  REQUIRE_LT(db(ratio(2'000.0)), db(ratio(20.0)) - 25.0, "lift takes the lows away");
}

void crush_quantises() {
  const auto render_with = [](double crush) {
    auto v = plain("Metal", 700.0, 0.5, 0.3);
    v[pad_at(0, "finish.crush")] = crush;
    return hit(v, frames_of(0.1));
  };
  const auto clean = render_with(0.0);
  const auto crushed = render_with(0.6);
  double error = 0.0;
  double power = 0.0;
  std::size_t held = 0;
  std::size_t held_clean = 0;
  for (std::size_t i = 1; i < frames_of(0.05); ++i) {
    error += (crushed[i] - clean[i]) * (crushed[i] - clean[i]);
    power += clean[i] * clean[i];
    held += crushed[i] == crushed[i - 1] ? 1U : 0U;
    held_clean += clean[i] == clean[i - 1] ? 1U : 0U;
  }
  REQUIRE_GT(error, 0.01 * power, "crush adds quantisation error");
  REQUIRE_GT(held, frames_of(0.05) / 2, "crush holds samples");
  REQUIRE_LT(held_clean, frames_of(0.05) / 20, "crush holds samples");
}

void coupling_rings_the_coupled_modes() {
  const auto share = [](double amount) {
    auto v = plain("Wood", 300.0, 1.0, 0.15);
    v[pad_at(0, "couple.material")] = 2.0; // Metal.
    v[pad_at(0, "couple.tuning")] = 1.3;
    v[pad_at(0, "couple.amount")] = amount;
    auto designed = v;
    designed[pad_at(0, "couple.amount")] = 1.0;
    const auto modes = knock::design_pad(designed, 0, rate);
    const auto x = hit(v, frames_of(0.4));
    // Coupled modes clear of every resonator mode, so the resonator's own
    // ring does not count.
    const auto clear = [&](double hz) {
      return std::ranges::none_of(modes, [&](const knock::ModeView& other) {
        return other.part == knock::ModePart::resonator && std::fabs(other.hz - hz) < 40.0;
      });
    };
    double coupled = 0.0;
    for (const auto& mode : modes) {
      if (mode.part == knock::ModePart::coupled && mode.hz < 4'000.0 && clear(mode.hz)) {
        coupled += magnitude(x, frames_of(0.05), frames_of(0.2), mode.hz);
      }
    }
    return coupled / magnitude(x, frames_of(0.05), frames_of(0.2), 300.0);
  };
  REQUIRE_GT(share(1.0), 2.0 * share(0.0), "coupling rings the coupled resonator");
}

// The kernel's striker on a resonator as the kernel builds it: contact time
// follows hardness within 25% at velocity 100.
void contact_time_follows_hardness() {
  const double velocity = knock::reference_velocity_unit;
  for (std::size_t t = 0; t < knock::striker_type_count; ++t) {
    const auto type = static_cast<knock::StrikerType>(t);
    for (const double hardness : {0.0, 0.5, 1.0}) {
      const double h = knock::velocity_hardness(hardness, velocity, 1.0);
      auto striker = knock::make_striker(type, h);
      modal::ModalBank bank;
      bank.prepare(8, rate);
      constexpr std::array<double, 6> wood{1, 2.756, 5.404, 8.933, 13.344, 18.64};
      double power = 0.0;
      std::array<double, 6> shapes{};
      for (std::size_t k = 0; k < wood.size(); ++k) {
        shapes[k] = 0.25 + 0.75 * std::fabs(std::sin(static_cast<double>(k + 1) * pi * 0.3 *
                                                     (1.0 + 0.13 * static_cast<double>(k % 3))));
        power += shapes[k] * shapes[k];
      }
      const double mass = knock::surface_mass(type, power);
      for (std::size_t k = 0; k < wood.size(); ++k) {
        require(bank.add({400.0 * wood[k], 0.5, shapes[k], mass, 1.0, 0.0}), "mode added");
      }
      striker.launch(0.0, knock::strike_speed(velocity, 1.0));
      for (int frame = 0; frame < 4'800 && striker.active; ++frame) {
        (void)bank.step_contact(striker, 0.0, 0.0);
      }
      const double target = knock::contact_target_seconds(type, h);
      REQUIRE_LT(std::fabs(striker.contact_seconds / target - 1.0), 0.25,
                 "contact time follows hardness");
    }
  }
  REQUIRE_EQ(knock::contact_target_seconds(knock::StrikerType::stick, 0.0), 0.0065,
             "6.5 ms soft to 0.23 ms hard; a finger stays longer");
  REQUIRE_LT(std::fabs(knock::contact_target_seconds(knock::StrikerType::stick, 1.0) - 0.0002275),
             tolerance::double_accumulation, "6.5 ms soft to 0.23 ms hard; a finger stays longer");
  REQUIRE_GT(knock::contact_target_seconds(knock::StrikerType::finger, 0.0), 0.011,
             "6.5 ms soft to 0.23 ms hard; a finger stays longer");
}

// A harder hit's shorter contact reaches higher modes: a wooden bar's
// second mode rises against its first. Measured with Tone at note, where
// the bar's own modes stand clear, rather than by spectral centroid, which
// the grit's noise floor swamps.
void velocity_brightens() {
  auto v = plain("Wood", 300.0, 1.0, 0.2);
  constexpr double second = 300.0 * 2.756;
  const auto brightness = [&](std::uint8_t velocity) {
    const auto x = hit(v, 4'096, velocity);
    return 20.0 * std::log10(magnitude(x, 0, 2'048, second) / magnitude(x, 0, 2'048, 300.0));
  };
  REQUIRE_GT(brightness(127), brightness(40) + 3.0, "a harder hit is brighter");
  REQUIRE_GT(peak(hit(v, 4'096, 127)), peak(hit(v, 4'096, 40)), "and louder");
}

// The time to fall 60 dB from the peak.
double fall_seconds(const std::vector<double>& x) {
  const double top = peak(x);
  std::size_t last = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (std::fabs(x[i]) > top * 1e-3) {
      last = i;
    }
  }
  return static_cast<double>(last) / rate;
}

void global_decay_shortens_every_ring() {
  auto v = plain("Tube", 220.0, 1.0, 0.2);
  const double full = fall_seconds(hit(v, frames_of(2.0)));
  v[at("knock.decay")] = 30.0;
  const double short_ring = fall_seconds(hit(v, frames_of(2.0)));
  const double ratio = short_ring / full;
  REQUIRE_GT(ratio, 0.22, "Decay at 30% shortens the ring about proportionally");
  REQUIRE_LT(ratio, 0.4, "Decay at 30% shortens the ring about proportionally");
}

// The loudest short-term (2 ms) RMS of a stretch of output, in dB.
double short_term_db(const Rendered& r, std::size_t from, std::size_t to) {
  const double smoothing = std::exp(-1.0 / (0.002 * rate));
  double power = 0.0;
  double loudest = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    const double x = 0.5 * (r.left[i] + r.right[i]);
    power = smoothing * power + (1.0 - smoothing) * x * x;
    loudest = std::max(loudest, power);
  }
  return 10.0 * std::log10(std::max(loudest, 1e-24));
}

// Every pad of every kit at 0 dB, centred and velocity 100, without Lift
// or Length, is as loud as every other: the median short-term level of a
// few hits (Hand varies each one) sits within 3 dB of the calibration
// target unless a click's peak holds it lower, and no hit clips.
void preset_levels_sit_in_a_window() {
  constexpr double target_db = -13.98; // knock.cpp's target_level, 0.2...
  constexpr double ceiling_db = -6.02; // ...or its peak_ceiling, 0.5.
  constexpr std::size_t hits = 7;
  const std::size_t gap = frames_of(0.4);
  for (const auto& entry : table().presets) {
    for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
      auto v = preset(table(), entry.name);
      v[pad_at(pad, "level")] = 0.0;
      v[pad_at(pad, "pan")] = 0.0;
      // Calibration measures the source; Lift and Length act on it after.
      v[pad_at(pad, "finish.lift")] = 20.0;
      v[pad_at(pad, "finish.length")] = 2.0;
      // Width moves the image, not the level (calibration_ignores_width);
      // a mid mix of a spread or Haas-delayed hit is not its level.
      v[pad_at(pad, "width.spread")] = 0.0;
      v[pad_at(pad, "width.motion")] = 0.0;
      v[pad_at(pad, "width.haas")] = 0.0;
      v[at("knock.room.level")] = -60.0;
      v[at("knock.out.level")] = 0.0;
      auto runtime = make_runtime(knock_engine(), v);
      std::vector<Event> events;
      for (std::size_t h = 0; h < hits; ++h) {
        auto n = note(h * gap, 100, key(pad), h + 1);
        events.insert(events.end(), n.begin(), n.end());
      }
      const auto r = render(*runtime, events, hits * gap);
      std::vector<double> levels;
      for (std::size_t h = 0; h < hits; ++h) {
        levels.push_back(short_term_db(r, h * gap, (h + 1) * gap));
      }
      std::ranges::sort(levels);
      const double median = levels[hits / 2];
      const double top = db(std::max(peak(r.left), peak(r.right)));
      const auto name = std::string(entry.name) + " pad " + std::to_string(pad + 1);
      if (std::getenv("KNOCK_LEVELS") != nullptr) {
        std::printf("%-22s median %6.2f dB  peak %6.2f dBFS\n", name.c_str(), median, top);
        continue;
      }
      // A click is held at the peak ceiling instead, so it may sit lower.
      require(median < target_db + 3.0 && (median > target_db - 3.0 || top > ceiling_db - 4.0),
              name + " sits at the kit level");
      REQUIRE_LT(top, -1.0, name + " never clips");
    }
  }
}

void design_pad_is_what_the_kernel_plays() {
  auto v = plain("Glass", 500.0, 1.0, 0.4);
  const auto modes = knock::design_pad(v, 0, rate);
  constexpr std::array<double, 6> glass{1, 2.32, 4.25, 6.63, 9.38, 12.6};
  REQUIRE_EQ(modes.size(), glass.size(), "Tone 1 plays the material's own modes");
  double strongest = 0.0;
  for (std::size_t k = 0; k < glass.size(); ++k) {
    REQUIRE_LT(std::fabs(modes[k].hz - 500.0 * glass[k]), tolerance::double_accumulation,
               "at the material's ratios");
    REQUIRE_EQ(modes[k].part, knock::ModePart::resonator, "at the material's ratios");
    strongest = std::max(strongest, modes[k].level);
  }
  REQUIRE_EQ(strongest, 1.0, "levels are relative to the strongest");
  const auto x = hit(v, frames_of(0.2));
  for (std::size_t k = 0; k < 4; ++k) {
    const double hz = modes[k].hz;
    const double between = std::sqrt(hz * (k + 1 < glass.size() ? modes[k + 1].hz : 2.0 * hz));
    REQUIRE_GT(magnitude(x, frames_of(0.01), frames_of(0.1), hz),
               4.0 * magnitude(x, frames_of(0.01), frames_of(0.1), between),
               "the kernel rings at the designed modes");
  }
  // Tone 0 adds the scattered cloud; coupling and a body add their parts.
  v[pad_at(0, "res.tone")] = 0.0;
  v[pad_at(0, "couple.amount")] = 0.5;
  v[pad_at(0, "body.hollow")] = 0.5;
  const auto cloud = knock::design_pad(v, 0, rate);
  std::size_t res = 0;
  std::size_t coupled = 0;
  std::size_t body = 0;
  for (const auto& mode : cloud) {
    res += mode.part == knock::ModePart::resonator ? 1U : 0U;
    coupled += mode.part == knock::ModePart::coupled ? 1U : 0U;
    body += mode.part == knock::ModePart::body ? 1U : 0U;
    REQUIRE_LT(mode.hz, 0.45 * rate, "every mode is in range");
    REQUIRE_GT(mode.t60, 0.0, "every mode is in range");
    REQUIRE_GE(mode.level, 0.0, "every mode is in range");
    REQUIRE_LE(mode.level, 1.0, "every mode is in range");
  }
  REQUIRE_GT(res, glass.size() + 20, "the cloud, coupled and body");
  REQUIRE_GT(coupled, 20, "the cloud, coupled and body");
  REQUIRE_EQ(body, 6, "the cloud, coupled and body");
  REQUIRE_TRUE(knock::design_pad(v, knock::pad_count, rate).empty(),
               "a bad pad or short values design nothing");
  REQUIRE_TRUE(knock::design_pad(std::span<const double>(v).subspan(0, 10), 0, rate).empty(),
               "a bad pad or short values design nothing");
}

void telemetry_reports_hits() {
  auto runtime = make_runtime(knock_engine(), preset(table(), "Minimal"));
  std::vector<float> frame;
  (void)render(*runtime, {}, 128);
  REQUIRE_TRUE(runtime->read_telemetry(frame), "no hit yet");
  REQUIRE_EQ(frame.size(), knock::telemetry::size, "no hit yet");
  REQUIRE_EQ(frame[knock::telemetry::last_pad], -1.0F, "no hit yet");
  REQUIRE_EQ(frame[knock::telemetry::hits], 0.0F, "no hit yet");
  auto events = note(0, 10, key(1), 1);
  const auto second = note(500, 10, key(5), 2);
  events.insert(events.end(), second.begin(), second.end());
  (void)render(*runtime, events, 2'400);
  require(runtime->read_telemetry(frame), "telemetry is published");
  REQUIRE_EQ(frame[knock::telemetry::last_pad], 5.0F, "the latest pad and the hit count");
  REQUIRE_EQ(frame[knock::telemetry::hits], 2.0F, "the latest pad and the hit count");
  REQUIRE_GT(frame[knock::telemetry::first_level + 5], 0.01F, "levels per pad and the output peak");
  REQUIRE_EQ(frame[knock::telemetry::first_level + 0], 0.0F, "levels per pad and the output peak");
  REQUIRE_GT(frame[knock::telemetry::output_peak_db], -30.0F, "levels per pad and the output peak");
}

void full_load_does_not_allocate() {
  auto runtime = make_runtime(knock_engine(), preset(table(), "Dub Techno"));
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> events(knock::pad_count);
  for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
    events[pad].clip_id_bits = {1, 1};
    events[pad].offset = static_cast<std::uint32_t>(pad * 5);
    events[pad].kind = engine::MidiEventKind::note_on;
    events[pad].key = key(pad);
    events[pad].velocity = 120;
  }
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t b = 0; b < 400; ++b) {
    for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
      events[pad].note_id_bits = {0, b * 8 + pad + 1};
    }
    const auto hits = b % 20 == 0 ? std::span(events) : std::span(events).subspan(0, 0);
    (void)runtime->process(buffer, hits, b * 128, b < 300 ? 1 : 2, {}, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void tail_bound_covers_every_kit() {
  for (const auto& entry : table().presets) {
    const auto values = preset(table(), entry.name);
    auto runtime = make_runtime(knock_engine(), values);
    std::vector<Event> events;
    for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
      const auto n = note(0, 10, key(pad), pad + 1, 127);
      events.insert(events.end(), n.begin(), n.end());
    }
    const auto tail = knock_engine().tail_frames(values, rate);
    const auto r = render(*runtime, events, tail + 4'800);
    require(peak(r.left, tail) < 1e-4 && peak(r.right, tail) < 1e-4,
            std::string(entry.name) + ": silent after the tail bound");
  }
}

// Golden fingerprints are not pinned yet. KNOCK_REBASELINE=1 prints each
// kit's fingerprint (hold level, tail level, centroid) for a golden table.
// Width and Swing changed every kit's sound (default widths, three swung
// kits): rebaseline these rows with KNOCK_REBASELINE=1 after that change.
// Each kit's pattern: loudness while it plays, its tail and its brightness.
// Rebaseline with KNOCK_REBASELINE=1 after an intended change of sound.
struct Fingerprint {
  double hold_db;
  double tail_db;
  double bright_hz;
};
const std::vector<std::pair<std::string_view, Fingerprint>> golden{
    {"Minimal", {-24.582, -69.991, 4293.8}},
    {"Tech House", {-25.845, -69.933, 5440.5}},
    {"Dub Techno", {-19.187, -34.059, 7765.1}},
    {"Organic", {-19.128, -31.750, 2330.1}},
    {"Microhouse Clicks", {-27.336, -76.533, 4479.3}},
    {"Afro Percussion", {-22.494, -67.274, 3883.8}},
    {"Kitchen Foley", {-23.929, -64.161, 3101.0}},
    {"Glitch Crush", {-27.138, -78.879, 4727.0}},
    {"Deep Underbelly", {-21.432, -55.219, 5836.9}},
    {"Rubber Bounce", {-24.895, -75.229, 5207.5}},
    {"Metal Workshop", {-26.746, -56.289, 3524.1}},
    {"Glass Garden", {-22.492, -39.596, 3257.0}},
};

void preset_fingerprints() {
  const bool print = std::getenv("KNOCK_REBASELINE") != nullptr;
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(knock_engine(), preset(table(), entry.name));
    const auto r = render(*runtime, pattern(), 96'000);
    const auto x = mono(r);
    double hold = 0.0;
    for (std::size_t i = 0; i < 48'000; ++i) {
      hold += x[i] * x[i];
    }
    double tail = 0.0;
    for (std::size_t i = 60'000; i < 72'000; ++i) {
      tail += x[i] * x[i];
    }
    const double hold_db = 10.0 * std::log10(hold / 48'000.0 + 1e-30);
    const double tail_db = 10.0 * std::log10(tail / 12'000.0 + 1e-30);
    const double bright = centroid(x, 4'096);
    if (print) {
      std::printf("      {\"%s\", {%.3f, %.3f, %.1f}},\n", std::string(entry.name).c_str(), hold_db,
                  tail_db, bright);
    }
    require(std::isfinite(hold_db) && hold_db > -60.0 && bright > 0.0,
            std::string(entry.name) + " plays its pattern");
    if (print) {
      continue;
    }
    const auto expected =
        std::ranges::find(golden, entry.name, &std::pair<std::string_view, Fingerprint>::first);
    REQUIRE_NE(expected, golden.end(), std::string(entry.name) + " has a golden fingerprint");
    require(std::fabs(hold_db - expected->second.hold_db) < 0.01 &&
                std::fabs(tail_db - expected->second.tail_db) < 0.01 &&
                std::fabs(bright - expected->second.bright_hz) < 0.1,
            std::string(entry.name) + " sounds as it did");
  }
}

// ---- Width and Swing -----------------------------------------------------------

double choice_of(std::size_t parameter, std::string_view name) {
  const auto& choices = table().parameters[parameter].choices;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i] == name) {
      return static_cast<double>(i);
    }
  }
  require(false, "choice exists: " + std::string(name));
  return 0.0;
}

double energy(const std::vector<double>& x, std::size_t from, std::size_t to) {
  double sum = 0.0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) {
    sum += x[i] * x[i];
  }
  return sum;
}

// Where a stretch of output sits: -1 all left, 1 all right.
double balance(const Rendered& r, std::size_t from, std::size_t to) {
  const double left = energy(r.left, from, to);
  const double right = energy(r.right, from, to);
  return (right - left) / (right + left + 1e-30);
}

// A short, gated metal hit, so each of a run of hits sits in its own window.
std::vector<double> short_hit() {
  auto v = plain("Metal", 900.0, 0.5, 0.05);
  v[pad_at(0, "finish.length")] = 0.1;
  return v;
}

std::vector<Event> hits_every(std::size_t gap, std::size_t count) {
  std::vector<Event> events;
  for (std::size_t h = 0; h < count; ++h) {
    const auto n = note(h * gap, 10, key(0), h + 1);
    events.insert(events.end(), n.begin(), n.end());
  }
  return events;
}

// Spread 0 is mono; Spread 1 places the modes across the field, but the
// modes below 150 Hz stay centred.
void spread_widens_and_keeps_lows_centred() {
  auto v = plain("Metal", 600.0, 0.0, 0.3);
  auto runtime = make_runtime(knock_engine(), v);
  const auto narrow = render(*runtime, note(0, 10, key(0), 1), frames_of(0.3));
  double difference = 0.0;
  for (std::size_t i = 0; i < narrow.left.size(); ++i) {
    difference = std::max(difference, std::fabs(narrow.left[i] - narrow.right[i]));
  }
  REQUIRE_GT(peak(narrow.left), 1e-2, "Spread 0 is mono");
  REQUIRE_LE(difference, 1e-5 * peak(narrow.left), "Spread 0 is mono");

  v[pad_at(0, "width.spread")] = 1.0;
  runtime = make_runtime(knock_engine(), v);
  const auto wide = render(*runtime, note(0, 10, key(0), 1), frames_of(0.3));
  double side = 0.0;
  double mid = 0.0;
  for (std::size_t i = 0; i < wide.left.size(); ++i) {
    const double s = 0.5 * (wide.left[i] - wide.right[i]);
    const double m = 0.5 * (wide.left[i] + wide.right[i]);
    side += s * s;
    mid += m * m;
  }
  REQUIRE_GT(side, 0.05 * mid, "Spread 1 is wide");

  // A low skin: its fundamental (60 Hz) and next three modes are below
  // 150 Hz, the rest above.
  auto low = plain("Skin", 60.0, 1.0, 0.4);
  low[pad_at(0, "width.spread")] = 1.0;
  runtime = make_runtime(knock_engine(), low);
  const auto r = render(*runtime, note(0, 10, key(0), 1), frames_of(0.5));
  std::vector<double> sides(r.left.size());
  std::vector<double> mids(r.left.size());
  for (std::size_t i = 0; i < sides.size(); ++i) {
    sides[i] = 0.5 * (r.left[i] - r.right[i]);
    mids[i] = 0.5 * (r.left[i] + r.right[i]);
  }
  for (const double hz : {60.0, 60.0 * 1.594}) {
    REQUIRE_LT(magnitude(sides, frames_of(0.05), frames_of(0.2), hz),
               0.01 * magnitude(mids, frames_of(0.05), frames_of(0.2), hz),
               "modes below 150 Hz stay centred");
  }
}

// Ping-pong: the first hit lands left, the next right, and so on.
void ping_pong_alternates_sides() {
  auto v = short_hit();
  v[pad_at(0, "width.motion")] = choice_of(pad_at(0, "width.motion"), "Ping-pong");
  v[pad_at(0, "width.depth")] = 0.8;
  const std::size_t gap = frames_of(0.25);
  auto runtime = make_runtime(knock_engine(), v);
  const auto r = render(*runtime, hits_every(gap, 4), 4 * gap);
  for (std::size_t h = 0; h < 4; ++h) {
    const double b = balance(r, h * gap, (h + 1) * gap);
    require(h % 2 == 0 ? b < -0.5 : b > 0.5, "Ping-pong alternates sides");
  }
}

// Scatter: a seeded draw per hit, so two runs land the same, and the hits
// land in different places.
void scatter_is_seeded_and_varies() {
  auto v = short_hit();
  v[pad_at(0, "width.motion")] = choice_of(pad_at(0, "width.motion"), "Scatter");
  v[pad_at(0, "width.depth")] = 1.0;
  const std::size_t gap = frames_of(0.25);
  constexpr std::size_t count = 8;
  const auto run = [&] {
    auto runtime = make_runtime(knock_engine(), v);
    const auto r = render(*runtime, hits_every(gap, count), count * gap);
    std::vector<double> places;
    for (std::size_t h = 0; h < count; ++h) {
      places.push_back(balance(r, h * gap, (h + 1) * gap));
    }
    return places;
  };
  const auto first = run();
  REQUIRE_EQ(first, run(), "Scatter is seeded: two runs land the same");
  const auto [lo, hi] = std::ranges::minmax(first);
  REQUIRE_GT(hi - lo, 0.5, "Scatter moves the hits");
}

// Orbit: depth sin(2 pi position / period). At 120 BPM in 4/4 with a
// one-bar period, beat 1 lands right and beat 3, half a period on, left.
void orbit_follows_the_transport() {
  auto v = short_hit();
  v[pad_at(0, "width.motion")] = choice_of(pad_at(0, "width.motion"), "Orbit");
  v[pad_at(0, "width.depth")] = 1.0;
  v[at("knock.orbit")] = choice_of(at("knock.orbit"), "1 bar");
  const std::size_t beat = frames_of(0.5);
  auto events = note(beat, 10, key(0), 1);
  const auto later = note(3 * beat, 10, key(0), 2);
  events.insert(events.end(), later.begin(), later.end());
  auto runtime = make_runtime(knock_engine(), v);
  const auto r = render(*runtime, events, 4 * beat);
  REQUIRE_GT(balance(r, beat, 2 * beat), 0.9, "a quarter of the way round lands right");
  REQUIRE_LT(balance(r, 3 * beat, 4 * beat), -0.9, "three quarters of the way round lands left");
}

// The delay of b behind a: the lag k (0..most) maximising sum a[i] b[i + k].
std::size_t lag_of(const std::vector<double>& a, const std::vector<double>& b, std::size_t most) {
  std::size_t best = 0;
  double best_sum = -1e300;
  for (std::size_t k = 0; k <= most; ++k) {
    double sum = 0.0;
    for (std::size_t i = 0; i + k < b.size() && i < a.size(); ++i) {
      sum += a[i] * b[i + k];
    }
    if (sum > best_sum) {
      best_sum = sum;
      best = k;
    }
  }
  return best;
}

// Haas: the ear away from where the hit lands (the right when centred)
// hears it the set number of milliseconds late.
void haas_delays_the_far_ear() {
  auto v = plain("Metal", 900.0, 0.3, 0.1);
  v[pad_at(0, "width.haas")] = 10.0;
  const auto expected = static_cast<std::size_t>(std::lround(0.010 * rate));
  const std::size_t most = frames_of(0.02);
  auto runtime = make_runtime(knock_engine(), v);
  const auto centred = render(*runtime, note(0, 10, key(0), 1), frames_of(0.6));
  REQUIRE_EQ(lag_of(centred.left, centred.right, most), expected,
             "centred: the right ear is 10 ms late");
  v[pad_at(0, "pan")] = 0.5;
  runtime = make_runtime(knock_engine(), v);
  const auto right = render(*runtime, note(0, 10, key(0), 1), frames_of(0.6));
  REQUIRE_EQ(lag_of(right.right, right.left, most), expected,
             "landing right: the left ear is 10 ms late");
  v[pad_at(0, "width.haas")] = 0.0;
  runtime = make_runtime(knock_engine(), v);
  const auto none = render(*runtime, note(0, 10, key(0), 1), frames_of(0.6));
  REQUIRE_EQ(lag_of(none.left, none.right, most), 0, "no Haas, no delay");
  REQUIRE_EQ(lag_of(none.right, none.left, most), 0, "no Haas, no delay");
}

std::size_t onset(const std::vector<double>& x) {
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (std::fabs(x[i]) > 1e-6) {
      return i;
    }
  }
  return x.size();
}

// Where one hit at `beat` starts, at a tempo, with Swing and the pad's
// follow switch.
std::size_t hit_onset(double swing, bool follow, double beat, double bpm) {
  auto v = plain("Wood", 800.0, 0.5, 0.05);
  v[at("knock.swing")] = swing;
  v[pad_at(0, "swing.follow")] = follow ? 1.0 : 0.0;
  const double frames_per_beat = 60.0 / bpm * rate;
  const auto start = static_cast<std::uint64_t>(std::llround(beat * frames_per_beat));
  auto runtime = make_runtime(knock_engine(), v);
  const auto r =
      render(*runtime, note(start, 10, key(0), 1), start + frames_of(0.3), 128, rate, bpm);
  return onset(mono(r));
}

// An off-beat sixteenth plays swing_delay_beats(swing) late at the tempo;
// on-beat notes, and pads that do not follow, stay where they are.
void swing_delays_off_beat_sixteenths() {
  REQUIRE_EQ(knock::swing_delay_beats(0.0), 0.0,
             "0.5 is the triplet, 0.75 the dotted sixteenth, and no further");
  REQUIRE_LT(std::fabs(knock::swing_delay_beats(0.5) - 1.0 / 12.0), tolerance::double_rounding,
             "0.5 is the triplet, 0.75 the dotted sixteenth, and no further");
  REQUIRE_LT(std::fabs(knock::swing_delay_beats(0.75) - 0.125), tolerance::double_rounding,
             "0.5 is the triplet, 0.75 the dotted sixteenth, and no further");
  REQUIRE_EQ(knock::swing_delay_beats(1.0), knock::swing_delay_beats(0.75),
             "0.5 is the triplet, 0.75 the dotted sixteenth, and no further");
  for (const double bpm : {120.0, 100.0}) {
    for (const double swing : {0.5, 0.75}) {
      const auto delay = static_cast<std::size_t>(
          std::llround(knock::swing_delay_beats(swing) * 60.0 / bpm * rate));
      for (const double off_beat : {0.25, 0.75, 1.25}) {
        REQUIRE_EQ(hit_onset(swing, true, off_beat, bpm),
                   hit_onset(0.0, true, off_beat, bpm) + delay,
                   "an off-beat sixteenth plays late by the swing at the tempo");
      }
      for (const double on_beat : {0.5, 1.0}) {
        REQUIRE_EQ(hit_onset(swing, true, on_beat, bpm), hit_onset(0.0, true, on_beat, bpm),
                   "on-beat notes stay");
      }
      REQUIRE_EQ(hit_onset(swing, false, 0.25, bpm), hit_onset(0.0, true, 0.25, bpm),
                 "a pad that does not follow Swing stays");
    }
  }
}

// Every pad on off-beat sixteenths for a bar at 120 BPM, some together.
std::vector<Event> off_beats() {
  std::vector<Event> events;
  std::uint64_t id = 1;
  constexpr std::uint64_t sixteenth = 6'000;
  for (std::uint64_t step = 1; step < 16; step += 2) {
    for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
      if ((step / 2 + pad) % 2 == 0) {
        const auto n = note(step * sixteenth, 50, key(pad), id++,
                            static_cast<std::uint8_t>(70 + (step * 5 + pad * 7) % 57));
        events.insert(events.end(), n.begin(), n.end());
      }
    }
  }
  return events;
}

// Swung hits wait on the kernel's own frame clock, so any block split plays
// them on the same frames; a seek drops those still waiting.
void swung_hits_survive_any_block_split_and_reset() {
  auto values = preset(table(), "Tech House");
  values[at("knock.swing")] = 0.6;
  constexpr std::size_t frames = 108'000;
  auto a = make_runtime(knock_engine(), values);
  auto b = make_runtime(knock_engine(), values);
  auto c = make_runtime(knock_engine(), values);
  auto d = make_runtime(knock_engine(), values);
  const auto w = render(*a, off_beats(), frames, 128);
  const auto x = render(*b, off_beats(), frames, 0);
  const auto y = render(*c, off_beats(), frames, 1'024);
  const auto z = render(*d, off_beats(), frames, 37);
  double largest = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    for (const auto* other : {&x, &y, &z}) {
      largest = std::max({largest, std::fabs(w.left[i] - other->left[i]),
                          std::fabs(w.right[i] - other->right[i])});
    }
  }
  REQUIRE_EQ(largest, 0.0, "swung hits render identically under any block split");
  values[at("knock.swing")] = 0.0;
  auto straight = make_runtime(knock_engine(), values);
  REQUIRE_NE(render(*straight, off_beats(), frames, 128).left, w.left, "and they were swung");

  // A note on beat 0.25 (frame 6000) with Swing 0.5 plays at frame 8000,
  // unless a seek comes first.
  auto v = plain("Wood", 800.0, 0.5, 0.05);
  v[at("knock.swing")] = 0.5;
  const auto pass = [&](bool seek) {
    auto runtime = make_runtime(knock_engine(), v);
    std::vector<engine::StereoFrame> buffer(128);
    constexpr double beats_per_frame = 120.0 / 60.0 / rate;
    constexpr std::uint64_t note_frame = 6'000;
    double loudest = 0.0;
    for (std::uint64_t block = 0; block < 120; ++block) {
      const std::uint64_t position = block * 128;
      std::vector<engine::BlockMidiEvent> events;
      if (position <= note_frame && note_frame < position + 128) {
        engine::BlockMidiEvent event;
        event.offset = static_cast<std::uint32_t>(note_frame - position);
        event.clip_id_bits = {1, 1};
        event.note_id_bits = {0, 1};
        event.kind = engine::MidiEventKind::note_on;
        event.key = key(0);
        event.velocity = 100;
        events.push_back(event);
      }
      std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
      const double beat = static_cast<double>(position) * beats_per_frame;
      const SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
      const std::uint64_t generation = seek && position > note_frame ? 2 : 1;
      const auto report = runtime->process(buffer, events, position, generation, transport, {});
      REQUIRE_EQ(report.status, SynthProcessStatus::rendered, "block renders");
      for (const auto& frame : buffer) {
        loudest = std::max({loudest, static_cast<double>(std::fabs(frame.left)),
                            static_cast<double>(std::fabs(frame.right))});
      }
    }
    return loudest;
  };
  REQUIRE_GT(pass(false), 1e-3, "the swung hit plays");
  REQUIRE_EQ(pass(true), 0.0, "a seek before it plays drops it");
}

// Telemetry: where each pad's latest hit landed, Motion included.
void telemetry_reports_landing_side() {
  auto v = plain();
  v[pad_at(0, "width.motion")] = choice_of(pad_at(0, "width.motion"), "Ping-pong");
  v[pad_at(0, "width.depth")] = 0.8;
  auto runtime = make_runtime(knock_engine(), v);
  std::vector<float> frame;
  (void)render(*runtime, {}, 128);
  REQUIRE_TRUE(runtime->read_telemetry(frame), "centre before any hit");
  REQUIRE_EQ(frame.size(), knock::telemetry::size, "centre before any hit");
  REQUIRE_EQ(frame[knock::telemetry::first_place], 0.0F, "centre before any hit");
  REQUIRE_EQ(frame[knock::telemetry::first_place + 3], 0.0F, "centre before any hit");
  (void)render(*runtime, note(0, 10, key(0), 1), 2'400);
  REQUIRE_TRUE(runtime->read_telemetry(frame), "the first hit lands left");
  REQUIRE_LT(std::fabs(frame[knock::telemetry::first_place] + 0.8F), tolerance::float_round_trip_f,
             "the first hit lands left");
  (void)render(*runtime, note(0, 10, key(0), 2), 2'400);
  REQUIRE_TRUE(runtime->read_telemetry(frame), "the second lands right");
  REQUIRE_LT(std::fabs(frame[knock::telemetry::first_place] - 0.8F), tolerance::float_round_trip_f,
             "the second lands right");
}

// Width never recalibrates: a pad's level is the same mono or spread, with
// or without Haas.
void calibration_ignores_width() {
  const auto level_db = [](double spread, double haas) {
    auto v = plain("Wood", 600.0, 1.0, 0.1);
    v[pad_at(0, "width.spread")] = spread;
    v[pad_at(0, "width.haas")] = haas;
    auto runtime = make_runtime(knock_engine(), v);
    const auto r = render(*runtime, note(0, 10, key(0), 1), frames_of(1.0));
    return 10.0 * std::log10(energy(r.left, 0, r.left.size()) + energy(r.right, 0, r.right.size()));
  };
  const double mono_db = level_db(0.0, 0.0);
  REQUIRE_LT(std::fabs(level_db(1.0, 0.0) - mono_db), 0.5, "Spread keeps the pad's level");
  REQUIRE_LT(std::fabs(level_db(1.0, 12.0) - mono_db), 0.5, "and so does Haas");
}

// Each pad's part in a groove, and the width it gets from its sound.
void pad_roles_and_default_widths() {
  const auto v = preset(table(), "Minimal");
  // Plop 600 Hz, Rubber 170 Hz, Click 3.2 kHz, Shaker (a brush).
  require(knock::pad_role(v, 0) == knock::PadRole::mid &&
              knock::pad_role(v, 1) == knock::PadRole::low &&
              knock::pad_role(v, 2) == knock::PadRole::high &&
              knock::pad_role(v, 6) == knock::PadRole::high,
          "low thuds, mid knocks, high ticks and shakers");
  REQUIRE_EQ(knock::pad_role(v, knock::pad_count), knock::PadRole::mid,
             "a bad pad or short values is a mid");
  REQUIRE_EQ(knock::pad_role(std::span<const double>(v).subspan(0, 10), 0), knock::PadRole::mid,
             "a bad pad or short values is a mid");
  const auto motion = [&](std::size_t pad) { return v[pad_at(pad, "width.motion")]; };
  REQUIRE_EQ(v[pad_at(6, "width.spread")], 0.6, "a brush scatters, with Haas");
  REQUIRE_EQ(motion(6), choice_of(pad_at(6, "width.motion"), "Scatter"),
             "a brush scatters, with Haas");
  REQUIRE_EQ(v[pad_at(6, "width.depth")], 0.55, "a brush scatters, with Haas");
  REQUIRE_EQ(v[pad_at(6, "width.haas")], 3.0, "a brush scatters, with Haas");
  REQUIRE_EQ(v[pad_at(1, "width.spread")], 0.15, "a low pad stays narrow and still");
  REQUIRE_EQ(motion(1), choice_of(pad_at(1, "width.motion"), "Fixed"),
             "a low pad stays narrow and still");
  REQUIRE_EQ(v[pad_at(2, "width.spread")], 0.5, "a high tick ping-pongs");
  REQUIRE_EQ(motion(2), choice_of(pad_at(2, "width.motion"), "Ping-pong"),
             "a high tick ping-pongs");
  REQUIRE_EQ(v[pad_at(2, "width.depth")], 0.45, "a high tick ping-pongs");
  REQUIRE_EQ(v[pad_at(0, "width.spread")], 0.4, "a mid pad spreads its modes and stays put");
  REQUIRE_EQ(motion(0), 0.0, "a mid pad spreads its modes and stays put");
  REQUIRE_EQ(v[pad_at(0, "width.haas")], 0.0, "a mid pad spreads its modes and stays put");
  REQUIRE_EQ(v[at("knock.swing")], 0.0,
             "no swing by default, a two-bar orbit, every pad following");
  REQUIRE_EQ(v[at("knock.orbit")], choice_of(at("knock.orbit"), "2 bars"),
             "no swing by default, a two-bar orbit, every pad following");
  REQUIRE_EQ(v[pad_at(0, "swing.follow")], choice_of(pad_at(0, "swing.follow"), "On"),
             "no swing by default, a two-bar orbit, every pad following");
  REQUIRE_EQ(format_value(table().parameters[at("knock.swing")], 0.75), "75 %",
             "Swing shows as a percentage");
  for (const auto* name : {"Tech House", "Afro Percussion", "Rubber Bounce"}) {
    REQUIRE_GT(preset(table(), name)[at("knock.swing")], 0.0, std::string(name) + " swings");
  }
}

// MIDI keys the key tests name, so each case reads as notes.
namespace notes {
constexpr double f_sharp1 = 30.0;
constexpr double g1 = 31.0;
constexpr double g_sharp1 = 32.0;
constexpr double d2 = 38.0;
constexpr double d_sharp2 = 39.0;
constexpr double e2 = 40.0;
constexpr double f_sharp2 = 42.0;
constexpr double c4 = 60.0;
constexpr double c_sharp4 = 61.0;
constexpr double d4 = 62.0;
constexpr double a4 = knock::tuning_a4_key;
constexpr double a_sharp4 = a4 + 1.0;
// Tonics, as pitch classes (C = 0).
constexpr std::uint8_t c = 0;
constexpr std::uint8_t e = 4;
constexpr std::uint8_t a = 9;
} // namespace notes
// A fraction of a semitone: "slightly sharp" of a note.
constexpr double slightly = 0.4;

// The nearest note of the key's scale, equal tempered (knock::note_hz);
// halfway goes down; nothing moves without a key.
void in_key_snaps_to_the_scale() {
  using knock::note_hz;
  const auto near = [](double a, double b) {
    return std::fabs(a - b) < tolerance::double_accumulation * b;
  };
  const auto snaps = [&](double from, project::MusicalKey key, double to) {
    return near(knock::in_key(note_hz(from), key), note_hz(to));
  };
  const project::MusicalKey c_major{notes::c, project::KeyMode::major};
  const project::MusicalKey a_minor{notes::a, project::KeyMode::minor};
  const project::MusicalKey e_minor{notes::e, project::KeyMode::minor};
  REQUIRE_EQ(note_hz(notes::a4), knock::tuning_a4_hz, "A4 is the tuning reference");
  const double unkeyed = note_hz(notes::d4 + slightly);
  REQUIRE_EQ(knock::in_key(unkeyed, std::nullopt), unkeyed, "no key, no change");
  REQUIRE_TRUE(snaps(notes::a4, a_minor, notes::a4),
               "A is in A minor, and a slightly sharp A falls back to it");
  REQUIRE_TRUE(snaps(notes::a4 + slightly, a_minor, notes::a4),
               "A is in A minor, and a slightly sharp A falls back to it");
  REQUIRE_TRUE(snaps(notes::c_sharp4, c_major, notes::c4),
               "C# and A# sit halfway in C major and go down");
  REQUIRE_TRUE(snaps(notes::a_sharp4, c_major, notes::a4),
               "C# and A# sit halfway in C major and go down");
  require(snaps(notes::c_sharp4 + slightly, c_major, notes::d4), "nearer D than C goes to D");
  REQUIRE_TRUE(snaps(notes::f_sharp2 - slightly, e_minor, notes::f_sharp2),
               "E minor has F# and E; D# halfway to E goes down to D");
  REQUIRE_TRUE(snaps(notes::d_sharp2 + slightly, e_minor, notes::e2),
               "E minor has F# and E; D# halfway to E goes down to D");
  REQUIRE_TRUE(snaps(notes::d_sharp2, e_minor, notes::d2),
               "E minor has F# and E; D# halfway to E goes down to D");
  REQUIRE_TRUE(snaps(notes::g1, e_minor, notes::g1),
               "G and F# are in E minor; G# halfway to A goes down to G");
  REQUIRE_TRUE(snaps(notes::g_sharp1, e_minor, notes::g1),
               "G and F# are in E minor; G# halfway to A goes down to G");
  REQUIRE_TRUE(snaps(notes::f_sharp1, e_minor, notes::f_sharp1),
               "G and F# are in E minor; G# halfway to A goes down to G");
  REQUIRE_EQ(knock::in_key(0.0, c_major), 0.0, "a non-positive pitch is left alone");
}

// A following pad plays exactly as if its Pitch were the in-key note; a pad
// that does not follow, or no key, plays its own Pitch; a new key reaches
// the next hit.
void pads_follow_the_project_key() {
  const project::MusicalKey c_major{notes::c, project::KeyMode::major};
  // Past the middle of A4 to A#4, which C major lacks: it plays A4.
  const double pitch = knock::note_hz(notes::a4 + slightly);
  const double in_key = knock::in_key(pitch, c_major);
  // A ringing note (Tone 1), long enough to measure.
  constexpr double note_tone = 1.0;
  constexpr double ringing_decay = 0.3;
  constexpr double hit_seconds = 0.3;
  const auto played = [&](bool follow, std::optional<project::MusicalKey> project_key, double hz) {
    auto v = plain("Wood", hz, note_tone, ringing_decay);
    v[pad_at(0, "key.follow")] = follow ? 1.0 : 0.0;
    auto runtime = make_runtime(knock_engine(), v);
    runtime->offer_project_key(project_key);
    return mono(render(*runtime, note(0, 100, key(0), 1), frames_of(hit_seconds)));
  };
  const auto tuned = played(false, std::nullopt, in_key);
  const auto own = played(false, std::nullopt, pitch);
  REQUIRE_NE(tuned, own, "the in-key note differs from the pad's own pitch");
  REQUIRE_EQ(played(true, c_major, pitch), tuned, "a following pad plays the in-key note");
  REQUIRE_EQ(played(false, c_major, pitch), own, "a pad that does not follow keeps its pitch");
  REQUIRE_EQ(played(true, std::nullopt, pitch), own,
             "without a key a following pad keeps its pitch");

  // A key offered between hits reaches the second, not the first. Short
  // hits, each over before the next, measured from A4 down a semitone to
  // A#4 up a semitone.
  constexpr double short_decay = 0.05;
  constexpr double settled_seconds = 0.5;
  constexpr double measure_from_seconds = 0.01;
  constexpr double measure_seconds = 0.2;
  const auto rings_at = [&](const std::vector<double>& x) {
    return strongest_hz(x, frames_of(measure_from_seconds), frames_of(measure_seconds),
                        knock::note_hz(notes::a4 - 1.0), knock::note_hz(notes::a_sharp4 + 1.0));
  };
  const auto lands = [](double measured, double expected) {
    return std::fabs(measured - expected) <= strongest_hz_step;
  };
  auto v = plain("Wood", pitch, note_tone, short_decay);
  v[pad_at(0, "key.follow")] = 1.0;
  auto runtime = make_runtime(knock_engine(), v);
  const auto first = mono(render(*runtime, note(0, 100, key(0), 1), frames_of(settled_seconds)));
  runtime->offer_project_key(c_major);
  const auto second = mono(render(*runtime, note(0, 100, key(0), 2), frames_of(settled_seconds)));
  REQUIRE_TRUE(lands(rings_at(first), pitch), "a new key reaches the next hit");
  REQUIRE_TRUE(lands(rings_at(second), in_key), "a new key reaches the next hit");
}

// Lows and mids follow the key by default; high ticks and shakers keep
// their pitch.
void key_follow_defaults_from_the_sound() {
  const auto v = preset(table(), "Minimal");
  const auto on = choice_of(pad_at(0, "key.follow"), "On");
  const auto off = choice_of(pad_at(0, "key.follow"), "Off");
  for (std::size_t pad = 0; pad < knock::pad_count; ++pad) {
    const bool high = knock::pad_role(v, pad) == knock::PadRole::high;
    REQUIRE_EQ(v[pad_at(pad, "key.follow")], (high ? off : on),
               "pad " + std::to_string(pad + 1) + " follows the key unless it is a high tick");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "knock engine contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"any_block_split_renders_identically", [] { any_block_split_renders_identically(); }},
          {"reset_is_deterministic", [] { reset_is_deterministic(); }},
          {"every_preset_is_finite_and_bounded", [] { every_preset_is_finite_and_bounded(); }},
          {"tone_zero_is_a_thing_tone_one_a_note", [] { tone_zero_is_a_thing_tone_one_a_note(); }},
          {"bend_falls_and_rises", [] { bend_falls_and_rises(); }},
          {"the_gate_ends_the_hit", [] { the_gate_ends_the_hit(); }},
          {"lift_attenuates_below_its_cutoff", [] { lift_attenuates_below_its_cutoff(); }},
          {"crush_quantises", [] { crush_quantises(); }},
          {"coupling_rings_the_coupled_modes", [] { coupling_rings_the_coupled_modes(); }},
          {"contact_time_follows_hardness", [] { contact_time_follows_hardness(); }},
          {"velocity_brightens", [] { velocity_brightens(); }},
          {"global_decay_shortens_every_ring", [] { global_decay_shortens_every_ring(); }},
          {"preset_levels_sit_in_a_window", [] { preset_levels_sit_in_a_window(); }},
          {"design_pad_is_what_the_kernel_plays", [] { design_pad_is_what_the_kernel_plays(); }},
          {"telemetry_reports_hits", [] { telemetry_reports_hits(); }},
          {"full_load_does_not_allocate", [] { full_load_does_not_allocate(); }},
          {"tail_bound_covers_every_kit", [] { tail_bound_covers_every_kit(); }},
          {"preset_fingerprints", [] { preset_fingerprints(); }},
          {"spread_widens_and_keeps_lows_centred", [] { spread_widens_and_keeps_lows_centred(); }},
          {"ping_pong_alternates_sides", [] { ping_pong_alternates_sides(); }},
          {"scatter_is_seeded_and_varies", [] { scatter_is_seeded_and_varies(); }},
          {"orbit_follows_the_transport", [] { orbit_follows_the_transport(); }},
          {"haas_delays_the_far_ear", [] { haas_delays_the_far_ear(); }},
          {"swing_delays_off_beat_sixteenths", [] { swing_delays_off_beat_sixteenths(); }},
          {"swung_hits_survive_any_block_split_and_reset",
           [] { swung_hits_survive_any_block_split_and_reset(); }},
          {"telemetry_reports_landing_side", [] { telemetry_reports_landing_side(); }},
          {"calibration_ignores_width", [] { calibration_ignores_width(); }},
          {"pad_roles_and_default_widths", [] { pad_roles_and_default_widths(); }},
          {"in_key_snaps_to_the_scale", [] { in_key_snaps_to_the_scale(); }},
          {"pads_follow_the_project_key", [] { pads_follow_the_project_key(); }},
          {"key_follow_defaults_from_the_sound", [] { key_follow_defaults_from_the_sound(); }},
      });
}
