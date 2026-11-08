pragma ComponentBehavior: Bound
import QtQuick
import "MacroPalette.js" as MacroPalette

// A row (or column) of tiles for a short choice list; the chosen tile is
// filled with the accent and its text set in the inverse ink. Arrow keys
// step, Delete resets; every pick is one undo step. The theme's segment
// tokens style the other tiles, and an optional glyph per choice is drawn
// by `glyph(ctx, index, width, height, colour)`.
Item {
    id: segment
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property color accent: theme.segmentAccent
    // The chosen tile's text, on the accent fill.
    property color onInk: theme.segmentOnInk
    property bool vertical: false
    // Mode-inactive (see Knob): still editable, its choice not in play in
    // the current setting, with the reason given; it dims to
    // segmentInactiveOpacity, unlike a disabled segment's fade.
    property bool inactive: false
    property string inactiveReason: ""
    readonly property string controlState: !controller.editable ? "disabled"
        : inactive ? "inactive" : "normal"
    property int spacing: vertical ? theme.segmentSpacingVertical : theme.segmentSpacing
    // Optional shorter labels, and a caption under each in vertical form.
    property var labels: []
    property var captions: []
    property var glyph: null
    readonly property var info: controller.parameter(parameterId)
    readonly property var choices: info.choices !== undefined ? info.choices : []
    readonly property int current: controller.revision >= 0 ? controller.choice(parameterId) : 0

    // The tiles' labels as shown, and the width that sets the longest of
    // them whole on every (equal) tile of a row: what a row is sized from.
    readonly property var shownLabels: choices.map((choice, index) => {
        const label = labels.length > index ? labels[index] : choice
        return theme.segmentUpper ? String(label).toUpperCase() : String(label)
    })
    readonly property real longestLabel: shownLabels.reduce((widest, label) =>
        Math.max(widest, labelMetrics.advanceWidth(label) + theme.segmentLetterSpacing * label.length), 0)
    readonly property real fitWidth: vertical ? theme.segmentWidth
        : choices.length * (Math.ceil(longestLabel) + theme.segmentTextInset
                            + (glyph !== null && theme.segmentGlyphMode === "lead"
                               ? theme.segmentGlyphLeadTextX : theme.segmentTextInset))
          + spacing * Math.max(0, choices.length - 1)
    // A vertical column's height that sets each tile's label and caption
    // whole, with segmentTextPadVertical above and below.
    readonly property real fitHeight: !vertical ? theme.segmentHeight
        : choices.length * (verticalLabelMetrics.height
                            + (captions.length > 0 ? theme.segmentCaptionGap + captionMetrics.height : 0)
                            + 2 * theme.segmentTextPadVertical)
          + spacing * Math.max(0, choices.length - 1)
    FontMetrics {
        id: verticalLabelMetrics
        font.family: segment.theme.segmentFont
        font.pixelSize: segment.theme.segmentSizeVertical
    }
    FontMetrics {
        id: captionMetrics
        font.pixelSize: segment.theme.segmentCaptionSize
    }
    FontMetrics {
        id: labelMetrics
        font.family: segment.theme.segmentFont
        font.pixelSize: segment.theme.segmentSize
        font.bold: true
    }
    // The captions laid out tracked, as a caption lays them out; never
    // shown. Captions all drop their tracking when the widest would not fit
    // its tile with it, so a column reads alike.
    Repeater {
        id: trackedCaptions
        model: segment.vertical ? segment.captions : []
        delegate: Text {
            required property string modelData
            visible: false
            text: modelData
            font.pixelSize: segment.theme.segmentCaptionSize
            font.letterSpacing: segment.theme.segmentCaptionSpacing
        }
    }
    readonly property real widestCaption: {
        let widest = 0
        for (let i = 0; i < trackedCaptions.count; ++i) {
            const caption = trackedCaptions.itemAt(i)
            if (caption !== null)
                widest = Math.max(widest, caption.implicitWidth)
        }
        return widest
    }

    objectName: theme.prefix + "Segment:" + parameterId
    implicitWidth: Math.max(theme.segmentWidth, fitWidth)
    implicitHeight: vertical ? fitHeight : theme.segmentHeight
    activeFocusOnTab: controller.editable
    opacity: !controller.editable ? theme.segmentDisabledOpacity
        : inactive ? theme.segmentInactiveOpacity : 1
    Accessible.role: Accessible.ComboBox
    Accessible.name: (info.group !== undefined ? info.group + " " : "") + (info.name !== undefined ? info.name : "")
    Accessible.description: (choices[current] !== undefined ? choices[current] : "")
        + (inactive && inactiveReason !== "" ? ". Inactive: " + inactiveReason : "")

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
                readonly property bool leadGlyph: glyphShown && segment.theme.segmentGlyphMode === "lead"
                // The chosen tile is filled with the accent and its text set
                // in the inverse ink; the theme styles the others.
                readonly property color selectionFill: on ? accent : segment.theme.segmentFill(tile, 0)
                readonly property color selectionInk: on ? segment.onInk : segment.theme.segmentLabelInk(tile)
                width: slot.width
                height: slot.height
                y: on ? segment.theme.segmentPressDepth : 0
                radius: segment.vertical ? segment.theme.segmentRadiusVertical
                    : segment.theme.segmentRadius < 0 ? height / 2 : segment.theme.segmentRadius
                gradient: Gradient {
                    orientation: segment.theme.segmentFillAcross ? Gradient.Horizontal : Gradient.Vertical
                    GradientStop { position: 0; color: tile.selectionFill }
                    GradientStop { position: 1; color: tile.on ? tile.accent : segment.theme.segmentFill(tile, 1) }
                }
                border.color: segment.theme.segmentEdge(tile)
                Accessible.role: Accessible.RadioButton
                Accessible.name: segment.choices[tile.index]
                Accessible.checked: tile.on

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
                                 tile.on ? segment.onInk : segment.theme.segmentGlyphIdleInk)
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
                // A label (unless a glyph fills the tile) and, in vertical
                // form, a caption.
                Column {
                    visible: !(tile.glyphShown && segment.theme.segmentGlyphMode === "fill")
                    anchors.verticalCenter: parent.verticalCenter
                    x: segment.vertical ? (tile.glyphShown ? segment.theme.segmentTextXVerticalGlyph
                                                           : segment.theme.segmentTextXVertical)
                                        : tile.leadGlyph ? segment.theme.segmentGlyphLeadTextX
                                                         : segment.theme.segmentTextInset
                    width: parent.width - x - (segment.vertical ? segment.theme.segmentTextRightVertical
                                                                : segment.theme.segmentTextInset)
                    spacing: segment.theme.segmentCaptionGap
                    Text {
                        width: parent.width
                        horizontalAlignment: segment.vertical ? Text.AlignLeft : Text.AlignHCenter
                        text: segment.theme.segmentUpper ? tile.label.toUpperCase() : tile.label
                        color: tile.selectionInk
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
                        color: tile.on ? segment.onInk : segment.theme.segmentCaptionInk(tile)
                        font.pixelSize: segment.theme.segmentCaptionSize
                        // A caption wider than its tile drops its tracking,
                        // then steps down to the least type size, before
                        // anything is elided.
                        font.letterSpacing: segment.widestCaption > width ? 0 : segment.theme.segmentCaptionSpacing
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: segment.theme.typeFitFloor
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
                    // At least the kit's least hit size, however small the control draws.
                    anchors.leftMargin: -segment.theme.hitOutset(parent.width)
                    anchors.rightMargin: -segment.theme.hitOutset(parent.width)
                    anchors.topMargin: -segment.theme.hitOutset(parent.height)
                    anchors.bottomMargin: -segment.theme.hitOutset(parent.height)
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
