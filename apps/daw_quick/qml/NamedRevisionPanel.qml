pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: namedRevisionPanel
    transformOrigin: Item.Top
    Reveal { target: namedRevisionPanel; fromScale: Motion.popScale }
    required property var host
    required property var bridge
    objectName: "namedRevisionPanel"
    property bool actionsEnabled: bridge.namedRevisionEnabled && !bridge.imports.importBusy
        && !bridge.exports.exportBusy && !bridge.vst3Browser.vst3BrowserBusy
    visible: bridge.projectOpen && host.revisionPanelOpen
    readonly property int panelWidth: 340
    readonly property int panelHeight: 405
    readonly property int titleY: 15
    readonly property int hintY: 38
    readonly property int nameY: 83
    readonly property int nameHeight: 32
    readonly property int saveWidth: 85
    readonly property int statusY: 130
    readonly property int emptyY: 160
    readonly property int listY: 151
    readonly property int listHeight: 151
    readonly property int rowHeight: 42
    readonly property int rowTextX: 10
    readonly property int rowActionRoom: 93
    readonly property int issuesY: 307
    z: 6
    anchors.right: parent.right
    anchors.rightMargin: Theme.menuPanelRightMargin
    width: panelWidth
    height: panelHeight
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.line
    readonly property bool canSave: actionsEnabled && revisionName.text.length > 0
    function close() { host.revisionPanelOpen = false }
    // Saves the typed name as a version; the field clears once it is listed.
    function save() {
        if (!canSave)
            return
        const name = revisionName.text
        bridge.create_named_revision(name)
        if (bridge.namedRevisions.some(revision => revision.name === name))
            revisionName.text = ""
    }
    // Escape, a click anywhere else, or × closes it.
    onVisibleChanged: if (visible) forceActiveFocus()
    Keys.onEscapePressed: close()
    MouseArea {
        parent: namedRevisionPanel.parent
        anchors.fill: parent
        z: namedRevisionPanel.z - 0.01
        visible: namedRevisionPanel.visible
        onPressed: namedRevisionPanel.close()
    }
    MouseArea { anchors.fill: parent }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        y: Theme.dialogCloseY
        text: "×"
        color: closeArea.containsMouse ? host.textMain : host.textDim
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        font.pixelSize: Theme.typeDisplay
        Accessible.role: Accessible.Button
        Accessible.name: "Close versions"
        MouseArea {
            Tip {}
            id: closeArea
            anchors.fill: parent
            anchors.margins: -Theme.closeHitSlop
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: namedRevisionPanel.close()
        }
    }
    Text {
        x: Theme.spaceXl
        y: namedRevisionPanel.titleY
        text: "VERSIONS"
        color: host.textMain
        font.pixelSize: Theme.typeBody
        font.bold: true
        font.letterSpacing: Theme.trackingXl
    }
    Text {
        x: Theme.spaceXl
        y: namedRevisionPanel.hintY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: "Save a version, then go back to it when playback is stopped. Going back can be undone until this project closes. Names use letters, numbers, spaces, dots, _ and -."
        color: host.textDim
        font.pixelSize: Theme.typeSmall
    }
    Rectangle {
        x: Theme.spaceXl
        y: namedRevisionPanel.nameY
        width: parent.width - 2 * Theme.spaceXl - namedRevisionPanel.saveWidth - Theme.spaceMd
        height: namedRevisionPanel.nameHeight
        radius: Theme.controlRadius
        color: host.panel
        border.color: revisionName.activeFocus ? host.accent : host.line
        TextInput {
            id: revisionName
            objectName: "namedRevisionInput"
            anchors.fill: parent
            anchors.margins: Theme.spaceMd
            color: host.textMain
            font.pixelSize: Theme.typeBody
            maximumLength: 64
            activeFocusOnTab: true
            Accessible.name: "New version name"
            Accessible.description: "Letters, numbers, spaces, dots, underscores and hyphens; up to 64 characters"
            Keys.onReturnPressed: namedRevisionPanel.save()
            Keys.onEnterPressed: namedRevisionPanel.save()
        }
    }
    Rectangle {
        objectName: "createNamedRevisionButton"
        x: parent.width - namedRevisionPanel.saveWidth - Theme.spaceXl
        y: namedRevisionPanel.nameY
        width: namedRevisionPanel.saveWidth
        height: namedRevisionPanel.nameHeight
        radius: Theme.controlRadius
        color: enabled ? host.accent : host.panel
        enabled: namedRevisionPanel.canSave
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: "Save version"
        Keys.onReturnPressed: namedRevisionPanel.save()
        Keys.onSpacePressed: namedRevisionPanel.save()
        Text {
            anchors.centerIn: parent
            text: "SAVE"
            color: parent.enabled ? Theme.onAccentText : host.textDim
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            enabled: parent.enabled
            onClicked: namedRevisionPanel.save()
        }
    }
    Text {
        x: Theme.spaceXl
        y: namedRevisionPanel.statusY
        text: "SAVED VERSIONS"
        color: host.textDim
        font.pixelSize: Theme.typeSmall
        font.bold: true
    }
    Text {
        x: Theme.spaceXl
        y: namedRevisionPanel.emptyY
        visible: bridge.namedRevisions.length === 0
        text: "No versions yet — save one to come back to it"
        color: host.textDim
        font.pixelSize: Theme.typeBody
    }
    ListView {
        x: Theme.spaceXl
        y: namedRevisionPanel.listY
        width: parent.width - 2 * Theme.spaceXl
        height: namedRevisionPanel.listHeight
        clip: true
        model: bridge.namedRevisions
        delegate: Rectangle {
            required property var modelData
            width: ListView.view.width
            height: namedRevisionPanel.rowHeight
            radius: Theme.controlRadius
            color: host.panel
            border.color: activeFocus ? host.accent : host.line
            activeFocusOnTab: namedRevisionPanel.actionsEnabled
            Accessible.role: Accessible.Button
            Accessible.name: "Go back to version " + modelData.name
            Accessible.description: "Requires saved current changes and stopped playback"
            Keys.onReturnPressed: bridge.restore_named_revision(modelData.name)
            Keys.onSpacePressed: bridge.restore_named_revision(modelData.name)
            Text {
                x: namedRevisionPanel.rowTextX
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - namedRevisionPanel.rowActionRoom
                elide: Text.ElideRight
                text: modelData.name
                color: host.textMain
                font.pixelSize: Theme.typeBody
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: namedRevisionPanel.rowTextX
                anchors.verticalCenter: parent.verticalCenter
                text: "GO BACK"
                color: host.accent
                font.pixelSize: Theme.typeCaption
                font.bold: true
            }
            MouseArea {
                Tip {}
                anchors.fill: parent
                enabled: namedRevisionPanel.actionsEnabled
                onClicked: bridge.restore_named_revision(modelData.name)
            }
        }
    }
    Text {
        x: Theme.spaceXl
        y: namedRevisionPanel.issuesY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: bridge.namedRevisionIssues.length > 0
            ? bridge.namedRevisionIssues.slice(0, 2).map(issue => issue.name + ": " + issue.reason).join(" · ")
                + (bridge.namedRevisionIssues.length > 2 ? " · +" + (bridge.namedRevisionIssues.length - 2) + " more" : "")
            : "A name is never reused, so saving never overwrites a version."
        color: bridge.namedRevisionIssues.length > 0 ? host.amber : host.textDim
        font.pixelSize: Theme.typeSmall
    }
}
