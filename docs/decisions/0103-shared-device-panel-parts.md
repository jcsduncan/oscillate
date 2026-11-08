# ADR-0103 — Device panels share one header, status badge, control states, plot frame and zoom

**Status:** Accepted; the device kit parts are implemented, the panels adopt them in turn. **Date:** 8 November 2026.

## Context

A design review of the 36 built-in instrument and effect panels ([#161](https://github.com/jcsduncan/oscillate/issues/161)) found each panel building its own header, status indicator, plot axes and pointer targets:

- The header was rebuilt per panel. The readout shared a box with the preset, and showed "TOUCH A CONTROL" while idle.
- The Live indicator was a dot pill, red text, a footer or missing.
- Disabled, mode-inactive and linked controls looked the same.
- Plots lacked scales, units or a sign of whether they showed the running device.
- Many pointer targets were 8 to 20 px, and many custom controls could not be reached from the keyboard.
- Only Nova had a panel zoom.

## Decision

The device kit (`apps/daw_quick/qml/devicekit`) provides the parts, and every panel uses them:

- **DeviceHeader.** One row: the brand (the kit wordmark and subtitle, or a panel's own drawn wordmark), then the preset browser (`PresetBrowser`), then the readout (`Readout`), then any small live readings, then the status badge. The readout has its own box and is hidden until a control is touched. Mode switches go in a second row. A device without presets shows its own item where the browser would be.
- **StatusBadge.** The panel's only status display. It states the status in words with a lamp (Live, Updating, Unsaved, Applies to new notes, Not applied, Read only), gives the detail in a tooltip, and takes no focus.
- **Control states.** These three states look different:
  - Disabled: the device is not editable, and the control fades.
  - Mode-inactive: the current setting ignores the control. Its face dims and its captions read at inkDim. It keeps an `inactiveReason`, shown in its tooltip and spoken.
  - Linked: another control drives this one. It keeps full contrast, wears a LINK badge, and has a `linkReason`.

  A whole section that does nothing in the current setting collapses to its title with an OFF chip.
- **PlotFrame.** Every plot sits in a frame with labelled axes, with the unit on the first tick, and a LIVE or PREVIEW chip. The device-chain strips' mini views use it too.
- **Hit targets.** A pointer target is at least 24 × 24 px in panel pixels (`minHitSize`). Kit widgets drawn smaller extend their pointer area by `hitOutset`, and a knob's face is at least `knobMinDiameter`. Every pointer control can also be reached and used from the keyboard, with a visible focus ring. A pointer area the keyboard reaches another way names that way.
- **Panel zoom.** The editor window's title bar offers 90, 100, 120 and 140 % for every instrument and effect panel. The choice is kept in the workspace preferences (`workspace/panelZoom`). A fixed-size panel draws at the zoom times its default scale, scaled down to fit the window. Nova reflows at the zoom, as ADR-0076 describes.

The app contracts (`quick_native_effect_app_contract`, `quick_native_synth_app_integration_contract`) check every rule on every built-in panel (`tests/ui/quick/panel_kit_checks.hpp`). The device-kit contract checks the parts on their own.

## Consequences

- A new panel gets the header, badge, states, plot frame, hit targets and zoom from the kit. The contracts fail a panel that builds its own, leaves a plot unframed, or leaves a target small or out of keyboard reach.
- Panels keep their palettes and drawn wordmarks. The header's structure, the readout and the status read the same everywhere.
- Nova loses its zoom chips and its status line. The readout and badge move into its header, and voices and CPU move to the header's second row.
