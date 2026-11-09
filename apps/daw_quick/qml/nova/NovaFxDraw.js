.pragma library
.import "NovaContract.js" as Contract

// The FX strips' displays (NOVA-UI-11): one painter per module visual,
// drawn from the slot's values and its live telemetry (contract §6). `m` is
// {visual, module, p[16], mix, mode, modeCount, synced, live, inDb, outDb,
// value, detail[3], spectrum[48] | null}; `t` is the Nova theme. Every
// curve is drawn once wide and faint, then thin: a glow without a blur.

// The display's level scale for spectra, dBFS at the top and bottom.
var spectrumTopDb = 0
var spectrumFloorDb = -96
// The EQ and filter scales, dB at the top (and minus it at the bottom).
var eqViewDb = 24
var filterTopDb = 24
var filterBottomDb = -36
// Pixels between a curve's samples (each EQ band's own curve is coarser),
// and the samples round a closed or swept path.
var sampleStep = 2
var bandSampleStep = 3
var pathSamples = 64
// The background grids' columns.
var gridColumns = 4
var dynamicsGridColumns = 6

function x01(f) { return Contract.displayFraction(f) }
function yOf(dbValue, top, bottom, H) { return (top - dbValue) / (top - bottom) * H }

function grid(ctx, t, W, H, columns) {
    ctx.strokeStyle = t.gridLine
    ctx.lineWidth = t.hairline
    for (let i = 1; i < columns; ++i) {
        ctx.beginPath()
        ctx.moveTo(Math.round(i * W / columns) + t.hairline / 2, 0)
        ctx.lineTo(Math.round(i * W / columns) + t.hairline / 2, H)
        ctx.stroke()
    }
}
// Octave lines at 100 Hz, 1 kHz and 10 kHz on the 20 Hz..20 kHz axis.
function frequencyGrid(ctx, t, W, H) {
    ctx.strokeStyle = t.gridLine
    ctx.lineWidth = t.hairline
    const marks = [100, 1000, 10000]
    for (let i = 0; i < marks.length; ++i) {
        const x = Math.round(x01(marks[i]) * W) + t.hairline / 2
        ctx.beginPath()
        ctx.moveTo(x, 0)
        ctx.lineTo(x, H)
        ctx.stroke()
    }
}
function glow(ctx, t, color) {
    ctx.strokeStyle = t.soft(color, t.glowAlpha)
    ctx.lineWidth = t.fxGlowWidth
    ctx.stroke()
    ctx.strokeStyle = color
    ctx.lineWidth = t.fxLineWidth
    ctx.stroke()
}
function spectrum(ctx, t, W, H, m) {
    if (m.spectrum === null)
        return
    ctx.beginPath()
    ctx.moveTo(0, H)
    for (let i = 0; i < m.spectrum.length; ++i) {
        const x = x01(Contract.spectrumHz(i)) * W
        const level = Math.max(spectrumFloorDb, Math.min(spectrumTopDb, m.spectrum[i]))
        ctx.lineTo(x, yOf(level, spectrumTopDb, spectrumFloorDb, H))
    }
    ctx.lineTo(W, H)
    ctx.closePath()
    ctx.fillStyle = t.soft(t.pk, t.fxSpectrumAlpha)
    ctx.fill()
}
function label(ctx, t, text, x, y, align) {
    ctx.fillStyle = t.dim
    ctx.font = t.typeSmall + "px \"" + t.mono + "\""
    ctx.textAlign = align || "left"
    ctx.fillText(text, x, y)
    ctx.textAlign = "left"
}
function dot(ctx, t, x, y, color) {
    ctx.beginPath()
    ctx.fillStyle = color
    ctx.arc(x, y, t.fxDotRadius, 0, Math.PI * 2)
    ctx.fill()
}
function linear(dbValue) { return dbValue <= Contract.floorDb ? 0 : Math.pow(10, dbValue / 20) }

// ---- Painters ---------------------------------------------------------------------------

// The shaper's transfer curve, with the live input level on it, at this
// share of the height either side of the middle.
var transferAmplitude = 0.42
function shaper(mode, drive, bias, x) {
    const u = drive * x + bias
    switch (mode) {
    case 2: return Math.max(-1, Math.min(1, u))                         // Hard
    case 4: case 6: return Math.sin(u * Math.PI / 2)                    // Fold, Sine Fold
    case 5: return Math.abs(Math.tanh(u)) * 2 - 1                       // Rectify
    case 7: return u > 0 ? Math.tanh(u) : Math.tanh(0.5 * u)            // Asym
    case 8: return Math.round(Math.tanh(u) * 6) / 6                     // Bit Crush
    case 9: return Math.tanh(u)                                         // Downsample
    case 10: return Math.tanh(u + 0.4 * Math.sin(3 * u))                // X-Shaper
    default: return Math.tanh(u)                                        // Tube, Soft, Diode, Overdrive
    }
}
function transfer(ctx, t, W, H, m) {
    grid(ctx, t, W, H, gridColumns)
    const drive = Math.pow(10, 48 * m.p[0] / 20 / 2)
    const bias = (2 * m.p[2] - 1) * 0.3
    const centre = shaper(m.mode, drive, bias, 0)
    const curve = x => (shaper(m.mode, drive, bias, x) - centre) / (1 + Math.abs(centre))
    ctx.beginPath()
    for (let i = 0; i <= W; i += sampleStep) {
        const y = H / 2 - curve(i / W * 2 - 1) * H * transferAmplitude
        if (i === 0) ctx.moveTo(i, y); else ctx.lineTo(i, y)
    }
    glow(ctx, t, t.pk)
    if (m.live) {
        const x = Math.min(1, linear(m.inDb))
        dot(ctx, t, W / 2 + x * W / 2, H / 2 - curve(x) * H * transferAmplitude, t.am)
    }
}

// The filter's response over its output spectrum; the node is draggable.
function response(ctx, t, W, H, m) {
    spectrum(ctx, t, W, H, m)
    frequencyGrid(ctx, t, W, H)
    const fc = Contract.expo(8, 22000)(m.p[0])
    ctx.beginPath()
    for (let i = 0; i <= W; i += sampleStep) {
        const d = Math.max(filterBottomDb, Math.min(filterTopDb, Contract.filterDb(m.mode, Contract.displayHz(i / W), fc, m.p[1])))
        const y = yOf(d, filterTopDb, filterBottomDb, H)
        if (i === 0) ctx.moveTo(i, y); else ctx.lineTo(i, y)
    }
    glow(ctx, t, t.pk)
}

// The EQ: each band's curve faintly, the sum bright, over the spectrum.
function eqSum(m, f) {
    let sum = 0
    for (let b = 0; b < Contract.eqBands; ++b) {
        const type = Contract.eqTypeIndex(m.p[4 * b])
        if (type !== Contract.eqType.off)
            sum += Contract.eqBandDb(type, Contract.eqHz(m.p[4 * b + 1]), Contract.eqDb(m.p[4 * b + 2]),
                                     Contract.eqQ(m.p[4 * b + 3]), f)
    }
    return sum
}
function eq(ctx, t, W, H, m) {
    spectrum(ctx, t, W, H, m)
    frequencyGrid(ctx, t, W, H)
    ctx.strokeStyle = t.gridLine2
    ctx.beginPath()
    ctx.moveTo(0, H / 2)
    ctx.lineTo(W, H / 2)
    ctx.stroke()
    for (let b = 0; b < Contract.eqBands; ++b) {
        const type = Contract.eqTypeIndex(m.p[4 * b])
        if (type === Contract.eqType.off)
            continue
        ctx.beginPath()
        for (let i = 0; i <= W; i += bandSampleStep) {
            const d = Contract.eqBandDb(type, Contract.eqHz(m.p[4 * b + 1]), Contract.eqDb(m.p[4 * b + 2]),
                                        Contract.eqQ(m.p[4 * b + 3]), Contract.displayHz(i / W))
            const y = yOf(Math.max(-eqViewDb, Math.min(eqViewDb, d)), eqViewDb, -eqViewDb, H)
            if (i === 0) ctx.moveTo(i, y); else ctx.lineTo(i, y)
        }
        ctx.strokeStyle = t.soft(t.vi, t.fxBandCurveAlpha)
        ctx.lineWidth = t.hairline
        ctx.stroke()
    }
    ctx.beginPath()
    for (let i = 0; i <= W; i += sampleStep) {
        const d = Math.max(-eqViewDb, Math.min(eqViewDb, eqSum(m, Contract.displayHz(i / W))))
        const y = yOf(d, eqViewDb, -eqViewDb, H)
        if (i === 0) ctx.moveTo(i, y); else ctx.lineTo(i, y)
    }
    glow(ctx, t, t.pk)
}
// Where band b's node sits, as {x, y} fractions of the display.
function eqNode(m, b) {
    const type = Contract.eqTypeIndex(m.p[4 * b])
    const f = Contract.eqHz(m.p[4 * b + 1])
    const d = Contract.eqHasGain(type) ? Contract.eqDb(m.p[4 * b + 2]) : eqSum(m, f)
    return { x: x01(f), y: (eqViewDb - Math.max(-eqViewDb, Math.min(eqViewDb, d))) / (2 * eqViewDb) }
}

// The compressor's static curve (6 dB soft knee) with the live level on it
// and a gain-reduction meter.
var compressorFloorDb = -60
var compressorKneeDb = 6
var grMeterDb = 24
function compress(input, threshold, ratio) {
    const over = input - threshold
    if (2 * over < -compressorKneeDb)
        return input
    if (2 * Math.abs(over) <= compressorKneeDb)
        return input + (1 / ratio - 1) * Math.pow(over + compressorKneeDb / 2, 2) / (2 * compressorKneeDb)
    return threshold + over / ratio
}
function dynamics(ctx, t, W, H, m) {
    const meter = t.fxGrMeterWidth
    const D = W - meter - t.fxGrMeterGap
    grid(ctx, t, D, H, dynamicsGridColumns)
    const threshold = -60 + 60 * m.p[0]
    const ratio = Contract.expo(1, 20)(m.p[1])
    const makeup = 24 * m.p[4]
    const span = -compressorFloorDb
    const out = d => compress(d, threshold, ratio) + makeup
    const y = d => H - (Math.max(compressorFloorDb, Math.min(0, d)) - compressorFloorDb) / span * H
    ctx.beginPath()
    for (let i = 0; i <= D; i += sampleStep) {
        const d = compressorFloorDb + span * i / D
        if (i === 0) ctx.moveTo(i, y(out(d))); else ctx.lineTo(i, y(out(d)))
    }
    glow(ctx, t, t.pk)
    if (m.live && m.inDb > compressorFloorDb)
        dot(ctx, t, (m.inDb - compressorFloorDb) / span * D, y(out(m.inDb)), t.am)
    ctx.fillStyle = t.knobTrack
    ctx.fillRect(W - meter, 0, meter, H)
    ctx.fillStyle = t.am
    ctx.fillRect(W - meter, 0, meter, Math.min(1, Math.max(0, m.value) / grMeterDb) * H)
    label(ctx, t, "GR", W - meter / 2, H - t.fxLabelLift, "center")
}

// Multiband: the three bands, each with its output gain and the live
// change pushing it down (amber) or lifting it (cyan).
var bandNames = ["LOW", "MID", "HIGH"]
var bandViewDb = 24
function bands(ctx, t, W, H, m) {
    const bw = W / bandNames.length
    for (let b = 0; b < bandNames.length; ++b) {
        const x = b * bw + t.fxBandInset
        const w = bw - 2 * t.fxBandInset
        const mid = H / 2
        const change = Math.max(-bandViewDb, Math.min(bandViewDb, m.detail[b]))
        const h = Math.abs(change) / bandViewDb * (H / 2 - t.fxBandHeadroom)
        ctx.fillStyle = change < 0 ? t.soft(t.am, t.fxCutAlpha) : t.soft(t.pk, t.fxBoostAlpha)
        ctx.fillRect(x, change < 0 ? mid : mid - h, w, h)
        const gain = 24 * m.p[3 + b] - 12
        const gy = mid - gain / bandViewDb * (H / 2 - t.fxBandHeadroom)
        ctx.beginPath()
        ctx.moveTo(x, gy)
        ctx.lineTo(x + w, gy)
        glow(ctx, t, t.pk)
        label(ctx, t, bandNames[b], x + w / 2, H - t.fxLabelLift, "center")
    }
}

// The delay's repeats on the beat grid (a bar of four beats, or two
// seconds when free); ping-pong alternates left (up) and right (down).
var delayViewBeats = 4
var delayViewMs = 2000
var delayGridMs = 500
// Repeats quieter than this are not drawn.
var tapFloor = 0.02
function taps(ctx, t, W, H, m) {
    const synced = m.synced
    const marks = synced ? delayViewBeats * 4 : delayViewMs / delayGridMs
    for (let i = 1; i < marks; ++i) {
        ctx.strokeStyle = synced && i % 4 === 0 ? t.gridLine2 : t.gridLine
        ctx.beginPath()
        ctx.moveTo(Math.round(i * W / marks) + t.hairline / 2, 0)
        ctx.lineTo(Math.round(i * W / marks) + t.hairline / 2, H)
        ctx.stroke()
    }
    ctx.strokeStyle = t.gridLine2
    ctx.beginPath()
    ctx.moveTo(0, H / 2)
    ctx.lineTo(W, H / 2)
    ctx.stroke()
    const step = synced
        ? Contract.delayBeats[Math.round(m.p[0] * (Contract.delayBeats.length - 1))] / delayViewBeats * W
        : Contract.expo(1, Contract.delayMaxMs)(m.p[0]) / delayViewMs * W
    const feedback = Math.min(0.98, 1.1 * m.p[1])
    const pingPong = m.mode === 1
    const tape = m.mode === 2
    let level = 1
    ctx.fillStyle = t.am
    ctx.fillRect(t.hairline, t.fxTapInset, t.fxTapWidth, H - 2 * t.fxTapInset)
    ctx.fillStyle = t.pk
    for (let k = 0, x = step; x < W && level > tapFloor && step > 1; ++k, x += step) {
        const h = level * (H / 2 - t.fxTapHeadroom)
        const w = tape ? t.fxTapeWidth : t.fxTapWidth
        if (!pingPong || k % 2 === 0)
            ctx.fillRect(x - w / 2, H / 2 - h, w, h)
        if (!pingPong || k % 2 === 1)
            ctx.fillRect(x - w / 2, H / 2, w, h)
        level *= feedback
    }
    // On the left: the plot frame's LIVE / PREVIEW chip holds the top right.
    label(ctx, t, "L", t.fxSideLabelInset, t.fxSideLabelTop, "center")
    label(ctx, t, "R", t.fxSideLabelInset, H - t.fxLabelLift, "center")
}

// The reverb's decay envelope in seconds, after its pre-delay. The view
// spans the tail with this margin, up to this many seconds; past this many
// it ticks every quarter of the span.
var decayViewMargin = 1.2
var decayViewMaximum = 32
var decayTickAbove = 8
var decayTicks = 4
// The decay view's span in seconds, and the seconds between its ticks.
function decaySpan(m) {
    const seconds = Contract.expo(0.1, 30)(m.p[1])
    const pre = 0.25 * m.p[3]
    return Math.max(1, Math.min(decayViewMaximum, (pre + seconds) * decayViewMargin))
}
function decayTickStep(span) {
    return span > decayTickAbove ? Math.ceil(span / decayTicks) : 1
}
function decay(ctx, t, W, H, m) {
    const seconds = Contract.expo(0.1, 30)(m.p[1])
    const pre = 0.25 * m.p[3]
    const span = decaySpan(m)
    const px = pre / span * W
    ctx.fillStyle = t.soft(t.am, t.fxPreDelayAlpha)
    ctx.fillRect(0, 0, px, H)
    const ticks = decayTickStep(span)
    for (let s = ticks; s < span; s += ticks) {
        const x = s / span * W
        ctx.strokeStyle = t.gridLine
        ctx.beginPath()
        ctx.moveTo(x, 0)
        ctx.lineTo(x, H)
        ctx.stroke()
    }
    // −60 dB over the decay time; damping darkens the tail's fill.
    ctx.beginPath()
    ctx.moveTo(px, H)
    for (let x = px; x <= W; x += sampleStep) {
        const s = (x - px) / W * span
        const level = Math.pow(10, -3 * s / seconds)
        ctx.lineTo(x, H - level * (H - t.fxDecayHeadroom))
    }
    ctx.lineTo(W, H)
    ctx.closePath()
    ctx.fillStyle = t.soft(t.pk, t.fxTailAlpha + t.fxTailDampAlpha * (1 - m.p[2]))
    ctx.fill()
    ctx.beginPath()
    for (let x = px; x <= W; x += sampleStep) {
        const s = (x - px) / W * span
        const y = H - Math.pow(10, -3 * s / seconds) * (H - t.fxDecayHeadroom)
        if (x === px) ctx.moveTo(x, y); else ctx.lineTo(x, y)
    }
    glow(ctx, t, t.pk)
}

// Chorus and Hyper: the voices across the stereo field, moving with the
// module's live phase, over these shares of the width and height.
var voicesSpread = 0.42
var voicesSwing = 0.36
function voices(ctx, t, W, H, m) {
    grid(ctx, t, W, H, gridColumns)
    const n = m.module === "hyper" ? m.mode + 2 : (m.mode === 1 ? 4 : 2)
    const width = m.module === "hyper" ? m.p[2] : 1
    const depth = m.module === "hyper" ? m.p[0] : m.p[1]
    const phase = m.live ? m.value : 0
    for (let v = 0; v < n; ++v) {
        const pan = n === 1 ? 0 : (v / (n - 1) * 2 - 1) * width
        const swing = Math.sin(2 * Math.PI * (phase + v / n)) * depth
        const x = W / 2 + pan * W * voicesSpread
        const y = H / 2 + swing * H * voicesSwing
        ctx.beginPath()
        ctx.fillStyle = t.soft(t.pk, t.fxVoiceHaloAlpha)
        ctx.arc(x, y, t.fxVoiceHaloRadius, 0, Math.PI * 2)
        ctx.fill()
        dot(ctx, t, x, y, v === 0 ? t.am : t.pk)
    }
    label(ctx, t, "L", t.fxLabelX, H - t.fxLabelLift)
    label(ctx, t, "R", W - t.fxLabelX, H - t.fxLabelLift, "right")
}

// Flanger and Phaser: the notch comb the sweep sits at now, over the
// output spectrum.
function comb(ctx, t, W, H, m) {
    spectrum(ctx, t, W, H, m)
    frequencyGrid(ctx, t, W, H)
    const phaser = m.module === "phaser"
    const centre = phaser ? Contract.expo(100, 8000)(m.p[3]) : 1000
    const phase = m.live ? m.value : 0.25
    const fc = centre * Math.pow(2, 2 * m.p[1] * Math.sin(2 * Math.PI * phase))
    const feedback = phaser ? 0.95 * m.p[2] : Math.abs(1.9 * m.p[2] - 0.95)
    ctx.beginPath()
    for (let i = 0; i <= W; i += sampleStep) {
        const d = Math.max(filterBottomDb, Contract.filterDb(phaser ? 17 : 18, Contract.displayHz(i / W), fc, feedback))
        const y = yOf(d, filterTopDb, filterBottomDb, H)
        if (i === 0) ctx.moveTo(i, y); else ctx.lineTo(i, y)
    }
    glow(ctx, t, t.pk)
}

// Dimension and Utility: the stereo image as a field as wide as the
// width, leaning with the pan, as loud as the output. Dimension's width
// grows from a floor with its size and amount; the level reads louder than
// the output (within a floor and one), or sits at a rest level while idle.
// The field leans over this share of the width, its radius is a share of
// the height (at least a few pixels, at most a share of the width), and
// it rises from the floor by half its level, this tall.
var dimensionFloor = 0.4
var dimensionSize = 0.6
var dimensionAmount = 0.4
var imageMinLevel = 0.15
var imageLevelGain = 1.5
var imageRestLevel = 0.6
var imageLean = 0.3
var imageMinRadius = 3
var imageMaxRadius = 0.48
var imageRadius = 0.55
var imageHeight = 0.4
function stereo(ctx, t, W, H, m) {
    ctx.strokeStyle = t.gridLine
    ctx.beginPath()
    ctx.moveTo(W / 2, 0)
    ctx.lineTo(W / 2, H)
    ctx.moveTo(W / 2 - H, H)
    ctx.lineTo(W / 2, 0)
    ctx.lineTo(W / 2 + H, H)
    ctx.stroke()
    const utility = m.module === "utility"
    const width = utility ? 2 * m.p[1] : dimensionFloor + m.p[1] * (dimensionSize + dimensionAmount * m.p[0])
    const pan = utility ? 2 * m.p[3] - 1 : 0
    const level = m.live ? Math.max(imageMinLevel, Math.min(1, linear(m.outDb) * imageLevelGain)) : imageRestLevel
    const cx = W / 2 + pan * W * imageLean
    const rx = Math.max(imageMinRadius, Math.min(W * imageMaxRadius, width * H * imageRadius))
    ctx.beginPath()
    for (let i = 0; i <= pathSamples; ++i) {
        const a = i / pathSamples * Math.PI * 2
        const x = cx + Math.cos(a) * rx
        const y = H * (1 - level / 2) + Math.sin(a) * H * imageHeight * level
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
    }
    ctx.fillStyle = t.soft(t.pk, t.fxImageAlpha)
    ctx.fill()
    glow(ctx, t, t.pk)
    if (utility && m.p[2] > 0)
        label(ctx, t, "MONO < " + Contract.hz(Contract.expo(20, 500)(m.p[2])), t.fxLabelX, t.fxLabelTop)
}

// Convolve: the impulse's shape after its pre-delay and cuts: a decaying
// ripple, faster with the brightness, its height cut by up to half.
var impulsePreDelay = 0.25
var impulseDarkest = 0.3
var impulseDecay = 5
var impulseRippleRate = 0.6
var impulseRippleBright = 1.6
var impulseBeatRate = 0.13
var impulseAmplitude = 0.42
function impulse(ctx, t, W, H, m) {
    grid(ctx, t, W, H, gridColumns)
    const px = m.p[0] * W * impulsePreDelay
    const brightness = impulseDarkest + (1 - impulseDarkest) * m.p[2]
    ctx.beginPath()
    for (let i = 0; i <= W - px; i += sampleStep) {
        const e = Math.exp(-i / W * impulseDecay)
        const ripple = Math.sin(i * (impulseRippleRate + impulseRippleBright * brightness)) * Math.cos(i * impulseBeatRate)
        const y = H / 2 - e * ripple * H * impulseAmplitude * (1 - m.p[1] / 2)
        if (i === 0) ctx.moveTo(px + i, y); else ctx.lineTo(px + i, y)
    }
    glow(ctx, t, t.pk)
}

// The shifter: the output spectrum, and an arrow from 1 kHz to where the
// shift moves it (both ways for Ring).
var shiftReferenceHz = 1000
// The arrow's height, as a share of the display's.
var shiftArrowAt = 0.35
function shift(ctx, t, W, H, m) {
    spectrum(ctx, t, W, H, m)
    frequencyGrid(ctx, t, W, H)
    const amount = m.live ? m.value : Contract.shifterHz(m.p[0])
    const ring = m.mode === 1
    const arrow = (to, color) => {
        const x0 = x01(shiftReferenceHz) * W
        const x1 = x01(Math.max(Contract.eqMinHz, to)) * W
        const y = H * shiftArrowAt
        ctx.beginPath()
        ctx.moveTo(x0, y)
        ctx.lineTo(x1, y)
        const dir = x1 >= x0 ? 1 : -1
        ctx.moveTo(x1, y)
        ctx.lineTo(x1 - t.fxArrowLength * dir, y - t.fxArrowSpread)
        ctx.moveTo(x1, y)
        ctx.lineTo(x1 - t.fxArrowLength * dir, y + t.fxArrowSpread)
        glow(ctx, t, color)
    }
    dot(ctx, t, x01(shiftReferenceHz) * W, H * shiftArrowAt, t.dim)
    arrow(shiftReferenceHz + amount, t.am)
    if (ring)
        arrow(shiftReferenceHz - amount, t.am)
    label(ctx, t, Contract.signedHz(amount), t.fxLabelX, t.fxLabelTop)
}

// Flyby: a small chart from above, you at the centre facing up, the path
// at its closest distance, and the source where Position puts it (or the
// distance it is heard from now).
var flybyMargin = 1.4
// The engine's paths (08a §5): straight passes reach 4 closest distances
// either side, 30 m at least and the map's 150 m at most; the arc's ends sit
// a third of the reach behind, its handles lifting the apex to d.
var flybyReachPerMetre = 4
var flybyMinReach = 30
var flybyMaxReach = 150
function flybyReach(d) {
    return Math.max(flybyMinReach, Math.min(flybyMaxReach, flybyReachPerMetre * d))
}
function flybyAt(path, d, u) {
    const r = flybyReach(d)
    switch (path) {
    case Contract.flybyPath.rightLeft: return [r - 2 * r * u, d]
    case Contract.flybyPath.arc: {
        const back = -r / 3
        const lift = Math.min((d - 0.25 * back) / 0.75, flybyMaxReach)
        const s = 1 - u
        const bx = [-r, -r / 2, r / 2, r], by = [back, lift, lift, back]
        const w = [s * s * s, 3 * s * s * u, 3 * s * u * u, u * u * u]
        return [w[0] * bx[0] + w[1] * bx[1] + w[2] * bx[2] + w[3] * bx[3],
                w[0] * by[0] + w[1] * by[1] + w[2] * by[2] + w[3] * by[3]]
    }
    case Contract.flybyPath.orbit: {
        const a = 2 * Math.PI * u
        return [d * Math.sin(a), d * Math.cos(a)]
    }
    case Contract.flybyPath.overhead: return [0, -r + 2 * r * u]
    default: return [-r + 2 * r * u, d]
    }
}
function flyby(ctx, t, W, H, m) {
    const path = Contract.modeIndex(m.p[0], Contract.flybyPaths.length)
    const d = Contract.flybyMetres(m.p[1])
    const h = Contract.flybyMaxHeight * m.p[10]
    const reach = (path === Contract.flybyPath.orbit ? d : flybyReach(d)) * flybyMargin
    const scale = Math.min(W, H) / 2 / Math.max(reach, Contract.flybyMinM)
    const px = q => [W / 2 + q[0] * scale, H / 2 - q[1] * scale]
    ctx.strokeStyle = t.gridLine
    ctx.lineWidth = t.hairline
    ctx.beginPath()
    ctx.arc(W / 2, H / 2, d * scale, 0, Math.PI * 2)
    ctx.stroke()
    ctx.beginPath()
    for (let i = 0; i <= pathSamples; ++i) {
        const q = px(flybyAt(path, d, i / pathSamples))
        if (i === 0) ctx.moveTo(q[0], q[1]); else ctx.lineTo(q[0], q[1])
    }
    glow(ctx, t, t.am)
    dot(ctx, t, W / 2, H / 2, t.pk)
    const run = Contract.modeIndex(m.p[3], Contract.flybyRuns.length)
    if (run === Contract.flybyRun.position) {
        const q = px(flybyAt(path, d, m.p[5]))
        dot(ctx, t, q[0], q[1], t.am)
    }
    const heard = m.live ? Contract.metres(m.value) : Contract.metres(d) + " closest"
    label(ctx, t, heard + (h > 0 ? " · " + Contract.metres(h) + " up" : ""), t.fxLabelX, t.fxLabelTop)
}

function paint(ctx, t, W, H, m) {
    switch (m.visual) {
    case "transfer": transfer(ctx, t, W, H, m); break
    case "response": response(ctx, t, W, H, m); break
    case "eq": eq(ctx, t, W, H, m); break
    case "dynamics": dynamics(ctx, t, W, H, m); break
    case "bands": bands(ctx, t, W, H, m); break
    case "taps": taps(ctx, t, W, H, m); break
    case "decay": decay(ctx, t, W, H, m); break
    case "voices": voices(ctx, t, W, H, m); break
    case "comb": comb(ctx, t, W, H, m); break
    case "impulse": impulse(ctx, t, W, H, m); break
    case "shift": shift(ctx, t, W, H, m); break
    case "flyby": flyby(ctx, t, W, H, m); break
    default: stereo(ctx, t, W, H, m)
    }
}

// ---- Scales (Kit.PlotFrame) -------------------------------------------------------------

// Each visual's axes for its plot frame: { x, y } when it has a scale, or
// { exempt: why } for a picture without measured axes. The ticks a scale
// shows are at the same places the painters above draw their grids.
var frequencyMarks = [{ hz: 100, label: "100" }, { hz: 1000, label: "1k" }, { hz: 10000, label: "10k" }]
var filterTicksDb = [-24, 0, 12]
var eqTicksDb = [-12, 0, 12]
var bandTicksDb = [-12, 0, 12]
var dynamicsTicksDb = [-60, -30, 0]
// The shaper's input and output, as a share of full scale (-1..1).
var transferTicks = [-1, 0, 1]
var percent = 100
function axes(visual) {
    switch (visual) {
    case "transfer": case "response": case "eq": case "comb": case "dynamics":
        return { x: true, y: true }
    case "shift": case "taps": case "decay":
        return { x: true, y: false }
    case "bands":
        return { x: false, y: true }
    case "voices":
        return { exempt: "a picture of the chorus voices across the stereo field; their pan reads on the knobs" }
    case "stereo":
        return { exempt: "a picture of the stereo image's width and lean; no measured axis" }
    case "impulse":
        return { exempt: "a sketch of the impulse's shape from its settings, not a measured response" }
    case "flyby":
        return { exempt: "a map from above; its distances are printed on it in metres" }
    default:
        return { exempt: "no module in the slot" }
    }
}
function signed(v) { return v > 0 ? "+" + v : String(v) }
function hzTicks() {
    return frequencyMarks.map(mark => ({ at: x01(mark.hz), label: mark.label }))
}
function dbTicks(list, top, bottom) {
    return list.map(d => ({ at: (d - bottom) / (top - bottom), label: signed(d) }))
}
// The ticks for visual `visual` drawn from slot model `m` (theme `t`) on a
// W x H plot:
// { xTicks, xUnit, yTicks, yUnit } (empty where the visual has no axis).
function scale(visual, m, t, W, H) {
    const none = { xTicks: [], xUnit: "", yTicks: [], yUnit: "" }
    if (m === null || W <= 0 || H <= 0)
        return none
    switch (visual) {
    case "transfer": {
        const across = transferTicks.map(v => ({ at: (v + 1) / 2, label: signed(v * percent) }))
        const up = transferTicks.map(v => ({ at: 0.5 + v * transferAmplitude, label: signed(v * percent) }))
        return { xTicks: across, xUnit: "%", yTicks: up, yUnit: "%" }
    }
    case "response": case "comb":
        return { xTicks: hzTicks(), xUnit: "Hz", yTicks: dbTicks(filterTicksDb, filterTopDb, filterBottomDb), yUnit: "dB" }
    case "eq":
        return { xTicks: hzTicks(), xUnit: "Hz", yTicks: dbTicks(eqTicksDb, eqViewDb, -eqViewDb), yUnit: "dB" }
    case "shift":
        return { xTicks: hzTicks(), xUnit: "Hz", yTicks: [], yUnit: "" }
    case "dynamics": {
        // The curve spans the width left of the gain-reduction meter.
        const D = (W - t.fxGrMeterWidth - t.fxGrMeterGap) / W
        const share = d => (d - compressorFloorDb) / -compressorFloorDb
        return { xTicks: dynamicsTicksDb.map(d => ({ at: share(d) * D, label: signed(d) })), xUnit: "dB",
                 yTicks: dynamicsTicksDb.map(d => ({ at: share(d), label: signed(d) })), yUnit: "dB" }
    }
    case "bands": {
        const reach = (H / 2 - t.fxBandHeadroom) / H
        return { xTicks: [], xUnit: "",
                 yTicks: bandTicksDb.map(d => ({ at: 0.5 + d / bandViewDb * reach, label: signed(d) })), yUnit: "dB" }
    }
    case "taps": {
        if (m.synced) {
            const beats = []
            for (let b = 0; b <= delayViewBeats; ++b)
                beats.push({ at: b / delayViewBeats, label: String(b) })
            return { xTicks: beats, xUnit: "beats", yTicks: [], yUnit: "" }
        }
        const times = []
        for (let at = 0; at <= delayViewMs; at += delayGridMs)
            times.push({ at: at / delayViewMs, label: String(at) })
        return { xTicks: times, xUnit: "ms", yTicks: [], yUnit: "" }
    }
    case "decay": {
        const span = decaySpan(m)
        const step = decayTickStep(span)
        const times = [{ at: 0, label: "0" }]
        for (let s = step; s < span; s += step)
            times.push({ at: s / span, label: String(s) })
        return { xTicks: times, xUnit: "s", yTicks: [], yUnit: "" }
    }
    default:
        return none
    }
}
