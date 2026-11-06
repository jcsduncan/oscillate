pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: recovery
    required property var target
    required property string trackId
    required property var entries
    required property string status
    required property bool busy
    required property bool editable
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    required property color warningColor
    signal closeRequested()
    // Layout: title and close at the top, then a scrolling column of rows.
    readonly property int buttonHeight: 27
    readonly property int titleX: 11
    readonly property int titleY: 9
    readonly property int titleRightRoom: 51
    readonly property int closeRight: 36
    readonly property int closeY: 5
    readonly property int bodyInset: 9
    readonly property int bodyY: 37
    readonly property int bodyBottomRoom: 45
    readonly property int stateY: 19
    readonly property int bundleY: 37
    readonly property int controlsY: 60
    readonly property int scanWidth: 77
    readonly property int pathX: 84
    readonly property int pathInsetX: 6
    readonly property int pathInsetY: 5
    readonly property int statusY: 92
    readonly property int statusHeight: 38
    readonly property int matchesY: 137
    readonly property int noMatchY: 157
    readonly property int candidatesY: 181
    readonly property int candidatePitch: 45
    readonly property int contentBaseHeight: 183

    readonly property var candidates: entries.filter(entry =>
        entry.state === "ready"
            && entry.kind === (target.slotId ? "effect" : "instrument")
            && entry.classId === target.classId
            && entry.vendor === target.vendor
            && entry.name === target.name
            && entry.version === target.version
            && entry.sha256 === target.sha256)

    color: panelColor
    border.color: lineColor
    radius: Theme.radiusLg

    component ActionButton: Rectangle {
        required property string label
        required property string actionName
        signal triggered()
        height: recovery.buttonHeight
        radius: Theme.radiusMd
        color: enabled ? recovery.raisedColor : recovery.panelColor
        border.color: activeFocus ? recovery.accentColor : recovery.lineColor
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: actionName
        Keys.onReturnPressed: triggered()
        Keys.onSpacePressed: triggered()
        Text {
            anchors.centerIn: parent
            text: parent.label
            color: parent.enabled ? recovery.textColor : recovery.dimColor
            font.pixelSize: Theme.typeSmall
            font.bold: true
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            enabled: parent.enabled
            onClicked: parent.triggered()
        }
    }

    Text {
        x: recovery.titleX
        y: recovery.titleY
        width: parent.width - recovery.titleRightRoom
        elide: Text.ElideRight
        text: "RELINK SAVED VST3  ·  " + (recovery.target.name || "Plugin")
        color: recovery.textColor
        font.pixelSize: Theme.typeBody
        font.bold: true
    }
    ActionButton {
        objectName: "vst3RecoveryClose"
        x: parent.width - recovery.closeRight
        y: recovery.closeY
        width: recovery.buttonHeight
        label: "×"
        actionName: "Close VST3 recovery"
        onTriggered: recovery.closeRequested()
    }
    Flickable {
        x: recovery.bodyInset
        y: recovery.bodyY
        width: parent.width - 2 * recovery.bodyInset
        height: parent.height - recovery.bodyBottomRoom
        clip: true
        contentWidth: width
        contentHeight: recovery.contentBaseHeight + recovery.candidates.length * recovery.candidatePitch
        boundsBehavior: Flickable.StopAtBounds
        Text {
            x: Theme.spaceXxs
            y: 0
            width: parent.width - 2 * Theme.spaceXxs
            elide: Text.ElideRight
            text: recovery.target.vendor + "  ·  " + recovery.target.version
            color: recovery.dimColor
            font.pixelSize: Theme.typeCaption
        }
        Text {
            x: Theme.spaceXxs
            y: recovery.stateY
            width: parent.width - 2 * Theme.spaceXxs
            elide: Text.ElideRight
            text: (recovery.target.missing ? "SAVED MISSING" : "SAVED INSTANCE")
                + "  ·  " + (recovery.target.stateBytes || 0) + " state bytes"
                + "  ·  " + (recovery.target.parameterCount || 0) + " parameter refs"
            color: recovery.warningColor
            font.pixelSize: Theme.typeCaption
        }
        Text {
            x: Theme.spaceXxs
            y: recovery.bundleY
            width: parent.width - 2 * Theme.spaceXxs
            elide: Text.ElideMiddle
            text: "Saved bundle: " + (recovery.target.bundle || "")
            color: recovery.dimColor
            font.pixelSize: Theme.typeCaption
        }
        ActionButton {
            objectName: "vst3RecoveryScan"
            x: Theme.spaceXxs
            y: recovery.controlsY
            width: recovery.scanWidth
            label: recovery.busy ? "SCANNING" : "SCAN"
            actionName: "Scan VST3 bundles for exact saved binary"
            enabled: recovery.editable && !recovery.busy
            onTriggered: projectBridge.vst3Browser.refresh_vst3_catalogue()
        }
        Rectangle {
            x: recovery.pathX
            y: recovery.controlsY
            width: parent.width - recovery.pathX - Theme.spaceXxs
            height: recovery.buttonHeight
            radius: Theme.radiusMd
            color: recovery.raisedColor
            border.color: recovery.lineColor
            TextInput {
                id: searchPath
                objectName: "vst3RecoveryPath"
                x: recovery.pathInsetX
                y: recovery.pathInsetY
                width: parent.width - 2 * recovery.pathInsetX
                color: recovery.textColor
                font.pixelSize: Theme.typeSmall
                clip: true
                activeFocusOnTab: true
                Accessible.name: "Additional VST3 bundle or folder path"
                Keys.onReturnPressed: {
                    if (recovery.editable && !recovery.busy && text.length > 0)
                        projectBridge.vst3Browser.add_vst3_search_path(text)
                }
            }
            Text {
                x: recovery.pathInsetX
                y: recovery.pathInsetY
                visible: searchPath.text.length === 0 && !searchPath.activeFocus
                text: "Absolute bundle or folder path; press Enter"
                color: recovery.dimColor
                font.pixelSize: Theme.typeCaption
            }
        }
        Text {
            objectName: "vst3RecoveryStatus"
            x: Theme.spaceXxs
            y: recovery.statusY
            width: parent.width - 2 * Theme.spaceXxs
            height: recovery.statusHeight
            wrapMode: Text.WordWrap
            text: recovery.status
            color: recovery.dimColor
            font.pixelSize: Theme.typeCaption
        }
        Text {
            x: Theme.spaceXxs
            y: recovery.matchesY
            text: "EXACT BINARY MATCHES  ·  " + recovery.candidates.length
            color: recovery.accentColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        Text {
            x: Theme.spaceXxs
            y: recovery.noMatchY
            width: parent.width - 2 * Theme.spaceXxs
            visible: recovery.candidates.length === 0
            text: recovery.target.sha256
                ? "No match yet. Scan or add the original VST3 bundle."
                : "Saved binary identity is unavailable; exact relink cannot be verified."
            color: recovery.dimColor
            font.pixelSize: Theme.typeCaption
        }
        Repeater {
            model: recovery.candidates
            delegate: ActionButton {
                required property var modelData
                required property int index
                objectName: "vst3RecoveryCandidate"
                x: Theme.spaceXxs
                y: recovery.candidatesY + index * recovery.candidatePitch
                width: parent.width - 2 * Theme.spaceXxs
                label: "RELINK  ·  " + modelData.bundle.split("/").pop()
                actionName: "Relink exact saved binary at " + modelData.bundle
                enabled: recovery.editable && !recovery.busy
                onTriggered: projectBridge.vst3Browser.relink_vst3_plugin(
                    recovery.trackId, recovery.target.slotId || "", modelData.index)
            }
        }
    }
}
