pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// One browser card: a preset, a starter sound or a VST3 plugin. Click
// selects, double-click runs the catalogue's primary action, dragging
// carries the catalogue's device payload (see BrowserCatalogue.dragPayload).
// A compact card asks its host for a context menu on right-click or the
// Menu key.
Rectangle {
    id: cardItem
    required property var card
    required property var catalogue
    property bool compact: false
    readonly property bool selected: catalogue.selectedCard && catalogue.selectedCard.key === card.key
    readonly property bool favourite: catalogue.isFavourite(card.key)
    signal activated()
    signal menuRequested(real x, real y)

    // Card layout: full cards keep an even inset; compact ones are tighter.
    readonly property int inset: 14
    readonly property int compactChipY: 10
    readonly property int textGap: 10
    readonly property int compactTitleY: 9
    readonly property int compactTextRight: 10
    readonly property int textRight: 40
    readonly property int compactPlayingRight: 10
    readonly property int playingRight: 34
    readonly property int compactPlayingY: 18
    readonly property int starRight: 30
    readonly property int shapeY: 60
    readonly property int shapeHeight: 34
    readonly property int tagRise: 26
    readonly property int tagHeight: 18

    implicitHeight: compact ? Theme.browserCompactCardHeight : Theme.browserCardHeight
    radius: compact ? Theme.radiusXl : Theme.browserCardRadius
    color: selected ? Theme.selection : pointer.containsMouse ? (compact ? Theme.browserCompactCardHover : Theme.browserCardHover)
        : compact ? "transparent" : Theme.browserCardFill
    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
    border.color: selected ? (compact ? Theme.lineStrong : card.color) : compact ? "transparent" : Theme.line
    border.width: Theme.hairline
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: catalogue.title(card) + ", " + card.engineName
    Accessible.description: catalogue.primaryLabel(card)
    Keys.onReturnPressed: catalogue.activate(card)
    Keys.onSpacePressed: catalogue.togglePreview(card)
    Keys.onMenuPressed: if (compact) menuRequested(width / 2, height / 2)
    onActiveFocusChanged: if (activeFocus) catalogue.selectedKey = card.key

    Rectangle {
        id: chip
        x: compact ? Theme.spaceMd : cardItem.inset; y: compact ? cardItem.compactChipY : cardItem.inset
        width: compact ? Theme.browserCompactChipSize : Theme.browserChipSize; height: width; radius: compact ? Theme.radiusXl : Theme.browserChipRadius
        color: cardItem.card.color
        // What it is at a glance: "Cp" for a compressor, "Rv" for a reverb.
        Text {
            anchors.centerIn: parent
            text: cardItem.card.abbreviation || (cardItem.card.engineName || "?").charAt(0)
            color: Theme.canvas
            font.pixelSize: cardItem.card.abbreviation ? Theme.typeLabel : Theme.typeSubtitle
            font.bold: true
        }
    }
    Column {
        x: chip.x + chip.width + cardItem.textGap
        y: compact ? cardItem.compactTitleY : cardItem.inset
        width: parent.width - x - (compact ? cardItem.compactTextRight : cardItem.textRight)
        spacing: Theme.spaceXxs
        Text {
            width: parent.width
            text: cardItem.catalogue.title(cardItem.card)
            color: Theme.textMain
            font.pixelSize: cardItem.compact ? Theme.typeEmphasis : Theme.typeSubtitle
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            // Its name and what sets it apart; VST3 entries name their vendor.
            text: cardItem.catalogue.subtitle(cardItem.card)
            color: Theme.textDim
            font.pixelSize: Theme.typeBody
            elide: Text.ElideRight
        }
    }
    // Sounding now (a sample preview).
    Text {
        visible: cardItem.catalogue.playingKey === cardItem.card.key
        anchors.right: parent.right
        anchors.rightMargin: cardItem.compact ? cardItem.compactPlayingRight : cardItem.playingRight
        y: cardItem.compact ? cardItem.compactPlayingY : cardItem.inset
        text: "♪"
        color: cardItem.card.color
        font.pixelSize: Theme.typeEmphasis
        Accessible.ignored: true
    }
    Text {
        visible: !cardItem.compact
        x: parent.width - cardItem.starRight; y: cardItem.inset
        text: cardItem.favourite ? "★" : "☆"
        color: cardItem.favourite ? Theme.amber : Theme.textDisabled
        font.pixelSize: Theme.typeHeading
        Accessible.ignored: true
    }
    // A static per-sound shape, seeded by the card key.
    Canvas {
        id: shape
        visible: !cardItem.compact
        x: cardItem.inset; y: cardItem.shapeY; width: parent.width - 2 * cardItem.inset; height: cardItem.shapeHeight
        renderStrategy: Canvas.Cooperative
        // The shape's hash and envelope: a decaying, seeded ripple.
        readonly property int hashMultiplier: 31
        readonly property int hashModulus: 100003
        readonly property int baseDecay: 20
        readonly property int decayStep: 14
        readonly property int decayVariants: 5
        readonly property real baseRate: 0.5
        readonly property real rateStep: 0.3
        readonly property int rateVariants: 3
        readonly property real envelopeDepth: 0.85
        readonly property real envelopeFloor: 0.12
        readonly property int edgeInset: 2
        readonly property int steps: 100
        onWidthChanged: requestPaint()
        Component.onCompleted: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            let seed = 0
            const key = cardItem.card.key
            for (let i = 0; i < key.length; ++i) seed = (seed * shape.hashMultiplier + key.charCodeAt(i)) % shape.hashModulus
            const mid = height / 2
            const decay = shape.baseDecay + (seed % shape.decayVariants) * shape.decayStep
            const rate = shape.baseRate + (seed % shape.rateVariants) * shape.rateStep
            ctx.beginPath()
            ctx.moveTo(0, mid)
            for (let i = 0; i <= shape.steps; ++i) {
                const env = Math.exp(-i / decay) * shape.envelopeDepth + shape.envelopeFloor
                const a = (mid - shape.edgeInset) * env * Math.abs(Math.sin(i * rate + seed))
                ctx.lineTo(i * width / shape.steps, mid - a)
            }
            for (let i = shape.steps; i >= 0; --i) {
                const env = Math.exp(-i / decay) * shape.envelopeDepth + shape.envelopeFloor
                const a = (mid - shape.edgeInset) * env * Math.abs(Math.sin(i * rate + seed))
                ctx.lineTo(i * width / shape.steps, mid + a)
            }
            ctx.closePath()
            ctx.fillStyle = cardItem.card.color
            ctx.globalAlpha = Theme.browserShapeFillAlpha
            ctx.fill()
            ctx.globalAlpha = 1
            ctx.strokeStyle = cardItem.card.color
            ctx.lineWidth = Theme.hairline
            ctx.stroke()
        }
    }
    Rectangle {
        visible: !cardItem.compact && cardItem.card.tag.length > 0
        x: cardItem.inset; y: parent.height - cardItem.tagRise
        width: tagText.implicitWidth + Theme.spaceXl; height: cardItem.tagHeight; radius: height / 2
        color: Theme.tagFill
        Text { id: tagText; anchors.centerIn: parent; text: cardItem.card.tag; color: Theme.tagText; font.pixelSize: Theme.typeSmall }
    }

    // Dragging moves an invisible proxy that carries the payload.
    Item {
        id: dragProxy
        width: 1; height: 1
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.mimeData: cardItem.catalogue.dragMime(cardItem.card)
    }
    MouseArea {
        id: pointer
        anchors.fill: parent
        drag.target: dragProxy
        hoverEnabled: true
        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.PointingHandCursor
        onClicked: {
            cardItem.catalogue.selectedKey = cardItem.card.key
            cardItem.forceActiveFocus()
        }
        onDoubleClicked: {
            cardItem.catalogue.activate(cardItem.card)
            cardItem.activated()
        }
        onReleased: { dragProxy.x = 0; dragProxy.y = 0 }
        // Set here, not bound: an automatic drag ends by itself, and a
        // binding to drag.active loops through it. Setting Drag.active (not
        // calling startDrag()) lets Qt start the drag after this mouse move,
        // once the press is finished with; startDrag() runs the drag inside
        // the move and the drop never lands.
        drag.onActiveChanged: {
            if (drag.active)
                dragProxy.Drag.active = true
        }
        Tip {
            anchorItem: cardItem
            enabled: !pointer.drag.active
            text: cardItem.catalogue.primaryLabel(cardItem.card)
                  + " · double-click, or drag onto a track"
                  + (cardItem.compact ? " · right-click for more" : "")
            shortcut: "Enter · Space previews"
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: cardItem.compact
        acceptedButtons: Qt.RightButton
        onClicked: mouse => {
            cardItem.catalogue.selectedKey = cardItem.card.key
            cardItem.menuRequested(mouse.x, mouse.y)
        }
    }
}
