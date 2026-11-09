pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The Concert device panel, "Concert black": a gloss-black lacquer case
// with brass hardware and gold-leaf lettering, for the sampled grand
// (ADR-0071). The library leads: what is installed, its attribution, or
// how to install it; beside it the strings, dampers and pedals seen with
// the lid up. Below, the controls grouped as a technician voices a sampled
// piano: Voicing, Dampers & Pedals, Mechanics, Stereo and Output. Every
// control reads and writes through `controller`
// (NativeSynthDeviceController for Concert); no project or audio state
// lives in QML.
Rectangle {
    id: panel
    required property var controller
    // Live engine state (ADR-0037), polled while the panel is visible: fast
    // while the synth publishes, slowly while it does not.
    property var live: ({ live: false })
    property ConcertTheme theme: ConcertTheme {}
    // The sample library's state (concert_library), read again when the
    // panel opens, when the patch changes and when the user asks.
    property int libraryLook: 0
    readonly property var library: controller.revision >= 0 && libraryLook >= 0
        ? controller.concert_library() : ({})
    readonly property bool ready: library.state === "ready"

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    // Where the lacquer's mid tone sits in the case gradient.
    readonly property real lacquerStop: 0.35

    objectName: "concertPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.lacquerTop }
        GradientStop { position: panel.lacquerStop; color: panel.theme.lacquer }
        GradientStop { position: 1; color: panel.theme.lacquerBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Concert grand piano"

    // The lacquer's reflection: a soft diagonal sheen across the case.
    Canvas {
        readonly property string plotExemptReason: "the case's decorative sheen, not a plot"
        // The sheen's run down the case and where its band peaks and fades.
        readonly property real sheenDepth: 0.6
        readonly property real sheenPeak: 0.28
        readonly property real sheenFade: 0.36
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const s = panel.theme.sheen
            const g = ctx.createLinearGradient(0, 0, width, height * sheenDepth)
            g.addColorStop(0, Qt.rgba(s.r, s.g, s.b, 0))
            g.addColorStop(sheenPeak, Qt.rgba(s.r, s.g, s.b, panel.theme.sheenAlpha))
            g.addColorStop(sheenFade, Qt.rgba(s.r, s.g, s.b, panel.theme.sheenTailAlpha))
            g.addColorStop(1, Qt.rgba(s.r, s.g, s.b, 0))
            ctx.fillStyle = g
            ctx.fillRect(0, 0, width, height)
        }
    }

    // A lacquered section framed by a fine brass line, titled in gold.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        radius: panel.theme.radiusLg
        color: Qt.rgba(panel.theme.plate.r, panel.theme.plate.g, panel.theme.plate.b, panel.theme.plateAlpha)
        border.color: panel.theme.plateEdge
        Rectangle {
            anchors.fill: parent
            anchors.margins: panel.theme.plateFrameInset
            radius: panel.theme.radiusMd
            color: "transparent"
            border.color: Qt.rgba(panel.theme.brass.r, panel.theme.brass.g, panel.theme.brass.b,
                                  panel.theme.plateFrameAlpha)
        }
        Text {
            id: plateTitle
            x: panel.theme.plateInset
            y: panel.theme.plateTitleY
            text: plate.title
            color: panel.theme.gold
            font.family: panel.theme.serif
            font.pixelSize: panel.theme.typeSmall
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
        Text {
            x: plateTitle.x + plateTitle.implicitWidth + panel.theme.plateNoteGap
            y: panel.theme.plateNoteY
            visible: plate.note !== ""
            text: plate.note
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
    }

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.knobDiameter
        property string label: ""
        width: panel.theme.cellWidth
        height: diameter + panel.theme.cellCaptionHeight
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            diameter: cell.diameter
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
        }
    }

    // A plate holding a centred row of knobs.
    component KnobPlate: Plate {
        id: knobPlate
        required property var parameters
        property var labels: []
        Row {
            id: knobRow
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.plateBodyY + (knobPlate.height - panel.theme.plateBodyY - knobRow.height) / 2
            Repeater {
                model: knobPlate.parameters
                delegate: Cell {
                    required property string modelData
                    required property int index
                    parameterId: modelData
                    label: knobPlate.labels.length > index ? knobPlate.labels[index] : ""
                }
            }
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
        // The gold serif wordmark over its brass rule, the subtitle under it.
        brand: Component {
            Item {
                implicitWidth: wordmark.implicitWidth
                implicitHeight: subtitle.y + subtitle.implicitHeight
                Text {
                    id: wordmark
                    objectName: "concertWordmark"
                    text: "CONCERT"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Rectangle {
                    x: panel.theme.wordmarkInset
                    y: wordmark.height + panel.theme.wordmarkRuleGap
                    width: wordmark.implicitWidth - panel.theme.wordmarkRuleTrim
                    height: panel.theme.hairline
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: panel.theme.brassHi }
                        GradientStop { position: 1; color: Qt.rgba(panel.theme.brass.r, panel.theme.brass.g, panel.theme.brass.b, 0) }
                    }
                }
                Text {
                    id: subtitle
                    objectName: "concertSubtitle"
                    x: panel.theme.wordmarkInset
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "SAMPLED GRAND"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }
    }

    // ---- The library and the strings -----------------------------------------------------

    readonly property real rowAY: header.y + header.height + theme.headerContentGap
    readonly property real rowAHeight: theme.rowAHeight

    Plate {
        id: libraryPlate
        objectName: "concertLibrary"
        // The notice names the plate itself.
        title: ""
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.libraryWidth
        height: panel.rowAHeight
        Accessible.role: Accessible.Pane
        Accessible.name: "Piano library"
        Accessible.description: panel.ready ? panel.library.name + ", " + panel.library.attribution
                                            : panel.library.message !== undefined ? panel.library.message : ""
        Kit.LibraryNotice {
            id: notice
            x: panel.theme.plateInset
            y: panel.theme.plateInset
            width: libraryPlate.width - 2 * panel.theme.plateInset
            controller: panel.controller
            theme: panel.theme
            library: panel.library
            instrument: "Concert"
            details: panel.ready
                ? "Sixteen velocity layers, sampled every minor third, with key-release, "
                  + "string-release and pedal recordings. " + Format.quantity(panel.library.resident_mb, "MB", 0)
                  + " stay in memory; the rest streams from disk."
                : ""
            onReload: {
                panel.controller.concert_reload_library()
                ++panel.libraryLook
            }
        }
        Text {
            objectName: "concertUnderruns"
            x: notice.x
            y: notice.y + notice.height + panel.theme.spaceSm
            width: notice.width
            visible: panel.ready && panel.live.live === true && panel.live.underruns > 0
            text: panel.live.underruns > 0
                ? "The disk was late " + panel.live.underruns + " times: those notes dropped out."
                : ""
            color: panel.theme.feltLifted
            font.pixelSize: panel.theme.typeSmall
            wrapMode: Text.WordWrap
        }
    }

    Plate {
        id: strings
        title: "STRINGS · DAMPERS · PEDALS"
        x: libraryPlate.x + libraryPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        ConcertStringsView {
            id: stringsView
            x: panel.theme.spaceLg
            y: panel.theme.plateBodyY
            width: strings.width - 2 * panel.theme.spaceLg
            height: strings.height - panel.theme.plateBodyY - panel.theme.spaceMd
            theme: panel.theme
            library: panel.library
            live: panel.live.live === true
            energies: panel.live.live === true ? panel.live.energies : []
            dampers: panel.live.live === true ? panel.live.dampers : []
            pedals: panel.live.live === true
                ? { sustain: panel.live.sustain, sostenuto: panel.live.sostenuto, soft: panel.live.soft }
                : { sustain: 0, sostenuto: 0, soft: 0 }
        }
    }

    // ---- Controls ----------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: theme.rowBHeight
    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: theme.rowCHeight
    readonly property real thirdWidth: (width - 2 * margin - 2 * gap) / 3

    KnobPlate {
        id: voicing
        title: "VOICING"
        x: panel.margin
        y: panel.rowBY
        width: panel.thirdWidth
        height: panel.rowBHeight
        parameters: ["concert.tone", "concert.touch", "concert.dynamics"]
    }
    KnobPlate {
        id: dampersPlate
        title: "DAMPERS & PEDALS"
        x: voicing.x + voicing.width + panel.gap
        y: panel.rowBY
        width: panel.thirdWidth
        height: panel.rowBHeight
        parameters: ["concert.damper", "concert.una_corda", "concert.resonance"]
    }
    KnobPlate {
        id: mechanics
        title: "MECHANICS"
        note: "RECORDED WITH THE PIANO"
        x: dampersPlate.x + dampersPlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        parameters: ["concert.noise.release", "concert.noise.strings", "concert.noise.pedal"]
        labels: ["Keys", "Strings", "Pedal"]
    }

    Plate {
        id: stereo
        title: "STEREO"
        x: panel.margin
        y: panel.rowCY
        width: panel.thirdWidth
        height: panel.rowCHeight
        Cell {
            x: panel.theme.stereoKnobX
            y: panel.theme.stereoKnobY
            parameterId: "concert.width"
        }
        Text {
            x: panel.theme.stereoChoiceX
            y: panel.theme.stereoCaptionY
            text: "PERSPECTIVE"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        Kit.Segment {
            x: panel.theme.stereoChoiceX
            y: panel.theme.stereoChoiceY
            width: stereo.width - panel.theme.stereoChoiceX - panel.theme.plateInset
            // Tall enough for each tile's name and caption.
            height: Math.max(panel.theme.stereoChoiceHeight, implicitHeight)
            vertical: true
            controller: panel.controller
            theme: panel.theme
            parameterId: "concert.perspective"
            captions: ["Bass on the left", "Mirrored, from the hall"]
        }
    }
    KnobPlate {
        id: output
        title: "OUTPUT"
        x: stereo.x + stereo.width + panel.gap
        y: panel.rowCY
        width: panel.thirdWidth
        height: panel.rowCHeight
        parameters: ["concert.tune", "concert.level"]
    }
    Plate {
        id: about
        objectName: "concertAbout"
        title: "ABOUT"
        x: output.x + output.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Text {
            objectName: "concertAboutText"
            x: panel.theme.plateInset
            y: panel.theme.plateTextY
            width: about.width - 2 * panel.theme.plateInset
            text: (panel.library.required !== undefined ? panel.library.required : "")
                + ". Converted for Concert by its install script; the recordings are otherwise "
                + "unchanged. Licensed under Creative Commons Attribution 3.0."
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeSmall
            wrapMode: Text.WordWrap
        }
    }

    Timer {
        // Fast while the synth publishes, slowly while it does not (ms).
        readonly property int livePoll: 33
        readonly property int idlePoll: 250
        objectName: "concertTelemetryPoll"
        interval: panel.live.live === true ? livePoll : idlePoll
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
