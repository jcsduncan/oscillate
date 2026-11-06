import QtQuick

// A titled plate grouping a panel's controls, with an optional note beside
// the title. Panels place their controls inside it.
Rectangle {
    id: card
    required property KitTheme theme
    required property string title
    property string note: ""
    property color noteColor: theme.inkFaint
    radius: theme.cardRadius
    color: theme.plate
    border.color: theme.plateEdge
    // A faint sheen along the top edge.
    Rectangle {
        x: 1
        y: 1
        width: parent.width - 2
        height: 1
        color: card.theme.cardSheen
    }
    Text {
        id: cardTitle
        x: card.theme.cardTitleX
        y: card.theme.cardTitleY
        text: card.title
        color: card.theme.cardTitleInk
        font.family: card.theme.cardTitleFont
        font.pixelSize: card.theme.cardTitleSize
        font.bold: true
        font.letterSpacing: card.theme.cardTitleSpacing
    }
    Text {
        x: cardTitle.x + cardTitle.implicitWidth + card.theme.cardNoteGap
        y: cardTitle.y + 1
        visible: card.note !== ""
        text: card.note
        color: card.noteColor
        font.pixelSize: card.theme.cardNoteSize
        font.bold: card.theme.cardNoteBold
        font.letterSpacing: card.theme.cardNoteSpacing
    }
}
