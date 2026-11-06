pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: managedMediaPanel
    transformOrigin: Item.Top
    Reveal { target: managedMediaPanel; fromScale: Motion.popScale }
    required property var host
    required property var bridge
    objectName: "managedMediaPanel"
    visible: bridge.projectOpen && host.mediaPanelOpen
    readonly property int panelRightMargin: 330
    readonly property int panelWidth: 410
    readonly property int panelHeight: 500
    readonly property int statusY: 42
    readonly property int issuesY: 100
    readonly property int issuesHeight: 270
    readonly property int issueHeight: 140
    readonly property int issueTextX: 10
    readonly property int issueKindY: 27
    readonly property int issuePathY: 46
    readonly property int issueDetailY: 63
    readonly property int issueStateY: 79
    readonly property int repairY: 103
    readonly property int repairWidth: 165
    readonly property int repairHeight: 29
    readonly property int actionsY: 386
    readonly property int refreshWidth: 110
    readonly property int collectX: 139
    readonly property int actionHeight: 30
    readonly property int collectionStatusY: 427
    readonly property int noteY: 465
    z: 7
    anchors.right: parent.right
    anchors.rightMargin: panelRightMargin
    width: panelWidth
    height: panelHeight
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.line
    function close() { host.mediaPanelOpen = false }
    // Escape, a click anywhere else, or × closes it.
    onVisibleChanged: if (visible) forceActiveFocus()
    Keys.onEscapePressed: close()
    MouseArea {
        parent: managedMediaPanel.parent
        anchors.fill: parent
        z: managedMediaPanel.z - 0.01
        visible: managedMediaPanel.visible
        onPressed: managedMediaPanel.close()
    }
    MouseArea { anchors.fill: parent }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        y: Theme.dialogCloseY
        text: "×"
        color: managedMediaPanelCloseArea.containsMouse ? managedMediaPanel.host.textMain : managedMediaPanel.host.textDim
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        font.pixelSize: Theme.typeDisplay
        Accessible.role: Accessible.Button
        Accessible.name: "Close project media"
        MouseArea {
            Tip {}
            id: managedMediaPanelCloseArea
            anchors.fill: parent
            anchors.margins: -Theme.closeHitSlop
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: managedMediaPanel.close()
        }
    }
    Text {
        x: Theme.spaceXl
        y: Theme.spaceXl
        text: "PROJECT MEDIA"
        color: host.textMain
        font.pixelSize: Theme.typeBody
        font.bold: true
    }
    Text {
        x: Theme.spaceXl
        y: managedMediaPanel.statusY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: bridge.managedMedia.managedMediaStatus
        color: bridge.managedMedia.managedMediaIssues.length > 0 ? host.amber : host.textDim
        font.pixelSize: Theme.typeSmall
    }
    ListView {
        x: Theme.spaceXl
        y: managedMediaPanel.issuesY
        width: parent.width - 2 * Theme.spaceXl
        height: managedMediaPanel.issuesHeight
        clip: true
        model: bridge.managedMedia.managedMediaIssues
        delegate: Rectangle {
            required property var modelData
            width: ListView.view.width
            height: managedMediaPanel.issueHeight
            radius: Theme.radiusLg
            color: host.panel
            border.color: host.line
            Text {
                x: managedMediaPanel.issueTextX
                y: Theme.spaceMd
                text: modelData.status.toUpperCase()
                    + (modelData.external ? " SOURCE" : " MANAGED SOURCE")
                color: host.amber
                font.pixelSize: Theme.typeSmall
                font.bold: true
            }
            Text {
                x: managedMediaPanel.issueTextX
                y: managedMediaPanel.issueKindY
                width: parent.width - 2 * managedMediaPanel.issueTextX
                elide: Text.ElideMiddle
                text: modelData.id
                color: host.textMain
                font.pixelSize: Theme.typeSmall
            }
            Text {
                x: managedMediaPanel.issueTextX
                y: managedMediaPanel.issuePathY
                width: parent.width - 2 * managedMediaPanel.issueTextX
                text: modelData.encoding + " · " + modelData.bytes + " bytes · "
                    + modelData.sampleRate + " Hz · " + modelData.channels + " ch"
                color: host.textDim
                font.pixelSize: Theme.typeCaption
            }
            Text {
                x: managedMediaPanel.issueTextX
                y: managedMediaPanel.issueDetailY
                width: parent.width - 2 * managedMediaPanel.issueTextX
                elide: Text.ElideMiddle
                text: modelData.external ? "Last known: " + modelData.lastKnownPath
                    : modelData.repairable ? "Expected SHA-256: " + modelData.sha256
                    : "This source cannot be replaced in this panel"
                color: host.textDim
                font.pixelSize: Theme.typeCaption
            }
            Text {
                x: managedMediaPanel.issueTextX
                y: managedMediaPanel.issueStateY
                width: parent.width - 2 * managedMediaPanel.issueTextX
                elide: Text.ElideMiddle
                visible: modelData.external
                text: "Expected SHA-256: " + modelData.sha256
                color: host.textDim
                font.pixelSize: Theme.typeCaption
            }
            Rectangle {
                x: managedMediaPanel.issueTextX
                y: managedMediaPanel.repairY
                width: managedMediaPanel.repairWidth
                height: managedMediaPanel.repairHeight
                radius: Theme.controlRadius
                color: enabled ? host.accent : host.panelRaised
                enabled: (modelData.repairable || modelData.external)
                    && bridge.managedMedia.managedMediaRepairEnabled
                    && !bridge.imports.importBusy && !bridge.exports.exportBusy
                    && !bridge.vst3Browser.vst3BrowserBusy
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: (modelData.external ? "Relink external source "
                    : "Choose exact replacement for managed source ") + modelData.id
                Keys.onReturnPressed: modelData.external
                    ? bridge.managedMedia.choose_external_relink(modelData.id)
                    : bridge.managedMedia.choose_replacement(modelData.id)
                Keys.onSpacePressed: modelData.external
                    ? bridge.managedMedia.choose_external_relink(modelData.id)
                    : bridge.managedMedia.choose_replacement(modelData.id)
                Text {
                    anchors.centerIn: parent
                    text: modelData.external ? "RELINK SOURCE" : "CHOOSE REPLACEMENT"
                    color: parent.enabled ? Theme.onAccentText : host.textDim
                    font.pixelSize: Theme.typeCaption
                    font.bold: true
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    enabled: parent.enabled
                    onClicked: modelData.external
                        ? bridge.managedMedia.choose_external_relink(modelData.id)
                        : bridge.managedMedia.choose_replacement(modelData.id)
                }
            }
        }
    }
    Rectangle {
        x: Theme.spaceXl
        y: managedMediaPanel.actionsY
        width: managedMediaPanel.refreshWidth
        height: managedMediaPanel.actionHeight
        radius: Theme.controlRadius
        color: host.panel
        border.color: host.line
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: bridge.managedMedia.managedMediaBusy ? "Cancel media verification" : "Refresh media status"
        Keys.onReturnPressed: bridge.managedMedia.managedMediaBusy
            ? bridge.managedMedia.cancel() : bridge.managedMedia.refresh()
        Keys.onSpacePressed: bridge.managedMedia.managedMediaBusy
            ? bridge.managedMedia.cancel() : bridge.managedMedia.refresh()
        Text {
            anchors.centerIn: parent
            text: bridge.managedMedia.managedMediaBusy ? "CANCEL" : "REFRESH"
            color: host.textMain
            font.pixelSize: Theme.typeCaption
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            onClicked: bridge.managedMedia.managedMediaBusy
                ? bridge.managedMedia.cancel() : bridge.managedMedia.refresh()
        }
    }
    Rectangle {
        x: managedMediaPanel.collectX
        y: managedMediaPanel.actionsY
        width: parent.width - managedMediaPanel.collectX - Theme.spaceXl
        height: managedMediaPanel.actionHeight
        radius: Theme.controlRadius
        color: bridge.collectionBusy ? host.panel : host.accent
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: bridge.collectionBusy ? "Cancel collected project copy"
            : "Collect all media into a saved project copy"
        Keys.onReturnPressed: bridge.collectionBusy
            ? bridge.cancel_project_collection() : bridge.collect_project_copy()
        Keys.onSpacePressed: bridge.collectionBusy
            ? bridge.cancel_project_collection() : bridge.collect_project_copy()
        Text {
            anchors.centerIn: parent
            text: bridge.collectionBusy ? "CANCEL COPY" : "COLLECT PROJECT COPY"
            color: bridge.collectionBusy ? host.textMain : Theme.onAccentText
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            onClicked: bridge.collectionBusy
                ? bridge.cancel_project_collection() : bridge.collect_project_copy()
        }
    }
    Text {
        x: Theme.spaceXl
        y: managedMediaPanel.collectionStatusY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: bridge.collectionStatus === ""
            ? "Copy the saved project and all media into your project library."
            : bridge.collectionStatus
        color: host.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        x: Theme.spaceXl
        y: managedMediaPanel.noteY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: "Managed repair and external relink both require the exact saved audio identity."
        color: host.textDim
        font.pixelSize: Theme.typeCaption
    }
}
