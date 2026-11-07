pragma ComponentBehavior: Bound
import QtQuick
import "../shell" as Shell
import "MacroPalette.js" as MacroPalette

// A parameter knob. Drag vertically (Shift for fine), scroll, use the arrow
// keys (Shift for fine), or double-click / Delete to return to the default.
// Every drag is one gesture, so one undo step. The face is the theme's
// paintKnob; the captions follow the theme's knob tokens.
//
// A device whose knobs read differently (Crate, Nova, the mixer) derives a
// thin wrapper: its theme turns the kit's captions off (knobCaptions) and
// the wrapper draws its own, and it may open the typing field (beginEdit),
// take the right button (menuOwned, menuRequested), redirect a drag
// (dragStarting) or follow each use of the knob (used).
Item {
    id: knob
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property int diameter: theme.knobDiameter
    property string label: info.name !== undefined ? info.name : ""
    // Spoken and tooltip name when the label alone is ambiguous.
    property string longName: theme.knobLongName === "label" ? label
        : (info.group !== undefined && (theme.knobLongName === "group" || info.group !== label)
           ? info.group + " " : "") + label
    // Dimmed but still editable, for a control the current setting ignores.
    property bool inactive: false
    property bool active: true
    // Hero knobs show their value under the name at all times.
    property bool readout: false
    // False while there is no parameter to drive (a wrapper's idle knob): the
    // knob then sits at the bottom of its range and ignores input.
    property bool bound: true
    readonly property var info: parameterId !== "" ? controller.parameter(parameterId) : ({})
    // The saved (or live) value as a knob position, and the position the
    // face shows: it eases to a new value (a preset, a macro, undo) but
    // follows the pointer exactly while dragging.
    readonly property real target: bound && controller.revision >= 0 ? controller.normalised(parameterId) : 0
    property real position: target
    Behavior on position {
        enabled: !knob.dragging && knob.theme.knobEases && Shell.Motion.enabled
        NumberAnimation { duration: Shell.Motion.fast; easing.type: Easing.OutCubic }
    }
    // A panel may format the value itself (pan as L/C/R, say).
    property string valueText: controller.revision >= 0 ? controller.display(parameterId) : ""
    readonly property bool bipolar: info.bipolar === true
    readonly property bool nextNote: info.next_note === true
    // Where the value arc starts, as a knob position.
    property real origin: bipolar ? 0.5 : 0
    property color accent: theme.knobAccent
    property color accent2: theme.accent2
    property color arc: accent
    property color labelInk: theme.knobLabelInk
    property color valueInk: theme.knobValueInk !== undefined ? theme.knobValueInk : accent
    readonly property bool editable: bound && controller.editable
    readonly property bool pressed: area.pressed
    readonly property bool hovered: area.containsMouse
    // Lit captions: pressed, hovered or focused (a wrapper may widen it).
    property bool touched: pressed || hovered || activeFocus
    readonly property bool valueSwapped: touched && !readout
    property real dragOrigin: 0
    property real dragStartY: 0
    // What the current drag moves: a parameter's knob position, or (with
    // dragDepth) a modulation depth from -1 to 1 as a value. A dragStarting
    // handler may point both elsewhere.
    property string gestureId: ""
    property bool dragDepth: false
    // Macros (ADR-0059): the track macro driving this knob (-1: none), and
    // the macro MAP mode is mapping (-1: off). Controllers without macros
    // (previews, tests) leave both off.
    readonly property bool macroAware: theme.knobMacros && typeof controller.macro_of === "function"
    readonly property int macroIndex: macroAware && controller.macro_revision >= 0
        ? controller.macro_of(parameterId) : -1
    readonly property int mapArmed: macroAware ? controller.macro_map_index : -1
    readonly property color macroColor: macroIndex >= 0 ? MacroPalette.color(macroIndex)
        : mapArmed >= 0 ? MacroPalette.color(mapArmed) : "transparent"
    property bool dragging: false
    // The right button opens the macro menu unless the wrapper owns the menu
    // (it then hears menuRequested); menuFocuses: that press focuses the knob.
    property bool menuOwned: false
    property bool menuFocuses: true
    // Typing a value: beginEdit opens a field over valueItem (the wrapper's
    // value readout), fieldHeight tall, while typable allows it.
    property bool typable: true
    property bool editing: false
    property Text valueItem: valueLine
    property real fieldHeight: valueItem.height
    // The tooltip: shown on hover while allowed.
    property bool tipAllowed: true
    property string tipText: longName + "  " + valueText
    property string tipDetail: (macroIndex >= 0 ? "Macro " + (macroIndex + 1) + " drives this\n" : "")
        + (mapArmed >= 0 ? "Click to map to Macro " + (mapArmed + 1)
           : "Drag · Shift fine · double-click resets · right-click maps to a macro")
        + (theme.knobNextNoteMark && nextNote ? "\nApplies to new notes" : "")
    // The dial, for a wrapper's overlays.
    readonly property Item face: dialCanvas

    // The knob was used: a drag began on its own parameter, a step, a reset
    // or a typed value.
    signal used()
    // A drag is about to begin: a handler may set gestureId, dragOrigin and
    // dragDepth to drive something else.
    signal dragStarting(var mouse)
    // The pointer moved over the dial with no button down.
    signal hoverMoved()
    // The right button on a knob whose wrapper owns the menu.
    signal menuRequested()

    implicitWidth: Math.max(diameter + theme.knobWidthPad, theme.knobMinWidth)
    implicitHeight: diameter + theme.knobHeightPad + (readout ? theme.knobReadoutPad : 0)
    objectName: theme.prefix + "Knob:" + parameterId
    activeFocusOnTab: editable
    opacity: !editable ? theme.knobDisabledOpacity
        : inactive || !active ? theme.knobInactiveOpacity : 1
    Accessible.role: Accessible.Dial
    Accessible.name: theme.knobLongName === "none" ? label : longName
    Accessible.description: valueText + (theme.knobNextNoteMark && nextNote ? ". Applies to new notes" : "")
        + (macroIndex >= 0 ? ". Driven by Macro " + (macroIndex + 1) : "")
        + ". Drag, scroll or use arrow keys; Delete resets; the context menu maps it to a macro"

    function touch() {
        theme.lastTouched = parameterId
        used()
    }
    function repaint() {
        dialCanvas.requestPaint()
    }

    Keys.onPressed: event => {
        if (!editable)
            return
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) {
            controller.step(parameterId, 1, fine)
            touch()
            event.accepted = true
        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) {
            controller.step(parameterId, -1, fine)
            touch()
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || (event.key === Qt.Key_Backspace && theme.resetOnBackspace)) {
            controller.reset_to_default(parameterId)
            touch()
            event.accepted = true
        } else if (event.key === Qt.Key_Menu && macroAware) {
            openMacroMenu()
            event.accepted = true
        } else if (event.key === Qt.Key_Escape && mapArmed >= 0) {
            controller.end_macro_map()
            event.accepted = true
        }
    }

    function openMacroMenu() {
        macroMenu.active = true
        macroMenu.item.openAt(knob)
    }

    // A typed value in the units the knob shows: "%" values are percent, "k"
    // means kHz, a time in seconds typed as one in ms, a choice by name, and
    // the theme's extra forms (L/R pan, "inf" dB, ms for seconds); anything
    // else as written. Null when unreadable.
    function parseValue(text) {
        const typed = text.trim().toLowerCase().replace("−", "-")
        const choices = info.choices !== undefined ? info.choices : []
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].toLowerCase() === typed)
                return info.minimum + i
        const unit = info.unit !== undefined ? info.unit : ""
        if (theme.knobTypeInfinity && unit === "dB" && typed.indexOf("inf") >= 0)
            return info.minimum
        if (theme.knobTypePan && unit === "%" && info.bipolar === true
                && (typed.charAt(0) === "l" || typed.charAt(0) === "r" || typed === "c")) {
            const amount = typed === "c" ? 0 : parseFloat(typed.substring(1))
            if (!isFinite(amount))
                return null
            return Math.max(info.minimum, Math.min(info.maximum, (typed.charAt(0) === "l" ? -amount : amount) / 100))
        }
        const match = typed.match(/^([-+]?[0-9]*\.?[0-9]+)\s*(.*)$/)
        if (match === null)
            return null
        let value = parseFloat(match[1])
        const suffix = match[2].trim()
        if (unit === "%")
            value /= 100
        else if (unit === "Hz" && suffix.charAt(0) === "k")
            value *= 1000
        else if (theme.knobTypeSeconds && unit === "s"
                 && (suffix === "ms" || (suffix === "" && valueText.indexOf("ms") >= 0)))
            value /= 1000
        else if (unit === "ms" && suffix === "s")
            value *= 1000
        if (!isFinite(value))
            return null
        return Math.max(info.minimum, Math.min(info.maximum, value))
    }
    function beginEdit() {
        if (!editable || !typable)
            return
        editing = true
    }
    // Sets the typed value as one undo step; an unreadable entry changes nothing.
    function commitEdit() {
        if (!editing)
            return
        const typed = fieldLoader.item.typed
        editing = false
        const value = parseValue(typed)
        if (value === null)
            return
        controller.begin_gesture(parameterId)
        controller.set_value(parameterId, value)
        controller.end_gesture(parameterId)
        touch()
    }
    function cancelEdit() {
        editing = false
    }

    Tip {
        objectName: knob.theme.prefix + "KnobTip:" + knob.parameterId
        theme: knob.theme
        shown: knob.theme.knobTip && knob.tipAllowed && area.containsMouse && !area.pressed && !knob.editing
        text: knob.tipText
        detail: knob.tipDetail
    }

    Canvas {
        id: dialCanvas
        x: (parent.width - width) / 2
        y: knob.theme.knobDialY
        width: knob.diameter + knob.theme.knobDialPad
        height: knob.diameter + knob.theme.knobDialPad
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            knob.theme.paintKnob(ctx, knob, width)
        }
        Connections {
            target: knob
            function onPositionChanged() { dialCanvas.requestPaint() }
            function onActiveFocusChanged() { dialCanvas.requestPaint() }
            function onParameterIdChanged() { dialCanvas.requestPaint() }
            function onBoundChanged() { dialCanvas.requestPaint() }
            function onArcChanged() { dialCanvas.requestPaint() }
            function onAccentChanged() { dialCanvas.requestPaint() }
            function onAccent2Changed() { dialCanvas.requestPaint() }
            function onOriginChanged() { dialCanvas.requestPaint() }
        }
    }

    // A knob a macro drives wears the macro's colour; in MAP mode every
    // knob that can be mapped is outlined in the armed macro's.
    Rectangle {
        id: macroRing
        readonly property bool armedOther: knob.mapArmed >= 0 && knob.macroIndex !== knob.mapArmed
        visible: knob.macroIndex >= 0 || knob.mapArmed >= 0
        x: dialCanvas.x - knob.theme.knobMacroRingOutset; y: dialCanvas.y - knob.theme.knobMacroRingOutset
        width: dialCanvas.width + 2 * knob.theme.knobMacroRingOutset
        height: dialCanvas.height + 2 * knob.theme.knobMacroRingOutset
        radius: width / 2
        color: armedOther
            ? Qt.rgba(knob.macroColor.r, knob.macroColor.g, knob.macroColor.b,
                      area.containsMouse ? knob.theme.knobMacroWashHoverAlpha : knob.theme.knobMacroWashAlpha)
            : "transparent"
        border.color: knob.macroColor
        border.width: knob.macroIndex >= 0 ? knob.theme.strokeBold + glow : knob.theme.hairline
        opacity: armedOther ? knob.theme.knobMacroArmedOpacity : knob.theme.knobMacroRingOpacity
        // Brightens while its macro turns the knob.
        property real glow: 0
        SequentialAnimation {
            id: pulse
            NumberAnimation {
                target: macroRing; property: "glow"; to: knob.theme.knobMacroGlow; duration: Shell.Motion.fast
            }
            NumberAnimation { target: macroRing; property: "glow"; to: 0; duration: Shell.Motion.normal }
        }
        Connections {
            target: knob
            function onTargetChanged() {
                if (knob.macroIndex >= 0 && !knob.dragging && Shell.Motion.enabled)
                    pulse.restart()
            }
        }
    }
    Rectangle {
        objectName: knob.theme.prefix + "KnobMacro:" + knob.parameterId
        visible: knob.macroIndex >= 0
        x: dialCanvas.x + dialCanvas.width - width + knob.theme.knobMacroBadgeOffset
        y: dialCanvas.y - knob.theme.knobMacroBadgeOffset
        width: knob.theme.knobMacroBadgeSize; height: knob.theme.knobMacroBadgeSize; radius: width / 2
        color: knob.macroColor
        Text {
            anchors.centerIn: parent
            text: knob.macroIndex + 1
            color: knob.theme.knobMacroBadgeInk
            font.pixelSize: knob.theme.knobMacroBadgeTextSize
            font.bold: true
        }
    }

    MouseArea {
        id: area
        anchors.fill: dialCanvas
        enabled: knob.editable || knob.theme.knobLockedHover
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: knob.theme.knobRightClick ? Qt.LeftButton | Qt.RightButton : Qt.LeftButton
        cursorShape: knob.mapArmed >= 0 ? Qt.PointingHandCursor : knob.theme.knobCursor
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton) {
                if (knob.menuFocuses)
                    knob.forceActiveFocus()
                if (knob.menuOwned)
                    knob.menuRequested()
                else if (knob.macroAware)
                    knob.openMacroMenu()
                return
            }
            if (!knob.editable)
                return
            knob.forceActiveFocus()
            // MAP mode: a touch maps the knob to the armed macro.
            if (knob.mapArmed >= 0) {
                knob.controller.map_to_macro(knob.parameterId, knob.mapArmed)
                return
            }
            if (knob.theme.knobCtrlClickResets && (mouse.modifiers & Qt.ControlModifier)) {
                knob.controller.reset_to_default(knob.parameterId)
                knob.touch()
                return
            }
            knob.gestureId = knob.parameterId
            knob.dragDepth = false
            knob.dragOrigin = knob.target
            knob.dragStartY = mouse.y
            knob.dragStarting(mouse)
            if (knob.gestureId === knob.parameterId)
                knob.touch()
            knob.dragging = true
            knob.controller.begin_gesture(knob.gestureId)
        }
        onPositionChanged: mouse => {
            if (!pressed) {
                knob.hoverMoved()
                return
            }
            if (!knob.dragging)
                return
            const fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
            const travel = (knob.dragStartY - mouse.y) / (fine ? knob.theme.knobFinePixels : knob.theme.knobDragPixels)
            if (knob.dragDepth)
                knob.controller.set_value(knob.gestureId, Math.max(-1, Math.min(1, knob.dragOrigin + travel)))
            else
                knob.controller.set_normalised(knob.gestureId, Math.max(0, Math.min(1, knob.dragOrigin + travel)))
        }
        onReleased: {
            if (knob.dragging)
                knob.controller.end_gesture(knob.gestureId)
            knob.dragging = false
        }
        onCanceled: {
            if (knob.dragging)
                knob.controller.cancel_gesture(knob.gestureId)
            knob.dragging = false
        }
        onDoubleClicked: mouse => {
            if (mouse.button !== Qt.LeftButton || knob.mapArmed >= 0)
                return
            knob.controller.reset_to_default(knob.parameterId)
            knob.touch()
        }
        onWheel: wheel => {
            if (wheel.angleDelta.y !== 0 && knob.editable) {
                knob.controller.step(knob.parameterId, wheel.angleDelta.y > 0 ? 1 : -1,
                    (wheel.modifiers & Qt.ShiftModifier) !== 0)
                knob.touch()
            }
            wheel.accepted = true
        }
    }

    // The caption: the label, swapped for the value while touched; or, in the
    // stacked style, the label above an always-shown value.
    Text {
        id: caption
        objectName: knob.theme.knobCaptions ? knob.theme.prefix + "KnobLabel:" + knob.parameterId : ""
        readonly property bool value: knob.valueSwapped && !knob.theme.knobStacked
        visible: knob.theme.knobCaptions
        y: dialCanvas.y + dialCanvas.height + knob.theme.knobCaptionGap
        width: parent.width + 2 * knob.theme.knobCaptionInset
        x: -knob.theme.knobCaptionInset
        horizontalAlignment: Text.AlignHCenter
        text: value ? knob.valueText : knob.theme.knobLabelUpper ? knob.label.toUpperCase() : knob.label
        color: value ? knob.valueInk
            : knob.theme.knobStacked && (knob.activeFocus || area.pressed) ? knob.theme.knobLabelActiveInk
            : knob.labelInk
        font.family: value ? knob.theme.knobValueFont : knob.theme.knobLabelFont
        font.pixelSize: value ? knob.theme.knobValueSize
            : knob.readout ? knob.theme.knobReadoutLabelSize : knob.theme.knobLabelSize
        font.bold: value ? knob.theme.knobValueBold : knob.theme.knobLabelBold
        font.letterSpacing: value ? 0 : knob.theme.knobLabelSpacing
        elide: Text.ElideRight
        clip: knob.theme.knobCaptionClip
    }
    // The value under the label: always in the stacked style, and on hero
    // knobs with a readout.
    Text {
        id: valueLine
        objectName: knob.theme.knobCaptions ? knob.theme.prefix + "KnobValue:" + knob.parameterId : ""
        visible: knob.theme.knobCaptions && (knob.readout || knob.theme.knobStacked)
        y: knob.theme.knobStacked ? caption.y + knob.theme.knobStackedValueGap
                                  : caption.y + caption.height + knob.theme.knobReadoutGap
        width: parent.width + 2 * knob.theme.knobCaptionInset
        x: -knob.theme.knobCaptionInset
        horizontalAlignment: Text.AlignHCenter
        text: knob.valueText
        color: knob.theme.knobStacked ? (knob.touched ? knob.valueInk : knob.theme.knobValueIdleInk)
                                      : knob.touched ? knob.theme.ink : knob.valueInk
        font.family: knob.theme.knobValueFont
        font.pixelSize: knob.theme.knobStacked ? knob.theme.knobStackedValueSize : knob.theme.knobReadoutValueSize
        elide: Text.ElideRight
        clip: knob.theme.knobCaptionClip
    }

    // Parameters that apply to new notes carry a small hollow mark.
    Rectangle {
        visible: knob.theme.knobNextNoteMark && knob.nextNote
        x: dialCanvas.x + dialCanvas.width - knob.theme.knobNextNoteInset
        y: dialCanvas.y + knob.theme.knobNextNoteTop
        width: knob.theme.knobNextNoteSize
        height: knob.theme.knobNextNoteSize
        radius: knob.theme.knobNextNoteRound ? width / 2 : 0
        rotation: knob.theme.knobNextNoteRound ? 0 : knob.theme.knobNextNoteTurn
        color: "transparent"
        border.color: knob.theme.knobNextNoteInk
        border.width: knob.theme.hairline
    }

    // The typing field over the value readout: Enter sets the value, Esc
    // or leaving it cancels.
    Loader {
        id: fieldLoader
        z: 1
        active: knob.editing
        sourceComponent: Rectangle {
            readonly property string typed: field.text
            x: Math.min(0, (knob.width - width) / 2)
            y: knob.valueItem.y - knob.theme.hairline
            width: Math.max(knob.width, field.contentWidth + knob.theme.knobFieldPad)
            height: knob.fieldHeight + 2 * knob.theme.hairline
            radius: knob.theme.radiusSm
            color: knob.theme.well
            border.color: knob.accent
            TextInput {
                id: field
                objectName: knob.theme.prefix + "KnobField:" + knob.parameterId
                anchors.fill: parent
                anchors.leftMargin: knob.theme.knobFieldMargin
                anchors.rightMargin: knob.theme.knobFieldMargin
                horizontalAlignment: TextInput.AlignHCenter
                verticalAlignment: TextInput.AlignVCenter
                color: knob.theme.ink
                selectionColor: knob.theme.accentSoft
                font: knob.valueItem.font
                selectByMouse: true
                Component.onCompleted: {
                    text = knob.valueText
                    selectAll()
                    forceActiveFocus()
                }
                onAccepted: knob.commitEdit()
                Keys.onEscapePressed: knob.cancelEdit()
                onActiveFocusChanged: if (!activeFocus && knob.editing) knob.cancelEdit()
            }
        }
    }

    // Map to Macro 1-8, or remove from its macro: made on first use, shown
    // over the whole window so no panel clips it.
    Loader {
        id: macroMenu
        active: false
        sourceComponent: MacroMenu {
            controller: knob.controller
            theme: knob.theme
            parameterId: knob.parameterId
            label: knob.longName
            onClosed: macroMenu.active = false
        }
    }
}
