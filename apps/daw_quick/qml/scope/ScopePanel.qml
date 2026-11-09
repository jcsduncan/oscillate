pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Scope device panel: a phosphor oscilloscope and alignment meter in
// green and cyan on bottle-green glass. The sweep leads: the track in green
// and the sidechain in cyan, overlaid on a graticule with bar and beat
// lines when synced; beside it the alignment meter, the track's delay
// behind the sidechain on a needle centred on zero, in milliseconds and
// samples, its polarity and how alike the two are; under them a hint in
// plain words; below, the controls. Every control reads and writes through
// `controller` (NativeSynthDeviceController for Scope); the sweep and the
// alignment come only from the telemetry (layout in scope.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: panel.idle()
    property ScopeTheme theme: ScopeTheme {}
    // What the hint calls the two signals; a host may name the tracks.
    property string trackName: "The track"
    property string keyName: "the sidechain"
    // For the delay in samples (the telemetry carries milliseconds).
    property real sampleRate: 48000

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int revision: controller.revision

    // scope.hpp: (min, max) for 480 columns of the track, then of the
    // sidechain, the write column, delay (ms, positive when the track is
    // late), polarity, correlation and the window length (ms).
    readonly property int columns: 480
    readonly property int trackAt: 0
    readonly property int keyAt: 2 * columns
    readonly property int writeAt: 4 * columns
    readonly property int frameSize: writeAt + 5
    readonly property real reachMs: 20

    readonly property bool playing: live.playing === true
    readonly property bool synced: revision >= 0 && controller.choice("scope.sync") === 1
    readonly property int lengthIndex: revision >= 0 ? controller.choice("scope.length") : 2
    readonly property int channelIndex: revision >= 0 ? controller.choice("scope.channel") : 0
    readonly property var lengthNames: ["1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BARS", "4 BARS"]
    // The length tiles, in bars under a caption that says so: seven
    // tiles leave no room for the word on each.
    readonly property var lengthTiles: ["1/16", "1/8", "1/4", "1/2", "1", "2", "4"]
    // The first length (1 BAR) whose grid counts bars rather than beats.
    readonly property int firstBarLength: 4
    readonly property var channelNames: ["MID", "SIDE", "LEFT", "RIGHT"]
    readonly property bool overlayOn: revision >= 0 && controller.choice("scope.overlay") === 1
    readonly property bool alignOn: revision >= 0 && controller.choice("scope.align") === 1
    readonly property bool lowBand: revision >= 0 && controller.choice("scope.align_band") === 1
    readonly property var grid: gridFor(synced, lengthIndex, playing ? live.windowMs : 0)
    readonly property string hint: alignmentHint(live)

    objectName: "scopePanel"
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
    Accessible.name: "Scope"

    function idle() {
        return { playing: false, track: [], key: [], hasKey: false, writeColumn: 0, delayMs: 0,
                 polarity: 1, correlation: 0, windowMs: 0 }
    }
    function decode(state) {
        const f = state.frame
        if (f === undefined || f.length < frameSize)
            return idle()
        const key = f.slice(keyAt, keyAt + 2 * columns)
        let hasKey = false
        for (let i = 0; i < key.length && !hasKey; ++i)
            hasKey = key[i] !== 0
        return {
            playing: true,
            track: f.slice(trackAt, trackAt + 2 * columns),
            key: key,
            hasKey: hasKey,
            writeColumn: Math.max(0, Math.min(columns - 1, Math.round(f[writeAt]))),
            delayMs: f[writeAt + 1],
            polarity: f[writeAt + 2] < 0 ? -1 : 1,
            correlation: Math.max(0, Math.min(1, f[writeAt + 3])),
            windowMs: f[writeAt + 4]
        }
    }

    // ---- Formatting ------------------------------------------------------------------

    function signed(value, digits) {
        return Format.signed_number(value, digits)
    }
    function msText(ms) {
        return Format.milliseconds(ms)
    }
    function samplesOf(ms) {
        return ms * sampleRate / 1000
    }
    // How far apart: aligned within a tenth of a millisecond (about five
    // samples, under a degree of phase at 50 Hz), close within one.
    function toneOf(ms) {
        const d = Math.abs(ms)
        return d < 0.1 ? theme.aligned : d < 1 ? theme.near : theme.off
    }
    readonly property bool measuring: playing && alignOn && live.hasKey && live.correlation > 0
    // Below this match the reading is a guess.
    readonly property real guessBelow: 0.3
    readonly property bool confident: measuring && live.correlation >= guessBelow
    // An uncertain reading is shown, but quietly.
    readonly property color delayTone: !measuring ? theme.inkFaint : !confident ? theme.inkDim
        : live.polarity < 0 ? theme.off : toneOf(live.delayMs)

    // The hint, in plain words: which way to move the track and by how
    // much, or that its polarity is inverted.
    function alignmentHint(state) {
        if (!state.playing)
            return "Play the track with a reference (the kick, say) on the sidechain to compare them."
        if (!alignOn)
            return "Align is off: switch it on to measure the delay against " + keyName + "."
        if (!state.hasKey || state.correlation <= 0)
            return "Nothing on the sidechain: route the kick (or any reference) to it to align against."
        const name = trackName
        const d = state.delayMs
        const amount = Format.milliseconds(Math.abs(d))
        const edge = Math.abs(d) >= reachMs - 0.5 ? " (at the edge of the ±20 ms search)" : ""
        if (state.correlation < guessBelow)
            return name + " and " + keyName + " have little in common here: the reading is uncertain."
        const aligned = Math.abs(d) < 0.1
        if (state.polarity < 0)
            return aligned ? "Polarity is inverted: flip " + lowerFirst(name) + "'s polarity."
                : "Polarity is inverted, and " + lowerFirst(name) + " is " + amount + " "
                  + (d > 0 ? "late" : "early") + ": flip it, then nudge it " + (d > 0 ? "earlier" : "later") + edge + "."
        if (aligned)
            return name + " is aligned with " + keyName + "."
        return name + " is " + amount + " " + (d > 0 ? "late" : "early") + " against " + keyName
            + ": nudge it " + (d > 0 ? "earlier" : "later") + edge + "."
    }
    function lowerFirst(text) {
        return text.startsWith("The ") ? "the " + text.slice(4) : text
    }

    // The window's grid. Synced: sixteenths, beats and bars counted as
    // "1 e & a"; free: ten time divisions.
    function gridFor(sync, length, windowMs) {
        const lines = []
        const labels = []
        if (sync) {
            const sixteenths = [1, 2, 4, 8, 16, 32, 64][Math.max(0, Math.min(6, length))]
            const bars = length >= panel.firstBarLength
            const count = ["1", "e", "&", "a"]
            for (let i = 0; i <= sixteenths; ++i) {
                const weight = bars && i % 16 === 0 ? 2 : i % 4 === 0 ? 1 : 0
                if (weight === 0 && sixteenths >= 64)
                    continue
                lines.push({ at: i / sixteenths, weight: weight })
                if (i === sixteenths)
                    continue
                if (bars) {
                    if (i % 4 === 0) {
                        const bar = Math.floor(i / 16) + 1
                        const beat = (i % 16) / 4 + 1
                        labels.push({ at: i / sixteenths, text: beat === 1 ? "" + bar : bar + "." + beat })
                    }
                } else {
                    labels.push({ at: i / sixteenths, text: i % 4 === 0 ? "" + (i / 4 + 1) : count[i % 4] })
                }
            }
            // Under a sixteenth or two, thirty-seconds too.
            if (sixteenths <= 2) {
                for (let i = 0; i < sixteenths; ++i)
                    lines.push({ at: (i + 0.5) / sixteenths, weight: 0 })
            }
        } else {
            for (let i = 0; i <= 10; ++i) {
                lines.push({ at: i / 10, weight: i === 5 ? 1 : 0 })
                if (i % 2 === 0 && i < 10 && windowMs > 0)
                    labels.push({ at: i / 10, text: i === 0 ? "0" : Format.number(windowMs * i / 10) })
            }
        }
        return { lines: lines, labels: labels }
    }

    // A plate: a fine bevel along the top, the title in spaced caps.
    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // ---- Header ----------------------------------------------------------------------

    // The wordmark is underlined in the two inks, track to sidechain.
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
                    objectName: "scopeWordmark"
                    text: "SCOPE"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Rectangle {
                    x: wordmark.x + panel.theme.wordmarkInset
                    y: wordmark.y + wordmark.height + panel.theme.underlineGap
                    width: wordmark.implicitWidth - panel.theme.underlineTrim
                    height: panel.theme.strokeBold
                    radius: height / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: panel.theme.trace }
                        GradientStop { position: 1; color: panel.theme.key }
                    }
                }
                Text {
                    id: subtitle
                    objectName: "scopeSubtitle"
                    x: wordmark.x + panel.theme.wordmarkInset
                    y: wordmark.y + wordmark.height + panel.theme.subtitleGap
                    text: "OSCILLOSCOPE · ALIGNMENT"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }
    }

    // ---- The sweep -------------------------------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        theme: panel.theme
        id: sweepPlate
        title: "SWEEP"
        note: !panel.playing ? "NO SIGNAL"
            : panel.synced ? "LOCKED · " + panel.lengthNames[panel.lengthIndex]
                             + " · " + panel.msText(panel.live.windowMs)
            : "FREE · " + panel.msText(panel.live.windowMs)
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.sweepWidth
        height: panel.rowAHeight
        // The legend, top right.
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spaceXl
            y: panel.theme.legendY
            spacing: panel.theme.spaceXl
            component Swatch: Row {
                id: swatch
                required property color tone
                required property string text
                property bool shown: true
                spacing: panel.theme.spaceSm
                opacity: shown ? 1 : panel.theme.legendHiddenOpacity
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.swatchLength
                    height: panel.theme.strokeBold
                    radius: height / 2
                    color: swatch.tone
                }
                Text {
                    text: swatch.text
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
            }
            Swatch {
                tone: panel.theme.trace
                text: "TRACK · " + panel.channelNames[panel.channelIndex]
            }
            Swatch {
                objectName: "scopeLegendKey"
                tone: panel.theme.key
                text: panel.overlayOn ? "SIDECHAIN" : "SIDECHAIN · HIDDEN"
                shown: panel.overlayOn
            }
        }
        ScopeScreen {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: sweepPlate.width - 2 * panel.theme.cardInset
            height: sweepPlate.height - panel.theme.cardContentY - panel.theme.cardInset
            theme: panel.theme
            live: panel.playing
            track: panel.live.track
            key: panel.live.key
            writeColumn: panel.live.writeColumn
            overlay: panel.overlayOn
            lines: panel.grid.lines
            labels: panel.grid.labels
            timeUnit: !panel.synced ? "ms" : panel.lengthIndex >= panel.firstBarLength ? "bars" : "beats"
        }
    }

    // ---- Alignment -------------------------------------------------------------------

    Kit.Card {
        theme: panel.theme
        id: alignPlate
        title: "ALIGNMENT"
        note: !panel.alignOn ? "OFF" : panel.lowBand ? "LOW BAND · BELOW 200 Hz" : "FULL BAND"
        noteColor: panel.measuring ? panel.theme.live : panel.theme.inkFaint
        x: sweepPlate.x + sweepPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        Caption {
            x: panel.theme.alignPad
            y: panel.theme.cardCaptionY
            text: "TRACK AGAINST SIDECHAIN"
        }
        ScopeNeedle {
            id: needle
            x: panel.theme.cardInset
            y: panel.theme.needleY
            width: alignPlate.width - 2 * panel.theme.cardInset
            height: panel.theme.needleHeight
            theme: panel.theme
            live: panel.measuring
            delayMs: panel.live.delayMs
            tone: panel.delayTone
        }
        Text {
            objectName: "scopeDelay"
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.delayY
            text: panel.measuring ? panel.signed(panel.live.delayMs, 2) : "—"
            color: panel.confident ? panel.theme.glacier : panel.measuring ? panel.theme.inkDim : panel.theme.inkFaint
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.delaySize
            font.weight: Font.Light
            Text {
                anchors.left: parent.right
                anchors.leftMargin: panel.theme.spaceSm
                anchors.baseline: parent.baseline
                visible: panel.measuring
                text: "ms"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeHeading
            }
        }
        Text {
            objectName: "scopeDelaySamples"
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.samplesY
            text: panel.measuring
                ? panel.signed(panel.samplesOf(panel.live.delayMs), 1) + " samples @ "
                  + Format.hertz(panel.sampleRate)
                : "SAMPLES"
            color: panel.measuring ? panel.delayTone : panel.theme.inkFaint
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeBody
            font.letterSpacing: panel.theme.samplesSpacing
        }

        Rectangle {
            x: panel.theme.cardPad
            y: panel.theme.alignRuleY
            width: alignPlate.width - 2 * panel.theme.cardPad
            height: panel.theme.hairline
            color: panel.theme.seam
        }

        // Polarity.
        Caption {
            x: panel.theme.alignPad
            y: panel.theme.polarityCaptionY
            text: "POLARITY"
        }
        Rectangle {
            id: polarity
            objectName: "scopePolarity"
            readonly property bool inverted: panel.measuring && panel.live.polarity < 0
            readonly property color tone: !panel.measuring ? panel.theme.inkFaint
                : !panel.confident ? panel.theme.inkDim : inverted ? panel.theme.off : panel.theme.aligned
            x: panel.theme.alignPad
            y: panel.theme.polarityY
            width: alignPlate.width - 2 * panel.theme.alignPad
            height: panel.theme.polarityHeight
            radius: panel.theme.radiusLg
            color: inverted && panel.confident ? Qt.rgba(tone.r, tone.g, tone.b, panel.theme.invertedFillAlpha)
                                                : panel.theme.well
            border.color: Qt.rgba(tone.r, tone.g, tone.b, panel.measuring ? panel.theme.polarityEdgeAlpha
                                                                          : panel.theme.polarityIdleEdgeAlpha)
            Text {
                x: panel.theme.polaritySignX
                anchors.verticalCenter: parent.verticalCenter
                text: !panel.measuring ? "±" : polarity.inverted ? "−" : "+"
                color: polarity.tone
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.polaritySignSize
                font.bold: true
            }
            Text {
                objectName: "scopePolarityText"
                x: panel.theme.polarityTextX
                anchors.verticalCenter: parent.verticalCenter
                text: !panel.measuring ? "—" : polarity.inverted ? "INVERTED" : "IN PHASE"
                color: panel.measuring ? panel.theme.ink : panel.theme.inkFaint
                font.pixelSize: panel.theme.typeBody
                font.bold: true
                font.letterSpacing: panel.theme.polaritySpacing
            }
            Text {
                objectName: "scopeInvertHint"
                visible: polarity.inverted && panel.confident
                anchors.right: parent.right
                anchors.rightMargin: panel.theme.polarityHintRight
                anchors.verticalCenter: parent.verticalCenter
                text: "INVERT THE TRACK"
                color: polarity.tone
                font.pixelSize: panel.theme.typeCaption
                font.bold: true
                font.letterSpacing: panel.theme.captionSpacing
            }
        }

        // How alike the two are at that lag: the reading's confidence.
        Caption {
            x: panel.theme.alignPad
            y: panel.theme.matchCaptionY
            text: "MATCH · CONFIDENCE IN THE READING"
        }
        Text {
            objectName: "scopeCorrelation"
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.alignPad
            y: panel.theme.matchValueY
            text: panel.measuring ? Format.number(panel.live.correlation, 2) : "—"
            color: panel.confident ? panel.theme.key : panel.theme.inkDim
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeBody
        }
        Rectangle {
            id: confidence
            x: panel.theme.alignPad
            y: panel.theme.confidenceY
            width: alignPlate.width - 2 * panel.theme.alignPad
            height: panel.theme.confidenceHeight
            radius: height / 2
            color: panel.theme.well
            border.color: panel.theme.wellEdge
            Rectangle {
                objectName: "scopeConfidenceBar"
                x: panel.theme.hairline
                y: panel.theme.hairline
                height: parent.height - 2 * panel.theme.hairline
                radius: height / 2
                width: (parent.width - 2 * panel.theme.hairline) * (panel.measuring ? panel.live.correlation : 0)
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: panel.theme.keyGlow }
                    GradientStop { position: 1; color: panel.theme.key }
                }
            }
            // The guess mark.
            Rectangle {
                x: parent.width * panel.guessBelow
                y: -panel.theme.confidenceMarkOverhang
                width: panel.theme.hairline
                height: parent.height + 2 * panel.theme.confidenceMarkOverhang
                color: panel.theme.inkFaint
            }
        }
        Row {
            x: panel.theme.alignPad
            y: panel.theme.confidenceScaleY
            width: alignPlate.width - 2 * panel.theme.alignPad
            Text {
                width: parent.width * panel.guessBelow
                text: "GUESS"
                color: panel.theme.inkFaint
                font.pixelSize: panel.theme.confidenceScaleSize
                font.letterSpacing: panel.theme.tagSpacing
            }
            Text {
                width: parent.width * (1 - panel.guessBelow)
                horizontalAlignment: Text.AlignRight
                text: "THE SAME SIGNAL"
                color: panel.theme.inkFaint
                font.pixelSize: panel.theme.confidenceScaleSize
                font.letterSpacing: panel.theme.tagSpacing
            }
        }
    }

    // ---- The hint --------------------------------------------------------------------

    Rectangle {
        id: hintBar
        readonly property color tone: !panel.measuring ? panel.theme.inkFaint
            : !panel.confident ? panel.theme.near : panel.delayTone
        x: panel.margin
        y: panel.rowAY + panel.rowAHeight + panel.gap
        width: panel.width - 2 * panel.margin
        height: panel.theme.hintHeight
        radius: panel.theme.radiusLg
        color: panel.theme.well
        border.color: Qt.rgba(tone.r, tone.g, tone.b, panel.theme.hintEdgeAlpha)
        Rectangle {
            x: panel.theme.hintLampX
            anchors.verticalCenter: parent.verticalCenter
            width: panel.theme.hintLampSize
            height: panel.theme.hintLampSize
            radius: width / 2
            color: hintBar.tone
            Rectangle {
                anchors.centerIn: parent
                width: panel.theme.hintHaloSize
                height: panel.theme.hintHaloSize
                radius: width / 2
                color: Qt.rgba(hintBar.tone.r, hintBar.tone.g, hintBar.tone.b, panel.theme.hintHaloAlpha)
            }
        }
        Text {
            objectName: "scopeHint"
            x: panel.theme.hintTextX
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - panel.theme.hintTextX - panel.theme.hintTextRight
            text: panel.hint
            color: panel.measuring ? panel.theme.ink : panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeHeading
            elide: Text.ElideRight
        }
    }

    // ---- Controls --------------------------------------------------------------------

    readonly property real rowCY: hintBar.y + hintBar.height + panel.gap
    readonly property real rowCHeight: panel.height - rowCY - margin

    Kit.Card {
        theme: panel.theme
        id: windowPlate
        title: "WINDOW"
        note: panel.synced ? "LOCKED TO THE BAR AND BEAT" : "FREE RUNNING"
        x: panel.margin
        y: panel.rowCY
        width: panel.theme.windowWidth
        height: panel.rowCHeight
        Kit.Toggle {
            x: panel.theme.cardPad
            y: panel.theme.cardBodyY
            width: panel.theme.syncToggleWidth
            height: panel.theme.syncToggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "scope.sync"
            label: "Sync"
            onCaption: "TO THE TRANSPORT"
            offCaption: "FREE TIME"
        }
        Caption {
            x: panel.theme.windowColumnX
            y: panel.theme.cardCaptionY
            text: panel.synced ? "LENGTH IN BARS" : "TIME"
        }
        Kit.Segment {
            visible: panel.synced
            x: panel.theme.windowColumnX
            y: panel.theme.cardControlY
            width: windowPlate.width - panel.theme.windowColumnX - panel.theme.cardPad
            height: panel.theme.lengthHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "scope.length"
            labels: panel.lengthTiles
        }
        Kit.Knob {
            visible: !panel.synced
            x: panel.theme.windowColumnX
            y: panel.theme.cardControlY
            controller: panel.controller
            theme: panel.theme
            parameterId: "scope.time"
            diameter: panel.theme.timeDiameter
            readout: true
        }
        Text {
            visible: !panel.synced
            x: panel.theme.timeNoteX
            y: panel.theme.timeNoteY
            width: windowPlate.width - panel.theme.timeNoteX - panel.theme.cardPad
            text: "One sweep across the screen, from a millisecond to four seconds. Sync locks it to the transport instead."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
        Text {
            visible: panel.synced
            x: panel.theme.windowColumnX
            y: panel.theme.lengthNoteY
            width: windowPlate.width - panel.theme.windowColumnX - panel.theme.cardPad
            text: "The sweep starts on the grid: bar lengths from a bar line, beat lengths from a beat."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        theme: panel.theme
        id: displayPlate
        title: "DISPLAY"
        x: windowPlate.x + windowPlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.displayWidth
        height: panel.rowCHeight
        Caption {
            x: panel.theme.cardPad
            y: panel.theme.cardCaptionY
            text: "CHANNEL"
        }
        Kit.Segment {
            x: panel.theme.cardPad
            y: panel.theme.cardControlY
            width: displayPlate.width - 2 * panel.theme.cardPad
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "scope.channel"
            labels: panel.channelNames
        }
        Kit.Toggle {
            x: panel.theme.cardPad
            y: panel.theme.displayToggleY
            width: displayPlate.width - 2 * panel.theme.cardPad
            height: panel.theme.toggleRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "scope.overlay"
            label: "Sidechain"
            lit: panel.theme.key
            onCaption: "OVERLAID IN CYAN"
            offCaption: "TRACK ONLY"
        }
    }

    Kit.Card {
        theme: panel.theme
        id: alignControls
        title: "ALIGN"
        x: displayPlate.x + displayPlate.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Kit.Toggle {
            x: panel.theme.cardPad
            y: panel.theme.cardBodyY
            width: alignControls.width - 2 * panel.theme.cardPad
            height: panel.theme.toggleRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "scope.align"
            label: "Align"
            onCaption: "MEASURING AGAINST THE SIDECHAIN"
            offCaption: "NO MEASUREMENT"
        }
        Caption {
            x: panel.theme.cardPad
            y: panel.theme.bandCaptionY
            text: "BAND"
        }
        Kit.Segment {
            x: panel.theme.cardPad
            y: panel.theme.bandY
            width: alignControls.width - 2 * panel.theme.cardPad
            height: panel.theme.segmentRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "scope.align_band"
            labels: ["FULL", "LOW · < 200 Hz"]
        }
    }

    Timer {
        objectName: "scopeTelemetryPoll"
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
