pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
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

    // Live speed and timing readings move every frame: whole numbers.
    readonly property int wholeDecimals: 0
    function reading(key) {
        if (!playing)
            return "—"
        const v = Number(live[key])
        switch (key) {
        case "distance": return Format.quantity(v, "m")
        case "speed": return Format.quantity(v * kmhPerMetrePerSecond, "km/h", wholeDecimals)
        case "pitch_cents": return Format.cents(v)
        case "itd_us": return speakers ? "pan only"
                                       : (v >= 0 ? "R " : "L ") + Format.quantity(Math.abs(v), "µs", wholeDecimals)
        case "level": {
            const db = (Number(live.level_left_db) + Number(live.level_right_db)) / 2
            return Format.signed_decibels(db <= floorDb ? -Infinity : db)
        }
        case "room": return Format.fraction(v)
        }
        return ""
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

    Kit.DeviceHeader {
        // Flyby's presets are scenes.
        presetTag: "SCENE · " + panel.controller.preset_category.toUpperCase()
            + (panel.controller.preset_modified ? " · EDITED" : "")
        id: header
        x: panel.margin
        y: panel.theme.headerTop
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        // FLY in glacier, BY in the path's amber, over the tagline.
        brand: Component {
            Item {
                implicitWidth: wordmark.implicitWidth
                implicitHeight: tagline.y + tagline.height
                Text {
                    id: wordmark
                    objectName: "flybyWordmark"
                    text: "FLY<font color=\"" + panel.theme.path + "\">BY</font>"
                    textFormat: Text.StyledText
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Text {
                    id: tagline
                    objectName: "flybyTagline"
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "SOUND IN MOTION · DOPPLER · DISTANCE · EARS"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                    // Kept clear of the preset browser: a wider fallback font
                    // steps the size down rather than running under it.
                    width: panel.theme.headerBrowserX - x - panel.theme.headerGap
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: panel.theme.typeFitFloor
                }
            }
        }
    }
    // In and out, held peaks, left of the status badge.
    Row {
        x: header.x + header.status.x - panel.theme.headerGap - width
        y: header.y + (header.rowHeight - height) / 2
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
        // The chart in metres from you; LIVE while the engine moves the
        // source, else a PREVIEW of the path from its settings.
        Kit.PlotFrame {
            anchors.fill: parent
            anchors.margins: panel.theme.hairline
            theme: panel.theme
            live: map.playing
            yUnit: "m"
            yTicks: map.metreTicks
            FlybyMap {
                id: map
                anchors.fill: parent
                controller: panel.controller
                theme: panel.theme
                live: panel.live
            }
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
                    inactiveReason: inactive ? "Radius sets an Orbit path" : ""
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
                    inactiveReason: panel.run === panel.runSync ? "Speed follows Sync's Length"
                        : panel.run === panel.runPosition ? "Position places the pass, not Speed" : ""
                }
                PanelKnob {
                    parameterId: "flyby.position"
                    inactive: panel.run !== panel.runPosition
                    inactiveReason: inactive ? "Position works in the Position run mode" : ""
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
            note: "100 % IS THE REAL WORLD"
            width: parent.width
            height: panel.theme.physicsCardHeight
            Row {
                x: panel.theme.spaceMd
                y: panel.theme.cardBodyY
                // Room between neighbouring labels, so two long ones never meet.
                spacing: panel.theme.spaceSm
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
