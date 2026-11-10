pragma ComponentBehavior: Bound
import QtQuick

// Stick's kit seen from above, the drummer at the bottom (ADR-0114): each
// piece the kit has, named with its key, lit as it sounds; the chosen
// piece is ringed. Click a piece, or Tab to it and press Space or Return,
// to show its controls. Pieces the kit lacks are left out.
Item {
    id: drawing
    required property StickTheme theme
    // stick_layout's pieces: [{piece, name, key, keys, present}].
    property var pieces: []
    // The live level of each piece, 0..1, in Piece order.
    property var levels: []
    property int selected: 0
    signal picked(int piece)

    // Each piece's place in the design box: centre and radius, and whether it
    // is a cymbal; in Piece order (kick, snare, hi-hat, toms 1-5, crashes,
    // ride, splash, china).
    readonly property var spots: [
        { x: 200, y: 180, r: 44, cymbal: false },
        { x: 124, y: 210, r: 26, cymbal: false },
        { x: 60, y: 176, r: 24, cymbal: true },
        { x: 146, y: 108, r: 20, cymbal: false },
        { x: 200, y: 96, r: 22, cymbal: false },
        { x: 256, y: 108, r: 23, cymbal: false },
        { x: 296, y: 204, r: 27, cymbal: false },
        { x: 362, y: 242, r: 29, cymbal: false },
        { x: 84, y: 76, r: 30, cymbal: true },
        { x: 286, y: 38, r: 28, cymbal: true },
        { x: 352, y: 132, r: 32, cymbal: true },
        { x: 30, y: 116, r: 18, cymbal: true },
        { x: 368, y: 44, r: 22, cymbal: true }
    ]
    readonly property real scale: Math.min(width / theme.drawingWidth, height / theme.drawingHeight)
    readonly property real originX: (width - theme.drawingWidth * scale) / 2
    readonly property real originY: (height - theme.drawingHeight * scale) / 2

    objectName: "stickKitDrawing"
    Accessible.role: Accessible.Pane
    Accessible.name: "Kit, from above"

    Rectangle {
        anchors.fill: parent
        radius: drawing.theme.tileRadius
        color: drawing.theme.caseColor
    }

    Repeater {
        model: drawing.pieces
        Item {
            id: piece
            required property var modelData
            required property int index
            readonly property var spot: drawing.spots[index]
            readonly property real radius: spot.r * drawing.scale
            readonly property real level: drawing.levels.length > index ? drawing.levels[index] : 0
            readonly property bool chosen: drawing.selected === index
            objectName: "stickPiece" + index
            visible: modelData.present === true
            x: drawing.originX + spot.x * drawing.scale - width / 2
            y: drawing.originY + spot.y * drawing.scale - height / 2
            width: 2 * (radius + drawing.theme.glowBase + drawing.theme.glowReach)
            height: width
            activeFocusOnTab: true
            Accessible.role: Accessible.RadioButton
            Accessible.checkable: true
            Accessible.checked: chosen
            Accessible.name: modelData.name + (modelData.keys !== "" ? ", " + modelData.keys : "")
            Keys.onSpacePressed: drawing.picked(index)
            Keys.onReturnPressed: drawing.picked(index)

            // The glow of a hit.
            Rectangle {
                readonly property real reach: piece.radius + drawing.theme.glowBase
                                              + piece.level * drawing.theme.glowReach
                anchors.centerIn: parent
                width: 2 * reach
                height: width
                radius: width / 2
                visible: piece.level > 0
                color: Qt.rgba(drawing.theme.amber.r, drawing.theme.amber.g, drawing.theme.amber.b,
                               piece.level * drawing.theme.glowAlpha)
            }
            Rectangle {
                id: face
                anchors.centerIn: parent
                width: 2 * piece.radius
                height: width
                radius: width / 2
                color: piece.spot.cymbal ? drawing.theme.cymbalFace : drawing.theme.drumFace
                border.width: piece.chosen || piece.activeFocus ? drawing.theme.selectedRing
                                                                 : drawing.theme.pieceRing
                border.color: piece.activeFocus ? drawing.theme.focusRing
                    : piece.chosen ? drawing.theme.amber
                    : piece.spot.cymbal ? drawing.theme.cymbalEdge : drawing.theme.drumEdge
            }
            Column {
                anchors.centerIn: face
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: piece.modelData.name
                    color: piece.chosen ? drawing.theme.amber : drawing.theme.ink
                    font.family: drawing.theme.sans
                    font.pixelSize: drawing.theme.pieceNameSize
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: piece.modelData.key
                    color: drawing.theme.inkDim
                    font.family: drawing.theme.mono
                    font.pixelSize: drawing.theme.pieceKeySize
                }
            }
            MouseArea {
                anchors.centerIn: face
                width: Math.max(face.width, drawing.theme.minHitSize)
                height: width
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    piece.forceActiveFocus()
                    drawing.picked(piece.index)
                }
            }
        }
    }
}
