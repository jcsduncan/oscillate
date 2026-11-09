import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The forge itself: the static transfer curve, input along the bottom and
// output up the side, drawn as molten metal whose colour and glow follow the
// heat (drive). Live, the input's peak level rides the curve as two sparks
// and the forge-light below breathes with the output.
Item {
    id: view
    required property ForgeTheme theme
    // Outputs for inputs evenly spaced over -1..1.
    property var points: []
    property real heat: 0.3
    property bool live: false
    // Peak input level (linear) and output RMS, while live.
    property real inputPeak: 0
    property real outputLevel: 0
    property string modelName: ""
    readonly property real range: 1.25
    // A finer sketch of the same curve for the wave and harmonic analysis
    // (a -6 dBFS sine through it); falls back to `points`.
    property var analysis: []
    readonly property real probe: 0.5
    readonly property int waveSamples: 128
    // One cycle out, and harmonics 1..9 in dBFS.
    readonly property var wave: analyse().wave
    readonly property var harmonics: analyse().harmonics
    readonly property int harmonicCount: 9

    // The screen's layout: the room round the unit square and beside it, the
    // square's share of the width and the extra room under it for its legend.
    readonly property real pad: 22
    readonly property real squareShare: 0.42
    readonly property real squareFoot: 6
    // The forge-light's centre and reach, as shares of the height.
    readonly property real lightCentre: 1.05
    readonly property real lightReach: 1.1
    // How far the output level opens the forge-light.
    readonly property real breathGain: 3
    readonly property int gridQuarters: 4
    // Below this input peak no sparks are drawn.
    readonly property real sparkFloor: 0.0005
    // The side readouts: their caption baseline, the gap above their boxes,
    // the narrowest they are drawn, and the harmonics box's gap from the square.
    readonly property real sideCaptionY: 16
    readonly property real sideTopGap: 14
    readonly property real sideMinWidth: 60
    readonly property real harmonicsGap: 12
    // The wave box: its trace's inset and the level its height spans.
    readonly property real waveInset: 4
    readonly property real waveSpan: 1.2
    // The harmonics box: its floor, its marked levels and their labels' offset,
    // each bar's start and width as shares of its slot, its halo and its
    // number's drop.
    readonly property real harmonicFloorDb: -72
    readonly property var harmonicMarksDb: [0, -24, -48]
    readonly property real markLabelOffset: 3
    readonly property real barStart: 0.22
    readonly property real barShare: 0.56
    readonly property real barHalo: 2
    readonly property real barNumberDrop: 10

    // The transfer square inside the canvas (which sits a hairline in from
    // the well): its side and its top-left corner, in canvas pixels.
    readonly property real canvasWidth: width - 2 * theme.hairline
    readonly property real canvasHeight: height - 2 * theme.hairline
    readonly property real squareSize: Math.max(0, Math.min(canvasWidth * squareShare,
                                                            canvasHeight - 2 * pad - squareFoot))
    readonly property real squareX: (canvasWidth - squareSize) / 2
    readonly property real squareY: (canvasHeight - squareSize) / 2
    // The square's scale for its frame: input across and output up, in
    // full scale (FS: ±1 is a full-scale sample), zero first so it carries
    // the unit.
    readonly property var levelMarks: [[0, "0"], [-1, "−1"], [1, "+1"]]
    readonly property var levelTicks: levelMarks.map(mark => ({ at: (mark[0] / range + 1) / 2, label: mark[1] }))

    objectName: "forgeCurveView"
    Accessible.role: Accessible.Graphic
    Accessible.name: "Transfer curve, " + modelName

    function curveAt(x) {
        const n = points.length
        if (n < 2)
            return 0
        const position = (Math.max(-1, Math.min(1, x)) + 1) / 2 * (n - 1)
        const i = Math.min(n - 2, Math.floor(position))
        const f = position - i
        return points[i] + (points[i + 1] - points[i]) * f
    }

    function sampleAt(table, x) {
        const n = table.length
        if (n < 2)
            return 0
        const position = (Math.max(-1, Math.min(1, x)) + 1) / 2 * (n - 1)
        const i = Math.min(n - 2, Math.floor(position))
        const f = position - i
        return table[i] + (table[i + 1] - table[i]) * f
    }
    function analyse() {
        const table = analysis.length >= 2 ? analysis : points
        const wave = []
        for (let i = 0; i < waveSamples; ++i)
            wave.push(sampleAt(table, probe * Math.sin(2 * Math.PI * i / waveSamples)))
        const harmonics = []
        for (let k = 1; k <= harmonicCount; ++k) {
            let re = 0
            let im = 0
            for (let i = 0; i < waveSamples; ++i) {
                const a = 2 * Math.PI * k * i / waveSamples
                re += wave[i] * Math.cos(a)
                im -= wave[i] * Math.sin(a)
            }
            const amplitude = 2 * Math.sqrt(re * re + im * im) / waveSamples
            harmonics.push(amplitude > 1e-7 ? 20 * Math.log(amplitude) / Math.LN10 : -140)
        }
        return { wave: wave, harmonics: harmonics }
    }

    onPointsChanged: canvas.requestPaint()
    onAnalysisChanged: canvas.requestPaint()
    onHeatChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onInputPeakChanged: canvas.requestPaint()
    onOutputLevelChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: view.theme.wellRadius
        color: view.theme.well
        border.color: view.theme.wellEdge
    }

    // The transfer square's frame: input along the bottom, output up the
    // side; LIVE while the input rides the curve, else a PREVIEW of the
    // settings. The canvas spans the whole well (the wave and harmonics
    // beside the square too), placed back at the well's origin from
    // inside the frame.
    Kit.PlotFrame {
        id: frame
        objectName: "forgeCurveFrame"
        x: view.theme.hairline + view.squareX - view.theme.plotGutterLeft
        y: view.theme.hairline + view.squareY
        width: view.squareSize + view.theme.plotGutterLeft
        height: view.squareSize + view.theme.plotGutterBottom
        theme: view.theme
        live: view.live
        xUnit: "FS"
        yUnit: "FS"
        xTicks: view.levelTicks
        yTicks: view.levelTicks

        Canvas {
            id: canvas
            x: -view.squareX
            y: -view.squareY
            width: view.canvasWidth
            height: view.canvasHeight
            renderStrategy: Canvas.Cooperative
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const t = view.theme
                const w = width
                const h = height
                const pad = view.pad
                const size = view.squareSize
                const ox = view.squareX
                const oy = view.squareY
                const sx = x => ox + (x / view.range + 1) / 2 * size
                const sy = y => oy + (1 - (y / view.range + 1) / 2) * size
                const hot = t.heat(view.heat)

                // Forge-light: a glow rising from below, brighter with heat and,
                // live, with the output.
                const breath = view.live ? Math.min(1, view.outputLevel * view.breathGain) : 0
                const glow = ctx.createRadialGradient(w / 2, h * view.lightCentre, 0,
                                                      w / 2, h * view.lightCentre, h * view.lightReach)
                glow.addColorStop(0, Qt.rgba(hot.r, hot.g * t.forgeLightGreen, hot.b * t.forgeLightBlue,
                    t.forgeLightAlpha + t.forgeLightHeatAlpha * view.heat + t.forgeLightBreathAlpha * breath))
                glow.addColorStop(t.forgeLightMidStop, Qt.rgba(hot.r, hot.g * t.forgeLightMidGreen, hot.b * t.forgeLightMidBlue,
                    t.forgeLightMidAlpha + t.forgeLightMidHeatAlpha * view.heat))
                glow.addColorStop(1, Qt.rgba(0, 0, 0, 0))
                ctx.fillStyle = glow
                ctx.fillRect(0, 0, w, h)

                // Grid: quarters, the unit square and the axes.
                ctx.lineWidth = t.hairline
                for (let k = -view.gridQuarters; k <= view.gridQuarters; ++k) {
                    const v = k / view.gridQuarters
                    ctx.strokeStyle = k === 0 ? t.gridStrong : t.grid
                    ctx.beginPath()
                    ctx.moveTo(sx(v), sy(-1))
                    ctx.lineTo(sx(v), sy(1))
                    ctx.moveTo(sx(-1), sy(v))
                    ctx.lineTo(sx(1), sy(v))
                    ctx.stroke()
                }
                ctx.strokeStyle = t.gridStrong
                ctx.strokeRect(sx(-1), sy(1), sx(1) - sx(-1), sy(-1) - sy(1))
                // Unity: what goes in comes out.
                ctx.setLineDash(t.unityDash)
                ctx.strokeStyle = Qt.rgba(t.inkFaint.r, t.inkFaint.g, t.inkFaint.b, t.unityAlpha)
                ctx.beginPath()
                ctx.moveTo(sx(-1), sy(-1))
                ctx.lineTo(sx(1), sy(1))
                ctx.stroke()
                ctx.setLineDash([])

                const n = view.points.length
                if (n < 2)
                    return
                const path = () => {
                    ctx.beginPath()
                    for (let i = 0; i < n; ++i) {
                        const x = -1 + 2 * i / (n - 1)
                        const y = Math.max(-view.range, Math.min(view.range, view.points[i]))
                        if (i === 0)
                            ctx.moveTo(sx(x), sy(y))
                        else
                            ctx.lineTo(sx(x), sy(y))
                    }
                }
                // Molten fill between the curve and zero.
                ctx.save()
                path()
                ctx.lineTo(sx(1), sy(0))
                ctx.lineTo(sx(-1), sy(0))
                ctx.closePath()
                const fill = ctx.createLinearGradient(0, sy(1), 0, sy(-1))
                fill.addColorStop(0, Qt.rgba(hot.r, hot.g, hot.b, t.moltenFillAlpha))
                fill.addColorStop(0.5, Qt.rgba(hot.r, hot.g, hot.b, t.moltenFillMidAlpha))
                fill.addColorStop(1, Qt.rgba(hot.r, hot.g, hot.b, t.moltenFillAlpha))
                ctx.fillStyle = fill
                ctx.fill()
                ctx.restore()
                // The curve: layered strokes for a glow, then a bright core.
                ctx.lineJoin = "round"
                ctx.lineCap = "round"
                for (const [width, alpha] of t.curveLayers) {
                    path()
                    ctx.strokeStyle = Qt.rgba(hot.r, hot.g, hot.b, alpha * (t.curveGlowBase + t.curveGlowHeat * view.heat))
                    ctx.lineWidth = width
                    ctx.stroke()
                }
                path()
                ctx.strokeStyle = Qt.rgba(t.whiteHot.r, t.whiteHot.g, t.whiteHot.b,
                    t.curveCoreAlpha + t.curveCoreHeatAlpha * view.heat)
                ctx.lineWidth = t.curveCoreStroke
                ctx.stroke()

                // Live: the input's peak as sparks on the curve.
                if (view.live && view.inputPeak > view.sparkFloor) {
                    for (const sign of [-1, 1]) {
                        const x = sign * Math.min(1, view.inputPeak)
                        const y = view.curveAt(x)
                        const px = sx(x)
                        const py = sy(Math.max(-view.range, Math.min(view.range, y)))
                        ctx.strokeStyle = Qt.rgba(t.amber.r, t.amber.g, t.amber.b, t.sparkLineAlpha)
                        ctx.lineWidth = t.hairline
                        ctx.setLineDash(t.sparkDash)
                        ctx.beginPath()
                        ctx.moveTo(px, sy(-1))
                        ctx.lineTo(px, sy(1))
                        ctx.stroke()
                        ctx.setLineDash([])
                        const spark = ctx.createRadialGradient(px, py, 0, px, py, t.sparkRadius)
                        spark.addColorStop(0, t.sparkCore)
                        spark.addColorStop(t.sparkMidStop, Qt.rgba(hot.r, hot.g, hot.b, t.sparkMidAlpha))
                        spark.addColorStop(1, Qt.rgba(hot.r, hot.g, hot.b, 0))
                        ctx.fillStyle = spark
                        ctx.beginPath()
                        ctx.arc(px, py, t.sparkRadius, 0, Math.PI * 2)
                        ctx.fill()
                    }
                }

                // Side readouts: one cycle through the curve, and its harmonics.
                const sideWidth = ox - 2 * pad - view.squareFoot
                const sideTop = oy + view.sideTopGap
                const sideHeight = size - view.sideTopGap
                if (sideWidth > view.sideMinWidth) {
                    ctx.font = "bold " + t.typeMicro + "px '" + t.sans + "'"
                    ctx.textAlign = "left"
                    ctx.fillStyle = t.inkFaint
                    ctx.fillText("WAVE · −6 dBFS SINE", pad, view.sideCaptionY)
                    ctx.fillText("HARMONICS · dBFS", ox + size + pad + view.harmonicsGap, view.sideCaptionY)
                    // Wave.
                    const wx = pad
                    const wy = sideTop + sideHeight / 2
                    const wh = sideHeight / 2 - view.waveInset
                    ctx.strokeStyle = t.grid
                    ctx.lineWidth = t.hairline
                    ctx.strokeRect(wx, sideTop, sideWidth, sideHeight)
                    ctx.beginPath()
                    ctx.moveTo(wx, wy)
                    ctx.lineTo(wx + sideWidth, wy)
                    ctx.stroke()
                    const wave = view.wave
                    const trace = (values, scale) => {
                        ctx.beginPath()
                        for (let i = 0; i <= values.length; ++i) {
                            const v = Math.max(-view.waveSpan, Math.min(view.waveSpan, values[i % values.length] * scale))
                            const px = wx + sideWidth * i / values.length
                            const py = wy - v * wh / view.waveSpan
                            if (i === 0)
                                ctx.moveTo(px, py)
                            else
                                ctx.lineTo(px, py)
                        }
                    }
                    const sine = []
                    for (let i = 0; i < wave.length; ++i)
                        sine.push(view.probe * Math.sin(2 * Math.PI * i / wave.length))
                    ctx.setLineDash(t.sineDash)
                    trace(sine, 1)
                    ctx.strokeStyle = Qt.rgba(t.inkDim.r, t.inkDim.g, t.inkDim.b, t.sineAlpha)
                    ctx.lineWidth = t.hairline
                    ctx.stroke()
                    ctx.setLineDash([])
                    for (const [width, alpha] of t.waveLayers) {
                        trace(wave, 1)
                        ctx.strokeStyle = Qt.rgba(hot.r, hot.g, hot.b, alpha)
                        ctx.lineWidth = width
                        ctx.stroke()
                    }
                    // Harmonics: odd in amber, even in copper.
                    const hx = ox + size + pad + view.harmonicsGap
                    const floor = view.harmonicFloorDb
                    ctx.strokeStyle = t.grid
                    ctx.lineWidth = t.hairline
                    ctx.strokeRect(hx, sideTop, sideWidth, sideHeight)
                    ctx.textAlign = "right"
                    ctx.font = t.typeMicro + "px '" + t.mono + "'"
                    for (const db of view.harmonicMarksDb) {
                        const py = sideTop + sideHeight * (db / floor)
                        ctx.strokeStyle = t.grid
                        ctx.beginPath()
                        ctx.moveTo(hx, py)
                        ctx.lineTo(hx + sideWidth, py)
                        ctx.stroke()
                        ctx.fillStyle = t.inkFaint
                        ctx.fillText(Format.number(db, 0), hx - view.markLabelOffset, py + view.markLabelOffset)
                    }
                    const bars = view.harmonics
                    const slot = sideWidth / bars.length
                    ctx.textAlign = "center"
                    for (let k = 0; k < bars.length; ++k) {
                        const level = Math.max(0, Math.min(1, 1 - bars[k] / floor))
                        const bh = (sideHeight - view.barHalo) * level
                        const bx = hx + slot * k + slot * view.barStart
                        const bw = slot * view.barShare
                        const colour = k % 2 === 0 ? t.heat(t.heatGlowBase + t.heatGlowRange * view.heat) : t.copper
                        if (bh > 0.5) {
                            ctx.fillStyle = Qt.rgba(colour.r, colour.g, colour.b, t.harmonicHaloAlpha)
                            ctx.fillRect(bx - view.barHalo, sideTop + sideHeight - bh - view.barHalo,
                                         bw + 2 * view.barHalo, bh + view.barHalo)
                            const bar = ctx.createLinearGradient(0, sideTop + sideHeight - bh, 0, sideTop + sideHeight)
                            bar.addColorStop(0, colour)
                            bar.addColorStop(1, Qt.rgba(colour.r, colour.g, colour.b, t.harmonicFootAlpha))
                            ctx.fillStyle = bar
                            ctx.fillRect(bx, sideTop + sideHeight - bh, bw, bh)
                        }
                        ctx.fillStyle = k % 2 === 0 ? t.inkDim : t.inkFaint
                        ctx.font = "bold " + t.scaleTextSize + "px '" + t.sans + "'"
                        ctx.fillText((k + 1).toString(), bx + bw / 2, sideTop + sideHeight + view.barNumberDrop)
                    }
                }
            }
        }
    }

    Text {
        objectName: "forgeCurveCaption"
        anchors.horizontalCenter: parent.horizontalCenter
        y: view.theme.spaceMd
        text: view.live
            ? "INPUT PEAK " + (view.inputPeak > 0.00001
                                      ? Format.quantity(20 * Math.log(view.inputPeak) / Math.LN10, "dBFS")
                                      : "SILENT")
            : "TRANSFER CURVE · " + view.modelName.toUpperCase()
        color: view.live ? view.theme.amber : view.theme.inkFaint
        font.pixelSize: view.theme.typeMicro
        font.bold: true
        font.letterSpacing: view.theme.labelSpacing
    }
}
