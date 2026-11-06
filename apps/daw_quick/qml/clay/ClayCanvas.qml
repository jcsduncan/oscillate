pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import "../shell"

// Clay's note canvas: a piano ruler with the
// take's scale, a bars.beats ruler, the clip's waveform faint behind, and
// every note as a blob on its pitch centre, thick where it is loud, with its
// pitch curve through it. Drags go through the controller's begin/update/
// commit, so each gesture is one undo step. One Canvas draws all the notes
// and repaints only when they, the view or the tool change.
Item {
    id: canvas
    required property ClayTheme style
    required property var clay
    // "main", "pitch", "modulation", "drift", "formant", "amplitude",
    // "time" or "separation".
    property string tool: "main"
    property bool showOriginal: false
    property bool auditionOnClick: false
    property real playheadSeconds: -1
    signal toolRequested(string tool)
    signal auditionRequested(string key)
    signal playRequested()

    readonly property real pianoWidth: 46
    readonly property real rulerHeight: 22
    readonly property real dragThreshold: 3
    readonly property real edgeGrab: 8
    readonly property real centsPerSemitone: 100
    readonly property real modulationPixels: 60
    readonly property real formantCentsPerPixel: 5
    readonly property real gainDbPerPixel: 0.15
    readonly property int sixteenthsPerBeat: 4
    readonly property real minimumViewSeconds: 0.25
    readonly property real zoomStep: 1.2
    readonly property var majorSteps: [0, 2, 4, 5, 7, 9, 11]
    readonly property var minorSteps: [0, 2, 3, 5, 7, 8, 10]
    // The view's defaults before a clip (cents of C3 to C6, 120 BPM in 4/4).
    readonly property real defaultLowCents: 4800
    readonly property real defaultHighCents: 8400
    readonly property real defaultBeatSeconds: 0.5
    readonly property int defaultBeatsPerBar: 4
    readonly property int semitonesPerOctave: 12
    readonly property real centsPerOctave: centsPerSemitone * semitonesPerOctave
    readonly property var blackKeys: [1, 3, 6, 8, 10]
    // The smallest span divided by, so a zero-length view never divides by 0.
    readonly property real minimumSpan: 1e-6
    // A note blob's half thickness: a core plus its level (capped) times a
    // reach that follows the row height within limits.
    readonly property real blobCore: 1.5
    readonly property real blobLevelCeiling: 1.4
    readonly property real blobRowFill: 0.9
    readonly property real blobMinReach: 3
    readonly property real blobMaxReach: 18
    // Hit testing: slop past a note's ends and around its blob.
    readonly property real hitSlop: 2
    readonly property real hitPad: 3
    readonly property real hitRowFraction: 0.6
    // The waveform's height as a fraction of the plot, each side of centre.
    readonly property real waveformReach: 0.36
    // Notes with more noise than this are hatched; the hatching's slope.
    readonly property real noiseThreshold: 0.15
    readonly property real hatchStep: 5
    readonly property real hatchRise: 20
    readonly property var harmonyDash: [4, 3]
    readonly property var originalDash: [3, 3]
    // A wheel notch's angle delta, and the share of the view one notch scrolls.
    readonly property real wheelNotch: 120
    readonly property real scrollFraction: 0.1
    readonly property var tools: ["main", "pitch", "modulation", "drift", "formant", "amplitude", "time", "separation"]

    // The shown span, song seconds; follows the clip until zoomed.
    property real viewStart: clay ? clay.clipStart : 0
    property real viewEnd: clay ? Math.max(clay.clipStart + minimumViewSeconds, clay.clipEnd) : 1
    property bool zoomed: false
    Connections {
        target: canvas.clay
        function onClipChanged() {
            canvas.zoomed = false
            canvas.fitView()
        }
        function onNotesChanged() { plot.requestPaint() }
        function onWaveformChanged() { plot.requestPaint() }
        function onTakeChanged() { plot.requestPaint(); piano.requestPaint() }
    }
    function fitView() {
        if (!clay || zoomed)
            return
        viewStart = clay.clipStart
        viewEnd = Math.max(clay.clipStart + minimumViewSeconds, clay.clipEnd)
    }

    readonly property real plotX: pianoWidth
    readonly property real plotY: rulerHeight
    readonly property real plotWidth: Math.max(1, width - pianoWidth)
    readonly property real plotHeight: Math.max(1, height - rulerHeight)
    readonly property real lowCents: clay ? clay.lowCents : defaultLowCents
    readonly property real highCents: clay ? Math.max(clay.lowCents + centsPerOctave, clay.highCents) : defaultHighCents
    readonly property real pixelsPerSecond: plotWidth / Math.max(minimumSpan, viewEnd - viewStart)
    readonly property real rowHeight: plotHeight * centsPerSemitone / (highCents - lowCents)
    readonly property real beatSeconds: clay && clay.beatSeconds > 0 ? clay.beatSeconds : defaultBeatSeconds
    readonly property int beatsPerBar: clay && clay.beatsPerBar > 0 ? clay.beatsPerBar : defaultBeatsPerBar

    function xOf(t) { return (t - viewStart) * pixelsPerSecond }
    function tOf(x) { return viewStart + x / pixelsPerSecond }
    function yOf(c) { return (highCents - c) / (highCents - lowCents) * plotHeight }
    function cOf(y) { return highCents - y / plotHeight * (highCents - lowCents) }
    function inScale(pitchClass) {
        if (!clay || clay.keyTonic < 0)
            return true
        const steps = clay.keyMinor ? minorSteps : majorSteps
        return steps.indexOf(((pitchClass - clay.keyTonic) % semitonesPerOctave + semitonesPerOctave) % semitonesPerOctave) >= 0
    }
    function noteName(cents) {
        const semitone = Math.round(cents / centsPerSemitone)
        return Contracts.note_name(semitone, Contracts.sharp, Contracts.scientific_middle_c)
    }
    function halfThickness(level) {
        const most = Math.max(blobMinReach, Math.min(blobMaxReach, rowHeight * blobRowFill))
        return blobCore + Math.max(0, Math.min(blobLevelCeiling, level)) * most
    }
    // The topmost note under a point in plot coordinates, or null.
    function noteAt(x, y) {
        const notes = clay ? clay.notes : []
        for (let i = notes.length - 1; i >= 0; --i) {
            const n = notes[i]
            const x0 = xOf(n.start), x1 = xOf(n.end)
            if (x < x0 - hitSlop || x > x1 + hitSlop)
                continue
            const reach = Math.max(rowHeight * hitRowFraction, halfThickness(n.level) + hitPad)
            if (Math.abs(y - yOf(n.centre)) <= reach)
                return n
        }
        return null
    }
    // The note that starts where `note` ends (Separation's Shift-click joins).
    function nextTouching(note) {
        const notes = clay ? clay.notes : []
        let best = null
        for (const n of notes) {
            if (n.key === note.key || n.voice !== note.voice || n.start < note.start)
                continue
            if (best === null || n.start < best.start)
                best = n
        }
        return best
    }
    function snapSeconds(seconds, fine) {
        const grid = beatSeconds / sixteenthsPerBeat
        return fine ? seconds : Math.round(seconds / grid) * grid
    }
    function zoomAround(t, factor) {
        const span = Math.max(minimumViewSeconds, (viewEnd - viewStart) / factor)
        const ratio = (t - viewStart) / Math.max(minimumSpan, viewEnd - viewStart)
        viewStart = t - ratio * span
        viewEnd = viewStart + span
        zoomed = true
    }

    onViewStartChanged: { plot.requestPaint(); timeRuler.requestPaint() }
    onViewEndChanged: { plot.requestPaint(); timeRuler.requestPaint() }
    onWidthChanged: { plot.requestPaint(); timeRuler.requestPaint() }
    onHeightChanged: { plot.requestPaint(); piano.requestPaint() }
    onToolChanged: plot.requestPaint()
    onShowOriginalChanged: plot.requestPaint()
    onLowCentsChanged: { plot.requestPaint(); piano.requestPaint() }
    onHighCentsChanged: { plot.requestPaint(); piano.requestPaint() }

    // ---- The rulers ----
    Canvas {
        id: piano
        objectName: "clayPianoRuler"
        x: 0; y: canvas.plotY
        width: canvas.pianoWidth; height: canvas.plotHeight
        onPaint: {
            const ctx = getContext("2d")
            const s = canvas.style
            ctx.reset()
            ctx.fillStyle = s.panel2
            ctx.fillRect(0, 0, width, height)
            const first = Math.floor(canvas.lowCents / canvas.centsPerSemitone)
            const last = Math.ceil(canvas.highCents / canvas.centsPerSemitone)
            ctx.font = "600 " + s.typeCaption + "px '" + s.mono + "', monospace"
            ctx.textBaseline = "middle"
            const octave = canvas.semitonesPerOctave
            for (let k = first; k <= last; ++k) {
                const pc = ((k % octave) + octave) % octave
                const y = canvas.yOf(k * canvas.centsPerSemitone)
                const h = canvas.rowHeight
                const black = canvas.blackKeys.indexOf(pc) >= 0
                ctx.fillStyle = black ? s.well : s.pianoWhiteKey
                ctx.fillRect(0, y - h / 2, width - s.pianoKeyInset, h - s.pianoKeyGap)
                if (canvas.inScale(pc) && canvas.clay && canvas.clay.keyTonic >= 0) {
                    ctx.fillStyle = pc === canvas.clay.keyTonic ? s.clay : s.clay3
                    ctx.globalAlpha = pc === canvas.clay.keyTonic ? s.tonicMarkAlpha : s.scaleMarkAlpha
                    ctx.fillRect(width - s.scaleMarkRight, y - h / 2 + s.hairline, s.scaleMarkWidth,
                                 Math.max(s.hairline, h - 2 * s.hairline))
                    ctx.globalAlpha = 1
                }
                if (pc === 0 && h > s.octaveLabelMinRow) {
                    ctx.fillStyle = s.dim
                    ctx.fillText(Contracts.note_name(k, Contracts.sharp, Contracts.scientific_middle_c), s.spaceXs, y)
                }
            }
        }
    }
    Canvas {
        id: timeRuler
        objectName: "clayTimeRuler"
        x: canvas.plotX; y: 0
        width: canvas.plotWidth; height: canvas.rulerHeight
        onPaint: {
            const ctx = getContext("2d")
            const s = canvas.style
            ctx.reset()
            ctx.fillStyle = s.panel2
            ctx.fillRect(0, 0, width, height)
            ctx.fillStyle = s.line
            ctx.fillRect(0, height - s.hairline, width, s.hairline)
            const beat = canvas.beatSeconds
            const beatPixels = beat * canvas.pixelsPerSecond
            ctx.font = "500 " + s.rulerFontSize + "px '" + s.mono + "', monospace"
            ctx.textBaseline = "middle"
            const firstBeat = Math.floor(canvas.viewStart / beat)
            const lastBeat = Math.ceil(canvas.viewEnd / beat)
            for (let b = firstBeat; b <= lastBeat; ++b) {
                const x = canvas.xOf(b * beat)
                const inBar = ((b % canvas.beatsPerBar) + canvas.beatsPerBar) % canvas.beatsPerBar
                const barNumber = Math.floor(b / canvas.beatsPerBar) + 1
                if (inBar === 0) {
                    ctx.fillStyle = s.faint
                    ctx.fillRect(x, s.barTickTop, s.hairline, height - s.barTickTop)
                    ctx.fillStyle = s.dim
                    ctx.fillText(String(barNumber), x + s.spaceXs, height / 2)
                } else if (beatPixels > s.beatTickMinPixels) {
                    ctx.fillStyle = s.line2
                    ctx.fillRect(x, height - s.beatTickHeight - s.hairline, s.hairline, s.beatTickHeight)
                    if (beatPixels > s.beatLabelMinPixels) {
                        ctx.fillStyle = s.faint
                        ctx.fillText(barNumber + "." + (inBar + 1), x + s.beatLabelOffset, height / 2)
                    }
                }
            }
        }
    }
    Rectangle {
        x: 0; y: 0; width: canvas.pianoWidth; height: canvas.rulerHeight
        color: canvas.style.panel2
        Text {
            anchors.centerIn: parent
            text: canvas.clay ? canvas.clay.keyName : ""
            color: canvas.style.faint
            font.family: canvas.style.mono
            font.pixelSize: canvas.style.typeMicro
            width: parent.width - canvas.style.spaceXs
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // ---- The notes ----
    Canvas {
        id: plot
        objectName: "clayNoteCanvas"
        x: canvas.plotX; y: canvas.plotY
        width: canvas.plotWidth; height: canvas.plotHeight
        renderStrategy: Canvas.Cooperative
        function blobPath(ctx, n) {
            const levels = n.levels || []
            const yc = canvas.yOf(n.centre)
            const x0 = canvas.xOf(n.start), x1 = canvas.xOf(n.end)
            ctx.beginPath()
            if (levels.length < 4) {
                const h = canvas.halfThickness(n.level)
                const r = Math.min(h, (x1 - x0) / 2)
                ctx.moveTo(x0 + r, yc - h)
                ctx.lineTo(x1 - r, yc - h)
                ctx.quadraticCurveTo(x1, yc - h, x1, yc)
                ctx.quadraticCurveTo(x1, yc + h, x1 - r, yc + h)
                ctx.lineTo(x0 + r, yc + h)
                ctx.quadraticCurveTo(x0, yc + h, x0, yc)
                ctx.quadraticCurveTo(x0, yc - h, x0 + r, yc - h)
                ctx.closePath()
                return
            }
            ctx.moveTo(x0, yc)
            for (let i = 0; i < levels.length; i += 2) {
                const x = Math.max(x0, Math.min(x1, canvas.xOf(levels[i])))
                ctx.lineTo(x, yc - canvas.halfThickness(levels[i + 1]))
            }
            ctx.lineTo(x1, yc)
            for (let i = levels.length - 2; i >= 0; i -= 2) {
                const x = Math.max(x0, Math.min(x1, canvas.xOf(levels[i])))
                ctx.lineTo(x, yc + canvas.halfThickness(levels[i + 1]))
            }
            ctx.closePath()
        }
        function curvePath(ctx, points) {
            ctx.beginPath()
            for (let i = 0; i < points.length; i += 2) {
                const x = canvas.xOf(points[i]), y = canvas.yOf(points[i + 1])
                if (i === 0)
                    ctx.moveTo(x, y)
                else
                    ctx.lineTo(x, y)
            }
        }
        onPaint: {
            const ctx = getContext("2d")
            const s = canvas.style
            const clay = canvas.clay
            ctx.reset()
            ctx.fillStyle = s.well
            ctx.fillRect(0, 0, width, height)
            // Semitone rows: the scale's notes a shade lighter, the root tinted.
            const first = Math.floor(canvas.lowCents / canvas.centsPerSemitone)
            const last = Math.ceil(canvas.highCents / canvas.centsPerSemitone)
            const octave = canvas.semitonesPerOctave
            for (let k = first; k <= last; ++k) {
                const pc = ((k % octave) + octave) % octave
                const y = canvas.yOf(k * canvas.centsPerSemitone)
                const h = canvas.rowHeight
                if (clay && clay.keyTonic >= 0 && pc === clay.keyTonic) {
                    ctx.fillStyle = s.clay
                    ctx.globalAlpha = s.tonicRowAlpha
                    ctx.fillRect(0, y - h / 2, width, h)
                    ctx.globalAlpha = 1
                } else if (canvas.inScale(pc)) {
                    ctx.fillStyle = s.scaleRowFill
                    ctx.fillRect(0, y - h / 2, width, h)
                }
                if (pc === 0) {
                    ctx.fillStyle = s.line
                    ctx.fillRect(0, y + h / 2, width, s.hairline)
                }
            }
            // Beats, bars and sixteenths.
            const beat = canvas.beatSeconds
            const beatPixels = beat * canvas.pixelsPerSecond
            const sixteenths = beatPixels > s.sixteenthMinPixels
            const step = sixteenths ? beat / canvas.sixteenthsPerBeat : beat
            const firstStep = Math.floor(canvas.viewStart / step)
            const lastStep = Math.ceil(canvas.viewEnd / step)
            for (let i = firstStep; i <= lastStep; ++i) {
                const x = canvas.xOf(i * step)
                const perBar = (sixteenths ? canvas.sixteenthsPerBeat : 1) * canvas.beatsPerBar
                const perBeat = sixteenths ? canvas.sixteenthsPerBeat : 1
                const bar = ((i % perBar) + perBar) % perBar === 0
                const onBeat = ((i % perBeat) + perBeat) % perBeat === 0
                ctx.fillStyle = s.gridLineInk
                ctx.globalAlpha = bar ? s.barLineAlpha : onBeat ? s.beatLineAlpha : s.stepLineAlpha
                ctx.fillRect(x, 0, s.hairline, height)
            }
            ctx.globalAlpha = 1
            // The waveform, faint behind the notes.
            const wave = clay ? clay.waveform : []
            const columns = wave.length / 2
            if (columns > 0 && clay.clipEnd > clay.clipStart) {
                ctx.fillStyle = s.ink
                ctx.globalAlpha = s.waveformAlpha
                const span = clay.clipEnd - clay.clipStart
                const mid = height / 2
                const reach = height * canvas.waveformReach
                const w = Math.max(s.hairline, canvas.pixelsPerSecond * span / columns)
                for (let c = 0; c < columns; ++c) {
                    const x = canvas.xOf(clay.clipStart + span * c / columns)
                    if (x < -w || x > width)
                        continue
                    const lo = wave[2 * c], hi = wave[2 * c + 1]
                    ctx.fillRect(x, mid - hi * reach, w, Math.max(s.hairline, (hi - lo) * reach))
                }
                ctx.globalAlpha = 1
            }
            if (!clay)
                return
            const notes = clay.notes
            for (const n of notes) {
                const x0 = canvas.xOf(n.start), x1 = canvas.xOf(n.end)
                if (x1 < 0 || x0 > width)
                    continue
                const tint = n.muted ? s.faint : n.harmony ? s.violet : s.clay
                // The blob.
                plot.blobPath(ctx, n)
                ctx.fillStyle = tint
                ctx.globalAlpha = n.muted ? s.mutedNoteAlpha : n.selected ? s.selectedNoteAlpha : s.noteAlpha
                ctx.fill()
                ctx.globalAlpha = 1
                ctx.lineWidth = n.selected ? s.selectedNoteStroke : s.hairline
                ctx.strokeStyle = n.selected ? s.clay2 : tint
                if (n.harmony || n.muted)
                    ctx.setLineDash(canvas.harmonyDash)
                ctx.stroke()
                ctx.setLineDash([])
                // Breath and sibilance: hatched as much as the note is noise.
                if (n.noise > canvas.noiseThreshold && !n.muted) {
                    ctx.save()
                    plot.blobPath(ctx, n)
                    ctx.clip()
                    ctx.strokeStyle = s.ink
                    ctx.globalAlpha = Math.min(s.hatchMaxAlpha, n.noise * s.hatchAlphaPerNoise)
                    ctx.lineWidth = s.hairline
                    ctx.beginPath()
                    const yc = canvas.yOf(n.centre)
                    const run = 2 * canvas.hatchRise
                    for (let x = x0 - run; x < x1; x += canvas.hatchStep) {
                        ctx.moveTo(x, yc + canvas.hatchRise)
                        ctx.lineTo(x + run, yc - canvas.hatchRise)
                    }
                    ctx.stroke()
                    ctx.restore()
                }
                // The original curve, then the curve as edited.
                if (canvas.showOriginal && (n.originalCurve || []).length >= 4) {
                    plot.curvePath(ctx, n.originalCurve)
                    ctx.strokeStyle = s.dim
                    ctx.globalAlpha = s.originalCurveAlpha
                    ctx.lineWidth = s.hairline
                    ctx.setLineDash(canvas.originalDash)
                    ctx.stroke()
                    ctx.setLineDash([])
                    ctx.globalAlpha = 1
                }
                if ((n.curve || []).length >= 4) {
                    plot.curvePath(ctx, n.curve)
                    ctx.strokeStyle = n.selected ? s.ink : n.harmony ? s.violet : s.clay2
                    ctx.globalAlpha = n.muted ? s.mutedCurveAlpha : s.curveAlpha
                    ctx.lineWidth = s.curveStroke
                    ctx.stroke()
                    ctx.globalAlpha = 1
                }
                // A moved formant, as a pink mark over the start.
                if (Math.abs(n.formant) >= 1) {
                    const yTop = canvas.yOf(n.centre) - canvas.halfThickness(n.level) - s.formantMarkLift
                    ctx.fillStyle = s.pink
                    ctx.fillRect(x0 + s.spaceXxs, yTop,
                                 Math.max(s.formantMarkMin, Math.min(s.formantMarkMax, x1 - x0 - 2 * s.spaceXxs)),
                                 s.formantMarkHeight)
                    ctx.font = "600 " + s.formantFontSize + "px '" + s.mono + "', monospace"
                    ctx.fillText((n.formant > 0 ? "F +" : "F ") + Math.round(n.formant), x0 + s.spaceXxs,
                                 yTop - s.formantLabelLift)
                }
                if (n.lyric && n.lyric.length > 0) {
                    ctx.fillStyle = s.dim
                    ctx.font = "500 " + s.typeSmall + "px '" + s.sans + "', sans-serif"
                    ctx.fillText(n.lyric, x0 + s.spaceXxs,
                                 canvas.yOf(n.centre) + canvas.halfThickness(n.level) + s.lyricDrop)
                }
            }
        }
    }

    // The selection box while dragging on empty space.
    Rectangle {
        id: box
        visible: false
        color: canvas.style.selectionFill
        border.color: canvas.style.clay
        border.width: canvas.style.hairline
    }
    // The transport's playhead.
    Rectangle {
        objectName: "clayPlayhead"
        visible: canvas.playheadSeconds >= canvas.viewStart && canvas.playheadSeconds <= canvas.viewEnd
        x: canvas.plotX + canvas.xOf(canvas.playheadSeconds)
        y: 0
        width: canvas.style.playheadWidth
        height: canvas.height
        color: canvas.style.clay2
    }
    // A/B: the original is what is heard.
    Rectangle {
        visible: canvas.clay ? canvas.clay.abOriginal : false
        x: canvas.plotX + canvas.style.abBadgeX; y: canvas.plotY + canvas.style.spaceMd
        width: abText.implicitWidth + canvas.style.spaceXl; height: canvas.style.abBadgeHeight
        radius: canvas.style.radiusMd
        color: canvas.style.teal
        Text {
            id: abText
            anchors.centerIn: parent
            text: "ORIGINAL"
            color: canvas.style.tealText
            font.family: canvas.style.mono; font.pixelSize: canvas.style.typeSmall; font.weight: Font.Bold
            font.letterSpacing: canvas.style.labelSpacing
        }
    }
    // Detection sweeps across the clip.
    Rectangle {
        objectName: "clayDetectSweep"
        visible: canvas.clay ? canvas.clay.detecting : false
        x: canvas.plotX + Math.max(0, canvas.plotWidth * (canvas.clay ? canvas.clay.progress : 0) - width)
        y: canvas.plotY
        width: Math.min(canvas.style.sweepWidth, canvas.plotWidth)
        height: canvas.plotHeight
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: canvas.style.sweepGlow }
        }
        Rectangle { anchors.right: parent.right; width: canvas.style.strokeBold; height: parent.height; color: canvas.style.clay2 }
    }

    // ---- Input ----
    activeFocusOnTab: true
    Accessible.role: Accessible.Canvas
    Accessible.name: "Clay notes"
    Keys.onPressed: function(event) {
        const digit = event.key - Qt.Key_1
        if (event.modifiers === Qt.NoModifier && digit >= 0 && digit < canvas.tools.length) {
            canvas.toolRequested(canvas.tools[digit])
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            canvas.clay.delete_selection()
            event.accepted = true
        } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
            canvas.clay.select_all()
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            if (input.begun) {
                canvas.clay.cancel_edit()
                input.reset()
            } else {
                canvas.clay.clear_selection()
            }
            event.accepted = true
        } else if (event.key === Qt.Key_Space && event.modifiers === Qt.NoModifier) {
            canvas.playRequested()
            event.accepted = true
        }
    }
    // The dock's keys stay with the canvas while it has focus.
    Keys.onShortcutOverride: function(event) {
        event.accepted = event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace
            || event.key === Qt.Key_Space || event.key === Qt.Key_Escape
            || (event.modifiers === Qt.NoModifier && event.key >= Qt.Key_1 && event.key <= Qt.Key_8)
    }

    MouseArea {
        id: input
        objectName: "clayCanvasInput"
        x: canvas.plotX; y: canvas.plotY
        width: canvas.plotWidth; height: canvas.plotHeight
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: Qt.LeftButton
        property var hit: null
        property string kind: ""
        property bool begun: false
        property bool boxing: false
        property real pressX: 0
        property real pressY: 0
        property bool fine: false
        function reset() {
            hit = null
            kind = ""
            begun = false
            boxing = false
            box.visible = false
        }
        function editKind(note, x) {
            if (canvas.tool === "main")
                return "move"
            if (canvas.tool === "time") {
                const x0 = canvas.xOf(note.start), x1 = canvas.xOf(note.end)
                return x - x0 <= canvas.edgeGrab ? "time_start"
                    : x1 - x <= canvas.edgeGrab ? "time_end" : "time_move"
            }
            return canvas.tool
        }
        cursorShape: {
            if (pressed && begun)
                return Qt.ClosedHandCursor
            const note = canvas.noteAt(mouseX, mouseY)
            if (!note)
                return Qt.CrossCursor
            if (canvas.tool === "separation")
                return Qt.SplitHCursor
            if (canvas.tool === "time") {
                const k = editKind(note, mouseX)
                return k === "time_move" ? Qt.SizeAllCursor : Qt.SizeHorCursor
            }
            return canvas.tool === "main" ? Qt.OpenHandCursor : Qt.SizeVerCursor
        }
        onPressed: function(mouse) {
            canvas.forceActiveFocus()
            reset()
            pressX = mouse.x
            pressY = mouse.y
            fine = (mouse.modifiers & Qt.AltModifier) !== 0
            const note = canvas.noteAt(mouse.x, mouse.y)
            const additive = (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)) !== 0
            if (canvas.tool === "separation") {
                if (note) {
                    if (mouse.modifiers & Qt.ShiftModifier) {
                        const next = canvas.nextTouching(note)
                        if (next)
                            canvas.clay.join(note.key, next.key)
                    } else {
                        canvas.clay.split(note.key, canvas.tOf(mouse.x))
                    }
                }
                return
            }
            if (note) {
                if (!note.selected || additive)
                    canvas.clay.select(note.key, additive)
                hit = note
                kind = editKind(note, mouse.x)
            } else {
                boxing = true
            }
        }
        onPositionChanged: function(mouse) {
            if (!pressed)
                return
            const dx = mouse.x - pressX, dy = mouse.y - pressY
            if (boxing) {
                box.visible = Math.abs(dx) > canvas.dragThreshold || Math.abs(dy) > canvas.dragThreshold
                box.x = canvas.plotX + Math.min(pressX, mouse.x)
                box.y = canvas.plotY + Math.min(pressY, mouse.y)
                box.width = Math.abs(dx)
                box.height = Math.abs(dy)
                return
            }
            if (!hit)
                return
            if (!begun) {
                if (Math.abs(dx) <= canvas.dragThreshold && Math.abs(dy) <= canvas.dragThreshold)
                    return
                begun = canvas.clay.begin_edit(kind)
                if (!begun) {
                    hit = null
                    return
                }
            }
            const seconds = canvas.snapSeconds(dx / canvas.pixelsPerSecond, fine)
            const cents = -dy / canvas.rowHeight * canvas.centsPerSemitone
            let amount = 0
            if (kind === "modulation" || kind === "drift")
                amount = Math.pow(2, -dy / canvas.modulationPixels)
            else if (kind === "formant")
                amount = -dy * canvas.formantCentsPerPixel
            else if (kind === "amplitude")
                amount = -dy * canvas.gainDbPerPixel
            const timed = kind === "move" || kind.startsWith("time")
            const pitched = kind === "move" || kind === "pitch"
            canvas.clay.update_edit(timed ? seconds : 0, pitched ? cents : 0, amount)
        }
        onReleased: function(mouse) {
            if (begun) {
                canvas.clay.commit_edit()
            } else if (boxing) {
                if (box.visible) {
                    const t0 = canvas.tOf(Math.min(pressX, mouse.x))
                    const t1 = canvas.tOf(Math.max(pressX, mouse.x))
                    const c0 = canvas.cOf(Math.max(pressY, mouse.y))
                    const c1 = canvas.cOf(Math.min(pressY, mouse.y))
                    canvas.clay.select_box(t0, t1, c0, c1,
                                           (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)) !== 0)
                } else {
                    canvas.clay.clear_selection()
                }
            } else if (hit && canvas.auditionOnClick) {
                canvas.auditionRequested(hit.key)
            }
            reset()
        }
        onDoubleClicked: function(mouse) {
            const note = canvas.noteAt(mouse.x, mouse.y)
            if (!note)
                return
            if (canvas.tool === "main")
                canvas.clay.split(note.key, canvas.tOf(mouse.x))
            else if (canvas.tool === "pitch") {
                canvas.clay.select(note.key, false)
                canvas.clay.snap_selection()
            }
        }
        onWheel: function(wheel) {
            const t = canvas.tOf(wheel.x)
            if (wheel.modifiers & Qt.ControlModifier) {
                canvas.zoomAround(t, wheel.angleDelta.y > 0 ? canvas.zoomStep : 1 / canvas.zoomStep)
            } else {
                const delta = wheel.angleDelta.x !== 0 ? wheel.angleDelta.x : wheel.angleDelta.y
                const shift = -delta / canvas.wheelNotch * (canvas.viewEnd - canvas.viewStart) * canvas.scrollFraction
                canvas.viewStart += shift
                canvas.viewEnd += shift
                canvas.zoomed = true
            }
            wheel.accepted = true
        }
    }
}
