pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Shape device panel: a mastering console's EQ in graphite and ice
// cyan. The response screen leads, the summed curve over each band's own
// curve, with a handle per band that is on; beside it the input and output
// peak meters. Below, the eight bands as tiles (select one, switch it on or
// off) and the selected band's strip: its kind, frequency, Q and gain.
// Every control reads and writes through `controller`
// (NativeSynthDeviceController for Shape); the curve comes from
// shape_response and the meters only from the telemetry (layout in
// shape.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property ShapeTheme theme: ShapeTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    readonly property int bandCount: 8
    // Under this a meter reads silence.
    readonly property real floorDb: -60
    // Bare level readouts (no unit beside them): one decimal.
    readonly property int levelDecimals: 1
    readonly property real hertzPerKilohertz: 1000
    // A meter's peak with no signal, and the peak a readout shows hot from.
    readonly property real silentDb: -200
    readonly property real hotDb: -3

    // The band in the strip (from 0).
    property int selected: 0
    readonly property bool playing: live.live === true
    readonly property var inPeak: playing ? live.input_peak : [silentDb, silentDb]
    readonly property var outPeak: playing ? live.output_peak : [silentDb, silentDb]
    readonly property int selectedKind: revision >= 0 ? controller.choice(bandId(selected, "kind")) : 4
    readonly property bool selectedOn: revision >= 0 && controller.choice(bandId(selected, "on")) === 1
    readonly property var kindNames: ["LOW PASS", "HIGH PASS", "LOW SHELF", "HIGH SHELF", "BELL"]
    readonly property var kindShort: ["LP", "HP", "LS", "HS", "BELL"]
    readonly property var slopeShort: ["12", "24", "48", "96"]

    objectName: "shapePanel"
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
    Accessible.name: "Shape EQ"

    Component.onCompleted: selected = firstOn()

    function bandId(band, name) {
        return "shape." + name + "_" + (band + 1)
    }
    function isOn(band) {
        return revision >= 0 && controller.choice(bandId(band, "on")) === 1
    }
    // The first band that is on, else the first.
    function firstOn() {
        for (let b = 0; b < bandCount; ++b) {
            if (isOn(b))
                return b
        }
        return 0
    }
    function select(band) {
        selected = Math.max(0, Math.min(bandCount - 1, band))
    }

    // ---- Formatting ------------------------------------------------------------------

    // A bare level, −∞ at the floor.
    function dbText(db) {
        return Format.signed_number(db <= floorDb ? -Infinity : db, levelDecimals)
    }
    // A band's frequency on its tile, "1.20k" above 1 kHz.
    function hzText(hz) {
        return hz >= hertzPerKilohertz ? Format.number(hz / hertzPerKilohertz) + "k" : Format.number(hz, 0)
    }
    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || revision < 0 ? "" : controller.display(id)
    }

    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.capsSpacing
    }
    component Note: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeCaption
        wrapMode: Text.WordWrap
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
            text: "SHAPE"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        // The eight band colours, as one rule.
        Row {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.bandRuleGap
            Repeater {
                model: panel.bandCount
                delegate: Rectangle {
                    required property int index
                    width: (wordmark.implicitWidth - panel.theme.bandRuleTrim) / panel.bandCount
                    height: panel.theme.strokeBold
                    color: panel.theme.bands[index]
                }
            }
        }
        Text {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "PARAMETRIC EQ · EIGHT BANDS"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.subtitleSpacing
        }

        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "shapePresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "shapePresetDisplay"
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
                    font.letterSpacing: panel.theme.capsSpacing
                }
                Text {
                    objectName: "shapePresetName"
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
                    objectName: "shapeReadout"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.presetReadoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.capsTightSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "shapeReadoutValue"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetValueY
                    width: display.width - x - panel.theme.presetReadoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.presetChevronMargin
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
                objectName: "shapePresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "shapeStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- Row A: the response and the meters ------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        id: responsePlate
        theme: panel.theme
        title: "RESPONSE"
        note: "THE CURVE THE EQ RUNS"
        x: panel.margin
        y: panel.rowAY
        width: panel.width - 2 * panel.margin - panel.gap - panel.theme.meterCardWidth
        height: panel.rowAHeight
        ShapeCurve {
            x: panel.theme.cardInset
            y: panel.theme.cardBodyY
            width: responsePlate.width - 2 * panel.theme.cardInset
            height: responsePlate.height - panel.theme.cardBodyY - panel.theme.cardInset
            controller: panel.controller
            theme: panel.theme
            selected: panel.selected
            live: panel.live
            onPicked: band => panel.select(band)
        }
    }

    Kit.Card {
        id: meterPlate
        theme: panel.theme
        title: "LEVELS"
        note: panel.playing ? "PEAK · HELD 1 s" : "NO SIGNAL"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: responsePlate.x + responsePlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        readonly property real meterHeight: panel.theme.meterHeight

        component Pair: Item {
            id: pair
            required property string title
            required property var peak
            required property string prefix
            width: panel.theme.meterPairWidth
            height: meterPlate.meterHeight + panel.theme.meterPairChrome
            Caption {
                anchors.horizontalCenter: parent.horizontalCenter
                text: pair.title
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.meterPeakY
                spacing: panel.theme.spaceXs
                Repeater {
                    model: 2
                    delegate: Text {
                        required property int index
                        objectName: pair.prefix + "Peak" + (index === 0 ? "L" : "R")
                        width: panel.theme.meterPeakWidth
                        horizontalAlignment: Text.AlignHCenter
                        text: panel.playing ? panel.dbText(pair.peak[index]) : "—"
                        color: panel.playing && pair.peak[index] > panel.hotDb ? panel.theme.meterOver
                            : panel.playing ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.meterBarY
                spacing: panel.theme.spaceMd
                Repeater {
                    model: 2
                    delegate: Kit.PeakMeter {
                        required property int index
                        objectName: pair.prefix + "Meter" + (index === 0 ? "L" : "R")
                        theme: panel.theme
                        width: panel.theme.meterBarWidth
                        height: meterPlate.meterHeight
                        live: panel.playing
                        floorDb: panel.floorDb
                        peakDb: pair.peak[index]
                    }
                }
            }
        }

        Pair {
            x: panel.theme.spaceXl
            y: panel.theme.meterPairY
            title: "IN"
            prefix: "shapeIn"
            peak: panel.inPeak
        }
        Pair {
            x: meterPlate.width - panel.theme.spaceXl - width
            y: panel.theme.meterPairY
            title: "OUT"
            prefix: "shapeOut"
            peak: panel.outPeak
        }
        Note {
            x: panel.theme.cardTextX
            y: meterPlate.height - panel.theme.meterNoteBottom
            width: meterPlate.width - 2 * panel.theme.cardTextX
            horizontalAlignment: Text.AlignHCenter
            text: "−60 to +6 dBFS"
        }
    }

    // ---- Row B: the bands and the selected band ------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: panel.height - rowBY - margin

    Kit.Card {
        id: bandsPlate
        theme: panel.theme
        title: "BANDS"
        note: "CLICK TO SELECT · SWITCH ON OR OFF"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.bandsCardWidth
        height: panel.rowBHeight

        Row {
            id: tiles
            objectName: "shapeBands"
            x: panel.theme.cardTextX
            y: panel.theme.cardRowY
            spacing: panel.theme.spaceSm
            activeFocusOnTab: true
            Accessible.role: Accessible.PageTabList
            Accessible.name: "Bands"
            Keys.onLeftPressed: panel.select(panel.selected - 1)
            Keys.onRightPressed: panel.select(panel.selected + 1)
            Repeater {
                model: panel.bandCount
                delegate: Rectangle {
                    id: tile
                    required property int index
                    readonly property bool chosen: index === panel.selected
                    readonly property bool on: panel.isOn(index)
                    readonly property color tone: panel.theme.bands[index]
                    readonly property int kind: panel.revision >= 0 ? panel.controller.choice(panel.bandId(index, "kind")) : 4
                    objectName: "shapeBand:" + (index + 1)
                    width: (bandsPlate.width - 2 * panel.theme.cardTextX
                        - (panel.bandCount - 1) * tiles.spacing) / panel.bandCount
                    height: bandsPlate.height - panel.theme.cardRowY - panel.theme.cardTextX
                    radius: panel.theme.tileRadius
                    color: chosen ? Qt.rgba(tone.r, tone.g, tone.b, panel.theme.tileChosenAlpha)
                        : tileArea.containsMouse ? panel.theme.buttonHi : panel.theme.button
                    border.color: !chosen ? panel.theme.seam
                        : tiles.activeFocus ? panel.theme.accent
                        : Qt.rgba(tone.r, tone.g, tone.b, panel.theme.tileChosenEdgeAlpha)
                    border.width: chosen ? panel.theme.tileChosenStroke : panel.theme.hairline
                    Accessible.role: Accessible.PageTab
                    Accessible.name: "Band " + (index + 1)
                    Accessible.checked: chosen
                    Rectangle {
                        x: panel.theme.spaceSm
                        y: panel.theme.spaceSm
                        width: parent.width - 2 * panel.theme.spaceSm
                        height: panel.theme.tileLedHeight
                        radius: height / 2
                        color: tile.on ? tile.tone : panel.theme.ledOff
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: panel.theme.tileNumberY
                        text: tile.index + 1
                        color: tile.on ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.tileNumberSize
                        font.weight: Font.Light
                    }
                    Caption {
                        objectName: "shapeBandKind:" + (tile.index + 1)
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: panel.theme.tileKindY
                        text: panel.kindShort[tile.kind] + (tile.kind < 2 && panel.revision >= 0
                            ? " " + panel.slopeShort[panel.controller.choice(panel.bandId(tile.index, "slope"))] : "")
                        color: tile.on ? tile.tone : panel.theme.inkFaint
                    }
                    Text {
                        objectName: "shapeBandFreq:" + (tile.index + 1)
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: panel.theme.tileFreqY
                        text: panel.revision >= 0 ? panel.hzText(panel.controller.value(panel.bandId(tile.index, "freq"))) : ""
                        color: tile.on ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: panel.theme.tileGainY
                        text: tile.kind >= 2 && panel.revision >= 0
                            ? Format.signed_number(panel.controller.value(panel.bandId(tile.index, "gain")), panel.levelDecimals) : "—"
                        color: tile.on ? panel.theme.inkDim : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                    MouseArea {
                        id: tileArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            tiles.forceActiveFocus()
                            panel.select(tile.index)
                        }
                    }
                    Kit.Switch {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: panel.theme.spaceMd
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.bandId(tile.index, "on")
                        accent: tile.tone
                        label: tile.on ? "ON" : "OFF"
                    }
                }
            }
        }
    }

    Kit.Card {
        id: bandPlate
        theme: panel.theme
        title: "BAND " + (panel.selected + 1)
        note: panel.selectedOn ? panel.kindNames[panel.selectedKind] : "OFF · SWITCH IT ON TO HEAR IT"
        noteColor: panel.selectedOn ? panel.theme.bands[panel.selected] : panel.theme.inkFaint
        x: bandsPlate.x + bandsPlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight

        Caption {
            x: panel.theme.cardTextX
            y: panel.theme.cardRowY
            text: "KIND"
        }
        Kit.Segment {
            x: panel.theme.cardTextX
            y: panel.theme.kindSegmentY
            width: bandPlate.width - 2 * panel.theme.cardTextX
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: panel.bandId(panel.selected, "kind")
            accent: panel.theme.bands[panel.selected]
            labels: panel.kindNames
        }
        Row {
            x: panel.theme.cardInset
            y: panel.theme.bandKnobsY
            spacing: panel.theme.bandKnobGap
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: panel.bandId(panel.selected, "freq")
                accent: panel.theme.bands[panel.selected]
                diameter: panel.theme.bandKnobDiameter
                readout: true
                inactive: !panel.selectedOn
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: panel.bandId(panel.selected, "q")
                accent: panel.theme.bands[panel.selected]
                diameter: panel.theme.bandKnobDiameter
                readout: true
                // A shelf's Q is its slope.
                label: panel.selectedKind === 2 || panel.selectedKind === 3 ? "Slope" : "Q"
                inactive: !panel.selectedOn
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: panel.bandId(panel.selected, "gain")
                accent: panel.theme.bands[panel.selected]
                diameter: panel.theme.bandKnobDiameter
                readout: true
                // The pass filters have no gain.
                inactive: !panel.selectedOn || panel.selectedKind < 2
            }
            Column {
                width: bandPlate.width - 3 * (panel.theme.bandKnobWidth + panel.theme.bandKnobGap)
                    - panel.theme.bandNotesRight
                y: panel.theme.spaceSm
                spacing: panel.theme.spaceSm
                // A pass filter's steepness: 12 dB an octave per stage.
                Caption {
                    visible: panel.selectedKind < 2
                    text: "SLOPE · dB / OCT"
                }
                Kit.Segment {
                    visible: panel.selectedKind < 2
                    width: parent.width
                    height: panel.theme.segmentHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: panel.bandId(panel.selected, "slope")
                    accent: panel.theme.bands[panel.selected]
                    labels: panel.slopeShort
                }
                Note {
                    width: parent.width
                    text: panel.selectedKind < 2
                        ? "A pass filter cuts everything past its corner, steeper with each slope; Q shapes the corner. It has no gain."
                        : panel.selectedKind === 4
                            ? "A bell lifts or cuts around its frequency; a higher Q narrows it."
                            : "A shelf lifts or cuts everything past its corner; the slope sets how steep. A slope over 1 is held at 1."
                }
            }
        }
    }

    Timer {
        objectName: "shapeTelemetryPoll"
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
