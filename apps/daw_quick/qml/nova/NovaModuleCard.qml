pragma ComponentBehavior: Bound
import QtQuick

// A card on the overview (ADR-0076 revision 2). The header holds the module's on LED,
// its bold mono title, its type dropdown, a sub-label in the module's
// accent on the right and ⤢. Under it sit the module's display (`display`)
// and its controls (`controls`): main knobs at full size, the rest small
// under group labels. A card with neither places free content in `body`,
// `bodyHeight` tall.
//
// A card whose module is off fades, and rises again under the pointer; a
// `collapsible` one instead folds to its header with an OFF chip, its light
// still there to switch it on (issue #163). ⤢,
// or a double-click on the title, opens the card large over the overview
// (`panel.focusModule`), with a bigger display; Esc, ⤢ again or a click on
// the scrim restores it. The overview places the card. The display takes
// whatever height the card is given beyond what it needs, so the cards in
// a row line up and a large card's display grows. `flash()` pulses a glow
// in the accent when the strip jumps to the card.
Rectangle {
    id: card
    required property var panel
    required property string key
    property string title: ""
    property string sub: ""
    property color accent: panel.theme.neutral
    property color subColor: accent
    // The module's [Off, On] switch, lit in the header; "" for none.
    property string ledParameter: ""
    // The module's type, as a dropdown in the header; "" for none.
    property string typeParameter: ""
    property var typeGroups: []
    property bool expandable: true
    // Folds to its header while its module is off.
    property bool collapsible: false
    property Component display: null
    property Component controls: null
    property real bodyHeight: 0
    default property alias content: body.data
    readonly property alias body: body
    readonly property bool off: !panel.moduleOn(key)
    readonly property bool collapsed: collapsible && off
    // A card folded while shown large gives the overview back.
    onCollapsedChanged: if (collapsed && panel.focusModule === key) panel.restore()
    readonly property bool expanded: expandable && !collapsed && panel.focusModule === key
    readonly property bool flashing: flashPulse.running
    // The height the card takes in the grid. It is kept while the card is
    // large, so the cards under the scrim keep their places.
    property real cellHeight: 0

    // Layout and type.
    readonly property real padding: panel.theme.moduleCardPad
    readonly property real headerHeight: panel.theme.moduleHeaderHeight
    readonly property real gap: panel.theme.spaceMd
    readonly property real headerGap: panel.theme.spaceMd
    readonly property real displayHeight: 80
    readonly property real expandedDisplayHeight: 168
    readonly property real cardRadius: panel.theme.overviewCardRadius
    readonly property real titleSize: panel.theme.typeSmall
    readonly property real titleSpacing: panel.theme.moduleTitleSpacing
    readonly property real subSize: panel.theme.typeCaption
    readonly property real typeWidth: 112
    readonly property real typeHeight: 20
    readonly property real typeFontSize: panel.theme.moduleTypeTextSize
    // ⤢ takes the kit's least hit size.
    readonly property real expandWidth: panel.theme.minHitSize
    readonly property real expandGlyphSize: panel.theme.typeSubtitle
    readonly property real edge: panel.theme.hairline
    readonly property real expandedEdge: panel.theme.moduleExpandedEdge
    // An off module's card, at rest and under the pointer.
    readonly property real offOpacity: panel.theme.moduleOffOpacity
    readonly property real offHoverOpacity: panel.theme.moduleOffHoverOpacity
    readonly property int fadeDuration: 200
    // The strip's jump: a glow that rises and falls back.
    readonly property int flashRise: 270
    readonly property int flashFall: 630
    readonly property real glowWidth: panel.theme.moduleFlashWidth
    // The OFF chip of a folded card.
    readonly property real offChipHeight: panel.theme.moduleOffChipHeight
    readonly property real offChipPadding: panel.theme.moduleOffChipPadding

    readonly property real naturalDisplayHeight: display === null ? 0
        : expanded ? expandedDisplayHeight : displayHeight
    readonly property real controlsHeight: controlsLoader.item !== null ? controlsLoader.item.implicitHeight : 0
    readonly property bool composed: display !== null || controls !== null
    readonly property real contentHeight: composed
        ? naturalDisplayHeight + (display !== null && controls !== null ? gap : 0) + controlsHeight
        : bodyHeight

    function flash() {
        flashPulse.restart()
    }
    function keepCellHeight() {
        if (!expanded)
            cellHeight = implicitHeight
    }

    objectName: "novaModuleCard:" + key
    implicitHeight: 2 * padding + headerHeight + (collapsed ? 0 : gap + contentHeight)
    radius: cardRadius
    color: panel.theme.card
    border.color: expanded ? accent : panel.theme.line
    border.width: expanded ? expandedEdge : edge
    opacity: off && !expanded && !collapsed ? (hover.hovered ? offHoverOpacity : offOpacity) : 1
    Accessible.role: Accessible.Grouping
    Accessible.name: title + (off ? ", off" : "")
    Behavior on opacity { NumberAnimation { duration: card.fadeDuration } }

    onImplicitHeightChanged: keepCellHeight()
    onExpandedChanged: {
        keepCellHeight()
        if (expanded)
            forceActiveFocus()
    }
    Component.onCompleted: keepCellHeight()
    // Esc restores a large card, from any control in it that leaves the key.
    Keys.onEscapePressed: event => {
        if (!expanded) {
            event.accepted = false
            return
        }
        panel.restore()
    }

    HoverHandler {
        id: hover
    }

    // The strip's jump: a glow in the accent just outside the edge.
    Rectangle {
        id: glow
        anchors.fill: parent
        anchors.margins: -card.glowWidth
        radius: card.radius + card.glowWidth
        z: -1
        color: "transparent"
        border.width: card.glowWidth
        border.color: card.panel.theme.soft(card.accent, card.panel.theme.haloAlpha)
        opacity: 0
    }
    SequentialAnimation {
        id: flashPulse
        NumberAnimation { target: glow; property: "opacity"; to: 1; duration: card.flashRise; easing.type: Easing.OutQuad }
        NumberAnimation { target: glow; property: "opacity"; to: 0; duration: card.flashFall; easing.type: Easing.InQuad }
    }

    // ---- Header ---------------------------------------------------------------------------

    Item {
        id: header
        x: card.padding
        y: card.padding
        width: card.width - 2 * card.padding
        height: card.headerHeight

        Loader {
            id: led
            active: card.ledParameter !== ""
            visible: active
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: NovaLed {
                panel: card.panel
                parameterId: card.ledParameter
            }
        }
        Text {
            id: titleText
            objectName: "novaCardTitle:" + card.key
            x: led.active ? led.width + card.headerGap : 0
            anchors.verticalCenter: parent.verticalCenter
            text: card.title
            color: card.panel.theme.text
            font.family: card.panel.theme.mono
            font.pixelSize: card.titleSize
            font.weight: Font.Bold
            font.letterSpacing: card.titleSpacing
            MouseArea {
                // A shortcut for ⤢, which takes Tab focus.
                readonly property string pointerOnlyReason: "double-click shortcut for the card's ⤢ button"
                anchors.fill: parent
                enabled: card.expandable
                onDoubleClicked: card.panel.expand(card.key)
            }
        }
        NovaDropdown {
            id: typeDropdown
            visible: card.typeParameter !== ""
            x: titleText.x + titleText.implicitWidth + card.headerGap
            anchors.verticalCenter: parent.verticalCenter
            panel: card.panel
            parameterId: card.typeParameter
            groups: card.typeGroups
            accentStyle: true
            width: Math.min(card.typeWidth, Math.max(0, (card.collapsed ? offChip.x : expandButton.x)
                                                         - card.headerGap - x))
            height: card.typeHeight
            fontSize: card.typeFontSize
        }
        Text {
            id: subText
            objectName: "novaCardSub:" + card.key
            readonly property real leftEdge: typeDropdown.visible ? typeDropdown.x + typeDropdown.width
                                                               : titleText.x + titleText.implicitWidth
            visible: card.sub !== "" && !card.collapsed
            anchors.verticalCenter: parent.verticalCenter
            x: expandButton.x - card.headerGap - width
            width: Math.max(0, Math.min(implicitWidth, expandButton.x - 2 * card.headerGap - leftEdge))
            horizontalAlignment: Text.AlignRight
            text: card.sub
            color: card.subColor
            font.family: card.panel.theme.mono
            font.pixelSize: card.subSize
            font.weight: Font.Medium
            elide: Text.ElideRight
        }
        // A folded card's OFF chip, at the right.
        Rectangle {
            id: offChip
            objectName: "novaCardOff:" + card.key
            visible: card.collapsed
            x: parent.width - width
            anchors.verticalCenter: parent.verticalCenter
            width: offLabel.implicitWidth + card.offChipPadding
            height: card.offChipHeight
            radius: card.panel.theme.radiusMd
            color: "transparent"
            border.color: card.panel.theme.line2
            Accessible.role: Accessible.StaticText
            Accessible.name: card.title + " is off"
            Text {
                id: offLabel
                anchors.centerIn: parent
                text: "OFF"
                color: card.panel.theme.dim
                font.family: card.panel.theme.mono
                font.pixelSize: card.panel.theme.typeMicro
                font.weight: Font.DemiBold
                font.letterSpacing: card.panel.theme.moduleOffChipSpacing
            }
        }
        // ⤢: open the card large, or restore it.
        Text {
            id: expandButton
            objectName: "novaExpand:" + card.key
            visible: card.expandable && !card.collapsed
            x: parent.width - (visible ? width : 0)
            anchors.verticalCenter: parent.verticalCenter
            width: card.expandWidth
            height: card.headerHeight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: "⤢"
            color: activeFocus || expandArea.containsMouse || card.expanded ? card.panel.theme.text : card.panel.theme.faint
            font.pixelSize: card.expandGlyphSize
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: card.expanded ? "Restore " + card.title : "Open " + card.title + " large"
            Accessible.onPressAction: card.panel.expand(card.key)
            Keys.onReturnPressed: card.panel.expand(card.key)
            Keys.onSpacePressed: card.panel.expand(card.key)
            // The focus edge.
            Rectangle {
                visible: expandButton.activeFocus
                anchors.fill: parent
                radius: card.panel.theme.radiusMd
                color: "transparent"
                border.color: card.accent
                border.width: card.panel.theme.strokeBold
            }
            MouseArea {
                id: expandArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: card.panel.expand(card.key)
            }
        }
    }

    // ---- Display and controls, or free content ------------------------------------------------

    Item {
        id: body
        x: card.padding
        y: card.padding + card.headerHeight + card.gap
        visible: !card.collapsed
        width: card.width - 2 * card.padding
        height: card.height - y - card.padding

        Loader {
            id: displayLoader
            active: card.display !== null
            visible: active
            width: body.width
            // The height beyond what the card needs goes to the display: the
            // body's height less the controls'. Taken from the body, not from
            // height - implicitHeight, which loops through the grid's rows.
            height: Math.max(card.naturalDisplayHeight,
                             body.height - (controlsLoader.active ? card.controlsHeight + card.gap : 0))
            sourceComponent: card.display
        }
        Loader {
            id: controlsLoader
            active: card.controls !== null
            visible: active
            y: displayLoader.active ? displayLoader.height + card.gap : 0
            width: body.width
            height: card.controlsHeight
            sourceComponent: card.controls
        }
    }
}
