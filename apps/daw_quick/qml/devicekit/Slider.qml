pragma ComponentBehavior: Bound
import QtQuick

// A labelled horizontal slider. Bound to a parameter (`controller` and
// `parameterId`) it edits the parameter: click or drag along it (Shift drags
// finely from where it is), scroll, use the arrow keys (Shift for fine), or
// double-click / Delete for the default; every drag is one gesture, so one
// undo step. Bound to a plain value (`from`, `to`, `step`, `value`) it
// reports `started` on press, `moved` while dragging and `released` when let
// go, and the left and right arrow keys step it as a press and release
// each; with `commitOnRelease` it reports only `moved`, once, when let go.
// The theme's slider tokens lay it out: stacked (the name and value on a
// caption line over the rail, an optional hint under it) or in one row
// (name, rail, value).
Item {
    id: slider
    required property KitTheme theme
    // A parameter: unset (null) for a plain value. An empty parameterId
    // leaves the slider inert.
    property var controller: null
    property string parameterId: ""
    readonly property bool valueMode: controller === null
    readonly property bool bound: !valueMode && parameterId !== ""
    readonly property var info: bound ? controller.parameter(parameterId) : ({})
    property string label: info.name !== undefined ? info.name : ""
    property string hint: ""
    property string tip: ""
    property color accent: theme.accent
    // A panel may word a value itself ("off"): format(value, info) returns
    // its text, or "" for the engine's own.
    property var format: null
    // A plain value and the text shown for it.
    property real from: 0
    property real to: 1
    property real step: 0
    property real value: 0
    property string readout: ""
    // Report the value once, when the drag ends, rather than while it moves.
    property bool commitOnRelease: false
    // The value while a commit-on-release drag moves, before it is reported.
    property real live: value
    readonly property bool pressed: area.pressed
    signal started()
    signal moved(real value)
    signal released(real value)

    readonly property real shown: commitOnRelease ? live : value
    readonly property real position: {
        if (valueMode)
            return to > from ? Math.max(0, Math.min(1, (shown - from) / (to - from))) : 0
        return bound && controller.revision >= 0 ? controller.normalised(parameterId) : 0
    }
    readonly property real parameterValue: bound && controller.revision >= 0 ? controller.value(parameterId) : 0
    readonly property string valueText: {
        if (valueMode)
            return readout
        if (!bound || controller.revision < 0)
            return ""
        const own = format ? format(parameterValue, info) : ""
        return own !== "" ? own : controller.display(parameterId)
    }
    // Where the fill starts: the middle of a bipolar range.
    readonly property real origin: info.bipolar === true ? 0.5 : 0
    readonly property bool editable: valueMode || (bound && controller.editable)
    readonly property bool stacked: theme.sliderStacked
    property bool dragging: false
    property bool fine: false
    property real dragOrigin: 0
    property real dragStartX: 0

    onValueChanged: if (!area.pressed) live = value

    objectName: valueMode ? "" : theme.prefix + "Slider:" + parameterId
    implicitWidth: theme.sliderWidth
    implicitHeight: stacked
        ? captionHeight + theme.sliderRailGap + theme.sliderRailHeight + (hint !== "" ? hintText.implicitHeight : 0)
        : theme.sliderHeight
    activeFocusOnTab: editable
    opacity: editable ? 1 : theme.sliderDisabledOpacity
    Accessible.role: Accessible.Slider
    Accessible.name: valueMode ? (theme.tipIsName && tip.length > 0 ? tip : label)
        : (info.group !== undefined ? info.group + " " : "") + label
    Accessible.description: valueMode ? (theme.tipIsName ? "" : tip)
        : valueText + ". Drag, scroll or use arrow keys; Delete resets"

    // The caption line's height in stacked form: the token, or the text's own.
    readonly property real captionHeight: theme.sliderCaptionHeight > 0 ? theme.sliderCaptionHeight
                                                                       : labelText.implicitHeight

    function touch() {
        theme.lastTouched = parameterId
    }
    function snapped(v) {
        const clamped = Math.max(from, Math.min(to, v))
        return step > 0 ? from + Math.round((clamped - from) / step) * step : clamped
    }
    // A plain value set from the pointer or the keys.
    function setShown(v) {
        if (commitOnRelease) {
            live = v
        } else {
            value = v
            moved(v)
        }
    }
    function nudge(direction) {
        const amount = step > 0 ? step : (to - from) / theme.sliderNudgeSteps
        if (commitOnRelease) {
            live = snapped(live + direction * amount)
            moved(live)
        } else {
            started()
            value = snapped(value + direction * amount)
            moved(value)
            released(value)
        }
    }

    Keys.onPressed: event => {
        if (!editable)
            return
        if (valueMode) {
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                nudge(event.key === Qt.Key_Right ? 1 : -1)
                event.accepted = true
            }
            return
        }
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) {
            controller.step(parameterId, 1, fine)
            touch()
            event.accepted = true
        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) {
            controller.step(parameterId, -1, fine)
            touch()
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            controller.reset_to_default(parameterId)
            touch()
            event.accepted = true
        }
    }

    Text {
        id: labelText
        width: !slider.stacked ? slider.theme.sliderLabelWidth
            : slider.theme.sliderValueInline ? implicitWidth
            : slider.width - valueLabel.implicitWidth - slider.theme.sliderCaptionGap
        height: slider.stacked ? slider.captionHeight : implicitHeight
        anchors.verticalCenter: slider.stacked ? undefined : parent.verticalCenter
        verticalAlignment: Text.AlignVCenter
        text: slider.theme.sliderLabelUpper ? slider.label.toUpperCase() : slider.label
        color: slider.activeFocus
            ? (slider.theme.sliderLabelFocusInk !== undefined ? slider.theme.sliderLabelFocusInk : slider.accent)
            : slider.theme.sliderLabelInk
        font.family: slider.theme.sliderLabelFont
        font.pixelSize: slider.theme.sliderLabelSize
        font.weight: slider.theme.sliderLabelWeight
        font.letterSpacing: slider.theme.sliderLabelSpacing
        elide: Text.ElideRight
    }
    Text {
        id: valueLabel
        objectName: slider.valueMode ? "" : slider.theme.prefix + "SliderValue:" + slider.parameterId
        x: !slider.stacked ? slider.width - width
            : slider.theme.sliderValueInline ? labelText.implicitWidth + slider.theme.sliderCaptionGap
            : slider.width - width
        width: slider.stacked ? implicitWidth : slider.theme.sliderValueWidth
        height: slider.stacked ? slider.captionHeight : implicitHeight
        anchors.verticalCenter: slider.stacked ? undefined : parent.verticalCenter
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: slider.stacked ? Text.AlignLeft : Text.AlignRight
        text: slider.valueText
        color: slider.dragging
            ? (slider.theme.sliderValueDragInk !== undefined ? slider.theme.sliderValueDragInk : slider.accent)
            : slider.theme.sliderValueInk
        font.family: slider.theme.sliderValueFont
        font.pixelSize: slider.theme.sliderValueSize
        font.weight: slider.theme.sliderValueWeight
        elide: slider.theme.sliderValueElide ? Text.ElideRight : Text.ElideNone
    }

    Item {
        id: rail
        x: slider.stacked ? 0 : slider.theme.sliderLabelWidth + slider.theme.sliderGap
        y: slider.stacked ? slider.captionHeight + slider.theme.sliderRailGap : 0
        width: slider.stacked ? slider.width
            : slider.width - x - slider.theme.sliderValueWidth - slider.theme.sliderGap
        height: slider.stacked ? slider.theme.sliderRailHeight : slider.height

        Rectangle {
            id: groove
            x: slider.theme.sliderGrooveInset ? handle.width / 2 : 0
            y: (parent.height - height) / 2
            width: parent.width - 2 * x
            height: slider.theme.sliderTrack
            radius: height / 2
            color: slider.theme.sliderGroove
            border.color: slider.activeFocus ? slider.theme.sliderGrooveFocusEdge : slider.theme.sliderGrooveEdge
        }
        Rectangle {
            readonly property real from: Math.min(slider.origin, slider.position)
            readonly property real to: Math.max(slider.origin, slider.position)
            x: groove.x + from * groove.width
            y: groove.y
            width: Math.max(0, (to - from) * groove.width)
            height: groove.height
            radius: height / 2
            color: slider.accent
        }
        Rectangle {
            id: handle
            readonly property bool active: slider.activeFocus || area.pressed
            x: slider.theme.sliderHandleInside ? groove.x + slider.position * (groove.width - width)
                                               : groove.x + slider.position * groove.width - width / 2
            y: (parent.height - height) / 2
            width: slider.theme.sliderHandle
            height: width
            radius: width / 2
            color: slider.theme.sliderHandleFill
            border.width: active ? slider.theme.sliderHandleActiveEdgeWidth : slider.theme.sliderHandleEdgeWidth
            border.color: {
                const edge = active ? slider.theme.sliderHandleActiveEdge : slider.theme.sliderHandleEdge
                return edge !== undefined ? edge : slider.accent
            }
        }

        MouseArea {
            id: area
            // The groove, widened by half the handle each side unless the
            // handle stays inside it; with sliderTouchCaption it reaches up
            // over the caption too, as one touch target.
            readonly property real overhang: slider.theme.sliderHandleInside ? 0 : handle.width / 2
            x: groove.x - overhang
            y: slider.stacked && slider.theme.sliderTouchCaption ? -slider.captionHeight : 0
            width: groove.width + 2 * overhang
            height: parent.height - y
            enabled: slider.editable
            hoverEnabled: slider.theme.sliderHover
            preventStealing: true
            cursorShape: slider.theme.sliderResizeCursor ? Qt.SizeHorCursor : Qt.ArrowCursor
            function at(mouseX) {
                return Math.max(0, Math.min(1, (mouseX - overhang) / Math.max(1, groove.width)))
            }
            function valueAt(mouseX) {
                return slider.snapped(slider.from + (slider.to - slider.from) * at(mouseX))
            }
            onPressed: mouse => {
                slider.forceActiveFocus()
                if (slider.valueMode) {
                    slider.dragging = true
                    if (!slider.commitOnRelease)
                        slider.started()
                    slider.setShown(valueAt(mouse.x))
                    return
                }
                slider.touch()
                slider.fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
                slider.dragOrigin = slider.position
                slider.dragStartX = mouse.x
                slider.dragging = true
                slider.controller.begin_gesture(slider.parameterId)
                if (!slider.fine)
                    slider.controller.set_normalised(slider.parameterId, area.at(mouse.x))
            }
            onPositionChanged: mouse => {
                if (!pressed || !slider.dragging)
                    return
                if (slider.valueMode) {
                    slider.setShown(valueAt(mouse.x))
                    return
                }
                const next = slider.fine
                    ? slider.dragOrigin + (mouse.x - slider.dragStartX) / (groove.width * slider.theme.sliderFineDivisor)
                    : area.at(mouse.x)
                slider.controller.set_normalised(slider.parameterId, Math.max(0, Math.min(1, next)))
            }
            onReleased: {
                if (slider.valueMode) {
                    slider.dragging = false
                    if (slider.commitOnRelease)
                        slider.moved(slider.live)
                    else
                        slider.released(slider.value)
                    return
                }
                if (slider.dragging)
                    slider.controller.end_gesture(slider.parameterId)
                slider.dragging = false
            }
            onCanceled: {
                if (slider.dragging && !slider.valueMode)
                    slider.controller.cancel_gesture(slider.parameterId)
                slider.dragging = false
            }
            onDoubleClicked: {
                if (slider.valueMode)
                    return
                slider.controller.reset_to_default(slider.parameterId)
                slider.touch()
            }
            onWheel: wheel => {
                if (slider.valueMode) {
                    wheel.accepted = false
                    return
                }
                if (wheel.angleDelta.y !== 0) {
                    slider.controller.step(slider.parameterId, wheel.angleDelta.y > 0 ? 1 : -1,
                        (wheel.modifiers & Qt.ShiftModifier) !== 0)
                    slider.touch()
                }
                wheel.accepted = true
            }
            HoverTip {
                theme: slider.theme
                anchorItem: area
                text: !slider.valueMode ? slider.Accessible.name + "  " + slider.valueText
                    : slider.theme.tipIsName && slider.tip.length === 0 ? slider.label : slider.tip
            }
        }
    }

    Text {
        id: hintText
        y: rail.y + rail.height
        width: slider.width
        visible: slider.stacked && slider.hint !== ""
        text: slider.hint
        color: slider.theme.sliderHintInk
        font.family: slider.theme.sliderHintFont
        font.pixelSize: slider.theme.sliderHintSize
        wrapMode: Text.WordWrap
        maximumLineCount: slider.theme.sliderHintLines
        elide: Text.ElideRight
    }
}
