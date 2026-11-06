import QtQuick

// The stereo image the settings make, seen from above: the listener at the
// bottom, the two speakers on an arc at 30 degrees either side, and the
// source as a band across the arc from its left edge (sky) to its right
// edge (coral). Pan moves the band, width spreads it (past the speakers
// above 100%, to a point at 0), mono collapses it, swap mirrors its
// colours and an inverted side is marked. A solo plays its one part on
// both speakers, so it shows as a point in the middle, named. It draws the
// settings, not the audio.
Canvas {
    id: stage
    required property TrimTheme theme
    property real pan: 0
    property real stereoWidth: 1
    property bool mono: false
    property bool swap: false
    property int invert: 0
    // 0 off, 1 left, 2 right, 3 mid, 4 side.
    property int solo: 0
    // The band's two edges on the arc, -1 (the left speaker) to 1 (the
    // right); beyond 1 is outside the speakers.
    readonly property real spread: mono || solo !== 0 ? 0 : Math.max(0, stereoWidth)
    readonly property real leftEdge: Math.max(-fieldEdge, Math.min(fieldEdge, pan - spread))
    readonly property real rightEdge: Math.max(-fieldEdge, Math.min(fieldEdge, pan + spread))

    // The drawing's geometry, in arc positions (1 is a speaker) or pixels.
    // How far past the speakers the field is drawn.
    readonly property real fieldEdge: 1.6
    // The speakers' angle either side of centre: 30 degrees.
    readonly property real speakerAngle: Math.PI / 6
    // The listener's height above the bottom, and the room the arc needs above it.
    readonly property real listenerRise: 22
    readonly property real arcHeadroom: 46
    // The arc's radius as a share of the width.
    readonly property real arcWidthShare: 0.42
    // The floor rings, and the angles their arcs span (in half turns).
    readonly property int ringCount: 3
    readonly property real ringStart: 1.05
    readonly property real ringEnd: 1.95
    // Radii as shares of the arc: the rays' reach, the speakers, the band.
    readonly property real rayReach: 1.06
    readonly property real speakerReach: 1.12
    readonly property real bandReach: 0.84
    // Narrower than this, the band draws as a point source.
    readonly property real pointSpread: 0.03
    // A speaker cabinet and its cone.
    readonly property real cabinetX: -9
    readonly property real cabinetY: -12
    readonly property real cabinetWidth: 18
    readonly property real cabinetHeight: 22
    readonly property real coneY: 1
    readonly property real coneRadius: 5
    // A point source: its glow, its dot, and its label above.
    readonly property real glowRadius: 22
    readonly property real dotRadius: 6
    readonly property real pointLabelLift: 26
    // The invert mark's distance past a speaker; text baselines drop this much.
    readonly property real invertMarkGap: 24
    readonly property real baselineDrop: 4
    // The listener: shoulders and head.
    readonly property real shoulderDrop: 4
    readonly property real shoulderRadius: 8
    readonly property real headRise: 8
    readonly property real headRadius: 5
    // The L and R labels beside their speakers.
    readonly property real speakerLabelGap: 22

    renderStrategy: Canvas.Cooperative
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const t = theme
        const w = width
        const h = height
        const cx = w / 2
        const cy = h - listenerRise
        const r = Math.min(w * arcWidthShare, h - arcHeadroom)
        const maxAngle = speakerAngle
        // A position on the arc: -1 left speaker, 1 right speaker.
        const angleOf = p => -Math.PI / 2 + p * maxAngle
        const pointOf = (p, radius) => ({ x: cx + Math.cos(angleOf(p)) * radius,
                                           y: cy + Math.sin(angleOf(p)) * radius })

        // The floor: rings and rays for orientation.
        ctx.lineWidth = t.hairline
        for (let i = 1; i <= ringCount; ++i) {
            ctx.strokeStyle = i === ringCount ? t.stageStrong : t.stageLine
            ctx.beginPath()
            ctx.arc(cx, cy, r * i / ringCount, Math.PI * ringStart, Math.PI * ringEnd)
            ctx.stroke()
        }
        ctx.strokeStyle = t.stageLine
        for (const p of [-fieldEdge, -1, 0, 1, fieldEdge]) {
            const a = pointOf(p, r * rayReach)
            ctx.beginPath()
            ctx.moveTo(cx, cy)
            ctx.lineTo(a.x, a.y)
            ctx.stroke()
        }
        // Outside the speakers, shaded.
        ctx.fillStyle = t.stageOutside
        for (const side of [-1, 1]) {
            ctx.beginPath()
            ctx.moveTo(cx, cy)
            ctx.arc(cx, cy, r * rayReach, angleOf(side < 0 ? -fieldEdge : 1), angleOf(side < 0 ? -1 : fieldEdge))
            ctx.closePath()
            ctx.fill()
        }

        // The speakers.
        for (const side of [-1, 1]) {
            const s = pointOf(side, r * speakerReach)
            const tone = side < 0 ? t.left : t.right
            ctx.save()
            ctx.translate(s.x, s.y)
            ctx.rotate(angleOf(side) + Math.PI / 2)
            ctx.fillStyle = t.plateHi
            ctx.strokeStyle = tone
            ctx.lineWidth = t.speakerStroke
            ctx.beginPath()
            ctx.rect(cabinetX, cabinetY, cabinetWidth, cabinetHeight)
            ctx.fill()
            ctx.stroke()
            ctx.beginPath()
            ctx.arc(0, coneY, coneRadius, 0, Math.PI * 2)
            ctx.stroke()
            ctx.restore()
        }

        // The image.
        const leftTone = swap ? t.right : t.left
        const rightTone = swap ? t.left : t.right
        const bandRadius = r * bandReach
        if (spread < pointSpread) {
            // A point source: a mono image, or a solo on both speakers.
            const p = solo !== 0 ? 0 : pan
            const c = pointOf(p, bandRadius)
            const tone = solo === 1 ? leftTone : solo === 2 ? rightTone : solo === 4 ? t.solo : t.image
            const glow = ctx.createRadialGradient(c.x, c.y, 1, c.x, c.y, glowRadius)
            glow.addColorStop(0, tone)
            glow.addColorStop(1, Qt.rgba(tone.r, tone.g, tone.b, 0))
            ctx.fillStyle = glow
            ctx.beginPath()
            ctx.arc(c.x, c.y, glowRadius, 0, Math.PI * 2)
            ctx.fill()
            ctx.fillStyle = t.glacier
            ctx.beginPath()
            ctx.arc(c.x, c.y, dotRadius, 0, Math.PI * 2)
            ctx.fill()
            if (solo !== 0) {
                ctx.font = "bold " + t.typeBody + "px " + t.sans
                ctx.textAlign = "center"
                ctx.fillStyle = tone
                ctx.fillText(["", "LEFT", "RIGHT", "MID", "SIDE"][solo] + " ON BOTH", c.x, c.y - pointLabelLift)
            }
        } else {
            const from = leftEdge
            const to = rightEdge
            const a = pointOf(from, bandRadius)
            const b = pointOf(to, bandRadius)
            const fill = ctx.createLinearGradient(a.x, a.y, b.x, b.y)
            fill.addColorStop(0, leftTone)
            fill.addColorStop(1, rightTone)
            ctx.strokeStyle = fill
            ctx.lineWidth = t.bandStroke
            ctx.lineCap = "round"
            ctx.beginPath()
            ctx.arc(cx, cy, bandRadius, angleOf(from), angleOf(to))
            ctx.stroke()
            // A soft halo along the band.
            ctx.lineWidth = t.bandHaloStroke
            ctx.globalAlpha = t.bandHaloAlpha
            ctx.stroke()
            ctx.globalAlpha = 1
        }

        // Inverted sides, marked at their speakers.
        ctx.font = "bold " + t.typeLabel + "px " + t.sans
        ctx.textAlign = "center"
        for (const side of [-1, 1]) {
            const bit = side < 0 ? 1 : 2
            if ((invert & bit) === 0)
                continue
            const m = pointOf(side, r * speakerReach + invertMarkGap)
            ctx.fillStyle = t.invert
            ctx.fillText("Ø", m.x, m.y + baselineDrop)
        }

        // The listener.
        ctx.fillStyle = t.plateHi
        ctx.beginPath()
        ctx.arc(cx, cy + shoulderDrop, shoulderRadius, Math.PI, 0)
        ctx.fill()
        ctx.beginPath()
        ctx.arc(cx, cy - headRise, headRadius, 0, Math.PI * 2)
        ctx.fill()
        // L and R labels.
        ctx.font = "bold " + t.typeSmall + "px " + t.sans
        ctx.fillStyle = t.left
        const l = pointOf(-1, r * speakerReach)
        ctx.fillText("L", l.x - speakerLabelGap, l.y + baselineDrop)
        ctx.fillStyle = t.right
        const rr = pointOf(1, r * speakerReach)
        ctx.fillText("R", rr.x + speakerLabelGap, rr.y + baselineDrop)
    }

    onPanChanged: requestPaint()
    onStereoWidthChanged: requestPaint()
    onMonoChanged: requestPaint()
    onSwapChanged: requestPaint()
    onInvertChanged: requestPaint()
    onSoloChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
}
