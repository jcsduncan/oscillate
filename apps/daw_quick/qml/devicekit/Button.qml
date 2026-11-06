import QtQuick
import "../shell" as Shell

// A push button: plain, on (a toggle that is set), prime (the main action)
// or done (an applied action), in its accent. `activate()` is the click, for
// keys and tests; `active: false` leaves it shown but inert. The theme's
// pushButton tokens and its pushButtonFill/Edge/Ink functions give it its
// look; `small` and `large` pick the theme's other sizes. `label` is its text.
// `interactive: false` ignores the pointer while keys still work (a panel
// that is not editable).
Rectangle {
    id: button
    required property KitTheme theme
    property string label: ""
    property string tip: ""
    property string shortcut: ""
    property bool on: false
    property bool prime: false
    property bool done: false
    property bool small: false
    property bool large: false
    property bool active: true
    property bool interactive: true
    property color accent: theme.accent
    // The text on the accent fill.
    property color accentInk: theme.accentInk
    readonly property bool lit: on || prime
    readonly property bool usable: enabled && active
    readonly property bool hovered: area.containsMouse
    readonly property bool pressed: area.pressed
    signal clicked()
    function activate() {
        if (usable)
            clicked()
    }

    implicitWidth: Math.max(theme.pushButtonMinWidth,
                            caption.implicitWidth + (large ? theme.pushButtonPadLarge
                                                     : small ? theme.pushButtonPadSmall : theme.pushButtonPad))
    implicitHeight: large ? theme.pushButtonHeightLarge
        : small ? theme.pushButtonHeightSmall : theme.pushButtonHeight
    radius: large ? theme.pushButtonRadiusLarge : theme.pushButtonRadius
    opacity: usable ? 1 : theme.pushButtonDisabledOpacity
    color: theme.pushButtonFill(button)
    Behavior on color {
        enabled: button.theme.pushButtonFade && Shell.Motion.enabled
        ColorAnimation { duration: Shell.Motion.hover }
    }
    border.color: theme.pushButtonEdge(button)
    border.width: activeFocus ? theme.pushButtonFocusEdgeWidth : theme.pushButtonEdgeWidth
    activeFocusOnTab: usable && visible
    Accessible.role: Accessible.Button
    Accessible.name: theme.tipIsName && tip.length > 0 ? tip : label
    Accessible.description: theme.tipIsName ? "" : tip
    Accessible.checked: on
    Accessible.onPressAction: activate()
    Keys.onReturnPressed: activate()
    Keys.onSpacePressed: activate()

    Text {
        id: caption
        anchors.centerIn: parent
        text: button.label
        color: button.theme.pushButtonInk(button)
        font.family: button.theme.pushButtonFont
        font.pixelSize: button.large ? button.theme.pushButtonTextSizeLarge
            : button.small ? button.theme.pushButtonTextSizeSmall : button.theme.pushButtonTextSize
        font.weight: button.theme.pushButtonWeight
        font.letterSpacing: button.theme.pushButtonLetterSpacing
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        enabled: button.active && button.interactive
        cursorShape: button.theme.pushButtonPointer && button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: {
            if (button.theme.pushButtonFocusOnClick)
                button.forceActiveFocus()
            button.activate()
        }
    }
    HoverTip {
        theme: button.theme
        enabled: button.tip !== "" || button.theme.pushButtonTipFallback
        text: button.tip.length > 0 ? button.tip : button.label
        shortcut: button.shortcut
    }
}
