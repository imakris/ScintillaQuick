# Render Invalidation State Design

This is a Phase 3A design document only. It records the current render
invalidation model and proposes a staged `RenderInvalidationState` migration.
No code implementation is authorized by this document.

Read-only inspection base:

- `docs/maintenance_invariants.md`
- `docs/review_remediation_plan.md`
- `src/public/scintillaquick_item.cpp`
- `src/core/scintillaquick_core.cpp`
- `src/core/scintillaquick_core.h`
- `src/core/scintillaquick_dispatch_table.h`
- targeted tests in `tests/smoke`, `tests/dispatch_table`, and
  `tests/frame_validation`

## Goals

Package 3A should make invalidation easier to reason about without changing
rendering behavior on the first implementation pass.

The future implementation must:

- centralize dirty-state intent, property-sync intent, scroll intent, and
  polish/update coalescing;
- preserve current public `send()` / `sends()` / direct-callback dispatch
  behavior;
- preserve the conservative unknown-message fallback from the dispatch table;
- preserve current overlay-only, blink-only, and vertical-scroll-reuse fast
  paths;
- avoid renderer, `Render_frame`, scene-graph node, and primitive-model
  changes;
- avoid extra render hot-path copies;
- keep no-under-invalidation as the primary correctness rule, with extra
  invalidation treated as a measurable performance risk.

## Current State

`ScintillaQuick_item::Render_data` owns both captured render data and
invalidation flags:

| State | Current role | Primary readers |
| --- | --- | --- |
| `snapshot_dirty` | A polish pass should rebuild or refresh the snapshot. | `updatePolish()`, caret blink shortcut in `build_render_snapshot()`. |
| `static_content_dirty` | Static captured content may need recapture. | `build_render_snapshot()` full/overlay/reuse decision. |
| `style_sync_needed` | Style data should be synced before static capture. | `build_render_snapshot()` argument to `current_render_frame()`. |
| `scrolling_update` | Change was classified as scrolling and may be eligible for scroll reuse. | `build_render_snapshot()` and `current_render_frame()`. |
| `overlay_content_dirty` | Overlay state needs refresh even when static content can be reused. | `build_render_snapshot()` overlay-only path. |
| `content_modified_since_last_capture` | Static content changed after the last capture, so vertical scroll reuse is unsafe. | `build_render_snapshot()` vertical-scroll reuse guard. |
| `update_pending` | A polish/update request is already queued. | `request_scene_graph_update()`, `updatePolish()`. |
| `capture_base_first_visible_line` | Base line for vertical scroll reuse buffer. | `build_render_snapshot()`. |
| `previous_first_visible_line` | Last captured first visible line. | `build_render_snapshot()` scroll-position comparison. |
| `previous_x_offset` | Last captured horizontal offset. | `build_render_snapshot()` scroll-position comparison. |
| `captured_caret_primitives` | Last captured caret geometry for blink-only updates. | `build_render_snapshot()`. |

The current scheduling funnel is `request_scene_graph_update(bool
static_content_dirty = false, bool needs_style_sync = false, bool scrolling =
false)`. It ORs the three supplied intent bits into `Render_data`, always marks
`snapshot_dirty` and `overlay_content_dirty`, increments profiling update
requests, and coalesces `polish()` / `update()` through `update_pending`.

Not every path uses this funnel fully. Some paths also write
`content_modified_since_last_capture` directly. Caret blink writes
`snapshot_dirty` and schedules `polish()` / `update()` directly so it can keep
the cheap blink-only path.

## Current Writers And Readers

### Dispatch And Direct Callbacks

`send()` and `sends()` call `dispatch_scintilla_message()`, which dispatches to
`m_core->WndProc()` and then applies `scene_graph_update_request(message)`.
Direct callbacks exposed through `SCI_GETDIRECTFUNCTION` and
`SCI_GETDIRECTSTATUSFUNCTION` route back to
`ScintillaQuick_item::send()`, so they share the same dispatch table and
invalidation behavior.

Current dispatch application:

- resets tracked scroll width when `scroll_width_reset` is set;
- then, while `m_in_sync_quick_view_properties` is true, skips the rest of the
  update application. In that re-entry case it does not call
  `syncQuickViewProperties()`, does not set
  `content_modified_since_last_capture`, and does not call
  `request_scene_graph_update()`. The scroll-width reset above is the only
  update side effect applied before the guard;
- calls `syncQuickViewProperties()` only when the request indicates static
  content, style, or scrolling may have changed;
- sets `content_modified_since_last_capture` when dispatch reports static
  content dirty;
- calls `request_scene_graph_update()` with dispatch-provided dirty flags.

The dispatch table currently has one sorted `Message_rule` source with binary
search. Effects map to:

| Dispatch effect | `needed` | Static | Style | Scroll |
| --- | --- | --- | --- | --- |
| `Read_only` | false | false | false | false |
| `Overlay` | true | false | false | false |
| `Static_content` | true | true | false | false |
| `Static_content_and_style` | true | true | true | false |
| `Scroll` | true | true | true | true |
| Unknown fallback | true | true | true | false |

### Notifications

`ScintillaQuick_core::NotifyParent()` emits `notifyParent(scn)` to
`ScintillaQuick_item::notifyParent()`. The item emits legacy and safe
notification signals, then handles selected notification codes.

Current notification invalidation:

| Notification | Current behavior |
| --- | --- |
| `StyleNeeded` | Sets `content_modified_since_last_capture`, schedules static/style update. |
| `UpdateUI` | Calls `updateQuickView(updated)`, emits `updateUi`. |
| `Modified` | Emits typed signals, sets `content_modified_since_last_capture`, resets tracked scroll width on deletes, schedules static/style update. |
| `Zoom` | Sets `content_modified_since_last_capture`, resets tracked scroll width, syncs properties, schedules static/style update. |
| Other typed notifications | Mostly emit Qt signals without explicit render scheduling. |

`updateQuickView(updated)` is a second mapping layer. It syncs properties for
content, vertical scroll, or horizontal scroll updates; calls
`cursorChangedUpdateMarker()`; then schedules:

- static dirty for content, vertical scroll, or horizontal scroll;
- style sync for content or vertical scroll;
- scrolling only for vertical scroll.

### Item Lifecycle And Update Gating

Several item-level paths also write invalidation state directly:

| Path | Current scheduling |
| --- | --- |
| `geometryChange()` size change | Calls `ChangeSize()`, emits `resized()`, sets `content_modified_since_last_capture`, and schedules static/style update. Position-only geometry changes are forwarded to `QQuickItem` but do not schedule an update. |
| `setFont()` | Applies the new editor font through Scintilla styles, syncs quick-view properties, emits `fontChanged()`, sets `content_modified_since_last_capture`, and schedules static/style update. |
| `enableUpdate(false)` / `m_updates_enabled` | Causes `request_scene_graph_update()` to return early without setting dirty flags, profiling update requests, `update_pending`, polish, or update. |
| `enableUpdate(true)` | Re-enables scheduling and calls `request_scene_graph_update()` with no static/style/scroll flags, which currently schedules an overlay update. |

### Scroll Handlers

| Path | Current scheduling |
| --- | --- |
| `scrollHorizontal(value)` | Calls `HorizontalScrollTo()`, syncs properties, schedules static update without `scrolling=true`. |
| `scrollVertical(value)` | Calls `ScrollTo()`, schedules static update with `scrolling=true`; property sync may arrive from Scintilla update notifications. |
| Wheel zoom | Runs Scintilla zoom command, sets `content_modified_since_last_capture`, syncs properties, schedules static/style update. |
| Wheel vertical scroll | Calls `scrollVertical()`. |
| Mouse move with autoscroll | Compares first visible line and x offset; if changed, syncs properties and schedules static update, with `scrolling=true` only for vertical changes. |
| Core fine timer scroll | `ScintillaQuick_core::timerEvent()` detects changed `topLine` / `xOffset` after `TickFor()` and schedules static update, with `scrolling=true` only for vertical changes. |

Vertical scroll reuse is decided later in `build_render_snapshot()`, not when
the update is requested.

### Qt Input And Focus Events

| Path | Current scheduling |
| --- | --- |
| Focus in/out | Updates Scintilla focus, syncs caret blink timer, schedules overlay update. |
| Key press | Scintilla key command or text insertion; when view changed, updates cursor marker and schedules overlay update. Scintilla notifications may add static invalidation for edits. |
| Mouse left/right press | Scintilla button handling, cursor marker update where applicable, overlay update. |
| Middle-click selection paste | Cursor marker update and static/style update. |
| Mouse release | Overlay update. |
| Mouse move | Overlay update unless scroll changed; then static update as above. |
| Touch press/release | Focus and cursor marker updates; current code does not directly call `request_scene_graph_update()` here. |
| Drag enter/move/leave/drop | Mostly calls core drag/drop helpers. Current event handlers rely on Scintilla side effects or notifications for visible updates. |
| IME read-only/protected path | Shows caret, updates cursor marker, schedules overlay update, accepts event. |
| IME commit/preedit path | Mutates tentative text/indicators, updates caret marker and properties, sets `content_modified_since_last_capture`, schedules static/style update. |

Touch and drag/drop are current-state observations, not proposed fixes. A
future implementation must preserve existing behavior first, then use focused
tests before adding scheduling to those paths.

### Caret Blink

The item-level caret blink timer toggles `m_caret_blink_visible`, sets
`snapshot_dirty`, and calls `polish()` / `update()` directly when updates are
enabled. It does not call `request_scene_graph_update()`.

`build_render_snapshot()` has a blink-only shortcut when static and overlay
flags are both clear and captured caret primitives are available. That path
only toggles caret primitives in the existing frame and clears
`snapshot_dirty`; it must remain cheap.

### Render Snapshot Capture

`updatePolish()` clears `update_pending` and calls `build_render_snapshot()` if
`snapshot_dirty` is set.

`build_render_snapshot()` reads dirty flags and scroll history to choose one of
four paths:

1. Blink-only update: no static or overlay flags, existing caret primitives.
2. Vertical scroll reuse: static dirty, scrolling update, no content modified
   since last capture, same x offset, bounded line delta inside the capture
   buffer.
3. Overlay-only capture: static content not dirty, overlay content dirty.
4. Full/static capture through `ScintillaQuick_core::current_render_frame()`.

At the end of capture it stores the new snapshot/frame, clears dirty flags,
updates scroll history, and may call `syncQuickViewProperties()` if the capture
changed Scintilla's scroll width.

## Pain Points

The current model works, but future edits are error-prone because invalidation
intent is split across:

- dispatch classification;
- duplicate `send()` / `sends()` apply lambdas;
- notification-specific branches;
- event handlers that directly choose boolean triples;
- direct writes to `content_modified_since_last_capture`;
- caret blink direct scheduling;
- snapshot capture and scroll-reuse decisions.

The problem is not only duplicated code. The real risk is that a future call
site may set `static_content_dirty` but forget `content_modified_since_last_capture`,
or schedule a scroll update without preserving vertical-scroll-reuse eligibility,
or turn overlay-only caret movement into repeated property sync and full static
capture.

## Proposed Model

Introduce a small internal staging object, tentatively named
`RenderInvalidationState`. The name is less important than the separation of
responsibilities.

The future class should own invalidation intent, not rendering data. It should
not own `Render_frame`, `Render_snapshot`, `Scene_graph_renderer`, or renderer
node state.

Sketch:

```cpp
class RenderInvalidationState {
public:
    void mark_overlay_dirty();
    void mark_static_content_dirty();
    void mark_style_sync_needed();
    void mark_scroll_update(bool vertical);
    void mark_content_modified_since_capture();
    void mark_scroll_width_reset();
    void mark_property_sync_needed();
    void mark_snapshot_dirty_only_for_caret_blink();

    void apply_dispatch_request(const scene_graph_update_request_info_t& request);
    void schedule(ScintillaQuick_item& item);
    void clear_after_snapshot_capture(int first_visible_line, int x_offset);

    // Query methods used by build_render_snapshot().
};
```

The public API should remain `request_scene_graph_update(...)` during the first
migration. Its implementation can delegate to the new state object, but callers
should not be forced to change all at once.

The class should track intent in semantic terms:

| Semantic intent | Current flags affected |
| --- | --- |
| Snapshot required | `snapshot_dirty`, schedule polish/update. |
| Static content invalid | `static_content_dirty`. |
| Style sync required | `style_sync_needed`. |
| Overlay invalid | `overlay_content_dirty`. |
| Vertical scroll candidate | `scrolling_update`, plus current scroll history rules. |
| Static mutation since capture | `content_modified_since_last_capture`. |
| Property sync required | currently an immediate call to `syncQuickViewProperties()`. |
| Scroll width reset required | currently immediate `reset_tracked_scroll_width()`. |
| Update already queued | `update_pending`. |

The state object should make illegal combinations hard to express. For example:

- document/style mutation must imply static dirty and content modified since
  capture;
- vertical scroll may set static dirty and scroll intent, but must not mark
  content modified since capture by itself;
- caret blink must be able to mark snapshot dirty without marking overlay
  dirty, preserving the blink-only shortcut;
- style sync should only be consumed for static captures, matching the current
  `style_sync_needed && static_content_dirty` call into `current_render_frame()`.

## Required Invariants

### Correctness

- No under-invalidation. If the old code recaptured, the migrated path must
  recapture unless a test and benchmark prove the narrower path is intended.
- Unknown dispatch messages must still schedule conservative full resync.
- `send()`, `sends()`, and direct callbacks must continue to share the same
  dispatch behavior.
- The `syncQuickViewProperties()` re-entry guard must remain effective.
- Scroll-width reset behavior must remain tied to dispatch rules and
  notification branches that explicitly reset it today.
- Vertical scroll reuse must remain disabled after content/style mutations
  until a fresh capture clears the mutation marker.
- Caret blink must remain a snapshot-only fast path.
- `updatePaintNode()` and renderer code must not call Scintilla or mutate
  invalidation state.

### Performance

- Do not add heap allocations or payload copies on the render hot path.
- Do not convert caret movement, selection movement, or caret blink into
  property-sync-heavy paths.
- Do not add linear scans to frequent update scheduling.
- Do not create new renderer nodes or change renderer node ownership in Phase
  3A.
- Do not build extra `Render_frame` or `Render_snapshot` instances beyond the
  current paths.
- Preserve profiling counters or explicitly map them to equivalent counters.

### API And Scope

- No public API changes are required for Phase 3A implementation.
- No `Render_frame` field changes.
- No scene-graph renderer changes.
- No dispatch classification changes unless a separate dispatch package
  approves them.
- No Package 3B batching or Package 3C property snapshot implementation inside
  3A.

## Migration Plan

### Phase 3A.0: Baseline And Review

Write scope:

- docs only, including this document and later status updates.

Required before code:

- two architecture reviewers agree that the mapped current state is complete;
- one performance reviewer agrees on the benchmark scenarios and counters;
- one test reviewer agrees on the focused test plan.

### Phase 3A.1: Introduce Equivalent State Object

Future write scope:

- `src/public/scintillaquick_item.cpp`;
- optionally a new private, non-installed helper under `src/public/` or
  `src/core/`;
- no public header changes unless reviewers approve a private nested type
  migration inside the existing installed header's private section.

Implementation shape:

- move only flag storage and OR/coalescing helpers into the state object;
- keep `request_scene_graph_update(...)` signature and behavior;
- keep `build_render_snapshot()` decisions behavior-equivalent;
- keep caret blink direct snapshot-dirty scheduling behavior-equivalent;
- add debug/test-only state dump only if needed by tests.

Tests:

- existing CI-compatible subset;
- smoke tests for `sends()` mutation and direct callback mutation;
- focused caret-blink fast-path test or profiler assertion proving a blink
  tick stays snapshot-only and does not increase static/full capture counts;
- frame-validation scroll-reuse tests;
- dispatch-table tests.

Review gate:

- reviewers compare pre/post scheduling behavior for all current writer groups.

### Phase 3A.2: Migrate Dispatch Application

Future write scope:

- `src/public/scintillaquick_item.cpp`;
- targeted smoke/dispatch tests only if behavior is asserted more directly.

Implementation shape:

- replace duplicate `send()` / `sends()` apply lambdas with one helper that
  applies dispatch intent through `RenderInvalidationState`;
- preserve property-sync skip for overlay-only movement;
- preserve `m_in_sync_quick_view_properties` behavior;
- preserve direct callback routing through non-virtual
  `ScintillaQuick_item::send()`.

Tests:

- `test_sends_syncs_properties`;
- direct callback property sync tests;
- dispatch recursion-safety tests;
- caret movement overlay-only dispatch tests.

### Phase 3A.3: Migrate Notifications And Scroll/Event Paths

Future write scope:

- `src/public/scintillaquick_item.cpp`;
- possibly focused smoke tests for touch/drag/drop only if behavior changes are
  approved.

Implementation shape:

- convert notification branches from boolean triples and direct mutation-marker
  writes to semantic state calls;
- convert wheel, scroll, focus, mouse, key, and IME scheduling to semantic
  calls;
- preserve current touch and drag/drop behavior unless tests explicitly approve
  additional scheduling;
- keep Scintilla notification ordering and signal behavior unchanged.

Tests:

- smoke coverage for wheel scroll, Ctrl+wheel zoom, middle-click paste, IME
  read-only/protected handling, and notification update paths;
- focused fine-timer/autoscroll validation covering timer-driven vertical
  scroll reuse eligibility and horizontal-scroll static recapture behavior;
- frame-validation scroll reuse;
- Linux visual-baseline gate if any render-visible scheduling behavior changes.

### Phase 3A.4: Snapshot Consumption Cleanup

Future write scope:

- `src/public/scintillaquick_item.cpp`;
- tests under `tests/frame_validation` and `tests/smoke` if observable paths
  are clarified.

Implementation shape:

- make `build_render_snapshot()` consume a read-only view of invalidation
  state;
- keep the four current capture paths intact;
- keep clearing state at exactly one successful snapshot boundary;
- keep scroll history and capture-base updates behavior-equivalent.

Tests:

- frame validation including vertical scroll reuse;
- smoke tests for focus/caret visibility and edit/scroll interaction;
- CI-compatible subset.

### Phase 3A.5: Documentation And Status Closeout

Future write scope:

- `docs/maintenance_invariants.md`;
- `docs/review_remediation_plan.md`;
- this document.

Update the current-state docs only after implementation and reviews are green.
Do not update docs ahead of code in a way that claims future behavior is
already implemented.

## Future Write Scopes

Keep implementation packages narrow:

| Package | Allowed source writes | Not allowed |
| --- | --- | --- |
| 3A.1 equivalent state object | `src/public/scintillaquick_item.cpp`, optional private helper file, focused tests | Renderer, dispatch classification, public API. |
| 3A.2 dispatch application | `src/public/scintillaquick_item.cpp`, tests | Dispatch table rules unless separate review approves. |
| 3A.3 notifications/events | `src/public/scintillaquick_item.cpp`, focused tests | Signal signature changes, renderer changes. |
| 3A.4 snapshot consumption | `src/public/scintillaquick_item.cpp`, frame/smoke tests | `Render_frame` schema changes, scene-graph renderer changes. |
| 3A docs closeout | docs only | Source changes. |

If a future agent needs to touch `src/core/scintillaquick_core.cpp`, the change
must be limited to how core timers or direct callbacks report invalidation
intent to the item. It must not change Scintilla core behavior.

## Review Gates

Every implementation subpackage needs:

- two independent code reviewers;
- one performance reviewer for any change that affects scheduling frequency,
  property sync, blink-only updates, or scroll reuse;
- one test reviewer for new or changed tests;
- final integration review after all subpackages are combined.

Reviewers should explicitly answer:

- Did any current writer lose a dirty flag?
- Did any overlay-only path become static/full capture?
- Did any static mutation keep vertical scroll reuse enabled?
- Did any dispatch path stop going through the central dispatch table?
- Did any direct callback bypass property sync or scheduling?
- Did caret blink remain cheap?
- Did touch or drag/drop behavior change, and if so is there focused coverage?
- Did `build_render_snapshot()` still own the final capture/reuse decision?

## Validation Gates

Minimum local gate for each code subpackage:

```powershell
$buildDir = "build/remediation-3a-<subpackage>"
cmake -S . -B $buildDir -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSCINTILLAQUICK_BUILD_TESTS=ON -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON
cmake --build $buildDir --config Release
ctest --test-dir $buildDir -C Release --output-on-failure -E "scintillaquick_visual_regression_test|scintillaquick_embedded_benchmark"
```

Focused tests to require where relevant:

- `scintillaquick_dispatch_table_test`;
- `scintillaquick_smoke_test` dispatch/direct/event/IME coverage;
- `scintillaquick_frame_validation_test`, especially vertical scroll reuse;
- caret blink fast-path coverage that asserts blink-only/snapshot-only capture
  and no static/full capture count increase;
- core fine-timer/autoscroll coverage for vertical scroll reuse eligibility and
  horizontal scroll static recapture;
- notification smoke tests when notification scheduling is touched.

Visual gate:

- required on the Linux visual-baseline runner for render-visible scheduling
  changes, scroll-reuse changes, or any change that affects captured static
  content.

Benchmark gate:

- required for changes that may alter update count, property-sync frequency,
  capture path selection, scroll reuse, caret movement, or blink-only behavior.

Recommended benchmark scenarios from the existing embedded benchmark:

- `caret_move_right_5000`;
- `caret_step_right_latency_64`;
- `caret_step_left_latency_64`;
- `vertical_scroll_step_latency_64`;
- `vertical_scroll_bounce_latency_48`;
- `vertical_wheel_bounce_latency_48`;
- `wrapped_wheel_bounce_latency_24`;
- `scroll_after_edit_latency_32`;
- `selection_drag_latency_48`;
- `insert_character_2000` if document-edit scheduling is touched.

Follow `docs/performance_optimization_process.md`: capture at least five
baseline and five post-change runs on the same machine, compare distributions,
and inspect profiler output. Do not claim performance improvement or accept
slowdown from a single run.

## Metrics Required Before Implementation

Before a 3A implementation agent edits source, capture or define the baseline
metrics that reviewers will compare:

- `update_requests`;
- `snapshot_build_count`;
- `blink_only_update_count`;
- `overlay_only_update_count`;
- `full_update_count`;
- benchmark paint latency distribution for selected scenarios;
- profiler totals for `item.update_polish`, `item.build_render_snapshot`,
  `item.update_paint_node`, and `item.update_quick_view`;
- scroll command counters for vertical and horizontal scroll scenarios.

If a required metric is missing from the current profiler output, add a
metrics-only design before changing scheduling behavior. Do not combine metrics
instrumentation with behavior-changing invalidation cleanup unless reviewers
explicitly approve it.

## Rollback Strategy

Each implementation phase should be revertible independently.

- Keep `request_scene_graph_update(...)` as a compatibility wrapper until all
  callers are migrated and reviewed.
- Keep `Render_data` storage layout changes minimal at first so a rollback can
  restore direct flags without touching renderer code.
- Do not remove the old boolean scheduling path until tests and benchmarks are
  green on the semantic path.
- If performance regresses, revert the specific migration phase rather than
  narrowing invalidation ad hoc.
- If a visual regression appears, prefer reverting the latest scheduling change
  and adding a focused test before trying to patch around it.

## Explicit Deferrals

This design does not authorize:

- Package 3B scoped editor update batching;
- Package 3C property snapshot work;
- renderer primitive batching;
- scene-graph node-pool rewrites;
- `Render_frame` schema changes;
- generated dispatch output;
- public API changes;
- changes to vendored Scintilla.

Those remain separate packages with their own design, review, visual, and
benchmark gates.
