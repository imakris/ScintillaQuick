# Remaining Remediation Roadmap

This document is a memory and handoff document. It records remediation work that
is still valid after the completed correctness, API-safety, dispatch, and design
packages. It does not authorize source implementation by itself.

Future maintainers and agents should treat this as the starting checklist for
the next planning pass. Before implementing any item, re-read the source and the
linked design documents because the branch may have moved.

## Status Snapshot

Completed locally and reviewed green:

- Phase 1 correctness hardening: public `send()` / `sends()` dispatch
  consistency, direct callback routing, notification text ownership,
  render-frame scroll translation, event/IME hardening, core frame-capture
  guards, platform guards, UTF-8 measurement robustness, and platform window
  ownership.
- Phase 1E validation closure: frame-validation coverage for cached vertical
  scroll reuse and secondary geometry families.
- Package 2A: `selectCurrentWord()` now delegates to Scintilla word-boundary
  logic.
- Package 2B: maintenance/source-map/invariant documentation.
- Package 2C: queued-safe C++ notification snapshot API
  `ScintillaQuick_notification` and
  `notificationReceived(const ScintillaQuick_notification&)`.
- Package 2D: `UpdateInfos()` removed; `PartialPaint()` / `PartialPaintQml()`
  documented as raster validation oracle support.
- Package 2E: test-only public API boundary design completed; implementation
  deferred.
- Package 2F Stage 1: centralized sorted constexpr dispatch `Message_rule`
  table with binary search and equivalence tests.
- Package 2F Stage 2: audit-only `Scintilla.iface` inventory script and CTest
  integration.
- Dispatch audit triage baseline.
- Phase 3A render invalidation state design.
- Phase 3A metrics-baseline design.

Known validation limitations:

- Linux visual baselines were not run in the completed local validation passes.
- The embedded benchmark target was generally built but not executed unless a
  package explicitly says otherwise.
- Benchmark non-execution for completed correctness/doc packages must not be
  treated as a general performance validation result.

Current next gate:

1. Capture Phase 3A metrics baseline evidence and/or plan a metrics-only
   instrumentation package for missing attribution counters.
2. Do not start `RenderInvalidationState` source implementation until reviewers
   approve the current baseline, required metrics, focused tests, and first
   behavior-equivalent extraction step.

## Global Operating Rules

- Do not edit `third_party/scintilla` unless the task is explicitly an upstream
  Scintilla update or vendored-dependency patch.
- Do not mix renderer refactors, invalidation refactors, dispatch
  classification changes, public API cleanup, and notification API changes in
  one package.
- Performance-sensitive changes need pre-change and post-change measurements,
  not single-run intuition.
- Unknown Scintilla messages must continue to use conservative full resync until
  a reviewed narrower rule is added.
- Public-header and Qt meta-object changes must be serialized and reviewed as
  API changes.
- Renderer work remains metrics-first. Phase 3A invalidation metrics do not
  authorize renderer batching, generic primitive layers, or node-pool rewrites.
- Any implementation package should have at least two independent code reviews,
  plus a performance reviewer when scheduling, rendering, or hot-path behavior
  can change.

## Roadmap Summary

| Order | Item | Status | First Gate |
| ---: | --- | --- | --- |
| 1 | Phase 3A-M0 metrics baseline evidence | Not started | Build and benchmark evidence package. |
| 2 | Phase 3A-M1 metrics-only instrumentation | Not started | Design/review if missing counters block 3A. |
| 3 | Phase 3A-T0 narrow observability/test guards | Not started | Tests only; no production behavior changes. |
| 4 | Phase 3A-P0 behavior-equivalent implementation plan | Not started | Plan from measured baseline and reviewed slices. |
| 5 | Phase 3A implementation slices | Not started | Behavior-equivalent source extraction only. |
| 6 | Phase 3B scoped editor update batching | Not started | Design after 3A ownership is stable. |
| 7 | Phase 3C property/view snapshot | Not started | Design and property-update tests. |
| 8 | Package 2E test-only public API cleanup | Deferred | API cleanup window. |
| 9 | Package 2F Stage 3 generated dispatch output | Deferred | Separate generation proposal. |
| 10 | Notification API follow-ups | Deferred | Compatibility policy decision. |
| 11 | PIMPL/public-header boundary cleanup | Deferred | Broad API/ABI design. |
| 12 | Phase 4 renderer metrics plan | Not started | Metrics design before source changes. |
| 13 | Phase 4 renderer simplification | Not started | Metrics identify hotspots first. |
| 14 | Scintilla upgrade and dispatch audit upkeep | Ongoing future maintenance | Run audit during upgrades. |
| 15 | Platform window wrapper-type cleanup | Optional future cleanup | Design-first, after current ownership policy remains stable. |
| 16 | Optional raster oracle isolation | Optional future cleanup | Coordinate with test-access/private-header work. |

## 1. Phase 3A-M0 Metrics Baseline Evidence

Status: not started. Design is complete in
[`render_invalidation_metrics_baseline.md`](render_invalidation_metrics_baseline.md).

Why it matters:

`RenderInvalidationState` is meant to be behavior-equivalent at first, but it
touches scheduling and capture-path decisions. Without a current baseline, a
future implementation can accidentally add full recaptures, property syncs,
missed scroll reuse, or caret-blink work and still pass functional tests.

Prerequisites and gates:

- Use the current branch tip. If source changes land before baseline capture,
  recapture from the new tip.
- Use Release builds with tests and benchmarks enabled.
- Record commit SHA, Qt version, compiler, platform plugin, environment, build
  directory, and exact commands.
- Run the CI-compatible correctness subset before benchmark capture.
- Run at least five benchmark runs for the selected scenarios and write a small
  baseline summary. Raw JSON may live outside the repo if large.

Rough write scope:

- Prefer a new docs summary such as
  `docs/render_invalidation_metrics_baseline_results.md`, or update
  `docs/render_invalidation_metrics_baseline.md` only if the project wants the
  baseline stored there.
- Optional raw artifacts under a clearly named local or ignored results folder.
  Do not commit large raw benchmark outputs without maintainer approval.

Required scenarios:

- `caret_move_right_5000`
- `caret_step_right_latency_64`
- `caret_step_left_latency_64`
- `vertical_scroll_step_latency_64`
- `vertical_scroll_bounce_latency_48`
- `vertical_wheel_bounce_latency_48`
- `wrapped_wheel_bounce_latency_24`
- `scroll_after_edit_latency_32`
- `selection_drag_latency_48`
- `insert_character_2000` if edit scheduling is in the implementation slice
- `zoom_wheel_bounce_latency_24` only as command-zoom coverage, not Ctrl-wheel
  coverage

Review requirements:

- One performance reviewer checks benchmark setup, noise floor, and scenario
  relevance.
- One architecture reviewer checks that the baseline maps to the planned 3A
  implementation slices.
- One test reviewer checks that correctness tests cover the same paths claimed
  by the baseline.

Validation and performance gates:

- Fresh configure/build for the selected toolchain.
- CI-compatible CTest subset excluding visual regression and embedded benchmark
  only when the local runner cannot own those gates.
- Embedded benchmark CTest as an environment sanity check.
- Five standalone JSON-producing benchmark runs for selected scenarios.
- Compare timing distributions, counters, and hierarchical profiles.
- `git diff --check` and markdown checks for any committed baseline summary.

Explicit deferrals and non-goals:

- Do not implement `RenderInvalidationState`.
- Do not add metrics instrumentation in this package unless it has its own
  reviewed metrics-only scope.
- Do not touch renderer, dispatch classification, public API, `Render_frame`,
  scene graph, or vendored Scintilla.

## 2. Phase 3A-M1 Metrics-Only Instrumentation

Status: not started. Required only if reviewers decide existing counters are not
enough for the first 3A implementation slice.

Why it matters:

Existing profiling can show broad update/capture changes, but it cannot
attribute important scheduling decisions to coalescing, property sync, scroll
reuse, IME, notifications, core timers, autoscroll, direct callbacks, or
`enableUpdate()` gating. Source migration should not rely on missing evidence.

Prerequisites and gates:

- A short metrics-only design must identify exactly which missing counters block
  the next 3A slice.
- The package must be inactive-by-default behind existing profiling/session
  mechanics.
- Reviewers must confirm the package is observability-only.

Rough write scope:

- `src/public/scintillaquick_item.cpp` for additive counters, source tags,
  timing scopes, and JSON fields.
- `src/core/scintillaquick_core.cpp` / `.h` only if core timer source tagging
  cannot be observed from the item side.
- `benchmarks/embedded_editor/main.cpp` for focused scenarios only when
  existing scenarios cannot exercise the path.
- `tests/smoke/main.cpp` or `tests/support` for deterministic profiling smoke
  coverage.
- `docs/render_invalidation_metrics_baseline.md` or a follow-up metrics doc.

Useful missing counters:

- Scheduled polish/update count.
- Coalesced update request count.
- Suppressed update request count while updates are disabled.
- Caret blink scheduling count.
- Vertical scroll reuse taken and rejected-by-reason counts.
- Static mutation marker set/clear counts.
- Property sync count and timing.
- Dispatch source counts for `send()`, `sends()`, direct function, and direct
  status callback paths.
- Notification source counts for `StyleNeeded`, `UpdateUI`, `Modified`, `Zoom`,
  and no-update notifications.
- `updateQuickView()` flag counts.
- Wheel zoom versus wheel scroll counts.
- IME, core timer, mouse autoscroll, focus, key, and `enableUpdate()` resume
  source counts.

Review requirements:

- One performance reviewer checks hot-path overhead and JSON compatibility.
- One architecture reviewer checks that metrics match the planned state object.
- One test reviewer checks deterministic coverage for new metrics.

Validation and performance gates:

- CI-compatible correctness subset.
- Benchmark smoke if instrumentation touches hot paths.
- Five-run baseline refresh if counters are intended for 3A comparisons.
- JSON output compatibility check: existing consumers should tolerate additive
  fields.
- `git diff --check`.

Explicit deferrals and non-goals:

- No invalidation behavior changes.
- No renderer metrics in this package. Renderer metrics need a separate Phase 4
  proposal.
- No public API.
- No dispatch classification changes.
- No `Render_frame` schema changes.

## 3. Phase 3A-T0 Narrow Observability and Test Guards

Status: not started.

Why it matters:

Some high-risk scheduling paths are described in the design docs but do not yet
have focused tests that prove the exact fast path. Adding narrow tests before
the state-object refactor reduces the chance that behavior-equivalent cleanup
silently changes capture-path selection.

Prerequisites and gates:

- Decide whether tests need new metrics from Phase 3A-M1. If they do, do not
  write tests that assert unavailable counters.
- Keep production behavior unchanged.

Rough write scope:

- `tests/smoke/main.cpp`
- `tests/frame_validation/main.cpp`
- `tests/CMakeLists.txt` only if a new focused target is needed.
- Existing test support helpers under `tests/support` if observation access is
  needed.

Focused guards to add or confirm:

- Caret blink fast path: a blink tick remains snapshot-only, does not increment
  full/static capture counts, and does not force static content dirty.
- Core fine-timer/autoscroll: vertical timer/autoscroll remains eligible for
  vertical scroll reuse where current behavior allows it; horizontal movement
  still performs static recapture.
- `enableUpdate(false)` suppression and `enableUpdate(true)` resume behavior.
- Real Ctrl-wheel zoom event path if later scheduling work touches wheel zoom;
  existing `zoom_wheel_bounce_latency_24` is command zoom.
- Optional direct evidence that the cached vertical-scroll reuse branch was
  taken, not only cached-vs-direct geometry equivalence.

Review requirements:

- One test reviewer for determinism and portability.
- One architecture reviewer for path coverage.
- Performance reviewer if tests depend on profiling counters or timings.

Validation and performance gates:

- Targeted test target.
- CI-compatible CTest subset.
- Frame validation if render-frame access is touched.
- No benchmark required unless the tests are coupled to performance assertions.

Explicit deferrals and non-goals:

- No production source behavior changes.
- No renderer changes.
- No dispatch table changes.
- No public API changes.

## 4. Phase 3A-P0 Implementation Plan

Status: not started.

Why it matters:

The design documents are broad. Before source edits, the first implementation
slice needs a concrete plan that says which flags move, which call sites change,
which tests prove equivalence, and what rollback looks like.

Prerequisites and gates:

- Metrics baseline evidence is captured from the current branch tip. Reviewers
  may accept existing counters as sufficient for a narrow first slice, but that
  acceptance replaces only missing metrics instrumentation, not the baseline
  evidence capture itself.
- Any required metrics-only instrumentation has landed and been baselined.
- Focused test guards are either present or scheduled in the same implementation
  batch as tests-only changes.

Rough write scope:

- New or updated doc such as
  `docs/render_invalidation_implementation_plan.md`.
- Update `docs/review_remediation_plan.md` only after review is green.

Plan content requirements:

- First behavior-equivalent extraction step.
- Complete writer/reader matrix for flags and scheduling paths.
- Pre/post metrics to compare.
- Exact test list.
- Rollback criteria.
- Explicit statement that renderer, dispatch classification, public API,
  `Render_frame`, batching, and property snapshot work are not included.

Review requirements:

- Two architecture reviewers.
- One performance reviewer.
- One test reviewer if test changes are part of the package.

Validation gates:

- Documentation checks only until implementation begins.

Explicit deferrals and non-goals:

- No source implementation.
- No benchmark claims unless actual baseline evidence is attached.

## 5. Phase 3A RenderInvalidationState Implementation

Status: not started. Design complete, implementation not authorized yet.

Why it matters:

Dirty flags and update scheduling are still distributed across dispatch,
notifications, scroll handlers, event handlers, timers, caret blink, IME, and
snapshot capture. A state object should make invalidation intent explicit and
harder to partially apply.

Prerequisites and gates:

- Phase 3A-M0 baseline evidence approved.
- Phase 3A-M1 metrics-only package completed if required.
- Phase 3A-T0 test guards completed or integrated with the relevant slice.
- Phase 3A-P0 implementation plan reviewed green.

Expected implementation slices:

1. Equivalent state object:
   - Move flag storage and OR/coalescing helpers only.
   - Keep `request_scene_graph_update(...)` signature and behavior.
   - Preserve caret blink direct scheduling and the blink-only shortcut.
2. Dispatch application:
   - Apply dispatch intent through the state object.
   - Preserve `m_in_sync_quick_view_properties` behavior: scroll-width reset
     happens first; nested update application then skips property sync,
     mutation marker writes, and scheduling.
   - Preserve direct callback routing through non-virtual
     `ScintillaQuick_item::send()`.
3. Notifications and event paths:
   - Convert notification branches, wheel, scroll, focus, mouse, key, and IME
     scheduling to semantic state calls.
   - Preserve touch and drag/drop behavior unless focused tests approve
     additional scheduling.
   - Preserve signal ordering and notification compatibility.
4. Snapshot consumption:
   - Make `build_render_snapshot()` consume a read-only invalidation state view.
   - Keep the four current capture paths: blink-only, vertical scroll reuse,
     overlay-only, and full/static capture.
   - Keep state clearing at the successful snapshot boundary.
5. Docs closeout:
   - Update maintenance/current-state docs only after implementation is reviewed
     and validated.

Rough write scope:

- `src/public/scintillaquick_item.cpp`
- Optional private, non-installed helper under `src/public` or `src/core`
- Focused tests under `tests/smoke`, `tests/frame_validation`, and possibly
  `tests/support`
- Docs closeout after green reviews

Review requirements:

- Two code reviewers per slice.
- Performance reviewer for any slice that changes scheduling frequency,
  property sync, scroll reuse, caret movement, or blink behavior.
- Test reviewer for new or changed tests.
- Final integration reviewer after slices are combined.

Validation and performance gates:

- Fresh configure/build.
- CI-compatible CTest subset.
- Dispatch table test.
- Smoke tests for dispatch/direct/event/IME/notification paths touched.
- Frame validation, especially vertical scroll reuse.
- Caret blink fast-path test.
- Core fine-timer/autoscroll test.
- Linux visual-baseline gate for render-visible scheduling changes.
- Five-run benchmark comparison for update/coalescing/capture-path changes.

Explicit deferrals and non-goals:

- No Package 3B batching.
- No Package 3C property snapshot.
- No renderer primitive batching, node-pool rewrite, or `Render_frame` schema
  change.
- No generated dispatch output.
- No public API changes.
- No vendored Scintilla changes.

## 6. Phase 3B Scoped Editor Update Batching

Status: not started. Must wait until 3A ownership of dirty state, property sync,
scheduling, and direct-callback interactions is settled.

Why it matters:

High-level public operations such as `setText()` send multiple Scintilla
messages. Today coalescing prevents some redundant work, but property sync and
bookkeeping can still happen repeatedly. A scoped batch can defer sync and
scene-graph scheduling to one exit point.

Prerequisites and gates:

- Phase 3A implementation planning or implementation has defined the ownership
  model for dirty flags and scheduling.
- Baseline metrics show update and property-sync counts for multi-message
  workflows.
- Design review confirms which public operations are safe to batch.

Rough write scope:

- `src/public/scintillaquick_item.cpp`
- Optional internal helper header/source
- Tests for multi-message public operations such as `setText()`, style/font
  setup, drag/drop, and IME composition if included

Review requirements:

- Two code reviewers.
- Performance reviewer for update/property-sync count changes.
- Test reviewer for property and notification ordering behavior.

Validation and performance gates:

- CI-compatible correctness subset.
- Smoke tests proving QML-facing properties update once and correctly.
- Notification ordering tests if batched operations can emit notifications.
- Five-run benchmark comparison for affected operations, especially
  `insert_character_2000` and edit-after-scroll scenarios.

Explicit deferrals and non-goals:

- Do not start before 3A plan is green.
- Do not change dispatch classification.
- Do not change renderer behavior.
- Do not change public API.
- Do not use batching to hide under-invalidation.

## 7. Phase 3C Property/View Snapshot

Status: not started.

Why it matters:

`syncQuickViewProperties()` and related getters query Scintilla repeatedly for
properties that are often needed together. A single internal snapshot could
reduce redundant queries and make property synchronization easier to test.

Prerequisites and gates:

- Phase 3A has stabilized invalidation/property-sync ownership.
- Design review defines the snapshot fields and proves no behavior changes for
  QML-facing properties.
- Baseline includes property-sync count/time if this work is expected to improve
  or preserve performance.

Candidate fields:

- document length;
- current position;
- anchor/selection bounds;
- first visible line;
- first visible column / x offset;
- visible lines and visible columns;
- total lines and columns;
- scroll width;
- character width and height;
- any cached geometry values currently used to avoid recursion.

Rough write scope:

- `src/public` internals
- Focused property-sync tests
- Docs if the synchronization contract changes

Review requirements:

- Two code reviewers.
- Performance reviewer if getter volume or sync timing is part of the claim.
- Test reviewer for property-change signal behavior.

Validation and performance gates:

- Smoke tests for document edits, scroll changes, selection changes, style/font
  changes, and zoom.
- Tests proving no recursion through property getters.
- CI-compatible CTest subset.
- Benchmark or profiling comparison for property-sync-heavy scenarios.

Explicit deferrals and non-goals:

- Do not combine with 3B batching.
- Do not change public property semantics.
- Do not use the snapshot as a stale read cache unless a separate design proves
  lifetime and invalidation.
- Do not change renderer or dispatch classification.

## 8. Package 2E Test-Only Public API Cleanup

Status: design complete and reviewed green; implementation deferred to an
accepted API cleanup window.

Why it matters:

The installed public header still exposes unsupported test concepts:
`Displayed_row_for_test`, test-access declarations, and private methods such as
`displayed_rows_for_test()` / `rendered_frame_for_test()`. This is not a runtime
bug, but it leaks implementation concepts and invites downstream dependency on
unsupported API.

Prerequisites and gates:

- Maintainer accepts a source-compatibility cleanup window.
- No concurrent public-header/meta-object work.
- Decide whether this cleanup is part of a broader PIMPL/private-header package
  or a narrow Option C cleanup from
  [`test_access_api_boundary.md`](test_access_api_boundary.md).

Preferred first implementation:

1. Centralize benchmark and validation-test access through a non-installed
   support helper.
2. Migrate `benchmarks/embedded_editor/main.cpp` away from its local validation
   access helper.
3. Reconstruct displayed rows from cached `Render_frame` in test support.
4. Remove `Displayed_row_for_test` from the installed public header after
   reviewers accept the source-compatibility impact.
5. Remove private test methods only when no longer used.
6. Keep `rendered_frame_for_test()` until a later private-header strategy can
   replace it safely.

Rough write scope:

- `include/scintillaquick/scintillaquick_item.h`
- `tests/support/scintillaquick_validation_access.h`
- `benchmarks/embedded_editor/main.cpp`
- `tests/CMakeLists.txt`
- Top-level CMake only if include/install behavior changes
- `docs/test_access_api_boundary.md` and release notes/docs if needed

Review requirements:

- API/build reviewer for installed header and consumer behavior.
- Test/benchmark reviewer for frame validation, review capture, and embedded
  benchmark access.
- Additional CMake/install reviewer if install/export logic changes.

Validation gates:

- Static and shared builds.
- CI-compatible CTest subset.
- Targeted frame-validation test.
- Embedded benchmark build and a small benchmark smoke if benchmark access code
  changes.
- Install/consumer smoke from a temporary install prefix.
- `git diff --check` and doc checks.

Explicit deferrals and non-goals:

- Do not introduce PIMPL by itself.
- Do not change renderer, invalidation, scrolling, or validation fixture
  semantics.
- Do not make test-access helpers part of the installed package.
- Do not create a supported public displayed-row or render-frame API.

## 9. Package 2F Stage 3 Generated Dispatch Output

Status: deferred. Stage 1 and Stage 2 are complete and validated; Stage 3 is
not started.

Why it matters:

The manual rule table is now centralized and audited, but it can still drift as
vendored Scintilla changes. Generation from `Scintilla.iface` plus explicit
overrides could reduce maintenance burden, but it can also create noisy reviews
or accidental under-invalidation if done too early.

Prerequisites and gates:

- Stage 2 audit output is stable across at least one Scintilla upgrade or a
  deliberate dry-run.
- Maintainers approve moving from audit-only tooling to generated runtime
  inputs.
- A generation proposal defines override format, failure modes, and
  reproducibility checks.

Rough write scope:

- Generator script under `tools/` or `scripts/`
- Checked-in override file, for example
  `tools/dispatch/scintillaquick_dispatch_rules.yml`
- Checked-in generated header, for example
  `src/core/scintillaquick_dispatch_table.generated.h`
- Thin wrapper in `src/core/scintillaquick_dispatch_table.h`
- `tests/dispatch_table/main.cpp`
- Build-system target only if generation is integrated into CMake

Review requirements:

- Two dispatch/code reviewers.
- One test reviewer for generation reproducibility and failure modes.
- One performance reviewer if any classification or hot-path lookup shape
  changes.

Validation gates:

- Regenerate output and assert clean `git diff`.
- Dispatch table tests.
- CI-compatible CTest subset.
- Static and shared builds if build graph or generated header behavior changes.
- Benchmark smoke for caret movement and read-only query hot paths if any
  classification differs from Stage 1.

Explicit deferrals and non-goals:

- Do not generate production dispatch output from the current Stage 2 audit
  script.
- Do not change renderer, invalidation, batching, public API, or direct
  callback behavior in this package.
- Unknown/unclassified messages must still default to conservative full resync.

## 10. Notification API Follow-Ups

Status: safe snapshot API implemented; follow-ups deferred.

Why it matters:

`notificationReceived(const ScintillaQuick_notification&)` gives C++ users a
queued-safe owned snapshot. The legacy `notify(NotificationData*)` and
`macroRecord(message, wParam, lParam)` signals remain for compatibility and are
direct-only for pointer-bearing payloads. The project still needs a long-term
compatibility policy for those legacy hooks.

Remaining decisions:

- Should `notify(NotificationData*)` remain indefinitely as an expert C++ hook?
- Should it be deprecated after the safe API has shipped for at least one
  release?
- How should warning-heavy deprecation interact with downstream
  warnings-as-errors builds?
- Should migration docs be added for C++ users?
- Should generic QML notification support ever exist? Current policy says no
  generic QML contract for `ScintillaQuick_notification`.

Prerequisites and gates:

- Decide release compatibility policy.
- Audit downstream usage if known.
- Keep any change serialized away from other public-header or Qt meta-object
  changes.

Rough write scope:

- Docs and release notes first.
- `include/scintillaquick/scintillaquick_item.h` only if deprecation attributes
  or comments are added.
- Tests only if signal behavior changes, which should be avoided outside a
  breaking-change window.

Review requirements:

- API reviewer.
- Qt/QML reviewer if any QML-facing wording or meta-object behavior changes.
- Compatibility reviewer if deprecation is proposed.

Validation gates:

- Static/shared builds for header changes.
- Consumer compile smoke if deprecation attributes are added.
- Existing notification smoke tests.
- No performance gate unless payload copying behavior changes.

Explicit deferrals and non-goals:

- Do not remove or change the legacy pointer signal in a non-breaking release.
- Do not expose `ScintillaQuick_notification` as a QML value type without a
  separate QML API design and tests.
- Do not store raw Scintilla payload pointers in queued-safe data.

## 11. PIMPL and Public Header Boundary Cleanup

Status: not started; long-term API/ABI design.

Why it matters:

The installed public header still contains many private fields and internal
types. A PIMPL or stronger private-header split would reduce rebuild churn,
improve ABI flexibility, and make it harder for consumers to depend on internal
Qt/Scintilla/render concepts.

Prerequisites and gates:

- Decide the library's API/ABI compatibility policy.
- Coordinate with Package 2E because test-access cleanup is one visible part of
  the same public-header boundary problem.
- Avoid overlapping with notification or other meta-object changes.

Rough write scope:

- `include/scintillaquick/scintillaquick_item.h`
- New private implementation header/source under `src/public` or `src/core`
- Tests/build/install docs
- CMake install/export configuration if header layout changes

Review requirements:

- API/ABI reviewer.
- Qt meta-object reviewer.
- Build/install reviewer.
- Test reviewer for public behavior.

Validation gates:

- Static and shared builds.
- Install/consumer smoke test.
- QML type registration smoke.
- CI-compatible CTest subset.
- Public API compatibility review.

Explicit deferrals and non-goals:

- Do not use PIMPL as a vehicle for behavior changes.
- Do not bundle renderer/invalidation refactors.
- Do not hide test hooks without following Package 2E migration rules.

## 12. Phase 4 Renderer Metrics Plan

Status: not started. Metrics-first and design-first.

Why it matters:

The renderer remains the largest custom subsystem and still has many primitive
families, node pools, and sync paths. Before simplifying it, the project needs
current evidence about node counts, cache behavior, texture rebuilds, and frame
update costs.

Prerequisites and gates:

- Finish or pause Phase 3A with clear boundaries. Do not overlap scheduling
  migration and renderer refactors.
- Write a renderer metrics plan before source instrumentation.
- Use existing performance docs and historical zoom-bounce case study as
  context, but remeasure current code.

Candidate metrics:

- Node count by primitive family.
- Rectangle node count and update time.
- Text node cache hits, backup hits, misses, evictions, and texture rebuilds.
- Text layout/shaping time by primitive family.
- Geometry node updates and material updates.
- Image/marker texture creation and reuse.
- Renderer update total time and child-scope breakdown.
- Scene graph node allocation/deletion counts if practical.

Rough write scope:

- Metrics design doc first, for example `docs/renderer_metrics_plan.md`.
- Later metrics-only source changes may touch
  `src/render/scintillaquick_scene_graph_renderer.cpp`,
  `src/render/scintillaquick_scene_graph_renderer.h`,
  benchmark output, and docs.

Review requirements:

- Renderer reviewer.
- Performance reviewer.
- Test/visual reviewer.

Validation gates:

- Benchmark baseline before and after metrics instrumentation.
- Linux visual-baseline gate if instrumentation risks output changes.
- CI-compatible CTest subset.
- JSON compatibility for additive benchmark fields.

Explicit deferrals and non-goals:

- No rectangle batching yet.
- No generic primitive/layer model yet.
- No node-pool rewrites yet.
- No `Render_frame` schema change.
- No invalidation scheduling change.

## 13. Phase 4 Renderer Simplification and Performance Work

Status: not started; highest-intrusion remaining work.

Why it matters:

The long-term performance/LOC opportunity is to move from many
Scintilla-specific scene graph sync paths to fewer generic primitive and batch
layers. This can reduce node count, heap churn, and duplicated code, but it can
also easily cause visual regressions or cache churn.

Prerequisites and gates:

- Renderer metrics identify the hotspot.
- Five-run benchmark baseline exists for relevant scenarios.
- Linux visual baselines are available for affected render fixtures.
- A design document defines the primitive/layer model, ordering, fallback, and
  migration slices.

Candidate subprojects:

### 13.1 Generic Render Primitive and Layer Model

Why:

- The renderer currently knows many Scintilla-specific primitive families.
- A normalized model could let capture stay Scintilla-aware while rendering
  handles a smaller set of generic primitives.

Potential write scope:

- Design doc first.
- Later normalization layer under `src/render` or `src/core`.
- Possible `Render_frame` additions only after visual and frame-validation
  plans are approved.

Non-goals:

- Do not rewrite capture and renderer together in one step.
- Do not drop Scintilla-specific information needed by tests or ordering.

### 13.2 Rectangle-Like Primitive Batching

Why:

- Selection rectangles, carets, indicator rectangles, marker raster rects,
  annotation rectangles, whitespace, indent guides, and underlines can create
  many `QSGRectangleNode` instances and duplicated sync paths.

Potential write scope:

- Renderer metrics first.
- Later `src/render/scintillaquick_scene_graph_renderer.cpp` and tests.

Validation gates:

- Node count and frame-time comparison.
- Visual regression on Linux.
- Frame validation for primitive geometry.
- Benchmark comparison for scrolling, selection drag, large documents, and
  edit-after-scroll.

Non-goals:

- Do not batch until metrics show benefit and ordering rules are documented.
- Do not change `Render_frame` unless the batch design requires it and is
  reviewed separately.

### 13.3 Generic Node Pools

Why:

- Many specific vectors and sync helpers can diverge over time.
- Generic keyed/unkeyed node-pool abstractions could reduce duplication.

Potential write scope:

- Renderer internals only.
- Tests that prove node reuse and visual equivalence.

Validation gates:

- Same visual and benchmark gates as rectangle batching.
- Allocation/reuse metrics if available.

Non-goals:

- Do not introduce a generic abstraction unless it reduces real duplication or
  allocation pressure shown by metrics.

### 13.4 Text Primitive Consolidation

Why:

- Normal text, margin text, fold text, annotations, EOL annotations, and
  represented text have separate conversion/sync paths.
- Consolidating text primitive handling may reduce code and improve cache
  behavior.

Potential write scope:

- Design doc and renderer metrics first.
- Later renderer and possibly render-frame normalization changes.

Validation gates:

- Text layout correctness in frame validation.
- Linux visual baselines for annotations, margins, represented text, folds,
  whitespace, and mixed fonts.
- Benchmark comparison for zoom, scrolling, and large text.

Non-goals:

- Do not erase special-case semantics until tests cover them.

### 13.5 Text Node Cache Metrics and Matching

Why:

- Text-node cache behavior is sophisticated and has historical performance
  impact. More instrumentation should come before algorithm changes.
- Future ideas include cache hit/miss/eviction metrics, fuzzy geometry matching
  where exact floating-point equality causes churn, and possibly more than two
  cache slots if current metrics justify it.

Potential write scope:

- Metrics-only renderer package first.
- Later cache matching changes in
  `src/render/scintillaquick_scene_graph_renderer.cpp`.

Validation gates:

- Cache metrics before and after.
- Zoom-bounce and scrolling benchmarks.
- Linux visual baselines.
- Memory-bound documentation if cache capacity increases.

Non-goals:

- Do not widen cache memory footprint silently.
- Do not add fuzzy comparisons without tests proving visual stability.

## 14. Scintilla Upgrade and Dispatch Audit Upkeep

Status: ongoing future maintenance.

Why it matters:

Scintilla message definitions can change when the vendored dependency is
updated. The dispatch audit exists to make drift visible without automatically
turning new messages into narrow invalidation rules.

Required procedure during upgrades:

- Run `python tools/dispatch/audit_dispatch_table.py --check`.
- Compare counts and warning categories against
  [`dispatch_audit_triage.md`](dispatch_audit_triage.md).
- Re-review deprecated/provisional entries whose status changed.
- Treat new unclassified messages as conservative fallback until semantically
  reviewed.
- Do not add generated output unless Stage 3 is separately approved.

Write scope:

- Dispatch table and tests only for reviewed classification changes.
- Dispatch docs and triage baseline for accepted warning changes.
- Vendored Scintilla only if the task is explicitly an upstream update.

Review requirements:

- Dispatch reviewer.
- Scintilla-upgrade reviewer.
- Test reviewer for table/audit tests.

Validation gates:

- Audit script.
- Dispatch table test.
- CI-compatible CTest subset.
- Benchmark smoke if hot-path classifications change.

Explicit deferrals and non-goals:

- No renderer/invalidation refactor in upgrade triage.
- No automatic fast paths for new messages.

## 15. Platform Window Wrapper-Type Cleanup

Status: optional future cleanup; not a current correctness blocker.

Why it matters:

Package 1F fixed the immediate lifetime hazard by making owned versus borrowed
Qt Quick window handles explicit in the platform registry and resolver policy.
The platform abstraction still follows Scintilla's opaque `WindowID` style,
which means future code can accidentally reintroduce raw handle casts or unclear
ownership unless it follows [`platform_window_ownership.md`](platform_window_ownership.md).

A later cleanup could make the Qt-side model clearer by introducing explicit
wrapper concepts for borrowed editor windows and owned popup/list/call-tip
windows. This should be treated as maintainability work, not as a reason to
reopen the completed 1F hardening.

Prerequisites and gates:

- Re-read `docs/platform_window_ownership.md` and current platform code.
- Confirm no current lifecycle bug is being hidden by the cleanup proposal.
- Write a short design note or update the ownership contract before source
  changes.
- Keep the existing resolver/tombstone/recreate behavior until tests prove an
  equivalent replacement.

Rough write scope:

- `src/platform/scintillaquick_platqt.cpp`
- `src/platform/scintillaquick_platqt.h`
- `src/core/scintillaquick_core.cpp` only if call-tip/list-box creation
  handoff changes
- focused lifecycle tests in `tests/smoke/main.cpp`
- `docs/platform_window_ownership.md`

Review requirements:

- Platform/lifetime reviewer.
- Qt object-ownership reviewer.
- Test reviewer for popup/list-box/call-tip lifecycle coverage.

Validation gates:

- Static and shared builds.
- CI-compatible CTest subset.
- Focused call-tip and autocomplete/list-box lifecycle tests.
- External deletion/recreate tests for owned popups.
- `git diff --check` and doc checks.

Explicit deferrals and non-goals:

- Do not edit vendored Scintilla `Window`.
- Do not register `wMain` as owned.
- Do not change renderer, invalidation, dispatch, public API, or
  `Render_frame`.
- Do not start this as cleanup-only work while higher-priority Phase 3A metrics
  gates are pending unless maintainers explicitly choose this path.

## 16. Optional Raster Oracle Isolation

Status: optional future cleanup; not urgent.

Why it matters:

`PartialPaint()` and `PartialPaintQml()` are retained as raster reference oracle
support for validation. They are documented as not being a second production
renderer. A future API/test-support cleanup could isolate them more strongly so
maintainers do not confuse them with production rendering.

Prerequisites and gates:

- Coordinate with Package 2E and possible PIMPL/private-header work.
- Keep frame-validation behavior unchanged.

Rough write scope:

- `src/core/scintillaquick_core.cpp` / `.h`
- `tests/support/scintillaquick_validation_access.h`
- Test CMake only if build gating changes
- Docs closeout

Review requirements:

- Core/test reviewer.
- Frame-validation reviewer.

Validation gates:

- Frame-validation test.
- CI-compatible CTest subset.
- Linux visual baselines only if render capture behavior changes.

Explicit deferrals and non-goals:

- Do not remove the raster oracle while tests depend on it.
- Do not create a second production rendering path.
- Do not bundle with renderer refactors.

## Completed Items Not To Reopen Without New Evidence

The following original review items have already been addressed or deliberately
closed by the completed packages. Do not schedule them again unless a new bug,
regression, or upstream change provides fresh evidence:

- `sends()` bypassing `send()` dispatch and invalidation.
- `modified(...)` non-owning `QByteArray::fromRawData` payload.
- Incomplete `translate_render_frame()` geometry translation for current
  `Render_frame` fields.
- Conditional base `geometryChange()` call.
- Platform `Window::Destroy()` ownership ambiguity for borrowed versus owned
  QQuickItems.
- Consumed wheel and middle-click event acceptance fixes.
- IME attribute range clamping and input-method update scheduling hardening.
- `current_render_frame()` buffered-draw restoration and line-height guard.
- UTF-8 measurement forward-progress robustness.
- Existing call-tip repositioning/reuse.
- Read-only cursor UI update suppression.
- Platform null/empty guards for fonts, surfaces, menus, images, shapes, and
  listbox string conversion.
- `Surface_impl::Copy()` source rectangle sizing.
- ASCII-oriented `selectCurrentWord()`.
- `keyPressEvent()` event-local modifiers.
- `UpdateInfos()` legacy wrapper audit/removal.
- Direct callback raw `WndProc()` bypass and status-pointer handling.
- Dispatch Stage 1 manual table equivalence and Stage 2 audit-only inventory.

## Resume Checklist

Before starting any future package:

1. Run `git status --short` and identify unrelated user changes.
2. Read this document and the package-specific design doc.
3. Confirm the package is still the next gate and has not been superseded.
4. Write or refresh a short package plan if the work is source-changing.
5. Keep write scope narrow.
6. Arrange independent review before and after implementation.
7. Run package-specific validation, then update this roadmap or the main
   remediation plan only after the package is actually green.

The safest immediate resume point is still Phase 3A-M0 metrics baseline
evidence capture, followed by metrics-only instrumentation planning if the
baseline exposes missing attribution needed for the first invalidation slice.
