pragma ComponentBehavior: Bound
import QtQuick
import "shell"
import "devicekit" as Kit

// The project screen, in the workspace's own language: the top bar's
// identity, a start panel where the browser sits, and projects as rows like
// tracks. Recent projects come first, then the rest of the library.
Item {
    id: welcome
    readonly property Kit.ShellTheme kit: Theme
    // Layout: the top bar, the start panel at the left, projects beside it.
    readonly property int identityTextX: 48
    readonly property int autosaveHeight: 32
    readonly property int startPanelWidth: 360
    readonly property int inset: 20
    readonly property int headlineGap: 10
    readonly property int fieldGap: 18
    readonly property int buttonHeight: 40
    readonly property int projectsHeaderHeight: 44
    readonly property int projectCountX: 140
    readonly property int filterWidth: 260
    readonly property int filterHeight: 30
    readonly property int projectRowHeight: 62
    readonly property int stripeWidth: 5
    readonly property int renameX: 14
    readonly property int renameMaxWidth: 320
    readonly property int renameActionsRoom: 360
    readonly property int renameHeight: 24
    readonly property int nameY: 13
    readonly property int pathY: 32
    readonly property int actionsRoom: 340
    readonly property int actionPadding: 24
    readonly property int actionHeight: 28
    readonly property real actionRadius: 7
    readonly property int recoveryMaxWidth: 610
    readonly property int recoveryWindowInsetX: 64
    readonly property int recoveryMaxHeight: 530
    readonly property int recoveryWindowInsetY: 72
    readonly property real recoveryRadius: 12
    readonly property int recoveryListMinHeight: 100
    readonly property int recoveryListRoom: 200
    readonly property int recoveryRowHeight: 62
    readonly property int recoveryRowTextX: 14
    readonly property int recoveryDetailY: 34
    readonly property int recoveryCancelWidth: 96
    readonly property int recoveryCancelHeight: 32
    readonly property var projects: projectBridge.availableProjects
    readonly property var shownProjects: {
        const query = filterInput.text.trim().toLowerCase()
        return query.length === 0 ? projects
            : projects.filter(project => String(project.name).toLowerCase().includes(query))
    }
    // The row asking "Move to Trash?", by path; empty when none is.
    property string confirmPath: ""
    // The row being renamed, by path.
    property string renamePath: ""
    function create() {
        if (projectNameInput.text.trim().length > 0)
            projectBridge.create_default_project(projectNameInput.text)
    }
    // A stable colour per project name, from the track palette.
    function projectColor(name) {
        let hash = 0
        for (let index = 0; index < name.length; ++index)
            hash = (hash * 31 + name.charCodeAt(index)) >>> 0
        return kit.trackPalette[hash % kit.trackPalette.length]
    }
    readonly property string autosaveText: projectBridge.autosaveIntervalSeconds === 0 ? "Off"
        : projectBridge.autosaveIntervalSeconds === 30 ? "30 s"
        : projectBridge.autosaveIntervalSeconds === 120 ? "2 min"
        : projectBridge.autosaveIntervalSeconds === 300 ? "5 min"
        : projectBridge.autosaveIntervalSeconds + " s"

    Rectangle {
        anchors.fill: parent
        color: Theme.welcomeBackdrop
    }

    // Identity and autosave, as in the workspace's top bar.
    Rectangle {
        id: topBar
        width: parent.width
        height: Theme.toolbarHeight
        color: welcome.kit.panel
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: welcome.kit.line }
        Canvas {
            x: Theme.spaceXl
            anchors.verticalCenter: parent.verticalCenter
            // A diamond outline round a dot, as in the toolbar.
            readonly property real inset: 2
            readonly property real dotRadius: 3
            width: Theme.logoSize
            height: Theme.logoSize
            onPaint: {
                const context = getContext("2d")
                context.clearRect(0, 0, width, height)
                context.strokeStyle = welcome.kit.accent
                context.lineWidth = Theme.strokeBold
                context.beginPath()
                context.moveTo(width / 2, inset); context.lineTo(width - inset, height / 2); context.lineTo(width / 2, height - inset); context.lineTo(inset, height / 2)
                context.closePath()
                context.stroke()
                context.fillStyle = welcome.kit.accent
                context.beginPath()
                context.arc(width / 2, height / 2, dotRadius, 0, Math.PI * 2)
                context.fill()
            }
        }
        Column {
            x: welcome.identityTextX
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.identityLineGap
            Text { text: "Oscillate"; color: welcome.kit.textMain; font.pixelSize: Theme.typeSubtitle; font.weight: Font.DemiBold }
            Text { text: "Projects"; color: welcome.kit.textDim; font.pixelSize: Theme.typeBody }
        }
        Rectangle {
            id: autosaveChoice
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceXl
            anchors.verticalCenter: parent.verticalCenter
            width: autosaveRow.implicitWidth + 2 * Theme.spaceLg
            height: welcome.autosaveHeight
            radius: Theme.radiusXl
            color: welcome.kit.panelRaised
            border.color: activeFocus ? welcome.kit.accent : Theme.fieldEdge
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Autosave interval " + welcome.autosaveText + ". Activate to change."
            function nextInterval() {
                const current = projectBridge.autosaveIntervalSeconds
                projectBridge.autosaveIntervalSeconds = current === 0 ? 30
                                                       : current === 30 ? 120
                                                       : current === 120 ? 300 : 0
            }
            Keys.onReturnPressed: nextInterval()
            Keys.onSpacePressed: nextInterval()
            Row {
                id: autosaveRow
                anchors.centerIn: parent
                spacing: Theme.spaceSm
                Text { text: "Autosave"; color: welcome.kit.textDim; font.pixelSize: Theme.typeLabel }
                Text { text: welcome.autosaveText; color: welcome.kit.textMain; font.pixelSize: Theme.typeLabel; font.family: welcome.kit.monoFamily }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: autosaveChoice.nextInterval()
                Tip {}
            }
        }
    }

    // Start: where the browser sits in the workspace.
    Rectangle {
        id: startPanel
        y: topBar.height
        width: welcome.startPanelWidth
        height: parent.height - y
        color: Theme.browserFill
        Rectangle { anchors.right: parent.right; width: Theme.hairline; height: parent.height; color: welcome.kit.line }
        Column {
            x: welcome.inset
            y: Theme.space2xl
            width: parent.width - 2 * welcome.inset
            spacing: 0
            Text { text: "NEW PROJECT"; color: welcome.kit.textDim; font.pixelSize: Theme.typeBody; font.letterSpacing: Theme.trackingXs }
            Item { width: 1; height: welcome.headlineGap }
            Text {
                width: parent.width
                text: "Make room for an idea."
                color: welcome.kit.textMain
                font.pixelSize: Theme.welcomeHeadlineSize
                font.weight: Font.DemiBold
            }
            Item { width: 1; height: welcome.fieldGap }
            Rectangle {
                width: parent.width
                height: welcome.buttonHeight
                radius: Theme.radiusXl
                color: welcome.kit.panelRaised
                border.color: projectNameInput.activeFocus ? welcome.kit.accent : Theme.fieldEdge
                TextInput {
                    id: projectNameInput
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    verticalAlignment: TextInput.AlignVCenter
                    text: "Untitled Project"
                    selectByMouse: true
                    activeFocusOnTab: true
                    clip: true
                    color: welcome.kit.textMain
                    selectionColor: Theme.textSelection
                    font.pixelSize: Theme.typeEmphasis
                    Accessible.role: Accessible.EditableText
                    Accessible.name: "Project name"
                    Keys.onReturnPressed: welcome.create()
                }
            }
            Item { width: 1; height: Theme.spaceSm }
            Text { text: "Saved in ~/.oscillate/projects"; color: welcome.kit.textFaint; font.pixelSize: Theme.typeBody }
            Item { width: 1; height: Theme.spaceXl }
            Rectangle {
                id: createAction
                width: parent.width
                height: welcome.buttonHeight
                radius: Theme.radiusXl
                readonly property bool usable: projectNameInput.text.trim().length > 0
                color: welcome.kit.accent
                opacity: usable ? 1 : Theme.disabledOpacity
                border.color: activeFocus ? welcome.kit.textMain : color
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Create project"
                Keys.onReturnPressed: welcome.create()
                Keys.onSpacePressed: welcome.create()
                Text {
                    anchors.centerIn: parent
                    text: "Create project"
                    color: Theme.canvas
                    font.pixelSize: Theme.typeEmphasis
                    font.weight: Font.DemiBold
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: createAction.usable
                    onClicked: welcome.create()
                    Tip {}
                }
            }
            Item { width: 1; height: Theme.spaceMd }
            Rectangle {
                id: openAction
                width: parent.width
                height: welcome.buttonHeight
                radius: Theme.radiusXl
                color: openArea.containsMouse ? Theme.raisedHover : welcome.kit.panelRaised
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                border.color: activeFocus ? welcome.kit.accent : Theme.fieldEdge
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Open existing project"
                Keys.onReturnPressed: projectBridge.choose_open_project()
                Keys.onSpacePressed: projectBridge.choose_open_project()
                Text {
                    anchors.centerIn: parent
                    text: "Open a project folder…"
                    color: Theme.textSoft
                    font.pixelSize: Theme.typeEmphasis
                }
                MouseArea {
                    Tip {}
                    id: openArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: projectBridge.choose_open_project()
                }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: statusText.implicitHeight + 2 * Theme.spaceLg
            color: "transparent"
            Rectangle { width: parent.width - Theme.hairline; height: Theme.hairline; color: welcome.kit.line }
            Text {
                id: statusText
                x: welcome.inset
                y: Theme.spaceLg
                width: parent.width - 2 * welcome.inset
                text: projectBridge.status
                color: welcome.kit.textDim
                font.pixelSize: Theme.typeBody
                wrapMode: Text.WordWrap
            }
        }
    }

    // Projects, laid out like the track list beside the arrangement.
    Item {
        id: projectsArea
        x: startPanel.width
        y: topBar.height
        width: parent.width - x
        height: parent.height - y
        Rectangle {
            id: projectsHeader
            width: parent.width
            height: welcome.projectsHeaderHeight
            color: Theme.browserFill
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: welcome.kit.line }
            Text {
                x: welcome.inset
                anchors.verticalCenter: parent.verticalCenter
                text: "YOUR PROJECTS"
                color: welcome.kit.textDim
                font.pixelSize: Theme.typeBody
                font.letterSpacing: Theme.trackingXs
            }
            Text {
                x: welcome.projectCountX
                anchors.verticalCenter: parent.verticalCenter
                text: String(welcome.projects.length)
                color: welcome.kit.textFaint
                font.pixelSize: Theme.typeBody
                font.family: welcome.kit.monoFamily
            }
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceXl
                anchors.verticalCenter: parent.verticalCenter
                width: welcome.filterWidth
                height: welcome.filterHeight
                radius: Theme.radiusXl
                color: welcome.kit.panelRaised
                border.color: filterInput.activeFocus ? welcome.kit.accent : Theme.fieldEdge
                Text { x: Theme.searchIconX; anchors.verticalCenter: parent.verticalCenter; text: "⌕"; color: welcome.kit.textDim; font.pixelSize: Theme.typeEmphasis }
                TextInput {
                    id: filterInput
                    anchors.fill: parent
                    anchors.leftMargin: Theme.searchTextInset
                    anchors.rightMargin: Theme.spaceMd
                    verticalAlignment: TextInput.AlignVCenter
                    color: welcome.kit.textMain
                    selectionColor: Theme.textSelection
                    font.pixelSize: Theme.typeLabel
                    clip: true
                    activeFocusOnTab: true
                    Accessible.name: "Filter projects"
                }
                Text {
                    x: Theme.searchTextInset
                    anchors.verticalCenter: parent.verticalCenter
                    visible: filterInput.text.length === 0
                    text: "Find a project"
                    color: welcome.kit.textFaint
                    font.pixelSize: Theme.typeLabel
                }
            }
        }
        Text {
            visible: welcome.shownProjects.length === 0
            x: welcome.inset
            y: projectsHeader.height + Theme.space2xl
            width: parent.width - 2 * welcome.inset
            text: welcome.projects.length === 0 ? "Projects you create or open will appear here."
                : "No project matches"
            color: welcome.kit.textDim
            font.pixelSize: Theme.typeLabel
        }
        ListView {
            id: projectList
            y: projectsHeader.height
            width: parent.width
            height: parent.height - y
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: welcome.shownProjects
            delegate: Rectangle {
                id: projectRow
                required property var modelData
                required property int index
                readonly property color stripe: welcome.projectColor(String(modelData.name))
                width: projectList.width
                height: welcome.projectRowHeight
                color: rowArea.containsMouse || activeFocus ? welcome.kit.selection : "transparent"
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Open " + modelData.name
                Keys.onReturnPressed: projectBridge.open_listed(modelData.path)
                Keys.onSpacePressed: projectBridge.open_listed(modelData.path)
                Rectangle { width: welcome.stripeWidth; height: parent.height; color: projectRow.stripe }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.rowDivider }
                Rectangle {
                    visible: welcome.renamePath === projectRow.modelData.path
                    x: welcome.renameX
                    y: Theme.spaceMd
                    width: Math.min(welcome.renameMaxWidth, parent.width - welcome.renameActionsRoom)
                    height: welcome.renameHeight
                    radius: Theme.controlRadius
                    color: Theme.renameFieldFill
                    border.color: welcome.kit.accent
                    z: 2
                    TextInput {
                        id: renameInput
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceSm
                        anchors.rightMargin: Theme.spaceSm
                        verticalAlignment: TextInput.AlignVCenter
                        color: welcome.kit.textMain
                        font.pixelSize: Theme.typeEmphasis
                        font.weight: Font.DemiBold
                        selectByMouse: true
                        clip: true
                        Accessible.role: Accessible.EditableText
                        Accessible.name: "New name for " + projectRow.modelData.name
                        function finish(commit) {
                            if (welcome.renamePath !== projectRow.modelData.path) return
                            welcome.renamePath = ""
                            if (commit)
                                projectBridge.rename_project(projectRow.modelData.path, text)
                        }
                        Keys.onReturnPressed: finish(true)
                        Keys.onEnterPressed: finish(true)
                        Keys.onEscapePressed: finish(false)
                        onActiveFocusChanged: if (!activeFocus) finish(true)
                    }
                    onVisibleChanged: {
                        if (visible) {
                            renameInput.text = projectRow.modelData.name
                            renameInput.forceActiveFocus()
                            renameInput.selectAll()
                        }
                    }
                }
                Text {
                    x: welcome.inset
                    y: welcome.nameY
                    visible: welcome.renamePath !== projectRow.modelData.path
                    width: parent.width - (projectRow.showActions ? welcome.actionsRoom : 2 * welcome.inset)
                    text: projectRow.modelData.name
                    color: welcome.kit.textMain
                    font.pixelSize: Theme.typeEmphasis
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    x: welcome.inset
                    y: welcome.pathY
                    width: parent.width - (projectRow.showActions ? welcome.actionsRoom : 2 * welcome.inset)
                    text: projectRow.modelData.path
                    color: welcome.kit.textDim
                    font.pixelSize: Theme.typeBody
                    elide: Text.ElideMiddle
                }
                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: projectBridge.open_listed(projectRow.modelData.path)
                }
                readonly property bool confirming: welcome.confirmPath === modelData.path
                readonly property bool showActions: rowArea.containsMouse || actionsHover.hovered
                                                    || projectRow.activeFocus || confirming
                HoverHandler { id: actionsHover }
                Keys.onDeletePressed: {
                    if (projectRow.modelData.inLibrary)
                        welcome.confirmPath = projectRow.modelData.path
                    else
                        projectBridge.forget_project(projectRow.modelData.path)
                }
                Row {
                    visible: projectRow.showActions
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spaceXl
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spaceMd
                    Text {
                        visible: projectRow.confirming
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Move to the Trash?"
                        color: welcome.kit.amber
                        font.pixelSize: Theme.typeLabel
                    }
                    Repeater {
                        model: projectRow.confirming
                            ? [{ key: "cancel", label: "Cancel" }, { key: "trash", label: "Delete", danger: true }]
                            : [{ key: "rename", label: "Rename", dim: true,
                                 hidden: !projectRow.modelData.inLibrary || projectRow.modelData.open === true },
                               { key: projectRow.modelData.inLibrary ? "ask" : "forget",
                                 label: projectRow.modelData.inLibrary ? "Delete" : "Remove from list",
                                 dim: true, hidden: projectRow.modelData.open === true },
                               { key: "open", label: "Open →" }]
                        delegate: Rectangle {
                            id: action
                            required property var modelData
                            visible: modelData.hidden !== true
                            width: actionLabel.implicitWidth + welcome.actionPadding
                            height: welcome.actionHeight
                            radius: welcome.actionRadius
                            color: modelData.danger ? (actionArea.containsMouse ? Theme.dangerHover : Theme.dangerFill)
                                 : actionArea.containsMouse ? Theme.buttonHover : welcome.kit.panelRaised
                            border.color: modelData.danger ? Theme.dangerEdge : Theme.fieldEdge
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.label + " " + projectRow.modelData.name
                            Text {
                                id: actionLabel
                                anchors.centerIn: parent
                                text: action.modelData.label
                                color: action.modelData.dim && !actionArea.containsMouse ? welcome.kit.textDim : Theme.textSoft
                                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                                font.pixelSize: Theme.typeLabel
                            }
                            MouseArea {
                                Tip {}
                                id: actionArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    const path = projectRow.modelData.path
                                    const key = action.modelData.key
                                    if (key === "open") projectBridge.open_listed(path)
                                    else if (key === "ask") welcome.confirmPath = path
                                    else if (key === "rename") welcome.renamePath = path
                                    else if (key === "cancel") welcome.confirmPath = ""
                                    else if (key === "forget") projectBridge.forget_project(path)
                                    else if (key === "trash") {
                                        welcome.confirmPath = ""
                                        projectBridge.trash_project(path)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: recoveryOverlay
        anchors.fill: parent
        z: 20
        visible: projectBridge.recoveryChoice.active === true
        color: Theme.welcomeScrim

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(welcome.recoveryMaxWidth, parent.width - welcome.recoveryWindowInsetX)
            height: Math.min(welcome.recoveryMaxHeight, parent.height - welcome.recoveryWindowInsetY)
            radius: welcome.recoveryRadius
            color: welcome.kit.panel
            border.color: welcome.kit.lineStrong

            Column {
                anchors.fill: parent
                anchors.margins: Theme.space2xl
                spacing: Theme.spaceLg

                Text {
                    text: "CHOOSE A VERSION TO OPEN"
                    color: welcome.kit.accent
                    font.pixelSize: Theme.typeBody
                    font.letterSpacing: Theme.trackingXs
                }
                Text {
                    text: projectBridge.recoveryChoice.projectName || "Project"
                    color: welcome.kit.textMain
                    font.weight: Font.DemiBold
                    font.pixelSize: Theme.recoveryTitleSize
                }
                Text {
                    width: parent.width
                    text: projectBridge.recoveryChoice.currentIssue
                          ? "This project's last save didn't finish cleanly. Pick the version to open:"
                          : "There's newer work in the autosave than in your last save. Pick the version to open; both stay available until you save."
                    color: welcome.kit.textDim
                    font.pixelSize: Theme.typeLabel
                    wrapMode: Text.WordWrap
                }
                Text {
                    visible: !!projectBridge.recoveryChoice.autosaveIssue
                    width: parent.width
                    text: "The autosave may be incomplete: " + projectBridge.recoveryChoice.autosaveIssue
                    color: welcome.kit.amber
                    font.pixelSize: Theme.typeBody
                    wrapMode: Text.WordWrap
                }
                ListView {
                    id: recoveryList
                    width: parent.width
                    height: Math.max(welcome.recoveryListMinHeight, parent.height - welcome.recoveryListRoom)
                    clip: true
                    spacing: Theme.spaceSm
                    model: projectBridge.recoveryChoice.options || []
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool selectable: modelData.kind !== "autosave"
                                                           || projectBridge.recoveryChoice.hasCurrent
                        width: recoveryList.width
                        height: welcome.recoveryRowHeight
                        radius: Theme.radiusXl
                        color: selectable ? welcome.kit.panelRaised : welcome.kit.panel
                        border.color: activeFocus ? welcome.kit.accent : Theme.fieldEdge
                        activeFocusOnTab: selectable
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.label + ", " + modelData.modified
                        Accessible.description: modelData.modified
                        Keys.onReturnPressed: if (selectable)
                                                  projectBridge.choose_recovery(modelData.generation, modelData.kind)
                        Keys.onSpacePressed: if (selectable)
                                                 projectBridge.choose_recovery(modelData.generation, modelData.kind)
                        Text {
                            x: welcome.recoveryRowTextX
                            y: Theme.spaceLg
                            text: modelData.label
                            color: parent.selectable ? welcome.kit.textMain : welcome.kit.textDim
                            font.pixelSize: Theme.typeEmphasis
                            font.weight: Font.DemiBold
                        }
                        Text {
                            x: welcome.recoveryRowTextX
                            y: welcome.recoveryDetailY
                            text: modelData.modified + "  ·  " + modelData.tracks + " tracks, "
                                  + modelData.clips + " clips"
                            color: welcome.kit.textDim
                            font.pixelSize: Theme.typeBody
                        }
                        MouseArea {
                            Tip {}
                            anchors.fill: parent
                            enabled: parent.selectable
                            onClicked: projectBridge.choose_recovery(modelData.generation, modelData.kind)
                        }
                    }
                }
                Rectangle {
                    width: welcome.recoveryCancelWidth
                    height: welcome.recoveryCancelHeight
                    radius: Theme.radiusXl
                    color: welcome.kit.panelRaised
                    border.color: activeFocus ? welcome.kit.accent : Theme.fieldEdge
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Cancel project recovery"
                    Keys.onReturnPressed: projectBridge.cancel_recovery()
                    Keys.onSpacePressed: projectBridge.cancel_recovery()
                    Text {
                        anchors.centerIn: parent
                        text: "Cancel"
                        color: Theme.textSoft
                        font.pixelSize: Theme.typeLabel
                    }
                    MouseArea {
                        Tip {}
                        anchors.fill: parent
                        onClicked: projectBridge.cancel_recovery()
                    }
                }
            }
        }
    }
}
