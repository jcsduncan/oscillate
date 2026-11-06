pragma ComponentBehavior: Bound
import QtQuick
import "../shell"

// One track in "Roles found in the loop": its colour,
// name and role, why that role, and a LOCK switch. Clicking the role opens
// the roles to choose from; "Found" goes back to what was detected.
Rectangle {
    id: card
    required property ArrangeTheme style
    // A controller track row: {id, name, role, roleIndex, detected,
    // evidence, locked, unsupported, hasContent, arranged}.
    required property var track
    required property var roleNames
    signal lockToggled(string trackId)
    signal rolePicked(string trackId, int role)

    property bool picking: false
    readonly property color tint: style.roleColor(track.roleIndex)
    readonly property bool idle: track.locked === true || track.arranged !== true

    implicitHeight: body.implicitHeight + 2 * style.roleCardInset
    radius: style.radiusXl
    color: style.panelRaised
    border.color: picking ? tint : style.line
    opacity: idle ? style.roleCardIdleOpacity : 1
    Accessible.role: Accessible.Grouping
    Accessible.name: track.name + ", " + track.role

    Rectangle {
        x: card.style.spaceMd
        y: card.style.roleCardInset
        width: card.style.roleStripeWidth
        height: parent.height - 2 * card.style.roleCardInset
        radius: card.style.radiusXs
        color: card.tint
    }
    Column {
        id: body
        x: card.style.roleCardBodyX
        y: card.style.roleCardInset
        width: parent.width - card.style.roleCardBodyX - card.style.spaceMd
        spacing: card.style.spaceXs
        Item {
            width: parent.width
            height: card.style.roleCardHeaderHeight
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - roleChip.width - lock.width - 2 * card.style.spaceSm
                text: card.track.name
                elide: Text.ElideRight
                color: card.style.textMain
                font.family: card.style.sansFamily
                font.pixelSize: card.style.roleNameSize
                font.weight: Font.DemiBold
            }
            Rectangle {
                id: roleChip
                objectName: "arrangeRole:" + card.track.id
                anchors.right: lock.left
                anchors.rightMargin: card.style.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                width: roleLabel.implicitWidth + card.style.roleChipPad
                height: card.style.roleTagHeight
                radius: card.style.radiusMd
                color: card.style.alpha(card.tint, roleArea.containsMouse ? card.style.roleChipHoverAlpha
                                                                          : card.style.roleChipWashAlpha)
                border.color: card.style.alpha(card.tint, card.style.roleChipEdgeAlpha)
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Role: " + card.track.role
                Keys.onReturnPressed: card.picking = !card.picking
                Keys.onSpacePressed: card.picking = !card.picking
                Text {
                    id: roleLabel
                    anchors.centerIn: parent
                    text: card.track.role.toUpperCase() + (card.track.detected ? "" : " ·")
                    color: card.tint
                    font.family: card.style.monoFamily
                    font.pixelSize: card.style.typeCaption
                    font.weight: Font.Bold
                    font.letterSpacing: card.style.arrangeCaptionSpacing
                }
                MouseArea {
                    id: roleArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: card.picking = !card.picking
                    Tip { text: "Change this track's role" }
                }
            }
            Rectangle {
                id: lock
                objectName: "arrangeLock:" + card.track.id
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: lockLabel.implicitWidth + card.style.roleTagPad
                height: card.style.roleTagHeight
                radius: card.style.radiusMd
                color: card.track.locked ? card.style.blue : "transparent"
                border.color: card.track.locked ? card.style.blue : card.style.lineStrong
                activeFocusOnTab: true
                Accessible.role: Accessible.CheckBox
                Accessible.name: "Lock " + card.track.name
                Accessible.checked: card.track.locked === true
                Keys.onReturnPressed: card.lockToggled(card.track.id)
                Keys.onSpacePressed: card.lockToggled(card.track.id)
                Text {
                    id: lockLabel
                    anchors.centerIn: parent
                    text: "LOCK"
                    color: card.track.locked ? card.style.canvas : card.style.textFaint
                    font.family: card.style.monoFamily
                    font.pixelSize: card.style.arrangeTagSize
                    font.weight: Font.Bold
                    font.letterSpacing: card.style.arrangeTagSpacing
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: card.lockToggled(card.track.id)
                    Tip { text: "Leave this track exactly as it is" }
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: String(card.track.unsupported || "").length > 0 ? card.track.unsupported
                  : card.track.locked ? "Locked: left exactly as it is"
                  : String(card.track.evidence || "")
            color: String(card.track.unsupported || "").length > 0 ? card.style.amber : card.style.textFaint
            font.family: card.style.sansFamily
            font.pixelSize: card.style.typeBody
            lineHeight: card.style.roleEvidenceLineHeight
        }
        Flow {
            visible: card.picking
            width: parent.width
            spacing: card.style.spaceXs
            Repeater {
                model: ["Found"].concat(card.roleNames)
                delegate: Rectangle {
                    id: choice
                    required property var modelData
                    required property int index
                    readonly property int role: index - 1
                    readonly property bool on: role >= 0 ? role === card.track.roleIndex && !card.track.detected
                                                         : card.track.detected === true
                    objectName: "arrangeRoleChoice:" + card.track.id + ":" + role
                    width: choiceLabel.implicitWidth + card.style.roleTagPad
                    height: card.style.roleTagHeight
                    radius: card.style.radiusMd
                    color: on ? card.style.alpha(role >= 0 ? card.style.roleColor(role) : card.style.accent,
                                                 card.style.roleChoiceWashAlpha)
                              : card.style.panel
                    border.color: role >= 0 ? card.style.alpha(card.style.roleColor(role), card.style.roleChoiceEdgeAlpha)
                                            : card.style.line
                    function activate() {
                        card.picking = false
                        card.rolePicked(card.track.id, choice.role)
                    }
                    Text {
                        id: choiceLabel
                        anchors.centerIn: parent
                        text: String(choice.modelData)
                        color: card.style.textMain
                        font.family: card.style.sansFamily
                        font.pixelSize: card.style.roleChoiceSize
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: choice.activate()
                    }
                }
            }
        }
    }
}
