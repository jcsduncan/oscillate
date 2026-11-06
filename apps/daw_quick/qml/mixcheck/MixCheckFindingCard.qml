pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "../shell"

// One finding: severity chip and title, the plain
// explanation, the tracks to blame and the band, the evidence, and on the
// right the fix: device, where it goes, its measured settings, Apply and
// Listen, and what to expect (or, once applied, what the re-check found).
Rectangle {
    id: card
    required property MixCheckTheme style
    required property var finding
    // The finding being auditioned (Listen), or "".
    property string listeningId: ""
    property bool highlighted: false
    signal applyRequested(string id)
    signal listenRequested(string id)
    signal stopRequested()

    readonly property string findingId: style.text(finding ? finding.id : "")
    readonly property bool applied: finding ? finding.applied === true : false
    readonly property bool listening: listeningId.length > 0 && listeningId === findingId
    readonly property color severityColour: style.findingColor(finding)
    readonly property var fix: finding && finding.fix ? finding.fix : null
    readonly property bool hasFix: fix !== null
    readonly property var band: finding && finding.band ? finding.band : null
    readonly property var tracks: style.list(finding ? finding.tracks : null)
    readonly property string outcome: {
        const expected = style.text(finding ? finding.expected : "")
        const result = style.text(finding ? finding.result : "")
        if (applied)
            return "Re-checked: " + (result.length > 0 ? result : expected)
        return expected.length > 0 ? "Expected: " + expected : ""
    }

    objectName: "mixCheckFinding:" + findingId
    implicitHeight: Math.max(body.implicitHeight, card.hasFix ? fixBox.implicitHeight : 0) + 2 * style.findingPadding
    radius: style.cardRadius
    color: highlighted ? Qt.lighter(style.panel, style.findingHighlightLift) : style.panel
    border.color: highlighted ? style.lineStrong : style.line
    border.width: highlighted ? style.strokeBold : style.hairline
    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.normal } }
    Accessible.role: Accessible.Grouping
    Accessible.name: style.severityLabel(finding) + ": " + style.text(finding ? finding.title : "")

    // The severity edge.
    Rectangle {
        x: 0
        y: card.style.spaceSm
        width: card.style.severityEdgeWidth
        height: parent.height - 2 * card.style.spaceSm
        radius: card.style.severityEdgeWidth / 2
        color: card.severityColour
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.slow } }
    }

    Column {
        id: body
        x: card.style.findingBodyX
        y: card.style.findingPadding
        width: (card.hasFix ? fixBox.x - card.style.spaceXl : card.width - card.style.findingPadding) - x
        spacing: card.style.spaceMd

        Flow {
            width: parent.width
            spacing: card.style.spaceMd
            Rectangle {
                id: severity
                width: severityText.implicitWidth + card.style.severityChipPadding
                height: card.style.severityChipHeight
                radius: card.style.radiusMd
                color: card.severityColour
                Text {
                    id: severityText
                    anchors.centerIn: parent
                    text: card.style.severityLabel(card.finding).toUpperCase()
                    color: card.style.canvas
                    font.family: card.style.sansFamily
                    font.pixelSize: card.style.typeCaption
                    font.weight: Font.Bold
                    font.letterSpacing: card.style.severityLetterSpacing
                }
            }
            Text {
                objectName: "mixCheckFindingTitle:" + card.findingId
                width: Math.min(implicitWidth, body.width - severity.width - card.style.spaceMd)
                height: severity.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: card.style.text(card.finding ? card.finding.title : "")
                color: card.style.textMain
                font.family: card.style.sansFamily
                font.pixelSize: card.style.typeHeading
                font.weight: Font.Bold
            }
        }
        Text {
            width: Math.min(parent.width, card.style.findingTextMaxWidth)
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            text: card.style.text(card.finding ? card.finding.text : "")
            color: card.style.textDim
            linkColor: card.style.accent
            lineHeight: card.style.proseLineHeight
            font.family: card.style.sansFamily
            font.pixelSize: card.style.typeEmphasis
        }
        Flow {
            width: parent.width
            spacing: card.style.spaceSm
            Repeater {
                model: card.tracks
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    width: chipRow.implicitWidth + card.style.trackChipPadding
                    height: card.style.findingChipHeight
                    radius: card.style.findingChipHeight / 2
                    color: card.style.panelRaised
                    border.color: card.style.line
                    Row {
                        id: chipRow
                        x: card.style.trackChipInset
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: card.style.spaceSm
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: card.style.trackDotSize
                            height: card.style.trackDotSize
                            radius: card.style.radiusSm
                            color: chip.modelData && chip.modelData.color ? chip.modelData.color
                                                                          : card.style.textDim
                        }
                        Text {
                            text: card.style.text(chip.modelData ? chip.modelData.name : "")
                            color: card.style.textMain
                            font.family: card.style.sansFamily
                            font.pixelSize: card.style.typeBody
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
            Rectangle {
                visible: card.band !== null && card.style.text(card.band.label).length > 0
                width: bandText.implicitWidth + card.style.bandChipPadding
                height: card.style.findingChipHeight
                radius: card.style.findingChipHeight / 2
                color: card.style.panelRaised
                border.color: card.style.line
                Text {
                    id: bandText
                    anchors.centerIn: parent
                    text: card.band ? card.style.text(card.band.label) : ""
                    color: card.style.textDim
                    font.family: card.style.monoFamily
                    font.pixelSize: card.style.typeBody
                }
            }
        }
        MixCheckEvidence {
            objectName: "mixCheckEvidence:" + card.findingId
            visible: card.finding !== null && card.finding !== undefined && card.finding.evidence !== undefined
                     && card.finding.evidence !== null
            width: parent.width
            height: implicitHeight
            style: card.style
            evidence: card.finding ? card.finding.evidence : ({})
            colour: card.severityColour
            applied: card.applied
        }
        // Without a fix box the actions sit under the evidence.
        Loader {
            active: !card.hasFix
            visible: active
            width: parent.width
            sourceComponent: actionsComponent
        }
    }

    // The fix.
    Rectangle {
        id: fixBox
        visible: card.hasFix
        x: card.width - width - card.style.findingPadding
        y: card.style.findingPadding
        width: card.style.fixBoxWidth
        implicitHeight: fixColumn.implicitHeight + 2 * card.style.tilePadding
        height: Math.max(implicitHeight, card.height - 2 * card.style.findingPadding)
        radius: card.style.radiusXl
        color: card.style.panelRaised
        border.color: card.style.line
        Column {
            id: fixColumn
            x: card.style.tilePadding
            y: card.style.tilePadding
            width: parent.width - 2 * card.style.tilePadding
            spacing: card.style.spaceMd
            Row {
                spacing: card.style.spaceMd
                Text {
                    id: deviceText
                    text: card.style.text(card.fix ? card.fix.device : "")
                    color: card.style.textMain
                    font.family: card.style.sansFamily
                    font.pixelSize: card.style.typeEmphasis
                    font.weight: Font.Bold
                }
                Text {
                    anchors.baseline: deviceText.baseline
                    width: Math.min(implicitWidth, fixColumn.width - deviceText.implicitWidth - card.style.spaceMd)
                    elide: Text.ElideRight
                    text: card.style.text(card.fix ? card.fix.where : "")
                    color: card.style.textFaint
                    font.family: card.style.sansFamily
                    font.pixelSize: card.style.typeBody
                }
            }
            Repeater {
                model: card.style.list(card.fix ? card.fix.params : null)
                delegate: Item {
                    id: param
                    required property var modelData
                    readonly property var pair: card.style.list(modelData)
                    width: fixColumn.width
                    height: valueText.implicitHeight
                    Text {
                        id: labelText
                        text: card.style.text(param.pair[0])
                        color: card.style.textFaint
                        font.family: card.style.monoFamily
                        font.pixelSize: card.style.typeBody
                    }
                    Text {
                        id: valueText
                        anchors.right: parent.right
                        width: Math.min(implicitWidth, param.width - labelText.implicitWidth - card.style.fixParamGap)
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideLeft
                        text: card.style.text(param.pair[1])
                        color: card.style.textMain
                        font.family: card.style.monoFamily
                        font.pixelSize: card.style.typeBody
                    }
                }
            }
            Loader {
                active: card.hasFix
                width: parent.width
                sourceComponent: actionsComponent
            }
        }
    }

    Component {
        id: actionsComponent
        Column {
            spacing: card.style.spaceMd
            Flow {
                width: parent.width
                spacing: card.style.spaceSm
                Kit.Button {
                    objectName: "mixCheckApply:" + card.findingId
                    theme: card.style
                    visible: card.hasFix
                    prime: !card.applied
                    done: card.applied
                    label: card.applied ? "✓ Applied" : "Apply"
                    tip: card.applied ? "Applied as one undo step; Ctrl+Z takes it back"
                                      : "Add " + card.style.text(card.fix ? card.fix.device : "the fix")
                                        + " with these settings, as one undo step"
                    onClicked: {
                        if (!card.applied)
                            card.applyRequested(card.findingId)
                    }
                }
                Kit.Button {
                    objectName: "mixCheckListen:" + card.findingId
                    theme: card.style
                    label: card.listening ? "■ Stop" : "▶ Listen"
                    tip: card.listening ? "Stop listening"
                        : card.band ? "Solo " + card.style.text(card.band.label) + " of the mix"
                                    : "Listen to where it happens"
                    onClicked: card.listening ? card.stopRequested() : card.listenRequested(card.findingId)
                }
            }
            Text {
                objectName: "mixCheckOutcome:" + card.findingId
                visible: text.length > 0
                width: parent.width
                wrapMode: Text.WordWrap
                text: card.outcome
                color: card.applied ? card.style.accent : card.style.textFaint
                font.family: card.style.monoFamily
                font.pixelSize: card.style.typeBody
            }
        }
    }
}
