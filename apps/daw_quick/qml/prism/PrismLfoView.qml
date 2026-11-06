pragma ComponentBehavior: Bound
import QtQuick

// LFO shape over a few cycles, with its fade-in drawn as a rising envelope.
Rectangle {
    id: view
    required property var controller
    required property PrismTheme theme
    required property string prefix // "prism.lfo1"
    property color accent: theme.lfo1
    readonly property int revision: controller.revision
    readonly property int shape: revision >= 0 ? controller.choice(prefix + ".shape") : 0
    readonly property real fade: revision >= 0 ? controller.normalised(prefix + ".fade") : 0
    readonly property bool synced: revision >= 0 && controller.choice(prefix + ".sync") === 1
    readonly property string rateText: revision < 0 ? ""
        : synced ? controller.display(prefix + ".division") : controller.display(prefix + ".rate")

    // Cycles drawn across the view, the share of the width a full fade-in
    // spans, the shortest fade worth outlining, and the sample-and-hold
    // levels (holdsPerCycle of them per cycle).
    readonly property int cycles: 3
    readonly property real fadeShare: 0.6
    readonly property real shortestFade: 2
    readonly property int holdsPerCycle: 3
    readonly property var heldLevels: [0.2, -0.7, 0.9, -0.3, 0.55, -0.9, 0.1, 0.75, -0.45, 0.35]

    radius: theme.viewRadius
    color: theme.well
    border.color: theme.line
    Accessible.role: Accessible.Graphic
    Accessible.name: "LFO shape at " + rateText

    function sample(phase) {
        phase = phase - Math.floor(phase)
        switch (shape) {
        case 0: return Math.sin(2 * Math.PI * phase)
        case 1: return phase < 0.25 ? 4 * phase : phase < 0.75 ? 2 - 4 * phase : 4 * phase - 4
        case 2: return 2 * phase - 1
        case 3: return 1 - 2 * phase
        case 4: return phase < 0.5 ? 1 : -1
        default: return 0
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const mid = h / 2
            const t = view.theme
            const amp = h / 2 - t.lfoPlotInset
            const cycles = view.cycles
            const fadeEnd = view.fade * w * view.fadeShare
            const held = view.heldLevels
            ctx.strokeStyle = t.line
            ctx.lineWidth = t.hairline
            ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(w, mid); ctx.stroke()
            ctx.strokeStyle = view.accent
            ctx.lineWidth = t.traceLineWidth
            ctx.lineJoin = "round"
            ctx.beginPath()
            for (let x = 0; x <= w; ++x) {
                const phase = x / w * cycles
                const value = view.shape === 5
                    ? held[Math.floor(phase * view.holdsPerCycle) % held.length]
                    : view.sample(phase)
                const gain = fadeEnd > 0 ? Math.min(1, x / fadeEnd) : 1
                const y = mid - value * gain * amp
                if (x === 0)
                    ctx.moveTo(x, y)
                else
                    ctx.lineTo(x, y)
            }
            ctx.stroke()
            if (fadeEnd > view.shortestFade) {
                ctx.strokeStyle = t.faint
                ctx.lineWidth = t.hairline
                ctx.setLineDash(t.lfoFadeDash)
                ctx.beginPath()
                ctx.moveTo(0, mid); ctx.lineTo(fadeEnd, mid - amp)
                ctx.moveTo(0, mid); ctx.lineTo(fadeEnd, mid + amp)
                ctx.stroke()
            }
        }
        Connections {
            target: view
            function onRevisionChanged() { canvas.requestPaint() }
        }
    }
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: view.theme.spaceXs
        width: rate.implicitWidth + view.theme.badgePad
        height: view.theme.lfoBadgeHeight
        radius: view.theme.radiusMd
        color: view.theme.lfoBadgeFill
        Text {
            id: rate
            anchors.centerIn: parent
            text: view.rateText
            color: view.accent
            font.pixelSize: view.theme.typeCaption
            font.bold: true
        }
    }
}
