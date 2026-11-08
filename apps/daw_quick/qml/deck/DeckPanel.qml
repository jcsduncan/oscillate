pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Deck device panel: a club mixer's channel EQ. On the right, the
// channel itself: HIGH, MID and LOW as big knobs top to bottom, each with
// its kill button and a live level bar in the band's colour. On the left,
// the response the isolator runs (each band and their sum), and under it
// the crossovers and the output gain beside the output peak meters. Every
// control reads and writes through `controller`
// (NativeSynthDeviceController for Deck); the levels come only from the
// telemetry (layout in deck.hpp: band RMS low, mid, high, then output peak
// left, right).
Rectangle {
    id: panel
    required property var controller
    property var live: ({ live: false })
    property DeckTheme theme: DeckTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int revision: controller.revision
    readonly property real floorDb: -60
    // The telemetry layout's length (deck.hpp); a full frame means it plays.
    readonly property int frameSize: 5
    readonly property bool playing: live.frame !== undefined && live.frame.length >= frameSize
    readonly property var frame: playing ? live.frame : []
    function bandRms(band) { return playing ? frame[band] : -200 }
    function outputPeak(side) { return playing ? frame[3 + side] : -200 }

    objectName: "deckPanel"
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
    Accessible.name: "Deck DJ EQ"

    // A band's bottom stop, where it reads as fully cut.
    readonly property real bottomStopDb: -40
    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        return info.name === undefined ? "" : (info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) { return id === "" || revision < 0 ? "" : controller.display(id) }
    // A band's shown state: KILLED, or its gain (−∞ at the bottom stop).
    function bandState(name) {
        if (revision < 0)
            return ""
        if (controller.choice("deck." + name + "_kill") === 1)
            return "KILLED"
        const db = controller.value("deck." + name)
        return Format.signed_decibels(db <= bottomStopDb ? -Infinity : db)
    }

    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.spaceXxs
            text: "DECK"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        // The three band colours, as one rule.
        Row {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.wordmarkRuleGap
            Repeater {
                model: panel.theme.bands.length
                delegate: Rectangle {
                    required property int index
                    width: (wordmark.implicitWidth - panel.theme.wordmarkRuleInset) / panel.theme.bands.length
                    height: panel.theme.wordmarkRuleHeight
                    color: panel.theme.bands[index]
                }
            }
        }
        Text {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "DJ ISOLATOR · THREE BANDS"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.subtitleSpacing
        }

        Row {
            x: panel.theme.presetX
            y: panel.theme.presetY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "deckPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "deckPresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.accent)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Text {
                    x: panel.theme.presetTextX
                    y: panel.theme.presetCaptionY
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
                Text {
                    objectName: "deckPresetName"
                    x: panel.theme.presetTextX
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeHeading
                    font.letterSpacing: panel.theme.presetNameSpacing
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
                    objectName: "deckReadout"
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
                    objectName: "deckReadoutValue"
                    x: panel.theme.readoutX
                    y: panel.theme.readoutValueY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
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
                objectName: "deckPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            objectName: "deckStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- Left: the response, then crossovers and output ------------------------------

    readonly property real bodyY: theme.bodyY
    readonly property real channelWidth: theme.channelWidth
    readonly property real leftWidth: width - 2 * margin - gap - channelWidth

    Kit.Card {
        id: responsePlate
        theme: panel.theme
        title: "RESPONSE"
        note: "EACH BAND AND THE SUM YOU HEAR"
        x: panel.margin
        y: panel.bodyY
        width: panel.leftWidth
        height: panel.theme.responseHeight
        DeckResponse {
            objectName: "deckResponse"
            x: panel.theme.screenInset
            y: panel.theme.cardBodyY
            width: responsePlate.width - 2 * panel.theme.screenInset
            height: responsePlate.height - panel.theme.cardBodyY - panel.theme.screenInset
            controller: panel.controller
            theme: panel.theme
        }
    }

    Kit.Card {
        id: shapePlate
        theme: panel.theme
        title: "CROSSOVERS · OUTPUT"
        note: "24 dB/OCT LINKWITZ-RILEY · FLAT AT 0 dB"
        x: panel.margin
        y: responsePlate.y + responsePlate.height + panel.gap
        width: panel.leftWidth
        height: panel.height - y - panel.margin

        Row {
            x: panel.theme.spaceXl
            y: panel.theme.knobRowY
            spacing: panel.theme.knobRowSpacing
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "deck.low_x"
                accent: panel.theme.bands[0]
                diameter: panel.theme.smallKnobDiameter
                readout: true
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "deck.high_x"
                accent: panel.theme.bands[2]
                diameter: panel.theme.smallKnobDiameter
                readout: true
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "deck.gain"
                diameter: panel.theme.smallKnobDiameter
                readout: true
            }
        }
        // The output peak meters.
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.outputMeterMargin
            y: panel.theme.cardContentY
            spacing: panel.theme.spaceSm
            Repeater {
                model: 2
                delegate: Column {
                    required property int index
                    spacing: panel.theme.spaceXs
                    Kit.PeakMeter {
                        objectName: "deckOutputPeak" + (index === 0 ? "L" : "R")
                        theme: panel.theme
                        width: panel.theme.outputMeterWidth
                        height: shapePlate.height - panel.theme.outputMeterInset
                        peakDb: panel.outputPeak(index)
                        live: panel.playing
                    }
                    Caption {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: index === 0 ? "L" : "R"
                    }
                }
            }
        }
    }

    // ---- Right: the channel, high at the top ------------------------------------------

    Kit.Card {
        id: channelPlate
        theme: panel.theme
        title: "CHANNEL"
        note: panel.playing ? "BAND LEVELS LIVE" : "NO SIGNAL"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: responsePlate.x + responsePlate.width + panel.gap
        y: panel.bodyY
        width: panel.channelWidth
        height: panel.height - y - panel.margin

        Column {
            x: panel.theme.spaceLg
            y: panel.theme.cardContentY
            width: channelPlate.width - 2 * panel.theme.spaceLg
            spacing: panel.theme.spaceMd
            Repeater {
                // High, mid, low: as the knobs stand on a mixer.
                model: [2, 1, 0]
                delegate: Rectangle {
                    id: bandRow
                    required property int modelData
                    readonly property int band: modelData
                    readonly property string name: ["low", "mid", "high"][band]
                    readonly property color tone: panel.theme.bands[band]
                    readonly property bool killed: panel.revision >= 0
                        && panel.controller.choice("deck." + name + "_kill") === 1
                    objectName: "deckBand:" + name
                    width: parent.width
                    height: (channelPlate.height - panel.theme.cardContentY - panel.theme.spaceLg
                             - (panel.theme.bands.length - 1) * panel.theme.spaceMd) / panel.theme.bands.length
                    radius: panel.theme.radiusXl
                    color: panel.theme.well
                    border.color: killed ? tone : panel.theme.wellEdge

                    Kit.Knob {
                        x: panel.theme.spaceSm
                        anchors.verticalCenter: parent.verticalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: "deck." + bandRow.name
                        accent: bandRow.tone
                        diameter: panel.theme.knobDiameter
                        // The arc grows from the 0 dB detent.
                        origin: 40 / 46
                        inactive: bandRow.killed
                    }
                    Text {
                        x: panel.theme.bandTextX
                        y: panel.theme.bandNameY
                        text: panel.theme.bandNames[bandRow.band]
                        color: bandRow.tone
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeHeading
                        font.bold: true
                        font.letterSpacing: panel.theme.bandNameSpacing
                    }
                    Text {
                        objectName: "deckBandState:" + bandRow.name
                        x: panel.theme.bandTextX
                        y: panel.theme.bandStateY
                        text: panel.bandState(bandRow.name)
                        color: bandRow.killed ? bandRow.tone : panel.theme.ink
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeEmphasis
                    }
                    // The band's live level after its gain, -60..0 dBFS.
                    Rectangle {
                        objectName: "deckBandLevel:" + bandRow.name
                        x: panel.theme.bandTextX
                        y: panel.theme.bandLevelY
                        width: panel.theme.bandLevelWidth
                        height: panel.theme.bandLevelHeight
                        radius: panel.theme.radiusSm
                        color: panel.theme.track
                        Rectangle {
                            readonly property real level: panel.playing
                                ? Math.max(0, Math.min(1, (panel.bandRms(bandRow.band) - panel.floorDb) / -panel.floorDb)) : 0
                            width: parent.width * level
                            height: parent.height
                            radius: panel.theme.radiusSm
                            color: bandRow.tone
                        }
                    }
                    // Under the level bar, clear of it.
                    Kit.Toggle {
                        x: panel.theme.bandTextX
                        y: panel.theme.bandKillY
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: "deck." + bandRow.name + "_kill"
                        label: "KILL"
                        lit: bandRow.tone
                    }
                }
            }
        }
    }

    Timer {
        objectName: "deckTelemetryPoll"
        interval: panel.playing ? 33 : 250
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
