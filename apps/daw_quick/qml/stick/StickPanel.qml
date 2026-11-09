pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The Stick drum kit panel (ADR-0114), as the approved mockup lays it out:
// the shared header with the library chip and presets; the kit picker and
// the Dry, Studio and Room mixes; the kit from above (click a piece) with
// the hi-hat pedal; the mic mixer (a strip per mic group: meter, fader, mute
// and solo) with Bleed; the chosen piece's seven controls; and the kit row
// (strikers, round robins, Humanise, Level, the voices sounding).
//
// Values go through `controller` (NativeSynthDeviceController); the kit's
// library and layout come from stick_library and stick_layout, live state
// from its telemetry, polled while the panel is visible. Without the chosen
// kit the mixer gives way to the shared library notice.
Rectangle {
    id: panel
    required property var controller
    property StickTheme theme: StickTheme {}

    // Live engine state (ADR-0037), polled while the panel is visible.
    property var live: ({ live: false })
    // The kit's library (stick_library), read again when the user asks or while it loads.
    property int libraryLook: 0
    readonly property var library: controller.revision >= 0 && libraryLook >= 0
        ? controller.stick_library() : ({})
    readonly property bool ready: library.state === "ready"
    readonly property var layout: controller.revision >= 0 && libraryLook >= 0
        ? controller.stick_layout() : ({})
    readonly property var pieces: layout.pieces !== undefined ? layout.pieces : []
    readonly property var groups: (layout.groups !== undefined ? layout.groups : [])
        .filter(g => g.channels > 0 || !panel.ready)
    property bool libraryOpen: false
    // The piece whose controls show: the snare until another is picked, and
    // again when the kit chosen lacks the one picked (every kit has a snare).
    readonly property int snarePiece: 1
    property int selected: snarePiece
    onPiecesChanged: {
        if (selected < pieces.length && pieces[selected].present === false)
            selected = snarePiece
    }
    readonly property var piece: selected < pieces.length ? pieces[selected] : ({ id: "snare", name: "Snare" })
    readonly property bool kitHasBrushes: pieces.some(p => p.brushes === true)

    readonly property real margin: theme.panelMargin
    readonly property real gap: theme.panelGap
    readonly property real pickerY: header.y + header.height + gap
    readonly property real bodyY: pickerY + theme.pickerHeight + gap
    readonly property real kitRowY: height - margin - theme.kitRowHeight
    readonly property real bodyHeight: kitRowY - gap - bodyY
    // A velocity curve this far from linear reads as soft or hard.
    readonly property real curveDeadband: 0.15
    readonly property int pieceKnobs: 7
    readonly property real msPerSecond: 1000

    objectName: "stickPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.cardRadius
    color: theme.caseColor
    border.color: theme.cardEdge
    Accessible.role: Accessible.Pane
    Accessible.name: "Stick acoustic drums"

    function libraryChipText() {
        const kit = controller.revision >= 0 ? controller.parameter("stick.kit").choices[controller.choice("stick.kit")] : ""
        if (ready)
            return "Library ready · " + kit + " · " + library.mics + " mics · "
                + Format.quantity(library.resident_mb, "MB", 0)
        if (library.state === "loading")
            return "Loading " + kit
        return kit + (library.state === "invalid" ? " damaged" : " not installed")
    }
    function curveText(value) {
        return value < -curveDeadband ? "Soft" : value > curveDeadband ? "Hard" : "Linear"
    }
    function pieceParameter(control) {
        return "stick." + piece.id + "." + control
    }

    component Card: Rectangle {
        radius: panel.theme.cardRadius
        color: panel.theme.card
        border.color: panel.theme.cardEdge
    }
    component Heading: Text {
        color: panel.theme.inkDim
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.smallSize
        font.weight: Font.DemiBold
        font.letterSpacing: panel.theme.headingSpacing
    }
    component Note: Text {
        color: panel.theme.inkDim
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.smallSize
    }

    // ---- Header ------------------------------------------------------------------------------

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.margin
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        wordmark: "STICK"
        subtitle: "SAMPLED ACOUSTIC DRUMS · MULTI-MIC"
        tools: Component {
            // The kit's state, mics and size; it opens the library.
            Rectangle {
                id: chip
                objectName: "stickLibraryChip"
                width: chipRow.implicitWidth + 2 * panel.theme.tilePad
                height: panel.theme.statusHeight
                radius: height / 2
                color: chipArea.containsMouse ? panel.theme.buttonHi : "transparent"
                border.color: chip.activeFocus ? panel.theme.focusRing : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: panel.libraryChipText()
                Accessible.description: "Show the library"
                Keys.onReturnPressed: panel.libraryOpen = !panel.libraryOpen
                Keys.onSpacePressed: panel.libraryOpen = !panel.libraryOpen
                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: panel.theme.captionGap
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.statusLampSize
                        height: width
                        radius: width / 2
                        color: panel.ready ? panel.theme.live : panel.theme.warning
                    }
                    Text {
                        objectName: "stickLibraryChipText"
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.libraryChipText()
                        color: panel.theme.inkDim
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.smallSize
                    }
                }
                MouseArea {
                    id: chipArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.libraryOpen = !panel.libraryOpen
                }
            }
        }
    }

    // ---- Kit and mix --------------------------------------------------------------------------

    Kit.ChoiceTiles {
        objectName: "stickKits"
        x: panel.margin
        y: panel.pickerY
        width: panel.width - 2 * panel.margin - panel.gap - panel.theme.mixWidth
        controller: panel.controller
        theme: panel.theme
        parameterId: "stick.kit"
        segment: true
        tileHeight: panel.theme.pickerHeight
        nameSize: panel.theme.bodySize
        hints: (panel.library.kits !== undefined ? panel.library.kits : [])
            .map(k => k.description + (k.installed ? "" : " · not installed"))
    }
    Kit.ChoiceTiles {
        objectName: "stickMix"
        x: panel.width - panel.margin - panel.theme.mixWidth
        y: panel.pickerY
        width: panel.theme.mixWidth
        controller: panel.controller
        theme: panel.theme
        parameterId: "stick.mix"
        segment: true
        tileHeight: panel.theme.pickerHeight
        nameSize: panel.theme.bodySize
    }

    // ---- The kit from above -------------------------------------------------------------------

    Card {
        id: kitCard
        objectName: "stickKitCard"
        x: panel.margin
        y: panel.bodyY
        width: panel.theme.kitWidth
        height: panel.bodyHeight

        Item {
            id: kitTitle
            x: panel.theme.cardPad
            y: panel.theme.cardPad
            width: kitCard.width - 2 * panel.theme.cardPad
            height: panel.theme.titleHeight
            Heading {
                anchors.verticalCenter: parent.verticalCenter
                text: "KIT · CLICK A PIECE"
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "GM map"
                color: panel.theme.inkDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.smallSize
            }
        }
        StickKit {
            x: panel.theme.cardPad
            y: kitTitle.y + kitTitle.height + panel.theme.titleGap
            width: kitCard.width - 2 * panel.theme.cardPad
            height: pedal.y - panel.theme.titleGap - y
            theme: panel.theme
            pieces: panel.pieces
            levels: panel.live.live === true && panel.live.piece_levels !== undefined
                ? panel.live.piece_levels : []
            selected: panel.selected
            onPicked: index => panel.selected = index
        }
        Column {
            id: pedal
            x: panel.theme.cardPad
            y: kitCard.height - panel.theme.cardPad - height
            width: kitCard.width - 2 * panel.theme.cardPad
            spacing: panel.theme.hintGap
            Item {
                width: parent.width
                height: panel.theme.titleHeight
                Note { anchors.verticalCenter: parent.verticalCenter; text: "Closed" }
                Note {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Open"
                }
            }
            Kit.Slider {
                objectName: "stickPedal"
                width: parent.width
                controller: panel.controller
                theme: panel.theme
                parameterId: "stick.perf.pedal"
                label: "Hi-hat openness (pedal)"
            }
        }
    }

    // ---- Mics ----------------------------------------------------------------------------------

    Card {
        id: mixer
        objectName: "stickMixer"
        x: kitCard.x + kitCard.width + panel.gap
        y: panel.bodyY
        width: panel.width - panel.margin - x
        height: panel.bodyHeight - panel.gap - panel.theme.pieceHeight

        Item {
            id: mixerTitle
            x: panel.theme.cardPad
            y: panel.theme.cardPad
            width: mixer.width - 2 * panel.theme.cardPad
            height: panel.theme.statusHeight
            Heading {
                anchors.verticalCenter: parent.verticalCenter
                text: "MICS · THE KIT'S OWN MIX"
            }
            Kit.Switch {
                objectName: "stickBleed"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
                parameterId: "stick.bleed"
                label: "Bleed between mics"
            }
        }
        Row {
            id: strips
            x: panel.theme.cardPad
            y: mixerTitle.y + mixerTitle.height + panel.theme.titleGap
            width: mixer.width - 2 * panel.theme.cardPad
            height: mixer.height - y - panel.theme.cardPad
            spacing: panel.theme.tileGap
            visible: panel.ready
            readonly property real stripWidth: Math.max(panel.theme.stripMinWidth,
                (width - (panel.groups.length - 1) * spacing) / Math.max(1, panel.groups.length))
            Repeater {
                model: panel.ready ? panel.groups : []
                Rectangle {
                    id: strip
                    required property var modelData
                    readonly property string base: "stick.mic." + modelData.id
                    readonly property real meterDb: panel.live.live === true && panel.live.mic_db !== undefined
                        ? panel.live.mic_db[modelData.group] : panel.theme.meterFloorDb
                    readonly property real meterLevel: Math.max(0, Math.min(1,
                        (meterDb - panel.theme.meterFloorDb) / (panel.theme.meterCeilDb - panel.theme.meterFloorDb)))
                    readonly property bool muted: panel.controller.revision >= 0
                        && panel.controller.choice(base + ".mute") === 1
                    objectName: "stickStrip:" + modelData.id
                    width: strips.stripWidth
                    height: strips.height
                    radius: panel.theme.tileRadius
                    color: panel.theme.caseColor
                    Accessible.role: Accessible.Grouping
                    Accessible.name: modelData.name + " mics"

                    Text {
                        id: stripName
                        y: panel.theme.tileGap
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: strip.modelData.name
                        color: panel.theme.ink
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.smallSize
                        font.weight: Font.DemiBold
                        wrapMode: Text.WordWrap
                        maximumLineCount: panel.theme.stripLabelLines
                    }
                    Item {
                        id: well
                        y: stripName.y + panel.theme.stripLabelLines * stripName.font.pixelSize
                           + panel.theme.tileGap
                        width: parent.width
                        height: stripValue.y - panel.theme.hintGap - y
                        // The meter, beside the fader.
                        Rectangle {
                            id: meterTrack
                            x: parent.width / 2 - fader.width / 2 - panel.theme.meterWidth - panel.theme.hintGap
                            width: panel.theme.meterWidth
                            height: parent.height
                            radius: width / 2
                            color: panel.theme.track
                            Rectangle {
                                objectName: "stickMeter:" + strip.modelData.id
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: parent.height * strip.meterLevel
                                radius: width / 2
                                opacity: strip.muted ? panel.theme.disabledOpacity : 1
                                color: strip.meterLevel > panel.theme.meterHot ? panel.theme.amber
                                                                               : panel.theme.meterInk
                            }
                        }
                        Kit.Fader {
                            id: fader
                            x: (parent.width - width) / 2
                            height: parent.height
                            controller: panel.controller
                            theme: panel.theme
                            parameterId: strip.base + ".level"
                            label: strip.modelData.name
                        }
                    }
                    Text {
                        id: stripValue
                        objectName: "stickStripLevel:" + strip.modelData.id
                        y: switches.y - panel.theme.hintGap - height
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: panel.controller.revision >= 0 ? panel.controller.display(strip.base + ".level") : ""
                        color: panel.theme.ink
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.smallSize
                    }
                    Row {
                        id: switches
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: strip.height - panel.theme.tileGap - height
                        spacing: panel.theme.hintGap
                        Kit.Switch {
                            objectName: "stickMute:" + strip.modelData.id
                            height: panel.theme.stripSwitchSize
                            controller: panel.controller
                            theme: panel.theme
                            parameterId: strip.base + ".mute"
                            label: "M"
                        }
                        Kit.Switch {
                            objectName: "stickSolo:" + strip.modelData.id
                            height: panel.theme.stripSwitchSize
                            controller: panel.controller
                            theme: panel.theme
                            parameterId: strip.base + ".solo"
                            label: "S"
                            accent: panel.theme.meterInk
                        }
                    }
                }
            }
        }
        Kit.LibraryNotice {
            x: panel.theme.cardPad
            y: mixerTitle.y + mixerTitle.height + panel.theme.titleGap
            width: mixer.width - 2 * panel.theme.cardPad
            visible: !panel.ready
            wide: true
            controller: panel.controller
            theme: panel.theme
            library: panel.library
            instrument: "Stick"
            onReload: {
                panel.controller.stick_reload_library()
                ++panel.libraryLook
            }
        }
    }

    // ---- The chosen piece ------------------------------------------------------------------------

    Card {
        id: pieceCard
        objectName: "stickPieceCard"
        x: mixer.x
        y: mixer.y + mixer.height + panel.gap
        width: mixer.width
        height: panel.theme.pieceHeight

        Item {
            id: pieceTitle
            x: panel.theme.cardPad
            y: panel.theme.cardPad / 2
            width: pieceCard.width - 2 * panel.theme.cardPad
            height: panel.theme.titleHeight
            Heading {
                objectName: "stickPieceTitle"
                anchors.verticalCenter: parent.verticalCenter
                text: panel.piece.name.toUpperCase() + " · THIS PIECE"
            }
            Note {
                objectName: "stickPieceFacts"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    const p = panel.piece
                    const parts = [p.name]
                    if (p.hits > 0)
                        parts.push(p.hits + (p.hits === 1 ? " hit" : " hits"))
                    if (p.articulations !== undefined && p.articulations !== "")
                        parts.push(p.articulations)
                    if (p.keys !== undefined && p.keys !== "")
                        parts.push(p.keys)
                    return parts.join(" · ")
                }
            }
        }
        Row {
            id: knobRow
            readonly property real cell: width / panel.pieceKnobs
            x: panel.theme.cardPad
            y: pieceTitle.y + pieceTitle.height
            width: pieceCard.width - 2 * panel.theme.cardPad
            height: pieceCard.height - y
            Repeater {
                model: [
                    { control: "tune", label: "Tune" },
                    { control: "decay", label: "Decay" },
                    { control: "level", label: "Level" },
                    { control: "pan", label: "Pan" },
                    { control: "curve", label: "Vel curve" },
                    { control: "overheads", label: "Overheads" },
                    { control: "room", label: "Room" }
                ]
                Item {
                    id: cell
                    required property var modelData
                    width: knobRow.cell
                    height: knobRow.height
                    Kit.Knob {
                        anchors.centerIn: parent
                        readout: true
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.pieceParameter(cell.modelData.control)
                        label: cell.modelData.label
                        valueText: {
                            const id = panel.pieceParameter(cell.modelData.control)
                            if (panel.controller.revision < 0)
                                return ""
                            return cell.modelData.control === "curve"
                                ? panel.curveText(panel.controller.value(id)) : panel.controller.display(id)
                        }
                    }
                }
            }
        }
    }

    // ---- The kit row -------------------------------------------------------------------------------

    Card {
        id: kitRow
        objectName: "stickKitRow"
        x: panel.margin
        y: panel.kitRowY
        width: panel.width - 2 * panel.margin
        height: panel.theme.kitRowHeight
        Row {
            x: panel.theme.cardPad
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2 * panel.theme.tilePad
            Heading {
                anchors.verticalCenter: parent.verticalCenter
                text: "KIT"
            }
            Kit.Segment {
                objectName: "stickStrikers"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
                parameterId: "stick.striker"
                inactive: !panel.kitHasBrushes
                inactiveReason: "This kit has no brush recordings"
            }
            Kit.Switch {
                objectName: "stickRoundRobins"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
                parameterId: "stick.round_robin"
            }
            Kit.Slider {
                objectName: "stickHumanise"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
                parameterId: "stick.humanise"
            }
            Kit.Slider {
                objectName: "stickLevel"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
                parameterId: "stick.level"
            }
        }
        Note {
            objectName: "stickVoices"
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.cardPad
            anchors.verticalCenter: parent.verticalCenter
            text: {
                const voices = panel.live.live === true && panel.live.voices !== undefined ? panel.live.voices : 0
                const late = panel.live.live === true && panel.live.underruns !== undefined ? panel.live.underruns : 0
                return voices + (voices === 1 ? " voice" : " voices")
                    + (late > 0 ? " · " + late + " frames late from disk" : "")
            }
        }
    }

    // ---- The library, from the header's chip ------------------------------------------------------------

    Rectangle {
        objectName: "stickLibraryPopup"
        visible: panel.libraryOpen
        x: panel.width - panel.margin - width
        y: header.y + header.rowHeight
        width: 2 * panel.theme.mixWidth
        height: popupNotice.implicitHeight + 2 * panel.theme.cardPad
        radius: panel.theme.cardRadius
        color: panel.theme.card
        border.color: panel.theme.tileEdge
        z: 1
        Kit.LibraryNotice {
            id: popupNotice
            x: panel.theme.cardPad
            y: panel.theme.cardPad
            width: parent.width - 2 * panel.theme.cardPad
            controller: panel.controller
            theme: panel.theme
            library: panel.library
            instrument: "Stick"
            details: panel.ready ? panel.library.samples + " hits on " + panel.library.mics + " mics: "
                                   + Format.quantity(panel.library.resident_mb, "MB", 0) + " in memory, "
                                   + Format.quantity(panel.library.pack_mb, "MB", 0)
                                   + " streamed from disk. Licence: " + panel.library.licence_url
                                 : ""
            onReload: {
                panel.controller.stick_reload_library()
                ++panel.libraryLook
            }
        }
    }

    Timer {
        objectName: "stickTelemetryPoll"
        interval: panel.live.live === true ? panel.theme.pollLiveMs : panel.theme.pollIdleMs
        repeat: true
        running: panel.visible
        onTriggered: {
            panel.live = panel.controller.telemetry()
            // A kit read in the background is picked up when it is done.
            if (panel.library.state === "loading" || (panel.live.library !== undefined
                                                      && panel.live.library !== panel.library.state))
                ++panel.libraryLook
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
