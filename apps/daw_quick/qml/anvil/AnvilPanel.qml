pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit
import "../shell" as Shell
import "AnvilMath.js" as AnvilMath

// The Anvil kick synth panel (ADR-0060): the body's pitch, amp and drive curves drawn over the kick they make (the
// engine renders it offline on every edit, so it shows with nothing
// playing); the one beat of the whole hit with the tuned rumble ducking
// under it; the layers (body, click, sub, rumble) on the left with the
// chosen one's controls on the right; the master drive, tone, punch, glue
// and output along the bottom. The header names the tail's note and tunes
// it into the song's key.
//
// Values go through `controller` (NativeSynthDeviceController) and the
// curves and the user's transient through its document; `anvil`
// (AnvilPanelModel) is the waveform preview, null in previews without one.
Rectangle {
    id: panel
    required property var controller
    property var anvil: null
    // The song's key as pitch classes (projectScale), empty when unset.
    property var songScale: []
    property real tempo: anvil !== null ? anvil.tempo : fallbackTempo
    property AnvilTheme theme: AnvilTheme {}
    readonly property var doc: controller.document
    readonly property int revision: controller.revision
    // Asks the host to play one hit (the bridge's audition).
    signal hitRequested()

    // The layer whose controls show on the right.
    property string selectedLayer: "body"
    // The curve the editor draws.
    property string curveId: "pitch"

    // The preview: the kick's peaks, its length and the tail's pitch.
    readonly property var kickPeaks: anvil !== null && anvil.revision >= 0 ? anvil.kick : []
    readonly property var beatPeaks: anvil !== null && anvil.revision >= 0 ? anvil.beat : []
    readonly property var rumblePeaks: anvil !== null && anvil.revision >= 0 ? anvil.rumble : []
    readonly property real kickFraction: anvil !== null ? anvil.kick_fraction : 1
    readonly property real kickSeconds: anvil !== null ? anvil.kick_seconds
        : revision >= 0 ? controller.value("anvil.length") / 1000 : 0
    readonly property real tailHz: anvil !== null ? anvil.tail_hz : 0
    readonly property var tail: AnvilMath.noteOf(tailHz)
    readonly property bool tailInKey: tail.midi >= 0 && AnvilMath.inScale(tail.midi, songScale)
    readonly property bool tailTuned: tailInKey && Math.abs(tail.cents) <= tunedCents

    // Live state (anvil.hpp telemetry): the position in the hit (0..1 of
    // the kick, -1 idle) and the peak in dB.
    property var tele: ({ live: false })
    readonly property var frame: tele.live === true && tele.frame !== undefined ? tele.frame : []
    readonly property real phase: frame.length > 0 ? frame[0] : -1
    readonly property real livePeakDb: frame.length > 3 ? frame[3] : silentDb

    // The tempo without a preview; a tail this near its note reads as
    // tuned; the peak of silence; Tune to key's reach either way
    // (semitones); the transient choice that plays the user's own sound;
    // the sample rate assumed for a sound that names none; how often the
    // telemetry is read.
    readonly property real fallbackTempo: 124
    readonly property int tunedCents: 5
    readonly property real silentDb: -200
    readonly property real tuneReach: 24
    readonly property int ownTransientChoice: 4
    readonly property int fallbackSampleRate: 48000
    readonly property int telemetryIntervalMs: 33

    // Layout: the panel's designed size and where each region sits.
    readonly property real designedWidth: 1440
    readonly property real designedHeight: 800
    // The header's second row: the tail-note card, Pitch from and Tune to key.
    readonly property real noteCardWidth: 236
    readonly property real noteCardHeight: 44
    readonly property real noteCaptionY: 6
    readonly property real tailNoteY: 17
    readonly property real tailDetailX: 92
    readonly property real tailDetailY: 7
    readonly property real tailDetailRight: 10
    readonly property real tailKeyY: 24
    readonly property real keyChoiceWidth: 170
    // Tune to key sits level with the Pitch from tiles under their label.
    readonly property real tuneButtonY: 14
    readonly property real tuneButtonWidth: 118
    readonly property real peakGap: 3
    readonly property real hitWidth: 92
    // The body, between the header and the master strip: layers left, the
    // editor and beat in the centre, the chosen layer's controls right.
    readonly property real bodyY: header.y + header.height + theme.spaceLg
    readonly property real bodyHeight: masterY - theme.spaceLg - bodyY
    readonly property real layerColumnWidth: 196
    readonly property real layerCardHeight: 76
    readonly property real layerDotSize: 10
    readonly property real layerDotY: 15
    readonly property real layerNameX: 30
    readonly property real layerNameY: 10
    readonly property real layerNoteY: 32
    readonly property real layerBarY: 56
    readonly property real hintCardHeight: 96
    readonly property real hintTextY: 10
    readonly property real centreX: 228
    readonly property real centreWidth: 952
    readonly property real tabWidth: 86
    readonly property real tabHeight: 30
    readonly property real curveReadoutY: 8
    readonly property real curveReadoutWidth: 560
    readonly property real tabRuleY: 31
    readonly property real curveY: 38
    readonly property real curveHeight: 296
    // The beat's note sits this much above its label's row.
    readonly property real beatNoteRaise: 1
    readonly property real beatGap: 18
    // One beat in ms at the tempo, and the beat ticks as fractions of it.
    readonly property real msPerMinute: 60000
    readonly property real beatMs: tempo > 0 ? msPerMinute / tempo : 0
    readonly property var beatTickFractions: [0, 0.25, 0.5, 0.75, 1]
    readonly property real controlsX: 1196
    readonly property real controlsWidth: 228
    readonly property real controlsPad: 10
    readonly property real controlsY: 38
    readonly property real controlsSpacing: 10
    readonly property int knobColumns: 3
    readonly property real toggleY: 6
    readonly property real toggleWidth: 72
    readonly property real toggleHeight: 24
    readonly property real transientHeight: 76
    readonly property real transientNamingHeight: 96
    readonly property real transientPad: 10
    readonly property real transientNameY: 24
    readonly property real transientFieldY: 62
    readonly property real transientFieldHeight: 24
    // The master strip along the bottom.
    readonly property real masterY: 602
    readonly property real masterPad: 14
    readonly property real masterCaptionY: 40
    readonly property real masterRowY: 34
    readonly property real masterRuleInset: 50
    readonly property real driveChoiceWidth: 240
    readonly property real masterKnobsX: 280
    readonly property real velocityRuleX: 760
    readonly property real velocityX: 780
    readonly property real lockRuleX: 940
    readonly property real lockX: 960
    // Wide enough for the Lock choice's longest label on each of its tiles.
    readonly property real lockWidth: Math.max(theme.lockMinWidth, lockChoice.fitWidth)

    // A curve gesture's readout over the editor, until a control is touched
    // again (the touched control reads in the header).
    property string gestureName: ""
    property string gestureValue: ""
    // The transient's path field: shown while typing one.
    property bool naming: false

    objectName: "anvilPanel"
    implicitWidth: designedWidth
    implicitHeight: designedHeight
    radius: theme.panelRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Anvil kick synth"

    function poll() { tele = controller.telemetry() }
    Timer {
        interval: panel.telemetryIntervalMs
        repeat: true
        running: panel.visible
        onTriggered: panel.poll()
    }

    function touch(name, value) {
        theme.lastTouched = ""
        gestureName = name
        gestureValue = value
    }
    Connections {
        target: panel.theme
        function onLastTouchedChanged() {
            if (panel.theme.lastTouched !== "") {
                panel.gestureName = ""
                panel.gestureValue = ""
            }
        }
    }
    function curveReadoutText() {
        if (gestureName !== "")
            return gestureName + " · " + gestureValue
        return curve.hoverText !== "" ? curve.hoverText
            : "Kick " + Format.seconds(kickSeconds)
    }

    function tuneToKey() {
        if (songScale.length === 0 || !(tailHz > 0))
            return
        const tune = controller.value("anvil.tune")
        const next = Math.max(-tuneReach, Math.min(tuneReach, AnvilMath.tuneToKey(tailHz, tune, songScale)))
        theme.lastTouched = "anvil.tune"
        controller.set_value("anvil.tune", next)
    }
    function importTransient(path) {
        const text = String(path).trim()
        if (text === "")
            return false
        const done = controller.import_media("click", text)
        if (done) {
            controller.set_choice("anvil.click.type", ownTransientChoice)
            touch("TRANSIENT", text.split("/").pop())
        } else {
            touch("TRANSIENT", controller.status_detail !== "" ? controller.status_detail : "Not loaded")
        }
        return done
    }
    readonly property var transientMedia: doc.media_revision >= 0 ? doc.media("click") : ({})
    readonly property string transientName: transientMedia.kind === "managed"
        ? "Your sound · " + Format.seconds((transientMedia.frames || 0) / Math.max(1, transientMedia.sample_rate || fallbackSampleRate))
        : "None yet: drop a WAV here, or type its path"

    readonly property var layers: [
        { id: "body", name: "Body", color: theme.body, level: "anvil.body.level", power: "" },
        { id: "click", name: "Click", color: theme.click, level: "anvil.click.level", power: "" },
        { id: "sub", name: "Sub", color: theme.sub, level: "anvil.sub.level", power: "anvil.sub.on" },
        { id: "rumble", name: "Rumble", color: theme.rumble, level: "anvil.rumble.amount", power: "anvil.rumble.on" }
    ]
    function layerOf(id) {
        for (const entry of layers)
            if (entry.id === id)
                return entry
        return layers[0]
    }
    function layerNote(id) {
        if (revision < 0)
            return ""
        if (id === "body")
            return controller.display("anvil.body.wave") + " · " + lengthNote()
        if (id === "click")
            return controller.display("anvil.click.type") + " · " + controller.display("anvil.click.length")
        if (id === "sub")
            return controller.choice("anvil.sub.on") === 1 ? "Tail note · " + controller.display("anvil.sub.octave") : "Off"
        return controller.choice("anvil.rumble.on") === 1 ? "Tuned · " + controller.display("anvil.rumble.decay") : "Off"
    }

    // How long a hit lasts: Length, a note value, or the played note.
    readonly property int lengthMode: revision >= 0 ? controller.choice("anvil.length.mode") : 0
    function lengthNote() {
        if (lengthMode === 1)
            return controller.display("anvil.length.division")
        if (lengthMode === 2)
            return "note length"
        return controller.display("anvil.length")
    }

    component Cell: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
        readout: true
    }
    component Seg: Kit.Segment {
        controller: panel.controller
        theme: panel.theme
    }
    component Drop: Kit.Dropdown {
        controller: panel.controller
        theme: panel.theme
        menu: menu
    }
    component Label: Text {
        color: panel.theme.inkDim
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.labelSize
        font.bold: true
        font.letterSpacing: panel.theme.labelSpacing
    }

    // A layer that is off says so in words (never only by fading).
    component OffChip: Rectangle {
        width: offText.implicitWidth + panel.theme.offChipPad
        height: panel.theme.offChipHeight
        radius: panel.theme.offChipRadius
        color: "transparent"
        border.color: panel.theme.offChipEdge
        Text {
            id: offText
            anchors.centerIn: parent
            text: panel.theme.offChip
            color: panel.theme.offChipInk
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.offChipSize
            font.bold: true
            font.letterSpacing: panel.theme.offChipSpacing
        }
    }

    // ---- Header ---------------------------------------------------------------------

    Kit.DeviceHeader {
        id: header
        x: panel.theme.spaceXl
        y: panel.theme.headerY
        width: panel.width - 2 * panel.theme.spaceXl
        controller: panel.controller
        theme: panel.theme
        menu: menu
        wordmark: "ANVIL"
        subtitle: "KICK SYNTH"

        // The tail's note, against the song's key.
        Rectangle {
            id: noteCard
            width: panel.noteCardWidth
            height: panel.noteCardHeight
            radius: panel.theme.wellRadius
            color: panel.theme.well
            border.color: panel.theme.wellEdge
            Label {
                x: panel.theme.spaceLg
                y: panel.noteCaptionY
                text: "TAIL NOTE"
            }
            Text {
                objectName: "anvilTailNote"
                x: panel.theme.spaceLg
                y: panel.tailNoteY
                text: panel.tail.name
                color: panel.tailTuned ? panel.theme.inKey : panel.theme.offKey
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.tailNoteSize
                font.bold: true
            }
            Text {
                objectName: "anvilTailDetail"
                x: panel.tailDetailX
                y: panel.tailDetailY
                width: noteCard.width - x - panel.tailDetailRight
                text: panel.tailHz > 0 ? AnvilMath.hzText(panel.tailHz) : "—"
                color: panel.theme.ink
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeLabel
            }
            Text {
                objectName: "anvilTailKey"
                x: panel.tailDetailX
                y: panel.tailKeyY
                width: noteCard.width - x - panel.tailDetailRight
                text: Format.cents(panel.tail.cents)
                    + (panel.songScale.length === 0 ? " · no song key" : panel.tailInKey ? " · in key" : " · off key")
                color: panel.tailTuned ? panel.theme.inKey : panel.theme.inkDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeSmall
                elide: Text.ElideRight
            }
        }
        Column {
            spacing: panel.theme.spaceXxs
            Label { text: "PITCH FROM" }
            Seg {
                parameterId: "anvil.key"
                width: panel.keyChoiceWidth
                labels: ["FIXED", "NOTE PLAYED"]
            }
        }
        Kit.Button {
            theme: panel.theme
            objectName: "anvilTuneToKey"
            interactive: panel.controller.editable
            y: panel.tuneButtonY
            width: panel.tuneButtonWidth
            label: "TUNE TO KEY"
            on: panel.songScale.length > 0 && !panel.tailTuned
            active: panel.songScale.length > 0 && panel.tailHz > 0
            tip: panel.songScale.length > 0
                ? "Tune the kick so its tail lands on the nearest note of the song's key"
                : "Set the song's key in the toolbar first"
            onClicked: panel.tuneToKey()
        }
    }

    // The peak and the audition, in the header's first row before its status.
    Row {
        x: header.x + header.status.x - header.theme.headerGap - width
        y: header.y
        height: header.rowHeight
        spacing: panel.theme.spaceMd
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: panel.peakGap
            Label { text: "PEAK" }
            Text {
                objectName: "anvilPeak"
                text: Format.decibels(panel.frame.length > 3 ? panel.livePeakDb
                       : panel.anvil !== null ? panel.anvil.peak_db : panel.silentDb)
                color: panel.theme.ink
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeLabel
            }
        }
        Kit.Button {
            theme: panel.theme
            objectName: "anvilHit"
            interactive: panel.controller.editable
            width: panel.hitWidth
            height: header.rowHeight
            label: "▶ HIT"
            on: true
            tip: "Play one hit"
            onClicked: panel.hitRequested()
        }
    }

    // ---- Layers ---------------------------------------------------------------------

    Column {
        id: layerColumn
        x: panel.theme.spaceXl
        y: panel.bodyY
        width: panel.layerColumnWidth
        spacing: panel.theme.spaceMd
        Label { text: "LAYERS" }
        Repeater {
            model: panel.layers
            delegate: Rectangle {
                id: layerCard
                required property var modelData
                readonly property bool chosen: panel.selectedLayer === modelData.id
                readonly property bool powered: modelData.power === ""
                    || (panel.revision >= 0 && panel.controller.choice(modelData.power) === 1)
                objectName: "anvilLayer:" + modelData.id
                width: layerColumn.width
                height: panel.layerCardHeight
                radius: panel.theme.layerCardRadius
                color: chosen ? panel.theme.selected : panel.theme.plate
                border.color: chosen ? panel.theme.accent : layerArea.containsMouse ? panel.theme.plateHi : panel.theme.plateEdge
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: modelData.name + " layer"
                Keys.onReturnPressed: panel.selectedLayer = modelData.id
                Keys.onSpacePressed: panel.selectedLayer = modelData.id
                Behavior on color {
                    enabled: Shell.Motion.enabled
                    ColorAnimation { duration: Shell.Motion.fast }
                }
                Rectangle {
                    x: panel.theme.spaceLg
                    y: panel.layerDotY
                    width: panel.layerDotSize
                    height: panel.layerDotSize
                    radius: width / 2
                    color: layerCard.modelData.color
                }
                Text {
                    id: layerName
                    x: panel.layerNameX
                    y: panel.layerNameY
                    text: layerCard.modelData.name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.layerNameSize
                    font.bold: true
                }
                OffChip {
                    objectName: "anvilLayerOff:" + layerCard.modelData.id
                    x: layerCard.width - width - panel.theme.spaceLg
                    anchors.verticalCenter: layerName.verticalCenter
                    visible: !layerCard.powered
                }
                Text {
                    x: panel.theme.spaceLg
                    y: panel.layerNoteY
                    width: layerCard.width - 2 * panel.theme.spaceLg
                    text: panel.layerNote(layerCard.modelData.id)
                    color: panel.theme.inkDim
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeBody
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.theme.spaceLg
                    y: panel.layerBarY
                    width: layerCard.width - 2 * panel.theme.spaceLg
                    height: panel.theme.levelBarHeight
                    radius: height / 2
                    color: panel.theme.track
                    Rectangle {
                        width: parent.width * (panel.revision >= 0 ? panel.controller.normalised(layerCard.modelData.level) : 0)
                        height: panel.theme.levelBarHeight
                        radius: height / 2
                        color: layerCard.modelData.color
                    }
                }
                MouseArea {
                    id: layerArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: panel.selectedLayer = layerCard.modelData.id
                    Shell.Tip { text: "Show the " + layerCard.modelData.name.toLowerCase() + "'s controls" }
                }
            }
        }
        Rectangle {
            width: layerColumn.width
            height: panel.hintCardHeight
            radius: panel.theme.layerCardRadius
            color: panel.theme.plate
            border.color: panel.theme.plateEdge
            Text {
                x: panel.theme.spaceLg
                y: panel.hintTextY
                width: parent.width - 2 * panel.theme.spaceLg
                text: "Drag points to shape the body. Drag a small ring to bend a segment. Double-click to add or remove a point."
                wrapMode: Text.WordWrap
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
                lineHeight: panel.theme.hintLineHeight
            }
        }
    }

    // ---- The editor and the beat ----------------------------------------------------

    Item {
        id: centre
        x: panel.centreX
        y: panel.bodyY
        width: panel.centreWidth
        height: panel.bodyHeight

        Row {
            id: tabs
            spacing: panel.theme.spaceXxs
            Repeater {
                model: [["pitch", "PITCH"], ["amp", "AMP"], ["drive", "DRIVE"]]
                delegate: Rectangle {
                    id: tab
                    required property var modelData
                    readonly property bool chosen: panel.curveId === modelData[0]
                    objectName: "anvilTab:" + modelData[0]
                    width: panel.tabWidth
                    height: panel.tabHeight
                    color: "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData[1] + " curve"
                    Keys.onReturnPressed: panel.curveId = modelData[0]
                    Keys.onSpacePressed: panel.curveId = modelData[0]
                    Text {
                        anchors.centerIn: parent
                        text: tab.modelData[1]
                        color: tab.chosen ? panel.theme.hotInk : panel.theme.inkDim
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeBody
                        font.bold: true
                        font.letterSpacing: panel.theme.tabSpacing
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: panel.theme.tabRuleHeight
                        color: tab.chosen ? panel.theme.accent : "transparent"
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: panel.curveId = tab.modelData[0]
                        Shell.Tip { text: "Draw the body's " + tab.modelData[1].toLowerCase() + " curve" }
                    }
                }
            }
        }
        Text {
            objectName: "anvilCurveReadout"
            anchors.right: parent.right
            y: panel.curveReadoutY
            width: panel.curveReadoutWidth
            horizontalAlignment: Text.AlignRight
            text: panel.curveReadoutText()
            color: panel.theme.inkDim
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeBody
            elide: Text.ElideLeft
        }
        Rectangle {
            y: panel.tabRuleY
            width: parent.width
            height: panel.theme.hairline
            color: panel.theme.seam
        }

        // The curve over the kick it makes (rendered from the settings),
        // LIVE while a hit plays its playhead across it.
        Kit.PlotFrame {
            id: curveFrame
            y: panel.curveY
            width: parent.width
            height: panel.curveHeight
            theme: panel.theme
            live: panel.phase >= 0
            xUnit: "ms"
            yUnit: curve.yUnit
            xTicks: curve.xTicks
            yTicks: curve.yTicks
            AnvilCurve {
                id: curve
                anchors.fill: parent
                panel: panel
                curveId: panel.curveId
                kick: panel.kickPeaks
                kickSeconds: panel.kickSeconds
                phase: panel.phase
            }
        }

        Label {
            id: beatLabel
            y: curveFrame.y + curveFrame.height + panel.theme.spaceLg
            text: "ONE BEAT AT " + Format.number(panel.tempo, 0) + " BPM"
        }
        Text {
            x: beatLabel.implicitWidth + panel.theme.spaceLg
            y: beatLabel.y - panel.beatNoteRaise
            text: "the whole hit, with the rumble (violet) ducking under the kick"
            color: panel.theme.inkFaint
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeBody
        }
        // The beat in ms, a tick each quarter of it.
        Kit.PlotFrame {
            y: beatLabel.y + panel.beatGap
            width: parent.width
            height: centre.height - y
            theme: panel.theme
            live: panel.phase >= 0
            xUnit: "ms"
            xTicks: beat.width > 0 ? panel.beatTickFractions.map(f => ({
                at: (panel.theme.beatInset + f * (beat.width - 2 * panel.theme.beatInset)) / beat.width,
                label: Format.number(f * panel.beatMs, 0) })) : []
        Rectangle {
            id: beat
            objectName: "anvilBeat"
            anchors.fill: parent
            radius: panel.theme.wellRadius
            color: panel.theme.well
            border.color: panel.theme.wellEdge
            clip: true
            Canvas {
                id: beatCanvas
                anchors.fill: parent
                renderStrategy: Canvas.Cooperative
                // The peaks reach this fraction of the well's height either side.
                readonly property real beatReach: 0.44
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const t = panel.theme
                    const L = t.beatInset, R = width - t.beatInset, mid = height / 2
                    const amp = height * beatCanvas.beatReach
                    const fill = (list, colour, alpha) => {
                        if (list.length === 0)
                            return
                        const columns = Math.max(1, Math.round(R - L))
                        ctx.beginPath()
                        ctx.moveTo(L, mid)
                        for (let c = 0; c <= columns; ++c)
                            ctx.lineTo(L + c, mid - list[Math.min(list.length - 1, Math.floor(c / columns * list.length))] * amp)
                        for (let c = columns; c >= 0; --c)
                            ctx.lineTo(L + c, mid + list[Math.min(list.length - 1, Math.floor(c / columns * list.length))] * amp)
                        ctx.closePath()
                        ctx.fillStyle = Qt.rgba(colour.r, colour.g, colour.b, alpha)
                        ctx.fill()
                    }
                    fill(panel.beatPeaks, t.beat, t.beatFillAlpha)
                    fill(panel.rumblePeaks, t.rumble, t.rumbleFillAlpha)
                    ctx.fillStyle = t.gridStrong
                    ctx.fillRect(Math.round(L + panel.kickFraction * (R - L)), 0, t.hairline, height)
                }
            }
            Connections {
                target: panel
                function onBeatPeaksChanged() { beatCanvas.requestPaint() }
                function onRumblePeaksChanged() { beatCanvas.requestPaint() }
            }
            Rectangle {
                visible: panel.phase >= 0
                x: panel.theme.beatInset + Math.min(1, panel.phase * panel.kickFraction)
                   * (beat.width - 2 * panel.theme.beatInset) - width / 2
                width: panel.theme.beatPlayheadWidth
                height: beat.height
                color: panel.theme.time
                opacity: panel.theme.beatPlayheadOpacity
            }
        }
        }
    }

    // ---- The chosen layer -----------------------------------------------------------

    Kit.Card {
        id: layerCardPanel
        objectName: "anvilLayerControls"
        x: panel.controlsX
        y: panel.bodyY
        width: panel.controlsWidth
        height: panel.bodyHeight
        theme: panel.theme
        // A layer that is off collapses to its title, its ON switch and a note.
        readonly property var chosen: panel.layerOf(panel.selectedLayer)
        readonly property bool off: chosen.power !== ""
            && panel.revision >= 0 && panel.controller.choice(chosen.power) !== 1
        title: chosen.name.toUpperCase()
        note: off ? panel.theme.offChip
            : panel.selectedLayer === "rumble" ? "TUNED" : panel.selectedLayer === "sub" ? "TAIL NOTE" : ""
        noteColor: off ? panel.theme.inkDim : chosen.color

        Kit.Toggle {
            x: parent.width - width - panel.controlsPad
            y: panel.toggleY
            width: panel.toggleWidth
            height: panel.toggleHeight
            visible: panel.selectedLayer === "sub"
            controller: panel.controller
            theme: panel.theme
            parameterId: "anvil.sub.on"
            label: "ON"
            lit: panel.theme.sub
        }
        Kit.Toggle {
            x: parent.width - width - panel.controlsPad
            y: panel.toggleY
            width: panel.toggleWidth
            height: panel.toggleHeight
            visible: panel.selectedLayer === "rumble"
            controller: panel.controller
            theme: panel.theme
            parameterId: "anvil.rumble.on"
            label: "ON"
            lit: panel.theme.rumble
        }

        // Body.
        Column {
            x: panel.controlsPad
            y: panel.controlsY
            width: parent.width - 2 * panel.controlsPad
            spacing: panel.controlsSpacing
            visible: panel.selectedLayer === "body"
            Label { text: "WAVE" }
            Drop { parameterId: "anvil.body.wave"; width: parent.width; accent: panel.theme.body }
            Label { text: "LENGTH" }
            Seg {
                parameterId: "anvil.length.mode"
                width: parent.width
                accent: panel.theme.body
                labels: ["TIME", "SYNC", "NOTE"]
            }
            Drop {
                visible: panel.lengthMode === 1
                parameterId: "anvil.length.division"
                width: parent.width
                accent: panel.theme.body
            }
            Text {
                visible: panel.lengthMode === 2
                width: parent.width
                text: "Each hit lasts as long as its note. Keys played live end when let go, within Length."
                wrapMode: Text.WordWrap
                color: panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeSmall
            }
            Grid {
                columns: panel.knobColumns
                rowSpacing: panel.theme.spaceSm
                Cell { parameterId: "anvil.tune"; accent: panel.theme.body }
                Cell {
                    parameterId: "anvil.length"
                    accent: panel.theme.body
                    inactive: panel.lengthMode === 1
                    inactiveReason: inactive ? "Length follows the Sync division" : ""
                }
                Cell { parameterId: "anvil.body.level"; accent: panel.theme.body }
                Cell { parameterId: "anvil.body.harmonics"; accent: panel.theme.body }
                Cell { parameterId: "anvil.body.phase"; accent: panel.theme.body }
                Cell { parameterId: "anvil.body.drive"; accent: panel.theme.body }
            }
        }
        // Click.
        Column {
            x: panel.controlsPad
            y: panel.controlsY
            width: parent.width - 2 * panel.controlsPad
            spacing: panel.controlsSpacing
            visible: panel.selectedLayer === "click"
            Label { text: "TRANSIENT" }
            Drop { parameterId: "anvil.click.type"; width: parent.width; accent: panel.theme.click }
            Grid {
                columns: panel.knobColumns
                rowSpacing: panel.theme.spaceSm
                Cell { parameterId: "anvil.click.level"; accent: panel.theme.click }
                Cell { parameterId: "anvil.click.tone"; accent: panel.theme.click }
                Cell { parameterId: "anvil.click.length"; accent: panel.theme.click }
                Cell { parameterId: "anvil.click.pitch"; accent: panel.theme.click }
                Cell { parameterId: "anvil.click.start"; accent: panel.theme.click }
                Cell { parameterId: "anvil.click.velocity"; accent: panel.theme.click }
            }
            // The user's own transient: drop a WAV, or type its path.
            Rectangle {
                id: transientWell
                objectName: "anvilTransient"
                width: parent.width
                height: panel.naming ? panel.transientNamingHeight : panel.transientHeight
                radius: panel.theme.radiusXl
                color: dropArea.containsDrag ? panel.theme.selected : panel.theme.well
                border.color: dropArea.containsDrag ? panel.theme.click : panel.theme.wellEdge
                Label {
                    x: panel.transientPad
                    y: panel.theme.spaceMd
                    text: "YOUR TRANSIENT"
                }
                Text {
                    objectName: "anvilTransientName"
                    x: panel.transientPad
                    y: panel.transientNameY
                    width: parent.width - 2 * panel.transientPad
                    text: panel.transientName
                    wrapMode: Text.WordWrap
                    color: panel.transientMedia.kind === "managed" ? panel.theme.click : panel.theme.inkDim
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeBody
                }
                Rectangle {
                    visible: panel.naming
                    x: panel.transientPad
                    y: panel.transientFieldY
                    width: parent.width - 2 * panel.transientPad
                    height: panel.transientFieldHeight
                    radius: panel.theme.fieldRadius
                    color: panel.theme.plate
                    border.color: panel.theme.accent
                    TextInput {
                        id: pathField
                        objectName: "anvilTransientPath"
                        x: panel.theme.spaceSm
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 2 * panel.theme.spaceSm
                        clip: true
                        color: panel.theme.ink
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeBody
                        Keys.onReturnPressed: {
                            if (panel.importTransient(text))
                                panel.naming = false
                        }
                        Keys.onEscapePressed: panel.naming = false
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !panel.naming && panel.controller.editable
                    onClicked: {
                        panel.naming = true
                        pathField.text = ""
                        pathField.forceActiveFocus()
                    }
                    Shell.Tip { text: "Drop a short WAV here, or click to type its path; it is kept in the project" }
                }
                DropArea {
                    id: dropArea
                    anchors.fill: parent
                    onDropped: drop => {
                        if (drop.hasUrls && drop.urls.length > 0)
                            panel.importTransient(drop.urls[0])
                    }
                }
            }
        }
        Text {
            objectName: "anvilLayerOffNote"
            x: panel.controlsPad
            y: panel.controlsY
            width: parent.width - 2 * panel.controlsPad
            visible: layerCardPanel.off
            text: "The " + layerCardPanel.chosen.name.toLowerCase() + " is off: switch it on to hear and shape it."
            wrapMode: Text.WordWrap
            color: panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeBody
        }
        // Sub.
        Column {
            x: panel.controlsPad
            y: panel.controlsY
            width: parent.width - 2 * panel.controlsPad
            spacing: panel.controlsSpacing
            visible: panel.selectedLayer === "sub" && !layerCardPanel.off
            Grid {
                columns: panel.knobColumns
                rowSpacing: panel.theme.spaceSm
                Cell { parameterId: "anvil.sub.level"; accent: panel.theme.sub }
                Cell { parameterId: "anvil.sub.octave"; accent: panel.theme.sub }
                Cell { parameterId: "anvil.sub.attack"; accent: panel.theme.sub }
            }
            Text {
                width: parent.width
                text: "A pure sine on the tail's note, under the body: weight on small speakers stays in tune."
                wrapMode: Text.WordWrap
                color: panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
            }
        }
        // Rumble.
        Column {
            x: panel.controlsPad
            y: panel.controlsY
            width: parent.width - 2 * panel.controlsPad
            spacing: panel.controlsSpacing
            visible: panel.selectedLayer === "rumble" && !layerCardPanel.off
            Grid {
                columns: panel.knobColumns
                rowSpacing: panel.theme.spaceSm
                Cell { parameterId: "anvil.rumble.amount"; accent: panel.theme.rumble }
                Cell { parameterId: "anvil.rumble.decay"; accent: panel.theme.rumble }
                Cell { parameterId: "anvil.rumble.duck"; accent: panel.theme.rumble }
                Cell { parameterId: "anvil.rumble.tone"; accent: panel.theme.rumble }
                Cell { parameterId: "anvil.rumble.drive"; accent: panel.theme.rumble }
                Cell { parameterId: "anvil.rumble.space"; accent: panel.theme.rumble }
                Cell { parameterId: "anvil.rumble.width"; accent: panel.theme.rumble }
            }
            Text {
                width: parent.width
                text: "The techno rumble: a resonance on the tail's note, driven, spread and ducked under every hit."
                wrapMode: Text.WordWrap
                color: panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
            }
        }
    }

    // ---- Master ---------------------------------------------------------------------

    Kit.Card {
        objectName: "anvilMaster"
        x: panel.theme.spaceXl
        y: panel.masterY
        width: panel.width - 2 * panel.theme.spaceXl
        height: panel.height - y - panel.theme.spaceXl
        theme: panel.theme
        title: "MASTER"
        note: "BODY · CLICK · SUB · RUMBLE → DRIVE → TONE → PUNCH → GLUE → LIMITER"

        Column {
            x: panel.masterPad
            y: panel.masterCaptionY
            spacing: panel.theme.spaceSm
            Label { text: "DRIVE" }
            Seg {
                parameterId: "anvil.drive.type"
                width: panel.driveChoiceWidth
            }
            Text {
                width: panel.driveChoiceWidth
                text: "Oversampled, after the layers are summed."
                color: panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
            }
        }
        Row {
            x: panel.masterKnobsX
            y: panel.masterRowY
            spacing: panel.theme.spaceSm
            Cell { parameterId: "anvil.drive.amount" }
            Cell { parameterId: "anvil.tone" }
            Cell { parameterId: "anvil.punch" }
            Cell { parameterId: "anvil.glue" }
            Cell { parameterId: "anvil.output" }
        }
        Rectangle {
            x: panel.velocityRuleX
            y: panel.masterRowY
            width: panel.theme.hairline
            height: parent.height - panel.masterRuleInset
            color: panel.theme.seam
        }
        Column {
            x: panel.velocityX
            y: panel.masterRowY
            spacing: panel.theme.spaceXxs
            Label { text: "VELOCITY" }
            Row {
                spacing: panel.theme.spaceSm
                Cell { parameterId: "anvil.velocity" }
                Cell { parameterId: "anvil.velocity.drive" }
            }
        }
        Rectangle {
            x: panel.lockRuleX
            y: panel.masterRowY
            width: panel.theme.hairline
            height: parent.height - panel.masterRuleInset
            color: panel.theme.seam
        }
        // Kick & Bass Lock (ADR-0072): the tail follows the bass.
        Column {
            objectName: "anvilLock"
            x: panel.lockX
            y: panel.masterRowY
            width: panel.lockWidth
            spacing: panel.theme.spaceXs
            Label { text: "LOCK" }
            Seg {
                id: lockChoice
                parameterId: "anvil.lock"
                width: parent.width
                labels: ["OFF", "BASS NOTE", "KEEP TONE"]
            }
            Text {
                width: parent.width
                text: "Bass note tunes each hit's tail to the bass note under it; Keep tone locks without retuning. Turn Lock on in the Nova bass too."
                wrapMode: Text.WordWrap
                color: panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
            }
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
