pragma ComponentBehavior: Bound
import QtQuick
import "arrange"
import "mixcheck"
import "devicekit" as Kit
import "shell"

// Arrange from a loop (ADR-0080):
// a whole DJ-friendly track proposed from the loop brace's 8 or 16 bars.
// Top to bottom: the loop and template, the options with Regenerate, Bin
// and Keep, the roles found (each with a lock and a role to correct), the
// proposal on a timeline with its automation and the 16-bar rule, the
// rules it keeps and every decision in words. Listen applies the proposal
// as one undo step so the transport plays it; Bin takes it back and Keep
// leaves it. `controller` is projectBridge.arrange.
Rectangle {
    id: view
    objectName: "arrangeLoopView"
    required property var controller
    required property var bridge

    readonly property ArrangeTheme style: ArrangeTheme {}
    readonly property var ac: controller !== undefined ? controller : null
    readonly property bool ready: ac !== null && ac.ready === true
    readonly property bool listening: ac !== null && ac.listening === true
    readonly property var tracks: style.list(ac !== null ? ac.tracks : null)
    readonly property var sections: style.list(ac !== null ? ac.sections : null)

    function sectionAt(bar) {
        for (let i = 0; i < sections.length; ++i) {
            const section = sections[i]
            if (bar >= section.start && bar < section.start + section.bars)
                return section
        }
        return null
    }
    // Hears the proposal from a bar (applying it first).
    function playFrom(bar) {
        if (ac === null)
            return
        if (!listening)
            ac.listen(true)
        if (!listening)
            return
        bridge.seek_to_bar(Math.max(0, bar))
        if (!bridge.playbackActive)
            bridge.toggle_playback()
    }

    Component.onCompleted: if (ac !== null) ac.open()
    Component.onDestruction: if (ac !== null) ac.close()

    color: style.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: "Arrange from a loop"

    Flickable {
        id: scroller
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 2 * view.style.space2xl
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            x: Math.max(view.style.space2xl, (parent.width - width) / 2)
            y: view.style.space2xl
            width: Math.min(scroller.width - 2 * view.style.space2xl, view.style.maxContentWidth)
            spacing: view.style.gap

            // Title and what it does.
            Column {
                width: parent.width
                spacing: view.style.spaceSm
                Text {
                    text: "Arrange from a loop"
                    color: view.style.textMain
                    font.family: view.style.sansFamily
                    font.pixelSize: view.style.arrangeTitleSize
                    font.weight: Font.ExtraBold
                    font.letterSpacing: view.style.arrangeTitleSpacing
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    textFormat: Text.StyledText
                    lineHeight: view.style.introLineHeight
                    text: "A whole, DJ-friendly track from bars <b>" + (view.ac ? view.ac.loopStartBar : 1) + "–"
                          + (view.ac ? view.ac.loopStartBar + view.ac.loopBars - 1 : 16) + "</b>. Each track's role "
                          + "comes from its name, instrument and notes; sections land on 8-bar lines with something "
                          + "new every 16 bars; the moves are written as automation (filter sweeps, a high-pass "
                          + "before the drop, send throws, Chop fills); the intro and outro are beats only, for "
                          + "mixing. <b>It is one change: Keep leaves it as a single undo step, Bin leaves the "
                          + "project as it was.</b>"
                    color: view.style.textDim
                    font.family: view.style.sansFamily
                    font.pixelSize: view.style.typeEmphasis
                }
            }

            // Loop, template, listen.
            Flow {
                width: parent.width
                spacing: view.style.toolbarSpacing
                Text {
                    height: view.style.segmentedHeight
                    verticalAlignment: Text.AlignVCenter
                    text: "LOOP"
                    color: view.style.textFaint
                    font.family: view.style.monoFamily
                    font.pixelSize: view.style.typeSmall
                    font.weight: Font.Bold
                }
                Kit.SegmentedControl {
                    theme: view.style
                    namePrefix: "arrangeLoop"
                    accessibleName: "Loop length"
                    options: view.style.list(view.ac ? view.ac.loopChoices : null).map(
                                 bars => ({ id: String(bars), label: bars + " bars", tip: "Use " + bars + " bars of the loop" }))
                    current: view.ac ? String(view.ac.loopBars) : ""
                    onPicked: id => view.ac.set_loop_bars(Number(id))
                }
                Text {
                    height: view.style.segmentedHeight
                    verticalAlignment: Text.AlignVCenter
                    text: "TEMPLATE"
                    color: view.style.textFaint
                    font.family: view.style.monoFamily
                    font.pixelSize: view.style.typeSmall
                    font.weight: Font.Bold
                }
                Kit.SegmentedControl {
                    theme: view.style
                    namePrefix: "arrangeTemplate"
                    accessibleName: "Template"
                    options: view.style.list(view.ac ? view.ac.templates : null).map(
                                 t => ({ id: String(t.id), label: t.name + " " + t.length, tip: t.name }))
                    current: view.ac ? String(view.ac.style) : ""
                    onPicked: id => view.ac.set_style(Number(id))
                }
                Item { width: view.style.spaceLg; height: 1 }
                Kit.Button {
                    objectName: "arrangeListenButton"
                    theme: view.style
                    prime: !view.listening
                    done: view.listening
                    enabled: view.ready
                    label: view.listening ? "B · proposal ■" : "▶ Listen"
                    tip: view.listening ? "Back to the loop as it was" : "Apply the proposal to hear it; Bin takes it back"
                    onClicked: {
                        if (view.listening) {
                            if (view.bridge.playbackActive)
                                view.bridge.stop_playback()
                            view.ac.listen(false)
                        } else {
                            view.playFrom(0)
                        }
                    }
                }
            }

            // Options and the decision.
            Flow {
                width: parent.width
                spacing: view.style.optionsSpacing
                Kit.Slider {
                    objectName: "arrangeLength"
                    width: view.style.lengthSliderWidth
                    theme: view.style
                    commitOnRelease: true
                    label: "Length"
                    readout: view.ac ? view.ac.lengthBars + " bars · " + view.ac.lengthText : ""
                    tip: "How long the track runs, on the 8-bar grid"
                    from: view.ac ? view.ac.minLengthBars : 0
                    to: view.ac ? view.ac.maxLengthBars : 1
                    step: view.ac ? view.ac.phraseBars : 8
                    value: view.ac ? view.ac.lengthBars : 0
                    accent: view.style.blue
                    onMoved: value => view.ac.set_length_bars(Math.round(value))
                }
                Column {
                    spacing: view.style.spaceSm
                    Text {
                        text: "INTRO / OUTRO · BEATS ONLY"
                        color: view.style.textFaint
                        font.family: view.style.monoFamily
                        font.pixelSize: view.style.sliderLabelSize
                        font.weight: Font.Bold
                        font.letterSpacing: view.style.arrangeCaptionSpacing
                    }
                    Kit.SegmentedControl {
                        theme: view.style
                        namePrefix: "arrangeEdge"
                        accessibleName: "Intro and outro length"
                        options: view.style.list(view.ac ? view.ac.edgeChoices : null).map(
                                     bars => ({ id: String(bars), label: String(bars), tip: bars + " bars of beats at each end" }))
                        current: view.ac ? String(view.ac.edgeBars) : ""
                        onPicked: id => view.ac.set_edge_bars(Number(id))
                    }
                }
                Kit.Slider {
                    objectName: "arrangeBreakdown"
                    width: view.style.breakdownSliderWidth
                    theme: view.style
                    commitOnRelease: true
                    label: "Breakdown at"
                    readout: view.ac ? Math.round(view.ac.breakdown * 100) + "%" : ""
                    tip: "Where the break sits between the build and the outro"
                    value: view.ac ? view.ac.breakdown : 0
                    accent: view.style.blue
                    onMoved: value => view.ac.set_breakdown(value)
                }
                Kit.Slider {
                    objectName: "arrangeVariation"
                    width: view.style.variationSliderWidth
                    theme: view.style
                    commitOnRelease: true
                    label: "Variation"
                    readout: view.ac ? Math.round(view.ac.variation * 100) + "%" : ""
                    tip: "How many fills and drop-outs it adds of its own"
                    value: view.ac ? view.ac.variation : 0
                    accent: view.style.blue
                    onMoved: value => view.ac.set_variation(value)
                }
                Kit.Button {
                    objectName: "arrangeTransitions"
                    theme: view.style
                    done: view.ac !== null && view.ac.transitions === true
                    label: (view.ac !== null && view.ac.transitions ? "✓ " : "") + "Add transitions"
                    tip: "Risers, a downlifter and impacts on new Nova tracks"
                    onClicked: view.ac.set_transitions(!view.ac.transitions)
                }
            }
            Flow {
                width: parent.width
                spacing: view.style.toolbarSpacing
                Kit.Button {
                    objectName: "arrangeRegenerate"
                    theme: view.style
                    label: "↻ Regenerate"
                    tip: "Another seed: other fills and drop-outs, the same rules"
                    onClicked: view.ac.regenerate()
                }
                Text {
                    height: view.style.pushButtonHeight
                    verticalAlignment: Text.AlignVCenter
                    text: view.ac ? "seed " + view.ac.seed : ""
                    color: view.style.textDim
                    font.family: view.style.monoFamily
                    font.pixelSize: view.style.typeLabel
                }
                Item { width: view.style.space2xl; height: 1 }
                Kit.Button {
                    objectName: "arrangeBin"
                    theme: view.style
                    label: "Bin"
                    tip: "Leave the project as it was"
                    onClicked: view.ac.bin()
                }
                Kit.Button {
                    objectName: "arrangeKeep"
                    theme: view.style
                    prime: true
                    enabled: view.ready
                    label: "Keep"
                    tip: "Write it into the project as one undo step"
                    onClicked: view.ac.keep()
                }
                Text {
                    height: view.style.pushButtonHeight
                    width: Math.min(implicitWidth, view.style.statusMaxWidth)
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: view.ac ? (String(view.ac.error).length > 0 ? view.ac.error : view.ac.status) : ""
                    color: view.ac && String(view.ac.error).length > 0 ? view.style.red : view.style.textDim
                    font.family: view.style.sansFamily
                    font.pixelSize: view.style.typeLabel
                }
            }

            // Roles found.
            MixCheckCard {
                width: parent.width
                style: view.style
                Text {
                    text: "Roles found in the loop"
                    color: view.style.textMain
                    font.family: view.style.sansFamily
                    font.pixelSize: view.style.typeSubtitle
                    font.weight: Font.Bold
                }
                Grid {
                    id: roleGrid
                    width: parent.width
                    columns: Math.max(1, Math.floor((width + spacing) / (view.style.roleCardMinWidth + spacing)))
                    spacing: view.style.spaceSm
                    Repeater {
                        model: view.tracks
                        delegate: ArrangeRoleCard {
                            required property var modelData
                            width: (roleGrid.width - (roleGrid.columns - 1) * roleGrid.spacing) / roleGrid.columns
                            style: view.style
                            track: modelData
                            roleNames: view.ac ? view.ac.roleNames : []
                            onLockToggled: id => view.ac.toggle_lock(id)
                            onRolePicked: (id, role) => view.ac.set_role(id, role)
                        }
                    }
                }
            }

            // The proposal.
            MixCheckCard {
                width: parent.width
                style: view.style
                visible: view.ready
                Item {
                    width: parent.width
                    height: view.style.proposalHeaderHeight
                    Text {
                        text: "The proposal"
                        color: view.style.textMain
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeSubtitle
                        font.weight: Font.Bold
                    }
                    Text {
                        anchors.right: parent.right
                        readonly property real bar: view.listening && view.bridge.playbackActive
                                                    ? view.bridge.playheadBar : -1
                        readonly property var section: bar >= 0 ? view.sectionAt(Math.floor(bar)) : null
                        text: section ? "bar " + (Math.floor(bar) + 1) + " · " + section.name
                                      : view.ac ? view.ac.lengthBars + " bars · " + view.ac.lengthText
                                                  + (view.ac.bpm > 0 ? " at " + Math.round(view.ac.bpm) + " BPM" : "")
                                                : ""
                        color: view.style.textDim
                        font.family: view.style.monoFamily
                        font.pixelSize: view.style.typeLabel
                    }
                }
                ArrangeTimeline {
                    objectName: "arrangeTimeline"
                    width: parent.width
                    height: implicitHeight
                    style: view.style
                    sections: view.sections
                    tracks: view.tracks
                    lanes: view.style.list(view.ac ? view.ac.lanes : null)
                    checks: view.style.list(view.ac ? view.ac.checks : null)
                    lengthBars: view.ac ? view.ac.lengthBars : 0
                    phraseBars: view.ac ? view.ac.phraseBars : 8
                    changeBars: view.ac ? view.ac.changeBars : 16
                    playheadBar: view.listening && view.bridge.playbackActive ? view.bridge.playheadBar : -1
                    onEdgeMoved: (index, bar) => view.ac.move_section_edge(index, bar)
                    onPlayRequested: bar => view.playFrom(bar)
                }
            }

            // The rules it keeps.
            Flow {
                width: parent.width
                spacing: view.style.spaceSm
                visible: view.ready
                Repeater {
                    model: view.style.list(view.ac ? view.ac.rules : null)
                    delegate: Rectangle {
                        id: rule
                        required property var modelData
                        width: ruleRow.implicitWidth + view.style.ruleChipPad
                        height: view.style.ruleChipHeight
                        radius: height / 2
                        color: view.style.panelRaised
                        border.color: view.style.line
                        Row {
                            id: ruleRow
                            anchors.centerIn: parent
                            spacing: view.style.ruleChipSpacing
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: view.style.ruleDotSize
                                height: view.style.ruleDotSize
                                radius: view.style.ruleDotRadius
                                color: rule.modelData.holds ? view.style.accent : view.style.red
                            }
                            Text {
                                text: rule.modelData.text
                                color: view.style.textDim
                                font.family: view.style.sansFamily
                                font.pixelSize: view.style.ruleTextSize
                            }
                        }
                    }
                }
            }

            // Every decision, in words.
            MixCheckCard {
                width: parent.width
                style: view.style
                visible: view.ready
                Text {
                    text: "Why, bar by bar"
                    color: view.style.textMain
                    font.family: view.style.sansFamily
                    font.pixelSize: view.style.typeSubtitle
                    font.weight: Font.Bold
                }
                Repeater {
                    model: view.style.list(view.ac ? view.ac.decisions : null)
                    delegate: Row {
                        id: decision
                        required property var modelData
                        spacing: view.style.decisionSpacing
                        Text {
                            width: view.style.decisionBarWidth
                            text: "bar " + (decision.modelData.bar + 1)
                            color: view.style.textFaint
                            font.family: view.style.monoFamily
                            font.pixelSize: view.style.typeBody
                        }
                        Text {
                            text: decision.modelData.text
                            color: view.style.textDim
                            font.family: view.style.sansFamily
                            font.pixelSize: view.style.typeLabel
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: note.implicitHeight + 2 * view.style.noteInsetY
                radius: view.style.cardRadius
                color: view.style.panel
                Rectangle {
                    width: view.style.noteBarWidth
                    height: parent.height
                    color: view.style.blue
                }
                Text {
                    id: note
                    x: view.style.spaceXl
                    y: view.style.noteInsetY
                    width: parent.width - view.style.spaceXl - view.style.noteTrailing
                    wrapMode: Text.WordWrap
                    textFormat: Text.StyledText
                    lineHeight: view.style.noteLineHeight
                    text: "<b>A deterministic rule engine for now.</b> The same loop, template and seed always "
                          + "give the same arrangement, and every rule is listed above. Later, an agent uses the "
                          + "same tools (the roles and the beat grid it reads, the session commands it writes "
                          + "with) and its proposals arrive the same way: one change you keep or bin."
                    color: view.style.textDim
                    font.family: view.style.sansFamily
                    font.pixelSize: view.style.typeLabel
                }
            }
        }
    }
}
