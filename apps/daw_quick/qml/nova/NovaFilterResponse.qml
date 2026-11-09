pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// A filter's response (NOVA-UI-08), the FILTER card's display: the
// live curve at the modulated cutoff and resonance (telemetry) with the
// saved setting ghosted behind it, in the filters' accent, in a plot frame
// (Kit.PlotFrame: Hz across, dB up; LIVE while the synth sounds). Drag on
// it: across sets the cutoff, up and down the resonance (one undo step);
// double-click resets both. The CUTOFF and RESONANCE knobs set the same
// from the keyboard.
Rectangle {
    id: view
    required property var panel
    required property int filter
    readonly property string base: "nova.filter." + filter
    readonly property var controller: panel.controller
    readonly property color accent: panel.accentFor(base)
    readonly property int model: controller.revision >= 0 ? controller.choice(base + ".model") : 0
    readonly property real savedCutoff: controller.revision >= 0 ? controller.value(base + ".cutoff") : 1000
    readonly property real savedResonance: controller.revision >= 0 ? controller.value(base + ".resonance") : 0
    readonly property real cutoff: panel.live ? panel.at(Contract.T.cutoff + filter - 1) : savedCutoff
    readonly property real resonance: panel.live ? panel.at(Contract.T.resonance + filter - 1) : savedResonance
    // The plot: 0 dB at this share of the height, 36 dB over this share.
    readonly property real zeroAt: 0.42
    readonly property real dbSpan: 0.55
    readonly property real dbFloor: -40
    readonly property real dbCeiling: 24
    readonly property real dbRange: 36
    readonly property int gridDivisions: 4
    readonly property real fillAlpha: panel.theme.displayFillAlpha
    readonly property real ghostAlpha: panel.theme.displayGhostAlpha
    // The pixels between the trace's samples; the cutoff marker's dot and
    // its lift off the floor.
    readonly property int sampleStep: 2
    readonly property real markerRadius: 2.5
    readonly property real markerLift: 6
    // The frame's scale: these frequencies across and levels up.
    readonly property var hzTicks: [100, 1000, 10000]
    readonly property var hzLabels: ["100", "1k", "10k"]
    readonly property var dbTicks: [-24, 0, 12]
    readonly property var dbLabels: ["-24", "0", "+12"]

    // A level's place up the plot, 0 at the foot and 1 at the top.
    function dbFraction(db) {
        return 1 - zeroAt + db / dbRange * dbSpan
    }

    radius: panel.theme.displayRadius
    color: panel.theme.wellFill
    border.color: panel.theme.line
    clip: true

    Kit.PlotFrame {
        anchors.fill: parent
        anchors.margins: view.panel.theme.spaceXs
        theme: view.panel.theme
        live: view.panel.live
        xUnit: "Hz"
        yUnit: "dB"
        xTicks: view.hzTicks.map((hz, i) => ({ at: Contract.displayFraction(hz), label: view.hzLabels[i] }))
        yTicks: view.dbTicks.map((db, i) => ({ at: view.dbFraction(db), label: view.dbLabels[i] }))

        Item {
            id: plot
            objectName: "novaFilterResponse:" + view.filter
            anchors.fill: parent

            Canvas {
                id: canvas
                anchors.fill: parent
                renderStrategy: Canvas.Cooperative
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const t = view.panel.theme
                    const W = width, H = height
                    ctx.strokeStyle = t.knobTrack
                    ctx.lineWidth = t.hairline
                    for (let i = 1; i < view.gridDivisions; ++i) {
                        ctx.beginPath()
                        ctx.moveTo(i * W / view.gridDivisions, 0)
                        ctx.lineTo(i * W / view.gridDivisions, H)
                        ctx.stroke()
                    }
                    const y = db => H * view.zeroAt - db / view.dbRange * H * view.dbSpan
                    const trace = (fc, res) => {
                        ctx.beginPath()
                        for (let i = 0; i <= W; i += view.sampleStep) {
                            const d = Math.max(view.dbFloor, Math.min(view.dbCeiling,
                                                                      Contract.filterDb(view.model, Contract.displayHz(i / W), fc, res)))
                            if (i === 0) ctx.moveTo(i, y(d)); else ctx.lineTo(i, y(d))
                        }
                    }
                    // The saved setting, faint, when modulation has moved it.
                    if (view.panel.live && Math.abs(view.cutoff - view.savedCutoff) > 1) {
                        trace(view.savedCutoff, view.savedResonance)
                        ctx.strokeStyle = t.soft(view.accent, view.ghostAlpha)
                        ctx.lineWidth = t.hairline
                        ctx.stroke()
                    }
                    trace(view.cutoff, view.resonance)
                    // The glow is a wide faint stroke under the line: a software shadow
                    // blur costs a hundred times more to paint.
                    ctx.strokeStyle = t.soft(view.accent, t.glowAlpha)
                    ctx.lineWidth = t.traceGlowWidth
                    ctx.stroke()
                    ctx.strokeStyle = view.accent
                    ctx.lineWidth = t.strokeBold
                    ctx.stroke()
                    ctx.lineTo(W, H)
                    ctx.lineTo(0, H)
                    ctx.closePath()
                    ctx.fillStyle = t.soft(view.accent, view.fillAlpha)
                    ctx.fill()
                    // The cutoff marker: violet while modulation moves it.
                    const cx = Contract.displayFraction(view.cutoff) * W
                    ctx.fillStyle = view.panel.live ? t.vi : view.accent
                    ctx.beginPath()
                    ctx.arc(cx, H - view.markerLift, view.markerRadius, 0, Math.PI * 2)
                    ctx.fill()
                }
            }
            MouseArea {
                objectName: "novaFilterDrag:" + view.filter
                readonly property string keyboardAlternative: "the card's CUTOFF and RESONANCE knobs"
                readonly property string cutoffId: view.base + ".cutoff"
                readonly property string resonanceId: view.base + ".resonance"
                property bool active: false
                anchors.fill: parent
                enabled: view.controller.editable
                preventStealing: true
                cursorShape: Qt.SizeAllCursor
                function apply(mouse) {
                    const fx = Math.max(0, Math.min(1, mouse.x / width))
                    const fy = Math.max(0, Math.min(1, 1 - mouse.y / height))
                    view.controller.set_value(cutoffId, Contract.displayHz(fx))
                    view.controller.set_normalised(resonanceId, fy)
                }
                onPressed: mouse => {
                    active = true
                    view.controller.begin_gesture(cutoffId)
                    view.controller.begin_gesture(resonanceId)
                    view.panel.touchId(cutoffId, "CUTOFF")
                    apply(mouse)
                }
                onPositionChanged: mouse => {
                    if (pressed && active)
                        apply(mouse)
                }
                onReleased: {
                    if (!active)
                        return
                    active = false
                    view.controller.end_gesture(cutoffId)
                    view.controller.end_gesture(resonanceId)
                }
                onCanceled: {
                    if (!active)
                        return
                    active = false
                    view.controller.cancel_gesture(cutoffId)
                    view.controller.cancel_gesture(resonanceId)
                }
                onDoubleClicked: {
                    view.controller.reset_to_default(cutoffId)
                    view.controller.reset_to_default(resonanceId)
                    view.panel.touchId(cutoffId, "CUTOFF")
                }
            }
        }
    }
    Connections {
        target: view
        function onCutoffChanged() { canvas.requestPaint() }
        function onResonanceChanged() { canvas.requestPaint() }
        function onModelChanged() { canvas.requestPaint() }
        function onWidthChanged() { canvas.requestPaint() }
        function onHeightChanged() { canvas.requestPaint() }
    }
}
