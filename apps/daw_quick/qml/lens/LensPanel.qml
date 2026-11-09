pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Lens device panel: a precision optical instrument in night blue. The
// spectrum leads, its first curve glowing over a filled gradient with the
// second curve, peak hold, sidechain overlay and difference over it, and the
// strongest resonances marked with their notes; beside it the resonance
// table; under it a scrolling spectrogram on the same axis and the reading;
// below, every control, Freeze given its own plate. Every control reads and
// writes through `controller` (NativeSynthDeviceController for Lens); the
// panel itself only keeps how it draws (dB range, ceiling, the difference
// curve). With nothing playing it shows a typical mix through the settings.
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037, layout in lens.hpp), polled while visible.
    property var live: panel.idle()
    property LensTheme theme: LensTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    // A resonance this far under the strongest shows an empty bar; every
    // bar keeps a sliver.
    readonly property real resonanceBarRangeDb: 30
    readonly property real resonanceBarFloor: 0.05
    // A resonance within this many cents of its note reads in tune.
    readonly property int inTuneCents: 10
    readonly property int revision: controller.revision

    // lens.hpp: four curves of 400 points (A, B, overlay, A's hold), eight
    // resonances as (hz, dB), then the count of analyses.
    readonly property int points: 400
    readonly property int peakCount: 8
    readonly property int bAt: points
    readonly property int overlayAt: 2 * points
    readonly property int holdAt: 3 * points
    readonly property int peaksAt: 4 * points
    readonly property int hopsAt: peaksAt + 2 * peakCount
    readonly property int frameSize: hopsAt + 1

    // How the panel draws: range and ceiling of the dB axis, the difference.
    property int rangeDb: 90
    property real ceilingDb: 0
    property bool showDifference: true
    property real lastHops: -1

    readonly property bool playing: live.playing === true
    readonly property var preview: revision >= 0 ? panel.previewState() : panel.idle()
    readonly property var shown: playing ? live : preview
    readonly property int view: revision >= 0 ? controller.choice("lens.view") : 0
    readonly property bool pairView: view >= 4
    readonly property bool overlayOn: revision >= 0 && controller.choice("lens.overlay") === 1
    readonly property bool holdOn: revision >= 0 && controller.choice("lens.hold") === 1
    readonly property bool frozen: revision >= 0 && controller.choice("lens.freeze") === 1
    readonly property var viewNames: [["MID", ""], ["SIDE", ""], ["LEFT", ""], ["RIGHT", ""],
                                      ["MID", "SIDE"], ["LEFT", "RIGHT"]]

    objectName: "lensPanel"
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
    Accessible.name: "Lens spectrum analyser"

    function pointHz(j) {
        return 20 * Math.pow(1000, j / (points - 1))
    }
    function idle() {
        return { playing: false, a: [], b: [], overlay: [], hold: [], peaks: [], hops: 0 }
    }
    function decode(state) {
        const frame = state.frame
        if (frame === undefined || frame.length < frameSize)
            return idle()
        const peaks = []
        for (let k = 0; k < peakCount; ++k) {
            const hz = frame[peaksAt + 2 * k]
            if (hz > 0)
                peaks.push({ hz: hz, db: frame[peaksAt + 2 * k + 1] })
        }
        return {
            playing: true,
            a: frame.slice(0, points),
            b: frame.slice(bAt, bAt + points),
            overlay: frame.slice(overlayAt, overlayAt + points),
            hold: frame.slice(holdAt, holdAt + points),
            peaks: peaks,
            hops: frame[hopsAt]
        }
    }
    function poll() {
        live = decode(controller.telemetry())
        // A new analysis adds a spectrogram row; a frozen or silent Lens adds
        // none.
        if (live.playing && live.hops !== lastHops) {
            lastHops = live.hops
            gram.push(live.a)
        } else if (!live.playing) {
            lastHops = -1
        }
    }

    // ---- The preview: a typical mix through the view and tilt ------------------

    function previewState() {
        const tilt = [0, 1.5, 3][controller.choice("lens.tilt")]
        const a = []
        const b = []
        const hold = []
        for (let j = 0; j < points; ++j) {
            const hz = pointHz(j)
            const octaves = Math.log2(hz / 1000)
            // A mix in band power: a kick around 55 Hz, a broad low-mid hump,
            // a gentle fall into the air.
            const kick = -14 - 22 * Math.pow(Math.log2(hz / 55), 2)
            const body = -22 - 1.5 * octaves - 0.6 * octaves * octaves
            const air = hz > 14000 ? -6 * Math.log2(hz / 14000) * 3 : 0
            const level = 10 * Math.log10(Math.pow(10, kick / 10) + Math.pow(10, body / 10)) + air
            const ripple = 1.6 * Math.sin(j * 0.37) + 0.9 * Math.sin(j * 1.13)
            a.push(level + ripple + tilt * octaves)
            b.push(level - 9 - 0.8 * Math.max(0, -octaves) * 3 + ripple * 0.7 + tilt * octaves)
            hold.push(level + ripple + 3.5 + tilt * octaves)
        }
        return { playing: false, a: a, b: pairView ? b : [], overlay: [], hold: hold, peaks: [], hops: 0 }
    }

    // "A1 +12c 55.4 Hz −8.1 dB"
    function resonanceText(peak) {
        return spectrum.noteText(peak.hz) + " " + spectrum.hzText(peak.hz) + " " + spectrum.dbText(peak.db)
    }

    // A glass plate: a fine highlight along the top, the title in spaced caps.

    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // A small glass button for the panel's own drawing settings.
    component Button: Rectangle {
        id: button
        required property string label
        property bool on: false
        signal pressed()
        // At least the kit's least hit size each way.
        width: Math.max(panel.theme.minHitSize, buttonText.implicitWidth + 2 * panel.theme.spaceMd)
        height: panel.theme.buttonHeight
        radius: height / 2
        color: on ? Qt.rgba(panel.theme.accent.r, panel.theme.accent.g, panel.theme.accent.b, panel.theme.buttonOnFillAlpha)
            : buttonArea.pressed ? panel.theme.button : panel.theme.buttonHi
        border.color: activeFocus ? panel.theme.accent
            : on ? Qt.rgba(panel.theme.accent.r, panel.theme.accent.g, panel.theme.accent.b, panel.theme.buttonOnEdgeAlpha)
            : buttonArea.containsMouse ? panel.theme.wellEdge : panel.theme.seam
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Kit.HoverTip { theme: panel.theme }
        Accessible.name: label
        Accessible.onPressAction: button.pressed()
        Keys.onReturnPressed: button.pressed()
        Keys.onSpacePressed: button.pressed()
        Text {
            id: buttonText
            anchors.centerIn: parent
            text: button.label
            color: button.on ? panel.theme.ink : panel.theme.inkDim
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeCaption
            font.bold: button.on
        }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.pressed()
        }
    }

    // ---- Header ---------------------------------------------------------------------

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
                    objectName: "lensWordmark"
                    text: "LENS"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                // A spectrum's sweep under the name.
                Rectangle {
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.hairline
                    width: wordmark.implicitWidth - panel.theme.wordmarkRuleTrim
                    height: panel.theme.strokeBold
                    radius: panel.theme.strokeBold / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: panel.theme.primaryDeep }
                        GradientStop { position: panel.theme.wordmarkSweepStop; color: panel.theme.primary }
                        GradientStop { position: 1; color: panel.theme.glacier }
                    }
                }
                Text {
                    id: subtitle
                    objectName: "lensSubtitle"
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "SPECTRUM ANALYSER"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }
    }

    // ---- The spectrum and its resonances ---------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight
    readonly property real sideWidth: theme.sideWidth

    Kit.Card {
        theme: panel.theme
        id: spectrumPlate
        title: "SPECTRUM"
        note: panel.frozen ? "FROZEN" : panel.playing ? "" : "A TYPICAL MIX THROUGH THESE SETTINGS"
        noteColor: panel.frozen ? panel.theme.freeze : panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.width - 2 * panel.margin - panel.gap - panel.sideWidth
        height: panel.rowAHeight

        // The key and the drawing settings, top right.
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spaceLg
            y: panel.theme.toolbarY
            spacing: panel.theme.toolbarSpacing
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.spaceLg
                Repeater {
                    model: {
                        const names = panel.viewNames[panel.view]
                        const keys = [[names[0], 0]]
                        if (panel.pairView)
                            keys.push([names[1], 1])
                        if (panel.holdOn)
                            keys.push(["HOLD", 2])
                        if (panel.overlayOn)
                            keys.push(["SIDECHAIN", 3])
                        if (panel.overlayOn && panel.showDifference)
                            keys.push(["Δ", 4])
                        return keys
                    }
                    delegate: Row {
                        id: key
                        required property var modelData
                        spacing: panel.theme.keyGap
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: panel.theme.keySwatchWidth
                            height: key.modelData[1] === 2 ? panel.theme.hairline : panel.theme.strokeBold
                            radius: panel.theme.strokeBold / 2
                            color: [panel.theme.primary, panel.theme.secondary, panel.theme.hold,
                                    panel.theme.overlay, panel.theme.difference][key.modelData[1]]
                        }
                        Text {
                            text: key.modelData[0]
                            color: panel.theme.inkFaint
                            font.pixelSize: panel.theme.typeMicro
                            font.bold: true
                            font.letterSpacing: panel.theme.captionTightSpacing
                        }
                    }
                }
            }
            Rectangle {
                width: panel.theme.hairline
                height: panel.theme.toolbarDividerHeight
                color: panel.theme.seam
            }
            Row {
                objectName: "lensRange"
                spacing: panel.theme.buttonSpacing
                Repeater {
                    model: [60, 90, 120]
                    delegate: Button {
                        required property int modelData
                        objectName: "lensRange:" + modelData
                        label: Format.decibels(modelData, 0)
                        on: panel.rangeDb === modelData
                        onPressed: panel.rangeDb = modelData
                    }
                }
            }
            Row {
                spacing: panel.theme.buttonSpacing
                Button {
                    objectName: "lensCeilingDown"
                    label: "−"
                    onPressed: panel.ceilingDb = Math.max(-24, panel.ceilingDb - 6)
                }
                Text {
                    objectName: "lensCeiling"
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.ceilingTextWidth
                    horizontalAlignment: Text.AlignHCenter
                    text: "TOP " + Format.signed_number(panel.ceilingDb, 0)
                    color: panel.theme.inkDim
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                }
                Button {
                    objectName: "lensCeilingUp"
                    label: "+"
                    onPressed: panel.ceilingDb = Math.min(12, panel.ceilingDb + 6)
                }
            }
        }

        LensSpectrum {
            id: spectrum
            x: panel.theme.spaceXs
            y: panel.theme.spectrumY
            width: spectrumPlate.width - 2 * panel.theme.spaceXs
            height: spectrumPlate.height - panel.theme.spectrumY - panel.theme.spaceXs
            theme: panel.theme
            live: panel.playing
            curveA: panel.shown.a
            curveB: panel.pairView ? panel.shown.b : []
            overlay: panel.overlayOn ? panel.shown.overlay : []
            hold: panel.holdOn ? panel.shown.hold : []
            peaks: panel.shown.peaks
            showB: panel.pairView
            showOverlay: panel.overlayOn
            showHold: panel.holdOn
            showDifference: panel.overlayOn && panel.showDifference
            topDb: panel.ceilingDb
            rangeDb: panel.rangeDb
        }
        // Frost over a frozen picture.
        Rectangle {
            objectName: "lensFrost"
            visible: panel.frozen
            x: spectrum.x + spectrum.px
            y: spectrum.y + spectrum.py
            width: spectrum.pw
            height: spectrum.ph
            color: panel.theme.frostFill
            border.color: panel.theme.frostEdge
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: panel.theme.spaceMd
                text: "❄  FROZEN"
                color: panel.theme.freeze
                font.pixelSize: panel.theme.typeSmall
                font.bold: true
                font.letterSpacing: panel.theme.bannerSpacing
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: resonancePlate
        title: "RESONANCES"
        note: panel.shown.peaks.length > 0 ? "STRONGEST FIRST" : ""
        x: spectrumPlate.x + spectrumPlate.width + panel.gap
        y: panel.rowAY
        width: panel.sideWidth
        height: panel.rowAHeight
        Row {
            x: panel.theme.cardInset
            y: panel.theme.tableHeaderY
            spacing: 0
            Repeater {
                model: [["NOTE", panel.theme.noteColumnWidth], ["FREQUENCY", panel.theme.frequencyColumnWidth],
                        ["LEVEL", panel.theme.noteColumnWidth]]
                delegate: Caption {
                    required property var modelData
                    width: modelData[1]
                    text: modelData[0]
                }
            }
        }
        Column {
            x: panel.theme.tableX
            y: panel.theme.tableY
            width: resonancePlate.width - 2 * panel.theme.tableX
            spacing: panel.theme.tableSpacing
            Repeater {
                model: panel.peakCount
                delegate: Rectangle {
                    id: rowItem
                    required property int index
                    readonly property var peak: panel.shown.peaks[index]
                    readonly property bool present: peak !== undefined
                    objectName: "lensResonance:" + index
                    property string text: present ? panel.resonanceText(peak) : ""
                    width: parent.width
                    height: panel.theme.tableRowHeight
                    radius: panel.theme.radiusMd
                    color: index === 0 && present ? Qt.rgba(panel.theme.resonance.r, panel.theme.resonance.g,
                                                            panel.theme.resonance.b, panel.theme.strongestWashAlpha)
                                                  : panel.theme.well
                    border.color: panel.theme.seam
                    Text {
                        x: panel.theme.spaceSm
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.rankWidth
                        text: rowItem.present ? String(rowItem.index + 1) : ""
                        color: panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeCaption
                    }
                    Text {
                        x: panel.theme.noteX
                        y: panel.theme.noteNameY
                        text: rowItem.present ? spectrum.note(rowItem.peak.hz).name : "—"
                        color: rowItem.present ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeSubtitle
                    }
                    Text {
                        x: panel.theme.noteX
                        y: panel.theme.noteCentsY
                        text: rowItem.present ? spectrum.centsText(spectrum.note(rowItem.peak.hz).cents) : ""
                        color: rowItem.present && Math.abs(spectrum.note(rowItem.peak.hz).cents) <= panel.inTuneCents
                            ? panel.theme.live : panel.theme.inkDim
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeCaption
                    }
                    Text {
                        x: panel.theme.frequencyX
                        anchors.verticalCenter: parent.verticalCenter
                        text: rowItem.present ? spectrum.hzText(rowItem.peak.hz) : ""
                        color: panel.theme.ink
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeBody
                    }
                    Text {
                        x: panel.theme.levelX
                        anchors.verticalCenter: parent.verticalCenter
                        text: rowItem.present ? spectrum.dbText(rowItem.peak.db) : ""
                        color: panel.theme.resonance
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeBody
                    }
                    // How loud against the strongest.
                    Rectangle {
                        visible: rowItem.present
                        x: panel.theme.levelBarX
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.levelBarWidth
                        height: panel.theme.levelBarHeight
                        radius: height / 2
                        color: panel.theme.track
                        Rectangle {
                            width: rowItem.present && panel.shown.peaks.length > 0
                                ? parent.width * Math.max(panel.resonanceBarFloor,
                                    Math.min(1, 1 + (rowItem.peak.db - panel.shown.peaks[0].db) / panel.resonanceBarRangeDb))
                                : 0
                            height: parent.height
                            radius: height / 2
                            color: panel.theme.resonance
                        }
                    }
                }
            }
        }
        Text {
            x: panel.theme.cardInset
            y: resonancePlate.height - panel.theme.noteBottom
            width: resonancePlate.width - 2 * panel.theme.cardInset
            text: "Notes in equal temperament, A4 = 440 Hz; green within 10 ct."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            wrapMode: Text.WordWrap
        }
    }

    // ---- Spectrogram and reading ------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: theme.rowBHeight

    Kit.Card {
        theme: panel.theme
        id: gramPlate
        title: "SPECTROGRAM"
        note: panel.frozen ? "PAUSED WHILE FROZEN" : panel.playing ? "NEWEST AT THE TOP" : ""
        noteColor: panel.frozen ? panel.theme.freeze : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowBY
        width: spectrumPlate.width
        height: panel.rowBHeight
        LensSpectrogram {
            id: gram
            x: spectrum.x
            y: panel.theme.gramY
            width: spectrum.width
            height: gramPlate.height - panel.theme.gramY - panel.theme.spaceMd
            theme: panel.theme
            plotX: spectrum.px
            plotWidth: spectrum.pw
            // Every row was measured from the running device, frozen or not.
            live: true
            topDb: panel.ceilingDb
            rangeDb: panel.rangeDb
        }
        Text {
            visible: gram.rows === 0
            x: gram.x + spectrum.px + panel.theme.spaceLg
            y: gram.y + gram.height / 2 - height / 2
            text: "PLAY SOMETHING TO PAINT THE SPECTROGRAM"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            font.bold: true
            font.letterSpacing: panel.theme.bannerSpacing
        }
    }

    Kit.Card {
        theme: panel.theme
        id: readingPlate
        title: "READING"
        x: resonancePlate.x
        y: panel.rowBY
        width: panel.sideWidth
        height: panel.rowBHeight
        // The spectrogram's own colour scale, floor to ceiling.
        Row {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Repeater {
                model: gram.colours
                delegate: Rectangle {
                    required property string modelData
                    width: (readingPlate.width - 2 * panel.theme.cardInset) / gram.levels
                    height: panel.theme.colourScaleHeight
                    color: modelData
                }
            }
        }
        Text {
            x: panel.theme.cardInset
            y: panel.theme.colourScaleLabelY
            text: Format.decibels(panel.ceilingDb - panel.rangeDb, 0)
            color: panel.theme.gridText
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeMicro
        }
        Text {
            x: readingPlate.width - panel.theme.cardInset - width
            y: panel.theme.colourScaleLabelY
            text: Format.signed_decibels(panel.ceilingDb, 0)
            color: panel.theme.gridText
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeMicro
        }
        Column {
            x: panel.theme.cardInset
            y: panel.theme.readingLinesY
            width: readingPlate.width - 2 * panel.theme.cardInset
            spacing: panel.theme.tableSpacing
            component Line: Item {
                id: line
                required property string name
                required property string value
                property string objectId: ""
                width: parent.width
                height: panel.theme.readingLineHeight
                Caption {
                    anchors.verticalCenter: parent.verticalCenter
                    text: line.name
                }
                Text {
                    objectName: line.objectId
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: line.value
                    color: panel.theme.ink
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSmall
                }
            }
            Line {
                name: "VIEW · RESOLUTION"
                value: panel.revision >= 0 ? panel.controller.display("lens.view") + " · "
                    + panel.controller.display("lens.resolution") : ""
            }
            Line {
                name: "AVERAGE · SMOOTHING"
                value: panel.revision >= 0 ? panel.controller.display("lens.average") + " · "
                    + panel.controller.display("lens.smoothing") : ""
            }
            Line {
                name: "ANALYSES"
                value: panel.playing ? String(Math.round(panel.live.hops)) : "—"
                objectId: "lensHops"
            }
            Line {
                name: "SPECTROGRAM ROWS"
                value: String(gram.rows)
                objectId: "lensHistory"
            }
        }
    }

    // ---- Controls --------------------------------------------------------------------

    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: panel.height - rowCY - margin

    Kit.Card {
        theme: panel.theme
        id: analysisPlate
        title: "ANALYSIS"
        x: panel.margin
        y: panel.rowCY
        width: panel.theme.analysisPlateWidth
        height: panel.rowCHeight
        Caption {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            text: "VIEW"
        }
        Kit.Segment {
            x: panel.theme.cardInset
            y: panel.theme.segmentY
            width: analysisPlate.width - 2 * panel.theme.cardInset
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.view"
            labels: ["MID", "SIDE", "LEFT", "RIGHT", "M / S", "L / R"]
        }
        Caption {
            x: panel.theme.cardInset
            y: panel.theme.secondCaptionY
            text: "RESOLUTION"
        }
        Kit.Segment {
            x: panel.theme.cardInset
            y: panel.theme.secondSegmentY
            width: analysisPlate.width - 2 * panel.theme.cardInset
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.resolution"
            labels: ["FAST", "BALANCED", "PRECISE"]
        }
    }

    Kit.Card {
        theme: panel.theme
        id: averagingPlate
        title: "AVERAGING"
        x: analysisPlate.x + analysisPlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.averagingPlateWidth
        height: panel.rowCHeight
        Caption {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            text: "AVERAGE"
        }
        Kit.Segment {
            x: panel.theme.cardInset
            y: panel.theme.segmentY
            width: averagingPlate.width - 2 * panel.theme.cardInset
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.average"
            labels: ["FAST", "MEDIUM", "SLOW", "∞"]
        }
        Caption {
            x: panel.theme.cardInset
            y: panel.theme.secondCaptionY
            text: "SMOOTHING · OCTAVES"
        }
        Kit.Segment {
            x: panel.theme.cardInset
            y: panel.theme.secondSegmentY
            width: averagingPlate.width - 2 * panel.theme.cardInset
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.smoothing"
            labels: ["1/24", "1/12", "1/6", "1/3"]
        }
    }

    Kit.Card {
        theme: panel.theme
        id: displayPlate
        title: "DISPLAY"
        x: averagingPlate.x + averagingPlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.displayPlateWidth
        height: panel.rowCHeight
        Caption {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            text: "TILT · PIVOT 1 kHz"
        }
        Kit.Segment {
            x: panel.theme.cardInset
            y: panel.theme.segmentY
            width: displayPlate.width - 2 * panel.theme.cardInset
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.tilt"
            labels: ["0", "1.5", "3 dB/oct"]
        }
        Kit.Toggle {
            x: panel.theme.cardInset
            y: panel.theme.holdToggleY
            width: panel.theme.holdToggleWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.hold"
            label: "Peak Hold"
            lit: panel.theme.hold
            onCaption: "HOLDS, THEN FALLS"
            offCaption: "LIVE ONLY"
        }
        Kit.Knob {
            x: panel.theme.decayKnobX
            y: panel.theme.decayKnobY
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.hold_decay"
            label: "Decay"
            diameter: panel.theme.decayKnobDiameter
            inactive: !panel.holdOn
            inactiveReason: panel.holdOn ? "" : "Peak Hold is off"
        }
    }

    Kit.Card {
        theme: panel.theme
        id: referencePlate
        title: "REFERENCE"
        x: displayPlate.x + displayPlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.referencePlateWidth
        height: panel.rowCHeight
        Kit.Toggle {
            x: panel.theme.cardInset
            y: panel.theme.toggleY
            width: referencePlate.width - 2 * panel.theme.cardInset
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.overlay"
            label: "Sidechain"
            lit: panel.theme.overlay
            onCaption: "OVERLAY A REFERENCE"
            offCaption: "OFF"
        }
        // The difference curve: how the panel draws, not a parameter.
        Rectangle {
            id: differenceToggle
            objectName: "lensDifferenceToggle"
            readonly property bool on: panel.showDifference
            x: panel.theme.cardInset
            y: panel.theme.differenceToggleY
            width: referencePlate.width - 2 * panel.theme.cardInset
            height: panel.theme.differenceToggleHeight
            radius: panel.theme.radiusLg
            opacity: panel.overlayOn ? 1 : panel.theme.idleOpacity
            color: differenceArea.pressed ? panel.theme.button : panel.theme.buttonHi
            border.color: activeFocus ? panel.theme.accent
                : on && panel.overlayOn ? Qt.rgba(panel.theme.difference.r, panel.theme.difference.g,
                                                  panel.theme.difference.b, panel.theme.buttonOnEdgeAlpha) : panel.theme.seam
            activeFocusOnTab: true
            Keys.onReturnPressed: panel.showDifference = !panel.showDifference
            Keys.onSpacePressed: panel.showDifference = !panel.showDifference
            Accessible.onToggleAction: panel.showDifference = !panel.showDifference
            Accessible.role: Accessible.CheckBox
            Kit.HoverTip { theme: panel.theme }
            Accessible.name: "Difference curve"
            Accessible.checked: on
            Rectangle {
                x: panel.theme.differenceLedX
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.differenceLedSize
                height: panel.theme.differenceLedSize
                radius: width / 2
                color: differenceToggle.on && panel.overlayOn ? panel.theme.difference : panel.theme.well
                border.color: panel.theme.wellEdge
            }
            Column {
                x: panel.theme.differenceTextX
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.hairline
                Text {
                    text: "DIFFERENCE"
                    color: differenceToggle.on ? panel.theme.ink : panel.theme.inkDim
                    font.pixelSize: panel.theme.typeSmall
                    font.bold: true
                    font.letterSpacing: panel.theme.bannerSpacing
                }
                Text {
                    text: !panel.overlayOn ? "NEEDS THE SIDECHAIN" : differenceToggle.on ? "MIX − REFERENCE, ±12 dB" : "HIDDEN"
                    color: differenceToggle.on && panel.overlayOn ? panel.theme.difference : panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.letterSpacing: panel.theme.subcaptionSpacing
                }
            }
            MouseArea {
                id: differenceArea
                anchors.fill: parent
                onClicked: panel.showDifference = !panel.showDifference
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: freezePlate
        title: "FREEZE"
        x: referencePlate.x + referencePlate.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Kit.Toggle {
            id: freezeToggle
            x: panel.theme.cardInset
            y: panel.theme.toggleY
            width: freezePlate.width - 2 * panel.theme.cardInset
            height: panel.theme.freezeToggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "lens.freeze"
            label: "Freeze"
            lit: panel.theme.freeze
            onCaption: "PICTURE HELD"
            offCaption: "HOLD TO COMPARE"
        }
        Text {
            x: panel.theme.cardInset
            y: panel.theme.freezeNoteY
            width: freezePlate.width - 2 * panel.theme.cardInset
            text: "Lens never touches the audio; it only looks."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Timer {
        objectName: "lensTelemetryPoll"
        interval: panel.playing ? 33 : 250
        repeat: true
        running: panel.visible
        onTriggered: panel.poll()
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
