pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract
import "NovaFxDraw.js" as Draw

// A module strip's live display (NOVA-UI-11), painted by NovaFxDraw from
// the slot's values and its telemetry (contract §6). It repaints when a
// value changes, and with each telemetry frame while its strip is in view.
// The filter's node and the EQ's band nodes drag: across for frequency, up
// and down for gain (resonance on the filter; Q for a band without gain,
// or with Shift); the wheel sets Q; clicking a band node picks its type.
Rectangle {
    id: display
    required property var panel
    required property var owner
    readonly property var controller: panel.controller
    readonly property int uid: owner.uid
    readonly property var spec: owner.spec
    readonly property string visual: spec !== null ? spec.visual : ""
    readonly property bool eq: visual === "eq"
    readonly property bool filter: visual === "response"
    readonly property bool animate: panel.live && owner.inView && !owner.collapsed
    // How far one wheel notch moves Q, in Q's knob travel.
    readonly property real wheelStep: 0.02
    // The filter node's resonance travel across the display's height.
    readonly property real resonanceTop: 0.15
    readonly property real resonanceSpan: 0.7
    // A press on a band node becomes a drag once it moves this far.
    readonly property real dragThreshold: 3

    objectName: "novaFxDisplay:" + uid
    radius: panel.theme.fxDisplayRadius
    color: panel.theme.wellFill
    border.color: panel.theme.line
    clip: true

    function values() {
        const p = []
        for (let k = 0; k < Contract.fxSlotParams; ++k)
            p.push(owner.p(k))
        return p
    }
    // The painter's view of the slot now.
    function model() {
        const p = values()
        const frame = panel.frame
        return {
            visual: visual, module: owner.module, p: p, mix: owner.p("mix"),
            mode: owner.mode, synced: owner.synced, live: panel.live,
            inDb: Contract.slotDb(frame, Contract.T.slotIn, uid),
            outDb: Contract.slotDb(frame, Contract.T.slotOut, uid),
            value: panel.at(Contract.T.fx + uid - 1),
            detail: Contract.slotDetail(frame, uid),
            spectrum: Contract.slotSpectrum(frame, uid)
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: display.panel.theme.hairline
        renderStrategy: Canvas.Cooperative
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!display.owner.ready || display.visual === "")
                return
            ctx.globalAlpha = display.owner.on ? 1 : display.panel.theme.fxDisplayOffAlpha
            Draw.paint(ctx, display.panel.theme, width, height, display.model())
        }
    }
    Connections {
        target: display.controller
        function onValues_changed() { canvas.requestPaint() }
    }
    Connections {
        target: display.panel
        enabled: display.animate
        function onFrameChanged() { canvas.requestPaint() }
    }
    Connections {
        target: display.owner
        function onCollapsedChanged() { canvas.requestPaint() }
    }

    // ---- The filter's node ----
    Rectangle {
        id: filterNode
        objectName: "novaFxNode:" + display.uid
        readonly property real cutoff: display.filter ? Contract.expo(8, 22000)(display.owner.p(0)) : 0
        visible: display.filter
        x: Contract.displayFraction(cutoff) * display.width - width / 2
        y: (display.resonanceTop + display.resonanceSpan * (1 - display.owner.p(1))) * display.height - height / 2
        width: display.panel.theme.fxNodeSize
        height: width
        radius: width / 2
        color: filterArea.pressed ? display.panel.theme.am : display.panel.theme.pk
        border.color: display.panel.theme.wellFill
        border.width: display.panel.theme.strokeBold
        MouseArea {
            id: filterArea
            anchors.fill: parent
            anchors.margins: -display.panel.theme.fxNodeHit
            preventStealing: true
            enabled: display.controller.editable
            cursorShape: Qt.SizeAllCursor
            onPressed: display.controller.begin_gesture(Contract.fxParam(display.uid, 0))
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const at = mapToItem(display, mouse.x, mouse.y)
                const fx = Math.max(0, Math.min(1, at.x / display.width))
                const res = Math.max(0, Math.min(1, 1 - (at.y / display.height - display.resonanceTop) / display.resonanceSpan))
                const values = {}
                values[Contract.fxParam(display.uid, 0)] = Contract.expoP(Contract.displayHz(fx), 8, 22000)
                values[Contract.fxParam(display.uid, 1)] = res
                display.controller.set_values(values)
            }
            onReleased: display.controller.end_gesture(Contract.fxParam(display.uid, 0))
            onCanceled: display.controller.cancel_gesture(Contract.fxParam(display.uid, 0))
        }
    }

    // ---- The EQ's band nodes ----
    Repeater {
        model: display.eq ? Contract.eqBands : 0
        delegate: Rectangle {
            id: node
            required property int index
            readonly property int type: Contract.eqTypeIndex(display.owner.p(4 * index))
            readonly property var at: display.controller.revision >= 0 ? Draw.eqNode({ p: display.values() }, index) : ({ x: 0, y: 0 })
            readonly property string typeId: Contract.fxParam(display.uid, 4 * index)
            readonly property string freqId: Contract.fxParam(display.uid, 4 * index + 1)
            readonly property string gainId: Contract.fxParam(display.uid, 4 * index + 2)
            readonly property string qId: Contract.fxParam(display.uid, 4 * index + 3)
            objectName: "novaFxEqNode:" + display.uid + ":" + index
            visible: type !== Contract.eqType.off
            x: at.x * display.width - width / 2
            y: at.y * display.height - height / 2
            width: display.panel.theme.fxEqNodeSize
            height: width
            radius: width / 2
            color: nodeArea.pressed ? display.panel.theme.am : display.panel.theme.vi
            border.color: display.panel.theme.wellFill
            border.width: display.panel.theme.strokeBold
            Accessible.role: Accessible.Slider
            Accessible.name: "Band " + (index + 1) + " " + Contract.eqTypes[type]
            Text {
                anchors.centerIn: parent
                text: node.index + 1
                color: display.panel.theme.wellFill
                font.family: display.panel.theme.mono
                font.pixelSize: display.panel.theme.typeSmall
                font.weight: Font.Bold
            }
            MouseArea {
                id: nodeArea
                property point start
                property bool moved: false
                property real startQ: 0
                anchors.fill: parent
                anchors.margins: -display.panel.theme.fxNodeHit
                preventStealing: true
                enabled: display.controller.editable
                cursorShape: Qt.SizeAllCursor
                onPressed: mouse => {
                    start = mapToItem(display, mouse.x, mouse.y)
                    startQ = display.controller.value(node.qId)
                    moved = false
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const at = mapToItem(display, mouse.x, mouse.y)
                    if (!moved) {
                        if (Math.hypot(at.x - start.x, at.y - start.y) < display.dragThreshold)
                            return
                        moved = true
                        display.controller.begin_gesture(node.freqId)
                    }
                    const values = {}
                    values[node.freqId] = Contract.expoP(Contract.displayHz(Math.max(0, Math.min(1, at.x / display.width))),
                                                         Contract.eqMinHz, Contract.eqMaxHz)
                    const qDrag = (mouse.modifiers & Qt.ShiftModifier) !== 0 || !Contract.eqHasGain(node.type)
                    if (qDrag)
                        values[node.qId] = Math.max(0, Math.min(1, startQ + (start.y - at.y) / display.height))
                    else
                        values[node.gainId] = Math.max(0, Math.min(1, 1 - at.y / display.height))
                    display.controller.set_values(values)
                }
                onReleased: {
                    if (moved) {
                        display.controller.end_gesture(node.freqId)
                        return
                    }
                    display.panel.menu.open(node, Contract.eqTypes.map((c, i) => ({ label: c, value: i })), node.type,
                                            display.panel.theme.vi,
                                            value => display.controller.set_value(node.typeId, Contract.choiceP(value, Contract.eqTypes.length)))
                }
                onCanceled: if (moved) display.controller.cancel_gesture(node.freqId)
                onWheel: wheel => {
                    const step = wheel.angleDelta.y > 0 ? display.wheelStep : -display.wheelStep
                    display.controller.set_value(node.qId, Math.max(0, Math.min(1, display.controller.value(node.qId) + step)))
                }
            }
        }
    }

    Text {
        visible: !display.panel.live && (display.eq || display.filter || display.visual === "comb" || display.visual === "shift")
        anchors.right: parent.right
        anchors.rightMargin: display.panel.theme.displayCaptionX
        y: display.panel.theme.displayCaptionY
        text: "PLAY A NOTE TO SEE THE SPECTRUM"
        color: display.panel.theme.faint
        font.family: display.panel.theme.sans
        font.pixelSize: display.panel.theme.typeSmall
        font.letterSpacing: display.panel.theme.fxHintSpacing
    }
}
