pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: editor
    required property var automation
    readonly property var lane: automation ? automation.lane : ({ present: false, points: [] })
    readonly property var parameters: automation ? automation.parameters : []
    // Layout, top to bottom: title, parameter chooser (its list over the
    // rest), summary and status, the bar / value / add row, timebase and
    // Clear lane, then the points.
    readonly property int titleY: 12
    readonly property int chooserY: 38
    readonly property int chooserHeight: 30
    readonly property int listY: 68
    readonly property int listHeight: 155
    readonly property int listRowHeight: 26
    readonly property int summaryY: 77
    readonly property int statusY: 95
    readonly property int entryY: 129
    readonly property int entrySpacing: 7
    readonly property int entryHeight: 31
    readonly property int barWidth: 65
    readonly property int valueWidth: 76
    readonly property int addWidth: 90
    readonly property int laneRowY: 172
    readonly property int laneButtonHeight: 28
    readonly property int timebaseWidth: 126
    readonly property int clearWidth: 103
    readonly property int scaleNoteY: 211
    readonly property int pointsY: 233
    readonly property int pointRowHeight: 43
    readonly property int pointLabelWidth: 110
    readonly property int pointValueX: 112
    readonly property int pointControlHeight: 30
    readonly property int pointValueWidth: 72
    readonly property int pointSetX: 189
    readonly property int pointSetWidth: 31
    readonly property int pointRemoveX: 225
    readonly property int pointRemoveWidth: 43
    radius: Theme.radiusXl
    color: Theme.synthAutomationGround
    border.color: Theme.synthAutomationEdge
    clip: true
    onAutomationChanged: parameterChoice.sync()
    Connections {
        target: editor.automation
        function onChanged() { parameterChoice.sync() }
    }

    component Action: Rectangle {
        id: action
        required property string label
        required property string accessibleName
        property bool available: true
        signal triggered()
        radius: Theme.radiusMd
        color: available ? (area.containsMouse ? Theme.synthAutomationActionHover : Theme.synthAutomationAction) : Theme.synthAutomationActionUnavailable
        border.color: activeFocus ? Theme.synthAutomationFocusEdge : Theme.synthAutomationEdge
        activeFocusOnTab: available
        Accessible.role: Accessible.Button
        Accessible.name: accessibleName
        Keys.onReturnPressed: if (available) triggered()
        Keys.onSpacePressed: if (available) triggered()
        Text {
            anchors.centerIn: parent
            text: action.label
            color: action.available ? Theme.synthAutomationText : Theme.synthAutomationTextUnavailable
            font.pixelSize: Theme.typeSmall
            font.bold: true
        }
        MouseArea {
            id: area
            anchors.fill: parent
            enabled: action.available
            hoverEnabled: true
            onClicked: action.triggered()
        }
    }
    component NumberInput: Rectangle {
        id: field
        property alias text: input.text
        property alias validator: input.validator
        readonly property bool acceptable: input.acceptableInput && input.text.length > 0
        required property string accessibleName
        radius: Theme.radiusMd
        color: Theme.synthAutomationField
        border.color: input.activeFocus ? Theme.synthAutomationFocusEdge : Theme.synthAutomationEdge
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceSm
            anchors.rightMargin: Theme.spaceSm
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.synthAutomationText
            font.pixelSize: Theme.typeBody
            selectByMouse: true
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: field.accessibleName
        }
    }

    Text {
        x: Theme.synthAutomationInset; y: editor.titleY
        text: "SYNTH AUTOMATION"
        color: Theme.synthAutomationLabel
        font.pixelSize: Theme.typeLabel
        font.bold: true
    }
    Action {
        id: parameterChoice
        objectName: "nativeSynthAutomationParameter"
        x: Theme.synthAutomationInset; y: editor.chooserY; width: parent.width - 2 * Theme.synthAutomationInset; height: editor.chooserHeight
        label: editor.lane.name ? editor.lane.name + "  ▾" : "CHOOSE PARAMETER  ▾"
        accessibleName: "Choose synthesizer automation parameter"
        available: editor.automation !== null
        onTriggered: parameterList.visible = !parameterList.visible
        function sync() { parameterList.visible = false }
    }
    ListView {
        id: parameterList
        z: 4
        x: Theme.synthAutomationInset; y: editor.listY; width: parent.width - 2 * Theme.synthAutomationInset; height: editor.listHeight
        visible: false
        clip: true
        model: editor.parameters
        delegate: Action {
            required property var modelData
            width: parameterList.width
            height: editor.listRowHeight
            label: modelData.group + " · " + modelData.name
            accessibleName: "Automate " + modelData.name
            onTriggered: {
                editor.automation.select_parameter(modelData.id)
                parameterList.visible = false
            }
        }
    }
    Text {
        x: Theme.synthAutomationInset; y: editor.summaryY; width: parent.width - 2 * Theme.synthAutomationInset
        text: editor.automation && editor.automation.message.length > 0
              ? editor.automation.message
              : editor.lane.present
              ? editor.lane.timebase + " · " + editor.lane.points.length + " points"
              : "No lane · baseline " + Number(editor.lane.baseline || 0).toFixed(3)
        color: Theme.synthAutomationSummary
        font.pixelSize: Theme.typeBody
    }
    Text {
        objectName: "nativeSynthAutomationPlaybackStatus"
        x: Theme.synthAutomationInset; y: editor.statusY; width: parent.width - 2 * Theme.synthAutomationInset
        text: editor.automation ? editor.automation.playbackStatus : ""
        color: text.indexOf("could not apply") >= 0 ? Theme.synthAutomationWarning : Theme.synthAutomationOk
        font.pixelSize: Theme.typeSmall
        wrapMode: Text.WordWrap
    }
    Row {
        x: Theme.synthAutomationInset; y: editor.entryY; spacing: editor.entrySpacing
        NumberInput {
            id: barInput
            objectName: "nativeSynthAutomationBar"
            width: editor.barWidth; height: editor.entryHeight
            text: "1"
            validator: IntValidator { bottom: 1; top: 65536 }
            accessibleName: "Automation bar"
        }
        NumberInput {
            id: valueInput
            objectName: "nativeSynthAutomationValue"
            width: editor.valueWidth; height: editor.entryHeight
            text: editor.lane.baseline === undefined
                  ? "0.500" : Number(editor.lane.baseline).toFixed(3)
            validator: DoubleValidator { bottom: 0; top: 1; decimals: 3; locale: "C" }
            accessibleName: "Normalized automation value"
        }
        Action {
            objectName: "nativeSynthAutomationAddPoint"
            width: editor.addWidth; height: editor.entryHeight
            label: editor.lane.present ? "+ POINT" : "+ LANE"
            accessibleName: "Add synth automation point"
            available: editor.automation !== null && barInput.acceptable && valueInput.acceptable
            onTriggered: editor.automation.set_point(Number(barInput.text) - 1,
                                                     Number(valueInput.text))
        }
    }
    Row {
        x: Theme.synthAutomationInset; y: editor.laneRowY; spacing: Theme.spaceMd
        Action {
            objectName: "nativeSynthAutomationTimebase"
            width: editor.timebaseWidth; height: editor.laneButtonHeight
            label: "TIME: " + (editor.lane.timebase || "Musical")
            available: editor.lane.present
            accessibleName: "Toggle synth automation timebase"
            onTriggered: editor.automation.toggle_timebase()
        }
        Action {
            objectName: "nativeSynthAutomationRemoveLane"
            width: editor.clearWidth; height: editor.laneButtonHeight
            label: "CLEAR LANE"
            available: editor.lane.present
            accessibleName: "Remove synth automation lane"
            onTriggered: editor.automation.remove_lane()
        }
    }
    Text {
        x: Theme.synthAutomationInset; y: editor.scaleNoteY
        text: "NORMALIZED 0–1"
        color: Theme.synthAutomationNote
        font.pixelSize: Theme.typeSmall
    }
    ListView {
        id: points
        objectName: "nativeSynthAutomationPoints"
        x: Theme.synthAutomationInset; y: editor.pointsY
        width: parent.width - 2 * Theme.synthAutomationInset
        height: parent.height - y - Theme.spaceLg
        clip: true
        model: editor.lane.points || []
        delegate: Item {
            id: row
            required property var modelData
            width: points.width
            height: editor.pointRowHeight
            Text {
                width: editor.pointLabelWidth
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.position
                elide: Text.ElideRight
                color: Theme.synthAutomationLabel
                font.pixelSize: Theme.typeBody
            }
            NumberInput {
                id: pointValue
                objectName: "nativeSynthAutomationPointValue"
                x: editor.pointValueX; y: Theme.spaceXs; width: editor.pointValueWidth; height: editor.pointControlHeight
                text: Number(row.modelData.value).toFixed(3)
                validator: DoubleValidator { bottom: 0; top: 1; decimals: 3; locale: "C" }
                accessibleName: "Automation point value at " + row.modelData.position
            }
            Action {
                objectName: "nativeSynthAutomationSetPointValue"
                x: editor.pointSetX; y: Theme.spaceXs; width: editor.pointSetWidth; height: editor.pointControlHeight
                label: "✓"
                accessibleName: "Apply automation value at " + row.modelData.position
                available: pointValue.acceptable
                onTriggered: editor.automation.set_value(row.modelData.id,
                                                          Number(pointValue.text))
            }
            Action {
                objectName: "nativeSynthAutomationRemovePoint"
                x: editor.pointRemoveX; y: Theme.spaceXs; width: editor.pointRemoveWidth; height: editor.pointControlHeight
                label: "×"
                accessibleName: "Remove automation point at " + row.modelData.position
                onTriggered: editor.automation.remove_point(row.modelData.id)
            }
        }
    }
}
