import QtQuick
import "shell"

// A folded group bus's lane: its members' clips as merged spans, each a
// dashed, bus-tinted outline.
Canvas {
    id: lane
    // [{startBar, endBar}], zero-based bars.
    required property var segments
    required property real viewStartBar
    required property real visibleBars
    required property color tone
    onSegmentsChanged: requestPaint()
    onViewStartBarChanged: requestPaint()
    onVisibleBarsChanged: requestPaint()
    onToneChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const context = getContext("2d")
        context.clearRect(0, 0, width, height)
        if (visibleBars <= 0)
            return
        const perBar = width / visibleBars
        const top = Theme.trackBusSumInsetY
        const tall = Math.max(0, height - 2 * Theme.trackBusSumInsetY)
        context.fillStyle = Qt.rgba(tone.r, tone.g, tone.b, Theme.trackBusSumAlpha)
        context.strokeStyle = Qt.rgba(tone.r, tone.g, tone.b, Theme.trackBusSumEdgeAlpha)
        context.lineWidth = Theme.hairline
        context.setLineDash(Theme.emptyLaneDash)
        for (const segment of segments) {
            const left = Math.max(-Theme.radiusLg, (segment.startBar - viewStartBar) * perBar + Theme.spaceXxs)
            const right = Math.min(width + Theme.radiusLg, (segment.endBar - viewStartBar) * perBar - Theme.spaceXxs)
            if (right <= left)
                continue
            context.beginPath()
            context.roundedRect(left + Theme.hairline / 2, top + Theme.hairline / 2, right - left - Theme.hairline,
                                tall - Theme.hairline, Theme.radiusLg, Theme.radiusLg)
            context.fill()
            context.stroke()
        }
    }
}
