pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Relay device panel: a vintage tape-echo machine in cream enamel with
// olive plates and brass hardware. The character switch picks Digital, Tape
// or Analog (the bay shows reels, a bucket-brigade chip or a DSP readout);
// the screen draws the echo train the current values make; a VU meter
// shows the wet level and the ducking. Every control reads and writes
// through `controller` (NativeSynthDeviceController for the Relay engine);
// `relay` (RelayPanelModel) supplies the echo train and live state. No
// project or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    required property var relay
    property RelayTheme theme: RelayTheme {
        accent: characterColors[panel.character]
    }

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int character: controller.revision >= 0 ? controller.choice("relay.character") : 0
    readonly property int routing: controller.revision >= 0 ? controller.choice("relay.routing") : 0
    readonly property bool synced: controller.revision >= 0 && controller.choice("relay.sync") === 1
    readonly property bool wowOff: controller.revision >= 0 && controller.value("relay.mod_depth") <= 0
    // Live engine state (ADR-0037), polled while the panel is visible: fast
    // while the effect publishes, slowly while it does not.
    property var live: ({ live: false })
    readonly property bool liveRunning: live.live === true

    objectName: "relayPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.bodyRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.bodyTop }
        GradientStop { position: panel.theme.bodyMidStop; color: panel.theme.body }
        GradientStop { position: 1; color: panel.theme.bodyBottom }
    }
    border.color: panel.theme.seam
    border.width: panel.theme.strokeBold
    Accessible.role: Accessible.Pane
    Accessible.name: "Relay delay"

    // An olive plate with brass screws and a stencilled title.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        radius: panel.theme.plateRadius
        gradient: Gradient {
            GradientStop { position: 0; color: panel.theme.plateTop }
            GradientStop { position: 1; color: panel.theme.plate }
        }
        border.color: panel.theme.plateEdge
        border.width: panel.theme.plateStroke
        Repeater {
            model: 4
            delegate: Rectangle {
                id: screw
                required property int index
                x: index % 2 === 0 ? panel.theme.screwInset
                                   : plate.width - panel.theme.screwInset - panel.theme.screwSize
                y: index < 2 ? panel.theme.screwInset
                             : plate.height - panel.theme.screwInset - panel.theme.screwSize
                width: panel.theme.screwSize
                height: panel.theme.screwSize
                radius: width / 2
                gradient: Gradient {
                    GradientStop { position: 0; color: panel.theme.brassHi }
                    GradientStop { position: 1; color: panel.theme.brassLo }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: panel.theme.screwSlotLength
                    height: panel.theme.hairline
                    rotation: panel.theme.screwSlotAngle + screw.index * panel.theme.screwSlotTurn
                    color: panel.theme.brassLo
                }
            }
        }
        Text {
            id: plateTitle
            x: panel.theme.plateTitleX
            y: panel.theme.plateTitleY
            text: plate.title
            color: panel.theme.label
            font.pixelSize: panel.theme.typeSmall
            font.bold: true
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
        Text {
            x: plateTitle.x + plateTitle.implicitWidth + panel.theme.plateNoteGap
            y: panel.theme.plateNoteY
            visible: plate.note !== ""
            text: plate.note
            color: panel.theme.labelDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.tagSpacing
        }
    }

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.knobDiameter
        property bool inactive: false
        property string inactiveReason: ""
        property string label: ""
        width: Math.max(diameter + panel.theme.cellWidthPad, panel.theme.cellMinWidth)
        height: diameter + panel.theme.cellHeightPad
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            diameter: cell.diameter
            inactive: cell.inactive
            inactiveReason: cell.inactiveReason
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
        }
    }

    component Caption: Text {
        color: panel.theme.labelDim
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // ---- Header ---------------------------------------------------------

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        // The italic serif wordmark, with the subtitle and model line beside it.
        brand: Component {
            Row {
                leftPadding: panel.theme.wordmarkInset
                spacing: panel.theme.brandSubtitleGap
                Text {
                    objectName: "relayWordmark"
                    text: "Relay"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.bold: true
                    font.italic: true
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: panel.theme.subtitleGap
                    Text {
                        objectName: "relaySubtitle"
                        text: "STEREO ECHO MACHINE"
                        color: panel.theme.subtitleInk
                        font.pixelSize: panel.theme.subtitleSize
                        font.bold: true
                        font.letterSpacing: panel.theme.subtitleSpacing
                    }
                    Text {
                        text: "NATIVE · MODEL R-1"
                        color: panel.theme.inkFaint
                        font.pixelSize: panel.theme.typeMicro
                        font.bold: true
                        font.letterSpacing: panel.theme.modelLineSpacing
                    }
                }
            }
        }

        // The character switch, in the header's second row.
        Item {
            id: characterSwitch
            width: panel.theme.characterWidth
            height: panel.theme.characterHeight
            Text {
                id: characterCaption
                anchors.verticalCenter: parent.verticalCenter
                text: "CHARACTER"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeCaption
                font.bold: true
                font.letterSpacing: panel.theme.characterCaptionSpacing
            }
            Kit.Segment {
                x: characterCaption.implicitWidth + panel.theme.characterSegmentGap
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - x
                height: panel.theme.characterSegmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "relay.character"
            }
        }
    }

    // Brass trim under the header.
    Rectangle {
        id: trim
        x: panel.margin
        y: header.y + header.height + panel.theme.trimGap
        width: panel.width - 2 * panel.margin
        height: panel.theme.trimHeight
        radius: height / 2
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: panel.theme.brassLo }
            GradientStop { position: panel.theme.trimMidStop; color: panel.theme.brassHi }
            GradientStop { position: 1; color: panel.theme.brassLo }
        }
    }

    // ---- Hero: bay, echo train, meter ----------------------------------

    RelayCharacterBay {
        id: bay
        x: panel.margin
        y: trim.y + trim.height + panel.theme.heroGap
        width: panel.theme.bayWidth
        height: panel.theme.heroHeight
        controller: panel.controller
        relay: panel.relay
        theme: panel.theme
    }
    RelayEchoView {
        id: echoView
        x: bay.x + bay.width + panel.gap
        y: bay.y
        width: vu.x - x - panel.gap
        height: bay.height
        controller: panel.controller
        relay: panel.relay
        theme: panel.theme
        live: panel.liveRunning
    }
    RelayVuMeter {
        id: vu
        x: panel.width - panel.margin - width
        y: bay.y
        width: panel.theme.vuWidth
        height: bay.height
        theme: panel.theme
        live: panel.liveRunning
        wetDb: panel.liveRunning ? panel.live.wet_db : -120
        duckDb: panel.liveRunning ? panel.live.duck_db : 0
    }

    // ---- Controls -------------------------------------------------------

    readonly property real plateTop: bay.y + bay.height + theme.plateRowGap
    readonly property real plateHeight: height - plateTop - margin

    Plate {
        id: timePlate
        x: panel.margin
        y: panel.plateTop
        width: panel.theme.timePlateWidth
        height: panel.plateHeight
        title: "TIME"
        note: panel.synced ? "SYNCED TO TEMPO" : "FREE"
        // The big time control: a knob, or the division window when synced.
        Item {
            x: panel.theme.platePad
            y: panel.theme.plateBodyY
            width: panel.theme.timeHeroWidth
            height: panel.theme.timeHeroHeight
            Cell {
                anchors.horizontalCenter: parent.horizontalCenter
                parameterId: "relay.time"
                diameter: panel.theme.heroDiameter
                visible: !panel.synced
            }
            Column {
                anchors.centerIn: parent
                visible: panel.synced
                spacing: panel.theme.spaceSm
                Kit.Dropdown {
                    width: panel.theme.divisionWidth
                    height: panel.theme.divisionHeight
                    fontSize: panel.theme.divisionFontSize
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "relay.division"
                    menu: menu
                }
                Caption {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "DIVISION · " + Format.number(panel.relay.tempo, 0) + " BPM"
                }
            }
        }
        Column {
            x: panel.theme.syncX
            y: panel.theme.syncY
            spacing: panel.theme.spaceSm
            Caption { text: "SYNC" }
            Kit.Segment {
                width: panel.theme.syncWidth
                controller: panel.controller
                theme: panel.theme
                parameterId: "relay.sync"
            }
        }
        Cell {
            x: panel.theme.offsetX
            y: panel.theme.offsetY
            parameterId: "relay.offset"
            label: "R Offset"
            inactive: panel.routing === 2
            inactiveReason: inactive ? "Offset needs Stereo or Ping-Pong routing" : ""
        }
        Column {
            x: panel.theme.platePad
            y: panel.theme.routingY
            spacing: panel.theme.spaceSm
            Caption { text: "ROUTING" }
            Kit.Segment {
                width: timePlate.width - 2 * panel.theme.platePad
                height: panel.theme.routingHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "relay.routing"
                labels: ["STEREO", "PING-PONG", "MONO"]
            }
            Caption {
                text: panel.routing === 1 ? "THE MONO SUM BOUNCES LEFT TO RIGHT"
                    : panel.routing === 2 ? "ONE LINE, HEARD ON BOTH SIDES"
                    : "TWO LINES, LEFT AND RIGHT"
                color: panel.theme.labelDim
                font.letterSpacing: panel.theme.noteSpacing
            }
        }
    }

    Plate {
        id: feedbackPlate
        x: timePlate.x + timePlate.width + panel.gap
        y: panel.plateTop
        width: panel.theme.feedbackPlateWidth
        height: panel.plateHeight
        title: "FEEDBACK"
        note: "IN THE LOOP"
        Cell {
            x: panel.theme.platePad
            y: panel.theme.plateBodyY
            parameterId: "relay.feedback"
            diameter: panel.theme.heroDiameter
        }
        Grid {
            x: panel.theme.feedbackGridX
            y: panel.theme.plateBodyY
            columns: 3
            columnSpacing: panel.theme.feedbackColumnSpacing
            rowSpacing: panel.theme.cellRowSpacing
            Cell { parameterId: "relay.low_cut" }
            Cell { parameterId: "relay.high_cut" }
            Cell {
                parameterId: "relay.drive"
                inactive: panel.character === 0
                inactiveReason: inactive ? "Drive needs the Tape or Analog character" : ""
            }
            Cell { parameterId: "relay.diffusion" }
        }
        // A helper sentence, set in sentence case.
        Caption {
            objectName: "relayFeedbackNote"
            x: panel.theme.platePad
            y: feedbackPlate.height - panel.theme.plateNoteBottom
            width: feedbackPlate.width - 2 * panel.theme.platePad
            text: panel.character === 0 ? "Digital is clean below 0 dBFS; Tape and Analog drive."
                : panel.character === 1 ? "Every pass: the cuts, tape HF loss and tanh saturation."
                : "Every pass: the cuts, the BBD clock filter and a soft clip."
            elide: Text.ElideRight
            font.letterSpacing: panel.theme.noteSpacing
        }
    }

    Plate {
        id: wowPlate
        x: feedbackPlate.x + feedbackPlate.width + panel.gap
        y: panel.plateTop
        width: panel.theme.wowPlateWidth
        height: panel.plateHeight
        title: "WOW"
        note: panel.wowOff ? "NO DEPTH" : "& FLUTTER"
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.plateBodyY
            spacing: panel.theme.cellRowSpacing
            Cell { parameterId: "relay.mod_depth"; diameter: panel.theme.smallDialDiameter }
            Cell {
                parameterId: "relay.mod_rate"
                inactive: panel.wowOff
                inactiveReason: inactive ? "Rate needs some Wow depth" : ""
            }
        }
    }

    Plate {
        id: outputPlate
        x: wowPlate.x + wowPlate.width + panel.gap
        y: panel.plateTop
        width: panel.width - panel.margin - x
        height: panel.plateHeight
        title: "OUTPUT"
        note: panel.routing === 2 ? "WIDTH OFF IN MONO" : ""
        Grid {
            x: panel.theme.outputGridX
            y: panel.theme.plateBodyY
            columns: 2
            columnSpacing: panel.theme.outputColumnSpacing
            rowSpacing: panel.theme.cellRowSpacing
            Cell { parameterId: "relay.ducking"; diameter: panel.theme.smallDialDiameter }
            Cell {
                parameterId: "relay.width"
                diameter: panel.theme.smallDialDiameter
                inactive: panel.routing === 2
                inactiveReason: inactive ? "Width does nothing in Mono routing" : ""
            }
            Cell { parameterId: "relay.mix"; diameter: panel.theme.smallDialDiameter }
            Cell { parameterId: "relay.output"; diameter: panel.theme.smallDialDiameter }
        }
    }

    Timer {
        interval: panel.liveRunning ? 50 : 400
        running: panel.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: panel.live = panel.relay.telemetry()
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
