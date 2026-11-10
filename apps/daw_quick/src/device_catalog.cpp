#include "device_catalog.hpp"

#include <algorithm>
#include <array>
#include <iterator>

namespace oscillate::quick {
namespace {

using S = DeviceSection;

// Listing order: by section, then the one reached for most first. A blurb
// says what sets the device apart; the kind beside it already says what it is.
constexpr std::array looks{
    // ---- Instruments ------------------------------------------------------
    DeviceLook{"nova", "Instruments", S::synths, "Wavetable synth", "Wt", "#46d6ff",
               "The flagship: huge unison and drag-to-modulate", "",
               "The flagship: three oscillators (wavetable, classic, sample, granular, spectral) "
               "with up to sixteen-voice unison and warps, two filters, drawable LFOs, drag-to-"
               "modulate on every knob and a reorderable effects rack.",
               "serum flagship lead pad bass pluck unison modern"},
    DeviceLook{"prism", "Instruments", S::synths, "Polysynth", "Ps", "#8fa6ff",
               "Leads, pads and bass with a modulation matrix", "",
               "A modern polyphonic synth: oscillators with unison, a choice of filters and a "
               "modulation matrix. Wide leads, deep pads and solid bass.",
               "subtractive synth poly lead pad bass"},
    DeviceLook{"ember", "Instruments", S::synths, "Analog polysynth", "An", "#ff8f6b",
               "Warm, drifting vintage poly with its classic chorus", "",
               "A warm, drifting vintage polysynth with its classic chorus. Strings, brass and "
               "soft keys.",
               "analogue juno prophet vintage warm chorus strings brass"},
    DeviceLook{"halo", "Instruments", S::synths, "FM synth", "Fm", "#c49bff",
               "Bells, keys and evolving tones", "",
               "Bells, keys and evolving tones from frequency modulation.",
               "dx7 frequency modulation bell electric piano keys"},
    DeviceLook{"drift", "Instruments", S::synths, "Granular synth", "Gr", "#9be36b",
               "Clouds and shifting textures from grains", "",
               "Clouds and shifting textures from grains of sound.",
               "grains texture ambient cloud pad atmosphere"},
    DeviceLook{"torque", "Instruments", S::synths, "Bass synth", "Bs", "#ff6b6b",
               "Sequenced low end and distortion", "",
               "Sequenced low end and distortion, with a step sequencer.",
               "303 acid bassline sequencer sub low end"},
    DeviceLook{"concert", "Instruments", S::keys, "Piano", "Pn", "#e8d9b8",
               "A sampled grand with real pedals", "",
               "A sampled Yamaha C5 grand (Salamander) with sixteen velocity layers, half-pedal, "
               "sostenuto, una corda and sympathetic resonance.",
               "grand piano keys acoustic sampled salamander felt"},
    DeviceLook{"pulse", "Drums", S::drums, "Drum machine", "Dm", "#f5b14c",
               "Synthesised kicks, snares, hats and toms", "",
               "Synthesised drums: designed kicks, snares and percussion, every hit tunable.",
               "808 909 drum kit kick snare clap hat hihat tom cowbell"},
    DeviceLook{"anvil", "Drums", S::drums, "Kick synth", "Kk", "#ff7a2f",
               "Draw the pitch, amp and drive over the kick", "",
               "One kick, drawn: pitch, amp and drive curves over its waveform, a click layer, a "
               "sub, and a tuned techno rumble that ducks under every hit.",
               "kick bass drum techno rumble sub"},
    DeviceLook{"knock", "Drums", S::drums, "Percussion", "Pc", "#ff7a59",
               "Modelled knocks, taps and thuds on wood, skin, metal or glass", "",
               "Eight pads of physically modelled percussion: a striker, a resonator from a "
               "pitched note to a knock on a thing, a coupled second resonator and a body, with a "
               "finish for tight, modern hits. Velocity changes the tone, not only the level.",
               "perc percussion knock tap click foley conga bongo shaker rim top loop minimal"},
    DeviceLook{"stick", "Drums", S::drums, "Acoustic drums", "Ak", "#e0a96d",
               "A sampled acoustic kit, every hit on every mic", "",
               "Three multi-mic acoustic kits from DrumGizmo.org on the General MIDI map. "
               "Velocity picks the hit by its recorded power with round robins; the hi-hat pedal "
               "opens the hat; the mic mixer sets each mic, with or without the bleed between "
               "them; each piece has its own tune, decay, level, pan and sends.",
               "drums acoustic kit sampled drumkit kick snare hihat toms cymbals ride brushes "
               "funk break room"},
    DeviceLook{"crate", "Instruments", S::samplers, "Sampler", "Sm", "#d6c38f",
               "Chop a sample onto pads, with vintage character", "",
               "An MPC-style chopper: slice a sample automatically, play the chops from pads and "
               "keys, and colour them with vintage sampler eras.",
               "sampler chop slice sample mpc sp1200 one-shot loop"},
    DeviceLook{"fret", "Instruments", S::samplers, "Bass guitar", "Bg", "#4ea1ff",
               "A sampled fingered bass that plays your notes like a bassist", "",
               "A sampled five-string fingered bass (Black and Blue Basses) with round robins, "
               "ghost notes, staccato, behind-the-bridge plucks and string releases. Feel picks "
               "each note's velocity and articulation as it plays, so flat notes groove.",
               "bass guitar electric fingered sampled funk ghost notes slap groove"},
    DeviceLook{"bow", "Instruments", S::samplers, "Strings", "St", "#e0a96d",
               "Sampled string sections and solos, bowed live from the mod wheel", "",
               "Sampled strings (VSCO 2 CE sections, University of Iowa MIS solos): violins, "
               "violas, cellos, basses or the whole ensemble, sustain, legato, staccato, "
               "pizzicato and tremolo on keyswitches. The mod wheel bows the dynamics live; Tone "
               "runs from sul tasto to sul ponticello; Ensemble layers more players.",
               "strings violin viola cello bass orchestra section pad legato pizzicato tremolo "
               "sampled ensemble"},
    DeviceLook{"wren", "Instruments", S::winds, "Flute", "Fu", "#7fd6c2",
               "A sampled concert flute that breathes your notes like a flautist", "",
               "A sampled concert flute (University of Iowa MIS) in three dynamics, with and "
               "without vibrato. Breath plays flat notes like a flautist as they sound: vibrato "
               "that blooms on long notes, swells, slurs, t-k tonguing and grace notes from the "
               "project's key, in a small hall.",
               "flute woodwind wind sampled lyrical folk vibrato breath lead melody"},
    // ---- Effects ------------------------------------------------------------
    DeviceLook{"press", "Effects", S::dynamics, "Compressor", "Cp", "#ffcf6b",
               "Soft knee, keyable from a sidechain",
               "Soft-knee compressor, keyable from a sidechain", "",
               "comp compression dynamics glue squash punch sidechain bus"},
    DeviceLook{"ceiling", "Effects", S::dynamics, "Limiter", "Lm", "#ff8a8a",
               "True-peak limiter for the master",
               "True-peak mastering limiter: onsets kept, oversampled clip, loudness target", "",
               "limit limiting brickwall maximiser maximizer loud master ceiling true peak lufs "
               "loudness transient clip"},
    DeviceLook{"edge", "Effects", S::dynamics, "Clipper", "Cl", "#f08a5d",
               "Clips peaks under a 0 dBFS ceiling, oversampled",
               "Clipper for drums and the master: hard to soft, up to 16x, never over 0 dBTP", "",
               "clip clipper clipping soft hard loud loudness drums kick 808 master true peak"},
    DeviceLook{"hush", "Effects", S::dynamics, "Gate", "Gt", "#b49cff",
               "Gate and expander with lookahead and a filtered key",
               "Gate and expander: hysteresis, hold, lookahead, a filtered or external key", "",
               "gate noise gate expander bleed drums kick snare tom hum duck gated reverb "
               "sidechain key"},
    DeviceLook{"strike", "Effects", S::dynamics, "Transient shaper", "Ts", "#d4f55b",
               "Attack and sustain, single-band or in up to four bands",
               "Transient shaper: attack and sustain at any level, up to four bands, clip", "",
               "transient shaper attack sustain punch snap drums kick snare room tighten "
               "multiband impact master"},
    DeviceLook{"pump", "Effects", S::dynamics, "Sidechain shaper", "Sc", "#ff7a6b",
               "Draw the duck over the kick",
               "Draw the duck over the kick: tempo or kick-triggered", "",
               "sidechain duck ducking pump volume shaper kickstart lfo tool"},
    DeviceLook{"chop", "Effects", S::dynamics, "Gate & stutter", "Gt", "#ff6b57",
               "Tempo-locked gate and stutter for chopping bass",
               "Step gate with ratchets, beat repeats and seeded chance on the song's grid", "",
               "gate stutter chop trance repeat glitch ratchet trancegate beat repeat sequencer "
               "volume step"},
    DeviceLook{"pocket", "Effects", S::dynamics, "Kick & bass carver", "Kb", "#5fe0bf",
               "Makes room for the kick in the bass", "Carves the bass around the kick, keyed", "",
               "sidechain duck low end kick bass dynamic eq"},
    DeviceLook{"temper", "Effects", S::dynamics, "Resonance suppressor", "Rs", "#ff8a3d",
               "Tames harsh resonances, with dynamic EQ bands",
               "Tames harsh resonances; dynamic bands and sidechain unmasking", "",
               "soothe dynamic eq harsh resonance de-ess deesser tame unmask"},
    DeviceLook{"strata", "Effects", S::dynamics, "Multiband dynamics", "Mb", "#ff9f5a",
               "Up to six bands, compressing or lifting each",
               "Multiband compressor and expander, upward and down, with OTT", "",
               "multiband multi-band ott compressor expander upward dynamics bus master "
               "pro-mb glue de-mud"},
    DeviceLook{"shape", "Effects", S::eq_filter, "EQ", "Eq", "#7ee0ff",
               "Parametric, up to eight bands", "Eight-band parametric EQ: shelves, bells and cuts",
               "", "equaliser equalizer parametric bell shelf cut tone"},
    DeviceLook{"keel", "Effects", S::eq_filter, "Tonal balance", "Tb", "#8fe3b0",
               "Steers the mix's balance toward a target",
               "Adaptive tonal balance: toward a tilt, a drawn or learned curve, or a reference",
               "",
               "tonal balance eq equaliser equalizer match master mastering target curve "
               "reference tilt spectrum learn"},
    DeviceLook{"contour", "Effects", S::eq_filter, "Filter", "Fl", "#ff7aa8",
               "Ladder, state-variable and comb, with motion",
               "Ladder, SVF, OTA and comb filters with motion", "",
               "lowpass highpass bandpass ladder moog sweep comb resonance"},
    DeviceLook{"deck", "Effects", S::eq_filter, "DJ EQ", "Dj", "#ff8fd0",
               "A DJ mixer's three bands with kills",
               "Three-band isolator with full kills, for mixing", "",
               "isolator kill three band dj mixer"},
    DeviceLook{"forge", "Effects", S::drive, "Saturation", "Sa", "#ff9d5c",
               "Tape, tube, fuzz, fold and crush", "Tape, tube, fuzz, fold and crush, oversampled",
               "", "distortion drive overdrive fuzz tape tube bitcrush crush warm grit"},
    DeviceLook{"expanse", "Effects", S::space, "Reverb", "Rv", "#7fb8ff",
               "Small rooms to cathedrals, shimmer and freeze",
               "Rooms to vast clouds, shimmer and freeze", "",
               "verb room hall plate space ambience shimmer"},
    DeviceLook{"cast", "Effects", S::space, "Convolution reverb", "Cv", "#f0b86e",
               "Real spaces, plates and springs, or your own IR",
               "Convolution reverb: modelled rooms, halls, plates and a spring, or your own IR", "",
               "convolution impulse response ir reverb verb room hall plate spring chamber "
               "cathedral space ambience true stereo"},
    DeviceLook{"relay", "Effects", S::space, "Delay", "Dl", "#ffb36b",
               "Tape, analog and digital echoes", "Digital, tape and analog echoes, tempo-synced",
               "", "echo tape ping pong dub repeat"},
    DeviceLook{"vista", "Effects", S::space, "Stereo width", "St", "#7ee0c3",
               "Width and imaging, mono-safe lows", "Multiband width, mono-safe lows and depth", "",
               "stereo imager widener mono width"},
    DeviceLook{"flyby", "Effects", S::space, "3D panner", "3D", "#f2a541",
               "Fly a sound past your head with Doppler",
               "Moves a sound past your head with Doppler, distance and ear cues", "",
               "spatial doppler pan panning movement binaural fly by"},
    DeviceLook{"dust", "Effects", S::texture, "Texture & vinyl", "Ds", "#c8a36a",
               "Vinyl crackle, tape hiss, rain and room tone under anything",
               "Eight seeded noise layers that duck under the kick, and age for the input", "",
               "vinyl crackle dust noise hiss tape lofi lo-fi rain wind ambience atmosphere room "
               "tone hum static radio texture bed background"},
    DeviceLook{"orbit", "Effects", S::modulation, "Chorus · flanger · phaser", "Ch", "#b6a3ff",
               "Chorus, ensemble, flanger and phaser", "Chorus, ensemble, flanger and phaser", "",
               "modulation chorus ensemble flanger phaser"},
    // ---- Utilities ----------------------------------------------------------
    DeviceLook{"lens", "Utilities", S::metering, "Spectrum analyser", "Sp", "#9be36b",
               "Spectrum and stereo, with a reference overlay",
               "Multi-resolution spectrum with reference overlay", "",
               "analyzer analyser spectrum fft frequency"},
    DeviceLook{"loudness", "Utilities", S::metering, "Loudness meter", "Lu", "#6be3d8",
               "LUFS and true peak", "EBU R128 LUFS, LRA and true peak", "",
               "lufs meter metering true peak r128"},
    DeviceLook{"scope", "Utilities", S::metering, "Oscilloscope", "Os", "#8fd3ff",
               "Waveforms and kick/bass timing", "Beat-synced scope and kick/bass alignment", "",
               "scope waveform phase timing alignment"},
    DeviceLook{"tune", "Utilities", S::metering, "Tuner", "Tu", "#ffd36b",
               "For instruments, bass and kicks", "Strobe tuner with a kick pitch tracker", "",
               "tuner pitch strobe"},
    DeviceLook{"trim", "Utilities", S::tools, "Gain & pan", "Gn", "#b9c3cf",
               "Gain, pan, width and polarity", "Gain, pan laws, width, mono, polarity, solo", "",
               "utility volume gain pan polarity phase mono width"},
    DeviceLook{"signal", "Utilities", S::tools, "Test tone", "Tg", "#ff9db8",
               "Tones, sweeps and noise", "Sine, sweep, pink and white noise, impulses", "",
               "generator signal sine sweep noise oscillator"},
};

static_assert(std::ranges::is_sorted(looks, {}, &DeviceLook::section),
              "looks are listed section by section");

constexpr std::array section_names{"Synths",      "Keys",       "Drums & percussion",
                                   "Samplers",    "Winds",      "Dynamics",
                                   "EQ & filter", "Drive",      "Space",
                                   "Texture",     "Modulation", "Metering & analysis",
                                   "Tools"};
static_assert(section_names.size() == static_cast<std::size_t>(DeviceSection::tools) + 1);

} // namespace

std::span<const DeviceLook> device_looks() noexcept { return looks; }

const DeviceLook* device_look(std::string_view id) noexcept {
  const auto found = std::ranges::find(looks, id, &DeviceLook::id);
  return found == looks.end() ? nullptr : &*found;
}

std::string_view section_name(DeviceSection section) noexcept {
  return section_names[static_cast<std::size_t>(section)];
}

bool is_utility(const DeviceLook& look) noexcept { return look.category == "Utilities"; }

std::optional<std::size_t> device_rank(std::string_view id) noexcept {
  const auto found = std::ranges::find(looks, id, &DeviceLook::id);
  if (found == looks.end())
    return std::nullopt;
  return static_cast<std::size_t>(std::distance(looks.begin(), found));
}

} // namespace oscillate::quick
