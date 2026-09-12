# Timeline Hover Editing

## Requested behavior

1. Capture the pointer when the user invokes `DalimaoCurves` over an AE Timeline.
2. Resolve that Timeline's composition and the layer under the pointer, even if another layer is selected.
3. Prefer the property row under the pointer. If only the layer is known, offer its animated properties with full paths in the popup. Do not edit every property or silently choose the first one.
4. Convert the captured pointer X to composition time using the current Timeline viewport. Select only the adjacent keys surrounding that time on the chosen property.
5. Open AE's Graph Editor and a temporary slider panel. Change the left key's outgoing ease and the right key's incoming ease, with undo support.
6. Repeating the shortcut closes the popup and restores the prior layer-bar/Graph Editor mode and selection, returning to the previous multi-layer view.

Proposed edge rules: no pair outside the property's keyed range or with fewer than two keys; at an exact key choose its following segment, except the last key has no following segment. These rules must be explicit in the UI and tests before implementation.

## Findings on 2026-09-12

The current implementation is a standalone GDI+ curve editor. It does not yet implement the requested Graph Editor/slider workflow.

- `ShowCurvesPanel` tries mouse-layer selection only when `AEGP_GetActiveLayer` fails or returns no layer. It subsequently chooses property index zero.
- `FindLayerRowIndex` infers an ordinal from bright pixel bands and row pitch. `SelectLayerUnderMouse` treats that ordinal as a layer index in the most recently used composition. This does not establish identity after scrolling, filtering, shy-layer hiding, or expanding properties.
- `CalibrateToTimeline` sets the popup's scale. Its results are not used to map pointer X to time or select a surrounding pair. Composition display start is not the current Timeline viewport start.
- The popup takes focus and mouse capture inside a blocking message loop. Shortcut loading assumes AE `24.6`; selection and view state are not snapshotted or restored.
- Calibration accepted detected rows containing up to three extra clusters, then indexed the shorter keyframe array. The accompanying fix rejects unequal counts. Equal counts alone still do not prove row identity or a correct time mapping.

Live inspection of the installed AE 2025 with `test project (converted).aep` open exposed Timeline search, scrollbars, and action buttons through the inspection tool's accessibility tree. It did not expose layer/property rows, keyframes, or ruler values. Point-specific accessibility hit testing remains unverified. Screenshot capture failed with `SetIsBorderRequired`, HRESULT `0x80004002`; pixel-based targeting was not tested.

## Implementation gates

Keep target resolution read-only until it produces a verified composition, layer, property, and mouse time. Do not substitute the selected layer, first property, or playhead for missing mouse information.

A Windows-specific resolver needs to establish Timeline identity, inspect point-specific UIA/MSAA results, and validate two independent pixel/time anchors against the current zoom and scroll. If it cannot establish a layer or time, report that limitation before changing AE state. A property chooser only resolves property ambiguity; it cannot repair missing layer or time information.

Before selection changes, snapshot the supported layer/property/key selection and Graph Editor mode. Use one cleanup path for toggle-off, cancel, failed initialization, and exceptions. Verify the original composition and targets still exist before restoring them. Avoid changing Timeline zoom, scroll, expansion, search, or shy flags unless their prior state can also be restored. Resolve and verify the Graph Editor command on the running AE version rather than guessing an ID.

Slider application must preserve key times and values, unrelated keys, and the opposite sides of both endpoints. Test temporal continuity/auto-Bezier coupling, Hold interpolation, separated dimensions, and spatial properties explicitly.

## Acceptance matrix

- Select A, hover B: resolve B; hover outside Timeline: no edits.
- Expanded properties, vertical scroll, hidden/shy layers, search filters, and multiple Timeline tabs: resolve the correct composition/layer/property or refuse ambiguity.
- Horizontal pan/zoom, nonzero display start, offscreen playhead, and different DPI: resolve the same known segment correctly.
- Duplicate property names: show full property paths; preserve the captured mouse time while choosing.
- Zero/one key, exact key, and outside keyed range: apply the stated edge rules.
- Apply sliders: only the two intended ease sides change; one gesture can be undone.
- Repeat shortcut in AE 2025, cancel, and failure: close the popup and restore the saved supported state.

## Build and evidence limits

At the time of the workflow investigation, VS 2022 Build Tools/MSVC 14.44 and AE 2025 were found locally, but the configured `D:\Adobe\...` and `G:\vs\...` paths were missing and no SDK was available.

Environment follow-up on 2026-09-12: the supplied SDK 25.6 build 61 was extracted in the repository, build/deployment paths were corrected, and Release/x64 built with zero warnings/errors. The resulting `.aex` was installed in AE 2025 (25.4), with its SHA-256 matching the build. See [development-environment.md](development-environment.md) for paths and commands. This resolves the missing-SDK build blocker only; the hover resolver, Graph Editor workflow and runtime acceptance matrix above remain unimplemented/unverified.

Official references:

- [Windows cursor coordinates](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getcursorpos): screen coordinates do not include AE layer/property/time semantics.
- [UI Automation point lookup](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-elementfrompoint) and [custom-control providers](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-providersoverview): semantic detail depends on the application provider.
- [Adobe keyboard shortcuts](https://helpx.adobe.com/after-effects/desktop/get-started/keyboard-shortcuts/keyboard-shortcuts-reference.html): Shift+F3 switches Timeline display modes.
- [Adobe SDK entry point](https://developer.adobe.com/after-effects/): obtain the native SDK through Get the SDKs.

The inspected source and accessibility result do not establish a reliable hover resolver. They also do not prove that an application-specific Windows implementation is impossible.
