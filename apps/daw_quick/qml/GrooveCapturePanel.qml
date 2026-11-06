pragma ComponentBehavior: Bound
import QtQuick
import "mixcheck"
import "devicekit" as Kit
import "shell"

// Groove from a record (ADR-0079):
// take the timing and accents of one band of a record, or of an audio clip
// in the project, and keep them in the groove pool. Left: the reference,
// the band, how many bars and the grid, then Take groove. Right: the
// fingerprint (a bar of the record with its hits, each step's push or drag
// with its spread and every hit folded onto it, and each step's level), the
// figures in plain words and an A/B of straight hats against grooved ones.
// Below: save to the pool and apply to the selected MIDI clips.
// `controller` is projectBridge.grooveCapture; Esc or ✕ closes.
Rectangle {
    id: panel
    objectName: "grooveCapturePanel"
    required property var bridge
    readonly property var gc: bridge.grooveCapture
    readonly property var noteTools: bridge.noteTools
    readonly property MixCheckTheme style: MixCheckTheme {}

    readonly property var result: style.map(gc.result)
    readonly property var stepsData: style.list(result.steps)
    readonly property var folded: style.list(result.folded)
    readonly property var barPeaks: style.list(result.barPeaks)
    readonly property var barOnsets: style.list(result.barOnsets)
    readonly property int stepsPerQuarter: Number(result.stepsPerQuarter) || gc.stepsPerQuarter
    readonly property real stepMs: Number(result.stepMs) || 0
    // The step under the mouse in the fingerprint, or -1.
    property int hotStep: -1
    // "A" (straight) or "B" (grooved): the audition playing.
    property string playing: ""

    // The grid choices: 16 steps of 16ths (a bar), 32 of 16ths (two bars),
    // 32 of 32nds (a bar).
    readonly property var gridOptions: [
        { id: "16x4", label: "16 · 1 bar", steps: 16, spq: 4, tip: "A bar of 16ths" },
        { id: "32x4", label: "32 · 2 bars", steps: 32, spq: 4,
          tip: "Two bars of 16ths: catches a part that changes in its second bar" },
        { id: "32x8", label: "32nds · 1 bar", steps: 32, spq: 8, tip: "A bar of 32nds, for rolls and flams" }
    ]
    readonly property string gridId: gc.steps + "x" + gc.stepsPerQuarter
    readonly property var barChoices: [4, 8, 16, 32]

    // The selected clips that are MIDI clips.
    readonly property var midiTargets: {
        const ids = style.list(bridge.selectedClipIds)
        const out = []
        const tracks = style.list(bridge.tracks)
        for (let t = 0; t < tracks.length; ++t) {
            const clips = style.list(tracks[t].clips)
            for (let c = 0; c < clips.length; ++c)
                if (clips[c].kind === "midi" && ids.indexOf(clips[c].id) >= 0)
                    out.push(clips[c].id)
        }
        return out
    }

    function close() {
        stopAudition()
        gc.open = false
    }
    function percent(value) { return Math.round(Number(value) * 100) + " %" }
    function signedMs(value) {
        const v = Number(value)
        if (!isFinite(v))
            return "—"
        return (v > 0.05 ? "+" : v < -0.05 ? "−" : "") + Math.abs(v).toFixed(1) + " ms"
    }
    function audition(which) {
        const path = which === "A" ? gc.auditionStraight : gc.auditionGrooved
        if (path.length === 0)
            return
        if (playing === which) {
            stopAudition()
            return
        }
        bridge.browserPreview.preview_sample("file:" + path)
        playing = which
    }
    function stopAudition() {
        if (playing.length > 0)
            bridge.browserPreview.stop()
        playing = ""
    }
    function applyToSelected() {
        if (!gc.save())
            return
        for (let i = 0; i < midiTargets.length; ++i)
            noteTools.apply_current_groove(midiTargets[i], [])
    }

    anchors.fill: parent
    color: style.alpha(style.canvas, style.scrimAlpha)
    focus: true
    Keys.onEscapePressed: close()
    Reveal { target: card; fromScale: Motion.popScale }
    // A click outside the card closes it.
    MouseArea { anchors.fill: parent; onClicked: panel.close() }
    Connections {
        target: panel.gc
        function onResultChanged() { panel.stopAudition() }
    }

    Rectangle {
        id: card
        objectName: "grooveCaptureCard"
        width: Math.min(panel.style.grooveCardMaxWidth, panel.width - 2 * panel.style.space2xl)
        height: Math.min(content.implicitHeight + 2 * panel.style.grooveCardPadding,
                         panel.height - 2 * panel.style.space2xl)
        anchors.centerIn: parent
        radius: panel.style.grooveCardRadius
        color: panel.style.canvas
        border.color: panel.style.lineStrong
        // Clicks on the card stay on it.
        MouseArea { anchors.fill: parent }

        Flickable {
            anchors.fill: parent
            anchors.margins: panel.style.grooveCardPadding
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: content
                width: parent.width
                spacing: panel.style.gap

                // Title row.
                Item {
                    width: parent.width
                    height: panel.style.grooveHeaderHeight
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "GROOVE FROM A RECORD"
                        color: panel.style.textMain
                        font.family: panel.style.sansFamily
                        font.pixelSize: panel.style.typeHeading
                        font.weight: Font.DemiBold
                        font.letterSpacing: panel.style.grooveTitleSpacing
                    }
                    Text {
                        objectName: "grooveCaptureClose"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"
                        color: panel.style.textDim
                        font.pixelSize: panel.style.typeSubtitle
                        Tip { text: "Close — Esc" }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -panel.style.closeHitOutset
                            onClicked: panel.close()
                        }
                    }
                }

                Row {
                    width: parent.width
                    spacing: panel.style.gap

                    // The reference and how to listen.
                    MixCheckCard {
                        id: setup
                        style: panel.style
                        width: panel.style.grooveSetupWidth
                        Text {
                            text: "REFERENCE"
                            color: panel.style.textDim
                            font.pixelSize: panel.style.typeSmall; font.weight: Font.DemiBold
                            font.letterSpacing: panel.style.capsLetterSpacing
                        }
                        Text {
                            width: setup.innerWidth
                            text: panel.gc.sourceName.length > 0 ? panel.gc.sourceName : "Choose a clip or a record"
                            color: panel.gc.sourceName.length > 0 ? panel.style.accent : panel.style.textFaint
                            elide: Text.ElideMiddle
                            font.pixelSize: panel.style.typeEmphasis; font.weight: Font.DemiBold
                        }
                        // Clips in the project, then the sample library.
                        Rectangle {
                            width: setup.innerWidth
                            height: panel.style.grooveSourceListHeight
                            radius: panel.style.radiusLg
                            color: panel.style.wellFill
                            border.color: panel.style.line
                            ListView {
                                id: sourceList
                                objectName: "grooveCaptureSources"
                                anchors.fill: parent
                                anchors.margins: panel.style.spaceXs
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                model: {
                                    const rows = []
                                    const clips = panel.style.list(panel.gc.clips)
                                    for (let i = 0; i < clips.length; ++i)
                                        rows.push({ key: "clip:" + clips[i].id, name: clips[i].name,
                                                    detail: clips[i].track.length > 0 ? clips[i].track : "Clip",
                                                    clip: clips[i].id, path: "" })
                                    const samples = panel.style.list(panel.bridge.browserPreview.samples)
                                    for (let i = 0; i < samples.length; ++i)
                                        rows.push({ key: "file:" + samples[i].path, name: samples[i].presetName,
                                                    detail: samples[i].kind, clip: "", path: samples[i].path })
                                    return rows
                                }
                                delegate: Rectangle {
                                    id: sourceRow
                                    required property var modelData
                                    readonly property bool chosen: panel.gc.source === modelData.key
                                    width: sourceList.width
                                    height: panel.style.grooveSourceRowHeight
                                    radius: panel.style.radiusMd
                                    color: chosen ? panel.style.alpha(panel.style.accent, panel.style.grooveChosenAlpha)
                                         : rowHover.containsMouse ? panel.style.panelRaised : "transparent"
                                    Accessible.role: Accessible.ListItem
                                    Accessible.name: modelData.name
                                    Text {
                                        x: panel.style.spaceSm; width: parent.width - panel.style.grooveSourceDetailReserve
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: (sourceRow.modelData.clip.length > 0 ? "◧ " : "◉ ") + sourceRow.modelData.name
                                        color: sourceRow.chosen ? panel.style.accent : panel.style.textMain
                                        elide: Text.ElideRight
                                        font.pixelSize: panel.style.typeBody
                                    }
                                    Text {
                                        anchors.right: parent.right; anchors.rightMargin: panel.style.spaceSm
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: panel.style.grooveSourceDetailWidth
                                        horizontalAlignment: Text.AlignRight
                                        text: sourceRow.modelData.detail
                                        color: panel.style.textFaint
                                        elide: Text.ElideRight
                                        font.pixelSize: panel.style.typeCaption
                                    }
                                    MouseArea {
                                        id: rowHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: {
                                            if (sourceRow.modelData.clip.length > 0)
                                                panel.gc.use_clip(sourceRow.modelData.clip)
                                            else
                                                panel.gc.use_file(sourceRow.modelData.path)
                                        }
                                    }
                                }
                                Text {
                                    visible: sourceList.count === 0
                                    anchors.centerIn: parent
                                    text: "No audio clips or samples yet"
                                    color: panel.style.textFaint
                                    font.pixelSize: panel.style.typeBody
                                }
                            }
                        }
                        Kit.Button {
                            objectName: "grooveCaptureChooseFile"
                            theme: panel.style
                            label: "Choose a record…"
                            tip: "Take the groove from any WAV, AIFF, FLAC or MP3"
                            onClicked: panel.gc.choose_file()
                        }
                        Text {
                            text: "LISTEN TO"
                            color: panel.style.textDim
                            font.pixelSize: panel.style.typeSmall; font.weight: Font.DemiBold
                            font.letterSpacing: panel.style.capsLetterSpacing
                        }
                        Kit.SegmentedControl {
                            theme: panel.style
                            namePrefix: "grooveCaptureBand"
                            accessibleName: "Band"
                            current: String(panel.gc.band)
                            options: [
                                { id: "0", label: "All", tip: "Every hit, whatever its pitch" },
                                { id: "1", label: "Kick", tip: "Below 150 Hz" },
                                { id: "2", label: "Perc", tip: "300 Hz to 3 kHz: claps, rims, toms" },
                                { id: "3", label: "Hats", tip: "Above 6 kHz" }
                            ]
                            onPicked: id => panel.gc.band = Number(id)
                        }
                        Text {
                            text: "AVERAGE OVER"
                            color: panel.style.textDim
                            font.pixelSize: panel.style.typeSmall; font.weight: Font.DemiBold
                            font.letterSpacing: panel.style.capsLetterSpacing
                        }
                        Kit.SegmentedControl {
                            theme: panel.style
                            namePrefix: "grooveCaptureBars"
                            accessibleName: "Bars"
                            current: String(panel.gc.bars)
                            options: panel.barChoices.map(bars => ({ id: String(bars), label: bars + " bars",
                                                                     tip: "Average every step over " + bars + " bars" }))
                            onPicked: id => panel.gc.bars = Number(id)
                        }
                        Text {
                            text: "STEPS"
                            color: panel.style.textDim
                            font.pixelSize: panel.style.typeSmall; font.weight: Font.DemiBold
                            font.letterSpacing: panel.style.capsLetterSpacing
                        }
                        Kit.SegmentedControl {
                            theme: panel.style
                            namePrefix: "grooveCaptureGrid"
                            accessibleName: "Steps"
                            current: panel.gridId
                            options: panel.gridOptions
                            onPicked: id => {
                                const chosen = panel.gridOptions.find(option => option.id === id)
                                panel.gc.steps = chosen.steps
                                panel.gc.stepsPerQuarter = chosen.spq
                            }
                        }
                        // A record's tempo: found unless typed.
                        Row {
                            visible: panel.gc.sourceIsFile
                            spacing: panel.style.spaceMd
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "TEMPO"
                                color: panel.style.textDim
                                font.pixelSize: panel.style.typeSmall; font.weight: Font.DemiBold
                                font.letterSpacing: panel.style.capsLetterSpacing
                            }
                            Rectangle {
                                width: panel.style.grooveFieldWidth; height: panel.style.grooveFieldHeight
                                radius: panel.style.grooveFieldRadius
                                color: panel.style.wellFill
                                border.color: bpmInput.activeFocus ? panel.style.accent : panel.style.line
                                TextInput {
                                    id: bpmInput
                                    objectName: "grooveCaptureBpm"
                                    anchors.fill: parent
                                    anchors.margins: panel.style.spaceSm
                                    color: panel.style.textMain
                                    font.pixelSize: panel.style.typeBody
                                    font.family: panel.style.monoFamily
                                    text: panel.gc.bpm > 0 ? String(panel.gc.bpm) : ""
                                    validator: DoubleValidator { bottom: 0; top: 400; decimals: 2; locale: "C" }
                                    onEditingFinished: panel.gc.bpm = text.length > 0 ? Number(text) : 0
                                    Text {
                                        visible: bpmInput.text.length === 0
                                        text: "auto"
                                        color: panel.style.textFaint
                                        font: bpmInput.font
                                    }
                                }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "BPM · from its name or its kick"
                                color: panel.style.textFaint
                                font.pixelSize: panel.style.typeSmall
                            }
                        }
                        Row {
                            spacing: panel.style.grooveButtonGap
                            Kit.Button {
                                objectName: "grooveCaptureTake"
                                theme: panel.style
                                prime: true
                                large: true
                                enabled: panel.gc.source.length > 0 && !panel.gc.running
                                label: panel.gc.running ? "Listening…" : "Take groove"
                                tip: "Measure the " + panel.gc.bandNames[panel.gc.band].toLowerCase()
                                     + " over " + panel.gc.bars + " bars"
                                onClicked: panel.gc.take()
                            }
                            Kit.Button {
                                visible: panel.gc.running
                                anchors.verticalCenter: parent.verticalCenter
                                theme: panel.style
                                label: "Stop"
                                onClicked: panel.gc.cancel()
                            }
                        }
                        // While it listens: a bar that runs back and forth.
                        Rectangle {
                            visible: panel.gc.running
                            width: setup.innerWidth; height: panel.style.progressBarHeight
                            radius: panel.style.progressBarHeight / 2
                            color: panel.style.line
                            clip: true
                            Rectangle {
                                id: runner
                                width: parent.width * panel.style.grooveRunnerShare; height: parent.height
                                radius: panel.style.progressBarHeight / 2
                                color: panel.style.accent
                                SequentialAnimation on x {
                                    running: panel.gc.running
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 0; to: setup.innerWidth - runner.width
                                                      duration: panel.style.grooveRunnerSweep; easing.type: Easing.InOutSine }
                                    NumberAnimation { from: setup.innerWidth - runner.width; to: 0
                                                      duration: panel.style.grooveRunnerSweep; easing.type: Easing.InOutSine }
                                }
                            }
                        }
                        Text {
                            width: setup.innerWidth
                            visible: text.length > 0
                            text: panel.gc.running ? panel.gc.stage : panel.gc.status
                            color: panel.style.textDim
                            wrapMode: Text.Wrap
                            font.pixelSize: panel.style.typeBody
                        }
                    }

                    // The fingerprint.
                    MixCheckCard {
                        id: fingerprintCard
                        style: panel.style
                        width: parent.width - setup.width - panel.style.gap
                        Item {
                            width: fingerprintCard.innerWidth
                            height: panel.style.grooveHeaderHeight
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "FINGERPRINT"
                                color: panel.style.textMain
                                font.pixelSize: panel.style.typeEmphasis; font.weight: Font.DemiBold
                                font.letterSpacing: panel.style.capsLetterSpacing
                            }
                            Row {
                                anchors.right: parent.right
                                spacing: panel.style.spaceSm
                                Kit.Button {
                                    objectName: "grooveCaptureAuditionA"
                                    theme: panel.style
                                    enabled: panel.gc.auditionStraight.length > 0
                                    done: panel.playing === "A"
                                    label: (panel.playing === "A" ? "■" : "▶") + " A · straight"
                                    tip: "Four bars of straight hats over a kick"
                                    onClicked: panel.audition("A")
                                }
                                Kit.Button {
                                    objectName: "grooveCaptureAuditionB"
                                    theme: panel.style
                                    enabled: panel.gc.auditionGrooved.length > 0
                                    done: panel.playing === "B"
                                    label: (panel.playing === "B" ? "■" : "▶") + " B · grooved"
                                    tip: "The same hats played through this groove's timing and accents"
                                    onClicked: panel.audition("B")
                                }
                            }
                        }
                        // The figures.
                        Row {
                            spacing: panel.style.grooveFactSpacing
                            visible: panel.gc.hasResult
                            Repeater {
                                model: [
                                    { label: "TEMPO", value: (Number(panel.result.bpm) || 0).toFixed(1) + " BPM" },
                                    { label: "SWING", value: (Number(panel.result.swing) || 50).toFixed(1) + " %" },
                                    { label: "OFFBEATS", value: panel.signedMs(panel.result.offbeatMs) },
                                    { label: "ACCENTS", value: String(panel.result.accents || 0) },
                                    { label: "HITS", value: String(panel.result.hits || 0) },
                                    { label: "CONFIDENCE", value: panel.percent(panel.result.confidence || 0),
                                      warn: Number(panel.result.confidence) < 0.35 }
                                ]
                                delegate: Column {
                                    id: fact
                                    required property var modelData
                                    spacing: panel.style.spaceXxs
                                    Text {
                                        text: fact.modelData.label
                                        color: panel.style.textFaint
                                        font.pixelSize: panel.style.typeCaption; font.weight: Font.DemiBold
                                        font.letterSpacing: panel.style.capsLetterSpacing
                                    }
                                    Text {
                                        text: fact.modelData.value
                                        color: fact.modelData.warn === true ? panel.style.amber : panel.style.textMain
                                        font.family: panel.style.monoFamily
                                        font.pixelSize: panel.style.typeHeading
                                    }
                                }
                            }
                        }
                        Rectangle {
                            width: fingerprintCard.innerWidth
                            height: panel.style.grooveFingerprintHeight
                            radius: panel.style.radiusLg
                            color: panel.style.wellFill
                            border.color: panel.style.line
                            Text {
                                visible: !panel.gc.hasResult
                                anchors.centerIn: parent
                                width: parent.width - 2 * panel.style.grooveHintInset
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                text: "Choose a reference and take its groove: a bar of it shows here with its hits, "
                                      + "every step's push or drag and how loud it is."
                                color: panel.style.textFaint
                                font.pixelSize: panel.style.typeLabel
                            }
                            Canvas {
                                id: fingerprint
                                objectName: "grooveCaptureFingerprint"
                                anchors.fill: parent
                                anchors.margins: panel.style.spaceMd
                                visible: panel.gc.hasResult
                                renderTarget: Canvas.Image
                                readonly property int count: panel.stepsData.length
                                // Rows: the bar's waveform, timing, level.
                                readonly property real waveTop: 0
                                readonly property real waveHeight: 62
                                readonly property real timingTop: 78
                                readonly property real timingHeight: 140
                                readonly property real levelTop: 232
                                readonly property real levelHeight: height - levelTop
                                // Timing scale: the least and most half a row may span (in
                                // steps), and the headroom over the widest step.
                                readonly property real minTimingRange: 0.12
                                readonly property real maxTimingRange: 0.5
                                readonly property real timingHeadroom: 1.15
                                // A step pushed or dragged less than this (in steps) is on time.
                                readonly property real onTimeBand: 0.01
                                // Folded hits scatter across their step so they don't stack:
                                // jitterSpan slots visited with stride jitterStride, each
                                // 1/jitterDivisor of a step apart.
                                readonly property int jitterStride: 7
                                readonly property int jitterSpan: 11
                                readonly property int jitterDivisor: 40
                                // Timing and level bars: their share of a step, least width
                                // and least height; the spread whisker's cap half-width.
                                readonly property real timingBarShare: 0.36
                                readonly property real levelBarShare: 0.5
                                readonly property real minBarWidth: 3
                                readonly property real minBarHeight: 1.5
                                readonly property real whiskerCap: 3
                                // Steps at least this wide label their push in ms, this far
                                // under the timing row.
                                readonly property real msLabelMinStep: 26
                                readonly property real msLabelDrop: 10
                                // A step at least this loud is drawn as an accent.
                                readonly property real accentLevel: 1.15
                                // The row names' inset and baseline.
                                readonly property real rowNameInset: 4
                                readonly property real rowNameBaseline: 10
                                // Timing scale: half a row is this share of a step.
                                readonly property real timingRange: {
                                    let most = minTimingRange
                                    for (let i = 0; i < panel.stepsData.length; ++i) {
                                        const s = panel.stepsData[i]
                                        most = Math.max(most, Math.abs(Number(s.timing)) + Number(s.spread))
                                    }
                                    return Math.min(maxTimingRange, most * timingHeadroom)
                                }
                                function column(step) { return width * (step + 0.5) / Math.max(1, count) }
                                onPaint: {
                                    const ctx = getContext("2d")
                                    ctx.reset()
                                    const st = panel.style
                                    const n = Math.max(1, count)
                                    const stepWidth = width / n
                                    const perBar = panel.stepsPerQuarter * 4
                                    // Grid lines: steps faint, beats stronger, bars strongest.
                                    for (let i = 0; i <= n; ++i) {
                                        const x = Math.round(i * stepWidth) + st.hairline / 2
                                        ctx.strokeStyle = i % perBar === 0 ? st.lineStrong
                                            : i % panel.stepsPerQuarter === 0 ? st.line : st.alpha(st.line, st.grooveStepLineAlpha)
                                        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke()
                                    }
                                    // The bar's waveform (one bar across the steps of one bar).
                                    const barWidth = width * Math.min(1, perBar / n)
                                    const peaks = panel.barPeaks
                                    const mid = waveTop + waveHeight / 2
                                    ctx.fillStyle = st.alpha(st.textDim, st.grooveWaveAlpha)
                                    for (let p = 0; p < peaks.length; ++p) {
                                        const x = barWidth * p / peaks.length
                                        const h = Number(peaks[p]) * waveHeight / 2
                                        ctx.fillRect(x, mid - h, Math.max(st.hairline, barWidth / peaks.length), 2 * h)
                                    }
                                    ctx.strokeStyle = st.accent
                                    ctx.lineWidth = st.grooveOnsetWidth
                                    for (let o = 0; o < panel.barOnsets.length; ++o) {
                                        const x = barWidth * Number(panel.barOnsets[o])
                                        ctx.beginPath(); ctx.moveTo(x, waveTop); ctx.lineTo(x, waveTop + waveHeight); ctx.stroke()
                                    }
                                    ctx.lineWidth = st.hairline
                                    // Timing: zero line, each hit folded on its step, the mean and spread.
                                    const zero = timingTop + timingHeight / 2
                                    const scale = (timingHeight / 2) / timingRange
                                    ctx.strokeStyle = st.lineStrong
                                    ctx.beginPath(); ctx.moveTo(0, zero); ctx.lineTo(width, zero); ctx.stroke()
                                    for (let h = 0; h < panel.folded.length; ++h) {
                                        const hit = panel.folded[h]
                                        const step = Math.round(Number(hit.position))
                                        const offset = Number(hit.position) - step
                                        const x = column(((step % n) + n) % n) + (((h * jitterStride) % jitterSpan) - Math.floor(jitterSpan / 2)) * stepWidth / jitterDivisor
                                        const y = zero - Math.max(-timingHeight / 2, Math.min(timingHeight / 2, offset * scale))
                                        ctx.fillStyle = st.alpha(st.textMain, st.grooveHitAlpha
                                                              + st.grooveHitStrengthAlpha * Math.min(1, Number(hit.strength)))
                                        ctx.fillRect(x - st.grooveHitDot / 2, y - st.grooveHitDot / 2, st.grooveHitDot, st.grooveHitDot)
                                    }
                                    for (let i = 0; i < panel.stepsData.length; ++i) {
                                        const s = panel.stepsData[i]
                                        if (Number(s.hits) === 0)
                                            continue
                                        const x = column(i)
                                        const t = Number(s.timing)
                                        const colour = t > onTimeBand ? st.amber : t < -onTimeBand ? st.blue : st.textDim
                                        const barW = Math.max(minBarWidth, stepWidth * timingBarShare)
                                        ctx.fillStyle = i === panel.hotStep ? st.textMain : colour
                                        const y = zero - t * scale
                                        ctx.fillRect(x - barW / 2, Math.min(zero, y), barW, Math.max(minBarHeight, Math.abs(y - zero)))
                                        // The spread's whisker.
                                        const spread = Number(s.spread) * scale
                                        ctx.strokeStyle = st.alpha(colour, st.grooveWhiskerAlpha)
                                        ctx.beginPath()
                                        ctx.moveTo(x, y - spread); ctx.lineTo(x, y + spread)
                                        ctx.moveTo(x - whiskerCap, y - spread); ctx.lineTo(x + whiskerCap, y - spread)
                                        ctx.moveTo(x - whiskerCap, y + spread); ctx.lineTo(x + whiskerCap, y + spread)
                                        ctx.stroke()
                                        if (stepWidth >= msLabelMinStep && Math.abs(Number(s.timingMs)) >= 1) {
                                            ctx.fillStyle = colour
                                            ctx.font = st.typeCaption + "px '" + st.monoFamily + "'"
                                            ctx.textAlign = "center"
                                            ctx.fillText((Number(s.timingMs) > 0 ? "+" : "−")
                                                         + Math.abs(Number(s.timingMs)).toFixed(0),
                                                         x, timingTop + timingHeight + msLabelDrop)
                                        }
                                    }
                                    // Level: height is the step's accent (1 is a third of the row).
                                    for (let i = 0; i < panel.stepsData.length; ++i) {
                                        const s = panel.stepsData[i]
                                        if (Number(s.hits) === 0)
                                            continue
                                        const v = Number(s.velocity)
                                        const h = Math.min(levelHeight, levelHeight * v / 2)
                                        const barW = Math.max(minBarWidth, stepWidth * levelBarShare)
                                        ctx.fillStyle = v >= accentLevel ? st.accent : st.alpha(st.accent, st.grooveLevelAlpha)
                                        ctx.fillRect(column(i) - barW / 2, levelTop + levelHeight - h, barW, h)
                                    }
                                    // Row names.
                                    ctx.fillStyle = st.textFaint
                                    ctx.font = st.typeCaption + "px '" + st.sansFamily + "'"
                                    ctx.textAlign = "left"
                                    ctx.fillText("A BAR", rowNameInset, waveTop + rowNameBaseline)
                                    ctx.fillText("LATE ↑ EARLY ↓", rowNameInset, timingTop + rowNameBaseline)
                                    ctx.fillText("LEVEL", rowNameInset, levelTop + rowNameBaseline)
                                }
                                Connections {
                                    target: panel.gc
                                    function onResultChanged() { fingerprint.requestPaint() }
                                }
                                onWidthChanged: requestPaint()
                                onVisibleChanged: if (visible) requestPaint()
                                MouseArea {
                                    id: fingerprintHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onPositionChanged: mouse => {
                                        const step = Math.floor(mouse.x / (fingerprint.width / Math.max(1, fingerprint.count)))
                                        if (step !== panel.hotStep) {
                                            panel.hotStep = step
                                            fingerprint.requestPaint()
                                        }
                                    }
                                    onExited: { panel.hotStep = -1; fingerprint.requestPaint() }
                                }
                                // The step under the mouse in words.
                                Rectangle {
                                    visible: panel.hotStep >= 0 && panel.hotStep < panel.stepsData.length
                                    readonly property var s: visible ? panel.stepsData[panel.hotStep] : ({})
                                    x: Math.min(fingerprint.width - width, Math.max(0, fingerprint.column(panel.hotStep) - width / 2))
                                    y: fingerprint.timingTop - panel.style.grooveTipLift
                                    width: hotText.implicitWidth + panel.style.grooveTipPaddingX
                                    height: hotText.implicitHeight + panel.style.grooveTipPaddingY
                                    radius: panel.style.grooveTipRadius
                                    color: panel.style.labelBox
                                    border.color: panel.style.line
                                    Text {
                                        id: hotText
                                        anchors.centerIn: parent
                                        color: panel.style.textMain
                                        font.pixelSize: panel.style.typeSmall
                                        font.family: panel.style.monoFamily
                                        text: {
                                            const s = parent.s
                                            if (!s || Number(s.hits) === 0)
                                                return "Step " + (panel.hotStep + 1) + " · no hits"
                                            return "Step " + (panel.hotStep + 1) + " · " + panel.signedMs(s.timingMs)
                                                   + " ± " + Number(s.spreadMs).toFixed(1) + " ms · level "
                                                   + Number(s.velocity).toFixed(2) + " · " + s.hits + " hits"
                                        }
                                    }
                                }
                            }
                        }
                        Text {
                            width: fingerprintCard.innerWidth
                            visible: panel.gc.hasResult
                            text: panel.style.text(panel.result.summary)
                            color: panel.style.textMain
                            wrapMode: Text.Wrap
                            font.pixelSize: panel.style.typeEmphasis
                        }
                    }
                }

                Row {
                    width: parent.width
                    spacing: panel.style.gap

                    // Save to the pool.
                    MixCheckCard {
                        id: saveCard
                        style: panel.style
                        width: (parent.width - panel.style.gap) / 2
                        Text {
                            text: "SAVE TO THE GROOVE POOL"
                            color: panel.style.textMain
                            font.pixelSize: panel.style.typeLabel; font.weight: Font.DemiBold
                            font.letterSpacing: panel.style.capsLetterSpacing
                        }
                        Row {
                            id: saveRow
                            spacing: panel.style.spaceMd
                            Rectangle {
                                width: saveCard.innerWidth - saveButton.width - saveRow.spacing
                                height: panel.style.grooveNameFieldHeight; radius: panel.style.radiusLg
                                color: panel.style.wellFill
                                border.color: nameInput.activeFocus ? panel.style.accent : panel.style.line
                                TextInput {
                                    id: nameInput
                                    objectName: "grooveCaptureName"
                                    anchors.fill: parent
                                    anchors.margins: panel.style.spaceMd
                                    color: panel.style.textMain
                                    font.pixelSize: panel.style.typeLabel
                                    clip: true
                                    maximumLength: 64
                                    text: panel.gc.name
                                    onTextEdited: panel.gc.name = text
                                    Accessible.role: Accessible.EditableText
                                    Accessible.name: "Groove name"
                                }
                            }
                            Kit.Button {
                                id: saveButton
                                objectName: "grooveCaptureSave"
                                theme: panel.style
                                prime: true
                                enabled: panel.gc.hasResult
                                label: "Save"
                                tip: "Keep it in the groove pool: the piano roll, the clip menu and the Nova arp list it"
                                onClicked: panel.gc.save()
                            }
                        }
                        Text {
                            width: saveCard.innerWidth
                            wrapMode: Text.Wrap
                            text: "Saved grooves live with your others and work wherever a groove does: MIDI clips, "
                                  + "Knock patterns and the Nova arp."
                            color: panel.style.textFaint
                            font.pixelSize: panel.style.typeBody
                        }
                    }

                    // Apply to the selected MIDI clips.
                    MixCheckCard {
                        id: applyCard
                        style: panel.style
                        width: (parent.width - panel.style.gap) / 2
                        Text {
                            text: "APPLY"
                            color: panel.style.textMain
                            font.pixelSize: panel.style.typeLabel; font.weight: Font.DemiBold
                            font.letterSpacing: panel.style.capsLetterSpacing
                        }
                        Repeater {
                            model: [
                                { label: "Quantise", key: "grooveQuantize",
                                  tip: "How far notes move onto the groove's grid" },
                                { label: "Timing", key: "grooveTiming", tip: "How much of each step's push or drag" },
                                { label: "Velocity", key: "grooveVelocity", tip: "How much of each step's accent" }
                            ]
                            delegate: Item {
                                id: amount
                                required property var modelData
                                readonly property real value: Number(panel.noteTools[modelData.key])
                                width: applyCard.innerWidth
                                height: panel.style.grooveAmountHeight
                                Text {
                                    width: panel.style.grooveAmountLabelWidth
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: amount.modelData.label.toUpperCase()
                                    color: panel.style.textDim
                                    font.pixelSize: panel.style.typeCaption; font.weight: Font.DemiBold
                                    font.letterSpacing: panel.style.grooveAmountLabelSpacing
                                }
                                Rectangle {
                                    id: track
                                    x: panel.style.grooveAmountBarX
                                    width: parent.width - panel.style.grooveAmountBarX - panel.style.grooveAmountValueWidth
                                    height: panel.style.grooveAmountBarHeight; radius: panel.style.grooveAmountBarHeight / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: panel.style.line
                                    Rectangle {
                                        width: parent.width * Math.max(0, Math.min(1, amount.value))
                                        height: parent.height; radius: panel.style.grooveAmountBarHeight / 2
                                        color: panel.style.accent
                                    }
                                    Tip { text: amount.modelData.tip }
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -panel.style.grooveAmountHitOutset
                                        function set(x) {
                                            panel.noteTools[amount.modelData.key] =
                                                Math.round(Math.max(0, Math.min(1, (x - panel.style.grooveAmountHitOutset)
                                                                                    / track.width)) * 100) / 100
                                        }
                                        onPressed: mouse => set(mouse.x)
                                        onPositionChanged: mouse => { if (pressed) set(mouse.x) }
                                    }
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: panel.percent(amount.value)
                                    color: panel.style.textMain
                                    font.family: panel.style.monoFamily
                                    font.pixelSize: panel.style.typeSmall
                                }
                            }
                        }
                        Kit.Button {
                            objectName: "grooveCaptureApply"
                            theme: panel.style
                            enabled: panel.gc.hasResult && panel.midiTargets.length > 0
                            label: panel.midiTargets.length > 1 ? "Save and apply to " + panel.midiTargets.length + " clips"
                                                                : "Save and apply to the selected clip"
                            tip: panel.midiTargets.length === 0 ? "Select a MIDI clip in the arrangement first"
                                                                : "Keeps the groove, then grooves each clip as one undo step"
                            onClicked: panel.applyToSelected()
                        }
                    }
                }
            }
        }
    }
}
