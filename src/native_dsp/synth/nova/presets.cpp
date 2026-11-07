#include "nova_presets.hpp"

#include "oscillate/native_dsp/synth/nova/fx.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The factory presets: the first ten, one per role, each built to show a
// part of the engine, then the library by category. Spec 08 §12 aims for
// 150+.
namespace oscillate::native_dsp::synth::nova {
namespace {

using Value = std::pair<std::string_view, double>;
using project::NativeBuiltinMedia;
using project::NativeChain;
using project::NativeCurve;
using project::NativeCurveSegment;
using project::NativeLabel;
using project::NativeMediaSlot;
using project::NativeModRoute;
using project::NativeSynthDocument;

// An open saw, as a blank patch should be: one voice, the filter off (its
// settings ready for when it is switched on), the amp held while the key is.
constexpr auto init = std::to_array<Value>({
    {"nova.master", -8.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 1.0},
    {"nova.filter.1.on", 0.0},
    {"nova.filter.1.cutoff", 2'500.0},
    {"nova.filter.1.resonance", 0.25},
    {"nova.env.1.sustain", 1.0},
});

constexpr auto glass_reese = std::to_array<Value>({
    {"nova.master", -0.5},
    {"nova.osc.a.position", 0.3},
    {"nova.osc.a.unison.voices", 7.0},
    {"nova.osc.a.unison.detune", 32.0},
    {"nova.osc.a.unison.width", 0.9},
    {"nova.osc.a.octave", 0.0},
    {"nova.osc.a.warp.1.mode", 1.0},
    {"nova.osc.a.warp.1.amount", 0.25},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 1.0},
    {"nova.osc.b.position", 0.6},
    {"nova.osc.b.unison.voices", 5.0},
    {"nova.osc.b.unison.detune", 24.0},
    {"nova.osc.b.level", 0.55},
    {"nova.osc.b.octave", 0.0},
    {"nova.osc.b.fine", 7.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.55},
    {"nova.sub.octave", 1.0},
    {"nova.filter.1.model", 0.0},
    {"nova.filter.1.cutoff", 900.0},
    {"nova.filter.1.resonance", 0.3},
    {"nova.filter.1.drive", 6.0},
    {"nova.lfo.1.sync", 4.0},
    {"nova.mod.0.amount", 0.35},
    {"nova.mod.1.amount", 0.25},
    {"nova.mod.2.amount", 0.6},
    {"nova.macro.1", 0.2},
    {"nova.global.voicing", 1.0},
});

constexpr auto supersaw_lead = std::to_array<Value>({
    {"nova.master", -4.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 9.0},
    {"nova.osc.a.unison.detune", 38.0},
    {"nova.osc.a.unison.blend", 0.75},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.octave", 1.0},
    {"nova.osc.b.unison.voices", 5.0},
    {"nova.osc.b.unison.detune", 22.0},
    {"nova.osc.b.level", 0.45},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 7'000.0},
    {"nova.filter.1.resonance", 0.1},
    {"nova.env.1.attack", 0.005},
    {"nova.env.1.release", 0.35},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.6},
    {"nova.env.2.sustain", 0.3},
    {"nova.mod.0.amount", 0.18},
    {"nova.fx.1.mix", 0.22},
    {"nova.fx.1.p0", 0.55},
    {"nova.fx.1.p1", 0.45},
    {"nova.fx.2.mix", 0.25},
    {"nova.fx.2.p0", 0.5714285714285714},
    {"nova.fx.2.p1", 0.35},
    {"nova.fx.2.p5", 1.0},
    {"nova.fx.2.p7", 0.5},
});

constexpr auto wobble_growl = std::to_array<Value>({
    {"nova.master", -3.0},
    {"nova.osc.a.position", 0.4},
    {"nova.osc.a.octave", 0.0},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 12.0},
    {"nova.osc.a.warp.1.mode", 12.0},
    {"nova.osc.a.warp.1.amount", 0.2},
    {"nova.osc.a.warp.1.source", 1.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 0.0},
    {"nova.osc.b.level", 0.0},
    {"nova.osc.b.octave", 0.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.6},
    {"nova.sub.octave", 1.0},
    {"nova.filter.1.model", 13.0},
    {"nova.filter.1.cutoff", 1'200.0},
    {"nova.filter.1.resonance", 0.55},
    {"nova.filter.1.fat", 0.3},
    {"nova.lfo.1.sync", 12.0},
    {"nova.lfo.1.mode", 2.0},
    {"nova.mod.0.amount", 0.5},
    {"nova.mod.1.amount", 0.6},
    {"nova.mod.2.amount", 0.4},
    {"nova.macro.1", 0.3},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.04},
    {"nova.fx.1.p0", 0.45},
    {"nova.fx.1.p7", 0.0},
    {"nova.fx.2.p0", 0.7},
    {"nova.fx.2.mix", 0.8},
});

constexpr auto analog_pad = std::to_array<Value>({
    {"nova.master", 6.0},
    {"nova.osc.a.position", 0.5},
    {"nova.osc.a.unison.voices", 6.0},
    {"nova.osc.a.unison.detune", 20.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.unison.voices", 4.0},
    {"nova.osc.b.unison.detune", 14.0},
    {"nova.osc.b.level", 0.5},
    {"nova.osc.b.fine", -6.0},
    {"nova.filter.1.model", 4.0},
    {"nova.filter.1.cutoff", 1'800.0},
    {"nova.filter.1.resonance", 0.2},
    {"nova.env.1.attack", 0.9},
    {"nova.env.1.decay", 1.0},
    {"nova.env.1.sustain", 0.85},
    {"nova.env.1.release", 2.5},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 0.15},
    {"nova.lfo.2.sync", 0.0},
    {"nova.lfo.2.rate", 0.07},
    {"nova.mod.0.amount", 0.4},
    {"nova.mod.1.amount", 0.2},
    {"nova.fx.1.mix", 0.5},
    {"nova.fx.2.mix", 0.4},
});

constexpr auto pluck = std::to_array<Value>({
    {"nova.master", -1.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 10.0},
    {"nova.filter.1.model", 0.0},
    {"nova.filter.1.cutoff", 400.0},
    {"nova.filter.1.resonance", 0.35},
    {"nova.filter.1.keytrack", 0.5},
    {"nova.env.1.decay", 0.45},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.3},
    {"nova.env.2.decay", 0.22},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.55},
    {"nova.fx.1.mix", 0.2},
});

constexpr auto fm_bell = std::to_array<Value>({
    {"nova.master", -5.0},
    {"nova.osc.a.position", 0.25},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 6.0},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.attack", 0.001},
    {"nova.env.1.decay", 2.5},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 1.8},
    {"nova.env.2.decay", 1.2},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.5},
    {"nova.fx.1.mix", 0.3},
});

constexpr auto deep_sub = std::to_array<Value>({
    {"nova.master", 3.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 0.0},
    {"nova.osc.a.octave", -1.0},
    {"nova.osc.a.unison.voices", 1.0},
    {"nova.osc.a.route", 2.0},
    {"nova.osc.a.random_phase", 0.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.shape", 1.0},
    {"nova.sub.level", 0.35},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.release", 0.08},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.03},
});

constexpr auto hoover = std::to_array<Value>({
    {"nova.master", 2.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 4.0},
    {"nova.osc.a.pw", 0.3},
    {"nova.osc.a.unison.voices", 12.0},
    {"nova.osc.a.unison.detune", 70.0},
    {"nova.osc.a.unison.stack", 1.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.octave", -1.0},
    {"nova.osc.b.unison.voices", 8.0},
    {"nova.osc.b.unison.detune", 45.0},
    {"nova.filter.1.cutoff", 5'000.0},
    {"nova.env.2.decay", 0.35},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", -0.08},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.08},
});

constexpr auto arp_sequence = std::to_array<Value>({
    {"nova.master", 0.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 3.0},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 8.0},
    {"nova.filter.1.model", 2.0},
    {"nova.filter.1.cutoff", 700.0},
    {"nova.filter.1.resonance", 0.6},
    {"nova.env.1.decay", 0.2},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.2.decay", 0.15},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.45},
    {"nova.arp.on", 1.0},
    {"nova.arp.octaves", 2.0},
});

// ---- The library ---------------------------------------------------------------------
// Each preset below names only what differs from the defaults (and from the
// placed effects' contract defaults). Levels are set by nova.master so the
// library plays at one level: a held C major chord (C1 for basses) peaks
// near -6 dBFS, or as near as the master's +6 dB reaches.

// Basses.
constexpr auto rolling_reese = std::to_array<Value>({
    {"nova.master", 1.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 24.0},
    {"nova.osc.a.unison.width", 0.6},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.fine", -14.0},
    {"nova.osc.b.level", 0.6},
    {"nova.filter.1.model", 0.0},
    {"nova.filter.1.cutoff", 520.0},
    {"nova.filter.1.resonance", 0.18},
    {"nova.filter.1.drive", 6.0},
    {"nova.lfo.1.sync", 4.0},
    {"nova.mod.0.amount", 0.12},
    {"nova.env.1.release", 0.12},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.04},
    {"nova.fx.1.p0", 0.2},
    {"nova.fx.1.p7", 1.0 / 11.0},
    {"nova.fx.2.p2", 0.557},
});

constexpr auto acid_line = std::to_array<Value>({
    {"nova.master", 4.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.random_phase", 0.0},
    {"nova.filter.1.model", 2.0},
    {"nova.filter.1.cutoff", 320.0},
    {"nova.filter.1.resonance", 0.78},
    {"nova.filter.1.drive", 8.0},
    {"nova.env.1.decay", 0.3},
    {"nova.env.1.sustain", 0.6},
    {"nova.env.1.release", 0.04},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.18},
    {"nova.env.2.sustain", 0.0},
    {"nova.env.2.velocity", 0.6},
    {"nova.mod.0.amount", 0.42},
    {"nova.mod.1.amount", 0.2},
    {"nova.mod.2.amount", 0.3},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.06},
    {"nova.fx.1.p0", 0.25},
    {"nova.fx.1.p7", 1.0},
    {"nova.fx.1.mix", 0.6},
    {"nova.fx.2.p0", 7.0 / 14.0},
    {"nova.fx.2.p1", 0.3},
    {"nova.fx.2.p7", 0.5},
    {"nova.fx.2.mix", 0.15},
});

constexpr auto organ_bass = std::to_array<Value>({
    {"nova.master", 3.0},
    {"nova.osc.a.position", 0.3},
    {"nova.osc.a.octave", -1.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.45},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 1'400.0},
    {"nova.filter.1.resonance", 0.1},
    {"nova.env.1.decay", 0.25},
    {"nova.env.1.sustain", 0.25},
    {"nova.env.1.release", 0.05},
    {"nova.global.voicing", 1.0},
});

constexpr auto analog_bass = std::to_array<Value>({
    {"nova.master", 6.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 3.0},
    {"nova.osc.b.octave", -1.0},
    {"nova.osc.b.level", 0.55},
    {"nova.filter.1.model", 3.0},
    {"nova.filter.1.cutoff", 420.0},
    {"nova.filter.1.resonance", 0.3},
    {"nova.filter.1.keytrack", 0.5},
    {"nova.env.1.decay", 0.4},
    {"nova.env.1.sustain", 0.5},
    {"nova.env.1.release", 0.08},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.25},
    {"nova.env.2.sustain", 0.1},
    {"nova.mod.0.amount", 0.35},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.03},
});

constexpr auto fm_bass = std::to_array<Value>({
    {"nova.master", 0.5},
    {"nova.osc.a.position", 0.3},
    {"nova.osc.a.octave", -1.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.5},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.decay", 0.5},
    {"nova.env.1.sustain", 0.7},
    {"nova.env.1.release", 0.08},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.22},
    {"nova.env.2.sustain", 0.2},
    {"nova.mod.0.amount", 0.4},
    {"nova.global.voicing", 2.0},
});

constexpr auto techno_rumble = std::to_array<Value>({
    {"nova.master", 6.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 1.0},
    {"nova.osc.a.octave", -1.0},
    {"nova.osc.a.random_phase", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.25},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 160.0},
    {"nova.filter.1.resonance", 0.25},
    {"nova.filter.1.drive", 12.0},
    {"nova.lfo.1.sync", 15.0},
    {"nova.mod.0.amount", 0.08},
    {"nova.env.1.decay", 0.6},
    {"nova.env.1.sustain", 0.4},
    {"nova.env.1.release", 0.4},
    {"nova.global.voicing", 1.0},
    {"nova.fx.1.p0", 0.7},
    {"nova.fx.1.p1", 0.6},
    {"nova.fx.1.p2", 0.6},
    {"nova.fx.1.p7", 2.0 / 3.0},
    {"nova.fx.1.mix", 0.35},
    {"nova.fx.2.p2", 0.4},
});

constexpr auto neuro_bass = std::to_array<Value>({
    {"nova.master", -7.5},
    {"nova.osc.a.position", 0.5},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 10.0},
    {"nova.osc.a.warp.1.mode", 12.0},
    {"nova.osc.a.warp.1.amount", 0.25},
    {"nova.osc.a.warp.1.source", 1.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 0.0},
    {"nova.osc.b.level", 0.0},
    {"nova.osc.b.octave", 1.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.5},
    {"nova.filter.1.model", 13.0},
    {"nova.filter.1.cutoff", 900.0},
    {"nova.filter.1.resonance", 0.45},
    {"nova.lfo.1.sync", 12.0},
    {"nova.mod.0.amount", 0.45},
    {"nova.mod.1.amount", 0.35},
    {"nova.mod.2.amount", 0.5},
    {"nova.global.voicing", 2.0},
    {"nova.fx.1.p0", 0.4},
    {"nova.fx.1.p7", 4.0 / 11.0},
    {"nova.fx.2.p0", 0.65},
});

constexpr auto talking_bass = std::to_array<Value>({
    {"nova.master", 1.0},
    {"nova.osc.a.position", 0.2},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 8.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.45},
    {"nova.filter.1.model", 14.0},
    {"nova.filter.1.cutoff", 900.0},
    {"nova.filter.1.resonance", 0.5},
    {"nova.lfo.1.sync", 9.0},
    {"nova.mod.0.amount", 0.6},
    {"nova.mod.1.amount", 0.3},
    {"nova.global.voicing", 2.0},
    {"nova.fx.1.p0", 0.3},
});

constexpr auto offbeat_bass = std::to_array<Value>({
    {"nova.master", 3.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 12.0},
    {"nova.osc.a.unison.width", 0.3},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 700.0},
    {"nova.filter.1.resonance", 0.2},
    {"nova.env.1.decay", 0.16},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.05},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.12},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.3},
    {"nova.global.voicing", 1.0},
});

// Leads.
constexpr auto sync_lead = std::to_array<Value>({
    {"nova.master", 1.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.sync", 2.2},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 14.0},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 6'500.0},
    {"nova.filter.1.resonance", 0.15},
    {"nova.env.1.release", 0.25},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.5},
    {"nova.env.2.sustain", 0.2},
    {"nova.mod.0.amount", 0.35},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.04},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.mix", 0.2},
    {"nova.fx.2.mix", 0.2},
});

constexpr auto chip_lead = std::to_array<Value>({
    {"nova.master", 4.5},
    {"nova.osc.a.position", 0.2},
    {"nova.osc.a.drift", 0.0},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.release", 0.06},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 6.0},
    {"nova.lfo.1.delay", 0.25},
    {"nova.lfo.1.rise", 0.2},
    {"nova.mod.0.amount", 0.0025},
    {"nova.global.voicing", 2.0},
    {"nova.global.drift", 0.0},
    {"nova.fx.1.p0", 7.0 / 14.0},
    {"nova.fx.1.p1", 0.35},
    {"nova.fx.1.mix", 0.2},
});

constexpr auto mono_saw_lead = std::to_array<Value>({
    {"nova.master", 4.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 16.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 3.0},
    {"nova.osc.b.octave", -1.0},
    {"nova.osc.b.level", 0.4},
    {"nova.filter.1.model", 0.0},
    {"nova.filter.1.cutoff", 3'200.0},
    {"nova.filter.1.resonance", 0.25},
    {"nova.env.1.release", 0.2},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.4},
    {"nova.env.2.sustain", 0.35},
    {"nova.mod.0.amount", 0.25},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.07},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.4},
    {"nova.fx.1.mix", 0.25},
    {"nova.fx.2.mix", 0.22},
});

constexpr auto screamer = std::to_array<Value>({
    {"nova.master", 5.0},
    {"nova.osc.a.position", 0.6},
    {"nova.osc.a.unison.voices", 4.0},
    {"nova.osc.a.unison.detune", 20.0},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 3'500.0},
    {"nova.filter.1.resonance", 0.45},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 5.5},
    {"nova.lfo.1.delay", 0.3},
    {"nova.lfo.1.rise", 0.3},
    {"nova.mod.0.amount", 0.003},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.05},
    {"nova.fx.1.p0", 0.5},
    {"nova.fx.1.p7", 2.0 / 11.0},
    {"nova.fx.2.mix", 0.2},
});

constexpr auto whistle = std::to_array<Value>({
    {"nova.master", 3.0},         {"nova.osc.a.type", 0.0},         {"nova.osc.a.shape", 0.0},
    {"nova.osc.b.on", 1.0},       {"nova.osc.b.type", 0.0},         {"nova.osc.b.shape", 1.0},
    {"nova.osc.b.octave", 1.0},   {"nova.osc.b.level", 0.15},       {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.04},   {"nova.filter.1.on", 0.0},        {"nova.env.1.attack", 0.04},
    {"nova.env.1.release", 0.3},  {"nova.lfo.1.sync", 0.0},         {"nova.lfo.1.rate", 5.0},
    {"nova.lfo.1.delay", 0.35},   {"nova.lfo.1.rise", 0.4},         {"nova.mod.0.amount", 0.002},
    {"nova.global.voicing", 2.0}, {"nova.global.portamento", 0.08}, {"nova.fx.1.mix", 0.3},
});

constexpr auto formant_lead = std::to_array<Value>({
    {"nova.master", 6.0},
    {"nova.osc.a.position", 0.3},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 12.0},
    {"nova.filter.1.on", 0.0},
    {"nova.lfo.1.sync", 6.0},
    {"nova.mod.0.amount", 0.45},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.05},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.mix", 0.25},
    {"nova.fx.2.mix", 0.25},
});

// Pads.
constexpr auto glass_pad = std::to_array<Value>({
    {"nova.master", -4.0},
    {"nova.osc.a.position", 0.3},
    {"nova.osc.a.unison.voices", 4.0},
    {"nova.osc.a.unison.detune", 14.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.osc.a.unison.position_spread", 0.2},
    {"nova.osc.a.level", 1.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.position", 0.55},
    {"nova.osc.b.octave", 1.0},
    {"nova.osc.b.unison.voices", 3.0},
    {"nova.osc.b.unison.detune", 10.0},
    {"nova.osc.b.unison.width", 1.0},
    {"nova.osc.b.level", 0.8},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 5'000.0},
    {"nova.env.1.attack", 1.2},
    {"nova.env.1.decay", 1.0},
    {"nova.env.1.sustain", 0.9},
    {"nova.env.1.release", 3.0},
    {"nova.lfo.1.sync", 2.0},
    {"nova.mod.0.amount", 0.3},
    {"nova.fx.1.mix", 0.3},
    {"nova.fx.2.p1", 0.647},
    {"nova.fx.2.mix", 0.3},
});

constexpr auto dark_strings = std::to_array<Value>({
    {"nova.master", -0.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 7.0},
    {"nova.osc.a.unison.detune", 16.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 1'100.0},
    {"nova.filter.1.resonance", 0.1},
    {"nova.env.1.attack", 0.6},
    {"nova.env.1.decay", 1.0},
    {"nova.env.1.sustain", 0.9},
    {"nova.env.1.release", 2.0},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 0.12},
    {"nova.mod.0.amount", 0.08},
    {"nova.fx.1.mix", 0.35},
    {"nova.fx.2.p1", 0.6},
    {"nova.fx.2.mix", 0.4},
});

constexpr auto spectral_drift = std::to_array<Value>({
    {"nova.master", 5.0},
    {"nova.osc.a.position", 0.2},
    {"nova.osc.a.unison.voices", 5.0},
    {"nova.osc.a.unison.detune", 10.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.osc.a.unison.position_spread", 0.5},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.attack", 2.0},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 4.0},
    {"nova.lfo.1.sync", 1.0},
    {"nova.mod.0.amount", 0.6},
    {"nova.fx.1.p0", 0.8},
    {"nova.fx.1.p1", 0.72},
    {"nova.fx.1.p7", 1.0},
    {"nova.fx.1.mix", 0.45},
});

constexpr auto choir_pad = std::to_array<Value>({
    {"nova.master", 6.0},
    {"nova.osc.a.position", 0.4},
    {"nova.osc.a.unison.voices", 5.0},
    {"nova.osc.a.unison.detune", 12.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.filter.1.model", 14.0},
    {"nova.filter.1.cutoff", 1'000.0},
    {"nova.filter.1.resonance", 0.35},
    {"nova.env.1.attack", 0.8},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 2.5},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 0.1},
    {"nova.mod.0.amount", 0.15},
    {"nova.fx.1.mix", 0.4},
    {"nova.fx.2.p1", 0.6},
    {"nova.fx.2.mix", 0.4},
});

constexpr auto warm_pad = std::to_array<Value>({
    {"nova.master", -1.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 1.0},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 10.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.level", 0.3},
    {"nova.osc.b.fine", 5.0},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 2'200.0},
    {"nova.env.1.attack", 0.7},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 1.8},
    {"nova.fx.1.mix", 0.4},
    {"nova.fx.2.mix", 0.35},
});

constexpr auto sweep_pad = std::to_array<Value>({
    {"nova.master", 3.5},
    {"nova.osc.a.position", 0.0},
    {"nova.osc.a.unison.voices", 4.0},
    {"nova.osc.a.unison.detune", 14.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.attack", 0.8},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 2.5},
    {"nova.lfo.1.sync", 2.0},
    {"nova.mod.0.amount", 0.7},
    {"nova.fx.1.mix", 0.4},
    {"nova.fx.2.mix", 0.35},
});

// Plucks.
constexpr auto glass_pluck = std::to_array<Value>({
    {"nova.osc.a.position", 0.1},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 8.0},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.decay", 0.5},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.4},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.3},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.5},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.4},
    {"nova.fx.1.p7", 0.5},
    {"nova.fx.1.mix", 0.3},
    {"nova.fx.2.mix", 0.25},
});

constexpr auto trance_pluck = std::to_array<Value>({
    {"nova.master", -0.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 7.0},
    {"nova.osc.a.unison.detune", 26.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 1'300.0},
    {"nova.filter.1.resonance", 0.3},
    {"nova.env.1.decay", 0.3},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.25},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.13},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.5},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.45},
    {"nova.fx.1.p7", 0.5},
    {"nova.fx.1.mix", 0.3},
    {"nova.fx.2.mix", 0.3},
});

constexpr auto marimba = std::to_array<Value>({
    {"nova.master", -4.0},       {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 0.0},   {"nova.osc.a.random_phase", 0.0},
    {"nova.osc.b.on", 1.0},      {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 1.0},   {"nova.osc.b.octave", 2.0},
    {"nova.osc.b.level", 0.0},   {"nova.osc.b.random_phase", 0.0},
    {"nova.filter.1.on", 0.0},   {"nova.env.1.decay", 0.4},
    {"nova.env.1.sustain", 0.0}, {"nova.env.1.release", 0.3},
    {"nova.env.2.attack", 0.0},  {"nova.env.2.decay", 0.06},
    {"nova.env.2.sustain", 0.0}, {"nova.mod.0.amount", 0.5},
    {"nova.fx.1.mix", 0.2},
});

constexpr auto mallet = std::to_array<Value>({
    {"nova.master", 2.5},
    {"nova.osc.a.position", 0.4},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.decay", 0.8},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.6},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.15},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.3},
    {"nova.fx.1.mix", 0.25},
});

constexpr auto lofi_pluck = std::to_array<Value>({
    {"nova.master", -0.5},
    {"nova.osc.a.position", 0.3},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 6.0},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 2'400.0},
    {"nova.env.1.decay", 0.6},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.4},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 0.6},
    {"nova.mod.0.amount", 0.002},
    {"nova.fx.1.p0", 0.25},
    {"nova.fx.1.p7", 8.0 / 11.0},
    {"nova.fx.1.mix", 0.5},
    {"nova.fx.2.mix", 0.25},
});

// Keys.
constexpr auto e_piano = std::to_array<Value>({
    {"nova.master", -3.5},
    {"nova.osc.a.position", 0.05},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 0.0},
    {"nova.osc.b.octave", 1.0},
    {"nova.osc.b.level", 0.2},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.decay", 1.6},
    {"nova.env.1.sustain", 0.25},
    {"nova.env.1.release", 0.4},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.4},
    {"nova.env.2.sustain", 0.0},
    {"nova.env.2.velocity", 0.8},
    {"nova.mod.0.amount", 0.25},
    {"nova.fx.1.mix", 0.35},
    {"nova.fx.2.mix", 0.2},
});

constexpr auto drawbar_organ = std::to_array<Value>({
    {"nova.master", 1.5},
    {"nova.osc.a.position", 0.5},
    {"nova.osc.a.drift", 0.05},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.attack", 0.005},
    {"nova.env.1.release", 0.05},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 6.5},
    {"nova.mod.0.amount", 0.001},
    {"nova.fx.1.mix", 0.4},
    {"nova.fx.2.mix", 0.15},
});

constexpr auto house_stab = std::to_array<Value>({
    {"nova.master", -4.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 4.0},
    {"nova.osc.a.unison.detune", 14.0},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 1'800.0},
    {"nova.filter.1.resonance", 0.2},
    {"nova.env.1.decay", 0.22},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.15},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.15},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.25},
    {"nova.fx.1.p1", 0.55},
    {"nova.fx.1.p7", 2.0 / 3.0},
    {"nova.fx.1.mix", 0.3},
});

constexpr auto detroit_chord = std::to_array<Value>({
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 3.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.fine", 8.0},
    {"nova.osc.b.level", 0.6},
    {"nova.filter.1.model", 3.0},
    {"nova.filter.1.cutoff", 1'200.0},
    {"nova.filter.1.resonance", 0.35},
    {"nova.env.1.decay", 0.35},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.2},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.25},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.3},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.5},
    {"nova.fx.1.p7", 1.0},
    {"nova.fx.1.mix", 0.3},
});

// Growls and effects.
constexpr auto roar = std::to_array<Value>({
    {"nova.master", -3.0},
    {"nova.osc.a.position", 0.3},
    {"nova.osc.a.unison.voices", 3.0},
    {"nova.osc.a.unison.detune", 14.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.45},
    {"nova.filter.1.model", 15.0},
    {"nova.filter.1.cutoff", 600.0},
    {"nova.filter.1.resonance", 0.5},
    {"nova.filter.1.mix", 0.6},
    {"nova.lfo.1.sync", 15.0},
    {"nova.mod.0.amount", 0.5},
    {"nova.global.voicing", 2.0},
    {"nova.fx.1.p0", 0.45},
    {"nova.fx.1.p7", 0.0},
    {"nova.fx.2.p0", 0.6},
});

constexpr auto riser = std::to_array<Value>({
    {"nova.master", -4.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 7.0},
    {"nova.osc.a.unison.detune", 30.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.4},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 200.0},
    {"nova.filter.1.resonance", 0.35},
    {"nova.env.1.attack", 0.05},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 1.5},
    {"nova.env.2.attack", 8.0},
    {"nova.env.2.attack_curve", 0.4},
    {"nova.env.2.sustain", 1.0},
    {"nova.mod.0.amount", 0.6},
    {"nova.mod.1.amount", 0.125},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.55},
    {"nova.fx.1.mix", 0.3},
    {"nova.fx.2.mix", 0.4},
});

constexpr auto downlifter = std::to_array<Value>({
    {"nova.master", -5.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 5.0},
    {"nova.osc.a.unison.detune", 25.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.35},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 400.0},
    {"nova.env.1.decay", 4.0},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 1.0},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 4.0},
    {"nova.env.2.sustain", 0.0},
    {"nova.env.2.decay_curve", 0.0},
    {"nova.mod.0.amount", 0.6},
    {"nova.mod.1.amount", 0.25},
    {"nova.fx.1.mix", 0.45},
});

// Noise FX. The sweeps ride LFO 1 in Env mode, synced to bars, so a note
// held for the LFO's length is exactly one sweep at any tempo; change
// lfo.1.sync to make one longer or shorter. Filtered noise is quiet, so
// the quieter ones make up level in a Utility at the end of the chain.
constexpr auto noise_riser = std::to_array<Value>({
    {"nova.master", 0.0},
    {"nova.osc.a.on", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.2},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 300.0},
    {"nova.filter.1.resonance", 0.25},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.4},
    {"nova.lfo.1.mode", 1.0},
    {"nova.lfo.1.sync", 2.0},
    {"nova.mod.0.amount", 0.56},
    {"nova.mod.1.amount", 0.8},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.45},
    {"nova.fx.1.mix", 0.2},
    {"nova.fx.2.p1", 0.65},
    {"nova.fx.2.mix", 0.3},
    {"nova.fx.3.p0", (24.0 + 7.5) / 48.0},
});

constexpr auto long_riser = std::to_array<Value>({
    {"nova.master", 0.0},
    {"nova.osc.a.on", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.2},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 200.0},
    {"nova.filter.1.resonance", 0.45},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.6},
    {"nova.lfo.1.mode", 1.0},
    {"nova.lfo.1.sync", 1.0},
    {"nova.mod.0.amount", 0.6},
    {"nova.mod.1.amount", 0.8},
    {"nova.mod.2.amount", 0.4},
    {"nova.fx.2.p0", 0.8},
    {"nova.fx.2.p1", 0.7},
    {"nova.fx.2.mix", 0.25},
    {"nova.fx.3.p0", (24.0 + 10.5) / 48.0},
});

constexpr auto band_riser = std::to_array<Value>({
    {"nova.master", 0.0},
    {"nova.osc.a.on", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.4},
    {"nova.filter.1.model", 7.0},
    {"nova.filter.1.cutoff", 250.0},
    {"nova.filter.1.resonance", 0.45},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.3},
    {"nova.lfo.1.mode", 1.0},
    {"nova.lfo.1.sync", 3.0},
    {"nova.mod.0.amount", 0.52},
    {"nova.mod.1.amount", 0.6},
    {"nova.fx.1.p0", 4.0 / 14.0},
    {"nova.fx.1.p1", 0.5},
    {"nova.fx.1.p7", 0.5},
    {"nova.fx.1.mix", 0.3},
    {"nova.fx.2.mix", 0.25},
    {"nova.fx.3.p0", (24.0 + 18.5) / 48.0},
});

constexpr auto gated_riser = std::to_array<Value>({
    {"nova.master", 0.0},
    {"nova.osc.a.on", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.1},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 300.0},
    {"nova.filter.1.resonance", 0.3},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.3},
    {"nova.lfo.1.mode", 1.0},
    {"nova.lfo.1.sync", 2.0},
    {"nova.lfo.2.sync", 15.0},
    {"nova.lfo.2.mode", 2.0},
    {"nova.mod.0.amount", 0.55},
    {"nova.mod.1.amount", 0.9},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.4},
    {"nova.fx.1.mix", 0.2},
    {"nova.fx.2.mix", 0.25},
    {"nova.fx.3.p0", (24.0 + 6.5) / 48.0},
});

constexpr auto noise_drop = std::to_array<Value>({
    {"nova.master", 0.0},
    {"nova.osc.a.on", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.0},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 150.0},
    {"nova.filter.1.resonance", 0.25},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.5},
    {"nova.lfo.1.mode", 1.0},
    {"nova.lfo.1.sync", 3.0},
    {"nova.mod.0.amount", 0.58},
    {"nova.mod.1.amount", 1.0},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.45},
    {"nova.fx.1.mix", 0.2},
    {"nova.fx.2.p1", 0.7},
    {"nova.fx.2.mix", 0.35},
    {"nova.fx.3.p0", (24.0 + 8.5) / 48.0},
});

constexpr auto impact_boom = std::to_array<Value>({
    {"nova.master", 3.6},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 0.0},
    {"nova.osc.a.level", 0.9},
    {"nova.osc.a.random_phase", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.6},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 180.0},
    {"nova.env.1.attack", 0.0},
    {"nova.env.1.decay", 3.0},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 3.0},
    {"nova.env.1.decay_curve", 0.6},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 1.2},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.55},
    {"nova.mod.1.amount", 0.125},
    {"nova.fx.1.p0", 0.7},
    {"nova.fx.1.p1", 0.72},
    {"nova.fx.1.mix", 0.35},
});

constexpr auto sub_drop = std::to_array<Value>({
    {"nova.master", 0.9},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 0.0},
    {"nova.osc.a.level", 1.0},
    {"nova.osc.a.random_phase", 0.0},
    {"nova.osc.a.pitch", -24.0},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.attack", 0.003},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.25},
    {"nova.lfo.1.mode", 1.0},
    {"nova.lfo.1.sync", 4.0},
    {"nova.mod.0.amount", 0.25},
});

constexpr auto reverse_swell = std::to_array<Value>({
    {"nova.master", 4.3},
    {"nova.osc.a.on", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.0},
    {"nova.filter.1.model", 5.0},
    {"nova.filter.1.cutoff", 1'500.0},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.02},
    {"nova.lfo.1.mode", 1.0},
    {"nova.lfo.1.sync", 4.0},
    {"nova.mod.0.amount", 0.3},
    {"nova.mod.1.amount", 1.0},
    {"nova.fx.1.mix", 0.12},
});

constexpr auto wind_sweep = std::to_array<Value>({
    {"nova.master", 0.0},
    {"nova.osc.a.on", 0.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.8},
    {"nova.filter.1.model", 7.0},
    {"nova.filter.1.cutoff", 900.0},
    {"nova.filter.1.resonance", 0.5},
    {"nova.env.1.attack", 1.0},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 2.0},
    {"nova.lfo.1.mode", 2.0},
    {"nova.lfo.1.sync", 3.0},
    {"nova.lfo.1.smooth", 0.3},
    {"nova.mod.0.amount", 0.22},
    {"nova.fx.1.p0", 0.7},
    {"nova.fx.1.p1", 0.7},
    {"nova.fx.1.mix", 0.35},
    {"nova.fx.2.p0", (24.0 + 18.0) / 48.0},
});

constexpr auto metal_hit = std::to_array<Value>({
    {"nova.master", -1.5},
    {"nova.osc.a.position", 0.5},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 30.0},
    {"nova.filter.1.on", 0.0},
    {"nova.env.1.decay", 1.2},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 1.0},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.5},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.4},
    {"nova.fx.1.p1", 0.6},
    {"nova.fx.1.mix", 0.35},
});

// Arps and sequences.
constexpr auto bubble_arp = std::to_array<Value>({
    {"nova.master", 6.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 3.0},
    {"nova.filter.1.model", 7.0},
    {"nova.filter.1.cutoff", 1'200.0},
    {"nova.filter.1.resonance", 0.5},
    {"nova.env.1.decay", 0.15},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.1},
    {"nova.lfo.1.sync", 2.0},
    {"nova.lfo.1.scope", 1.0},
    {"nova.mod.0.amount", 0.3},
    {"nova.arp.on", 1.0},
    {"nova.arp.mode", 2.0},
    {"nova.arp.octaves", 2.0},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.45},
    {"nova.fx.1.p7", 0.5},
    {"nova.fx.1.mix", 0.3},
});

constexpr auto trance_gate = std::to_array<Value>({
    {"nova.master", -5.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 7.0},
    {"nova.osc.a.unison.detune", 22.0},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 3'000.0},
    {"nova.env.1.attack", 0.01},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.3},
    {"nova.lfo.1.sync", 15.0},
    {"nova.lfo.1.scope", 1.0},
    {"nova.mod.0.amount", -1.0},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p7", 0.5},
    {"nova.fx.1.mix", 0.25},
    {"nova.fx.2.mix", 0.3},
});

// Analogue (ADR-0073): warm, moving parts for minimal techno and house, each
// with its own seed so no two share a set of voice cards. Chords sit near
// -9 dB so a four-note stack peaks well under full scale.
constexpr auto detroit_chord_stab = std::to_array<Value>({
    {"nova.master", -9.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 4.0},
    {"nova.osc.b.pw", 0.38},
    {"nova.osc.b.level", 0.5},
    {"nova.osc.b.fine", -4.0},
    {"nova.filter.1.model", 3.0},
    {"nova.filter.1.cutoff", 1'300.0},
    {"nova.filter.1.resonance", 0.3},
    {"nova.filter.1.keytrack", 0.4},
    {"nova.env.1.attack", 0.002},
    {"nova.env.1.decay", 0.32},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.22},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.2},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.32},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.45},
    {"nova.fx.1.p7", 1.0},
    {"nova.fx.1.mix", 0.22},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.45},
    {"nova.analogue.seed", 1'987.0},
    {"nova.analogue.cutoff", 0.6},
    {"nova.analogue.env", 0.6},
    {"nova.analogue.drift.amount", 0.35},
});

constexpr auto warehouse_poly_pad = std::to_array<Value>({
    {"nova.master", -10.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 7.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 4.0},
    {"nova.osc.b.pw", 0.42},
    {"nova.osc.b.level", 0.5},
    {"nova.osc.b.fine", -6.0},
    {"nova.filter.1.model", 0.0},
    {"nova.filter.1.cutoff", 1'100.0},
    {"nova.filter.1.resonance", 0.15},
    {"nova.env.1.attack", 0.9},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 2.2},
    {"nova.lfo.1.sync", 2.0},
    {"nova.mod.0.amount", 0.12},
    {"nova.fx.1.mix", 0.35},
    {"nova.fx.2.mix", 0.3},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.55},
    {"nova.analogue.seed", 808.0},
    {"nova.analogue.cutoff", 0.6},
    {"nova.analogue.drift.amount", 0.6},
    {"nova.analogue.drift.rate", 0.25},
    {"nova.analogue.drift.cutoff", 0.45},
});

// A whispered "ahh": breath (the noise layer) through a formant filter
// singing A, over a faint detuned saw stack through a second one. Slow free
// LFOs lean the vowel towards O, move the voice's size and pan the breath.
constexpr auto whispered_ahh = std::to_array<Value>({
    {"nova.master", -8.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.level", 0.35},
    {"nova.osc.a.unison.voices", 5.0},
    {"nova.osc.a.unison.detune", 16.0},
    {"nova.osc.a.unison.blend", 0.6},
    {"nova.osc.a.unison.width", 1.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.9},
    {"nova.noise.route", 1.0},
    {"nova.filter.routing", 1.0},
    // Formant: Fat picks the vowel (0 is A), cutoff the voice's size.
    {"nova.filter.1.model", 13.0},
    {"nova.filter.1.cutoff", 1'000.0},
    {"nova.filter.1.resonance", 0.35},
    {"nova.filter.1.fat", 0.0},
    {"nova.filter.2.on", 1.0},
    {"nova.filter.2.model", 13.0},
    {"nova.filter.2.cutoff", 1'150.0},
    {"nova.filter.2.resonance", 0.45},
    {"nova.filter.2.fat", 0.02},
    {"nova.env.1.attack", 1.8},
    {"nova.env.1.decay", 1.0},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 3.5},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 0.09},
    {"nova.lfo.1.mode", 2.0},
    {"nova.lfo.1.smooth", 0.7},
    {"nova.lfo.1.scope", 1.0},
    {"nova.lfo.2.sync", 0.0},
    {"nova.lfo.2.rate", 0.13},
    {"nova.lfo.2.mode", 2.0},
    {"nova.lfo.2.smooth", 0.6},
    {"nova.lfo.2.scope", 1.0},
    {"nova.lfo.3.sync", 0.0},
    {"nova.lfo.3.rate", 0.05},
    {"nova.lfo.3.mode", 2.0},
    {"nova.lfo.3.smooth", 0.8},
    {"nova.lfo.3.scope", 1.0},
    {"nova.mod.0.amount", 0.08},
    {"nova.mod.1.amount", 0.12},
    {"nova.mod.2.amount", 0.55},
    {"nova.mod.3.amount", 0.06},
    {"nova.fx.1.mix", 0.3},
    {"nova.fx.2.p0", 0.85},
    {"nova.fx.2.p1", 0.8},
    {"nova.fx.2.p2", 0.3},
    {"nova.fx.2.mix", 0.45},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.55},
    {"nova.analogue.seed", 2112.0},
    {"nova.analogue.pitch", 0.6},
    {"nova.analogue.cutoff", 0.5},
    {"nova.analogue.pan", 0.7},
    {"nova.analogue.drift.amount", 0.5},
    {"nova.analogue.drift.rate", 0.3},
    {"nova.analogue.drift.cutoff", 0.4},
});

// After Undulae's "throw away your analog synths" Serum 2 patch: a legato
// saw bass through a Moog-style 18 dB low-pass with a decaying filter
// envelope, a whisper of flutter and drift, noise that follows the saw's
// own wave, then asymmetric drive, a gentle compressor and a little room.
constexpr auto warm_flutter_bass = std::to_array<Value>({
    {"nova.master", 0.0},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.03},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.1},
    {"nova.noise.am.source", 1.0},
    {"nova.noise.am.amount", 0.85},
    {"nova.noise.am.curve", 0.3},
    {"nova.filter.1.model", 3.0},
    {"nova.filter.1.cutoff", 700.0},
    {"nova.filter.1.resonance", 0.7},
    {"nova.filter.1.drive", 6.0},
    {"nova.env.1.attack", 0.002},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 0.25},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.865},
    {"nova.env.2.sustain", 0.43},
    {"nova.env.2.release", 0.3},
    {"nova.mod.0.amount", 0.45},
    {"nova.fx.1.p0", 0.55},
    {"nova.fx.1.p1", 0.3},
    {"nova.fx.1.p2", 0.6},
    {"nova.fx.1.p7", 7.0 / 11.0},
    {"nova.fx.1.mix", 1.0},
    {"nova.fx.2.p0", 0.75},
    {"nova.fx.2.p1", 0.35},
    {"nova.fx.2.p4", 0.15},
    {"nova.fx.3.p0", 0.55},
    {"nova.fx.3.p1", 0.35},
    {"nova.fx.3.p7", 2.0 / 3.0},
    {"nova.fx.3.mix", 0.28},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.4},
    {"nova.analogue.seed", 6626.0},
    {"nova.analogue.drift.amount", 0.35},
    {"nova.analogue.drift.rate", 0.4},
    {"nova.analogue.flutter.amount", 0.25},
    {"nova.analogue.flutter.tone", 14.6},
});

constexpr auto worn_unison_lead = std::to_array<Value>({
    {"nova.master", -6.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 7.0},
    {"nova.osc.a.unison.detune", 14.0},
    {"nova.osc.a.unison.curve", 4.0},
    {"nova.osc.a.unison.blend", 0.7},
    {"nova.osc.a.unison.width", 0.7},
    {"nova.filter.1.model", 3.0},
    {"nova.filter.1.cutoff", 2'600.0},
    {"nova.filter.1.resonance", 0.25},
    {"nova.env.1.attack", 0.005},
    {"nova.env.1.sustain", 0.8},
    {"nova.env.1.release", 0.3},
    {"nova.global.voicing", 2.0},
    {"nova.global.portamento", 0.04},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.35},
    {"nova.fx.1.mix", 0.18},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.4},
    {"nova.analogue.seed", 303.0},
    {"nova.analogue.scope", 2.0},
    {"nova.analogue.pitch", 0.7},
    {"nova.analogue.drift.amount", 0.45},
});

constexpr auto dub_chord_echo = std::to_array<Value>({
    {"nova.master", -9.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 3.0},
    {"nova.osc.b.level", 0.45},
    {"nova.osc.b.fine", 5.0},
    {"nova.filter.1.model", 0.0},
    {"nova.filter.1.cutoff", 650.0},
    {"nova.filter.1.resonance", 0.35},
    {"nova.env.1.attack", 0.002},
    {"nova.env.1.decay", 0.28},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 0.3},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.16},
    {"nova.env.2.sustain", 0.0},
    {"nova.mod.0.amount", 0.25},
    {"nova.fx.1.p0", 8.0 / 14.0},
    {"nova.fx.1.p1", 0.6},
    {"nova.fx.1.p2", 0.65},
    {"nova.fx.1.p7", 1.0},
    {"nova.fx.1.mix", 0.35},
    {"nova.fx.2.p1", 0.6},
    {"nova.fx.2.mix", 0.22},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.5},
    {"nova.analogue.seed", 5'150.0},
    {"nova.analogue.drift.amount", 0.4},
    {"nova.analogue.drift.cutoff", 0.35},
});

constexpr auto old_card_brass = std::to_array<Value>({
    {"nova.master", -9.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.level", 0.6},
    {"nova.osc.b.fine", 6.0},
    {"nova.filter.1.model", 0.0},
    {"nova.filter.1.cutoff", 600.0},
    {"nova.filter.1.resonance", 0.1},
    {"nova.filter.1.keytrack", 0.5},
    {"nova.env.1.attack", 0.04},
    {"nova.env.1.decay", 0.3},
    {"nova.env.1.sustain", 0.85},
    {"nova.env.1.release", 0.35},
    {"nova.env.2.attack", 0.06},
    {"nova.env.2.decay", 0.5},
    {"nova.env.2.sustain", 0.45},
    {"nova.mod.0.amount", 0.42},
    {"nova.fx.1.mix", 0.2},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.6},
    {"nova.analogue.seed", 1'975.0},
    {"nova.analogue.cutoff", 0.6},
    {"nova.analogue.env", 0.7},
    {"nova.analogue.drift.amount", 0.5},
});

// ---- Pads for the flute ---------------------------------------------------------------
// Beds for Wren playing Hijaz and Shur phrases in C minor: breath, santur
// and a sub. Each leaves the space to the track's reverb, so the in-synth
// effects stay small.

// Route amounts are in a destination's knob travel (08a §3.1). These turn
// musical depths into that travel for the destinations the pads modulate.
constexpr double pitch_route_span_cents = 9'600.0;   // nova.osc.<o>.pitch: −48..48 st.
constexpr double cutoff_route_span_octaves = 11.425; // nova.filter.<f>.cutoff: log2(22000 / 8).
constexpr double cents_of_travel(double cents) { return cents / pitch_route_span_cents; }
constexpr double octaves_of_travel(double octaves) { return octaves / cutoff_route_span_octaves; }
// A filter keytracked at 100 % sits at its cutoff on middle C (key 60), so
// a comb or band-pass set here follows each note's fundamental.
constexpr double middle_c_hz = 261.63;
// The delay module's division index (08a §5) as its p0: 1/8 dotted.
constexpr double delay_divisions = 14.0;
constexpr double eighth_dotted = 8.0 / delay_divisions;

// Breath an octave under the flute, like a ney: breath noise through a
// band-pass keyed to each note's fundamental, so the air itself is pitched,
// beside a second, wide band for the hiss on top; a faint sine and triangle
// under it, a slow swell in the breath and a late, small vibrato.
constexpr auto ney_breath = std::to_array<Value>({
    {"nova.master", -1.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 0.0},
    {"nova.osc.a.level", 0.3},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 1.0},
    {"nova.osc.b.level", 0.08},
    {"nova.osc.b.fine", 6.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 1.0},
    {"nova.noise.tone", -0.2},
    {"nova.filter.routing", 1.0},
    {"nova.filter.1.model", 7.0},
    {"nova.filter.1.cutoff", middle_c_hz},
    {"nova.filter.1.resonance", 0.45},
    {"nova.filter.1.keytrack", 1.0},
    {"nova.filter.2.on", 1.0},
    {"nova.filter.2.model", 7.0},
    {"nova.filter.2.cutoff", 2'500.0},
    {"nova.filter.2.resonance", 0.2},
    {"nova.filter.2.keytrack", 0.5},
    {"nova.filter.2.mix", 0.5},
    {"nova.env.1.attack", 1.4},
    {"nova.env.1.decay", 1.0},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 3.5},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 0.18},
    {"nova.lfo.1.mode", 2.0},
    {"nova.lfo.1.smooth", 0.6},
    {"nova.lfo.2.sync", 0.0},
    {"nova.lfo.2.rate", 4.8},
    {"nova.lfo.2.delay", 0.8},
    {"nova.lfo.2.rise", 1.2},
    {"nova.lfo.3.sync", 0.0},
    {"nova.lfo.3.rate", 0.07},
    {"nova.lfo.3.mode", 2.0},
    {"nova.lfo.3.smooth", 0.6},
    {"nova.mod.0.amount", 0.12},
    {"nova.mod.1.amount", cents_of_travel(9.0)},
    {"nova.mod.2.amount", 0.1},
    {"nova.fx.1.mix", 0.25},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.4},
    {"nova.analogue.seed", 1'384.0},
});

// A santur course: four strings per note, a few cents apart, struck by a
// felt-less hammer. A bright, quickly closing filter and a click of metal
// noise make the strike; a keyed comb and an FM-bell table an octave up
// ring on into a long, glittering tail, with a little ping-pong echo.
constexpr auto santur_bloom = std::to_array<Value>({
    {"nova.master", -1.5},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 2.0},
    {"nova.osc.a.unison.voices", 4.0},
    {"nova.osc.a.unison.detune", 7.0},
    {"nova.osc.a.unison.width", 0.8},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.position", 0.3},
    {"nova.osc.b.octave", 1.0},
    {"nova.osc.b.level", 0.3},
    {"nova.osc.b.unison.voices", 3.0},
    {"nova.osc.b.unison.detune", 9.0},
    {"nova.osc.b.unison.width", 1.0},
    {"nova.noise.on", 1.0},
    {"nova.noise.level", 0.0},
    {"nova.noise.tone", 0.4},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 1'800.0},
    {"nova.filter.1.resonance", 0.1},
    {"nova.filter.1.keytrack", 0.6},
    {"nova.filter.2.on", 1.0},
    {"nova.filter.2.model", 15.0},
    {"nova.filter.2.cutoff", middle_c_hz},
    {"nova.filter.2.resonance", 0.75},
    {"nova.filter.2.keytrack", 1.0},
    {"nova.filter.2.mix", 0.4},
    {"nova.env.1.attack", 0.001},
    {"nova.env.1.decay", 6.0},
    {"nova.env.1.sustain", 0.0},
    {"nova.env.1.release", 6.0},
    {"nova.env.1.velocity", 0.8},
    {"nova.env.2.attack", 0.0},
    {"nova.env.2.decay", 0.35},
    {"nova.env.2.sustain", 0.15},
    {"nova.env.2.release", 3.0},
    {"nova.env.3.attack", 0.0},
    {"nova.env.3.decay", 0.04},
    {"nova.env.3.sustain", 0.0},
    {"nova.env.3.release", 0.04},
    {"nova.mod.0.amount", octaves_of_travel(4.0)},
    {"nova.mod.1.amount", 0.5},
    {"nova.fx.1.mix", 0.2},
    {"nova.fx.2.p0", eighth_dotted},
    {"nova.fx.2.p1", 0.35},
    {"nova.fx.2.p2", 0.3},
    {"nova.fx.2.mix", 0.18},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.25},
    {"nova.analogue.seed", 3'030.0},
});

// A warm floor under everything: a soft triangle pair, a hint of saw and a
// sine sub an octave down, through a gently driven low-pass that breathes
// over a long cycle, with the lows kept mono.
constexpr auto warm_sub_bed = std::to_array<Value>({
    {"nova.master", -4.0},
    {"nova.osc.a.type", 0.0},
    {"nova.osc.a.shape", 1.0},
    {"nova.osc.a.level", 0.6},
    {"nova.osc.a.unison.voices", 2.0},
    {"nova.osc.a.unison.detune", 6.0},
    {"nova.osc.a.unison.width", 0.3},
    {"nova.osc.b.on", 1.0},
    {"nova.osc.b.type", 0.0},
    {"nova.osc.b.shape", 2.0},
    {"nova.osc.b.level", 0.18},
    {"nova.osc.b.fine", 4.0},
    {"nova.sub.on", 1.0},
    {"nova.sub.level", 0.6},
    {"nova.filter.1.model", 6.0},
    {"nova.filter.1.cutoff", 420.0},
    {"nova.filter.1.resonance", 0.1},
    {"nova.filter.1.keytrack", 0.3},
    {"nova.filter.1.drive", 3.0},
    {"nova.env.1.attack", 1.5},
    {"nova.env.1.decay", 1.0},
    {"nova.env.1.sustain", 1.0},
    {"nova.env.1.release", 3.5},
    {"nova.lfo.1.sync", 0.0},
    {"nova.lfo.1.rate", 0.05},
    {"nova.lfo.1.mode", 2.0},
    {"nova.lfo.1.scope", 1.0},
    {"nova.lfo.1.smooth", 0.5},
    {"nova.mod.0.amount", octaves_of_travel(0.4)},
    // Utility: bass mono below about 120 Hz (p2, log 20..500 Hz).
    {"nova.fx.1.p2", 0.557},
    {"nova.analogue.on", 1.0},
    {"nova.analogue.age", 0.35},
    {"nova.analogue.seed", 909.0},
    {"nova.analogue.drift.amount", 0.4},
});

// ---- Tags -------------------------------------------------------------------------------
// A small shared vocabulary: character, then genre. Categories already say
// what a preset is for, so no tag repeats one.
using Tags = std::span<const std::string_view>;
constexpr auto t_init = std::to_array<std::string_view>({"clean"});
constexpr auto t_glass_reese = std::to_array<std::string_view>({"reese", "dark", "wide", "dnb"});
constexpr auto t_supersaw = std::to_array<std::string_view>({"bright", "wide", "trance"});
constexpr auto t_wobble = std::to_array<std::string_view>({"aggressive", "wobble", "dubstep"});
constexpr auto t_analog_pad = std::to_array<std::string_view>({"warm", "analog", "evolving"});
constexpr auto t_pluck = std::to_array<std::string_view>({"short", "analog", "house"});
constexpr auto t_fm_bell = std::to_array<std::string_view>({"metallic", "digital"});
constexpr auto t_deep_sub = std::to_array<std::string_view>({"mono", "clean"});
constexpr auto t_hoover = std::to_array<std::string_view>({"wide", "aggressive", "rave"});
constexpr auto t_arp_sequence = std::to_array<std::string_view>({"short", "analog"});
constexpr auto t_rolling_reese = std::to_array<std::string_view>({"reese", "dark", "mono", "dnb"});
constexpr auto t_acid = std::to_array<std::string_view>({"acid", "mono", "techno"});
constexpr auto t_organ_bass = std::to_array<std::string_view>({"short", "house"});
constexpr auto t_analog_bass = std::to_array<std::string_view>({"warm", "analog", "mono"});
constexpr auto t_fm_bass = std::to_array<std::string_view>({"digital", "mono", "dubstep"});
constexpr auto t_rumble = std::to_array<std::string_view>({"dark", "techno"});
constexpr auto t_neuro = std::to_array<std::string_view>({"aggressive", "digital", "dnb"});
constexpr auto t_talking = std::to_array<std::string_view>({"vocal", "wobble", "dubstep"});
constexpr auto t_offbeat = std::to_array<std::string_view>({"short", "mono", "trance"});
constexpr auto t_sync_lead = std::to_array<std::string_view>({"bright", "mono"});
constexpr auto t_chip = std::to_array<std::string_view>({"digital", "mono"});
constexpr auto t_mono_saw = std::to_array<std::string_view>({"analog", "mono", "synthwave"});
constexpr auto t_screamer = std::to_array<std::string_view>({"aggressive", "bright", "rave"});
constexpr auto t_whistle = std::to_array<std::string_view>({"soft", "clean", "mono"});
constexpr auto t_formant_lead = std::to_array<std::string_view>({"vocal", "evolving"});
constexpr auto t_glass_pad = std::to_array<std::string_view>({"bright", "wide", "ambient"});
constexpr auto t_strings = std::to_array<std::string_view>({"dark", "wide"});
constexpr auto t_spectral = std::to_array<std::string_view>({"evolving", "wide", "ambient"});
constexpr auto t_choir = std::to_array<std::string_view>({"vocal", "soft", "ambient"});
constexpr auto t_warm_pad = std::to_array<std::string_view>({"warm", "soft", "analog"});
constexpr auto t_sweep_pad = std::to_array<std::string_view>({"evolving", "wide", "trance"});
constexpr auto t_glass_pluck = std::to_array<std::string_view>({"bright", "short", "digital"});
constexpr auto t_trance_pluck = std::to_array<std::string_view>({"short", "wide", "trance"});
constexpr auto t_marimba = std::to_array<std::string_view>({"percussive", "short", "clean"});
constexpr auto t_mallet = std::to_array<std::string_view>({"percussive", "metallic", "short"});
constexpr auto t_lofi = std::to_array<std::string_view>({"lofi", "warm", "short"});
constexpr auto t_e_piano = std::to_array<std::string_view>({"warm", "soft"});
constexpr auto t_organ = std::to_array<std::string_view>({"house"});
constexpr auto t_stab = std::to_array<std::string_view>({"short", "house"});
constexpr auto t_detroit = std::to_array<std::string_view>({"short", "analog", "techno"});
constexpr auto t_roar = std::to_array<std::string_view>({"aggressive", "dubstep"});
constexpr auto t_riser = std::to_array<std::string_view>({"wide", "riser"});
constexpr auto t_downlifter = std::to_array<std::string_view>({"wide", "drop"});
constexpr auto t_noise_riser = std::to_array<std::string_view>({"noise", "riser"});
constexpr auto t_band_riser = std::to_array<std::string_view>({"noise", "riser", "wide"});
constexpr auto t_noise_drop = std::to_array<std::string_view>({"noise", "drop"});
constexpr auto t_impact = std::to_array<std::string_view>({"noise", "drop", "percussive"});
constexpr auto t_sub_drop = std::to_array<std::string_view>({"drop", "clean"});
constexpr auto t_swell = std::to_array<std::string_view>({"noise", "riser", "short"});
constexpr auto t_wind = std::to_array<std::string_view>({"noise", "ambient", "evolving"});
constexpr auto t_metal_hit = std::to_array<std::string_view>({"metallic", "percussive"});
constexpr auto t_bubble = std::to_array<std::string_view>({"bright"});
constexpr auto t_gate = std::to_array<std::string_view>({"wide", "trance"});
constexpr auto t_detroit_stab = std::to_array<std::string_view>({"short", "analog", "techno"});
constexpr auto t_warehouse = std::to_array<std::string_view>({"warm", "analog", "evolving"});
constexpr auto t_whisper = std::to_array<std::string_view>({"airy", "vocal", "analog"});
constexpr auto t_flutter_bass = std::to_array<std::string_view>({"warm", "analog", "bass"});
constexpr auto t_worn_lead = std::to_array<std::string_view>({"analog", "mono", "wide"});
constexpr auto t_dub_chord = std::to_array<std::string_view>({"analog", "dark", "techno"});
constexpr auto t_brass = std::to_array<std::string_view>({"warm", "analog"});
constexpr auto t_ney = std::to_array<std::string_view>({"airy", "noise", "soft"});
constexpr auto t_santur = std::to_array<std::string_view>({"metallic", "percussive", "bright"});
constexpr auto t_sub_bed = std::to_array<std::string_view>({"warm", "soft", "bass"});

// ---- Documents --------------------------------------------------------------------------

NativeMediaSlot table(std::string slot, std::string name) {
  return {std::move(slot), NativeBuiltinMedia{std::move(name)}, 2'048};
}

NativeModRoute route(std::uint32_t slot, std::string source, std::string destination,
                     bool bipolar = false) {
  return {slot, std::move(source), "", std::move(destination), bipolar, 0.0, 1.0};
}

std::vector<NativeLabel> macros(std::initializer_list<std::string_view> names) {
  std::vector<NativeLabel> labels;
  std::size_t index = 1;
  for (const auto name : names)
    labels.push_back({"macro." + std::to_string(index++), std::string(name)});
  return labels;
}

NativeSynthDocument base() {
  NativeSynthDocument document;
  document.media = {table("osc.a", "basic_shapes"),
                    table("osc.b", "basic_shapes"),
                    table("osc.c", "basic_shapes"),
                    {"noise", NativeBuiltinMedia{"white"}, 0}};
  return document;
}

// An LFO that rises and falls once per cycle, as a wobble does.
NativeCurve wobble_curve(std::string id) {
  return {std::move(id),
          {{0.0, 0.0, 0.0, NativeCurveSegment::curve},
           {0.1, 1.0, 0.0, NativeCurveSegment::curve},
           {0.5, 0.2, 0.5, NativeCurveSegment::curve},
           {1.0, 0.0, 0.0, NativeCurveSegment::curve}}};
}

// Open for the first half of the cycle, shut for the second: a gate.
NativeCurve gate_curve(std::string id) {
  return {std::move(id),
          {{0.0, 0.0, 0.0, NativeCurveSegment::step},
           {0.5, 1.0, 0.0, NativeCurveSegment::step},
           {1.0, 1.0, 0.0, NativeCurveSegment::curve}}};
}

NativeSynthDocument plain() { return base(); }

NativeSynthDocument glass_reese_document() {
  auto document = base();
  document.media[0] = table("osc.a", "reese_saw");
  document.media[1] = table("osc.b", "glass_formant");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position", true),
                     route(1, "lfo.1", "nova.filter.1.cutoff", true),
                     route(2, "macro.1", "nova.osc.a.warp.1.amount")};
  document.curves = {{"lfo.1",
                      {{0.0, 0.5, 0.0, NativeCurveSegment::curve},
                       {0.25, 1.0, 0.3, NativeCurveSegment::curve},
                       {0.75, 0.0, -0.3, NativeCurveSegment::curve},
                       {1.0, 0.5, 0.0, NativeCurveSegment::curve}}}};
  document.labels = macros({"Bend"});
  return document;
}

NativeSynthDocument supersaw_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "delay"}, {3, "reverb"}}}};
  document.labels = macros({"Bright", "Space"});
  return document;
}

NativeSynthDocument wobble_document() {
  auto document = base();
  document.media[0] = table("osc.a", "grit_fold");
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff"),
                     route(1, "lfo.1", "nova.osc.a.position"),
                     route(2, "macro.1", "nova.osc.a.warp.1.amount")};
  document.curves = {wobble_curve("lfo.1")};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "multiband"}}}};
  document.labels = macros({"Growl"});
  return document;
}

NativeSynthDocument analog_pad_document() {
  auto document = base();
  document.media[0] = table("osc.a", "analog_pulse");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position", true),
                     route(1, "lfo.2", "nova.filter.1.cutoff", true)};
  document.curves = {{"lfo.1",
                      {{0.0, 0.0, 0.0, NativeCurveSegment::curve},
                       {0.5, 1.0, 0.0, NativeCurveSegment::curve},
                       {1.0, 0.0, 0.0, NativeCurveSegment::curve}}}};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  document.labels = macros({"Motion", "Air"});
  return document;
}

NativeSynthDocument filter_env_delay_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "delay"}}}};
  return document;
}

NativeSynthDocument fm_bell_document() {
  auto document = base();
  document.media[0] = table("osc.a", "fm_bell");
  document.routes = {route(0, "env.2", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

NativeSynthDocument hoover_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.osc.a.pitch")};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument rolling_reese_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff", true)};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "utility"}}}};
  document.labels = macros({"Grit"});
  return document;
}

NativeSynthDocument acid_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff"),
                     route(1, "macro.1", "nova.filter.1.resonance"),
                     route(2, "macro.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "delay"}}}};
  document.labels = macros({"Squelch", "Cutoff"});
  return document;
}

NativeSynthDocument organ_bass_document() {
  auto document = base();
  document.media[0] = table("osc.a", "organ_drawbars");
  return document;
}

NativeSynthDocument filter_env_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  return document;
}

NativeSynthDocument fm_bass_document() {
  auto document = base();
  document.media[0] = table("osc.a", "fm_bark");
  document.routes = {route(0, "env.2", "nova.osc.a.position")};
  return document;
}

NativeSynthDocument rumble_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "reverb"}, {2, "utility"}}}};
  return document;
}

NativeSynthDocument neuro_document() {
  auto document = base();
  document.media[0] = table("osc.a", "growl_saw");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position"),
                     route(1, "lfo.1", "nova.filter.1.cutoff"),
                     route(2, "macro.1", "nova.osc.a.warp.1.amount")};
  document.curves = {wobble_curve("lfo.1")};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "multiband"}}}};
  document.labels = macros({"FM"});
  return document;
}

NativeSynthDocument talking_document() {
  auto document = base();
  document.media[0] = table("osc.a", "talk_box");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position"),
                     route(1, "lfo.1", "nova.filter.1.cutoff", true)};
  document.curves = {wobble_curve("lfo.1")};
  document.chains = {{"fx.main", {{1, "distortion"}}}};
  return document;
}

NativeSynthDocument sync_lead_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.osc.a.sync")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument chip_document() {
  auto document = base();
  document.media[0] = table("osc.a", "chiptune");
  document.routes = {route(0, "lfo.1", "nova.osc.a.pitch", true)};
  document.chains = {{"fx.main", {{1, "delay"}}}};
  return document;
}

NativeSynthDocument mono_saw_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument screamer_document() {
  auto document = base();
  document.media[0] = table("osc.a", "sync_saw");
  document.routes = {route(0, "lfo.1", "nova.osc.a.pitch", true)};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument vibrato_reverb_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.osc.a.pitch", true)};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

NativeSynthDocument formant_lead_document() {
  auto document = base();
  document.media[0] = table("osc.a", "vowel_morph");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument glass_pad_document() {
  auto document = base();
  document.media[0] = table("osc.a", "glass_formant");
  document.media[1] = table("osc.b", "glass_formant");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position", true)};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument strings_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff", true)};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument spectral_document() {
  auto document = base();
  document.media[0] = table("osc.a", "spectral_pad");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  document.labels = macros({"Drift"});
  return document;
}

NativeSynthDocument choir_document() {
  auto document = base();
  document.media[0] = table("osc.a", "formant_yoi");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position", true)};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument chorus_reverb_document() {
  auto document = base();
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument sweep_pad_document() {
  auto document = base();
  document.media[0] = table("osc.a", "harmonic_sweep");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "phaser"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument glass_pluck_document() {
  auto document = base();
  document.media[0] = table("osc.a", "fm_bell");
  document.routes = {route(0, "env.2", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument trance_pluck_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument marimba_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.osc.b.level")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

NativeSynthDocument mallet_document() {
  auto document = base();
  document.media[0] = table("osc.a", "metal_clang");
  document.routes = {route(0, "env.2", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

NativeSynthDocument lofi_document() {
  auto document = base();
  document.media[0] = table("osc.a", "pluck_damp");
  document.routes = {route(0, "lfo.1", "nova.osc.a.pitch", true)};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument e_piano_document() {
  auto document = base();
  document.media[0] = table("osc.a", "fm_bell");
  document.routes = {route(0, "env.2", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument organ_document() {
  auto document = base();
  document.media[0] = table("osc.a", "organ_drawbars");
  document.routes = {route(0, "lfo.1", "nova.osc.a.pitch", true)};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument stab_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

NativeSynthDocument detroit_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "delay"}}}};
  return document;
}

NativeSynthDocument roar_document() {
  auto document = base();
  document.media[0] = table("osc.a", "roar");
  document.routes = {route(0, "lfo.1", "nova.osc.a.position")};
  document.curves = {wobble_curve("lfo.1")};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "multiband"}}}};
  return document;
}

NativeSynthDocument riser_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff"),
                     route(1, "env.2", "nova.osc.a.pitch")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument downlifter_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff"),
                     route(1, "env.2", "nova.osc.a.pitch")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

// LFO 1 as a one-way ramp for an Env-mode sweep, from `from` to `to`;
// tension above 0 holds back and rushes at the end, as a riser does.
NativeCurve ramp_curve(std::string id, double from, double to, double tension) {
  return {
      std::move(id),
      {{0.0, from, tension, NativeCurveSegment::curve}, {1.0, to, 0.0, NativeCurveSegment::curve}}};
}

// A riser: the ramp opens the filter (slot 0) and raises the noise (slot 1).
NativeSynthDocument noise_riser_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff"),
                     route(1, "lfo.1", "nova.noise.level")};
  document.curves = {ramp_curve("lfo.1", 0.0, 1.0, 0.3)};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}, {3, "utility"}}}};
  return document;
}

// Pink noise, and the reverb grows with the sweep (slot 2).
NativeSynthDocument long_riser_document() {
  auto document = base();
  document.media[3] = {"noise", NativeBuiltinMedia{"pink"}, 0};
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff"),
                     route(1, "lfo.1", "nova.noise.level"), route(2, "lfo.1", "nova.fx.2.mix")};
  document.curves = {ramp_curve("lfo.1", 0.0, 1.0, 0.4)};
  document.chains = {{"fx.main", {{1, "phaser"}, {2, "reverb"}, {3, "utility"}}}};
  return document;
}

NativeSynthDocument band_riser_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff"),
                     route(1, "lfo.1", "nova.noise.level")};
  document.curves = {ramp_curve("lfo.1", 0.0, 1.0, 0.3)};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}, {3, "utility"}}}};
  return document;
}

// The level ramp is chopped by LFO 2's sixteenth gate (the route's aux).
NativeSynthDocument gated_riser_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff"),
                     {1, "lfo.1", "lfo.2", "nova.noise.level", false, 0.0, 1.0}};
  document.curves = {ramp_curve("lfo.1", 0.0, 1.0, 0.3), gate_curve("lfo.2")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}, {3, "utility"}}}};
  return document;
}

// A riser run backwards: bright and loud at the downbeat, closing to nothing.
NativeSynthDocument noise_drop_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff"),
                     route(1, "lfo.1", "nova.noise.level")};
  document.curves = {ramp_curve("lfo.1", 1.0, 0.0, 0.0)};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}, {3, "utility"}}}};
  return document;
}

// A noise crash closing down (slot 0) over a sine that falls an octave
// into the note (slot 1).
NativeSynthDocument impact_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff"),
                     route(1, "env.2", "nova.osc.a.pitch")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

// The pitch starts on the note and falls two octaves over a bar.
NativeSynthDocument sub_drop_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.osc.a.pitch")};
  document.curves = {ramp_curve("lfo.1", 1.0, 0.0, -0.3)};
  return document;
}

// A reverse cymbal: shimmer noise swelling into the downbeat, cut dead.
NativeSynthDocument reverse_swell_document() {
  auto document = base();
  document.media[3] = {"noise", NativeBuiltinMedia{"metal_shimmer"}, 0};
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff"),
                     route(1, "lfo.1", "nova.noise.level")};
  document.curves = {ramp_curve("lfo.1", 0.0, 1.0, 0.7)};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

// Wind through a band-pass that drifts up and down every two bars.
NativeSynthDocument wind_document() {
  auto document = base();
  document.media[3] = {"noise", NativeBuiltinMedia{"wind"}, 0};
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff", true)};
  document.curves = {{"lfo.1",
                      {{0.0, 0.0, 0.0, NativeCurveSegment::curve},
                       {0.5, 1.0, 0.0, NativeCurveSegment::curve},
                       {1.0, 0.0, 0.0, NativeCurveSegment::curve}}}};
  document.chains = {{"fx.main", {{1, "reverb"}, {2, "utility"}}}};
  return document;
}

NativeSynthDocument metal_hit_document() {
  auto document = base();
  document.media[0] = table("osc.a", "metal_fold");
  document.routes = {route(0, "env.2", "nova.osc.a.position")};
  document.chains = {{"fx.main", {{1, "reverb"}}}};
  return document;
}

NativeSynthDocument bubble_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff", true)};
  document.chains = {{"fx.main", {{1, "delay"}}}};
  return document;
}

NativeSynthDocument gate_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.osc.a.level")};
  document.curves = {gate_curve("lfo.1")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

// Analogue presets' documents.
NativeSynthDocument detroit_stab_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "delay"}}}};
  return document;
}

// A slow LFO opens the filter a little; drift does the rest.
NativeSynthDocument warehouse_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff", true)};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument whispered_document() {
  auto document = base();
  document.routes = {
      route(0, "lfo.1", "nova.filter.2.fat", true), route(1, "lfo.3", "nova.filter.2.cutoff", true),
      route(2, "lfo.2", "nova.noise.pan", true), route(3, "lfo.1", "nova.filter.1.fat", true)};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument warm_flutter_bass_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "distortion"}, {2, "compressor"}, {3, "reverb"}}}};
  return document;
}

NativeSynthDocument worn_lead_document() {
  auto document = base();
  document.chains = {{"fx.main", {{1, "delay"}}}};
  return document;
}

// Dotted-eighth tape echo into a dark reverb.
NativeSynthDocument dub_chord_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "delay"}, {2, "reverb"}}}};
  return document;
}

NativeSynthDocument brass_document() {
  auto document = base();
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff")};
  document.chains = {{"fx.main", {{1, "chorus"}}}};
  return document;
}

// Pads for the flute's documents.
NativeSynthDocument ney_document() {
  auto document = base();
  document.media[3] = {"noise", NativeBuiltinMedia{"breath"}, 0};
  document.routes = {route(0, "lfo.1", "nova.noise.level", true),
                     route(1, "lfo.2", "nova.osc.a.pitch", true),
                     route(2, "lfo.3", "nova.filter.1.resonance", true)};
  document.chains = {{"fx.main", {{1, "chorus"}}}};
  return document;
}

NativeSynthDocument santur_document() {
  auto document = base();
  document.media[1] = table("osc.b", "fm_bell");
  document.media[3] = {"noise", NativeBuiltinMedia{"metal_shimmer"}, 0};
  document.routes = {route(0, "env.2", "nova.filter.1.cutoff"),
                     route(1, "env.3", "nova.noise.level")};
  document.chains = {{"fx.main", {{1, "chorus"}, {2, "delay"}}}};
  return document;
}

NativeSynthDocument sub_bed_document() {
  auto document = base();
  document.routes = {route(0, "lfo.1", "nova.filter.1.cutoff", true)};
  document.chains = {{"fx.main", {{1, "utility"}}}};
  return document;
}

// A factory preset: its values, tags and document.
struct PresetSpec {
  std::string_view name;
  std::string_view category;
  std::span<const Value> values;
  Tags tags;
  NativeSynthDocument (*document)();
};

// Init first: a new Nova starts from it. Then by category.
const std::array preset_specs{
    PresetSpec{"Init", "Init", init, t_init, plain},
    PresetSpec{"Glass Reese", "Bass", glass_reese, t_glass_reese, glass_reese_document},
    PresetSpec{"Supersaw Lead", "Lead", supersaw_lead, t_supersaw, supersaw_document},
    PresetSpec{"Wobble Growl", "Growl", wobble_growl, t_wobble, wobble_document},
    PresetSpec{"Analog Pad", "Pad", analog_pad, t_analog_pad, analog_pad_document},
    PresetSpec{"Pluck", "Pluck", pluck, t_pluck, filter_env_delay_document},
    PresetSpec{"FM Bell", "Keys", fm_bell, t_fm_bell, fm_bell_document},
    PresetSpec{"Deep Sub", "Bass", deep_sub, t_deep_sub, plain},
    PresetSpec{"Hoover", "Lead", hoover, t_hoover, hoover_document},
    PresetSpec{"Arp Sequence", "Arp/Seq", arp_sequence, t_arp_sequence, filter_env_delay_document},
    PresetSpec{"Rolling Reese", "Bass", rolling_reese, t_rolling_reese, rolling_reese_document},
    PresetSpec{"Acid Line", "Bass", acid_line, t_acid, acid_document},
    PresetSpec{"Organ Bass", "Bass", organ_bass, t_organ_bass, organ_bass_document},
    PresetSpec{"Analog Bass", "Bass", analog_bass, t_analog_bass, filter_env_document},
    PresetSpec{"FM Bass", "Bass", fm_bass, t_fm_bass, fm_bass_document},
    PresetSpec{"Techno Rumble", "Bass", techno_rumble, t_rumble, rumble_document},
    PresetSpec{"Neuro Bass", "Bass", neuro_bass, t_neuro, neuro_document},
    PresetSpec{"Talking Bass", "Bass", talking_bass, t_talking, talking_document},
    PresetSpec{"Offbeat Bass", "Bass", offbeat_bass, t_offbeat, filter_env_document},
    PresetSpec{"Sync Lead", "Lead", sync_lead, t_sync_lead, sync_lead_document},
    PresetSpec{"Chip Lead", "Lead", chip_lead, t_chip, chip_document},
    PresetSpec{"Mono Saw Lead", "Lead", mono_saw_lead, t_mono_saw, mono_saw_document},
    PresetSpec{"Screamer", "Lead", screamer, t_screamer, screamer_document},
    PresetSpec{"Whistle", "Lead", whistle, t_whistle, vibrato_reverb_document},
    PresetSpec{"Formant Lead", "Lead", formant_lead, t_formant_lead, formant_lead_document},
    PresetSpec{"Glass Pad", "Pad", glass_pad, t_glass_pad, glass_pad_document},
    PresetSpec{"Dark Strings", "Pad", dark_strings, t_strings, strings_document},
    PresetSpec{"Spectral Drift", "Pad", spectral_drift, t_spectral, spectral_document},
    PresetSpec{"Choir Pad", "Pad", choir_pad, t_choir, choir_document},
    PresetSpec{"Warm Pad", "Pad", warm_pad, t_warm_pad, chorus_reverb_document},
    PresetSpec{"Sweep Pad", "Pad", sweep_pad, t_sweep_pad, sweep_pad_document},
    PresetSpec{"Glass Pluck", "Pluck", glass_pluck, t_glass_pluck, glass_pluck_document},
    PresetSpec{"Trance Pluck", "Pluck", trance_pluck, t_trance_pluck, trance_pluck_document},
    PresetSpec{"Marimba", "Pluck", marimba, t_marimba, marimba_document},
    PresetSpec{"Mallet", "Pluck", mallet, t_mallet, mallet_document},
    PresetSpec{"Lofi Pluck", "Pluck", lofi_pluck, t_lofi, lofi_document},
    PresetSpec{"E-Piano", "Keys", e_piano, t_e_piano, e_piano_document},
    PresetSpec{"Drawbar Organ", "Keys", drawbar_organ, t_organ, organ_document},
    PresetSpec{"House Stab", "Keys", house_stab, t_stab, stab_document},
    PresetSpec{"Detroit Chord", "Keys", detroit_chord, t_detroit, detroit_document},
    PresetSpec{"Roar", "Growl", roar, t_roar, roar_document},
    PresetSpec{"Riser", "FX", riser, t_riser, riser_document},
    PresetSpec{"Downlifter", "FX", downlifter, t_downlifter, downlifter_document},
    PresetSpec{"Metal Hit", "FX", metal_hit, t_metal_hit, metal_hit_document},
    PresetSpec{"Noise Riser", "FX", noise_riser, t_noise_riser, noise_riser_document},
    PresetSpec{"Long Noise Riser", "FX", long_riser, t_noise_riser, long_riser_document},
    PresetSpec{"Band Riser", "FX", band_riser, t_band_riser, band_riser_document},
    PresetSpec{"Gated Riser", "FX", gated_riser, t_noise_riser, gated_riser_document},
    PresetSpec{"Reverse Swell", "FX", reverse_swell, t_swell, reverse_swell_document},
    PresetSpec{"Noise Drop", "FX", noise_drop, t_noise_drop, noise_drop_document},
    PresetSpec{"Impact Boom", "FX", impact_boom, t_impact, impact_document},
    PresetSpec{"Sub Drop", "FX", sub_drop, t_sub_drop, sub_drop_document},
    PresetSpec{"Wind Sweep", "FX", wind_sweep, t_wind, wind_document},
    PresetSpec{"Bubble Arp", "Arp/Seq", bubble_arp, t_bubble, bubble_document},
    PresetSpec{"Trance Gate", "Arp/Seq", trance_gate, t_gate, gate_document},
    // Analogue (ADR-0073).
    PresetSpec{"Detroit Chord Stab", "Keys", detroit_chord_stab, t_detroit_stab,
               detroit_stab_document},
    PresetSpec{"Warehouse Poly Pad", "Pad", warehouse_poly_pad, t_warehouse, warehouse_document},
    PresetSpec{"Worn Unison Lead", "Lead", worn_unison_lead, t_worn_lead, worn_lead_document},
    PresetSpec{"Dub Chord Echo", "Keys", dub_chord_echo, t_dub_chord, dub_chord_document},
    PresetSpec{"Old Card Brass", "Keys", old_card_brass, t_brass, brass_document},
    PresetSpec{"Whispered Ahh", "Pad", whispered_ahh, t_whisper, whispered_document},
    PresetSpec{"Warm Flutter Bass", "Bass", warm_flutter_bass, t_flutter_bass,
               warm_flutter_bass_document},
    // Pads for the flute.
    PresetSpec{"Ney Breath", "Pad", ney_breath, t_ney, ney_document},
    PresetSpec{"Santur Bloom", "Pluck", santur_bloom, t_santur, santur_document},
    PresetSpec{"Warm Sub Bed", "Pad", warm_sub_bed, t_sub_bed, sub_bed_document},
};

} // namespace

// The presets with every placed effect's contract defaults (08a §5) filled
// in under the preset's own values, so a preset names only what it changes.
std::span<const SynthPreset> factory_presets() noexcept {
  struct Built {
    std::deque<std::string> ids;
    std::vector<std::vector<Value>> values;
    std::vector<SynthPreset> presets;
  };
  static const Built built = [] {
    Built b;
    b.values.resize(preset_specs.size());
    for (std::size_t i = 0; i < preset_specs.size(); ++i) {
      const auto& spec = preset_specs[i];
      auto& values = b.values[i];
      values.assign(spec.values.begin(), spec.values.end());
      const auto named = [&](std::string_view id) {
        return std::ranges::any_of(spec.values, [&](const Value& v) { return v.first == id; });
      };
      for (const auto& chain : spec.document().chains) {
        for (const auto& slot : chain.modules) {
          const auto kind = fx_kind_from_id(slot.module);
          if (!kind)
            continue;
          const auto defaults = fx_defaults(*kind);
          const auto prefix = "nova.fx." + std::to_string(slot.uid) + ".";
          for (std::size_t k = 0; k < defaults.p.size(); ++k) {
            b.ids.push_back(prefix + "p" + std::to_string(k));
            if (!named(b.ids.back()))
              values.emplace_back(b.ids.back(), defaults.p[k]);
          }
          b.ids.push_back(prefix + "mix");
          if (!named(b.ids.back()))
            values.emplace_back(b.ids.back(), defaults.mix);
        }
      }
      b.presets.push_back({spec.name, spec.category, values, spec.tags});
    }
    return b;
  }();
  return built.presets;
}

NativeSynthDocument preset_document(std::optional<std::size_t> preset) {
  if (!preset || *preset >= preset_specs.size())
    return base();
  return preset_specs[*preset].document();
}

} // namespace oscillate::native_dsp::synth::nova
