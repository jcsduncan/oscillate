import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1180
    height: 820
    minimumWidth: 900
    minimumHeight: 640
    visible: true
    title: "Warp Lab"
    color: "#15181d"

    readonly property color panel: "#1f242b"
    readonly property color raised: "#2a3038"
    readonly property color line: "#3a414b"
    readonly property color text: "#e6e9ee"
    readonly property color dim: "#9aa3ae"
    readonly property color accent: "#4fd1a5"
    readonly property color warm: "#f0b45a"
    readonly property var modeNames: ["Beats", "Tones", "Texture", "Re-Pitch", "Complex", "Complex Pro"]
    readonly property var modeHints: [
        "Slices at transients; each slice plays unstretched. Best for drums.",
        "Pitch-synchronous grains. Best for monophonic, pitched material (vocals, bass).",
        "Granular. Best for pads, ambiences and noisy textures.",
        "Varispeed: pitch follows speed, like a turntable.",
        "Phase vocoder. Good general choice for full mixes.",
        "Complex plus formant preservation when transposing. Best for vocals and mixes."
    ]

    palette.window: panel
    palette.base: raised
    palette.button: raised
    palette.text: text
    palette.buttonText: text
    palette.windowText: text
    palette.highlight: accent

    FileDialog {
        id: openDialog
        title: "Open audio"
        nameFilters: ["Audio (*.wav *.flac *.mp3 *.ogg *.aif *.aiff *.m4a)", "All files (*)"]
        onAccepted: lab.load(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: "Save warped render"
        fileMode: FileDialog.SaveFile
        nameFilters: ["WAV (*.wav)"]
        onAccepted: lab.saveRender(selectedFile)
    }

    // Shared mid-signal waveform drawing: samples are [min, max] per column.
    component Waveform: Rectangle {
        id: wave
        property var samples: []
        property var markers: []
        property color stroke: window.accent
        property string empty: ""
        color: window.raised
        radius: 6
        border.color: window.line
        Canvas {
            id: canvas
            anchors.fill: parent
            anchors.margins: 6
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const columns = wave.samples.length / 2;
                const middle = height / 2;
                ctx.strokeStyle = window.line;
                ctx.beginPath();
                ctx.moveTo(0, middle);
                ctx.lineTo(width, middle);
                ctx.stroke();
                if (columns > 0) {
                    ctx.strokeStyle = wave.stroke;
                    ctx.beginPath();
                    for (let column = 0; column < columns; ++column) {
                        const x = column * width / columns;
                        ctx.moveTo(x, middle - wave.samples[2 * column + 1] * middle);
                        ctx.lineTo(x, middle - wave.samples[2 * column] * middle + 1);
                    }
                    ctx.stroke();
                }
                ctx.strokeStyle = window.warm;
                ctx.globalAlpha = 0.55;
                ctx.beginPath();
                for (const position of wave.markers) {
                    ctx.moveTo(position * width, 0);
                    ctx.lineTo(position * width, 8);
                }
                ctx.stroke();
            }
            Connections {
                target: wave
                function onSamplesChanged() { canvas.requestPaint(); }
                function onMarkersChanged() { canvas.requestPaint(); }
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
        }
        Label {
            anchors.centerIn: parent
            visible: wave.samples.length === 0
            text: wave.empty
            color: window.dim
        }
    }

    component SliderRow: RowLayout {
        id: row
        property string label: ""
        property real from: 0
        property real to: 100
        property real value: 0
        property real step: 1
        property int decimals: 0
        property string suffix: ""
        property string shown: Number(value).toFixed(decimals) + suffix
        signal moved(real value)
        spacing: 10
        Label { text: row.label; color: window.dim; Layout.preferredWidth: 120 }
        Slider {
            Layout.fillWidth: true
            from: row.from
            to: row.to
            stepSize: row.step
            value: row.value
            onMoved: row.moved(value)
            Accessible.name: row.label
        }
        Label {
            text: row.shown
            color: window.text
            Layout.preferredWidth: 70
            horizontalAlignment: Text.AlignRight
        }
    }

    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (drop.hasUrls && drop.urls.length > 0)
                lab.load(drop.urls[0]);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "WARP LAB"
                font.pixelSize: 20
                font.bold: true
                font.letterSpacing: 2
                color: window.text
            }
            Label {
                text: "sandbox · not the DAW"
                color: window.dim
                Layout.leftMargin: 8
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: lab.busy; visible: lab.busy; Layout.preferredHeight: 28 }
            Label {
                text: lab.status
                color: lab.status.startsWith("Render failed") || lab.status.indexOf("only") >= 0 ? window.warm : window.dim
                elide: Text.ElideRight
                Layout.maximumWidth: 560
            }
        }

        // Source
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 190
            color: window.panel
            radius: 8
            border.color: window.line
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "SOURCE"; color: window.dim; font.bold: true; font.letterSpacing: 1 }
                    Label {
                        text: lab.hasSource ? lab.sourceName + "  ·  " + lab.sourceInfo : "Drop an audio file anywhere, or open one"
                        color: window.text
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Button { text: "Convert with ffmpeg"; visible: lab.needsConversion; enabled: !lab.busy; onClicked: lab.convertAndLoad() }
                    Button { text: "Open…"; enabled: !lab.busy; onClicked: openDialog.open() }
                    Button {
                        text: lab.playing === "source" ? "■ Stop" : "▶ Original"
                        enabled: lab.hasSource
                        onClicked: lab.playing === "source" ? lab.stop() : lab.playSource()
                    }
                }
                Waveform {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    samples: lab.sourceWaveform
                    markers: lab.transients
                    stroke: "#7fa7d9"
                    empty: "No audio loaded"
                }
            }
        }

        // Settings
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 250
            color: window.panel
            radius: 8
            border.color: window.line
            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 18
                ColumnLayout {
                    Layout.preferredWidth: 520
                    Layout.fillHeight: true
                    spacing: 8
                    Label { text: "WARP MODE"; color: window.dim; font.bold: true; font.letterSpacing: 1 }
                    RowLayout {
                        spacing: 6
                        Repeater {
                            model: window.modeNames
                            delegate: Button {
                                required property string modelData
                                required property int index
                                text: modelData
                                checkable: true
                                checked: lab.mode === index
                                highlighted: checked
                                onClicked: lab.mode = index
                            }
                        }
                    }
                    Label {
                        text: window.modeHints[lab.mode]
                        color: window.dim
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    SliderRow {
                        Layout.fillWidth: true
                        label: "Speed"
                        from: -2
                        to: 2
                        step: 0.01
                        value: Math.log2(lab.speed)
                        // Moved on a log2 scale, shown as the speed factor.
                        shown: lab.speed.toFixed(2) + "×"
                        onMoved: (value) => lab.speed = Math.pow(2, value)
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Presets"; color: window.dim; Layout.preferredWidth: 120 }
                        Repeater {
                            model: [0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 4]
                            delegate: Button {
                                required property real modelData
                                text: modelData + "×"
                                flat: Math.abs(lab.speed - modelData) > 0.001
                                onClicked: lab.speed = modelData
                            }
                        }
                    }
                    Label {
                        text: "Speed " + lab.speed.toFixed(2) + "×  ·  duration " + (lab.speed > 1 ? "shorter" : lab.speed < 1 ? "longer" : "unchanged")
                        color: window.text
                    }
                    CheckBox {
                        text: "Mid-clip tempo change (markers at ⅓ and ⅔: speed, 0.6× speed, 1.5× speed) to hear seams"
                        checked: lab.tempoChange
                        onToggled: lab.tempoChange = checked
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 6
                    RowLayout {
                        spacing: 10
                        Label { text: "Transpose"; color: window.dim; Layout.preferredWidth: 120 }
                        SpinBox {
                            from: -24
                            to: 24
                            value: lab.semitones
                            enabled: lab.mode !== 3
                            onValueModified: lab.semitones = value
                            textFromValue: (value) => (value > 0 ? "+" : "") + value + " st"
                            Accessible.name: "Transpose semitones"
                        }
                        SpinBox {
                            from: -50
                            to: 50
                            value: lab.cents
                            enabled: lab.mode !== 3
                            onValueModified: lab.cents = value
                            textFromValue: (value) => (value > 0 ? "+" : "") + value + " ct"
                            Accessible.name: "Detune cents"
                        }
                        Label { visible: lab.mode === 3; text: "(Re-Pitch: pitch follows speed)"; color: window.dim }
                    }
                    RowLayout {
                        spacing: 10
                        Label { text: "Quality tier"; color: window.dim; Layout.preferredWidth: 120 }
                        ComboBox {
                            model: ["Q0 (export)", "Q1", "Q2 (lightest)"]
                            currentIndex: lab.tier
                            onActivated: (index) => lab.tier = index
                        }
                    }
                    // Mode-specific controls
                    RowLayout {
                        visible: lab.mode === 0
                        spacing: 10
                        Label { text: "Gap fill"; color: window.dim; Layout.preferredWidth: 120 }
                        ComboBox {
                            model: ["Loop off", "Loop forward", "Loop back-and-forth"]
                            currentIndex: lab.beatsLoop
                            onActivated: (index) => lab.beatsLoop = index
                        }
                    }
                    SliderRow {
                        visible: lab.mode === 0
                        Layout.fillWidth: true
                        label: "Transient envelope"
                        to: 100
                        value: lab.beatsEnvelope
                        onMoved: (value) => lab.beatsEnvelope = value
                    }
                    SliderRow {
                        visible: lab.mode === 1
                        Layout.fillWidth: true
                        label: "Grain size"
                        from: 5
                        to: 200
                        value: lab.tonesGrain
                        suffix: " ms"
                        onMoved: (value) => lab.tonesGrain = value
                    }
                    SliderRow {
                        visible: lab.mode === 2
                        Layout.fillWidth: true
                        label: "Grain size"
                        from: 10
                        to: 500
                        value: lab.textureGrain
                        suffix: " ms"
                        onMoved: (value) => lab.textureGrain = value
                    }
                    SliderRow {
                        visible: lab.mode === 2
                        Layout.fillWidth: true
                        label: "Fluctuation"
                        to: 100
                        value: lab.flux
                        onMoved: (value) => lab.flux = value
                    }
                    SliderRow {
                        visible: lab.mode === 5
                        Layout.fillWidth: true
                        label: "Formants"
                        to: 100
                        value: lab.formants
                        suffix: " %"
                        onMoved: (value) => lab.formants = value
                    }
                    SliderRow {
                        visible: lab.mode === 5
                        Layout.fillWidth: true
                        label: "Envelope"
                        from: 8
                        to: 256
                        value: lab.envelope
                        onMoved: (value) => lab.envelope = value
                    }
                    Item { Layout.fillHeight: true }
                    RowLayout {
                        spacing: 10
                        CheckBox {
                            text: "Re-render on every change"
                            checked: lab.autoRender
                            onToggled: lab.autoRender = checked
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "Render"
                            highlighted: true
                            enabled: lab.hasSource && !lab.busy
                            onClicked: lab.render()
                        }
                    }
                }
            }
        }

        // Render
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: window.panel
            radius: 8
            border.color: window.line
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "WARPED"; color: window.dim; font.bold: true; font.letterSpacing: 1 }
                    Label {
                        text: lab.hasRender ? lab.renderInfo : "Nothing rendered yet"
                        color: window.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Button {
                        text: lab.playing === "render" ? "■ Stop" : "▶ Warped"
                        highlighted: true
                        enabled: lab.hasRender
                        onClicked: lab.playing === "render" ? lab.stop() : lab.playRender()
                    }
                    Button { text: "Save…"; enabled: lab.hasRender; onClicked: saveDialog.open() }
                }
                Waveform {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    samples: lab.renderWaveform
                    empty: "Load audio to render"
                }
                Label {
                    text: "Space: play/stop warped  ·  O: play/stop original  ·  1–6: warp mode  ·  Drop a file anywhere to load it"
                    color: window.dim
                }
            }
        }
    }

    Shortcut {
        sequence: "Space"
        onActivated: lab.playing === "render" ? lab.stop() : lab.playRender()
    }
    Shortcut {
        sequence: "O"
        onActivated: lab.playing === "source" ? lab.stop() : lab.playSource()
    }
    Repeater {
        model: 6
        delegate: Item {
            required property int index
            Shortcut {
                sequence: String(index + 1)
                onActivated: lab.mode = index
            }
        }
    }
}
