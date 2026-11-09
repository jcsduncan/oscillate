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
    readonly property string bandOffReason: "Band " + (selected + 1) + " is off"
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

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        brand: Component {
            Item {
                implicitWidth: wordmark.implicitWidth
                implicitHeight: subtitle.y + subtitle.implicitHeight
                Text {
                    id: wordmark
                    objectName: "shapeWordmark"
                    text: "SHAPE"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                // The eight band colours, as one rule.
                Row {
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.bandRuleGap
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
                    id: subtitle
                    objectName: "shapeSubtitle"
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "PARAMETRIC EQ · EIGHT BANDS"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
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
        note: panel.selectedOn ? panel.kindNames[panel.selectedKind] : ""
        noteColor: panel.theme.bands[panel.selected]
        x: bandsPlate.x + bandsPlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight

        // A band that is off has nothing to set here: the card shows OFF and
        // where to switch it on, not a card of dimmed controls.
        Rectangle {
            objectName: "shapeBandOff"
            visible: !panel.selectedOn
            x: panel.theme.cardTextX
            y: panel.theme.cardRowY
            width: offText.implicitWidth + 2 * panel.theme.offChipPad
            height: panel.theme.offChipHeight
            radius: panel.theme.offChipRadius
            color: panel.theme.well
            border.color: panel.theme.wellEdge
            Text {
                id: offText
                anchors.centerIn: parent
                text: "OFF"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeMicro
                font.bold: true
                font.letterSpacing: panel.theme.capsSpacing
            }
        }
        Note {
            visible: !panel.selectedOn
            x: panel.theme.cardTextX
            y: panel.theme.cardRowY + panel.theme.offChipHeight + panel.theme.spaceSm
            width: bandPlate.width - 2 * panel.theme.cardTextX
            text: "Switch the band on at its tile to hear it."
        }
        Caption {
            visible: panel.selectedOn
            x: panel.theme.cardTextX
            y: panel.theme.cardRowY
            text: "KIND"
        }
        Kit.Segment {
            visible: panel.selectedOn
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
            visible: panel.selectedOn
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
                inactiveReason: panel.selectedOn ? "" : panel.bandOffReason
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
                inactiveReason: panel.selectedOn ? "" : panel.bandOffReason
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
                inactiveReason: !panel.selectedOn ? panel.bandOffReason
                    : panel.selectedKind < 2 ? "A pass filter has no gain" : ""
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
