import QtQuick

// A small line drawing for one of Dust's layers: a record, two tape reels,
// a mains wave, an aerial, raindrops, wind, a room and a fan.
Canvas {
    id: glyph
    required property string kind
    required property DustTheme theme
    property color color: theme.glacier

    // Each drawing's proportions, as shares of the glyph's radius, width
    // or height (angles in radians).
    // vinyl: the groove arc and its span, the label.
    readonly property real grooveShare: 0.62
    readonly property real grooveStart: -0.6
    readonly property real grooveEnd: 0.9
    readonly property real labelShare: 0.22
    // tape: the cassette's top and height, the reels' centres and radius.
    readonly property real cassetteTop: 0.2
    readonly property real cassetteHeight: 0.6
    readonly property real leftReel: 0.32
    readonly property real rightReel: 0.68
    readonly property real reelShare: 0.15
    // hum: two cycles drawn in sixteen steps, this tall.
    readonly property int humSteps: 16
    readonly property int humCycles: 2
    readonly property real humHeight: 0.3
    // radio: the mast's head, the waves' radii and their arc.
    readonly property real mastHead: 1.4
    readonly property var waveShares: [0.32, 0.56]
    readonly property real waveReach: 1.4
    readonly property real waveStart: -0.8
    readonly property real waveEnd: -0.2
    // rain: each drop's [x, y] share, its slant and length.
    readonly property var drops: [[0.28, 0.25], [0.68, 0.18], [0.48, 0.58], [0.85, 0.62], [0.15, 0.7]]
    readonly property real dropSlant: 1.5
    readonly property real dropLength: 0.22
    // wind: three gusts, two curling back.
    readonly property real gustTop: 0.35
    readonly property real gustTopEnd: 0.7
    readonly property real gustTopCurl: 0.22
    readonly property real gustTopCurlRadius: 0.13
    readonly property real gustTopCurlEnd: -0.9
    readonly property real gustMid: 0.6
    readonly property real gustMidEnd: 0.85
    readonly property real gustLow: 0.82
    readonly property real gustLowStart: 0.2
    readonly property real gustLowEnd: 0.62
    readonly property real gustLowCurl: 0.72
    readonly property real gustLowCurlRadius: 0.1
    // room: the roof's eaves and peak, the walls, the door.
    readonly property real eaves: 0.45
    readonly property real roofPeak: 1.5
    readonly property real wallTop: 0.38
    readonly property real leftWall: 0.18
    readonly property real rightWall: 0.82
    readonly property real doorX: 0.42
    readonly property real doorTop: 0.62
    readonly property real doorWidth: 0.16
    // machine: three blades, each curving out through a control point.
    readonly property int blades: 3
    readonly property real bladeSweep: 0.9
    readonly property real bladeControl: 0.9
    readonly property real bladeTip: 0.8

    objectName: "dustGlyph:" + kind
    implicitWidth: theme.glyphSize
    implicitHeight: theme.glyphSize
    onColorChanged: requestPaint()
    onKindChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        const w = width
        const h = height
        const cx = w / 2
        const cy = h / 2
        const r = Math.min(w, h) / 2 - 1
        ctx.clearRect(0, 0, w, h)
        ctx.strokeStyle = glyph.color
        ctx.fillStyle = glyph.color
        ctx.lineWidth = glyph.theme.glyphStroke
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        switch (glyph.kind) {
        case "vinyl":
            ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.stroke()
            ctx.beginPath(); ctx.arc(cx, cy, r * glyph.grooveShare, glyph.grooveStart, glyph.grooveEnd); ctx.stroke()
            ctx.beginPath(); ctx.arc(cx, cy, r * glyph.labelShare, 0, 2 * Math.PI); ctx.fill()
            break
        case "tape":
            ctx.strokeRect(1, h * glyph.cassetteTop, w - 2, h * glyph.cassetteHeight)
            ctx.beginPath(); ctx.arc(w * glyph.leftReel, cy, h * glyph.reelShare, 0, 2 * Math.PI); ctx.stroke()
            ctx.beginPath(); ctx.arc(w * glyph.rightReel, cy, h * glyph.reelShare, 0, 2 * Math.PI); ctx.stroke()
            break
        case "hum":
            ctx.beginPath()
            for (let i = 0; i <= glyph.humSteps; ++i) {
                const x = 1 + (w - 2) * i / glyph.humSteps
                const y = cy - Math.sin(i / glyph.humSteps * glyph.humCycles * 2 * Math.PI) * h * glyph.humHeight
                if (i === 0)
                    ctx.moveTo(x, y)
                else
                    ctx.lineTo(x, y)
            }
            ctx.stroke()
            break
        case "radio":
            ctx.beginPath(); ctx.moveTo(cx, h - 1); ctx.lineTo(cx, cy); ctx.stroke()
            ctx.beginPath(); ctx.arc(cx, cy, glyph.mastHead, 0, 2 * Math.PI); ctx.fill()
            for (const k of glyph.waveShares) {
                ctx.beginPath(); ctx.arc(cx, cy, r * k * glyph.waveReach, Math.PI * glyph.waveStart, Math.PI * glyph.waveEnd); ctx.stroke()
            }
            break
        case "rain":
            for (const d of glyph.drops) {
                ctx.beginPath()
                ctx.moveTo(w * d[0] + glyph.dropSlant, h * d[1])
                ctx.lineTo(w * d[0] - glyph.dropSlant, h * d[1] + h * glyph.dropLength)
                ctx.stroke()
            }
            break
        case "wind":
            ctx.beginPath(); ctx.moveTo(1, h * glyph.gustTop); ctx.lineTo(w * glyph.gustTopEnd, h * glyph.gustTop)
            ctx.arc(w * glyph.gustTopEnd, h * glyph.gustTopCurl, h * glyph.gustTopCurlRadius, Math.PI / 2,
                    Math.PI * glyph.gustTopCurlEnd, true); ctx.stroke()
            ctx.beginPath(); ctx.moveTo(1, h * glyph.gustMid); ctx.lineTo(w * glyph.gustMidEnd, h * glyph.gustMid); ctx.stroke()
            ctx.beginPath(); ctx.moveTo(w * glyph.gustLowStart, h * glyph.gustLow); ctx.lineTo(w * glyph.gustLowEnd, h * glyph.gustLow)
            ctx.arc(w * glyph.gustLowEnd, h * glyph.gustLowCurl, h * glyph.gustLowCurlRadius, Math.PI / 2, -Math.PI / 2, true); ctx.stroke()
            break
        case "room":
            ctx.beginPath()
            ctx.moveTo(1, h * glyph.eaves); ctx.lineTo(cx, glyph.roofPeak); ctx.lineTo(w - 1, h * glyph.eaves)
            ctx.moveTo(w * glyph.leftWall, h * glyph.wallTop); ctx.lineTo(w * glyph.leftWall, h - 1)
            ctx.lineTo(w * glyph.rightWall, h - 1); ctx.lineTo(w * glyph.rightWall, h * glyph.wallTop)
            ctx.stroke()
            ctx.strokeRect(w * glyph.doorX, h * glyph.doorTop, w * glyph.doorWidth, h * (1 - glyph.doorTop) - 1)
            break
        case "machine":
            ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.stroke()
            for (let i = 0; i < glyph.blades; ++i) {
                const a = i * 2 * Math.PI / glyph.blades
                const b = a + glyph.bladeSweep
                ctx.beginPath()
                ctx.moveTo(cx, cy)
                ctx.quadraticCurveTo(cx + Math.cos(b) * r * glyph.bladeControl, cy + Math.sin(b) * r * glyph.bladeControl,
                                     cx + Math.cos(a) * r * glyph.bladeTip, cy + Math.sin(a) * r * glyph.bladeTip)
                ctx.stroke()
            }
            break
        }
    }
}
