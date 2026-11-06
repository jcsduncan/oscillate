import QtQuick

// Fades (and optionally grows) its target in each time it becomes visible
// (ADR-0059). Put it inside the item: `Reveal { target: menu }`. Closing is
// instant, so a hidden item never lingers over the one taking its place.
// The target's opacity and scale must not be bound elsewhere.
QtObject {
    id: reveal
    required property Item target
    property int duration: Motion.normal
    property real fromScale: 1
    property real fromOpacity: 0

    property ParallelAnimation animation: ParallelAnimation {
        NumberAnimation {
            target: reveal.target
            property: "opacity"
            from: reveal.fromOpacity
            to: 1
            duration: reveal.duration
            easing.type: Motion.easing
        }
        NumberAnimation {
            target: reveal.target
            property: "scale"
            from: reveal.fromScale
            to: 1
            duration: reveal.duration
            easing.type: Motion.easing
        }
    }
    function settle() {
        animation.stop()
        if (target) {
            target.opacity = 1
            target.scale = 1
        }
    }
    property Connections watcher: Connections {
        target: reveal.target
        function onVisibleChanged() {
            if (reveal.target.visible && Motion.enabled)
                reveal.animation.restart()
            else
                reveal.settle()
        }
    }
}
