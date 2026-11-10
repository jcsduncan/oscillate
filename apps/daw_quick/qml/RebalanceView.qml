pragma ComponentBehavior: Bound
import QtQuick
import "rebalance"
import "devicekit" as Kit
import "shell"

// Rebalance (ADR-0123), opened from the master: each part of the mix
// (vocals, drums, bass, music) gets a gain and a three-band EQ that act on
// its own tracks' outputs, so moving a role on the master moves those tracks
// exactly. Left, one card per role with its meter; right, every track's
// role, why it has it and a menu to choose another. Listen hears the audio
// tracks for their part. `controller` is projectBridge.rebalance; `meters`
// is the bridge's MeterModel (a role's meter is its channel "role:<id>").
Rectangle {
    id: view
    objectName: "rebalanceView"
    required property var controller
    required property var meters

    // The mixer, from the header's button.
    signal closeRequested()

    readonly property RebalanceTheme style: RebalanceTheme {}
    readonly property bool enabled_: controller !== null && controller.enabled === true
    readonly property var tracks: controller !== null ? controller.tracks : []

    function openRoleMenu(anchor, track) {
        const items = []
        const choices = controller.choices
        for (let i = 0; i < choices.length; ++i)
            items.push({ label: choices[i].name, value: choices[i].id })
        menu.open(anchor, items, track.chosen ? track.role : "auto", style.roleColor(track.role),
                  value => view.controller.setTrackRole(track.id, value))
    }

    color: style.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: "Rebalance"

    // Title, status and the actions.
    Item {
        id: header
        x: view.style.viewMargin
        y: view.style.viewMargin
        width: view.width - 2 * view.style.viewMargin
        height: view.style.headerHeight
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: view.style.spaceXxs
            Text {
                objectName: "rebalanceTitle"
                text: "Rebalance"
                color: view.style.textMain
                font.family: view.style.sansFamily
                font.pixelSize: view.style.titleSize
                font.weight: Font.ExtraBold
                font.letterSpacing: view.style.titleSpacing
            }
            Text {
                text: "Each part of the mix, moved on the master through its own tracks"
                color: view.style.textDim
                font.pixelSize: view.style.typeBody
            }
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: view.style.spaceLg
            Text {
                objectName: "rebalanceStatus"
                anchors.verticalCenter: parent.verticalCenter
                text: view.controller !== null ? view.controller.status : ""
                color: view.style.textDim
                font.pixelSize: view.style.typeSmall
            }
            // On / off: off, every role is flat whatever it is set to.
            Rectangle {
                id: power
                objectName: "rebalanceEnabled"
                anchors.verticalCenter: parent.verticalCenter
                width: view.style.switchWidth
                height: view.style.buttonHeight
                radius: height / 2
                color: view.enabled_ ? view.style.accent : view.style.panelRaised
                border.color: view.enabled_ ? view.style.accent : view.style.lineStrong
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                Accessible.role: Accessible.CheckBox
                Accessible.name: "Rebalance on"
                Accessible.checked: view.enabled_
                Text {
                    anchors.centerIn: parent
                    text: view.enabled_ ? "ON" : "OFF"
                    color: view.enabled_ ? view.style.onAccentText : view.style.textDim
                    font.pixelSize: view.style.typeCaption
                    font.bold: true
                    font.letterSpacing: view.style.trackingSm
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: view.controller.enabled = !view.enabled_
                    Tip { text: "Turn the rebalance on or off; off, the mix plays as its tracks are set" }
                }
            }
            HeaderButton {
                objectName: "rebalanceListen"
                readonly property bool busy: view.controller !== null && view.controller.listening === true
                label: busy ? "Stop" : "Listen"
                tip: "Hear each audio track for the part it plays; the result is one undo step"
                border.color: busy ? view.style.accent : view.style.lineStrong
                Accessible.name: busy ? "Stop listening" : "Listen to the audio tracks"
                onClicked: busy ? view.controller.cancelListening() : view.controller.listen()
            }
            HeaderButton {
                objectName: "rebalanceClose"
                label: "Mixer"
                tip: "Back to the mixer (Esc)"
                Accessible.name: "Back to the mixer"
                onClicked: view.closeRequested()
            }
        }
    }

    // A header action.
    component HeaderButton: Rectangle {
        id: button
        property string label
        property string tip
        signal clicked()
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        width: buttonLabel.implicitWidth + view.style.buttonPadding
        height: view.style.buttonHeight
        radius: view.style.controlRadius
        color: buttonArea.containsMouse ? view.style.buttonFace : view.style.panelRaised
        border.color: view.style.lineStrong
        Accessible.role: Accessible.Button
        Text {
            id: buttonLabel
            anchors.centerIn: parent
            text: button.label
            color: view.style.textMain
            font.pixelSize: view.style.typeBody
        }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
            Tip { text: button.tip }
        }
    }

    // One card per role.
    Flickable {
        id: cards
        objectName: "rebalanceCards"
        anchors.left: parent.left
        anchors.leftMargin: view.style.viewMargin
        anchors.top: header.bottom
        anchors.topMargin: view.style.sectionGap
        anchors.bottom: parent.bottom
        anchors.bottomMargin: view.style.viewMargin
        anchors.right: list.left
        anchors.rightMargin: view.style.sectionGap
        contentWidth: cardRow.width
        contentHeight: cardRow.height
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Row {
            id: cardRow
            spacing: view.style.cardGap
            Repeater {
                id: roleCards
                model: view.controller !== null ? view.controller.roles : []
                delegate: RebalanceRoleCard {
                    required property var modelData
                    // The cards share the width beside the list.
                    width: Math.max(view.style.cardWidth,
                                    (cards.width - (roleCards.count - 1) * view.style.cardGap) / roleCards.count)
                    height: cards.height
                    style: view.style
                    role: modelData
                    meters: view.meters
                    lowHz: view.controller.curveLowHz
                    highHz: view.controller.curveHighHz
                    rangeDb: view.controller.rangeDb
                    active: view.enabled_
                }
            }
        }
    }

    // Every track's role.
    Rectangle {
        id: list
        objectName: "rebalanceTrackList"
        anchors.right: parent.right
        anchors.rightMargin: view.style.viewMargin
        anchors.top: header.bottom
        anchors.topMargin: view.style.sectionGap
        anchors.bottom: parent.bottom
        anchors.bottomMargin: view.style.viewMargin
        width: view.style.listWidth
        radius: view.style.cardRadius
        color: view.style.panel
        border.color: view.style.line
        Item {
            id: listHeader
            x: view.style.cardPadding
            y: view.style.cardPadding
            width: parent.width - 2 * view.style.cardPadding
            height: view.style.cardHeaderHeight
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "TRACKS"
                color: view.style.textMain
                font.pixelSize: view.style.typeBody
                font.bold: true
                font.letterSpacing: view.style.trackingXl
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "WHY · ROLE"
                color: view.style.textFaint
                font.pixelSize: view.style.typeMicro
                font.letterSpacing: view.style.trackingXxs
            }
        }
        ListView {
            id: rows
            objectName: "rebalanceTrackRows"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: listHeader.bottom
            anchors.bottom: parent.bottom
            anchors.margins: view.style.cardPadding
            anchors.topMargin: view.style.spaceMd
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: view.tracks
            delegate: RebalanceTrackRow {
                required property var modelData
                width: rows.width
                style: view.style
                track: modelData
                onMenuRequested: anchor => view.openRoleMenu(anchor, modelData)
            }
        }
        Text {
            anchors.centerIn: parent
            visible: view.tracks.length === 0
            text: "Add tracks to rebalance them"
            color: view.style.textFaint
            font.pixelSize: view.style.typeBody
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: view.style
    }
}
