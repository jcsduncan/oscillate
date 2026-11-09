import QtQuick

// The stage: a listener's view of the two speakers (at +-30 degrees) and
// the image between them. Its spread follows the side-to-mid ratio (a half
// correlated source fills the speakers, a mono one is a point, wider
// settings reach past them); rotation and balance move its centre.
Item {
    id: stage
    required property VistaTheme theme
    // The speakers' angle either side of centre, in degrees.
    readonly property real speakerAngle: 30
    // Side-to-mid amplitude of a half-correlated source: it fills the speakers.
    readonly property real fillRatio: 0.577
    // The widest image, as a multiple of the speakers' spread.
    readonly property real maxSpread: 1.6
    // How far rotation (at its full 45 degrees) and balance swing the centre.
    readonly property real fullRotation: 45
    readonly property real balanceSwing: 20
    // The listening arc's half angle, and the narrowest image drawn.
    readonly property real arcHalfAngle: 60
    readonly property real minSpread: 1.2
    // Canvas geometry: the listener's height above the foot, the arc's
    // clearance from the sides and top, and the speakers beyond the arc.
    readonly property real listenerInset: 8
    readonly property real arcSideClearance: 16
    readonly property real arcTopClearance: 18
    readonly property real speakerReach: 10
    readonly property real speakerLabelGap: 9
    // Side-to-mid amplitude of the image; 0.577 fills the speakers.
    property real ratio: fillRatio
    property real rotation: 0
    property real balance: 0
    // Half the image's angle, in degrees, and its centre (right positive).
    readonly property real spread: speakerAngle * Math.min(maxSpread, Math.max(0, ratio) / fillRatio)
    readonly property real centre: Math.max(-fullRotation, Math.min(fullRotation, speakerAngle * rotation / fullRotation + balanceSwing * balance))

    objectName: "vistaStage"
    readonly property string plotExemptReason: "a picture of the image between the two speakers, not a plot with axes"
    implicitWidth: theme.stageDefaultWidth
    implicitHeight: theme.stageDefaultHeight

    onSpreadChanged: canvas.requestPaint()
    onCentreChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = stage.theme
            const cx = width / 2
            const cy = height - stage.listenerInset
            const r = Math.min(width / 2 - stage.arcSideClearance, height - stage.arcTopClearance)
            const angle = deg => (deg - 90) * Math.PI / 180
            // The listening arc.
            ctx.strokeStyle = t.grid
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            ctx.arc(cx, cy, r, angle(-stage.arcHalfAngle), angle(stage.arcHalfAngle))
            ctx.stroke()
            // The image: a bright band on the arc with a soft wash beneath.
            const from = angle(stage.centre - Math.max(stage.minSpread, stage.spread))
            const to = angle(stage.centre + Math.max(stage.minSpread, stage.spread))
            ctx.fillStyle = t.imageWash
            ctx.beginPath()
            ctx.moveTo(cx, cy)
            ctx.arc(cx, cy, r, from, to)
            ctx.closePath()
            ctx.fill()
            ctx.lineCap = "round"
            ctx.strokeStyle = t.imageHalo
            ctx.lineWidth = t.imageHaloWidth
            ctx.beginPath()
            ctx.arc(cx, cy, r, from, to)
            ctx.stroke()
            ctx.strokeStyle = t.glacier
            ctx.lineWidth = t.imageLineWidth
            ctx.beginPath()
            ctx.arc(cx, cy, r, from, to)
            ctx.stroke()
            // The speakers.
            for (const side of [-stage.speakerAngle, stage.speakerAngle]) {
                const a = angle(side)
                const x = cx + Math.cos(a) * (r + stage.speakerReach)
                const y = cy + Math.sin(a) * (r + stage.speakerReach)
                ctx.fillStyle = t.plateHi
                ctx.strokeStyle = t.inkDim
                ctx.lineWidth = t.hairline
                ctx.beginPath()
                ctx.rect(x - t.speakerWidth / 2, y - t.speakerHeight / 2, t.speakerWidth, t.speakerHeight)
                ctx.fill()
                ctx.stroke()
                ctx.fillStyle = t.inkDim
                ctx.font = "bold " + t.typeMicro + "px '" + t.sans + "'"
                ctx.textAlign = "center"
                ctx.fillText(side < 0 ? "L" : "R", x, y - stage.speakerLabelGap)
            }
            // The listener.
            ctx.fillStyle = t.inkFaint
            ctx.beginPath()
            ctx.arc(cx, cy, t.listenerRadius, 0, Math.PI * 2)
            ctx.fill()
        }
    }
}
