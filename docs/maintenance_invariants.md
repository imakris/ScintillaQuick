# Maintenance Invariants

This document is for maintainers changing ScintillaQuick internals. It records
contracts that are easy to break when editing dispatch, capture, rendering,
input, or platform code.

## Source Map

Use this map before starting a change.

| Path | Ownership |
| --- | --- |
| `include/scintillaquick/` | Installed public API. Avoid exposing internals or test-only concepts here. |
| `src/public/` | `QQuickItem` boundary: Qt events, QML properties, public signals, message dispatch, scene-graph scheduling, IME. |
| `src/core/` | Scintilla integration: `WndProc`, direct callbacks, notifications, timers, clipboard/drop helpers, render-frame capture. |
| `src/platform/` | Qt implementation of Scintilla platform services: surfaces, fonts, menus, list boxes, call tips, platform `Window`. |
| `src/render/` | `Render_frame` and Qt Quick scene-graph renderer. Renderer code consumes captured values and must not query mutable editor state. |
| `tests/` | Smoke, dispatch-table, frame-validation, visual-regression, and behavior tests. Add focused tests near the behavior changed. |
| `benchmarks/` | Performance scenarios. Use before and after changing render scheduling or renderer hot paths. |
| `docs/` | Public docs and maintainer contracts. Keep implementation contracts current with code changes. |
| `third_party/scintilla/` | Maintained Scintilla fork with render-capture and Qt Quick surface hooks. Keep local patches recorded in its `LOCAL_CHANGES.md` and upstream provenance in `VERSION`. |

Most ScintillaQuick code lives in the top-level build files, `include`, `src`,
`tests`, `benchmarks`, `examples`, `docs`, and `cmake`. Integration hooks also
live in the vendored fork; consult its
[patch ledger](../third_party/scintilla/LOCAL_CHANGES.md) before changing them.
Preserve upstream copyright notices when updating derived files and use the
project attribution for local additions such as `RenderCapture.h`.

## Render Frame Contract

`Render_frame` is the immutable scene description passed from capture to the
scene-graph renderer for one item update.

### Coordinate Spaces

- `Render_frame` geometry is in `ScintillaQuick_item` item coordinates unless a
  field name explicitly says otherwise.
- `text_rect` and `margin_rect` are item-coordinate regions for the text area
  and margins.
- Text positions, baselines, clips, marker rectangles, indicator rectangles,
  whitespace rectangles, annotation rectangles, and indent-guide extents are
  item-coordinate geometry.
- Document identifiers such as `document_line`, `subline_index`, `style_id`,
  marker numbers, and indicator numbers are metadata. Do not translate them
  during scrolling.

If a new render primitive stores visible geometry, document whether it is in
item coordinates and update all frame translation, validation, and renderer
paths at the same time.

### Static And Overlay Content

Static content is the captured document and style-dependent visual body:

- visual lines and text runs
- margin text and markers
- fold display text
- annotations and EOL annotations
- indicators and decoration underlines
- whitespace marks and indent guides
- current-line and selection rectangles when they require fresh capture

Overlay content is cheap, frequently changing visual state that can be updated
without rebuilding the whole static frame when nothing else changed:

- caret blink visibility
- caret rectangles captured from the last full or overlay frame
- selection/caret UI changes that Scintilla reports without document/style
  mutation
- effective backgrounds whose colors or layers depend on selection, current-line
  highlighting, or body markers

`build_render_snapshot()` may reuse previous static content for overlay-only
updates. A path that changes visible document text, style, margins, wrapping,
scroll width, annotations, indicators, markers, whitespace settings, or
representation settings must mark static content dirty.

Each static recapture advances the snapshot's static revision. The renderer may
reuse static nodes only when that nonzero revision, the text clip, and the
device-pixel ratio are unchanged. Overlay updates retain the revision while
refreshing backgrounds, current-line highlights, selections, and carets. A zero
revision requires a full update. Newly attached nodes must inherit the item's
transform and opacity even when the static nodes are reused.

Scintilla's caret fine ticker owns the blink phase. Frame capture temporarily
enables the caret while collecting its geometry, then restores the phase. A
caret-only tick marks the snapshot dirty and selects or hides the valid cached
caret primitives using focus, caret activity, phase, and width. It reuses the
captured viewport without requesting overlay capture. Geometry changes still
require the usual capture invalidation.

### Layering

Layer order must match Scintilla visual semantics. The renderer currently keeps
this order locally, so changes to primitive families must be checked against
both `Render_frame` capture and
`src/render/scintillaquick_scene_graph_renderer.cpp`.

General ordering expectations:

- background and gutter bands first
- under-text decorations before glyphs when their Scintilla style says so
- selection/current-line visuals behind text unless Scintilla semantics require
  otherwise
- text and represented text in captured visual-line order
- over-text indicators, carets, and focus-sensitive overlays last

Every text-area group belongs below the shared `text_rect` clip. Margin groups
remain outside it. Preserve that boundary for new primitive families and for
horizontal scrolling.

Do not reorder renderer node groups for cleanup alone. If ordering changes, run
frame validation, renderer conformance on the software and RHI scene graphs,
and `scintillaquick_visual_regression_test` locally on Windows under the native
`windows` platform plugin. The stored PNG baselines remain a Windows
native-QPA oracle; Linux and macOS CI exclude that baseline suite and run
software renderer conformance (see `docs/limitations.md`).

### Threading

Scintilla is queried while building `Render_frame`, on the GUI thread. The
scene-graph renderer consumes `Render_snapshot` and `Render_frame` as captured
data during `updatePaintNode()`.

Renderer code must not send Scintilla messages, query or mutate editor/document
state, or depend on GUI-owned Scintilla pointers. If a renderer needs more
data, capture it into `Render_frame` or `Render_snapshot` first.

Value-based drawing helpers are permitted: indicator and marker nodes construct
render-local `Indicator`, `LineMarker`, and `Surface_impl` objects, then draw
into `QImage` textures using captured primitive fields. Character-marker fonts
are created on the render thread from captured `QFont` values. The shape cache
must include all visible shape fields, device-pixel ratio, and raster alignment;
it must not retain editor objects or GUI-owned Scintilla fonts.

## Invalidation Contract

The item-level render state is intentionally conservative. Missing an
invalidation is a correctness bug; extra invalidation is allowed only when it is
measured and understood.

| State | Meaning | Set when |
| --- | --- | --- |
| `snapshot_dirty` | A polish/update pass must rebuild or refresh the render snapshot before rendering. | Any path schedules visible scene-graph work. |
| `static_content_dirty` | Captured static visual content cannot be reused without recapture. | Document mutations, style changes, item resize, margin changes, wrapping/layout changes, representation changes, scroll-position changes, annotations, indicators, markers, IME text changes. |
| `overlay_content_dirty` | Overlay geometry/state needs capture even if static content can be reused. | Focus/caret activity changes, selection/caret rectangle changes, input-method cursor/anchor updates. |
| `style_sync_needed` | Scintilla styles need syncing into the Qt render snapshot before capture. | Style, font, zoom, element colour, marker/indicator style, or default-style changes. |
| `scrolling_update` | The change was caused by vertical scrolling. | `SCI_SETFIRSTVISIBLELINE`, public vertical scroll calls, and wheel scrolling routed through vertical scroll. |

Common cases:

- Document edit such as insert, append, replace, delete, paste, undo, redo:
  static content dirty, property sync, and scene-graph update.
- Style/font/zoom change: static content dirty and style sync needed if
  captured geometry or glyph appearance changes.
- Caret blink only: snapshot dirty; show or hide valid cached caret geometry.
- Vertical scroll: scrolling update and static dirty; recapture static content.
- Horizontal scroll: static content dirty; do not use vertical scroll
  translation.
- Notification from Scintilla: map the notification to document/style/view
  intent first, then schedule through the same dirty-state rules as public
  dispatch.
- IME preedit/commit/cancel: treat tentative text and indicator changes as
  visible static content changes; update input-method cursor/anchor state and
  accept handled Qt input-method events.
- Direct callbacks from `SCI_GETDIRECTFUNCTION` and
  `SCI_GETDIRECTSTATUSFUNCTION`: mutations must route through the same central
  dispatch policy as public `send()`/`sends()` so properties and rendering do not
  go stale.

Before adding a new direct flag write, first decide whether the operation is a
document mutation, style mutation, viewport/scroll change, or overlay-only
change. Prefer extending the central dispatch/invalidation helpers over adding
one-off scheduling at a call site.

## Dispatch Policy

All public and direct Scintilla message paths must go through one central
dispatcher:

- `send()`
- `sends()`
- direct-function callbacks exposed through Scintilla direct APIs
- public convenience methods that internally send Scintilla messages

Do not call `WndProc()` directly from a public or direct callback path unless the
call is part of the central dispatcher implementation or a reviewed raw-bypass
policy.

The dispatch table has three classes:

- Known read-only query: no scene-graph invalidation and no property sync.
- Known mutating or visual-state message: classify as narrowly as correctness
  allows.
- Unknown message: conservative full resync.

The unknown-message fallback must stay conservative. It is better to rebuild too
much for an unclassified message than to miss a visible update.

When adding or changing a classification:

1. Confirm the Scintilla message is pure query, document mutation, style
   mutation, viewport change, overlay change, or special direct-callback state.
2. Add it to the dispatch table with comments for non-obvious behavior.
3. Add dispatch-table tests for both the intended classification and nearby
   messages that must not be fast-pathed.
4. If the message is used internally by property sync or IME queries, ensure it
   is read-only-classified to avoid scheduling redundant resynchronization.
5. Run the dispatch-table test and CI-compatible correctness subset.

## GUI Notifications And Property Synchronization

Pending `SCN_UPDATEUI` flags are coalesced by GUI-thread idle work and drained
before snapshot preparation. Clear delivered flags before notifying observers
so edits or selection changes made by a synchronous observer remain pending for
a later delivery.

Mutating Scintilla message bursts coalesce geometry-property notifications at
GUI-thread polish or queued delivery for editors without a window. Property
getters still read the current Scintilla state synchronously. Changes made by a
property observer schedule another notification batch after the active batch
finishes, including when the observer processes a nested event loop.

`painted()` is emitted on the GUI thread after the prepared snapshot and frame
are ready for presentation, including a snapshot that only changes caret
visibility. Its completion point is snapshot preparation.

## Scene-Graph Rules

`updatePaintNode()` is a renderer boundary, not an editor boundary.

- It may consume `Render_snapshot` and `Render_frame`.
- It may create, reuse, detach, and delete `QSGNode` objects according to Qt
  Quick scene-graph rules.
- It must not call the mutable Scintilla editor or query QML-facing properties
  through `send()`.
- It must not mutate document, selection, style, IME, or platform-window state.
- Any data needed for rendering must be captured before the renderer runs.
- Indicator and marker drawing may use the render-local value objects described
  in the threading contract above.

Scene-graph changes are performance-sensitive. Before reducing or increasing
node counts, changing node cache keys, or changing update scheduling, capture a
benchmark baseline and compare post-change results against normal noise.

## Platform Ownership

The main editor item is borrowed by Scintilla platform `Window` wrappers.
Platform-created call-tip and list-box items are owned by ScintillaQuick's
platform lifecycle. Raw `WindowID` values must be resolved through the ownership
tracking policy before dereferencing or deleting.

- `wMain` borrowed ownership
- call-tip and list-box owned item lifecycle
- stale Qt deletion handling
- `Window::Destroy()` policy
- resolver behavior for `WindowID` consumers
- `Surface_impl` owned versus borrowed device rules

Do not make ad hoc platform-window deletion changes without updating these
rules and their tests.

## IME And Composition

IME handling bridges Qt input-method events to Scintilla tentative text and
indicators.

Maintenance rules:

- Inline IME is the supported mode.
- `m_preedit_pos` identifies the Scintilla position where the current preedit
  text starts.
- Preedit replacement, commit, and cancel paths must keep Scintilla text,
  Scintilla indicators, Qt input-method cursor/anchor rectangles, and render
  invalidation in sync.
- Clamp or reject malformed `QInputMethodEvent` attributes before indexing
  preedit text or indicator arrays.
- Read-only and protected-text paths may reject edits, but they still need
  explicit event acceptance decisions and cursor/selection UI updates.
- Handled input-method events should schedule visible updates and notify Qt of
  cursor/anchor rectangle changes where needed.

Any IME change should add focused smoke coverage for malformed attributes,
commit/cancel behavior, read-only/protected behavior, and input-method query
geometry when feasible. Manual platform IME checks are still useful because
input methods vary by OS.

## Legacy Core Hooks

ScintillaQuick should expose only hooks required by Scintilla's current
interfaces or by local validation support.

The old local `UpdateInfos()` wrapper is intentionally absent. Scintilla's
current interface uses `Editor::SetCtrlID()` / `GetCtrlID()` and the
`SCI_SETIDENTIFIER` / `SCI_GETIDENTIFIER` messages for notification identifiers.
Do not reintroduce a parallel wrapper unless a Scintilla upgrade adds a real
virtual/interface requirement.

## Signal Lifetime Rules

Signals that expose owned Qt values, such as `QByteArray` copied from
notification text, are safe to retain after delivery.

Signals that expose raw Scintilla notification pointers are only valid during
direct synchronous delivery unless an additive value-based API explicitly says
otherwise. Do not store such pointers, do not rely on queued delivery for them,
and do not expose new pointer-bearing notification APIs without API review.

Keep the compatibility rule here: value-based notification signals must own
copied payloads, and raw pointer signals remain direct-only.

The same rule binds the write side of `notify(Scintilla::NotificationData*)`.
`notifyParent()` copies the notification's text payload before it emits
`notify()`, and every signal emitted afterwards reads that owned copy. A slot may
still adjust the notification's scalar fields, including `length`, and those are
forwarded as written; assigning `NotificationData::text` has no effect and is
reported with a warning. A slot that needs to change the delivered text calls
`ScintillaQuick_item::replace_notification_text()`, which copies the bytes.
Never reintroduce a post-`notify()` read of `NotificationData::text` or a
`NotificationData::length`-sized copy out of it: the item owns neither the
buffer a slot may install nor any guarantee that a slot-supplied length still
describes Scintilla's buffer.

## Test And Benchmark Playbook

Use fresh build directories for validation work.

| Change area | Required checks |
| --- | --- |
| Dispatch table, `send()`, `sends()`, direct callbacks | `scintillaquick_dispatch_table_test`, smoke tests covering property sync and render invalidation, CI-compatible subset. |
| Render-frame capture or translation | `scintillaquick_frame_validation_test`, relevant smoke tests, renderer conformance and local Windows visual-baseline run for visible changes. |
| Scene-graph renderer, text cache, node pools, update scheduling | Frame validation, renderer conformance, local Windows visual-baseline run, benchmark baseline and post-change comparison. |
| IME/composition | Focused smoke tests for malformed attributes, commit/cancel, read-only/protected behavior; manual OS IME check when practical. |
| Platform windows, list boxes, call tips, surfaces, fonts, menus | Lifecycle smoke tests, stale deletion tests where feasible, CI-compatible subset. |
| Mouse, wheel, keyboard, focus, selection | Smoke tests that assert event acceptance, focus state, selection/caret state, and repaint scheduling. |
| Public API or signals | API review, examples if affected, install/consumer smoke if the installed surface changes. |
| Documentation-only changes | `git diff --check` and link/cross-reference inspection. |

A visual-baseline run means `scintillaquick_visual_regression_test` on Windows
under the native `windows` platform plugin. Only `ci-windows.yml` runs it; see
`docs/limitations.md` for why the baselines only reproduce there.

Renderer conformance uses `scintillaquick_renderer_software_1_test` and
`scintillaquick_renderer_software_1_25_test` on every supported platform, plus
`scintillaquick_renderer_rhi_1_test` and
`scintillaquick_renderer_rhi_1_25_test` on Windows. These compare scene-graph
output with direct drawing of the same captured values through Scintilla's
shape algorithms and validate the requested backend and device-pixel ratio.

Performance-sensitive changes need before/after measurement on the same machine.
Run each selected benchmark scenario several times, record distribution rather
than one result, and do not claim an improvement or accept a slowdown unless the
delta clears the measured noise floor.
