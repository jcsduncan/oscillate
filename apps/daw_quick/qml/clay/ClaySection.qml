import QtQuick

// A side panel section: a dotted heading, an optional
// hint and the "ours" badge for the tools only a DAW that owns its synths
// can offer, then its controls in a column.
Item {
    id: section
    required property ClayTheme style
    property string title: ""
    property string hint: ""
    property bool ours: false
    default property alias content: body.data
    readonly property real padding: style.sectionPadding

    width: parent ? parent.width : style.sectionDefaultWidth
    height: visible ? body.y + body.implicitHeight + padding : 0

    Row {
        id: heading
        x: section.padding; y: section.padding
        spacing: section.style.spaceMd
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: section.style.sectionDotSize; height: section.style.sectionDotSize
            radius: section.style.sectionDotSize / 2
            color: section.style.clay
        }
        Text {
            text: section.title
            color: section.style.dim
            font.family: section.style.mono; font.pixelSize: section.style.typeSmall
            font.weight: Font.Bold; font.letterSpacing: section.style.sectionTitleSpacing
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: section.hint.length > 0
            text: section.hint
            color: section.style.faint
            font.family: section.style.mono; font.pixelSize: section.style.typeCaption
        }
    }
    Rectangle {
        visible: section.ours
        anchors.right: parent.right; anchors.rightMargin: section.padding
        y: section.padding - section.style.spaceXxs
        width: oursText.implicitWidth + section.style.badgePad; height: section.style.badgeHeight
        radius: section.style.badgeHeight / 2
        color: "transparent"
        border.color: section.style.badgeEdge
        Text {
            id: oursText
            anchors.centerIn: parent
            text: "ONLY HERE"
            color: section.style.clay2
            font.family: section.style.mono; font.pixelSize: section.style.typeCaption; font.weight: Font.DemiBold
            font.letterSpacing: section.style.badgeSpacing
        }
    }
    Column {
        id: body
        x: section.padding
        y: heading.y + heading.height + section.style.sectionGap
        width: section.width - 2 * section.padding
        spacing: section.style.sectionGap
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: section.style.hairline
        color: section.style.line
    }
}
