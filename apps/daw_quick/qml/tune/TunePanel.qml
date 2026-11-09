pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import Oscillate.Format
import "../devicekit" as Kit

// The Tune device panel: a precision strobe tuner in warm amber on black.
// The note leads, large enough to read from across the room, with its
// octave; beside it the cents needle, the cents and the frequency; under
// them the strobe band drifting with the error and a hint in plain words;
// to the right the signal's clarity and level against the reference. In
// Kick mode the note's place goes to the kick: its pitch trajectory over
// 400 ms, the pitch it settles at, the hits counted and how to tune it.
// Below, the history of recent notes and the controls. Every control reads
// and writes through `controller` (NativeSynthDeviceController for Tune);
// readings come only from the telemetry (layout in tune.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: panel.idle()
    property TuneTheme theme: TuneTheme {}
    // The last pitch read, held (dimmed) through a gap until the next.
    property var lastPitch: null

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    // The poll's interval while playing and while idle (ms).
    readonly property int livePollInterval: 33
    readonly property int idlePollInterval: 250
    // The level gauge's span below 0 dBFS, and where it turns red.
    readonly property real levelSpanDb: 60
    readonly property real levelHotDb: -1
    readonly property int revision: controller.revision

    // tune.hpp: nine readings, 80 trajectory points, 200 history notes.
    readonly property int trajectoryAt: 9
    readonly property int trajectoryPoints: 80
    readonly property int notesAt: trajectoryAt + trajectoryPoints
    readonly property int historyPoints: 200
    readonly property int frameSize: notesAt + historyPoints

    readonly property bool playing: live.playing === true
    readonly property int mode: revision >= 0 ? controller.choice("tune.mode") : 0
    readonly property bool kick: mode === 2
    readonly property bool flats: revision >= 0 && controller.choice("tune.notation") === 1
    readonly property real reference: revision >= 0 ? controller.value("tune.reference") : 440
    readonly property bool sounding: playing && live.hz > 0
    readonly property bool held: !sounding && lastPitch !== null
    readonly property var shown: sounding ? live : lastPitch
    readonly property var parts: shown !== null ? noteParts(shown.note, flats) : null
    readonly property bool settled: playing && live.settledHz > 0
    readonly property var settledParts: settled ? noteParts(live.settledNote, flats) : null
    readonly property string hint: kick ? kickHint(live, flats) : instrumentHint(live, flats)

    objectName: "tunePanel"
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
    Accessible.name: "Tune"

    function idle() {
        return { playing: false, hz: 0, note: 0, cents: 0, clarity: 0, levelDb: -200, settledHz: 0,
                 settledNote: 0, settledCents: 0, hits: 0, trajectory: [], notes: [] }
    }
    function decode(state) {
        const f = state.frame
        if (f === undefined || f.length < frameSize)
            return idle()
        return {
            playing: true,
            hz: f[0], note: f[1], cents: f[2], clarity: f[3], levelDb: f[4],
            settledHz: f[5], settledNote: f[6], settledCents: f[7], hits: Math.round(f[8]),
            trajectory: f.slice(trajectoryAt, trajectoryAt + trajectoryPoints),
            notes: f.slice(notesAt, notesAt + historyPoints)
        }
    }
    function poll() {
        live = decode(controller.telemetry())
        if (!live.playing)
            lastPitch = null
        else if (live.hz > 0)
            lastPitch = { note: live.note, cents: live.cents, hz: live.hz }
    }

    // ---- Notes ---------------------------------------------------------------------

    readonly property var sharpNames: Contracts.pitch_class_names(Contracts.sharp)
    readonly property var flatNames: Contracts.pitch_class_names(Contracts.flat)
    // A fractional MIDI note as its nearest note (letter, accidental,
    // octave, MIDI 60 = C4) and the cents from it.
    function noteParts(note, useFlats) {
        const k = Math.round(note)
        const name = (useFlats ? flatNames : sharpNames)[((k % 12) + 12) % 12]
        return { letter: name.charAt(0), accidental: name.slice(1), octave: Math.floor(k / 12) - 1,
                 cents: 100 * (note - k) }
    }
    function signed(value, digits) {
        return Format.signed_number(value, digits)
    }
    // "A4 +25 ct", "B♭4 −40 ct".
    function noteLabel(note, useFlats) {
        const p = noteParts(note, useFlats)
        return p.letter + p.accidental + p.octave + " " + Format.cents(p.cents)
    }
    // A tuner reads frequency finer than the display rule: two decimals,
    // one from 1 kHz.
    function hzText(hz) {
        return Format.number(hz, hz >= 1000 ? 1 : 2)
    }
    // In tune within theme.inTuneCents, close within theme.nearCents.
    function toneOf(cents) {
        const c = Math.abs(cents)
        return c < theme.inTuneCents ? theme.inTune : c < theme.nearCents ? theme.near : theme.off
    }

    function instrumentHint(state, useFlats) {
        if (!state.playing)
            return "Play a single note into the tuner."
        if (state.hz <= 0)
            return lastPitch !== null ? "Held: " + noteLabel(lastPitch.note, useFlats) + ". Play the next note."
                                      : "Listening: play a single, sustained note."
        const p = noteParts(state.note, useFlats)
        const name = p.letter + p.accidental + p.octave
        const c = Math.round(Math.abs(state.cents))
        if (Math.abs(state.cents) < theme.inTuneCents)
            return name + " is in tune."
        return name + " is " + Format.quantity(c, "ct") + " " + (state.cents > 0 ? "sharp: tune down." : "flat: tune up.")
    }
    // "Kick settles at F1 −12 ct: tune it +0.12 st to land on F1."
    function kickHint(state, useFlats) {
        if (!state.playing || state.settledHz <= 0)
            return "Play the kick: Tune follows its pitch glide and reads where it settles."
        const p = noteParts(state.settledNote, useFlats)
        const name = p.letter + p.accidental + p.octave
        const label = noteLabel(state.settledNote, useFlats)
        if (Math.abs(p.cents) < theme.inTuneCents)
            return "Kick settles at " + label + ": in tune on " + name + "."
        return "Kick settles at " + label + ": tune it " + Format.semitones(-p.cents / 100, 2) + " to land on " + name + "."
    }

    // A black plate: a fine bevel along the top, the title in spaced caps.
    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }
    // A big note: the letter, its accidental raised beside it and the
    // octave lowered, in amber light with a soft glow.
    component BigNote: Item {
        id: big
        property var parts: null
        property int size: panel.theme.bigNoteSize
        property color tone: panel.theme.amber
        property bool dim: false
        property string prefix: "tuneNote"
        // The accidental and octave, as fractions of the letter's size: how
        // far down each sits and how large it is.
        readonly property real accidentalDrop: 0.16
        readonly property real accidentalScale: 0.42
        readonly property real octaveDrop: 0.72
        readonly property real octaveScale: 0.34
        implicitWidth: letter.implicitWidth + Math.max(accidental.implicitWidth, octave.implicitWidth) + panel.theme.spaceXs
        implicitHeight: letter.implicitHeight
        Text {
            id: glow
            x: letter.x
            y: letter.y
            text: letter.text
            color: Qt.rgba(big.tone.r, big.tone.g, big.tone.b, panel.theme.noteGlowAlpha)
            font: letter.font
            scale: panel.theme.noteGlowScale
            visible: !big.dim && big.parts !== null
        }
        Text {
            id: letter
            objectName: big.prefix + "Name"
            text: big.parts !== null ? big.parts.letter : "–"
            color: big.parts === null ? panel.theme.inkFaint : big.dim ? panel.theme.amberDim : big.tone
            font.family: panel.theme.sans
            font.pixelSize: big.size
            font.weight: Font.Light
        }
        Text {
            id: accidental
            objectName: big.prefix + "Accidental"
            x: letter.implicitWidth + panel.theme.spaceXxs
            y: big.size * big.accidentalDrop
            text: big.parts !== null ? big.parts.accidental : ""
            color: letter.color
            font.family: panel.theme.sans
            font.pixelSize: big.size * big.accidentalScale
        }
        Text {
            id: octave
            objectName: big.prefix + "Octave"
            x: letter.implicitWidth + panel.theme.spaceXs
            y: big.size * big.octaveDrop
            text: big.parts !== null ? "" + big.parts.octave : ""
            color: big.dim ? panel.theme.inkFaint : panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: big.size * big.octaveScale
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
        // The wordmark with its amber underline, over the tagline.
        brand: Component {
            Item {
                implicitWidth: Math.max(wordmark.implicitWidth, tagline.x + tagline.implicitWidth)
                implicitHeight: tagline.y + tagline.implicitHeight
                Text {
                    id: wordmark
                    objectName: "tuneWordmark"
                    text: "TUNE"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Rectangle {
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.underlineGap
                    width: wordmark.implicitWidth - panel.theme.underlineTrim
                    height: panel.theme.underlineHeight
                    radius: panel.theme.underlineHeight / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: panel.theme.accentSoft }
                        GradientStop { position: 1; color: panel.theme.amberHot }
                    }
                }
                Text {
                    id: tagline
                    objectName: "tuneSubtitle"
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "STROBE TUNER · KICK TRACKER"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }
    }

    // ---- The note --------------------------------------------------------------------

    readonly property real rowAY: 76
    readonly property real rowAHeight: 380
    readonly property real notePlateWidth: 900

    Kit.Card {
        theme: panel.theme
        id: notePlate
        title: panel.kick ? "KICK" : "NOTE"
        note: !panel.playing ? "NO SIGNAL"
            : panel.kick ? "PITCH GLIDE FROM THE ONSET · SETTLED PITCH"
            : panel.held ? "HELD" : panel.sounding ? (panel.mode === 1 ? "BASS · LISTENING LOW" : "LISTENING") : "LISTENING"
        noteColor: panel.sounding || panel.settled ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.notePlateWidth
        height: panel.rowAHeight
        // Content below the plate's title, and the hint bar's bottom edge
        // (its height and inset) from the plate's foot.
        readonly property real bodyY: 30
        readonly property real hintHeight: 40
        readonly property real hintBottom: 54

        // Instrument and Bass.
        Item {
            id: tuner
            objectName: "tuneInstrumentView"
            anchors.fill: parent
            visible: !panel.kick
            readonly property color tone: panel.sounding ? panel.toneOf(panel.live.cents) : panel.theme.amber
            // The note's column, the cents meter's place, the readings' tops
            // under it, the cents' shift left to balance its unit, and the strobe.
            readonly property real noteColumnWidth: 390
            readonly property real noteY: 26
            readonly property real meterX: 400
            readonly property real meterWidth: 480
            readonly property real meterHeight: 132
            readonly property real centsY: 164
            readonly property real centsShift: 16
            readonly property real hzY: 218
            readonly property real strobeY: 252
            readonly property real strobeHeight: 64
            BigNote {
                x: Math.max(panel.theme.space2xl, (tuner.noteColumnWidth - implicitWidth) / 2)
                y: tuner.noteY
                size: panel.theme.noteSize
                parts: panel.parts
                dim: panel.held
                tone: panel.theme.amber
            }
            TuneCents {
                x: tuner.meterX
                y: notePlate.bodyY
                width: tuner.meterWidth
                height: tuner.meterHeight
                theme: panel.theme
                live: panel.shown !== null
                held: panel.held
                cents: panel.shown !== null ? panel.shown.cents : 0
                tone: tuner.tone
            }
            Text {
                objectName: "tuneCents"
                x: tuner.meterX + (tuner.meterWidth - width) / 2 - tuner.centsShift
                y: tuner.centsY
                text: panel.shown !== null ? panel.signed(panel.shown.cents, 1) : "—"
                color: panel.shown === null ? panel.theme.inkFaint : panel.held ? panel.theme.amberDim : tuner.tone
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.centsReadoutSize
                font.weight: Font.Light
                Text {
                    anchors.left: parent.right
                    anchors.leftMargin: panel.theme.spaceSm
                    anchors.baseline: parent.baseline
                    text: "ct"
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeSmall
                    font.bold: true
                    font.letterSpacing: panel.theme.unitSpacing
                }
            }
            Text {
                objectName: "tuneHz"
                x: tuner.meterX + (tuner.meterWidth - width) / 2
                y: tuner.hzY
                text: panel.shown !== null ? panel.hzText(panel.shown.hz) + " Hz" : "— Hz"
                color: panel.held || panel.shown === null ? panel.theme.inkFaint : panel.theme.ink
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeHeading
            }
            TuneStrobe {
                x: panel.theme.spaceXl
                y: tuner.strobeY
                width: notePlate.width - 2 * panel.theme.spaceXl
                height: tuner.strobeHeight
                theme: panel.theme
                live: panel.sounding
                cents: panel.sounding ? panel.live.cents : 0
                clarity: panel.live.clarity
            }
        }

        // Kick.
        Item {
            id: kickView
            objectName: "tuneKickView"
            anchors.fill: parent
            visible: panel.kick
            readonly property color tone: panel.settled ? panel.toneOf(panel.live.settledCents) : panel.theme.amber
            // The trajectory's size; the readings' column and their tops;
            // the hits count's place; the advice box's top.
            readonly property real plotWidth: 540
            readonly property real plotHeight: 286
            readonly property real columnX: 580
            readonly property real captionY: 34
            readonly property real noteY: 44
            readonly property real centsY: 196
            readonly property real hzY: 244
            readonly property real hitsCaptionX: 760
            readonly property real hitsCaptionY: 252
            readonly property real hitsX: 796
            readonly property real adviceY: 276
            readonly property real adviceTextPad: 20
            TuneTrajectory {
                x: panel.theme.plateInset
                y: notePlate.bodyY
                width: kickView.plotWidth
                height: kickView.plotHeight
                theme: panel.theme
                live: panel.playing
                points: panel.live.trajectory
                settledHz: panel.settled ? panel.live.settledHz : 0
                settledName: panel.settled ? panel.noteLabel(panel.live.settledNote, panel.flats) : ""
                names: panel.flats ? panel.flatNames : panel.sharpNames
                reference: panel.reference
            }
            Caption {
                x: kickView.columnX
                y: kickView.captionY
                text: "SETTLES AT"
            }
            BigNote {
                x: kickView.columnX
                y: kickView.noteY
                size: panel.theme.settledNoteSize
                prefix: "tuneSettled"
                parts: panel.settledParts
                tone: panel.theme.amber
            }
            Text {
                objectName: "tuneSettledCents"
                x: kickView.columnX + panel.theme.spaceXxs
                y: kickView.centsY
                text: panel.settled ? panel.signed(panel.live.settledCents, 1) : "—"
                color: panel.settled ? kickView.tone : panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.settledCentsSize
                font.weight: Font.Light
                Text {
                    anchors.left: parent.right
                    anchors.leftMargin: panel.theme.spaceSm
                    anchors.baseline: parent.baseline
                    text: "ct"
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeSmall
                    font.bold: true
                    font.letterSpacing: panel.theme.unitSpacing
                }
            }
            Text {
                objectName: "tuneSettledHz"
                x: kickView.columnX + panel.theme.spaceXs
                y: kickView.hzY
                text: panel.settled ? panel.hzText(panel.live.settledHz) + " Hz" : "— Hz"
                color: panel.settled ? panel.theme.ink : panel.theme.inkFaint
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeHeading
            }
            Caption {
                x: kickView.hitsCaptionX
                y: kickView.hitsCaptionY
                text: "HITS"
            }
            Text {
                objectName: "tuneHits"
                x: kickView.hitsX
                y: kickView.hzY
                text: panel.playing ? "" + panel.live.hits : "—"
                color: panel.theme.amberHot
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeHeading
            }
            Rectangle {
                x: kickView.columnX
                y: kickView.adviceY
                width: notePlate.width - kickView.columnX - panel.theme.spaceXl
                height: notePlate.hintHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: panel.theme.wellEdge
                Text {
                    anchors.centerIn: parent
                    width: parent.width - kickView.adviceTextPad
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.settled ? "Tune the kick by " + panel.signed(-panel.live.settledCents / 100, 2) + " semitones" : "No hit yet"
                    color: panel.settled ? panel.theme.amberHot : panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeBody
                    font.bold: true
                    font.letterSpacing: panel.theme.adviceSpacing
                    elide: Text.ElideRight
                }
            }
        }

        // The hint, in plain words.
        Rectangle {
            id: hintBar
            readonly property color tone: panel.kick ? (panel.settled ? kickView.tone : panel.theme.inkFaint)
                : panel.sounding ? tuner.tone : panel.theme.inkFaint
            // The hint's text starts past the lamp and its halo.
            readonly property real textX: 38
            x: panel.theme.spaceXl
            y: notePlate.height - notePlate.hintBottom
            width: notePlate.width - 2 * panel.theme.spaceXl
            height: notePlate.hintHeight
            radius: panel.theme.radiusLg
            color: panel.theme.well
            border.color: Qt.rgba(tone.r, tone.g, tone.b, panel.theme.hintEdgeAlpha)
            Rectangle {
                x: panel.theme.plateInset
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.hintLampSize
                height: panel.theme.hintLampSize
                radius: panel.theme.hintLampSize / 2
                color: hintBar.tone
                Rectangle {
                    anchors.centerIn: parent
                    width: panel.theme.hintHaloSize
                    height: panel.theme.hintHaloSize
                    radius: panel.theme.hintHaloSize / 2
                    color: Qt.rgba(hintBar.tone.r, hintBar.tone.g, hintBar.tone.b, panel.theme.hintHaloAlpha)
                }
            }
            Text {
                objectName: "tuneHint"
                x: hintBar.textX
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - hintBar.textX - panel.theme.plateInset
                text: panel.hint
                color: panel.sounding || panel.settled ? panel.theme.ink : panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeHeading
                elide: Text.ElideRight
            }
        }
    }

    // ---- Signal ----------------------------------------------------------------------

    Kit.Card {
        theme: panel.theme
        id: signalPlate
        title: "SIGNAL"
        x: notePlate.x + notePlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        component Gauge: Item {
            id: gauge
            required property string name
            required property string value
            required property real fraction
            property string objectId: ""
            property color tone: panel.theme.amber
            // The gauge's height and its bar's top below the caption.
            readonly property real gaugeHeight: 40
            readonly property real barY: 18
            width: signalPlate.width - 2 * panel.theme.spaceXl
            height: gaugeHeight
            Caption {
                text: gauge.name
            }
            Text {
                objectName: gauge.objectId
                anchors.right: parent.right
                y: -panel.theme.gaugeValueLift
                text: gauge.value
                color: panel.theme.ink
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeLabel
            }
            Rectangle {
                y: gauge.barY
                width: parent.width
                height: panel.theme.gaugeBarHeight
                radius: panel.theme.gaugeBarHeight / 2
                color: panel.theme.well
                border.color: panel.theme.wellEdge
                Rectangle {
                    x: panel.theme.hairline
                    y: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.hairline
                    radius: panel.theme.radiusSm
                    width: (parent.width - 2 * panel.theme.hairline) * Math.max(0, Math.min(1, gauge.fraction))
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: panel.theme.amberDim }
                        GradientStop { position: 1; color: gauge.tone }
                    }
                }
            }
        }
        // The gauges' top and spacing; the rule's place; the reference,
        // mode and notation captions and their readings' tops.
        readonly property real gaugesY: 36
        readonly property real gaugeSpacing: 14
        readonly property real ruleY: 160
        readonly property real referenceCaptionY: 176
        readonly property real referenceY: 192
        readonly property real modeCaptionY: 250
        readonly property real modeY: 264
        readonly property real notationCaptionY: 304
        readonly property real notationY: 318
        Column {
            x: panel.theme.spaceXl
            y: signalPlate.gaugesY
            spacing: signalPlate.gaugeSpacing
            Gauge {
                name: "CLARITY"
                // Kick mode times cycles instead: no clarity to show.
                value: panel.playing && !panel.kick ? Format.number(panel.live.clarity, 2) : "—"
                fraction: panel.playing && !panel.kick ? panel.live.clarity : 0
                objectId: "tuneClarity"
            }
            Gauge {
                name: "LEVEL"
                value: panel.playing && panel.live.levelDb > -199 ? panel.signed(panel.live.levelDb, 1) + " dBFS" : "—"
                fraction: panel.playing ? (panel.live.levelDb + panel.levelSpanDb) / panel.levelSpanDb : 0
                objectId: "tuneLevel"
                tone: panel.live.levelDb > panel.levelHotDb ? panel.theme.off : panel.theme.amber
            }
        }
        Rectangle {
            x: panel.theme.plateInset
            y: signalPlate.ruleY
            width: signalPlate.width - 2 * panel.theme.plateInset
            height: panel.theme.hairline
            color: panel.theme.seam
        }
        Caption {
            x: panel.theme.spaceXl
            y: signalPlate.referenceCaptionY
            text: "REFERENCE"
        }
        Text {
            objectName: "tuneReference"
            x: panel.theme.spaceXl
            y: signalPlate.referenceY
            text: "A4 = " + Format.number(panel.reference, 1)
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.referenceReadoutSize
            font.weight: Font.Light
            Text {
                anchors.left: parent.right
                anchors.leftMargin: panel.theme.spaceSm
                anchors.baseline: parent.baseline
                text: "Hz"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeEmphasis
            }
        }
        Caption {
            x: panel.theme.spaceXl
            y: signalPlate.modeCaptionY
            text: "MODE"
        }
        Text {
            x: panel.theme.spaceXl
            y: signalPlate.modeY
            width: signalPlate.width - 2 * panel.theme.spaceXl
            text: ["Instrument · 40 Hz to 4 kHz", "Bass · 20 Hz to 1 kHz, a longer window", "Kick · each hit's glide below 300 Hz"][panel.mode]
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeBody
            wrapMode: Text.WordWrap
        }
        Caption {
            x: panel.theme.spaceXl
            y: signalPlate.notationCaptionY
            text: "NOTATION"
        }
        Text {
            x: panel.theme.spaceXl
            y: signalPlate.notationY
            text: panel.flats ? "♭  Flats" : "♯  Sharps"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeBody
        }
    }

    // ---- History ---------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: 140
    // Plate content's top below the plate's title.
    readonly property real plateBodyY: 28

    Kit.Card {
        theme: panel.theme
        id: historyPlate
        title: "HISTORY"
        note: "LAST 4 s · GREEN WITHIN 3 ct, AMBER 15, RED BEYOND"
        x: panel.margin
        y: panel.rowBY
        width: panel.width - 2 * panel.margin
        height: panel.rowBHeight
        TuneHistory {
            x: panel.theme.spaceMd
            y: panel.plateBodyY
            width: historyPlate.width - 2 * panel.theme.spaceMd
            height: historyPlate.height - panel.plateBodyY - panel.theme.spaceMd
            theme: panel.theme
            notes: panel.live.notes
            names: panel.flats ? panel.flatNames : panel.sharpNames
        }
    }

    // ---- Controls --------------------------------------------------------------------

    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: panel.height - rowCY - margin
    readonly property real modePlateWidth: 470
    readonly property real referencePlateWidth: 300
    // The controls' tops, the segments' height, the notes' tops under them.
    readonly property real controlY: 34
    readonly property real segmentHeight: 30
    readonly property real controlNoteY: 74
    // The reference knob's place and size, and its note beside it.
    readonly property real knobX: 10
    readonly property real knobY: 26
    readonly property real knobDiameter: 36
    readonly property real referenceNoteX: 104

    Kit.Card {
        theme: panel.theme
        id: modePlate
        title: "MODE"
        x: panel.margin
        y: panel.rowCY
        width: panel.modePlateWidth
        height: panel.rowCHeight
        Kit.Segment {
            x: panel.theme.plateInset
            y: panel.controlY
            width: modePlate.width - 2 * panel.theme.plateInset
            height: panel.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "tune.mode"
            labels: ["INSTRUMENT", "BASS", "KICK"]
        }
        Text {
            x: panel.theme.plateInset
            y: panel.controlNoteY
            width: modePlate.width - 2 * panel.theme.plateInset
            text: "Instrument and Bass read a held note to a fraction of a cent; Kick traces each hit's glide and where it settles."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        theme: panel.theme
        id: referencePlate
        title: "REFERENCE"
        x: modePlate.x + modePlate.width + panel.gap
        y: panel.rowCY
        width: panel.referencePlateWidth
        height: panel.rowCHeight
        Kit.Knob {
            x: panel.knobX
            y: panel.knobY
            controller: panel.controller
            theme: panel.theme
            parameterId: "tune.reference"
            label: "A4"
            diameter: panel.knobDiameter
            readout: true
        }
        Text {
            x: panel.referenceNoteX
            y: panel.controlY
            width: referencePlate.width - panel.referenceNoteX - panel.theme.plateInset
            text: "Concert pitch for A above middle C: 440 Hz by default, 415 for baroque, 432 to taste."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        theme: panel.theme
        id: notationPlate
        title: "NOTATION"
        x: referencePlate.x + referencePlate.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Kit.Segment {
            x: panel.theme.plateInset
            y: panel.controlY
            width: notationPlate.width - 2 * panel.theme.plateInset
            height: panel.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "tune.notation"
            labels: ["♯  SHARPS", "♭  FLATS"]
        }
        Text {
            x: panel.theme.plateInset
            y: panel.controlNoteY
            width: notationPlate.width - 2 * panel.theme.plateInset
            text: "How the black keys are named: A♯ or B♭, everywhere on the panel."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Timer {
        objectName: "tuneTelemetryPoll"
        interval: panel.playing ? panel.livePollInterval : panel.idlePollInterval
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
