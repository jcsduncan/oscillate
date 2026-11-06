pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// One segment, identified by its owning lane and starting point. The session
// owns saved values; pointer edits are staged until release as one undo step.
Item {
    id: editor
    required property var bridge
    required property string laneId
    required property string pointId
    required property color panelColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    readonly property var curve: bridge.trackAutomation.segment_curve(laneId, pointId, bridge.revision)
    readonly property bool editEnabled: bridge.audioEditEnabled && Boolean(curve.present)
    readonly property int shapeIndex: curve.present ? Number(curve.shapeIndex) : 0
    readonly property bool bezier: shapeIndex === 5
    readonly property var shapeNames: ["HOLD", "LINEAR", "QUADRATIC", "EXPONENTIAL", "EASE IN/OUT", "BÉZIER"]
    property real previewX1: Number(curve.x1 ?? 0)
    property real previewY1: Number(curve.y1 ?? 0)
    property real previewX2: Number(curve.x2 ?? 0)
    property real previewY2: Number(curve.y2 ?? 0)
    height: bezier ? Theme.automationSegmentBezierHeight : Theme.automationSegmentHeight

    function resetPreview() {
        previewX1 = Number(curve.x1 ?? 0)
        previewY1 = Number(curve.y1 ?? 0)
        previewX2 = Number(curve.x2 ?? 0)
        previewY2 = Number(curve.y2 ?? 0)
    }
    onCurveChanged: resetPreview()

    function cubic(t, first, second) {
        const rest = 1 - t
        return 3 * rest * rest * t * first + 3 * rest * t * t * second + t * t * t
    }
    function progress(u) {
        switch (shapeIndex) {
        case 0: return 0
        case 1: return u
        case 2: return u * u
        case 3: return Math.expm1(4 * u) / Math.expm1(4)
        case 4: return u * u * (3 - 2 * u)
        case 5: {
            let low = 0
            let high = 1
            for (let iteration = 0; iteration < 24; ++iteration) {
                const middle = (low + high) * 0.5
                if (cubic(middle, previewX1, previewX2) < u)
                    low = middle
                else
                    high = middle
            }
            return cubic((low + high) * 0.5, previewY1, previewY2)
        }
        }
        return 0
    }
    function moveHandle(which, x, y) {
        if (!editEnabled || !bezier)
            return
        if (which === 1) {
            previewX1 = Math.max(0, Math.min(previewX2, x))
            previewY1 = Math.max(0, Math.min(1, y))
        } else {
            previewX2 = Math.max(previewX1, Math.min(1, x))
            previewY2 = Math.max(0, Math.min(1, y))
        }
    }
    function commitPreview() {
        if (!editEnabled || !bezier)
            return
        if (Math.abs(previewX1 - Number(curve.x1)) < 0.000001 &&
                Math.abs(previewY1 - Number(curve.y1)) < 0.000001 &&
                Math.abs(previewX2 - Number(curve.x2)) < 0.000001 &&
                Math.abs(previewY2 - Number(curve.y2)) < 0.000001)
            return
        bridge.trackAutomation.set_segment_curve(laneId, pointId,
            { shapeIndex: 5, x1: previewX1, y1: previewY1,
              x2: previewX2, y2: previewY2 })
    }
    function nudgeHandle(which, axis, amount) {
        const x = which === 1 ? previewX1 : previewX2
        const y = which === 1 ? previewY1 : previewY2
        moveHandle(which, axis === "x" ? x + amount : x,
                   axis === "y" ? y + amount : y)
        commitPreview()
    }

    function savedCurve(shapeIndex) {
        return { shapeIndex: shapeIndex, x1: Number(curve.x1), y1: Number(curve.y1),
                 x2: Number(curve.x2), y2: Number(curve.y2) }
    }
    function selectShape(delta) {
        if (!editEnabled || curve.discrete)
            return
        const count = shapeNames.length
        bridge.trackAutomation.set_segment_curve(laneId, pointId,
                                                  savedCurve((shapeIndex + delta + count) % count))
    }
    function applyHandles() {
        if (!editEnabled || !bezier || !x1Input.acceptableInput || !y1Input.acceptableInput ||
                !x2Input.acceptableInput || !y2Input.acceptableInput ||
                Number(x1Input.text) > Number(x2Input.text))
            return
        bridge.trackAutomation.set_segment_curve(laneId, pointId,
            { shapeIndex: 5, x1: Number(x1Input.text), y1: Number(y1Input.text),
              x2: Number(x2Input.text), y2: Number(y2Input.text) })
    }

    Rectangle {
        id: previousShape
        objectName: "automationShapePrevious"
        width: Theme.automationShapeButtonSize; height: Theme.automationShapeButtonSize
        radius: Theme.radiusMd; color: editor.panelColor; border.color: editor.lineColor
        enabled: editor.editEnabled && !editor.curve.discrete
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: "Previous automation segment shape"
        function activate() { editor.selectShape(-1) }
        Accessible.onPressAction: editor.selectShape(-1)
        Keys.onReturnPressed: editor.selectShape(-1)
        Keys.onSpacePressed: editor.selectShape(-1)
        Text { anchors.centerIn: parent; text: "‹"; color: previousShape.enabled ? editor.textColor : editor.dimColor }
        MouseArea {
            anchors.fill: parent
            enabled: previousShape.enabled
            onClicked: editor.selectShape(-1)
            Tip {}
        }
    }
    Text {
        x: Theme.automationShapeLabelX; y: Theme.spaceSm; width: editor.width - 2 * Theme.automationShapeLabelX
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: editor.curve.discrete ? "HOLD · DISCRETE" : editor.shapeNames[editor.shapeIndex]
        color: editor.accentColor
        font.pixelSize: Theme.typeCaption
        font.bold: true
        Accessible.role: Accessible.StaticText
        Accessible.name: "Segment shape " + text
    }
    Rectangle {
        id: nextShape
        objectName: "automationShapeNext"
        x: editor.width - width; width: Theme.automationShapeButtonSize; height: Theme.automationShapeButtonSize
        radius: Theme.radiusMd; color: editor.panelColor; border.color: editor.lineColor
        enabled: editor.editEnabled && !editor.curve.discrete
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: "Next automation segment shape"
        function activate() { editor.selectShape(1) }
        Accessible.onPressAction: editor.selectShape(1)
        Keys.onReturnPressed: editor.selectShape(1)
        Keys.onSpacePressed: editor.selectShape(1)
        Text { anchors.centerIn: parent; text: "›"; color: nextShape.enabled ? editor.textColor : editor.dimColor }
        MouseArea {
            anchors.fill: parent
            enabled: nextShape.enabled
            onClicked: editor.selectShape(1)
            Tip {}
        }
    }

    Canvas {
        id: preview
        objectName: "automationShapePreview"
        x: Theme.automationPreviewX; y: Theme.automationPreviewY; width: Math.max(1, editor.width - 2 * Theme.automationPreviewX); height: Theme.automationPreviewHeight
        property int currentShape: editor.shapeIndex
        // The curve drawn as previewSteps straight pieces, inside the frame.
        readonly property int previewSteps: 64
        readonly property real inset: Theme.hairline
        property real firstX: editor.previewX1
        property real firstY: editor.previewY1
        property real secondX: editor.previewX2
        property real secondY: editor.previewY2
        onCurrentShapeChanged: requestPaint()
        onFirstXChanged: requestPaint()
        onFirstYChanged: requestPaint()
        onSecondXChanged: requestPaint()
        onSecondYChanged: requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            context.strokeStyle = editor.lineColor
            context.lineWidth = Theme.hairline
            context.strokeRect(inset / 2, inset / 2, width - inset, height - inset)
            context.strokeStyle = editor.accentColor
            context.lineWidth = Theme.strokeBold
            context.beginPath()
            context.moveTo(inset, height - inset)
            for (let step = 1; step <= previewSteps; ++step) {
                const u = step / previewSteps
                context.lineTo(inset + u * (width - 2 * inset), (1 - editor.progress(u)) * (height - 2 * inset) + inset)
            }
            // A held value jumps only at the next point.
            if (editor.shapeIndex === 0)
                context.lineTo(width - inset, inset)
            context.stroke()
            if (editor.bezier) {
                context.strokeStyle = editor.dimColor
                context.beginPath()
                context.moveTo(inset, height - inset)
                context.lineTo(inset + editor.previewX1 * (width - 2 * inset),
                               inset + (1 - editor.previewY1) * (height - 2 * inset))
                context.moveTo(width - inset, inset)
                context.lineTo(inset + editor.previewX2 * (width - 2 * inset),
                               inset + (1 - editor.previewY2) * (height - 2 * inset))
                context.stroke()
            }
        }
    }
    Repeater {
        model: editor.bezier ? 2 : 0
        delegate: Rectangle {
            id: graphicalHandle
            required property int index
            readonly property int which: index + 1
            objectName: which === 1 ? "automationGraphHandleX1Y1" : "automationGraphHandleX2Y2"
            width: Theme.automationHandleSize; height: Theme.automationHandleSize; radius: width / 2
            x: preview.x + preview.inset + (which === 1 ? editor.previewX1 : editor.previewX2) *
               (preview.width - 2 * preview.inset) - width / 2
            y: preview.y + preview.inset + (1 - (which === 1 ? editor.previewY1 : editor.previewY2)) *
               (preview.height - 2 * preview.inset) - height / 2
            color: editor.accentColor
            border.color: editor.panelColor
            z: 2
            activeFocusOnTab: editor.editEnabled
            Accessible.role: Accessible.Slider
            Accessible.name: "Bézier handle " + which + ", horizontal and vertical position"
            Accessible.description: "X " + (which === 1 ? editor.previewX1 : editor.previewX2).toFixed(3) +
                                    ", Y " + (which === 1 ? editor.previewY1 : editor.previewY2).toFixed(3)
            Keys.onLeftPressed: editor.nudgeHandle(which, "x", -0.025)
            Keys.onRightPressed: editor.nudgeHandle(which, "x", 0.025)
            Keys.onUpPressed: editor.nudgeHandle(which, "y", 0.025)
            Keys.onDownPressed: editor.nudgeHandle(which, "y", -0.025)
            MouseArea {
                anchors.fill: parent
                anchors.margins: -Theme.automationHandleHitOutset
                enabled: editor.editEnabled
                cursorShape: Qt.SizeAllCursor
                onPressed: graphicalHandle.forceActiveFocus()
                onPositionChanged: mouse => {
                    if (!pressed) return
                    const point = mapToItem(preview, mouse.x, mouse.y)
                    editor.moveHandle(graphicalHandle.which,
                        (point.x - preview.inset) / Math.max(1, preview.width - 2 * preview.inset),
                        1 - (point.y - preview.inset) / Math.max(1, preview.height - 2 * preview.inset))
                }
                onReleased: editor.commitPreview()
                onCanceled: editor.resetPreview()
            }
        }
    }

    component HandleField: Rectangle {
        id: field
        required property string label
        required property string savedText
        property alias text: input.text
        readonly property bool acceptableInput: input.acceptableInput && input.text.length > 0
        width: Theme.automationFieldWidth; height: Theme.automationFieldHeight; radius: Theme.radiusMd
        color: editor.panelColor; border.color: input.activeFocus ? editor.accentColor : editor.lineColor
        Text { y: -Theme.automationFieldLabelRise; text: field.label; color: editor.dimColor; font.pixelSize: Theme.typeMicro }
        TextInput {
            id: input
            anchors.fill: parent; anchors.margins: Theme.spaceXs
            text: field.savedText
            color: editor.textColor
            font.pixelSize: Theme.typeCaption
            validator: DoubleValidator { bottom: 0; top: 1; decimals: 3; locale: "C" }
            enabled: editor.editEnabled && editor.bezier
            activeFocusOnTab: enabled
            Accessible.role: Accessible.EditableText
            Accessible.name: "Bézier " + field.label + " handle, 0 to 1"
        }
    }
    Row {
        y: Theme.automationFieldsY
        spacing: Theme.automationFieldSpacing
        visible: editor.bezier
        HandleField { id: x1Input; objectName: "automationHandleX1"; label: "X1"; savedText: Number(editor.curve.x1 ?? 0).toFixed(3) }
        HandleField { id: y1Input; objectName: "automationHandleY1"; label: "Y1"; savedText: Number(editor.curve.y1 ?? 0).toFixed(3) }
        HandleField { id: x2Input; objectName: "automationHandleX2"; label: "X2"; savedText: Number(editor.curve.x2 ?? 0).toFixed(3) }
        HandleField { id: y2Input; objectName: "automationHandleY2"; label: "Y2"; savedText: Number(editor.curve.y2 ?? 0).toFixed(3) }
        Rectangle {
            id: applyHandles
            objectName: "automationApplyHandles"
            width: Math.max(Theme.automationApplyMinWidth, editor.width - Theme.automationApplyReserve); height: Theme.automationFieldHeight
            radius: Theme.radiusMd; color: enabled ? Theme.automationApplyFill : editor.panelColor
            border.color: enabled ? editor.accentColor : editor.lineColor
            enabled: editor.editEnabled && x1Input.acceptableInput && y1Input.acceptableInput &&
                     x2Input.acceptableInput && y2Input.acceptableInput &&
                     Number(x1Input.text) <= Number(x2Input.text)
            activeFocusOnTab: enabled
            Accessible.role: Accessible.Button
            Accessible.name: "Apply Bézier segment handles"
            function activate() { editor.applyHandles() }
            Accessible.onPressAction: editor.applyHandles()
            Keys.onReturnPressed: editor.applyHandles()
            Keys.onSpacePressed: editor.applyHandles()
            Text { anchors.centerIn: parent; text: "APPLY"; color: applyHandles.enabled ? editor.accentColor : editor.dimColor; font.pixelSize: Theme.typeCaption }
            MouseArea {
                anchors.fill: parent
                enabled: applyHandles.enabled
                onClicked: editor.applyHandles()
                Tip {}
            }
        }
    }
}
