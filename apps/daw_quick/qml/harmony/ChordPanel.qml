pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "../shell"

// The piano roll's chord tools (ADR-0084),
// beside the notes. Write fills the clip from a mood and a shape; Chord
// changes the chord selected in the strip, its notes re-voiced in their
// rhythm; Rhythm turns held chords into a part, as notes (Apply) or played
// live by a Comp on the track. Every action is one undo step.
// `controller` is projectBridge.harmony, scoped to the roll's clip.
Rectangle {
    id: panel
    objectName: "chordPanel"
    required property var controller
    readonly property HarmonyTheme style: HarmonyTheme {}
    readonly property var hc: controller !== undefined ? controller : null
    readonly property var chosen: style.map(hc ? hc.selection : null)
    readonly property bool hasChords: hc !== null && hc.count > 0
    property int tab: 0
    signal closeRequested()

    color: style.bg
    border.color: style.line
    Accessible.role: Accessible.Pane
    Accessible.name: "Chord tools: write, chord and rhythm"
    // Clicks and wheels stay in the panel.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

    component Label: Text {
        color: panel.style.faint
        font.family: panel.style.mono
        font.pixelSize: panel.style.labelSize
        font.weight: Font.DemiBold
        font.letterSpacing: panel.style.labelSpacing
    }
    component Note: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.WordWrap
        color: panel.style.faint
        font.family: panel.style.sans
        font.pixelSize: panel.style.typeBody
        lineHeight: panel.style.noteLineHeight
    }
    component Tag: Rectangle {
        property string text: ""
        property color ink: panel.style.dim
        width: tagText.implicitWidth + panel.style.spaceLg
        height: panel.style.tagHeight
        radius: panel.style.radiusMd
        color: panel.style.alpha(ink, panel.style.tagFillAlpha)
        border.color: panel.style.alpha(ink, panel.style.tagEdgeAlpha)
        Text {
            id: tagText
            anchors.centerIn: parent
            text: parent.text
            color: parent.ink
            font.family: panel.style.mono
            font.pixelSize: panel.style.typeSmall
            font.weight: Font.DemiBold
        }
    }
    // A small grid button: a root or a quality.
    component Pick: Rectangle {
        id: pick
        property string text: ""
        property bool lit: false
        property string tip: text
        signal picked()
        width: panel.style.pickWidth
        height: panel.style.pickHeight
        radius: panel.style.pickRadius
        color: lit ? panel.style.violet : pickArea.containsMouse ? panel.style.panel3 : panel.style.panel2
        border.color: activeFocus ? panel.style.ink : lit ? panel.style.violet : panel.style.line2
        activeFocusOnTab: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: tip
        Accessible.checked: lit
        Keys.onReturnPressed: picked()
        Keys.onSpacePressed: picked()
        Text {
            anchors.centerIn: parent
            text: pick.text
            color: pick.lit ? panel.style.violetText : panel.style.ink
            font.family: panel.style.mono
            font.pixelSize: panel.style.typeBody
            font.weight: Font.DemiBold
        }
        MouseArea { id: pickArea; anchors.fill: parent; hoverEnabled: true; onClicked: pick.picked() }
    }

    // A 0-100% amount: drag or click along it, arrows step by 5%.
    component Amount: Column {
        id: amount
        property string label: ""
        property string help: label
        property real value: 0
        property color tint: panel.style.amber
        signal moved(real amount)
        signal released()
        readonly property real step: 0.05
        width: parent ? parent.width : 0
        spacing: panel.style.itemGap
        Label { text: amount.label + " " + Math.round(amount.value * 100) + "%" }
        Rectangle {
            width: amount.width
            height: panel.style.amountHeight
            radius: height / 2
            color: panel.style.well
            border.color: activeFocus ? panel.style.ink : panel.style.line2
            activeFocusOnTab: true
            Accessible.role: Accessible.Slider
            Accessible.name: amount.help
            Keys.onLeftPressed: amount.moved(Math.max(0, amount.value - amount.step))
            Keys.onRightPressed: amount.moved(Math.min(1, amount.value + amount.step))
            Keys.onReleased: event => {
                if (event.key === Qt.Key_Left || event.key === Qt.Key_Right)
                    amount.released()
            }
            Rectangle {
                width: Math.max(height, parent.width * amount.value)
                height: parent.height
                radius: height / 2
                color: panel.style.alpha(amount.tint, panel.style.amountFillAlpha)
            }
            MouseArea {
                anchors.fill: parent
                onReleased: amount.released()
                function pick(x) {
                    amount.moved(Math.max(0, Math.min(1, Math.round(x / width / amount.step) * amount.step)))
                }
                onPressed: mouse => pick(mouse.x)
                onPositionChanged: mouse => { if (pressed) pick(mouse.x) }
            }
        }
    }

    property bool restruckOpen: false
    // The chosen rhythm strikes the chord again (a Chord Comp pattern).
    readonly property bool restruckChosen: hc !== null && hc.rhythmPatterns.length > hc.rhythmPattern
                                           && hc.rhythmPatterns[hc.rhythmPattern].restruck === true
    component PatternCard: Rectangle {
                    id: pattern
                    required property var modelData
                    readonly property bool lit: panel.hc && panel.hc.rhythmPattern === modelData.index
                    width: scroller.width
                    height: panel.style.patternCardHeight
                    radius: panel.style.radius
                    color: lit ? panel.style.alpha(panel.style.amber, panel.style.cardLitAlpha) : panel.style.panel2
                    border.color: activeFocus ? panel.style.ink : lit ? panel.style.amber
                        : patternArea.containsMouse ? panel.style.faint : panel.style.line
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name + ": " + modelData.summary
                    Accessible.checked: lit
                    Keys.onReturnPressed: panel.hc.set_rhythm_pattern(modelData.index)
                    Keys.onSpacePressed: panel.hc.set_rhythm_pattern(modelData.index)
                    Text {
                        x: panel.style.patternTextX; y: panel.style.patternTitleY
                        text: pattern.modelData.name
                        color: panel.style.ink
                        font.family: panel.style.sans
                        font.pixelSize: panel.style.typeLabel
                        font.weight: Font.DemiBold
                    }
                    Text {
                        x: panel.style.patternTextX; y: panel.style.patternSummaryY
                        width: parent.width - 2 * panel.style.patternTextX
                        elide: Text.ElideRight
                        text: pattern.modelData.summary
                        color: panel.style.faint
                        font.family: panel.style.mono
                        font.pixelSize: panel.style.typeCaption
                    }
                    MouseArea {
                        id: patternArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: panel.hc.set_rhythm_pattern(pattern.modelData.index)
                    }
                }

    // ---- Tabs ---------------------------------------------------------------------------
    Item {
        id: header
        x: panel.style.pad
        y: panel.style.spaceMd
        width: panel.width - 2 * panel.style.pad
        height: panel.style.tabBarHeight
        Kit.SegmentedControl {
            objectName: "chordPanelTabs"
            theme: panel.style
            accessibleName: "Chord tools"
            labels: ["Write", "Chord", "Rhythm"]
            currentIndex: panel.tab
            onChosen: index => panel.tab = index
        }
        Kit.Button {
            anchors.right: parent.right
            theme: panel.style
            small: true
            label: "×"
            tip: "Close the chord tools"
            onClicked: panel.closeRequested()
        }
    }

    Flickable {
        id: scroller
        x: panel.style.pad
        y: header.y + header.height + panel.style.groupGap
        width: panel.width - 2 * panel.style.pad
        height: panel.height - y - panel.style.spaceSm
        contentWidth: width
        contentHeight: (panel.tab === 0 ? write : panel.tab === 1 ? chord : rhythm).height + panel.style.spaceLg
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // ---- Write: the Progression Lab --------------------------------------------------
        Column {
            id: write
            objectName: "chordPanelWrite"
            visible: panel.tab === 0
            width: scroller.width
            spacing: panel.style.spaceLg
            Row {
                width: parent.width
                spacing: panel.style.groupGap
                MoodPad {
                    width: Math.min(panel.style.moodPadMaxWidth, parent.width * panel.style.moodPadShare)
                    style: panel.style
                    controller: panel.hc
                }
                Column {
                    width: parent.width - x
                    spacing: panel.style.spaceSm
                    Text {
                        text: panel.hc ? panel.hc.feel : ""
                        color: panel.style.violet
                        font.family: panel.style.mono
                        font.pixelSize: panel.style.typeLabel
                        font.weight: Font.Bold
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: panel.hc ? panel.hc.keyName.split(" ")[0] + " " + panel.hc.modeName.toLowerCase()
                                         + " · " + panel.hc.motion : ""
                        color: panel.style.ink
                        font.family: panel.style.mono
                        font.pixelSize: panel.style.typeBody
                    }
                    Note { text: "Across: dark to bright. Up: still to restless. Drag to hear it change." }
                }
            }
            Kit.SegmentedControl {
                visible: panel.hc !== null && panel.hc.hasKey
                theme: panel.style
                accessibleName: "Modes the mood pad offers: the song key's, or all of them"
                labels: panel.hc ? [panel.hc.keyName + " modes", "All modes"] : []
                currentIndex: panel.hc && panel.hc.inKey ? 0 : 1
                onChosen: index => panel.hc.inKey = index === 0
            }
            Amount {
                objectName: "chordHarmonyComplexity"
                label: "COMPLEXITY"
                tint: panel.style.violet
                help: "Complexity: plain triads changing every two bars, up to lush chords changing every half bar with suspensions, a walking bass and passing chords"
                value: panel.hc ? panel.hc.harmonyComplexity : 0
                onMoved: amount => panel.hc.set_harmony_complexity(amount)
                onReleased: panel.hc.finish_harmony_complexity()
            }
            Label { text: "SHAPE" }
            Grid {
                columns: 2
                spacing: panel.style.itemGap
                Repeater {
                    model: panel.hc ? panel.hc.shapes : []
                    delegate: Rectangle {
                        id: shapeCard
                        required property var modelData
                        readonly property bool lit: panel.hc && panel.hc.shape === modelData.index
                        width: (write.width - panel.style.itemGap) / 2
                        height: panel.style.shapeCardHeight
                        radius: panel.style.radius
                        color: lit ? panel.style.alpha(panel.style.violet, panel.style.cardLitAlpha) : panel.style.panel2
                        border.color: activeFocus ? panel.style.ink : lit ? panel.style.violet
                            : shapeArea.containsMouse ? panel.style.faint : panel.style.line
                        activeFocusOnTab: true
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.name + ": " + modelData.summary
                        Accessible.checked: lit
                        Keys.onReturnPressed: panel.hc.set_shape(modelData.index)
                        Keys.onSpacePressed: panel.hc.set_shape(modelData.index)
                        Canvas {
                            id: glyph
                            // The contour spans the scale's degrees, inset a pixel.
                            readonly property real inset: 1
                            readonly property real topDegree: 6
                            x: parent.width - width - panel.style.shapeCardInset; y: panel.style.shapeCardInset
                            width: panel.style.shapeGlyphWidth
                            height: panel.style.shapeGlyphHeight
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                const d = shapeCard.modelData.degrees.concat([shapeCard.modelData.degrees[0]])
                                ctx.strokeStyle = shapeCard.lit ? panel.style.violet : panel.style.dim
                                ctx.lineWidth = panel.style.shapeGlyphStroke
                                ctx.lineJoin = "round"
                                ctx.beginPath()
                                d.forEach((deg, i) => {
                                    const x = inset + i / (d.length - 1) * (width - 2 * inset)
                                    const y = height - inset - deg / topDegree * (height - 2 * inset)
                                    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                                })
                                ctx.stroke()
                            }
                            Connections {
                                target: shapeCard
                                function onLitChanged() { glyph.requestPaint() }
                            }
                        }
                        Text {
                            x: panel.style.shapeCardInset; y: panel.style.spaceSm
                            text: shapeCard.modelData.name
                            color: panel.style.ink
                            font.family: panel.style.sans
                            font.pixelSize: panel.style.typeLabel
                            font.weight: Font.DemiBold
                        }
                        Text {
                            x: panel.style.shapeCardInset; y: panel.style.shapeSummaryY
                            width: parent.width - 2 * panel.style.shapeCardInset
                            elide: Text.ElideRight
                            text: shapeCard.modelData.summary
                            color: panel.style.faint
                            font.family: panel.style.mono
                            font.pixelSize: panel.style.typeCaption
                        }
                        MouseArea {
                            id: shapeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: panel.hc.set_shape(shapeCard.modelData.index)
                        }
                    }
                }
            }
            Flow {
                width: parent.width
                spacing: panel.style.groupGap
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "COLOUR" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accessibleName: "Colour"
                        labels: ["Natural", "Triads", "Add9", "Sus2", "7ths", "Lush"]
                        currentIndex: panel.hc ? panel.hc.colour : 0
                        onChosen: index => panel.hc.set_colour(index)
                    }
                }
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "CHORD EVERY" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accessibleName: "Chord every"
                        readonly property var beats: panel.hc ? [8, 4, 2] : []
                        labels: ["2 bars", "Bar", "½"]
                        currentIndex: panel.hc ? Math.max(0, beats.indexOf(panel.hc.paceBeats)) : 1
                        onChosen: index => panel.hc.set_pace_beats(beats[index])
                    }
                }
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "BASS" }
                    Kit.SegmentedControl {
                        objectName: "chordBass"
                        theme: panel.style
                        accessibleName: "Bass: the low bass note under each chord, or the chords alone"
                        labels: ["On", "Off"]
                        currentIndex: panel.hc && !panel.hc.bass ? 1 : 0
                        onChosen: index => panel.hc.set_bass(index === 0)
                    }
                }
            }
            Label { text: "DEVICES" }
            Flow {
                width: parent.width
                spacing: panel.style.itemGap
                Repeater {
                    model: panel.hc ? panel.hc.devices : []
                    delegate: Kit.Button {
                        required property var modelData
                        theme: panel.style
                        small: true
                        on: modelData.on
                        label: modelData.name
                        tip: modelData.name + ": " + modelData.hint
                        onClicked: panel.hc.toggle_device(modelData.index)
                    }
                }
            }
            Item {
                width: parent.width
                height: panel.style.sectionHeaderHeight
                Label { anchors.verticalCenter: parent.verticalCenter; text: "SUGGESTIONS" }
                Kit.Button {
                    anchors.right: parent.right
                    theme: panel.style
                    small: true
                    label: "⚄ MORE"
                    tip: "Another set of suggestions"
                    onClicked: panel.hc.more()
                }
            }
            Repeater {
                model: panel.hc ? panel.hc.suggestions : []
                delegate: Rectangle {
                    id: suggestion
                    required property var modelData
                    readonly property bool lit: panel.hc && panel.hc.suggestion === modelData.index
                    objectName: "chordSuggestion" + modelData.index
                    width: write.width
                    height: suggestionText.implicitHeight + 2 * panel.style.suggestionPadY
                    radius: panel.style.radiusXl
                    color: lit ? panel.style.alpha(panel.style.violet, panel.style.cardLitAlpha) : panel.style.panel2
                    border.color: activeFocus ? panel.style.ink : lit ? panel.style.violet
                        : suggestionArea.containsMouse ? panel.style.faint : panel.style.line
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Write " + modelData.title + " into the clip: " + modelData.chords
                    Keys.onReturnPressed: panel.hc.choose_suggestion(modelData.index)
                    Keys.onSpacePressed: panel.hc.choose_suggestion(modelData.index)
                    Column {
                        id: suggestionText
                        x: panel.style.suggestionPadX; y: panel.style.suggestionPadY
                        width: parent.width - 2 * panel.style.suggestionPadX
                        spacing: panel.style.spaceXxs
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: suggestion.modelData.title + "  ·  "
                                  + String(suggestion.modelData.label).split(" · ").slice(1).join(" · ")
                            color: panel.style.faint
                            font.family: panel.style.mono
                            font.pixelSize: panel.style.typeCaption
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: suggestion.modelData.chords
                            color: panel.style.ink
                            font.family: panel.style.mono
                            font.pixelSize: panel.style.typeBody
                        }
                    }
                    MouseArea {
                        id: suggestionArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: panel.hc.choose_suggestion(suggestion.modelData.index)
                    }
                }
            }
            Note { text: "A suggestion fills the clip. Chords already in a rhythm keep it; Undo brings the old ones back." }
        }

        // ---- Chord: the selected chord ----------------------------------------------------
        Column {
            id: chord
            objectName: "chordPanelChord"
            visible: panel.tab === 1
            width: scroller.width
            spacing: panel.style.groupGap
            Note {
                visible: !panel.hasChords
                text: "No chords in this clip yet. Write some, or play two or more notes together: the strip names them."
            }
            Column {
                visible: panel.hasChords
                width: parent.width
                spacing: panel.style.groupGap
                Row {
                    spacing: panel.style.groupGap
                    Text {
                        id: chordName
                        objectName: "chordPanelName"
                        text: panel.chosen.name || ""
                        color: panel.style.ink
                        font.family: panel.style.sans
                        font.pixelSize: panel.style.chordNameSize
                        font.weight: Font.ExtraBold
                    }
                    Text {
                        anchors.baseline: chordName.baseline
                        text: panel.chosen.roman || ""
                        color: panel.style.violet
                        font.family: panel.style.mono
                        font.pixelSize: panel.style.typeEmphasis
                        font.weight: Font.DemiBold
                    }
                }
                Flow {
                    width: parent.width
                    spacing: panel.style.itemGap
                    Tag { text: panel.chosen.role || ""; ink: panel.style.violet }
                    Tag { text: "tension " + (panel.chosen.tension || 0); ink: panel.style.tensionColor(panel.chosen.tension) }
                    Tag { text: panel.chosen.inKey ? "in " + (panel.hc ? panel.hc.keyName : "") : "outside the key" }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: (panel.chosen.voices || []).join("  ")
                    color: panel.style.dim
                    font.family: panel.style.mono
                    font.pixelSize: panel.style.typeSmall
                }
                Label { text: "ROOT" }
                Grid {
                    id: rootGrid
                    columns: 6
                    spacing: panel.style.spaceXs
                    Repeater {
                        model: panel.hc ? panel.hc.rootNames : []
                        delegate: Pick {
                            required property string modelData
                            required property int index
                            width: (chord.width - (rootGrid.columns - 1) * rootGrid.spacing) / rootGrid.columns
                            text: modelData
                            tip: "Root " + modelData
                            lit: panel.chosen.root === index
                            onPicked: panel.hc.set_root(index)
                        }
                    }
                }
                Label { text: "QUALITY" }
                Grid {
                    id: qualityGrid
                    columns: 5
                    spacing: panel.style.spaceXs
                    Repeater {
                        model: panel.hc ? panel.hc.qualities : []
                        delegate: Pick {
                            required property var modelData
                            width: (chord.width - (qualityGrid.columns - 1) * qualityGrid.spacing) / qualityGrid.columns
                            text: String(modelData.suffix).length > 0 ? modelData.suffix : "maj"
                            tip: "Quality " + text
                            lit: panel.chosen.quality === modelData.value
                            onPicked: panel.hc.set_quality(modelData.value)
                        }
                    }
                }
                Label { text: "BASS" }
                Grid {
                    id: bassGrid
                    columns: 7
                    spacing: panel.style.spaceXs
                    Pick {
                        width: (chord.width - (bassGrid.columns - 1) * bassGrid.spacing) / bassGrid.columns
                        text: "root"
                        tip: "The root in the bass"
                        lit: panel.chosen.bass === -1
                        onPicked: panel.hc.set_bass(-1)
                    }
                    Repeater {
                        model: panel.hc ? panel.hc.rootNames : []
                        delegate: Pick {
                            required property string modelData
                            required property int index
                            visible: index !== panel.chosen.root
                            width: (chord.width - (bassGrid.columns - 1) * bassGrid.spacing) / bassGrid.columns
                            text: "/" + modelData
                            tip: modelData + " in the bass"
                            lit: panel.chosen.bass === index
                            onPicked: panel.hc.set_bass(index)
                        }
                    }
                }
                Label { text: "INVERSION" }
                Row {
                    id: inversionRow
                    spacing: panel.style.spaceXs
                    Repeater {
                        id: inversions
                        model: ["Root", "1st", "2nd"]
                        delegate: Pick {
                            required property string modelData
                            required property int index
                            width: (chord.width - (inversions.count - 1) * inversionRow.spacing) / inversions.count
                            text: modelData
                            tip: index === 0 ? "Root position: the root in the bass"
                                 : index === 1 ? "First inversion: the third in the bass"
                                               : "Second inversion: the fifth in the bass"
                            onPicked: panel.hc.set_inversion(index)
                        }
                    }
                }
                Label { text: "VOICING" }
                Kit.SegmentedControl {
                    objectName: "chordVoicingStyle"
                    theme: panel.style
                    accessibleName: "Voicing: re-voices the selected chord. Open is melodic house's: root, fifth, root, third, low and wide"
                    labels: panel.hc ? panel.hc.voicingStyles.map(entry => entry.name) : []
                    currentIndex: panel.hc ? panel.hc.voicingStyles.findIndex(entry => entry.value === panel.hc.voicingStyle) : 0
                    onChosen: index => panel.hc.set_voicing_style(panel.hc.voicingStyles[index].value)
                }
                Kit.Button {
                    theme: panel.style
                    small: true
                    label: "REMOVE CHORD"
                    tip: "Delete this chord's notes"
                    onClicked: panel.hc.remove(panel.hc.selected)
                }

                Label { text: "TRY HERE" }
                Note {
                    visible: panel.hc !== null && panel.hc.hints.length === 0
                    text: "Nothing obvious here. This chord already sits well."
                }
                Repeater {
                    model: panel.hc ? panel.hc.hints : []
                    delegate: Rectangle {
                        id: hint
                        required property var modelData
                        width: chord.width
                        height: hintText.implicitHeight + 2 * panel.style.spaceSm
                        radius: panel.style.radius
                        opacity: modelData.already ? panel.style.hintAppliedOpacity : 1
                        color: panel.style.alpha(panel.style.violet, hintArea.containsMouse ? panel.style.hintHoverAlpha
                                                                                            : panel.style.hintAlpha)
                        border.color: activeFocus ? panel.style.ink : panel.style.alpha(panel.style.violet, panel.style.hintEdgeAlpha)
                        activeFocusOnTab: !modelData.already
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.title + ". " + modelData.why
                        Keys.onReturnPressed: panel.hc.apply_hint(modelData.index)
                        Column {
                            id: hintText
                            x: panel.style.hintPadX; y: panel.style.spaceSm
                            width: parent.width - 2 * panel.style.hintPadX
                            spacing: panel.style.spaceXxs
                            Text {
                                text: hint.modelData.title + (hint.modelData.already ? "  ·  ON" : "  ·  " + hint.modelData.preview)
                                color: panel.style.violet
                                font.family: panel.style.sans
                                font.pixelSize: panel.style.typeLabel
                                font.weight: Font.Bold
                            }
                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                text: hint.modelData.why
                                color: panel.style.dim
                                font.family: panel.style.sans
                                font.pixelSize: panel.style.typeBody
                            }
                        }
                        MouseArea {
                            id: hintArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !hint.modelData.already
                            onClicked: panel.hc.apply_hint(hint.modelData.index)
                        }
                    }
                }

                Label { text: "SUBSTITUTIONS" }
                Flow {
                    width: parent.width
                    spacing: panel.style.itemGap
                    Repeater {
                        model: panel.hc ? panel.hc.substitutions : []
                        delegate: Kit.Button {
                            required property var modelData
                            theme: panel.style
                            small: true
                            label: modelData.preview
                            tip: modelData.title + ": " + modelData.preview
                            onClicked: panel.hc.apply_substitution(modelData.index)
                        }
                    }
                }

                Label { text: "WHAT COMES NEXT" }
                Kit.SegmentedControl {
                    theme: panel.style
                    accessibleName: "Replace the next chord, or split this one and put it in its second half"
                    labels: ["Replace next", "Split in"]
                    currentIndex: panel.hc && panel.hc.insertNext ? 1 : 0
                    onChosen: index => panel.hc.insertNext = index === 1
                }
                NextGraph {
                    width: Math.min(parent.width, panel.style.nextGraphMaxWidth)
                    anchors.horizontalCenter: parent.horizontalCenter
                    style: panel.style
                    controller: panel.hc
                }
            }
        }

        // ---- Rhythm -----------------------------------------------------------------------
        Column {
            id: rhythm
            objectName: "chordPanelRhythm"
            visible: panel.tab === 2
            width: scroller.width
            spacing: panel.style.groupGap
            Note { text: "Melodic house holds its chords: in PML's pack 49 of 52 progressions never re-strike one. The movement comes from changes arriving early, and from the sound." }
            Repeater {
                model: panel.hc ? panel.hc.rhythmPatterns.filter(entry => !entry.restruck) : []
                delegate: PatternCard {}
            }
            Column {
                spacing: panel.style.itemGap
                Label { text: "CHANGES ARRIVE EARLY BY" }
                Kit.SegmentedControl {
                    objectName: "chordAnticipation"
                    theme: panel.style
                    accent: panel.style.amber
                    accentInk: panel.style.amberText
                    accessibleName: "How early each chord change arrives"
                    labels: panel.hc ? panel.hc.anticipationNames : []
                    currentIndex: panel.hc ? panel.hc.anticipation : 0
                    onChosen: index => panel.hc.set_anticipation(index)
                }
            }
            Label { text: "MOVEMENT ON THIS TRACK" }
            Flow {
                width: parent.width
                spacing: panel.style.spaceSm
                Repeater {
                    model: panel.hc ? panel.hc.movements : []
                    delegate: Kit.Button {
                        required property var modelData
                        objectName: "chordMovement" + modelData.index
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        small: true
                        label: "+ " + String(modelData.name).toUpperCase()
                        tip: modelData.name + ": " + modelData.summary + ". Added to the end of this track's effects"
                        onClicked: panel.hc.add_movement(modelData.index)
                    }
                }
            }
            Kit.Button {
                objectName: "chordMoreRhythms"
                theme: panel.style
                small: true
                label: (panel.restruckOpen ? "▾ " : "▸ ") + "MORE · RE-STRUCK PATTERNS"
                tip: "Patterns that strike the chord again in a rhythm, played by a Chord Comp"
                onClicked: panel.restruckOpen = !panel.restruckOpen
            }
            Repeater {
                model: panel.hc && panel.restruckOpen ? panel.hc.rhythmPatterns.filter(entry => entry.restruck) : []
                delegate: PatternCard {}
            }
            Column {
                visible: panel.restruckOpen && panel.restruckChosen
                width: parent.width
                spacing: panel.style.groupGap
            Amount {
                objectName: "chordPushChance"
                label: "PUSH CHANCE"
                help: "Push chance: how often a chord arrives early"
                value: panel.hc ? panel.hc.pushChance : 0
                onMoved: amount => panel.hc.set_push_chance(amount)
            }
            Flow {
                width: parent.width
                spacing: panel.style.groupGap
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "PUSH BY" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        accessibleName: "Push by"
                        labels: panel.hc ? panel.hc.pushNames : []
                        currentIndex: panel.hc ? panel.hc.pushBy : 0
                        onChosen: index => panel.hc.set_push_by(index)
                    }
                }
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "GROOVE" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        accessibleName: "Groove"
                        labels: panel.hc ? panel.hc.grooveNames : []
                        currentIndex: panel.hc ? panel.hc.groove : 0
                        onChosen: index => panel.hc.set_groove(index)
                    }
                }
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "APPLY TO" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        accessibleName: "Apply to the whole clip or the selected chord"
                        labels: ["Whole clip", "Selected chord"]
                        currentIndex: panel.hc && panel.hc.rhythmSelectedOnly ? 1 : 0
                        onChosen: index => panel.hc.rhythmSelectedOnly = index === 1
                    }
                }
            }
            Amount {
                objectName: "chordArticulation"
                label: "ARTICULATION"
                help: "Articulation: every hit shortened towards staccato, or played as written"
                value: panel.hc ? panel.hc.articulation : 1
                onMoved: amount => panel.hc.set_articulation(amount)
            }
            Label { text: "MOTION · KEEPS THE LOOP MOVING" }
            Amount {
                objectName: "chordRhythmComplexity"
                label: "COMPLEXITY"
                help: "Complexity: from the pattern thinned to the 8ths and played the same every bar, up to ghost fills, variations, pushes and a polymeter on top. It sets the controls below"
                value: panel.hc ? panel.hc.rhythmComplexity : 0
                onMoved: amount => panel.hc.set_rhythm_complexity(amount)
            }
            Amount {
                objectName: "chordDensity"
                label: "DENSITY"
                help: "Density: below the middle the pattern thins to the 8ths, above it ghost 16ths fill after hits"
                value: panel.hc ? panel.hc.density : 0
                onMoved: amount => panel.hc.set_density(amount)
            }
            Amount {
                objectName: "chordVariation"
                label: "VARIATION"
                help: "Variation: how often a bar changes a little, a hit dropped or a ghost 16th added"
                value: panel.hc ? panel.hc.variation : 0
                onMoved: amount => panel.hc.set_variation(amount)
            }
            Amount {
                objectName: "chordSwell"
                label: "SWELL"
                help: "Swell: the velocity rises through each phrase and dips at its turn"
                value: panel.hc ? panel.hc.swell : 0
                onMoved: amount => panel.hc.set_swell(amount)
            }
            Flow {
                width: parent.width
                spacing: panel.style.groupGap
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "EVERY" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        accessibleName: "The phrase the variations are measured in"
                        labels: panel.hc ? panel.hc.phraseNames : []
                        currentIndex: panel.hc ? panel.hc.phrase : 0
                        onChosen: index => panel.hc.set_phrase(index)
                    }
                }
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "TOP CYCLE" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        accessibleName: "The top note's cycle against the bar: a bar, or a polymeter of 12 or 7 16ths"
                        labels: panel.hc ? panel.hc.topCycles.map(steps => steps === panel.hc.topCycles[0] ? "Bar" : String(steps)) : []
                        currentIndex: panel.hc ? panel.hc.topCycle : 0
                        onChosen: index => panel.hc.set_top_cycle(index)
                    }
                }
            }
            Flow {
                width: parent.width
                spacing: panel.style.spaceSm
                Kit.Button {
                    objectName: "chordTurnaround"
                    theme: panel.style
                    accent: panel.style.amber
                    accentInk: panel.style.amberText
                    small: true
                    on: panel.hc !== null && panel.hc.turnaround
                    label: "TURNAROUNDS"
                    tip: "Each phrase's last bar turns around: a 16th tucked in, a gap on beat 3, or the next chord early"
                    onClicked: panel.hc.set_turnaround(!on)
                }
                Kit.Button {
                    objectName: "chordDrift"
                    theme: panel.style
                    accent: panel.style.amber
                    accentInk: panel.style.amberText
                    small: true
                    on: panel.hc !== null && panel.hc.drift
                    label: "TOP DRIFT"
                    tip: "For each phrase's second half another chord tone moves to the top"
                    onClicked: panel.hc.set_drift(!on)
                }
                Kit.Button {
                    objectName: "chordLift"
                    theme: panel.style
                    accent: panel.style.amber
                    accentInk: panel.style.amberText
                    small: true
                    on: panel.hc !== null && panel.hc.lift
                    label: "LIFT"
                    tip: "For each phrase's second half the top note is doubled an octave up, brighter"
                    onClicked: panel.hc.set_lift(!on)
                }
            }
            }
            Row {
                spacing: panel.style.spaceMd
                Kit.Button {
                    objectName: "chordRhythmApply"
                    theme: panel.style
                    accent: panel.style.amber
                    accentInk: panel.style.amberText
                    prime: true
                    enabled: panel.hasChords
                    label: "APPLY TO NOTES"
                    tip: "Rewrite the notes in this rhythm (one undo step)"
                    onClicked: panel.hc.apply_rhythm()
                }
                Kit.Button {
                    objectName: "chordRhythmLive"
                    // Held chords play from the clip; live playing is Comp's.
                    visible: panel.restruckChosen || (panel.hc !== null && panel.hc.liveRhythm)
                    theme: panel.style
                    accent: panel.style.amber
                    accentInk: panel.style.amberText
                    on: panel.hc !== null && panel.hc.liveRhythm
                    label: on ? "● PLAYING LIVE" : "PLAY LIVE"
                    tip: on ? "Stop the track's Chord Comp: the clip plays as written"
                            : "A Chord Comp on the track plays this rhythm over the held chords"
                    onClicked: panel.hc.set_live_rhythm(!on)
                }
            }
            Label { text: "LAYERS · NEW TRACKS THAT FOLLOW THESE CHORDS" }
            Flow {
                width: parent.width
                spacing: panel.style.groupGap
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "TONIC RUNS IN" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        accessibleName: "How the running tonic pulses"
                        labels: panel.hc ? panel.hc.tonicPulses : []
                        currentIndex: panel.hc ? panel.hc.tonicPulse : 0
                        onChosen: index => panel.hc.set_tonic_pulse(index)
                    }
                }
                Column {
                    spacing: panel.style.itemGap
                    Label { text: "BASS" }
                    Kit.SegmentedControl {
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        accessibleName: "How the bass layer plays"
                        labels: panel.hc ? panel.hc.bassStyles : []
                        currentIndex: panel.hc ? panel.hc.bassStyle : 0
                        onChosen: index => panel.hc.set_bass_style(index)
                    }
                }
            }
            Flow {
                width: parent.width
                spacing: panel.style.spaceSm
                Repeater {
                    model: panel.hc ? panel.hc.layers : []
                    delegate: Kit.Button {
                        required property var modelData
                        objectName: "chordLayer" + modelData.index
                        theme: panel.style
                        accent: panel.style.amber
                        accentInk: panel.style.amberText
                        small: true
                        enabled: panel.hasChords
                        label: "+ " + String(modelData.name).toUpperCase()
                        tip: modelData.name + ": " + modelData.summary + ". Added on a new track under this one"
                        onClicked: panel.hc.add_layer(modelData.index)
                    }
                }
            }
        }
    }

    Text {
        visible: panel.hc !== null && panel.hc.status.length > 0
        anchors.bottom: parent.bottom
        anchors.bottomMargin: panel.style.spaceSm
        x: panel.style.pad
        width: panel.width - 2 * panel.style.pad
        wrapMode: Text.WordWrap
        text: panel.hc ? panel.hc.status : ""
        color: panel.style.amber
        font.family: panel.style.sans
        font.pixelSize: panel.style.typeBody
    }
}
