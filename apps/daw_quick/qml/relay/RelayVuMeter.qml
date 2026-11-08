pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// A cream-faced VU meter for the wet signal: 0 VU sits at -18 dBFS RMS, the
// scale runs -20 to +3 with the red zone above 0, and the needle moves with
// VU ballistics (300 ms). Below it, the ducking lamp strip shows how far the
// repeats are tucked under the input.
Rectangle {
    id: meter
    required property RelayTheme theme
    // dBFS RMS of the wet signal, or a large negative value when idle.
    property real wetDb: -120
    // Gain applied by ducking, in dB (0 or negative).
    property real duckDb: 0
    property bool live: false
    readonly property real vu: Math.max(-22, Math.min(4, wetDb + 18))
    property real needle: -22
    Behavior on needle { NumberAnimation { duration: 300; easing.type: Easing.OutQuad } }
    readonly property real reduction: Math.max(0, Math.min(30, -duckDb))

    objectName: "relayVu"
    onVuChanged: needle = vu
    Component.onCompleted: needle = vu
    radius: theme.radiusLg
    gradient: Gradient {
        GradientStop { position: 0; color: meter.theme.brassHi }
        GradientStop { position: 1; color: meter.theme.brassLo }
    }
    Accessible.role: Accessible.Indicator
    Accessible.name: "Wet level " + Format.quantity(vu, "VU") + ", ducking " + Format.decibels(reduction)

    Rectangle {
        id: face
        x: meter.theme.vuInset
        y: meter.theme.vuInset
        width: parent.width - 2 * meter.theme.vuInset
        height: parent.height * meter.theme.vuFaceRatio
        radius: meter.theme.radiusMd
        gradient: Gradient {
            GradientStop { position: 0; color: meter.theme.vuFaceTop }
            GradientStop { position: 1; color: meter.theme.vuFaceBottom }
        }
        border.color: meter.theme.vuFaceEdge
        clip: true
        Canvas {
            id: dial
            // The dial's proportions, as shares of the face and of the
            // scale radius: the pivot's height, the radius's limits by
            // height and width, the scale's sweep (rad), the arc and tick
            // radii, the "VU" title's height and the needle's reach.
            readonly property real pivotHeight: 0.97
            readonly property real radiusByHeight: 0.74
            readonly property real radiusByWidth: 0.6
            readonly property real sweep: 1.6
            readonly property real arcRadius: 0.82
            readonly property real redZoneRadius: 0.83
            readonly property real majorTickReach: 0.92
            readonly property real minorTickReach: 0.88
            readonly property real titleHeight: 0.42
            readonly property real needleReach: 0.95
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                // The pivot sits just below the face; the scale spans
                // +-0.8 rad, so it always fits the width.
                const th = meter.theme
                const cx = width / 2
                const cy = height * dial.pivotHeight
                const r = Math.min(height * dial.radiusByHeight, width * dial.radiusByWidth)
                const angle = vu => {
                    const t = (vu + 20) / 23
                    return -Math.PI / 2 + (t - 0.5) * dial.sweep
                }
                // Scale arc: black to 0, red above.
                ctx.lineWidth = th.strokeBold
                ctx.strokeStyle = th.ink
                ctx.beginPath()
                ctx.arc(cx, cy, r * dial.arcRadius, angle(-20), angle(0))
                ctx.stroke()
                ctx.lineWidth = th.vuRedZoneWidth
                ctx.strokeStyle = th.vuRed
                ctx.beginPath()
                ctx.arc(cx, cy, r * dial.redZoneRadius, angle(0), angle(3))
                ctx.stroke()
                ctx.font = "bold " + th.typeCaption + "px " + th.serif
                ctx.textAlign = "center"
                for (const v of [-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3]) {
                    const a = angle(v)
                    const inner = r * dial.arcRadius
                    const outer = r * (v % 5 === 0 || v === -7 || v === -3 ? dial.majorTickReach : dial.minorTickReach)
                    ctx.strokeStyle = v > 0 ? th.vuRed : th.ink
                    ctx.lineWidth = th.vuTickStroke
                    ctx.beginPath()
                    ctx.moveTo(cx + Math.cos(a) * inner, cy + Math.sin(a) * inner)
                    ctx.lineTo(cx + Math.cos(a) * outer, cy + Math.sin(a) * outer)
                    ctx.stroke()
                    if (v === -20 || v === -10 || v === -5 || v === -3 || v === 0 || v === 3) {
                        ctx.fillStyle = v > 0 ? th.vuRed : th.ink
                        ctx.fillText(Format.signed_number(v, 0), cx + Math.cos(a) * r,
                                     cy + Math.sin(a) * r + th.vuLabelDrop)
                    }
                }
                ctx.font = "bold italic " + th.typeHeading + "px " + th.serif
                ctx.fillStyle = th.ink
                ctx.fillText("VU", cx, cy - r * dial.titleHeight)
                // The pivot's brass cover.
                const pivot = ctx.createLinearGradient(0, cy - th.pivotRadius, 0, cy + th.pivotRadius)
                pivot.addColorStop(0, th.brassHi)
                pivot.addColorStop(1, th.brassLo)
                // Needle.
                const a = angle(meter.needle)
                ctx.strokeStyle = th.vuNeedle
                ctx.lineWidth = th.vuNeedleStroke
                ctx.beginPath()
                ctx.moveTo(cx, cy)
                ctx.lineTo(cx + Math.cos(a) * r * dial.needleReach, cy + Math.sin(a) * r * dial.needleReach)
                ctx.stroke()
                ctx.fillStyle = pivot
                ctx.beginPath()
                ctx.arc(cx, cy, th.pivotRadius, Math.PI, Math.PI * 2)
                ctx.fill()
            }
            Connections {
                target: meter
                function onNeedleChanged() { dial.requestPaint() }
            }
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: meter.theme.vuCaptionInset
            anchors.top: parent.top
            anchors.topMargin: meter.theme.vuCaptionTop
            text: "WET"
            color: meter.theme.vuCaptionInk
            font.pixelSize: meter.theme.typeMicro
            font.bold: true
            font.letterSpacing: meter.theme.tagSpacing
        }
        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: meter.theme.vuLampX
            anchors.top: parent.top
            anchors.topMargin: meter.theme.vuLampY
            width: meter.theme.liveLampSize
            height: meter.theme.liveLampSize
            radius: width / 2
            color: meter.live ? meter.theme.live : meter.theme.vuLampOff
        }
    }

    // Ducking: a strip of lamps, one per 3 dB.
    Item {
        objectName: "relayDuckMeter"
        readonly property real reduction: meter.reduction
        x: meter.theme.duckInset
        y: face.y + face.height + meter.theme.duckGap
        width: parent.width - 2 * meter.theme.duckInset
        height: parent.height - y - meter.theme.duckGap
        Text {
            id: duckLabel
            text: "DUCK"
            color: meter.theme.duckInk
            font.pixelSize: meter.theme.typeMicro
            font.bold: true
            font.letterSpacing: meter.theme.tagSpacing
        }
        Text {
            anchors.right: parent.right
            text: Format.decibels(-meter.reduction)
            color: meter.theme.duckInk
            font.family: meter.theme.mono
            font.pixelSize: meter.theme.typeCaption
            font.bold: true
        }
        Row {
            id: duckLamps
            readonly property int count: 10
            y: duckLabel.height + meter.theme.spaceXs
            spacing: meter.theme.duckLampSpacing
            Repeater {
                model: duckLamps.count
                delegate: Rectangle {
                    required property int index
                    width: (meter.width - 2 * meter.theme.duckInset - (duckLamps.count - 1) * duckLamps.spacing)
                           / duckLamps.count
                    height: meter.theme.duckLampHeight
                    radius: meter.theme.radiusXs
                    color: meter.reduction >= (index + 1) * 3 - 1.5 ? meter.theme.duckLampOn : meter.theme.duckLampOff
                    border.color: meter.theme.brassShadowEdge
                }
            }
        }
    }
}
