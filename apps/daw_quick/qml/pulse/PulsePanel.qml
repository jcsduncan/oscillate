pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Pulse device panel: a drum-machine faceplate with twelve pads on keys
// C1..B1, an editor for the selected pad, a mixer and the kit bus. Every
// control reads and writes through `controller` (NativeSynthDeviceController
// for the Pulse engine); no project or audio state lives in QML. Pressing a
// pad selects it and emits padTriggered(key) so root can audition it.
Rectangle {
    id: panel
    required property var controller
    property PulseTheme theme: PulseTheme {}
    property int selectedPad: 0

    signal padTriggered(int key)

    readonly property int firstKey: 36
    readonly property int padCount: padIds.length
    // The closed hat chokes a ringing open hat (Pulse kernel).
    readonly property int closedHatPad: 6
    readonly property int openHatPad: 10
    // The cream pads show their envelope in yellow rather than orange.
    readonly property int firstCreamPad: 9
    readonly property int livePollMs: 33
    readonly property int idlePollMs: 250
    // Live engine state (ADR-0037), polled while the panel is visible: fast
    // while the synth publishes, slowly while it does not.
    property var live: ({ live: false })
    readonly property real liveReduction: live.live === true ? live.gain_reduction : 0
    function padLevel(index) {
        return live.live === true && live.pads !== undefined ? live.pads[index] : 0
    }
    readonly property var padIds: ["kick", "kick2", "snare", "rim", "clap", "tom_low", "chh",
        "tom_mid", "cowbell", "tom_high", "ohh", "cymbal"]
    // What each pad's Character control does.
    readonly property var characterNames: ["Click", "Attack", "Snappy", "Click", "Spread", "Noise",
        "Noise", "Noise", "Body", "Noise", "Noise", "Noise"]
    readonly property var keyNames: ["C1", "C♯1", "D1", "D♯1", "E1", "F1", "F♯1", "G1", "G♯1", "A1",
        "A♯1", "B1"]

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.sectionGap
    readonly property real editY: theme.editorTop
    readonly property real editHeight: theme.editorHeight
    readonly property real mixY: editY + editHeight + gap
    readonly property real mixHeight: theme.mixerHeight
    readonly property real stripY: mixY + mixHeight + gap
    readonly property real columnWidth: (width - 2 * margin) / padCount

    readonly property string padPrefix: "pulse." + padIds[selectedPad] + "."
    readonly property color padTint: theme.padColor(selectedPad)

    objectName: "pulsePanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.bodyLight }
        GradientStop { position: panel.theme.caseGradientMid; color: panel.theme.body }
        GradientStop { position: 1; color: panel.theme.bodyDark }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Pulse drum synthesizer"

    function padName(index) {
        return controller.parameter("pulse." + padIds[index] + ".level").group
    }
    function selectPad(index) {
        selectedPad = Math.max(0, Math.min(padCount - 1, index))
    }
    function pressPad(index) {
        selectPad(index)
        padTriggered(firstKey + index)
    }

    // An engraved section plate with a printed title.
    component Plate: Rectangle {
        id: plate
        required property string title
        radius: panel.theme.radiusLg
        color: panel.theme.plateWash
        border.color: panel.theme.plateRim
        Rectangle {
            x: panel.theme.hairline
            y: panel.theme.hairline
            width: parent.width - 2 * panel.theme.hairline
            height: panel.theme.hairline
            color: panel.theme.plateLip
        }
        Text {
            x: panel.theme.spaceLg
            y: panel.theme.plateTitleTop
            text: plate.title
            color: panel.theme.ink
            font.pixelSize: panel.theme.typeSmall
            font.bold: true
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
    }

    // ---- Header ---------------------------------------------------------------

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.theme.headerTop
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        // The kit list highlights in the pads' orange.
        opener: function(display) {
            menu.openPresets(display, panel.controller, panel.theme.orange)
        }
        // Four slanted colour stripes, the italic wordmark and its tagline.
        brand: Component {
            Item {
                implicitWidth: wordmark.x + Math.max(wordmark.implicitWidth,
                                                     tagline.x - wordmark.x + tagline.implicitWidth)
                implicitHeight: panel.theme.headerHeight
                Row {
                    id: stripes
                    y: panel.theme.stripesTop
                    spacing: panel.theme.stripeGap
                    Repeater {
                        model: panel.theme.padColours
                        delegate: Rectangle {
                            required property color modelData
                            width: panel.theme.stripeWidth
                            height: panel.theme.stripeHeight
                            radius: panel.theme.stripeRadius
                            color: modelData
                            border.color: panel.theme.stripeEdge
                            transform: Matrix4x4 {
                                matrix: Qt.matrix4x4(1, panel.theme.stripeSlant, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
                            }
                        }
                    }
                }
                Text {
                    id: wordmark
                    objectName: "pulseWordmark"
                    x: stripes.width + panel.theme.wordmarkGap
                    text: "PULSE"
                    color: panel.theme.wordmarkInk
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.italic: true
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Text {
                    id: tagline
                    objectName: "pulseSubtitle"
                    x: wordmark.x + panel.theme.taglineIndent
                    y: wordmark.y + wordmark.height - panel.theme.taglineRise
                    text: "ANALOG RHYTHM COMPOSER"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }
    }

    // ---- Selected pad editor ----------------------------------------------------

    Plate {
        id: editor
        objectName: "pulseEditor"
        title: "INSTRUMENT"
        x: panel.margin
        y: panel.editY
        width: panel.theme.editorWidth
        height: panel.editHeight

        Rectangle {
            id: badge
            objectName: "pulsePadBadge"
            x: panel.theme.spaceLg
            y: panel.theme.plateBodyTop
            width: panel.theme.badgeWidth
            height: panel.theme.badgeHeight
            radius: panel.theme.radiusMd
            color: panel.theme.lcd
            border.color: panel.theme.strip
            Accessible.role: Accessible.StaticText
            Accessible.name: "Editing " + badgeName.text + ", key " + panel.keyNames[panel.selectedPad]
            Rectangle {
                x: panel.theme.badgeInset
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.badgeSwatch
                height: panel.theme.badgeSwatch
                radius: panel.theme.radiusSm
                color: panel.padTint
                border.color: Qt.darker(panel.padTint, panel.theme.badgeSwatchRimDarken)
            }
            Text {
                id: badgeName
                objectName: "pulsePadName"
                x: panel.theme.badgeNameX
                y: panel.theme.badgeNameTop
                text: panel.padName(panel.selectedPad).toUpperCase()
                color: panel.theme.lcdText
                font.pixelSize: panel.theme.typeDisplay
                font.bold: true
                font.letterSpacing: panel.theme.badgeNameSpacing
            }
            Text {
                x: panel.theme.badgeCaptionX
                y: badgeName.y + badgeName.height - panel.theme.badgeCaptionTuck
                text: "PAD " + (panel.selectedPad + 1) + " OF " + panel.padCount
                    + (panel.selectedPad === panel.closedHatPad ? "  ·  CHOKES OPEN HAT"
                       : panel.selectedPad === panel.openHatPad ? "  ·  CHOKED BY CLOSED HAT" : "")
                color: panel.theme.stripDim
                font.pixelSize: panel.theme.typeMicro
                font.bold: true
                font.letterSpacing: panel.theme.badgeCaptionSpacing
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: panel.theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                text: panel.keyNames[panel.selectedPad]
                color: panel.theme.orange
                font.pixelSize: panel.theme.typeDisplay
                font.bold: true
            }
        }

        Grid {
            x: panel.theme.spaceMd
            y: panel.theme.padKnobsTop
            columns: panel.theme.padKnobColumns
            columnSpacing: 0
            rowSpacing: panel.theme.spaceMd
            Repeater {
                model: ["tune", "decay", "tone", "character", "drive", "velocity", "level", "pan"]
                delegate: Item {
                    id: cell
                    required property string modelData
                    width: panel.theme.padKnobCellWidth
                    height: panel.theme.padKnobCellHeight
                    Kit.Knob {
                        id: padKnob
                        anchors.horizontalCenter: parent.horizontalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.padPrefix + cell.modelData
                        diameter: panel.theme.padKnobDiameter
                        accent: panel.padTint
                        label: cell.modelData === "character" ? panel.characterNames[panel.selectedPad]
                            : padKnob.info.name !== undefined ? padKnob.info.name : ""
                        longName: panel.padName(panel.selectedPad) + " " + padKnob.label.toLowerCase()
                    }
                }
            }
        }

        // Both displays draw the selected pad's settings: PREVIEW.
        Kit.PlotFrame {
            objectName: "pulseEnvelopeFrame"
            x: panel.theme.editorDisplayX
            y: panel.theme.plateBodyTop
            width: editor.width - x - panel.theme.spaceLg
            height: panel.theme.envelopeHeight
            theme: panel.theme
            xTicks: envelope.timeTicks
            xUnit: "s"
            PulseEnvelopeView {
                id: envelope
                anchors.fill: parent
                controller: panel.controller
                theme: panel.theme
                pad: panel.selectedPad
                decayId: panel.padPrefix + "decay"
                accent: panel.selectedPad >= panel.firstCreamPad ? panel.theme.yellow : panel.theme.orange
            }
        }
        Kit.PlotFrame {
            objectName: "pulseShapeFrame"
            x: panel.theme.editorDisplayX
            y: panel.theme.shapeTop
            width: editor.width - x - panel.theme.spaceLg
            height: editor.height - y - panel.theme.spaceLg
            theme: panel.theme
            xTicks: shapeView.timeTicks
            xUnit: shapeView.timeUnit
            PulseShapeView {
                id: shapeView
                anchors.fill: parent
                controller: panel.controller
                theme: panel.theme
                pad: panel.selectedPad
                accent: panel.theme.yellow
            }
        }
    }

    // ---- Kit bus -----------------------------------------------------------------

    Plate {
        id: bus
        objectName: "pulseBus"
        title: "KIT BUS"
        x: editor.x + editor.width + panel.gap
        y: panel.editY
        width: panel.width - panel.margin - x
        height: panel.editHeight

        Grid {
            x: panel.theme.busInset
            y: panel.theme.busKnobsTop
            columns: panel.theme.busColumns
            columnSpacing: panel.theme.spaceXs
            rowSpacing: panel.theme.busRowGap
            Repeater {
                model: [
                    { id: "pulse.bus.glue", accent: "red" },
                    { id: "pulse.bus.release", accent: "red" },
                    { id: "pulse.bus.saturation", accent: "orange" },
                    { id: "pulse.out.level", accent: "cream" }
                ]
                delegate: Item {
                    id: busCell
                    required property var modelData
                    width: panel.theme.busCellWidth
                    height: panel.theme.busCellHeight
                    Kit.Knob {
                        id: busKnob
                        anchors.horizontalCenter: parent.horizontalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: busCell.modelData.id
                        diameter: panel.theme.busKnobDiameter
                        accent: panel.theme[busCell.modelData.accent]
                        label: busCell.modelData.id === "pulse.out.level" ? "Output"
                             : busKnob.info.name !== undefined ? busKnob.info.name : ""
                    }
                }
            }
        }
        Rectangle {
            x: panel.theme.busDividerX
            y: panel.theme.busDividerTop
            width: panel.theme.hairline
            height: bus.height - panel.theme.busDividerTrim
            color: panel.theme.dividerInk
        }
        PulseGlueMeter {
            x: panel.theme.glueMeterX
            y: panel.theme.busKnobsTop
            height: bus.height - panel.theme.glueMeterTrim
            theme: panel.theme
            glue: panel.controller.revision >= 0 ? panel.controller.value("pulse.bus.glue") : 0
            reduction: panel.liveReduction
        }
    }

    // ---- Mixer -------------------------------------------------------------------

    Plate {
        id: mixer
        objectName: "pulseMixer"
        title: "MIXER"
        x: panel.margin
        y: panel.mixY
        width: panel.width - 2 * panel.margin
        height: panel.mixHeight

        Repeater {
            model: panel.padCount
            delegate: Item {
                id: channel
                required property int index
                readonly property string prefix: "pulse." + panel.padIds[index] + "."
                readonly property bool selected: index === panel.selectedPad
                x: index * panel.columnWidth
                y: 0
                width: panel.columnWidth
                height: mixer.height

                Rectangle {
                    x: panel.theme.channelInset
                    y: panel.theme.channelTop
                    width: parent.width - 2 * panel.theme.channelInset
                    height: parent.height - panel.theme.channelTrim
                    radius: panel.theme.radiusMd
                    color: channel.selected ? panel.theme.channelSelectedWash : "transparent"
                    border.color: channel.selected ? panel.theme.channelSelectedRim : "transparent"
                }
                Rectangle {
                    visible: channel.index > 0
                    x: 0
                    y: panel.theme.channelRuleTop
                    width: panel.theme.hairline
                    height: parent.height - panel.theme.channelRuleTrim
                    color: panel.theme.channelRuleInk
                }
                Kit.Knob {
                    x: panel.theme.spaceSm
                    y: panel.theme.channelPanTop
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: channel.prefix + "pan"
                    diameter: panel.theme.channelPanDiameter
                    accent: panel.theme.padColor(channel.index)
                    longName: panel.padName(channel.index) + " pan"
                }
                Text {
                    x: panel.theme.spaceSm
                    width: panel.theme.channelPanReadoutWidth
                    y: channelFader.y + channelFader.height + panel.theme.channelReadoutGap
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.controller.revision >= 0 ? panel.controller.display(channel.prefix + "pan") : ""
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                }
                Kit.Fader {
                    id: channelFader
                    x: parent.width - width - panel.theme.spaceLg
                    y: panel.theme.channelFaderTop
                    height: parent.height - panel.theme.channelFaderTrim
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: channel.prefix + "level"
                    accent: panel.theme.padColor(channel.index)
                    longName: panel.padName(channel.index) + " level"
                }
                Text {
                    x: channelFader.x + channelFader.width / 2 - width / 2
                    width: panel.theme.channelLevelReadoutWidth
                    y: channelFader.y + channelFader.height + panel.theme.channelReadoutGap
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.controller.revision >= 0 ? panel.controller.display(channel.prefix + "level") : ""
                    color: channel.selected ? panel.theme.red : panel.theme.inkDim
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                }
            }
        }
    }

    // ---- Instrument strip and pads -------------------------------------------------

    Rectangle {
        id: strip
        objectName: "pulseInstrumentStrip"
        x: panel.margin
        y: panel.stripY
        width: panel.width - 2 * panel.margin
        height: panel.theme.stripHeight
        radius: panel.theme.radiusMd
        color: panel.theme.strip
        border.color: panel.theme.stripLine
        Repeater {
            model: panel.padCount
            delegate: Item {
                id: label
                required property int index
                x: index * panel.columnWidth
                width: panel.columnWidth
                height: strip.height
                Rectangle {
                    visible: label.index > 0
                    y: panel.theme.stripRuleInset
                    width: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.stripRuleInset
                    color: panel.theme.stripLine
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: panel.theme.stripNameTop
                    width: parent.width - panel.theme.spaceSm
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.padName(label.index).toUpperCase()
                    color: label.index === panel.selectedPad ? panel.theme.orange : panel.theme.stripText
                    font.pixelSize: panel.theme.typeBody
                    font.bold: true
                    font.letterSpacing: panel.theme.legendSpacing
                    elide: Text.ElideRight
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: panel.theme.stripKeyTop
                    text: panel.keyNames[label.index]
                    color: panel.theme.stripDim
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                }
                MouseArea {
                    readonly property string keyboardAlternative: "the pads below take Tab focus; the arrow keys select a pad"
                    anchors.fill: parent
                    onClicked: panel.selectPad(label.index)
                }
            }
        }
    }

    Repeater {
        id: pads
        model: panel.padCount
        delegate: PulsePad {
            id: padItem
            theme: panel.theme
            name: panel.padName(padItem.index)
            keyName: panel.keyNames[padItem.index]
            selected: padItem.index === panel.selectedPad
            level: panel.padLevel(padItem.index)
            x: panel.margin + padItem.index * panel.columnWidth + (panel.columnWidth - width) / 2
            y: strip.y + strip.height + panel.theme.spaceMd
            width: panel.columnWidth - panel.theme.spaceMd
            height: panel.height - y - panel.theme.padsBottomMargin
            onPressed: panel.pressPad(padItem.index)
            Keys.onLeftPressed: {
                const next = Math.max(0, padItem.index - 1)
                panel.selectPad(next)
                pads.itemAt(next).forceActiveFocus()
            }
            Keys.onRightPressed: {
                const next = Math.min(panel.padCount - 1, padItem.index + 1)
                panel.selectPad(next)
                pads.itemAt(next).forceActiveFocus()
            }
        }
    }

    Timer {
        objectName: "pulseTelemetryPoll"
        interval: panel.live.live === true ? panel.livePollMs : panel.idlePollMs
        repeat: true
        running: panel.visible
        onTriggered: panel.live = panel.controller.telemetry()
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
