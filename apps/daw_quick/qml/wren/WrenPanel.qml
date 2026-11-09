pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The Wren flute panel (ADR-0089): the header
// with the preset, the output and, under them, Mono or Poly 4; the Breath card (style
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
    readonly property real designedHeight: 952
    readonly property real margin: 28
    readonly property real headerY: theme.space2xl
    readonly property real headerGap: theme.spaceXl
    readonly property real gap: 18
    readonly property real stack: theme.spaceXl
    readonly property real rightWidth: 380
    readonly property real rightPad: 18
    readonly property real voicesWidth: 168
    readonly property real ornamentsWidth: 210
    // The library's message, at most this many lines.
    readonly property int messageLines: 4
    readonly property real sliderColumnGap: 22
    readonly property real sliderRowGap: 14
    readonly property int breathColumns: 3
    readonly property int articulationColumns: 2
    readonly property real bodyY: header.y + header.height + headerGap
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

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.headerY
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        wordmark: "WREN"
        subtitle: "Concert flute · Iowa MIS · B3 to D♭7 · pp, mf, ff · straight and vibrato"

        // Mono or Poly 4: a mode, in the header's second row.
        Kit.ChoiceTiles {
            width: panel.voicesWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "wren.voices"
            segment: true
            nameSize: panel.theme.bodySize
        }
    }

    // Beside the status badge: the output.
    Row {
        x: header.x + header.status.x - panel.gap - width
        y: header.y
        height: header.rowHeight
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
        height: notice.implicitHeight + 2 * panel.rightPad
        radius: panel.theme.cardRadius
        color: panel.theme.libraryCard
        border.color: panel.theme.cardEdge
        Accessible.role: Accessible.Pane
        Accessible.name: "Flute library"
        Accessible.description: panel.ready ? panel.library.name + ", " + panel.library.attribution
                                            : panel.library.message !== undefined ? panel.library.message : ""

        Kit.LibraryNotice {
            id: notice
            x: panel.rightPad
            y: panel.rightPad
            width: libraryPlate.width - 2 * panel.rightPad
            controller: panel.controller
            theme: panel.theme
            library: panel.library
            instrument: "Wren"
            messageLines: panel.messageLines
            details: panel.ready
                ? "Free to use without restrictions. " + panel.library.samples + " notes, "
                  + Format.quantity(panel.library.resident_mb, "MB", 0) + ", all in memory."
                : ""
            onReload: {
                panel.controller.wren_reload_library()
                ++panel.libraryLook
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
