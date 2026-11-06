pragma ComponentBehavior: Bound
import QtQuick

// A section card: a softly lit graphite plate with an optional on LED, a
// wide-tracked title and a faint subtitle. Contents are placed by the user.
// The module the signal spine opened (ADR-0076) is `focused`: a border and
// glow in its accent. A click on the card's bare plate asks for focus.
Rectangle {
    id: card
    required property var panel
    property string title: ""
    property string sub: ""
    property color titleColor: panel.theme.text
    // An [Off, On] parameter: shows the LED and dims the card while off.
    property string ledParameter: ""
    readonly property bool off: ledParameter !== "" && panel.controller.revision >= 0
        && panel.controller.choice(ledParameter) === 0
    // Where the title row ends, for header controls placed after it.
    readonly property real titleRight: titleText.x + titleText.implicitWidth
    property bool dimWhenOff: true
    property bool focused: false
    property color focusColor: panel.theme.cy
    signal focusRequested()

    radius: panel.theme.moduleCardRadius
    border.color: focused ? focusColor : panel.theme.line
    border.width: focused ? panel.theme.strokeBold : panel.theme.hairline
    gradient: Gradient {
        GradientStop { position: 0; color: card.panel.theme.card }
        GradientStop { position: 1; color: card.panel.theme.cardBottom }
    }
    opacity: off && dimWhenOff ? panel.theme.cardOffOpacity : 1
    Behavior on opacity { NumberAnimation { duration: card.panel.theme.cardDimDuration } }

    // The focus glow: a soft halo just outside the border.
    Rectangle {
        visible: card.focused
        anchors.fill: parent
        anchors.margins: -card.panel.theme.cardGlowWidth
        radius: card.radius + card.panel.theme.cardGlowWidth
        z: -1
        color: "transparent"
        border.width: card.panel.theme.cardGlowWidth
        border.color: card.panel.theme.soft(card.focusColor, card.panel.theme.cardGlowAlpha)
    }
    // Under every control, so only the bare plate takes the click.
    MouseArea {
        anchors.fill: parent
        onClicked: card.focusRequested()
    }

    Loader {
        active: card.ledParameter !== ""
        x: card.panel.theme.cardInsetX
        y: card.panel.theme.cardTitleTop
        sourceComponent: NovaLed {
            panel: card.panel
            parameterId: card.ledParameter
        }
    }
    Text {
        id: titleText
        visible: card.title !== ""
        x: card.ledParameter !== "" ? card.panel.theme.cardTitleAfterLed : card.panel.theme.cardInsetX
        y: card.panel.theme.cardTitleTop
        text: card.title
        color: card.titleColor
        font.family: card.panel.theme.sans
        font.pixelSize: card.panel.theme.cardTitleSize
        font.weight: Font.ExtraBold
        font.letterSpacing: card.panel.theme.cardTitleSpacing
    }
    Text {
        visible: card.sub !== ""
        x: card.titleRight + card.panel.theme.spaceMd
        y: card.panel.theme.cardSubTop
        text: card.sub
        color: card.panel.theme.faint
        font.family: card.panel.theme.sans
        font.pixelSize: card.panel.theme.typeSmall
        font.letterSpacing: card.panel.theme.cardSubSpacing
    }
}
