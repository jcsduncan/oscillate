pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: envelope
    required property var bridge
    required property var clipData
    required property color accentColor
    required property color lineColor
    readonly property var points: (clipData.channelAutomation || {}).points || []
    readonly property string laneId: (clipData.channelAutomation || {}).laneId || ""
    readonly property var axisMap: (clipData.channelAutomation || {}).axisMap || []
    readonly property string timebase: (clipData.channelAutomation || {}).timebase || "Musical"
    // The level scale's top: gain 4 (+12 dB) at the curve's top edge.
    readonly property real maxLevel: 4
    // Amplitude decibels: 20 per decade of gain.
    readonly property real decibelsPerDecade: 20
    // The point last pressed: its exact level is typed in the tools row.
    property string selectedPoint: ""
    function decibels(level) {
        return level > 0 ? decibelsPerDecade * Math.log10(level) : -Infinity
    }
    // A typed level in dB ("-6", "−6 dB", "-inf") as a gain, or -1.
    function levelFromText(typed) {
        const cleaned = String(typed).trim().replace("−", "-").replace(/\s*dB$/i, "")
        if (/^-?(inf|∞)$/i.test(cleaned))
            return 0
        const db = Number(cleaned)
        if (cleaned.length === 0 || !Number.isFinite(db))
            return -1
        const level = Math.pow(10, db / decibelsPerDecade)
        return level <= maxLevel ? level : -1
    }

    // Match the shared automation evaluator's normalized segment equations.
    // The projected x coordinate is a clip fraction, so a lane whose timebase
    // differs from its clip's placement may need a nonlinear x projection.
    function cubic(t, first, second) {
        const rest = 1 - t
        return 3 * rest * rest * t * first + 3 * rest * t * t * second + t * t * t
    }
    function segmentProgress(shapeIndex, u, handles) {
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
                if (cubic(middle, Number(handles.x1), Number(handles.x2)) < u)
                    low = middle
                else
                    high = middle
            }
            return cubic((low + high) * 0.5, Number(handles.y1), Number(handles.y2))
        }
        }
        return 0
    }

    function boundedFraction(x, width) {
        return Math.max(0, Math.min(0.999999, x / Math.max(1, width)))
    }
    function levelAt(y, height) {
        return Math.max(0, Math.min(maxLevel, maxLevel * (1 - y / Math.max(1, height))))
    }
    function projectedFraction(laneFraction) {
        const samples = axisMap
        if (samples.length < 2)
            return laneFraction
        const last = samples.length - 1
        const scaled = laneFraction * last
        const index = Math.max(0, Math.min(last - 1, Math.floor(scaled)))
        const weight = scaled - index
        return Number(samples[index]) * (1 - weight) + Number(samples[index + 1]) * weight
    }

    Canvas {
        id: curve
        objectName: "clipLevelCurve"
        x: Theme.spaceSm; y: Theme.clipLevelTop; width: Math.max(1, envelope.width - Theme.spaceLg)
        height: Math.max(1, envelope.height - Theme.clipLevelBottomGap)
        property var data: envelope.points
        property var savedRevision: envelope.bridge.revision
        onDataChanged: requestPaint()
        onSavedRevisionChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            const ordered = data.filter(point => Number.isFinite(point.clipFraction))
                                .sort((left, right) => left.clipFraction - right.clipFraction)
            if (ordered.length === 0)
                return
            context.strokeStyle = envelope.accentColor
            context.lineWidth = Theme.strokeBold
            context.beginPath()
            const yAt = value => (1 - value / envelope.maxLevel) * height
            context.moveTo(0, yAt(ordered[0].value))
            context.lineTo(ordered[0].clipFraction * width, yAt(ordered[0].value))
            for (let index = 0; index + 1 < ordered.length; ++index) {
                const left = ordered[index]
                const right = ordered[index + 1]
                const start = left.clipFraction * width
                const span = (right.clipFraction - left.clipFraction) * width
                if (span <= 0) continue
                // Each point carries its segment's shape.
                const saved = left
                const shape = Number(saved.shapeIndex ?? 0)
                if (shape === 0) {
                    context.lineTo(start + span, yAt(left.value))
                } else {
                    const steps = Math.max(2, Math.min(64, Math.ceil(Math.min(span, width) / Theme.clipLevelCurveStep)))
                    for (let step = 1; step <= steps; ++step) {
                        const u = step / steps
                        const value = Number(left.value) +
                                      (Number(right.value) - Number(left.value)) *
                                      envelope.segmentProgress(shape, u, saved)
                        const xFraction = envelope.axisMap.length > 1 &&
                                          Number.isFinite(left.laneFraction) &&
                                          Number.isFinite(right.laneFraction)
                                        ? envelope.projectedFraction(
                                              Number(left.laneFraction) +
                                              (Number(right.laneFraction) -
                                               Number(left.laneFraction)) * u)
                                        : left.clipFraction +
                                          (right.clipFraction - left.clipFraction) * u
                        context.lineTo(xFraction * width, yAt(value))
                    }
                }
                context.lineTo(start + span, yAt(right.value))
            }
            context.lineTo(width, yAt(ordered[ordered.length - 1].value))
            context.stroke()
        }
    }

    MouseArea {
        id: surface
        objectName: "clipLevelAddSurface"
        x: curve.x; y: curve.y; width: curve.width; height: curve.height
        enabled: envelope.bridge.audioEditEnabled
        cursorShape: Qt.CrossCursor
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: "Add clip level point"
        function addAt(fraction, level) {
            envelope.bridge.clipAutomation.set_point_at_fraction(
                envelope.clipData.id, fraction, level)
        }
        Keys.onReturnPressed: addAt(0.5, 1)
        Keys.onSpacePressed: addAt(0.5, 1)
        onClicked: mouse => addAt(envelope.boundedFraction(mouse.x, width),
                                  envelope.levelAt(mouse.y, height))
    }

    // Keyed by count, so a refresh during a drag keeps the dragged handle.
    Repeater {
        model: envelope.points.length
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property var modelData: envelope.points[index] || ({})
            objectName: "clipLevelPointHandle"
            property bool dragging: false
            property real dragFraction: 0
            property real dragLevel: 0
            readonly property real previewFraction: dragging ? dragFraction : modelData.clipFraction
            readonly property real previewLevel: dragging ? dragLevel : modelData.value
            visible: previewFraction >= 0 && previewFraction < 1
            x: curve.x + previewFraction * curve.width - width / 2
            y: curve.y + (1 - previewLevel / envelope.maxLevel) * curve.height - height / 2
            width: Theme.clipLevelHandleSize; height: Theme.clipLevelHandleSize; radius: Theme.clipLevelHandleRadius
            color: envelope.accentColor
            border.color: envelope.lineColor
            z: 2
            activeFocusOnTab: envelope.bridge.audioEditEnabled
            Accessible.role: Accessible.Slider
            Accessible.name: "Clip level at " + modelData.position
            Accessible.description: "Level " + Number(previewLevel).toFixed(2)
            Keys.onDeletePressed: envelope.bridge.clipAutomation.remove_point(
                envelope.clipData.id, modelData.id)
            Keys.onLeftPressed: envelope.bridge.clipAutomation.move_point(
                envelope.clipData.id, modelData.id,
                envelope.boundedFraction(modelData.clipFraction - 0.01, 1), modelData.value)
            Keys.onRightPressed: envelope.bridge.clipAutomation.move_point(
                envelope.clipData.id, modelData.id,
                envelope.boundedFraction(modelData.clipFraction + 0.01, 1), modelData.value)
            Keys.onUpPressed: envelope.bridge.clipAutomation.set_value(
                envelope.clipData.id, modelData.id, Math.min(envelope.maxLevel, modelData.value + 0.05))
            Keys.onDownPressed: envelope.bridge.clipAutomation.set_value(
                envelope.clipData.id, modelData.id, Math.max(0, modelData.value - 0.05))
            MouseArea {
                anchors.fill: parent
                anchors.margins: -Theme.clipLevelHandleHitOutset
                cursorShape: Qt.SizeAllCursor
                enabled: envelope.bridge.audioEditEnabled
                onPressed: {
                    envelope.selectedPoint = handle.modelData.id
                    handle.forceActiveFocus()
                    handle.dragFraction = handle.modelData.clipFraction
                    handle.dragLevel = handle.modelData.value
                }
                onPositionChanged: mouse => {
                    if (!pressed) return
                    const position = mapToItem(curve, mouse.x, mouse.y)
                    handle.dragFraction = envelope.boundedFraction(position.x, curve.width)
                    handle.dragLevel = envelope.levelAt(position.y, curve.height)
                    handle.dragging = true
                }
                onReleased: {
                    const moved = handle.dragging &&
                        (Math.abs(handle.dragFraction - handle.modelData.clipFraction) > 0.000001 ||
                         Math.abs(handle.dragLevel - handle.modelData.value) > 0.000001)
                    handle.dragging = false
                    if (moved)
                        envelope.bridge.clipAutomation.move_point(
                            envelope.clipData.id, handle.modelData.id,
                            handle.dragFraction, handle.dragLevel)
                }
                onCanceled: handle.dragging = false
            }
        }
    }

    // The envelope's tools, along its foot: the selected point's exact
    // level (typed in dB), the timebase and Clear.
    Row {
        id: tools
        objectName: "clipLevelTools"
        visible: envelope.laneId.length > 0
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceSm
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceXxs
        z: 3
        spacing: Theme.spaceXs
        Rectangle {
            id: levelField
            objectName: "clipLevelValueField"
            property bool refused: false
            readonly property var point: envelope.points.find(
                candidate => candidate.id === envelope.selectedPoint) || null
            visible: point !== null
            width: Theme.automationValueFieldWidth
            height: Theme.automationDrawHeight
            radius: Theme.radiusMd
            color: Theme.canvas
            border.color: refused ? Theme.automationValueRefused
                                  : levelInput.activeFocus ? Theme.focusRing : Theme.controlEdge
            TextInput {
                id: levelInput
                objectName: "clipLevelValueInput"
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceSm
                anchors.rightMargin: Theme.spaceSm
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                text: levelField.point
                      ? (Number.isFinite(envelope.decibels(levelField.point.value))
                         ? envelope.decibels(levelField.point.value).toFixed(1) + " dB" : "-inf dB")
                      : ""
                color: Theme.chipText
                font.pixelSize: Theme.typeSmall
                selectByMouse: true
                activeFocusOnTab: true
                Accessible.role: Accessible.EditableText
                Accessible.name: "Selected point's level in decibels"
                onTextEdited: levelField.refused = false
                function commit() {
                    const level = envelope.levelFromText(text)
                    if (level < 0) {
                        levelField.refused = true
                        return
                    }
                    envelope.bridge.clipAutomation.set_value(envelope.clipData.id,
                                                             envelope.selectedPoint, level)
                }
                Keys.onReturnPressed: commit()
                Keys.onEnterPressed: commit()
            }
        }
        Repeater {
            model: [["timebase", "Timebase: " + envelope.timebase,
                     "Switch to " + (envelope.timebase === "Musical" ? "Elapsed" : "Musical")
                     + " timebase: points keep their times"],
                    ["clear", "Clear envelope", "Remove this clip's level envelope"]]
            delegate: Rectangle {
                id: choice
                required property var modelData
                objectName: choice.modelData[0] === "clear" ? "clipLevelClear" : "clipLevelTimebase"
                width: choiceText.implicitWidth + Theme.spaceLg
                height: Theme.automationDrawHeight
                radius: Theme.radiusMd
                color: choiceArea.containsMouse ? Theme.automationChoiceHover : Theme.panelRaised
                border.color: Theme.controlEdge
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: choice.modelData[2]
                function activate() {
                    if (choice.modelData[0] === "clear") {
                        envelope.selectedPoint = ""
                        envelope.bridge.clipAutomation.remove_lane(envelope.clipData.id)
                    } else {
                        envelope.bridge.arrangementAutomation.toggle_timebase(envelope.laneId)
                    }
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text {
                    id: choiceText
                    anchors.centerIn: parent
                    text: choice.modelData[1]
                    color: Theme.chipText
                    font.pixelSize: Theme.typeSmall
                }
                MouseArea {
                    Tip {}
                    id: choiceArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: choice.activate()
                }
            }
        }
    }
}
