import QtQuick

// A Mix Check card: a panel with a column of content.
Rectangle {
    id: card
    required property MixCheckTheme style
    default property alias content: column.data
    property alias spacing: column.spacing
    readonly property real innerWidth: width - 2 * style.cardPadding

    implicitHeight: column.implicitHeight + 2 * style.cardPadding
    radius: style.cardRadius
    color: style.panel
    border.color: style.line

    Column {
        id: column
        x: card.style.cardPadding
        y: card.style.cardPadding
        width: card.innerWidth
        spacing: card.style.cardSpacing
    }
}
