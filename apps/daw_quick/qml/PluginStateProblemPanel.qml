pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// A project change (switching projects, Save a copy or closing) stopped
// because an open plug-in editor's latest state could not be saved. The
// user keeps editing, or goes on without that state.
Rectangle {
    id: problemPanel
    transformOrigin: Item.Center
    Reveal { target: problemPanel; fromScale: Motion.popScale }
    required property var host
    required property var bridge
    objectName: "pluginStateProblemPanel"
    visible: bridge.pluginStateProblem.length > 0
    readonly property int panelWidth: 420
    readonly property int panelHeight: 196
    readonly property int titleY: 15
    readonly property int messageY: 42
    readonly property int messageLines: 4
    readonly property int buttonWidth: 176
    z: 9
    anchors.centerIn: parent
    width: panelWidth
    height: panelHeight
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.amber
    function keepEditing() { bridge.cancel_project_change() }
    function goOn() { bridge.continue_without_plugin_state() }
    onVisibleChanged: if (visible) keepButton.forceActiveFocus()
    Keys.onEscapePressed: keepEditing()
    MouseArea {
        parent: problemPanel.parent
        anchors.fill: parent
        z: problemPanel.z - 0.01
        visible: problemPanel.visible
    }
    MouseArea { anchors.fill: parent }
    Text {
        x: Theme.spaceXl
        y: problemPanel.titleY
        text: "PLUG-IN STATE NOT SAVED"
        color: problemPanel.host.amber
        font.pixelSize: Theme.typeBody
        font.bold: true
        font.letterSpacing: Theme.trackingXl
    }
    Text {
        objectName: "pluginStateProblemMessage"
        x: Theme.spaceXl
        y: problemPanel.messageY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        maximumLineCount: problemPanel.messageLines
        elide: Text.ElideRight
        text: problemPanel.bridge.pluginStateProblem
        color: problemPanel.host.textMain
        font.pixelSize: Theme.typeSmall
    }
    component ChoiceButton: Rectangle {
        id: choice
        required property string label
        required property bool primary
        signal chosen()
        width: problemPanel.buttonWidth
        height: Theme.dialogButtonHeight
        radius: Theme.controlRadius
        color: primary ? problemPanel.host.accent : problemPanel.host.panel
        border.color: activeFocus ? problemPanel.host.textMain : problemPanel.host.line
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Keys.onReturnPressed: chosen()
        Keys.onSpacePressed: chosen()
        Text {
            anchors.centerIn: parent
            text: choice.label
            color: choice.primary ? Theme.onAccentText : problemPanel.host.textMain
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            onClicked: choice.chosen()
        }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceXl
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceXl
        spacing: Theme.spaceMd
        ChoiceButton {
            id: keepButton
            objectName: "pluginStateKeepButton"
            label: "KEEP EDITING"
            primary: true
            Accessible.name: "Stay in this project with the plug-in editor open"
            onChosen: problemPanel.keepEditing()
        }
        ChoiceButton {
            objectName: "pluginStateContinueButton"
            label: "GO ON WITHOUT IT"
            primary: false
            Accessible.name: "Go on without the plug-in's latest state"
            onChosen: problemPanel.goOn()
        }
    }
}
