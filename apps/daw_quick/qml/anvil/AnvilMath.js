.pragma library
.import Oscillate.Contracts 1.0 as Law
.import Oscillate.Format 1.0 as Fmt

// Anvil's document curves (anvil.hpp): `pitch`, `amp` and `drive`, each
// x 0..1 of the kick's length. Pitch is stored 0..1 on a logarithmic scale
// from pitchMinHz to pitchMaxHz; amp and drive are plain 0..1.
var pitchMinHz = 20
var pitchMaxHz = 2000

function hzOfY(y) { return pitchMinHz * Math.pow(pitchMaxHz / pitchMinHz, Math.max(0, Math.min(1, y))) }
function yOfHz(hz) {
    return Math.max(0, Math.min(1, Math.log(Math.max(pitchMinHz, hz) / pitchMinHz)
                                     / Math.log(pitchMaxHz / pitchMinHz)))
}

function noteOf(hz) {
    if (!(hz > 0))
        return { midi: -1, name: "—", cents: 0 }
    var m = Law.Contracts.midi_of_hz(hz)
    var r = Math.round(m)
    return { midi: r, name: Law.Contracts.note_name(r, Law.Contracts.sharp, Law.Contracts.scientific_middle_c),
             cents: Math.round((m - r) * 100) }
}
function hzText(hz) {
    return Fmt.Format.hertz(hz)
}
function inScale(midi, scale) {
    return scale.length > 0 && scale.indexOf(((midi % 12) + 12) % 12) >= 0
}
// The tune (semitones) that moves a tail at `tailHz`, already tuned by
// `tune`, onto the nearest note of `scale` (pitch classes); `tune` itself
// when there is no scale.
function tuneToKey(tailHz, tune, scale) {
    if (!(tailHz > 0) || scale.length === 0)
        return tune
    var m = Law.Contracts.midi_of_hz(tailHz)
    var best = null
    for (var c = Math.floor(m) - 7; c <= Math.ceil(m) + 7; ++c) {
        if (!inScale(c, scale))
            continue
        if (best === null || Math.abs(c - m) < Math.abs(best - m))
            best = c
    }
    return best === null ? tune : tune + (best - m)
}
