import QtQuick

// The device-panel toolkit's design tokens. A panel's theme derives from
// this and supplies its palette as data: the shared colours below, its own
// named colours for its own views, and any widget styling that differs from
// the defaults. Widgets read only these tokens, so one Knob, Menu or Segment
// serves every panel while each keeps its own look.
QtObject {
    id: kit

    // objectName prefix for the panel's widgets ("trim" gives "trimKnob:<id>").
    property string prefix: "kit"
    // The parameter last moved from the panel (for status lines).
    property string lastTouched: ""

    // Type.
    property string sans: ""
    property string mono: "monospace"
    property string serif: "serif"

    // The panel's case: a vertical gradient from caseTop through caseColor
    // to caseBottom. A theme sets the three; a flat case sets caseColor only.
    property color caseColor: plate
    property color caseTop: caseColor
    property color caseBottom: caseColor

    // Shared scales. Components take a size from a scale when it fits and
    // name their own token for a size with a particular job.
    // Type: from small print to display headings (pixel sizes).
    property int typeMicro: 8
    property int typeCaption: 9
    property int typeSmall: 10
    property int typeBody: 11
    property int typeLabel: 12
    property int typeEmphasis: 13
    property int typeSubtitle: 14
    property int typeHeading: 15
    property int typeTitle: 16
    property int typeDisplay: 18
    // Spacing between and inside controls.
    property int spaceXxs: 2
    property int spaceXs: 4
    property int spaceSm: 6
    property int spaceMd: 8
    property int spaceLg: 12
    property int spaceXl: 16
    property int space2xl: 24
    // Corner radii.
    property real radiusXs: 2
    property real radiusSm: 3
    property real radiusMd: 4
    property real radiusLg: 6
    property real radiusXl: 8
    // Strokes: a hairline edge and an emphasised (focused, selected) one.
    property real hairline: 1
    property real strokeBold: 2

    // Shared colours. Panels map their palette onto these.
    property color accent: "#5cb8ff"
    property color accent2: accent
    property color accentSoft: "#2f6fa3"
    property color steel: "#2d5f8a"
    property color plate: "#191d23"
    property color plateEdge: "#2a303a"
    property color plateHi: "#3b4350"
    property color well: "#0b0d11"
    property color wellEdge: "#262c36"
    property color seam: "#222730"
    property color grid: "#1e2530"
    property color gridStrong: "#2e3846"
    property color ink: "#e9eef5"
    property color inkDim: "#8a94a3"
    property color inkFaint: "#566070"
    property color glacier: "#eef4fb"
    property color knobTop: "#3a414c"
    property color knobBottom: "#15181d"
    property color knobRing: "#2a303a"
    property color pointer: "#eef4fb"
    property color track: "#1a1f27"
    property color button: "#161a20"
    property color buttonHi: "#1f242c"
    property color ledOff: "#2a2f37"
    property color warning: "#ff8a5c"
    property color live: "#4fd18b"

    // Tip: a small tooltip after a short hover.
    property color tipFill: well
    property color tipEdge: wellEdge
    property color tipInk: ink
    property real tipRadius: 10
    property string tipFont: mono
    property bool tipBold: false

    // Menu: the popup list behind dropdowns and the preset browser.
    property color menuFill: plate
    property color menuEdge: plateEdge
    property color menuInk: ink
    property color menuHeaderInk: inkFaint
    property real menuRadius: 4
    property real menuRowRadius: 2
    property int menuRowHeight: 24
    property int menuHeaderHeight: 22
    property real menuHeaderOffset: 0
    property real menuHeaderSpacing: 1.4
    property real menuHighlightAlpha: 0.22
    property int menuMinWidth: 150
    property int menuGap: 3
    // The list's inner padding and its margin from the window's edges; it
    // unfolds from this scale.
    property real menuPad: 4
    property real menuMargin: 8
    property real menuAppearScale: 0.97
    // Row text: inset from the left (headers less), the room it leaves on
    // the right, and its sizes.
    property real menuHeaderX: 8
    property real menuRowX: 12
    property real menuTextRight: 20
    property int menuHeaderSize: typeCaption
    property int menuRowSize: typeLabel
    // The highlighted row's text: in the accent, or bold in the ink.
    property bool menuBoldHighlight: false

    // Knob geometry and captions. paintKnob draws the face; the default is a
    // fine graduated scale, an arc from the origin, a brushed cap and an
    // index line.
    property int knobDiameter: 38
    property int knobDialPad: 16
    property real knobDialY: 0
    property int knobWidthPad: 26
    property int knobMinWidth: 66
    property int knobHeightPad: 36
    property int knobReadoutPad: 14
    property real knobDisabledOpacity: 0.45
    property real knobInactiveOpacity: 0.38
    // The knob's own colour, unless a knob sets one.
    property color knobAccent: accent
    // The spoken and tooltip name: "groupUnlessSame" prefixes the group
    // unless the label already is it, "group" always prefixes it, "label"
    // uses the label alone, and "none" speaks the label with no tooltip name.
    property string knobLongName: "groupUnlessSame"
    property bool knobTip: true
    // Captions: the label, swapped for the value while touched, or (stacked)
    // the label above an always-shown value.
    property bool knobStacked: false
    property real knobCaptionGap: 1
    property real knobCaptionInset: 6
    // Captions clipped to their width: for tight rows of knobs, where Qt's
    // elision (which ignores letter spacing) may leave a glyph past the edge.
    property bool knobCaptionClip: false
    property bool knobLabelUpper: true
    property string knobLabelFont: sans
    property int knobLabelSize: 8
    property bool knobLabelBold: false
    property real knobLabelSpacing: 1.6
    property color knobLabelInk: inkDim
    property color knobLabelActiveInk: knobLabelInk
    property string knobValueFont: mono
    property int knobValueSize: 10
    property bool knobValueBold: false
    // Unset: the knob's own accent.
    property var knobValueInk: undefined
    property color knobValueIdleInk: inkFaint
    property int knobReadoutLabelSize: 9
    property real knobStackedValueGap: 12
    property int knobStackedValueSize: 9
    // A small hollow diamond (or ring) on parameters that apply to new notes;
    // the spoken description says so too.
    property bool knobNextNoteMark: false
    property bool knobNextNoteRound: false
    property color knobNextNoteInk: inkDim
    property int knobNextNoteSize: 6
    property real knobNextNoteInset: 7
    property real knobNextNoteTop: 1
    // The square mark turned to a diamond.
    property real knobNextNoteTurn: 45
    // The kit's own captions under the dial. A device with its own caption
    // layout (a wrapper over Knob) turns them off and draws its own.
    property bool knobCaptions: true
    // The face eases to a new value (a preset, a macro, undo) or jumps to it.
    property bool knobEases: true
    // Hero knobs: the value's size and its gap under the label.
    property int knobReadoutValueSize: typeLabel
    property real knobReadoutGap: 2
    // Macros (ADR-0059): the ring a driven knob wears, MAP mode and the macro
    // keys. Off for knobs whose device has its own menu or none.
    property bool knobMacros: true
    property real knobMacroRingOutset: 2
    property real knobMacroGlow: 1.5
    property real knobMacroRingOpacity: 0.85
    property real knobMacroArmedOpacity: 0.9
    property real knobMacroWashAlpha: 0.08
    property real knobMacroWashHoverAlpha: 0.22
    property int knobMacroBadgeSize: 13
    property real knobMacroBadgeOffset: 3
    property color knobMacroBadgeInk: "#0b0f14"
    property int knobMacroBadgeTextSize: typeMicro
    // Gestures: pixels of vertical drag across the whole range (Shift: fine),
    // the pointer over the dial, the right button (the knob's menu), Ctrl+click
    // to reset, and hover (the tooltip) on a knob that cannot be edited.
    property real knobDragPixels: 180
    property real knobFinePixels: 1600
    property int knobCursor: Qt.SizeVerCursor
    property bool knobRightClick: true
    property bool knobCtrlClickResets: false
    property bool knobLockedHover: false
    // Typing a value (a wrapper's value readout opens the field): the field's
    // padding around the text and its inner margin, and the extra forms a
    // typed value may take: L/R for bipolar percentages, "inf" for the bottom
    // of a decibel range, and milliseconds for a range in seconds.
    property real knobFieldPad: 10
    property real knobFieldMargin: 3
    property bool knobTypePan: false
    property bool knobTypeInfinity: false
    property bool knobTypeSeconds: false
    // The default face (paintKnob): a 270° sweep from the lower left; the
    // graduated scale outside the arc; the arc; the brushed cap and its index.
    property real knobStartAngle: Math.PI * 0.75
    property real knobSweepAngle: Math.PI * 1.5
    property real knobScaleOutset: 2
    property int knobScaleTicks: 10
    property real knobScaleTickInner: 1
    property real knobScaleTickMajor: 5
    property real knobScaleTickMinor: 3
    // How near a tick must lie to the origin to be drawn long.
    property real knobScaleOriginTolerance: 0.051
    property real knobArcInset: 2
    property real knobArcStroke: 2
    property real knobArcStrokeLarge: 3
    // Dials wider than this draw the heavier arc.
    property real knobArcLargeAbove: 70
    // The smallest arc worth drawing, in radians.
    property real knobArcMinimum: 0.001
    property real knobCapInset: 6
    // The cap's sheen: the highlight's offset and core, as fractions of the cap.
    property real knobSheenX: 0.3
    property real knobSheenY: 0.45
    property real knobSheenCore: 0.05
    property real knobFocusEdgeWidth: 1.6
    property real knobIndexStroke: 2
    // The index runs from this fraction of the cap to just inside its rim.
    property real knobIndexFrom: 0.35
    property real knobIndexInset: 3

    // Both knobs and faders: Backspace resets like Delete.
    property bool resetOnBackspace: true

    // Fader: a vertical slider over a parameter. The cap travels between
    // faderTop and faderBottom; the track runs along the travel (or, with
    // faderTrackFull, the cap's whole extent); a lit bar marks the value;
    // scales, a foot line (label or value) and a value bubble are optional.
    property color faderAccent: accent
    property int faderWidth: 30
    property int faderHeight: 96
    property real faderDisabledOpacity: 0.5
    property real faderInactiveOpacity: knobInactiveOpacity
    property bool faderTip: true
    // The spoken name includes the parameter's group.
    property bool faderSpeaksGroup: false
    property int faderCursor: Qt.SizeVerCursor
    // Left and Right step too (Up and Down always do).
    property bool faderSideKeys: true
    property real faderTop: 2
    property real faderBottom: 2
    // The pointer area stops this far above the bottom (over a foot line).
    property real faderAreaBottom: 0
    // A press: "never" jumps (drags move from where the cap is), "offCap"
    // jumps to a press on the track beside the cap, "always" follows the
    // pointer unless Shift was held at the press (then a fine drag).
    property string faderJump: "never"
    // A press sets the value at once (even on the cap).
    property bool faderPressSets: false
    // A fine drag (Shift): over faderFinePixels when set, else the travel
    // scaled by faderFineScale.
    property real faderFinePixels: 0
    property real faderFineScale: 0.1
    // The track.
    property real faderTrackWidth: 6
    property real faderTrackRadius: radiusSm
    property bool faderTrackFull: true
    property color faderTrackFill: well
    property color faderTrackEdge: wellEdge
    // Unset: the fader's accent while focused.
    property var faderTrackFocusEdge: undefined
    // The lit bar: "origin" from the range's origin (the centre of a bipolar
    // range) to the value, "end" from the value to the track's end (less
    // faderLitInset), or "none".
    property string faderLit: "origin"
    // Whether a bipolar range lights from its centre (else from the bottom).
    property bool faderLitFromCentre: true
    property real faderLitWidth: 2
    property real faderLitRadius: 1
    property real faderLitOpacity: 1
    property real faderLitInset: 0
    // The cap: faderCapWidth wide, or (0) the fader's width less faderCapInset;
    // a grip line across it.
    property real faderCapWidth: 22
    property real faderCapInset: 0
    property real faderCapHeight: 12
    property real faderCapRadius: radiusXs
    property real faderGripInset: 6
    property real faderGripHeight: 2
    // Unset: the fader's accent.
    property var faderGripInk: undefined
    // A short rule across the track at a fader's detent.
    property real faderDetentWidth: 14
    property color faderDetentInk: gridStrong
    // Scales beside the track: faderTicks even divisions, every
    // faderTickMajorEvery-th drawn long. "edges" sets them at the fader's
    // sides (the right one right-aligned), "track" beside the track. With
    // faderStepScale the left scale marks each step of a stepped parameter of
    // up to faderStepLimit steps (all long; the right scale then hides).
    property int faderTicks: 0
    property int faderTickMajorEvery: 5
    property real faderTickMajor: 5
    property real faderTickMinor: 3
    property real faderTickGap: 2
    property string faderTickAnchor: "track"
    // Ticks centred on their positions (else hanging below them).
    property bool faderTickCentred: true
    property bool faderStepScale: false
    property int faderStepLimit: 12
    property color faderTickInk: inkFaint
    // The foot line under the fader: "label", "value" (the value less
    // faderValueSuffix, in the accent while dragged) or "none".
    property string faderFoot: "none"
    property real faderFootGap: 2
    property real faderLabelInset: 4
    property int faderLabelSize: typeCaption
    property bool faderLabelBold: true
    property real faderLabelSpacing: 0.4
    property color faderLabelInk: inkDim
    property int faderValueSize: typeCaption
    property color faderValueInk: inkDim
    property string faderValueSuffix: " dB"
    // A value bubble over the cap while touched.
    property bool faderBubble: false
    property real faderBubbleHeight: 16
    property real faderBubblePad: 10
    property real faderBubbleGap: 4
    property real faderBubbleRadius: radiusSm
    property int faderBubbleSize: typeCaption
    property color faderBubbleFill: well
    property color faderBubbleInk: ink
    function faderCapFill(fader, stop) {
        return stop === 0 ? knobTop : knobBottom
    }
    function faderCapEdge(fader) {
        return fader.activeFocus || fader.pressed ? fader.accent : plateHi
    }

    // Segment: tiles for a short choice list. The default is a row of pills
    // (a column of tiles when vertical) lit in the accent, with a rule down
    // the chosen tile; segmentLed gives latching keys with an LED instead.
    property color segmentAccent: accent
    property int segmentWidth: 140
    property int segmentHeight: 26
    property int segmentSpacing: 4
    property int segmentSpacingVertical: 4
    property real segmentFocusMargin: 3
    property real segmentFocusRadius: 6
    property color segmentFocusFill: "transparent"
    // Corner radius; negative makes horizontal tiles pills.
    property real segmentRadius: -1
    property real segmentRadiusVertical: 5
    // The fill gradient runs across the tile (or top to bottom).
    property bool segmentFillAcross: true
    // How far the chosen key sinks.
    property real segmentPressDepth: 0
    // "rule" down the chosen tile in vertical form, "bar" along it, or "none".
    property string segmentRule: "rule"
    property bool segmentWash: false
    property bool segmentLed: false
    property real segmentLedX: 5
    property int segmentLedSize: 4
    property color segmentLedOff: ledOff
    property bool segmentLedRim: false
    // Glyphs: "lead" before a key's label, "column" beside a vertical
    // tile's text, or "fill" in place of the label.
    property string segmentGlyphMode: "lead"
    property color segmentGlyphIdleInk: inkDim
    property real segmentTextOffset: 3
    property bool segmentUpper: false
    property string segmentFont: sans
    property int segmentSizeVertical: 12
    property real segmentLetterSpacing: 0.6
    property real segmentLetterSpacingVertical: 2.2
    property real segmentTextXVertical: 16
    property real segmentTextRightVertical: 6
    property real segmentCaptionGap: 2
    property real segmentCaptionSpacing: 0.8
    property int segmentSize: typeSmall
    property int segmentCaptionSize: typeMicro
    property real segmentDisabledOpacity: 0.5
    // Horizontal tiles' text inset from both sides; a vertical tile's text
    // beside a glyph column.
    property real segmentTextInset: 4
    property real segmentTextXVerticalGlyph: 48
    // The rule down a chosen vertical tile: its inset and width (inset from
    // the top and bottom alike).
    property real segmentRuleX: 4
    property real segmentRuleY: 5
    property real segmentRuleWidth: 3
    // The bar along a chosen tile: inset from the tile's ends, its width
    // (vertical) or height (horizontal), and its corners.
    property real segmentBarInset: 3
    property real segmentBarWidth: 3
    property real segmentBarHeight: 2
    property real segmentBarRadius: 1
    property color segmentLedRimInk: Qt.rgba(0, 0, 0, 0.4)
    // Glyph boxes. "fill": inset (top and bottom alike) and the width it
    // gives up; "column": fixed width and at most this tall, giving up
    // segmentGlyphColumnPad of the tile's height; "lead": before the label.
    property real segmentGlyphFillX: 9
    property real segmentGlyphFillY: 4
    property real segmentGlyphFillPad: 12
    property real segmentGlyphColumnX: 12
    property real segmentGlyphColumnWidth: 26
    property real segmentGlyphColumnHeight: 18
    property real segmentGlyphColumnPad: 6
    property real segmentGlyphLeadX: 11
    property real segmentGlyphLeadWidth: 16
    property real segmentGlyphLeadPad: 9
    // A key's label: shifted past a leading glyph and narrowed by
    // segmentKeyGlyphPad, else narrowed by segmentKeyTextPad more.
    property real segmentKeyGlyphOffset: 9
    property real segmentKeyGlyphPad: 30
    property real segmentKeyTextPad: 6
    property int segmentKeySize: typeCaption

    function segmentFill(tile, stop) {
        if (stop === 0)
            return tile.on ? Qt.rgba(tile.accent.r, tile.accent.g, tile.accent.b, 0.16)
                           : tile.pressed ? kit.button : kit.buttonHi
        return tile.on ? Qt.rgba(kit.steel.r, kit.steel.g, kit.steel.b, 0.10) : kit.button
    }
    function segmentEdge(tile) {
        return tile.on ? Qt.rgba(tile.accent.r, tile.accent.g, tile.accent.b, 0.55)
                       : tile.hovered ? kit.wellEdge : kit.seam
    }
    function segmentWashColor(tile, stop) {
        return "transparent"
    }
    function segmentLabelInk(tile) {
        return tile.on ? kit.ink : kit.inkDim
    }
    function segmentBold(tile) {
        return tile.on && !tile.vertical
    }
    function segmentCaptionInk(tile) {
        return tile.on ? tile.accent : kit.inkFaint
    }

    // PeakMeter: a channel's held peak, safe to -12 dBFS, hot to -3 and
    // over above, in a recessed well with a bright hold line.
    property color meterSafe: live
    property color meterHot: "#f2c35c"
    property color meterOver: "#ff5d55"
    property color meterWell: well
    property color meterHold: glacier
    // The scale (dBFS): its floor and ceiling, the hot and over zones, and
    // the level standing for silence.
    property real peakMeterFloorDb: -60
    property real peakMeterCeilDb: 6
    property real peakMeterHotDb: -12
    property real peakMeterOverDb: -3
    property real peakMeterSilentDb: -200
    property int peakMeterWidth: 12
    property int peakMeterHeight: 200
    // The bar's inset in the well, the hold line's height, the step that
    // makes zone boundaries hard edges, and how far the safe zone darkens
    // toward the floor.
    property real peakMeterInset: spaceXxs
    property real peakMeterHoldHeight: strokeBold
    property real peakMeterZoneEdge: 0.001
    property real peakMeterSafeFade: 1.6

    // Views: the filter scope (and the envelope view's frame defaults).
    property real viewRadius: 6
    property color viewEdge: wellEdge
    property color viewGrid: grid
    property color viewMarker: ink
    property color viewLabelInk: inkDim
    // FilterScope: the response from scopeCeilingDb down to scopeFloorDb,
    // padded inside the plot; a faint grid; a fill fading across the plot
    // under the line; the cutoff dot; the cutoff readout's corner.
    property real scopeTopPad: 6
    property real scopeBottomPad: 4
    property real scopeCeilingDb: 12
    property real scopeFloorDb: -48
    property real scopeGridOpacity: 0.6
    property real scopeFillAlpha: 0.22
    property real scopeFillEndAlpha: 0.05
    property real scopeLineWidth: 1.6
    property real scopeMarkerRadius: 3
    property real scopeLabelX: 6
    property real scopeLabelY: 3
    // FilterView: the plot inside its frame (room above for the caption and
    // below for the frequency labels), its dB range, the fill fading down,
    // the glow under the line, the dashed cutoff marker, the caption and
    // readout insets, and the frequency labels' offset and lift.
    property real filterViewMargin: 6
    property real filterViewTop: 22
    property real filterViewBottom: 16
    property real filterViewCeilingDb: 24
    property real filterViewFloorDb: -42
    property real filterViewFillAlpha: 0.32
    property real filterViewFillEndAlpha: 0.02
    property real filterViewGlowAlpha: 0.25
    property real filterViewGlowWidth: 6
    property var filterViewMarkerDash: [3, 3]
    property real filterViewMarkerOpacity: 0.55
    property real filterViewTextInset: 10
    property real filterViewCaptionY: 6
    property real filterViewCaptionSpacing: 1.4
    property real filterViewReadoutY: 5
    property real filterViewTickLabelOffset: 3
    property real filterViewTickLabelLift: 14

    // Curve editor: a document curve's points joined by bent segments in a
    // well (CurveEditor). The line is a wide faint glow under a solid
    // stroke; points are ringed dots; bend rings sit on segment midpoints.
    property real curveRadius: radiusXl
    property color curveFill: well
    property color curveEdge: wellEdge
    property color curveLine: accent
    property real curveLineWidth: strokeBold
    property real curveGlowWidth: 7
    property real curveGlowAlpha: 0.18
    // Points the line is drawn through, across the whole curve.
    property int curveSamples: 400
    property real curvePointRadius: 5
    property real curvePointWidth: 1.5
    property color curvePointFill: well
    // A point being dragged or selected.
    property color curvePointActiveFill: curveLine
    property color curvePointEdge: curveLine
    // How near (px) the pointer must be to grab a point.
    property real curveHitRadius: 10
    // Vertical drag (px) for one unit of tension on an Alt-drag bend.
    property real curveBendTravel: 80
    property real curveRingRadius: 4.5
    property real curveRingWidth: 1.4
    property real curveRingHitRadius: 8
    property color curveRingFill: well
    property color curveRingEdge: curveLine
    property real curvePlayheadWidth: strokeBold

    // Envelope: EnvelopeView's outline over a fill in a framed well. The
    // drawing sits envelopeMargin inside the frame (the top and bottom may
    // differ, leaving room for labels); the plot sits inside the drawing by
    // the plot insets, and full level stops envelopeHeadroom below its top.
    property real envelopeRadius: viewRadius
    property color envelopeWell: well
    property color envelopeEdge: viewEdge
    property real envelopeEdgeWidth: hairline
    property real envelopeMargin: 4
    property real envelopeMarginTop: envelopeMargin
    property real envelopeMarginBottom: envelopeMargin
    property real envelopePlotSide: 0
    property real envelopePlotTop: 2
    property real envelopePlotBottom: 2
    property real envelopeHeadroom: 0
    // The outline, a wide faint glow stroked under it (none at zero width),
    // and the fill: the accent at envelopeFillAlpha, or a gradient down the
    // plot to envelopeFillEndAlpha (or to clear), painted at
    // envelopeFillOpacity, over the outline or under it.
    property real envelopeLineWidth: 1.6
    property string envelopeLineJoin: "miter"
    property real envelopeGlowWidth: 0
    property real envelopeGlowAlpha: 0
    property bool envelopeFilled: true
    property bool envelopeFillOverLine: true
    property real envelopeFillAlpha: 0.12
    property bool envelopeGradient: false
    property real envelopeFillEndAlpha: 0
    property bool envelopeFadeToClear: false
    property real envelopeFillOpacity: 1
    // A rule along a level, offset from it (after rounding to a pixel when
    // snapped); a shaded band from the left edge, full height.
    property bool envelopeBaseline: false
    property color envelopeBaselineInk: viewGrid
    property real envelopeBaselineOffset: 0
    property bool envelopeBaselineSnap: false
    property color envelopeBandFill: "transparent"
    // A grid of rows and columns over the plot, snapped to pixel centres or not.
    property int envelopeGridRows: 0
    property int envelopeGridColumns: 0
    property color envelopeGridInk: viewGrid
    property bool envelopeGridSnap: false
    // Dashed markers (release points, stage edges), under or over the outline.
    property color envelopeMarkerInk: inkFaint
    property real envelopeMarkerWidth: hairline
    property var envelopeMarkerDash: [3, 3]
    property bool envelopeMarkerSnap: false
    property bool envelopeMarkersOver: true
    // Nodes on the outline (drawn, or a device's drag handles), how near a
    // press grabs one, and the playhead with the dot riding the level.
    property color envelopeKnotFill: plate
    property real envelopeKnotRadius: 4
    property real envelopeKnotLineWidth: 1.5
    property real envelopeKnotGrab: 8
    property color envelopePlayheadInk: ink
    property real envelopePlayheadWidth: 2
    property real envelopePlayheadRadius: 4
    // Labels a device adds: inset from the side, down from the top, up from
    // the bottom (anchored), or lifted from the bottom edge (a top offset).
    property color envelopeLabelInk: viewLabelInk
    property string envelopeLabelFont: mono
    property int envelopeLabelSize: 9
    property real envelopeLabelSpacing: 0
    property real envelopeLabelInset: 8
    property real envelopeLabelTop: 3
    property real envelopeLabelBottom: 3
    property real envelopeLabelLift: 0

    // Card: a titled plate grouping controls.
    property real cardRadius: 6
    property color cardSheen: Qt.rgba(1, 1, 1, 0.04)
    property real cardTitleX: 14
    property real cardTitleY: 10
    property color cardTitleInk: inkDim
    property string cardTitleFont: sans
    property int cardTitleSize: 9
    property real cardTitleSpacing: 3
    property real cardNoteGap: 10
    property bool cardNoteBold: false
    property int cardNoteSize: typeMicro
    property real cardNoteSpacing: 1.2

    // Preset arrows beside the preset browser.
    property int arrowWidth: 28
    property int arrowHeight: 42
    property real arrowRadius: 6
    property string arrowPrevious: "‹"
    property string arrowNext: "›"
    property color arrowInk: inkDim
    property int arrowGlyphSize: 20
    property real arrowGlyphOffset: 0
    // What the arrows step through, as spoken ("Previous preset").
    property string arrowNoun: "preset"
    function arrowFill(arrow, stop) {
        return arrow.pressed ? button : buttonHi
    }
    function arrowEdge(arrow) {
        return arrow.activeFocus ? accent : wellEdge
    }

    // The status pill.
    property int statusHeight: 24
    property int statusPad: 30
    property real statusLampX: 10
    property int statusLampSize: 8
    property real statusTextX: 22
    property real statusEdgeAlpha: 0.7
    property color statusPendingInk: inkDim
    property int statusTextSize: typeSmall
    function statusFill(tone) {
        return well
    }
    function statusInk(tone) {
        return ink
    }

    // Dropdown: a readout well that opens the menu.
    property color dropdownAccent: accent
    property int dropdownWidth: 96
    property int dropdownHeight: 24
    property real dropdownRadius: 3
    property color dropdownEdge: wellEdge
    property real dropdownDisabledOpacity: 0.5
    property bool dropdownSpeaksGroup: true
    property real dropdownTextX: 8
    property real dropdownTextRight: 24
    property string dropdownFont: mono
    property int dropdownTextSize: 11
    property bool dropdownBold: false
    property real dropdownChevronMargin: 7
    // Unset: the dropdown's accent.
    property var dropdownChevronInk: undefined
    // A drawn caret instead of the ▾ glyph.
    property bool dropdownCaret: false
    // An inactive dropdown (its choice not in play) dims to this.
    property real dropdownMutedOpacity: 0.36
    property int dropdownChevronSize: typeSmall
    // The drawn caret: its left edge from the right, its box and stroke.
    property real dropdownCaretRight: 15
    property real dropdownCaretWidth: 8
    property real dropdownCaretHeight: 5
    property real dropdownCaretStroke: 1.4
    function dropdownFill(dropdown) {
        return well
    }
    function dropdownInk(dropdown) {
        return ink
    }

    // Stepper: a value well with step arrows.
    property color stepperAccent: accent
    property color stepperFill: well
    property color stepperEdge: wellEdge
    property color stepperInk: ink
    property int stepperWellWidth: 80
    property int stepperWellHeight: 22
    property real stepperDisabledOpacity: 0.5
    property real stepperArrowWidth: 14
    property int stepperArrowSize: typeMicro
    property real stepperLabelSpacing: 0.9

    // Toggle: a tile with an LED (blooming when lit), the name and a
    // caption for the state.
    property int toggleTileWidth: 150
    property int toggleTileHeight: 36
    property real toggleTileDisabledOpacity: 0.5
    property real toggleTileLitEdgeAlpha: 0.6
    property real toggleTileFocusEdgeWidth: 1.5
    property real toggleTileLedX: 11
    property real toggleTileLedSize: 9
    property real toggleTileLedLift: 1.3
    property real toggleTileBloomSize: 21
    property real toggleTileBloomAlpha: 0.16
    property real toggleTileTextGap: 10
    property real toggleTileTextRight: 8
    property real toggleTileCaptionGap: 1
    property real toggleTileNameSpacing: 2
    property real toggleTileCaptionSpacing: 0.6

    // MacroMenu: a knob's map-to-macro popup, the same on every panel.
    property real macroMenuMargin: 8
    property int macroMenuWidth: 196
    property real macroMenuPad: 6
    property real macroMenuRadius: 7
    property color macroMenuFill: "#151b23"
    property color macroMenuEdge: "#2b3542"
    property color macroMenuTitleInk: "#8793a3"
    property int macroMenuRowHeight: 24
    property color macroMenuHighlight: "#243042"
    property real macroMenuSwatchX: 6
    property real macroMenuSwatchSize: 8
    property real macroMenuTextX: 20
    property color macroMenuInk: "#d6dde6"
    property color macroMenuRemoveInk: "#f0a0a0"

    // Switch: a compact LED button for two-way choices.
    property int switchHeight: 20
    property int switchPad: 24
    // Corner radius; negative makes a pill.
    property real switchRadius: 2
    property real switchDisabledOpacity: 0.5
    property real switchLedX: 5
    property int switchLedSize: 6
    property bool switchLedRim: true
    property real switchTextX: 15
    property int switchTextSize: 9
    property real switchLetterSpacing: 0
    function switchFill(toggle, stop) {
        return stop === 0 ? (toggle.pressed ? well : buttonHi) : button
    }
    function switchEdge(toggle) {
        return toggle.activeFocus ? toggle.accent : wellEdge
    }
    function switchLed(toggle) {
        return toggle.on ? toggle.accent : well
    }
    function switchLedEdge(toggle) {
        return toggle.on ? Qt.lighter(toggle.accent, 1.3) : wellEdge
    }
    function switchInk(toggle) {
        return toggle.on ? ink : inkDim
    }

    // Shared by Slider, ChoiceTiles, Button and SegmentedControl.
    // The tip doubles as the control's spoken name instead of describing it.
    property bool tipIsName: false
    // Text drawn on an accent fill: a lit button or segment.
    property color accentInk: well

    // Slider: a labelled horizontal slider. Stacked, the name and value sit
    // on a caption line over the rail with an optional hint under it;
    // otherwise name, rail and value share one row.
    property bool sliderStacked: true
    property int sliderWidth: 200
    // Stacked: the caption line (0 takes the caption text's own height),
    // the gap from it to the rail, the rail and the hint's line count.
    property int sliderCaptionHeight: 18
    property int sliderRailGap: 0
    property int sliderRailHeight: 22
    property int sliderHintLines: 2
    // Between the name and the value. The value ends the caption line, or
    // (inline) follows the name.
    property int sliderCaptionGap: spaceMd
    property bool sliderValueInline: false
    // One row: its height, the name and value columns, and the gap each
    // side of the rail.
    property int sliderHeight: 26
    property int sliderLabelWidth: 62
    property int sliderValueWidth: 58
    property int sliderGap: 5
    // The groove, and its edge idle and focused (transparent: none).
    property int sliderTrack: 4
    property color sliderGroove: track
    property color sliderGrooveEdge: "transparent"
    property color sliderGrooveFocusEdge: "transparent"
    // The groove inset by half the handle each side, so the handle's
    // centre reaches the ends; or the handle kept inside the groove.
    property bool sliderGrooveInset: true
    property bool sliderHandleInside: false
    property int sliderHandle: 14
    property color sliderHandleFill: ink
    property real sliderHandleEdgeWidth: hairline
    property real sliderHandleActiveEdgeWidth: strokeBold
    // The handle's edge idle and while focused or held; unset: the accent.
    property var sliderHandleEdge: wellEdge
    property var sliderHandleActiveEdge: undefined
    // The name, idle and focused (unset: the accent).
    property string sliderLabelFont: sans
    property real sliderLabelSize: typeEmphasis
    property int sliderLabelWeight: Font.Normal
    property real sliderLabelSpacing: 0
    property bool sliderLabelUpper: false
    property color sliderLabelInk: ink
    property var sliderLabelFocusInk: undefined
    // The value, idle and while dragged (unset: the accent); in one row it
    // elides to its column.
    property string sliderValueFont: mono
    property real sliderValueSize: typeLabel
    property int sliderValueWeight: Font.Normal
    property color sliderValueInk: inkDim
    property var sliderValueDragInk: undefined
    property bool sliderValueElide: true
    property string sliderHintFont: sans
    property real sliderHintSize: typeBody
    property color sliderHintInk: inkDim
    // Behaviour: the touch target also covers the caption (stacked); the
    // pointer shows a resize cursor; hover is tracked. Shift drags this
    // many times finer; a key steps a plain value with no step by this
    // fraction of its range. Dimmed while the parameter cannot change.
    property bool sliderTouchCaption: false
    property bool sliderResizeCursor: true
    property bool sliderHover: true
    property real sliderFineDivisor: 10
    property int sliderNudgeSteps: 20
    property real sliderDisabledOpacity: 0.5

    // ChoiceTiles: a choice parameter as tiles (a name and an optional hint)
    // or as a segment of joined buttons.
    property int choiceWidth: 320
    property int choiceColumns: 4
    property int choiceTileHeight: 44
    property int choiceSpacing: spaceMd
    property int choiceSegmentWidth: 44
    property real choiceRadius: 10
    property real choiceEdgeWidth: hairline
    property color choiceTile: button
    property color choiceTileHover: buttonHi
    // A chosen tile's tint, unless a choice sets its own, and a chosen
    // segment button.
    property color choiceTileOn: accentSoft
    property color choiceSegmentOn: buttonHi
    property color choiceTileEdge: wellEdge
    // The focus ring outside the tiles; its ink unset: the accent.
    property real choiceFocusMargin: 3
    property real choiceFocusRadius: 12
    property real choiceFocusWidth: hairline
    property var choiceFocusInk: undefined
    // A tile's text: inset from its edges when it has a hint, else from
    // both sides; the gap between name and hint.
    property real choiceTextPad: 14
    property real choiceTextInset: 0
    property real choiceHintGap: 3
    property string choiceFont: sans
    // The name's size; a hint is choiceHintSize under a name this size or
    // larger, else choiceHintSizeSmall.
    property int choiceNameSize: typeHeading
    property int choiceHintSize: typeLabel
    property int choiceHintSizeSmall: typeBody
    property color choiceHintInk: inkDim
    // Tiles in whole pixels; the last tile of a short row spanning the rest.
    property bool choiceWholePixels: false
    property bool choiceSpanLast: true
    // The spoken name: "groupUnlessSame" prefixes the group unless the name
    // already is it, "group" always prefixes it.
    property string choiceLongName: "groupUnlessSame"
    property real choiceDisabledOpacity: 0.5
    function choiceTileFill(tile) {
        if (tile.on)
            return tile.segment ? choiceSegmentOn : tile.tint
        return tile.hovered ? choiceTileHover : tile.segment ? "transparent" : choiceTile
    }
    function choiceNameInk(tile) {
        return tile.on || !tile.segment ? ink : inkDim
    }
    function choiceNameWeight(tile) {
        return tile.hint !== "" && tile.nameSize >= choiceNameSize ? Font.DemiBold : Font.Normal
    }

    // Button: a push button, plain, on, prime (the main action) or done, in
    // three sizes (small, regular, large).
    property int pushButtonHeight: 26
    property int pushButtonHeightSmall: 22
    property int pushButtonHeightLarge: 42
    // Added to the text's width.
    property int pushButtonPad: 20
    property int pushButtonPadSmall: 14
    property int pushButtonPadLarge: 44
    property real pushButtonRadius: 7
    property real pushButtonRadiusLarge: 9
    property string pushButtonFont: sans
    property int pushButtonTextSize: typeBody
    property int pushButtonTextSizeSmall: typeSmall
    property int pushButtonTextSizeLarge: typeSubtitle
    property int pushButtonWeight: Font.DemiBold
    property real pushButtonLetterSpacing: 0
    property real pushButtonDisabledOpacity: 0.35
    // The narrowest a button sizes itself (0: as wide as its text and pad).
    property int pushButtonMinWidth: 0
    // The edge's width, idle and while focused.
    property real pushButtonEdgeWidth: hairline
    property real pushButtonFocusEdgeWidth: hairline
    // The default look: lit (on or prime) in the accent, else a face that
    // lightens and an edge and text that brighten under the pointer.
    property color pushButtonFace: button
    property color pushButtonHover: buttonHi
    property color pushButtonIdleEdge: wellEdge
    property color pushButtonHoverEdge: inkFaint
    property color pushButtonFocusEdge: ink
    property color pushButtonIdleInk: inkDim
    property color pushButtonHoverInk: ink
    // A pointing-hand cursor; colour changes fade; with no tip the tooltip
    // reads the button's text.
    property bool pushButtonPointer: false
    property bool pushButtonFade: false
    property bool pushButtonTipFallback: true
    // A click also takes keyboard focus (so the focus edge shows).
    property bool pushButtonFocusOnClick: false
    function pushButtonFill(button) {
        return button.lit ? button.accent : button.hovered ? pushButtonHover : pushButtonFace
    }
    function pushButtonEdge(button) {
        return button.activeFocus ? pushButtonFocusEdge : button.lit ? button.accent
             : button.hovered ? pushButtonHoverEdge : pushButtonIdleEdge
    }
    function pushButtonInk(button) {
        return button.lit ? button.accentInk : button.hovered ? pushButtonHoverInk : pushButtonIdleInk
    }

    // SegmentedControl: joined choices in a frame. Cells fit their text, or
    // (equal cells) share the width; latching keys may carry an LED.
    property int segmentedHeight: 26
    // An equal cell's width, for the implicit width.
    property int segmentedCellWidth: 34
    property bool segmentedEqualCells: false
    property real segmentedMargin: 1
    property real segmentedSpacing: 0
    // Added to a fitted cell's text width.
    property real segmentedCellPad: 18
    property real segmentedRadius: 7
    property real segmentedCellRadius: 0
    property color segmentedFrame: well
    property color segmentedFrameEdge: wellEdge
    property bool segmentedClip: false
    // A rule between cells, inset from the top and bottom.
    property bool segmentedDividers: false
    property color segmentedDivider: wellEdge
    property real segmentedDividerInset: 0
    // Cells filled top to bottom from segmentedFill(cell, 0) to (cell, 1).
    property bool segmentedGradient: false
    // A key's LED, and its text between the LED and the key's edge.
    property bool segmentedLeds: false
    property real segmentedLedX: 4
    property int segmentedLedSize: 5
    property real segmentedLedGap: 2
    property real segmentedLedTextInset: 3
    property real segmentedLedLift: 1.3
    // An icon in place of the label: at most this wide, inset from the
    // cell's sides, and this tall.
    property real segmentedIconWidth: 22
    property real segmentedIconInset: 6
    property real segmentedIconHeight: 11
    property string segmentedFontFamily: sans
    property int segmentedTextSize: typeBody
    property int segmentedWeight: Font.DemiBold
    property color segmentedHover: buttonHi
    property color segmentedIdleInk: inkDim
    property color segmentedHoverInk: ink
    property color segmentedFocusEdge: ink
    // A tooltip per cell; cells spoken as radio buttons (else tabs); a
    // pointing-hand cursor; colour changes fade.
    property bool segmentedTips: false
    property bool segmentedRadios: false
    property bool segmentedPointer: false
    property bool segmentedFade: false
    property real segmentedDisabledOpacity: 0.5
    function segmentedFill(cell, stop) {
        return cell.on ? cell.accent : cell.hovered ? segmentedHover : "transparent"
    }
    function segmentedEdge(cell) {
        return cell.activeFocus ? segmentedFocusEdge : "transparent"
    }
    function segmentedInk(cell) {
        return cell.on ? cell.accentInk : cell.hovered ? segmentedHoverInk : segmentedIdleInk
    }
    function segmentedLedInk(cell) {
        return cell.on ? cell.accent : well
    }
    function segmentedLedEdge(cell) {
        return cell.on ? Qt.lighter(cell.accent, segmentedLedLift) : wellEdge
    }
    function segmentedFont(cell) {
        return Qt.font({ family: segmentedFontFamily, pixelSize: segmentedTextSize, weight: segmentedWeight })
    }

    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2 + kit.knobScaleOutset
        const start = kit.knobStartAngle
        const sweep = kit.knobSweepAngle
        const ticks = kit.knobScaleTicks
        const at = start + sweep * knob.position
        // The graduated scale: fine ticks, the ends and origin longer.
        ctx.strokeStyle = kit.gridStrong
        ctx.lineWidth = kit.hairline
        for (let i = 0; i <= ticks; ++i) {
            const a = start + sweep * i / ticks
            const major = i === 0 || i === ticks
                || (knob.origin > 0 && Math.abs(i / ticks - knob.origin) < kit.knobScaleOriginTolerance)
            const r0 = radius + kit.knobScaleTickInner
            const r1 = radius + (major ? kit.knobScaleTickMajor : kit.knobScaleTickMinor)
            ctx.beginPath()
            ctx.moveTo(c + Math.cos(a) * r0, c + Math.sin(a) * r0)
            ctx.lineTo(c + Math.cos(a) * r1, c + Math.sin(a) * r1)
            ctx.stroke()
        }
        // The track and the value arc.
        ctx.lineCap = "butt"
        ctx.lineWidth = knob.diameter > kit.knobArcLargeAbove ? kit.knobArcStrokeLarge : kit.knobArcStroke
        ctx.strokeStyle = kit.track
        ctx.beginPath()
        ctx.arc(c, c, radius - kit.knobArcInset, start, start + sweep)
        ctx.stroke()
        const from = start + sweep * knob.origin
        if (Math.abs(at - from) > kit.knobArcMinimum) {
            ctx.strokeStyle = knob.arc
            ctx.beginPath()
            ctx.arc(c, c, radius - kit.knobArcInset, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // The brushed cap: a soft radial sheen with a fine bevel.
        const cap = radius - kit.knobCapInset
        const fill = ctx.createRadialGradient(c - cap * kit.knobSheenX, c - cap * kit.knobSheenY,
                                              cap * kit.knobSheenCore, c, c, cap)
        fill.addColorStop(0, kit.knobTop)
        fill.addColorStop(1, kit.knobBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, cap, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? kit.knobFocusEdgeWidth : kit.hairline
        ctx.strokeStyle = knob.activeFocus ? kit.accent : kit.plateHi
        ctx.stroke()
        // The index: a white line from the centre toward the rim.
        ctx.lineCap = "round"
        ctx.lineWidth = kit.knobIndexStroke
        ctx.strokeStyle = kit.glacier
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * cap * kit.knobIndexFrom, c + Math.sin(at) * cap * kit.knobIndexFrom)
        ctx.lineTo(c + Math.cos(at) * (cap - kit.knobIndexInset), c + Math.sin(at) * (cap - kit.knobIndexInset))
        ctx.stroke()
    }
}
