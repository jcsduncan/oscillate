pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Fret bass panel (ADR-0088): the header
// with the preset, the latest note as Feel played it and the output; the
// Feel card (style tiles, the groove lane of notes about to play, Amount,
// Dynamics, Pocket, Humanize and Ghost notes) over the sample library's
// plate; on the right the Articulation card and the Tone card with Tune and
// Level. Feel works in realtime only: nothing is written into clips.
//
// Values go through `controller` (NativeSynthDeviceController). The lane is
// the controller's fret_groove over the track's first MIDI clip, decided
// as the kernel decides with the panel's values; it is read again whenever
// the values or the project change.
Rectangle {
    id: panel
    required property var controller
    property FretTheme theme: FretTheme {}

    // Live engine state (ADR-0037), polled while the panel is visible.
    property var live: ({ live: false })
    // The sample library (fret_library), read again when the user asks.
    property int libraryLook: 0
    readonly property var library: controller.revision >= 0 && libraryLook >= 0
        ? controller.fret_library() : ({})
    readonly property bool ready: library.state === "ready"
    // The groove lane, as drawn while comparing.
    property bool compare: false
    property int projectLook: 0
    readonly property var groove: controller.revision >= 0 && projectLook >= 0
        ? controller.fret_groove(compare) : ({})

    readonly property real margin: 28
    readonly property real gap: 18
    readonly property real cardPad: 20
    readonly property real rightWidth: 360
    readonly property real bodyY: 92
    readonly property real feelHeight: 504
    readonly property real libraryHeight: 182
    readonly property real articulationHeight: 372
    readonly property real designedWidth: 1280
    readonly property real designedHeight: 820
    readonly property real headerY: 20
    readonly property real headerHeight: 52
    readonly property real headerSpacing: 18
    readonly property real lastNoteWidth: 140
    readonly property real feelTitleHeight: 36
    readonly property real laneHeight: 236
    readonly property real sliderColumnGap: 18
    readonly property int feelColumns: 5
    // The library plate's text starts this far down, under its title.
    readonly property real libraryBodyY: 50
    readonly property real rightPad: 18
    // The library's message, at most this many lines.
    readonly property int messageLines: 5
    readonly property real pollLiveMs: 33
    readonly property real pollIdleMs: 250

    objectName: "fretPanel"
    implicitWidth: designedWidth
    implicitHeight: designedHeight
    radius: theme.caseRadius
    color: theme.caseColor
    border.color: theme.cardEdge
    Accessible.role: Accessible.Pane
    Accessible.name: "Fret bass guitar"

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
        property color dot: panel.theme.pluck
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
            font.pixelSize: panel.theme.typeEmphasis
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

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: panel.theme.titleGap
            Text {
                id: wordmark
                text: "FRET"
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.wordmarkSize
                font.weight: Font.Bold
                font.letterSpacing: panel.theme.wordmarkSpacing
            }
            Text {
                objectName: "fretSubtitle"
                anchors.baseline: wordmark.baseline
                text: "Black and Blue · fingered 5-string · B0 to E4"
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeSubtitle
            }
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: panel.headerSpacing

            Row {
                spacing: panel.theme.spaceXs
                Kit.PresetArrow {
                    objectName: "fretPresetPrevious"
                    controller: panel.controller
                    theme: panel.theme
                    direction: -1
                }
                Rectangle {
                    id: presetBox
                    objectName: "fretPresetDisplay"
                    width: panel.theme.presetWidth
                    height: panel.theme.presetHeight
                    radius: panel.theme.tileRadius
                    color: panel.theme.tile
                    border.color: activeFocus ? panel.theme.pluck : panel.theme.tileEdge
                    activeFocusOnTab: true
                    Accessible.role: Accessible.ComboBox
                    Accessible.name: "Preset"
                    Accessible.description: panel.controller.preset_name
                        + (panel.controller.preset_modified ? ", edited" : "")
                    function openPresets() {
                        if (panel.controller.editable)
                            menu.openPresets(presetBox, panel.controller, panel.theme.pluck)
                    }
                    Keys.onReturnPressed: openPresets()
                    Keys.onSpacePressed: openPresets()
                    Row {
                        x: panel.theme.presetInset
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: panel.theme.captionGap
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: panel.controller.preset_modified ? "EDITED" : "PRESET"
                            color: panel.theme.inkDim
                            font.family: panel.theme.sans
                            font.pixelSize: panel.theme.typeLabel
                            font.letterSpacing: panel.theme.labelSpacing
                        }
                        Text {
                            objectName: "fretPresetName"
                            anchors.verticalCenter: parent.verticalCenter
                            width: presetBox.width - panel.theme.presetNameRoom
                            text: panel.controller.preset_name !== "" ? panel.controller.preset_name : "Custom"
                            color: panel.theme.ink
                            font.family: panel.theme.sans
                            font.pixelSize: panel.theme.typeSubtitle
                            elide: Text.ElideRight
                        }
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: panel.theme.presetChevronInset
                        anchors.verticalCenter: parent.verticalCenter
                        text: "▾"
                        color: panel.theme.inkDim
                        font.pixelSize: panel.theme.typeLabel
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: panel.controller.editable
                        onClicked: {
                            presetBox.forceActiveFocus()
                            presetBox.openPresets()
                        }
                    }
                }
                Kit.PresetArrow {
                    objectName: "fretPresetNext"
                    controller: panel.controller
                    theme: panel.theme
                    direction: 1
                }
            }

            // The latest note as Feel played it.
            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: panel.lastNoteWidth
                spacing: panel.theme.spaceXxs
                Text {
                    text: "LAST NOTE"
                    color: panel.theme.inkDim
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeSmall
                    font.letterSpacing: panel.theme.labelSpacing
                }
                Row {
                    spacing: panel.theme.spaceMd
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.swatch
                        height: panel.theme.swatch
                        radius: panel.theme.radiusXs
                        visible: lastNote.known
                        color: panel.theme.articulationColor(panel.live.last_articulation)
                        border.width: panel.live.last_pushed === true ? panel.theme.strokeBold : 0
                        border.color: panel.theme.pushedRing
                    }
                    Text {
                        id: lastNote
                        objectName: "fretLastNote"
                        readonly property bool known: panel.live.live === true && panel.live.last_key >= 0
                        text: known ? panel.live.last_name + " · " + panel.live.last_velocity + " · "
                                      + panel.live.last_articulation + (panel.live.last_pushed ? " · pushed" : "")
                                    : "—"
                        color: panel.theme.ink
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeLabel
                    }
                }
            }

            Kit.StatusBadge {
                objectName: "fretStatus"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
            }

            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.captionGap
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "OUT"
                    color: panel.theme.inkDim
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeLabel
                    font.letterSpacing: panel.theme.labelSpacing
                }
                Rectangle {
                    id: meter
                    objectName: "fretOutputMeter"
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
                        color: meter.peakDb > 0 ? panel.theme.warning : panel.theme.pluck
                    }
                }
            }
        }
    }

    // ---- Feel -------------------------------------------------------------------------

    Card {
        id: feel
        objectName: "fretFeel"
        x: panel.margin
        y: panel.bodyY
        width: panel.width - 2 * panel.margin - panel.gap - panel.rightWidth
        height: panel.feelHeight

        Item {
            id: feelTitle
            x: panel.cardPad
            y: panel.cardPad
            width: feel.width - 2 * panel.cardPad
            height: panel.feelTitleHeight
            CardTitle {
                id: feelHeading
                anchors.verticalCenter: parent.verticalCenter
                title: "FEEL"
            }
            Text {
                x: feelHeading.width + panel.theme.titleGap
                width: compareButton.x - x - panel.theme.titleGap
                anchors.verticalCenter: parent.verticalCenter
                text: "Draw notes flat. Feel plays them like a bassist: accents, ghosts, staccato, pops and pushes."
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeEmphasis
                elide: Text.ElideRight
            }
            Kit.Button {
                id: compareButton
                theme: panel.theme
                objectName: "fretCompare"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                label: panel.compare ? "Showing: as drawn" : "Compare with as drawn"
                on: panel.compare
                onClicked: panel.compare = !panel.compare
            }
        }

        Kit.ChoiceTiles {
            id: styles
            x: panel.cardPad
            y: feelTitle.y + feelTitle.height + panel.theme.spaceXl
            width: feel.width - 2 * panel.cardPad
            controller: panel.controller
            theme: panel.theme
            parameterId: "fret.feel.style"
            tileHeight: panel.theme.styleTileHeight
            spacing: panel.theme.styleTileGap
            hints: ["Your velocities", "Leans on the one", "Every eighth up", "Flatter, behind"]
        }

        Rectangle {
            id: laneWell
            x: panel.cardPad
            y: styles.y + styles.height + panel.theme.spaceXl
            width: feel.width - 2 * panel.cardPad
            height: panel.laneHeight
            radius: panel.theme.laneRadius
            color: panel.theme.lane
            border.color: panel.theme.laneEdge
            FretGroove {
                x: panel.theme.laneInset
                y: panel.theme.laneInset
                width: laneWell.width - 2 * panel.theme.laneInset
                height: laneWell.height - panel.theme.laneInset - panel.theme.laneFootInset
                theme: panel.theme
                groove: panel.groove
            }
        }

        Row {
            id: feelControls
            x: panel.cardPad
            y: laneWell.y + laneWell.height + panel.theme.spaceXl
            width: feel.width - 2 * panel.cardPad
            spacing: panel.sliderColumnGap
            readonly property real column: (width - (panel.feelColumns - 1) * spacing) / panel.feelColumns
            Slide {
                width: feelControls.column
                parameterId: "fret.feel.amount"
                hint: "0 plays your velocities as drawn"
            }
            Slide {
                width: feelControls.column
                parameterId: "fret.feel.dynamics"
                hint: "How far accents reach"
            }
            Slide {
                width: feelControls.column
                parameterId: "fret.feel.pocket"
                hint: "Off-beats sit behind; the one stays put"
                format: panel.offText
            }
            Slide {
                width: feelControls.column
                parameterId: "fret.feel.humanize"
                hint: "Seeded, so a loop repeats"
                format: panel.offText
            }
            Column {
                width: feelControls.column
                spacing: panel.theme.spaceXs
                Text {
                    height: panel.theme.sliderCaptionHeight
                    verticalAlignment: Text.AlignVCenter
                    text: "Ghost notes"
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeEmphasis
                }
                Kit.ChoiceTiles {
                    width: parent.width
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "fret.feel.ghosts"
                    columns: 2
                    tileHeight: panel.theme.compactTileHeight
                    nameSize: panel.theme.typeEmphasis
                    // Auto first.
                    order: [1, 0]
                }
            }
        }
    }

    // ---- The sample library --------------------------------------------------------

    Card {
        id: libraryPlate
        objectName: "fretLibrary"
        x: panel.margin
        y: feel.y + feel.height + panel.gap
        width: feel.width
        height: panel.libraryHeight
        Accessible.role: Accessible.Pane
        Accessible.name: "Bass library"
        Accessible.description: panel.ready ? panel.library.name + ", " + panel.library.attribution
                                            : panel.library.message !== undefined ? panel.library.message : ""

        Row {
            x: panel.cardPad
            y: panel.theme.spaceXl
            spacing: panel.theme.titleGap
            CardTitle {
                title: "LIBRARY"
                dot: panel.ready ? panel.theme.live : panel.theme.warning
            }
            Text {
                objectName: "fretLibraryState"
                anchors.verticalCenter: parent.verticalCenter
                text: panel.ready ? "INSTALLED" : panel.library.state === "invalid" ? "DAMAGED" : "NOT INSTALLED"
                color: panel.ready ? panel.theme.inkDim : panel.theme.warning
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
                font.letterSpacing: panel.theme.labelSpacing
            }
        }

        // Installed: the library, its credit and how it plays.
        Column {
            x: panel.cardPad
            y: panel.libraryBodyY
            width: libraryPlate.width - 2 * panel.cardPad
            spacing: panel.theme.spaceSm
            visible: panel.ready
            Text {
                objectName: "fretLibraryName"
                width: parent.width
                text: panel.ready ? panel.library.name : ""
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeHeading
                elide: Text.ElideRight
            }
            Text {
                objectName: "fretAttribution"
                width: parent.width
                visible: panel.ready
                text: panel.ready ? panel.library.attribution : ""
                color: panel.theme.pluck
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeLabel
            }
            Text {
                width: parent.width
                text: panel.ready
                    ? panel.library.samples + " samples: four pluck layers, ghosts, staccato, behind the bridge "
                      + "and string releases. " + panel.library.resident_mb.toFixed(0)
                      + " MB stay in memory; the rest streams from disk."
                    : ""
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeLabel
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: panel.ready ? panel.library.directory : ""
                color: panel.theme.inkFaint
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeSmall
                elide: Text.ElideMiddle
            }
            Text {
                objectName: "fretUnderruns"
                width: parent.width
                visible: panel.live.live === true && panel.live.underruns > 0
                text: panel.live.underruns > 0
                    ? "The disk was late " + panel.live.underruns + " times: those notes dropped out." : ""
                color: panel.theme.warning
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeLabel
            }
        }

        // Missing or damaged: why, and how to install it.
        Item {
            x: panel.cardPad
            y: panel.libraryBodyY
            width: libraryPlate.width - 2 * panel.cardPad
            height: libraryPlate.height - y - panel.theme.spaceXl
            visible: !panel.ready
            Text {
                id: missingText
                objectName: "fretLibraryMessage"
                width: parent.width * panel.theme.libraryMessageShare
                text: (panel.library.message !== undefined ? panel.library.message + ". " : "")
                    + "Fret is silent until its library is installed. It plays "
                    + (panel.library.required !== undefined ? panel.library.required : "") + "."
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeLabel
                wrapMode: Text.WordWrap
                maximumLineCount: panel.messageLines
                elide: Text.ElideRight
            }
            Column {
                x: missingText.width + panel.theme.space2xl
                width: parent.width - x
                spacing: panel.theme.spaceMd
                Text {
                    text: "INSTALL FROM THE SOURCE TREE"
                    color: panel.theme.inkFaint
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeSmall
                    font.letterSpacing: panel.theme.labelSpacing
                }
                Rectangle {
                    width: parent.width
                    height: command.implicitHeight + 2 * panel.theme.commandPadY
                    radius: panel.theme.radiusXl
                    color: panel.theme.lane
                    border.color: panel.theme.laneEdge
                    Text {
                        id: command
                        objectName: "fretInstallCommand"
                        x: panel.theme.commandPadX
                        y: panel.theme.commandPadY
                        width: parent.width - 2 * panel.theme.commandPadX
                        text: panel.library.install_command !== undefined ? panel.library.install_command : ""
                        color: panel.theme.pluck
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeLabel
                        wrapMode: Text.WrapAnywhere
                    }
                }
                Row {
                    spacing: panel.theme.spaceMd
                    Kit.Button {
                        theme: panel.theme
                        objectName: "fretCopyInstall"
                        label: "Copy command"
                        onClicked: panel.controller.set_clipboard_text(command.text)
                    }
                    Kit.Button {
                        theme: panel.theme
                        objectName: "fretLibraryReload"
                        label: "Look again"
                        onClicked: {
                            panel.controller.fret_reload_library()
                            ++panel.libraryLook
                        }
                    }
                }
            }
        }
    }

    // ---- Articulation -----------------------------------------------------------------

    Card {
        id: articulation
        objectName: "fretArticulation"
        x: feel.x + feel.width + panel.gap
        y: panel.bodyY
        width: panel.rightWidth
        height: panel.articulationHeight

        Column {
            x: panel.cardPad
            y: panel.rightPad
            width: articulation.width - 2 * panel.cardPad
            spacing: panel.theme.spaceLg
            CardTitle {
                title: "ARTICULATION"
                dot: panel.theme.staccato
            }
            Kit.ChoiceTiles {
                width: parent.width
                controller: panel.controller
                theme: panel.theme
                parameterId: "fret.articulation"
                columns: 2
                tileHeight: panel.theme.compactTileHeight
                nameSize: panel.theme.typeEmphasis
                accent: panel.theme.staccato
                tint: panel.theme.tileOnAmber
            }
            Text {
                width: parent.width
                text: "Auto lets Feel choose per note. The others force one sound for every note: handy for "
                    + "a muted, staccato or behind-the-bridge part."
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeLabel
                lineHeight: panel.theme.bodyLineHeight
                wrapMode: Text.WordWrap
            }
            Slide {
                width: parent.width
                parameterId: "fret.release"
                label: "Release noise"
                accent: panel.theme.staccato
                format: panel.offText
            }
            Slide {
                width: parent.width
                parameterId: "fret.slide"
                label: "Slides"
                accent: panel.theme.staccato
                hint: "Overlapping notes slide instead of plucking again"
                format: panel.offText
            }
        }
    }

    // ---- Tone -------------------------------------------------------------------------

    Card {
        id: tone
        objectName: "fretTone"
        x: articulation.x
        y: articulation.y + articulation.height + panel.gap
        width: panel.rightWidth
        // Down to the library plate's foot.
        height: libraryPlate.y + libraryPlate.height - y

        Column {
            x: panel.cardPad
            y: panel.rightPad
            width: tone.width - 2 * panel.cardPad
            spacing: panel.theme.spaceXs
            CardTitle {
                title: "TONE"
                dot: panel.theme.toneDot
            }
            Slide { width: parent.width; parameterId: "fret.tone.amp"; accent: panel.theme.toneDot }
            Slide { width: parent.width; parameterId: "fret.tone.tilt"; accent: panel.theme.toneDot }
            Slide { width: parent.width; parameterId: "fret.tone.squash"; accent: panel.theme.toneDot; format: panel.offText }
            Slide { width: parent.width; parameterId: "fret.tone.grit"; accent: panel.theme.toneDot; format: panel.offText }
            Slide { width: parent.width; parameterId: "fret.tune"; accent: panel.theme.toneDot }
            Slide { width: parent.width; parameterId: "fret.level"; accent: panel.theme.toneDot }
        }
    }

    Timer {
        objectName: "fretTelemetryPoll"
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
