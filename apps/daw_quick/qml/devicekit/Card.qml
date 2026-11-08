import QtQuick

// A titled plate grouping a panel's controls, with an optional note beside
// the title. Panels place their controls inside it.
Rectangle {
    id: card
    required property KitTheme theme
    required property string title
    property string note: ""
    property color noteColor: theme.inkFaint
    // Whether the note ends inside the card, as far in from the right edge
    // as the title sits from the left.
    readonly property bool noteFits: note === ""
        || cardNote.x + cardNote.implicitWidth <= width - theme.cardTitleX
    // The longest note the card shows (it may change while playing): the
    // card's width is at least fitWidth, so that note ends inside it.
    property string longestNote: note
    readonly property real fitWidth: cardTitle.x + cardTitle.implicitWidth
        + (longestNote === "" ? 0 : theme.cardNoteGap + noteMetrics.implicitWidth)
        + theme.cardTitleX
    // The longest note laid out as the note lays it out, tracking and
    // hinting included; never shown.
    Text {
        id: noteMetrics
        visible: false
        text: card.longestNote
        font: cardNote.font
    }
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
        id: cardNote
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
