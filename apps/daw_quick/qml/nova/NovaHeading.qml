pragma ComponentBehavior: Bound
import QtQuick

// A section heading inside a card's controls ("SUB", "OUT"):
// faint, wide-tracked capitals.
Text {
    required property var panel
    readonly property real headingSize: panel.theme.typeSmall
    readonly property real headingSpacing: panel.theme.headingSpacing
    color: panel.theme.faint
    font.family: panel.theme.sans
    font.pixelSize: headingSize
    font.weight: Font.ExtraBold
    font.letterSpacing: headingSpacing
}
