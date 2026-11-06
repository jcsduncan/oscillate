pragma ComponentBehavior: Bound
import QtQuick

// A panel section with an engraved title strip. Children go in the body;
// `trailing` sits at the right of the title strip.
Rectangle {
    id: section
    required property EmberTheme theme
    required property string title
    property color accent: theme.amber
    default property alias content: body.data
    property alias trailing: trailingSlot.data
    readonly property real bodyWidth: body.width
    readonly property real bodyHeight: body.height

    radius: theme.radiusLg
    color: theme.panel
    border.color: theme.line
    Accessible.role: Accessible.Grouping
    Accessible.name: title

    Rectangle {
        id: strip
        x: 1
        y: 1
        width: parent.width - 2
        height: section.theme.sectionStripHeight
        radius: section.theme.sectionStripRadius
        color: section.theme.engraving
        Rectangle {
            x: 0
            y: parent.height - section.theme.strokeBold
            width: parent.width
            height: section.theme.strokeBold
            color: section.accent
            opacity: section.theme.sectionUnderlineOpacity
        }
        Text {
            x: section.theme.sectionTitleX
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -1
            text: section.title.toUpperCase()
            color: section.theme.cream
            font.pixelSize: section.theme.typeSmall
            font.bold: true
            font.letterSpacing: section.theme.sectionTitleSpacing
        }
        Row {
            id: trailingSlot
            anchors.right: parent.right
            anchors.rightMargin: section.theme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -1
            height: section.theme.stripControlHeight
            spacing: section.theme.spaceXs
        }
    }
    Item {
        id: body
        x: section.theme.sectionPad
        y: strip.height + section.theme.sectionPad
        width: parent.width - 2 * section.theme.sectionPad
        height: parent.height - strip.height - section.theme.sectionPad - section.theme.sectionFoot
    }
}
