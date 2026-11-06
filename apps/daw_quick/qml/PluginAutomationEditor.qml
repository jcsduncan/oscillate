pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Track-owned VST3 parameter lane for the plugin open in the VST3 editor.
// Values are normalized 0..1; the saved parameter reference is the baseline.
Item {
    id: editor
    required property var bridge
    required property var controller
    required property var view
    required property color panelColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    property color warningColor: Theme.automationInactiveWarning
    readonly property var parameter: view.parameter || ({ id: 0, name: "", baseline: 0, hasBaseline: false })
    readonly property var lane: view.lane || ({ present: false, laneId: "", timebase: "Musical", pointCount: 0, points: [] })
    readonly property bool editEnabled: bridge.audioEditEnabled
    readonly property bool canCreate: lane.present || Boolean(parameter.canAutomate)
    // Layout, top to bottom: back and timebase, title, identity, status, the
    // bar / value / add row, the count and Clear lane, then the point list.
    readonly property int backWidth: 92
    readonly property int timebaseY: 2
    readonly property int titleY: 32
    readonly property int identityY: 50
    readonly property int statusY: 67
    readonly property int statusMinHeight: 12
    readonly property int barFieldWidth: 46
    readonly property int valueFieldX: 52
    readonly property int valueFieldWidth: 56
    readonly property int addX: 114
    readonly property int countGap: 32
    readonly property int clearGap: 28
    readonly property int clearWidth: 80
    readonly property int listGap: 58
    readonly property int segmentRowExtra: 32
    readonly property int pointRowHeight: 28
    readonly property int pointTextY: 7
    readonly property int pointTextReserve: 96
    readonly property int pointValueRight: 90
    readonly property int pointRemoveWidth: 28
    readonly property int segmentY: 30

    component ActionButton: Rectangle {
        id: button
        required property string label
        required property string actionName
        property bool primary: false
        signal triggered()
        height: Theme.automationControlHeight
        radius: Theme.radiusMd
        color: enabled && primary ? Theme.automationApplyFill : editor.panelColor
        border.color: activeFocus || (enabled && primary) ? editor.accentColor : editor.lineColor
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: actionName
        Accessible.onPressAction: if (enabled) triggered()
        Keys.onReturnPressed: if (enabled) triggered()
        Keys.onEnterPressed: if (enabled) triggered()
        Keys.onSpacePressed: if (enabled) triggered()
        function activate() {
            if (enabled)
                triggered()
        }
        Text {
            anchors.centerIn: parent
            text: button.label
            color: !button.enabled ? editor.dimColor : button.primary ? editor.accentColor : editor.textColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            anchors.fill: parent
            enabled: button.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: button.triggered()
        }
    }

    component NumberField: Rectangle {
        id: field
        property alias text: input.text
        property alias validator: input.validator
        property alias editable: input.enabled
        readonly property bool acceptable: input.acceptableInput && input.text.length > 0
        required property string accessibleName
        signal committed()
        height: Theme.automationControlHeight
        radius: Theme.radiusMd
        color: editor.panelColor
        border.color: input.activeFocus ? editor.accentColor : editor.lineColor
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: Theme.automationFieldTextInset
            anchors.rightMargin: Theme.automationFieldTextInset
            verticalAlignment: TextInput.AlignVCenter
            color: editor.textColor
            font.pixelSize: Theme.typeSmall
            selectByMouse: true
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: field.accessibleName
            onEditingFinished: field.committed()
        }
    }

    ActionButton {
        id: backButton
        objectName: "pluginAutomationBack"
        x: 0
        y: 0
        width: editor.backWidth
        label: "‹ PARAMETERS"
        actionName: "Back to plugin parameters"
        onTriggered: editor.controller.show_parameters()
    }
    AutomationTimebaseAction {
        objectName: "pluginAutomationTimebase"
        x: editor.width - width
        y: editor.timebaseY
        bridge: editor.bridge
        laneId: editor.lane.laneId || ""
        timebase: editor.lane.timebase || "Musical"
        accentColor: editor.accentColor
    }
    Text {
        objectName: "pluginAutomationTitle"
        y: editor.titleY
        width: editor.width
        elide: Text.ElideRight
        text: "AUTOMATION  ·  " + editor.parameter.name
        color: editor.textColor
        font.pixelSize: Theme.typeBody
        font.bold: true
    }
    Text {
        objectName: "pluginAutomationIdentity"
        y: editor.identityY
        width: editor.width
        elide: Text.ElideRight
        text: "ID " + editor.parameter.id + "  ·  "
              + (editor.parameter.hasBaseline ? "BASELINE " + Number(editor.parameter.baseline).toFixed(3)
                                              : "NO SAVED BASELINE")
              + "  ·  NORMALIZED 0–1"
        color: editor.dimColor
        font.pixelSize: Theme.typeCaption
    }
    Text {
        id: statusText
        objectName: "pluginAutomationStatus"
        y: editor.statusY
        width: editor.width
        wrapMode: Text.WordWrap
        maximumLineCount: 3
        elide: Text.ElideRight
        text: editor.view.renderStatus || ""
        color: editor.view.renderState === "active" ? editor.accentColor
             : ["stopped", "none"].includes(editor.view.renderState) ? editor.dimColor
             : editor.warningColor
        font.pixelSize: Theme.typeCaption
        Accessible.role: Accessible.StaticText
        Accessible.name: "Automation render status: " + text
    }
    Item {
        id: entryRow
        y: statusText.y + Math.max(statusText.implicitHeight, editor.statusMinHeight) + Theme.spaceMd
        width: editor.width
        height: Theme.automationControlHeight
        NumberField {
            id: barField
            objectName: "pluginAutomationBarInput"
            width: editor.barFieldWidth
            text: "1"
            validator: IntValidator { bottom: 1; top: 65536 }
            accessibleName: "Plugin automation bar, 1 to 65536"
        }
        NumberField {
            id: valueField
            objectName: "pluginAutomationValueInput"
            x: editor.valueFieldX
            width: editor.valueFieldWidth
            text: editor.parameter.hasBaseline ? Number(editor.parameter.baseline).toFixed(3) : ""
            validator: DoubleValidator { bottom: 0; top: 1; decimals: 3; locale: "C" }
            accessibleName: "Plugin automation normalized value, 0 to 1"
        }
        ActionButton {
            id: addPoint
            objectName: "pluginAutomationAddPoint"
            x: editor.addX
            width: entryRow.width - editor.addX
            primary: true
            label: editor.lane.present ? "+ POINT" : "+ LANE AND POINT"
            actionName: editor.lane.present ? "Add or replace plugin automation point at bar"
                                            : "Create plugin automation lane with a point at bar"
            enabled: editor.editEnabled && editor.canCreate
                     && barField.acceptable && valueField.acceptable
            onTriggered: editor.controller.set_point(Number(barField.text) - 1,
                                                     Number(valueField.text))
        }
    }
    Text {
        y: entryRow.y + editor.countGap
        text: editor.lane.pointCount + " POINT" + (editor.lane.pointCount === 1 ? "" : "S")
        color: editor.dimColor
        font.pixelSize: Theme.typeCaption
    }
    ActionButton {
        id: removeLane
        objectName: "pluginAutomationRemoveLane"
        x: editor.width - width
        y: entryRow.y + editor.clearGap
        width: editor.clearWidth
        visible: editor.lane.present
        label: "CLEAR LANE"
        actionName: "Remove automation lane for " + editor.parameter.name
        enabled: editor.editEnabled
        onTriggered: editor.controller.remove_lane()
    }
    ListView {
        id: pointList
        objectName: "pluginAutomationPointList"
        y: entryRow.y + editor.listGap
        width: editor.width
        height: Math.max(0, editor.height - y)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: editor.lane.points || []
        delegate: Item {
            id: pointRow
            required property var modelData
            required property int index
            objectName: "pluginAutomationPointRow"
            width: pointList.width
            height: index < editor.lane.pointCount - 1 ? segmentControls.height + editor.segmentRowExtra : editor.pointRowHeight
            Text {
                y: editor.pointTextY
                width: parent.width - editor.pointTextReserve
                elide: Text.ElideRight
                text: pointRow.modelData.position
                color: editor.textColor
                font.pixelSize: Theme.typeSmall
            }
            NumberField {
                id: pointValue
                objectName: "pluginAutomationPointValue"
                x: parent.width - editor.pointValueRight
                y: Theme.spaceXxs
                width: editor.valueFieldWidth
                text: Number(pointRow.modelData.value).toFixed(3)
                validator: DoubleValidator { bottom: 0; top: 1; decimals: 3; locale: "C" }
                accessibleName: "Normalized value at " + pointRow.modelData.position
                editable: editor.editEnabled
                onCommitted: {
                    if (pointValue.acceptable
                            && Math.abs(Number(pointValue.text) - Number(pointRow.modelData.value)) > 0.000001)
                        editor.controller.set_value(pointRow.modelData.id, Number(pointValue.text))
                }
            }
            ActionButton {
                objectName: "pluginAutomationRemovePoint"
                x: parent.width - width
                y: Theme.spaceXxs
                width: editor.pointRemoveWidth
                label: "×"
                actionName: "Remove plugin automation point at " + pointRow.modelData.position
                enabled: editor.editEnabled
                onTriggered: editor.controller.remove_point(pointRow.modelData.id)
            }
            AutomationSegmentControls {
                id: segmentControls
                objectName: "pluginAutomationSegmentControls"
                y: editor.segmentY
                width: pointRow.width
                visible: pointRow.index < editor.lane.pointCount - 1
                bridge: editor.bridge
                laneId: editor.lane.laneId || ""
                pointId: pointRow.modelData.id
                panelColor: editor.panelColor
                lineColor: editor.lineColor
                textColor: editor.textColor
                dimColor: editor.dimColor
                accentColor: editor.accentColor
            }
        }
    }
}
