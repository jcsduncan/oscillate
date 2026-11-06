pragma ComponentBehavior: Bound
import QtQuick

// The decay landscape: the reverb's energy over time (across) and frequency
// (up), drawn as aurora curtains under the night sky. It is computed from
// the settings exactly as the reverb designs them:
//  - each band's RT60 comes from the network's absorption, second-order low
//    and high shelves designed after Jot (see expanse.cpp), so the bright
//    RT60 line bends with the damping;
//  - pre-delay is the dark gap at the front, with the space's early
//    reflections as stars on the horizon (the reverb's own pattern);
//  - the field blooms in as diffusion builds;
//  - shimmer lifts a ghost curtain by its interval;
//  - freeze holds everything.
// While the reverb plays, its early and late levels light the sky.
Item {
    id: view
    required property var controller
    required property ExpanseTheme theme
    // Live levels in dBFS (from telemetry), and whether they are live.
    property bool live: false
    property real earlyDb: -120
    property real lateDb: -120

    readonly property int revision: controller.revision
    readonly property real decay: revision >= 0 ? controller.value("expanse.decay") : 2
    readonly property real lowMultiplier: revision >= 0 ? controller.value("expanse.low_decay") : 1
    readonly property real lowFreq: revision >= 0 ? controller.value("expanse.low_freq") : 300
    readonly property real highMultiplier: revision >= 0 ? controller.value("expanse.high_decay") : 1
    readonly property real highFreq: revision >= 0 ? controller.value("expanse.high_freq") : 4500
    readonly property real predelay: revision >= 0 ? controller.value("expanse.predelay") / 1000 : 0
    readonly property real size: revision >= 0 ? controller.value("expanse.size") : 0.6
    readonly property int mode: revision >= 0 ? controller.choice("expanse.mode") : 1
    readonly property real earlyDiffusion: revision >= 0 ? controller.value("expanse.early_diffusion") : 0.7
    readonly property real modDepth: revision >= 0 ? controller.value("expanse.mod_depth") : 0
    readonly property real shimmer: revision >= 0 ? controller.value("expanse.shimmer") : 0
    readonly property real earlyLevel: revision >= 0 ? controller.value("expanse.early") : 0.5
    readonly property real lateLevel: revision >= 0 ? controller.value("expanse.late") : 1
    readonly property bool frozen: revision >= 0 && controller.choice("expanse.freeze") === 1
    readonly property real shimmerRatio: {
        const semitones = [-12, 5, 7, 12, 19, 24]
        const index = revision >= 0 ? controller.choice("expanse.shimmer_pitch") : 3
        return Math.pow(2, semitones[Math.max(0, Math.min(5, index))] / 12)
    }

    // The three readouts, in seconds (Infinity when frozen): the band ends
    // (DC and Nyquist) and the mid band between the crossovers.
    readonly property real lowRt: rtAt(1)
    readonly property real midRt: rtAt(Math.sqrt(Math.max(lowFreq, 20) * Math.min(highFreq, 20000)))
    readonly property real highRt: rtAt(0.4999 * sampleRate)
    // Seconds across the view.
    readonly property real span: {
        if (frozen)
            return Math.max(0.5, decay * 1.1 + predelay)
        let longest = 0
        for (const hz of [25, 80, 250, 1000, 4000, 12000, 20000])
            longest = Math.max(longest, rtAt(hz))
        return Math.max(0.4, Math.min(45, longest * 1.06 + predelay))
    }
    readonly property real glow: live ? Math.max(0, Math.min(1, (lateDb + 54) / 48)) : 0

    objectName: "expanseLandscape"
    implicitWidth: theme.landscapeDefaultWidth
    implicitHeight: theme.landscapeDefaultHeight

    // ---- The decay maths (mirrors the reverb's absorption design) -------------

    readonly property real sampleRate: 48000
    // A representative loop: the shelves' gains scale with each line's
    // length; 50 ms is mid-network, and the band ends are exact for any.
    readonly property real loopSeconds: 0.05

    // An RBJ shelf (S = 1) with `gain` at DC (low) or Nyquist (high), as the
    // reverb designs it; its magnitude in dB at `hz`.
    function shelfDb(high, gain, corner, hz) {
        const rate = sampleRate
        const a = Math.sqrt(gain)
        const w = 2 * Math.PI * Math.min(corner, 0.45 * rate) / rate
        const c = Math.cos(w)
        const alpha = Math.sin(w) / 2 * Math.SQRT2
        const k = 2 * Math.sqrt(a) * alpha
        const sg = high ? -1 : 1
        const n0 = a * ((a + 1) - sg * (a - 1) * c + k)
        const n1 = sg * 2 * a * ((a - 1) - sg * (a + 1) * c)
        const n2 = a * ((a + 1) - sg * (a - 1) * c - k)
        const d0 = (a + 1) + sg * (a - 1) * c + k
        const d1 = -sg * 2 * ((a - 1) + sg * (a + 1) * c)
        const d2 = (a + 1) + sg * (a - 1) * c - k
        const o = 2 * Math.PI * hz / rate
        const nr = n0 + n1 * Math.cos(o) + n2 * Math.cos(2 * o)
        const ni = -n1 * Math.sin(o) - n2 * Math.sin(2 * o)
        const dr = d0 + d1 * Math.cos(o) + d2 * Math.cos(2 * o)
        const di = -d1 * Math.sin(o) - d2 * Math.sin(2 * o)
        return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di))
    }
    // RT60 at `hz` in seconds: 60 dB over the loop's loss per second.
    function rtAt(hz) {
        if (frozen)
            return Infinity
        const tau = loopSeconds
        const mid = Math.pow(10, -3 * tau / decay)
        const low = Math.pow(10, -3 * tau / (decay * lowMultiplier))
        const high = Math.pow(10, -3 * tau / (decay * highMultiplier))
        const db = 20 * Math.log10(mid) + shelfDb(false, low / mid, lowFreq, hz)
                   + shelfDb(true, high / mid, highFreq, hz)
        return -60 * tau / db
    }

    // ---- Early reflections: the reverb's own per-space pattern ----------------

    readonly property var tapCounts: [12, 16, 0, 20, 14, 6]
    readonly property var firstTapMs: [3, 8, 0, 2, 20, 30]
    readonly property var lastTapMs: [35, 80, 0, 45, 160, 200]
    // The left channel's taps as [{seconds, gain}], from the same generator.
    function reflections() {
        const count = tapCounts[mode]
        const taps = []
        if (count === 0)
            return taps
        let state = (91 + 37 * mode) >>> 0
        const next = () => {
            state = (Math.imul(state, 1664525) + 1013904223) >>> 0
            return (state >>> 8) / 16777216
        }
        const scale = 0.25 + 0.75 * size
        let energy = 0
        for (let k = 0; k < count; ++k) {
            const position = (k + 0.5 * next()) / count
            const ms = firstTapMs[mode] + (lastTapMs[mode] - firstTapMs[mode]) * Math.pow(position, 1.4)
            const gain = Math.pow(0.82, k) * (0.7 + 0.3 * next()) * (k % 2 === 0 ? 1 : -1)
            next() // The crossing choice.
            taps.push({ seconds: predelay + ms * scale / 1000, gain: gain })
            energy += gain * gain
        }
        for (const tap of taps)
            tap.gain /= Math.sqrt(energy)
        return taps
    }

    // How quickly the field fills in: the space's diffusion time.
    readonly property var bloomSeconds: [0.006, 0.01, 0.004, 0.008, 0.02, 0.05]
    function bloom(t) {
        const tau = bloomSeconds[mode] * (0.4 + 1.2 * earlyDiffusion)
        return t <= 0 ? 0 : 1 - Math.exp(-t / tau)
    }

    // ---- Geometry ---------------------------------------------------------------

    readonly property real padLeft: 40
    readonly property real padTop: 30
    readonly property real padRight: 12
    readonly property real stripHeight: 30
    // The time axis under the field, and the early strip's distance below it.
    readonly property real timeAxisHeight: 26
    readonly property real stripGap: 22
    readonly property real padBottom: timeAxisHeight + stripHeight
    readonly property real plotWidth: width - padLeft - padRight
    readonly property real plotHeight: height - padTop - padBottom
    readonly property real stripTop: padTop + plotHeight + stripGap
    // The early strip's span: through the last reflection, or 40 ms past
    // the pre-delay for a plate.
    readonly property real earlySpan: {
        const taps = revision >= 0 ? reflections() : []
        const last = taps.length > 0 ? taps[taps.length - 1].seconds : predelay + 0.04
        return Math.max(0.03, last * 1.12 + 0.004)
    }
    readonly property real minHz: 20
    readonly property real maxHz: 20000
    function xAt(seconds) { return padLeft + plotWidth * Math.max(0, Math.min(1, seconds / span)) }
    function yAt(hz) {
        const f = Math.log(hz / minHz) / Math.log(maxHz / minHz)
        return padTop + plotHeight * (1 - f)
    }
    function hzAtFraction(f) { return minHz * Math.pow(maxHz / minHz, f) }
    function secondsText(s) {
        if (!isFinite(s))
            return "∞"
        return s < 1 ? Math.round(s * 1000) + " ms" : s < 10 ? s.toFixed(2) + " s" : s.toFixed(1) + " s"
    }

    // The aurora's colour at a height: green at its base, violet at its crown.
    function auroraAt(fraction, alpha) {
        const g = theme.aurora
        const t = theme.auroraTeal
        const v = theme.auroraViolet
        let r, gg, b
        if (fraction < 0.5) {
            const u = fraction / 0.5
            r = g.r + (t.r - g.r) * u; gg = g.g + (t.g - g.g) * u; b = g.b + (t.b - g.b) * u
        } else {
            const u = (fraction - 0.5) / 0.5
            r = t.r + (v.r - t.r) * u; gg = t.g + (v.g - t.g) * u; b = t.b + (v.b - t.b) * u
        }
        return Qt.rgba(r, gg, b, alpha)
    }

    Rectangle {
        x: view.padLeft
        y: view.padTop
        width: view.plotWidth
        height: view.plotHeight
        radius: view.theme.radiusMd
        gradient: Gradient {
            GradientStop { position: 0; color: view.theme.fieldTop }
            GradientStop { position: 1; color: view.theme.fieldBottom }
        }
        border.color: view.theme.seam
    }

    Canvas {
        id: field
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const x0 = view.padLeft
            const y0 = view.padTop
            const w = view.plotWidth
            const h = view.plotHeight
            ctx.save()
            ctx.beginPath()
            ctx.rect(x0, y0, w, h)
            ctx.clip()

            // Faint grid: decades of frequency.
            ctx.lineWidth = t.hairline
            for (const hz of [100, 1000, 10000]) {
                const y = Math.round(view.yAt(hz)) + t.hairline / 2
                ctx.strokeStyle = t.fieldGrid
                ctx.beginPath()
                ctx.moveTo(x0, y)
                ctx.lineTo(x0 + w, y)
                ctx.stroke()
            }

            // The curtains: one vertical gradient per column, its stops the
            // energy left at each frequency.
            const stops = 16
            const rt = []
            const shimmerRt = []
            for (let k = 0; k < stops; ++k) {
                const f = k / (stops - 1)
                rt.push(view.rtAt(view.hzAtFraction(f)))
                const source = view.hzAtFraction(f) / view.shimmerRatio
                shimmerRt.push(source >= view.minHz ? view.rtAt(source) : 0)
            }
            const onset = view.predelay
            const step = 2
            const level = Math.max(t.curtainLevelFloor, view.lateLevel)
            // The curtain's folds: a slow ripple of brightness across it.
            const fold = (x, f) => 0.74 + 0.26 * (0.5 + 0.5 * Math.sin(x * 0.05 + f * 7 + 2.2 * Math.sin(x * 0.011)))
            const column = (x, alphaAt) => {
                const g = ctx.createLinearGradient(0, y0 + h, 0, y0)
                for (let k = 0; k < stops; ++k) {
                    const f = k / (stops - 1)
                    g.addColorStop(f, alphaAt(k, f))
                }
                ctx.fillStyle = g
                ctx.fillRect(x0 + x, y0, step, h)
            }
            for (let x = 0; x < w; x += step) {
                const since = view.span * (x + step / 2) / w - onset
                if (since <= 0)
                    continue
                const b = view.bloom(since)
                column(x, (k, f) => {
                    const left = isFinite(rt[k]) ? Math.max(0, 1 - since / rt[k]) : 1
                    return view.auroraAt(f, t.curtainAlpha * Math.pow(left, 1.5) * b * level * fold(x, f))
                })
            }
            // Shimmer: a magenta ghost curtain lifted by the interval, building
            // as the tail recirculates through the pitch shifter.
            if (view.shimmer > 0.001 && view.shimmerRatio > 1) {
                ctx.globalCompositeOperation = "lighter"
                const m = t.auroraMagenta
                for (let x = 0; x < w; x += step) {
                    const since = view.span * (x + step / 2) / w - onset
                    if (since <= 0)
                        continue
                    const build = 1 - Math.exp(-since / 0.35)
                    column(x, (k, f) => {
                        const r = shimmerRt[k]
                        const left = r === 0 ? 0 : isFinite(r) ? Math.max(0, 1 - since / r) : 1
                        return Qt.rgba(m.r, m.g, m.b, t.shimmerCurtainAlpha * Math.sqrt(view.shimmer) * build
                                       * Math.pow(left, 1.3) * Math.sqrt(f) * fold(x + 37, f))
                    })
                }
                ctx.globalCompositeOperation = "source-over"
            }

            // Pre-delay: the quiet before the space answers.
            const xp = view.xAt(onset)
            if (xp > x0 + 1) {
                ctx.fillStyle = t.predelayShade
                ctx.fillRect(x0, y0, xp - x0, h)
                ctx.strokeStyle = t.predelayEdge
                ctx.setLineDash(t.predelayDash)
                ctx.beginPath()
                ctx.moveTo(xp + t.hairline / 2, y0)
                ctx.lineTo(xp + t.hairline / 2, y0 + h)
                ctx.stroke()
                ctx.setLineDash([])
            }

            // Crossovers: where the low and high decays take over.
            ctx.setLineDash(t.crossoverDash)
            ctx.lineWidth = t.hairline
            for (const hz of [view.lowFreq, view.highFreq]) {
                const y = Math.round(view.yAt(hz)) + t.hairline / 2
                ctx.strokeStyle = t.crossoverLine
                ctx.beginPath()
                ctx.moveTo(x0, y)
                ctx.lineTo(x0 + w, y)
                ctx.stroke()
            }
            ctx.setLineDash([])

            // Iso-decay lines (-20, -40 dB) and the RT60 line itself, rippled
            // by the modulation.
            if (!view.frozen) {
                const wobble = view.modDepth * t.rtWobble
                for (const [drop, alpha, width] of [[20, t.isoLineAlpha, t.hairline], [40, t.isoLineAlpha, t.hairline],
                                                    [60, t.rtLineAlpha, t.rtLineWidth]]) {
                    ctx.strokeStyle = Qt.rgba(t.star.r, t.star.g, t.star.b, alpha)
                    ctx.lineWidth = width
                    ctx.beginPath()
                    const rows = 64
                    for (let i = 0; i <= rows; ++i) {
                        const f = i / rows
                        const hz = view.hzAtFraction(f)
                        const y = y0 + h * (1 - f)
                        const x = view.xAt(onset + view.rtAt(hz) * drop / 60)
                            + wobble * Math.sin(i * 0.9 + drop)
                        if (i === 0)
                            ctx.moveTo(x, y)
                        else
                            ctx.lineTo(x, y)
                    }
                    ctx.stroke()
                }
                // Shimmer's copy of the RT60 line, lifted by the interval.
                if (view.shimmer > 0.001) {
                    const m = t.auroraMagenta
                    ctx.strokeStyle = Qt.rgba(m.r, m.g, m.b, t.shimmerLineAlpha + t.shimmerLineAlphaRange * view.shimmer)
                    ctx.lineWidth = t.shimmerLineWidth
                    ctx.setLineDash(t.shimmerDash)
                    ctx.beginPath()
                    let started = false
                    const rows = 64
                    for (let i = 0; i <= rows; ++i) {
                        const hz = view.hzAtFraction(i / rows)
                        const source = hz / view.shimmerRatio
                        if (source < view.minHz || source > view.maxHz)
                            continue
                        const x = view.xAt(onset + view.rtAt(source))
                        const y = view.yAt(hz)
                        if (!started) {
                            ctx.moveTo(x, y)
                            started = true
                        } else {
                            ctx.lineTo(x, y)
                        }
                    }
                    ctx.stroke()
                    ctx.setLineDash([])
                }
            }

            // Frozen: a frost over the held field.
            if (view.frozen) {
                const frost = ctx.createLinearGradient(x0, 0, x0 + w, 0)
                frost.addColorStop(0, t.frostStart)
                frost.addColorStop(1, t.frostEnd)
                ctx.fillStyle = frost
                ctx.fillRect(x0, y0, w, h)
            }
            ctx.restore()

            // Axes: frequency on the left, time underneath.
            ctx.fillStyle = t.inkFaint
            ctx.font = t.typeMicro + "px '" + t.sans + "'"
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            for (const [hz, label] of [[100, "100"], [1000, "1k"], [10000, "10k"]])
                ctx.fillText(label, x0 - t.axisLabelGap, view.yAt(hz))
            ctx.fillText("Hz", x0 - t.axisLabelGap, y0 + t.spaceSm)
            const choices = [0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10]
            let tick = choices[choices.length - 1]
            for (const c of choices) {
                if (view.span / c <= t.maxTimeTicks) {
                    tick = c
                    break
                }
            }
            ctx.textAlign = "center"
            ctx.textBaseline = "top"
            for (let s = 0; s <= view.span + 1e-9; s += tick) {
                const x = view.xAt(s)
                ctx.fillStyle = t.timeTick
                ctx.fillRect(x, y0 + h, t.hairline, t.timeTickLength)
                ctx.fillStyle = t.inkFaint
                ctx.fillText(s === 0 ? "0" : view.secondsText(s), Math.min(x, x0 + w - t.timeLabelInset),
                             y0 + h + t.axisLabelGap)
            }

            // The early reflections, magnified on their own strip: the
            // pre-delay's quiet, then the space's taps as stars, brighter for
            // louder ones, and the late field's onset.
            const sy = view.stripTop
            const sh = view.stripHeight
            ctx.fillStyle = t.stripFill
            ctx.fillRect(x0, sy, w, sh)
            ctx.strokeStyle = t.seam
            ctx.lineWidth = t.hairline
            ctx.strokeRect(x0 + t.hairline / 2, sy + t.hairline / 2, w - t.hairline, sh - t.hairline)
            ctx.fillStyle = t.inkFaint
            ctx.font = "bold " + t.stripLabelSize + "px '" + t.sans + "'"
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            ctx.fillText("EARLY", x0 - t.spaceSm, sy + sh / 2)
            const taps = view.reflections()
            const stripSpan = view.earlySpan
            const sx = s => x0 + w * Math.min(1, s / stripSpan)
            const xpre = sx(onset)
            if (xpre > x0 + 1) {
                ctx.fillStyle = t.stripPredelayShade
                ctx.fillRect(x0 + t.hairline, sy + t.hairline, xpre - x0 - t.hairline, sh - 2 * t.hairline)
            }
            ctx.strokeStyle = t.stripOnset
            ctx.beginPath()
            ctx.moveTo(xpre + t.hairline / 2, sy + t.stripOnsetInset)
            ctx.lineTo(xpre + t.hairline / 2, sy + sh - t.stripOnsetInset)
            ctx.stroke()
            let loudest = 0
            for (const tap of taps)
                loudest = Math.max(loudest, Math.abs(tap.gain))
            const presence = t.tapPresenceFloor + (1 - t.tapPresenceFloor) * view.earlyLevel
            for (const tap of taps) {
                const x = sx(tap.seconds) + t.hairline / 2
                const strength = Math.abs(tap.gain) / loudest
                const yb = sy + sh - t.tapBase
                const len = t.tapMinLength + (sh - t.tapLengthInset) * strength
                const g = ctx.createLinearGradient(0, yb, 0, yb - len)
                g.addColorStop(0, Qt.rgba(t.star.r, t.star.g, t.star.b, t.tapAlpha * presence))
                g.addColorStop(1, t.tapFade)
                ctx.strokeStyle = g
                ctx.lineWidth = t.tapWidth
                ctx.beginPath()
                ctx.moveTo(x, yb)
                ctx.lineTo(x, yb - len)
                ctx.stroke()
                const halo = ctx.createRadialGradient(x, yb - len, 0, x, yb - len, t.tapHaloRadius)
                halo.addColorStop(0, Qt.rgba(t.star.r, t.star.g, t.star.b, t.tapHaloAlpha * presence))
                halo.addColorStop(1, t.starFade)
                ctx.fillStyle = halo
                ctx.beginPath()
                ctx.arc(x, yb - len, t.tapHaloRadius, 0, Math.PI * 2)
                ctx.fill()
            }
            ctx.fillStyle = t.inkFaint
            ctx.font = t.typeMicro + "px '" + t.sans + "'"
            ctx.textAlign = "right"
            ctx.textBaseline = "top"
            ctx.fillText(Math.round(stripSpan * 1000) + " ms", x0 + w - t.spaceXs, sy + t.stripTextY)
            if (taps.length === 0) {
                ctx.textAlign = "left"
                ctx.fillText("NO EARLY REFLECTIONS · A PLATE ANSWERS AT ONCE", xpre + t.spaceMd, sy + t.stripNoteY)
            }
        }
        Connections {
            target: view
            function onRevisionChanged() { field.requestPaint() }
            function onWidthChanged() { field.requestPaint() }
            function onHeightChanged() { field.requestPaint() }
        }
    }

    // The live sky: the reverb's level lights the curtains from within.
    Rectangle {
        objectName: "expanseGlow"
        x: view.padLeft
        y: view.padTop
        width: view.plotWidth
        height: view.plotHeight
        radius: view.theme.radiusMd
        opacity: view.glow
        visible: opacity > 0.005
        gradient: Gradient {
            GradientStop { position: 0; color: view.theme.glowTop }
            GradientStop { position: view.theme.glowMidStop; color: view.theme.glowMid }
            GradientStop { position: 1; color: view.theme.glowBottom }
        }
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    // Readouts along the top.
    Row {
        x: view.padLeft
        y: view.theme.spaceXs
        spacing: view.theme.readoutRowSpacing
        component Readout: Row {
            id: readout
            required property string band
            required property string value
            required property color tone
            spacing: view.theme.spaceSm
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: view.theme.readoutDot
                height: view.theme.readoutDot
                radius: view.theme.readoutDot / 2
                color: readout.tone
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: readout.band
                color: view.theme.inkFaint
                font.pixelSize: view.theme.typeMicro
                font.bold: true
                font.letterSpacing: view.theme.captionSpacing
            }
            Text {
                objectName: "expanseRt:" + readout.band.toLowerCase()
                anchors.verticalCenter: parent.verticalCenter
                text: readout.value
                color: view.theme.ink
                font.family: view.theme.mono
                font.pixelSize: view.theme.typeLabel
            }
        }
        Readout { band: "LOW"; value: view.secondsText(view.lowRt); tone: view.theme.aurora }
        Readout { band: "MID"; value: view.secondsText(view.midRt); tone: view.theme.auroraTeal }
        Readout { band: "HIGH"; value: view.secondsText(view.highRt); tone: view.theme.auroraViolet }
        Readout { band: "PRE"; value: view.secondsText(view.predelay); tone: view.theme.star }
        Readout {
            visible: view.shimmer > 0.001
            band: "SHIMMER"
            value: (view.shimmerRatio >= 1 ? "+" : "−")
                + Math.round(Math.abs(12 * Math.log2(view.shimmerRatio))) + " st"
            tone: view.theme.auroraMagenta
        }
    }
    Text {
        objectName: "expanseLandscapeCaption"
        anchors.right: parent.right
        anchors.rightMargin: view.padRight
        y: view.theme.spaceSm
        text: view.frozen ? "FROZEN · HOLDING"
            : view.live ? "LIVE · LATE " + Math.round(view.lateDb) + " dB"
            : "FROM SETTINGS"
        color: view.frozen ? view.theme.ice : view.live ? view.theme.aurora : view.theme.inkFaint
        font.pixelSize: view.theme.typeMicro
        font.bold: true
        font.letterSpacing: view.theme.captionSpacing
    }
}
