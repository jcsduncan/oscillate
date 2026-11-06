pragma ComponentBehavior: Bound
import QtQuick

// Two cycles of the LFO shape at its depth: left channel in blue, right in
// a fainter blue offset by the stereo phase, and the live offset as a dot.
Rectangle {
    id: view
    required property var controller
    required property ContourTheme theme
    property var live: ({ live: false })
    readonly property int shape: controller.revision >= 0 ? controller.choice("contour.lfo.shape") : 0
    readonly property real depth: controller.revision >= 0 ? controller.value("contour.lfo.depth") : 0
    readonly property real stereo: controller.revision >= 0 ? controller.value("contour.lfo.stereo") : 0
    readonly property real maxDepth: 48
    // The drawing: two cycles in this many segments, the trace never
    // flatter than this fraction of full height, and the right channel drawn
    // only once the stereo phase exceeds this many degrees.
    readonly property int cycles: 2
    readonly property int samples: 160
    readonly property real smallestAmplitude: 0.08
    readonly property real stereoThreshold: 0.5
    readonly property real degreesPerCycle: 360
    // Sample-and-hold: deterministic held steps per cycle.
    readonly property int holdSteps: 8

    objectName: "contourLfoView"
    radius: theme.radiusMd
    color: theme.well
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "LFO shape"

    // Bipolar shape value for a phase in cycles.
    function wave(phase) {
        const p = phase - Math.floor(phase)
        switch (shape) {
        case 1: return 4 * Math.abs(((p + 0.75) % 1) - 0.5) - 1
        case 2: return 2 * p - 1
        case 3: return 1 - 2 * p
        case 4: return p < 0.5 ? 1 : -1
        case 5: {
            const step = Math.floor(phase * holdSteps)
            const h = Math.sin(step * 12.9898) * 43758.5453
            return 2 * (h - Math.floor(h)) - 1
        }
        default: return Math.sin(2 * Math.PI * p)
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.theme.spaceSm
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const mid = h / 2
            ctx.strokeStyle = view.theme.grid
            ctx.lineWidth = view.theme.hairline
            ctx.beginPath()
            ctx.moveTo(0, Math.round(mid) + view.theme.hairline / 2)
            ctx.lineTo(w, Math.round(mid) + view.theme.hairline / 2)
            ctx.stroke()
            const amplitude = (h / 2 - view.theme.lfoHeadroom) * Math.max(view.smallestAmplitude, view.depth / view.maxDepth)
            const draw = (offset, colour, lineWidth) => {
                ctx.strokeStyle = colour
                ctx.lineWidth = lineWidth
                ctx.beginPath()
                const n = view.samples
                for (let i = 0; i <= n; ++i) {
                    const x = i / n * w
                    const y = mid - view.wave(view.cycles * i / n + offset) * amplitude
                    if (i === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
            const c = view.theme.lfo
            const t = view.theme
            if (view.stereo > view.stereoThreshold)
                draw(view.stereo / view.degreesPerCycle, Qt.rgba(c.r, c.g, c.b, t.lfoRightAlpha), t.lfoRightWidth)
            draw(0, Qt.rgba(c.r, c.g, c.b, t.lfoGlowAlpha), t.lfoGlowWidth)
            draw(0, c, t.lfoLineWidth)
            if (view.live.live === true && view.live.lfo !== undefined && view.depth > 0) {
                const y = mid - (view.live.lfo / view.depth) * amplitude
                ctx.fillStyle = t.lfoDot
                ctx.beginPath()
                ctx.arc(w - t.lfoDotInset, Math.max(t.lfoDotRadius, Math.min(h - t.lfoDotRadius, y)),
                        t.lfoDotRadius, 0, Math.PI * 2)
                ctx.fill()
            }
        }
    }
    onShapeChanged: canvas.requestPaint()
    onDepthChanged: canvas.requestPaint()
    onStereoChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
}
