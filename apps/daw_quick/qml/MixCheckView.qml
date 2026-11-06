pragma ComponentBehavior: Bound
import QtQuick
import "mixcheck"
import "devicekit" as Kit
import "shell"

// Mix Check: one button renders the song and
// every track offline, measures them and lists what pokes out in plain words,
// each with the track to blame, a Listen that solos the band and an Apply
// that adds the right device as one undo step. Top to bottom: the run bar,
// the spectrum with each finding pinned to its band, loudness against a
// target, energy through the song and what changed since the last check,
// the findings, what was checked and fine, a reference comparison and how it
// works. The shell shows it as a full view (Ctrl+Shift+M); `controller` is
// projectBridge.mixCheck. Every property may be empty before the first run.
Rectangle {
    id: view
    objectName: "mixCheckView"
    required property var controller

    readonly property MixCheckTheme style: MixCheckTheme {}
    readonly property var mc: controller !== undefined ? controller : null

    // The controller, made safe to read before a first run.
    readonly property bool running: mc !== null && mc.running === true
    readonly property real progress: mc !== null ? Math.max(0, Math.min(1, style.number(mc.progress) || 0)) : 0
    readonly property string stage: mc !== null ? style.text(mc.stage) : ""
    readonly property bool stale: mc !== null && mc.stale === true
    readonly property string status: mc !== null ? style.text(mc.status) : ""
    readonly property bool hasResult: mc !== null && mc.hasResult === true
    readonly property var summary: style.map(mc !== null ? mc.summary : null)
    readonly property var spectrumCurves: style.map(mc !== null ? mc.spectrum : null)
    readonly property var meters: style.list(mc !== null ? mc.meters : null)
    readonly property string target: mc !== null ? style.text(mc.target) : ""
    readonly property var targets: style.list(mc !== null ? mc.targets : null)
    readonly property var loudness: style.map(mc !== null ? mc.loudness : null)
    readonly property var energy: style.list(mc !== null ? mc.energy : null)
    readonly property var sections: style.list(mc !== null ? mc.sections : null)
    readonly property string energyNote: mc !== null ? style.text(mc.energyNote) : ""
    readonly property var findings: style.list(mc !== null ? mc.findings : null)
    readonly property var fine: style.list(mc !== null ? mc.fine : null)
    readonly property var sinceLast: style.list(mc !== null ? mc.sinceLast : null)
    readonly property string lastCheckedAt: mc !== null ? style.text(mc.lastCheckedAt) : ""
    readonly property var reference: style.list(mc !== null ? mc.reference : null)
    readonly property string referenceName: mc !== null ? style.text(mc.referenceName) : ""

    readonly property int fixedCount: {
        let count = 0
        for (let i = 0; i < findings.length; ++i) {
            if (findings[i] && findings[i].applied === true)
                ++count
        }
        return count
    }
    readonly property string findingsHeading: "Findings · " + (findings.length - fixedCount) + " to look at · "
                                              + fixedCount + " fixed"

    // The spectrum's view: "both" (before + after) or "mono" (the mono fold).
    property string spectrumView: "both"
    // The finding picked from the spectrum, highlighted in its card.
    property string hotId: ""
    // The finding being auditioned by Listen.
    property string listeningId: ""

    function run() {
        if (mc !== null)
            mc.run()
    }
    function cancel() {
        if (mc !== null)
            mc.cancel()
    }
    function applyFinding(id) {
        hotId = id
        if (mc !== null)
            mc.apply(id)
    }
    function listen(id) {
        if (mc === null)
            return
        mc.listen(id)
        listeningId = id
    }
    function stopListening() {
        if (mc !== null)
            mc.stopListening()
        listeningId = ""
    }
    function setTarget(id) {
        if (mc !== null)
            mc.target = id
    }
    function chooseReference() {
        if (mc !== null)
            mc.chooseReference()
    }
    function setReference(path) {
        if (mc !== null)
            mc.setReference(path)
    }
    function clearReference() {
        if (mc !== null)
            mc.clearReference()
    }
    function localPath(url) {
        const text = String(url)
        return text.startsWith("file://") ? decodeURIComponent(text.substring(7)) : text
    }
    // Highlights a finding's card and scrolls it into the middle of the view.
    function selectFinding(id) {
        hotId = id
        for (let i = 0; i < findingRepeater.count; ++i) {
            const card = findingRepeater.itemAt(i) as MixCheckFindingCard
            if (!card || card.findingId !== id)
                continue
            const top = card.mapToItem(content, 0, 0).y
            const wanted = top - (scroller.height - card.height) / 2
            scrollTo(Math.max(0, Math.min(wanted, scroller.contentHeight - scroller.height)))
            return
        }
    }
    function scrollTo(y) {
        scrollAnimation.stop()
        if (Motion.enabled) {
            scrollAnimation.to = y
            scrollAnimation.start()
        } else {
            scroller.contentY = y
        }
    }
    onFindingsChanged: {
        let found = false
        for (let i = 0; i < findings.length; ++i) {
            if (findings[i] && findings[i].id === listeningId)
                found = true
        }
        if (!found)
            listeningId = ""
    }
    onRunningChanged: {
        if (running)
            listeningId = ""
    }

    color: style.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: "Mix Check"

    // Before the first check: what it does, and one big button.
    Item {
        id: emptyState
        objectName: "mixCheckEmpty"
        anchors.fill: parent
        visible: !view.hasResult
        Reveal { target: emptyState }
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 2 * view.style.emptyMargin, view.style.emptyMaxWidth)
            spacing: view.style.spaceXl
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: view.style.emptyBadgeSize
                height: view.style.emptyBadgeSize
                radius: view.style.emptyBadgeRadius
                color: "transparent"
                border.color: view.style.accent
                border.width: view.style.strokeBold
                Text {
                    anchors.centerIn: parent
                    text: "✓"
                    color: view.style.accent
                    font.pixelSize: view.style.emptyGlyphSize
                    font.weight: Font.Bold
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Mix Check"
                color: view.style.textMain
                font.family: view.style.sansFamily
                font.pixelSize: view.style.emptyTitleSize
                font.weight: Font.ExtraBold
                font.letterSpacing: view.style.emptyTitleSpacing
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                textFormat: Text.StyledText
                lineHeight: view.style.emptyLineHeight
                text: "One button renders " + (view.style.text(view.summary.projectName).length > 0
                          ? "<b>" + view.style.text(view.summary.projectName) + "</b>" : "your song")
                      + " and every track <b>offline in the background</b>, measures it and lists what pokes "
                      + "out in plain words: each with <b>the track to blame</b>, a <b>Listen</b> that solos "
                      + "just that band, and an <b>Apply</b> that adds the right device with measured settings "
                      + "as one undo step. Then it checks again. It never changes anything you don't apply."
                color: view.style.textDim
                font.family: view.style.sansFamily
                font.pixelSize: view.style.typeEmphasis
            }
            Kit.Button {
                objectName: "mixCheckEmptyRunButton"
                anchors.horizontalCenter: parent.horizontalCenter
                theme: view.style
                large: true
                prime: !view.running
                label: view.running ? "Cancel" : "Run Mix Check"
                tip: view.running ? "Stop the check; nothing changes" : "Render and measure the song"
                shortcut: view.running ? "" : "Ctrl+Shift+M"
                onClicked: view.running ? view.cancel() : view.run()
            }
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: view.running
                spacing: view.style.spaceMd
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: view.style.emptyProgressWidth
                    height: view.style.progressBarHeight
                    radius: view.style.radiusXs
                    color: view.style.line
                    Rectangle {
                        width: parent.width * view.progress
                        height: parent.height
                        radius: view.style.radiusXs
                        color: view.style.accent
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: view.stage.length > 0 ? view.stage + "…" : "Starting…"
                    color: view.style.textDim
                    font.family: view.style.monoFamily
                    font.pixelSize: view.style.typeLabel
                }
            }
        }
    }

    Flickable {
        id: scroller
        objectName: "mixCheckContent"
        anchors.fill: parent
        visible: view.hasResult
        contentWidth: width
        contentHeight: content.implicitHeight + view.style.contentTail
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Reveal { target: scroller; duration: Motion.slow }
        NumberAnimation {
            id: scrollAnimation
            target: scroller
            property: "contentY"
            duration: Motion.slow
            easing.type: Motion.easing
        }

        Column {
            id: content
            x: Math.max(view.style.gap, (scroller.width - width) / 2)
            y: view.style.contentTop
            width: Math.min(scroller.width - 2 * view.style.gap, view.style.maxContentWidth)
            spacing: view.style.gap

            // 1. Run, progress and status; the spectrum view.
            Rectangle {
                id: runBar
                width: content.width
                height: view.style.runBarHeight
                radius: view.style.cardRadius
                color: view.style.panel
                border.color: view.style.line
                Row {
                    x: view.style.runBarPadding
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: view.style.spaceLg
                    Kit.Button {
                        objectName: "mixCheckRunButton"
                        anchors.verticalCenter: parent.verticalCenter
                        theme: view.style
                        prime: !view.running
                        label: view.running ? "Cancel" : "Run Mix Check"
                        tip: view.running ? "Stop the check; nothing changes" : "Render and measure the song again"
                        shortcut: view.running ? "" : "Ctrl+Shift+M"
                        onClicked: view.running ? view.cancel() : view.run()
                    }
                    Rectangle {
                        objectName: "mixCheckProgress"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: view.running
                        width: view.style.runProgressWidth
                        height: view.style.progressBarHeight
                        radius: view.style.radiusXs
                        color: view.style.line
                        Rectangle {
                            width: parent.width * view.progress
                            height: parent.height
                            radius: view.style.radiusXs
                            color: view.style.accent
                            Behavior on width { enabled: Motion.enabled; NumberAnimation { duration: Motion.normal } }
                        }
                    }
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: view.style.spaceSm
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: view.style.statusLampSize
                            height: view.style.statusLampSize
                            radius: view.style.statusLampSize / 2
                            color: view.stale ? view.style.amber : view.style.accent
                        }
                        Text {
                            objectName: "mixCheckStatus"
                            anchors.verticalCenter: parent.verticalCenter
                            text: view.running ? (view.stage.length > 0 ? view.stage + "…" : "Checking…") : view.status
                            color: view.style.textDim
                            font.family: view.style.monoFamily
                            font.pixelSize: view.style.typeLabel
                        }
                    }
                    Text {
                        objectName: "mixCheckStale"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: view.stale && !view.running
                        text: "Changed since the check — run again"
                        color: view.style.amber
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeLabel
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: view.style.runBarPadding
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: view.style.controlLabelGap
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Spectrum"
                        color: view.style.textFaint
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeBody
                    }
                    Kit.SegmentedControl {
                        anchors.verticalCenter: parent.verticalCenter
                        theme: view.style
                        namePrefix: "mixCheckSpectrumView"
                        accessibleName: "Spectrum view"
                        options: [{ id: "both", label: "Before + after", tip: "The mix before and after the fixes you applied" },
                                  { id: "mono", label: "Mono fold", tip: "Add the mix folded to mono: what a club system or a phone hears" }]
                        current: view.spectrumView
                        onPicked: function(id) { view.spectrumView = id }
                    }
                }
            }

            // 2. The spectrum with the meters above it.
            MixCheckCard {
                width: content.width
                style: view.style
                Item {
                    width: parent.width
                    height: legend.height
                    MixCheckHead {
                        id: spectrumHead
                        anchors.verticalCenter: parent.verticalCenter
                        width: view.style.spectrumHeadWidth
                        style: view.style
                        title: "The mix, 20 Hz – 20 kHz"
                    }
                    Row {
                        id: legend
                        x: spectrumHead.width
                        spacing: view.style.spectrumLegendSpacing
                        Repeater {
                            model: [
                                { label: "Before", colour: view.style.beforeCurve, shown: true },
                                { label: "After the fixes you applied", colour: view.style.afterCurve,
                                  shown: view.style.list(view.spectrumCurves.after).length > 0 },
                                { label: "Mono fold", colour: view.style.blue, shown: view.spectrumView === "mono" },
                                { label: "Finding", colour: view.style.red, shown: true },
                                { label: "Fixed", colour: view.style.accent, shown: true }
                            ]
                            delegate: Row {
                                id: legendItem
                                required property var modelData
                                visible: modelData.shown
                                spacing: view.style.spaceSm
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: view.style.spectrumSwatchWidth
                                    height: view.style.spectrumSwatchHeight
                                    radius: view.style.radiusXs
                                    color: legendItem.modelData.colour
                                }
                                Text {
                                    text: legendItem.modelData.label
                                    color: view.style.textDim
                                    font.family: view.style.sansFamily
                                    font.pixelSize: view.style.typeBody
                                }
                            }
                        }
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: legend.x + legend.width + implicitWidth + view.style.spaceXl < parent.width
                        text: "Click a marker to jump to its finding"
                        color: view.style.textFaint
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeBody
                    }
                }
                Row {
                    id: meterRow
                    objectName: "mixCheckMeters"
                    visible: view.meters.length > 0
                    spacing: view.style.spaceMd
                    Repeater {
                        model: view.meters
                        delegate: Rectangle {
                            id: meter
                            required property var modelData
                            readonly property var entry: view.style.map(modelData)
                            objectName: "mixCheckMeter:" + view.style.text(entry.id)
                            width: (content.width - 2 * view.style.cardPadding
                                    - (view.meters.length - 1) * meterRow.spacing) / Math.max(1, view.meters.length)
                            height: meterColumn.implicitHeight + 2 * view.style.tilePaddingTop
                            radius: view.style.radiusXl
                            color: view.style.panelRaised
                            border.color: view.style.line
                            Column {
                                id: meterColumn
                                x: view.style.tilePadding
                                y: view.style.tilePaddingTop
                                width: parent.width - 2 * view.style.tilePadding
                                spacing: view.style.meterSpacing
                                Text {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    text: view.style.text(meter.entry.label).toUpperCase()
                                    color: view.style.textFaint
                                    font.family: view.style.sansFamily
                                    font.pixelSize: view.style.typeSmall
                                    font.weight: Font.Bold
                                    font.letterSpacing: view.style.headingLetterSpacing
                                }
                                Text {
                                    text: view.style.text(meter.entry.value)
                                    color: view.style.stateColor(meter.entry.state)
                                    font.family: view.style.monoFamily
                                    font.pixelSize: view.style.figureSize
                                    font.weight: Font.DemiBold
                                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.slow } }
                                }
                                Text {
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                    text: view.style.text(meter.entry.note)
                                    color: view.style.textFaint
                                    font.family: view.style.sansFamily
                                    font.pixelSize: view.style.typeBody
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    width: parent.width
                    height: view.style.spectrumWellHeight
                    radius: view.style.radiusXl
                    color: view.style.wellFill
                    border.color: view.style.line
                    MixCheckSpectrum {
                        id: spectrum
                        objectName: "mixCheckSpectrum"
                        anchors.fill: parent
                        anchors.margins: view.style.hairline
                        style: view.style
                        curves: view.spectrumCurves
                        findings: view.findings
                        monoView: view.spectrumView === "mono"
                        hotId: view.hotId
                        onMarkerClicked: function(id) { view.selectFinding(id) }
                    }
                }
            }

            // 3. Loudness against the chosen target.
            MixCheckCard {
                width: content.width
                style: view.style
                MixCheckLoudness {
                    width: parent.width
                    style: view.style
                    targets: view.targets
                    target: view.target
                    loudness: view.loudness
                    onTargetPicked: function(id) { view.setTarget(id) }
                }
            }

            // 4. Energy through the song; since the last check.
            Row {
                id: energyRow
                spacing: view.style.gap
                readonly property real columnWidth: (content.width - view.style.gap) / 2
                readonly property real rowHeight: Math.max(energyCard.implicitHeight, sinceCard.implicitHeight)
                MixCheckCard {
                    id: energyCard
                    width: energyRow.columnWidth
                    height: energyRow.rowHeight
                    style: view.style
                    MixCheckHead {
                        width: parent.width
                        style: view.style
                        title: "Energy through the song"
                        hint: "short-term loudness per bar"
                    }
                    Rectangle {
                        width: parent.width
                        height: view.style.energyHeight
                        radius: view.style.radiusXl
                        color: view.style.wellFill
                        border.color: view.style.line
                        MixCheckEnergy {
                            objectName: "mixCheckEnergy"
                            anchors.fill: parent
                            anchors.margins: view.style.hairline
                            style: view.style
                            energy: view.energy
                            sections: view.sections
                        }
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        wrapMode: Text.WordWrap
                        textFormat: Text.StyledText
                        text: view.energyNote
                        color: view.style.textFaint
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeLabel
                        lineHeight: view.style.proseLineHeight
                    }
                }
                MixCheckCard {
                    id: sinceCard
                    objectName: "mixCheckSinceLast"
                    width: energyRow.columnWidth
                    height: energyRow.rowHeight
                    style: view.style
                    MixCheckHead {
                        width: parent.width
                        style: view.style
                        title: "Since your last check"
                        hint: view.lastCheckedAt.length > 0 ? view.lastCheckedAt + " → now" : ""
                    }
                    Text {
                        visible: view.sinceLast.length === 0
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "This is the first check of this project. The next one lists what got better and what got worse."
                        color: view.style.textDim
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeLabel
                    }
                    Repeater {
                        model: view.sinceLast
                        delegate: Item {
                            id: delta
                            required property var modelData
                            required property int index
                            readonly property var entry: view.style.map(modelData)
                            readonly property string direction: view.style.text(entry.direction)
                            readonly property bool up: direction === "fixed" || direction === "better"
                            readonly property bool down: direction === "worse" || direction === "new"
                            readonly property color tone: up ? view.style.accent : down ? view.style.red
                                                                                   : view.style.textFaint
                            objectName: "mixCheckSinceLast:" + index
                            width: sinceCard.innerWidth
                            height: Math.max(deltaText.implicitHeight, view.style.deltaRowHeight)
                            Text {
                                width: view.style.deltaGlyphWidth
                                text: delta.up ? "▲" : delta.down ? "▼" : "="
                                color: delta.tone
                                font.family: view.style.monoFamily
                                font.pixelSize: view.style.typeLabel
                                font.weight: Font.DemiBold
                            }
                            Text {
                                id: deltaText
                                x: view.style.deltaTextX
                                width: parent.width - x - directionText.implicitWidth - view.style.spaceLg
                                wrapMode: Text.WordWrap
                                text: view.style.text(delta.entry.text)
                                color: view.style.textDim
                                font.family: view.style.sansFamily
                                font.pixelSize: view.style.typeLabel
                            }
                            Text {
                                id: directionText
                                anchors.right: parent.right
                                text: delta.direction
                                color: delta.tone
                                font.family: view.style.monoFamily
                                font.pixelSize: view.style.typeLabel
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Each check is kept with the project, so a change that fixes one thing and breaks another shows up straight away."
                        color: view.style.textFaint
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeLabel
                    }
                }
            }

            // 5. The findings.
            Text {
                objectName: "mixCheckFindings"
                text: view.findingsHeading.toUpperCase()
                color: view.style.textFaint
                font.family: view.style.sansFamily
                font.pixelSize: view.style.typeSmall
                font.weight: Font.Bold
                font.letterSpacing: view.style.headingLetterSpacing
            }
            Text {
                visible: view.findings.length === 0
                width: content.width
                wrapMode: Text.WordWrap
                text: "Nothing pokes out. Everything the check measures is listed under Checked and fine."
                color: view.style.textDim
                font.family: view.style.sansFamily
                font.pixelSize: view.style.typeEmphasis
            }
            Column {
                width: content.width
                spacing: view.style.findingSpacing
                Repeater {
                    id: findingRepeater
                    model: view.findings
                    delegate: MixCheckFindingCard {
                        id: findingCard
                        required property var modelData
                        width: content.width
                        style: view.style
                        finding: modelData
                        listeningId: view.listeningId
                        highlighted: view.hotId.length > 0 && view.hotId === findingCard.findingId
                        onApplyRequested: function(id) { view.applyFinding(id) }
                        onListenRequested: function(id) { view.listen(id) }
                        onStopRequested: view.stopListening()
                    }
                }
            }

            // 6. Checked and fine; against a reference.
            Row {
                id: fineRow
                spacing: view.style.gap
                readonly property real columnWidth: (content.width - view.style.gap) / 2
                readonly property real rowHeight: Math.max(fineCard.implicitHeight, referenceCard.implicitHeight)
                MixCheckCard {
                    id: fineCard
                    objectName: "mixCheckFine"
                    width: fineRow.columnWidth
                    height: fineRow.rowHeight
                    style: view.style
                    MixCheckHead {
                        width: parent.width
                        style: view.style
                        title: "Checked and fine"
                    }
                    Repeater {
                        model: view.fine
                        delegate: Item {
                            id: fineItem
                            required property var modelData
                            width: fineCard.innerWidth
                            height: fineText.implicitHeight
                            Text {
                                text: "✓"
                                color: view.style.accent
                                font.pixelSize: view.style.typeLabel
                                font.weight: Font.Bold
                            }
                            Text {
                                id: fineText
                                x: view.style.fineTextX
                                width: parent.width - x
                                wrapMode: Text.WordWrap
                                text: view.style.text(fineItem.modelData)
                                color: view.style.textDim
                                font.family: view.style.sansFamily
                                font.pixelSize: view.style.typeLabel
                                lineHeight: view.style.listLineHeight
                            }
                        }
                    }
                }
                MixCheckCard {
                    id: referenceCard
                    objectName: "mixCheckReference"
                    width: fineRow.columnWidth
                    height: fineRow.rowHeight
                    style: view.style
                    MixCheckHead {
                        width: parent.width
                        style: view.style
                        title: "Against a reference"
                        hint: "optional"
                    }
                    Rectangle {
                        id: dropZone
                        width: parent.width
                        height: dropRow.implicitHeight + 2 * view.style.tilePadding
                        radius: view.style.radiusXl
                        color: referenceDrop.containsDrag ? view.style.alpha(view.style.accent, view.style.dropWashAlpha)
                                                          : "transparent"
                        border.color: referenceDrop.containsDrag ? view.style.accent : view.style.lineStrong
                        border.width: view.style.dropZoneEdgeWidth
                        Row {
                            id: dropRow
                            x: view.style.tilePadding
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: view.style.controlLabelGap
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: dropZone.width - 2 * view.style.tilePadding - chooseButton.width
                                       - (clearButton.visible ? clearButton.width + dropRow.spacing : 0) - dropRow.spacing
                                wrapMode: Text.WordWrap
                                text: view.referenceName.length > 0
                                    ? "Compared with " + view.referenceName
                                    : "Drop a finished track you like here, or choose one: the check compares bands with it"
                                color: view.referenceName.length > 0 ? view.style.textMain : view.style.textFaint
                                font.family: view.style.sansFamily
                                font.pixelSize: view.style.typeLabel
                            }
                            Kit.Button {
                                id: chooseButton
                                objectName: "mixCheckReferenceChoose"
                                anchors.verticalCenter: parent.verticalCenter
                                theme: view.style
                                label: "Choose…"
                                tip: "Choose a reference track to compare the mix with"
                                onClicked: view.chooseReference()
                            }
                            Kit.Button {
                                id: clearButton
                                objectName: "mixCheckReferenceClear"
                                anchors.verticalCenter: parent.verticalCenter
                                visible: view.referenceName.length > 0
                                theme: view.style
                                label: "Clear"
                                tip: "Stop comparing with the reference"
                                onClicked: view.clearReference()
                            }
                        }
                        DropArea {
                            id: referenceDrop
                            anchors.fill: parent
                            onEntered: function(drag) { drag.accepted = drag.hasUrls && drag.urls.length > 0 }
                            onDropped: function(drop) {
                                if (!drop.hasUrls || drop.urls.length === 0)
                                    return
                                view.setReference(view.localPath(drop.urls[0]))
                                drop.acceptProposedAction()
                            }
                        }
                    }
                    Repeater {
                        model: view.reference
                        delegate: Item {
                            id: referenceRow
                            required property var modelData
                            readonly property var entry: view.style.map(modelData)
                            readonly property real diff: view.style.number(entry.diffDb)
                            readonly property real fullScaleDb: 4
                            readonly property real closeDb: 1
                            readonly property color tone: !isFinite(diff) || Math.abs(diff) <= closeDb ? view.style.lineStrong
                                : diff > 0 ? view.style.amber : view.style.blue
                            objectName: "mixCheckReferenceBand:" + view.style.text(entry.band)
                            width: referenceCard.innerWidth
                            height: view.style.referenceRowHeight
                            Text {
                                width: view.style.referenceLabelWidth
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideRight
                                text: view.style.text(referenceRow.entry.band)
                                color: view.style.textDim
                                font.family: view.style.monoFamily
                                font.pixelSize: view.style.typeBody
                            }
                            Rectangle {
                                id: referenceTrack
                                x: view.style.referenceBarX
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - x - view.style.referenceValueWidth
                                height: view.style.referenceBarHeight
                                radius: view.style.radiusSm
                                color: view.style.wellFill
                                border.color: view.style.line
                                Rectangle {
                                    readonly property real share: isFinite(referenceRow.diff)
                                        ? Math.min(1, Math.abs(referenceRow.diff) / referenceRow.fullScaleDb) * 0.5 : 0
                                    x: referenceRow.diff >= 0 ? referenceTrack.width / 2
                                                              : referenceTrack.width / 2 - share * referenceTrack.width
                                    y: view.style.hairline
                                    width: share * referenceTrack.width
                                    height: parent.height - 2 * view.style.hairline
                                    radius: view.style.radiusXs
                                    color: referenceRow.tone
                                }
                                Rectangle {
                                    x: referenceTrack.width / 2
                                    y: -view.style.referenceCentreOutset
                                    width: view.style.hairline
                                    height: parent.height + 2 * view.style.referenceCentreOutset
                                    color: view.style.lineStrong
                                }
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: view.style.signedDb(referenceRow.diff, 1) + " dB"
                                color: view.style.textDim
                                font.family: view.style.monoFamily
                                font.pixelSize: view.style.typeBody
                            }
                        }
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Bars show your mix minus the reference, per band, after matching loudness. Within ±1 dB is drawn grey."
                        color: view.style.textFaint
                        font.family: view.style.sansFamily
                        font.pixelSize: view.style.typeLabel
                    }
                }
            }

            // 7. How it works.
            MixCheckCard {
                width: content.width
                style: view.style
                MixCheckHead {
                    width: parent.width
                    style: view.style
                    title: "How it works: the same steps every time"
                }
                Row {
                    id: steps
                    spacing: view.style.spaceMd
                    Repeater {
                        model: [
                            ["1 RENDER", "Song and every track", "Offline, with the export engine, while you keep working. Nothing plays."],
                            ["2 MEASURE", "Six checks", "Sub rumble, mono safety, resonances, kick/bass cancellation, band balance, level and crest."],
                            ["3 BLAME", "Which track", "A problem in the mix is traced to the track renders that contain it."],
                            ["4 FIX", "One rule, one device", "Rumble → Contour high-pass. Width → Vista band. Ringing → Temper cuts. Clash → Lock."],
                            ["5 RE-CHECK", "Proof", "Renders again and compares: did the target move, and did anything else move by more than 0.1 dB?"]
                        ]
                        delegate: Rectangle {
                            id: step
                            required property var modelData
                            width: (content.width - 2 * view.style.cardPadding - 4 * steps.spacing) / 5
                            height: stepColumn.implicitHeight + 2 * view.style.tilePadding
                            radius: view.style.radiusXl
                            color: view.style.panelRaised
                            border.color: view.style.line
                            Column {
                                id: stepColumn
                                x: view.style.tilePadding
                                y: view.style.tilePadding
                                width: parent.width - 2 * view.style.tilePadding
                                spacing: view.style.spaceXs
                                Text {
                                    text: step.modelData[0]
                                    color: view.style.accent
                                    font.family: view.style.monoFamily
                                    font.pixelSize: view.style.typeSmall
                                    font.weight: Font.Bold
                                }
                                Text {
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                    text: step.modelData[1]
                                    color: view.style.textMain
                                    font.family: view.style.sansFamily
                                    font.pixelSize: view.style.typeLabel
                                    font.weight: Font.Bold
                                }
                                Text {
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                    text: step.modelData[2]
                                    color: view.style.textDim
                                    font.family: view.style.sansFamily
                                    font.pixelSize: view.style.typeLabel
                                    lineHeight: view.style.listLineHeight
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
