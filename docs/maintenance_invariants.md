# Maintenance Invariants

This document is for maintainers changing ScintillaQuick internals. It records
contracts that are easy to break when editing dispatch, capture, rendering,
input, or platform code.

For remediation sequencing and validation gates, see
[`review_remediation_plan.md`](review_remediation_plan.md). For platform-window
ownership details, see
[`platform_window_ownership.md`](platform_window_ownership.md).

## Source Map

Use this map before starting a change.

| Path | Ownership |
| --- | --- |
| `include/scintillaquick/` | Installed public API. Avoid exposing internals or test-only concepts here. |
| `src/public/` | `QQuickItem` boundary: Qt events, QML properties, public signals, message dispatch, scene-graph scheduling, IME. |
| `src/core/` | Scintilla integration: `WndProc`, direct callbacks, notifications, timers, clipboard/drop helpers, render-frame capture. |
| `src/platform/` | Qt implementation of Scintilla platform services: surfaces, fonts, menus, list boxes, call tips, platform `Window`. |
| `src/render/` | `Render_frame` and Qt Quick scene-graph renderer. Renderer code consumes captured data and should not call back into Scintilla. |
| `tests/` | Smoke, dispatch-table, frame-validation, visual-regression, and behavior tests. Add focused tests near the behavior changed. |
| `benchmarks/` | Performance scenarios and profiling entry points. Use before and after changing render scheduling or renderer hot paths. |
| `docs/` | Public docs plus maintainer contracts and remediation plans. Keep implementation contracts current with code changes. |
| `third_party/scintilla/` | Vendored upstream Scintilla. Treat as a dependency boundary. Do not make local behavior changes there unless the task is explicitly to update or patch Scintilla. |

ScintillaQuick-specific code lives in the top-level build files, `include`,
`src`, `tests`, `benchmarks`, `examples`, `docs`, and `cmake`. When reviewing or
editing normal ScintillaQuick work, inspect vendored Scintilla as reference
only.

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

`build_render_snapshot()` may reuse previous static content for overlay-only
updates. A path that changes visible document text, style, margins, wrapping,
scroll width, annotations, indicators, markers, whitespace settings, or
representation settings must mark static content dirty.

### Vertical Scroll Reuse

Vertical scroll reuse translates a previous frame instead of recapturing every
primitive. The invariant is:

> If a render primitive contains item-coordinate geometry, vertical scroll reuse
> must translate every such field together.

Today that includes:

- `Render_frame::text_rect`, `margin_rect`
- `Visual_line_frame::origin`, `baseline_y`, `clip_rect`
- `Text_run::position`, `top`, `bottom`, `blob_text_clip_rect`,
  `blob_outer_rect`, `blob_inner_rect`
- rectangles in selections, carets, indicators, current-line primitives,
  markers, fold text, annotations, EOL annotations, whitespace, and decoration
  underlines
- margin text positions, baselines, and clips
- indent-guide `top` and `bottom`

When adding a geometry field to `src/render/render_frame.h`, also update
`translate_render_frame()` and add or extend frame-validation coverage for
vertical scroll reuse. Visual-regression coverage should include represented
text, blob/control-character drawing, indicators, whitespace, annotations,
margins, and fold display text where practical.

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

Do not reorder renderer node groups for cleanup alone. If ordering changes, run
frame validation and the Linux visual-baseline gate.

### Threading

Scintilla is queried while building `Render_frame`, on the GUI thread. The
scene-graph renderer consumes `Render_snapshot` and `Render_frame` as captured
data during `updatePaintNode()`.

Renderer code must not call Scintilla, send Scintilla messages, mutate document
state, or depend on mutable editor pointers. If a renderer needs more data,
capture that data into `Render_frame` or `Render_snapshot` first.

## Invalidation Contract

The item-level render state is intentionally conservative. Missing an
invalidation is a correctness bug; extra invalidation is allowed only when it is
measured and understood.

For the reviewed Phase 3A migration design and implementation gates, see
[`render_invalidation_state_design.md`](render_invalidation_state_design.md).
For the required pre-implementation measurement gate, see
[`render_invalidation_metrics_baseline.md`](render_invalidation_metrics_baseline.md).

| State | Meaning | Set when |
| --- | --- | --- |
| `snapshot_dirty` | A polish/update pass must rebuild or refresh the render snapshot before rendering. | Any path schedules visible scene-graph work. |
| `static_content_dirty` | Captured static visual content cannot be reused without recapture or approved scroll translation. | Document mutations, style changes, margin changes, wrapping/layout changes, representation changes, scroll-position changes, annotations, indicators, markers, IME text changes. |
| `overlay_content_dirty` | Overlay state needs refresh even if static content can be reused. | Caret blink, focus/caret visibility changes, selection/caret rectangle changes, input-method cursor/anchor updates. |
| `style_sync_needed` | Scintilla styles need syncing into the Qt render snapshot before capture. | Style, font, zoom, element colour, marker/indicator style, or default-style changes. |
| `scrolling_update` | The change was caused by scrolling and may be eligible for scroll-specific reuse. | Vertical or horizontal scroll commands and wheel scroll handling. |
| `content_modified_since_last_capture` | Static content changed after the last capture, so vertical scroll reuse is unsafe. | Text/document modifications, IME composition mutations, style or representation mutations that alter captured primitives. |

Common cases:

- Document edit such as insert, append, replace, delete, paste, undo, redo:
  static content dirty, content modified since last capture, property sync, and
  scene-graph update.
- Style/font/zoom change: static content dirty, style sync needed, content
  modified since last capture if captured geometry or glyph appearance changes.
- Caret blink only: overlay dirty and snapshot dirty; do not recapture static
  content.
- Vertical scroll: scrolling update and static dirty, but reuse is allowed only
  when no content was modified since capture and the scroll delta fits the
  capture buffer.
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
   is read-only-classified to avoid recursive full resync.
5. Run the dispatch-table test and CI-compatible correctness subset.

## Scene-Graph Rules

`updatePaintNode()` is a renderer boundary, not an editor boundary.

- It may consume `Render_snapshot` and `Render_frame`.
- It may create, reuse, detach, and delete `QSGNode` objects according to Qt
  Quick scene-graph rules.
- It must not call back into Scintilla or query QML-facing properties through
  `send()`.
- It must not mutate document, selection, style, IME, or platform-window state.
- Any data needed for rendering must be captured before the renderer runs.

Scene-graph changes are performance-sensitive. Before reducing or increasing
node counts, changing node cache keys, or changing update scheduling, capture a
benchmark baseline and compare post-change results against normal noise.

## Platform Ownership

The main editor item is borrowed by Scintilla platform `Window` wrappers.
Platform-created call-tip and list-box items are owned by ScintillaQuick's
platform lifecycle. Raw `WindowID` values must be resolved through the ownership
tracking policy before dereferencing or deleting.

Use [`platform_window_ownership.md`](platform_window_ownership.md) as the
source of truth for:

- `wMain` borrowed ownership
- call-tip and list-box owned item lifecycle
- stale Qt deletion handling
- `Window::Destroy()` policy
- resolver behavior for `WindowID` consumers
- `Surface_impl` owned versus borrowed device rules

Do not make ad hoc platform-window deletion changes without updating that doc
and its tests.

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

`PartialPaint()` and `PartialPaintQml()` are retained as a raster reference path
for validation access. They let tests ask Scintilla to paint into a `QImage`
oracle so frame and scene-graph output can be compared against Scintilla's
raster behavior. They are not a second production renderer. Production Qt Quick
rendering must continue to capture `Render_frame` data and render it through the
scene graph.

## Signal Lifetime Rules

Signals that expose owned Qt values, such as `QByteArray` copied from
notification text, are safe to retain after delivery.

Signals that expose raw Scintilla notification pointers are only valid during
direct synchronous delivery unless an additive value-based API explicitly says
otherwise. Do not store such pointers, do not rely on queued delivery for them,
and do not expose new pointer-bearing notification APIs without API review.

Detailed compatibility decisions for notification signals belong to the Package
2C notification API design. This document records only the current maintenance
rule.

## Test And Benchmark Playbook

Use fresh build directories for remediation work. The normal local gate is the
CI-compatible subset from
[`review_remediation_plan.md`](review_remediation_plan.md).

| Change area | Required checks |
| --- | --- |
| Dispatch table, `send()`, `sends()`, direct callbacks | `scintillaquick_dispatch_table_test`, smoke tests covering property sync and render invalidation, CI-compatible subset. |
| Render-frame capture or translation | `scintillaquick_frame_validation_test`, relevant smoke tests, Linux visual-baseline gate for visible changes. |
| Raster reference access through `PartialPaint()` / `PartialPaintQml()` | `scintillaquick_frame_validation_test`, plus the CI-compatible subset if source behavior changes. |
| Scene-graph renderer, text cache, node pools, update scheduling | Frame validation, Linux visual-baseline gate, benchmark baseline and post-change comparison. |
| IME/composition | Focused smoke tests for malformed attributes, commit/cancel, read-only/protected behavior; manual OS IME check when practical. |
| Platform windows, list boxes, call tips, surfaces, fonts, menus | Lifecycle smoke tests, stale deletion tests where feasible, CI-compatible subset. |
| Mouse, wheel, keyboard, focus, selection | Smoke tests that assert event acceptance, focus state, selection/caret state, and repaint scheduling. |
| Public API or signals | API review, examples if affected, install/consumer smoke if the installed surface changes. |
| Documentation-only changes | `git diff --check` and link/cross-reference inspection. |

Performance-sensitive changes need before/after measurement on the same machine.
Run each selected benchmark scenario several times, record distribution rather
than one result, and do not claim an improvement or accept a slowdown unless the
delta clears the measured noise floor.
