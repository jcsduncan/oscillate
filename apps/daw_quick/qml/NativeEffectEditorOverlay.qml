pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// A built-in effect's designed panel (ADR-0043), floating over the device
// chain. `controller` is the slot's NativeSynthDeviceController; `automation`
// names its lane on the arrangement (absent on the master, which owns no
// lanes yet); `context` carries {trackName, keyName, sampleRate} for panels that name
// their inputs; `panelModel` is the open panel's own model (Relay's echo
// train), null for effects without one. The panel is looked up by engine id
// in the device catalogue (controller.panel_url).
FloatingDeviceFrame {
    id: editor
    objectName: "nativeEffectEditorOverlay"
    required property var bridge
    required property var controller
    // {trackId, laneKey} of the device on the arrangement, or null.
    required property var automation
    // Show the lane `key` of track `trackId` on the arrangement.
    signal automationRequested(string trackId, string key)
    required property var context
    required property var panelModel
    readonly property bool hasPanel: controller !== null && controller.panel_url !== ""
    readonly property real panelWidth: controller !== null ? controller.panel_width : 0
    readonly property real panelHeight: controller !== null ? controller.panel_height : 0
    // Floating (the default): a movable window. Docked: the panel spans the
    // bottom of the window over the device chain.
    property bool docked: false
    readonly property real dockHeight: Math.round(height * Theme.editorDockShare)
    // Room an unresized floating window leaves at its sides.
    readonly property real floatingMarginWidth: 40
    // The panel draws at the panel zoom (ADR-0103; floating, of the default
    // scale), scaled down to fit the window.
    readonly property var layout: bridge ? bridge.workspaceLayout : null
    zoomable: hasPanel
    zoom: layout !== null ? layout.panelZoom : 1
    onZoomChosen: z => { if (layout !== null) layout.panelZoom = z }
    readonly property real panelScale: docked
        ? Math.min(zoom, (width - Theme.editorDockChromeWidth) / panelWidth,
                   (dockHeight - Theme.editorChromeHeight) / panelHeight)
        : Math.min(Theme.editorFloatingScale * zoom, (width - floatingMarginWidth) / panelWidth,
                   (height - Theme.editorFloatingMarginHeight) / panelHeight)
    title: controller ? controller.engine_name + " effect" : "Effect"
    surfaceObjectName: "nativeEffectFloatingFrame"
    dragObjectName: "nativeEffectDragHandle"
    surfaceWidth: docked ? width - Theme.editorChromeWidth : panelWidth * panelScale + Theme.editorChromeWidth
    surfaceHeight: docked ? dockHeight : panelHeight * panelScale + Theme.editorChromeHeight
    preferredX: docked ? Theme.editorDockedInset : -1
    preferredY: docked ? height - dockHeight - Theme.editorDockedBottomGap : Theme.editorFloatingTop
    visible: controller !== null
    onControllerChanged: {
        loadPanel()
    }
    // A panel that needs its model waits for it; once made, the panel
    // follows the model through its input binding.
    onPanelModelChanged: {
        if (panelLoader.item === null)
            loadPanel()
    }

    // What a panel may take from its host beyond the controller, by the
    // names the device catalogue lists for it (device_panels.cpp).
    QtObject {
        id: host
        readonly property var relay: editor.panelModel
        readonly property bool keyed: editor.context.keyed === true
        readonly property string keyName: editor.context && editor.context.keyName
            ? editor.context.keyName : "the sidechain"
        readonly property string trackName: editor.context && editor.context.trackName
            ? editor.context.trackName : "The track"
        readonly property real sampleRate: editor.context && editor.context.sampleRate
            ? editor.context.sampleRate : editor.defaultSampleRate
    }
    // Scope's rate before the context names one.
    readonly property real defaultSampleRate: 48000

    // A panel whose inputs include a model waits for it.
    function inputsReady() {
        return !controller.panel_inputs.includes("relay") || panelModel !== null
    }
    // Reads the controller itself: it runs from the controller's change
    // handler, before bindings on it (hasPanel) have caught up.
    function loadPanel() {
        if (controller === null || controller.panel_url === "" || !inputsReady()) {
            panelLoader.source = ""
            return
        }
        const initial = { controller: editor.controller }
        for (const name of controller.panel_inputs)
            initial[name] = host[name]
        panelLoader.setSource(controller.panel_url, initial)
    }

    Loader {
        id: panelLoader
        objectName: "nativeEffectPanelLoader"
        x: editor.docked ? Math.max(0, (parent.width - editor.panelWidth * editor.panelScale) / 2) : 0
        width: editor.panelWidth
        height: editor.panelHeight
        scale: editor.panelScale
        transformOrigin: Item.TopLeft
        // Inputs follow the host after the panel is made.
        onLoaded: {
            for (const name of editor.controller.panel_inputs)
                item[name] = Qt.binding(() => host[name])
        }
    }
    // An effect the catalogue has no panel for says so.
    Text {
        objectName: "nativeEffectMissingPanel"
        visible: editor.controller !== null && !editor.hasPanel
        anchors.centerIn: parent
        text: editor.controller !== null
            ? "No panel for the " + editor.controller.engine_name + " effect" : ""
        color: editor.textColor
        font.pixelSize: Theme.typeLabel
    }

    // The effect's key, chosen here on the effect, for those that read one.
    SidechainStrip {
        objectName: "nativeEffectSidechainStrip"
        x: 0
        y: parent.height - height
        width: Math.max(0, parent.width - Theme.editorSidechainRightGap)
        height: Theme.editorSidechainHeight
        z: 3
        bridge: editor.bridge
        trackId: editor.context && editor.context.pathId && editor.context.pathId !== "master"
            ? editor.context.pathId : ""
        slotId: editor.context && editor.context.slotId ? editor.context.slotId : ""
        accentColor: editor.accentColor
    }

    Rectangle {
        objectName: "nativeEffectDockToggle"
        x: Math.max(0, parent.width - width - (automationToggle.visible ? Theme.editorButtonGap : 0))
        y: parent.height - height
        width: Theme.editorDockButtonWidth; height: Theme.editorButtonHeight; radius: Theme.editorButtonRadius
        color: dockArea.containsMouse ? Theme.editorButtonHover : Theme.editorButton
        border.color: activeFocus ? editor.accentColor : Theme.editorButtonEdge
        activeFocusOnTab: editor.visible
        Accessible.role: Accessible.Button
        Accessible.name: editor.docked ? "Float the effect panel" : "Dock the effect panel"
        Keys.onReturnPressed: editor.docked = !editor.docked
        Keys.onSpacePressed: editor.docked = !editor.docked
        Text { anchors.centerIn: parent; text: editor.docked ? "FLOAT" : "DOCK"; color: Theme.editorButtonInk; font.pixelSize: Theme.typeSmall }
        MouseArea {
            id: dockArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: editor.docked = !editor.docked
        }
    }

    // Automation is drawn on the arrangement (ADR-0051): this opens the
    // device's lane there, on its track.
    Rectangle {
        id: automationToggle
        objectName: "nativeEffectShowAutomation"
        visible: editor.automation !== null
        x: Math.max(0, parent.width - width)
        y: parent.height - height
        width: Theme.editorAutomationButtonWidth; height: Theme.editorButtonHeight; radius: Theme.editorButtonRadius
        color: automationArea.containsMouse ? Theme.editorButtonHover : Theme.editorButton
        border.color: activeFocus ? editor.accentColor : Theme.editorButtonEdge
        activeFocusOnTab: editor.visible && visible
        Accessible.role: Accessible.Button
        Accessible.name: "Show this effect's automation on the arrangement"
        function activate() {
            if (editor.automation !== null && editor.automation.laneKey.length > 0)
                editor.automationRequested(editor.automation.trackId, editor.automation.laneKey)
        }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "AUTOMATE"; color: Theme.editorButtonInk; font.pixelSize: Theme.typeSmall }
        MouseArea {
            Tip {}
            id: automationArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: automationToggle.activate()
        }
    }
}
