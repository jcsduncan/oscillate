pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// The wavetable editor overlay (NOVA-UI-15): DRAW / FORMULA tools, the frame
// editor, a draggable harmonics view, a formula field and a frame strip,
// with UNDO kept in the editor. DONE commits the oscillator's table as one
// document edit; CANCEL leaves it. Saving drawn frames needs managed-media
// writes (spec phase 6), so drawn frames are a preview until then, and the
// import, image and export tools stay disabled.
Item {
    id: editor
    required property var panel
    required property string osc
    readonly property string slot: "osc." + osc
    readonly property var savedMedia: panel.doc.media(slot)
    // The table being edited; DONE commits it.
    property string table: savedMedia.kind === "builtin" && savedMedia.name !== "" ? savedMedia.name : "basic_shapes"
    property string mode: "draw"
    property int selected: 10
    property var frames: []
    property var history: []
    property string formula: "sin(w*x + y*3*sin(2*w*x))"
    property string formulaError: ""
    readonly property int frameCount: 32
    readonly property int points: 256
    property int revision: 0
    // UNDO keeps this many steps.
    readonly property int historyLimit: 50
    // The harmonics view shows and edits this many partials, the first
    // `litHarmonics` lit.
    readonly property int harmonicCount: 40
    readonly property int litHarmonics: 12
    readonly property var theme: panel.theme

    objectName: "novaTableEditor"

    function load() {
        const next = []
        for (let f = 0; f < frameCount; ++f) {
            const frame = []
            for (let i = 0; i < points; ++i)
                frame.push(Contract.tableSample(table, f / (frameCount - 1), i / points))
            next.push(frame)
        }
        frames = next
        history = []
        ++revision
    }
    function remember() {
        history.push(frames.map(f => f.slice()))
        if (history.length > historyLimit)
            history.shift()
    }
    function undo() {
        if (history.length === 0)
            return
        frames = history.pop()
        ++revision
    }
    function changed() {
        ++revision
    }
    function harmonics(frame, count) {
        const out = []
        for (let h = 1; h <= count; ++h) {
            let re = 0, im = 0
            for (let i = 0; i < frame.length; ++i) {
                const a = 2 * Math.PI * h * i / frame.length
                re += frame[i] * Math.cos(a)
                im -= frame[i] * Math.sin(a)
            }
            out.push({ magnitude: 2 * Math.hypot(re, im) / frame.length, phase: Math.atan2(im, re) })
        }
        return out
    }
    function setHarmonic(index, magnitude) {
        const frame = frames[selected]
        const bins = harmonics(frame, harmonicCount)
        bins[index].magnitude = magnitude
        for (let i = 0; i < points; ++i) {
            let v = 0
            for (let h = 0; h < bins.length; ++h)
                v += bins[h].magnitude * Math.cos(2 * Math.PI * (h + 1) * i / points + bins[h].phase)
            frame[i] = Math.max(-1, Math.min(1, v))
        }
        changed()
    }
    function applyFormula() {
        let f
        try {
            f = new Function("x", "y", "w",
                "const {sin, cos, tan, abs, pow, exp, log, sqrt, floor, ceil, min, max, sign, tanh, PI} = Math; return (" + formula + ");")
            f(0.5, 0.5, 2 * Math.PI)
        } catch (error) {
            formulaError = String(error)
            return
        }
        remember()
        for (let fr = 0; fr < frameCount; ++fr)
            for (let i = 0; i < points; ++i) {
                const v = Number(f(i / points, fr / (frameCount - 1), 2 * Math.PI))
                frames[fr][i] = isFinite(v) ? Math.max(-1, Math.min(1, v)) : 0
            }
        formulaError = ""
        changed()
    }
    function normalise() {
        remember()
        const frame = frames[selected]
        const peak = frame.reduce((m, v) => Math.max(m, Math.abs(v)), 0)
        if (peak > 1e-6)
            for (let i = 0; i < points; ++i)
                frame[i] /= peak
        changed()
    }
    function removeDc() {
        remember()
        const frame = frames[selected]
        const mean = frame.reduce((s, v) => s + v, 0) / points
        for (let i = 0; i < points; ++i)
            frame[i] -= mean
        changed()
    }
    function done() {
        if (savedMedia.kind !== "builtin" || savedMedia.name !== table)
            panel.doc.set_builtin_media(slot, table, Contract.tableFrameLength)
        panel.editorOsc = ""
    }

    Component.onCompleted: load()
    onTableChanged: load()

    // The scrim: the panel behind is dimmed and inert.
    Rectangle {
        anchors.fill: parent
        color: editor.theme.soft(editor.theme.bg, editor.theme.tableScrimAlpha)
        MouseArea {
            anchors.fill: parent
            onWheel: wheel => { wheel.accepted = true }
        }
    }

    Rectangle {
        id: sheet
        x: editor.theme.tableSheetSide
        y: editor.theme.tableSheetTop
        width: parent.width - 2 * editor.theme.tableSheetSide
        height: parent.height - editor.theme.tableSheetTop - editor.theme.tableSheetBottom
        radius: editor.theme.tableSheetRadius
        color: editor.panel.theme.popFill
        border.color: editor.panel.theme.line2

        // Header.
        Row {
            x: editor.theme.tableSheetPadding
            y: editor.theme.tableSheetPadding
            spacing: editor.theme.tableHeaderSpacing
            Text {
                text: "WAVETABLE EDITOR"
                color: editor.panel.theme.text
                font.family: editor.panel.theme.sans
                font.pixelSize: editor.theme.typeSmall
                font.weight: Font.ExtraBold
                font.letterSpacing: editor.theme.tableTitleSpacing
            }
            Text {
                text: "OSC " + editor.osc.toUpperCase() + " · " + Contract.mediaName(Contract.tables, editor.table, "").toUpperCase()
                      + " · 256 FRAMES × 2048 SAMPLES"
                color: editor.panel.theme.faint
                font.family: editor.panel.theme.sans
                font.pixelSize: editor.theme.typeSmall
                font.letterSpacing: editor.theme.tableSubtitleSpacing
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: editor.theme.tableSheetPadding
            y: editor.theme.tableActionsTop
            spacing: editor.theme.spaceSm
            NovaDropdown {
                objectName: "novaEditorTable"
                panel: editor.panel
                text: Contract.mediaName(Contract.tables, editor.table, "")
                entries: Contract.tables.map(t => ({ label: t.name, value: t.id }))
                currentValue: editor.table
                width: editor.theme.tableDropdownWidth
                onPicked: value => editor.table = value
            }
            NovaPill {
                objectName: "novaEditorUndo"
                panel: editor.panel
                text: "UNDO"
                opacity: editor.history.length > 0 || editor.revision < 0 ? 1 : editor.theme.tableIdleOpacity
                onClicked: editor.undo()
            }
            NovaPill {
                objectName: "novaEditorCancel"
                panel: editor.panel
                text: "CANCEL"
                onClicked: editor.panel.editorOsc = ""
            }
            NovaPill {
                objectName: "novaEditorDone"
                panel: editor.panel
                text: "DONE"
                localOn: true
                onClicked: editor.done()
            }
        }

        // Tools.
        Row {
            x: editor.theme.tableSheetPadding
            y: editor.theme.tableToolsTop
            spacing: editor.theme.spaceSm
            NovaPill { panel: editor.panel; text: "DRAW"; localOn: editor.mode === "draw"; onClicked: editor.mode = "draw" }
            NovaPill { panel: editor.panel; text: "FORMULA"; localOn: editor.mode === "formula"; onClicked: editor.mode = "formula" }
            NovaPill { panel: editor.panel; text: "IMPORT AUDIO → RESYNTH"; opacity: editor.theme.tableUnavailableOpacity; enabled: false }
            NovaPill { panel: editor.panel; text: "FROM IMAGE"; opacity: editor.theme.tableUnavailableOpacity; enabled: false }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: editor.theme.tableSheetPadding
            y: editor.theme.tableToolsTop
            spacing: editor.theme.spaceSm
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "MORPH"
                color: editor.panel.theme.faint
                font.family: editor.panel.theme.sans
                font.pixelSize: editor.theme.typeSmall
                font.weight: Font.Bold
                font.letterSpacing: editor.theme.tableMorphSpacing
            }
            NovaSegs {
                panel: editor.panel
                parameterId: "nova.osc." + editor.osc + ".interpolation"
                labels: ["CROSSFADE", "SPECTRAL"]
            }
            NovaPill { panel: editor.panel; text: "NORMALISE"; onClicked: editor.normalise() }
            NovaPill { panel: editor.panel; text: "REMOVE DC"; onClicked: editor.removeDc() }
            NovaPill { panel: editor.panel; text: "EXPORT .WAV"; opacity: editor.theme.tableUnavailableOpacity; enabled: false }
        }

        // The frame editor.
        Rectangle {
            id: wave
            objectName: "novaEditorWave"
            // The frame's peak reaches this fraction of the height from the middle.
            readonly property real reach: 0.38
            // Vertical grid divisions, and the neighbouring frames drawn each side.
            readonly property int gridColumns: 8
            readonly property int neighbours: 2
            x: editor.theme.tableSheetPadding
            y: editor.theme.tableEditorTop
            width: sheet.width - 2 * editor.theme.tableSheetPadding - editor.theme.tableSideWidth - editor.theme.tableSideGap
            height: sheet.height - y - editor.theme.tableFrameStripRoom
            radius: editor.theme.radiusXl
            color: editor.panel.theme.wellFill
            border.color: editor.panel.theme.line
            clip: true
            Canvas {
                id: waveCanvas
                anchors.fill: parent
                renderStrategy: Canvas.Cooperative
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const t = editor.panel.theme
                    ctx.strokeStyle = t.gridLine
                    ctx.lineWidth = t.hairline
                    for (let i = 1; i < wave.gridColumns; ++i) {
                        ctx.beginPath()
                        ctx.moveTo(i * width / wave.gridColumns, 0)
                        ctx.lineTo(i * width / wave.gridColumns, height)
                        ctx.stroke()
                    }
                    ctx.beginPath()
                    ctx.moveTo(0, height / 2)
                    ctx.lineTo(width, height / 2)
                    ctx.stroke()
                    const frame = editor.frames[editor.selected]
                    if (!frame)
                        return
                    // Neighbouring frames, faint, for context.
                    for (let d = -wave.neighbours; d <= wave.neighbours; ++d) {
                        const other = editor.frames[editor.selected + d]
                        if (d === 0 || !other)
                            continue
                        ctx.beginPath()
                        for (let i = 0; i < other.length; ++i) {
                            const x = i / (other.length - 1) * width, y = height / 2 - other[i] * height * wave.reach
                            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                        }
                        ctx.strokeStyle = t.soft(t.dim, t.tableNeighbourAlpha)
                        ctx.stroke()
                    }
                    ctx.beginPath()
                    for (let i = 0; i < frame.length; ++i) {
                        const x = i / (frame.length - 1) * width, y = height / 2 - frame[i] * height * wave.reach
                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                    }
                    // The glow is a wide faint stroke under the line: a software shadow
                    // blur costs a hundred times more to paint.
                    ctx.strokeStyle = t.soft(t.cy, t.glowAlpha)
                    ctx.lineWidth = t.tableGlowWidth
                    ctx.stroke()
                    ctx.strokeStyle = t.cy
                    ctx.lineWidth = t.tableLineWidth
                    ctx.stroke()
                }
            }
            Text {
                x: editor.theme.spaceMd
                y: editor.theme.spaceSm
                text: "FRAME " + (editor.selected + 1) + " · DRAW WITH THE MOUSE · SHIFT FOR STRAIGHT LINES"
                color: editor.panel.theme.faint
                font.family: editor.panel.theme.sans
                font.pixelSize: editor.theme.typeSmall
                font.weight: Font.Bold
                font.letterSpacing: editor.theme.tableLabelSpacing
            }
            MouseArea {
                property real lastX: -1
                property real lastY: 0
                property real startX: 0
                property real startY: 0
                anchors.fill: parent
                enabled: editor.mode === "draw"
                preventStealing: true
                cursorShape: Qt.CrossCursor
                function paintSegment(x0, y0, x1, y1) {
                    const frame = editor.frames[editor.selected]
                    const i0 = Math.round(Math.max(0, Math.min(1, x0 / width)) * (editor.points - 1))
                    const i1 = Math.round(Math.max(0, Math.min(1, x1 / width)) * (editor.points - 1))
                    const v0 = Math.max(-1, Math.min(1, (height / 2 - y0) / (height * wave.reach)))
                    const v1 = Math.max(-1, Math.min(1, (height / 2 - y1) / (height * wave.reach)))
                    const from = Math.min(i0, i1), to = Math.max(i0, i1)
                    for (let i = from; i <= to; ++i)
                        frame[i] = to === from ? v1 : (i0 <= i1 ? v0 + (v1 - v0) * (i - i0) / (i1 - i0) : v1 + (v0 - v1) * (i - i1) / (i0 - i1))
                    editor.changed()
                }
                onPressed: mouse => {
                    editor.remember()
                    startX = mouse.x
                    startY = mouse.y
                    lastX = mouse.x
                    lastY = mouse.y
                    paintSegment(mouse.x, mouse.y, mouse.x, mouse.y)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    if (mouse.modifiers & Qt.ShiftModifier) {
                        editor.frames[editor.selected] = editor.history[editor.history.length - 1][editor.selected].slice()
                        paintSegment(startX, startY, mouse.x, mouse.y)
                    } else {
                        paintSegment(lastX, lastY, mouse.x, mouse.y)
                    }
                    lastX = mouse.x
                    lastY = mouse.y
                }
            }
        }

        // Harmonics, formula, resynthesis.
        Column {
            x: wave.x + wave.width + editor.theme.tableSideGap
            y: wave.y
            width: editor.theme.tableSideWidth
            spacing: editor.theme.spaceMd
            Rectangle {
                id: harmonicsView
                objectName: "novaEditorHarmonics"
                // The bars sit `barInset` inside the sides and bottom, leave
                // `barRoom` of the height free (the caption) and a gap between.
                readonly property real barInset: 8
                readonly property real barRoom: 30
                readonly property real barGap: 2
                width: parent.width
                height: editor.theme.tableHarmonicsHeight
                radius: editor.theme.radiusXl
                color: editor.panel.theme.wellFill
                border.color: editor.panel.theme.line
                clip: true
                Canvas {
                    id: harmonicsCanvas
                    anchors.fill: parent
                    renderStrategy: Canvas.Cooperative
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        const frame = editor.frames[editor.selected]
                        if (!frame)
                            return
                        const inset = harmonicsView.barInset
                        const room = height - harmonicsView.barRoom
                        const bins = editor.harmonics(frame, editor.harmonicCount)
                        const w = (width - 2 * inset) / editor.harmonicCount
                        for (let h = 0; h < bins.length; ++h) {
                            const a = Math.min(1, bins[h].magnitude)
                            ctx.fillStyle = h < editor.litHarmonics ? editor.panel.theme.cy : editor.panel.theme.line2
                            ctx.fillRect(inset + h * w, height - inset - a * room, w - harmonicsView.barGap, a * room)
                        }
                    }
                }
                Text {
                    x: editor.theme.spaceMd
                    y: editor.theme.spaceSm
                    text: "HARMONICS · DRAG BARS"
                    color: editor.panel.theme.faint
                    font.family: editor.panel.theme.sans
                    font.pixelSize: editor.theme.typeSmall
                    font.weight: Font.Bold
                    font.letterSpacing: editor.theme.tableLabelSpacing
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function set(mouse) {
                        const inset = harmonicsView.barInset
                        const h = Math.floor((mouse.x - inset) / ((width - 2 * inset) / editor.harmonicCount))
                        if (h >= 0 && h < editor.harmonicCount)
                            editor.setHarmonic(h, Math.max(0, Math.min(1, (height - inset - mouse.y)
                                                                          / (height - harmonicsView.barRoom))))
                    }
                    onPressed: mouse => {
                        editor.remember()
                        set(mouse)
                    }
                    onPositionChanged: mouse => { if (pressed) set(mouse) }
                }
            }
            NovaCard {
                panel: editor.panel
                title: "FORMULA"
                width: parent.width
                height: editor.theme.tableFormulaCardHeight
                Rectangle {
                    x: editor.theme.spaceLg
                    y: editor.theme.tableCardBodyTop
                    width: parent.width - 2 * editor.theme.spaceLg
                    height: editor.theme.tableFieldHeight
                    radius: editor.theme.radiusLg
                    color: editor.panel.theme.wellFill
                    border.color: formulaInput.activeFocus ? editor.panel.theme.cy : editor.panel.theme.line
                    TextInput {
                        id: formulaInput
                        objectName: "novaEditorFormula"
                        x: editor.theme.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 2 * editor.theme.spaceMd
                        text: editor.formula
                        color: editor.panel.theme.cy
                        font.family: editor.panel.theme.mono
                        font.pixelSize: editor.theme.typeBody
                        clip: true
                        selectByMouse: true
                        onAccepted: {
                            editor.formula = text
                            editor.applyFormula()
                        }
                    }
                }
                Text {
                    x: editor.theme.spaceLg
                    y: editor.theme.tableFormulaFootTop
                    width: parent.width - editor.theme.tableFormulaHelpRoom
                    text: editor.formulaError !== "" ? editor.formulaError : "x is the phase, y the frame (0–1), w is 2π."
                    color: editor.formulaError !== "" ? editor.panel.theme.red : editor.panel.theme.faint
                    font.family: editor.panel.theme.sans
                    font.pixelSize: editor.theme.typeSmall
                    wrapMode: Text.WordWrap
                }
                NovaPill {
                    anchors.right: parent.right
                    anchors.rightMargin: editor.theme.spaceLg
                    y: editor.theme.tableFormulaFootTop
                    panel: editor.panel
                    text: "APPLY"
                    onClicked: {
                        editor.formula = formulaInput.text
                        editor.applyFormula()
                    }
                }
            }
            NovaCard {
                panel: editor.panel
                title: "RESYNTHESIS"
                width: parent.width
                height: editor.theme.tableResynthesisCardHeight
                Text {
                    x: editor.theme.spaceLg
                    y: editor.theme.tableCardBodyTop
                    width: parent.width - 2 * editor.theme.spaceLg
                    text: "Import audio, detect its pitch, slice one cycle per frame and band-limit it; or spread a whole sample over 256 frames. Arrives with media import."
                    color: editor.panel.theme.faint
                    font.family: editor.panel.theme.sans
                    font.pixelSize: editor.theme.typeSmall
                    lineHeight: editor.theme.tableNoteLineHeight
                    wrapMode: Text.WordWrap
                }
            }
        }

        // The frame strip.
        Row {
            objectName: "novaEditorFrames"
            x: editor.theme.tableSheetPadding
            y: sheet.height - editor.theme.tableFrameStripBottom
            spacing: editor.theme.spaceXs
            Repeater {
                model: editor.frameCount
                delegate: Rectangle {
                    id: thumb
                    required property int index
                    width: Math.min(editor.theme.tableThumbMaxWidth,
                                    (sheet.width - 2 * editor.theme.tableSheetPadding
                                     - (editor.frameCount - 1) * editor.theme.spaceXs) / editor.frameCount)
                    height: editor.theme.tableThumbHeight
                    radius: editor.theme.tableThumbRadius
                    color: editor.panel.theme.wellFill
                    border.color: index === editor.selected ? editor.panel.theme.cy : editor.panel.theme.line
                    Canvas {
                        id: thumbCanvas
                        anchors.fill: parent
                        // Every `stride`-th point, reaching this fraction of the height.
                        readonly property int stride: 4
                        readonly property real reach: 0.35
                        anchors.margins: editor.theme.spaceXxs
                        renderStrategy: Canvas.Cooperative
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const frame = editor.frames[thumb.index]
                            if (!frame)
                                return
                            ctx.beginPath()
                            for (let i = 0; i < frame.length; i += thumbCanvas.stride) {
                                const x = i / (frame.length - 1) * width, y = height / 2 - frame[i] * height * thumbCanvas.reach
                                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                            }
                            ctx.strokeStyle = editor.panel.theme.cy
                            ctx.lineWidth = editor.theme.tableThumbLineWidth
                            ctx.stroke()
                        }
                        Connections {
                            target: editor
                            function onRevisionChanged() { thumbCanvas.requestPaint() }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: editor.selected = thumb.index
                    }
                }
            }
        }
    }

    onRevisionChanged: {
        waveCanvas.requestPaint()
        harmonicsCanvas.requestPaint()
    }
    onSelectedChanged: {
        waveCanvas.requestPaint()
        harmonicsCanvas.requestPaint()
    }
}
