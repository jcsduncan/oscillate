pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// ADSR picture with three handles: the peak sets attack, the knee sets decay
// (sideways) and sustain (up/down), the tail sets release. The sustain hold
// is dashed.
Kit.EnvelopeView {
    id: view
    // prefix: "prism.amp" or "prism.env2".
    readonly property real attack: revision >= 0 ? controller.normalised(prefix + ".attack") : 0
    readonly property real decay: revision >= 0 ? controller.normalised(prefix + ".decay") : 0
    readonly property real sustain: revision >= 0 ? controller.value(prefix + ".sustain") : 0
    readonly property real release: revision >= 0 ? controller.normalised(prefix + ".release") : 0
    readonly property real curve: revision >= 0 ? controller.value(prefix + ".curve") : 0

    // A stage's full travel and the sustain hold, as shares of the plot's
    // width; a stage at zero still spans shortestStage pixels.
    readonly property real stageShare: 0.27
    readonly property real holdShare: 0.16
    readonly property real shortestStage: 2
    // Samples drawn along each curved stage.
    readonly property int stageSteps: 24
    // Below this curve a stage is a straight line; above it the engine's
    // RC constant falls by this many decades across the curve's range.
    readonly property real straightCurve: 0.001
    readonly property real curveDecades: 4

    readonly property real plotLeft: theme.envelopePlotSide
    readonly property real plotWidth: width - 2 * theme.envelopePlotSide
    readonly property real segment: plotWidth * stageShare
    readonly property real plotTop: theme.envelopePlotTop
    readonly property real plotBottom: height - theme.envelopePlotBottom
    readonly property real attackX: plotLeft + Math.max(shortestStage, attack * segment)
    readonly property real decayX: attackX + Math.max(shortestStage, decay * segment)
    readonly property real sustainY: plotBottom - sustain * (plotBottom - plotTop)
    readonly property real releaseStart: decayX + plotWidth * holdShare
    readonly property real releaseX: releaseStart + Math.max(shortestStage, release * segment)

    objectName: "prismEnvelopeView:" + prefix
    points: trace()
    markers: [{ x: across(decayX), from: 0, to: sustain }, { x: across(releaseStart), from: 0, to: sustain }]

    // RC-style segment shape, the same overshoot curve the engine uses.
    function shape(t) {
        if (curve < straightCurve)
            return t
        const r = Math.pow(10, 1 - curveDecades * curve)
        return (1 + r) * (1 - Math.pow(r / (1 + r), t))
    }
    // A view x as a share of the plot's width.
    function across(x) {
        return (x - plotLeft) / plotWidth
    }
    function trace() {
        const points = [[0, 0]]
        for (let i = 1; i <= stageSteps; ++i) {
            const t = i / stageSteps
            points.push([across(plotLeft + (attackX - plotLeft) * t), shape(t)])
        }
        for (let i = 1; i <= stageSteps; ++i) {
            const t = i / stageSteps
            points.push([across(attackX + (decayX - attackX) * t), 1 - shape(t) * (1 - sustain)])
        }
        points.push([across(releaseStart), sustain])
        for (let i = 1; i <= stageSteps; ++i) {
            const t = i / stageSteps
            points.push([across(releaseStart + (releaseX - releaseStart) * t), sustain * (1 - shape(t))])
        }
        return points
    }

    component Handle: Rectangle {
        id: handle
        required property string parameter
        required property real centreX
        required property real centreY
        property bool vertical: false
        property real originX: 0
        property real originY: 0
        property real startA: 0
        property real startB: 0
        x: centreX - width / 2
        y: centreY - height / 2
        width: 2 * view.theme.envelopeKnotRadius
        height: 2 * view.theme.envelopeKnotRadius
        radius: view.theme.envelopeKnotRadius
        color: handleArea.pressed ? view.accent : view.theme.envelopeKnotFill
        border.color: view.accent
        border.width: view.theme.envelopeKnotLineWidth
        MouseArea {
            id: handleArea
            anchors.fill: parent
            anchors.margins: -view.theme.envelopeKnotGrab
            enabled: view.controller.editable
            hoverEnabled: true
            preventStealing: true
            cursorShape: handle.vertical ? Qt.SizeAllCursor : Qt.SizeHorCursor
            onPressed: mouse => {
                const p = mapToItem(view, mouse.x, mouse.y)
                handle.originX = p.x
                handle.originY = p.y
                handle.startA = view.controller.normalised(handle.parameter)
                handle.startB = view.sustain
                view.controller.begin_gesture(handle.parameter)
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const p = mapToItem(view, mouse.x, mouse.y)
                view.controller.set_normalised(handle.parameter,
                    Math.max(0, Math.min(1, handle.startA + (p.x - handle.originX) / view.segment)))
                if (handle.vertical)
                    view.controller.set_value(view.prefix + ".sustain", Math.max(0, Math.min(1,
                        handle.startB - (p.y - handle.originY) / (view.plotBottom - view.plotTop))))
            }
            onReleased: view.controller.end_gesture(handle.parameter)
            onCanceled: view.controller.cancel_gesture(handle.parameter)
            onDoubleClicked: {
                view.controller.reset_to_default(handle.parameter)
                if (handle.vertical)
                    view.controller.reset_to_default(view.prefix + ".sustain")
            }
        }
    }

    Handle {
        objectName: "prismEnvelopeAttack:" + view.prefix
        parameter: view.prefix + ".attack"
        centreX: view.attackX
        centreY: view.plotTop
    }
    Handle {
        objectName: "prismEnvelopeDecay:" + view.prefix
        parameter: view.prefix + ".decay"
        centreX: view.decayX
        centreY: view.sustainY
        vertical: true
    }
    Handle {
        objectName: "prismEnvelopeRelease:" + view.prefix
        parameter: view.prefix + ".release"
        centreX: view.releaseX
        centreY: view.plotBottom
    }
}
