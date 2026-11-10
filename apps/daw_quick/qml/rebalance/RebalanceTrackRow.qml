import QtQuick
import "../shell"

// One track in the role list: its colour and name (indented under the
// group it plays into), why it has its role, and the role as a pill that
// opens the override menu. A chosen role's pill is outlined.
Item {
    id: row
    required property RebalanceTheme style
    // {id, name, color, order, kind, nested, role, roleName, reason, chosen}.
    required property var track
    signal menuRequested(Item anchor)

    readonly property color roleColor: style.roleColor(track.role)
    objectName: "rebalanceTrack:" + track.id
    implicitHeight: style.rowHeight
    Accessible.role: Accessible.Row
    Accessible.name: track.name + ": " + track.roleName + ", " + track.reason

    Rectangle {
        anchors.fill: parent
        radius: row.style.radiusMd
        color: rowHover.hovered ? row.style.selection : "transparent"
        HoverHandler { id: rowHover }
    }
    Rectangle {
        id: swatch
        x: row.track.nested ? row.style.rowIndent : row.style.spaceXs
        anchors.verticalCenter: parent.verticalCenter
        width: row.style.swatchWidth
        height: row.height - 2 * row.style.spaceSm
        radius: width / 2
        color: row.style.trackColor(row.track, row.track.order)
    }
    Text {
        id: name
        objectName: "rebalanceTrackName:" + row.track.id
        readonly property bool tipShowsWhole: true
        anchors.left: swatch.right
        anchors.leftMargin: row.style.spaceMd
        anchors.right: reason.left
        anchors.rightMargin: row.style.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        text: row.track.name
        color: row.track.kind === "group" || row.track.kind === "return" ? row.style.textDim : row.style.textMain
        font.pixelSize: row.style.typeBody
        font.bold: row.track.kind === "group"
        Tip { text: row.track.name }
    }
    Text {
        id: reason
        objectName: "rebalanceReason:" + row.track.id
        readonly property bool tipShowsWhole: true
        anchors.right: pill.left
        anchors.rightMargin: row.style.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        width: row.style.reasonWidth
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideRight
        text: row.track.reason
        color: row.track.chosen ? row.roleColor : row.style.textFaint
        font.pixelSize: row.style.typeSmall
        font.italic: !row.track.chosen
        Tip { text: "Why: " + row.track.reason }
    }
    Rectangle {
        id: pill
        objectName: "rebalanceRolePill:" + row.track.id
        anchors.right: parent.right
        anchors.rightMargin: row.style.spaceXs
        anchors.verticalCenter: parent.verticalCenter
        width: row.style.pillWidth
        height: row.style.pillHeight
        radius: height / 2
        color: pillArea.containsMouse ? row.style.buttonFace : row.style.panelRaised
        border.color: row.track.chosen ? row.roleColor : row.style.line
        border.width: row.track.chosen ? row.style.chosenEdge : row.style.hairline
        activeFocusOnTab: true
        Accessible.role: Accessible.ComboBox
        Accessible.name: row.track.name + " role"
        Accessible.description: row.track.roleName
        Keys.onReturnPressed: row.menuRequested(pill)
        Keys.onSpacePressed: row.menuRequested(pill)
        Rectangle {
            id: pillDot
            x: row.style.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            width: row.style.roleDotSize - row.style.spaceXxs
            height: width
            radius: width / 2
            color: row.roleColor
        }
        Text {
            anchors.left: pillDot.right
            anchors.leftMargin: row.style.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            text: row.track.roleName
            color: row.style.textMain
            font.pixelSize: row.style.typeSmall
            font.bold: row.track.chosen
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: row.style.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            text: "▾"
            color: row.style.textDim
            font.pixelSize: row.style.typeSmall
        }
        MouseArea {
            id: pillArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.menuRequested(pill)
            Tip { text: "Choose the role, or Auto to infer it" }
        }
    }
}
