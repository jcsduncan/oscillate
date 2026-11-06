pragma ComponentBehavior: Bound
import QtQuick
import "shell"

FloatingDeviceFrame {
    id: editor
    required property var bridge
    required property color panelColor
    required property color panelRaisedColor
    required property color lineColor
    required property color dimColor
    objectName: "vst3NativeEditorOverlay"
    // The plug-in's own window size, kept within these bounds, plus room
    // for the frame, the sidechain strip and the status line.
    readonly property int minSurfaceWidth: 680
    readonly property int maxSurfaceWidth: 1000
    readonly property int minSurfaceHeight: 480
    readonly property int maxSurfaceHeight: 800
    readonly property int frameWidthRoom: 26
    readonly property int frameHeightRoom: 106
    readonly property int sidechainRoom: 36
    readonly property int sidechainHeight: 30
    readonly property int sidechainRise: 64
    readonly property int statusRoom: 35
    readonly property int statusRise: 29
    visible: Boolean(bridge.vst3Editor.vst3NativeWindow)
    title: bridge.vst3Editor.vst3InsertEditor.name || "VST3 plugin"
    frameColor: panelColor
    borderColor: lineColor
    surfaceWidth: Math.max(minSurfaceWidth, Math.min(maxSurfaceWidth, bridge.vst3Editor.vst3InsertEditor.nativeWidth + frameWidthRoom))
    surfaceHeight: Math.max(minSurfaceHeight, Math.min(maxSurfaceHeight, bridge.vst3Editor.vst3InsertEditor.nativeHeight + frameHeightRoom
        + (sidechain.visible ? sidechainRoom : 0)))
    preferredX: Math.max(Theme.spaceLg, width - surfaceWidth - frameWidthRoom)
    onCloseRequested: bridge.vst3Editor.apply_vst3_insert_editor()

    Rectangle {
        width: parent.width
        height: parent.height - editor.statusRoom - (sidechain.visible ? editor.sidechainRoom : 0)
        color: Theme.pluginWindowFill
        border.color: editor.lineColor
        WindowContainer {
            objectName: "vst3NativeWindowContainer"
            anchors.fill: parent
            anchors.margins: Theme.hairline
            window: editor.bridge.vst3Editor.vst3NativeWindow
        }
    }
    // A VST3 effect's aux input, keyed from here like a built-in effect's.
    SidechainStrip {
        id: sidechain
        objectName: "vst3NativeSidechainStrip"
        x: Theme.spaceXs
        y: parent.height - editor.sidechainRise
        width: parent.width - 2 * Theme.spaceXs
        height: editor.sidechainHeight
        bridge: editor.bridge
        trackId: editor.bridge.vst3Editor.vst3InsertEditor.role === "effect"
            ? (editor.bridge.vst3Editor.vst3InsertEditor.trackId || "") : ""
        slotId: editor.bridge.vst3Editor.vst3InsertEditor.slotId || ""
        listenable: false
    }
    Text {
        x: Theme.spaceXs
        y: parent.height - editor.statusRise
        width: parent.width - 2 * Theme.spaceXs
        elide: Text.ElideRight
        text: editor.bridge.vst3Editor.vst3InsertEditor.liveStatus
              || editor.bridge.vst3Editor.vst3InsertEditor.message
              || "Changes save automatically; closing saves remaining state"
        color: editor.dimColor
        font.pixelSize: Theme.typeSmall
    }
}
