import QtQuick

// The shell's palette as kit tokens. DawShell reads its colours from here,
// and kit widgets hosted in the shell (rather than in a device panel) take
// this theme, so both draw from one set of tokens.
KitTheme {
    prefix: "shell"

    // The shell's named colours, as DawShell hands them to its children:
    // graphite grounds, one teal accent, amber for time (playhead, loop).
    readonly property color canvas: "#0b0e12"
    readonly property color panel: "#11151b"
    readonly property color panelRaised: "#151b22"
    readonly property color line: "#1f2731"
    readonly property color lineStrong: "#2f3b48"
    readonly property color textMain: "#e7ecf2"
    readonly property color textDim: "#8d99a8"
    readonly property color textFaint: "#798592"
    readonly property color amber: "#f5b14c"
    readonly property color selection: "#18202a"

    // Type: Inter for words, JetBrains Mono for numbers. Qt falls back to the
    // system families when these are not installed.
    readonly property string sansFamily: "Inter"
    readonly property string monoFamily: "JetBrains Mono"

    // Track colours, cycled by track order until a track carries its own.
    readonly property var trackPalette: ["#f5b14c", "#8fa6ff", "#c49bff", "#e8d9b8", "#ff8f6b",
                                         "#5fe0bf", "#7fb8ff", "#ff7aa8", "#9be36b", "#ffb36b"]
    function trackColor(track, index) {
        if (track && track.color !== undefined && String(track.color).length > 0)
            return track.color
        return trackPalette[Math.max(0, index) % trackPalette.length]
    }

    sans: sansFamily
    mono: monoFamily
    accent: "#5fe0bf"
    accent2: amber
    plate: panel
    plateEdge: line
    plateHi: panelRaised
    well: canvas
    wellEdge: line
    seam: line
    grid: line
    gridStrong: lineStrong
    ink: textMain
    inkDim: textDim
    inkFaint: textFaint
    knobRing: line
    track: panelRaised
    button: panel
    buttonHi: panelRaised
    ledOff: line
    warning: amber

    // ---- Shell surfaces: arrangement, timeline, clip and note editors ----
    // (tokens for ArrangementView, TrackListPanel, PianoRoll, ClipEditor,
    // WarpEditor, automation lanes and their helpers)

    // Shared by the editors' small controls.
    readonly property color controlEdge: "#2b3540"
    readonly property color menuHover: "#2c4b43"
    readonly property color toggleOn: "#245343"
    readonly property color toggleOff: "#222b35"
    readonly property color buttonFace: "#1a212a"
    readonly property color buttonHover: "#26303b"
    readonly property color chipText: "#e6edf3"
    // Tracking for small upper-case labels.
    readonly property real capsLetterSpacing: 1

    // -- Arrangement view, workspace, drop areas, loop controls --
    // Track list beside the arrangement, and the rows' heights.
    readonly property int trackListWidth: 232
    readonly property int elapsedRulerHeight: 22
    readonly property int automationLaneHeight: 52
    readonly property int trackRowHeight: 62
    readonly property int trackRowMinHeight: 48
    readonly property int trackRowMaxHeight: 480
    // The empty lane below the last track: ground, bar and beat lines.
    readonly property color creationLaneGround: "#19212a"
    readonly property color creationLaneBarLine: "#354852"
    readonly property color creationLaneBeatLine: "#2a3740"
    // A file dragged over the track list's empty space.
    readonly property color dropZoneFill: "#1c72e3c0"
    readonly property color dropZoneEdge: "#72e3c0"
    // Accent wash over a device/track drop target.
    readonly property real dropWashAlpha: 0.08
    readonly property real dropHintLetterSpacing: 0.7
    // The transport's Loop button.
    readonly property color loopEdge: "#26303b"
    readonly property int loopHeight: 36
    readonly property real loopActiveWashAlpha: 0.14
    readonly property int loopIconWidth: 16
    readonly property int loopIconHeight: 12
    readonly property real loopIconStroke: 1.6
    readonly property real loopRangeOpacity: 0.8
    // The key-match pill over the arrangement.
    readonly property int keyMatchMaxWidth: 560
    readonly property int keyMatchHeight: 34
    readonly property int keyMatchRadius: 17
    readonly property color keyMatchEdge: "#2f6b5c"
    readonly property int keyMatchInset: 14
    readonly property int keyMatchSpacing: 10
    readonly property int keyMatchTextMaxWidth: 380
    readonly property color keyMatchText: "#c3ccd6"
    readonly property int keyMatchButtonPadding: 18
    readonly property int keyMatchButtonHeight: 22
    readonly property int keyMatchButtonRadius: 11
    readonly property color keyMatchButtonFill: "#1d3a32"
    // The bar ruler, level with the track list header row; its tools; the master row.
    readonly property int timelineHeaderHeight: 30
    readonly property int masterRowHeight: 62
    readonly property color recordingNote: "#ffd27a"
    readonly property int arrangementGridMinSpacing: 12
    readonly property int arrangementContextMenuReach: 224
    readonly property color arrangementRulerGround: "#0d1116"
    readonly property int arrangementToolHeight: 22
    readonly property int arrangementToolRadius: 5
    readonly property int arrangementToolSpacing: 3
    readonly property color arrangementToolActive: "#17362f"
    readonly property color arrangementToolHover: "#1d252e"
    readonly property color arrangementToolActiveEdge: "#2f5a51"
    readonly property color arrangementToolEdge: "#26303b"
    // The loop range on the ruler and over the lanes.
    readonly property real loopRangeWashAlpha: 0.18
    readonly property real loopRangeRestOpacity: 0.8
    readonly property real loopLaneWashAlpha: 0.05
    // Markers: a flag on the ruler and a line down the lanes.
    readonly property int markerCullMargin: 120
    readonly property int markerMinWidth: 18
    readonly property real markerLineOpacity: 0.18
    readonly property int markerFlagHeight: 15
    readonly property color markerFlagFill: "#3a3150"
    readonly property color markerFlagEdge: "#a48ce0"
    readonly property color markerText: "#e6dcff"
    readonly property color markerEditText: "#ffffff"
    readonly property int markerEditorMinWidth: 60
    // Ruler ticks and labels.
    readonly property int rulerBeatLabelMinSpacing: 40
    readonly property int rulerLabelMinSpacing: 48
    readonly property int rulerTickLabelWidth: 60
    readonly property color rulerBeatLine: "#171d24"
    readonly property real selectionBoxWashAlpha: 0.12
    // Lanes: ground and the grid's bar, beat and step lines.
    readonly property int laneFallbackHeight: 88
    readonly property color laneGroundSelected: "#10161d"
    readonly property color laneGround: "#0d1116"
    readonly property color laneGridPhraseLine: "#2a3542"
    readonly property color laneGridBeatLine: "#161d25"
    readonly property color laneGridStepLine: "#12181f"
    readonly property color emptyLaneEdge: "#2a3440"
    readonly property var emptyLaneDash: [4, 3]
    readonly property int recordingNoteHeight: 4
    readonly property int recordingNoteMinWidth: 3
    readonly property int recordingNoteSpan: 20
    // Clip cards on the lanes.
    readonly property int clipTrimMinWidth: 4
    readonly property int clipTrimMaxWidth: 10
    readonly property real clipTrimFraction: 0.25
    readonly property int clipMinWidth: 7
    readonly property int clipMinHeight: 30
    readonly property int clipLandingDrop: 8
    readonly property int clipCullMargin: 16
    readonly property real clipGlowRestOpacity: 0.15
    readonly property real clipGlowLevelOpacity: 0.55
    readonly property real clipBodyWashAlpha: 0.15
    readonly property real clipEdgeAlpha: 0.55
    readonly property int clipCapHeight: 18
    readonly property int clipNameX: 7
    readonly property int clipNameY: 3
    readonly property int clipBadgeMinClipWidth: 105
    readonly property real clipWaveEdgeLighten: 1.35
    readonly property int clipWaveTop: 22
    readonly property int clipWaveBottomGap: 26
    readonly property int clipGlowOutset: 2
    readonly property real clipWaveFillAlpha: 0.7
    readonly property real clipWaveEdgeStroke: 0.75
    // Over an audio clip's body: the analysis veil, beat grid and downbeat flag.
    readonly property int clipOverlayTop: 20
    readonly property int clipOverlayBottomGap: 22
    readonly property color clipVeil: Qt.rgba(0.06, 0.07, 0.09, 0.72)
    readonly property int clipVeilLabelMinClipWidth: 140
    readonly property int clipVeilLabelTop: 22
    readonly property int clipBeatGridMinBeatSpacing: 3
    readonly property int clipBeatGridMinOffbeatSpacing: 6
    readonly property string clipBeatGridBarLine: "rgba(255,255,255,0.38)"
    readonly property string clipBeatGridBeatLine: "rgba(255,255,255,0.14)"
    readonly property int clipFlagWidth: 18
    readonly property int clipFlagBadgeHeight: 14
    readonly property int clipFlagGrabSize: 16
    readonly property color clipWaveLoadingText: "#dac5df"
    readonly property color clipWaveErrorText: "#f0b0a7"
    // Chord names along a MIDI clip's foot, and its notes.
    readonly property int clipChordMinClipHeight: 40
    readonly property int clipChordBottom: 16
    readonly property int clipChordHeight: 13
    readonly property int clipChordMinWidth: 14
    readonly property real clipChordLineOpacity: 0.7
    readonly property int clipChordTextInset: 3
    readonly property color clipChordText: "#e6ebf0"
    readonly property color clipChordTextOutline: "#10141a"
    readonly property int clipNoteTop: 22
    readonly property int clipNoteSpan: 30
    readonly property int clipNoteMinWidth: 3
    readonly property int clipNoteHeight: 2
    readonly property real clipNoteLighten: 1.2
    // Edge grips: MIDI resize and audio trim.
    readonly property int clipGrabMinWidth: 4
    readonly property int clipGrabMaxWidth: 12
    readonly property real clipGrabFraction: 0.3
    readonly property int clipGrabOverhang: 8
    readonly property int clipGripWidth: 3
    readonly property int clipGripMinHeight: 10
    readonly property int clipGripInset: 16
    readonly property real clipGripSelectedOpacity: 0.35
    readonly property int clipTrimMinClipWidth: 12
    // The plus a clip carries while a Ctrl-drag copies it.
    readonly property int clipCopyBadgeSize: 14
    // The playhead, the master row and the elapsed-time ruler.
    readonly property int playheadGlowWidth: 8
    readonly property real playheadGlowAlpha: 0.12
    readonly property color masterRowGroundSelected: "#10161d"
    readonly property color masterRowGround: "#0d1116"
    readonly property color elapsedRulerGround: "#0f1318"
    readonly property int elapsedLabelMinSpacing: 80
    readonly property int elapsedTickWidth: 90
    readonly property int elapsedTickHeight: 7
    readonly property int elapsedTickLabelX: 7
    readonly property int elapsedMillisMinBarWidth: 400
    readonly property int elapsedReadoutWidth: 166
    // The empty arrangement's invitation.
    readonly property real emptyArrangementLetterSpacing: 1.7
    readonly property real emptyArrangementHintOpacity: 0.7
    readonly property int clipGripGap: 3
    // -- end arrangement view

    // -- Track list --
    // The track list header row and its + Track menu.
    readonly property color trackCreationDropHover: "#16302b"
    readonly property color trackCreationGround: "#0f1318"
    readonly property real trackListHeaderLetterSpacing: 0.7
    readonly property int trackCreationButtonMargin: 10
    readonly property int trackCreationButtonHeight: 22
    readonly property int trackCreationButtonRadius: 5
    readonly property color trackCreationButtonOpen: "#1f2a35"
    readonly property color trackCreationButtonEdge: "#26303b"
    readonly property color trackCreationButtonText: "#c3ccd6"
    readonly property int trackCreationMenuGap: 2
    readonly property int trackCreationMenuWidth: 200
    readonly property int trackCreationOptionHeight: 34
    readonly property int trackCreationOptionRadius: 5
    readonly property color trackCreationOptionHover: "#1f2a35"
    readonly property int trackCreationOptionTextX: 10
    // A track's send knobs.
    readonly property int sendKnobSize: 30
    readonly property int sendKnobStroke: 3
    readonly property color sendKnobTrack: "#252e39"
    readonly property int sendKnobArcRadius: 11
    readonly property int sendKnobCapRadius: 7
    readonly property color sendKnobCapFocused: "#202a35"
    readonly property color sendKnobCap: "#171d25"
    readonly property int sendKnobTipPadding: 14
    readonly property int sendKnobTipHeight: 22
    readonly property int sendKnobTipRadius: 5
    // Track rows: colour bar, frozen wash and badges.
    readonly property int trackColorBarWidth: 5
    readonly property real trackMutedOpacity: 0.35
    readonly property color trackFrozenWashStart: "#3384c8ec"
    readonly property color trackFrozenWashEnd: "#0884c8ec"
    readonly property int trackBadgeRightMargin: 18
    readonly property int trackBadgeHeight: 15
    readonly property int trackBadgeRadius: 7
    readonly property color trackFrozenBadgeFill: "#1d3a4a"
    readonly property color trackFrozenEdge: "#84c8ec"
    readonly property color trackFrozenText: "#bfe6fa"
    readonly property int trackRenderBarWidth: 118
    readonly property color trackRenderGround: "#13222b"
    readonly property color trackRenderFill: "#2f6f8f"
    readonly property int trackRenderFillRadius: 6
    readonly property color trackRenderText: "#dff3ff"
    readonly property int trackRenderLabelInset: 7
    // A track row's swatch, name, duck hint and automation chooser.
    readonly property int trackRowTallHeight: 88
    readonly property int trackSwatchX: 14
    readonly property int trackSwatchY: 15
    readonly property int trackSwatchTallY: 16
    readonly property int trackSwatchSize: 12
    readonly property int trackNameX: 32
    readonly property int trackNameY: 13
    readonly property int trackNameTallY: 14
    readonly property int trackNameRightReserve: 139
    readonly property int trackDuckHintPadding: 14
    readonly property int trackDuckHintHeight: 17
    readonly property int trackNameEditorInsetX: 5
    readonly property int trackNameEditorInsetY: 3
    readonly property int trackNameEditorMinWidth: 80
    readonly property int trackNameEditorHeight: 22
    readonly property int trackChooserX: 11
    readonly property int trackChooserY: 30
    readonly property int trackDetailRightReserve: 122
    readonly property int trackChooserHeight: 20
    readonly property int trackChooserRadius: 5
    readonly property color trackChooserGround: "#11161c"
    readonly property int trackChooserTextRightGap: 18
    readonly property int trackChooserArrowMargin: 5
    readonly property int trackChooserMenuOffset: 52
    readonly property int trackSummaryX: 15
    readonly property int trackSummaryY: 32
    readonly property int trackSummaryTallY: 33
    readonly property color trackRowDivider: "#1a212a"
    readonly property int trackSendsY: 52
    // A track row's A, M, S and delete buttons, and its level meter, from the right edge.
    readonly property int trackButtonSize: 22
    readonly property int trackButtonRadius: 5
    // The dot on a closed A button whose track holds automation.
    readonly property real trackAutomationMarkSize: 5
    readonly property real trackAutomationMarkInset: 2
    readonly property int trackButtonTallY: 54
    readonly property int trackAutomationButtonRight: 112
    readonly property int trackMuteButtonRight: 86
    readonly property int trackSoloButtonRight: 60
    readonly property int trackDeleteButtonRight: 36
    readonly property color trackDeleteHover: "#4a2626"
    readonly property color trackDeleteGlyphHover: "#ff8f8f"
    readonly property int trackDeleteGlyphSize: 17
    readonly property int trackMeterRight: 12
    readonly property int trackMeterTallY: 47
    readonly property int trackMeterWidth: 4
    readonly property int trackMeterHeight: 36
    readonly property real trackMeterHotStop: 0.1
    readonly property color trackMeterClip: "#ff6b6b"
    readonly property int trackResizeHandleHeight: 6
    readonly property color trackResizeHandleHover: "#2f5a51"
    // Automation lane headers under a track.
    readonly property color laneHeaderGround: "#0d1116"
    readonly property real laneHeaderColorBarOpacity: 0.45
    readonly property int laneHeaderChooserX: 15
    readonly property int laneHeaderChooserRightReserve: 96
    readonly property color laneHeaderChooserEdge: "#232c36"
    readonly property int laneHeaderChooserTextX: 7
    readonly property int laneHeaderChooserTextRightGap: 20
    readonly property int laneHeaderMenuOffset: 30
    readonly property int laneHeaderCountY: 32
    readonly property int laneHeaderButtonsRight: 78
    readonly property int laneHeaderButtonStride: 24
    readonly property int laneHeaderCloseGlyphSize: 15
    readonly property color trackMeterWell: "#1a212a"
    // The automation target chooser.
    readonly property int trackChooserMaxHeight: 320
    readonly property int trackChooserHeadingHeight: 20
    readonly property int trackChooserRowHeight: 22
    readonly property color trackChooserRowHover: "#22303c"
    readonly property real emptyTrackListLineHeight: 1.5
    // The track menu, duck popover and colour popover.
    readonly property int trackPopoverTitleHeight: 29
    readonly property int trackPopoverTitleY: 9
    readonly property int trackMenuWidth: 210
    readonly property int trackMenuEntryStride: 38
    readonly property int trackMenuEntryHeight: 36
    readonly property int trackMenuEntryRadius: 5
    readonly property int trackMenuEntryTextX: 9
    readonly property int trackMenuEntryHintY: 19
    readonly property color trackMenuDeleteText: "#ff9b9b"
    readonly property int duckPopoverWidth: 230
    readonly property int duckPopoverMaxRows: 8
    readonly property int duckRowHeight: 26
    readonly property int duckFooterHeight: 44
    readonly property int duckCheckX: 8
    readonly property int duckCheckSize: 12
    readonly property int duckRowTextX: 28
    readonly property int duckApplyHeight: 32
    readonly property int duckApplyBottom: 38
    readonly property color duckApplyIdle: "#1d2a27"
    readonly property real duckApplyDisabledOpacity: 0.5
    readonly property int paletteSwatchSize: 22
    readonly property int paletteColumns: 5
    readonly property color trackMasterGround: "#0f1318"
    readonly property int trackMenuEntryTextY: 3
    readonly property int trackSwatchHitOutset: 3
    // Bus nesting: a group bus heads its members, joined by its colour.
    readonly property int trackBusBarWidth: 4
    readonly property real trackBusHeaderTintAlpha: 0.1
    readonly property real trackBusLaneTintAlpha: 0.05
    readonly property real trackMemberTintAlpha: 0.04
    readonly property int trackBusChevronX: 4
    readonly property int trackBusChevronSize: 16
    readonly property real trackBusChevronStroke: 1.6
    // The chevron's glyph, in its own square's units.
    readonly property real trackBusChevronInset: 0.3
    readonly property int trackBusSwatchBorder: 2
    // The bus's swatch, name and summary sit right of its chevron.
    readonly property int trackBusContentShift: 8
    // Members' content steps in; their rail runs under the bus's chevron.
    readonly property int trackMemberIndent: 12
    readonly property real trackBusRailX: 10.5
    readonly property int trackBusRailWidth: 3
    // The rail stops this far above the last member's bottom.
    readonly property int trackBusRailEndGap: 10
    readonly property int trackBusRailRadius: 2
    readonly property int trackBusRailGlowWidth: 9
    readonly property real trackBusRailGlowAlpha: 0.35
    // Badges ("3 tracks", a return's effect) and send pills.
    readonly property real trackBusBadgeAlpha: 0.14
    readonly property int trackBusBadgePadding: 10
    readonly property int trackSendPillHeight: 14
    readonly property int trackSendPillPadding: 10
    readonly property real trackSendPillEdgeAlpha: 0.5
    // The text before a row's send pills shows only when this much is left.
    readonly property int trackSummaryMinWidth: 24
    // The returns' section at the bottom of the list.
    readonly property int trackReturnsLabelHeight: 26
    readonly property color trackReturnGround: "#121620"
    // A track header being dragged: the slot it opens, the chip naming its
    // destination and the ghost it leaves.
    readonly property int trackDropSlotHeight: 40
    readonly property int trackDropSlotInset: 6
    readonly property real trackDropSlotAlpha: 0.12
    readonly property real trackDropLaneAlpha: 0.06
    readonly property var trackDropSlotDash: [5, 4]
    readonly property int trackDropSlotStroke: 2
    // The top and bottom of a bus's bracket that drop outside it.
    readonly property real trackDropEdgeFraction: 0.25
    readonly property real trackDragGhostOpacity: 0.35
    readonly property int trackDragChipOffset: 14
    readonly property int trackDragChipHeight: 30
    readonly property int trackDragChipPadding: 10
    readonly property color trackDragChipGround: "#1f2533"
    readonly property int trackDragArrowSize: 14
    // A folded bus's lane: its members' clips as one dashed outline.
    readonly property real trackBusSumAlpha: 0.16
    readonly property real trackBusSumEdgeAlpha: 0.6
    readonly property int trackBusSumInsetY: 10
    // -- end track list

    // -- Piano roll, velocity lane, pedal strip --
    // The piano roll's frame: keyboard, chord panel, rows.
    readonly property int pianoRollHeight: 270
    readonly property int pianoRollLaneReserve: 150
    readonly property int pianoRollMinRowHeight: 7
    readonly property int pianoRollComfortRowHeight: 14
    readonly property int pianoRollMinNoteRowHeight: 8
    readonly property int pianoRollKeyboardWidth: 52
    readonly property int pianoRollDrumKeyboardWidth: 120
    readonly property int pianoRollChordPanelMinWidth: 260
    readonly property int pianoRollChordPanelMaxWidth: 340
    readonly property real pianoRollChordPanelFraction: 0.3
    readonly property int pianoRollHeaderBottom: 46
    readonly property int pianoRollChordStripHeight: 30
    readonly property int pianoRollRowTop: 48
    // The header: title, view and grid buttons, actions and the hint, left to right.
    readonly property int pianoRollTitleX: 14
    readonly property int pianoRollTitleY: 11
    readonly property int pianoRollHeaderButtonHeight: 27
    readonly property int pianoRollHeaderButtonRadius: 5
    readonly property int pianoRollViewButtonX: 158
    readonly property int pianoRollViewButtonStride: 55
    readonly property int pianoRollViewButtonWidth: 52
    readonly property int pianoRollGridLabelX: 275
    readonly property int pianoRollHeaderLabelY: 14
    readonly property int pianoRollGridButtonX: 311
    readonly property int pianoRollGridButtonStride: 36
    readonly property int pianoRollGridButtonWidth: 34
    readonly property int pianoRollQuantiseX: 538
    readonly property int pianoRollQuantiseWidth: 91
    readonly property color noteActionFill: "#263d39"
    readonly property int pianoRollFindNotesX: 638
    readonly property int pianoRollFindNotesWidth: 90
    readonly property int pianoRollLoopX: 738
    readonly property int pianoRollLoopWidth: 58
    readonly property int pianoRollFoldX: 1010
    readonly property int pianoRollFoldWidth: 92
    readonly property real pianoRollUnavailableOpacity: 0.45
    readonly property int pianoRollLockX: 1108
    readonly property int pianoRollLockWidth: 58
    readonly property int pianoRollChordX: 1172
    readonly property int pianoRollChordWidth: 70
    readonly property int pianoRollToolsX: 1248
    readonly property int pianoRollDrumToolsX: 802
    readonly property int pianoRollToolsWidth: 70
    readonly property int pianoRollPedalButtonX: 802
    readonly property int pianoRollPedalButtonStride: 104
    readonly property int pianoRollPedalButtonWidth: 98
    readonly property color pianoRollPedalButtonFill: "#2d2a1f"
    readonly property int pianoRollHintGap: 10
    // Chords: their violet, in clips, the roll and the chord tools.
    readonly property color chordAccent: "#a98bff"
    readonly property color pianoRollChordsOpenFill: "#2d2547"
    readonly property int pianoRollChordsButtonWidth: 76
    // The key strip: drum lanes, C keys, black and white keys, scale marks.
    readonly property color pianoRollKeyDrumEven: "#2b3944"
    readonly property color pianoRollKeyDrumOdd: "#26333e"
    readonly property color pianoRollKeyC: "#33414a"
    readonly property color pianoRollKeyBlack: "#172028"
    readonly property color pianoRollKeyWhite: "#28323c"
    readonly property int pianoRollScaleMarkWidth: 3
    readonly property real pianoRollTonicMarkOpacity: 0.9
    readonly property real pianoRollScaleMarkOpacity: 0.35
    // Row grounds, the greyed pattern end and grid lines.
    readonly property color pianoRollRowOutOfKey: "#141c22"
    readonly property color pianoRollRowC: "#1b2930"
    readonly property color pianoRollRow: "#19232c"
    readonly property color pianoRollRowLine: "#30404a"
    readonly property color pianoRollPatternGround: "#12181d"
    readonly property int pianoRollGridMinLineSpacing: 3
    readonly property color pianoRollGridBarLine: "#4d6170"
    readonly property color pianoRollGridBeatLine: "#3a4c58"
    readonly property color pianoRollGridStepLine: "#2a3943"
    // Notes: ringing tails, blocks, resize handles and ghosts.
    readonly property real pianoRollTailTop: 0.3
    readonly property real pianoRollTailHeight: 0.4
    readonly property color pianoRollTail: "#3f9c80"
    readonly property real pianoRollTailOpacity: 0.32
    readonly property real pianoRollSilentNoteOpacity: 0.4
    readonly property real pianoRollNotePopFrom: 0.55
    readonly property int pianoRollNoteInset: 2
    readonly property int pianoRollNoteMinWidth: 6
    readonly property color pianoRollNoteSelected: "#88ebcd"
    readonly property var pianoRollNoteBase: [0.14, 0.33, 0.27]
    readonly property var pianoRollNoteLoudnessGain: [0.18, 0.38, 0.30]
    readonly property color pianoRollNoteEdge: "#77c7a9"
    readonly property color pianoRollNoteText: "#10231e"
    readonly property int pianoRollNoteLabelMinWidth: 36
    readonly property int pianoRollNoteHandleWidth: 8
    readonly property color pianoRollNoteHandleSelected: "#d4fff1"
    readonly property color pianoRollNoteHandle: "#7cd2b4"
    readonly property real pianoRollNoteHandleOpacity: 0.8
    readonly property real pianoRollFadingNoteOpacity: 0.6
    readonly property real pianoRollFadingNoteScale: 0.8
    readonly property int pianoRollRecordingNoteMinWidth: 3
    readonly property real pianoRollStampWashAlpha: 0.22
    readonly property int pianoRollStampLabelMinWidth: 30
    readonly property real pianoRollFollowerOpacity: 0.45
    readonly property real pianoRollDragGhostOpacity: 0.65
    readonly property color pianoRollPatternEndVeil: Qt.rgba(0, 0, 0, 0.35)
    readonly property real pianoRollPatternEndLineOpacity: 0.8
    readonly property int pianoRollToolsY: 40
    readonly property int pianoRollCopyMarkX: 3
    // The piano roll's Tools panel: chips, sliders, actions and the groove pool.
    readonly property int pianoToolsWidth: 776
    readonly property int pianoToolsHeight: 228
    readonly property color pianoToolsGround: "#151d24"
    readonly property real pianoToolsUnfoldScale: 0.97
    readonly property int pianoToolsChipWidth: 40
    readonly property int pianoToolsChipHeight: 20
    readonly property int pianoToolsSlideWidth: 230
    readonly property int pianoToolsSlideHeight: 22
    readonly property int pianoToolsSlideLabelWidth: 74
    readonly property int pianoToolsSlideTextY: 4
    readonly property int pianoToolsSlideTrackY: 9
    readonly property int pianoToolsSlideTrackHeight: 4
    readonly property color pianoToolsSlideTrack: "#26323c"
    readonly property real pianoToolsSlideFillOpacity: 0.8
    readonly property int pianoToolsSlideThumbSize: 10
    readonly property int pianoToolsSlideValueReserve: 40
    readonly property int pianoToolsSlideValueWidth: 36
    readonly property int pianoToolsActionWidth: 104
    readonly property int pianoToolsActionHeight: 24
    readonly property int pianoToolsActionRadius: 5
    readonly property real pianoToolsLinkLetterSpacing: 0.6
    readonly property int pianoToolsLinkHitOutset: 3
    readonly property color pianoToolsPathGround: "#10171d"
    readonly property real pianoToolsPathLetterSpacing: 0.8
    readonly property int pianoToolsPathInputY: 18
    readonly property int pianoToolsGrooveRowHeight: 18
    readonly property color pianoToolsGrooveRowHover: "#1f2a33"
    readonly property int pianoToolsGrooveTextReserve: 60
    readonly property int pianoToolsGrooveFeelRight: 54
    readonly property int pianoToolsGrooveFeelWidth: 48
    readonly property int pianoToolsGrooveFeelMinBarWidth: 2
    readonly property int pianoToolsGrooveFeelBase: 4
    readonly property int pianoToolsGrooveFeelRange: 8
    readonly property color pianoToolsGrooveEarly: "#7aa7ff"
    // The velocity lane and pedal strip under the notes.
    readonly property color noteLaneEdge: "#2a363f"
    readonly property int velocityHeaderHeight: 22
    readonly property int velocityStemArea: 50
    readonly property color velocityHeaderGround: "#182027"
    readonly property int velocityChipPadding: 14
    readonly property int velocityChipHeight: 17
    readonly property int velocityValueWidth: 38
    readonly property color velocityValueGround: "#10161b"
    readonly property real velocityDisabledOpacity: 0.4
    readonly property color velocityGround: "#131a20"
    readonly property color velocityGuide: "#22303a"
    readonly property int velocityStemInset: 4
    readonly property int velocityFanOffset: 5
    readonly property real velocityDrawStroke: 1.5
    readonly property int velocityStemHitWidth: 10
    readonly property int velocityStemWidth: 2
    readonly property color velocityStem: "#4d7d70"
    readonly property int velocityHeadSize: 8
    readonly property color velocityHead: "#3f9c80"
    readonly property color velocityHeadFocusEdge: "#ffffff"
    readonly property color velocityHeadEdge: "#77c7a9"
    readonly property int velocityLabelX: 11
    readonly property int velocityLabelRise: 10
    readonly property int pedalRowHeight: 26
    readonly property color pedalLabelGround: "#1d252d"
    readonly property color pedalLaneGround: "#151c22"
    readonly property int pedalSpanInset: 3
    readonly property real pedalPreviewOpacity: 0.45
    readonly property int pedalSpanMinSize: 4
    readonly property color pedalSpanFocused: "#f3c76a"
    readonly property real pedalSpanRestOpacity: 0.55
    readonly property real pedalSpanDepthOpacity: 0.45
    readonly property color pedalLiftMark: "#ffe3a0"
    readonly property int pedalGrabWidth: 6
    readonly property int pedalDepthGrabHeight: 5
    readonly property int pedalExpandMargin: 5
    // -- end piano roll

    // -- Clip editor, clip menus and popovers --
    // The clip editor: header, overview, ruler, detail and warp markers.
    readonly property color clipEditorMarker: "#ffcf5c"
    readonly property color clipEditorLate: "#ff7a6b"
    readonly property int clipEditorSnapDistance: 12
    readonly property int clipEditorControlHeight: 28
    readonly property int clipEditorButtonPadding: 20
    readonly property real clipEditorInactiveOpacity: 0.4
    readonly property real clipEditorLitWashAlpha: 0.12
    readonly property color clipEditorButtonHover: "#1c242d"
    readonly property int clipEditorHeaderY: 10
    readonly property int clipEditorHeaderHeight: 30
    readonly property int clipEditorHeaderSpacing: 10
    readonly property int clipEditorSwatchSize: 10
    readonly property int clipEditorDividerHeight: 24
    readonly property int clipEditorTempoWidth: 70
    readonly property int clipEditorTempoTextInset: 7
    readonly property int clipEditorSwitchWidth: 22
    readonly property int clipEditorSwitchHeight: 12
    readonly property int clipEditorSwitchKnobSize: 8
    readonly property int clipEditorStatusDotSize: 8
    readonly property int clipEditorOverviewY: 52
    readonly property int clipEditorOverviewHeight: 28
    readonly property real clipEditorOverviewWaveAlpha: 0.45
    readonly property int clipEditorViewBoxMinWidth: 4
    readonly property real clipEditorViewWashAlpha: 0.08
    readonly property int clipEditorRulerY: 86
    readonly property int clipEditorRulerHeight: 20
    readonly property int clipEditorRulerBeatLabelMinBar: 160
    readonly property int clipEditorRulerLabelMinSpacing: 56
    readonly property color clipEditorRulerTick: "#2a3542"
    readonly property int clipEditorDetailY: 106
    readonly property int clipEditorDetailMinHeight: 80
    readonly property int clipEditorFooterHeight: 52
    readonly property color clipEditorDetailGround: "#0d1116"
    readonly property color clipEditorDetailEdge: "#222a33"
    readonly property int clipEditorBeatMinSpacing: 3
    readonly property string clipEditorBarLine: "rgba(255,255,255,0.16)"
    readonly property string clipEditorBeatLine: "rgba(255,255,255,0.05)"
    readonly property real clipEditorDetailWaveAlpha: 0.66
    readonly property string clipEditorOnset: "rgba(255,255,255,0.45)"
    readonly property int clipEditorOnsetHeight: 10
    readonly property real clipEditorSelectionWashAlpha: 0.08
    readonly property real clipEditorSelectionEdgeAlpha: 0.5
    readonly property color clipEditorHoverGhost: Qt.rgba(1, 0.81, 0.36, 0.6)
    readonly property int clipEditorHoverLabelY: 40
    readonly property int clipEditorFlagHeight: 16
    readonly property int clipEditorMarkerDash: 3
    readonly property int clipEditorMarkerHeadSize: 11
    readonly property int clipEditorSegmentTagY: 20
    readonly property int clipEditorSegmentTagHeight: 15
    readonly property int clipEditorMarkerGrabWidth: 16
    readonly property int clipEditorMarkerGrabHeight: 24
    readonly property real clipEditorDriftY: 0.62
    readonly property int clipEditorDriftSpacing: 5
    readonly property int clipEditorDriftBadgeSize: 18
    readonly property color clipEditorDriftGround: "#1c1414"
    readonly property color clipEditorDriftEdge: "#5a2a26"
    readonly property color clipEditorDriftText: "#ffb3a8"
    readonly property int clipEditorFooterDividerHeight: 20
    // Warp markers on the selected clip (WarpMarkerOverlay, ADR-0112): the
    // band along the waveform's foot where the pointer pins them.
    readonly property int warpBandHeight: 14
    // ...but never more than this share of the waveform's height.
    readonly property real warpBandMaxShare: 0.34
    readonly property int warpMarkerHeadSize: 9
    readonly property int warpTagHeight: 15
    readonly property int warpTagGap: 4
    readonly property color warpTagGround: Qt.rgba(0.043, 0.055, 0.071, 0.85)
    readonly property color warpMarkerFaded: Qt.rgba(1, 0.81, 0.36, 0.35)
    readonly property int warpMenuWidth: 220
    readonly property int warpMenuEntryHeight: 24
    readonly property int warpMenuPadding: 4
    // A stretch or slide keeps markers this far (px) from their neighbours.
    readonly property int warpMarkerMinGap: 2
    // The project tempo lane under the ruler (TempoLane, ADR-0112).
    readonly property int tempoLaneCollapsedHeight: 14
    readonly property int tempoLaneExpandedHeight: 96
    readonly property color tempoLaneGround: "#0f1318"
    readonly property color tempoLaneGuide: "#1a222b"
    readonly property color tempoLaneCurve: "#f5b14c"
    readonly property real tempoLaneCurveWidth: 1.5
    readonly property int tempoLanePointSize: 8
    readonly property int tempoLanePointGrab: 14
    readonly property int tempoLanePadding: 8
    // BPM guides at least this far apart (px), on a whole-BPM step.
    readonly property int tempoLaneGuideMinSpacing: 18
    readonly property real tempoLaneRampWashAlpha: 0.07
    readonly property int tempoLaneTagHeight: 16
    readonly property int tempoLaneMenuWidth: 180
    readonly property int tempoLaneHeaderSwatch: 8
    // A clip stretched further than this (percent) shows its stretch in amber.
    readonly property real stretchWarnPercent: 6
    // The strip on an audio clip's cap: name and tempo, Follow and mode chips.
    readonly property int clipStripSpacing: 4
    readonly property int clipStripHeight: 16
    readonly property int clipChipHeight: 14
    readonly property int clipChipPadding: 10
    readonly property real clipChipLitAlpha: 0.18
    readonly property color clipChipEdge: Qt.rgba(1, 1, 1, 0.18)
    readonly property int clipStripMinWidth: 200
    // A clip's level envelope and its fades.
    readonly property int clipLevelTop: 20
    readonly property int clipLevelBottomGap: 26
    readonly property int clipLevelCurveStep: 3
    readonly property int clipLevelHandleSize: 11
    readonly property int clipLevelHandleRadius: 6
    readonly property int clipLevelHandleHitOutset: 5
    readonly property int clipFadeLineY: 18
    readonly property int clipFadeMinClipWidth: 52
    readonly property int clipFadeHandleMargin: 10
    readonly property int clipFadeHandleWidth: 10
    readonly property int clipFadeHandleHeight: 12
    readonly property int clipFadeHandleY: 20
    // The clip tempo and stretch-mode popover.
    readonly property int clipTempoTempoWidth: 360
    readonly property int clipTempoModeWidth: 300
    readonly property int clipTempoSpacing: 10
    readonly property int clipTempoRowHeight: 30
    readonly property int clipTempoFieldWidth: 78
    readonly property real clipTempoNoteLineHeight: 1.3
    readonly property real clipTempoHeadingLetterSpacing: 1.2
    readonly property real clipTempoModeWashAlpha: 0.10
    readonly property color clipTempoModeEdge: "#222a33"
    readonly property int clipTempoModeTextX: 10
    readonly property int clipTempoModeNameWidth: 84
    readonly property int clipTempoModeHintReserve: 114
    readonly property int clipTempoButtonPadding: 18
    // Clip context menus and their colour and name foot.
    readonly property int clipMenuWidth: 240
    readonly property int clipMenuRowHeight: 34
    readonly property int clipMenuFieldHeight: 26
    readonly property int clipMenuFieldRadius: 5
    readonly property color clipMenuFieldGround: "#11161c"
    readonly property int clipMenuSwatchX: 10
    readonly property int clipMenuSwatchSpacing: 5
    readonly property int clipMenuSwatchSize: 18
    readonly property int clipMenuTitleHeight: 29
    readonly property int clipMenuTitleY: 9
    readonly property int clipMenuEntryStride: 42
    readonly property int clipMenuEntryHeight: 40
    readonly property int clipMenuEntryRadius: 5
    readonly property int clipMenuEntryTextX: 9
    readonly property int clipMenuEntryHintY: 21
    readonly property int clipMenuKeysY: 5
    readonly property int clipMenuDividerGap: 9
    // -- end clip editor

    // -- Note effects, warp and tempo editors --
    // A track's MIDI effects strip: its cards, phrase drawings and engine chooser.
    readonly property color noteFxAccent: "#b490ff"
    readonly property color noteFxGround: "#0f1118"
    readonly property int noteFxKnobDiameter: 32
    readonly property int noteFxKnobMinWidth: 54
    readonly property int noteFxKnobWidthPad: 18
    readonly property color noteFxWell: "#090b10"
    readonly property real noteFxCardPopScale: 0.92
    readonly property int noteFxCardPopDrop: 8
    readonly property int noteFxCardWidth: 236
    readonly property color noteFxCardGround: "#141824"
    readonly property color noteFxCardEdge: "#262c3a"
    readonly property real noteFxBypassedOpacity: 0.55
    readonly property int noteFxHeadHeight: 22
    readonly property int noteFxPowerSize: 14
    readonly property real noteFxNameLetterSpacing: 1.2
    readonly property int noteFxToolSize: 18
    readonly property color noteFxToolHover: "#232a3a"
    readonly property int noteFxToolGlyphSize: 13
    readonly property int noteFxPresetRowHeight: 20
    readonly property int noteFxGlyphHeight: 38
    readonly property int noteFxGlyphInset: 5
    readonly property real noteFxGlyphFadeFrom: 0.35
    readonly property real noteFxGlyphInkAlpha: 0.85
    readonly property real noteFxGlyphGuideAlpha: 0.25
    readonly property real noteFxGlyphFaintAlpha: 0.12
    readonly property real noteFxGlyphDimAlpha: 0.4
    readonly property real noteFxGlyphLitAlpha: 0.95
    readonly property int noteFxKnobsTop: 48
    readonly property int noteFxKnobsBottomGap: 22
    readonly property int noteFxKnobDiameterSmall: 30
    readonly property int noteFxMoreHeight: 16
    readonly property color noteFxMoreOn: "#2a2440"
    readonly property color noteFxMoreEdge: "#2a3040"
    readonly property int noteFxAddWidth: 30
    readonly property color noteFxAddHover: "#1b1f2c"
    readonly property real noteFxChooserScale: 0.96
    readonly property int noteFxChooserWidth: 150
    readonly property color noteFxChooserGround: "#171b26"
    readonly property int noteFxChoiceHeight: 24
    readonly property int noteFxChoiceRadius: 5
    readonly property color noteFxChoiceHover: "#262040"
    // The warp panel's palette, pills, steppers and status.
    readonly property color warpTextMain: "#e6e9ef"
    readonly property color warpTextDim: "#8b93a1"
    readonly property color warpAccent: "#5fb3ff"
    readonly property color warpAmber: "#f2b84b"
    readonly property color warpDanger: "#ff6b6b"
    readonly property color warpLine: "#2c3440"
    readonly property int warpPillHeight: 24
    readonly property int warpPillMinWidth: 44
    readonly property int warpPillRadius: 5
    readonly property color warpControlFill: "#1c222c"
    readonly property real warpUnavailableOpacity: 0.5
    readonly property color warpPillSelectedText: "#0d1015"
    readonly property int warpHeaderHeight: 14
    readonly property real warpTitleLetterSpacing: 1.1
    readonly property int warpStatusRadius: 5
    readonly property color warpStatusGround: "#141920"
    readonly property int warpLabelWidth: 108
    readonly property int warpMarkerLaneHeight: 96
    readonly property int warpStepperHeight: 34
    readonly property int warpStepperNoteY: 15
    readonly property int warpStepperTrackX: 112
    readonly property int warpStepperTrackY: 5
    readonly property int warpStepperTrackMinWidth: 40
    readonly property int warpStepperValueReserve: 70
    readonly property int warpStepperTrackHeight: 12
    readonly property int warpStepperFillMinWidth: 6
    readonly property int warpStepperValueWidth: 64
    readonly property int warpStepperValueY: 3
    // The warp marker lane: beats, the mapped waveform, cursor and markers.
    readonly property int warpSnapDistance: 8
    readonly property color warpBeatLineDownbeat: "#3a4452"
    readonly property color warpBeatLine: "#232b36"
    readonly property color warpWave: "#6f8aa8"
    readonly property real warpWaveScale: 0.9
    readonly property real warpCursorFocusedOpacity: 0.8
    readonly property real warpCursorOpacity: 0.25
    readonly property int warpProposalSize: 10
    readonly property real warpMarkStroke: 1.5
    readonly property int warpSuggestionSize: 10
    readonly property real warpSuggestionStemOpacity: 0.45
    readonly property int warpMarkerWidth: 12
    readonly property int warpMarkerFootSize: 12
    readonly property int warpMarkerFootBottom: 14
    // The project tempo map editor.
    readonly property real tempoMapTitleLetterSpacing: 1.1
    readonly property int tempoMapControlRadius: 5
    readonly property real tempoMapLockedOpacity: 0.55
    readonly property color tempoMapApplyText: "#101b22"
    readonly property color tempoMapRowSelected: "#254649"
    readonly property color tempoMapEditFill: "#25443f"
    readonly property color tempoMapDeleteFill: "#4b3037"
    // -- end warp

    // -- Automation lanes and editors --
    // Automation lanes: ground, curve, points, readout, Draw switch and curve menu.
    readonly property color automationOverlayGround: Qt.rgba(0.04, 0.055, 0.07, 0.55)
    readonly property color automationLaneGround: "#0f1318"
    readonly property int automationPointHitRadius: 8
    readonly property int automationSegmentHitDistance: 6
    readonly property color automationLaneDivider: "#1a212a"
    readonly property real automationFillAlpha: 0.13
    readonly property real automationStroke: 1.5
    readonly property real automationIdleAlpha: 0.45
    readonly property var automationIdleDash: [4, 4]
    readonly property int automationCurveStep: 3
    readonly property int automationPointCullMargin: 8
    readonly property int automationPointRadius: 4
    readonly property int automationPointRadiusSelected: 5
    readonly property real automationBandWashAlpha: 0.10
    readonly property real automationBandEdgeAlpha: 0.6
    readonly property int automationReadoutOffset: 10
    readonly property int automationReadoutPadding: 10
    readonly property int automationReadoutHeight: 16
    readonly property int automationDrawColumn: 3
    readonly property int automationDrawHeight: 18
    readonly property int automationChoiceHeight: 22
    readonly property color automationChoiceHover: "#22303c"
    // The segment shape editor and its Bézier handles.
    readonly property int automationSegmentHeight: 70
    readonly property int automationSegmentBezierHeight: 120
    readonly property int automationShapeButtonSize: 24
    readonly property int automationShapeLabelX: 29
    readonly property int automationPreviewX: 9
    readonly property int automationPreviewY: 32
    readonly property int automationPreviewHeight: 32
    readonly property int automationHandleSize: 10
    readonly property int automationHandleHitOutset: 5
    readonly property int automationFieldWidth: 42
    readonly property int automationFieldHeight: 25
    readonly property int automationFieldLabelRise: 13
    readonly property int automationFieldsY: 86
    readonly property int automationFieldSpacing: 3
    readonly property int automationApplyMinWidth: 27
    readonly property int automationApplyReserve: 180
    readonly property color automationApplyFill: "#234239"
    // The automation timebase switch.
    readonly property int automationTimebaseHeight: 20
    readonly property color automationTimebaseFill: "#203c38"
    readonly property color automationTimebaseUnavailable: "#30343b"
    readonly property color automationTimebaseUnavailableEdge: "#51565d"
    readonly property color automationTimebaseUnavailableText: "#90959d"
    // The channel automation editor and graph.
    readonly property int automationFieldRadius: 5
    readonly property int automationFieldInset: 5
    readonly property color automationActionDisabled: "#30343b"
    readonly property color automationInactiveWarning: "#e0a03a"
    readonly property color automationGraphGround: "#18231f"
    readonly property int automationGraphPagerWidth: 18
    readonly property int automationGraphPagerStride: 21
    readonly property int automationGraphPlotTop: 24
    readonly property int automationGraphHandleSize: 11
    readonly property int automationGraphHandleRadius: 6
    // The plugin automation editor.
    readonly property int automationControlHeight: 24
    readonly property int automationFieldTextInset: 5
    // The native synth automation panel.
    readonly property color synthAutomationGround: "#15202c"
    readonly property color synthAutomationEdge: "#61758b"
    readonly property color synthAutomationFocusEdge: "#8ac5ec"
    readonly property color synthAutomationActionHover: "#35556a"
    readonly property color synthAutomationAction: "#293b4c"
    readonly property color synthAutomationActionUnavailable: "#25303a"
    readonly property color synthAutomationText: "#e1edf7"
    readonly property color synthAutomationTextUnavailable: "#8496a8"
    readonly property color synthAutomationField: "#253545"
    readonly property color synthAutomationLabel: "#dce9f5"
    readonly property color synthAutomationSummary: "#aec4d8"
    readonly property color synthAutomationWarning: "#f0bf80"
    readonly property color synthAutomationOk: "#9bd5b3"
    readonly property color synthAutomationNote: "#8199af"
    readonly property int synthAutomationInset: 14
    // -- end automation
    // END arrangement tokens

    // ---- Shell surfaces: browser, inspector, toolbar, dialogs, mixer ----
    // (tokens for DockedBrowser, InspectorPanel, ProjectToolbar,
    // WelcomeScreen, ExportPanel, the device chain, mixer/ and overlays)

    // -- shell frame, browser, inspector --
    // Shared by the shell's panels, browser and dialogs.
    readonly property color fieldEdge: "#26303b"
    readonly property color rowHighlight: "#1f2a35"
    readonly property color textSoft: "#c3ccd6"
    readonly property color textDisabled: "#56616e"
    readonly property color accentWash: "#1d3a32"
    readonly property color accentWashEdge: "#2f6b5c"
    readonly property color onAccentText: "#102a23"
    readonly property color focusOnAccent: "#ffffff"
    // Keyboard focus: an accent ring round the focused control, a bold
    // stroke standing just outside its edge (shell/FocusRing), or its own
    // edge turned accent and bold.
    readonly property color focusRing: accent
    readonly property real focusRingWidth: strokeBold
    readonly property real focusRingOffset: strokeBold
    readonly property color tagFill: "#1a212a"
    readonly property color tagText: "#aeb8c4"
    readonly property real controlRadius: 5
    readonly property real panelRadius: 9
    readonly property real popoverRadius: 10
    readonly property int fieldPadding: 5
    readonly property int closeGlyphSize: 17
    readonly property string monoSystem: "monospace"
    // Letter spacing for capitals labels, tightest to widest.
    readonly property real trackingXxs: 0.6
    readonly property real trackingXs: 0.7
    readonly property real trackingSm: 0.8
    readonly property real trackingMd: 1
    readonly property real trackingLg: 1.1
    readonly property real trackingXl: 1.2
    readonly property real tracking2xl: 1.4
    readonly property real tracking3xl: 2
    // The window and its docks.
    readonly property int windowWidth: 1440
    readonly property int windowHeight: 900
    readonly property int windowMinWidth: 1024
    readonly property int windowMinHeight: 680
    readonly property int browserDockWidth: 248
    readonly property int deviceDockMinHeight: 270
    readonly property int deviceDockMidiHeight: 440
    // Height always left for the arrangement above the device dock.
    readonly property int deviceDockArrangeReserve: 220
    readonly property int keyHintBottomGap: 18
    readonly property int developerDetailsWidth: 460
    readonly property color browserScrim: "#99080c12"
    readonly property int pluginBrowserWidth: 460
    readonly property real pluginBrowserWidthShare: 0.42
    // The problem toast.
    readonly property color toastFill: "#2a1d14"
    readonly property color toastText: "#f3dcc0"
    readonly property int toastBottomMargin: 36
    readonly property int toastWindowInset: 64
    readonly property int toastPaddingX: 44
    readonly property int toastPaddingY: 20
    readonly property int toastTextInset: 108
    // The shell tooltip (TipLayer).
    readonly property color tipBorder: "#33414d"
    readonly property color tipText: "#e8edf2"
    readonly property color tipTextDim: "#8a98a6"
    readonly property color tipKeyFill: "#1a2129"
    readonly property real shellTipRadius: 5
    readonly property int tipGap: 6
    readonly property int tipMaxWidth: 360
    readonly property int tipTextMaxWidth: 300
    readonly property int tipPaddingX: 16
    readonly property int tipPaddingY: 10
    readonly property int tipKeyPaddingX: 10
    readonly property int tipKeyHeight: 16
    // Docked browser and its cards.
    readonly property color browserFill: "#0f1318"
    readonly property color browserToggleHover: "#1d252e"
    readonly property color browserChipHover: "#161d25"
    readonly property color browserCardFill: "#121820"
    readonly property color browserCardHover: "#151c25"
    readonly property color browserCompactCardHover: "#131920"
    readonly property color browserVst3InstrumentColor: "#8fa6ff"
    readonly property color browserVst3EffectColor: "#7fb8ff"
    readonly property color browserRackColor: "#ff9f43"
    readonly property color browserNoteEffectColor: "#b490ff"
    readonly property real browserShapeFillAlpha: 0.18
    readonly property int browserSearchHeight: 34
    readonly property int searchIconX: 10
    readonly property int searchTextInset: 28
    readonly property int browserTabHeight: 28
    readonly property int browserChipHeight: 22
    readonly property int browserHeadingHeight: 26
    readonly property int browserCompactCardHeight: 52
    readonly property int browserCardHeight: 128
    readonly property real browserCardRadius: 12
    readonly property int browserCompactChipSize: 32
    readonly property int browserChipSize: 34
    readonly property real browserChipRadius: 9
    readonly property int browserHintTop: 10
    readonly property int browserHintButtonsHeight: 58
    readonly property int browserHintFolderHeight: 34
    readonly property int browserFolderButtonRise: 70
    readonly property int browserExpandButtonRise: 36
    readonly property int browserButtonHeight: 28
    readonly property int browserMenuWidth: 224
    readonly property int browserMenuRowHeight: 34
    readonly property int browserMenuTextInset: 10
    // Inspector.
    readonly property int inspectorWideBreakpoint: 1220
    readonly property int inspectorWideWidth: 286
    readonly property int inspectorNarrowWidth: 242
    readonly property int inspectorCloseRight: 37
    readonly property int inspectorCloseSize: 25
    readonly property color inspectorBadgeFill: "#234239"
    readonly property int inspectorBadgeGlyphSize: 22
    // Built-in synth source card.
    readonly property color synthCardFill: "#1b2330"
    readonly property color synthCardEdge: "#3d5264"
    readonly property color synthCardTitle: "#a9d8ff"
    readonly property color synthCardText: "#d6e1ee"
    readonly property color synthCardTextDim: "#98a9bc"
    readonly property color synthCardButton: "#263646"
    readonly property color synthCardButtonHover: "#35485d"
    readonly property color synthCardButtonEdge: "#5c7895"
    readonly property color synthCardOpen: "#2b4865"
    readonly property color synthCardOpenHover: "#395979"
    readonly property color synthCardOpenEdge: "#75b6e4"
    readonly property color synthCardOpenText: "#e7f4ff"
    readonly property int synthCardTitleSize: 17
    // -- end shell frame --

    // -- toolbar, welcome screen, palettes, mixer view --
    // Mixer view: strip colours by track order, return order and engine.
    // Kept as strings: strip data carrying them is compared as JSON.
    readonly property var mixerTrackPalette: ["#f5b14c", "#8fa6ff", "#c49bff", "#e8d9b8", "#ff8f6b", "#ff7aa8", "#9be36b", "#6be3d8", "#ffb36b", "#7fb8ff"]
    readonly property var mixerReturnPalette: ["#7fb8ff", "#ffb36b", "#c49bff", "#9be36b", "#ff7aa8", "#6be3d8"]
    readonly property var mixerEngineColors: ({ contour: "#ff7aa8", forge: "#ff9d5c", orbit: "#c49bff", relay: "#ffb36b", expanse: "#7fb8ff", vista: "#6be3d8", deck: "#ff8fd0", pocket: "#5fe0bf", lens: "#9be36b", loudness: "#6be3d8", scope: "#8fa6ff", tune: "#f5b14c", trim: "#b9c3cf", signal: "#ff6b6b", shape: "#8fa6ff", press: "#f5b14c", ceiling: "#ff6b6b" })
    readonly property var mixerNativeKindColors: ({ convolution: "#c49bff" })
    // Project toolbar.
    readonly property int toolbarHeight: 56
    readonly property color recordColor: "#ff4d4d"
    readonly property color transportGlyphOff: "#4a5562"
    readonly property color meterTrack: "#1a212a"
    readonly property color masterMeterHot: "#ff6b6b"
    readonly property color meterPeakHot: "#ff9b9b"
    // Hover lifts a raised control by this factor (Qt.lighter).
    readonly property real hoverLighten: 1.25
    readonly property real unusableOpacity: 0.55
    readonly property real recordArmedAlpha: 0.22
    readonly property real recordBlinkLow: 0.3
    readonly property real amberToggleAlpha: 0.14
    readonly property real accentToggleAlpha: 0.16
    readonly property real toolbarIconStroke: 1.2
    // The project identity (logo, name and status) in the toolbar and the welcome screen.
    readonly property int logoSize: 22
    readonly property int identityLineGap: 1
    // Welcome screen.
    readonly property color welcomeBackdrop: "#0d1116"
    readonly property color welcomeScrim: "#d90b0e12"
    readonly property color textSelection: "#2f5a51"
    readonly property color raisedHover: "#1a212a"
    readonly property color rowDivider: "#1a212a"
    readonly property color renameFieldFill: "#11161c"
    readonly property color dangerFill: "#5c2222"
    readonly property color dangerHover: "#7a2a2a"
    readonly property color dangerEdge: "#8c3a3a"
    readonly property real disabledOpacity: 0.5
    readonly property int welcomeHeadlineSize: 22
    readonly property int recoveryTitleSize: 20
    // Command palette.
    readonly property color paletteScrim: "#990a0d12"
    readonly property color paletteSelection: Qt.rgba(0.37, 0.88, 0.75, 0.13)
    readonly property color paletteTrackKind: "#2a2340"
    readonly property color paletteInstrumentKind: "#1d2b45"
    readonly property color paletteEffectKind: "#163a33"
    readonly property color palettePresetKind: "#3a2e17"
    readonly property real disabledControlOpacity: 0.45
    // Shortcuts overlay.
    readonly property color shortcutsScrim: "#b3080b10"
    // -- end toolbar --

    // -- floating panels and dialogs --
    // Floating panels (cache, history, media, versions, audio output).
    readonly property int dialogCloseY: 9
    readonly property int closeHitSlop: 6
    readonly property int dialogButtonHeight: 31
    readonly property int dialogInputPadding: 7
    // History and saved versions open under the toolbar's right-hand actions.
    readonly property int menuPanelRightMargin: 202
    readonly property color historyCurrentFill: Qt.rgba(0.37, 0.88, 0.75, 0.12)
    // Key review.
    readonly property color keyReviewControlFill: "#1c222c"
    readonly property color keyReviewApplyText: "#0d1015"
    // Audio output choices, routing, the plug-in window and VST3 automation.
    readonly property color choiceSelectedFill: "#24473d"
    readonly property color routeButtonOff: "#151a22"
    readonly property color pluginWindowFill: "#0b0e13"
    readonly property color automatedFill: "#234239"
    // Full browser (Vst3BrowserPanel).
    readonly property color browserCategoryCurrent: "#1a222c"
    readonly property color browserCategoryText: "#aeb8c4"
    readonly property color chipEdgeCurrent: "#3a4756"
    readonly property color disabledFill: "#1a212a"
    readonly property color browserActionEdge: "#2e5a4d"
    readonly property color pluginStateWarning: "#ecbc76"
    readonly property color previewPaneFill: "#10141a"
    readonly property color presetRowFill: "#141a21"
    readonly property int largeCloseGlyphSize: 20
    readonly property int dragGlyphSize: 19
    readonly property int previewChipGlyphSize: 24
    readonly property int previewTitleSize: 20
    // -- end panels --

    // -- device chain --
    // Device chain dock.
    readonly property color deviceDockFill: "#10141a"
    readonly property color resizeHandleActive: "#2f5a51"
    readonly property real tabRevealFrom: 0.35
    readonly property real inactiveOpacity: 0.4
    readonly property int clipDetailTitleSize: 17
    readonly property color frozenCover: "#b30e1a21"
    readonly property color frozenTitle: "#bfe6fa"
    readonly property color unfreezeFill: "#1a3442"
    readonly property color unfreezeHover: "#24485a"
    readonly property color unfreezeEdge: "#84c8ec"
    readonly property color unfreezeText: "#dff3ff"
    // Device strip body: knobs and the mini-view.
    readonly property int stripKnobDiameter: 34
    readonly property int stripKnobMinWidth: 58
    readonly property int stripKnobWidthPad: 18
    // A strip knob's slot: an equal share of the strip, at most this wide.
    readonly property int stripKnobMaxWidth: 96
    // The mini-view: its reduction band bars (share of the view),
    // the gap between bars, the meters' label column and dimmed marks.
    readonly property real stripBandShare: 0.3
    readonly property int stripBarGap: 2
    readonly property int stripMeterLabelRoom: 24
    readonly property real stripDimAlpha: 0.45
    readonly property real stripBaselineAlpha: 0.35
    readonly property real stripFillAlpha: 0.16
    readonly property real stripCurveWidth: 1.5
    // Floating device windows (FloatingDeviceFrame) and their sidechain strip.
    readonly property int floatingPanelTop: 76
    readonly property int floatingSurfaceWidth: 640
    readonly property int floatingSurfaceHeight: 480
    readonly property int floatingMinSurfaceWidth: 480
    readonly property int floatingMinSurfaceHeight: 360
    readonly property int floatingGripSize: 16
    readonly property int floatingEdgeGrab: 6
    readonly property real gripStroke: 1.5
    readonly property color deviceChromeFill: "#151a22"
    readonly property color deviceChromeEdge: "#526879"
    readonly property color deviceChromeText: "#e9eef5"
    readonly property color deviceChromeAccent: "#72e3c0"
    readonly property color deviceChromeTitleBar: "#202a35"
    readonly property color deviceChromeButton: "#293342"
    readonly property color deviceChromeButtonHover: "#35556a"
    readonly property color deviceChromeButtonEdge: "#738397"
    readonly property real keyGainFillAlpha: 0.22
    readonly property color keyMeterTrack: "#111720"
    readonly property color listenOnText: "#0d1117"
    readonly property color sourceMenuFill: "#1b222c"
    readonly property color sourceMenuHover: "#2c3846"
    // Macro strip.
    readonly property color macroStripFill: "#0e1217"
    readonly property color macroButtonEdge: "#2b3542"
    readonly property color macroInkDark: "#0b0f14"
    readonly property color rackNameFill: "#151b23"
    readonly property color macroTrack: "#26303b"
    readonly property color macroKnobCap: "#1c242e"
    readonly property color removeHoverFill: "#3a2226"
    readonly property color removeGlyph: "#f0a0a0"
    readonly property color handlePressed: "#ffffff"
    readonly property real macroRingWidth: 1.5
    readonly property real macroArcWidth: 3.5
    readonly property real macroHaloHigh: 0.55
    readonly property real macroHaloLow: 0.15
    readonly property real macroUnmappedAlpha: 0.45
    readonly property real macroRangeOpacity: 0.55
    readonly property int handleHitSlop: 4
    // Device chain strips (InsertChainPanel).
    readonly property color chainNeutralAccent: "#9aa6b5"
    readonly property color stripFill: "#141a21"
    readonly property color stripEdge: "#232c37"
    readonly property real stripHeaderAlpha: 0.10
    readonly property color powerOffEdge: "#3a4552"
    readonly property real powerGlyphStroke: 1.6
    readonly property color flowArrow: "#3a4552"
    readonly property real draggedStripOpacity: 0.65
    readonly property real bypassedStripOpacity: 0.78
    readonly property color chainDropZoneEdge: "#2a3440"
    readonly property real chainDropZoneStroke: 1.5
    readonly property color outputCardFill: "#11161c"
    readonly property real sendDialWidth: 4
    readonly property color sendDialTrack: "#252e39"
    readonly property color sendDialCap: "#171d25"
    // -- end device chain --
    // END browser tokens

    // ---- Export panel and dialog (ExportPanel, ExportDialog) ----
    // The dialog: its scrim, its card and title, the close button, and the
    // top of the scrolling panel under the title.
    readonly property color exportScrim: "#b3080b10"
    readonly property int exportDialogMaxWidth: 560
    readonly property real exportDialogRadius: 14
    readonly property int exportTitleY: 22
    readonly property int exportProjectY: 48
    // Room the project name leaves on the right for the close button.
    readonly property int exportProjectRight: 120
    // The close button's inset from the card's top right corner.
    readonly property int exportCloseInset: 18
    readonly property int exportCloseSize: 30
    readonly property real exportCloseRadius: 7
    readonly property int exportBodyY: 76
    // The panel's fields and buttons.
    readonly property int exportFieldHeight: 34
    readonly property int exportFieldPadding: 10
    // Room the name field leaves beside the extension label.
    readonly property int exportNameRightPad: 26
    readonly property int exportSecondaryButtonWidth: 92
    readonly property int exportPrimaryButtonWidth: 120
    readonly property real exportDisabledOpacity: 0.5
    // Trim tail: the gap above it, its row and its check box.
    readonly property int exportTrimGap: 14
    readonly property int exportCheckRowHeight: 22
    readonly property int exportCheckSize: 16
    readonly property int exportProgressHeight: 4
    readonly property real exportLineHeight: 1.25
    // Show in folder: padding round its label, and its height.
    readonly property int exportButtonPad: 24
    readonly property int exportSmallButtonHeight: 30
    // The footer: its gap above, its height, the More exports toggle.
    readonly property int exportFooterGap: 18
    readonly property int exportFooterHeight: 38
    readonly property int exportToggleHeight: 28
    // More exports: its rows' top, their height, the end-bar field's width.
    readonly property int exportSectionTop: 14
    readonly property int exportRowHeight: 32
    readonly property int exportBarFieldWidth: 44
    // The drop-down menus and their options.
    readonly property real exportMenuRadius: 7
    readonly property int exportMenuOptionHeight: 28
    readonly property int exportMenuTextX: 9
    readonly property int exportStemOptionHeight: 26
    // END export tokens

    // ---- Mixer view (mixer/): strips, meters, faders, knobs ----
    readonly property color mixerStripFill: "#11161c"
    readonly property color mixerMasterFill: "#141b23"
    readonly property color mixerSlotHoverEdge: "#3a4552"
    readonly property color mixerAddSlotHover: "#131920"
    readonly property color mixerKeyIdleInk: "#aeb8c4"
    readonly property color mixerRouteMenuFill: "#1a222c"
    readonly property color mixerRouteMenuEdge: "#3a4756"
    readonly property color mixerRouteChoiceHover: "#24303c"
    readonly property color mixerMeterWellColor: "#151b22"
    readonly property color mixerMeterOverColor: "#ff6b6b"
    readonly property color mixerMeterHotColor: "#f5b14c"
    readonly property color mixerMeterSafeColor: "#5fe0bf"
    readonly property color mixerMeterFloorColor: "#3fa58a"
    readonly property color mixerMeterHoldColor: "#e7ecf2"
    readonly property color masterSectionWarnColor: "#ff6b6b"
    readonly property color mixerFaderTrack: "#26303b"
    readonly property color mixerFaderDetent: "#3a4552"
    readonly property color mixerFaderCapTop: "#3a4552"
    readonly property color mixerFaderCapBottom: "#252e39"
    readonly property color mixerFaderCapRim: "#4a5664"
    readonly property color mixerKnobFaceEdge: "#2a3440"
    // The decibel scale beside each strip's meters and fader: its marks, the
    // brighter unity (0 dB) mark and their labels.
    readonly property color mixerScaleTick: "#3a4552"
    readonly property color mixerScaleUnity: "#aeb8c4"
    readonly property real mixerScaleTickLength: 4
    readonly property real mixerScaleUnityLength: 7

    // ---- Device editor windows (NativeSynthEditorOverlay, NativeEffectEditorOverlay) ----
    // The panel zoom chips in a device window's title bar (ADR-0103): the
    // steps, a chip's least width and the rounding a chosen step absorbs.
    readonly property var panelZoomSteps: [0.9, 1, 1.2, 1.4]
    readonly property int panelZoomChipWidth: 44
    readonly property real panelZoomTolerance: 0.001
    // Docked, a device panel spans this share of the window's height.
    readonly property real editorDockShare: 0.6
    // The window's chrome around a panel: the frame's insets, and its title
    // bar with the DOCK and AUTOMATION row under the panel; docked, the side
    // room the panel leaves.
    readonly property int editorChromeWidth: 24
    readonly property int editorChromeHeight: 84
    readonly property int editorDockChromeWidth: 64
    // An unresized floating window: the room it leaves above and below, and
    // the most a fixed-size panel is drawn at.
    readonly property int editorFloatingMarginHeight: 170
    readonly property real editorFloatingScale: 0.82
    readonly property int editorMinimumWidth: 480
    readonly property int editorMinimumHeight: 360
    // Where a window opens: floating, from the top; docked, its inset and
    // the room left for the transport bar below it.
    readonly property int editorFloatingTop: 76
    readonly property int editorDockedInset: 12
    readonly property int editorDockedBottomGap: 34
    // The DOCK and AUTOMATION buttons, and the room between them.
    readonly property int editorButtonHeight: 30
    readonly property int editorDockButtonWidth: 76
    readonly property int editorAutomationButtonWidth: 112
    readonly property int editorButtonGap: 120
    readonly property real editorButtonRadius: 5
    // The automation drawer and its inset from the window's top and bottom.
    readonly property int editorDrawerWidth: 300
    readonly property int editorDrawerMaxHeight: 540
    // The effect window's sidechain strip.
    readonly property int editorSidechainHeight: 30
    readonly property int editorSidechainRightGap: 200
    // The effect window's own button colours (the instrument window takes
    // the frame's).
    readonly property color editorButton: "#293342"
    readonly property color editorButtonHover: "#35556a"
    readonly property color editorButtonEdge: "#738397"
    readonly property color editorButtonInk: "#e9eef5"

    // ---- Crash report notice (CrashReportNotice) ----
    // A card over the shell after a crash: the export dialog's scrim and
    // card shape, a fixed width, and a row of buttons under the report path.
    readonly property int crashNoticeWidth: 520
    readonly property int crashNoticeZ: 60
    readonly property int crashNoticeButtonHeight: 30
    readonly property int crashNoticePathPadding: 8
}
