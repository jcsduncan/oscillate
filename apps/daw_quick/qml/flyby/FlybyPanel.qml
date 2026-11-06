pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "FlybyPaths.js" as Paths

// The Flyby device panel (ADR-0067): a sound travelling along a path past
// your head. The chart leads (you at the centre, the path to drag, the
// source where the engine hears it now) with the live readings under it;
// the cards on the right set the path, how it moves, the physics, the room
// and how you listen. Every control reads and writes through `controller`
// (NativeSynthDeviceController for Flyby); live readings come only from the
// telemetry (layout in flyby.hpp).
Rectangle {
    id: panel
    required property var controller
    // The tracks keying Flyby (Once plays a pass on each of their notes).
    property bool keyed: false
    property string keyName: "the sidechain"
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property FlybyTheme theme: FlybyTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.panelGap
    readonly property real sideWidth: theme.sideWidth
    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true
    readonly property int run: revision >= 0 ? controller.choice("flyby.run") : 0
    readonly property bool orbit: revision >= 0 && controller.choice("flyby.path") === Paths.orbit
    readonly property bool speakers: revision >= 0 && controller.choice("flyby.listen") === 1
    // Flyby's run modes (flyby_table.hpp): Loop, Once, Sync, Position.
    readonly property int runLoop: 0
    readonly property int runOnce: 1
    readonly property int runSync: 2
    readonly property int runPosition: 3
    readonly property real kmhPerMetrePerSecond: 3.6
    readonly property real floorDb: -60
    // A meter's reading with nothing playing.
    readonly property real silentDb: -200
    // Telemetry polls: a frame's time while playing, slower while idle.
    readonly property int livePoll: 33
    readonly property int idlePoll: 250

    objectName: "flybyPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Flyby spatial mover"

    // ---- Formatting ------------------------------------------------------------------

    function signed(value, digits) {
        const text = Math.abs(value).toFixed(digits)
        const tiny = 0.5 * Math.pow(10, -digits)
        return value < -tiny ? "−" + text : value > tiny ? "+" + text : text
    }
    function reading(key) {
        if (!playing)
            return "—"
        const v = Number(live[key])
        switch (key) {
        case "distance": return v.toFixed(1) + " m"
        case "speed": return Math.round(v * kmhPerMetrePerSecond) + " km/h"
        case "pitch_cents": return signed(v, 0) + " ct"
        case "itd_us": return speakers ? "pan only" : (v >= 0 ? "R " : "L ") + Math.abs(v).toFixed(0) + " µs"
        case "level": {
            const db = (Number(live.level_left_db) + Number(live.level_right_db)) / 2
            return db <= floorDb ? "−∞" : signed(db, 1) + " dB"
        }
        case "room": return Math.round(v * 100) + "%"
        }
        return ""
    }
    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        return info.name === undefined ? "" : (info.group + " · " + info.name).toUpperCase()
    }

    component Note: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeCaption
        wrapMode: Text.WordWrap
    }
    component PanelKnob: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
        readout: true
        accent: panel.theme.path
    }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerTop
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: 0
            text: "FLY<font color=\"" + panel.theme.path + "\">BY</font>"
            textFormat: Text.StyledText
            color: panel.theme.glacier
            font.family: panel.theme.display
            font.pixelSize: panel.theme.wordmarkSize
            font.bold: true
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            x: panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.spaceXxs
            text: "SOUND IN MOTION · DOPPLER · DISTANCE · EARS"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }
        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "flybyPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "flybyPresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Scene"
                Accessible.description: panel.controller.preset_name
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.accent)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Text {
                    x: panel.theme.spaceLg
                    y: panel.theme.presetCaptionY
                    text: "SCENE · " + panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.presetCaptionSpacing
                }
                Text {
                    objectName: "flybyPresetName"
                    x: panel.theme.spaceLg
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeSubtitle
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.theme.presetDividerX
                    y: panel.theme.presetDividerInset
                    width: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.presetDividerInset
                    color: panel.theme.wellEdge
                }
                Text {
                    objectName: "flybyReadout"
                    x: panel.theme.readoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "flybyReadoutValue"
                    x: panel.theme.readoutX
                    y: panel.theme.readoutValueY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.theme.lastTouched === "" || panel.revision < 0 ? ""
                        : panel.controller.display(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeEmphasis
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.chevronMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.inkDim
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
                objectName: "flybyPresetNext"
                direction: 1
            }
        }
        // In and out, held peaks.
        Row {
            anchors.right: parent.right
            y: panel.theme.meterTop
            height: panel.theme.meterHeight
            spacing: panel.theme.spaceXs
            Kit.PeakMeter {
                objectName: "flybyMeterIn"
                theme: panel.theme
                width: panel.theme.meterWidth
                height: panel.theme.meterHeight
                live: panel.playing
                peakDb: panel.playing ? Number(panel.live.input_peak) : panel.silentDb
            }
            Item { width: panel.theme.spaceXs; height: 1 }
            Kit.PeakMeter {
                objectName: "flybyMeterOutLeft"
                theme: panel.theme
                width: panel.theme.meterWidth
                height: panel.theme.meterHeight
                live: panel.playing
                peakDb: panel.playing ? Number(panel.live.output_peak_left) : panel.silentDb
            }
            Kit.PeakMeter {
                objectName: "flybyMeterOutRight"
                theme: panel.theme
                width: panel.theme.meterWidth
                height: panel.theme.meterHeight
                live: panel.playing
                peakDb: panel.playing ? Number(panel.live.output_peak_right) : panel.silentDb
            }
        }
    }

    // ---- The chart -------------------------------------------------------------------

    Rectangle {
        id: chartFrame
        x: panel.margin
        y: header.y + header.height + panel.gap
        width: panel.width - 2 * panel.margin - panel.sideWidth - panel.gap
        height: panel.height - y - panel.margin
        radius: panel.theme.radiusXl
        color: panel.theme.chart
        border.color: panel.theme.plateEdge
        clip: true
        FlybyMap {
            id: map
            anchors.fill: parent
            anchors.margins: panel.theme.hairline
            controller: panel.controller
            theme: panel.theme
            live: panel.live
        }
        Flow {
            x: panel.theme.spaceLg
            width: parent.width - 2 * panel.theme.spaceLg
            anchors.bottom: parent.bottom
            anchors.bottomMargin: panel.theme.spaceLg
            spacing: panel.theme.spaceSm
            Repeater {
                model: [
                    { key: "distance", label: "DISTANCE" }, { key: "speed", label: "SPEED" },
                    { key: "pitch_cents", label: "DOPPLER" }, { key: "itd_us", label: "EARS L/R" },
                    { key: "level", label: "LEVEL" }, { key: "room", label: "ROOM" }
                ]
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    objectName: "flybyReading:" + modelData.key
                    width: panel.theme.readingWidth
                    height: panel.theme.readingHeight
                    radius: panel.theme.readingRadius
                    color: panel.theme.readingFill
                    border.color: panel.theme.plateEdge
                    Text {
                        x: panel.theme.readingInset
                        y: panel.theme.readingCaptionY
                        text: chip.modelData.label
                        color: panel.theme.inkFaint
                        font.pixelSize: panel.theme.typeMicro
                        font.bold: true
                        font.letterSpacing: panel.theme.readingSpacing
                    }
                    Text {
                        objectName: "flybyReadingValue:" + chip.modelData.key
                        x: panel.theme.readingInset
                        y: panel.theme.readingValueY
                        text: panel.reading(chip.modelData.key)
                        color: chip.modelData.key === "pitch_cents" ? panel.theme.path : panel.theme.ink
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeEmphasis
                    }
                }
            }
        }
    }

    // ---- The cards -------------------------------------------------------------------

    Column {
        id: side
        x: panel.width - panel.margin - panel.sideWidth
        y: chartFrame.y
        width: panel.sideWidth
        spacing: panel.gap

        Kit.Card {
            id: pathCard
            theme: panel.theme
            title: "PATH"
            note: panel.orbit ? "A CIRCLE ROUND YOU" : "DRAG THE POINTS ON THE CHART"
            width: parent.width
            height: panel.theme.pathCardHeight
            Kit.Segment {
                x: panel.theme.spaceLg
                y: panel.theme.cardBodyY
                width: panel.theme.pathSegmentWidth
                height: panel.theme.segmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "flyby.path"
                labels: ["PASS", "ORBIT"]
            }
            Kit.Segment {
                visible: panel.orbit
                x: panel.theme.directionX
                y: panel.theme.cardBodyY
                width: pathCard.width - panel.theme.directionX - panel.theme.spaceLg
                height: panel.theme.segmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "flyby.direction"
                labels: ["CLOCKWISE", "ANTICLOCKWISE"]
            }
            Row {
                x: panel.theme.spaceMd
                y: panel.theme.cardKnobRowY
                PanelKnob {
                    parameterId: "flyby.radius"
                    inactive: !panel.orbit
                }
                PanelKnob { parameterId: "flyby.height" }
            }
            Note {
                x: panel.theme.cardSideX
                y: panel.theme.pathNoteY
                width: pathCard.width - panel.theme.cardSideX - panel.theme.spaceLg
                text: "Height lifts the path above you: a plane overhead, a bird on a wire."
            }
        }

        Kit.Card {
            id: motionCard
            theme: panel.theme
            title: "MOTION"
            note: panel.run === panel.runOnce
                ? (panel.keyed ? "ON EACH NOTE · " + panel.keyName.toUpperCase() : "NO KEY · SEND A MIDI TRACK HERE")
                : ""
            noteColor: panel.keyed ? panel.theme.listener : panel.theme.inkFaint
            width: parent.width
            height: panel.theme.motionCardHeight
            Kit.Segment {
                x: panel.theme.spaceLg
                y: panel.theme.cardBodyY
                width: motionCard.width - 2 * panel.theme.spaceLg
                height: panel.theme.segmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "flyby.run"
                labels: ["LOOP", "ONCE", "SYNC", "POSITION"]
            }
            Row {
                x: panel.theme.spaceMd
                y: panel.theme.cardKnobRowY
                PanelKnob {
                    parameterId: "flyby.speed"
                    inactive: panel.run === panel.runSync || panel.run === panel.runPosition
                }
                PanelKnob {
                    parameterId: "flyby.position"
                    inactive: panel.run !== panel.runPosition
                }
            }
            Column {
                x: panel.theme.cardSideX
                y: panel.theme.motionSideY
                width: motionCard.width - panel.theme.cardSideX - panel.theme.spaceLg
                spacing: panel.theme.spaceSm
                Kit.Dropdown {
                    visible: panel.run === panel.runSync
                    width: parent.width
                    controller: panel.controller
                    theme: panel.theme
                    menu: menu
                    parameterId: "flyby.length"
                }
                Kit.Segment {
                    visible: panel.run === panel.runOnce
                    width: parent.width
                    height: panel.theme.segmentHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "flyby.rest"
                    labels: ["WAIT AT START", "SILENT"]
                }
                Note {
                    width: parent.width
                    text: panel.run === panel.runLoop ? "Round and round at Speed, on the song's clock."
                        : panel.run === panel.runOnce ? "One pass at Speed from each note; between passes as set."
                        : panel.run === panel.runSync ? "One pass every Length of the song."
                        : "Draw Position as automation to place the pass on the timeline."
                }
            }
        }

        Kit.Card {
            id: physicsCard
            theme: panel.theme
            title: "PHYSICS"
            note: "100% IS THE REAL WORLD"
            width: parent.width
            height: panel.theme.physicsCardHeight
            Row {
                x: panel.theme.spaceMd
                y: panel.theme.cardBodyY
                Repeater {
                    model: ["flyby.doppler", "flyby.air", "flyby.rolloff", "flyby.reference", "flyby.near"]
                    delegate: PanelKnob {
                        required property string modelData
                        parameterId: modelData
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: panel.gap
            Kit.Card {
                id: roomCard
                theme: panel.theme
                title: "ROOM"
                width: panel.theme.roomCardWidth
                height: side.roomHeight
                Row {
                    x: panel.theme.spaceMd
                    y: panel.theme.cardBodyY
                    PanelKnob { parameterId: "flyby.room"; accent: panel.theme.listener }
                    PanelKnob { parameterId: "flyby.room_size"; accent: panel.theme.listener }
                }
                Note {
                    x: panel.theme.spaceLg
                    y: roomCard.height - panel.theme.cardFootNote
                    width: roomCard.width - 2 * panel.theme.spaceLg
                    text: "Heard more as the source moves away."
                }
            }
            Kit.Card {
                id: outputCard
                theme: panel.theme
                title: "LISTEN"
                width: side.width - roomCard.width - panel.gap
                height: side.roomHeight
                Kit.Segment {
                    x: panel.theme.spaceLg
                    y: panel.theme.cardBodyY
                    width: outputCard.width - 2 * panel.theme.spaceLg
                    height: panel.theme.segmentHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "flyby.listen"
                    labels: ["HEADPHONES", "SPEAKERS"]
                }
                Row {
                    x: panel.theme.spaceMd
                    y: panel.theme.cardKnobRowY
                    PanelKnob { parameterId: "flyby.mix"; accent: panel.theme.glacier }
                    PanelKnob { parameterId: "flyby.output"; accent: panel.theme.glacier }
                }
                Note {
                    objectName: "flybyListenHint"
                    x: panel.theme.spaceLg
                    y: outputCard.height - panel.theme.cardFootNote
                    width: outputCard.width - 2 * panel.theme.spaceLg
                    text: panel.speakers
                        ? "A constant-power pan: ear timing blurs when both ears hear both speakers."
                        : "Each ear its own delay and head shadow: the source sits outside your head."
                }
            }
        }
        readonly property real roomHeight: chartFrame.height - pathCard.height - motionCard.height
            - physicsCard.height - 3 * panel.gap
    }

    Timer {
        objectName: "flybyTelemetryPoll"
        interval: panel.playing ? panel.livePoll : panel.idlePoll
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
