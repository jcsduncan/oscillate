pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The shimmer reverb's tail: level falling 60 dB over the decay time (the
// FDN's RT60), with the octave-up shimmer rising out of it in proportion to
// the shimmer amount.
Rectangle {
    id: view
    required property var controller
    required property DriftTheme theme
    readonly property real decay: controller.revision >= 0 ? controller.value("drift.verb.decay") : 6
    readonly property real shimmer: controller.revision >= 0 ? controller.value("drift.verb.shimmer") : 0
    readonly property real mix: controller.revision >= 0 ? controller.value("drift.verb.mix") : 0
    // The drawing: the time shown (at least minimumSpan, else the decay
    // with room after it), points along the curve, the curve's headroom
    // under the top (px), how bright the tail is at zero mix, and how fast
    // the shimmer blooms against the decay.
    readonly property real minimumSpan: 1.5
    readonly property real spanOverDecay: 1.2
    readonly property int steps: 80
    readonly property real headroom: 2
    readonly property real dryStrength: 0.3
    readonly property real bloomRate: 4
    // -60 dB at RT60, as a power of ten of amplitude.
    readonly property real rt60Decades: 3

    objectName: "driftDecayView"
    radius: theme.radiusLg
    color: theme.well
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Reverb decay, " + Format.seconds(decay)

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.theme.viewPad
        anchors.topMargin: view.theme.decayTop
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const span = Math.max(view.minimumSpan, view.decay * view.spanOverDecay)
            const level = t => Math.pow(10, -view.rt60Decades * t / view.decay)
            const strength = view.dryStrength + (1 - view.dryStrength) * view.mix
            const steps = view.steps
            const fill = ctx.createLinearGradient(0, 0, w, 0)
            fill.addColorStop(0, Qt.rgba(view.theme.accent2.r, view.theme.accent2.g, view.theme.accent2.b,
                                         view.theme.tailAlpha * strength))
            fill.addColorStop(1, "transparent")
            ctx.beginPath()
            ctx.moveTo(0, h)
            for (let i = 0; i <= steps; ++i) {
                const t = i / steps * span
                ctx.lineTo(i / steps * w, h - Math.sqrt(level(t)) * (h - view.headroom))
            }
            ctx.lineTo(w, h)
            ctx.closePath()
            ctx.fillStyle = fill
            ctx.fill()
            // The shimmer: an octave-up halo that blooms and fades.
            if (view.shimmer > 0) {
                ctx.strokeStyle = Qt.rgba(view.theme.magenta.r, view.theme.magenta.g, view.theme.magenta.b,
                                          view.theme.shimmerLineAlpha)
                ctx.lineWidth = view.theme.shimmerLineWidth
                ctx.beginPath()
                for (let i = 0; i <= steps; ++i) {
                    const t = i / steps * span
                    const bloom = view.shimmer * (1 - Math.exp(-view.bloomRate * t / view.decay)) * Math.sqrt(level(t))
                    const y = h - bloom * (h - view.headroom)
                    if (i === 0)
                        ctx.moveTo(0, y)
                    else
                        ctx.lineTo(i / steps * w, y)
                }
                ctx.stroke()
            }
            const rt = view.decay / span * w
            ctx.setLineDash(view.theme.rtDash)
            ctx.strokeStyle = view.theme.inkFaint
            ctx.beginPath()
            ctx.moveTo(rt, 0)
            ctx.lineTo(rt, h)
            ctx.stroke()
        }
    }
    Text {
        x: view.theme.viewLabelX
        y: view.theme.viewLabelY
        text: "TAIL"
        color: view.theme.inkFaint
        font.pixelSize: view.theme.captionSize
        font.bold: true
        font.letterSpacing: view.theme.captionSpacing
    }
    Text {
        objectName: "driftDecayReadout"
        anchors.right: parent.right
        anchors.rightMargin: view.theme.viewLabelX
        y: view.theme.viewLabelY
        text: "RT60 " + Format.seconds(view.decay)
        color: view.theme.inkDim
        font.family: view.theme.mono
        font.pixelSize: view.theme.decayReadoutSize
    }
    onDecayChanged: canvas.requestPaint()
    onShimmerChanged: canvas.requestPaint()
    onMixChanged: canvas.requestPaint()
}
