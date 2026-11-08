pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Save a copy as…: saves the open project, writes it with its saved versions
// and media as a new project beside it, and opens the copy.
Rectangle {
    id: saveCopyPanel
    transformOrigin: Item.Top
    Reveal { target: saveCopyPanel; fromScale: Motion.popScale }
    required property var host
    required property var bridge
    objectName: "saveCopyPanel"
    visible: bridge.projectOpen && host.saveCopyPanelOpen
    readonly property int panelWidth: 340
    readonly property int panelHeight: 196
    readonly property int titleY: 15
    readonly property int hintY: 38
    readonly property int nameY: 104
    readonly property int nameHeight: 32
    readonly property int saveWidth: 85
    readonly property int noteY: 150
    readonly property int maxNameLength: 80
    z: 6
    anchors.right: parent.right
    anchors.rightMargin: Theme.menuPanelRightMargin
    width: panelWidth
    height: panelHeight
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.line
    readonly property bool canSave: copyName.text.trim().length > 0
    function close() { host.saveCopyPanelOpen = false }
    function save() {
        if (!canSave)
            return
        const name = copyName.text.trim()
        close()
        bridge.save_copy(name)
    }
    // Each opening offers the first free "<project> copy" name.
    onVisibleChanged: {
        if (!visible)
            return
        copyName.text = bridge.suggested_copy_name()
        copyName.selectAll()
        copyName.forceActiveFocus()
    }
    Keys.onEscapePressed: close()
    MouseArea {
        parent: saveCopyPanel.parent
        anchors.fill: parent
        z: saveCopyPanel.z - 0.01
        visible: saveCopyPanel.visible
        onPressed: saveCopyPanel.close()
    }
    MouseArea { anchors.fill: parent }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        y: Theme.dialogCloseY
        text: "×"
        color: closeArea.containsMouse ? saveCopyPanel.host.textMain : saveCopyPanel.host.textDim
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        font.pixelSize: Theme.typeDisplay
        Accessible.role: Accessible.Button
        Accessible.name: "Close save a copy"
        MouseArea {
            Tip {}
            id: closeArea
            anchors.fill: parent
            anchors.margins: -Theme.closeHitSlop
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: saveCopyPanel.close()
        }
    }
    Text {
        x: Theme.spaceXl
        y: saveCopyPanel.titleY
        text: "SAVE A COPY AS"
        color: saveCopyPanel.host.textMain
        font.pixelSize: Theme.typeBody
        font.bold: true
        font.letterSpacing: Theme.trackingXl
    }
    Text {
        x: Theme.spaceXl
        y: saveCopyPanel.hintY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: "Saves this project, then writes it to a new project beside it and opens the copy. This project stays as it is."
        color: saveCopyPanel.host.textDim
        font.pixelSize: Theme.typeSmall
    }
    Rectangle {
        x: Theme.spaceXl
        y: saveCopyPanel.nameY
        width: parent.width - 2 * Theme.spaceXl - saveCopyPanel.saveWidth - Theme.spaceMd
        height: saveCopyPanel.nameHeight
        radius: Theme.controlRadius
        color: saveCopyPanel.host.panel
        border.color: copyName.activeFocus ? saveCopyPanel.host.accent : saveCopyPanel.host.line
        TextInput {
            id: copyName
            objectName: "saveCopyNameInput"
            anchors.fill: parent
            anchors.margins: Theme.spaceMd
            clip: true
            color: saveCopyPanel.host.textMain
            font.pixelSize: Theme.typeBody
            maximumLength: saveCopyPanel.maxNameLength
            selectByMouse: true
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Name of the copy"
            Keys.onReturnPressed: saveCopyPanel.save()
            Keys.onEnterPressed: saveCopyPanel.save()
        }
    }
    Rectangle {
        objectName: "saveCopyButton"
        x: parent.width - saveCopyPanel.saveWidth - Theme.spaceXl
        y: saveCopyPanel.nameY
        width: saveCopyPanel.saveWidth
        height: saveCopyPanel.nameHeight
        radius: Theme.controlRadius
        color: enabled ? saveCopyPanel.host.accent : saveCopyPanel.host.panel
        enabled: saveCopyPanel.canSave
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: "Save the copy"
        Keys.onReturnPressed: saveCopyPanel.save()
        Keys.onSpacePressed: saveCopyPanel.save()
        Text {
            anchors.centerIn: parent
            text: "SAVE"
            color: parent.enabled ? Theme.onAccentText : saveCopyPanel.host.textDim
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            enabled: parent.enabled
            onClicked: saveCopyPanel.save()
        }
    }
    Text {
        x: Theme.spaceXl
        y: saveCopyPanel.noteY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: "The copy keeps the saved versions and holds its own copy of every sound, linked files included."
        color: saveCopyPanel.host.textDim
        font.pixelSize: Theme.typeSmall
    }
}
