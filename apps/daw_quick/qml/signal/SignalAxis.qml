import QtQuick

// The LCD's frequency strip: a log axis from 20 Hz to 20 kHz with decade
// lines. A tone stands as a marker at its frequency; a sweep fills from
// 20 Hz to where it is now, with a cursor; noise and impulses draw their
// spectrum's slope per octave (pink level, white and impulses rising
// 3 dB an octave in octave bands).
Canvas {
    id: axis
    required property SignalTheme theme
    property int wave: 0
    property bool on: true
    // The tone's frequency or the sweep's position now (Hz), 0 when none.
    property real hz: 0
    property real progress: 0
    property bool live: false
    readonly property real lowHz: 20
    readonly property real highHz: 20000
    // Grid lines sit on pixel centres.
    readonly property real pixelCentre: 0.5
    // The tone's glow peaks at its centre.
    readonly property real glowCentreStop: 0.5
    // The sweep's band, as shares of the strip's height: its top and depth.
    readonly property real sweepBandTop: 0.3
    readonly property real sweepBandDepth: 0.4
    // The noise slope's 30 dB span, as shares of the strip's height: the
    // margin above it and the height it fills.
    readonly property real slopeMargin: 0.15
    readonly property real slopeSpan: 0.7

    function at(f) {
        return Math.log(Math.max(lowHz, Math.min(highHz, f)) / lowHz) / Math.log(highHz / lowHz)
    }

    renderStrategy: Canvas.Cooperative
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const t = theme
        const w = width
        const top = t.axisTop
        const bottom = height - t.axisLabelBand
        const h = bottom - top
        const ink = on ? t.lcdInk : t.lcdDim
        // Decades and their halves.
        ctx.lineWidth = t.hairline
        ctx.font = t.typeCaption + "px " + t.mono
        ctx.textAlign = "center"
        const marks = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
        for (const f of marks) {
            const x = Math.round(at(f) * (w - 1)) + pixelCentre
            const decade = f === 100 || f === 1000 || f === 10000
            ctx.strokeStyle = decade ? t.lcdGridStrong : t.lcdGrid
            ctx.beginPath()
            ctx.moveTo(x, top)
            ctx.lineTo(x, bottom)
            ctx.stroke()
            ctx.fillStyle = on ? t.lcdDim : t.lcdGhost
            ctx.textAlign = f === 20 ? "left" : f === 20000 ? "right" : "center"
            ctx.fillText(f >= 1000 ? (f / 1000) + "k" : "" + f, x, height - t.axisLabelBaseline)
        }
        ctx.strokeStyle = t.lcdGrid
        ctx.beginPath()
        ctx.moveTo(0, bottom + pixelCentre)
        ctx.lineTo(w, bottom + pixelCentre)
        ctx.stroke()
        if (!on)
            return

        if (wave === 0 && hz > 0) {
            const x = at(hz) * (w - 1)
            const half = t.toneGlowHalfWidth
            const glow = ctx.createLinearGradient(x - half, 0, x + half, 0)
            glow.addColorStop(0, Qt.rgba(t.lcdGlow.r, t.lcdGlow.g, t.lcdGlow.b, 0))
            glow.addColorStop(glowCentreStop, Qt.rgba(t.lcdGlow.r, t.lcdGlow.g, t.lcdGlow.b, t.toneGlowAlpha))
            glow.addColorStop(1, Qt.rgba(t.lcdGlow.r, t.lcdGlow.g, t.lcdGlow.b, 0))
            ctx.fillStyle = glow
            ctx.fillRect(x - half, top, 2 * half, h)
            ctx.strokeStyle = ink
            ctx.lineWidth = t.strokeBold
            ctx.beginPath()
            ctx.moveTo(x, top)
            ctx.lineTo(x, bottom)
            ctx.stroke()
        } else if (wave === 1) {
            const x = (live ? at(hz) : 0) * (w - 1)
            const fill = ctx.createLinearGradient(0, 0, x, 0)
            fill.addColorStop(0, Qt.rgba(t.lcdGlow.r, t.lcdGlow.g, t.lcdGlow.b, t.sweepFillStartAlpha))
            fill.addColorStop(1, Qt.rgba(t.lcdGlow.r, t.lcdGlow.g, t.lcdGlow.b, t.sweepFillEndAlpha))
            ctx.fillStyle = fill
            ctx.fillRect(0, top + h * sweepBandTop, x, h * sweepBandDepth)
            if (live) {
                ctx.strokeStyle = ink
                ctx.lineWidth = t.strokeBold
                ctx.beginPath()
                ctx.moveTo(x, top)
                ctx.lineTo(x, bottom)
                ctx.stroke()
            }
        } else if (wave >= 2) {
            // Level in octave bands, relative: pink flat; white and impulses
            // rise 3 dB an octave. Drawn over a 30 dB span.
            const rise = wave === 2 ? 0 : 3
            const octaves = Math.log2(highHz / lowHz)
            const yOf = db => top + h * (slopeMargin + (1 - (db + 30) / 30) * slopeSpan)
            const start = wave === 2 ? -12 : -30
            ctx.strokeStyle = ink
            ctx.lineWidth = t.strokeBold
            ctx.beginPath()
            ctx.moveTo(0, yOf(start))
            ctx.lineTo(w, yOf(start + rise * octaves))
            ctx.stroke()
            ctx.lineTo(w, bottom)
            ctx.lineTo(0, bottom)
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(t.lcdGlow.r, t.lcdGlow.g, t.lcdGlow.b, t.slopeFillAlpha)
            ctx.fill()
            ctx.fillStyle = t.lcdDim
            ctx.textAlign = "left"
            ctx.fillText("LEVEL IN OCTAVE BANDS", t.axisCaptionX, top + t.axisCaptionY)
        }
    }
    onWaveChanged: requestPaint()
    onOnChanged: requestPaint()
    onHzChanged: requestPaint()
    onLiveChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
}
