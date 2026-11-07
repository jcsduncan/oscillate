pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The Concert device panel, "Concert black": a gloss-black lacquer case
// with brass hardware and gold-leaf lettering, for the sampled grand
// (ADR-0071). The library leads: what is installed, its attribution, or
// how to install it; beside it the strings, dampers and pedals seen with
// the lid up. Below, the controls grouped as a technician voices a sampled
// piano: Voicing, Dampers & Pedals, Mechanics, Stereo and Output. Every
// control reads and writes through `controller`
// (NativeSynthDeviceController for Concert); no project or audio state
// lives in QML.
Rectangle {
    id: panel
    required property var controller
    // Live engine state (ADR-0037), polled while the panel is visible: fast
    // while the synth publishes, slowly while it does not.
    property var live: ({ live: false })
    property ConcertTheme theme: ConcertTheme {}
    // The sample library's state (concert_library), read again when the
    // panel opens, when the patch changes and when the user asks.
    property int libraryLook: 0
    readonly property var library: controller.revision >= 0 && libraryLook >= 0
        ? controller.concert_library() : ({})
    readonly property bool ready: library.state === "ready"

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    // Where the lacquer's mid tone sits in the case gradient.
    readonly property real lacquerStop: 0.35

    objectName: "concertPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.lacquerTop }
        GradientStop { position: panel.lacquerStop; color: panel.theme.lacquer }
        GradientStop { position: 1; color: panel.theme.lacquerBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Concert grand piano"

    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group === info.name ? info.name : info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || controller.revision < 0 ? "" : controller.display(id)
    }

    // The lacquer's reflection: a soft diagonal sheen across the case.
    Canvas {
        // The sheen's run down the case and where its band peaks and fades.
        readonly property real sheenDepth: 0.6
        readonly property real sheenPeak: 0.28
        readonly property real sheenFade: 0.36
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const s = panel.theme.sheen
            const g = ctx.createLinearGradient(0, 0, width, height * sheenDepth)
            g.addColorStop(0, Qt.rgba(s.r, s.g, s.b, 0))
            g.addColorStop(sheenPeak, Qt.rgba(s.r, s.g, s.b, panel.theme.sheenAlpha))
            g.addColorStop(sheenFade, Qt.rgba(s.r, s.g, s.b, panel.theme.sheenTailAlpha))
            g.addColorStop(1, Qt.rgba(s.r, s.g, s.b, 0))
            ctx.fillStyle = g
            ctx.fillRect(0, 0, width, height)
        }
    }

    // A lacquered section framed by a fine brass line, titled in gold.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        radius: panel.theme.radiusLg
        color: Qt.rgba(panel.theme.plate.r, panel.theme.plate.g, panel.theme.plate.b, panel.theme.plateAlpha)
        border.color: panel.theme.plateEdge
        Rectangle {
            anchors.fill: parent
            anchors.margins: panel.theme.plateFrameInset
            radius: panel.theme.radiusMd
            color: "transparent"
            border.color: Qt.rgba(panel.theme.brass.r, panel.theme.brass.g, panel.theme.brass.b,
                                  panel.theme.plateFrameAlpha)
        }
        Text {
            id: plateTitle
            x: panel.theme.plateInset
            y: panel.theme.plateTitleY
            text: plate.title
            color: panel.theme.gold
            font.family: panel.theme.serif
            font.pixelSize: panel.theme.typeSmall
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
        Text {
            x: plateTitle.x + plateTitle.implicitWidth + panel.theme.plateNoteGap
            y: panel.theme.plateNoteY
            visible: plate.note !== ""
            text: plate.note
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
    }

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.knobDiameter
        property string label: ""
        width: panel.theme.cellWidth
        height: diameter + panel.theme.cellCaptionHeight
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            diameter: cell.diameter
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
        }
    }

    // A plate holding a centred row of knobs.
    component KnobPlate: Plate {
        id: knobPlate
        required property var parameters
        property var labels: []
        Row {
            id: knobRow
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.plateBodyY + (knobPlate.height - panel.theme.plateBodyY - knobRow.height) / 2
            Repeater {
                model: knobPlate.parameters
                delegate: Cell {
                    required property string modelData
                    required property int index
                    parameterId: modelData
                    label: knobPlate.labels.length > index ? knobPlate.labels[index] : ""
                }
            }
        }
    }

    // ---- Header ---------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.spaceXxs
            text: "CONCERT"
            color: panel.theme.gold
            font.family: panel.theme.serif
            font.pixelSize: panel.theme.wordmarkSize
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.wordmarkRuleGap
            width: wordmark.implicitWidth - panel.theme.wordmarkRuleTrim
            height: panel.theme.hairline
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.brassHi }
                GradientStop { position: 1; color: Qt.rgba(panel.theme.brass.r, panel.theme.brass.g, panel.theme.brass.b, 0) }
            }
        }
        Text {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.wordmarkSubtitleGap
            text: "SAMPLED GRAND"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.wordmarkSubtitleSpacing
        }

        // Preset browser and readout.
        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.spaceXs
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "concertPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "concertPresetDisplay"
                width: panel.theme.displayWidth
                height: panel.theme.displayHeight
                radius: panel.theme.displayRadius
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.gold : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.gold)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Text {
                    x: panel.theme.plateInset
                    y: panel.theme.displayCaptionY
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
                Text {
                    objectName: "concertPresetName"
                    x: panel.theme.plateInset
                    y: panel.theme.displayNameY
                    width: panel.theme.displayNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.serif
                    font.pixelSize: panel.theme.typeTitle
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.theme.displayDividerX
                    y: panel.theme.displayDividerInset
                    width: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.displayDividerInset
                    color: panel.theme.wellEdge
                }
                Text {
                    objectName: "concertReadout"
                    x: panel.theme.displayReadoutX
                    y: panel.theme.displayCaptionY
                    width: display.width - x - panel.theme.displayReadoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "concertReadoutValue"
                    x: panel.theme.displayReadoutX
                    y: panel.theme.displayValueY
                    width: display.width - x - panel.theme.displayReadoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.gold
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.displayChevronMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.brass
                    font.pixelSize: panel.theme.typeBody
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: panel.controller.editable
                    onClicked: {
                        display.forceActiveFocus()
                        display.openPresets()
                    }
                }
            }
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "concertPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "concertStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- The library and the strings -----------------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Plate {
        id: libraryPlate
        objectName: "concertLibrary"
        title: "LIBRARY"
        note: panel.ready ? "INSTALLED" : panel.library.state === "invalid" ? "DAMAGED" : "NOT INSTALLED"
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.libraryWidth
        height: panel.rowAHeight
        Accessible.role: Accessible.Pane
        Accessible.name: "Piano library"
        Accessible.description: panel.ready ? panel.library.name + ", " + panel.library.attribution
                                            : panel.library.message !== undefined ? panel.library.message : ""
        Column {
            x: panel.theme.plateInset
            y: panel.theme.plateTextY
            width: libraryPlate.width - 2 * panel.theme.plateInset
            spacing: panel.theme.spaceSm
            Text {
                objectName: "concertLibraryName"
                width: parent.width
                text: panel.ready ? panel.library.name : "No piano to play"
                color: panel.theme.ink
                font.family: panel.theme.serif
                font.pixelSize: panel.theme.typeHeading
                wrapMode: Text.WordWrap
            }
            Text {
                objectName: "concertLibraryMessage"
                width: parent.width
                visible: !panel.ready
                text: (panel.library.message !== undefined ? panel.library.message + ". " : "")
                    + "Concert is silent until its library is installed. It plays "
                    + (panel.library.required !== undefined ? panel.library.required : "") + "."
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeSmall
                wrapMode: Text.WordWrap
            }
            Text {
                visible: !panel.ready
                text: "INSTALL FROM THE SOURCE TREE"
                color: panel.theme.inkFaint
                font.pixelSize: panel.theme.typeMicro
                font.bold: true
                font.letterSpacing: panel.theme.captionSpacing
            }
            Rectangle {
                visible: !panel.ready
                width: parent.width
                height: command.implicitHeight + 2 * panel.theme.spaceSm
                radius: panel.theme.radiusMd
                color: panel.theme.well
                border.color: panel.theme.wellEdge
                Text {
                    id: command
                    objectName: "concertInstallCommand"
                    x: panel.theme.spaceMd
                    y: panel.theme.spaceSm
                    width: parent.width - 2 * panel.theme.spaceMd
                    text: panel.library.install_command !== undefined ? panel.library.install_command : ""
                    color: panel.theme.gold
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSmall
                    wrapMode: Text.WrapAnywhere
                }
            }
            Row {
                visible: !panel.ready
                spacing: panel.theme.spaceMd
                LibraryAction {
                    objectName: "concertCopyInstall"
                    label: "COPY COMMAND"
                    onActivated: panel.controller.set_clipboard_text(command.text)
                }
                LibraryAction {
                    objectName: "concertLibraryReload"
                    label: "LOOK AGAIN"
                    onActivated: {
                        panel.controller.concert_reload_library()
                        ++panel.libraryLook
                    }
                }
            }
            Text {
                objectName: "concertAttribution"
                width: parent.width
                visible: panel.ready
                text: panel.ready ? panel.library.attribution : ""
                color: panel.theme.gold
                font.pixelSize: panel.theme.typeSmall
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                visible: panel.ready
                text: panel.ready
                    ? "Sixteen velocity layers, sampled every minor third, with key-release, "
                      + "string-release and pedal recordings. " + Format.quantity(panel.library.resident_mb, "MB", 0)
                      + " stay in memory; the rest streams from disk."
                    : ""
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeSmall
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                visible: panel.ready
                text: panel.ready ? panel.library.directory : ""
                color: panel.theme.inkFaint
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeMicro
                wrapMode: Text.WrapAnywhere
            }
            Text {
                objectName: "concertUnderruns"
                width: parent.width
                visible: panel.live.live === true && panel.live.underruns > 0
                text: panel.live.underruns > 0
                    ? "The disk was late " + panel.live.underruns + " times: those notes dropped out."
                    : ""
                color: panel.theme.feltLifted
                font.pixelSize: panel.theme.typeSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    // A small brass push button for the library's actions.
    component LibraryAction: Rectangle {
        id: action
        required property string label
        signal activated()
        width: actionText.implicitWidth + panel.theme.actionPad
        height: panel.theme.actionHeight
        radius: panel.theme.radiusMd
        color: actionArea.containsMouse || activeFocus ? panel.theme.brassLo : panel.theme.plate
        border.color: activeFocus ? panel.theme.gold : panel.theme.brass
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text {
            id: actionText
            anchors.centerIn: parent
            text: action.label
            color: panel.theme.gold
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        MouseArea {
            id: actionArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: action.activated()
        }
    }

    Plate {
        id: strings
        title: "STRINGS · DAMPERS · PEDALS"
        x: libraryPlate.x + libraryPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        ConcertStringsView {
            id: stringsView
            x: panel.theme.spaceLg
            y: panel.theme.plateBodyY
            width: strings.width - 2 * panel.theme.spaceLg
            height: strings.height - panel.theme.plateBodyY - panel.theme.spaceMd
            theme: panel.theme
            library: panel.library
            live: panel.live.live === true
            energies: panel.live.live === true ? panel.live.energies : []
            dampers: panel.live.live === true ? panel.live.dampers : []
            pedals: panel.live.live === true
                ? { sustain: panel.live.sustain, sostenuto: panel.live.sostenuto, soft: panel.live.soft }
                : { sustain: 0, sostenuto: 0, soft: 0 }
        }
    }

    // ---- Controls ----------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: theme.rowBHeight
    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: theme.rowCHeight
    readonly property real thirdWidth: (width - 2 * margin - 2 * gap) / 3

    KnobPlate {
        id: voicing
        title: "VOICING"
        x: panel.margin
        y: panel.rowBY
        width: panel.thirdWidth
        height: panel.rowBHeight
        parameters: ["concert.tone", "concert.touch", "concert.dynamics"]
    }
    KnobPlate {
        id: dampersPlate
        title: "DAMPERS & PEDALS"
        x: voicing.x + voicing.width + panel.gap
        y: panel.rowBY
        width: panel.thirdWidth
        height: panel.rowBHeight
        parameters: ["concert.damper", "concert.una_corda", "concert.resonance"]
    }
    KnobPlate {
        id: mechanics
        title: "MECHANICS"
        note: "RECORDED WITH THE PIANO"
        x: dampersPlate.x + dampersPlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        parameters: ["concert.noise.release", "concert.noise.strings", "concert.noise.pedal"]
        labels: ["Keys", "Strings", "Pedal"]
    }

    Plate {
        id: stereo
        title: "STEREO"
        x: panel.margin
        y: panel.rowCY
        width: panel.thirdWidth
        height: panel.rowCHeight
        Cell {
            x: panel.theme.stereoKnobX
            y: panel.theme.stereoKnobY
            parameterId: "concert.width"
        }
        Text {
            x: panel.theme.stereoChoiceX
            y: panel.theme.stereoCaptionY
            text: "PERSPECTIVE"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        Kit.Segment {
            x: panel.theme.stereoChoiceX
            y: panel.theme.stereoChoiceY
            width: stereo.width - panel.theme.stereoChoiceX - panel.theme.plateInset
            height: panel.theme.stereoChoiceHeight
            vertical: true
            controller: panel.controller
            theme: panel.theme
            parameterId: "concert.perspective"
            captions: ["Bass on the left", "Mirrored, from the hall"]
        }
    }
    KnobPlate {
        id: output
        title: "OUTPUT"
        x: stereo.x + stereo.width + panel.gap
        y: panel.rowCY
        width: panel.thirdWidth
        height: panel.rowCHeight
        parameters: ["concert.tune", "concert.level"]
    }
    Plate {
        id: about
        objectName: "concertAbout"
        title: "ABOUT"
        x: output.x + output.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Text {
            objectName: "concertAboutText"
            x: panel.theme.plateInset
            y: panel.theme.plateTextY
            width: about.width - 2 * panel.theme.plateInset
            text: (panel.library.required !== undefined ? panel.library.required : "")
                + ". Converted for Concert by its install script; the recordings are otherwise "
                + "unchanged. Licensed under Creative Commons Attribution 3.0."
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeSmall
            wrapMode: Text.WordWrap
        }
    }

    Timer {
        // Fast while the synth publishes, slowly while it does not (ms).
        readonly property int livePoll: 33
        readonly property int idlePoll: 250
        objectName: "concertTelemetryPoll"
        interval: panel.live.live === true ? livePoll : idlePoll
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
