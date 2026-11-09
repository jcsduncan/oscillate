pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The alignment needle: a meter arc from 20 ms early to 20 ms late with
// zero at the top. The scale opens out around zero (asinh), so a tenth of
// a millisecond moves the needle visibly while the full search still fits.
Item {
    id: needle
    required property ScopeTheme theme
    property real delayMs: 0
    property bool live: false
    property color tone: theme.aligned
    readonly property real reach: 20
    readonly property real knee: 0.5
    readonly property real sweep: Math.PI * 0.5
    // The EARLY and LATE labels sit this share of the radius either side.
    readonly property real sideLabelReach: 0.55
    // -1 (20 ms early) .. +1 (20 ms late).
    readonly property real position: {
        const d = Math.max(-reach, Math.min(reach, delayMs))
        const p = Math.asinh(Math.abs(d) / knee) / Math.asinh(reach / knee)
        return d < 0 ? -p : p
    }

    objectName: "scopeNeedle"
    readonly property string plotExemptReason: "a meter dial with its own printed scale (ms early and late)"

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
            const t = needle.theme
            const cx = width / 2
            const cy = height - t.needleHubBottom
            const r = Math.min(cx - t.needleSideRoom, cy - t.needleTopRoom)
            const scale = d => {
                const p = Math.asinh(Math.abs(d) / needle.knee) / Math.asinh(needle.reach / needle.knee)
                return d < 0 ? -p : p
            }
            // The zone: green within a tenth of a millisecond.
            ctx.lineCap = "butt"
            ctx.lineWidth = t.zoneWidth
            ctx.strokeStyle = Qt.rgba(t.aligned.r, t.aligned.g, t.aligned.b, t.zoneAlpha)
            ctx.beginPath()
            ctx.arc(cx, cy, r, needle.angleAt(scale(-0.1)), needle.angleAt(scale(0.1)))
            ctx.stroke()
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.gridStrong
            ctx.beginPath()
            ctx.arc(cx, cy, r, needle.angleAt(-1), needle.angleAt(1))
            ctx.stroke()
            // Ticks and their labels.
            const ticks = [0, 0.25, 0.5, 1, 2, 5, 10, 20]
            ctx.font = t.typeCaption + "px \"" + t.mono + "\""
            ctx.textAlign = "center"
            for (let i = 0; i < ticks.length; ++i) {
                for (const s of [-1, 1]) {
                    if (ticks[i] === 0 && s < 0)
                        continue
                    const a = needle.angleAt(scale(s * ticks[i]))
                    const major = ticks[i] === 0 || ticks[i] >= 1
                    const r1 = r + (major ? t.majorTickLength : t.minorTickLength)
                    ctx.strokeStyle = major ? t.graticuleInk : t.gridStrong
                    ctx.lineWidth = ticks[i] === 0 ? t.strokeBold : t.hairline
                    ctx.beginPath()
                    ctx.moveTo(cx + Math.cos(a) * (r + t.tickGap), cy + Math.sin(a) * (r + t.tickGap))
                    ctx.lineTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
                    ctx.stroke()
                    if (major && ticks[i] !== 0.25) {
                        ctx.fillStyle = ticks[i] === 0 ? t.ink : t.inkDim
                        const label = ticks[i] === 0 ? "0" : (s < 0 ? Format.minus : "+") + ticks[i]
                        ctx.fillText(label, cx + Math.cos(a) * (r + t.tickLabelRadius),
                                     cy + Math.sin(a) * (r + t.tickLabelRadius) + t.tickLabelDrop)
                    }
                }
            }
            ctx.fillStyle = t.inkFaint
            ctx.font = "bold " + t.typeMicro + "px \"" + t.sans + "\""
            ctx.textAlign = "center"
            ctx.fillText("◂ EARLY", cx - r * needle.sideLabelReach, cy + t.sideLabelDrop)
            ctx.fillText("LATE ▸", cx + r * needle.sideLabelReach, cy + t.sideLabelDrop)
            if (!needle.live)
                return
            // The needle, with a soft glow, and its hub.
            const a = needle.angleAt(needle.position)
            const tone = needle.tone
            ctx.lineCap = "round"
            const tip = r - t.needleShort
            ctx.lineWidth = t.needleGlowWidth
            ctx.strokeStyle = Qt.rgba(tone.r, tone.g, tone.b, t.needleGlowAlpha)
            ctx.beginPath()
            ctx.moveTo(cx, cy)
            ctx.lineTo(cx + Math.cos(a) * tip, cy + Math.sin(a) * tip)
            ctx.stroke()
            ctx.lineWidth = t.strokeBold
            ctx.strokeStyle = tone
            ctx.beginPath()
            ctx.moveTo(cx, cy)
            ctx.lineTo(cx + Math.cos(a) * tip, cy + Math.sin(a) * tip)
            ctx.stroke()
            ctx.fillStyle = t.plateHi
            ctx.beginPath()
            ctx.arc(cx, cy, t.hubRadius, 0, Math.PI * 2)
            ctx.fill()
            ctx.fillStyle = tone
            ctx.beginPath()
            ctx.arc(cx, cy, t.hubDotRadius, 0, Math.PI * 2)
            ctx.fill()
        }
        Connections {
            target: needle
            function onPositionChanged() { dial.requestPaint() }
            function onLiveChanged() { dial.requestPaint() }
            function onToneChanged() { dial.requestPaint() }
        }
    }
}
