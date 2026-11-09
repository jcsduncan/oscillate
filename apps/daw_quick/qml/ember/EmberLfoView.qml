pragma ComponentBehavior: Bound
import QtQuick

// Two cycles of the LFO shape, faded in over its delay.
Rectangle {
    id: view
    required property var controller
    required property EmberTheme theme
    property color accent: theme.amber
    readonly property int revision: controller.revision
    // Choice indices of ember.lfo.shape (0 is the triangle).
    readonly property int squareShape: 1
    readonly property int sampleHoldShape: 2
    readonly property int sawShape: 3
    // Stepped levels the sample-and-hold shape walks through.
    readonly property var heldLevels: [0.6, -0.3, 0.9, -0.7, 0.2, -0.5, 0.75, -0.1]
    readonly property int heldStepsPerCycle: 4
    readonly property int cyclesShown: 2
    readonly property int plotSteps: 160
    // The delay fade reaches full depth at this share of the delay time.
    readonly property real delayFadeShare: 0.8
    readonly property real delayFadeFloor: 0.001
    // Pixels kept clear above and below the wave's peaks.
    readonly property real peakHeadroom: 3

    readonly property string plotExemptReason: "a thumbnail of the LFO's shape over two cycles; its rate and delay read on their knobs"
    radius: theme.viewRadius
    color: theme.well
    border.color: theme.engraving
    Accessible.role: Accessible.Graphic
    Accessible.name: "LFO shape"

    Canvas {
        id: plot
        anchors.fill: parent
        anchors.margins: view.theme.spaceXs
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = view.controller
            const shape = c.choice("ember.lfo.shape")
            const delay = c.normalised("ember.lfo.delay")
            const w = width
            const h = height
            const valueAt = phase => {
                const p = phase - Math.floor(phase)
                if (shape === view.squareShape)
                    return p < 0.5 ? 1 : -1
                if (shape === view.sampleHoldShape)
                    return view.heldLevels[Math.floor(phase * view.heldStepsPerCycle) % view.heldLevels.length]
                if (shape === view.sawShape)
                    return 2 * p - 1
                return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4
            }
            ctx.beginPath()
            for (let i = 0; i <= view.plotSteps; ++i) {
                const t = i / view.plotSteps
                const fade = delay <= 0 ? 1 : Math.min(1, t / (delay * view.delayFadeShare + view.delayFadeFloor))
                const v = valueAt(t * view.cyclesShown) * fade
                const x = t * w
                const yy = h / 2 - v * (h / 2 - view.peakHeadroom)
                if (i === 0)
                    ctx.moveTo(x, yy)
                else
                    ctx.lineTo(x, yy)
            }
            ctx.strokeStyle = view.accent
            ctx.lineWidth = view.theme.lfoStroke
            ctx.stroke()
        }
        Connections {
            target: view
            function onRevisionChanged() { plot.requestPaint() }
        }
    }
}
