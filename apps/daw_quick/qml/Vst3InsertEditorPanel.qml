pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: editorPanel
    required property var editor
    required property color panelColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    readonly property var automation: projectBridge.pluginAutomation.view
    readonly property bool automationMode: Boolean(automation.active) && Boolean(automation.parameterSelected)
    color: panelColor
    border.color: lineColor
    // Editor values keyed by stable parameter ID, built once per editor change.
    readonly property var editorEntries: {
        const entries = {}
        for (const item of (editor.parameters || []))
            entries[item.id] = item
        return entries
    }
    // The automation projection filters and pages in C++. Without it, filter
    // the editor's own list here; that list is bounded by the host and model.
    readonly property var fallbackRows: {
        if (automation.active)
            return []
        const query = searchInput.text.trim().toLowerCase()
        return (editor.parameters || []).filter(item => query.length === 0
            || String(item.id).startsWith(query) || String(item.name).toLowerCase().includes(query))
    }
    readonly property var parameterRows: automation.active ? (automation.parameters || []) : fallbackRows
    readonly property string resultStatus: {
        if (automation.active)
            return automation.resultStatus || ""
        const total = (editor.parameters || []).length
        const query = searchInput.text.trim()
        if (query.length === 0)
            return total + " parameters"
        return fallbackRows.length === 0 ? "No parameter name or ID matches \u201c" + query + "\u201d"
                                         : fallbackRows.length + " of " + total + " parameters match \u201c" + query + "\u201d"
    }
    // Layout: title and close, the editor message, then parameter search and rows.
    readonly property int titleY: 10
    readonly property int titleRightRoom: 64
    readonly property int closeRight: 35
    readonly property int closeSize: 25
    readonly property int messageY: 40
    readonly property int browserMinTop: 88
    readonly property int searchOffset: 16
    readonly property int searchHeight: 28
    readonly property int resultsOffset: 48
    readonly property int listOffset: 66
    readonly property int rowHeight: 29
    readonly property int rowNameRoom: 124
    readonly property int automateRight: 68
    readonly property int automateWidth: 50
    readonly property int rowControlHeight: 24
    readonly property int valueWidth: 62
    readonly property int browserTop: Math.max(browserMinTop, messageY + editorMessage.implicitHeight + Theme.spaceLg)
    // A different plugin starts with an empty search; the controller resets too.
    readonly property string browsedPlugin: automation.pluginId || ""
    onBrowsedPluginChanged: searchInput.text = ""
    // Stable ID of the row that last held keyboard focus. A saved edit
    // republishes the rows and recreates their delegates; focus follows the ID.
    property var keyboardParameterId: undefined

    function restoreRowFocus() {
        if (keyboardParameterId === undefined || automationMode)
            return
        // Recreated rows leave focus on the view, a row root or nowhere; any
        // other focused control means the user moved on.
        const focused = Window.activeFocusItem
        if (focused && focused !== parameterList && focused !== Window.contentItem
                && focused.parent !== parameterList.contentItem)
            return
        const row = parameterRows.findIndex(item => item.id === keyboardParameterId)
        if (row >= 0)
            focusRow(row)
    }

    // Moves focus to a row's first control, creating its delegate if needed.
    function focusRow(row) {
        if (row < 0) {
            searchInput.forceActiveFocus()
            return
        }
        if (row >= parameterList.count)
            return
        parameterList.positionViewAtIndex(row, ListView.Contain)
        const item = parameterList.itemAtIndex(row)
        if (item)
            item.focusControl()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        enabled: editorPanel.visible && !editorPanel.automationMode
        onActivated: searchInput.forceActiveFocus()
    }

    Text {
        x: Theme.spaceLg
        y: editorPanel.titleY
        width: parent.width - editorPanel.titleRightRoom
        elide: Text.ElideRight
        text: (editorPanel.editor.role === "source" ? "SOURCE  ·  " : "")
              + (editorPanel.editor.name || "VST3 EFFECT")
        color: editorPanel.textColor
        font.pixelSize: Theme.typeLabel
        font.bold: true
    }
    Rectangle {
        id: closeButton
        objectName: "vst3EditorClose"
        x: parent.width - editorPanel.closeRight
        y: Theme.spaceMd
        width: editorPanel.closeSize
        height: editorPanel.closeSize
        color: editorPanel.panelColor
        border.color: activeFocus ? editorPanel.accentColor : editorPanel.lineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Close VST3 editor and save remaining changes"
        Keys.onReturnPressed: editorPanel.editor.native ? projectBridge.vst3Editor.apply_vst3_insert_editor() : projectBridge.vst3Editor.discard_vst3_insert_editor()
        Keys.onSpacePressed: editorPanel.editor.native ? projectBridge.vst3Editor.apply_vst3_insert_editor() : projectBridge.vst3Editor.discard_vst3_insert_editor()
        Text {
            anchors.centerIn: parent
            text: "×"
            color: editorPanel.textColor
            font.pixelSize: Theme.typeSubtitle
        }
        MouseArea {
            anchors.fill: parent
            onClicked: editorPanel.editor.native ? projectBridge.vst3Editor.apply_vst3_insert_editor() : projectBridge.vst3Editor.discard_vst3_insert_editor()
        }
    }
    PluginAutomationEditor {
        objectName: "pluginAutomationEditor"
        x: Theme.spaceLg
        y: editorPanel.messageY
        width: parent.width - 2 * Theme.spaceLg
        height: parent.height - y - Theme.spaceMd
        visible: editorPanel.automationMode
        bridge: projectBridge
        controller: projectBridge.pluginAutomation
        view: editorPanel.automation
        panelColor: editorPanel.panelColor
        lineColor: editorPanel.lineColor
        textColor: editorPanel.textColor
        dimColor: editorPanel.dimColor
        accentColor: editorPanel.accentColor
    }
    // During playback the message distinguishes a saved edit from an audible one.
    Text {
        id: editorMessage
        objectName: "vst3EditorMessage"
        x: Theme.spaceLg
        y: editorPanel.messageY
        width: parent.width - 2 * Theme.spaceLg
        visible: !editorPanel.automationMode
        wrapMode: Text.WordWrap
        text: editorPanel.editor.liveStatus || editorPanel.editor.message || ""
        color: editorPanel.editor.liveStatus ? editorPanel.textColor : editorPanel.dimColor
        font.pixelSize: Theme.typeSmall
    }
    Text {
        x: Theme.spaceLg
        y: editorPanel.browserTop
        width: parent.width - 2 * Theme.spaceLg
        visible: !editorPanel.automationMode
        elide: Text.ElideRight
        text: (editorPanel.editor.editable ? "PARAMETERS · ENTER A VALUE FROM 0 TO 1"
                                           : "PARAMETER REFERENCES · READ ONLY")
              + "  ·  AUTO EDITS A LANE"
        color: editorPanel.dimColor
        font.pixelSize: Theme.typeCaption
        font.bold: true
    }
    Rectangle {
        x: Theme.spaceLg
        y: editorPanel.browserTop + editorPanel.searchOffset
        width: parent.width - 2 * Theme.spaceLg
        height: editorPanel.searchHeight
        radius: Theme.radiusMd
        visible: !editorPanel.automationMode
        color: editorPanel.panelColor
        border.color: searchInput.activeFocus ? editorPanel.accentColor : editorPanel.lineColor
        TextInput {
            id: searchInput
            objectName: "vst3ParameterSearch"
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceMd
            anchors.rightMargin: Theme.spaceMd
            verticalAlignment: TextInput.AlignVCenter
            color: editorPanel.textColor
            selectionColor: editorPanel.accentColor
            font.pixelSize: Theme.typeBody
            clip: true
            selectByMouse: true
            maximumLength: 128
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Search parameters by name or ID"
            Accessible.description: editorPanel.resultStatus
            onActiveFocusChanged: {
                if (activeFocus)
                    editorPanel.keyboardParameterId = undefined
            }
            onTextChanged: {
                if (editorPanel.automation.active)
                    projectBridge.pluginAutomation.set_parameter_filter(text)
            }
            Keys.onDownPressed: editorPanel.focusRow(0)
            Keys.onReturnPressed: editorPanel.focusRow(0)
            Keys.onEnterPressed: editorPanel.focusRow(0)
            Keys.onEscapePressed: event => {
                event.accepted = text.length > 0
                text = ""
            }
        }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            visible: searchInput.text.length === 0
            text: "Search name or ID  ·  Ctrl+F"
            color: editorPanel.dimColor
            font.pixelSize: Theme.typeSmall
        }
    }
    // Always states how many rows are shown out of how many exist.
    Text {
        objectName: "vst3ParameterResultStatus"
        x: Theme.spaceLg
        y: editorPanel.browserTop + editorPanel.resultsOffset
        width: parent.width - 2 * Theme.spaceLg
        visible: !editorPanel.automationMode
        elide: Text.ElideRight
        text: editorPanel.resultStatus
        color: editorPanel.automation.resultsLimited || parameterList.count === 0
               ? editorPanel.textColor : editorPanel.dimColor
        font.pixelSize: Theme.typeCaption
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
    ListView {
        id: parameterList
        objectName: "vst3ParameterList"
        x: Theme.spaceLg
        y: editorPanel.browserTop + editorPanel.listOffset
        width: parent.width - 2 * Theme.spaceLg
        height: parent.height - y - Theme.spaceMd
        visible: !editorPanel.automationMode
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.List
        Accessible.name: "VST3 parameters"
        Accessible.description: editorPanel.resultStatus
        // The automation projection is the superset: verified host parameters,
        // saved references and any saved lane, each identified by stable ID.
        model: editorPanel.parameterRows
        onModelChanged: Qt.callLater(editorPanel.restoreRowFocus)
        delegate: Item {
            id: parameterRow
            required property var modelData
            required property int index
            readonly property var editorEntry: editorPanel.editorEntries[modelData.id]
                || ({ value: "—", editable: false })
            readonly property bool canEdit: Boolean(editorPanel.editor.editable)
                && Boolean(editorEntry.editable)
            readonly property bool lanePresent: Boolean(modelData.lanePresent)
            readonly property bool canAutomate: Boolean(modelData.canAutomate)
                && (lanePresent || projectBridge.audioEditEnabled)
            objectName: "vst3ParameterRow_" + modelData.id
            width: ListView.view.width
            height: editorPanel.rowHeight
            function focusControl() {
                if (parameterRow.canEdit)
                    valueInput.forceActiveFocus()
                else if (automateButton.visible && automateButton.enabled)
                    automateButton.forceActiveFocus()
                else
                    rowName.forceActiveFocus()
            }
            // Keep a keyboard-focused row inside the virtualized viewport.
            function reveal(focused) {
                if (!focused)
                    return
                editorPanel.keyboardParameterId = parameterRow.modelData.id
                parameterRow.ListView.view.positionViewAtIndex(parameterRow.index, ListView.Contain)
            }
            Text {
                id: rowName
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - editorPanel.rowNameRoom
                elide: Text.ElideRight
                // Plugin-supplied names are plain text, never markup.
                text: parameterRow.modelData.name + "  #" + parameterRow.modelData.id
                textFormat: Text.PlainText
                color: rowName.activeFocus ? editorPanel.accentColor : editorPanel.textColor
                font.pixelSize: Theme.typeSmall
                Accessible.role: Accessible.StaticText
                Accessible.name: parameterRow.modelData.name + ", parameter ID " + parameterRow.modelData.id
                onActiveFocusChanged: parameterRow.reveal(activeFocus)
                Keys.onUpPressed: editorPanel.focusRow(parameterRow.index - 1)
                Keys.onDownPressed: editorPanel.focusRow(parameterRow.index + 1)
            }
            Rectangle {
                id: automateButton
                objectName: "vst3ParameterAutomate_" + parameterRow.modelData.id
                visible: Boolean(editorPanel.automation.active)
                anchors.right: parent.right
                anchors.rightMargin: editorPanel.automateRight
                anchors.verticalCenter: parent.verticalCenter
                width: editorPanel.automateWidth
                height: editorPanel.rowControlHeight
                radius: Theme.radiusSm
                enabled: parameterRow.canAutomate
                color: parameterRow.lanePresent && enabled ? Theme.automatedFill : editorPanel.panelColor
                border.color: activeFocus ? editorPanel.accentColor : editorPanel.lineColor
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: (parameterRow.lanePresent ? "Edit automation lane for "
                                                           : "Create automation lane for ")
                                 + parameterRow.modelData.name
                Accessible.description: parameterRow.modelData.note || ""
                Accessible.onPressAction: activate()
                Keys.onReturnPressed: activate()
                Keys.onEnterPressed: activate()
                Keys.onSpacePressed: activate()
                Keys.onUpPressed: editorPanel.focusRow(parameterRow.index - 1)
                Keys.onDownPressed: editorPanel.focusRow(parameterRow.index + 1)
                onActiveFocusChanged: parameterRow.reveal(activeFocus)
                function activate() {
                    if (enabled)
                        projectBridge.pluginAutomation.automate(parameterRow.modelData.id)
                }
                Text {
                    anchors.centerIn: parent
                    text: parameterRow.lanePresent ? "LANE " + parameterRow.modelData.pointCount : "AUTO"
                    color: automateButton.enabled ? editorPanel.accentColor : editorPanel.dimColor
                    font.pixelSize: Theme.typeCaption
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: automateButton.enabled
                    onClicked: automateButton.activate()
                }
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !parameterRow.canEdit
                text: parameterRow.editorEntry.value
                color: editorPanel.accentColor
                font.pixelSize: Theme.typeSmall
            }
            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: editorPanel.valueWidth
                height: editorPanel.rowControlHeight
                radius: Theme.radiusSm
                visible: parameterRow.canEdit
                color: editorPanel.panelColor
                border.color: valueInput.activeFocus ? editorPanel.accentColor
                                                    : editorPanel.lineColor
                TextInput {
                    id: valueInput
                    objectName: "vst3ParameterValue_" + parameterRow.modelData.id
                    anchors.fill: parent
                    anchors.leftMargin: Theme.fieldPadding
                    anchors.rightMargin: Theme.fieldPadding
                    verticalAlignment: TextInput.AlignVCenter
                    horizontalAlignment: TextInput.AlignRight
                    text: parameterRow.editorEntry.value === "—" ? ""
                                                                 : parameterRow.editorEntry.value
                    color: editorPanel.accentColor
                    font.pixelSize: Theme.typeSmall
                    selectByMouse: true
                    activeFocusOnTab: parameterRow.canEdit
                    validator: DoubleValidator { bottom: 0; top: 1; decimals: 3; locale: "C" }
                    Accessible.role: Accessible.EditableText
                    Accessible.name: "Normalized value for " + parameterRow.modelData.name
                                     + ", parameter ID " + parameterRow.modelData.id
                    Keys.onUpPressed: editorPanel.focusRow(parameterRow.index - 1)
                    Keys.onDownPressed: editorPanel.focusRow(parameterRow.index + 1)
                    onActiveFocusChanged: parameterRow.reveal(activeFocus)
                    onAccepted: {
                        if (acceptableInput && text.length > 0)
                            projectBridge.vst3Editor.set_vst3_insert_parameter(
                                parameterRow.modelData.id, Number(text));
                    }
                }
            }
        }
    }
    Text {
        objectName: "vst3ParameterEmpty"
        x: Theme.spaceLg
        y: parameterList.y + Theme.spaceMd
        width: parent.width - 2 * Theme.spaceLg
        visible: !editorPanel.automationMode && parameterList.count === 0
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        text: searchInput.text.trim().length > 0
              ? "No matching parameters. Clear the search (Esc) or try an ID."
              : "No parameters to show"
        color: editorPanel.dimColor
        font.pixelSize: Theme.typeSmall
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
