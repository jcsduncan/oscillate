pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The undo history (Ctrl+H): every step by what it changed, oldest at the
// top. Click a step to go back (or forward) to just after it, in one go;
// "Project opened" goes back before them all. Undone steps stay listed,
// dimmed, until a new edit replaces them. Saved versions live in their own
// panel, a click away.
Rectangle {
    id: historyPanel
    transformOrigin: Item.Top
    Reveal { target: historyPanel; fromScale: Motion.popScale }
    required property var host
    required property var bridge
    readonly property var history: bridge.history
    objectName: "historyPanel"
    visible: bridge.projectOpen && host.historyPanelOpen
    readonly property int panelWidth: 340
    readonly property int panelMaxHeight: 520
    readonly property int titleY: 15
    readonly property int hintY: 36
    readonly property int listY: 72
    readonly property int listBottomRoom: 46
    readonly property int rowHeight: 28
    readonly property int currentBarWidth: 3
    readonly property int stepNumberWidth: 26
    readonly property int labelX: 40
    readonly property int labelRightRoom: 100
    readonly property int badgeRight: 10
    readonly property int versionsInset: 10
    readonly property int versionsPadding: 20
    readonly property int versionsHeight: 24
    readonly property real undoneNumberOpacity: 0.5
    readonly property real undoneLabelOpacity: 0.6
    z: 6
    anchors.right: parent.right
    anchors.rightMargin: Theme.menuPanelRightMargin
    width: panelWidth
    height: Math.min(panelMaxHeight, parent.height - y - Theme.space2xl)
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.line

    readonly property var steps: history ? history.steps : []
    readonly property int position: history ? history.position : 0
    // The row the keyboard points at: 0 is "Project opened", n the n-th step.
    property int cursor: position

    function close() { host.historyPanelOpen = false }
    function jump(to) {
        if (history && to !== position)
            history.jump_to(to)
    }
    onVisibleChanged: {
        if (history)
            history.active = visible
        if (visible) {
            cursor = position
            forceActiveFocus()
            list.positionViewAtIndex(Math.max(0, position), ListView.Center)
        }
    }
    onPositionChanged: {
        cursor = position
        list.positionViewAtIndex(Math.max(0, position), ListView.Contain)
    }
    Keys.onEscapePressed: close()
    Keys.onUpPressed: cursor = Math.max(0, cursor - 1)
    Keys.onDownPressed: cursor = Math.min(steps.length, cursor + 1)
    Keys.onReturnPressed: jump(cursor)
    Keys.onEnterPressed: jump(cursor)

    // A click anywhere else closes it.
    MouseArea {
        parent: historyPanel.parent
        anchors.fill: parent
        z: historyPanel.z - 0.01
        visible: historyPanel.visible
        onPressed: historyPanel.close()
    }
    MouseArea { anchors.fill: parent }

    Text {
        x: Theme.spaceXl
        y: historyPanel.titleY
        text: "HISTORY"
        color: host.textMain
        font.pixelSize: Theme.typeBody
        font.bold: true
        font.letterSpacing: Theme.trackingXl
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        y: Theme.dialogCloseY
        text: "×"
        color: closeArea.containsMouse ? host.textMain : host.textDim
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        font.pixelSize: Theme.typeDisplay
        Accessible.role: Accessible.Button
        Accessible.name: "Close history"
        MouseArea {
            Tip {}
            id: closeArea
            anchors.fill: parent
            anchors.margins: -Theme.closeHitSlop
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: historyPanel.close()
        }
    }
    Text {
        x: Theme.spaceXl
        y: historyPanel.hintY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: "Click a step to go back to just after it. Undone steps stay here until you make a new edit."
        color: host.textDim
        font.pixelSize: Theme.typeSmall
    }

    ListView {
        id: list
        objectName: "historyList"
        x: Theme.spaceLg
        y: historyPanel.listY
        width: parent.width - 2 * Theme.spaceLg
        height: parent.height - y - historyPanel.listBottomRoom
        clip: true
        spacing: Theme.spaceXxs
        // Row 0 is the project as opened; row n is the n-th step.
        model: historyPanel.steps.length + 1
        boundsBehavior: Flickable.StopAtBounds
        delegate: Rectangle {
            id: row
            required property int index
            readonly property var step: index > 0 ? historyPanel.steps[index - 1] : null
            readonly property bool undone: index > historyPanel.position
            readonly property bool current: index === historyPanel.position
            readonly property bool pointed: index === historyPanel.cursor
            width: ListView.view.width
            height: historyPanel.rowHeight
            radius: Theme.controlRadius
            color: current ? Theme.historyCurrentFill
                 : rowArea.containsMouse || pointed ? host.panel : "transparent"
            border.color: pointed && historyPanel.activeFocus ? host.accent : "transparent"
            Accessible.role: Accessible.Button
            Accessible.name: (index === 0 ? "Project opened" : step.label)
                + (current ? ", current" : undone ? ", undone" : "")
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Rectangle {
                visible: row.current
                width: historyPanel.currentBarWidth
                height: parent.height - 2 * Theme.spaceXs
                anchors.verticalCenter: parent.verticalCenter
                x: historyPanel.currentBarWidth
                radius: width / 2
                color: host.accent
            }
            Text {
                x: Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                width: historyPanel.stepNumberWidth
                text: row.index === 0 ? "" : row.index
                color: host.textDim
                font.pixelSize: Theme.typeCaption
                font.family: host.kit.monoFamily
                opacity: row.undone ? historyPanel.undoneNumberOpacity : 1
            }
            Text {
                x: historyPanel.labelX
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - historyPanel.labelRightRoom
                elide: Text.ElideRight
                text: row.index === 0 ? "Project opened" : row.step.label
                color: row.undone ? host.textDim : host.textMain
                font.pixelSize: Theme.typeBody
                font.italic: row.undone
                opacity: row.undone ? historyPanel.undoneLabelOpacity : 1
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: historyPanel.badgeRight
                anchors.verticalCenter: parent.verticalCenter
                visible: row.current || rowArea.containsMouse
                text: row.current ? "NOW" : row.undone ? "REDO TO HERE" : "UNDO TO HERE"
                color: row.current ? host.accent : host.textDim
                font.pixelSize: Theme.typeMicro
                font.bold: true
                font.letterSpacing: Theme.trackingXxs
            }
            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: row.current ? Qt.ArrowCursor : Qt.PointingHandCursor
                onClicked: {
                    historyPanel.cursor = row.index
                    historyPanel.jump(row.index)
                }
            }
        }
    }
    Text {
        x: Theme.spaceXl
        y: list.y + list.height + Theme.spaceSm
        visible: historyPanel.steps.length === 0
        text: "Nothing to undo yet"
        color: host.textDim
        font.pixelSize: Theme.typeSmall
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: historyPanel.versionsInset
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        width: versionsText.implicitWidth + historyPanel.versionsPadding
        height: historyPanel.versionsHeight
        radius: Theme.controlRadius
        color: versionsArea.containsMouse ? host.panel : "transparent"
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        border.color: host.line
        Accessible.role: Accessible.Button
        Accessible.name: "Saved versions"
        Text {
            id: versionsText
            anchors.centerIn: parent
            text: "SAVED VERSIONS…"
            color: host.textMain
            font.pixelSize: Theme.typeCaption
            font.bold: true
            font.letterSpacing: Theme.trackingXxs
        }
        MouseArea {
            Tip {}
            id: versionsArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                historyPanel.close()
                historyPanel.host.revisionPanelOpen = true
                historyPanel.bridge.refresh_named_revisions()
            }
        }
    }
}
