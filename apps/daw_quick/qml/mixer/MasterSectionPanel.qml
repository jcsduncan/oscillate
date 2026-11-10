pragma ComponentBehavior: Bound
import QtQuick
import "../shell"

// The master section, beside the master strip: loudness (short-term large,
// integrated and momentary), true peak, a compact spectrum and the mono
// check (phase correlation with how much the mix loses in mono). Hold MONO
// to hear the mix folded to mono; the meters keep reading the stereo mix.
// Everything reads the final output, so it says what the listener hears.
Rectangle {
    id: section
    objectName: "masterSectionPanel"
    required property var bridge
    // The Rebalance view (ADR-0123), from the button under the mono check.
    signal rebalanceRequested()
    readonly property var master: bridge.masterSection
    readonly property var reading: master ? master.reading : ({ valid: false })
    // Streaming loudness targets: a tick on the short-term bar.
    property real targetLufs: -14
    // Short-term loudness this far over the target (LU) shows in amber.
    readonly property real targetTolerance: 2
    // A true peak over this (dBTP) shows in the warning colour.
    readonly property real truePeakCeiling: -1.0
    // Mono loss beyond this (dB), and correlation under this, warn.
    readonly property real monoLossWarnDb: -6
    readonly property real wideCorrelation: 0.3

    // The section's own sizes and colours.
    readonly property int sectionWidth: 196
    readonly property int sectionSpacing: 10
    readonly property int headerHeight: 14
    readonly property int heroSize: 26
    readonly property int barHeight: 6
    // The target tick stands this far above and below the bar.
    readonly property real tickOverhang: 2
    readonly property real tickOpacity: 0.6
    readonly property int readoutColumnGap: 10
    readonly property int spectrumHeight: 54
    readonly property real spectrumInset: 3
    readonly property real spectrumBarGap: 1
    readonly property real spectrumBarRadius: 1
    // Spectrum bars run from teal (quiet) toward orange (loud): each channel
    // starts at its quiet value and moves by its swing times the level.
    readonly property real spectrumQuietRed: 0.37
    readonly property real spectrumRedSwing: 0.6
    readonly property real spectrumQuietGreen: 0.88
    readonly property real spectrumGreenSwing: -0.3
    readonly property real spectrumQuietBlue: 0.75
    readonly property real spectrumBlueSwing: -0.4
    readonly property real spectrumBarAlpha: 0.9
    readonly property int monoHeaderHeight: 12
    readonly property int correlationHeight: 10
    readonly property real outOfPhaseOpacity: 0.12
    readonly property real correlationMarkerWidth: 4
    readonly property int scaleHeight: 10
    readonly property int monoButtonHeight: 26
    readonly property int rebalanceButtonHeight: 26
    readonly property color warnColor: Theme.masterSectionWarnColor
    // Level bars ease over this long; spectrum bars a little quicker.
    readonly property int levelEase: 90
    readonly property int spectrumEase: 80

    // Never narrower than its loudness readings, set side by side.
    implicitWidth: Math.max(sectionWidth, masterReadings.implicitWidth + 2 * Theme.spaceLg)
    radius: Theme.radiusXl
    color: Theme.panel
    border.color: Theme.line
    Accessible.role: Accessible.Pane
    Accessible.name: "Master section"

    function show(value, digits) {
        const number = Number(value)
        return reading.valid && isFinite(number) ? number.toFixed(digits).replace("-", "−") : "—"
    }
    readonly property real truePeak: Number(reading.truePeakDb)
    readonly property bool overPeak: reading.valid && isFinite(truePeak) && truePeak > truePeakCeiling
    readonly property real correlation: Number(reading.correlation)

    Column {
        anchors.fill: parent
        anchors.margins: Theme.spaceLg
        spacing: section.sectionSpacing

        Item {
            width: parent.width
            height: section.headerHeight
            Text {
                text: "MASTER"
                color: Theme.textMain
                font.pixelSize: Theme.typeBody
                font.bold: true
                font.letterSpacing: Theme.trackingXl
            }
            Text {
                objectName: "masterLoudnessReset"
                anchors.right: parent.right
                text: "RESET"
                color: resetArea.containsMouse ? Theme.textMain : Theme.textDim
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                font.pixelSize: Theme.typeCaption
                font.bold: true
                font.letterSpacing: Theme.trackingSm
                Accessible.role: Accessible.Button
                Accessible.name: "Reset integrated loudness"
                MouseArea {
                    id: resetArea
                    anchors.fill: parent
                    anchors.margins: -Theme.spaceSm
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (section.master) section.master.reset_loudness()
                    Tip { text: "Start measuring integrated loudness again from now" }
                }
            }
        }

        // Short-term loudness, large, with a bar from -36 to -6 LUFS.
        Column {
            width: parent.width
            spacing: Theme.spaceXs
            Row {
                spacing: Theme.spaceSm
                Text {
                    id: shortTermText
                    objectName: "masterShortTerm"
                    text: section.show(section.reading.shortTerm, 1)
                    color: Theme.textMain
                    font.pixelSize: section.heroSize
                    font.family: Theme.monoFamily
                    Accessible.role: Accessible.StaticText
                    Accessible.name: "Short-term loudness " + text + " LUFS"
                }
                Text {
                    anchors.baseline: shortTermText.baseline
                    text: "LUFS"
                    color: Theme.textDim
                    font.pixelSize: Theme.typeSmall
                }
            }
            Item {
                id: loudnessBar
                width: parent.width
                height: section.barHeight
                readonly property real low: -36
                readonly property real high: -6
                function at(lufs) { return Math.max(0, Math.min(1, (lufs - low) / (high - low))) }
                Rectangle { anchors.fill: parent; radius: Theme.radiusSm; color: Theme.meterTrack }
                Rectangle {
                    readonly property real value: Number(section.reading.shortTerm)
                    width: section.reading.valid && isFinite(value) ? loudnessBar.width * loudnessBar.at(value) : 0
                    height: parent.height
                    radius: Theme.radiusSm
                    color: value > section.targetLufs + section.targetTolerance ? Theme.amber : Theme.accent
                    Behavior on width {
                        enabled: Motion.enabled
                        NumberAnimation { duration: section.levelEase; easing.type: Easing.OutQuad }
                    }
                }
                Rectangle {
                    x: loudnessBar.width * loudnessBar.at(section.targetLufs) - width / 2
                    width: Theme.strokeBold
                    height: parent.height + 2 * section.tickOverhang
                    y: -section.tickOverhang
                    color: Theme.textMain
                    opacity: section.tickOpacity
                }
            }
        }

        Grid {
            id: masterReadings
            objectName: "masterReadings"
            // One column per reading.
            columns: readouts.model.length
            columnSpacing: section.readoutColumnGap
            rowSpacing: Theme.spaceXxs
            Repeater {
                id: readouts
                model: [
                    { label: "INTEGRATED", key: "integrated", unit: "LUFS" },
                    { label: "MOMENTARY", key: "momentary", unit: "LUFS" },
                    { label: "TRUE PEAK", key: "truePeakDb", unit: "dBTP" }
                ]
                delegate: Column {
                    required property var modelData
                    spacing: 1
                    Text {
                        text: modelData.label
                        color: Theme.textFaint
                        font.pixelSize: Theme.typeMicro
                        font.letterSpacing: Theme.trackingXxs
                    }
                    Text {
                        objectName: "master_" + modelData.key
                        text: section.show(section.reading[modelData.key], 1)
                        color: modelData.key === "truePeakDb" && section.overPeak ? section.warnColor : Theme.textMain
                        font.pixelSize: Theme.typeEmphasis
                        font.family: Theme.monoFamily
                        Accessible.role: Accessible.StaticText
                        Accessible.name: modelData.label.toLowerCase() + " " + text + " " + modelData.unit
                    }
                }
            }
        }

        // The spectrum, 20 Hz to 20 kHz on a log axis.
        Item {
            id: spectrum
            objectName: "masterSpectrumView"
            width: parent.width
            height: section.spectrumHeight
            readonly property var bands: section.bridge.masterSpectrum || []
            Rectangle { anchors.fill: parent; radius: Theme.radiusMd; color: Theme.well }
            Row {
                id: spectrumBars
                anchors.fill: parent
                anchors.margins: section.spectrumInset
                spacing: section.spectrumBarGap
                Repeater {
                    model: spectrum.bands.length
                    delegate: Rectangle {
                        required property int index
                        readonly property real level: Number(spectrum.bands[index]) || 0
                        width: Math.max(1, (spectrumBars.width - (spectrum.bands.length - 1) * section.spectrumBarGap)
                                           / spectrum.bands.length)
                        height: Math.max(1, level * spectrumBars.height)
                        y: spectrumBars.height - height
                        radius: section.spectrumBarRadius
                        color: Qt.rgba(section.spectrumQuietRed + section.spectrumRedSwing * level,
                                       section.spectrumQuietGreen + section.spectrumGreenSwing * level,
                                       section.spectrumQuietBlue + section.spectrumBlueSwing * level,
                                       section.spectrumBarAlpha)
                        Behavior on height { enabled: Motion.enabled; NumberAnimation { duration: section.spectrumEase } }
                    }
                }
            }
            Text {
                visible: spectrum.bands.length === 0
                anchors.centerIn: parent
                text: "Play to see the spectrum"
                color: Theme.textFaint
                font.pixelSize: Theme.typeCaption
            }
        }

        // The mono check: correlation from -1 (cancels) to +1 (mono).
        Column {
            width: parent.width
            spacing: Theme.spaceXs
            Item {
                width: parent.width
                height: section.monoHeaderHeight
                Text {
                    text: "MONO CHECK"
                    color: Theme.textFaint
                    font.pixelSize: Theme.typeMicro
                    font.letterSpacing: Theme.trackingXxs
                }
                Text {
                    objectName: "masterMonoLoss"
                    anchors.right: parent.right
                    text: section.reading.valid && isFinite(Number(section.reading.monoLossDb))
                          ? "mono " + section.show(section.reading.monoLossDb, 1) + " dB" : "—"
                    color: Number(section.reading.monoLossDb) < section.monoLossWarnDb ? section.warnColor : Theme.textDim
                    font.pixelSize: Theme.typeCaption
                    font.family: Theme.monoFamily
                }
            }
            Item {
                objectName: "masterCorrelation"
                width: parent.width
                height: section.correlationHeight
                Accessible.role: Accessible.Indicator
                Accessible.name: "Phase correlation " + section.show(section.correlation, 2)
                Tip { text: "Stereo correlation: +1 is mono-safe, near 0 is wide, below 0 cancels in mono" }
                Rectangle { anchors.fill: parent; radius: Theme.radiusSm; color: Theme.meterTrack }
                // The out-of-phase half is tinted.
                Rectangle {
                    width: parent.width / 2
                    height: parent.height
                    radius: Theme.radiusSm
                    color: section.warnColor
                    opacity: section.outOfPhaseOpacity
                }
                Rectangle {
                    x: parent.width / 2
                    width: Theme.hairline
                    height: parent.height
                    color: Theme.lineStrong
                }
                Rectangle {
                    visible: section.reading.valid && isFinite(section.correlation)
                    x: (section.correlation + 1) / 2 * (parent.width - width)
                    width: section.correlationMarkerWidth
                    height: parent.height
                    radius: width / 2
                    color: section.correlation < 0 ? section.warnColor
                        : section.correlation < section.wideCorrelation ? Theme.amber : Theme.accent
                    Behavior on x { enabled: Motion.enabled; NumberAnimation { duration: Motion.fast; easing.type: Easing.OutQuad } }
                }
            }
            Item {
                width: parent.width
                height: section.scaleHeight
                Text { text: "−1"; color: Theme.textFaint; font.pixelSize: Theme.typeMicro }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "0"; color: Theme.textFaint; font.pixelSize: Theme.typeMicro
                }
                Text { anchors.right: parent.right; text: "+1"; color: Theme.textFaint; font.pixelSize: Theme.typeMicro }
            }
        }

        // Hold to hear the mix in mono.
        Rectangle {
            objectName: "masterMonoListen"
            width: parent.width
            height: section.monoButtonHeight
            radius: Theme.controlRadius
            readonly property bool on: section.master ? section.master.monoListen : false
            color: on ? Theme.accent : monoArea.containsMouse ? Theme.buttonFace : Theme.panelRaised
            border.color: on ? Theme.accent : Theme.lineStrong
            Accessible.role: Accessible.Button
            Accessible.name: "Hold to listen in mono"
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Text {
                anchors.centerIn: parent
                text: parent.on ? "LISTENING IN MONO" : "HOLD FOR MONO"
                color: parent.on ? Theme.onAccentText : Theme.textMain
                font.pixelSize: Theme.typeCaption
                font.bold: true
                font.letterSpacing: Theme.trackingSm
            }
            MouseArea {
                id: monoArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onPressed: if (section.master) section.master.monoListen = true
                onReleased: if (section.master) section.master.monoListen = false
                onCanceled: if (section.master) section.master.monoListen = false
                Tip { text: "Hold to hear the mix folded to mono: phase problems thin it out. Export is unaffected" }
            }
        }

        // Each part of the mix, moved through its own tracks.
        Rectangle {
            objectName: "masterRebalance"
            width: parent.width
            height: section.rebalanceButtonHeight
            radius: Theme.controlRadius
            readonly property bool on: section.bridge.rebalance ? section.bridge.rebalance.enabled === true : false
            color: rebalanceArea.containsMouse ? Theme.buttonFace : Theme.panelRaised
            border.color: on ? Theme.accent : Theme.lineStrong
            Accessible.role: Accessible.Button
            Accessible.name: "Rebalance vocals, drums, bass and music"
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Text {
                anchors.centerIn: parent
                text: parent.on ? "REBALANCE · ON" : "REBALANCE"
                color: parent.on ? Theme.accent : Theme.textMain
                font.pixelSize: Theme.typeCaption
                font.bold: true
                font.letterSpacing: Theme.trackingSm
            }
            MouseArea {
                id: rebalanceArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: section.rebalanceRequested()
                Tip { text: "Move vocals, drums, bass and music on the master, each through its own tracks" }
            }
        }
    }
}
