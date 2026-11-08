pragma ComponentBehavior: Bound
import QtQuick
import "shell"
import "nova/NovaContract.js" as Contract

FloatingDeviceFrame {
    id: editor
    objectName: "nativeSynthEditorOverlay"
    required property var bridge
    required property var controller
    required property var automation
    // The open panel's own model (bridge.nativeSynthPanelModel: Anvil's
    // waveform preview, Knock's pads), null for engines without one.
    property var panelModel: null
    // A reflowing panel names its own designed size.
    readonly property real panelWidth: reflowPanel !== null ? reflowPanel.designedWidth
        : controller !== null ? controller.panel_width : 0
    readonly property real panelHeight: reflowPanel !== null ? reflowPanel.designedHeight
        : controller !== null ? controller.panel_height : 0
    // Floating (the default): a movable, resizable window. Docked: the panel
    // spans the bottom of the window over the device chain.
    property bool docked: false
    readonly property real dockHeight: Math.round(height * Theme.editorDockShare)
    // The window's chrome around the panel: the frame's insets, and its
    // title bar with the DOCK and AUTOMATION row under the panel.
    readonly property real frameChromeWidth: Theme.editorChromeWidth
    readonly property real frameChromeHeight: Theme.editorChromeHeight
    readonly property real dockChromeWidth: Theme.editorDockChromeWidth
    // Room an unresized floating window leaves around itself.
    readonly property real floatingMarginWidth: 2 * windowMargin + frameChromeWidth
    readonly property real floatingMarginHeight: Theme.editorFloatingMarginHeight
    // An unresized floating panel of fixed size draws at most this large.
    readonly property real fixedFloatingScale: Theme.editorFloatingScale
    readonly property real minimumWindowWidth: Theme.editorMinimumWidth
    readonly property real minimumWindowHeight: Theme.editorMinimumHeight
    // Where workspace preferences live (the window size, Nova's zoom).
    readonly property var layout: bridge ? bridge.workspaceLayout : null
    // The floating window's size, once dragged (0 until then).
    property real floatingWidth: 0
    property real floatingHeight: 0
    readonly property bool resized: floatingWidth > 0 && floatingHeight > 0
    readonly property bool hasPanel: controller !== null && controller.panel_url !== ""
    // Nova reflows (ADR-0076): it is laid out at the area it gets over its
    // zoom, scaled down only below its minimum. Other panels keep their
    // designed size and scale to fit.
    readonly property bool reflows: controller !== null && controller.panel_reflows
    readonly property var reflowPanel: reflows ? panelLoader.item : null
    readonly property real zoom: reflowPanel !== null ? reflowPanel.zoom : 1
    readonly property real minimumWidth: reflowPanel !== null ? reflowPanel.minimumWidth : panelWidth
    readonly property real minimumHeight: reflowPanel !== null ? reflowPanel.minimumHeight : panelHeight
    // The area the panel draws in, in window pixels.
    readonly property real areaWidth: Math.max(1, docked ? width - dockChromeWidth
        : resized ? Math.min(floatingWidth, width - 2 * windowMargin) - frameChromeWidth
        : reflows ? Math.min(panelWidth * zoom, width - floatingMarginWidth)
        : width - floatingMarginWidth)
    readonly property real areaHeight: Math.max(1, docked ? dockHeight - frameChromeHeight
        : resized ? Math.min(floatingHeight, height - 2 * windowMargin) - frameChromeHeight
        : reflows ? Math.min(panelHeight * zoom, height - floatingMarginHeight)
        : height - floatingMarginHeight)
    readonly property real panelScale: Math.max(minimumPanelScale, reflows
        ? Math.min(zoom, areaWidth / minimumWidth, areaHeight / minimumHeight)
        : Math.min(docked ? 1 : resized ? Number.POSITIVE_INFINITY : fixedFloatingScale,
                   areaWidth / panelWidth, areaHeight / panelHeight))
    // The panel's own size: a reflowing panel fills the area.
    readonly property real logicalWidth: reflows ? areaWidth / panelScale : panelWidth
    readonly property real logicalHeight: reflows ? areaHeight / panelScale : panelHeight
    // A panel is never drawn smaller than this.
    readonly property real minimumPanelScale: 0.01
    // A reflowing panel's window takes the panel's own chrome (Nova's
    // neutral graphite, ADR-0076); other panels keep the frame's look.
    readonly property var chrome: reflowPanel !== null ? reflowPanel.theme : null
    // Anvil's audition hit: the kick on C1.
    readonly property int kickKey: 36
    // The note the typing keyboard's A key plays (its C), for Nova's key
    // strip: 4 is middle C.
    readonly property var computerKeyboard: bridge ? bridge.computerKeyboard : null
    readonly property int keyboardBase: computerKeyboard !== null
        ? Contract.semitonesPerOctave * (computerKeyboard.octave + 1) : Contract.defaultKeyBase
    title: controller ? controller.engine_id + " instrument" : "Instrument"
    Binding { target: editor; property: "frameColor"; value: editor.chrome ? editor.chrome.plateMid : "transparent"; when: editor.chrome !== null }
    Binding { target: editor; property: "borderColor"; value: editor.chrome ? editor.chrome.line2 : "transparent"; when: editor.chrome !== null }
    Binding { target: editor; property: "titleBarColor"; value: editor.chrome ? editor.chrome.card : "transparent"; when: editor.chrome !== null }
    Binding { target: editor; property: "textColor"; value: editor.chrome ? editor.chrome.text : "transparent"; when: editor.chrome !== null }
    Binding { target: editor; property: "accentColor"; value: editor.chrome ? editor.chrome.text : "transparent"; when: editor.chrome !== null }
    Binding { target: editor; property: "buttonColor"; value: editor.chrome ? editor.chrome.card2 : "transparent"; when: editor.chrome !== null }
    Binding { target: editor; property: "buttonHoverColor"; value: editor.chrome ? editor.chrome.hover : "transparent"; when: editor.chrome !== null }
    Binding { target: editor; property: "buttonBorderColor"; value: editor.chrome ? editor.chrome.line2 : "transparent"; when: editor.chrome !== null }
    surfaceObjectName: "nativeSynthFloatingFrame"
    dragObjectName: "nativeSynthDragHandle"
    surfaceWidth: docked ? width - frameChromeWidth
        : resized ? floatingWidth : logicalWidth * panelScale + frameChromeWidth
    surfaceHeight: docked ? dockHeight
        : resized ? floatingHeight : logicalHeight * panelScale + frameChromeHeight
    preferredX: docked ? Theme.editorDockedInset : -1
    preferredY: docked ? height - dockHeight - Theme.editorDockedBottomGap : Theme.editorFloatingTop
    resizable: !docked
    minimumSurfaceWidth: minimumWindowWidth
    minimumSurfaceHeight: minimumWindowHeight
    visible: controller !== null
    onControllerChanged: {
        automationDrawer.visible = false
        loadPanel()
    }
    // The size and zoom last chosen, from the workspace preferences.
    Component.onCompleted: {
        if (layout !== null) {
            floatingWidth = layout.instrumentWindowWidth
            floatingHeight = layout.instrumentWindowHeight
        }
    }
    onResizeRequested: (w, h) => {
        floatingWidth = w
        floatingHeight = h
    }
    onResizeFinished: {
        if (layout !== null) {
            layout.instrumentWindowWidth = Math.round(floatingWidth)
            layout.instrumentWindowHeight = Math.round(floatingHeight)
        }
    }

    // What a panel may take from its host beyond the controller, by the
    // names the device catalogue lists for it (device_panels.cpp).
    QtObject {
        id: host
        readonly property var anvil: editor.panelModel
        readonly property var knock: editor.panelModel
        readonly property var songScale: editor.bridge.projectScale
        readonly property real playheadBar: editor.bridge.playheadBar
        // Write to clip: the selected MIDI clip, or a new one at the playhead.
        readonly property var selectedClipIds: editor.bridge.selectedClipIds
        // Nova's zoom chip's choice, kept across sessions.
        readonly property real zoom: editor.layout !== null ? editor.layout.panelZoom : 1
        // Nova's key strip plays from the typing keyboard's octave.
        readonly property int keyBase: editor.keyboardBase
        // The groove pool, for Nova's arp GROOVE list (ADR-0078).
        readonly property var grooves: editor.bridge && editor.bridge.noteTools
            ? editor.bridge.noteTools.grooves : []
    }

    // Reads the controller itself: it runs from the controller's change
    // handler, before bindings on it (hasPanel) have caught up.
    function loadPanel() {
        if (controller === null || controller.panel_url === "") {
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
        objectName: "nativeSynthPanelHost"
        x: Math.max(0, (parent.width - width * scale) / 2)
        width: editor.logicalWidth
        height: editor.logicalHeight
        scale: editor.panelScale
        transformOrigin: Item.TopLeft
        // Inputs follow the host after the panel is made.
        onLoaded: {
            for (const name of editor.controller.panel_inputs)
                item[name] = Qt.binding(() => host[name])
        }
    }
    // What panels ask of the host: auditions and live notes on the open
    // synth's track (held until released), and Nova's zoom kept.
    Connections {
        target: panelLoader.item
        ignoreUnknownSignals: true
        function onPadTriggered(key) { editor.bridge.audition_native_synth_note(key) }
        function onHitRequested() { editor.bridge.audition_native_synth_note(editor.kickKey) }
        function onAuditionRequested() { editor.bridge.audition_knock_pattern() }
        function onKeyPressed(key, velocity) {
            editor.bridge.play_note(editor.bridge.nativeSynthTrackId, key, velocity)
        }
        function onKeyReleased(key) { editor.bridge.release_note(key) }
        function onPadPressed(key, velocity) {
            editor.bridge.play_note(editor.bridge.nativeSynthTrackId, key, velocity)
        }
        function onPadReleased(key) { editor.bridge.release_note(key) }
        function onZoomChanged() {
            if (editor.layout !== null)
                editor.layout.panelZoom = panelLoader.item.zoom
        }
    }
    // An engine the catalogue has no panel for says so.
    Text {
        objectName: "nativeSynthMissingPanel"
        visible: editor.controller !== null && !editor.hasPanel
        anchors.centerIn: parent
        text: editor.controller !== null
            ? "No panel for the " + editor.controller.engine_name + " instrument" : ""
        color: editor.textColor
        font.pixelSize: Theme.typeLabel
    }

    Rectangle {
        objectName: "nativeSynthDockToggle"
        x: Math.max(0, parent.width - width - Theme.editorButtonGap)
        y: parent.height - height
        width: Theme.editorDockButtonWidth; height: Theme.editorButtonHeight; radius: Theme.editorButtonRadius
        color: dockArea.containsMouse ? editor.buttonHoverColor : editor.buttonColor
        border.color: activeFocus ? editor.accentColor : editor.buttonBorderColor
        activeFocusOnTab: editor.visible
        Accessible.role: Accessible.Button
        Accessible.name: editor.docked ? "Float the instrument panel" : "Dock the instrument panel"
        Keys.onReturnPressed: editor.docked = !editor.docked
        Keys.onSpacePressed: editor.docked = !editor.docked
        Text { anchors.centerIn: parent; text: editor.docked ? "FLOAT" : "DOCK"; color: editor.textColor; font.pixelSize: Theme.typeSmall }
        MouseArea {
            id: dockArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: editor.docked = !editor.docked
        }
    }

    Rectangle {
        id: automationToggle
        objectName: "nativeSynthAutomationToggle"
        x: Math.max(0, parent.width - width)
        y: parent.height - height
        width: Theme.editorAutomationButtonWidth; height: Theme.editorButtonHeight; radius: Theme.editorButtonRadius
        color: automationArea.containsMouse ? editor.buttonHoverColor : editor.buttonColor
        border.color: activeFocus ? editor.accentColor : editor.buttonBorderColor
        activeFocusOnTab: editor.visible
        Accessible.role: Accessible.Button
        Accessible.name: "Toggle synthesizer automation editor"
        Keys.onReturnPressed: automationDrawer.visible = !automationDrawer.visible
        Keys.onSpacePressed: automationDrawer.visible = !automationDrawer.visible
        Text { anchors.centerIn: parent; text: "AUTOMATION"; color: editor.textColor; font.pixelSize: Theme.typeSmall }
        MouseArea {
            id: automationArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: automationDrawer.visible = !automationDrawer.visible
        }
    }

    NativeSynthAutomationEditor {
        id: automationDrawer
        objectName: "nativeSynthAutomationEditor"
        visible: false
        automation: editor.automation
        x: Math.max(0, parent.width - width)
        y: Theme.spaceXs
        width: Theme.editorDrawerWidth
        height: Math.min(Theme.editorDrawerMaxHeight, parent.height - 2 * Theme.spaceXs)
        z: 2
    }
}
