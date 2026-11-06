pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// A Nova knob (NOVA-UI-05): the kit Knob with Nova's face (NovaTheme
// paintKnob: a value arc in the module's accent and one violet outer arc per
// route into it spanning the route's depth) and a live violet dot per route
// at the modulated value (from telemetry). Drag to change the value (Shift
// fine), double-click or Ctrl+click to reset; drag on the ring, or Alt-drag,
// to change the newest route's depth. Click the value to type one (Enter
// sets it, Esc cancels). Hover names the parameter in full and lists the
// routes; right-click opens the route menu. A modulation source dropped on
// it (NovaHandle) creates a route at 30 %. The knob is as wide as its
// caption and widest value, so knobs side by side never overlap.
Kit.Knob {
    id: knob
    required property var panel
    // "sm", "" (normal) or "big".
    property string size: ""
    // A panel-side formatter for the knob position (FX slots use the module's).
    property var formatter: null
    property bool showValue: true
    // Show the value only while the knob is hovered or touched (for dense
    // rows); the knob then reserves no room for it.
    property bool valueOnHover: false

    readonly property var doc: panel.doc
    readonly property bool modulatable: info.automatable === true && info.discrete !== true
    readonly property real dial: diameter
    readonly property real baseWidth: size === "big" ? theme.knobBaseWidthBig
        : size === "sm" ? theme.knobBaseWidthSmall : theme.knobBaseWidth
    readonly property bool small: size === "sm"
    readonly property real captionHeight: small ? theme.knobCaptionHeightSmall : theme.knobCaptionHeight
    // Counts (voices, octaves) read without a sign: stepped values from 0 up.
    readonly property bool count: info.scale === "stepped" && info.minimum >= 0
    readonly property string fullName: (info.group !== undefined && info.group !== "" ? info.group + " · " : "")
        + (info.name !== undefined ? info.name : label)
    readonly property bool valueShown: showValue && (!valueOnHover || touched || hovered || editing)
    // The widest the value reads, at either end of the range or the default.
    readonly property real widestValue: {
        if (!showValue || valueOnHover)
            return 0
        const ends = formatter ? [formatter(0), formatter(1), formatter(info.default_normalised || 0)]
            : [controller.display_at(parameterId, 0), controller.display_at(parameterId, 1),
               controller.display_at(parameterId, info.default_normalised || 0)]
        let widest = 0
        for (let i = 0; i < ends.length; ++i)
            widest = Math.max(widest, valueMetrics.advanceWidth(plain(ends[i])))
        return widest
    }
    readonly property var routes: doc.routes_revision >= 0 ? doc.routes_to(parameterId) : []
    readonly property var amounts: {
        const list = []
        if (controller.revision >= 0)
            for (let i = 0; i < routes.length; ++i)
                list.push(controller.value(routes[i].amount_id))
        return list
    }
    readonly property bool dropHover: panel.dropKnob === knob

    controller: panel.controller
    theme: panel.theme
    diameter: size === "big" ? theme.knobDiameterBig : size === "sm" ? theme.knobDiameterSmall : theme.knobDiameter
    label: info.name !== undefined ? info.name.toUpperCase() : ""
    longName: fullName
    // The value arc's colour: its module's accent (NovaPanel.accentFor).
    accent: panel.accentFor(parameterId)
    valueText: controller.revision >= 0
        ? (formatter ? formatter(position) : plain(controller.display(parameterId))) : ""
    touched: panel.touchedId === parameterId || pressed
    typable: formatter === null && showValue
    valueItem: valueReadout
    fieldHeight: theme.knobValueHeight
    menuOwned: true
    menuFocuses: false
    tipAllowed: panel.dragSource === ""
    tipDetail: formatter === null
        ? "Click the value to type one · double-click or Ctrl+click resets"
        : "Double-click or Ctrl+click resets"
    implicitWidth: Math.ceil(Math.max(baseWidth, caption.implicitWidth + theme.knobTextPad,
                                      widestValue + theme.knobTextPad))
    implicitHeight: dial + theme.knobCaptionGap + captionHeight
        + (showValue && !valueOnHover ? theme.knobValueHeight : 0)
    Accessible.name: label
    Accessible.description: valueText + (routes.length > 0 ? ", modulated by " + routes.length + " routes" : "")

    Component.onCompleted: panel.registerKnob(knob)
    Component.onDestruction: panel.unregisterKnob(knob)

    function plain(text) {
        return count && text.charAt(0) === "+" ? text.substring(1) : text
    }
    function angle(at) {
        return (theme.knobStartDegrees + theme.knobSweepDegrees * Math.max(0, Math.min(1, at))) * Math.PI / 180
    }

    onUsed: panel.touch(knob)
    onMenuRequested: panel.openKnobMenu(knob)
    onHoveredChanged: {
        if (hovered)
            panel.hoverKnob = knob
        else if (panel.hoverKnob === knob)
            panel.hoverKnob = null
    }
    // A drag that ended elsewhere leaves the hover unchanged, so any movement
    // over the knob claims it again.
    onHoverMoved: {
        if (hovered && panel.hoverKnob !== knob)
            panel.hoverKnob = knob
    }
    // A press on the ring (or with Alt) drags the newest route's depth.
    onDragStarting: mouse => {
        const dx = mouse.x - face.width / 2
        const dy = mouse.y - face.height / 2
        const onRing = routes.length > 0
            && (Math.sqrt(dx * dx + dy * dy) > face.width / 2 - theme.knobRingGrab
                || (mouse.modifiers & Qt.AltModifier))
        if (!onRing)
            return
        const route = routes[routes.length - 1]
        gestureId = route.amount_id
        dragOrigin = controller.value(gestureId)
        dragDepth = true
        panel.touchRoute(route, knob)
    }

    Connections {
        target: knob
        function onAmountsChanged() { knob.repaint() }
        function onDropHoverChanged() { knob.repaint() }
    }

    // A violet halo while a modulation source hovers over the knob.
    Rectangle {
        visible: knob.dropHover
        x: knob.face.x - knob.theme.knobHaloOutset
        y: knob.face.y - knob.theme.knobHaloOutset
        width: knob.face.width + 2 * knob.theme.knobHaloOutset
        height: width
        radius: width / 2
        color: "transparent"
        border.color: knob.theme.vi
        border.width: knob.theme.hairline
        opacity: knob.theme.knobHaloOpacity
    }

    // Live dots: each route's current modulated value, at telemetry rate.
    Repeater {
        model: knob.panel.live ? knob.routes : []
        delegate: Rectangle {
            id: dot
            required property var modelData
            readonly property real contribution: Number(knob.panel.frame[Contract.T.routes + modelData.slot]) || 0
            readonly property real at: knob.angle(knob.position + contribution)
            readonly property real ring: knob.dial / 2 - knob.theme.knobRouteInset
            objectName: "novaModDot:" + knob.parameterId + ":" + modelData.slot
            width: knob.theme.knobRouteDot
            height: knob.theme.knobRouteDot
            radius: width / 2
            color: knob.theme.vi
            x: knob.face.x + knob.dial / 2 + Math.cos(at) * ring - width / 2
            y: knob.face.y + knob.dial / 2 + Math.sin(at) * ring - height / 2
        }
    }

    Text {
        id: caption
        objectName: "novaKnobCaption:" + knob.parameterId
        y: knob.face.height + knob.theme.knobCaptionGap
        width: knob.width
        horizontalAlignment: Text.AlignHCenter
        text: knob.label
        color: knob.theme.dim
        font.family: knob.theme.sans
        font.pixelSize: knob.small ? knob.theme.knobCaptionSizeSmall : knob.theme.knobCaptionSize
        font.weight: Font.Bold
        font.letterSpacing: knob.small ? knob.theme.knobCaptionSpacingSmall : knob.theme.knobCaptionSpacing
        elide: Text.ElideRight
    }
    FontMetrics {
        id: valueMetrics
        font: valueReadout.font
    }
    Text {
        id: valueReadout
        objectName: "novaKnobValue:" + knob.parameterId
        visible: knob.valueShown && !knob.editing
        y: caption.y + knob.captionHeight
        // A value shown on hover may spill past the knob, as may one under a
        // knob set narrower than it asks for; otherwise the knob fits it.
        width: knob.valueOnHover ? Math.max(knob.width, implicitWidth) : Math.max(knob.width, knob.implicitWidth)
        x: (knob.width - width) / 2
        horizontalAlignment: Text.AlignHCenter
        text: knob.valueText
        color: knob.touched ? knob.accent : knob.theme.faint
        font.family: knob.theme.mono
        font.pixelSize: knob.theme.knobValueSize
        elide: Text.ElideRight
        MouseArea {
            objectName: "novaKnobType:" + knob.parameterId
            anchors.fill: parent
            enabled: knob.formatter === null && knob.controller.editable
            cursorShape: Qt.IBeamCursor
            onClicked: knob.beginEdit()
        }
    }
}
