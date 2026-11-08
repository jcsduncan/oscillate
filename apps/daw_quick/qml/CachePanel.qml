pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: cachePanel
    transformOrigin: Item.Top
    Reveal { target: cachePanel; fromScale: Motion.popScale }
    required property var host
    required property var controls
    objectName: "cachePanel"
    // Rows from the top of the panel.
    readonly property int panelWidth: 390
    readonly property int panelHeight: 485
    readonly property int statusY: 40
    readonly property int usageY: 79
    readonly property int sharedUsageY: 100
    readonly property int sharedStatusY: 119
    readonly property int budgetLabelY: 149
    readonly property int budgetY: 170
    readonly property int budgetInputWidth: 142
    readonly property int applyX: 170
    readonly property int applyWidth: 100
    readonly property int projectsLabelY: 219
    readonly property int projectsY: 240
    readonly property int projectsHeight: 128
    readonly property int actionsY: 388
    readonly property int clearProjectWidth: 120
    readonly property int actionWidth: 95
    readonly property int noteY: 435
    readonly property int forgetWidth: 64
    readonly property int confirmButtonWidth: 80
    readonly property real mib: 1048576
    // The registered project waiting for "Forget?" to be confirmed, by path.
    property string confirmPath: ""
    readonly property var confirmProject: {
        const path = confirmPath
        return controls.projects.find(project => project.path === path) || null
    }
    // A project that leaves the list takes its question with it.
    onConfirmProjectChanged: if (confirmProject === null) confirmPath = ""
    function askForget(path) { confirmPath = path }
    function cancelForget() { confirmPath = "" }
    function confirmForget() {
        if (confirmPath.length === 0)
            return
        controls.forget_project(confirmPath)
        confirmPath = ""
    }
    visible: host.cachePanelOpen
    z: 8
    x: host.browserWidth + Theme.spaceLg
    y: Theme.floatingPanelTop
    width: panelWidth
    height: panelHeight
    radius: Theme.panelRadius
    color: host.panelRaised
    border.color: host.line
    function close() {
        cancelForget()
        host.cachePanelOpen = false
    }
    // Escape, a click anywhere else, or × closes it.
    onVisibleChanged: if (visible) forceActiveFocus()
    Keys.onEscapePressed: close()
    MouseArea {
        parent: cachePanel.parent
        anchors.fill: parent
        z: cachePanel.z - 0.01
        visible: cachePanel.visible
        onPressed: cachePanel.close()
    }
    MouseArea { anchors.fill: parent }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        y: Theme.dialogCloseY
        text: "×"
        color: cachePanelCloseArea.containsMouse ? cachePanel.host.textMain : cachePanel.host.textDim
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        font.pixelSize: Theme.typeDisplay
        Accessible.role: Accessible.Button
        Accessible.name: "Close processing cache"
        MouseArea {
            Tip {}
            id: cachePanelCloseArea
            anchors.fill: parent
            anchors.margins: -Theme.closeHitSlop
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: cachePanel.close()
        }
    }

    Text {
        x: Theme.spaceXl; y: Theme.spaceXl
        text: "PROCESSING CACHE"
        color: cachePanel.host.textMain
        font.pixelSize: Theme.typeLabel
        font.bold: true
    }
    Text {
        x: Theme.spaceXl; y: cachePanel.statusY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: cachePanel.controls.status
        color: cachePanel.host.textDim
        font.pixelSize: Theme.typeSmall
    }
    Text {
        x: Theme.spaceXl; y: cachePanel.usageY
        text: "GLOBAL CACHE  " + (cachePanel.controls.usageBytes / 1048576).toFixed(1)
            + " MiB  ·  " + cachePanel.controls.entryCount + " entries"
        color: cachePanel.host.textMain
        font.pixelSize: Theme.typeSmall
    }
    Text {
        x: Theme.spaceXl; y: cachePanel.sharedUsageY
        text: "SHARED ANALYSIS (2 GiB LIMIT)  "
            + (cachePanel.controls.sharedUsageBytes / 1048576).toFixed(1)
            + " MiB  ·  " + cachePanel.controls.sharedEntryCount + " entries"
        color: cachePanel.host.textMain
        font.pixelSize: Theme.typeSmall
    }
    Text {
        x: Theme.spaceXl; y: cachePanel.sharedStatusY
        width: parent.width - 2 * Theme.spaceXl
        elide: Text.ElideRight
        text: cachePanel.controls.sharedStatus
        color: cachePanel.host.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        x: Theme.spaceXl; y: cachePanel.budgetLabelY
        text: "GLOBAL CACHE BUDGET (MiB)"
        color: cachePanel.host.textDim
        font.pixelSize: Theme.typeSmall
    }
    Rectangle {
        x: Theme.spaceXl; y: cachePanel.budgetY; width: cachePanel.budgetInputWidth; height: Theme.dialogButtonHeight
        radius: Theme.controlRadius
        color: cachePanel.host.panel
        border.color: budgetInput.activeFocus ? cachePanel.host.accent : cachePanel.host.line
        TextInput {
            id: budgetInput
            objectName: "cacheBudgetInput"
            anchors.fill: parent
            anchors.margins: Theme.dialogInputPadding
            text: cachePanel.controls.budgetMiB.toString()
            color: cachePanel.host.textMain
            font.pixelSize: Theme.typeBody
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 64; top: 1048576 }
            Accessible.name: "Global cache budget in mebibytes"
            onAccepted: if (acceptableInput) cachePanel.controls.set_budget_mib(Number(text))
        }
    }
    Rectangle {
        x: cachePanel.applyX; y: cachePanel.budgetY; width: cachePanel.applyWidth; height: Theme.dialogButtonHeight
        radius: Theme.controlRadius
        color: cachePanel.host.panel
        border.color: activeFocus ? cachePanel.host.accent : cachePanel.host.line
        enabled: !cachePanel.controls.busy && budgetInput.acceptableInput
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: "Apply global cache budget"
        Keys.onReturnPressed: cachePanel.controls.set_budget_mib(Number(budgetInput.text))
        Keys.onSpacePressed: cachePanel.controls.set_budget_mib(Number(budgetInput.text))
        Text { anchors.centerIn: parent; text: "APPLY"; color: cachePanel.host.textMain; font.pixelSize: Theme.typeSmall }
        MouseArea {
            anchors.fill: parent
            enabled: parent.enabled
            onClicked: cachePanel.controls.set_budget_mib(Number(budgetInput.text))
            Tip {}
        }
    }
    Text {
        x: Theme.spaceXl; y: cachePanel.projectsLabelY
        text: "REGISTERED PROJECTS"
        color: cachePanel.host.textDim
        font.pixelSize: Theme.typeSmall
        font.bold: true
    }
    ListView {
        x: Theme.spaceXl; y: cachePanel.projectsY
        width: parent.width - 2 * Theme.spaceXl
        height: cachePanel.projectsHeight
        clip: true
        model: cachePanel.controls.projects
        delegate: Item {
            id: projectRow
            required property var modelData
            width: ListView.view.width
            height: projectText.height
            Column {
                id: projectText
                width: parent.width - (projectRow.modelData.current ? 0 : cachePanel.forgetWidth)
                spacing: Theme.spaceXxs
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: (projectRow.modelData.current ? "● " : "") + projectRow.modelData.name
                        + " · " + (projectRow.modelData.bytes / cachePanel.mib).toFixed(1) + " MiB"
                    color: cachePanel.host.textMain
                    font.pixelSize: Theme.typeSmall
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: projectRow.modelData.status + (projectRow.modelData.detail ? " · " + projectRow.modelData.detail : "")
                    color: projectRow.modelData.status === "Ready" ? cachePanel.host.textDim : cachePanel.host.amber
                    font.pixelSize: Theme.typeCaption
                }
            }
            // The open project is cleared with CLEAR PROJECT; others can go.
            Text {
                objectName: "cacheForget:" + projectRow.modelData.path
                visible: !projectRow.modelData.current
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "FORGET"
                enabled: !cachePanel.controls.busy
                activeFocusOnTab: visible && enabled
                opacity: enabled ? 1 : Theme.disabledOpacity
                color: forgetArea.containsMouse || activeFocus ? cachePanel.host.textMain : cachePanel.host.accent
                Keys.onReturnPressed: cachePanel.askForget(projectRow.modelData.path)
                Keys.onSpacePressed: cachePanel.askForget(projectRow.modelData.path)
                font.pixelSize: Theme.typeCaption
                font.bold: true
                Accessible.role: Accessible.Button
                Accessible.name: "Forget " + projectRow.modelData.name + " and clear its cache"
                MouseArea {
                    Tip {}
                    id: forgetArea
                    anchors.fill: parent
                    anchors.margins: -Theme.closeHitSlop
                    hoverEnabled: true
                    enabled: parent.enabled
                    cursorShape: Qt.PointingHandCursor
                    onClicked: cachePanel.askForget(projectRow.modelData.path)
                }
            }
        }
    }
    Row {
        x: Theme.spaceXl; y: cachePanel.actionsY
        visible: cachePanel.confirmProject === null
        spacing: Theme.spaceMd
        Repeater {
            model: [
                { label: "REFRESH", action: "refresh" },
                { label: "CLEAR PROJECT", action: "current" },
                { label: "CLEAR ALL", action: "all" }
            ]
            delegate: Rectangle {
                required property var modelData
                width: modelData.action === "current" ? cachePanel.clearProjectWidth : cachePanel.actionWidth
                height: Theme.dialogButtonHeight
                radius: Theme.controlRadius
                color: cachePanel.host.panel
                border.color: activeFocus ? cachePanel.host.accent : cachePanel.host.line
                enabled: !cachePanel.controls.busy
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: modelData.label + " reconstructable cache"
                function invoke() {
                    if (modelData.action === "refresh") cachePanel.controls.refresh()
                    else if (modelData.action === "current") cachePanel.controls.clear_current()
                    else cachePanel.controls.clear_all()
                }
                Keys.onReturnPressed: invoke()
                Keys.onSpacePressed: invoke()
                Text { anchors.centerIn: parent; text: parent.modelData.label; color: cachePanel.host.textMain; font.pixelSize: Theme.typeCaption }
                MouseArea {
                    anchors.fill: parent
                    enabled: parent.enabled
                    onClicked: parent.invoke()
                    Tip {}
                }
            }
        }
    }
    // "Forget?" replaces the actions until it is answered.
    Item {
        objectName: "cacheForgetConfirm"
        x: Theme.spaceXl; y: cachePanel.actionsY
        width: parent.width - 2 * Theme.spaceXl
        height: Theme.dialogButtonHeight
        visible: cachePanel.confirmProject !== null
        Text {
            objectName: "cacheForgetQuestion"
            anchors.left: parent.left
            anchors.right: forgetButtons.left
            anchors.rightMargin: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            text: cachePanel.confirmProject === null ? ""
                : "Forget " + cachePanel.confirmProject.name + " and free "
                    + (cachePanel.confirmProject.bytes / cachePanel.mib).toFixed(1) + " MiB?"
            color: cachePanel.host.textMain
            font.pixelSize: Theme.typeSmall
        }
        Row {
            id: forgetButtons
            anchors.right: parent.right
            spacing: Theme.spaceMd
            Repeater {
                model: [
                    { label: "FORGET", objectName: "cacheForgetConfirmButton" },
                    { label: "CANCEL", objectName: "cacheForgetCancelButton" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    objectName: modelData.objectName
                    width: cachePanel.confirmButtonWidth
                    height: Theme.dialogButtonHeight
                    radius: Theme.controlRadius
                    color: cachePanel.host.panel
                    border.color: activeFocus ? cachePanel.host.accent : cachePanel.host.line
                    enabled: !cachePanel.controls.busy
                    activeFocusOnTab: enabled
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label === "FORGET" ? "Forget the project and clear its cache" : "Keep the project"
                    function invoke() {
                        if (modelData.label === "FORGET") cachePanel.confirmForget()
                        else cachePanel.cancelForget()
                    }
                    Keys.onReturnPressed: invoke()
                    Keys.onSpacePressed: invoke()
                    Text { anchors.centerIn: parent; text: parent.modelData.label; color: cachePanel.host.textMain; font.pixelSize: Theme.typeCaption }
                    MouseArea {
                        anchors.fill: parent
                        enabled: parent.enabled
                        onClicked: parent.invoke()
                        Tip {}
                    }
                }
            }
        }
    }
    Text {
        x: Theme.spaceXl; y: cachePanel.noteY
        width: parent.width - 2 * Theme.spaceXl
        wrapMode: Text.WordWrap
        text: "Only generated analysis and processing files are cleared. Source media, project state and user renders remain."
        color: cachePanel.host.textDim
        font.pixelSize: Theme.typeCaption
    }
}
