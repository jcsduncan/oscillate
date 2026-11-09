pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: routing
    required property var bridge
    required property var track
    required property var destinations
    required property int destinationCount
    required property color panelRaised
    required property color lineColor
    required property color textMain
    required property color textDim
    required property color accent
    property string chosenSendId: ""
    property bool mainMenuOpen: false
    property bool sendMenuOpen: false
    // Rows from the top; the send rows push everything below them down.
    readonly property int busButtonsY: 22
    readonly property int buttonHeight: 27
    readonly property int fieldHeight: 26
    readonly property int mainLabelY: 57
    readonly property int mainButtonY: 73
    readonly property int mainMenuY: 101
    readonly property int menuMaxRows: 6
    readonly property int sendLabelY: 110
    readonly property int sendControlsY: 127
    readonly property int newSendGainWidth: 46
    readonly property int addSendWidth: 47
    readonly property int sendMenuY: 155
    readonly property int sendsLabelY: 165
    readonly property int sendRowY: 181
    readonly property int sendRowPitch: 29
    readonly property int sendRowTextRight: 92
    readonly property int sendRowGainRight: 90
    readonly property int sendRowGainWidth: 51
    readonly property int removeSendWidth: 32
    readonly property int footnoteY: 190
    readonly property int sidechainHintY: 218
    readonly property int channelControlsY: 250
    readonly property int channelControlsHeight: 100
    readonly property var mainOptions: {
        const options = [{ id: "", name: "Master", kind: "master" }]
        for (const target of destinations) {
            if (target.kind === "group" && target.id !== track.id)
                options.push(target)
        }
        return options
    }
    readonly property var sendOptions: {
        const options = []
        for (const target of destinations) {
            if (target.id !== track.id)
                options.push(target)
        }
        return options
    }
    function chosenSendName() {
        for (const target of sendOptions) {
            if (target.id === chosenSendId)
                return target.name
        }
        return "Choose bus"
    }
    component RouteButton: Rectangle {
        id: button
        required property string label
        property bool actionable: true
        signal activated()
        radius: Theme.controlRadius
        color: actionable ? routing.panelRaised : Theme.routeButtonOff
        border.color: activeFocus ? routing.accent : routing.lineColor
        enabled: actionable
        activeFocusOnTab: actionable
        Accessible.role: Accessible.Button
        Accessible.name: label
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text {
            anchors.centerIn: parent
            width: parent.width - Theme.spaceMd
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            text: button.label
            color: button.actionable ? routing.textMain : routing.textDim
            font.pixelSize: Theme.typeCaption
        }
        MouseArea {
            anchors.fill: parent
            enabled: button.actionable
            onClicked: button.activated()
            Tip {}
        }
    }

    Text {
        y: 0
        text: "SIGNAL FLOW"
        color: routing.textDim
        font.pixelSize: Theme.typeSmall
        font.bold: true
        font.letterSpacing: Theme.trackingXl
    }
    RouteButton {
        objectName: "createGroupBus"
        x: 0; y: routing.busButtonsY; width: (routing.width - Theme.spaceSm) / 2; height: routing.buttonHeight
        label: "+ GROUP BUS"
        onActivated: routing.bridge.trackCommands.add_bus_track("group")
    }
    RouteButton {
        objectName: "createReturnBus"
        x: (routing.width + Theme.spaceSm) / 2; y: routing.busButtonsY; width: (routing.width - Theme.spaceSm) / 2; height: routing.buttonHeight
        label: "+ RETURN BUS"
        onActivated: routing.bridge.trackCommands.add_bus_track("return")
    }
    Text {
        y: routing.mainLabelY
        text: "MAIN OUTPUT"
        color: routing.textDim
        font.pixelSize: Theme.typeCaption
    }
    RouteButton {
        id: mainButton
        objectName: "mainRouteButton"
        x: 0; y: routing.mainButtonY; width: routing.width; height: routing.fieldHeight
        label: "→ " + (routing.track.mainDestinationName || "Master") + "  ▾"
        onActivated: { routing.mainMenuOpen = !routing.mainMenuOpen; routing.sendMenuOpen = false }
    }
    // A press outside the open route menu (or its button) closes it;
    // Escape too. Presses on the menu and button pass through.
    MouseArea {
        parent: routing.Window.window ? routing.Window.window.contentItem : routing
        anchors.fill: parent
        z: 1000
        visible: routing.mainMenuOpen || routing.sendMenuOpen
        onPressed: mouse => {
            const keep = [mainMenu, mainButton, sendMenu, sendButton].some(
                item => item.visible && item.contains(mapToItem(item, mouse.x, mouse.y)))
            if (keep) {
                mouse.accepted = false
                return
            }
            routing.mainMenuOpen = false
            routing.sendMenuOpen = false
        }
    }
    Rectangle {
        id: mainMenu
        visible: routing.mainMenuOpen
        onVisibleChanged: if (visible) forceActiveFocus()
        Keys.onEscapePressed: routing.mainMenuOpen = false
        x: 0; y: routing.mainMenuY; width: routing.width
        height: Math.min(routing.menuMaxRows, routing.mainOptions.length) * routing.buttonHeight + 2 * Theme.hairline
        z: 30
        color: routing.panelRaised
        border.color: routing.accent
        ListView {
            anchors.fill: parent
            anchors.margins: Theme.hairline
            clip: true
            model: routing.mainOptions
            delegate: RouteButton {
                required property var modelData
                objectName: "mainRouteChoice"
                width: ListView.view.width
                height: routing.buttonHeight
                label: "→ " + modelData.name
                onActivated: {
                    routing.mainMenuOpen = false
                    routing.bridge.routeCommands.set_main_route(routing.track.id, modelData.id)
                }
            }
        }
    }
    Text {
        y: routing.sendLabelY
        text: "ADD SEND"
        color: routing.textDim
        font.pixelSize: Theme.typeCaption
    }
    RouteButton {
        id: sendButton
        objectName: "sendDestinationButton"
        x: 0; y: routing.sendControlsY; width: routing.width - routing.addSendWidth - routing.newSendGainWidth - 2 * Theme.spaceSm; height: routing.fieldHeight
        label: routing.chosenSendName() + "  ▾"
        actionable: routing.sendOptions.length > 0
        onActivated: { routing.sendMenuOpen = !routing.sendMenuOpen; routing.mainMenuOpen = false }
    }
    Rectangle {
        x: routing.width - routing.addSendWidth - Theme.spaceSm - routing.newSendGainWidth; y: routing.sendControlsY; width: routing.newSendGainWidth; height: routing.fieldHeight
        radius: Theme.controlRadius; color: routing.panelRaised; border.color: routing.lineColor
        TextInput {
            id: newSendGain
            objectName: "newSendGain"
            anchors.fill: parent
            anchors.margins: Theme.fieldPadding
            text: "0.50"
            color: routing.textMain
            font.pixelSize: Theme.typeSmall
            validator: DoubleValidator { bottom: 0; top: 4; decimals: 2; locale: "C" }
            activeFocusOnTab: true
            Accessible.name: "New send gain, 0 to 4"
        }
    }
    RouteButton {
        objectName: "addSendButton"
        x: routing.width - routing.addSendWidth; y: routing.sendControlsY; width: routing.addSendWidth; height: routing.fieldHeight
        label: "ADD"
        actionable: routing.chosenSendId.length > 0 && routing.track.sendCount < 32 &&
            newSendGain.acceptableInput && newSendGain.text.length > 0
        onActivated: routing.bridge.routeCommands.add_send_route(routing.track.id, routing.chosenSendId, Number(newSendGain.text))
    }
    Rectangle {
        id: sendMenu
        visible: routing.sendMenuOpen
        onVisibleChanged: if (visible) forceActiveFocus()
        Keys.onEscapePressed: routing.sendMenuOpen = false
        x: 0; y: routing.sendMenuY; width: sendButton.width
        height: Math.min(routing.menuMaxRows, routing.sendOptions.length) * routing.buttonHeight + 2 * Theme.hairline
        z: 30
        color: routing.panelRaised
        border.color: routing.accent
        ListView {
            anchors.fill: parent
            anchors.margins: Theme.hairline
            clip: true
            model: routing.sendOptions
            delegate: RouteButton {
                required property var modelData
                objectName: "sendRouteChoice"
                width: ListView.view.width
                height: routing.buttonHeight
                label: modelData.name
                onActivated: {
                    routing.chosenSendId = modelData.id
                    routing.sendMenuOpen = false
                }
            }
        }
    }
    Text {
        y: routing.sendsLabelY
        // Up to 32 per track, sidechain keys included (the model's limit).
        text: "SENDS · " + (routing.track.sendCount || 0)
        color: routing.textDim
        font.pixelSize: Theme.typeCaption
    }
    Repeater {
        model: routing.track.sends || []
        delegate: Item {
            id: sendRow
            objectName: "routingSendRow"
            required property var modelData
            required property int index
            x: 0; y: routing.sendRowY + index * routing.sendRowPitch
            width: routing.width; height: routing.buttonHeight
            Text {
                x: 0
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - routing.sendRowTextRight
                elide: Text.ElideRight
                text: sendRow.modelData.purpose === "sidechain"
                    ? "SC → " + sendRow.modelData.destinationName + " · " + sendRow.modelData.targetName
                    : "↗ " + sendRow.modelData.destinationName
                color: routing.textMain
                font.pixelSize: Theme.typeSmall
            }
            Rectangle {
                x: parent.width - routing.sendRowGainRight; width: routing.sendRowGainWidth; height: routing.fieldHeight
                radius: Theme.controlRadius; color: routing.panelRaised; border.color: routing.lineColor
                TextInput {
                    objectName: "sendGainInput"
                    anchors.fill: parent
                    anchors.margins: Theme.fieldPadding
                    text: Number(sendRow.modelData.gain).toFixed(2)
                    color: routing.textMain
                    font.pixelSize: Theme.typeSmall
                    validator: DoubleValidator { bottom: 0; top: 4; decimals: 2; locale: "C" }
                    activeFocusOnTab: true
                    Accessible.name: "Gain to " + sendRow.modelData.destinationName
                    onEditingFinished: {
                        if (acceptableInput && text.length > 0 &&
                            Math.abs(Number(text) - Number(sendRow.modelData.gain)) > 0.000001)
                            routing.bridge.routeCommands.set_send_gain(sendRow.modelData.id, Number(text))
                    }
                }
            }
            RouteButton {
                objectName: "removeSendButton"
                x: parent.width - routing.removeSendWidth; width: routing.removeSendWidth; height: routing.fieldHeight
                label: "×"
                Accessible.name: "Remove send to " + sendRow.modelData.destinationName
                onActivated: routing.bridge.routeCommands.remove_send(sendRow.modelData.id)
            }
        }
    }
    Text {
        y: routing.footnoteY + (routing.track.sendCount || 0) * routing.sendRowPitch
        width: parent.width
        wrapMode: Text.WordWrap
        text: routing.destinationCount > routing.destinations.length
            ? "First 16 buses shown. Other saved routes remain intact."
            : "Save routes to keep them. Stop and Play to hear edits made during playback."
        color: routing.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        objectName: "sidechainHint"
        y: routing.sidechainHintY + (routing.track.sendCount || 0) * routing.sendRowPitch
        width: parent.width
        wrapMode: Text.WordWrap
        text: "Sidechains are chosen on the effect they key: open it and pick a source."
        color: routing.textDim
        font.pixelSize: Theme.typeCaption
    }
    ChannelControls {
        objectName: "trackChannelControls"
        visible: routing.track.id !== ""
        y: routing.channelControlsY + (routing.track.sendCount || 0) * routing.sendRowPitch
        width: routing.width
        height: routing.channelControlsHeight
        bridge: routing.bridge
        track: routing.track
        panelRaised: routing.panelRaised
        lineColor: routing.lineColor
        textMain: routing.textMain
        textDim: routing.textDim
        accent: routing.accent
    }
}
