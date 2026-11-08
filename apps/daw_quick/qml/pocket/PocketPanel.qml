pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Pocket device panel: a precision low-end instrument in charcoal, the
// kick in amber and the bass in cyan. The masking map leads, with the kick
// and bass spectra, their held peaks, the overlap Pocket resolves and the
// cut it makes; beside it the space cleared; under it the last 0.7 s of
// kick, bass in and bass out; below, every control. Every control reads and
// writes through `controller` (NativeSynthDeviceController for Pocket); no
// project or audio state lives in QML. With nothing playing, the map shows
// what the settings would do to a typical kick and bass (ADR-0041's laws).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037, layout in pocket.hpp), polled while visible.
    property var live: panel.idle()
    property PocketTheme theme: PocketTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int revision: controller.revision
    // Levels at or below this read as silence; bare levels have one decimal.
    readonly property real silentDb: -99
    readonly property int levelDecimals: 1

    // pocket.hpp: kick, bass and cut per band (24 each), overlap before and
    // after, then three waveform histories of 256 (min, max) pairs.
    readonly property int bands: 24
    readonly property int wavePoints: 256
    readonly property int overlapAt: 3 * bands
    readonly property int waveAt: overlapAt + 2
    readonly property int frameSize: waveAt + 3 * 2 * wavePoints

    // Held peaks per band: held for holdSeconds, then falling decayDbPerSecond.
    readonly property real holdSeconds: 0.6
    readonly property real decayDbPerSecond: 14
    property var heldKick: []
    property var heldBass: []
    property var heldAge: []
    property real heldAt: 0

    readonly property bool playing: live.playing === true
    readonly property var preview: revision >= 0 ? panel.previewState() : panel.idle()
    readonly property var shown: playing ? live : preview

    readonly property int mode: revision >= 0 ? controller.choice("pocket.mode") : 0

    objectName: "pocketPanel"
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
    Accessible.name: "Pocket kick and bass resolver"

    function bandHz(b) {
        return 30 * Math.pow(8000 / 30, b / 23)
    }
    function idle() {
        return { playing: false, kick: [], bass: [], cut: [], before: -120, after: -120,
                 waveKick: [], waveIn: [], waveOut: [] }
    }
    function decode(state) {
        const frame = state.frame
        if (frame === undefined || frame.length < frameSize)
            return idle()
        const cut = []
        for (let b = 0; b < bands; ++b)
            cut.push(Math.max(0, -frame[2 * bands + b]))
        return {
            playing: true,
            kick: frame.slice(0, bands),
            bass: frame.slice(bands, 2 * bands),
            cut: cut,
            before: frame[overlapAt],
            after: frame[overlapAt + 1],
            waveKick: frame.slice(waveAt, waveAt + 2 * wavePoints),
            waveIn: frame.slice(waveAt + 2 * wavePoints, waveAt + 4 * wavePoints),
            waveOut: frame.slice(waveAt + 4 * wavePoints, waveAt + 6 * wavePoints)
        }
    }
    // Held peaks: a new peak holds, then falls; `seconds` since last update.
    function updateHeld(state, seconds) {
        if (!state.playing) {
            heldKick = []
            heldBass = []
            heldAge = []
            return
        }
        const fall = Math.max(0, seconds) * decayDbPerSecond
        const kick = []
        const bass = []
        const age = []
        for (let b = 0; b < bands; ++b) {
            const oldK = heldKick.length === bands ? heldKick[b] : -200
            const oldB = heldBass.length === bands ? heldBass[b] : -200
            const oldAge = heldAge.length === bands ? heldAge[b] : [0, 0]
            const ageK = oldAge[0] + seconds
            const ageB = oldAge[1] + seconds
            const fallK = ageK > holdSeconds ? fall : 0
            const fallB = ageB > holdSeconds ? fall : 0
            if (state.kick[b] >= oldK - fallK) {
                kick.push(state.kick[b])
            } else {
                kick.push(oldK - fallK)
            }
            if (state.bass[b] >= oldB - fallB) {
                bass.push(state.bass[b])
            } else {
                bass.push(oldB - fallB)
            }
            age.push([state.kick[b] >= oldK - fallK ? 0 : ageK, state.bass[b] >= oldB - fallB ? 0 : ageB])
        }
        heldKick = kick
        heldBass = bass
        heldAge = age
    }
    function poll() {
        const now = Date.now() / 1000
        const seconds = heldAt > 0 ? now - heldAt : 0
        heldAt = now
        live = decode(controller.telemetry())
        updateHeld(live, seconds)
    }

    // ---- The preview: a typical kick and bass through the settings ----------------

    function previewKick(hz) {
        // A body around 55 Hz and a click around 3.5 kHz.
        const body = -6 - 20 * Math.pow(Math.log2(hz / 55), 2)
        const click = -40 - 7 * Math.pow(Math.log2(hz / 3500), 2)
        return 10 * Math.log10(Math.pow(10, body / 10) + Math.pow(10, click / 10) + 1e-12)
    }
    function previewBass(hz) {
        // Strongest around 70 Hz, rolling off into the mids.
        const x = Math.log2(hz / 70)
        return x < 0 ? -8 - 16 * x * x : -8 - 5 * x - 2.4 * x * x
    }
    function focusWeight(hz) {
        const low = controller.value("pocket.focus_low")
        const high = Math.max(controller.value("pocket.focus_high"), low)
        let outside = 0
        if (hz < low)
            outside = Math.log2(low / hz)
        else if (hz > high)
            outside = Math.log2(hz / high)
        return Math.max(0, 1 - outside / 0.5)
    }
    // ADR-0041's laws on band levels, in dB of cut per band.
    function lawCuts(kick, bass) {
        const depth = controller.value("pocket.depth")
        const margin = controller.value("pocket.margin")
        const m = controller.choice("pocket.mode")
        let loudest = -200
        for (let b = 0; b < bands; ++b) {
            if (focusWeight(bandHz(b)) > 0)
                loudest = Math.max(loudest, kick[b])
        }
        const cuts = []
        for (let b = 0; b < bands; ++b) {
            const weight = focusWeight(bandHz(b))
            let need = 0
            if (weight > 0) {
                if (m === 2) {
                    need = loudest > -50 ? depth : 0
                } else if (m === 1) {
                    need = kick[b] > -50 ? depth * Math.max(0, Math.min(1, (kick[b] - (loudest - 24)) / 24)) : 0
                } else if (kick[b] > -60) {
                    const deficit = bass[b] + margin - kick[b]
                    need = deficit > depth ? depth * Math.max(0, Math.min(1, 1 - (deficit - depth) / 12))
                                           : Math.max(0, deficit)
                }
            }
            cuts.push(need * weight)
        }
        return cuts
    }
    function overlapDb(kick, bass, cut) {
        let total = 0
        for (let b = 0; b < bands; ++b) {
            if (focusWeight(bandHz(b)) <= 0)
                continue
            const k = Math.pow(10, kick[b] / 10)
            const s = Math.pow(10, (bass[b] - (cut ? cut[b] : 0)) / 10)
            total += Math.min(k, s)
        }
        return 10 * Math.log10(total + 1e-24)
    }
    function previewState() {
        const kick = []
        const bass = []
        for (let b = 0; b < bands; ++b) {
            kick.push(previewKick(bandHz(b)))
            bass.push(previewBass(bandHz(b)))
        }
        const cut = lawCuts(kick, bass)
        let deepest = 0
        for (let b = 0; b < bands; ++b) {
            if (bandHz(b) < 160)
                deepest = Math.max(deepest, cut[b])
        }
        // Waveforms: two kicks half a second apart over a steady bass that
        // dips by the cut while each kick sounds.
        const mix = controller.value("pocket.mix")
        const waveKick = []
        const waveIn = []
        const waveOut = []
        for (let i = 0; i < wavePoints; ++i) {
            const t = 0.68 * i / wavePoints
            let since = 1
            for (const hit of [0.12, 0.62]) {
                if (t >= hit)
                    since = Math.min(since, t - hit)
            }
            const k = since < 1 ? 0.78 * Math.exp(-since / 0.09) : 0
            const dip = since < 1 ? deepest * Math.exp(-since / 0.11) * mix : 0
            // Eighth-note bass notes, each with a little attack bloom.
            const b = 0.42 * (0.72 + 0.28 * Math.exp(-((t + 0.03) % 0.25) / 0.07))
            const out = b * Math.pow(10, -dip / 20)
            waveKick.push(-k, k)
            waveIn.push(-b, b)
            waveOut.push(-out, out)
        }
        return { playing: false, kick: kick, bass: bass, cut: cut,
                 before: overlapDb(kick, bass, null), after: overlapDb(kick, bass, cut),
                 waveKick: waveKick, waveIn: waveIn, waveOut: waveOut }
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
    readonly property real cleared: Math.max(0, shown.before - shown.after)
    readonly property var deepestCut: {
        let best = { db: 0, hz: 0 }
        const cut = shown.cut
        for (let b = 0; b < cut.length; ++b) {
            if (cut[b] > best.db)
                best = { db: cut[b], hz: bandHz(b) }
        }
        return best
    }

    // A graphite plate: a fine bevel along the top, the title in spaced caps.

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.cellDiameter
        property string label: ""
        property bool readout: true
        width: panel.theme.cellWidth
        height: diameter + (readout ? panel.theme.cellReadoutPad : panel.theme.cellLabelPad)
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
            text: "POCKET"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        // Amber into cyan under the name: kick and bass.
        Rectangle {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.underlineGap
            width: wordmark.implicitWidth - panel.theme.underlineTrim
            height: panel.theme.underlineHeight
            radius: panel.theme.underlineRadius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.kick }
                GradientStop { position: 1; color: panel.theme.bass }
            }
        }
        Text {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "KICK & BASS RESOLVER"
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
                objectName: "pocketPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "pocketPresetDisplay"
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
                    objectName: "pocketPresetName"
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
                    objectName: "pocketReadout"
                    x: panel.theme.readoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.keySpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "pocketReadoutValue"
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
                objectName: "pocketPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "pocketStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- The map and the meters ------------------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        theme: panel.theme
        id: mapPlate
        title: "MASKING MAP"
        note: panel.playing ? "LIVE" : "PREVIEW · A TYPICAL KICK AND BASS THROUGH THESE SETTINGS"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.width - 2 * panel.margin - panel.gap - panel.theme.metersWidth
        height: panel.rowAHeight
        // The key, top right.
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.legendMargin
            y: panel.theme.legendY
            spacing: panel.theme.legendSpacing
            Repeater {
                model: [["KICK", 0], ["BASS", 1], ["MASKING", 2], ["CUT", 3], ["HELD PEAK", 4]]
                delegate: Row {
                    id: key
                    required property var modelData
                    spacing: panel.theme.legendSwatchGap
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: key.modelData[1] === 4 ? panel.theme.legendHeldWidth : panel.theme.legendSwatch
                        height: key.modelData[1] >= 2 && key.modelData[1] <= 3 ? panel.theme.legendSwatch : panel.theme.legendLine
                        radius: panel.theme.legendSwatchRadius
                        color: key.modelData[1] === 0 ? panel.theme.kick
                            : key.modelData[1] === 1 ? panel.theme.bass
                            : key.modelData[1] === 2 ? Qt.rgba(panel.theme.masked.r, panel.theme.masked.g, panel.theme.masked.b, panel.theme.maskedKeyAlpha)
                            : key.modelData[1] === 3 ? panel.theme.cleared : panel.theme.inkDim
                    }
                    Text {
                        text: key.modelData[0]
                        color: panel.theme.inkFaint
                        font.pixelSize: panel.theme.typeMicro
                        font.bold: true
                        font.letterSpacing: panel.theme.keySpacing
                    }
                }
            }
        }
        PocketMap {
            id: maskingMap
            x: panel.theme.plotInset
            y: panel.theme.mapTop
            width: mapPlate.width - 2 * panel.theme.plotInset
            height: mapPlate.height - panel.theme.mapTop - panel.theme.plotInset
            controller: panel.controller
            theme: panel.theme
            live: panel.playing
            kick: panel.shown.kick
            bass: panel.shown.bass
            cut: panel.shown.cut
            heldKick: panel.heldKick
            heldBass: panel.heldBass
        }
    }

    Kit.Card {
        theme: panel.theme
        id: meters
        title: "SPACE CLEARED"
        x: mapPlate.x + mapPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        // The headline: how much masking Pocket removed.
        Text {
            objectName: "pocketCleared"
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.clearedY
            text: Format.number(panel.cleared, panel.levelDecimals)
            color: panel.cleared >= 0.05 ? panel.theme.cleared : panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.clearedSize
            font.weight: Font.Light
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.clearedLabelY
            text: "dB LESS MASKING IN THE FOCUS"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        // Overlap in and out, on one scale.
        Column {
            x: panel.theme.statInset
            y: panel.theme.barsY
            width: meters.width - 2 * panel.theme.statInset
            spacing: panel.theme.barGap
            component Bar: Item {
                id: bar
                required property string name
                required property real db
                required property color tone
                property string objectId: ""
                width: parent.width
                height: panel.theme.barRowHeight
                readonly property real fraction: Math.max(0, Math.min(1, (db + 72) / 72))
                Text {
                    text: bar.name
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.statSpacing
                }
                Text {
                    objectName: bar.objectId
                    anchors.right: parent.right
                    text: bar.db <= panel.silentDb ? "—" : Format.decibels(bar.db)
                    color: panel.theme.ink
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSmall
                }
                Rectangle {
                    y: panel.theme.barTrackY
                    width: parent.width
                    height: panel.theme.barTrackHeight
                    radius: panel.theme.barTrackRadius
                    color: panel.theme.well
                    border.color: panel.theme.wellEdge
                    Rectangle {
                        x: panel.theme.barFillInset
                        y: panel.theme.barFillInset
                        width: Math.max(0, (parent.width - 2 * panel.theme.barFillInset) * bar.fraction)
                        height: parent.height - 2 * panel.theme.barFillInset
                        radius: panel.theme.radiusMd
                        color: bar.tone
                        Behavior on width { NumberAnimation { duration: 120 } }
                    }
                }
            }
            Bar {
                name: "OVERLAP · BASS IN"
                db: panel.shown.before
                tone: Qt.rgba(panel.theme.masked.r, panel.theme.masked.g, panel.theme.masked.b, panel.theme.overlapFillAlpha)
                objectId: "pocketOverlapBefore"
            }
            Bar {
                name: "OVERLAP · BASS OUT"
                db: panel.shown.after
                tone: panel.theme.cleared
                objectId: "pocketOverlapAfter"
            }
        }
        Rectangle {
            x: panel.theme.statInset
            y: panel.theme.statDividerY
            width: meters.width - 2 * panel.theme.statInset
            height: panel.theme.hairline
            color: panel.theme.seam
        }
        Column {
            x: panel.theme.statInset
            y: panel.theme.statLinesY
            width: meters.width - 2 * panel.theme.statInset
            spacing: panel.theme.spaceSm
            component Line: Item {
                id: line
                required property string name
                required property string value
                property string objectId: ""
                property color tone: panel.theme.ink
                width: parent.width
                height: panel.theme.statLineHeight
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: line.name
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.statSpacing
                }
                Text {
                    objectName: line.objectId
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: line.value
                    color: line.tone
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeBody
                }
            }
            Line {
                name: "DEEPEST CUT"
                value: panel.deepestCut.db >= 0.05
                    ? Format.decibels(-panel.deepestCut.db) + " @ " + Format.hertz(panel.deepestCut.hz) : "—"
                objectId: "pocketDeepestCut"
                tone: panel.theme.cleared
            }
            Line {
                name: "LATENCY"
                value: panel.revision >= 0 ? panel.controller.display("pocket.lookahead") : ""
                objectId: "pocketLatency"
            }
            Line {
                name: "MODE"
                value: panel.revision >= 0 ? panel.controller.display("pocket.mode").toUpperCase() : ""
            }
        }
        Text {
            objectName: "pocketModeDescription"
            x: panel.theme.statInset
            y: panel.theme.modeNoteY
            width: meters.width - 2 * panel.theme.statInset
            text: panel.mode === 2
                ? "The whole focus ducks with the kick: the classic sidechain pump."
                : panel.mode === 1
                    ? "Each band of the bass ducks with the kick's energy there."
                    : "Cuts the bass only where it hides the kick, by just enough to let the kick through."
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    // ---- Waveforms -------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: theme.rowBHeight

    Kit.Card {
        theme: panel.theme
        id: wavePlate
        title: "KICK · BASS IN · BASS OUT"
        note: panel.playing ? "LAST 0.7 s" : "PREVIEW"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowBY
        width: panel.width - 2 * panel.margin
        height: panel.rowBHeight
        PocketWaves {
            x: panel.theme.plotInset
            y: panel.theme.wavesTop
            width: wavePlate.width - 2 * panel.theme.plotInset
            height: wavePlate.height - panel.theme.wavesTop - panel.theme.plotInset
            theme: panel.theme
            live: panel.playing
            kick: panel.shown.waveKick
            bassIn: panel.shown.waveIn
            bassOut: panel.shown.waveOut
        }
    }

    // ---- Controls --------------------------------------------------------------------

    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: panel.height - rowCY - margin

    Kit.Card {
        theme: panel.theme
        id: modePlate
        title: "MODE"
        x: panel.margin
        y: panel.rowCY
        width: panel.theme.modeWidth
        height: panel.rowCHeight
        Kit.Segment {
            x: panel.theme.spaceLg
            y: panel.theme.controlY
            width: modePlate.width - 2 * panel.theme.spaceLg
            height: modePlate.height - panel.theme.controlY - panel.theme.spaceLg
            vertical: true
            controller: panel.controller
            theme: panel.theme
            parameterId: "pocket.mode"
            labels: ["UNMASK", "DUCK", "PUMP"]
            captions: ["CUT ONLY WHAT HIDES THE KICK", "FOLLOW THE KICK'S SPECTRUM", "CLASSIC SIDECHAIN"]
        }
    }

    Kit.Card {
        theme: panel.theme
        id: duck
        title: "DUCK"
        x: modePlate.x + modePlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.duckWidth
        height: panel.rowCHeight
        Row {
            x: panel.theme.knobRowX
            y: panel.theme.knobRowY
            Cell {
                parameterId: "pocket.depth"
                diameter: panel.theme.duckKnobDiameter
            }
            Cell {
                parameterId: "pocket.margin"
                diameter: panel.theme.duckKnobDiameter
                visible: panel.mode === 0
            }
        }
        Text {
            visible: panel.mode !== 0
            x: panel.theme.duckNoteX
            y: panel.theme.duckNoteY
            width: panel.theme.duckNoteWidth
            text: "Margin applies to Unmask."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        theme: panel.theme
        id: focusPlate
        title: "FOCUS"
        note: "OR DRAG MAP"
        x: duck.x + duck.width + panel.gap
        y: panel.rowCY
        width: panel.theme.focusWidth
        height: panel.rowCHeight
        Row {
            x: panel.theme.knobRowX
            y: panel.theme.knobRowY
            Cell {
                parameterId: "pocket.focus_low"
                label: "Low"
                diameter: panel.theme.focusKnobDiameter
            }
            Cell {
                parameterId: "pocket.focus_high"
                label: "High"
                diameter: panel.theme.focusKnobDiameter
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: timing
        title: "TIMING"
        x: focusPlate.x + focusPlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.timingWidth
        height: panel.rowCHeight
        readonly property bool following: panel.revision >= 0 && panel.controller.choice("pocket.follow") === 1
        Row {
            x: panel.theme.timingKnobX
            y: panel.theme.knobRowY
            Cell {
                parameterId: "pocket.attack"
                diameter: panel.theme.timingKnobDiameter
            }
            Cell {
                parameterId: "pocket.release"
                diameter: panel.theme.timingKnobDiameter
                opacity: timing.following ? panel.theme.unusedOpacity : 1
            }
        }
        Kit.Toggle {
            x: panel.theme.timingControlsX
            y: panel.theme.controlY
            width: timing.width - panel.theme.timingControlsX - panel.theme.spaceLg
            controller: panel.controller
            theme: panel.theme
            parameterId: "pocket.follow"
            label: "Follow Kick"
            onCaption: "RELEASE WITH THE KICK"
            offCaption: "RELEASE KNOB"
        }
        Caption {
            x: panel.theme.timingControlsX
            y: panel.theme.lookaheadCaptionY
            text: "LOOKAHEAD · LATENCY ms"
        }
        Kit.Segment {
            x: panel.theme.timingControlsX
            y: panel.theme.lookaheadY
            width: timing.width - panel.theme.timingControlsX - panel.theme.spaceLg
            height: panel.theme.lookaheadHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "pocket.lookahead"
            labels: ["OFF", "5", "10", "20", "40"]
        }
    }

    Kit.Card {
        theme: panel.theme
        id: sub
        title: "SUB"
        x: timing.x + timing.width + panel.gap
        y: panel.rowCY
        width: panel.theme.subWidth
        height: panel.rowCHeight
        Kit.Toggle {
            x: panel.theme.spaceLg
            y: panel.theme.controlY
            width: sub.width - 2 * panel.theme.spaceLg
            controller: panel.controller
            theme: panel.theme
            parameterId: "pocket.mono_sub"
            label: "Mono Sub"
            lit: panel.theme.bass
            onCaption: "MONO BELOW"
            offCaption: "FULL-RANGE SIDE"
        }
        Cell {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.subKnobY
            parameterId: "pocket.mono_freq"
            label: "Below"
            diameter: panel.theme.subKnobDiameter
            opacity: panel.revision >= 0 && panel.controller.choice("pocket.mono_sub") === 1 ? 1 : panel.theme.unusedOpacity
        }
    }

    Kit.Card {
        theme: panel.theme
        id: output
        title: "OUTPUT"
        x: sub.x + sub.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.knobRowY
            Cell {
                parameterId: "pocket.mix"
                diameter: panel.theme.outputKnobDiameter
            }
            Cell {
                parameterId: "pocket.output"
                diameter: panel.theme.outputKnobDiameter
            }
        }
    }

    Timer {
        objectName: "pocketTelemetryPoll"
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
