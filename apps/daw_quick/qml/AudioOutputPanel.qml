pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: audioOutputPanel
    transformOrigin: Item.Top
    Reveal { target: audioOutputPanel; fromScale: Motion.popScale }
    required property var host
    required property var bridge
    objectName: "audioOutputPanel"
    visible: bridge.projectOpen && host.audioMenuOpen
    readonly property int panelRightMargin: 18
    readonly property int panelWidth: 300
    readonly property int jackPanelHeight: 290
    readonly property int panelMaxHeight: 450
    readonly property int panelBaseHeight: 276
    readonly property int deviceRowHeight: 36
    readonly property int deviceListMaxHeight: 190
    readonly property int titleX: 14
    readonly property int titleY: 13
    readonly property int statusY: 34
    readonly property int statusHeight: 28
    readonly property int contentX: 10
    readonly property int contentWidth: panelWidth - 2 * contentX
    readonly property int backendY: 66
    readonly property int backendHeight: 29
    readonly property int deviceListGap: 7
    readonly property int bufferSpacing: 5
    readonly property int bufferChoiceHeight: 28
    readonly property int refreshHeight: 32
    z: 5
    anchors.right: parent.right
    anchors.rightMargin: panelRightMargin
    width: panelWidth
    height: bridge.audioOutput.audioBackend === "jack" ? jackPanelHeight : Math.min(panelMaxHeight, panelBaseHeight + bridge.audioOutput.audioDevices.length * deviceRowHeight)
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.line
    function close() { host.audioMenuOpen = false }
    // Escape, a click anywhere else, or × closes it.
    onVisibleChanged: if (visible) forceActiveFocus()
    Keys.onEscapePressed: close()
    MouseArea {
        parent: audioOutputPanel.parent
        anchors.fill: parent
        z: audioOutputPanel.z - 0.01
        visible: audioOutputPanel.visible
        onPressed: audioOutputPanel.close()
    }
    MouseArea { anchors.fill: parent }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        y: Theme.dialogCloseY
        text: "×"
        color: audioOutputPanelCloseArea.containsMouse ? audioOutputPanel.host.textMain : audioOutputPanel.host.textDim
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        font.pixelSize: Theme.typeDisplay
        Accessible.role: Accessible.Button
        Accessible.name: "Close audio output"
        MouseArea {
            Tip {}
            id: audioOutputPanelCloseArea
            anchors.fill: parent
            anchors.margins: -Theme.closeHitSlop
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: audioOutputPanel.close()
        }
    }
    Text {
        x: audioOutputPanel.titleX
        y: audioOutputPanel.titleY
        text: "AUDIO OUTPUT · " + bridge.audioOutput.audioBackend.toUpperCase()
        color: host.textMain
        font.pixelSize: Theme.typeSmall
        font.bold: true
        font.letterSpacing: Theme.trackingLg
    }
    Text {
        x: audioOutputPanel.titleX
        y: audioOutputPanel.statusY
        width: parent.width - 2 * audioOutputPanel.titleX
        height: audioOutputPanel.statusHeight
        text: bridge.audioOutput.audioDeviceStatus
        color: host.textDim
        font.pixelSize: Theme.typeSmall
        wrapMode: Text.WordWrap
        elide: Text.ElideRight
    }
    Row {
        id: backendChoices
        x: audioOutputPanel.contentX
        y: audioOutputPanel.backendY
        spacing: Theme.spaceSm
        Repeater {
            model: bridge.audioOutput.jackPlaybackAvailable ? ["pipewire", "jack"] : ["pipewire"]
            delegate: Rectangle {
                required property string modelData
                readonly property bool selected: modelData === bridge.audioOutput.audioBackend
                objectName: "audioBackendChoice"
                width: bridge.audioOutput.jackPlaybackAvailable ? (audioOutputPanel.contentWidth - backendChoices.spacing) / 2 : audioOutputPanel.contentWidth
                height: audioOutputPanel.backendHeight
                radius: Theme.controlRadius
                color: selected ? Theme.choiceSelectedFill : host.panel
                border.color: activeFocus ? host.accent : host.line
                activeFocusOnTab: bridge.audioOutput.audioDeviceSelectionEnabled
                Accessible.role: Accessible.Button
                Accessible.name: modelData.toUpperCase() + " audio backend" + (selected ? ", selected" : "")
                Keys.onReturnPressed: bridge.audioOutput.select_audio_backend(modelData)
                Keys.onSpacePressed: bridge.audioOutput.select_audio_backend(modelData)
                Text {
                    anchors.centerIn: parent
                    text: parent.modelData.toUpperCase()
                    color: parent.selected ? host.accent : host.textMain
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    enabled: bridge.audioOutput.audioDeviceSelectionEnabled
                    onClicked: bridge.audioOutput.select_audio_backend(modelData)
                }
            }
        }
    }
    Flickable {
        id: deviceList
        x: audioOutputPanel.contentX
        y: backendChoices.y + backendChoices.height + audioOutputPanel.deviceListGap
        width: parent.width - 2 * audioOutputPanel.contentX
        height: bridge.audioOutput.audioBackend === "pipewire" ? Math.min(audioOutputPanel.deviceListMaxHeight, bridge.audioOutput.audioDevices.length * audioOutputPanel.deviceRowHeight) : 0
        visible: bridge.audioOutput.audioBackend === "pipewire"
        contentWidth: width
        contentHeight: deviceColumn.height
        clip: true
        Column {
            id: deviceColumn
            width: parent.width
            Repeater {
                model: bridge.audioOutput.audioDevices
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool selected: modelData.serial === bridge.audioOutput.selectedAudioDevice
                    objectName: "audioOutputChoice"
                    width: deviceColumn.width
                    height: audioOutputPanel.deviceRowHeight
                    radius: Theme.controlRadius
                    color: selected ? Theme.choiceSelectedFill : host.panelRaised
                    border.color: activeFocus ? host.accent : "transparent"
                    activeFocusOnTab: bridge.audioOutput.audioDeviceSelectionEnabled
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.name + (selected ? ", selected" : "")
                    Keys.onReturnPressed: {
                        bridge.audioOutput.select_audio_device(modelData.serial)
                        host.audioMenuOpen = false
                    }
                    Keys.onSpacePressed: {
                        bridge.audioOutput.select_audio_device(modelData.serial)
                        host.audioMenuOpen = false
                    }
                    Text {
                        x: audioOutputPanel.contentX
                        width: parent.width - 2 * audioOutputPanel.contentX
                        anchors.verticalCenter: parent.verticalCenter
                        text: (parent.selected ? "✓  " : "     ") + parent.modelData.name
                        color: parent.selected ? host.accent : host.textMain
                        font.pixelSize: Theme.typeBody
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        Tip {}
                        anchors.fill: parent
                        enabled: bridge.audioOutput.audioDeviceSelectionEnabled
                        onClicked: {
                            bridge.audioOutput.select_audio_device(modelData.serial)
                            host.audioMenuOpen = false
                        }
                    }
                }
            }
        }
    }
    Column {
        x: audioOutputPanel.contentX
        y: deviceList.y + deviceList.height + Theme.spaceMd
        width: parent.width - 2 * audioOutputPanel.contentX
        spacing: audioOutputPanel.bufferSpacing
        Text {
            text: bridge.audioOutput.audioBackend === "jack" ? "GRAPH PREPARATION (FRAMES)" : "REQUESTED BUFFER (FRAMES)"
            color: host.textMain
            font.pixelSize: Theme.typeSmall
            font.bold: true
        }
        Row {
            id: bufferChoices
            width: parent.width
            spacing: Theme.spaceSm
            Repeater {
                model: bridge.audioOutput.bufferFrameChoices
                delegate: Rectangle {
                    required property int modelData
                    readonly property bool selected: modelData === bridge.audioOutput.requestedBufferFrames
                    objectName: "audioBufferChoice"
                    width: (bufferChoices.width - bufferChoices.spacing * (bridge.audioOutput.bufferFrameChoices.length - 1))
                           / bridge.audioOutput.bufferFrameChoices.length
                    height: audioOutputPanel.bufferChoiceHeight
                    radius: Theme.controlRadius
                    color: selected ? Theme.choiceSelectedFill : host.panel
                    border.color: activeFocus ? host.accent : host.line
                    activeFocusOnTab: bridge.audioOutput.audioDeviceSelectionEnabled
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData + " frame buffer request" + (selected ? ", selected" : "")
                    Keys.onReturnPressed: bridge.audioOutput.select_buffer_frames(modelData)
                    Keys.onSpacePressed: bridge.audioOutput.select_buffer_frames(modelData)
                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: parent.selected ? host.accent : host.textMain
                        font.pixelSize: Theme.typeSmall
                    }
                    MouseArea {
                        Tip {}
                        anchors.fill: parent
                        enabled: bridge.audioOutput.audioDeviceSelectionEnabled
                        onClicked: bridge.audioOutput.select_buffer_frames(modelData)
                    }
                }
            }
        }
        Text {
            width: audioOutputPanel.contentWidth
            text: bridge.audioOutput.bufferSizeStatus
            wrapMode: Text.WordWrap
            color: host.textDim
            font.pixelSize: Theme.typeCaption
        }
    }
    Rectangle {
        objectName: "refreshAudioOutputs"
        visible: bridge.audioOutput.audioBackend === "pipewire"
        x: audioOutputPanel.contentX
        anchors.bottom: parent.bottom
        anchors.bottomMargin: audioOutputPanel.contentX
        width: parent.width - 2 * audioOutputPanel.contentX
        height: audioOutputPanel.refreshHeight
        radius: Theme.radiusLg
        color: host.panel
        border.color: activeFocus ? host.accent : host.line
        activeFocusOnTab: !bridge.audioOutput.audioDeviceBusy
        Accessible.role: Accessible.Button
        Accessible.name: "Refresh audio outputs"
        Keys.onReturnPressed: bridge.audioOutput.refresh_audio_devices()
        Keys.onSpacePressed: bridge.audioOutput.refresh_audio_devices()
        Text {
            anchors.centerIn: parent
            text: bridge.audioOutput.audioDeviceBusy ? "FINDING OUTPUTS…" : "REFRESH OUTPUTS"
            color: host.textMain
            font.pixelSize: Theme.typeSmall
            font.bold: true
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            enabled: !bridge.audioOutput.audioDeviceBusy
            onClicked: bridge.audioOutput.refresh_audio_devices()
        }
    }
}
