import QtQuick

// A card's heading row: the small uppercase title on the left, a dim hint
// on the right.
Item {
    id: head
    required property MixCheckTheme style
    property string title: ""
    property string hint: ""
    property color hintColor: style.textFaint

    implicitHeight: Math.max(titleText.implicitHeight, hintText.implicitHeight)
    Text {
        id: titleText
        anchors.verticalCenter: parent.verticalCenter
        text: head.title.toUpperCase()
        color: head.style.textFaint
        font.family: head.style.sansFamily
        font.pixelSize: head.style.typeSmall
        font.weight: Font.Bold
        font.letterSpacing: head.style.headingLetterSpacing
    }
    Text {
        id: hintText
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, head.width - titleText.implicitWidth - head.style.spaceXl)
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideLeft
        text: head.hint
        color: head.hintColor
        font.family: head.style.sansFamily
        font.pixelSize: head.style.typeBody
    }
}
