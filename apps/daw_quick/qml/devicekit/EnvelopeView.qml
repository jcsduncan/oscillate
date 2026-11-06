pragma ComponentBehavior: Bound
import QtQuick

// An envelope drawn as an outline over a fill in a framed well, for every
// device. A device gives its shape as `points` ([x, y], or [x, y, cx, cy]
// for a quadratic segment bent toward the control point cx, cy) or as
// `levels` (evenly spaced samples), normalised: x 0..1 across the plot, y
// 0..1 up it. Around the outline sit optional layers, painted in this
// order: a baseline, a shaded band from the left, a grid, dashed markers
// (under or over the outline), nodes and a playhead. The look comes from
// the theme's envelope tokens; a device adds its labels and drag handles as
// children. With `prefix` set (e.g. "ember.env") and no shape given, it
// draws that ADSR with segment widths following knob travel, so short and
// long times both read.
Rectangle {
    id: view
    property var controller: null
    required property KitTheme theme
    property string prefix: ""
    property color accent: theme.accent
    readonly property int revision: controller !== null ? controller.revision : 0

    // The shape.
    property var points: prefix !== "" && revision >= 0 ? travelPoints() : []
    property var levels: []
    // Dashed markers, all in one stroke: { x, from, to } with from and to
    // as levels; left out, a marker runs the plot's full height.
    property var markers: []
    // Nodes on the outline: [x, level] pairs.
    property var knots: []
    // The playhead's x (negative: none) and the level its dot rides.
    property real playhead: -1
    property real playheadLevel: 0
    // Where the shaded band ends (0: none) and the level the baseline marks.
    property real bandEnd: 0
    property real baselineLevel: 0

    property color lineColor: accent
    property color glowColor: Qt.rgba(accent.r, accent.g, accent.b, theme.envelopeGlowAlpha)
    property color fillColor: Qt.rgba(accent.r, accent.g, accent.b, theme.envelopeFillAlpha)
    property color fillEndColor: theme.envelopeFadeToClear ? "transparent"
        : Qt.rgba(accent.r, accent.g, accent.b, theme.envelopeFillEndAlpha)

    // The travel ADSR: the sustain's share of the width against the knobs'
    // travel, and where each segment's curve pulls (its control point's
    // share along the segment, and the level it reaches toward).
    readonly property real travelSustainShare: 0.6
    readonly property real travelAttackPull: 0.35
    readonly property real travelAttackKnee: 0.92
    readonly property real travelFallPull: 0.25
    readonly property real travelFallKnee: 0.15
    // Snaps a hairline onto the middle of a pixel row or column.
    readonly property real halfPixel: 0.5

    readonly property var scene: [points, levels, markers, knots, playhead, playheadLevel, bandEnd,
                                  baselineLevel, lineColor, glowColor, fillColor, fillEndColor]

    radius: theme.envelopeRadius
    color: theme.envelopeWell
    border.color: theme.envelopeEdge
    border.width: theme.envelopeEdgeWidth
    Accessible.role: Accessible.Graphic
    Accessible.name: "Envelope shape"

    onSceneChanged: plot.requestPaint()
    onOpacityChanged: plot.requestPaint()

    function travelPoints() {
        const a = controller.normalised(prefix + ".attack")
        const d = controller.normalised(prefix + ".decay")
        const s = controller.value(prefix + ".sustain")
        const r = controller.normalised(prefix + ".release")
        const total = a + d + travelSustainShare + r
        const x1 = a / total
        const x2 = x1 + d / total
        const x3 = x2 + travelSustainShare / total
        return [[0, 0],
                [x1, 1, x1 * travelAttackPull, travelAttackKnee],
                [x2, s, x1 + (x2 - x1) * travelFallPull, s + (1 - s) * travelFallKnee],
                [x3, s],
                [1, 0, x3 + (1 - x3) * travelFallPull, s * travelFallKnee]]
    }

    // A normalised position on the plot, in the view's coordinates.
    function plotX(f) {
        return plot.x + theme.envelopePlotSide + f * (plot.width - 2 * theme.envelopePlotSide)
    }
    function plotY(v) {
        const bottom = plot.height - theme.envelopePlotBottom
        return plot.y + bottom - v * (bottom - theme.envelopePlotTop - theme.envelopeHeadroom)
    }

    function paint(ctx, w, h) {
        ctx.reset()
        const t = theme
        const left = t.envelopePlotSide
        const right = w - t.envelopePlotSide
        const span = w - 2 * t.envelopePlotSide
        const top = t.envelopePlotTop
        const bottom = h - t.envelopePlotBottom
        const rise = bottom - top - t.envelopeHeadroom
        const X = f => left + f * span
        const Y = v => bottom - v * rise
        const snap = v => Math.round(v) + halfPixel
        const shape = levels.length > 0 ? levels.map((v, i) => [i / (levels.length - 1), v]) : points
        const drawn = shape.length >= 2

        if (drawn && t.envelopeBaseline) {
            const y = t.envelopeBaselineSnap ? Math.round(Y(baselineLevel)) + t.envelopeBaselineOffset
                                             : Y(baselineLevel) + t.envelopeBaselineOffset
            ctx.strokeStyle = t.envelopeBaselineInk
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            ctx.moveTo(left, y)
            ctx.lineTo(right, y)
            ctx.stroke()
        }
        if (drawn && bandEnd > 0) {
            ctx.fillStyle = t.envelopeBandFill
            ctx.fillRect(left, 0, X(bandEnd) - left, h)
        }
        ctx.strokeStyle = t.envelopeGridInk
        ctx.lineWidth = t.hairline
        for (let i = 0; t.envelopeGridRows > 0 && i <= t.envelopeGridRows; ++i) {
            const y = top + (bottom - top) * i / t.envelopeGridRows
            ctx.beginPath()
            ctx.moveTo(left, t.envelopeGridSnap ? snap(y) : y)
            ctx.lineTo(right, t.envelopeGridSnap ? snap(y) : y)
            ctx.stroke()
        }
        for (let i = 0; t.envelopeGridColumns > 0 && i <= t.envelopeGridColumns; ++i) {
            const x = left + span * i / t.envelopeGridColumns
            ctx.beginPath()
            ctx.moveTo(t.envelopeGridSnap ? snap(x) : x, top)
            ctx.lineTo(t.envelopeGridSnap ? snap(x) : x, bottom)
            ctx.stroke()
        }
        if (!drawn)
            return

        const strokeMarkers = () => {
            if (markers.length === 0)
                return
            ctx.strokeStyle = t.envelopeMarkerInk
            ctx.lineWidth = t.envelopeMarkerWidth
            ctx.setLineDash(t.envelopeMarkerDash)
            ctx.beginPath()
            for (let i = 0; i < markers.length; ++i) {
                const m = markers[i]
                const x = t.envelopeMarkerSnap ? snap(X(m.x)) : X(m.x)
                ctx.moveTo(x, m.from === undefined ? top : Y(m.from))
                ctx.lineTo(x, m.to === undefined ? bottom : Y(m.to))
            }
            ctx.stroke()
            ctx.setLineDash([])
        }
        // The outline; closed, it runs from the baseline's left end back
        // along it from the right.
        const trace = closed => {
            if (closed) {
                ctx.moveTo(X(0), Y(0))
                ctx.lineTo(X(shape[0][0]), Y(shape[0][1]))
            } else {
                ctx.moveTo(X(shape[0][0]), Y(shape[0][1]))
            }
            for (let i = 1; i < shape.length; ++i) {
                const p = shape[i]
                if (p.length > 2)
                    ctx.quadraticCurveTo(X(p[2]), Y(p[3]), X(p[0]), Y(p[1]))
                else
                    ctx.lineTo(X(p[0]), Y(p[1]))
            }
            if (closed) {
                ctx.lineTo(X(1), Y(0))
                ctx.closePath()
            }
        }
        const fill = () => {
            if (!t.envelopeFilled)
                return
            if (t.envelopeGradient) {
                const gradient = ctx.createLinearGradient(0, top, 0, bottom)
                gradient.addColorStop(0, fillColor)
                gradient.addColorStop(1, fillEndColor)
                ctx.fillStyle = gradient
            } else {
                ctx.fillStyle = fillColor
            }
            ctx.globalAlpha = t.envelopeFillOpacity
            ctx.beginPath()
            trace(true)
            ctx.fill()
            ctx.globalAlpha = 1
        }
        const line = () => {
            if (t.envelopeGlowWidth > 0) {
                ctx.strokeStyle = glowColor
                ctx.lineWidth = t.envelopeGlowWidth
                ctx.beginPath()
                trace(false)
                ctx.stroke()
            }
            ctx.strokeStyle = lineColor
            ctx.lineWidth = t.envelopeLineWidth
            ctx.lineJoin = t.envelopeLineJoin
            ctx.beginPath()
            trace(false)
            ctx.stroke()
        }

        if (!t.envelopeMarkersOver)
            strokeMarkers()
        if (t.envelopeFillOverLine) {
            line()
            fill()
        } else {
            fill()
            line()
        }
        if (t.envelopeMarkersOver)
            strokeMarkers()
        for (let i = 0; i < knots.length; ++i) {
            ctx.fillStyle = t.envelopeKnotFill
            ctx.strokeStyle = lineColor
            ctx.lineWidth = t.envelopeKnotLineWidth
            ctx.beginPath()
            ctx.arc(X(knots[i][0]), Y(knots[i][1]), t.envelopeKnotRadius, 0, Math.PI * 2)
            ctx.fill()
            ctx.stroke()
        }
        if (playhead >= 0) {
            const x = X(playhead)
            ctx.fillStyle = t.envelopePlayheadInk
            ctx.fillRect(x - t.envelopePlayheadWidth / 2, top, t.envelopePlayheadWidth, bottom - top)
            ctx.beginPath()
            ctx.arc(x, Y(playheadLevel), t.envelopePlayheadRadius, 0, Math.PI * 2)
            ctx.fill()
        }
    }

    Canvas {
        id: plot
        anchors.fill: parent
        anchors.margins: view.theme.envelopeMargin
        anchors.topMargin: view.theme.envelopeMarginTop
        anchors.bottomMargin: view.theme.envelopeMarginBottom
        renderStrategy: Canvas.Cooperative
        onPaint: view.paint(getContext("2d"), width, height)
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
}
