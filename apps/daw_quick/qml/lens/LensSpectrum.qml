pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts

// Lens's hero: the spectrum on a log frequency axis (20 Hz to 20 kHz,
// labelled decades) and a dB axis whose range and ceiling the panel sets.
// The first curve glows over a filled gradient; the second (Mid/Side,
// Left/Right) is a warm line; the peak hold is a fine white line, the
// sidechain overlay a dashed violet, and the difference (first curve minus
// overlay) a gold line on its own ±12 dB scale about the plot's middle.
// Resonances are marked with their notes; hovering reads frequency, note,
// cents and level at the cursor.
Item {
    id: spectrum
    required property LensTheme theme
    property bool live: false
    // 400 points each, in dB (-200 means silent); peaks as [{hz, db}].
    property var curveA: []
    property var curveB: []
    property var overlay: []
    property var hold: []
    property var peaks: []
    property bool showB: false
    property bool showOverlay: false
    property bool showHold: true
    property bool showDifference: false
    property real topDb: 0
    property real rangeDb: 90

    readonly property int points: 400
    readonly property real lowHz: 20
    readonly property real highHz: 20000
    readonly property real bottomDb: topDb - rangeDb
    // Difference: A minus overlay, where both sound; NaN elsewhere.
    readonly property var difference: {
        const out = []
        const n = Math.min(curveA.length, overlay.length)
        for (let j = 0; j < n; ++j) {
            out.push(curveA[j] > -199 && overlay[j] > -199 ? curveA[j] - overlay[j] : NaN)
        }
        return out
    }
    readonly property real differenceRange: 12
    // The difference scale spans this fraction of the plot height each way.
    readonly property real differenceSpan: 0.42
    // The overlay's dash, in points.
    readonly property int overlayDash: 3
    // Resonances whose note is lettered.
    readonly property int notedPeaks: 5

    // Hover: -1 when the cursor is away.
    property real hoverX: -1
    readonly property real hoverHz: hoverX < 0 ? 0 : hzOf(hoverX)
    readonly property int hoverPoint: hoverX < 0 ? -1 : pointAt(hoverX)
    readonly property string hoverText: hoverPoint < 0 ? "" : readoutAt(hoverPoint, hoverHz)

    // Plot area.
    readonly property real px: theme.spectrumScaleWidth
    readonly property real py: theme.spectrumTop
    readonly property real pw: width - px - (showDifference ? theme.differenceScaleWidth : theme.spectrumRightPad)
    readonly property real ph: height - py - theme.spectrumAxisHeight

    objectName: "lensSpectrum"
    implicitWidth: theme.spectrumWidth
    implicitHeight: theme.spectrumHeight

    function pointHz(j) {
        return lowHz * Math.pow(highHz / lowHz, j / (points - 1))
    }
    function pointAt(x) {
        const hz = hzOf(x)
        return Math.max(0, Math.min(points - 1, Math.round((points - 1) * Math.log(hz / lowHz) / Math.log(highHz / lowHz))))
    }
    function xOf(hz) {
        return px + pw * Math.log(Math.max(lowHz, Math.min(highHz, hz)) / lowHz) / Math.log(highHz / lowHz)
    }
    function hzOf(x) {
        return lowHz * Math.pow(highHz / lowHz, Math.max(0, Math.min(1, (x - px) / pw)))
    }
    function yOf(db) {
        return py + ph * (topDb - Math.max(bottomDb, Math.min(topDb, db))) / rangeDb
    }
    function yOfDifference(d) {
        const c = py + ph / 2
        return c - (ph * differenceSpan) * Math.max(-1, Math.min(1, d / differenceRange))
    }
    // Equal temperament, A4 = 440 Hz: the nearest note and the cents off it.
    function note(hz) {
        if (!(hz > 0))
            return { name: "", cents: 0, midi: 0 }
        const midi = Contracts.midi_of_hz(hz)
        const nearest = Math.round(midi)
        const cents = Math.round(100 * (midi - nearest))
        return { name: Contracts.note_name(nearest, Contracts.sharp, Contracts.scientific_middle_c),
                 cents: cents, midi: nearest }
    }
    function centsText(cents) {
        return (cents > 0 ? "+" : cents < 0 ? "−" : "±") + Math.abs(cents) + "c"
    }
    function noteText(hz) {
        const n = note(hz)
        return n.name === "" ? "" : n.name + " " + centsText(n.cents)
    }
    function hzText(hz) {
        if (hz >= 10000)
            return (hz / 1000).toFixed(1) + " kHz"
        if (hz >= 1000)
            return (hz / 1000).toFixed(2) + " kHz"
        return hz >= 100 ? Math.round(hz) + " Hz" : hz.toFixed(1) + " Hz"
    }
    function dbText(db) {
        return db <= -199 ? "−∞" : (db > 0.05 ? "+" : db < -0.05 ? "−" : "") + Math.abs(db).toFixed(1) + " dB"
    }
    function readoutAt(j, hz) {
        let text = hzText(hz) + "   " + noteText(hz)
        if (curveA.length > j)
            text += "   " + dbText(curveA[j])
        if (showB && curveB.length > j)
            text += " / " + dbText(curveB[j])
        if (showDifference && difference.length > j && !isNaN(difference[j]))
            text += "   Δ " + dbText(difference[j])
        return text
    }

    onCurveAChanged: plot.requestPaint()
    onCurveBChanged: plot.requestPaint()
    onOverlayChanged: plot.requestPaint()
    onHoldChanged: plot.requestPaint()
    onPeaksChanged: plot.requestPaint()
    onShowBChanged: plot.requestPaint()
    onShowOverlayChanged: plot.requestPaint()
    onShowHoldChanged: plot.requestPaint()
    onShowDifferenceChanged: plot.requestPaint()
    onTopDbChanged: plot.requestPaint()
    onRangeDbChanged: plot.requestPaint()
    onHoverXChanged: plot.requestPaint()
    onWidthChanged: plot.requestPaint()
    onHeightChanged: plot.requestPaint()

    Canvas {
        id: plot
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        // The curve through every point, or dashed (every other run of
        // `dash` points drawn).
        function trace(ctx, values, dash) {
            let started = false
            for (let j = 0; j < values.length; ++j) {
                const v = values[j]
                const drawn = v > -199 && !isNaN(v) && (dash === 0 || Math.floor(j / dash) % 2 === 0)
                if (!drawn) {
                    started = false
                    continue
                }
                const x = spectrum.xOf(spectrum.pointHz(j))
                const y = spectrum.yOf(v)
                if (!started) {
                    ctx.moveTo(x, y)
                    started = true
                } else {
                    ctx.lineTo(x, y)
                }
            }
        }
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = spectrum.theme
            const left = spectrum.px
            const right = spectrum.px + spectrum.pw
            const top = spectrum.py
            const bottom = spectrum.py + spectrum.ph
            // The well: a faint glow toward the top.
            const well = ctx.createLinearGradient(0, top, 0, bottom)
            well.addColorStop(0, t.wellGlow)
            well.addColorStop(1, t.well)
            ctx.fillStyle = well
            ctx.fillRect(left, top, spectrum.pw, spectrum.ph)

            // Frequency grid: every 1-2-5, decades and their labels strong.
            ctx.lineWidth = t.hairline
            ctx.font = t.typeCaption + "px '" + t.mono + "'"
            ctx.textBaseline = "top"
            ctx.textAlign = "center"
            for (let decade = 10; decade <= 10000; decade *= 10) {
                for (let m = 1; m <= 9; ++m) {
                    const hz = decade * m
                    if (hz < spectrum.lowHz || hz > spectrum.highHz)
                        continue
                    const x = Math.round(spectrum.xOf(hz)) + t.hairline / 2
                    ctx.strokeStyle = m === 1 ? t.gridStrong : t.grid
                    ctx.beginPath()
                    ctx.moveTo(x, top)
                    ctx.lineTo(x, bottom)
                    ctx.stroke()
                    if (m === 1 || m === 2 || m === 5) {
                        ctx.fillStyle = m === 1 ? t.inkDim : t.gridText
                        ctx.fillText(hz >= 1000 ? (hz / 1000) + "k" : String(hz), x, bottom + t.spaceSm)
                    }
                }
            }

            // Level grid.
            const step = spectrum.rangeDb <= 60 ? 6 : spectrum.rangeDb <= 90 ? 10 : 12
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            const first = Math.floor(spectrum.topDb / step) * step
            for (let db = first; db >= spectrum.bottomDb; db -= step) {
                const y = Math.round(spectrum.yOf(db)) + t.hairline / 2
                ctx.strokeStyle = db === 0 ? t.gridStrong : t.grid
                ctx.beginPath()
                ctx.moveTo(left, y)
                ctx.lineTo(right, y)
                ctx.stroke()
                ctx.fillStyle = db === 0 ? t.inkDim : t.gridText
                ctx.fillText(String(db), left - t.spaceSm, y)
            }

            // The overlay first, so the live curve reads over it.
            if (spectrum.showOverlay && spectrum.overlay.length > 0) {
                ctx.strokeStyle = t.overlay
                ctx.globalAlpha = t.overlayAlpha
                ctx.lineWidth = t.overlayStroke
                ctx.beginPath()
                trace(ctx, spectrum.overlay, spectrum.overlayDash)
                ctx.stroke()
                ctx.globalAlpha = 1
            }

            // Curve A: a filled gradient, a soft glow and a bright line.
            const a = spectrum.curveA
            if (a.length > 0) {
                ctx.beginPath()
                let first = true
                let lastX = left
                for (let j = 0; j < a.length; ++j) {
                    const x = spectrum.xOf(spectrum.pointHz(j))
                    const y = spectrum.yOf(a[j] > -199 ? a[j] : spectrum.bottomDb)
                    if (first) {
                        ctx.moveTo(x, bottom)
                        ctx.lineTo(x, y)
                        first = false
                    } else {
                        ctx.lineTo(x, y)
                    }
                    lastX = x
                }
                ctx.lineTo(lastX, bottom)
                ctx.closePath()
                const fill = ctx.createLinearGradient(0, top, 0, bottom)
                fill.addColorStop(0, Qt.rgba(t.primary.r, t.primary.g, t.primary.b, t.primaryFillTopAlpha))
                fill.addColorStop(t.primaryFillMidStop,
                                  Qt.rgba(t.primaryDeep.r, t.primaryDeep.g, t.primaryDeep.b, t.primaryFillMidAlpha))
                fill.addColorStop(1, Qt.rgba(t.primaryDeep.r, t.primaryDeep.g, t.primaryDeep.b, t.primaryFillBottomAlpha))
                ctx.fillStyle = fill
                ctx.fill()
                ctx.lineJoin = "round"
                ctx.strokeStyle = Qt.rgba(t.primary.r, t.primary.g, t.primary.b, t.primaryGlowAlpha)
                ctx.lineWidth = t.primaryGlowWidth
                ctx.beginPath()
                trace(ctx, a, 0)
                ctx.stroke()
                ctx.strokeStyle = t.primary
                ctx.lineWidth = t.primaryStroke
                ctx.beginPath()
                trace(ctx, a, 0)
                ctx.stroke()
            }

            // Curve B.
            if (spectrum.showB && spectrum.curveB.length > 0) {
                ctx.strokeStyle = Qt.rgba(t.secondary.r, t.secondary.g, t.secondary.b, t.secondaryGlowAlpha)
                ctx.lineWidth = t.secondaryGlowWidth
                ctx.beginPath()
                trace(ctx, spectrum.curveB, 0)
                ctx.stroke()
                ctx.strokeStyle = t.secondary
                ctx.lineWidth = t.secondaryStroke
                ctx.beginPath()
                trace(ctx, spectrum.curveB, 0)
                ctx.stroke()
            }

            // The peak hold.
            if (spectrum.showHold && spectrum.hold.length > 0) {
                ctx.strokeStyle = Qt.rgba(t.hold.r, t.hold.g, t.hold.b, t.holdAlpha)
                ctx.lineWidth = t.hairline
                ctx.beginPath()
                trace(ctx, spectrum.hold, 0)
                ctx.stroke()
            }

            // The difference, on its own scale about the middle.
            if (spectrum.showDifference && spectrum.difference.length > 0) {
                const mid = Math.round(spectrum.yOfDifference(0)) + t.hairline / 2
                ctx.strokeStyle = Qt.rgba(t.difference.r, t.difference.g, t.difference.b, t.differenceAxisAlpha)
                ctx.lineWidth = t.hairline
                for (let x = left; x < right; x += t.differenceDashPeriod) {
                    ctx.beginPath()
                    ctx.moveTo(x, mid)
                    ctx.lineTo(Math.min(right, x + t.differenceDashLength), mid)
                    ctx.stroke()
                }
                ctx.strokeStyle = t.difference
                ctx.lineWidth = t.differenceStroke
                ctx.beginPath()
                let started = false
                for (let j = 0; j < spectrum.difference.length; ++j) {
                    const d = spectrum.difference[j]
                    if (isNaN(d)) {
                        started = false
                        continue
                    }
                    const x = spectrum.xOf(spectrum.pointHz(j))
                    const y = spectrum.yOfDifference(d)
                    if (!started) {
                        ctx.moveTo(x, y)
                        started = true
                    } else {
                        ctx.lineTo(x, y)
                    }
                }
                ctx.stroke()
                ctx.fillStyle = t.difference
                ctx.textAlign = "left"
                ctx.textBaseline = "middle"
                ctx.font = t.typeCaption + "px '" + t.mono + "'"
                const range = spectrum.differenceRange
                for (const d of [range, range / 2, 0, -range / 2, -range]) {
                    ctx.fillText((d > 0 ? "+" : d < 0 ? "−" : "") + Math.abs(d), right + t.spaceSm, spectrum.yOfDifference(d))
                }
                ctx.fillText("Δ dB", right + t.spaceSm, top + t.spaceXs)
            }

            // Resonances: a marker on each, notes on the strongest five.
            ctx.font = "bold " + t.typeCaption + "px '" + t.sans + "'"
            ctx.textAlign = "center"
            ctx.textBaseline = "bottom"
            for (let k = 0; k < spectrum.peaks.length; ++k) {
                const peak = spectrum.peaks[k]
                if (!(peak.hz > 0))
                    continue
                const x = spectrum.xOf(peak.hz)
                const y = Math.max(top + t.spaceXs, spectrum.yOf(peak.db) - t.resonanceLift)
                ctx.fillStyle = t.resonance
                ctx.beginPath()
                ctx.moveTo(x - t.resonanceMarkHalfWidth, y - t.resonanceMarkHeight)
                ctx.lineTo(x + t.resonanceMarkHalfWidth, y - t.resonanceMarkHeight)
                ctx.lineTo(x, y - 1)
                ctx.closePath()
                ctx.fill()
                if (k < spectrum.notedPeaks) {
                    ctx.fillStyle = t.ink
                    ctx.fillText(spectrum.note(peak.hz).name, x, y - t.resonanceLabelLift)
                }
            }

            // The hover crosshair.
            if (spectrum.hoverPoint >= 0) {
                const x = Math.round(spectrum.hoverX) + t.hairline / 2
                ctx.strokeStyle = Qt.rgba(t.glacier.r, t.glacier.g, t.glacier.b, t.crosshairAlpha)
                ctx.lineWidth = t.hairline
                ctx.beginPath()
                ctx.moveTo(x, top)
                ctx.lineTo(x, bottom)
                ctx.stroke()
                if (a.length > spectrum.hoverPoint && a[spectrum.hoverPoint] > -199) {
                    ctx.fillStyle = t.glacier
                    ctx.beginPath()
                    ctx.arc(x, spectrum.yOf(a[spectrum.hoverPoint]), t.crosshairDotRadius, 0, Math.PI * 2)
                    ctx.fill()
                }
            }
            ctx.strokeStyle = t.wellEdge
            ctx.lineWidth = t.hairline
            ctx.strokeRect(left + t.hairline / 2, top + t.hairline / 2, spectrum.pw - t.hairline, spectrum.ph - t.hairline)
        }
    }

    // The cursor readout, top right of the plot.
    Rectangle {
        visible: spectrum.hoverPoint >= 0
        x: spectrum.px + spectrum.pw - width - spectrum.theme.spaceMd
        y: spectrum.py + spectrum.theme.spaceMd
        width: readout.implicitWidth + spectrum.theme.cursorReadoutPad
        height: spectrum.theme.cursorReadoutHeight
        radius: spectrum.theme.radiusMd
        color: spectrum.theme.cursorReadoutFill
        border.color: spectrum.theme.plateHi
        Text {
            id: readout
            objectName: "lensCursorReadout"
            anchors.centerIn: parent
            text: spectrum.hoverText
            color: spectrum.theme.ink
            font.family: spectrum.theme.mono
            font.pixelSize: spectrum.theme.typeBody
        }
    }

    MouseArea {
        objectName: "lensSpectrumHover"
        x: spectrum.px
        y: spectrum.py
        width: spectrum.pw
        height: spectrum.ph
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onPositionChanged: mouse => { spectrum.hoverX = spectrum.px + mouse.x }
        onExited: spectrum.hoverX = -1
    }
}
