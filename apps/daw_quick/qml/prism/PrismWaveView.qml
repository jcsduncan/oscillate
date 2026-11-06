pragma ComponentBehavior: Bound
import QtQuick

// Live picture of one oscillator: its shape and pulse width, with faint
// detuned copies when unison is on.
Rectangle {
    id: view
    required property var controller
    required property PrismTheme theme
    required property string prefix // "prism.osc1"
    property color accent: theme.osc1
    readonly property int revision: controller.revision
    readonly property int wave: revision >= 0 ? controller.choice(prefix + ".wave") : 0
    readonly property real width01: revision >= 0 ? controller.value(prefix + ".pw") : 0.5
    readonly property int unison: revision >= 0 ? Math.round(controller.value(prefix + ".unison")) : 1
    readonly property real detune: revision >= 0 ? controller.value(prefix + ".detune") : 0
    readonly property real level: revision >= 0 ? controller.value(prefix + ".level") : 1

    // Cycles drawn across the view; the trace's height at zero level and the
    // share level adds; unison copies: at most mostGhosts, spread apart by
    // ghostSpread plus the detune over detuneSpread, drawn at ghostScale.
    readonly property int cycles: 2
    readonly property real quietGain: 0.35
    readonly property real levelGain: 0.65
    readonly property int mostGhosts: 6
    readonly property real ghostSpread: 0.02
    readonly property real detuneSpread: 900
    readonly property real ghostScale: 0.9

    radius: theme.viewRadius
    color: theme.well
    border.color: theme.line
    Accessible.role: Accessible.Graphic
    Accessible.name: "Waveform preview"

    function sample(phase) {
        phase = phase - Math.floor(phase)
        if (wave === 0)
            return 2 * phase - 1
        if (wave === 1)
            return phase < width01 ? 1 : -1
        if (wave === 2)
            return phase < 0.5 ? 4 * phase - 1 : 3 - 4 * phase
        return Math.sin(2 * Math.PI * phase)
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.theme.hairline
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const mid = h / 2
            const t = view.theme
            const amp = (h / 2 - t.wavePlotInset) * (view.quietGain + view.levelGain * view.level)
            ctx.strokeStyle = t.line
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            ctx.moveTo(0, mid); ctx.lineTo(w, mid)
            ctx.stroke()
            const cycles = view.cycles
            const draw = (offset, alpha, lineWidth, scale) => {
                ctx.globalAlpha = alpha
                ctx.lineWidth = lineWidth
                ctx.beginPath()
                for (let x = 0; x <= w; x += 1) {
                    const phase = x / w * cycles + offset
                    const y = mid - view.sample(phase) * amp * scale
                    if (x === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
            ctx.strokeStyle = view.accent
            ctx.lineJoin = "round"
            const ghosts = Math.min(view.unison, view.mostGhosts) - 1
            for (let g = 1; g <= ghosts; ++g) {
                const spread = (g % 2 === 0 ? 1 : -1) * Math.ceil(g / 2) * (view.ghostSpread + view.detune / view.detuneSpread)
                draw(spread, t.ghostAlpha, t.hairline, view.ghostScale)
            }
            const gradient = ctx.createLinearGradient(0, mid - amp, 0, mid + amp)
            gradient.addColorStop(0, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, t.waveFillAlpha))
            gradient.addColorStop(1, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, 0))
            ctx.globalAlpha = 1
            ctx.fillStyle = gradient
            ctx.beginPath()
            ctx.moveTo(0, mid)
            for (let x = 0; x <= w; x += 1)
                ctx.lineTo(x, mid - view.sample(x / w * cycles) * amp)
            ctx.lineTo(w, mid)
            ctx.closePath()
            ctx.fill()
            draw(0, 1, t.traceLineWidth, 1)
        }
        Connections {
            target: view
            function onRevisionChanged() { canvas.requestPaint() }
            function onAccentChanged() { canvas.requestPaint() }
        }
    }
    Rectangle {
        visible: view.unison > 1
        x: view.theme.voicesBadgeInset
        y: parent.height - height - view.theme.voicesBadgeInset
        width: voices.implicitWidth + view.theme.badgePad
        height: view.theme.voicesBadgeHeight
        radius: view.theme.radiusMd
        color: view.theme.voicesBadgeFill
        Text {
            id: voices
            anchors.centerIn: parent
            text: view.unison + " VOICES"
            color: view.accent
            font.pixelSize: view.theme.typeMicro
            font.bold: true
            font.letterSpacing: view.theme.badgeLetterSpacing
        }
    }
}
