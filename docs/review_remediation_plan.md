# Review Remediation Plan

This plan turns the validated review findings into conservative work packages
for implementation agents. Do not edit `third_party/scintilla` except for
read-only reference checks.

## Operating Rules

- Keep each implementation branch/worktree scoped to one package below.
- Prefer small, testable patches over broad rewrites.
- Do not mix correctness fixes with renderer or performance refactors.
- Preserve public API compatibility unless a package explicitly moves the work
  into an API-review package.
- **Phase 1 API constraint:** no public signal signature changes, virtual method
  signature changes, or public class-layout changes. In Phase 1, do not replace
  or remove public signals.
- Treat performance-sensitive changes as unsafe until baseline and post-change
  measurements are recorded.
- Do not make ad hoc platform `Window::Destroy()` ownership changes. Ownership
  work requires the design note described in Package 1F.

## Review Gates

1. **Plan gate:** before implementation starts, two independent review agents
   review this document for sequencing, write-scope conflicts, test gaps,
   platform assumptions, API risk, and performance risk. Revise until both are
   green.
2. **Implementation batch gate:** every implementation batch needs at least two
   independent review agents who did not write the code.
3. **Iteration rule:** if either reviewer finds a blocker, the owning
   implementation agent fixes it and the same batch is reviewed again until both
   reviewers are green.
4. **Integration gate:** after a phase is merged together, run the applicable
   validation gates below and have two final reviewers inspect the integrated
   diff for cross-package regressions.

## Current Status / Next Gate

**Completed and green locally:**

- First-wave correctness hardening is integrated and reviewed green: Packages
  1A, 1B, 1C, 1D, and 1F, plus the production translation fix for Package 1E.
- Phase 1E closure is integrated and reviewed green: vertical-scroll reuse
  frame-validation tests now cover the secondary geometry families identified
  by reviewers.
- Package 2A is integrated and reviewed green: `selectCurrentWord()` delegates
  word-boundary behavior to Scintilla `Document::ExtendWordSelect(..., true)`
  and has focused smoke coverage for configured word characters.
- Package 2B is integrated and reviewed green: maintenance invariants, source
  map, invalidation, dispatch/direct-callback, scene-graph threading, IME,
  platform ownership, signal-lifetime, and test/benchmark playbook docs are in
  place.
- Package 2C implementation is integrated and reviewed green locally:
  `ScintillaQuick_notification`,
  `notificationReceived(const ScintillaQuick_notification&)`, queued metatype
  registration, owned notification payload copying, `lParam` classification,
  smoke coverage, and notification API docs are in place.
- Package 2D is integrated and reviewed green: `UpdateInfos()` was removed
  after audit, and `PartialPaint()` / `PartialPaintQml()` are documented as
  raster validation oracle support rather than a second production renderer.
- Package 2E design is complete and reviewed green:
  [`docs/test_access_api_boundary.md`](test_access_api_boundary.md)
  documents the installed test-only API exposure and defers implementation to
  a future accepted API cleanup window.
- Package 2F Stage 1 is implemented, reviewed green, and final-validated:
  [`docs/dispatch_table_maintainability.md`](dispatch_table_maintainability.md)
  documents the current centralized sorted constexpr `Message_rule` table,
  binary-search lookup, sorted/unique compile-time check, intact helper APIs,
  conservative unknown fallback, and automated dispatch equivalence sweep.
- Package 2F Stage 2 is implemented, reviewed green, and final-validated:
  `tools/dispatch/audit_dispatch_table.py` provides an audit-only
  `Scintilla.iface` inventory check against the Stage 1 rule table. It reports
  drift and candidates without changing runtime dispatch behavior or generating
  production output.
- Dispatch audit triage baseline is complete and reviewed green:
  [`docs/dispatch_audit_triage.md`](dispatch_audit_triage.md) records the
  current Stage 2 warning baseline, accepted conservative fallback risks, and
  Scintilla-upgrade triage rules without changing runtime dispatch behavior.
- Phase 3A render invalidation design is complete and reviewed green:
  [`docs/render_invalidation_state_design.md`](render_invalidation_state_design.md)
  maps the current dirty-state writers, scheduling paths, dispatch re-entry
  behavior, scroll reuse, caret blink, timer/autoscroll gates, and validation
  requirements. Phase 3A implementation is not started.
- Phase 3A metrics-baseline design is complete and reviewed green:
  [`docs/render_invalidation_metrics_baseline.md`](render_invalidation_metrics_baseline.md)
  defines the required pre-implementation correctness and scheduling baseline.
  It is design-only and does not authorize metrics instrumentation,
  invalidation source changes, renderer changes, or Phase 3A implementation.

**Latest integration validation:**

- Package 2C final validation passed: static and shared MinGW Release
  configure/builds, CI-compatible CTest subset 3/3 in both build variants, and
  targeted smoke coverage.
- `git diff --check` passed with only LF-to-CRLF working-copy warnings.
- Markdown link checks and trailing-whitespace scans for changed docs passed.
- Package 2D follow-up validation passed in `build/remediation-2d-mingw`:
  fresh MinGW configure/build, frame-validation test, CI-compatible CTest
  subset 3/3, and scoped `git diff --check`.
- Package 2F Stage 1 final validation passed: fresh static and shared MinGW
  Release configure/builds, CI-compatible CTest subset 3/3 in both build
  variants, dispatch-table test coverage in both variants, `git diff --check`,
  and changed-doc link/trailing-whitespace checks.
- Package 2F Stage 2 final validation passed: direct
  `python tools/dispatch/audit_dispatch_table.py --check`,
  `python -m py_compile tools/dispatch/audit_dispatch_table.py`, fresh MinGW
  Release configure/build, CI-compatible CTest subset 4/4 including
  `scintillaquick_dispatch_iface_audit_test`, targeted dispatch audit/table
  CTest 2/2, `git diff --check`, and changed-doc link/trailing-whitespace
  checks.
- Package 2E and 2F docs passed doc review, `git diff --check`, and direct
  trailing-whitespace scans.
- Dispatch audit triage, Phase 3A design docs, and Phase 3A metrics-baseline
  design docs passed green review, scoped `git diff --check`, and direct
  trailing-whitespace scans.
- Linux visual baselines were not run.
- The embedded benchmark target was built but not run. Expensive benchmarks
  were not run because Package 2C did not touch renderer, invalidation,
  scrolling, or scene-graph hot paths; the no-safe-receiver copy gate was
  verified in code. Do not treat this as a general performance validation
  claim.

**Next recommended gate:**

1. Treat Phase 3A as design-complete but implementation-not-started. The next
   gate is metrics baseline evidence capture and/or a metrics-only
   instrumentation planning package, not invalidation source implementation.
   Missing attribution counters still block scheduling-sensitive Phase 3A work
   for coalescing, scroll reuse, property sync, IME, timer/autoscroll,
   notifications, dispatch attribution, and caret blink unless reviewers
   explicitly accept the existing counters for a narrowly scoped first slice.
2. Keep Package 2E implementation deferred. Its design is green, but moving
   installed test-only API surface should wait for an accepted API cleanup
   window and must not overlap with future public-header or meta-object churn.
3. Package 2F Stage 3 generated output remains a later decision gated by
   audit stability, reproducibility checks, and review.
4. If Phase 3A implementation remains paused after baseline capture, run
   another read-only planning pass before authorizing any Phase 3 source
   implementation.
5. Treat Package 3B and Package 3C as dependent architecture work after 3A
   design settles. They are not quick cleanup packages.
6. Treat Phase 4 renderer work as metrics-first and review-first. No renderer
   batching or node-pool refactor should start before baseline metrics identify
   the target hotspot.

## Validation Gates

Do not require a universal whole-suite CTest run on every platform. The
repository has platform-specific caveats in
[`docs/limitations.md`](limitations.md): visual baselines are Linux-only,
font rendering differs on Windows/macOS, the benchmark target is built in CI
but not executed there, and Windows visual tests require an interactive desktop
session.

### CI-Compatible Correctness Subset

Use this as the normal local gate on this Windows workspace and as the portable
cross-platform correctness gate. It intentionally excludes visual regression and
benchmark execution. Use a fresh package-specific build directory for every
package, for example `build/remediation-1a`; do not reuse stale `build`
contents. If a package touches examples or public API behavior that examples
exercise, configure `-DSCINTILLAQUICK_BUILD_EXAMPLES=ON` as well.

```powershell
$buildDir = "build/remediation-<package-id>"
cmake -S . -B $buildDir -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSCINTILLAQUICK_BUILD_TESTS=ON -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON
cmake --build $buildDir --config Release
ctest --test-dir $buildDir -C Release --output-on-failure -E "scintillaquick_visual_regression_test|scintillaquick_embedded_benchmark"
```

This subset should include smoke, dispatch-table, frame-validation, and any new
focused tests added by the package.

### Linux Visual-Baseline Gate

Run visual regression only on a Linux runner configured for the existing PNG
baselines and deterministic test font. Do not use Windows/macOS failures against
the Linux PNG baselines as a blocker unless per-platform baselines have been
added.

```bash
build_dir=build/remediation-visual-<package-id>
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSCINTILLAQUICK_BUILD_TESTS=ON -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON
cmake --build "$build_dir" --config Release
ctest --test-dir "$build_dir" -C Release --output-on-failure -R scintillaquick_visual_regression_test
```

Windows visual runs, if requested manually, need an interactive desktop session
and the `windows` Qt platform plugin; they are diagnostic, not the baseline gate.

### Local or Dedicated Benchmark Gate

Benchmarks are not a normal CI pass/fail signal. For performance-sensitive
packages, follow
[`docs/performance_optimization_process.md`](performance_optimization_process.md):
run the relevant benchmark scenario at least five times before and after the
change from a fresh benchmark build directory, record the distribution and
profiler output, and require changes to beat the measured noise floor.

```powershell
$buildDir = "build/remediation-benchmark-<package-id>"
cmake -S . -B $buildDir -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSCINTILLAQUICK_BUILD_TESTS=ON -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON -DSCINTILLAQUICK_BUILD_EXAMPLES=ON
cmake --build $buildDir --config Release
# Run the selected benchmark scenario from this build directory at least 5 times.
```

## Phase 1: Correctness Hardening

Goal: fix high-confidence correctness and lifetime issues with minimal design
change.

**Status:** complete and reviewed green in the current working tree. The
implementation was integrated through the first-wave and current-wave gates.
Linux visual baselines remain an external validation gap, not a local blocker.

### Package 1A: Public Item Dispatch, Signals, Events, and IME

**Status:** complete and reviewed green. The integrated change covers
`send()`/`sends()` dispatch unification, owned `modified()` bytes,
`geometryChange()`, event acceptance, IME bounds/acceptance, read-only cursor
updates, and focused dispatch/event tests. The legacy
`notify(NotificationData*)` pointer signal remains unchanged by design; Package
2C added the safer queued C++ snapshot API while preserving the legacy pointer
signal.

**Write scope:** `include/scintillaquick/scintillaquick_item.h`,
`src/public/scintillaquick_item.cpp`, `src/core/scintillaquick_dispatch_table.h`
if classification changes are needed, targeted tests under `tests/smoke`,
`tests/dispatch_table`, or a new focused test target.

**Findings:**

- Copy owned bytes for the `modified()` signal; do not use
  `QByteArray::fromRawData` for notification text.
- Do not change, replace, or remove `notify(NotificationData*)` in Phase 1. The
  raw stack-pointer notification signal remains a legacy direct-only hazard;
  use the Package 2C snapshot API for queued-safe C++ notification delivery.
- Route `sends()` through the same dispatch/invalidation path as `send()`.
- Preserve read-only classifications for string getter messages such as
  `SCI_STYLEGETFONT`.
- Call `QQuickItem::geometryChange()` unconditionally.
- Do not suppress cursor/input-method UI updates in read-only mode.
- Use event-local keyboard modifiers in `keyPressEvent()`.
- Clamp IME preedit attribute ranges for `TextFormat`, `Cursor`, and
  `Selection` attributes.
- Define and test read-only/protected-text IME acceptance, cancellation, and
  update behavior.
- Explicitly accept and schedule handled input-method events.
- Accept consumed wheel events and middle-click paste paths.

**Tests to add/run:**

- Mutating `sends()` calls update QML-facing properties and paint/render state.
- String getter `sends()` calls do not spuriously invalidate or recurse.
- Queued or delayed `modified()` receiver can safely retain copied
  `QByteArray` text after signal delivery.
- Geometry position-only changes still reach base behavior where observable.
- Read-only cursor, selection, and input-method UI updates still happen.
- Malformed IME `TextFormat`, `Cursor`, and `Selection` attributes are clamped
  or ignored without out-of-bounds access.
- Read-only/protected IME commit/cancel paths have explicit acceptance and
  repaint behavior.
- Wheel and middle-click consumed paths set event acceptance.

### Package 1B: Core Direct Callbacks and Guards

**Status:** complete and reviewed green. Direct callbacks now follow the
documented dispatch policy, direct status capture is guarded, MIME/direct-pointer
helpers have defensive checks, and targeted smoke/dispatch coverage is present.

**Write scope:** `src/core/scintillaquick_core.cpp`,
`src/core/scintillaquick_core.h` if needed,
`src/core/scintillaquick_dispatch_table.h` if classification changes are
needed, targeted tests under `tests/dispatch_table`, `tests/smoke`, or a new
focused test target.

**Findings:**

- Add a direct Scintilla function policy for `SCI_GETDIRECTFUNCTION` and
  `SCI_GETDIRECTSTATUSFUNCTION`. Dispatch unification is not considered fixed
  until direct callback mutations either route through the same dispatcher or an
  explicit raw-bypass API policy and mutation test prove the behavior is
  intentional and documented.
- Add the core-side status pointer guard for `DirectStatusFunction()`.
- Add core MIME helper guards where helper inputs can be null.

**Tests to add/run:**

- Direct-function mutation policy test: direct callback mutation updates state
  through the unified path, or a documented raw-bypass test captures the chosen
  contract.
- Null status pointer and MIME helper guard tests where feasible.
- CI-compatible correctness subset.

### Package 1C: Core Frame Capture

**Status:** complete and reviewed green. `current_render_frame()` now restores
temporary `view.bufferedDraw` state with scope safety and guards line-height
dependent estimates.

**Write scope:** `src/core/scintillaquick_core.cpp`,
`src/core/scintillaquick_core.h` if needed, targeted tests under
`tests/frame_validation` or `tests/smoke`.

**Findings:**

- Add a scope guard around temporary `view.bufferedDraw` changes in
  `current_render_frame()`.
- Guard line-height dependent estimates against zero or negative line height.

**Tests to add/run:**

- CI-compatible correctness subset, including existing frame validation.
- Any existing scenario that waits for paint boundaries, to catch extra paint
  churn.

### Package 1D: Platform Guards and UTF-8 Robustness

**Status:** complete and reviewed green. Platform null/state guards, invalid
UTF-8 forward progress, requested-rectangle copy behavior, and embedded-NUL
list-box value handling are integrated with focused coverage where feasible.

**Write scope:** `src/platform/scintillaquick_platqt.cpp`,
`src/platform/scintillaquick_platqt.h` if needed, targeted platform tests if
available or new focused smoke coverage.

**Findings:**

- Add cheap null/state guards for font casts, surface device use, menu display,
  empty polylines/polygons, and empty images.
- Make UTF-8 measurement always advance on invalid bytes and never read beyond
  the input.
- Cast bytes to `unsigned char` before byte-table or DBCS helper use.
- Fix `Surface_impl::Copy()` to copy only the requested source rectangle.
- Preserve embedded NUL bytes in `List_box_impl::GetValue()`.
- Do not change platform window ownership or `Window::Destroy()` behavior in
  this package. That belongs to Package 1F after design review.

**Tests to add/run:**

- CI-compatible correctness subset.
- Focused measurement test with invalid UTF-8 byte sequences.
- Focused empty-shape/image/menu/device guard tests where the platform API can
  be exercised without broad new test hooks.

### Package 1E: Render Frame Translation Coverage

**Status:** complete and reviewed green for production code and local
frame-validation coverage. The translation implementation covers all current
item-coordinate geometry fields found in `Render_frame`; tests now compare
cached vertical-scroll reuse against direct capture for represented/blob text,
indicator secondary geometry, whitespace, margin text, annotations, EOL
annotations, indent guides, and decoration underlines. Linux visual baselines
were not run.

**Write scope:** render-frame translation code in
`src/public/scintillaquick_item.cpp`, render-frame structs only if required,
tests under `tests/frame_validation` and `tests/frame_visual_regression`.

**Finding:**

- Translate every item-coordinate geometry field during vertical scroll reuse,
  including `Text_run` top/bottom/blob rectangles and indicator sub-rectangles.

**Tests to add/run:**

- Vertical scroll reuse with represented/control characters, blob text,
  indicators, whitespace, annotations, and margins visible.
- CI-compatible correctness subset.
- Linux visual-baseline gate after merge or on a Linux runner.

### Package 1F: Platform Window Ownership Design and Targeted Hardening

**Status:** complete and reviewed green. The ownership contract is documented in
[`docs/platform_window_ownership.md`](platform_window_ownership.md), and the
implementation hardens owned versus borrowed window handles, external deletion,
call-tip reuse/recreate, autocomplete/list-box lifecycle, resolver fallback, and
stale-handle behavior.

**Completed design scope:** the design note
`docs/platform_window_ownership.md`, describing main editor, popup/list-box,
call-tip, platform `Window`, and `Surface_impl` ownership. This design was
reviewed green before code changes.

**Completed implementation scope:** `src/platform`, `src/core` window
creation/destruction paths, and targeted lifecycle tests.

**Findings:**

- Make ownership of the main editor item versus platform-created popup/list-box
  and call-tip items explicit.
- Harden stale raw `WindowID` handling for every consumer, not only
  `Window::Destroy()`. The approved design must require either clearing owning
  `wid` slots on `QObject::destroyed` or retaining destroyed-owned tombstones so
  deleted owned handles cannot be mistaken for unregistered borrowed handles.
- Route or otherwise guard all `WindowID` consumers under the approved policy,
  including `Window` show, position, invalidation, cursor, monitor-rect, and
  destroy paths plus `List_box_impl::GetWidget()`.
- Reposition and refresh existing call tip items, not only newly created ones,
  under the reviewed ownership model.
- Add targeted hardening before broad Phase 3 architecture work; do not defer
  this high-severity lifetime risk to generic cleanup.
- Continue to avoid vendored Scintilla edits, public API/ABI changes, or ad hoc
  `Window::Destroy()` changes outside the documented ownership policy.

**Tests to add/run:**

- Focused call-tip reuse test that opens two call tips at different positions.
- Popup/list-box lifecycle test if feasible.
- Required stale Qt deletion coverage: externally or parent-delete an owned
  call-tip/list-box item, then exercise later show, position, invalidation,
  cursor, monitor-rect, destroy, and list-box access paths. If a path is not
  practical to exercise, document infeasibility and require reviewer inspection
  proving it is guarded by the resolver, tombstone, or handle-clearing policy.
- CI-compatible correctness subset.

## Phase 2: API Safety and Internal Contracts

Goal: make future agent work safer and address high-severity API hazards that
require compatibility review.

### Package 2A: Word Boundary Behavior

**Status:** complete and reviewed green. `selectCurrentWord()` now delegates to
Scintilla word-boundary logic, with smoke coverage for configured word
characters. Non-ASCII behavior is delegated to Scintilla but is not separately
asserted by the current smoke test.

**Write scope:** `src/core/scintillaquick_core.cpp`, targeted smoke tests.

Replace ASCII-oriented `selectCurrentWord()` logic with Scintilla word-boundary
behavior. Test lexer/configured word characters and non-ASCII text where
Scintilla supports it.

### Package 2B: Maintenance Documentation

**Status:** complete and reviewed green. The docs now cover the source map,
vendored Scintilla boundary, render-frame invariants, invalidation contract,
dispatch/direct-callback policy, scene-graph threading rules, platform ownership
contract, IME notes, signal lifetime rules, and test/benchmark playbook.

**Write scope:** `docs/*.md`, `README.md` only for a short boundary note.

Add concise internal docs for source map and vendored Scintilla boundary,
render-frame coordinate and translation invariants, invalidation flag contract,
dispatch-table policy including direct callbacks, scene-graph threading rules,
platform ownership rules, IME/composition model, signal lifetime rules, and the
test/benchmark playbook.

### Package 2C: Notification API Compatibility

**Status:** complete and reviewed green locally. Package 2C added the exported
`ScintillaQuick_notification` snapshot type, append-only
`notificationReceived(const ScintillaQuick_notification&)` signal, queued
metatype registration, owned notification payload copying, `lParam`
classification, smoke tests, and updated notification API docs.

Two implementation review rounds found and fixed compatibility/performance
issues before final green status: existing `modified(...)` now preserves legacy
post-`notify(...)` mutation semantics, and the safe snapshot no longer exposes
raw pointer-valued `lParam` while unchanged `SCN_MODIFIED` text reuses the
snapshot `QByteArray` storage.

**Write scope:** public header, `src/public`, examples/docs/tests.

This is not low-risk cleanup. The existing `notify(NotificationData*)` signal
passes a pointer whose payload is valid only during direct synchronous delivery,
and the legacy `macroRecord(message, wParam, lParam)` signal is direct-only for
pointer-bearing macro messages. Package 2C keeps those legacy signals
compatible and adds the safer queued C++ snapshot API under the approved design.
Do not remove or change existing signal signatures without an explicit
breaking-change decision.

### Package 2D: Legacy Hook Closure

**Status:** complete and reviewed green locally. `UpdateInfos()` was audited
and removed because it was not required by Scintilla's interfaces and had no
local call sites. `PartialPaint()` / `PartialPaintQml()` were retained with
documentation as raster reference oracle support for validation tests, not as a
second production renderer.

**Write scope:** `src/core/scintillaquick_core.cpp`,
`src/core/scintillaquick_core.h` if declarations change,
`tests/support/scintillaquick_validation_access.h`, targeted tests, and a short
maintenance-doc note if behavior is intentionally retained.

Completed audit result: `UpdateInfos()` was removed, and the retained
`PartialPaint()` / `PartialPaintQml()` path is documented as validation support
for the raster reference oracle.

Do not mix this package with notification API changes, public-header cleanup,
renderer work, invalidation refactors, or performance claims.

### Package 2E: Test-Only Public API Boundary

**Status:** design complete and reviewed green; implementation deferred.
[`docs/test_access_api_boundary.md`](test_access_api_boundary.md) recommends a
staged future cleanup rather than immediate code changes: centralize in-tree
test and benchmark access through non-installed validation support first, move
the installed row DTO only during an accepted API cleanup window, and leave
broader private-header/PIMPL cleanup for a later package.

**Write scope:** design doc first. Later implementation may touch
`include/scintillaquick/scintillaquick_item.h`, `tests/support`, test CMake, and
install/export docs if the reviewed design approves it.

`Displayed_row_for_test` and related test-access declarations remain in the
installed public header. The green design defers moving them because this is
installed-header/source-compatibility cleanup, not a correctness or performance
blocker. Because future implementation changes the public header/API boundary,
keep it serialized away from public-signal/meta-object churn and run
static/shared build validation plus an install/consumer smoke test if one is
available.

### Package 2F: Dispatch Table Maintainability

**Status:** Stage 1 complete, reviewed green, and final-validated. Stage 2 is
complete, reviewed green, and final-validated. The Stage 2 audit triage
baseline is complete and reviewed green in
[`docs/dispatch_audit_triage.md`](dispatch_audit_triage.md). Stage 3 is not
started.
[`docs/dispatch_table_maintainability.md`](dispatch_table_maintainability.md)
documents the staged path. Stage 1 consolidated the previous decision surfaces
into one behavior-equivalent sorted constexpr `Message_rule` table with binary
search, a sorted/unique compile-time check, helper APIs intact, conservative
unknown fallback, and automated equivalence coverage over the swept message
range. Stage 2 added an audit-only `Scintilla.iface` parser/inventory check,
with no runtime dispatch output generation. Stage 3 generated output is a later
decision only after audit stability, reproducibility, and review gates are met.

**Completed Stage 1 write scope:** `src/core/scintillaquick_dispatch_table.h`
and `tests/dispatch_table/main.cpp`.

**Completed Stage 2 write scope:** `tools/dispatch/audit_dispatch_table.py`,
`tests/CMakeLists.txt`, `docs/dispatch_table_maintainability.md`, and this
plan. Stage 2 did not edit `src/core/scintillaquick_dispatch_table.h` or any
runtime dispatch/source behavior.

**Future write scope:** Stage 3 may touch generator scripts, checked-in
overrides, generated headers, and build integration if approved.

Stage 1 preserved the conservative unknown-message full-resync fallback, uses
binary-search lookup over the sorted constexpr table, and added mandatory
automated equivalence coverage for every swept message and all caller-visible
output fields: `needed`, `static_content_dirty`, `needs_style_sync`,
`scrolling`, and `scroll_width_reset`. Do not combine future dispatch work with
renderer batching, invalidation refactors, or Phase 4 scene-graph changes.

Stage 2 parses `third_party/scintilla/include/Scintilla.iface` and the manual
Stage 1 table, then reports message inventory, manual-table coverage,
unclassified messages, drift/duplicates/parse gaps, getter/mutator ambiguity,
and override candidates. `--check` fails only on structural issues such as parse
failures, duplicate manual table entries, rule entries missing from iface
without an explicit compatibility explanation, `set` entries classified
`Read_only`, or detectable nondeterministic ordering. Unclassified messages and
heuristic candidates remain warnings.

## Phase 3: Architecture Stabilization

Goal: reduce repeated invalidation and synchronization logic after Phase 1 and
the ownership hardening package are stable.

### Package 3A: RenderInvalidationState

**Status:** design complete and reviewed green; implementation not started.
[`docs/render_invalidation_state_design.md`](render_invalidation_state_design.md)
is the implementation-planning input and does not authorize source changes by
itself. The companion metrics-baseline design
[`docs/render_invalidation_metrics_baseline.md`](render_invalidation_metrics_baseline.md)
is also reviewed green, but it authorizes only baseline evidence capture or a
separate metrics-only instrumentation plan/package.

**Future write scope:** new internal helper under `src/public` or `src/core`,
plus call sites in `src/public/scintillaquick_item.cpp` and relevant tests.

Centralize dirty flags, property sync intent, scroll/update scheduling, and
caret/document/style invalidation. Require the CI-compatible correctness subset,
Linux visual-baseline gate for render-visible changes, and benchmark gate for
update/coalescing changes. Before implementation, capture the baseline evidence
defined by the metrics-baseline design or land a reviewed metrics-only
instrumentation package for the missing attribution counters, then run an
explicit planning and review pass that confirms the behavior-equivalent
extraction step, measured baseline, and rollback criteria.

### Package 3B: Scoped Editor Update Batching

**Status:** not started. Start only after Package 3A implementation planning
and review resolves ownership of dirty flags, property sync, scheduling, and
direct-callback interactions.

**Write scope:** `src/public/scintillaquick_item.cpp`, internal helper header if
needed, tests for multi-message public operations.

Batch high-level operations such as `setText()` so property sync and scene graph
updates happen once at scope exit. Measure paint/update counts before and after.

### Package 3C: Property Snapshot

**Status:** not started. Treat as behavior-preserving architecture work with
reviewed design and property-update tests before implementation.

**Write scope:** `src/public` internals and tests.

Replace repeated property queries with one internal snapshot where behavior is
equivalent. Add tests proving QML-facing properties still update correctly for
document edits, scroll changes, selection changes, and style changes.

## Phase 4: Renderer Performance and Simplification

Goal: reduce scene graph node count and renderer duplication only after metrics
show the target hotspots.

**Status:** not started. This phase is metrics-first and review-first; it is the
highest-intrusion remaining work.

1. Add renderer/text-cache metrics: cache hits, backup hits, misses, evictions,
   texture rebuilds, node counts by primitive family, and frame update time.
2. Run benchmark baselines at least five times for relevant scenarios, including
   scroll-wheel bounce and large-document editing.
3. Only then consider generic primitive layers, rectangle batching, generic node
   pools, and text primitive consolidation.
4. Require visual regression on the Linux visual-baseline runner and benchmark
   comparison for every renderer change.

Renderer refactors are high-intrusion work. They should be split into measured
steps that can be reverted independently.

## Support-Agent Verification Matrix

Support agents should verify that each implementation batch adds or updates
tests for the relevant row below before reviewer sign-off.

| Area | Required verification |
| --- | --- |
| Public dispatch and `sends()` | Paint/render invalidation and QML property updates for mutating `sends()`; no spurious invalidation for string getters. |
| Core direct callbacks and guards | Direct-function mutation policy test; null status pointer and MIME helper guard tests where feasible. |
| Notification text | Queued/delayed receiver retains `modified()` `QByteArray` safely after signal delivery. |
| Notification safe API implementation | Follow `docs/notification_api_compatibility.md`; verify metatypes, queued delivery, exhaustive scalar snapshot coverage, owned text and macro payload copies, QML behavior if exposed, and append-only signal compatibility. |
| Legacy hook closure | `UpdateInfos()` is removed or justified; `PartialPaintQml()` is isolated/documented as raster validation support if retained. |
| Test-only public API cleanup | Public-header compatibility, install/export behavior, and internal test-access replacement are reviewed before code changes; do not overlap with notification meta-object work. |
| Dispatch table maintainability | Design prevents under-invalidation, preserves conservative unknown-message fallback, and proves generated/table-driven output is reproducible if generation is introduced. |
| Vertical scroll reuse | Geometry translation tests for represented/control text, blob rectangles, indicators, whitespace, annotations, and margins. |
| Wheel/mouse events | Consumed wheel, Ctrl+wheel, and middle-click paste paths accept events and schedule needed updates. |
| IME | Malformed `TextFormat`, `Cursor`, and `Selection` attributes are clamped/ignored; read-only/protected commit and cancel behavior is explicit. |
| Read-only cursor UI | Cursor rectangle, selection handles, and cursor-change notifications update in read-only mode. |
| Platform guards | Invalid UTF-8 measurement, empty shape/image guards, null menu/device guards where feasible. |
| Performance-sensitive changes | Five-run pre/post benchmark loop with profiler output and noise-floor comparison before claiming improvement or accepting extra cost. |

## Suggested Batch Order

1. Completed: plan gate review, first-wave correctness hardening, Package 1F
   ownership design/implementation, Phase 1E test-coverage closure, Package 2A,
   Package 2B, Package 2C design and implementation, Package 2D, Package 2E
   design, Package 2F design, Package 2F Stage 1 implementation/final
   validation, Package 2F Stage 2 implementation/final validation, dispatch
   audit triage baseline, Phase 3A render invalidation design, and Phase 3A
   metrics-baseline design.
2. Dispatch generation gate: Package 2F Stage 3 remains deferred to a later
   proposal after audit stability, reproducibility, and review. Do not generate
   production dispatch output from the Stage 2 audit script.
3. Deferred public-header cleanup package: Package 2E implementation only in an
   accepted API cleanup window. Do not run test-only API cleanup concurrently
   with any future notification API follow-up that changes public headers or
   meta-object layout.
4. Next architecture gate: Phase 3A metrics baseline evidence capture and/or a
   metrics-only instrumentation planning package for missing attribution
   counters. Do not authorize Phase 3 source implementation from this plan
   status update alone, and do not begin until reviewers approve the measured
   baseline and the first behavior-equivalent extraction step.
5. Follow-on architecture packages: Package 3B and Package 3C only after 3A
   implementation planning is green. Require focused behavior tests and, for
   batching, measured paint/update counts.
6. Renderer/performance work: Phase 4 renderer metrics instrumentation first,
   benchmark baselines second, renderer simplification only after the hotspot
   and acceptable performance envelope are measured. Phase 3A invalidation
   metrics do not authorize renderer batching, node-pool, or primitive-layer
   refactors.
