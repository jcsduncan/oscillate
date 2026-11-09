pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The Bow strings panel (ADR-0111), as the approved mockup lays it out: the
// shared header with the library chip and presets; the section picker and
// Solo or Section; the articulation list with each keyswitch note; the bow
// display (the wheel's dynamics, the newest note's vibrato and where the bow
// is now) with Tone from sul tasto to sul ponticello; six knobs; and the
// keyboard strip with the keyswitches, the section's playable range, the
// held keys and the voice count.
//
// Values go through `controller` (NativeSynthDeviceController); live state
// comes from its telemetry, polled while the panel is visible. Without the
// library the bow display gives way to the shared library notice.
Rectangle {
    id: panel
    required property var controller
    property BowTheme theme: BowTheme {}

    // Live engine state (ADR-0037), polled while the panel is visible.
    property var live: ({ live: false })
    // The library (bow_library), read again when the user asks or while it loads.
    property int libraryLook: 0
    readonly property var library: controller.revision >= 0 && libraryLook >= 0
        ? controller.bow_library() : ({})
    readonly property bool ready: library.state === "ready"
    readonly property var layout: controller.revision >= 0 ? controller.bow_layout() : ({})
    // The wheel's and the vibrato's last few seconds, oldest first.
    property var history: []
    property bool libraryOpen: false

    // The designed size and the layout's spacing, from the theme.
    readonly property real designedWidth: theme.panelWidth
    readonly property real designedHeight: theme.panelHeight
    readonly property real margin: theme.panelMargin
    readonly property real gap: theme.panelGap
    readonly property real pickerHeight: theme.pickerHeight
    readonly property real modeWidth: theme.modeWidth
    readonly property real articulationWidth: theme.articulationWidth
    readonly property real knobsHeight: theme.knobsHeight
    readonly property int knobCount: 6
    readonly property real stripHeight: theme.stripHeight
    readonly property real keyGap: theme.keyGap
    readonly property real keyRadius: theme.keyRadius
    // The bow display's span: its history fills while notes play.
    readonly property real historySeconds: theme.historySeconds
    readonly property real pollLiveMs: theme.pollLiveMs
    readonly property real msPerSecond: 1000
    readonly property int historyLength: Math.round(historySeconds * msPerSecond / pollLiveMs)
    readonly property real pollIdleMs: theme.pollIdleMs
    readonly property real pickerY: header.y + header.height + gap
    readonly property real bodyY: pickerY + pickerHeight + gap
    readonly property real stripY: height - margin - stripHeight
    readonly property real bodyHeight: stripY - gap - bodyY
    // The articulation playing: the keyswitch's when one latched it.
    readonly property int playing: live.live === true && live.articulation !== undefined
        ? live.articulation : controller.revision >= 0 ? controller.choice("bow.articulation") : 0
    readonly property var articulationNames: controller.parameter("bow.articulation").choices
    readonly property var sectionNames: controller.parameter("bow.section").choices

    objectName: "bowPanel"
    implicitWidth: designedWidth
    implicitHeight: designedHeight
    radius: theme.cardRadius
    color: theme.caseColor
    border.color: theme.cardEdge
    Accessible.role: Accessible.Pane
    Accessible.name: "Bow strings"

    // The mod wheel's position named as a dynamic.
    function dynamicName(wheel) {
        const names = layout.dynamics !== undefined ? layout.dynamics : []
        for (let i = 0; i < names.length; ++i) {
            if (wheel <= names[i].top)
                return names[i].name
        }
        return names.length > 0 ? names[names.length - 1].name : ""
    }
    function keyswitchName(index) {
        const switches = layout.keyswitches !== undefined ? layout.keyswitches : []
        return index < switches.length ? switches[index].name : ""
    }
    function libraryChipText() {
        if (ready)
            return "Library ready · " + Format.quantity(library.resident_mb, "MB", 0)
        if (library.state === "loading")
            return "Loading library"
        return library.state === "invalid" ? "Library damaged" : "Library missing"
    }
    function record() {
        if (live.live !== true)
            return
        const next = history.concat([{ dynamics: live.dynamics, vibrato: live.vibrato }])
        history = next.length > historyLength ? next.slice(next.length - historyLength) : next
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

    // ---- Header ---------------------------------------------------------------------

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.margin
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        wordmark: "BOW"
        subtitle: "SAMPLED STRINGS · SECTIONS AND SOLO"
        tools: Component {
            // The library's state and size; it opens the library's notice.
            Rectangle {
                id: chip
                objectName: "bowLibraryChip"
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
                        objectName: "bowLibraryChipText"
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

    // ---- Section and players ------------------------------------------------------------

    Kit.ChoiceTiles {
        objectName: "bowSections"
        x: panel.margin
        y: panel.pickerY
        width: panel.width - 2 * panel.margin - panel.gap - panel.modeWidth
        controller: panel.controller
        theme: panel.theme
        parameterId: "bow.section"
        segment: true
        tileHeight: panel.pickerHeight
        nameSize: panel.theme.bodySize
    }
    Kit.ChoiceTiles {
        objectName: "bowMode"
        x: panel.width - panel.margin - panel.modeWidth
        y: panel.pickerY
        width: panel.modeWidth
        controller: panel.controller
        theme: panel.theme
        parameterId: "bow.mode"
        segment: true
        tileHeight: panel.pickerHeight
        nameSize: panel.theme.bodySize
    }

    // ---- Articulations ---------------------------------------------------------------------

    Card {
        id: articulations
        objectName: "bowArticulations"
        x: panel.margin
        y: panel.bodyY
        width: panel.articulationWidth
        height: panel.bodyHeight

        Column {
            x: panel.theme.cardPad
            y: panel.theme.cardPad
            width: articulations.width - 2 * panel.theme.cardPad
            spacing: panel.theme.tileGap
            Heading { text: "ARTICULATION" }
            Repeater {
                model: panel.articulationNames
                Rectangle {
                    id: row
                    required property int index
                    required property string modelData
                    readonly property bool chosen: panel.playing === index
                    objectName: "bowArticulation" + index
                    width: parent.width
                    height: panel.theme.articulationTileHeight
                    radius: panel.theme.tileRadius
                    color: chosen ? panel.theme.accent : rowArea.containsMouse ? panel.theme.buttonHi
                                                                                 : panel.theme.tile
                    border.color: row.activeFocus ? panel.theme.focusRing
                                                  : chosen ? panel.theme.accent : panel.theme.tileEdge
                    border.width: row.activeFocus ? panel.theme.focusBorder : panel.theme.idleBorder
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.checkable: true
                    Accessible.checked: chosen
                    Accessible.name: modelData + ", keyswitch " + panel.keyswitchName(index)
                    function pick() {
                        if (panel.controller.editable)
                            panel.controller.set_choice("bow.articulation", index)
                    }
                    Keys.onReturnPressed: pick()
                    Keys.onSpacePressed: pick()
                    Text {
                        x: panel.theme.tilePad
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData
                        color: row.chosen ? panel.theme.accentInk : panel.theme.ink
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.bodySize
                        font.weight: row.chosen ? Font.Bold : Font.Medium
                    }
                    Text {
                        objectName: "bowKeyswitch" + row.index
                        anchors.right: parent.right
                        anchors.rightMargin: panel.theme.tilePad
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.keyswitchName(row.index)
                        color: row.chosen ? panel.theme.accentInk : panel.theme.inkDim
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.smallSize
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: row.pick()
                    }
                }
            }
            Text {
                objectName: "bowLatched"
                width: parent.width
                text: panel.live.live === true && panel.live.latched === true
                    ? "Latched by keyswitch " + panel.keyswitchName(panel.playing)
                    : "Keyswitch notes below the range"
                color: panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.smallSize
                wrapMode: Text.WordWrap
            }
        }
    }

    // ---- The bow ----------------------------------------------------------------------------

    Card {
        id: bow
        objectName: "bowCard"
        x: articulations.x + articulations.width + panel.gap
        y: panel.bodyY
        width: panel.width - panel.margin - x
        height: panel.bodyHeight - panel.gap - panel.knobsHeight

        Item {
            id: bowTitle
            x: panel.theme.cardPad
            y: panel.theme.cardPad
            width: bow.width - 2 * panel.theme.cardPad
            height: panel.theme.titleHeight
            Heading {
                anchors.verticalCenter: parent.verticalCenter
                text: "BOW · DYNAMICS AND VIBRATO"
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "mod wheel → dynamics"
                color: panel.theme.inkDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.smallSize
            }
        }
        BowDisplay {
            x: panel.theme.cardPad
            y: bowTitle.y + bowTitle.height + panel.theme.captionGap
            width: bow.width - 2 * panel.theme.cardPad
            height: tone.y - panel.theme.captionGap - y
            visible: panel.ready
            theme: panel.theme
            live: panel.live.live === true
            history: panel.history
            seconds: panel.historySeconds
            wheel: panel.controller.revision >= 0 ? panel.controller.value("bow.perf.wheel") : 0
        }
        Kit.LibraryNotice {
            x: panel.theme.cardPad
            y: bowTitle.y + bowTitle.height + panel.theme.captionGap
            width: bow.width - 2 * panel.theme.cardPad
            visible: !panel.ready
            controller: panel.controller
            theme: panel.theme
            library: panel.library
            instrument: "Bow"
            onReload: {
                panel.controller.bow_reload_library()
                ++panel.libraryLook
            }
        }
        Column {
            id: tone
            x: panel.theme.cardPad
            y: bow.height - panel.theme.cardPad - height
            width: bow.width - 2 * panel.theme.cardPad
            spacing: panel.theme.hintGap
            Item {
                width: parent.width
                height: panel.theme.titleHeight
                Repeater {
                    model: [{ text: "Sul tasto", at: 0 }, { text: "Ordinario", at: 0.5 },
                            { text: "Sul ponticello", at: 1 }]
                    Text {
                        required property var modelData
                        x: Math.max(0, Math.min(parent.width - width, modelData.at * parent.width - modelData.at * width))
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.text
                        color: panel.theme.inkDim
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.smallSize
                    }
                }
            }
            Kit.Slider {
                objectName: "bowTone"
                width: parent.width
                controller: panel.controller
                theme: panel.theme
                parameterId: "bow.tone"
            }
        }
    }

    // ---- Knobs ----------------------------------------------------------------------------------

    Card {
        id: knobs
        objectName: "bowKnobs"
        x: bow.x
        y: bow.y + bow.height + panel.gap
        width: bow.width
        height: panel.knobsHeight
        Row {
            id: knobRow
            readonly property real cell: width / panel.knobCount
            x: panel.theme.cardPad
            width: knobs.width - 2 * panel.theme.cardPad
            height: knobs.height
            Item {
                width: knobRow.cell
                height: knobRow.height
                Kit.Knob {
                    anchors.centerIn: parent
                    readout: true
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "bow.perf.wheel"
                    valueText: panel.controller.revision >= 0
                        ? panel.dynamicName(panel.controller.value("bow.perf.wheel")) : ""
                }
            }
            Item {
                width: knobRow.cell
                height: knobRow.height
                Kit.Knob {
                    anchors.centerIn: parent
                    readout: true
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "bow.vibrato"
                }
            }
            Item {
                width: knobRow.cell
                height: knobRow.height
                Kit.Knob {
                    anchors.centerIn: parent
                    readout: true
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "bow.players"
                    valueText: {
                        const players = panel.controller.revision >= 0
                            ? Math.round(panel.controller.value("bow.players")) : 0
                        return players + (players === 1 ? " player" : " players")
                    }
                }
            }
            Item {
                width: knobRow.cell
                height: knobRow.height
                Kit.Knob {
                    anchors.centerIn: parent
                    readout: true
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "bow.humanise"
                }
            }
            Item {
                width: knobRow.cell
                height: knobRow.height
                Kit.Knob {
                    anchors.centerIn: parent
                    readout: true
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "bow.legato"
                    label: "Legato"
                }
            }
            Item {
                width: knobRow.cell
                height: knobRow.height
                Kit.Knob {
                    anchors.centerIn: parent
                    readout: true
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "bow.release"
                }
            }
        }
    }

    // ---- Keyboard ---------------------------------------------------------------------------------

    Card {
        id: strip
        objectName: "bowKeyboard"
        x: panel.margin
        y: panel.stripY
        width: panel.width - 2 * panel.margin
        height: panel.stripHeight
        readonly property int first: panel.layout.keyswitches !== undefined && panel.layout.keyswitches.length > 0
            ? panel.layout.keyswitches[0].key : 0
        readonly property int last: panel.layout.last_key !== undefined ? panel.layout.last_key : 0
        readonly property int count: Math.max(1, last - first + 1)
        readonly property var held: panel.live.live === true && panel.live.held !== undefined ? panel.live.held : []

        Item {
            id: stripTitle
            x: panel.theme.cardPad
            y: panel.theme.cardPad / 2
            width: strip.width - 2 * panel.theme.cardPad
            height: panel.theme.titleHeight
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Keyswitches"
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.smallSize
            }
            Text {
                objectName: "bowRange"
                anchors.centerIn: parent
                text: (panel.controller.revision >= 0
                       ? panel.sectionNames[panel.controller.choice("bow.section")] : "")
                    + " · " + panel.layout.low_name + "–" + panel.layout.high_name + " playable"
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.smallSize
            }
            Text {
                objectName: "bowVoices"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    const notes = strip.held.length
                    const voices = panel.live.live === true && panel.live.voices !== undefined ? panel.live.voices : 0
                    return notes + (notes === 1 ? " note · " : " notes · ") + voices
                        + (voices === 1 ? " voice" : " voices")
                }
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.smallSize
            }
        }
        Row {
            x: panel.theme.cardPad
            y: stripTitle.y + stripTitle.height + panel.theme.captionGap
            width: strip.width - 2 * panel.theme.cardPad
            height: strip.height - y - panel.theme.cardPad / 2
            spacing: panel.keyGap
            Repeater {
                model: strip.count
                Rectangle {
                    required property int index
                    readonly property int key: strip.first + index
                    readonly property bool keyswitch: index < (panel.layout.keyswitches !== undefined
                                                               ? panel.layout.keyswitches.length : 0)
                    readonly property bool inRange: key >= panel.layout.low && key <= panel.layout.high
                    readonly property bool down: strip.held.indexOf(key) >= 0
                    objectName: "bowKey" + key
                    width: (parent.width - (strip.count - 1) * panel.keyGap) / strip.count
                    height: parent.height
                    radius: panel.keyRadius
                    color: down ? panel.theme.accent : keyswitch ? panel.theme.vibratoInk
                         : inRange ? panel.theme.keyInRange : panel.theme.keyOutOfRange
                    opacity: inRange || keyswitch || down ? 1 : panel.theme.keyOutOfRangeOpacity
                }
            }
        }
    }

    // ---- The library, from the header's chip ----------------------------------------------------

    Rectangle {
        objectName: "bowLibraryPopup"
        visible: panel.libraryOpen
        x: panel.width - panel.margin - width
        y: header.y + header.rowHeight
        width: panel.modeWidth * 2
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
            instrument: "Bow"
            details: panel.ready ? panel.library.samples + " samples, "
                                   + Format.quantity(panel.library.resident_mb, "MB", 0) + ", all in memory."
                                 : ""
            onReload: {
                panel.controller.bow_reload_library()
                ++panel.libraryLook
            }
        }
    }

    Timer {
        objectName: "bowTelemetryPoll"
        interval: panel.live.live === true ? panel.pollLiveMs : panel.pollIdleMs
        repeat: true
        running: panel.visible
        onTriggered: {
            panel.live = panel.controller.telemetry()
            panel.record()
            // A library read in the background is picked up when it is done.
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
