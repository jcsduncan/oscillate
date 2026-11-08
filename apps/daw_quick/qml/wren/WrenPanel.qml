pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The Wren flute panel (ADR-0089): the header
// with Mono or Poly 4, the preset and the output; the Breath card (style
// tiles, the phrase lane of notes about to play, Amount, Dynamics, Vibrato,
// Bloom, Swell, Humanize and Ornaments); on the right the Articulation card
// with Slur time, the Tone card with Tune and Level, and the sample
// library's plate. Breath works in realtime only: nothing is written into
// clips.
//
// Values go through `controller` (NativeSynthDeviceController). The lane is
// the controller's wren_phrase over the track's first MIDI clip, decided as
// the kernel decides with the panel's values; it is read again whenever the
// values or the project change.
Rectangle {
    id: panel
    required property var controller
    property WrenTheme theme: WrenTheme {}

    // Live engine state (ADR-0037), polled while the panel is visible.
    property var live: ({ live: false })
    // The sample library (wren_library), read again when the user asks.
    property int libraryLook: 0
    readonly property var library: controller.revision >= 0 && libraryLook >= 0
        ? controller.wren_library() : ({})
    readonly property bool ready: library.state === "ready"
    property int projectLook: 0
    readonly property var phrase: controller.revision >= 0 && projectLook >= 0
        ? controller.wren_phrase() : ({})

    // The designed size, and the layout's spacing.
    readonly property real designedWidth: 1280
    readonly property real designedHeight: 900
    readonly property real margin: 28
    readonly property real headerY: theme.space2xl
    readonly property real headerHeight: 52
    readonly property real headerGap: theme.spaceXl
    readonly property real gap: 18
    readonly property real stack: theme.spaceXl
    readonly property real rightWidth: 380
    readonly property real rightPad: 18
    readonly property real presetWidth: 220
    readonly property real voicesWidth: 168
    readonly property real ornamentsWidth: 210
    // The library's message, at most this many lines.
    readonly property int messageLines: 4
    readonly property real sliderColumnGap: 22
    readonly property real sliderRowGap: 14
    readonly property int breathColumns: 3
    readonly property int articulationColumns: 2
    readonly property real bodyY: headerY + headerHeight + headerGap
    readonly property real bodyHeight: height - bodyY - headerY
    readonly property real pollLiveMs: 33
    readonly property real pollIdleMs: 250

    objectName: "wrenPanel"
    implicitWidth: designedWidth
    implicitHeight: designedHeight
    radius: theme.cardRadius - theme.focusBorder
    color: theme.caseColor
    border.color: theme.cardEdge
    Accessible.role: Accessible.Pane
    Accessible.name: "Wren flute"

    Connections {
        target: panel.controller
        // A clip edit changes the project, not the patch.
        function onStatus_changed() { ++panel.projectLook }
    }

    // "off" at the minimum, where the engine's own reading says less.
    function offText(value, info) {
        return value <= info.minimum ? "off" : ""
    }

    component Slide: Kit.Slider {
        controller: panel.controller
        theme: panel.theme
    }
    component CardTitle: Row {
        id: cardTitle
        property string title: ""
        property color dot: panel.theme.tongued
        spacing: panel.theme.titleGap
        height: panel.theme.titleHeight
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: panel.theme.titleDot
            height: width
            radius: width / 2
            color: cardTitle.dot
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: cardTitle.title
            color: panel.theme.ink
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.bodySize
            font.weight: Font.DemiBold
            font.letterSpacing: panel.theme.headingSpacing
        }
    }
    component Card: Rectangle {
        radius: panel.theme.cardRadius
        color: panel.theme.card
        border.color: panel.theme.cardEdge
    }

    // ---- Header ---------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.headerY
        width: panel.width - 2 * panel.margin
        height: panel.headerHeight

        Text {
            id: wordmark
            anchors.verticalCenter: parent.verticalCenter
            text: "WREN"
            color: panel.theme.ink
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Bold
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            objectName: "wrenSubtitle"
            x: wordmark.width + panel.theme.titleGap
            width: headerControls.x - x - panel.gap
            anchors.baseline: wordmark.baseline
            text: "Concert flute · Iowa MIS · B3 to D♭7 · pp, mf, ff · straight and vibrato"
            color: panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.subtitleSize
            elide: Text.ElideRight
        }

        Row {
            id: headerControls
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: panel.gap

            Kit.ChoiceTiles {
                anchors.verticalCenter: parent.verticalCenter
                width: panel.voicesWidth
                controller: panel.controller
                theme: panel.theme
                parameterId: "wren.voices"
                segment: true
                nameSize: panel.theme.bodySize
            }

            Rectangle {
                id: presetBox
                objectName: "wrenPresetDisplay"
                anchors.verticalCenter: parent.verticalCenter
                width: panel.presetWidth
                height: panel.theme.touch
                radius: panel.theme.tileRadius
                color: presetArea.containsMouse ? panel.theme.buttonHi : panel.theme.tile
                border.width: activeFocus ? panel.theme.focusBorder : panel.theme.idleBorder
                border.color: activeFocus ? panel.theme.focusRing : panel.theme.tileEdge
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(presetBox, panel.controller, panel.theme.tongued)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Row {
                    x: panel.theme.tilePad + panel.theme.focusBorder * 2
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: panel.theme.captionGap
                    Text {
                        id: presetCaption
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.controller.preset_modified ? "EDITED" : "PRESET"
                        color: panel.theme.inkDim
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.captionSize
                        font.letterSpacing: panel.theme.labelSpacing
                    }
                    Text {
                        objectName: "wrenPresetName"
                        anchors.verticalCenter: parent.verticalCenter
                        width: presetBox.width - presetCaption.width - presetChevron.width
                            - 3 * panel.theme.tilePad - 2 * panel.theme.captionGap
                        text: panel.controller.preset_name !== "" ? panel.controller.preset_name : "Custom"
                        color: panel.theme.ink
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.subtitleSize
                        elide: Text.ElideRight
                    }
                }
                Text {
                    id: presetChevron
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.tilePad
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.captionSize
                }
                MouseArea {
                    id: presetArea
                    anchors.fill: parent
                    enabled: panel.controller.editable
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        presetBox.forceActiveFocus()
                        presetBox.openPresets()
                    }
                }
            }

            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.captionGap
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "OUT"
                    color: panel.theme.inkDim
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.captionSize
                    font.letterSpacing: panel.theme.labelSpacing
                }
                Rectangle {
                    id: meter
                    objectName: "wrenOutputMeter"
                    readonly property real peakDb: panel.live.live === true && panel.live.output_peak_db !== undefined
                        ? panel.live.output_peak_db : panel.theme.meterFloorDb
                    readonly property real level: Math.max(0, Math.min(1,
                        (peakDb - panel.theme.meterFloorDb) / (panel.theme.meterCeilDb - panel.theme.meterFloorDb)))
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.meterWidth
                    height: panel.theme.meterHeight
                    radius: height / 2
                    color: panel.theme.track
                    clip: true
                    Accessible.role: Accessible.ProgressBar
                    Accessible.name: "Output level"
                    Rectangle {
                        width: parent.width * meter.level
                        height: parent.height
                        color: meter.peakDb > 0 ? panel.theme.warning : panel.theme.tongued
                    }
                }
            }
        }
    }

    // ---- Breath ---------------------------------------------------------------------

    Card {
        id: breath
        objectName: "wrenBreath"
        x: panel.margin
        y: panel.bodyY
        width: panel.width - 2 * panel.margin - panel.gap - panel.rightWidth
        height: panel.bodyHeight

        Item {
            id: breathTitle
            x: panel.theme.cardPad
            y: panel.theme.cardPad
            width: breath.width - 2 * panel.theme.cardPad
            height: panel.theme.titleHeight
            CardTitle {
                id: breathHeading
                anchors.verticalCenter: parent.verticalCenter
                title: "BREATH"
            }
            Text {
                x: breathHeading.width + panel.theme.titleGap
                width: parent.width - x
                anchors.verticalCenter: parent.verticalCenter
                text: "Draw notes flat. Breath plays them like a flautist: phrasing, vibrato that blooms, "
                    + "swells and grace notes."
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.bodySize
                // The note gives way to a narrow card; its tooltip shows it
                // whole.
                elide: Text.ElideRight
                readonly property bool tipShowsWhole: true
                Accessible.name: text
                Kit.HoverTip { theme: panel.theme }
            }
        }

        Kit.ChoiceTiles {
            id: styles
            x: panel.theme.cardPad
            y: breathTitle.y + breathTitle.height + panel.stack
            width: breath.width - 2 * panel.theme.cardPad
            controller: panel.controller
            theme: panel.theme
            parameterId: "wren.breath.style"
            columns: panel.breathColumns
            tileHeight: panel.theme.styleTileHeight
            spacing: panel.theme.tileRadius
            hints: ["Your velocities, no vibrato", "Sung lines, warm vibrato, swells",
                    "Light, late vibrato, folk cuts"]
        }

        Rectangle {
            id: laneWell
            x: panel.theme.cardPad
            y: styles.y + styles.height + panel.stack
            width: breath.width - 2 * panel.theme.cardPad
            height: breathControls.y - panel.stack - y
            radius: panel.theme.tileRadius + panel.theme.focusBorder
            color: panel.theme.lane
            border.color: panel.theme.laneEdge
            WrenPhrase {
                x: panel.theme.tilePad
                y: panel.theme.tilePad
                width: laneWell.width - 2 * panel.theme.tilePad
                height: laneWell.height - panel.theme.tilePad - panel.theme.captionGap
                theme: panel.theme
                phrase: panel.phrase
            }
        }

        Grid {
            id: breathControls
            x: panel.theme.cardPad
            y: ornamentsRow.y - panel.stack - height
            width: breath.width - 2 * panel.theme.cardPad
            columns: panel.breathColumns
            columnSpacing: panel.sliderColumnGap
            rowSpacing: panel.sliderRowGap
            readonly property real column: (width - (columns - 1) * columnSpacing) / columns
            Slide {
                width: breathControls.column
                parameterId: "wren.breath.amount"
                hint: "Your velocities ↔ Breath"
            }
            Slide {
                width: breathControls.column
                parameterId: "wren.breath.dynamics"
                hint: "How far phrases rise and fall"
            }
            Slide {
                width: breathControls.column
                parameterId: "wren.breath.vibrato"
                hint: "Depth on long notes"
                format: panel.offText
            }
            Slide {
                width: breathControls.column
                parameterId: "wren.breath.bloom"
                hint: "Straight tone before vibrato"
            }
            Slide {
                width: breathControls.column
                parameterId: "wren.breath.swell"
                hint: "Long notes lean in, then ease"
                format: panel.offText
            }
            Slide {
                width: breathControls.column
                parameterId: "wren.breath.humanize"
                hint: "Seeded; loops repeat exactly"
                format: panel.offText
            }
        }

        Row {
            id: ornamentsRow
            x: panel.theme.cardPad
            y: breath.height - panel.theme.cardPad - height
            width: breath.width - 2 * panel.theme.cardPad
            height: panel.theme.touch
            spacing: panel.theme.titleGap
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Ornaments"
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.bodySize
            }
            Kit.ChoiceTiles {
                id: ornaments
                anchors.verticalCenter: parent.verticalCenter
                width: panel.ornamentsWidth
                controller: panel.controller
                theme: panel.theme
                parameterId: "wren.breath.ornaments"
                segment: true
                nameSize: panel.theme.bodySize
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Graces from the project key's scale step above, on the beat."
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.bodySize
            }
        }
    }

    // ---- Right column ---------------------------------------------------------------

    Card {
        id: articulation
        objectName: "wrenArticulation"
        x: breath.x + breath.width + panel.gap
        y: panel.bodyY
        width: panel.rightWidth
        height: articulationColumn.implicitHeight + 2 * panel.rightPad

        Column {
            id: articulationColumn
            x: panel.theme.cardPad
            y: panel.rightPad
            width: articulation.width - 2 * panel.theme.cardPad
            spacing: panel.sliderRowGap
            CardTitle {
                title: "ARTICULATION"
                dot: panel.theme.slur
            }
            Kit.ChoiceTiles {
                width: parent.width
                controller: panel.controller
                theme: panel.theme
                parameterId: "wren.articulation"
                columns: panel.articulationColumns
                tileHeight: panel.theme.articulationTileHeight
                nameSize: panel.theme.bodySize
                accent: panel.theme.slur
                tint: panel.theme.tileOnLilac
                hints: ["Breath decides", "Slur near notes", "Every note tongued", "Short and light"]
            }
            Slide {
                width: parent.width
                parameterId: "wren.slur"
            }
        }
    }

    Card {
        id: tone
        objectName: "wrenTone"
        x: articulation.x
        y: articulation.y + articulation.height + panel.gap
        width: panel.rightWidth
        // Down to the library plate.
        height: libraryPlate.y - panel.gap - y

        Column {
            id: toneColumn
            x: panel.theme.cardPad
            y: panel.rightPad
            width: tone.width - 2 * panel.theme.cardPad
            // The sliders share what height the card has.
            readonly property int sliders: 5
            spacing: Math.max(0, (tone.height - 2 * panel.rightPad - panel.theme.titleHeight
                                  - sliders * panel.theme.touch) / sliders)
            CardTitle {
                title: "TONE"
                dot: panel.theme.toneDot
            }
            Slide { width: parent.width; parameterId: "wren.tone.air" }
            Slide { width: parent.width; parameterId: "wren.tone.warmth" }
            Slide { width: parent.width; parameterId: "wren.tone.space"; format: panel.offText }
            Slide { width: parent.width; parameterId: "wren.tune" }
            Slide { width: parent.width; parameterId: "wren.level" }
        }
    }

    // ---- The sample library -----------------------------------------------------------

    Rectangle {
        id: libraryPlate
        objectName: "wrenLibrary"
        x: articulation.x
        y: panel.bodyY + panel.bodyHeight - height
        width: panel.rightWidth
        height: libraryColumn.implicitHeight + 2 * panel.rightPad
        radius: panel.theme.cardRadius
        color: panel.theme.libraryCard
        border.color: panel.theme.cardEdge
        Accessible.role: Accessible.Pane
        Accessible.name: "Flute library"
        Accessible.description: panel.ready ? panel.library.name + ", " + panel.library.attribution
                                            : panel.library.message !== undefined ? panel.library.message : ""

        Column {
            id: libraryColumn
            x: panel.rightPad
            y: panel.rightPad
            width: libraryPlate.width - 2 * panel.rightPad
            spacing: panel.theme.captionGap

            Row {
                spacing: panel.theme.titleGap
                CardTitle {
                    title: "LIBRARY"
                    dot: panel.ready ? panel.theme.live : panel.theme.warning
                }
                Text {
                    objectName: "wrenLibraryState"
                    anchors.verticalCenter: parent.verticalCenter
                    text: panel.ready ? "INSTALLED" : panel.library.state === "invalid" ? "DAMAGED" : "NOT INSTALLED"
                    color: panel.ready ? panel.theme.inkDim : panel.theme.warning
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.smallSize
                    font.letterSpacing: panel.theme.labelSpacing
                }
            }

            // Installed: the library, its credit and how it plays.
            Text {
                objectName: "wrenLibraryName"
                width: parent.width
                visible: panel.ready
                text: panel.ready ? panel.library.name : ""
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.bodySize
                elide: Text.ElideRight
            }
            Text {
                objectName: "wrenAttribution"
                width: parent.width
                visible: panel.ready
                text: panel.ready ? panel.library.attribution : ""
                color: panel.theme.tongued
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.captionSize
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                visible: panel.ready
                text: panel.ready
                    ? "Free to use without restrictions. " + panel.library.samples + " notes, "
                      + Format.quantity(panel.library.resident_mb, "MB", 0) + ", all in memory."
                    : ""
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.captionSize
                wrapMode: Text.WordWrap
            }

            // Missing or damaged: why, and how to install it.
            Text {
                objectName: "wrenLibraryMessage"
                width: parent.width
                visible: !panel.ready
                text: (panel.library.message !== undefined ? panel.library.message + ". " : "")
                    + "Wren is silent until its library is installed. It plays "
                    + (panel.library.required !== undefined ? panel.library.required : "") + "."
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.captionSize
                wrapMode: Text.WordWrap
                maximumLineCount: panel.messageLines
                elide: Text.ElideRight
            }
            Rectangle {
                width: parent.width
                height: command.implicitHeight + 2 * panel.theme.captionGap
                visible: !panel.ready
                radius: panel.theme.captionGap
                color: panel.theme.lane
                border.color: panel.theme.laneEdge
                Text {
                    id: command
                    objectName: "wrenInstallCommand"
                    x: panel.theme.captionGap
                    y: panel.theme.captionGap
                    width: parent.width - 2 * x
                    text: panel.library.install_command !== undefined ? panel.library.install_command : ""
                    color: panel.theme.tongued
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.smallSize
                    wrapMode: Text.WrapAnywhere
                    Accessible.role: Accessible.StaticText
                    Accessible.name: "Install command"
                    Accessible.description: text
                }
            }
            Row {
                visible: !panel.ready
                spacing: panel.theme.captionGap
                Kit.Button {
                    theme: panel.theme
                    objectName: "wrenCopyInstall"
                    label: "Copy command"
                    onClicked: panel.controller.set_clipboard_text(command.text)
                }
                Kit.Button {
                    theme: panel.theme
                    objectName: "wrenLibraryReload"
                    label: "Look again"
                    onClicked: {
                        panel.controller.wren_reload_library()
                        ++panel.libraryLook
                    }
                }
            }
        }
    }

    Timer {
        objectName: "wrenTelemetryPoll"
        interval: panel.live.live === true ? panel.pollLiveMs : panel.pollIdleMs
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
