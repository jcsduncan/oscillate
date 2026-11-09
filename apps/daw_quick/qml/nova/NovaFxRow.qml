pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// One module in the rack's chain (NOVA-UI-11): the whole row drags (to
// reorder it, or onto another bus's tab to move it there; Ctrl copies),
// a click selects its strip. LED, name, the preset and mode it plays,
// its input and output level, and ⋯ for the slot menu.
Rectangle {
    id: row
    required property var page
    required property var panel
    required property int index
    required property int uid
    required property string module
    readonly property var spec: Contract.module(module)
    readonly property var controller: panel.controller
    readonly property bool on: controller.revision >= 0 && controller.choice("nova.fx." + uid + ".on") === 1
    readonly property bool selected: page.selected === uid
    readonly property bool dragging: page.dragFrom === index
    readonly property string modeName: {
        if (spec === null || spec.mode === null || controller.revision < 0)
            return ""
        const choices = spec.mode.choices === "filterModels"
            ? controller.parameter("nova.filter.1.model").choices : spec.mode.choices
        return choices[Contract.modeIndex(controller.value(Contract.fxParam(uid, spec.mode.p)), choices.length)]
    }
    // The meter's span: dBFS at its left end.
    readonly property real meterFloorDb: -60
    function fraction(dbValue) {
        return Math.max(0, Math.min(1, (dbValue - meterFloorDb) / -meterFloorDb))
    }

    objectName: "novaFxRow:" + uid
    radius: panel.theme.fxRackTileRadius
    color: selected ? panel.theme.selectedFill : panel.theme.card
    border.color: activeFocus || selected ? panel.theme.pk : panel.theme.line
    border.width: activeFocus ? panel.theme.strokeBold : panel.theme.hairline
    opacity: dragging ? panel.theme.fxRowDragOpacity : on ? 1 : panel.theme.fxRowOffOpacity
    activeFocusOnTab: true
    Accessible.role: Accessible.ListItem
    Accessible.name: (spec !== null ? spec.name : module) + (on ? "" : ", off")
    Accessible.description: "Return selects its strip; Ctrl with Up or Down moves it in the chain"

    // Keys: Return or Space selects the strip; Ctrl+Up/Down moves the
    // module one place (one chain edit), as a drag would.
    function moveBy(step) {
        const to = index + step
        if (!controller.editable || to < 0 || to >= page.modules.length)
            return
        panel.doc.move_chain_module(panel.fxChain, index, to)
    }
    Keys.onReturnPressed: page.select(uid)
    Keys.onSpacePressed: page.select(uid)
    Keys.onUpPressed: event => {
        if (event.modifiers & Qt.ControlModifier)
            moveBy(-1)
        else
            event.accepted = false
    }
    Keys.onDownPressed: event => {
        if (event.modifiers & Qt.ControlModifier)
            moveBy(1)
        else
            event.accepted = false
    }

    // The selection edge.
    Rectangle {
        visible: row.selected
        x: row.panel.theme.hairline
        y: row.panel.theme.fxRowEdgeInset
        width: row.panel.theme.fxRowEdgeWidth
        height: parent.height - 2 * row.panel.theme.fxRowEdgeInset
        radius: width / 2
        color: row.panel.theme.pk
    }

    // Press, drag and release: the whole row is the handle.
    MouseArea {
        id: area
        property point start
        property bool moved: false
        anchors.fill: parent
        preventStealing: true
        enabled: row.controller.editable
        cursorShape: row.dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onPressed: mouse => {
            start = Qt.point(mouse.x, mouse.y)
            moved = false
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            if (!moved && Math.hypot(mouse.x - start.x, mouse.y - start.y) < row.page.dragThreshold)
                return
            if (!moved) {
                moved = true
                row.page.dragStart(row.index)
            }
            const at = mapToItem(row.page, mouse.x, mouse.y)
            row.page.dragMove(at.x, at.y)
        }
        onReleased: mouse => {
            if (!moved) {
                row.page.select(row.uid)
                return
            }
            const at = mapToItem(row.page, mouse.x, mouse.y)
            row.page.dragEnd(at.x, at.y, (mouse.modifiers & Qt.ControlModifier) !== 0)
        }
        onCanceled: {
            row.page.dragFrom = -1
            row.page.dragInsert = -1
            row.page.dragBus = -1
        }
    }

    Text {
        x: row.panel.theme.fxRowEndInset
        anchors.verticalCenter: parent.verticalCenter
        text: "⋮⋮"
        color: row.panel.theme.faint
        font.pixelSize: row.panel.theme.typeLabel
    }
    NovaLed {
        x: row.panel.theme.fxRowLedX
        anchors.verticalCenter: parent.verticalCenter
        panel: row.panel
        parameterId: "nova.fx." + row.uid + ".on"
    }
    Column {
        x: row.panel.theme.fxRowNameX
        anchors.verticalCenter: parent.verticalCenter
        width: meter.x - x - row.panel.theme.spaceMd
        spacing: row.panel.theme.fxRowLineGap
        Text {
            objectName: "novaFxRowName:" + row.uid
            width: parent.width
            text: row.spec !== null ? row.spec.name.toUpperCase() : row.module
            color: row.panel.theme.text
            font.family: row.panel.theme.sans
            font.pixelSize: row.panel.theme.typeBody
            font.weight: Font.ExtraBold
            font.letterSpacing: row.panel.theme.fxRackNameSpacing
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            text: row.page.presetName(row.uid, row.module) + (row.modeName !== "" ? " · " + row.modeName : "")
            color: row.panel.theme.faint
            font.family: row.panel.theme.sans
            font.pixelSize: row.panel.theme.typeSmall
            elide: Text.ElideRight
        }
    }
    // Input (dim) over output (cyan; red when it clips).
    Column {
        id: meter
        objectName: "novaFxRowMeter:" + row.uid
        readonly property real input: row.fraction(Contract.slotDb(row.panel.frame, Contract.T.slotIn, row.uid))
        readonly property real output: row.fraction(Contract.slotDb(row.panel.frame, Contract.T.slotOut, row.uid))
        x: kebab.x - width - row.panel.theme.spaceSm
        anchors.verticalCenter: parent.verticalCenter
        width: row.panel.theme.fxRowMeterWidth
        spacing: row.panel.theme.spaceXs
        Rectangle {
            width: parent.width
            height: row.panel.theme.fxRowMeterHeight
            radius: height / 2
            color: row.panel.theme.knobTrack
            Rectangle {
                width: parent.width * meter.input
                height: parent.height
                radius: height / 2
                color: row.panel.theme.faint
            }
        }
        Rectangle {
            width: parent.width
            height: row.panel.theme.fxRowMeterHeight
            radius: height / 2
            color: row.panel.theme.knobTrack
            Rectangle {
                width: parent.width * meter.output
                height: parent.height
                radius: height / 2
                color: meter.output >= 1 ? row.panel.theme.red : row.panel.theme.pk
            }
        }
    }
    Text {
        id: kebab
        objectName: "novaFxRowMenu:" + row.uid
        x: parent.width - width - row.panel.theme.fxRowEndInset
        anchors.verticalCenter: parent.verticalCenter
        text: "⋯"
        color: kebabArea.containsMouse ? row.panel.theme.text : row.panel.theme.faint
        font.pixelSize: row.panel.theme.typeHeading
        activeFocusOnTab: row.controller.editable
        Accessible.role: Accessible.Button
        Accessible.name: "More actions"
        Keys.onReturnPressed: row.page.openSlotMenu(kebab, row.index)
        Keys.onSpacePressed: row.page.openSlotMenu(kebab, row.index)
        // The focus ring, in the FX accent.
        Rectangle {
            visible: kebab.activeFocus
            anchors.fill: parent
            anchors.margins: -row.panel.theme.fxFocusOutset
            radius: row.panel.theme.radiusMd
            color: "transparent"
            border.color: row.panel.theme.pk
            border.width: row.panel.theme.strokeBold
        }
        MouseArea {
            id: kebabArea
            anchors.fill: parent
            anchors.leftMargin: -Math.max(row.panel.theme.fxMenuHit, row.panel.theme.hitOutset(parent.width))
            anchors.rightMargin: -Math.max(row.panel.theme.fxMenuHit, row.panel.theme.hitOutset(parent.width))
            anchors.topMargin: -Math.max(row.panel.theme.fxMenuHit, row.panel.theme.hitOutset(parent.height))
            anchors.bottomMargin: -Math.max(row.panel.theme.fxMenuHit, row.panel.theme.hitOutset(parent.height))
            hoverEnabled: true
            enabled: row.controller.editable
            onClicked: row.page.openSlotMenu(kebab, row.index)
        }
    }
}
