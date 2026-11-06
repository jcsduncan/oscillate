pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The context panel at the right edge: editing controls for the selection
// or the master effects, opened from the top bar's More menu.
Item {
    id: inspectorWorkspace
    required property var bridge
    required property int selectedTrack
    required property string selectedClipId
    required property var currentRouteTrack
    required property bool contextPanelOpen
    required property color canvas
    required property color panel
    required property color panelRaised
    required property color line
    required property color textMain
    required property color textDim
    required property color accent
    required property color amber

    readonly property int inspectorWidth: contextPanelOpen ? (width >= Theme.inspectorWideBreakpoint ? Theme.inspectorWideWidth : Theme.inspectorNarrowWidth) : 0
    readonly property bool fxPanelActive: inspector.fxPanelActive
    property alias selectedSendId: inspector.selectedSendId

    signal contextPanelRequested(bool effects)
    signal contextPanelCloseRequested()
    signal exportRequested()

    function showContext(effects) {
        inspector.showSection(effects)
        contextPanelRequested(effects)
    }

    InspectorPanel {
        id: inspector
        visible: inspectorWorkspace.contextPanelOpen
        anchors.right: parent.right
        y: 0
        width: inspectorWorkspace.inspectorWidth
        height: parent.height
        bridge: inspectorWorkspace.bridge
        selectedTrack: inspectorWorkspace.selectedTrack
        selectedClipId: inspectorWorkspace.selectedClipId
        currentRouteTrack: inspectorWorkspace.currentRouteTrack
        canvas: inspectorWorkspace.canvas
        panel: inspectorWorkspace.panel
        panelRaised: inspectorWorkspace.panelRaised
        line: inspectorWorkspace.line
        textMain: inspectorWorkspace.textMain
        textDim: inspectorWorkspace.textDim
        accent: inspectorWorkspace.accent
        amber: inspectorWorkspace.amber
        onExportRequested: inspectorWorkspace.exportRequested()
    }
    Rectangle {
        objectName: "closeContextPanel"
        visible: inspectorWorkspace.contextPanelOpen
        x: inspector.x + inspector.width - Theme.inspectorCloseRight
        y: Theme.spaceLg
        width: Theme.inspectorCloseSize
        height: Theme.inspectorCloseSize
        radius: Theme.controlRadius
        color: inspectorWorkspace.panelRaised
        border.color: activeFocus ? inspectorWorkspace.accent : inspectorWorkspace.line
        activeFocusOnTab: visible
        Accessible.role: Accessible.Button
        Accessible.name: "Close context panel"
        Keys.onReturnPressed: inspectorWorkspace.contextPanelCloseRequested()
        Keys.onSpacePressed: inspectorWorkspace.contextPanelCloseRequested()
        Text { anchors.centerIn: parent; text: "×"; color: inspectorWorkspace.textMain; font.pixelSize: Theme.closeGlyphSize }
        MouseArea {
            anchors.fill: parent
            onClicked: inspectorWorkspace.contextPanelCloseRequested()
            Tip {}
        }
    }
}
