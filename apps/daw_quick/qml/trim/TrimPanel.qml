pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Trim device panel: a console channel strip in graphite and sky. The
// gain leads, a big knob with unity marked; beside it the input and output
// meters (peak held for a second, RMS over 300 ms, from the telemetry)
// with the gain change between them; then the stereo image the settings
// make, with pan, its law and width under it. Below: mono, swap and the
// two polarity switches; the monitor solo; and the repair stages. Every
// control reads and writes through `controller`
// (NativeSynthDeviceController for Trim); the meters come only from the
// telemetry (layout in trim.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: panel.idle()
    property TrimTheme theme: TrimTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int revision: controller.revision

    // trim.hpp: input peak, output peak, input RMS, output RMS (dBFS), each
    // left then right.
    readonly property int frameSize: 8
    // Under this a meter reads silence.
    readonly property real floorDb: -60
    // A level change smaller than this reads as none.
    readonly property real unchangedDb: 0.05
    // Unity (0 dB) on the gain knob's -36..+24 dB travel.
    readonly property real gainUnityOrigin: 0.6

    readonly property bool playing: live.playing === true
    readonly property real gainDb: revision >= 0 ? controller.value("trim.gain") : 0
    readonly property real pan: revision >= 0 ? controller.value("trim.pan") : 0
    readonly property real stereoWidth: revision >= 0 ? controller.value("trim.width") : 1
    readonly property bool mono: revision >= 0 && controller.choice("trim.mono") === 1
    readonly property bool swap: revision >= 0 && controller.choice("trim.swap") === 1
    readonly property int invert: revision >= 0 ? controller.choice("trim.invert") : 0
    readonly property int solo: revision >= 0 ? controller.choice("trim.solo") : 0
    readonly property bool dc: revision >= 0 && controller.choice("trim.dc") === 1
    readonly property bool rotate: revision >= 0 && controller.choice("trim.rotate") === 1
    readonly property var soloNames: ["OFF", "LEFT", "RIGHT", "MID", "SIDE"]
    // Every stage at rest: the output is the input, bit for bit.
    readonly property bool atUnity: Math.abs(gainDb) < 1e-9 && Math.abs(pan) < 1e-9
        && Math.abs(stereoWidth - 1) < 1e-9 && !mono && !swap && invert === 0 && solo === 0
        && !dc && !rotate

    objectName: "trimPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Trim"

    function idle() {
        return { playing: false, inPeak: [-200, -200], outPeak: [-200, -200],
                 inRms: [-200, -200], outRms: [-200, -200] }
    }
    function decode(state) {
        const f = state.frame
        if (f === undefined || f.length < frameSize)
            return idle()
        return {
            playing: true,
            inPeak: [f[0], f[1]],
            outPeak: [f[2], f[3]],
            inRms: [f[4], f[5]],
            outRms: [f[6], f[7]]
        }
    }

    // ---- Formatting ------------------------------------------------------------------

    function signed(value, digits) {
        const text = Math.abs(value).toFixed(digits)
        const tiny = 0.5 * Math.pow(10, -digits)
        return value < -tiny ? "−" + text : value > tiny ? "+" + text : text
    }
    function dbText(db) {
        return db <= floorDb ? "−∞" : signed(db, 1)
    }
    function panText(value) {
        const amount = Math.round(Math.abs(value) * 100)
        return amount === 0 ? "C" : (value < 0 ? "L " : "R ") + amount
    }
    // The louder channel's level, as a meter would quote it.
    function louder(pair) {
        return Math.max(pair[0], pair[1])
    }
    // Output RMS over input RMS (the louder channel each side), when both
    // carry signal.
    readonly property bool changeKnown: playing && louder(live.inRms) > floorDb
        && louder(live.outRms) > floorDb
    readonly property real changeDb: changeKnown ? louder(live.outRms) - louder(live.inRms) : 0

    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group === info.name ? info.name : info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        if (id === "" || revision < 0)
            return ""
        return id === "trim.pan" ? panText(pan) : controller.display(id)
    }
    // A summary of what Trim is doing, in a line.
    function summary() {
        if (atUnity)
            return "UNITY · BIT-EXACT PASS-THROUGH"
        const parts = []
        if (Math.abs(gainDb) >= 1e-9)
            parts.push(signed(gainDb, 1) + " DB")
        if (Math.abs(pan) >= 1e-9)
            parts.push("PAN " + panText(pan))
        if (mono)
            parts.push("MONO")
        else if (Math.abs(stereoWidth - 1) >= 1e-9)
            parts.push("WIDTH " + Math.round(stereoWidth * 100) + "%")
        if (swap)
            parts.push("SWAPPED")
        if (invert !== 0)
            parts.push("Ø " + ["", "L", "R", "L+R"][invert])
        if (solo !== 0)
            parts.push("SOLO " + soloNames[solo])
        if (dc)
            parts.push("DC BLOCK")
        if (rotate)
            parts.push("ROTATED")
        return parts.join(" · ")
    }

    // A plate: a fine bevel along the top, the title in spaced caps.
    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
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
            y: panel.theme.wordmarkY
            text: "TRIM"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.wordmarkIndent
            y: wordmark.y + wordmark.height + panel.theme.underlineGap
            width: wordmark.implicitWidth - panel.theme.underlineTrim
            height: panel.theme.strokeBold
            radius: panel.theme.underlineRadius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.left }
                GradientStop { position: 1; color: panel.theme.right }
            }
        }
        Text {
            x: wordmark.x + panel.theme.wordmarkIndent
            y: wordmark.y + wordmark.height + panel.theme.taglineGap
            text: "GAIN · PAN · STEREO · REPAIR"
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
                objectName: "trimPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "trimPresetDisplay"
                width: panel.theme.presetDisplayWidth
                height: panel.theme.presetDisplayHeight
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
                    objectName: "trimPresetName"
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
                    objectName: "trimReadout"
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
                    objectName: "trimReadoutValue"
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
                objectName: "trimPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "trimStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- Row A: level, meters, stereo image ---------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        theme: panel.theme
        id: levelPlate
        title: "LEVEL"
        note: panel.atUnity ? "UNITY" : ""
        noteColor: panel.theme.live
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.levelPlateWidth
        height: panel.rowAHeight

        Kit.Knob {
            id: gainKnob
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.gainKnobY
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.gain"
            diameter: panel.theme.gainKnobDiameter
            origin: panel.gainUnityOrigin
            label: ""
            valueText: panel.signed(panel.gainDb, 1) + " dB"
        }
        Text {
            objectName: "trimGainValue"
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.gainValueY
            text: panel.signed(panel.gainDb, 1)
            color: Math.abs(panel.gainDb) < 1e-9 ? panel.theme.glacier : panel.theme.accent
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.gainValueSize
            font.weight: Font.Light
            Text {
                anchors.left: parent.right
                anchors.leftMargin: panel.theme.spaceSm
                anchors.baseline: parent.baseline
                text: "dB"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeHeading
            }
        }
        Caption {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.gainCaptionY
            text: "GAIN · −36 TO +24 DB"
        }
        Rectangle {
            id: unity
            objectName: "trimUnity"
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.unityY
            width: panel.theme.unityWidth
            height: panel.theme.unityHeight
            radius: height / 2
            color: unityArea.pressed ? panel.theme.button : panel.theme.buttonHi
            border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
            activeFocusOnTab: panel.controller.editable
            opacity: Math.abs(panel.gainDb) < 1e-9 ? panel.theme.unityIdleOpacity : 1
            Accessible.role: Accessible.Button
            Kit.HoverTip { theme: panel.theme }
            Accessible.name: "Gain to unity"
            function press() {
                panel.controller.reset_to_default("trim.gain")
                panel.theme.lastTouched = "trim.gain"
            }
            Keys.onReturnPressed: press()
            Keys.onSpacePressed: press()
            Text {
                anchors.centerIn: parent
                text: "0 dB · UNITY"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeSmall
                font.bold: true
                font.letterSpacing: panel.theme.unitySpacing
            }
            MouseArea {
                id: unityArea
                anchors.fill: parent
                enabled: panel.controller.editable
                onClicked: unity.press()
            }
        }
        Note {
            x: panel.theme.centredNoteInset
            y: panel.theme.levelNoteY
            width: levelPlate.width - 2 * panel.theme.centredNoteInset
            horizontalAlignment: Text.AlignHCenter
            text: "Shift-drag for fine steps; double-click or Delete returns to unity."
        }
    }

    Kit.Card {
        theme: panel.theme
        id: meterPlate
        title: "METERS"
        note: panel.playing ? "PEAK HELD 1 S · RMS 300 MS" : "NO SIGNAL"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: levelPlate.x + levelPlate.width + panel.gap
        y: panel.rowAY
        width: panel.theme.meterPlateWidth
        height: panel.rowAHeight

        readonly property real meterHeight: panel.theme.meterLength
        readonly property var marks: [6, 0, -6, -12, -18, -24, -36, -48, -60]

        component Pair: Item {
            id: pair
            required property string title
            required property var peak
            required property var rms
            required property string prefix
            width: panel.theme.meterPairWidth
            height: meterPlate.meterHeight + panel.theme.meterPairChrome
            Caption {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 0
                text: pair.title
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.peakReadoutY
                spacing: panel.theme.spaceSm
                Repeater {
                    model: 2
                    delegate: Text {
                        required property int index
                        objectName: pair.prefix + "Peak" + (index === 0 ? "L" : "R")
                        width: panel.theme.peakReadoutWidth
                        horizontalAlignment: Text.AlignHCenter
                        text: panel.playing ? panel.dbText(pair.peak[index]) : "—"
                        color: panel.playing && pair.peak[index] > panel.theme.meterOverDb ? panel.theme.meterHigh
                            : panel.playing ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.pairMeterY
                spacing: panel.theme.spaceMd
                Repeater {
                    model: 2
                    delegate: TrimMeter {
                        required property int index
                        objectName: pair.prefix + "Meter" + (index === 0 ? "L" : "R")
                        theme: panel.theme
                        width: panel.theme.meterWidth
                        height: meterPlate.meterHeight
                        live: panel.playing
                        floorDb: panel.floorDb
                        peakDb: pair.peak[index]
                        rmsDb: pair.rms[index]
                    }
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.pairMeterY + meterPlate.meterHeight + panel.theme.meterLabelGap
                spacing: panel.theme.spaceMd
                Repeater {
                    model: ["L", "R"]
                    delegate: Text {
                        required property string modelData
                        width: panel.theme.meterWidth
                        horizontalAlignment: Text.AlignHCenter
                        text: modelData
                        color: modelData === "L" ? panel.theme.left : panel.theme.right
                        font.pixelSize: panel.theme.typeCaption
                        font.bold: true
                    }
                }
            }
        }

        Pair {
            x: panel.theme.meterPairX
            y: panel.theme.meterPairY
            title: "IN"
            prefix: "trimIn"
            peak: panel.live.inPeak
            rms: panel.live.inRms
        }
        // The dB scale between the two pairs.
        Item {
            id: scale
            x: panel.theme.meterScaleX
            y: panel.theme.meterPairY + panel.theme.pairMeterY
            width: panel.theme.meterScaleWidth
            height: meterPlate.meterHeight
            Repeater {
                model: meterPlate.marks
                delegate: Item {
                    id: mark
                    required property int modelData
                    readonly property real at: (panel.theme.meterCeilDb - modelData) / (panel.theme.meterCeilDb - panel.floorDb)
                    width: scale.width
                    y: (scale.height - 2 * panel.theme.meterInset) * at + panel.theme.meterInset
                    Rectangle {
                        x: 0
                        width: panel.theme.scaleTickLength
                        height: panel.theme.hairline
                        color: panel.theme.gridStrong
                    }
                    Rectangle {
                        x: scale.width - width
                        width: panel.theme.scaleTickLength
                        height: panel.theme.hairline
                        color: panel.theme.gridStrong
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: -panel.theme.scaleLabelLift
                        text: mark.modelData > 0 ? "+" + mark.modelData : mark.modelData === 0 ? "0" : "−" + (-mark.modelData)
                        color: mark.modelData === 0 ? panel.theme.inkDim : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeCaption
                    }
                }
            }
        }
        Pair {
            x: panel.theme.outPairX
            y: panel.theme.meterPairY
            title: "OUT"
            prefix: "trimOut"
            peak: panel.live.outPeak
            rms: panel.live.outRms
        }
        Rectangle {
            x: panel.theme.changeX - panel.theme.spaceMd
            y: panel.theme.meterPairY + panel.theme.pairMeterY
            width: panel.theme.hairline
            height: meterPlate.meterHeight
            color: panel.theme.seam
        }
        // The change from input to output.
        Column {
            x: panel.theme.changeX
            y: panel.theme.meterPairY + panel.theme.pairMeterY
            width: meterPlate.width - x - panel.theme.changeRight
            spacing: panel.theme.spaceXs
            Caption {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "CHANGE"
            }
            Text {
                objectName: "trimChange"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: panel.changeKnown ? panel.signed(panel.changeDb, 1) : "—"
                color: !panel.changeKnown ? panel.theme.inkFaint
                    : Math.abs(panel.changeDb) < panel.unchangedDb ? panel.theme.live : panel.theme.accent
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.changeValueSize
                font.weight: Font.Light
            }
            Caption {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "DB RMS"
            }
        }
        Note {
            x: panel.theme.centredNoteInset
            y: meterPlate.height - panel.theme.footNoteRise
            width: meterPlate.width - 2 * panel.theme.centredNoteInset
            horizontalAlignment: Text.AlignHCenter
            text: "Peak reads above each bar; green to −12 dBFS, amber to −3, red over."
        }
    }

    Kit.Card {
        theme: panel.theme
        id: stagePlate
        title: "STEREO IMAGE"
        note: panel.summary()
        noteColor: panel.atUnity ? panel.theme.live : panel.theme.accent
        x: meterPlate.x + meterPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        Rectangle {
            id: stageWell
            x: panel.theme.spaceLg
            y: panel.theme.stageWellY
            width: stagePlate.width - 2 * panel.theme.spaceLg
            height: panel.theme.stageWellHeight
            radius: panel.theme.stageWellRadius
            color: panel.theme.stage
            border.color: panel.theme.wellEdge
            TrimStage {
                objectName: "trimStage"
                anchors.fill: parent
                anchors.margins: panel.theme.spaceXs
                theme: panel.theme
                pan: panel.pan
                stereoWidth: panel.stereoWidth
                mono: panel.mono
                swap: panel.swap
                invert: panel.invert
                solo: panel.solo
            }
            Text {
                x: panel.theme.stageNoteX
                y: panel.theme.stageNoteY
                text: "AS SET · NOT MEASURED"
                color: panel.theme.inkFaint
                font.pixelSize: panel.theme.stageNoteSize
                font.letterSpacing: panel.theme.stageNoteSpacing
            }
        }
        Kit.Knob {
            x: panel.theme.plateInset
            y: panel.theme.panRowY
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.pan"
            diameter: panel.theme.panKnobDiameter
            readout: true
            valueText: panel.panText(panel.pan)
        }
        Caption {
            x: panel.theme.lawX
            y: panel.theme.lawCaptionY
            text: "PAN LAW · CENTRE ALWAYS UNITY"
        }
        Kit.Segment {
            x: panel.theme.lawX
            y: panel.theme.lawSegmentY
            width: stagePlate.width - panel.theme.lawX - panel.theme.lawRight
            height: panel.theme.lawSegmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.law"
            labels: ["BALANCE", "−3 DB", "−4.5 DB", "−6 DB"]
        }
        Note {
            x: panel.theme.lawX
            y: panel.theme.lawNoteY
            width: stagePlate.width - panel.theme.lawX - panel.theme.lawRight
            text: ["Balance turns the far side down and leaves the near side alone: for stereo sources.",
                   "Constant power: the near side rises 3 dB at the edge, so a mono source keeps its loudness across the field.",
                   "Up 4.5 dB at the edge: between constant power and linear, the usual console compromise.",
                   "Linear: up 6 dB at the edge, so the two sides always sum to the centre's level in mono."][Math.max(0, Math.min(3, panel.revision >= 0 ? panel.controller.choice("trim.law") : 0))]
        }
        Kit.Knob {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.plateInset
            y: panel.theme.panRowY
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.width"
            diameter: panel.theme.panKnobDiameter
            readout: true
            inactive: panel.mono
        }
    }

    // ---- Row B: stereo, monitor, repair ---------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: panel.height - rowBY - margin

    Kit.Card {
        theme: panel.theme
        id: stereoPlate
        title: "STEREO"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.stereoPlateWidth
        height: panel.rowBHeight
        Kit.Toggle {
            x: panel.theme.plateInset
            y: panel.theme.toggleRowY
            width: panel.theme.stereoToggleWidth
            height: panel.theme.toggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.mono"
            onCaption: "SUMMED TO MONO"
            offCaption: "STEREO"
        }
        Kit.Toggle {
            x: panel.theme.swapToggleX
            y: panel.theme.toggleRowY
            width: stereoPlate.width - x - panel.theme.plateInset
            height: panel.theme.toggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.swap"
            onCaption: "LEFT AND RIGHT EXCHANGED"
            offCaption: "AS RECORDED"
        }
        Caption {
            x: panel.theme.plateInset
            y: panel.theme.polarityCaptionY
            text: "POLARITY"
        }
        // Two switches over one four-way choice: None, Left, Right, Both.
        Row {
            id: polarity
            objectName: "trimPolarity:trim.invert"
            x: panel.theme.plateInset
            y: panel.theme.polarityY
            spacing: panel.theme.spaceLg
            Repeater {
                model: [{ bit: 1, name: "L" }, { bit: 2, name: "R" }]
                delegate: Rectangle {
                    id: flip
                    required property var modelData
                    readonly property bool on: (panel.invert & modelData.bit) !== 0
                    readonly property color tone: modelData.bit === 1 ? panel.theme.left : panel.theme.right
                    objectName: "trimPolarity:" + modelData.name
                    width: panel.theme.polarityWidth
                    height: panel.theme.polarityHeight
                    radius: panel.theme.radiusLg
                    activeFocusOnTab: panel.controller.editable
                    opacity: panel.controller.editable ? 1 : panel.theme.switchDisabledOpacity
                    color: on ? Qt.rgba(panel.theme.invert.r, panel.theme.invert.g, panel.theme.invert.b, panel.theme.invertFillAlpha)
                        : flipArea.pressed ? panel.theme.button : panel.theme.buttonHi
                    border.color: activeFocus ? panel.theme.accent
                        : on ? Qt.rgba(panel.theme.invert.r, panel.theme.invert.g, panel.theme.invert.b, panel.theme.invertEdgeAlpha)
                        : panel.theme.seam
                    Accessible.role: Accessible.CheckBox
                    Kit.HoverTip { theme: panel.theme }
                    Accessible.name: "Invert " + (modelData.bit === 1 ? "left" : "right") + " polarity"
                    Accessible.checked: on
                    function press() {
                        panel.theme.lastTouched = "trim.invert"
                        panel.controller.set_choice("trim.invert", panel.invert ^ modelData.bit)
                    }
                    Keys.onSpacePressed: press()
                    Keys.onReturnPressed: press()
                    Text {
                        x: panel.theme.plateInset
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Ø"
                        color: flip.on ? panel.theme.invert : panel.theme.inkDim
                        font.pixelSize: panel.theme.polarityGlyphSize
                        font.bold: true
                    }
                    Column {
                        x: panel.theme.polarityTextX
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: panel.theme.polarityTextGap
                        Text {
                            text: flip.modelData.bit === 1 ? "LEFT" : "RIGHT"
                            color: flip.on ? panel.theme.ink : flip.tone
                            font.pixelSize: panel.theme.typeSmall
                            font.bold: true
                            font.letterSpacing: panel.theme.polarityNameSpacing
                        }
                        Text {
                            text: flip.on ? "INVERTED" : "AS RECORDED"
                            color: flip.on ? panel.theme.invert : panel.theme.inkFaint
                            font.pixelSize: panel.theme.typeMicro
                            font.letterSpacing: panel.theme.polarityStateSpacing
                        }
                    }
                    MouseArea {
                        id: flipArea
                        anchors.fill: parent
                        enabled: panel.controller.editable
                        onClicked: {
                            flip.forceActiveFocus()
                            flip.press()
                        }
                    }
                }
            }
        }
        Note {
            x: panel.theme.plateInset
            y: panel.theme.stereoNoteY
            width: stereoPlate.width - 2 * panel.theme.plateInset
            text: panel.invert === 3 ? "Both sides inverted: the whole signal flips, as a polarity switch on a desk."
                : panel.invert !== 0 ? "One side inverted: mono sums of the two will cancel. Use it to fix a reversed cable, or to check one."
                : "Flip a side's polarity, mono the pair, or swap the sides."
        }
    }

    Kit.Card {
        theme: panel.theme
        id: monitorPlate
        title: "MONITOR"
        note: panel.solo !== 0 ? "SOLO " + panel.soloNames[panel.solo] : "LISTENING TO EVERYTHING"
        noteColor: panel.solo !== 0 ? panel.theme.solo : panel.theme.inkFaint
        x: stereoPlate.x + stereoPlate.width + panel.gap
        y: panel.rowBY
        width: panel.theme.monitorPlateWidth
        height: panel.rowBHeight
        Caption {
            x: panel.theme.plateInset
            y: panel.theme.soloCaptionY
            text: "SOLO"
        }
        Kit.Segment {
            x: panel.theme.plateInset
            y: panel.theme.soloSegmentY
            width: monitorPlate.width - 2 * panel.theme.plateInset
            height: panel.theme.soloSegmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.solo"
            labels: ["OFF", "L", "R", "MID", "SIDE"]
        }
        Note {
            x: panel.theme.plateInset
            y: panel.theme.soloNoteY
            width: monitorPlate.width - 2 * panel.theme.plateInset
            text: ["Solo one part of the stereo signal to hear it alone, on both speakers.",
                   "The left channel alone, on both speakers.",
                   "The right channel alone, on both speakers.",
                   "The mid, (L + R) / 2: what a mono speaker plays.",
                   "The side, (L − R) / 2: the width alone. Reverb, doubled parts and phase trouble live here."][panel.solo]
        }
    }

    Kit.Card {
        theme: panel.theme
        id: repairPlate
        title: "REPAIR"
        x: monitorPlate.x + monitorPlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Kit.Toggle {
            x: panel.theme.plateInset
            y: panel.theme.toggleRowY
            width: repairPlate.width - 2 * panel.theme.plateInset
            height: panel.theme.toggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.dc"
            onCaption: "5 HZ HIGH-PASS, OFFSET REMOVED"
            offCaption: "OFF · DC PASSES"
        }
        Kit.Toggle {
            x: panel.theme.plateInset
            y: panel.theme.secondToggleY
            width: repairPlate.width - 2 * panel.theme.plateInset
            height: panel.theme.toggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "trim.rotate"
            onCaption: "ALLPASS AT 150 HZ: PEAKS LOWERED"
            offCaption: "OFF"
        }
        Note {
            x: panel.theme.plateInset
            y: panel.theme.repairNoteY
            width: repairPlate.width - 2 * panel.theme.plateInset
            text: "Phase rotation reshapes lopsided waveforms (voice, brass, kicks) for more headroom without changing how they sound."
        }
    }

    Timer {
        objectName: "trimTelemetryPoll"
        interval: panel.playing ? 33 : 250
        repeat: true
        running: panel.visible
        onTriggered: panel.live = panel.decode(panel.controller.telemetry())
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
