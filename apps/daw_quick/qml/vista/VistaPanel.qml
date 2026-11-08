pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Vista device panel, "glacier": a precision stereo imager in machined
// slate and steel, lettered in glacier white. The image leads: the
// vectorscope and correlation meter on the left, the side width of each
// band over a frequency axis at the centre, the overall width on the right;
// below, mono bass, stereoize, safe width, the field and output. Every
// control reads and writes through `controller` (NativeSynthDeviceController
// for Vista); no project or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037, layout in vista.hpp), polled while visible: fast
    // while the imager publishes, slowly while it does not.
    property var live: panel.idle()
    property VistaTheme theme: VistaTheme {}

    readonly property real margin: theme.panelMargin
    readonly property real gap: theme.panelGap
    readonly property int revision: controller.revision

    // vista.hpp: correlation in/out, width in/out (dB), safe (dB), four
    // band correlations, four band widths (dB), then 256 (mid, side) pairs.
    readonly property int scopeAt: 13
    readonly property int scopePoints: 256
    // Side-to-mid amplitude of a half-correlated reference source, and the
    // preview ratio shown before the controller has state.
    readonly property real referenceRatio: 0.577
    readonly property real previewFallback: 0.58
    // Telemetry polling, in milliseconds: fast while live, slow while idle.
    readonly property int livePollMs: 33
    readonly property int idlePollMs: 250

    objectName: "vistaPanel"
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
    Accessible.name: "Vista stereo imager"

    function idle() {
        return { playing: false, correlationIn: 1, correlationOut: 1, widthIn: -120, widthOut: -120,
                 safeDb: 0, bandCorrelation: [1, 1, 1, 1], bandWidthDb: [-120, -120, -120, -120],
                 points: [] }
    }
    function decode(state) {
        const frame = state.frame
        if (frame === undefined || frame.length < scopeAt)
            return idle()
        return {
            playing: true,
            correlationIn: frame[0],
            correlationOut: frame[1],
            widthIn: frame[2],
            widthOut: frame[3],
            safeDb: frame[4],
            bandCorrelation: frame.slice(5, 9),
            bandWidthDb: frame.slice(9, 13),
            points: frame.slice(scopeAt, scopeAt + 2 * scopePoints)
        }
    }
    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group === info.name ? info.name : info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || revision < 0 ? "" : controller.display(id)
    }
    // Levels at or below this read as silence.
    readonly property real silentDb: -99
    function dbText(db) {
        return Format.signed_decibels(db <= silentDb ? -Infinity : db)
    }
    // The preview cloud's side-to-mid ratio: a half-correlated reference
    // source (side 0.577 of mid) at the overall and average band widths,
    // with stereoize's synthesised side added in quadrature.
    readonly property real previewRatio: {
        if (revision < 0)
            return previewFallback
        const count = controller.choice("vista.bands") + 1
        let sum = 0
        for (let b = 1; b <= count; ++b)
            sum += controller.value("vista.width_" + b)
        const width = controller.value("vista.width") * sum / count
        const amount = controller.value("vista.stereoize")
        const synthesised = Math.sqrt(amount / (2 - amount))
        return Math.sqrt(Math.pow(referenceRatio * width, 2) + synthesised * synthesised)
    }

    // A machined plate: a fine bevel along the top, the title in spaced caps.

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.knobDiameter
        property string label: ""
        property bool readout: false
        width: panel.theme.cellWidth
        height: diameter + (readout ? panel.theme.cellReadoutPad : panel.theme.cellPad)
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            diameter: cell.diameter
            readout: cell.readout
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
        }
    }

    // A small caption over a control.
    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // ---- Header ---------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.wordmarkY
            text: "VISTA"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.wordmarkIndent
            y: wordmark.y + wordmark.height + panel.theme.wordmarkUnderlineGap
            // Short of the trailing letter spacing.
            width: wordmark.implicitWidth - panel.theme.wordmarkSpacing
            height: panel.theme.hairline
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.accent }
                GradientStop { position: 1; color: panel.theme.wordmarkFade }
            }
        }
        Text {
            x: wordmark.x + panel.theme.wordmarkIndent
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "STEREO IMAGER"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.subtitleSpacing
        }

        // Preset browser and readout.
        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "vistaPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "vistaPresetDisplay"
                width: panel.theme.presetDisplayWidth
                height: panel.theme.presetDisplayHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.presetFocusStroke : panel.theme.hairline
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
                    objectName: "vistaPresetName"
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
                    objectName: "vistaReadout"
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
                    objectName: "vistaReadoutValue"
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
                objectName: "vistaPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "vistaStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- The image ------------------------------------------------------------------

    readonly property real rowAY: theme.imageRowY
    readonly property real rowAHeight: theme.imageRowHeight

    Kit.Card {
        theme: panel.theme
        id: scopePlate
        title: "VECTORSCOPE"
        note: panel.live.playing === true ? "LIVE" : "PREVIEW · SETTINGS"
        noteColor: panel.live.playing === true ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.scopeCardWidth
        height: panel.rowAHeight
        VistaScope {
            id: scope
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.scopeY
            width: panel.theme.scopeSize
            height: panel.theme.scopeSize
            theme: panel.theme
            live: panel.live.playing === true
            points: panel.live.points
            previewRatio: panel.previewRatio
        }
        Caption {
            x: panel.theme.cardInset
            y: panel.theme.correlationCaptionY
            text: "CORRELATION"
        }
        Text {
            objectName: "vistaCorrelationReadout"
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.cardInset
            y: panel.theme.correlationReadoutY
            text: panel.live.playing === true
                ? "IN " + Format.signed_number(panel.live.correlationIn) + "   OUT " + Format.signed_number(panel.live.correlationOut)
                : "—"
            color: panel.theme.glacier
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeSmall
        }
        VistaCorrelation {
            id: correlation
            x: 0
            y: panel.theme.correlationY
            width: scopePlate.width
            height: panel.theme.correlationHeight
            theme: panel.theme
            live: panel.live.playing === true
            inValue: panel.live.correlationIn
            outValue: panel.live.correlationOut
            floorOn: panel.revision >= 0 && panel.controller.choice("vista.safe") === 1
            floor: panel.revision >= 0 ? panel.controller.value("vista.safe_floor") : 0
        }
    }

    Kit.Card {
        theme: panel.theme
        id: bandsPlate
        title: "WIDTH BY BAND"
        note: "SIDE ONLY · THE MID IS NEVER TOUCHED"
        x: scopePlate.x + scopePlate.width + panel.gap
        y: panel.rowAY
        width: panel.theme.bandsCardWidth
        height: panel.rowAHeight
        Caption {
            anchors.right: bandCount.left
            anchors.rightMargin: panel.theme.spaceMd
            y: panel.theme.bandCountCaptionY
            text: "BANDS"
        }
        Kit.Segment {
            id: bandCount
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.cardInset
            y: panel.theme.bandCountY
            width: panel.theme.bandCountWidth
            height: panel.theme.bandCountHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "vista.bands"
        }
        VistaBands {
            id: bands
            x: panel.theme.bandsX
            y: panel.theme.bandsY
            width: bandsPlate.width - 2 * panel.theme.bandsX
            height: bandsPlate.height - panel.theme.bandsY - panel.theme.bandsFoot
            controller: panel.controller
            theme: panel.theme
            live: panel.live.playing === true
            bandCorrelation: panel.live.bandCorrelation
            bandWidthDb: panel.live.bandWidthDb
        }
    }

    Kit.Card {
        theme: panel.theme
        id: image
        title: "IMAGE"
        x: bandsPlate.x + bandsPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        Cell {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.widthKnobY
            width: panel.theme.widthCellWidth
            parameterId: "vista.width"
            label: "Width"
            diameter: panel.theme.widthKnobDiameter
            readout: true
        }
        // Live, the image spread follows the output's side-to-mid level;
        // otherwise the settings' preview.
        VistaStage {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.stageY
            width: image.width - 2 * panel.theme.stageSideInset
            height: panel.theme.stageCardHeight
            theme: panel.theme
            ratio: panel.live.playing === true ? Math.pow(10, panel.live.widthOut / 20) : panel.previewRatio
            rotation: panel.revision >= 0 ? panel.controller.value("vista.rotation") : 0
            balance: panel.revision >= 0 ? panel.controller.value("vista.balance") : 0
        }
        Column {
            x: panel.theme.imageInset
            y: panel.theme.readoutListY
            width: image.width - 2 * panel.theme.imageInset
            spacing: panel.theme.spaceXs
            component Row2: Item {
                id: row2
                required property string name
                required property string value
                property string objectId: ""
                property color tone: panel.theme.glacier
                width: parent.width
                height: panel.theme.readoutRowHeight
                Text {
                    text: row2.name
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutRowSpacing
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    objectName: row2.objectId
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: row2.value
                    color: row2.tone
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeBody
                }
            }
            Row2 {
                name: "SIDE / MID IN"
                value: panel.live.playing === true ? panel.dbText(panel.live.widthIn) : "—"
                objectId: "vistaWidthIn"
                tone: panel.theme.inkDim
            }
            Row2 {
                name: "SIDE / MID OUT"
                value: panel.live.playing === true ? panel.dbText(panel.live.widthOut) : "—"
                objectId: "vistaWidthOut"
            }
            Row2 {
                name: "SAFE WIDTH"
                value: panel.live.playing === true && panel.controller.choice("vista.safe") === 1
                    ? (panel.live.safeDb < -0.05 ? panel.dbText(panel.live.safeDb) : "HOLDING")
                    : panel.controller.choice("vista.safe") === 1 ? "ARMED" : "OFF"
                objectId: "vistaSafeReadout"
                tone: panel.live.playing === true && panel.live.safeDb < -0.05 ? panel.theme.warning
                                                                               : panel.theme.glacier
            }
        }
        // The promise: whatever the widths, the mono sum is the input's.
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: image.height - panel.theme.promiseRise
            width: image.width - 2 * panel.theme.imageInset
            height: panel.theme.promiseHeight
            radius: height / 2
            color: "transparent"
            border.color: panel.theme.wellEdge
            Text {
                anchors.centerIn: parent
                text: "MONO SUM UNCHANGED"
                color: panel.theme.positive
                font.pixelSize: panel.theme.typeCaption
                font.bold: true
                font.letterSpacing: panel.theme.promiseSpacing
            }
        }
    }

    // ---- Tools ----------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: panel.height - rowBY - margin

    Kit.Card {
        theme: panel.theme
        id: bass
        title: "MONO BASS"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.bassCardWidth
        height: panel.rowBHeight
        Kit.Toggle {
            x: panel.theme.toggleX
            y: panel.theme.toggleY
            width: bass.width - 2 * panel.theme.toggleX
            controller: panel.controller
            theme: panel.theme
            parameterId: "vista.mono_bass"
            label: "Mono Bass"
            onCaption: "SIDE REMOVED BELOW"
            offCaption: "FULL-RANGE SIDE"
        }
        Cell {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.lowerKnobY
            parameterId: "vista.mono_freq"
            label: "Below"
            diameter: panel.theme.lowerKnobDiameter
            readout: true
        }
    }

    Kit.Card {
        theme: panel.theme
        id: stereoize
        readonly property bool haas: panel.revision >= 0 && panel.controller.choice("vista.stereoize_mode") === 1
        title: "STEREOIZE"
        note: "ABOVE 300 Hz · SIDE ONLY"
        x: bass.x + bass.width + panel.gap
        y: panel.rowBY
        width: panel.theme.stereoizeCardWidth
        height: panel.rowBHeight
        Cell {
            x: panel.theme.amountX
            y: panel.theme.knobRowY
            width: panel.theme.amountCellWidth
            parameterId: "vista.stereoize"
            label: "Amount"
            diameter: panel.theme.amountDiameter
            readout: true
        }
        Caption {
            x: panel.theme.modeX
            y: panel.theme.modeCaptionY
            text: "MODE"
        }
        Kit.Segment {
            x: panel.theme.modeX
            y: panel.theme.modeY
            width: stereoize.width - panel.theme.modeX - panel.theme.cardInset
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "vista.stereoize_mode"
        }
        Cell {
            x: panel.theme.modeX + (stereoize.width - panel.theme.modeX - panel.theme.cardInset - width) / 2
            y: panel.theme.haasY
            visible: stereoize.haas
            parameterId: "vista.haas_time"
            label: "Time"
            diameter: panel.theme.haasDiameter
        }
        Text {
            visible: !stereoize.haas
            x: panel.theme.modeX
            y: panel.theme.stereoizeNoteY
            width: stereoize.width - panel.theme.modeX - panel.theme.cardInset
            text: "Six allpasses of prime lengths spread the mid into a diffuse side."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        theme: panel.theme
        id: safe
        title: "SAFE WIDTH"
        x: stereoize.x + stereoize.width + panel.gap
        y: panel.rowBY
        width: panel.theme.safeCardWidth
        height: panel.rowBHeight
        Kit.Toggle {
            x: panel.theme.toggleX
            y: panel.theme.toggleY
            width: safe.width - 2 * panel.theme.toggleX
            controller: panel.controller
            theme: panel.theme
            parameterId: "vista.safe"
            label: "Safe Width"
            lit: panel.theme.warning
            onCaption: "HOLDING THE FLOOR"
            offCaption: "NO CORRELATION LIMIT"
        }
        Cell {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.lowerKnobY
            parameterId: "vista.safe_floor"
            label: "Floor"
            diameter: panel.theme.lowerKnobDiameter
            readout: true
        }
    }

    Kit.Card {
        theme: panel.theme
        id: field
        title: "FIELD"
        x: safe.x + safe.width + panel.gap
        y: panel.rowBY
        width: panel.theme.fieldCardWidth
        height: panel.rowBHeight
        Row {
            x: panel.theme.fieldRowX
            y: panel.theme.knobRowY
            Cell { parameterId: "vista.rotation"; diameter: panel.theme.fieldDiameter; readout: true }
            Cell { parameterId: "vista.balance"; diameter: panel.theme.fieldDiameter; readout: true }
        }
        Kit.Toggle {
            x: panel.theme.fieldControlsX
            y: panel.theme.toggleY
            width: field.width - panel.theme.fieldControlsX - panel.theme.cardInset
            controller: panel.controller
            theme: panel.theme
            parameterId: "vista.swap"
            label: "Swap L / R"
            onCaption: "CHANNELS EXCHANGED"
            offCaption: ""
        }
        Caption {
            x: panel.theme.fieldControlsX
            y: panel.theme.invertCaptionY
            text: "INVERT POLARITY"
        }
        Kit.Segment {
            x: panel.theme.fieldControlsX
            y: panel.theme.invertY
            width: field.width - panel.theme.fieldControlsX - panel.theme.cardInset
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "vista.invert"
            labels: ["OFF", "L", "R", "L + R"]
        }
    }

    Kit.Card {
        theme: panel.theme
        id: output
        title: "OUTPUT"
        x: field.x + field.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Cell {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.knobRowY
            parameterId: "vista.output"
            label: "Output"
            diameter: panel.theme.outputDiameter
            readout: true
        }
    }

    Timer {
        objectName: "vistaTelemetryPoll"
        interval: panel.live.playing === true ? panel.livePollMs : panel.idlePollMs
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
