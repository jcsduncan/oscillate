pragma ComponentBehavior: Bound
import QtQuick

// Where each pad's latest hit landed, left to right (knock.hpp telemetry
// first_place: pan and Motion together), as a dot that fades over
// `fadeMs`. The selected pad's dot is in the strike colour.
Canvas {
    id: strip
    required property KnockTheme theme
    // Per pad: where its latest hit landed (-1 left .. 1 right), and when
    // (Date.now() ms, -1 before any).
    property var places: []
    property var landedAt: []
    property int selectedPad: 0
    property real placeMin: -1
    property real placeMax: 1
    readonly property int fadeMs: 900
    readonly property real dotRadius: 3
    readonly property real margin: 10
    // The centre line stops this far inside the margins (clear of L and R),
    // and how far the letters sit left of and below their marks.
    readonly property real railInset: 12
    readonly property real letterShift: 4
    // Repaint at this rate while a dot fades.
    readonly property int frameMs: 33
    // Dots drawn by the last paint; true while any still fades.
    property int shownCount: 0
    property bool fading: false

    objectName: "knockStereo"
    renderStrategy: Canvas.Cooperative
    Accessible.role: Accessible.Graphic
    Accessible.name: "Where each pad lands, left to right"

    function xOf(place) {
        const span = placeMax - placeMin
        const t = span > 0 ? (Math.max(placeMin, Math.min(placeMax, place)) - placeMin) / span : 0.5
        return margin + (width - 2 * margin) * t
    }

    onLandedAtChanged: {
        fading = true
        requestPaint()
    }
    onSelectedPadChanged: requestPaint()
    onWidthChanged: requestPaint()

    Timer {
        interval: strip.frameMs
        repeat: true
        running: strip.visible && strip.fading
        onTriggered: strip.requestPaint()
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const y = Math.round(height / 2) + theme.hairline / 2
        ctx.strokeStyle = theme.line
        ctx.lineWidth = theme.hairline
        ctx.beginPath()
        ctx.moveTo(margin + railInset, y)
        ctx.lineTo(width - margin - railInset, y)
        ctx.stroke()
        ctx.font = theme.typeSmall + "px \"" + theme.mono + "\""
        ctx.fillStyle = theme.inkFaint
        ctx.fillText("L", margin - letterShift, y + letterShift)
        ctx.fillText("R", width - margin - letterShift, y + letterShift)
        const now = Date.now()
        let shown = 0
        for (let pad = 0; pad < places.length; ++pad) {
            const at = landedAt[pad] !== undefined ? landedAt[pad] : -1
            if (at < 0)
                continue
            const alpha = 1 - (now - at) / fadeMs
            if (alpha <= 0)
                continue
            ctx.globalAlpha = alpha
            ctx.fillStyle = pad === selectedPad ? theme.strike : theme.ink
            ctx.beginPath()
            ctx.arc(xOf(places[pad]), y, dotRadius * (1 + alpha), 0, Math.PI * 2)
            ctx.fill()
            shown += 1
        }
        ctx.globalAlpha = 1
        shownCount = shown
        if (shown === 0)
            fading = false
    }
}
