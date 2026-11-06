pragma ComponentBehavior: Bound
import QtQuick

// The cents needle: a meter arc from 50 cents flat to 50 sharp, ticks at
// every five cents, a green window of three cents either side of zero and
// the needle in the tone of how far out the note is.
Item {
    id: meter
    required property TuneTheme theme
    property real cents: 0
    property bool live: false
    property bool held: false
    property color tone: theme.amber
    // The cents either side of zero at the arc's ends.
    readonly property real rangeCents: 50
    // Half the arc, as a fraction of π.
    readonly property real halfSweep: 0.3
    readonly property real sweep: Math.PI * halfSweep
    readonly property real position: Math.max(-1, Math.min(1, cents / rangeCents))
    // Ticks every tickCents, major (longer, labelled) every majorTickCents.
    readonly property int tickCents: 5
    readonly property int majorTickCents: 10
    readonly property real zeroTickLength: 14
    readonly property real majorTickLength: 10
    readonly property real minorTickLength: 6
    // The arc's room: beside it for the labels, above it, and in all.
    readonly property real sideRoom: 30
    readonly property real arcTop: 24
    readonly property real verticalRoom: 44
    // Tick labels: their radius beyond the arc and baseline drop.
    readonly property real labelRadius: 13
    readonly property real labelDrop: 3
    // The needle: its longest reach inside the arc, how far it stops above
    // the meter's foot, and how far it passes the arc.
    readonly property real needleLength: 120
    readonly property real needleFloor: 4
    readonly property real needleOvershoot: 2

    objectName: "tuneCentsNeedle"

    function angleAt(p) {
        return -Math.PI / 2 + p * sweep
    }

    Canvas {
        id: dial
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = meter.theme
            const cx = width / 2
            const r = Math.min((cx - meter.sideRoom) / Math.sin(meter.sweep), (height - meter.verticalRoom) / (1 - Math.cos(meter.sweep)))
            const cy = meter.arcTop + r
            // The in-tune window.
            ctx.lineCap = "butt"
            ctx.lineWidth = t.centsWindowStroke
            ctx.strokeStyle = Qt.rgba(t.inTune.r, t.inTune.g, t.inTune.b, t.centsWindowAlpha)
            ctx.beginPath()
            ctx.arc(cx, cy, r, meter.angleAt(-t.inTuneCents / meter.rangeCents), meter.angleAt(t.inTuneCents / meter.rangeCents))
            ctx.stroke()
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.gridStrong
            ctx.beginPath()
            ctx.arc(cx, cy, r, meter.angleAt(-1), meter.angleAt(1))
            ctx.stroke()
            ctx.font = t.typeSmall + "px \"" + t.mono + "\""
            ctx.textAlign = "center"
            for (let c = -meter.rangeCents; c <= meter.rangeCents; c += meter.tickCents) {
                const a = meter.angleAt(c / meter.rangeCents)
                const major = c % meter.majorTickCents === 0
                const r1 = r - (c === 0 ? meter.zeroTickLength : major ? meter.majorTickLength : meter.minorTickLength)
                ctx.strokeStyle = c === 0 ? t.glacier : major ? t.inkDim : t.gridStrong
                ctx.lineWidth = c === 0 ? t.strokeBold : t.hairline
                ctx.beginPath()
                ctx.moveTo(cx + Math.cos(a) * r, cy + Math.sin(a) * r)
                ctx.lineTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
                ctx.stroke()
                if (major) {
                    ctx.fillStyle = c === 0 ? t.ink : t.inkFaint
                    const label = c === 0 ? "0" : (c < 0 ? "−" : "+") + Math.abs(c)
                    ctx.fillText(label, cx + Math.cos(a) * (r + meter.labelRadius), cy + Math.sin(a) * (r + meter.labelRadius) + meter.labelDrop)
                }
            }
            ctx.fillStyle = t.inkFaint
            ctx.font = "bold " + t.typeCaption + "px \"" + t.sans + "\""
            ctx.textAlign = "left"
            ctx.fillText("♭ FLAT", t.spaceSm, height - t.spaceSm)
            ctx.textAlign = "right"
            ctx.fillText("SHARP ♯", width - t.spaceSm, height - t.spaceSm)
            if (!meter.live)
                return
            const a = meter.angleAt(meter.position)
            const tone = meter.held ? t.amberDim : meter.tone
            // From well inside the arc, but never below the meter's edge.
            const inner = Math.max(r - meter.needleLength, (cy - height + meter.needleFloor) / Math.cos(a + Math.PI / 2))
            ctx.lineCap = "round"
            const outer = r + meter.needleOvershoot
            ctx.lineWidth = t.glowStroke
            ctx.strokeStyle = Qt.rgba(tone.r, tone.g, tone.b, t.glowAlpha)
            ctx.beginPath()
            ctx.moveTo(cx + Math.cos(a) * inner, cy + Math.sin(a) * inner)
            ctx.lineTo(cx + Math.cos(a) * outer, cy + Math.sin(a) * outer)
            ctx.stroke()
            ctx.lineWidth = t.needleStroke
            ctx.strokeStyle = tone
            ctx.beginPath()
            ctx.moveTo(cx + Math.cos(a) * inner, cy + Math.sin(a) * inner)
            ctx.lineTo(cx + Math.cos(a) * outer, cy + Math.sin(a) * outer)
            ctx.stroke()
        }
        Connections {
            target: meter
            function onPositionChanged() { dial.requestPaint() }
            function onLiveChanged() { dial.requestPaint() }
            function onHeldChanged() { dial.requestPaint() }
            function onToneChanged() { dial.requestPaint() }
        }
    }
}
