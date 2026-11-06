pragma ComponentBehavior: Bound
import QtQuick
import "MacroPalette.js" as MacroPalette

// A row (or column) of tiles for a short choice list; the chosen tile is
// lit. Arrow keys step, Delete resets; every pick is one undo step. The
// theme's segment tokens give the tiles their look: lit tiles or latching
// keys with an LED, a rule or bar on the chosen one, and an optional glyph
// per choice drawn by `glyph(ctx, index, width, height, colour)`.
Item {
    id: segment
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property color accent: theme.segmentAccent
    property bool vertical: false
    property int spacing: vertical ? theme.segmentSpacingVertical : theme.segmentSpacing
    // Optional shorter labels, and a caption under each in vertical form.
    property var labels: []
    property var captions: []
    property var glyph: null
    readonly property var info: controller.parameter(parameterId)
    readonly property var choices: info.choices !== undefined ? info.choices : []
    readonly property int current: controller.revision >= 0 ? controller.choice(parameterId) : 0

    objectName: theme.prefix + "Segment:" + parameterId
    implicitWidth: theme.segmentWidth
    implicitHeight: theme.segmentHeight
    activeFocusOnTab: controller.editable
    opacity: controller.editable ? 1 : theme.segmentDisabledOpacity
    Accessible.role: Accessible.ComboBox
    Accessible.name: (info.group !== undefined ? info.group + " " : "") + (info.name !== undefined ? info.name : "")
    Accessible.description: choices[current] !== undefined ? choices[current] : ""

    function pick(index) {
        theme.lastTouched = parameterId
        controller.set_choice(parameterId, index)
    }
    function stepBy(direction) {
        controller.step(parameterId, direction, false)
        theme.lastTouched = parameterId
    }
    Keys.onLeftPressed: stepBy(-1)
    Keys.onUpPressed: stepBy(-1)
    Keys.onRightPressed: stepBy(1)
    Keys.onDownPressed: stepBy(1)
    Keys.onDeletePressed: { controller.reset_to_default(parameterId); theme.lastTouched = parameterId }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -segment.theme.segmentFocusMargin
        radius: segment.theme.segmentFocusRadius
        color: segment.theme.segmentFocusFill
        border.color: segment.activeFocus ? segment.accent : "transparent"
    }

    Grid {
        anchors.fill: parent
        columns: segment.vertical ? 1 : Math.max(1, segment.choices.length)
        spacing: segment.spacing
        Repeater {
            model: segment.choices.length
            delegate: Item {
                id: slot
                required property int index
                readonly property int count: Math.max(1, segment.choices.length)
                width: segment.vertical ? segment.width
                    : (segment.width - segment.spacing * (count - 1)) / count
                height: segment.vertical ? (segment.height - segment.spacing * (count - 1)) / count
                    : segment.height

              Rectangle {
                id: tile
                readonly property int index: slot.index
                readonly property bool on: index === segment.current
                readonly property bool pressed: tileArea.pressed
                readonly property bool hovered: tileArea.containsMouse
                readonly property bool vertical: segment.vertical
                readonly property color accent: segment.accent
                readonly property string label: segment.labels.length > index ? segment.labels[index]
                                                                              : segment.choices[index]
                readonly property bool glyphShown: segment.glyph !== null
                    && (segment.theme.segmentGlyphMode !== "column" || segment.vertical)
                width: slot.width
                height: slot.height
                y: on ? segment.theme.segmentPressDepth : 0
                radius: segment.vertical ? segment.theme.segmentRadiusVertical
                    : segment.theme.segmentRadius < 0 ? height / 2 : segment.theme.segmentRadius
                gradient: Gradient {
                    orientation: segment.theme.segmentFillAcross ? Gradient.Horizontal : Gradient.Vertical
                    GradientStop { position: 0; color: segment.theme.segmentFill(tile, 0) }
                    GradientStop { position: 1; color: segment.theme.segmentFill(tile, 1) }
                }
                border.color: segment.theme.segmentEdge(tile)
                Accessible.role: Accessible.RadioButton
                Accessible.name: segment.choices[tile.index]
                Accessible.checked: tile.on

                // A wash over the chosen tile.
                Rectangle {
                    visible: tile.on && segment.theme.segmentWash
                    anchors.fill: parent
                    radius: tile.radius
                    gradient: Gradient {
                        orientation: segment.vertical ? Gradient.Horizontal : Gradient.Vertical
                        GradientStop { position: 0; color: segment.theme.segmentWashColor(tile, 0) }
                        GradientStop { position: 1; color: segment.theme.segmentWashColor(tile, 1) }
                    }
                }
                // The rule down the chosen tile's leading edge (vertical form).
                Rectangle {
                    visible: tile.on && segment.vertical && segment.theme.segmentRule === "rule"
                    x: segment.theme.segmentRuleX
                    y: segment.theme.segmentRuleY
                    width: segment.theme.segmentRuleWidth
                    height: parent.height - 2 * segment.theme.segmentRuleY
                    radius: width / 2
                    gradient: Gradient {
                        GradientStop { position: 0; color: segment.theme.steel }
                        GradientStop { position: 1; color: segment.accent }
                    }
                }
                // A bar under (or beside) the chosen tile.
                Rectangle {
                    visible: tile.on && segment.theme.segmentRule === "bar"
                    x: segment.vertical ? 0 : segment.theme.segmentBarInset
                    y: segment.vertical ? segment.theme.segmentBarInset : parent.height - segment.theme.segmentBarInset
                    width: segment.vertical ? segment.theme.segmentBarWidth : parent.width - 2 * segment.theme.segmentBarInset
                    height: segment.vertical ? parent.height - 2 * segment.theme.segmentBarInset
                                             : segment.theme.segmentBarHeight
                    radius: segment.theme.segmentBarRadius
                    color: segment.accent
                }
                // The key's LED.
                Rectangle {
                    visible: segment.theme.segmentLed
                    x: segment.theme.segmentLedX
                    anchors.verticalCenter: parent.verticalCenter
                    width: segment.theme.segmentLedSize
                    height: segment.theme.segmentLedSize
                    radius: width / 2
                    color: tile.on ? segment.accent : segment.theme.segmentLedOff
                    border.width: segment.theme.segmentLedRim ? segment.theme.hairline : 0
                    border.color: segment.theme.segmentLedRimInk
                }
                Canvas {
                    id: glyphCanvas
                    readonly property string mode: segment.theme.segmentGlyphMode
                    visible: tile.glyphShown
                    x: mode === "fill" ? segment.theme.segmentGlyphFillX
                        : mode === "column" ? segment.theme.segmentGlyphColumnX : segment.theme.segmentGlyphLeadX
                    y: mode === "fill" ? segment.theme.segmentGlyphFillY : (parent.height - height) / 2
                    width: mode === "fill" ? parent.width - segment.theme.segmentGlyphFillPad
                        : mode === "column" ? segment.theme.segmentGlyphColumnWidth : segment.theme.segmentGlyphLeadWidth
                    height: mode === "fill" ? parent.height - 2 * segment.theme.segmentGlyphFillY
                        : mode === "column" ? Math.min(segment.theme.segmentGlyphColumnHeight,
                                                       parent.height - segment.theme.segmentGlyphColumnPad)
                        : parent.height - segment.theme.segmentGlyphLeadPad
                    renderStrategy: Canvas.Cooperative
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        const draw = segment.glyph
                        if (draw)
                            draw(ctx, tile.index, width, height,
                                 tile.on ? segment.accent : segment.theme.segmentGlyphIdleInk)
                    }
                    Connections {
                        target: tile
                        function onOnChanged() { glyphCanvas.requestPaint() }
                    }
                    Connections {
                        target: segment
                        function onGlyphChanged() { glyphCanvas.requestPaint() }
                    }
                }
                // Keys: one centred label.
                Text {
                    visible: segment.theme.segmentLed
                        && !(tile.glyphShown && segment.theme.segmentGlyphMode === "fill")
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: tile.glyphShown ? segment.theme.segmentKeyGlyphOffset
                                                                    : segment.theme.segmentTextOffset
                    width: parent.width - (tile.glyphShown ? segment.theme.segmentKeyGlyphPad
                                                           : 2 * segment.theme.segmentTextOffset + segment.theme.segmentKeyTextPad)
                    horizontalAlignment: Text.AlignHCenter
                    text: segment.theme.segmentUpper ? tile.label.toUpperCase() : tile.label
                    color: segment.theme.segmentLabelInk(tile)
                    font.pixelSize: segment.theme.segmentKeySize
                    font.bold: true
                    font.letterSpacing: segment.theme.segmentLetterSpacing
                    elide: Text.ElideRight
                }
                // Tiles: a label and, in vertical form, a caption.
                Column {
                    visible: !segment.theme.segmentLed
                    anchors.verticalCenter: parent.verticalCenter
                    x: segment.vertical ? (tile.glyphShown ? segment.theme.segmentTextXVerticalGlyph
                                                           : segment.theme.segmentTextXVertical)
                                        : segment.theme.segmentTextInset
                    width: parent.width - x - (segment.vertical ? segment.theme.segmentTextRightVertical
                                                                : segment.theme.segmentTextInset)
                    spacing: segment.theme.segmentCaptionGap
                    Text {
                        width: parent.width
                        horizontalAlignment: segment.vertical ? Text.AlignLeft : Text.AlignHCenter
                        text: tile.label
                        color: segment.theme.segmentLabelInk(tile)
                        font.family: segment.theme.segmentFont
                        font.pixelSize: segment.vertical ? segment.theme.segmentSizeVertical : segment.theme.segmentSize
                        font.letterSpacing: segment.vertical ? segment.theme.segmentLetterSpacingVertical
                                                             : segment.theme.segmentLetterSpacing
                        font.bold: segment.theme.segmentBold(tile)
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: segment.vertical && segment.captions.length > tile.index
                        width: parent.width
                        text: segment.captions.length > tile.index ? segment.captions[tile.index] : ""
                        color: segment.theme.segmentCaptionInk(tile)
                        font.pixelSize: segment.theme.segmentCaptionSize
                        font.letterSpacing: segment.theme.segmentCaptionSpacing
                        elide: Text.ElideRight
                    }
                }
                Tip {
                    theme: segment.theme
                    shown: tileArea.containsMouse && !tileArea.pressed
                    text: segment.Accessible.name + "  " + segment.choices[tile.index]
                    detail: MacroPalette.tipLine(segment.controller, segment.parameterId)
                        + (tile.on ? "Chosen · arrow keys step · Delete resets" : "Click to choose")
                }
                MouseArea {
                    id: tileArea
                    anchors.fill: parent
                    enabled: segment.controller.editable
                    hoverEnabled: true
                    onClicked: {
                        segment.forceActiveFocus()
                        segment.pick(tile.index)
                    }
                }
            }
              }
        }
    }
}
