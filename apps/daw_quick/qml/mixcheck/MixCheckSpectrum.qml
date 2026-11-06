pragma ComponentBehavior: Bound
import QtQuick

// The mix's spectrum, 20 Hz – 20 kHz on a log axis with a dB grid: grey
// "before", white "after" once a fix is applied, the mono fold when asked
// for, and each finding's band shaded and labelled along the top (red bad,
// amber worth a look, teal fixed). Clicking a band's marker reports it.
Canvas {
    id: spectrum
    required property MixCheckTheme style
    // { freqs: [], before: [], after: [], mono: [] } in dB.
    property var curves: ({})
    property var findings: []
    property bool monoView: false
    property string hotId: ""
    signal markerClicked(string id)

    readonly property real minimumFrequency: 20
    readonly property real maximumFrequency: 20000
    readonly property var gridFrequencies: [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
    readonly property int axisHeight: 18
    readonly property real dbSpan: 54
    readonly property real dbGridStep: 12
    readonly property int labelTop: 8
    readonly property int labelHeight: 17
    readonly property real labelCharWidth: 6.2
    // The plot's margins in the well, and the dB grid's reach under its top.
    readonly property int plotTop: 18
    readonly property int plotBottom: 22
    readonly property real dbGridSpan: 48
    // The axis labels: their column, the gap from their line, the bottom
    // row's baseline and the side labels' inset and lift.
    readonly property int axisLabelWidth: 22
    readonly property int axisLabelGap: 3
    readonly property int axisLabelBottom: 6
    readonly property int axisLabelInset: 4
    readonly property int axisLabelLift: 3
    // A band's least width and the strip across its top; a band label's
    // padding, its margins from the edges and its baseline in its box.
    readonly property int minBandWidth: 6
    readonly property int bandCapHeight: 3
    readonly property int labelPadding: 10
    readonly property int labelRightMargin: 2
    readonly property int labelLeftMargin: 4
    readonly property int labelBaseline: 12

    readonly property var freqs: style.list(style.map(curves).freqs)
    readonly property var before: style.list(style.map(curves).before)
    readonly property var after: style.list(style.map(curves).after)
    readonly property var mono: style.list(style.map(curves).mono)
    // The grid's top line: the loudest band plus headroom, rounded up to a
    // whole step; 0 when empty.
    readonly property real dbTopHeadroom: 3
    readonly property real dbTopStep: 6
    readonly property real dbTop: {
        let loudest = -Infinity
        for (const series of [before, after, mono]) {
            for (let i = 0; i < series.length; ++i) {
                const v = Number(series[i])
                if (isFinite(v) && v > loudest)
                    loudest = v
            }
        }
        return isFinite(loudest) ? Math.ceil((loudest + dbTopHeadroom) / dbTopStep) * dbTopStep : 0
    }

    function xOfFrequency(frequency) {
        return Math.log(Number(frequency) / minimumFrequency)
               / Math.log(maximumFrequency / minimumFrequency) * width
    }
    function yOfDb(db) {
        return height - plotBottom - (db - (dbTop - dbSpan)) / dbSpan * (height - plotTop - plotBottom)
    }
    // Each finding with a band: where its shading and its label sit.
    function markerLayout() {
        const result = []
        const all = style.list(findings)
        for (let i = 0; i < all.length; ++i) {
            const finding = all[i]
            const band = finding ? finding.band : null
            if (!band || !isFinite(Number(band.low)) || !isFinite(Number(band.high)))
                continue
            const x0 = Math.max(0, xOfFrequency(Math.max(minimumFrequency, Number(band.low))))
            const x1 = Math.max(x0 + minBandWidth, Math.min(width, xOfFrequency(Number(band.high))))
            const applied = finding.applied === true
            const name = style.text(band.label)
            const label = applied ? "✓ " + name : name
            const labelWidth = label.length * labelCharWidth + labelPadding
            const labelX = Math.min(width - labelWidth - labelRightMargin, Math.max(labelLeftMargin, (x0 + x1) / 2 - labelWidth / 2))
            result.push({ id: style.text(finding.id), x0: x0, x1: x1, label: label,
                          labelX: labelX, labelWidth: labelWidth, colour: style.findingColor(finding) })
        }
        return result
    }
    // The finding whose marker (band or label) is under x, nearest first.
    function markerAt(x) {
        let best = ""
        let bestDistance = Infinity
        for (const marker of markerLayout()) {
            const left = Math.min(marker.x0, marker.labelX)
            const right = Math.max(marker.x1, marker.labelX + marker.labelWidth)
            if (x < left || x > right)
                continue
            const distance = Math.abs(x - (marker.x0 + marker.x1) / 2)
            if (distance < bestDistance) {
                bestDistance = distance
                best = marker.id
            }
        }
        return best
    }

    onCurvesChanged: requestPaint()
    onFindingsChanged: requestPaint()
    onMonoViewChanged: requestPaint()
    onHotIdChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    Accessible.role: Accessible.Graphic
    Accessible.name: "Spectrum of the mix with findings marked"

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const w = width
        const h = height
        if (w <= 0 || h <= 0)
            return
        const style = spectrum.style

        // Grid: frequencies along the bottom, decibels up the side.
        ctx.font = style.typeSmall + "px \"" + style.monoFamily + "\""
        ctx.lineWidth = style.hairline
        ctx.strokeStyle = style.line
        ctx.fillStyle = style.textFaint
        for (const f of gridFrequencies) {
            const x = Math.round(xOfFrequency(f)) + style.hairline / 2
            ctx.beginPath()
            ctx.moveTo(x, 0)
            ctx.lineTo(x, h - axisHeight)
            ctx.stroke()
            ctx.fillText(style.hz(f), Math.min(w - axisLabelWidth, x + axisLabelGap), h - axisLabelBottom)
        }
        for (let db = dbTop - dbGridSpan; db <= dbTop; db += dbGridStep) {
            const y = Math.round(yOfDb(db)) + style.hairline / 2
            ctx.beginPath()
            ctx.moveTo(0, y)
            ctx.lineTo(w, y)
            ctx.stroke()
            ctx.fillText(db + " dB", axisLabelInset, y - axisLabelLift)
        }

        // Each finding's band, with its label along the top.
        ctx.font = "bold " + style.typeBody + "px \"" + style.sansFamily + "\""
        for (const marker of markerLayout()) {
            ctx.fillStyle = style.alpha(marker.colour, marker.id === hotId ? style.spectrumBandHotAlpha
                                                                         : style.spectrumBandAlpha)
            ctx.fillRect(marker.x0, 0, marker.x1 - marker.x0, h - axisHeight)
            ctx.fillStyle = marker.colour
            ctx.fillRect(marker.x0, 0, marker.x1 - marker.x0, bandCapHeight)
            ctx.fillStyle = style.labelBox
            ctx.fillRect(marker.labelX, labelTop, marker.labelWidth, labelHeight)
            ctx.strokeStyle = marker.colour
            ctx.strokeRect(marker.labelX + style.hairline / 2, labelTop + style.hairline / 2,
                           marker.labelWidth - style.hairline, labelHeight - style.hairline)
            const measured = ctx.measureText(marker.label).width
            ctx.fillStyle = marker.colour
            ctx.fillText(marker.label, marker.labelX + (marker.labelWidth - measured) / 2, labelTop + labelBaseline)
        }

        // The curves.
        function path(series, colour, lineWidth) {
            const count = Math.min(freqs.length, series.length)
            let started = false
            ctx.beginPath()
            for (let i = 0; i < count; ++i) {
                const f = Number(freqs[i])
                const v = Number(series[i])
                if (!(f > 0) || !isFinite(v))
                    continue
                const x = xOfFrequency(f)
                const y = yOfDb(v)
                if (started) {
                    ctx.lineTo(x, y)
                } else {
                    ctx.moveTo(x, y)
                    started = true
                }
            }
            ctx.strokeStyle = colour
            ctx.lineWidth = lineWidth
            ctx.stroke()
        }
        path(before, style.beforeCurve, style.spectrumBeforeWidth)
        if (after.length > 0)
            path(after, style.afterCurve, style.spectrumAfterWidth)
        if (monoView && mono.length > 0)
            path(mono, style.blue, style.spectrumMonoWidth)
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: spectrum.markerAt(mouseX).length > 0 ? Qt.PointingHandCursor : Qt.CrossCursor
        onClicked: function(mouse) {
            const id = spectrum.markerAt(mouse.x)
            if (id.length > 0)
                spectrum.markerClicked(id)
        }
    }
}
