import QtQuick
import Oscillate.Format

// The hero: the signal as a glowing core with the two channels' LFOs as
// bodies orbiting it. The orbit grows with depth; the bodies sit `stereo`
// degrees apart, joined by an arc; far bodies pass behind the core and
// shrink, near ones grow, each trailing light along its path.
Canvas {
    id: orrery
    required property OrbitTheme theme
    // Angles on the orbit in radians, 0 at the right, positive downward
    // (toward the viewer).
    property real angleLeft: 0
    property real angleRight: 0
    // +1 when the bodies run toward the viewer along the lower half first.
    property real direction: 1
    property real depth: 0.5
    property real stereoDegrees: 90
    property bool live: false

    // The drawing's proportions. Nebulae: centre and radius, of the size.
    readonly property var nebulaAAt: ({ x: 0.22, y: 0.3, r: 0.42 })
    readonly property var nebulaBAt: ({ x: 0.8, y: 0.78, r: 0.4 })
    readonly property var hazeAt: ({ x: 0.5, y: 0.52, r: 0.3 })
    // The fixed starfield: its seed, its stars, and how many shine bright.
    readonly property int starSeed: 7
    readonly property int starCount: 140
    readonly property real brightStarChance: 0.08
    // The orbit's centre below the middle, its width (of the canvas, growing
    // with depth) and its tilt (height over width).
    readonly property real centreDrop: 8
    readonly property real orbitWidthMin: 0.2
    readonly property real orbitWidthDepth: 0.17
    readonly property real orbitTilt: 0.36
    // The guide rings outside and inside the orbit, and the stereo arc's ring.
    readonly property real outerGuide: 1.42
    readonly property real innerGuide: 0.62
    readonly property real arcRing: 1.2
    readonly property int arcSteps: 40
    // The stereo offset's caption ring; offsets below this are not drawn.
    readonly property real captionRing: 1.34
    readonly property real stereoShown: 0.5
    // A body's trail: its dots and their spacing along the orbit (radians).
    readonly property int trailDots: 18
    readonly property real trailStep: 0.055
    // A body's glow radius, of its own, and its lit spot (offset and size,
    // of its radius).
    readonly property real bodyGlowScale: 3.4
    readonly property var bodyHighlight: ({ x: 0.35, y: 0.4, r: 0.1 })
    // The core's lit spot, offset from its centre.
    readonly property var coreHighlight: ({ x: 4, y: 5, r: 1 })

    objectName: "orbitOrrery"
    renderStrategy: Canvas.Cooperative

    onAngleLeftChanged: requestPaint()
    onAngleRightChanged: requestPaint()
    onDepthChanged: requestPaint()
    onStereoDegreesChanged: requestPaint()
    onLiveChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    Connections {
        target: orrery.theme
        function onAccentChanged() { orrery.requestPaint() }
    }

    function rgba(c, alpha) {
        return Qt.rgba(c.r, c.g, c.b, alpha)
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const w = width
        const h = height
        const t = theme
        const a = t.accent
        const b = t.companion

        // Sky and two faint nebulae.
        const sky = ctx.createLinearGradient(0, 0, 0, h)
        sky.addColorStop(0, t.skyTop)
        sky.addColorStop(1, t.sky)
        ctx.fillStyle = sky
        ctx.fillRect(0, 0, w, h)
        const nebula = (x, y, r, color, alpha) => {
            const g = ctx.createRadialGradient(x, y, 0, x, y, r)
            g.addColorStop(0, rgba(color, alpha))
            g.addColorStop(1, rgba(color, 0))
            ctx.fillStyle = g
            ctx.fillRect(0, 0, w, h)
        }
        nebula(w * nebulaAAt.x, h * nebulaAAt.y, w * nebulaAAt.r, t.nebulaA, t.nebulaAAlpha)
        nebula(w * nebulaBAt.x, h * nebulaBAt.y, w * nebulaBAt.r, t.nebulaB, t.nebulaBAlpha)
        nebula(w * hazeAt.x, h * hazeAt.y, w * hazeAt.r, a, t.hazeAlpha)

        // A fixed starfield.
        let seed = starSeed
        const random = () => {
            seed = (seed * 1103515245 + 12345) % 2147483648
            return seed / 2147483648
        }
        for (let i = 0; i < starCount; ++i) {
            const x = random() * w
            const y = random() * h
            const size = random() < brightStarChance ? t.brightStarSize : t.starSize
            ctx.fillStyle = rgba(t.star, t.starAlphaMin + t.starAlphaRange * random())
            ctx.fillRect(x, y, size, size)
        }

        const cx = w / 2
        const cy = h / 2 + centreDrop
        const rx = w * (orbitWidthMin + orbitWidthDepth * depth)
        const ry = rx * orbitTilt
        const ellipse = (scale, color, lineWidth) => {
            ctx.strokeStyle = color
            ctx.lineWidth = lineWidth
            ctx.beginPath()
            ctx.ellipse(cx - rx * scale, cy - ry * scale, 2 * rx * scale, 2 * ry * scale)
            ctx.stroke()
        }
        // Guide rings, then the orbit itself with a glow.
        ellipse(outerGuide, rgba(t.inkFaint, t.outerGuideAlpha), t.hairline)
        ellipse(innerGuide, rgba(t.inkFaint, t.innerGuideAlpha), t.hairline)
        ellipse(1.0, rgba(a, t.orbitGlowAlpha), t.orbitGlowWidth)
        ellipse(1.0, rgba(a, t.orbitLineAlpha), t.orbitLineWidth)

        const point = (angle, scale) => ({
            x: cx + Math.cos(angle) * rx * scale,
            y: cy + Math.sin(angle) * ry * scale
        })

        // The stereo arc between the bodies, on an outer ring.
        const span = angleRight - angleLeft
        if (Math.abs(stereoDegrees) > stereoShown) {
            ctx.strokeStyle = rgba(b, t.stereoArcAlpha)
            ctx.lineWidth = t.stereoArcWidth
            ctx.setLineDash ? ctx.setLineDash([t.stereoDash, t.stereoDashGap]) : null
            ctx.beginPath()
            const steps = arcSteps
            for (let s = 0; s <= steps; ++s) {
                const p = point(angleLeft + span * s / steps, arcRing)
                if (s === 0)
                    ctx.moveTo(p.x, p.y)
                else
                    ctx.lineTo(p.x, p.y)
            }
            ctx.stroke()
            ctx.setLineDash ? ctx.setLineDash([]) : null
        }

        const drawBody = (angle, color) => {
            const near = Math.sin(angle) // -1 far, 1 near.
            // Trail behind the body.
            for (let k = trailDots; k > 0; --k) {
                const p = point(angle - direction * k * trailStep, 1.0)
                const fade = 1 - k / (trailDots + 1)
                ctx.fillStyle = rgba(color, t.trailAlpha * fade)
                ctx.beginPath()
                ctx.arc(p.x, p.y, t.trailDotSize + t.trailDotGrow * fade, 0, Math.PI * 2)
                ctx.fill()
            }
            const p = point(angle, 1.0)
            const r = t.bodyRadius + t.bodyNearGrow * near
            const glow = ctx.createRadialGradient(p.x, p.y, 0, p.x, p.y, r * bodyGlowScale)
            glow.addColorStop(0, rgba(color, t.bodyGlowAlpha))
            glow.addColorStop(1, rgba(color, 0))
            ctx.fillStyle = glow
            ctx.beginPath()
            ctx.arc(p.x, p.y, r * bodyGlowScale, 0, Math.PI * 2)
            ctx.fill()
            const fill = ctx.createRadialGradient(p.x - r * bodyHighlight.x, p.y - r * bodyHighlight.y,
                                                  r * bodyHighlight.r, p.x, p.y, r)
            fill.addColorStop(0, t.glint)
            fill.addColorStop(1, color)
            ctx.fillStyle = fill
            ctx.beginPath()
            ctx.arc(p.x, p.y, r, 0, Math.PI * 2)
            ctx.fill()
        }
        const drawCore = () => {
            const glow = ctx.createRadialGradient(cx, cy, 0, cx, cy, t.coreGlowRadius)
            glow.addColorStop(0, rgba(a, t.coreGlowAlpha))
            glow.addColorStop(t.coreGlowKnee, rgba(a, t.coreGlowKneeAlpha))
            glow.addColorStop(1, rgba(a, 0))
            ctx.fillStyle = glow
            ctx.beginPath()
            ctx.arc(cx, cy, t.coreGlowRadius, 0, Math.PI * 2)
            ctx.fill()
            const core = ctx.createRadialGradient(cx - coreHighlight.x, cy - coreHighlight.y, coreHighlight.r,
                                                  cx, cy, t.coreRadius)
            core.addColorStop(0, t.glint)
            core.addColorStop(1, rgba(a, t.coreAlpha))
            ctx.fillStyle = core
            ctx.beginPath()
            ctx.arc(cx, cy, t.coreRadius, 0, Math.PI * 2)
            ctx.fill()
        }
        const label = (angle, text, color) => {
            const p = point(angle, 1.0)
            ctx.fillStyle = color
            ctx.font = "bold " + t.typeBody + "px sans-serif"
            ctx.fillText(text, p.x + t.bodyLabelX, p.y - t.bodyLabelRise)
        }

        // Far bodies pass behind the core; near ones in front.
        const bodies = [{ angle: angleLeft, color: a, name: "L" },
                        { angle: angleRight, color: b, name: "R" }]
        bodies.sort((p, q) => Math.sin(p.angle) - Math.sin(q.angle))
        for (const body of bodies) {
            if (Math.sin(body.angle) < 0)
                drawBody(body.angle, body.color)
        }
        drawCore()
        for (const body of bodies) {
            if (Math.sin(body.angle) >= 0)
                drawBody(body.angle, body.color)
        }
        for (const body of bodies)
            label(body.angle, body.name, body.color)

        // The stereo offset, written at the arc's middle.
        if (Math.abs(stereoDegrees) > stereoShown) {
            const mid = point(angleLeft + span / 2, captionRing)
            ctx.fillStyle = rgba(b, t.stereoCaptionAlpha)
            ctx.font = "bold " + t.typeSmall + "px sans-serif"
            const text = "Δ " + Format.degrees(stereoDegrees)
            ctx.fillText(text, mid.x - ctx.measureText(text).width / 2, mid.y + t.stereoCaptionDrop)
        }
    }
}
