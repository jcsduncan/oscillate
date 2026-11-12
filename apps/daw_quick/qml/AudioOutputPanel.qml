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
    // The render-ahead setting below the buffer (ADR-0128).
    readonly property int renderAheadHeight: 124
    // ALSA direct (ADR-0130): the period count and the measured latency.
    readonly property int alsaPeriodsHeight: 92
    // The gap kept between the panel and the window's bottom edge.
    readonly property int windowBottomMargin: Theme.spaceMd
    readonly property string backend: bridge.audioOutput.audioBackend
    readonly property bool alsa: backend === "alsa"
    readonly property bool listsDevices: backend === "pipewire" || alsa
    readonly property var devices: alsa ? bridge.audioOutput.alsaDevices : bridge.audioOutput.audioDevices
    readonly property var backends: ["pipewire"].concat(bridge.audioOutput.jackPlaybackAvailable ? ["jack"] : [])
                                                .concat(bridge.audioOutput.alsaPlaybackAvailable ? ["alsa"] : [])
    function deviceKey(entry) { return alsa ? entry.id : entry.serial }
    function selectDevice(entry) {
        if (alsa)
            bridge.audioOutput.select_alsa_device(entry.id)
        else
            bridge.audioOutput.select_audio_device(entry.serial)
        host.audioMenuOpen = false
    }
    z: 5
    anchors.right: parent.right
    anchors.rightMargin: panelRightMargin
    width: panelWidth
    readonly property int alsaExtraHeight: alsa ? alsaPeriodsHeight + (alsaFallbackReason.visible ? alsaFallbackReason.implicitHeight : 0) : 0
    readonly property int naturalHeight: renderAheadHeight + (backend === "jack" ? jackPanelHeight : Math.min(panelMaxHeight + alsaExtraHeight, panelBaseHeight + alsaExtraHeight + devices.length * deviceRowHeight))
    // A short window scrolls the settings; the refresh control stays at the bottom.
    height: Math.min(naturalHeight, parent.height - y - windowBottomMargin)
    readonly property int refreshReserve: listsDevices ? refreshHeight + 2 * contentX : 0
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.line
    function close() { host.audioMenuOpen = false }
    // Scrolls a Flickable so a focused control inside it is in view.
    function reveal(flickable, item) {
        let inside = false
        for (let ancestor = item; ancestor; ancestor = ancestor.parent)
            inside = inside || ancestor === flickable.contentItem
        if (!inside)
            return
        const top = item.mapToItem(flickable.contentItem, 0, 0).y
        const bottom = top + item.height
        if (top < flickable.contentY)
            flickable.contentY = top
        else if (bottom > flickable.contentY + flickable.height)
            flickable.contentY = bottom - flickable.height
    }
    Connections {
        target: audioOutputPanel.Window.window
        function onActiveFocusItemChanged() {
            const item = audioOutputPanel.Window.activeFocusItem
            if (!audioOutputPanel.visible || !item)
                return
            audioOutputPanel.reveal(deviceList, item)
            audioOutputPanel.reveal(panelBody, item)
        }
    }
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
        // Above the scrolling body, which would otherwise take its clicks.
        z: 1
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
    Flickable {
        id: panelBody
        objectName: "audioOutputPanelBody"
        anchors.fill: parent
        anchors.bottomMargin: audioOutputPanel.refreshReserve
        contentWidth: width
        contentHeight: audioOutputPanel.naturalHeight - audioOutputPanel.refreshReserve
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        // A taller window shows more; keep the content within bounds.
        onHeightChanged: contentY = Math.max(0, Math.min(contentY, contentHeight - height))
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
                model: audioOutputPanel.backends
                delegate: Rectangle {
                    required property string modelData
                    readonly property bool selected: modelData === bridge.audioOutput.audioBackend
                    objectName: "audioBackendChoice"
                    width: (audioOutputPanel.contentWidth - backendChoices.spacing * (audioOutputPanel.backends.length - 1)) / audioOutputPanel.backends.length
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
            height: audioOutputPanel.listsDevices ? Math.min(audioOutputPanel.deviceListMaxHeight, audioOutputPanel.devices.length * audioOutputPanel.deviceRowHeight) : 0
            visible: audioOutputPanel.listsDevices
            contentWidth: width
            contentHeight: deviceColumn.height
            clip: true
            Column {
                id: deviceColumn
                width: parent.width
                Repeater {
                    model: audioOutputPanel.devices
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool selected: audioOutputPanel.deviceKey(modelData)
                                                         === (audioOutputPanel.alsa ? bridge.audioOutput.selectedAlsaDevice : bridge.audioOutput.selectedAudioDevice)
                        objectName: "audioOutputChoice"
                        width: deviceColumn.width
                        height: audioOutputPanel.deviceRowHeight
                        radius: Theme.controlRadius
                        color: selected ? Theme.choiceSelectedFill : host.panelRaised
                        border.color: activeFocus ? host.accent : "transparent"
                        activeFocusOnTab: bridge.audioOutput.audioDeviceSelectionEnabled
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.name + (selected ? ", selected" : "")
                        Keys.onReturnPressed: audioOutputPanel.selectDevice(modelData)
                        Keys.onSpacePressed: audioOutputPanel.selectDevice(modelData)
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
                            onClicked: audioOutputPanel.selectDevice(modelData)
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
                text: audioOutputPanel.backend === "jack" ? "GRAPH PREPARATION (FRAMES)"
                    : audioOutputPanel.alsa ? "PERIOD (FRAMES)" : "REQUESTED BUFFER (FRAMES)"
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
                        Accessible.name: modelData + (audioOutputPanel.alsa ? " frame period" : " frame buffer request")
                                         + (selected ? ", selected" : "")
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
            Text {
                visible: audioOutputPanel.alsa
                topPadding: Theme.spaceMd
                text: "PERIODS"
                color: host.textMain
                font.pixelSize: Theme.typeSmall
                font.bold: true
            }
            Row {
                id: periodChoices
                visible: audioOutputPanel.alsa
                width: parent.width
                spacing: Theme.spaceSm
                Repeater {
                    model: bridge.audioOutput.alsaPeriodChoices
                    delegate: Rectangle {
                        required property int modelData
                        readonly property bool selected: modelData === bridge.audioOutput.alsaPeriods
                        objectName: "alsaPeriodChoice"
                        width: (periodChoices.width - periodChoices.spacing * (bridge.audioOutput.alsaPeriodChoices.length - 1))
                               / bridge.audioOutput.alsaPeriodChoices.length
                        height: audioOutputPanel.bufferChoiceHeight
                        radius: Theme.controlRadius
                        color: selected ? Theme.choiceSelectedFill : host.panel
                        border.color: activeFocus ? host.accent : host.line
                        activeFocusOnTab: bridge.audioOutput.audioDeviceSelectionEnabled
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData + " periods" + (selected ? ", selected" : "")
                        Keys.onReturnPressed: bridge.audioOutput.select_alsa_periods(modelData)
                        Keys.onSpacePressed: bridge.audioOutput.select_alsa_periods(modelData)
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
                            onClicked: bridge.audioOutput.select_alsa_periods(modelData)
                        }
                    }
                }
            }
            Text {
                id: alsaFallbackReason
                objectName: "alsaFallbackReason"
                visible: audioOutputPanel.alsa && text.length > 0
                width: audioOutputPanel.contentWidth
                text: bridge.audioOutput.alsaFallback
                wrapMode: Text.WordWrap
                color: host.textMain
                font.pixelSize: Theme.typeCaption
            }
            Text {
                objectName: "alsaLatencyStatus"
                visible: audioOutputPanel.alsa
                width: audioOutputPanel.contentWidth
                text: bridge.audioOutput.latencyStatus
                wrapMode: Text.WordWrap
                color: host.textDim
                font.pixelSize: Theme.typeCaption
            }
            Text {
                topPadding: Theme.spaceMd
                text: "RENDER AHEAD"
                color: host.textMain
                font.pixelSize: Theme.typeSmall
                font.bold: true
            }
            Row {
                id: renderAheadChoices
                width: parent.width
                spacing: Theme.spaceSm
                Repeater {
                    model: bridge.audioOutput.renderAheadChoices
                    delegate: Rectangle {
                        required property int modelData
                        readonly property bool selected: modelData === bridge.audioOutput.renderAheadMs
                        readonly property string label: modelData === 0 ? "Off" : modelData + " ms"
                        objectName: "renderAheadChoice"
                        width: (renderAheadChoices.width - renderAheadChoices.spacing * (bridge.audioOutput.renderAheadChoices.length - 1))
                               / bridge.audioOutput.renderAheadChoices.length
                        height: audioOutputPanel.bufferChoiceHeight
                        radius: Theme.controlRadius
                        color: selected ? Theme.choiceSelectedFill : host.panel
                        border.color: activeFocus ? host.accent : host.line
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: "Render ahead " + label + (selected ? ", selected" : "")
                        Keys.onReturnPressed: bridge.audioOutput.select_render_ahead(modelData)
                        Keys.onSpacePressed: bridge.audioOutput.select_render_ahead(modelData)
                        Text {
                            anchors.centerIn: parent
                            text: parent.label
                            color: parent.selected ? host.accent : host.textMain
                            font.pixelSize: Theme.typeSmall
                        }
                        MouseArea {
                            Tip {}
                            anchors.fill: parent
                            onClicked: bridge.audioOutput.select_render_ahead(modelData)
                        }
                    }
                }
            }
            Text {
                objectName: "renderAheadStatus"
                width: audioOutputPanel.contentWidth
                text: bridge.audioOutput.renderAheadStatus
                wrapMode: Text.WordWrap
                color: host.textDim
                font.pixelSize: Theme.typeCaption
            }
        }
    }
    Rectangle {
        objectName: "refreshAudioOutputs"
        visible: audioOutputPanel.listsDevices
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
