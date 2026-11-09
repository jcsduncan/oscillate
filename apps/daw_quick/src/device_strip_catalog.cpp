#include "device_strip_catalog.hpp"

#include <algorithm>
#include <cstddef>

namespace oscillate::quick {
namespace {
// Synths first, then effects, each with its accent colour.
constexpr std::array styles{
    DeviceStripStyle{
        "prism",
        "Synth",
        "#8fa6ff",
        {"prism.filter.cutoff", "prism.filter.resonance", "prism.amp.attack", "prism.amp.release"}},
    DeviceStripStyle{
        "ember",
        "Analog synth",
        "#ff8f6b",
        {"ember.filter.cutoff", "ember.filter.resonance", "ember.env.attack", "ember.env.release"}},
    DeviceStripStyle{
        "pulse",
        "Drum synth",
        "#f5b14c",
        {"pulse.bus.glue", "pulse.bus.saturation", "pulse.bus.release", "pulse.out.level"}},
    DeviceStripStyle{"anvil",
                     "Kick synth",
                     "#ff7a2f",
                     {"anvil.length", "anvil.tune", "anvil.rumble.amount", "anvil.drive.amount"}},
    DeviceStripStyle{"knock",
                     "Modelled percussion",
                     "#ff7a59",
                     {"knock.decay", "knock.room.size", "knock.room.level", "knock.out.level"}},
    DeviceStripStyle{"halo",
                     "FM synth",
                     "#c49bff",
                     {"halo.brightness", "halo.feedback", "halo.filter.cutoff", "halo.fx.delay"}},
    DeviceStripStyle{"torque",
                     "Bass synth",
                     "#ff6b6b",
                     {"torque.cutoff", "torque.resonance", "torque.envmod", "torque.decay"}},
    DeviceStripStyle{
        "drift",
        "Granular synth",
        "#8be3b4",
        {"drift.grain.size", "drift.grain.density", "drift.filter.cutoff", "drift.blur"}},
    DeviceStripStyle{
        "fret",
        "Bass guitar",
        "#4ea1ff",
        {"fret.feel.amount", "fret.feel.dynamics", "fret.tone.amp", "fret.tone.squash"}},
    DeviceStripStyle{
        "wren",
        "Flute",
        "#7fd6c2",
        {"wren.breath.amount", "wren.breath.vibrato", "wren.tone.air", "wren.tone.space"}},
    DeviceStripStyle{
        "bow", "Strings", "#e0a96d", {"bow.perf.wheel", "bow.vibrato", "bow.tone", "bow.space"}},
    DeviceStripStyle{"concert",
                     "Piano",
                     "#e8d9b8",
                     {"concert.tone", "concert.dynamics", "concert.resonance", "concert.width"}},
    DeviceStripStyle{"nova",
                     "Wavetable synth",
                     "#46d6ff",
                     {"nova.macro.1", "nova.macro.2", "nova.macro.3", "nova.macro.4"}},
    DeviceStripStyle{"contour",
                     "Filter",
                     "#ff7aa8",
                     {"contour.cutoff", "contour.resonance", "contour.drive", "contour.mix"}},
    DeviceStripStyle{"forge",
                     "Saturation",
                     "#ff9d5c",
                     {"forge.drive", "forge.tone", "forge.mix", "forge.output"}},
    DeviceStripStyle{"orbit",
                     "Modulation",
                     "#b48cff",
                     {"orbit.rate", "orbit.depth", "orbit.feedback", "orbit.mix"}},
    DeviceStripStyle{"relay",
                     "Delay",
                     "#ffb36b",
                     {"relay.time", "relay.feedback", "relay.mod_depth", "relay.mix"}},
    DeviceStripStyle{"expanse",
                     "Reverb",
                     "#7fb8ff",
                     {"expanse.size", "expanse.decay", "expanse.low_cut", "expanse.mix"}},
    DeviceStripStyle{"vista",
                     "Stereo",
                     "#6bd4ff",
                     {"vista.width", "vista.mono_freq", "vista.stereoize", "vista.output"}},
    DeviceStripStyle{
        "deck", "DJ EQ", "#ff8fd0", {"deck.low", "deck.mid", "deck.high", "deck.gain"}},
    DeviceStripStyle{"pocket",
                     "Kick & bass",
                     "#5fe0bf",
                     {"pocket.depth", "pocket.focus_low", "pocket.release", "pocket.mono_freq"}},
    DeviceStripStyle{
        "pump", "Sidechain", "#ff7a6b", {"pump.depth", "pump.nudge", "pump.anticlick", "pump.mix"}},
    DeviceStripStyle{
        "chop", "Gate", "#ff6b57", {"chop.depth", "chop.smooth", "chop.swing", "chop.mix"}},
    DeviceStripStyle{"lens",
                     "Analyser",
                     "#9be36b",
                     {"lens.tilt", "lens.smoothing", "lens.hold_decay", "lens.average"}},
    DeviceStripStyle{
        "loudness",
        "Meter",
        "#6be3d8",
        {"loudness.target", "loudness.ceiling", "loudness.integrate", "loudness.scale"}},
    DeviceStripStyle{"scope",
                     "Oscilloscope",
                     "#d0e36b",
                     {"scope.length", "scope.time", "scope.channel", "scope.align_band"}},
    DeviceStripStyle{
        "tune", "Tuner", "#f5d36b", {"tune.reference", "tune.mode", "tune.notation", ""}},
    DeviceStripStyle{
        "trim", "Utility", "#b9c3cf", {"trim.gain", "trim.pan", "trim.width", "trim.law"}},
    DeviceStripStyle{
        "shape", "EQ", "#7ee0ff", {"shape.gain_2", "shape.gain_4", "shape.gain_5", "shape.gain_7"}},
    DeviceStripStyle{"press",
                     "Compressor",
                     "#ffcf6b",
                     {"press.threshold", "press.ratio", "press.attack", "press.makeup"}},
    DeviceStripStyle{"temper",
                     "Resonance",
                     "#ff8a3d",
                     {"temper.depth", "temper.detail", "temper.max_cut", "temper.mix"}},
    DeviceStripStyle{"strata",
                     "Multiband",
                     "#ff9f5a",
                     {"strata.ott.depth", "strata.mix", "strata.input", "strata.output"}},
    DeviceStripStyle{
        "flyby", "Spatial", "#f2a541", {"flyby.speed", "flyby.doppler", "flyby.room", "flyby.mix"}},
    DeviceStripStyle{"dust",
                     "Texture",
                     "#c8a36a",
                     {"dust.mix.bed", "dust.vinyl.crackle", "dust.age.amount", "dust.duck.amount"}},
    // Oversampling is the latency: not a strip control.
    DeviceStripStyle{
        "edge", "Clipper", "#f08a5d", {"edge.input", "edge.shape", "edge.output", "edge.link"}},
    // Lookahead is the latency: not a strip control.
    DeviceStripStyle{
        "ceiling", "Limiter", "#ff8a8a", {"ceiling.ceiling", "ceiling.release", "", ""}},
    DeviceStripStyle{
        "signal",
        "Generator",
        "#ff6b9d",
        {"signal.frequency", "signal.level", "signal.sweep_seconds", "signal.impulse_rate"}},
};

constexpr DeviceStripStyle neutral{"", "Device", "#9aa6b5", {"", "", "", ""}};

} // namespace

const DeviceStripStyle& device_strip_style(std::string_view engine_id) noexcept {
  const auto found = std::ranges::find(styles, engine_id, &DeviceStripStyle::engine);
  return found == styles.end() ? neutral : *found;
}

std::vector<std::string> device_strip_parameters(const native_dsp::EngineDescriptor& engine) {
  constexpr std::size_t slots = 4;
  std::vector<std::string> result;
  const auto taken = [&](std::string_view id) {
    return std::ranges::find(result, id) != result.end();
  };
  for (const auto id : device_strip_style(engine.id).parameters) {
    if (!id.empty() && native_dsp::find_parameter(engine, id) && !taken(id))
      result.emplace_back(id);
  }
  for (const auto& parameter : engine.parameters) {
    if (result.size() >= slots)
      break;
    if (parameter.automatable && !native_dsp::is_discrete(parameter) &&
        parameter.timing == native_dsp::ParameterTiming::live && !taken(parameter.id))
      result.emplace_back(parameter.id);
  }
  if (result.size() > slots)
    result.resize(slots);
  return result;
}

} // namespace oscillate::quick
