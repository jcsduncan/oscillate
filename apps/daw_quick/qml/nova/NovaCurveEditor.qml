pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// The drawable LFO shape (NOVA-MOD-02): points joined by bent or stepped
// segments, snapped to the LFO's grid (Shift is free). Press empty space to
// add a point and drag it; drag a point to move it; Alt-drag a segment to
// bend it; double-click a point to toggle its segment between curve and
// step; right-click a point to remove it. Each drag is one document gesture
// (begin, updates, commit). An amber playhead follows the LFO's phase.
Kit.CurveEditor {
    id: editor
    required property var panel
    required property string lfo
    readonly property int number: parseInt(lfo.substring(4))
    readonly property string base: "nova." + lfo
    readonly property int grid: panel.controller.revision >= 0 ? Math.max(1, Math.round(panel.controller.value(base + ".grid"))) : 8
    readonly property real phase: panel.at(Contract.T.lfoPhase + number - 1)
    readonly property real pad: panel.theme.lfoEditorPad
    // The room above the plot for the caption.
    readonly property real captionRoom: panel.theme.lfoCaptionRoom
    // The step mark under a step point: its size and gap below the point.
    readonly property real stepMarkSize: 3
    readonly property real stepMarkGap: 7
    readonly property real playheadDotRadius: panel.theme.lfoPlayheadDotRadius
    readonly property real fillFloorAlpha: panel.theme.lfoFillFloorAlpha

    objectName: "novaCurveEditor:" + lfo
    theme: panel.theme
    doc: panel.doc
    curveId: lfo
    editable: panel.controller.editable
    plotLeft: pad
    plotRight: width - pad
    plotTop: pad + captionRoom
    plotBottom: height - pad
    snapSteps: grid
    altBends: true
    pressAddsPoint: true
    doubleClickSteps: true
    lastPointRemovable: true
    focusOnPress: false
    backLayer: false

    onPointGrabbed: editor.panel.touchId(editor.base + ".rate", "LFO " + editor.number + " SHAPE")
    onPaintShape: ctx => {
        const t = editor.theme
        const pts = editor.points
        ctx.lineWidth = t.hairline
        for (let i = 0; i <= editor.grid; ++i) {
            ctx.strokeStyle = i === 0 || i === editor.grid ? t.gridLine2 : t.gridLine
            ctx.beginPath()
            ctx.moveTo(editor.px(i / editor.grid), editor.pad)
            ctx.lineTo(editor.px(i / editor.grid), editor.height - editor.pad)
            ctx.stroke()
        }
        ctx.strokeStyle = t.gridLine
        ctx.beginPath()
        ctx.moveTo(editor.pad, editor.py(0.5))
        ctx.lineTo(editor.width - editor.pad, editor.py(0.5))
        ctx.stroke()
        if (pts.length < 2)
            return
        // The glow is a wide faint stroke under the line: a software shadow
        // blur costs a hundred times more to paint.
        editor.traceCurve(ctx, editor.sample(pts, t.curveSamples))
        editor.strokeCurve(ctx, [])
        ctx.lineTo(editor.px(1), editor.py(0))
        ctx.lineTo(editor.px(0), editor.py(0))
        ctx.closePath()
        const fill = ctx.createLinearGradient(0, editor.pad, 0, editor.height)
        fill.addColorStop(0, t.soft(t.vi, t.glowAlpha))
        fill.addColorStop(1, t.soft(t.vi, editor.fillFloorAlpha))
        ctx.fillStyle = fill
        ctx.fill()
        editor.paintPoints(ctx, i => false, (i, x, y) => {
            if (pts[i].step && i + 1 < pts.length) {
                ctx.fillStyle = t.vi
                ctx.fillRect(x - editor.stepMarkSize / 2, y + editor.stepMarkGap,
                             editor.stepMarkSize, editor.stepMarkSize)
            }
        })
        if (editor.panel.live) {
            const x = editor.px(editor.phase)
            ctx.fillStyle = t.am
            ctx.fillRect(x - t.curvePlayheadWidth / 2, editor.pad, t.curvePlayheadWidth, editor.height - 2 * editor.pad)
            ctx.beginPath()
            ctx.arc(x, editor.py(editor.valueAt(editor.phase)), editor.playheadDotRadius, 0, Math.PI * 2)
            ctx.fill()
        }
    }
    onGridChanged: editor.repaintShape()
    onPhaseChanged: editor.repaintShape()

    Text {
        x: editor.panel.theme.displayCaptionX
        y: editor.panel.theme.displayCaptionY
        text: "LFO " + editor.number + " · DRAW POINTS · ALT-DRAG CURVES"
        color: editor.panel.theme.faint
        font.family: editor.panel.theme.sans
        font.pixelSize: editor.panel.theme.typeSmall
        font.weight: Font.Bold
        font.letterSpacing: editor.panel.theme.displayCaptionSpacing
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: editor.panel.theme.displayCaptionX
        y: editor.panel.theme.displayReadoutY
        text: editor.panel.controller.revision < 0 ? ""
            : editor.panel.controller.choice(editor.base + ".sync") === 0
              ? editor.panel.controller.display(editor.base + ".rate") + " · FREE RATE"
              : editor.panel.controller.display(editor.base + ".sync") + " · SYNC"
        color: editor.panel.theme.am
        font.family: editor.panel.theme.mono
        font.pixelSize: editor.panel.theme.typeSmall
    }
}
