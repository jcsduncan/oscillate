pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit"

// Nova design tokens (spec NOVA-UI-16, ADR-0076): true neutral graphite
// (no blue in the greys), and one accent per module so no single colour
// owns the panel: oscillators cyan, Sub · Noise green, filters and
// envelopes amber, FX pink, voices, LFOs and the other sources violet, the
// output and global settings neutral white (NovaPanel.accentFor). Set in
// Inter with JetBrains Mono numbers; the category colours mark only the
// preset categories.
KitTheme {
    prefix: "nova"
    accent: cy
    accent2: vi
    accentSoft: "#3a3c41"
    sans: "Inter"
    mono: "JetBrains Mono"
    cardTitleInk: text
    cardTitleSize: 10
    cardTitleSpacing: 2
    dropdownWidth: 110
    dropdownHeight: 24
    dropdownTextSize: 11
    dropdownRadius: 6
    dropdownFont: sans
    dropdownEdge: line2
    dropdownChevronInk: dim
    menuMinWidth: 170
    menuRadius: 9
    menuRowRadius: 5
    menuFill: popFill
    menuEdge: line2
    menuHeaderInk: faint
    tipFill: popFill
    tipEdge: line2
    tipFont: mono
    statusHeight: 26
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 11
    arrowWidth: 34
    arrowHeight: 44

    // The plate and its parts: neutral graphite, darkest at the rails.
    property color bg: "#0a0a0b"
    property color plateTop: "#121314"
    property color plateMid: "#0f1011"
    property color plateBottom: "#0d0e0f"
    property color rail: "#0c0c0d"
    property color card: "#151618"
    property color cardBottom: "#131416"
    property color card2: "#1b1c1f"
    property color wellFill: "#08090a"
    property color line: "#232427"
    property color line2: "#303236"
    property color text: "#ececee"
    property color dim: "#9a9ca3"
    property color faint: "#7e8189"
    property color tab: "#1e1f22"
    property color popFill: "#111214"
    // Over the overview while a card is open large: near-black at 60 %.
    property color scrim: "#99050506"
    // Hover over a well or plate, and a deeper hover over a dark well.
    property color hover: "#1a1b1e"
    property color hoverDeep: "#131416"
    // A selected row or tile: graphite a step up from the card; the
    // selection's accent edge carries its colour.
    property color selectedFill: "#171a1c"
    // The clip sequencer's piano rows.
    property color keyRowBlack: "#0b0b0c"
    property color keyRowWhite: "#121315"
    // The key strip: ivory-grey white keys, graphite black keys, lit in
    // cyan while a key sounds.
    property color keyWhiteTop: "#dedfe2"
    property color keyWhiteBottom: "#b9bbc0"
    property color keyBlack: "#1a1b1e"
    property color keyLabel: "#55575d"
    property color keyLitTop: cy
    property color keyLitBottom: Qt.darker(cy, 1.35)

    // The three accents, and the strip's two module accents.
    property color cy: "#46d6ff"
    property color vi: "#a98bff"
    property color am: "#f5b14c"
    property color pk: "#ff6fb5"
    property color gr: "#5fe3a1"
    property color cyLight: "#9fe9ff"
    property color cySoft: soft(cy)
    property color viSoft: soft(vi, 0.16)
    property color viBorder: "#433c5c"
    property color viTab: "#1d1b26"
    // Dark ink on a violet fill (the drag ghost).
    property color viInk: "#120a2a"
    property color red: "#ff6b7a"
    // The output and global settings: neutral white.
    property color neutral: text
    // How strongly an accent tints a soft fill, a glow and a halo.
    property real softAlpha: 0.14
    property real glowAlpha: 0.18
    property real haloAlpha: 0.2
    function soft(colour, alpha) {
        return Qt.rgba(colour.r, colour.g, colour.b, alpha === undefined ? softAlpha : alpha)
    }

    // The LFO curve editor: violet on the well.
    curveEdge: line
    curveLine: vi
    curveGlowWidth: 6.8
    curveGlowAlpha: glowAlpha
    curveSamples: 300
    curvePointRadius: 4.5
    curvePointFill: popFill
    curveHitRadius: 9

    // Preset categories, by the factory's category names; anything else
    // (and User) is dim.
    readonly property var categoryColours: ({ "Bass": cy, "Lead": pk, "Pad": vi, "Pluck": gr, "Keys": am,
                                              "FX": red, "Growl": "#ff8a4c", "Arp/Seq": "#7fe0d0" })
    function categoryColour(name) {
        const colour = categoryColours[name]
        return colour !== undefined ? colour : dim
    }
    // The arp pattern library's categories (ADR-0078).
    readonly property var arpCategoryColours: ({ "Minimal": gr, "Deep House": am, "Techno": red,
                                                 "Acid": "#c8f25c", "Dub Chords": cy, "Trance Gate": pk,
                                                 "Classic": vi, "Polymetric": cyLight })
    function arpCategoryColour(name) {
        const colour = arpCategoryColours[name]
        return colour !== undefined ? colour : dim
    }

    // Knob faces.
    property color knobTrack: "#26272a"
    property color knobBodyHi: "#36383d"
    property color knobBodyMid: "#1d1e21"
    property color knobBodyLow: "#0f1012"
    property color knobEdge: "#303236"
    property color knobPointer: "#f2f2f3"
    property color gridLine: "#18191b"
    property color gridLine2: "#27282c"

    // Knob type (NovaKnob): a caption under every dial (smaller under the
    // small dials) and the value under it; each knob is at least as wide as
    // both, so neighbours never overlap.
    property int knobCaptionSize: 10
    property int knobCaptionSizeSmall: 9
    property int knobCaptionHeight: 13
    property int knobCaptionHeightSmall: 11
    property real knobCaptionSpacing: 0.85
    property real knobCaptionSpacingSmall: 0.4
    knobValueSize: 10
    property int knobValueHeight: 12
    // The caption's gap under the dial, and the room the caption and the
    // widest value leave beside them.
    knobCaptionGap: 3
    property real knobTextPad: 2
    // Dials: "big", normal and "sm", and the least width each knob takes.
    knobDiameter: 38
    property int knobDiameterBig: 54
    property int knobDiameterSmall: 30
    property real knobBaseWidth: 46
    property real knobBaseWidthBig: 64
    property real knobBaseWidthSmall: 36

    // Knob behaviour (NovaKnob over the kit Knob): its own captions, no
    // easing or macro rings (right-click opens the route menu), Ctrl+click
    // resets, hover works while locked, and typed values may say "inf" dB or
    // give seconds in ms.
    knobCaptions: false
    knobEases: false
    knobMacros: false
    knobDialPad: 0
    knobCtrlClickResets: true
    knobLockedHover: true
    knobDragPixels: 160
    knobFinePixels: 600
    knobFieldPad: 8
    knobTypeInfinity: true
    knobTypeSeconds: true
    // Modulation: a press this near the rim (or Alt) drags the newest route's
    // depth; route arcs and live dots ride a ring just inside the rim; a
    // dragged source rings the dial in a halo.
    property real knobRingGrab: 4
    property real knobRouteInset: 1.5
    property real knobRouteStroke: 2
    property real knobRouteAlpha: 0.95
    property real knobRouteDot: 4.4
    property real knobHaloOutset: 3
    property real knobHaloOpacity: 0.6

    // The face (paintKnob): a 270° sweep from the lower left in degrees; the
    // track ring inset from the rim (heavier on dials over knobArcLargeAbove);
    // a lit dome inset inside it; a pointer from the dome's centre to near
    // the track.
    property real knobStartDegrees: -225
    property real knobSweepDegrees: 270
    property real knobTrackInset: 5
    knobArcStroke: 3
    knobArcStrokeLarge: 3.5
    knobArcLargeAbove: 40
    property real knobBodyInset: 5
    knobSheenX: 0.24
    knobSheenY: 0.36
    property real knobSheenReach: 1.1
    property real knobBodyMidStop: 0.6
    knobIndexFrom: 0.2
    property real knobPointerReach: 8
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const rT = c - knobTrackInset
        const rM = c - knobRouteInset
        const a = knob.angle(knob.position)
        ctx.lineCap = "round"
        ctx.lineWidth = size > knobArcLargeAbove ? knobArcStrokeLarge : knobArcStroke
        ctx.strokeStyle = knobTrack
        ctx.beginPath()
        ctx.arc(c, c, rT, knob.angle(0), knob.angle(1))
        ctx.stroke()
        const from = knob.angle(knob.origin)
        if (Math.abs(a - from) > knobArcMinimum) {
            ctx.strokeStyle = knob.accent
            ctx.beginPath()
            ctx.arc(c, c, rT, Math.min(from, a), Math.max(from, a))
            ctx.stroke()
        }
        // One violet arc per route, from the value over the route's depth.
        ctx.lineWidth = knobRouteStroke
        ctx.strokeStyle = vi
        for (let i = 0; i < knob.amounts.length; ++i) {
            const end = knob.angle(knob.position + knob.amounts[i])
            if (Math.abs(end - a) < knobArcMinimum)
                continue
            ctx.globalAlpha = knobRouteAlpha
            ctx.beginPath()
            ctx.arc(c, c, rM, Math.min(a, end), Math.max(a, end))
            ctx.stroke()
        }
        ctx.globalAlpha = 1
        // The body: a lit graphite dome with a fine rim, violet while a source hovers.
        const r = rT - knobBodyInset
        const fill = ctx.createRadialGradient(c - r * knobSheenX, c - r * knobSheenY, r * knobSheenCore,
                                              c, c, r * knobSheenReach)
        fill.addColorStop(0, knobBodyHi)
        fill.addColorStop(knobBodyMidStop, knobBodyMid)
        fill.addColorStop(1, knobBodyLow)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, r, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.dropHover ? strokeBold : hairline
        ctx.strokeStyle = knob.dropHover ? vi : knob.activeFocus ? knob.accent : knobEdge
        ctx.stroke()
        ctx.lineWidth = knobIndexStroke
        ctx.strokeStyle = knobPointer
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(a) * r * knobIndexFrom, c + Math.sin(a) * r * knobIndexFrom)
        ctx.lineTo(c + Math.cos(a) * (rT - knobPointerReach), c + Math.sin(a) * (rT - knobPointerReach))
        ctx.stroke()
    }

    // Knob groups (NovaKnobGroup): the sideways label's letter spacing.
    property real knobGroupLabelSpacing: 1.2

    plate: card
    plateEdge: line
    plateHi: line2
    well: wellFill
    wellEdge: line
    seam: line
    grid: gridLine
    gridStrong: line2
    ink: text
    inkDim: dim
    inkFaint: faint
    glacier: knobPointer
    button: wellFill
    buttonHi: hover
    ledOff: wellFill
    live: "#4fd18b"
    warning: am

    // The envelope editor: a glowing outline over a faint fill on a time
    // grid, ringed nodes, the newest voice's playhead, a caption on top.
    envelopeRadius: 8
    envelopeWell: wellFill
    envelopeEdge: line
    envelopeMargin: 0
    envelopePlotSide: 14
    envelopePlotTop: 14
    envelopePlotBottom: 14
    envelopeHeadroom: 10
    envelopeLineWidth: 2
    envelopeGlowWidth: 6.8
    envelopeGlowAlpha: glowAlpha
    envelopeFillAlpha: 0.08
    envelopeGridColumns: 8
    envelopeGridInk: gridLine
    envelopeKnotFill: popFill
    envelopeKnotRadius: 4
    envelopeKnotLineWidth: 1.5
    envelopeKnotGrab: 8
    envelopePlayheadInk: neutral
    envelopePlayheadWidth: 2
    envelopePlayheadRadius: 4
    envelopeLabelInk: faint
    envelopeLabelFont: sans
    envelopeLabelSize: 10
    envelopeLabelSpacing: 1.2
    envelopeLabelInset: 8
    envelopeLabelTop: 6

    function statusFill(tone) {
        return "transparent"
    }
    function statusInk(tone) {
        return tone
    }
    function arrowFill(arrow, stop) {
        return arrow.pressed ? wellFill : hover
    }
    function arrowEdge(arrow) {
        return arrow.activeFocus ? cy : "transparent"
    }

    // ---- Panel, voices, presets, tables, constellation, arp page tokens ----
    // The panel (NovaPanel): its case's corner.
    property real panelRadius: 14
    // The header: the gap between its parts; BROWSE, ✦ CONSTELLATION, the
    // preset box and the page bar share one height and corner; the keys
    // pill and zoom chips are shorter. Button labels' padding and tracking.
    property real headerSpacing: 10
    property real headerControlHeight: 30
    property real headerControlRadius: 7
    property real headerChipHeight: 24
    property real headerButtonPadding: 22
    property real headerLabelSpacing: 0.8
    // The NOVA wordmark: light capitals tracked wide, the gradient's cyan
    // stop, and the baseline's height above the box's foot.
    property int wordmarkSize: 19
    property real wordmarkTracking: 7
    property real wordmarkWidth: 96
    property real wordmarkHeight: 26
    property real wordmarkMidStop: 0.4
    property real wordmarkDescent: 5
    // The preset box: its arrows' and name's corners, the CATEGORY and
    // EDITED tags' tracking, and the EDITED tag.
    property real presetArrowRadius: 6
    property real presetNameRadius: 5
    property real presetTagSpacing: 0.8
    property real presetEditedPadding: 10
    property real presetEditedHeight: 15
    // The page bar's tabs.
    property real pageTabPadding: 9
    property real pageTabRadius: 5
    property real pageTabLabelSize: 9.5
    // The keys pill: its padding, its tone's fill and edge strength, the
    // light, the label's tracking.
    property real keysPillPadding: 20
    property real keysPillFillAlpha: 0.08
    property real keysPillEdgeAlpha: 0.3
    property real keysLedSize: 7
    property real keysLabelSpacing: 0.5
    // The signal strip: an item's height, the › separator's width, the
    // light, the title's tracking, the thumbnail (about 3.4 × 1.2 of 12 px
    // type) and an off module's thumbnail opacity.
    property real stripItemHeight: 20
    property real stripSeparatorWidth: 12
    property real stripLedSize: 6
    property real stripTitleSpacing: 0.8
    property real stripThumbnailWidth: 41
    property real stripThumbnailHeight: 14
    property real stripOffOpacity: 0.3
    // Thumbnail drawing: the trace's stroke, the output bars' side inset,
    // the voices sketch's first voice and each further voice's extra
    // strength, the output bars' track and level strength.
    property real stripThumbnailStroke: 1.4
    property real stripThumbnailInset: 2
    property real stripThumbnailVoiceAlpha: 0.35
    property real stripThumbnailVoiceAlphaStep: 0.15
    property real stripThumbnailMeterTrackAlpha: 0.25
    property real stripThumbnailMeterAlpha: 0.85
    // The status line: its side margin and its labels' tracking.
    property real statusLineMargin: 14
    property real statusLabelSpacing: 0.8
    // The route popover over a modulated knob.
    property real routePopoverWidth: 220
    property real routePopoverRadius: 9
    property real routePopoverPadding: 10
    property real routePopoverSpacing: 3
    property real routePopoverHeadingSpacing: 1.2
    property real routePopoverRowHeight: 18
    property real routePopoverSourceWidth: 70
    property real routePopoverBarWidth: 86
    property real routePopoverBarHeight: 4
    // The drag ghost: its offset from the pointer, padding and height.
    property real dragGhostOffsetX: 12
    property real dragGhostOffsetY: 8
    property real dragGhostPadding: 16
    property real dragGhostHeight: 20

    // The VOICES page (NovaVoicesPage).
    // Controls that cannot be edited fade to this.
    property real voicesLockedOpacity: 0.45
    // Section labels' tracking and the notes' line height.
    property real voicesLabelSpacing: 1.2
    property real voicesNoteLineHeight: 1.3
    // Cards: the content's side inset, the Analogue column's top, the lanes'
    // and voice cards' top under the header controls, and those controls' top.
    property real voicesCardInset: 14
    property real voicesAnalogueTop: 42
    property real voicesBodyTop: 44
    property real voicesHeaderControlTop: 9
    property real voicesSectionSpacing: 10
    property real voicesTabsGap: 14
    // Tolerance and drift sliders: row height, track, thumb, the grab
    // outset beside the track, the distance a press jumps the thumb from,
    // and the Shift drag's divisor.
    property real voicesSliderRowHeight: 22
    property real voicesSliderTrackHeight: 4
    property real voicesSliderThumbSize: 11
    property real voicesSliderGrabOutset: 6
    property real voicesSliderJumpDistance: 8
    property real voicesSliderFineDivisor: 4
    // The on/off switch.
    property real voicesToggleGap: 9
    property real voicesToggleHeight: 18
    property real voicesToggleTrackWidth: 30
    property real voicesToggleTrackHeight: 17
    property real voicesToggleKnobInset: 2
    property real voicesToggleKnobSize: 13
    property int voicesToggleDuration: 140
    // The Age dial: its arc's inset and stroke, the inner ring's inset from
    // the arc, the smallest age drawn, the value, the AGE caption and the hint.
    property real voicesAgeGap: 14
    property real voicesAgeArcInset: 10
    property real voicesAgeArcStroke: 7
    property real voicesAgeRingInset: 10
    property real voicesAgeArcMinimum: 0.001
    property int voicesAgeValueSize: 22
    property real voicesAgeCaptionBottom: 24
    property real voicesAgeCaptionSpacing: 1.4
    property real voicesAgeHintHeight: 54
    // The seed readout and the voice-cards stepper.
    property real voicesFieldHeight: 22
    property real voicesSeedPad: 14
    property real voicesStepperWidth: 112
    property real voicesStepperButtonWidth: 22
    // Voice Lanes' bars: width bounds and share of a voice's cell, the zero
    // line's lift, the room above and below the swing, the fill's tint, the
    // value cap, the +/0/− scale's column and baseline, and the voice
    // number and value lines from the bottom.
    property real voicesBarMinWidth: 6
    property real voicesBarWidthRatio: 0.56
    property real voicesBarMaxWidth: 56
    property real voicesLaneMidLift: 8
    property real voicesLaneAmpInset: 34
    property real voicesBarFillAlpha: 0.2
    property real voicesBarCapHeight: 3
    property real voicesScaleX: 16
    property real voicesScaleBaseline: 4
    property real voicesVoiceNumberBottom: 22
    property real voicesLaneValueSize: 9.5
    property real voicesLaneValueBottom: 8
    // Under the lanes: the legend, its swatches, the tools and the hint.
    property real voicesLegendTop: 14
    property real voicesLegendSpacing: 14
    property real voicesSwatchSize: 10
    property real voicesToolsTop: 10
    property real voicesHintTop: 44
    // The per-voice source chips.
    property real voicesSourceChipPad: 12
    property real voicesSourceChipHeight: 24
    property real voicesSourceChipSpacing: 5
    // A voice card: inset, the parts' top, spacing and height, and the
    // bipolar bar under each value.
    property real voicesVoiceCardInset: 9
    property real voicesVoiceCardPartsTop: 30
    property real voicesVoiceCardPartSpacing: 5
    property real voicesVoiceCardPartHeight: 17
    property real voicesPartBarBottom: 3
    property real voicesPartBarHeight: 2

    // The preset browser drawer (NovaPresetBrowser): its padding, the
    // chips' most height before they scroll, chip, row, field and action
    // heights.
    property real browserPad: 10
    property real browserChipsMaximum: 132
    property real browserChipHeight: 21
    property real browserRowHeight: 30
    property real browserTaggedRowHeight: 46
    property real browserHeaderRowHeight: 26
    property real browserFieldHeight: 28
    property real browserActionHeight: 24
    // The width a chip or footer action adds around its label.
    property real browserLabelPadding: 14
    property real browserChipSpacing: 0.6
    property real browserActionSpacing: 0.8
    // Fields, footer actions and the close button.
    property real browserControlRadius: 7
    property real browserFieldInset: 10
    property real browserDisabledOpacity: 0.4
    // The "/" key cap beside the search, and the × close button.
    property real browserSlashKeyWidth: 22
    property real browserSlashKeyHeight: 20
    property real browserCloseSize: 26
    property real browserChipGap: 5
    // The FACTORY / USER group headers' letter spacing.
    property real browserHeaderSpacing: 1.4
    // A row: the category dot, the name's left edge (the tags line up under
    // it), the ★ button, and how far the tags line rises into the row above.
    property real browserDotSize: 7
    property real browserNameX: 27
    property real browserStarSize: 22
    property real browserTagsRaise: 7
    // The Constellation (NovaConstellationView). Canvas CSS colours: the
    // sky's centre, the grid, the cursor, the links from the cursor to the
    // blended stars and the cursor's glow.
    property string constellationSkyCentre: "#121117"
    property string constellationGridInk: "rgba(255,255,255,0.035)"
    property string constellationCursorInk: "#ffffff"
    property string constellationLinkInk: "rgba(255,255,255,0.7)"
    property string constellationCursorGlowInk: "rgba(255,255,255,0.5)"
    // A star's glow, an unselected label's alpha and the cursor ring's stroke.
    property real constellationGlowAlpha: 0.35
    property real constellationLabelAlpha: 0.55
    property real constellationCursorStroke: 1.5
    // The axis captions: letter spacing and inset from the sky's bottom.
    property real constellationAxisSpacing: 1.6
    property real constellationAxisInset: 10
    // LISTENING TO PRESETS: the caption's spacing and the progress bar.
    property real constellationProgressSpacing: 1.4
    property real constellationProgressWidth: 220
    property real constellationProgressHeight: 3
    // The bottom bar: height, translucency over the sky, item spacing,
    // the labels' spacing, and its chips (height, lit wash, label spacing).
    property real constellationBarHeight: 40
    property real constellationBarAlpha: 0.9
    property real constellationBarSpacing: 10
    property real constellationLabelSpacing: 1.4
    property real constellationChipHeight: 22
    property real constellationChipFillAlpha: 0.08
    property real constellationChipSpacing: 0.8

    // The wavetable editor (NovaTableEditor): a sheet over the panel dimmed
    // by the plate's black, the frame editor beside a column of harmonics,
    // formula and resynthesis, and the frame strip along the bottom.
    property real tableScrimAlpha: 0.55
    property real tableSheetSide: 40
    property real tableSheetTop: 60
    property real tableSheetBottom: 40
    property real tableSheetRadius: 14
    property real tableSheetPadding: 14
    property real tableHeaderSpacing: 10
    property real tableTitleSpacing: 2
    property real tableSubtitleSpacing: 0.6
    // The action pills' and the tool rows' tops.
    property real tableActionsTop: 9
    property real tableToolsTop: 44
    property real tableDropdownWidth: 150
    // UNDO with nothing to undo, and tools not available yet.
    property real tableIdleOpacity: 0.45
    property real tableUnavailableOpacity: 0.4
    property real tableMorphSpacing: 1
    property real tableLabelSpacing: 1.2
    // The frame editor: its top, the room left under it for the strip, and
    // the side column's width and gap.
    property real tableEditorTop: 78
    property real tableFrameStripRoom: 70
    property real tableSideWidth: 300
    property real tableSideGap: 10
    // The frame's line, its glow and the neighbouring frames' faint ink.
    property real tableLineWidth: 2.5
    property real tableGlowWidth: 8.9
    property real tableNeighbourAlpha: 0.12
    property real tableHarmonicsHeight: 180
    property real tableFormulaCardHeight: 118
    property real tableResynthesisCardHeight: 96
    // A card's body under its title, the formula field, and the line under it
    // (help text beside APPLY, which keeps this much room).
    property real tableCardBodyTop: 36
    property real tableFieldHeight: 28
    property real tableFormulaFootTop: 72
    property real tableFormulaHelpRoom: 90
    property real tableNoteLineHeight: 1.3
    // The frame strip: its top above the sheet's bottom, and each thumbnail.
    property real tableFrameStripBottom: 58
    property real tableThumbMaxWidth: 64
    property real tableThumbHeight: 44
    property real tableThumbRadius: 5
    property real tableThumbLineWidth: 1.2

    // The ARP · CLIP page (NovaArpPage): its cards' content inset and the
    // top of their bodies under the title.
    property real arpPageCardPadding: 14
    property real arpPageCardBodyTop: 38
    property real arpPageLabelSpacing: 1.2
    // The VARIATION BAR badge.
    property real arpPageBadgeTop: 10
    property real arpPageBadgePadding: 14
    property real arpPageBadgeHeight: 18
    property real arpPageBadgeSpacing: 0.8
    // The mode buttons: the gap within a group and under its label, and each button.
    property real arpPageModeSpacing: 3
    property real arpPageModePadding: 14
    property real arpPageModeHeight: 20
    property real arpPageModeRadius: 5
    property real arpPageVaryWidth: 78
    // FILL.
    property real arpPageFillPadding: 18
    property real arpPageFillHeight: 22
    property real arpPageFillSpacing: 0.6
    // HARMONY · RHYTHM: the scale row, the Euclidean label and knobs, dimmed
    // outside Euclidean mode, and the rhythm ring (its radius, inset from the
    // card's right, top, the room its dots take past the radius, a hit's and
    // a rest's dot).
    property real arpPageScaleRowTop: 66
    property real arpPageScaleWidth: 130
    property real arpPageRootWidth: 52
    property real arpPageEuclidLabelTop: 102
    property real arpPageEuclidKnobsTop: 116
    property real arpPageInactiveOpacity: 0.45
    property real arpPageRingRadius: 30
    property real arpPageRingRight: 26
    property real arpPageRingTop: 110
    property real arpPageRingDotRoom: 8
    property real arpPageRingHitSize: 7
    property real arpPageRingStepSize: 4
    property real arpPageGrooveWidth: 200
    // MUTATE, the seed readout, and the amount bar (its pointer area reaches
    // arpPageAmountSlop past the bar).
    property real arpPageMutatePadding: 20
    property real arpPageMutateHeight: 24
    property real arpPageMutateHoverAlpha: 0.22
    property real arpPageMutateSpacing: 0.8
    property real arpPageMutationWidth: 74
    property real arpPageAmountWidth: 110
    property real arpPageAmountHeight: 8
    property real arpPageAmountOpacity: 0.7
    property real arpPageAmountSlop: 6
    // The note on routing ARP MOD and ARP ACCENT, above the card's bottom.
    property real arpPageNoteBottom: 17
    // END nova block 1

    // ---- Pages, cards, controls, FX, matrix and displays tokens ----
    // Section headings (NovaHeading): their letter spacing.
    property real headingSpacing: 1.3
    // LEDs (NovaLed): the lamp, its glow's growth past it and the click margin.
    property int ledSize: 13
    property real ledGlowGrow: 8
    property real ledHitOutset: 3
    // A modulation source's drag handle (NovaHandle).
    property int handleSize: 16
    // Pills (NovaPill): the label's padding, the height and the tracking.
    property real pillPad: 16
    property int pillHeight: 22
    property real pillLetterSpacing: 0.6
    // Module cards (NovaCard): corner, dimmed while off, the focus glow, title row.
    property real moduleCardRadius: 11
    property real cardOffOpacity: 0.45
    property int cardDimDuration: 140
    property real cardGlowWidth: 4
    property real cardGlowAlpha: 0.16
    property real cardInsetX: 12
    property real cardTitleTop: 14
    property real cardTitleAfterLed: 33
    property real cardSubTop: 15
    property real cardSubSpacing: 0.6
    // Segment buttons (NovaSegs).
    property real segsPadding: 7
    property int segsFillWidth: 120
    property int segsHeight: 24
    // Card displays (NovaSubNoiseDisplay, NovaFilterResponse): a rounded well;
    // a module that is off is drawn at this alpha; each trace is a wide glow
    // under a fine line; the response's fill and the ghost of its saved setting.
    property real displayRadius: 8
    property real displayOffAlpha: 0.25
    property real displayGlowWidth: 6
    property real displayLineWidth: 1.8
    property real traceGlowWidth: 6.8
    property real displayFillAlpha: 0.07
    property real displayGhostAlpha: 0.25
    // A display's captions: the name top left (on a backdrop over the
    // wavetable), the readout top right, a note along the foot.
    property real displayCaptionX: 8
    property real displayCaptionY: 6
    property real displayReadoutY: 5
    property real displayCaptionSpacing: 1.2
    property real displayCaptionBackdrop: 3
    property real displayFootLift: 16
    // The oscillator display (NovaOscDisplay): the wavetable's frames, faint
    // at the back and the lit frame lifted with a glow; the grains' alpha by
    // age; the sample sketch's bars and the engine's cycle over them.
    property real frameBaseAlpha: 0.08
    property real frameDepthAlpha: 0.18
    property real frameGlowWidth: 8.6
    property real frameLineWidth: 2.2
    property real grainMinAlpha: 0.15
    property real grainAgeAlpha: 0.5
    property real sketchAlpha: 0.3
    property real sketchBarWidth: 1.5
    property real sketchCycleWidth: 1.5
    // Dropdowns (NovaDropdown): an inactive one is faded; the text's inset
    // and the room it leaves for the chevron; the chevron from the right edge.
    property real dropdownInactiveOpacity: 0.4
    property real dropdownTextInset: 9
    property real dropdownTextRoom: 28
    property real dropdownChevronRight: 16
    property real dropdownChevronWidth: 8
    property real dropdownChevronHeight: 5
    // Filter routing (NovaRouting): the mode segments, the grid and its cells.
    property real routingSegsTextSize: 9.5
    property real routingSegsPadding: 3
    property real routingRowGap: 3
    property real routingHeaderSpacing: 1
    property real routingCellRadius: 5
    property real routingDotSize: 6
    // Macros (NovaMacros): the name's side room and click margin, the rename field.
    property real macroNameRoom: 4
    property real macroNameHit: 3
    property real macroEditOutset: 2
    property real macroEditRoom: 6
    // Modulation source chips (NovaSourceChip): the tab, its tint and the
    // raised route count.
    property real chipHeight: 22
    property real chipRadius: 5
    property real chipLetterSpacing: 0.7
    property real chipFillAlpha: 0.1
    property real chipEdgeAlpha: 0.3
    property real chipCountSize: typeMicro
    property real chipCountOpacity: 0.8
    property real chipCountGap: 3
    // The LFO curve editor (NovaCurveEditor): the plot's padding, the room
    // above it for the caption, the playhead dot; the fill fades to this alpha.
    property real lfoEditorPad: 14
    property real lfoCaptionRoom: 10
    property real lfoPlayheadDotRadius: 4
    property real lfoFillFloorAlpha: 0.02
    // Card controls: the gap between knob groups, a row of controls, and the
    // tighter segment padding where a row is short of room.
    property real knobGroupGap: 10
    // A knob stacked over its mode menus, and the small knobs in a group's rows.
    property real knobStackGap: 3
    property real controlRowHeight: 24
    property real compactSegsPadding: 4
    // The key strip (NovaKeyStrip): its padding, the gap between keys and the
    // lift of the white and black keys' letters.
    property real keyStripPadX: 12
    property real keyStripPadY: 6
    property real keyGap: 2
    property real whiteKeyLabelLift: 3
    property real blackKeyLabelLift: 2
    // The effect browser (NovaFxBrowser): the dialog, its search field, the
    // category column and the module tiles.
    property int fxBrowserWidth: 560
    property int fxBrowserHeight: 360
    property real fxBrowserRadius: 12
    property real fxBrowserPad: 12
    property real fxBrowserListTop: 54
    property real fxBrowserGridLeft: 164
    property real fxSearchHeight: 32
    property real fxSearchInset: 10
    property real fxCategoryWidth: 140
    property real fxCategoryHeight: 28
    property real fxCategoryRadius: 7
    property real fxCategorySpacing: 1
    property real fxTileInset: 10
    property real fxTileHeight: 62
    property real fxTileRadius: 9
    property real fxTileTitleTop: 8
    property real fxTileTitleSpacing: 1.2
    property real fxTileBlurbTop: 25
    // A module's row in the rack's chain (NovaFxRow): faded while dragged or
    // off; the selection edge; the grip, LED and name columns; the level meters
    // (its corner and name tracking are the rack's, below).
    property real fxRowDragOpacity: 0.4
    property real fxRowOffOpacity: 0.55
    property real fxRowEdgeInset: 6
    property real fxRowEdgeWidth: 3
    property real fxRowEndInset: 9
    property real fxRowLedX: 24
    property real fxRowNameX: 46
    property real fxRowLineGap: 1
    property real fxRowMeterWidth: 34
    property real fxRowMeterHeight: 4
    property real fxMenuHit: 5
    // A card's header controls row, from the card's top; a card's body: its
    // inset, its top under the title and the gap between its rows.
    property real cardHeaderControlsY: 9
    property real cardBodyX: 14
    property real cardBodyTop: 40
    property real cardBodyGap: 10
    // The clip sequencer (NovaClipSequencer): the root stepper, the export
    // button, and a note's alpha by velocity.
    property real seqRootInset: 6
    property real seqArrowHit: 4
    property real seqExportPad: 14
    property real seqNoteMinAlpha: 0.45
    property real seqNoteVelocityAlpha: 0.55
    // A module strip's display (NovaFxDisplay): the well, a slot that is off,
    // the filter and EQ nodes with their click margin, the idle hint's tracking.
    property real fxDisplayRadius: 9
    property real fxDisplayOffAlpha: 0.4
    property real fxNodeSize: 14
    property real fxEqNodeSize: 18
    property real fxNodeHit: 4
    property real fxHintSpacing: 1
    // The arp pattern library (NovaArpLibrary): padding, rows, the search and
    // the category chips.
    property real arpLibraryPad: 10
    property real arpLibraryRowHeight: 26
    property real arpLibraryTitleTop: 12
    property real arpSearchTop: 32
    property real arpSearchHeight: 24
    property real arpChipHeight: 19
    property real arpChipPad: 12
    property real arpChipSpacing: 0.6
    property real arpDotSize: 6
    property real arpNameIndent: 14
    // Overview module cards (NovaModuleCard): padding, header, corner, the
    // mono title's tracking, the type dropdown's text, the edge while large,
    // an off module's fade at rest and under the pointer, the jump's glow.
    property real moduleCardPad: 10
    property real moduleHeaderHeight: 24
    property real overviewCardRadius: 9
    property real moduleTitleSpacing: 1.7
    property real moduleTypeTextSize: 10.5
    property real moduleExpandedEdge: 1.5
    property real moduleOffOpacity: 0.42
    property real moduleOffHoverOpacity: 0.75
    property real moduleFlashWidth: 5
    // Arp lanes (NovaArpLane): the lane, its header (lock, name, length box)
    // and its step cells; steps past the length fade; level and number bars.
    property real arpLaneHeight: 28
    property real arpLaneHeaderWidth: 166
    property real arpLockSize: 18
    property real arpLaneNameSpacing: 1.1
    property real arpLengthWidth: 54
    property real arpLengthHeight: 20
    property real arpLengthRadius: 5
    property real arpLengthHit: 3
    property real arpLengthDigits: 16
    property real arpCellOutsideOpacity: 0.28
    property real arpBarInsetX: 2
    property real arpBarInsetY: 1
    property real arpBarMinHeight: 1.5
    property real arpBarRadius: 1.5
    property real arpLevelBarOpacity: 0.85
    property real arpNumberBarOpacity: 0.35
    // Pages: the gap between cards; a card's notes and section labels.
    property real pageGap: 10
    property real noteLineHeight: 1.3
    property real sectionLabelSpacing: 1.2
    // The GLOBAL page (NovaGlobalPage): the chaos sources' value bars, the
    // live sources' grid and bars, the output meters.
    property real valueBarHeight: 6
    property real sourceColumnGap: 20
    property real sourceRowGap: 7
    property real sourceBarHeight: 5
    property real meterGap: 22
    property real meterBarWidth: 10
    // A module strip on the FX page (NovaFxModule): its corner and padding,
    // the selected edge's tint, the header's gaps and name, the preset box,
    // the collapsed summary, the gap between the display and the knobs, the
    // EQ's band grid with a band that is off faded.
    property real fxStripRadius: 12
    property real fxStripPad: 12
    property real fxStripSelectedAlpha: 0.55
    property real fxStripHeaderGap: 10
    property real fxStripNameSpacing: 2.2
    property real fxStripNameHit: 4
    property real fxPresetTextRoom: 12
    property real fxPresetArrowWidth: 22
    property real fxSummaryGap: 14
    property real fxBodyGap: 30
    property real eqColumnGap: 18
    property real eqRowGap: 10
    property real eqBandOffOpacity: 0.4
    // The MATRIX page (NovaMatrixPage): the header pills, column labels and
    // rule; the rows; each column's inset for a control, a bar or a mark, and
    // text; the source chip, the aux menu's room, the amount, curve and scale.
    property real matrixHeaderTop: 10
    property real matrixLabelTop: 44
    property real matrixRuleTop: 62
    property real matrixRowsTop: 64
    property real matrixRowHeight: 36
    property real matrixControlInset: 4
    property real matrixBarInset: 6
    property real matrixTextInset: 8
    property real matrixChipPad: 12
    property real matrixChipGap: 5
    property real matrixAuxRoom: 12
    property real matrixAmountHeight: 16
    property real matrixAmountInset: 2
    property real matrixCurveLineWidth: 1.5
    property real matrixScaleHeight: 8
    property real matrixScaleOpacity: 0.8
    property real matrixScaleHit: 6
    // The FX page's rack (NovaFxPage): a translucent well with its padding;
    // its tiles (bus tabs, chain rows, the routing box, + Add effect) share a
    // corner and their names' tracking; a bus tab's text and level meter; the
    // routing box's note and send/return row; the signal-flow nodes and wires;
    // the foot's labels; the gap between module strips; the preset-name prompt.
    property real fxRackRadius: 11
    property color fxRackFill: soft(wellFill, 0.55)
    property real fxRackPad: 10
    property real fxRackTileRadius: 9
    property real fxRackNameSpacing: 1.4
    property real fxBusTextX: 8
    property real fxBusNameTop: 7
    property real fxBusCountTop: 22
    property real fxBusMeterRight: 11
    property real fxBusMeterInset: 8
    property real fxBusMeterWidth: 4
    property real fxRouteInset: 10
    property real fxRouteTextTop: 7
    property real fxRouteKnobsX: 8
    property real fxRouteKnobsY: 4
    property real fxRouteLabelSpacing: 1
    property real flowNodeHeight: 18
    property real flowNodeSize: 9
    property real flowNodeRadius: 5
    property real flowNodeStroke: 1.5
    property real flowNodeSpacing: 1.6
    property real wireWidth: 20
    property real wireX: 4
    property real footLabelWidth: 78
    property real fxStripGap: 10
    property real promptRadius: 10
    property real promptPad: 12
    property real promptTitleTop: 9
    property real promptFieldTop: 30
    property real promptFieldHeight: 30
    property real promptFieldRadius: 7
    property real promptFieldInset: 8
    property real promptSaveTop: 34
    // The FX displays' painters (NovaFxDraw): every curve a glow under a line;
    // dots; the spectrum's fill; labels top left, along the foot, by the side
    // and beside an axis mark; each EQ band's faint curve; the compressor's GR
    // meter; Multiband's bands, cut and boost; the delay's input mark and
    // repeats; the reverb's pre-delay and tail; the chorus voices' halos; the
    // stereo image; the shifter's arrow head.
    property real fxGlowWidth: 5
    property real fxLineWidth: 1.6
    property real fxDotRadius: 4
    property real fxSpectrumAlpha: 0.1
    property real fxLabelX: 8
    property real fxLabelTop: 14
    property real fxLabelLift: 4
    property real fxSideLabelInset: 10
    property real fxSideLabelTop: 12
    property real fxAxisLabelGap: 3
    property real fxBandCurveAlpha: 0.45
    property real fxGrMeterWidth: 14
    property real fxGrMeterGap: 10
    property real fxBandInset: 10
    property real fxBandHeadroom: 12
    property real fxCutAlpha: 0.55
    property real fxBoostAlpha: 0.4
    property real fxTapInset: 6
    property real fxTapWidth: 3
    property real fxTapeWidth: 5
    property real fxTapHeadroom: 8
    property real fxPreDelayAlpha: 0.12
    property real fxDecayHeadroom: 10
    property real fxTailAlpha: 0.08
    property real fxTailDampAlpha: 0.1
    property real fxVoiceHaloAlpha: 0.14
    property real fxVoiceHaloRadius: 14
    property real fxImageAlpha: 0.12
    property real fxArrowLength: 6
    property real fxArrowSpread: 4
    // END nova block 2
}
