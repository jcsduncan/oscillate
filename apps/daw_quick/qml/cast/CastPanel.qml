pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Cast device panel: a convolution reverb. On top, the IR browser (the
// bundled rooms, halls, plates and spring by category, a user's file loaded
// or dropped on the panel, and its right-input half for true stereo), the
// response as Cast plays it (the shaped IR's envelope and its energy decay
// curve after the pre-delay, with its length, RT60 and routing), and the
// IR's shape (start, length, stretch, decay, fade in, reverse, normalise).
// Below, space (pre-delay and width), tone (low cut, high cut, tilt),
// ducking under the input or a sidechain, and the wet gain and mix with
// the input and wet meters. Every control reads and writes through
// `controller` (NativeSynthDeviceController for Cast); the readings come
// only from the telemetry (layout in cast.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property CastTheme theme: CastTheme {}
    // Set by the host when a sidechain send keys this slot.
    property bool keyed: false
    property string keyName: "the sidechain"

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    readonly property var doc: controller.document
    readonly property bool playing: live.live === true
    readonly property real silentDb: -120
    readonly property real millisecondsPerSecond: 1000
    // The meters' bars: -60 dBFS to 0 dBFS; the duck's: 0 to 30 dB.
    readonly property real barFloorDb: -60
    readonly property real barTopDb: 0
    readonly property real duckFullDb: 30
    readonly property int levelDecimals: 1
    readonly property int secondsDecimals: 2
    // The routings by response count (cast::telemetry::responses).
    readonly property var routingNames: ({ 1: "MONO", 2: "STEREO", 4: "TRUE STEREO" })

    readonly property var irs: controller.cast_irs()
    readonly property var irMedia: doc.media_revision >= 0 ? doc.media("ir") : ({})
    readonly property var rightMedia: doc.media_revision >= 0 ? doc.media("ir.right") : ({})
    readonly property bool userIr: irMedia.kind === "managed"
    readonly property bool trueStereoFile: userIr && rightMedia.kind === "managed"
    readonly property bool stereoFile: userIr && irMedia.channels === stereoChannels
    readonly property int stereoChannels: 2
    // The bundled IR playing (an empty document plays the hall).
    readonly property string currentIr: irMedia.kind === "builtin" ? irMedia.name
        : userIr ? "" : "hall"
    readonly property string irName: {
        if (userIr)
            return trueStereoFile ? "YOUR IR · TRUE STEREO" : "YOUR IR"
        for (const ir of irs)
            if (ir.id === currentIr)
                return ir.name.toUpperCase()
        return ""
    }
    readonly property real predelaySeconds: revision >= 0
        ? controller.value("cast.predelay") / millisecondsPerSecond : 0
    readonly property bool sidechainKey: revision >= 0 && controller.choice("cast.duck_key") === 1

    readonly property real inPeak: playing ? Math.max(live.input_peak[0], live.input_peak[1]) : silentDb
    readonly property real wetPeak: playing ? Math.max(live.wet_peak[0], live.wet_peak[1]) : silentDb
    readonly property real duckDb: playing ? live.duck : 0

    objectName: "castPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.caseColor
    border.color: theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Cast convolution reverb"

    function poll() {
        live = controller.telemetry()
    }
    function pick(id) {
        if (!controller.editable)
            return
        theme.lastTouched = ""
        doc.set_sole_builtin_media("ir", id)
    }
    function loadFile(url) {
        return controller.import_media("ir", String(url))
    }
    function levelText(db) {
        return playing && db > silentDb ? Format.signed_number(db, levelDecimals) + " dBFS" : "—"
    }
    function fraction(value, low, high) {
        return Math.max(0, Math.min(1, (value - low) / (high - low)))
    }

    component Caption: Text {
        color: panel.theme.inkDim
        font.pixelSize: panel.theme.typeSmall
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // A knob with its readout and a hint under it.
    component Hinted: Column {
        id: hinted
        required property string parameterId
        property string hint: ""
        property int diameter: panel.theme.knobDiameterLarge
        spacing: panel.theme.knobHintGap
        Kit.Knob {
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: hinted.parameterId
            diameter: hinted.diameter
            readout: true
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: hinted.hint !== ""
            text: hinted.hint
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
        }
    }

    // A labelled level bar.
    component Meter: Item {
        id: meter
        required property string title
        required property string valueName
        required property string value
        required property real level
        property color barColor: panel.theme.inkDim
        height: panel.theme.meterHeight
        Caption {
            text: meter.title
        }
        Text {
            objectName: meter.valueName
            anchors.right: parent.right
            text: meter.value
            color: panel.theme.ink
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeLabel
        }
        Rectangle {
            y: meter.height - height
            width: meter.width
            height: panel.theme.meterBarHeight
            radius: panel.theme.meterBarRadius
            color: panel.theme.grid
            Rectangle {
                width: parent.width * meter.level
                height: parent.height
                radius: parent.radius
                color: meter.barColor
            }
        }
    }

    // A reading under the response view.
    component Reading: Column {
        id: reading
        required property string title
        required property string valueName
        required property string value
        spacing: panel.theme.spaceXs
        Caption {
            text: reading.title
        }
        Text {
            objectName: reading.valueName
            text: reading.value
            color: panel.theme.ink
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.readoutValueSize
        }
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
        // The mark (an impulse and its echoes in a ring) beside the
        // wordmark over the IR's name.
        brand: Component {
            Item {
                implicitWidth: words.x + words.implicitWidth
                implicitHeight: Math.max(mark.height, words.implicitHeight)
                Rectangle {
                    id: mark
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.markSize
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.color: panel.theme.accent
                    border.width: panel.theme.markStroke
                    Canvas {
                        readonly property string plotExemptReason: "Cast's logo, not a plot"
                        anchors.fill: parent
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const w = width
                            ctx.strokeStyle = panel.theme.accent
                            ctx.lineWidth = panel.theme.markStroke
                            ctx.lineCap = "round"
                            ctx.beginPath()
                            for (const line of panel.theme.markLines) {
                                ctx.moveTo(w * line[0], w * panel.theme.markBaseline)
                                ctx.lineTo(w * line[0], w * line[1])
                            }
                            ctx.stroke()
                        }
                    }
                }
                Column {
                    id: words
                    x: mark.width + panel.theme.markGap
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: panel.theme.subtitleGap
                    Text {
                        objectName: "castWordmark"
                        text: "CAST"
                        color: panel.theme.wordmarkInk
                        font.family: panel.theme.wordmarkFont
                        font.pixelSize: panel.theme.wordmarkSize
                        font.weight: panel.theme.wordmarkWeight
                        font.letterSpacing: panel.theme.wordmarkSpacing
                    }
                    Text {
                        objectName: "castSubtitle"
                        text: "CONVOLUTION · " + panel.irName
                        color: panel.theme.subtitleInk
                        font.pixelSize: panel.theme.subtitleSize
                        font.bold: true
                        font.letterSpacing: panel.theme.subtitleSpacing
                    }
                }
            }
        }
    }

    readonly property real bodyY: theme.bodyY
    readonly property real rowBY: bodyY + theme.rowAHeight + gap
    readonly property real rowBHeight: height - margin - rowBY

    // ---- The IR browser --------------------------------------------------------------

    Kit.Card {
        id: browserCard
        theme: panel.theme
        title: "IMPULSE RESPONSE"
        note: panel.userIr ? "FILE" : "BUNDLED"
        x: panel.margin
        y: panel.bodyY
        width: panel.theme.browserWidth
        height: panel.theme.rowAHeight
        ListView {
            id: irList
            objectName: "castIrList"
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: browserCard.width - 2 * panel.theme.cardInset
            height: browserCard.height - y - panel.theme.browserButtonsHeight - panel.theme.spaceSm
            clip: true
            model: panel.irs
            boundsBehavior: Flickable.StopAtBounds
            section.property: "category"
            section.delegate: Item {
                id: sectionRow
                required property string section
                width: irList.width
                height: panel.theme.listSectionHeight
                Caption {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: panel.theme.spaceXs
                    text: sectionRow.section.toUpperCase()
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeCaption
                }
            }
            delegate: Rectangle {
                id: row
                required property var modelData
                readonly property bool current: modelData.id === panel.currentIr
                objectName: "castIr:" + modelData.id
                width: irList.width
                height: panel.theme.listRowHeight
                radius: panel.theme.listRowRadius
                color: current ? panel.theme.button : rowArea.containsMouse ? panel.theme.listRowHover
                                                                         : "transparent"
                activeFocusOnTab: panel.controller.editable
                Accessible.role: Accessible.Button
                Accessible.name: modelData.name
                Accessible.checked: current
                Accessible.onPressAction: panel.pick(modelData.id)
                Keys.onReturnPressed: panel.pick(modelData.id)
                Keys.onSpacePressed: panel.pick(modelData.id)
                Rectangle {
                    visible: row.current
                    width: panel.theme.listMarkWidth
                    height: parent.height
                    radius: panel.theme.listMarkWidth / 2
                    color: panel.theme.accent
                }
                Text {
                    x: panel.theme.listTextX
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.name
                    color: row.current ? panel.theme.accent : panel.theme.ink
                    font.pixelSize: panel.theme.typeBody
                    font.bold: row.current
                }
                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: panel.pick(row.modelData.id)
                }
            }
        }
        Column {
            x: panel.theme.cardInset
            y: browserCard.height - panel.theme.browserButtonsHeight + panel.theme.spaceSm
            width: browserCard.width - 2 * panel.theme.cardInset
            spacing: panel.theme.spaceSm
            Row {
                spacing: panel.theme.spaceSm
                Kit.Button {
                    objectName: "castLoad"
                    theme: panel.theme
                    label: "Load IR…"
                    tip: "A WAV, FLAC or AIFF impulse response, mono or stereo"
                    on: panel.userIr
                    onClicked: panel.controller.choose_media("ir", "Load an impulse response")
                }
                Kit.Button {
                    objectName: "castLoadRight"
                    theme: panel.theme
                    label: "Right input…"
                    tip: "The right input's stereo response, for a true-stereo IR"
                    active: panel.stereoFile
                    on: panel.trueStereoFile
                    onClicked: panel.controller.choose_media("ir.right", "Load the right input's response")
                }
            }
            Text {
                objectName: "castDropHint"
                width: parent.width
                text: panel.userIr
                    ? Format.seconds((panel.irMedia.frames || 0) / Math.max(1, panel.irMedia.sample_rate || 1))
                      + " · " + Format.number((panel.irMedia.sample_rate || 0) / panel.millisecondsPerSecond, panel.levelDecimals)
                      + " kHz · drop another file to replace it"
                    : "Or drop a WAV, FLAC or AIFF on the panel"
                color: panel.theme.inkFaint
                font.pixelSize: panel.theme.typeCaption
                elide: Text.ElideRight
            }
        }
    }

    // ---- The response ------------------------------------------------------------------

    Kit.Card {
        id: responseCard
        theme: panel.theme
        title: "RESPONSE"
        note: panel.playing ? "AS CAST PLAYS IT" : "PLAY TO DRAW THE IR"
        x: browserCard.x + browserCard.width + panel.gap
        y: panel.bodyY
        width: panel.theme.responseWidth
        height: panel.theme.rowAHeight
        Kit.PlotFrame {
            id: frame
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: responseCard.width - 2 * panel.theme.cardInset
            height: responseCard.height - y - panel.theme.readoutsHeight - panel.theme.spaceMd
            theme: panel.theme
            live: panel.playing
            xUnit: "s"
            yUnit: "dB"
            xTicks: irView.timeTicks.map(s => ({ at: irView.timeAt(s), label: Format.number(s, irView.timeStep < 1 ? panel.secondsDecimals : 0) }))
            yTicks: irView.dbTicks.map(db => ({ at: irView.levelAt(db), label: Format.signed_number(db, 0) }))
            CastIrView {
                id: irView
                objectName: "castIrView"
                anchors.fill: parent
                theme: panel.theme
                live: panel.playing
                envelope: panel.playing ? panel.live.envelope : []
                decayCurve: panel.playing ? panel.live.decay_curve : []
                lengthSeconds: panel.playing ? panel.live.length_seconds : 0
                predelaySeconds: panel.predelaySeconds
            }
        }
        Row {
            x: panel.theme.cardInset
            y: responseCard.height - panel.theme.readoutsHeight - panel.theme.spaceSm
            readonly property real slot: (responseCard.width - 2 * panel.theme.cardInset) / 4
            Reading {
                width: parent.slot
                title: "LENGTH"
                valueName: "castLength"
                value: panel.playing ? Format.number(panel.live.length_seconds, panel.secondsDecimals) + " s" : "—"
            }
            Reading {
                width: parent.slot
                title: "RT60"
                valueName: "castRt60"
                value: panel.playing ? Format.number(panel.live.rt60_seconds, panel.secondsDecimals) + " s" : "—"
            }
            Reading {
                width: parent.slot
                title: "ROUTING"
                valueName: "castRouting"
                value: panel.playing ? (panel.routingNames[panel.live.responses] || "—") : "—"
            }
            Reading {
                width: parent.slot
                title: "LATENCY"
                valueName: "castLatency"
                value: "0 ms"
            }
        }
    }

    // ---- The IR's shape ------------------------------------------------------------------

    Kit.Card {
        id: shapeCard
        theme: panel.theme
        title: "SHAPE"
        note: "REBUILDS THE IR"
        x: responseCard.x + responseCard.width + panel.gap
        y: panel.bodyY
        width: panel.width - panel.margin - x
        height: panel.theme.rowAHeight
        readonly property real slot: (width - 2 * panel.theme.cardInset) / 3
        Column {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            spacing: panel.theme.shapeRowGap
            Row {
                Hinted { width: shapeCard.slot; parameterId: "cast.start"; hint: "skip the start" }
                Hinted { width: shapeCard.slot; parameterId: "cast.length"; hint: "keep this much" }
                Hinted { width: shapeCard.slot; parameterId: "cast.stretch"; hint: "time, not pitch" }
            }
            Row {
                Hinted { width: shapeCard.slot; parameterId: "cast.decay"; hint: "shorter tail" }
                Hinted { width: shapeCard.slot; parameterId: "cast.fade_in"; hint: "swell in" }
                Column {
                    width: shapeCard.slot
                    spacing: panel.theme.switchRowGap
                    topPadding: panel.theme.spaceLg
                    Kit.Switch {
                        anchors.horizontalCenter: parent.horizontalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: "cast.reverse"
                    }
                    Kit.Switch {
                        anchors.horizontalCenter: parent.horizontalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: "cast.normalise"
                    }
                }
            }
        }
    }

    // ---- Space, tone, duck, mix ------------------------------------------------------------

    Kit.Card {
        id: spaceCard
        theme: panel.theme
        title: "SPACE"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.spaceWidth
        height: panel.rowBHeight
        Row {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Hinted { width: (spaceCard.width - 2 * panel.theme.cardInset) / 2; parameterId: "cast.predelay"; hint: "before the IR" }
            Hinted { width: (spaceCard.width - 2 * panel.theme.cardInset) / 2; parameterId: "cast.width"; hint: "of the wet" }
        }
    }

    Kit.Card {
        id: toneCard
        theme: panel.theme
        title: "TONE"
        note: "ON THE WET"
        x: spaceCard.x + spaceCard.width + panel.gap
        y: panel.rowBY
        width: panel.theme.toneWidth
        height: panel.rowBHeight
        readonly property real slot: (width - 2 * panel.theme.cardInset) / 3
        Row {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Hinted { width: toneCard.slot; parameterId: "cast.low_cut"; hint: "off at 20 Hz" }
            Hinted { width: toneCard.slot; parameterId: "cast.high_cut"; hint: "off at 20 kHz" }
            Hinted { width: toneCard.slot; parameterId: "cast.tilt"; hint: "darker · brighter" }
        }
    }

    Kit.Card {
        id: duckCard
        theme: panel.theme
        title: "DUCK"
        note: panel.sidechainKey ? (panel.keyed ? "UNDER " + panel.keyName.toUpperCase()
                                                : "SIDECHAIN · NONE ROUTED")
                                 : "UNDER THE INPUT"
        noteColor: panel.sidechainKey && panel.keyed ? panel.theme.decay : panel.theme.inkFaint
        x: toneCard.x + toneCard.width + panel.gap
        y: panel.rowBY
        width: panel.theme.duckWidth
        height: panel.rowBHeight
        readonly property real slot: (width - 2 * panel.theme.cardInset) / 3
        Row {
            id: duckKnobs
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Hinted { width: duckCard.slot; parameterId: "cast.duck"; hint: "at most"; diameter: panel.theme.knobDiameterSmall }
            Hinted { width: duckCard.slot; parameterId: "cast.duck_threshold"; hint: "from"; diameter: panel.theme.knobDiameterSmall }
            Hinted { width: duckCard.slot; parameterId: "cast.duck_release"; hint: "back up"; diameter: panel.theme.knobDiameterSmall }
        }
        Kit.SegmentedControl {
            id: duckKey
            x: panel.theme.cardInset
            y: duckKnobs.y + duckKnobs.height + panel.theme.spaceMd
            width: panel.theme.keySegmentWidth
            theme: panel.theme
            controller: panel.controller
            parameterId: "cast.duck_key"
            labels: ["Input", "Sidechain"]
        }
        Meter {
            x: duckKey.x + duckKey.width + panel.theme.spaceLg
            y: duckKey.y
            width: duckCard.width - x - panel.theme.cardInset
            title: "DUCKING"
            valueName: "castDuck"
            value: panel.playing ? Format.number(-panel.duckDb, panel.levelDecimals) + " dB" : "—"
            level: panel.fraction(panel.duckDb, 0, panel.duckFullDb)
            barColor: panel.theme.accent
        }
    }

    Kit.Card {
        id: mixCard
        theme: panel.theme
        title: "MIX"
        note: "EQUAL POWER"
        x: duckCard.x + duckCard.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Row {
            id: mixKnobs
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Hinted { width: (mixCard.width - 2 * panel.theme.cardInset) / 2; parameterId: "cast.gain"; diameter: panel.theme.knobDiameterSmall }
            Hinted { width: (mixCard.width - 2 * panel.theme.cardInset) / 2; parameterId: "cast.mix"; diameter: panel.theme.knobDiameterSmall }
        }
        Column {
            x: panel.theme.cardInset
            y: mixKnobs.y + mixKnobs.height + panel.theme.spaceSm
            width: mixCard.width - 2 * panel.theme.cardInset
            spacing: panel.theme.spaceXs
            Meter {
                width: parent.width
                title: "IN"
                valueName: "castInPeak"
                value: panel.levelText(panel.inPeak)
                level: panel.playing ? panel.fraction(panel.inPeak, panel.barFloorDb, panel.barTopDb) : 0
            }
            Meter {
                width: parent.width
                title: "WET"
                valueName: "castWetPeak"
                value: panel.levelText(panel.wetPeak)
                level: panel.playing ? panel.fraction(panel.wetPeak, panel.barFloorDb, panel.barTopDb) : 0
                barColor: panel.theme.decay
            }
        }
    }

    DropArea {
        objectName: "castDropArea"
        anchors.fill: parent
        onEntered: dragEvent => {
            dragEvent.accepted = dragEvent.hasUrls && dragEvent.urls.length > 0 && panel.controller.editable
        }
        onDropped: dropEvent => {
            if (dropEvent.hasUrls && dropEvent.urls.length > 0)
                panel.loadFile(dropEvent.urls[0])
        }
    }

    Timer {
        objectName: "castTelemetryPoll"
        interval: panel.playing ? panel.theme.pollPlayingMs : panel.theme.pollIdleMs
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
