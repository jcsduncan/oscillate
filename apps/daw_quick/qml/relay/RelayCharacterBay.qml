pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The machine's window onto its character: two tape reels turning past the
// heads for Tape, a bucket-brigade chip with its clock lamp for Analog, and
// a DSP readout for Digital. The reels turn faster for shorter delays, as
// the tape speed would be set.
Rectangle {
    id: bay
    required property var controller
    required property var relay
    required property RelayTheme theme
    readonly property int character: controller.revision >= 0 ? controller.choice("relay.character") : 0
    readonly property real timeMs: relay.revision >= 0 ? relay.time_ms(0) : 375
    // The analog line's clock follows its length (the kernel's law).
    readonly property real clockHz: Math.max(2500, Math.min(12000, 12000 * Math.sqrt(50 / Math.max(1, timeMs))))

    objectName: "relayCharacterBay"
    radius: theme.radiusLg
    gradient: Gradient {
        GradientStop { position: 0; color: bay.theme.bayTop }
        GradientStop { position: 1; color: bay.theme.bayBottom }
    }
    border.color: theme.brassLo
    border.width: theme.strokeBold
    Accessible.role: Accessible.Graphic
    Accessible.name: theme.characterNames[character] + " character"

    Text {
        x: bay.theme.bayTextX
        y: bay.theme.bayNameY
        text: bay.theme.characterNames[bay.character]
        color: bay.theme.accent
        font.pixelSize: bay.theme.typeBody
        font.bold: true
        font.letterSpacing: bay.theme.bayNameSpacing
    }
    Text {
        x: bay.theme.bayTextX
        y: bay.theme.bayTaglineY
        width: parent.width - 2 * bay.theme.bayTextX
        text: bay.theme.characterTaglines[bay.character]
        color: bay.theme.bayCaptionInk
        font.pixelSize: bay.theme.typeMicro
        font.bold: true
        font.letterSpacing: bay.theme.noteSpacing
        elide: Text.ElideRight
    }

    // Tape: two reels and the head block.
    Item {
        id: reels
        objectName: "relayReels"
        readonly property bool spinning: visible && rotation.running
        anchors.fill: parent
        anchors.topMargin: bay.theme.bayStageTop
        visible: bay.character === 1
        property real angle: 0
        // The take-up reel turns faster than the supply reel.
        readonly property real takeupSpeed: 1.2
        NumberAnimation on angle {
            id: rotation
            running: reels.visible
            from: 0
            to: 360
            loops: Animation.Infinite
            // Shorter delays: faster tape.
            duration: Math.max(900, Math.min(6000, bay.timeMs * 8))
        }
        component Reel: Canvas {
            id: reel
            required property real spin
            // The flange's three windows, each this many radians either side.
            readonly property real windowHalfAngle: 0.28
            width: bay.theme.reelSize
            height: bay.theme.reelSize
            onSpinChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const t = bay.theme
                const c = width / 2
                // Wound tape.
                ctx.fillStyle = t.reelTape
                ctx.beginPath()
                ctx.arc(c, c, c - t.reelRimInset, 0, Math.PI * 2)
                ctx.fill()
                ctx.strokeStyle = t.reelTapeRing
                for (let r = t.reelHubRadius; r < c - t.reelRimInset; r += t.reelRingStep) {
                    ctx.beginPath()
                    ctx.arc(c, c, r, 0, Math.PI * 2)
                    ctx.stroke()
                }
                // Metal flange with three windows.
                ctx.save()
                ctx.translate(c, c)
                ctx.rotate(spin * Math.PI / 180)
                ctx.fillStyle = t.reelFlange
                for (let k = 0; k < 3; ++k) {
                    ctx.rotate(Math.PI * 2 / 3)
                    ctx.beginPath()
                    ctx.moveTo(0, 0)
                    ctx.arc(0, 0, c - t.reelFlangeInset, -reel.windowHalfAngle, reel.windowHalfAngle)
                    ctx.closePath()
                    ctx.fill()
                }
                ctx.restore()
                ctx.fillStyle = t.brass
                ctx.beginPath()
                ctx.arc(c, c, t.reelHubCapRadius, 0, Math.PI * 2)
                ctx.fill()
                ctx.fillStyle = t.reelSpindle
                ctx.beginPath()
                ctx.arc(c, c, t.reelSpindleRadius, 0, Math.PI * 2)
                ctx.fill()
            }
        }
        Reel {
            id: supply
            x: bay.theme.reelInset
            y: bay.theme.reelY
            spin: reels.angle
        }
        Reel {
            id: takeup
            x: reels.width - width - bay.theme.reelInset
            y: bay.theme.reelY
            spin: reels.angle * reels.takeupSpeed
        }
        // Tape path past the heads.
        Canvas {
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const t = bay.theme
                // The tape leaves each reel at the bottom of its wound tape.
                const leave = t.reelSize - t.reelRimInset
                ctx.strokeStyle = t.tapePath
                ctx.lineWidth = t.strokeBold
                ctx.beginPath()
                ctx.moveTo(supply.x + t.reelSize / 2, supply.y + leave)
                ctx.lineTo(width / 2 - t.tapeHeadSpan, height - t.headRowBottom)
                ctx.lineTo(width / 2 + t.tapeHeadSpan, height - t.headRowBottom)
                ctx.lineTo(takeup.x + t.reelSize / 2, takeup.y + leave)
                ctx.stroke()
            }
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height - bay.theme.headRowBottom
            spacing: bay.theme.spaceSm
            Repeater {
                model: ["E", "P", "P", "P"]
                delegate: Rectangle {
                    required property string modelData
                    width: bay.theme.headWidth
                    height: bay.theme.headHeight
                    radius: bay.theme.radiusXs
                    color: bay.theme.brass
                    border.color: bay.theme.brassLo
                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: bay.theme.brassInk
                        font.pixelSize: bay.theme.finePrintSize
                        font.bold: true
                    }
                }
            }
        }
    }

    // Analog: the bucket-brigade chip and its clock.
    Item {
        objectName: "relayChip"
        anchors.fill: parent
        anchors.topMargin: bay.theme.bayStageTop
        visible: bay.character === 2
        Rectangle {
            id: chip
            anchors.centerIn: parent
            anchors.verticalCenterOffset: bay.theme.chipOffset
            width: bay.theme.chipWidth
            height: bay.theme.chipHeight
            radius: bay.theme.radiusSm
            color: bay.theme.chipBody
            border.color: bay.theme.chipEdge
            Repeater {
                model: 8
                delegate: Item {
                    id: pin
                    required property int index
                    Rectangle {
                        x: bay.theme.chipPinX + pin.index * bay.theme.chipPinPitch
                        y: -bay.theme.chipPinLength
                        width: bay.theme.chipPinWidth
                        height: bay.theme.chipPinLength
                        color: bay.theme.chipPin
                    }
                    Rectangle {
                        x: bay.theme.chipPinX + pin.index * bay.theme.chipPinPitch
                        y: chip.height
                        width: bay.theme.chipPinWidth
                        height: bay.theme.chipPinLength
                        color: bay.theme.chipPin
                    }
                }
            }
            Rectangle {
                x: bay.theme.chipDotInset
                y: bay.theme.chipDotInset
                width: bay.theme.chipDotSize
                height: bay.theme.chipDotSize
                radius: width / 2
                color: bay.theme.chipDot
            }
            Text {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: bay.theme.chipLabelOffset
                text: "MN3005"
                color: bay.theme.chipLabelInk
                font.family: bay.theme.mono
                font.pixelSize: bay.theme.typeEmphasis
                font.bold: true
            }
            Text {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: bay.theme.chipSubOffset
                text: "4096-STAGE BBD"
                color: bay.theme.chipSubInk
                font.pixelSize: bay.theme.finePrintSize
                font.bold: true
                font.letterSpacing: bay.theme.finePrintSpacing
            }
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: bay.theme.clockRowBottom
            spacing: bay.theme.spaceSm
            Rectangle {
                id: clockLamp
                anchors.verticalCenter: parent.verticalCenter
                width: bay.theme.clockLampSize
                height: bay.theme.clockLampSize
                radius: width / 2
                color: bay.theme.accent
                SequentialAnimation on opacity {
                    running: clockLamp.visible
                    loops: Animation.Infinite
                    NumberAnimation { to: bay.theme.clockLampDim; duration: Math.max(60, 400 * 2500 / bay.clockHz) }
                    NumberAnimation { to: 1; duration: Math.max(60, 400 * 2500 / bay.clockHz) }
                }
            }
            Text {
                text: "CLOCK " + Format.hertz(bay.clockHz) + " LOW-PASS"
                color: bay.theme.clockInk
                font.family: bay.theme.mono
                font.pixelSize: bay.theme.typeCaption
            }
        }
    }

    // Digital: a DSP readout.
    Item {
        objectName: "relayDigital"
        anchors.fill: parent
        anchors.topMargin: bay.theme.bayStageTop
        visible: bay.character === 0
        Rectangle {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: bay.theme.dspReadoutOffset
            width: parent.width - bay.theme.dspReadoutInset
            height: bay.theme.dspReadoutHeight
            radius: bay.theme.radiusMd
            color: bay.theme.dspReadoutBody
            border.color: bay.theme.dspReadoutEdge
            Text {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: bay.theme.dspReadoutValueOffset
                text: Format.milliseconds(bay.timeMs)
                color: bay.theme.accent
                font.family: bay.theme.mono
                font.pixelSize: bay.theme.dspReadoutValueSize
                font.bold: true
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: bay.theme.dspReadoutCaptionBottom
                text: "64-BIT LINE · LAGRANGE READ"
                color: bay.theme.dspReadoutCaptionInk
                font.pixelSize: bay.theme.finePrintSize
                font.bold: true
                font.letterSpacing: bay.theme.finePrintSpacing
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: bay.theme.bayFootBottom
            text: "TIME CHANGES CROSSFADE · PITCH HOLDS"
            color: bay.theme.bayCaptionInk
            font.pixelSize: bay.theme.typeMicro
            font.bold: true
            font.letterSpacing: bay.theme.bayFootSpacing
        }
    }
}
