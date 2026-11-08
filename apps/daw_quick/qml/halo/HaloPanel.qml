pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Halo device panel: a retro-digital FM faceplate. Six operator cards
// show each operator's role in the current algorithm; the algorithm plate
// draws the routing and selects one of 32 with membrane keys; the LCD names
// the preset and reads back the last control touched. Every control reads
// and writes through `controller` (NativeSynthDeviceController for the Halo
// engine); no project or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    property HaloTheme theme: HaloTheme {}

    // Six operators in a row; 32 algorithms on a grid of eight columns.
    readonly property int operatorCount: 6
    readonly property int algorithmCount: 32
    readonly property int algorithmColumns: 8
    readonly property int algorithmRows: algorithmCount / algorithmColumns

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.sectionGap
    readonly property real cardY: theme.operatorTop
    readonly property real cardHeight: theme.operatorHeight
    readonly property real cardWidth: (width - 2 * margin - (operatorCount - 1) * theme.operatorGap) / operatorCount
    readonly property real lowerY: cardY + cardHeight + gap
    readonly property real lowerHeight: height - lowerY - theme.lowerBottomMargin
    readonly property real rowHeight: (lowerHeight - gap) / 2

    readonly property int algorithm: controller.revision >= 0 ? controller.choice("halo.algorithm") : 0
    readonly property var routing: controller.fm_algorithm(algorithm)
    // Algorithms 4 and 6 loop one operator's output into another's input.
    readonly property bool feedbackLoop: routing.feedback !== undefined && routing.feedback_from !== routing.feedback

    objectName: "haloPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.bodyTop }
        GradientStop { position: 0.5; color: panel.theme.body }
        GradientStop { position: 1; color: panel.theme.bodyBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Halo FM synthesizer"

    function isCarrier(op) {
        return routing.carriers !== undefined && routing.carriers[op - 1] === true
    }
    // The LCD's second window: what the last touched control is, and its value.
    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        return info.name === undefined ? "" : (info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || controller.revision < 0 ? "" : controller.display(id)
    }

    // ---- Header ---------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerTop
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Rectangle {
            id: ring
            y: panel.theme.logoTop
            width: panel.theme.logoSize
            height: panel.theme.logoSize
            radius: width / 2
            color: "transparent"
            border.color: panel.theme.teal
            border.width: panel.theme.logoStroke
            Rectangle {
                anchors.centerIn: parent
                width: panel.theme.logoDotSize
                height: panel.theme.logoDotSize
                radius: width / 2
                color: panel.theme.amber
            }
        }
        Text {
            id: wordmark
            x: ring.width + panel.theme.wordmarkGap
            y: 0
            text: "HALO"
            color: panel.theme.ink
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Black
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            x: wordmark.x + panel.theme.taglineIndent
            y: wordmark.y + wordmark.height - panel.theme.taglineLift
            text: "SIX-OPERATOR FM SYNTHESIS"
            color: panel.theme.amber
            font.pixelSize: panel.theme.typeCaption
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }

        Row {
            anchors.centerIn: parent
            spacing: panel.theme.spaceSm
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "haloPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: lcd
                objectName: "haloLcd"
                width: panel.theme.lcdWidth
                height: panel.theme.lcdHeight
                radius: panel.theme.radiusMd
                color: panel.theme.lcd
                border.color: activeFocus ? panel.theme.teal : panel.theme.lcdEdge
                border.width: activeFocus ? panel.theme.strokeBold : panel.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(lcd, panel.controller, panel.theme.teal)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                // Faint scan lines.
                Column {
                    anchors.fill: parent
                    anchors.margins: panel.theme.lcdScanInset
                    clip: true
                    Repeater {
                        model: panel.theme.lcdScanLines
                        delegate: Rectangle {
                            width: lcd.width - 2 * panel.theme.lcdScanInset
                            height: panel.theme.lcdScanHeight
                            color: index % 2 === 0 ? panel.theme.lcdScanLine : "transparent"
                            required property int index
                        }
                    }
                }
                Text {
                    x: panel.theme.lcdTextX
                    y: panel.theme.lcdTopLineY
                    text: panel.controller.preset_category.toUpperCase()
                    color: panel.theme.lcdDim
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                }
                Text {
                    objectName: "haloPresetName"
                    x: panel.theme.lcdTextX
                    y: panel.theme.lcdNameY
                    width: panel.theme.lcdNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.lcdText
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeTitle
                    font.bold: true
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.theme.lcdDividerX
                    y: panel.theme.lcdDividerInset
                    width: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.lcdDividerInset
                    color: panel.theme.lcdDim
                }
                Text {
                    objectName: "haloReadout"
                    x: panel.theme.lcdReadoutX
                    y: panel.theme.lcdTopLineY
                    width: lcd.width - x - panel.theme.lcdReadoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.lcdDim
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "haloReadoutValue"
                    x: panel.theme.lcdReadoutX
                    y: panel.theme.lcdReadoutValueY
                    width: lcd.width - x - panel.theme.lcdReadoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.lcdText
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "haloLcdAlgorithm"
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.lcdTagRight
                    y: panel.theme.lcdAlgorithmY
                    text: "ALG " + (panel.algorithm + 1 < 10 ? "0" : "") + (panel.algorithm + 1)
                    color: panel.theme.lcdAmber
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
                    font.bold: true
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.lcdTagRight
                    y: panel.theme.lcdEditedY
                    text: panel.controller.preset_modified ? "EDITED" : ""
                    color: panel.theme.lcdDim
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.lcdChevronRight
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.teal
                    font.pixelSize: panel.theme.typeBody
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: panel.controller.editable
                    onClicked: {
                        lcd.forceActiveFocus()
                        lcd.openPresets()
                    }
                }
            }
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "haloPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "haloStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // ---- Operators ----------------------------------------------------------------

    Repeater {
        model: panel.operatorCount
        delegate: HaloOperatorCard {
            required property int index
            x: panel.margin + index * (panel.cardWidth + panel.theme.operatorGap)
            y: panel.cardY
            width: panel.cardWidth
            height: panel.cardHeight
            controller: panel.controller
            theme: panel.theme
            menu: menu
            op: index + 1
            carrier: panel.isCarrier(index + 1)
            feedback: panel.routing.feedback === index
            loopFrom: panel.feedbackLoop && panel.routing.feedback === index ? panel.routing.feedback_from : -1
            loopInto: panel.feedbackLoop && panel.routing.feedback_from === index ? panel.routing.feedback : -1
        }
    }

    // ---- Algorithm -----------------------------------------------------------------

    Kit.Card {
        theme: panel.theme
        id: algorithmPlate
        title: "ALGORITHM"
        note: ""
        x: panel.margin
        y: panel.lowerY
        width: panel.theme.algorithmCardWidth
        height: panel.lowerHeight

        HaloAlgorithmView {
            x: panel.theme.spaceMd
            y: panel.theme.cardBodyTop
            width: parent.width - 2 * panel.theme.spaceMd
            height: panel.theme.algorithmViewHeight
            controller: panel.controller
            theme: panel.theme
            algorithm: panel.algorithm
        }

        Item {
            id: algorithmGrid
            objectName: "haloAlgorithmGrid"
            x: panel.theme.spaceMd
            y: panel.theme.algorithmGridTop
            width: parent.width - 2 * panel.theme.spaceMd
            height: panel.algorithmRows * panel.theme.algorithmKeyHeight
                + (panel.algorithmRows - 1) * panel.theme.spaceXs
            activeFocusOnTab: panel.controller.editable
            Accessible.role: Accessible.List
            Accessible.name: "Algorithm"
            Accessible.description: String(panel.algorithm + 1)
            function choose(index) {
                if (!panel.controller.editable)
                    return
                panel.controller.set_choice("halo.algorithm", Math.max(0, Math.min(panel.algorithmCount - 1, index)))
                panel.theme.lastTouched = "halo.algorithm"
            }
            Keys.onLeftPressed: choose(panel.algorithm - 1)
            Keys.onRightPressed: choose(panel.algorithm + 1)
            Keys.onUpPressed: choose(panel.algorithm - panel.algorithmColumns)
            Keys.onDownPressed: choose(panel.algorithm + panel.algorithmColumns)
            Keys.onDeletePressed: {
                panel.controller.reset_to_default("halo.algorithm")
                panel.theme.lastTouched = "halo.algorithm"
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -panel.theme.algorithmFocusOutset
                radius: panel.theme.radiusMd
                color: "transparent"
                border.color: algorithmGrid.activeFocus ? panel.theme.amber : "transparent"
            }
            Grid {
                columns: panel.algorithmColumns
                spacing: panel.theme.spaceXs
                Repeater {
                    model: panel.algorithmCount
                    delegate: Rectangle {
                        id: key
                        required property int index
                        readonly property bool on: index === panel.algorithm
                        objectName: "haloAlgorithm:" + (index + 1)
                        width: (algorithmGrid.width - (panel.algorithmColumns - 1) * panel.theme.spaceXs) / panel.algorithmColumns
                        height: panel.theme.algorithmKeyHeight
                        radius: panel.theme.radiusSm
                        gradient: Gradient {
                            GradientStop { position: 0; color: keyArea.pressed ? panel.theme.membraneLo : panel.theme.membraneHi }
                            GradientStop { position: 1; color: panel.theme.membraneLo }
                        }
                        border.color: key.on ? panel.theme.amber : panel.theme.knobRing
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: "Algorithm " + (index + 1)
                        Accessible.checked: key.on
                        Rectangle {
                            x: panel.theme.algorithmKeyLedX
                            anchors.verticalCenter: parent.verticalCenter
                            width: panel.theme.algorithmKeyLedSize
                            height: panel.theme.algorithmKeyLedSize
                            radius: width / 2
                            color: key.on ? panel.theme.amber : panel.theme.ledOff
                        }
                        Text {
                            anchors.centerIn: parent
                            anchors.horizontalCenterOffset: panel.theme.algorithmKeyTextOffset
                            text: String(key.index + 1)
                            color: key.on ? panel.theme.amber : panel.theme.inkDim
                            font.family: panel.theme.mono
                            font.pixelSize: panel.theme.typeBody
                            font.bold: true
                        }
                        MouseArea {
                            id: keyArea
                            anchors.fill: parent
                            enabled: panel.controller.editable
                            onClicked: {
                                algorithmGrid.forceActiveFocus()
                                algorithmGrid.choose(key.index)
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Global sections -------------------------------------------------------------

    component Cell: Item {
        id: cell
        required property string parameterId
        property color accent: panel.theme.teal
        property int diameter: panel.theme.knobDiameter
        property bool inactive: false
        property string label: ""
        width: panel.theme.cellWidth
        height: panel.theme.cellHeight
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            accent: cell.accent
            diameter: cell.diameter
            inactive: cell.inactive
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
        }
    }

    readonly property real rightX: algorithmPlate.x + algorithmPlate.width + gap

    Kit.Card {
        theme: panel.theme
        id: fm
        title: "FM"
        x: panel.rightX
        y: panel.lowerY
        width: panel.theme.fmCardWidth
        height: panel.rowHeight
        Row {
            x: panel.theme.fmRowX
            y: panel.theme.knobRowTop
            Cell {
                parameterId: "halo.feedback"
                accent: panel.theme.red
                diameter: panel.theme.heroDiameter
            }
            Cell {
                parameterId: "halo.brightness"
                accent: panel.theme.amber
                diameter: panel.theme.heroDiameter
            }
        }
        Text {
            x: panel.theme.cardPad
            y: fm.height - panel.theme.cardCaptionBottom
            objectName: "haloFeedbackCaption"
            text: panel.routing.feedback === undefined ? ""
                : panel.feedbackLoop ? "FEEDBACK " + (panel.routing.feedback_from + 1) + " → " + (panel.routing.feedback + 1)
                : "FEEDBACK ON OP " + (panel.routing.feedback + 1)
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.cardCaptionSpacing
        }
    }

    Kit.Card {
        theme: panel.theme
        id: filter
        title: "FILTER"
        note: ""
        x: fm.x + fm.width + panel.gap
        y: panel.lowerY
        width: panel.theme.filterCardWidth
        height: panel.rowHeight
        readonly property bool active: panel.controller.revision >= 0 && panel.controller.choice("halo.filter.type") > 0
        Kit.Dropdown {
            x: filter.width - width - panel.theme.cardPad
            y: panel.theme.cardDropdownY
            width: panel.theme.cardDropdownWidth
            height: panel.theme.compactControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.filter.type"
            menu: menu
            accent: panel.theme.amber
        }
        HaloFilterView {
            id: filterView
            x: panel.theme.cardPad
            y: panel.theme.controlRowTop
            width: panel.theme.filterViewWidth
            height: filter.height - y - panel.theme.cardPad
            controller: panel.controller
            theme: panel.theme
        }
        Grid {
            x: filterView.x + filterView.width + panel.theme.cardPad
            y: panel.theme.knobRowTop
            columns: 2
            columnSpacing: panel.theme.filterKnobGap
            Cell {
                parameterId: "halo.filter.cutoff"
                accent: panel.theme.amber
                inactive: !filter.active
            }
            Cell {
                parameterId: "halo.filter.resonance"
                accent: panel.theme.amber
                inactive: !filter.active
            }
            Cell {
                parameterId: "halo.filter.keytrack"
                accent: panel.theme.amber
                inactive: !filter.active
                label: "Keytrack"
            }
            Cell {
                parameterId: "halo.filter.velocity"
                accent: panel.theme.amber
                inactive: !filter.active
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: lfo
        title: "LFO"
        x: filter.x + filter.width + panel.gap
        y: panel.lowerY
        width: panel.width - panel.margin - x
        height: panel.rowHeight
        readonly property bool synced: panel.controller.revision >= 0 && panel.controller.choice("halo.lfo.sync") === 1
        Kit.Dropdown {
            x: lfo.width - width - panel.theme.cardPad
            y: panel.theme.cardDropdownY
            width: panel.theme.cardDropdownWidth
            height: panel.theme.compactControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.lfo.shape"
            menu: menu
        }
        Row {
            x: (lfo.width - width) / 2
            y: panel.theme.knobRowTop
            Cell {
                parameterId: "halo.lfo.rate"
                inactive: lfo.synced
            }
            Cell { parameterId: "halo.lfo.delay" }
            Cell { parameterId: "halo.lfo.pitch" }
            Cell { parameterId: "halo.lfo.amp" }
        }
        Row {
            x: panel.theme.cardPad
            y: lfo.height - panel.theme.cardFootRowBottom
            spacing: panel.theme.spaceMd
            Kit.Segment {
                width: panel.theme.lfoSyncWidth
                height: panel.theme.compactControlHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "halo.lfo.sync"
                accent: panel.theme.teal
                labels: ["FREE", "SYNC"]
            }
            Kit.Dropdown {
                width: panel.theme.lfoDivisionWidth
                height: panel.theme.compactControlHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "halo.lfo.division"
                menu: menu
                opacity: lfo.synced ? 1 : panel.theme.dropdownInactiveOpacity
            }
        }
    }

    readonly property real bottomY: lowerY + rowHeight + gap

    Kit.Card {
        theme: panel.theme
        id: voice
        title: "VOICE"
        note: "MODE · NEXT NOTE"
        x: panel.rightX
        y: panel.bottomY
        width: panel.theme.voiceCardWidth
        height: panel.rowHeight
        Kit.Segment {
            id: voiceMode
            x: panel.theme.cardPad
            y: panel.theme.controlRowTop
            width: panel.theme.voiceModeWidth
            height: panel.theme.rowControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.voice.mode"
            accent: panel.theme.teal
            labels: ["POLY", "MONO", "LEGATO"]
        }
        Kit.Stepper {
            id: unison
            x: voiceMode.x + voiceMode.width + panel.theme.cardPad
            y: panel.theme.controlRowTop
            width: panel.theme.unisonWidth
            height: panel.theme.rowControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.voice.unison"
            label: "Unison"
            plain: true
        }
        Kit.Stepper {
            x: unison.x + unison.width + panel.theme.spaceMd
            y: panel.theme.controlRowTop
            width: panel.theme.bendWidth
            height: panel.theme.rowControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.voice.bend"
            label: "Bend"
        }
        Row {
            x: panel.theme.cardPad
            y: panel.theme.lowerKnobRowTop
            spacing: panel.theme.voiceKnobGap
            Cell { parameterId: "halo.voice.detune" }
            Cell { parameterId: "halo.voice.spread" }
            Cell { parameterId: "halo.voice.glide" }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: effects
        title: "EFFECTS"
        x: voice.x + voice.width + panel.gap
        y: panel.bottomY
        width: panel.theme.effectsCardWidth
        height: panel.rowHeight
        readonly property bool synced: panel.controller.revision >= 0 && panel.controller.choice("halo.fx.delay_sync") === 1
        Kit.Segment {
            id: chorus
            x: panel.theme.cardPad
            y: panel.theme.controlRowTop
            width: panel.theme.chorusWidth
            height: panel.theme.rowControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.fx.chorus"
            accent: panel.theme.teal
            labels: ["OFF", "I", "II", "I+II"]
        }
        Kit.Segment {
            id: delaySync
            x: chorus.x + chorus.width + panel.theme.cardPad
            y: panel.theme.controlRowTop
            width: panel.theme.delaySyncWidth
            height: panel.theme.rowControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.fx.delay_sync"
            accent: panel.theme.teal
            labels: ["FREE", "SYNC"]
        }
        Kit.Dropdown {
            x: delaySync.x + delaySync.width + panel.theme.spaceMd
            y: panel.theme.controlRowTop
            width: panel.theme.cardDropdownWidth
            height: panel.theme.rowControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "halo.fx.delay_division"
            menu: menu
            opacity: effects.synced ? 1 : panel.theme.dropdownInactiveOpacity
        }
        Row {
            x: (effects.width - width) / 2
            y: panel.theme.lowerKnobRowTop
            spacing: panel.theme.effectsKnobGap
            Cell { parameterId: "halo.fx.ensemble" }
            Cell {
                parameterId: "halo.fx.delay"
                label: "Delay"
            }
            Cell {
                parameterId: "halo.fx.delay_time"
                inactive: effects.synced
            }
            Cell {
                parameterId: "halo.fx.delay_feedback"
                label: "Repeats"
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: output
        title: "PLAY · OUT"
        x: effects.x + effects.width + panel.gap
        y: panel.bottomY
        width: panel.width - panel.margin - x
        height: panel.rowHeight
        Grid {
            x: (output.width - width) / 2
            y: panel.theme.cardBodyTop
            columns: 2
            columnSpacing: panel.theme.spaceSm
            Cell {
                parameterId: "halo.perf.wheel"
                diameter: panel.theme.playDiameter
                height: panel.theme.playCellHeight
                label: "Wheel"
            }
            Cell {
                parameterId: "halo.perf.bend"
                diameter: panel.theme.playDiameter
                height: panel.theme.playCellHeight
                label: "Bend"
            }
            Cell {
                parameterId: "halo.perf.pressure"
                diameter: panel.theme.playDiameter
                height: panel.theme.playCellHeight
                label: "Pressure"
            }
            Cell {
                parameterId: "halo.out.level"
                accent: panel.theme.amber
                diameter: panel.theme.playDiameter
                height: panel.theme.playCellHeight
                label: "Output"
            }
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
