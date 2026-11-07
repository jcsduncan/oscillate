pragma ComponentBehavior: Bound
import QtQuick
import "devicekit" as Kit
import "shell"

// The body of a device-chain strip for a built-in engine: a live mini-view
// and four key knobs on the engine's own controller, so every knob move is a
// gesture with undo exactly as in the full panel. The controller is held per
// path and slot ("__source__" for the track's instrument) and re-acquired only
// when the slot's identity changes, so value edits never rebuild the strip.
Item {
    id: body
    objectName: "deviceStripBody"
    required property var bridge
    required property string pathId
    required property string slotId
    required property string engineId
    required property color accent
    property color well: Theme.canvas
    property color ink: Theme.textMain
    property color inkDim: Theme.textDim
    property bool active: true
    property bool playing: false
    property var controller: null
    property string heldPath: ""
    property string heldSlot: ""
    property var response: []
    property var liveFrame: []
    readonly property string identity: pathId + "|" + slotId + "|" + engineId
    // The strip's sound, for its card header ("Concert Hall", "* " once edited).
    readonly property string presetName: controller && controller.revision >= 0
        ? (controller.preset_modified ? "* " : "") + controller.preset_name : ""
    readonly property var info: engineId !== "" ? bridge.device_strip_info(engineId)
        : ({kind: "", accent: "", parameters: []})
    // The mini-view's size: as tall as the strip allows, leaving room for the knobs.
    readonly property int vizMinHeight: 40
    readonly property int vizMaxHeight: 92
    readonly property int knobRoom: 78
    readonly property real vizRadius: 7
    readonly property int unavailableGap: 14
    // Telemetry: a live frame has more points than this; it is polled at this rate.
    readonly property int liveFrameMinPoints: 8
    readonly property int liveFrameMs: 66
    readonly property int responsePoints: 48

    function release() {
        if (heldSlot !== "")
            bridge.release_device_strip(heldPath, heldSlot)
        heldPath = ""
        heldSlot = ""
        controller = null
    }
    function acquire() {
        release()
        if (pathId === "" || slotId === "" || engineId === "")
            return
        controller = bridge.device_strip_controller(pathId, slotId)
        if (controller) {
            heldPath = pathId
            heldSlot = slotId
        }
        refreshStatic()
    }
    function refreshStatic() {
        response = controller && controller.has_filter_response
            ? controller.filter_response(responsePoints) : []
        viz.requestPaint()
    }
    onIdentityChanged: acquire()
    Component.onCompleted: acquire()
    Component.onDestruction: release()

    Connections {
        target: body.bridge
        function onRevisionChanged() {
            // Values may have changed from elsewhere (undo, automation edits);
            // a knob mid-drag already holds the newest values.
            if (body.controller && body.controller.gesture_parameter === "") {
                body.controller.refresh()
                body.refreshStatic()
            }
        }
    }

    Timer {
        interval: body.liveFrameMs
        repeat: true
        running: body.controller !== null && body.playing && body.visible && body.active
        onTriggered: {
            const telemetry = body.controller.telemetry()
            // Only an engine's output spectrum is drawn live; its raw frame is a
            // packed record (peaks, flags, band data, several spectra).
            body.liveFrame = telemetry.spectrum ? telemetry.spectrum : []
            viz.requestPaint()
        }
        onRunningChanged: if (!running) { body.liveFrame = []; viz.requestPaint() }
    }

    Kit.KitTheme {
        id: stripTheme
        prefix: "strip"
        accent: body.accent
        knobAccent: body.accent
        knobDiameter: Theme.stripKnobDiameter
        knobMinWidth: Theme.stripKnobMinWidth
        knobWidthPad: Theme.stripKnobWidthPad
        knobLabelSize: Theme.typeCaption
        knobValueSize: Theme.typeSmall
        // Label above an always-shown value.
        knobStacked: true
        knobStackedValueSize: Theme.typeBody
        knobValueIdleInk: body.ink
        ink: body.ink
        inkDim: body.inkDim
        well: body.well
    }

    // The mini-view: the output spectrum while playing (engines that have one),
    // the filter response when the engine has one, else a quiet baseline.
    Rectangle {
        id: vizWell
        objectName: "deviceStripViz"
        x: 0; y: 0
        width: parent.width
        height: Math.max(body.vizMinHeight, Math.min(body.vizMaxHeight, parent.height - body.knobRoom))
        radius: body.vizRadius
        color: body.well
        Canvas {
            id: viz
            anchors.fill: parent
            anchors.margins: Theme.spaceXs
            // A baseline when there is nothing to draw; else the curve in a fixed
            // dB window: dBFS for the live spectrum (as the full panels draw it),
            // gain for the response.
            readonly property real baselineRatio: 0.62
            readonly property int maxPoints: 256
            readonly property real responseFloorDb: -30
            readonly property real responseCeilingDb: 12
            readonly property real spectrumFloorDb: -90
            readonly property real spectrumCeilingDb: 0
            readonly property real edgeInset: 2
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const w = width
                const h = height
                const values = body.liveFrame.length > body.liveFrameMinPoints ? body.liveFrame
                    : body.response.length > 1 ? body.response : []
                if (values.length < 2) {
                    ctx.strokeStyle = Qt.rgba(body.accent.r, body.accent.g, body.accent.b, Theme.stripBaselineAlpha)
                    ctx.lineWidth = Theme.hairline
                    ctx.beginPath()
                    ctx.moveTo(0, h * baselineRatio)
                    ctx.lineTo(w, h * baselineRatio)
                    ctx.stroke()
                    return
                }
                const count = Math.min(values.length, maxPoints)
                const live = body.liveFrame.length > body.liveFrameMinPoints
                const low = live ? spectrumFloorDb : responseFloorDb
                const high = live ? spectrumCeilingDb : responseCeilingDb
                const span = high - low
                const point = i => {
                    const v = Number(values[i])
                    const y = isFinite(v) ? (v - low) / span : 0
                    return [i / (count - 1) * w, h - edgeInset - Math.max(0, Math.min(1, y)) * (h - 2 * edgeInset)]
                }
                ctx.beginPath()
                ctx.moveTo(0, h)
                for (let i = 0; i < count; ++i) {
                    const p = point(i)
                    ctx.lineTo(p[0], p[1])
                }
                ctx.lineTo(w, h)
                ctx.closePath()
                ctx.fillStyle = Qt.rgba(body.accent.r, body.accent.g, body.accent.b, Theme.stripFillAlpha)
                ctx.fill()
                ctx.beginPath()
                for (let i = 0; i < count; ++i) {
                    const p = point(i)
                    if (i === 0)
                        ctx.moveTo(p[0], p[1])
                    else
                        ctx.lineTo(p[0], p[1])
                }
                ctx.strokeStyle = body.accent
                ctx.lineWidth = Theme.stripCurveWidth
                ctx.stroke()
            }
        }
        Text {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spaceSm
            text: body.liveFrame.length > body.liveFrameMinPoints ? "LIVE" : ""
            color: body.accent
            font.pixelSize: Theme.typeMicro
            font.bold: true
            font.letterSpacing: Theme.trackingMd
        }
    }

    Row {
        objectName: "deviceStripKnobs"
        anchors.horizontalCenter: parent.horizontalCenter
        y: vizWell.height + Theme.spaceSm
        spacing: Theme.spaceXxs
        Repeater {
            model: body.controller ? body.info.parameters : []
            delegate: Kit.Knob {
                required property string modelData
                controller: body.controller
                theme: stripTheme
                parameterId: modelData
                diameter: Theme.stripKnobDiameter
                opacity: body.active ? 1 : Theme.disabledOpacity
            }
        }
    }

    Text {
        visible: body.engineId !== "" && body.controller === null
        y: vizWell.height + body.unavailableGap
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: "Controls unavailable"
        color: body.inkDim
        font.pixelSize: Theme.typeSmall
    }
}
