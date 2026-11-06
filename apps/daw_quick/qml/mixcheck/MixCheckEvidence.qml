pragma ComponentBehavior: Bound
import QtQuick

// A finding's evidence: the culprit's slice of spectrum, time or mono fold.
// Open, the measured curve is drawn in the finding's colour; applied, the
// "before" fades to grey under the re-checked curve in teal. Markers (a
// note's fundamental, a clip edge) are thin blue rules.
//   evidence: { kind: "spectrum"|"time"|"mono", x: [], before: [], after: [],
//               markers: [number | { x, label }], label? }
Rectangle {
    id: evidenceView
    required property MixCheckTheme style
    property var evidence: ({})
    property color colour: style.red
    property bool applied: false

    implicitHeight: style.evidenceHeight
    radius: style.radiusLg
    color: style.wellFill
    border.color: style.line

    readonly property var source: style.map(evidence)
    readonly property string kind: style.text(source.kind)
    readonly property var xs: style.list(source.x)
    readonly property var before: style.list(source.before)
    readonly property var after: style.list(source.after)
    readonly property var markers: style.list(source.markers)
    readonly property bool logAxis: kind === "spectrum" && xs.length > 1
                                    && Number(xs[0]) > 0 && Number(xs[xs.length - 1]) > Number(xs[0])
    readonly property string caption: {
        if (style.text(source.label).length > 0)
            return style.text(source.label)
        const range = xs.length > 1 ? style.hz(xs[0]) + "–" + style.hz(xs[xs.length - 1]) + " Hz" : ""
        if (kind === "spectrum")
            return "Spectrum " + range
        if (kind === "mono")
            return "Mono minus stereo" + (range.length > 0 ? ", " + range : "")
        if (kind === "time")
            return "Over time"
        return ""
    }

    onEvidenceChanged: plot.requestPaint()
    onAppliedChanged: plot.requestPaint()
    onColourChanged: plot.requestPaint()

    Accessible.role: Accessible.Graphic
    Accessible.name: caption

    Canvas {
        id: plot
        anchors.fill: parent
        anchors.margins: evidenceView.style.hairline
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const style = evidenceView.style
            const w = width
            const h = height
            ctx.font = style.typeSmall + "px \"" + style.monoFamily + "\""
            ctx.fillStyle = style.textFaint
            ctx.fillText(evidenceView.caption, style.spaceSm, style.plotLabelBaseline)
            const xs = evidenceView.xs
            if (xs.length < 2 || w <= 0 || h <= 0)
                return
            const first = Number(xs[0])
            const last = Number(xs[xs.length - 1])
            if (!(last > first))
                return
            const xOf = value => evidenceView.logAxis
                ? Math.log(Number(value) / first) / Math.log(last / first) * w
                : (Number(value) - first) / (last - first) * w

            let low = Infinity
            let high = -Infinity
            const series = [evidenceView.before, evidenceView.after]
            for (const values of series) {
                for (let i = 0; i < values.length; ++i) {
                    const v = Number(values[i])
                    if (isFinite(v)) {
                        low = Math.min(low, v)
                        high = Math.max(high, v)
                    }
                }
            }
            if (evidenceView.kind === "mono") {
                low = Math.min(low, 0)
                high = Math.max(high, 0)
            }
            if (!isFinite(low) || !isFinite(high))
                return
            if (high - low < 1e-6) {
                low -= 1
                high += 1
            }
            const yOf = value => h - style.evidencePlotBottom - (Number(value) - low) / (high - low)
                                 * (h - style.evidencePlotTop - style.evidencePlotBottom)

            if (evidenceView.kind === "mono") {
                ctx.fillStyle = style.lineStrong
                ctx.fillRect(0, Math.round(yOf(0)), w, style.hairline)
            }
            ctx.font = style.typeCaption + "px \"" + style.monoFamily + "\""
            for (let m = 0; m < evidenceView.markers.length; ++m) {
                const marker = evidenceView.markers[m]
                const at = typeof marker === "object" && marker !== null ? Number(marker.x) : Number(marker)
                if (!isFinite(at) || at < first || at > last)
                    continue
                const x = Math.round(xOf(at))
                ctx.fillStyle = style.blue
                ctx.fillRect(x, style.evidencePlotTop, style.hairline,
                             h - style.evidencePlotTop - style.evidenceMarkerBottom)
                const label = typeof marker === "object" && marker !== null && marker.label !== undefined
                    ? String(marker.label) : evidenceView.kind === "time" ? "" : style.hz(at)
                if (label.length > 0)
                    ctx.fillText(label, Math.min(w - style.evidenceMarkerLabelWidth, x + style.evidenceMarkerLabelGap),
                                 h - style.evidenceMarkerBottom)
            }
            function line(values, colour, lineWidth) {
                const count = Math.min(xs.length, values.length)
                let started = false
                ctx.beginPath()
                for (let i = 0; i < count; ++i) {
                    const v = Number(values[i])
                    const at = Number(xs[i])
                    if (!isFinite(v) || !isFinite(at) || (evidenceView.logAxis && !(at > 0)))
                        continue
                    const x = xOf(at)
                    const y = yOf(v)
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
            if (evidenceView.applied && evidenceView.after.length > 0) {
                line(evidenceView.before, style.beforeCurve, style.evidenceBeforeWidth)
                line(evidenceView.after, style.accent, style.evidenceCurveWidth)
            } else {
                line(evidenceView.before, evidenceView.colour, style.evidenceCurveWidth)
            }
        }
    }
}
