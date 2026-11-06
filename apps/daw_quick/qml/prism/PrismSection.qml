pragma ComponentBehavior: Bound
import QtQuick

// A titled card. Children go in the body; `trailing` sits at the header's right.
Rectangle {
    id: section
    required property PrismTheme theme
    required property string title
    property color accent: theme.osc1
    default property alias content: body.data
    property alias trailing: trailingSlot.data
    readonly property real bodyWidth: body.width
    readonly property real bodyHeight: body.height

    radius: theme.sectionRadius
    color: theme.panel
    border.color: theme.line
    Accessible.role: Accessible.Grouping
    Accessible.name: title

    Rectangle {
        x: section.theme.sectionBarX
        y: section.theme.sectionBarY
        width: section.theme.sectionBarWidth
        height: section.theme.sectionBarHeight
        radius: width / 2
        color: section.accent
    }
    Text {
        x: section.theme.sectionTitleX
        y: section.theme.sectionTitleY
        text: section.title.toUpperCase()
        color: section.theme.text
        font.pixelSize: section.theme.typeBody
        font.bold: true
        font.letterSpacing: section.theme.sectionTitleSpacing
    }
    Row {
        id: trailingSlot
        anchors.right: parent.right
        anchors.rightMargin: section.theme.sectionPad
        y: section.theme.sectionTrailingY
        height: section.theme.sectionTrailingHeight
        spacing: section.theme.spaceSm
    }
    Item {
        id: body
        x: section.theme.sectionPad
        y: section.theme.sectionBodyY
        width: parent.width - 2 * section.theme.sectionPad
        height: parent.height - section.theme.sectionBodyY - section.theme.sectionBodyBottom
    }
}
