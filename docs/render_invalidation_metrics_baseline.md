# Render Invalidation Metrics Baseline

This is a Phase 3A design-only document. It defines the measurement baseline
required before any `RenderInvalidationState` implementation changes are made.
It does not authorize source implementation, metrics instrumentation, renderer
changes, dispatch changes, or scene-graph changes.

The companion design is
[`render_invalidation_state_design.md`](render_invalidation_state_design.md).
The general benchmark process is
[`performance_optimization_process.md`](performance_optimization_process.md).

## Scope

Write scope for this package:

- `docs/render_invalidation_metrics_baseline.md`

Out of scope:

- implementing `RenderInvalidationState`;
- changing invalidation behavior;
- changing dispatch classification;
- changing `Render_frame`;
- renderer primitive batching or node-pool work;
- scene-graph renderer changes;
- editor transaction or property snapshot work;
- public API changes;
- benchmark or profiler source changes.

## Baseline Rule

No Phase 3A source migration should start until the current branch tip has a
recorded baseline for both correctness and scheduling performance.

The baseline must capture:

- correctness status from the existing CI-compatible tests;
- benchmark timing distributions from at least five Release runs;
- existing profiling counters and hierarchical profiles for the selected
  scenarios;
- an explicit list of missing counters that must be added by a metrics-only
  instrumentation package before behavior-equivalent refactoring can rely on
  them.

Do not compare a Phase 3A implementation against an old baseline after the
branch moves. If source changes land before 3A starts, recapture the baseline
from the new tip.

## Existing Measurement Surface

Current profiling is exposed through `ScintillaQuick_item::startProfilingSession`
and embedded benchmark JSON output. The benchmark starts a profiling session per
scenario and attaches the emitted item profiling report to that scenario.

| Surface | Existing data | Useful for Phase 3A | Limitation |
| --- | --- | --- | --- |
| Embedded benchmark scalar stats | `mean_ms`, `median_ms`, `p95_ms`, `max_ms`, `elapsed_ms`, `timeout_count` | Detects end-to-end paint-latency and command-latency regressions. | Does not explain why a regression happened. |
| Profiling counters | `update_requests`, `snapshot_build_count`, `snapshot_line_total`, `snapshot_line_max`, `wheel_event_count`, `horizontal_scroll_command_count`, `vertical_scroll_command_count`, `blink_only_update_count`, `overlay_only_update_count`, `full_update_count` | Detects broad scheduling and capture-path changes. | Missing coalescing, vertical-scroll-reuse, property-sync, notification, direct-callback, IME, timer, and autoscroll source attribution. |
| Profiling metrics | `item.update_polish`, `item.build_render_snapshot`, `item.update_paint_node`, `item.update_quick_view`, `item.wheel_event`, `item.scroll_horizontal`, `item.scroll_vertical` | Detects time spent in item scheduling, capture, paint, update notification, and scroll paths. | `syncQuickViewProperties()` is not measured as its own scope. |
| Hierarchical `scope_tree` | Existing `item.`, `core.`, `renderer.`, and `platform.` scopes | Supports drill-down if a benchmark regresses. | Scheduling-source counters are not represented as a stable tree of events. |
| `Paint_counted_editor` benchmark support | Paint-boundary callback from `updatePaintNode()` | Lets paint-latency scenarios wait for a real scene-graph update. | Counts paints, not requested or coalesced updates. |
| Smoke tests | `send()` / `sends()`, direct callbacks, notification payloads, IME guards, platform lifecycle paths | Correctness guard for specific invalidation-sensitive APIs. | Most smoke tests do not collect profiling counters. |
| Dispatch-table test | Message classification and helper equivalence | Guards the policy that drives public dispatch invalidation. | It does not exercise Qt event, notification, or capture scheduling. |
| Frame validation | Captured frame consistency, horizontal scroll, vertical scroll reuse geometry | Guards render-frame correctness after scheduling/capture changes. | It does not currently assert that reuse was taken instead of direct recapture. |
| Visual regression test | Pixel-level render output, including scroll-wheel bounce fixtures | Guards visible rendering. | Platform-specific gate; not part of the normal Windows offscreen subset. |

Important existing-counter semantics:

- `update_requests` counts calls through `request_scene_graph_update()` while
  profiling is active. It does not count caret blink's direct `polish()` /
  `update()` scheduling.
- `snapshot_build_count` is incremented for non-blink snapshot builds. The
  current blink-only early return increments `blink_only_update_count` but not
  `snapshot_build_count`.
- `overlay_only_update_count` currently includes both true overlay-only capture
  and vertical-scroll-reuse capture. A dedicated scroll-reuse counter is missing.
- `full_update_count` counts static/full recapture when static content is dirty
  and vertical scroll reuse was not used.

## Required Metrics

The following metrics define the baseline needed for Phase 3A. "Existing" means
the current tree can report the metric without source edits. "Needed later"
means a metrics-only package should add the metric before a behavior-equivalent
invalidation migration relies on it.

| Metric | Status | Required baseline use |
| --- | --- | --- |
| Public update requests | Existing: `counters.update_requests` | Compare request count per benchmark scenario. A behavior-equivalent refactor should not increase it outside the measured baseline range. |
| Scheduled polish/update pairs | Needed later | Distinguish request volume from actual scheduled scene-graph work. Required before changing coalescing ownership. |
| Coalesced update requests | Needed later | Count requests ignored because `update_pending` was already true. Required before moving coalescing into `RenderInvalidationState`. |
| `updatePolish()` calls and time | Existing: `metrics.update_polish` | Compare call count, total time, and max time. |
| `updatePaintNode()` calls and time | Existing: `metrics.update_paint_node` | Compare paint count/time against benchmark paint-latency stats. |
| Snapshot builds | Existing: `counters.snapshot_build_count`, `metrics.build_render_snapshot` | Compare non-blink snapshot count and capture cost. |
| Full/static recaptures | Existing: `counters.full_update_count` | Must not increase for overlay, caret, scroll-reuse, and direct-query scenarios. |
| Overlay-only captures | Existing but ambiguous: `counters.overlay_only_update_count` | Useful as a broad count, but not enough to prove scroll reuse was preserved. |
| Blink-only updates | Existing: `counters.blink_only_update_count` | Must remain available and cheap. Current embedded benchmark disables caret blinking, so a focused scenario is needed before changing blink scheduling. |
| Vertical-scroll reuse taken | Needed later | Required before changing scroll invalidation state. Must distinguish reuse from overlay-only capture. |
| Vertical-scroll reuse rejected with reason | Needed later | Useful to detect accidental `content_modified_since_last_capture`, x-offset, buffer, or first-visible-line regressions. |
| Static mutation marker set/cleared | Needed later | Required before moving `content_modified_since_last_capture` writes into a semantic state object. |
| Property sync calls and time | Needed later | Required before changing dispatch, notification, scroll, or IME property-sync behavior. |
| Dispatch source counts | Needed later | Count `send()`, `sends()`, direct function, and direct status callback update application. |
| Notification source counts | Needed later | Count `StyleNeeded`, `UpdateUI`, `Modified`, `Zoom`, and default/no-update notification paths. |
| `updateQuickView()` flags | Existing time only; counts needed later | Count `Content`, `VScroll`, `HScroll`, and overlay-only combinations. |
| Wheel source counts | Existing broad count: `wheel_event_count`; needed split | Split Ctrl-wheel zoom from vertical wheel scroll. |
| IME source counts | Needed later | Split read-only/protected rejection, commit, preedit, cancel/tentative undo, and malformed-attribute paths. |
| Core timer source counts | Needed later | Split timer tick with vertical scroll, horizontal scroll, and overlay-only/no-scroll updates. |
| Mouse autoscroll source counts | Needed later | Split vertical, horizontal, combined, and overlay-only mouse-move scheduling. |
| `enableUpdate()` suppression/resume | Needed later | Count suppressed requests while updates are disabled and the resume request after re-enable. |

## Baseline Workflows

Use Release builds and a fixed Qt rendering environment. The exact build
directory can vary, but each baseline run should record the build path, commit
SHA, Qt version, compiler, platform plugin, and environment variables.

Build:

```powershell
$buildDir = "build/phase3a-metrics-baseline"
cmake -S . -B $buildDir -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSCINTILLAQUICK_BUILD_TESTS=ON -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON
cmake --build $buildDir --config Release
```

Correctness gate:

```powershell
ctest --test-dir $buildDir -C Release --output-on-failure -E "scintillaquick_visual_regression_test|scintillaquick_embedded_benchmark"
```

Benchmark environment sanity check:

```powershell
ctest --test-dir $buildDir -C Release --output-on-failure -R "^scintillaquick_embedded_benchmark$"
```

The CTest-registered benchmark run verifies the Qt environment, but it does not
pass selected `-s` scenarios or `-o` output arguments. Run the embedded
benchmark standalone for selected scenarios so each run writes a JSON artifact.
On Windows, reproduce the environment configured in `tests/CMakeLists.txt`:
`QT_QPA_PLATFORM=windows`, the fixed DPI/scale variables, Qt plugin paths, and
`PATH` entries for the benchmark executable directory, the `ScintillaQuick`
library target directory, and the Qt `bin` directory. The executable path is
generator-specific: single-config generators commonly place it under
`$buildDir`, while multi-config generators commonly place it under
`$buildDir\Release`.

```powershell
$config = "Release"
$benchmarkExe = Join-Path $buildDir "scintillaquick_embedded_benchmark.exe"
if (-not (Test-Path $benchmarkExe)) {
  $benchmarkExe = Join-Path (Join-Path $buildDir $config) "scintillaquick_embedded_benchmark.exe"
}

$qtTool = Get-Command qmake6 -ErrorAction SilentlyContinue
if (-not $qtTool) { $qtTool = Get-Command qmake -ErrorAction Stop }
$qtBinDir = Split-Path $qtTool.Source -Parent
$qtPluginDir = Resolve-Path (Join-Path $qtBinDir "..\plugins")
$targetDir = Split-Path $benchmarkExe -Parent
$scintillaQuickTargetDir = $targetDir # Replace if the ScintillaQuick library target dir differs.

$env:QT_QPA_PLATFORM = "windows"
$env:QT_PLUGIN_PATH = "$qtPluginDir"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtPluginDir "platforms"
$env:QT_FONT_DPI = "96"
$env:QT_SCALE_FACTOR = "1"
$env:QT_ENABLE_HIGHDPI_SCALING = "0"
$env:PATH = "$targetDir;$scintillaQuickTargetDir;$qtBinDir;$env:PATH"

& $benchmarkExe `
  -s caret_move_right_5000 `
  -s caret_step_right_latency_64 `
  -s caret_step_left_latency_64 `
  -s vertical_scroll_step_latency_64 `
  -s vertical_scroll_bounce_latency_48 `
  -s vertical_wheel_bounce_latency_48 `
  -s wrapped_wheel_bounce_latency_24 `
  -s scroll_after_edit_latency_32 `
  -s selection_drag_latency_48 `
  -s insert_character_2000 `
  -s zoom_wheel_bounce_latency_24 `
  -o phase3a_baseline_run_01.json
```

Repeat the same command at least five times, using a fresh output filename for
each run.

Linux visual gate:

- Required before accepting any Phase 3A implementation that changes captured
  static content, scroll reuse, capture-path selection, or renderer-visible
  scheduling.
- Use `scintillaquick_visual_regression_test` on the runner that owns the
  checked-in baselines.

## Scenario Matrix

| Workflow risk | Existing scenario or test | Metrics to compare |
| --- | --- | --- |
| Caret movement becoming static/full capture | `caret_move_right_5000`, `caret_step_right_latency_64`, `caret_step_left_latency_64`, smoke direct/caret tests | `update_requests`, `full_update_count`, `overlay_only_update_count`, `metrics.update_quick_view`, paint latency. |
| Public dispatch mutation path | Smoke `sends()` and direct callback tests, `insert_character_2000` | Correctness, `update_requests`, `full_update_count`, `snapshot_build_count`, `metrics.build_render_snapshot`. |
| Direct callback dispatch path | Smoke direct function/status tests | Existing tests only now; add source counters later before refactoring direct callback attribution. |
| Vertical scroll reuse | `vertical_scroll_step_latency_64`, `vertical_scroll_bounce_latency_48`, frame validation scroll-reuse test, visual scroll fixtures | Existing broad counters now; add `vertical_scroll_reuse_count` before changing scroll state ownership. |
| Wheel scroll | `vertical_wheel_bounce_latency_48`, `wrapped_wheel_bounce_latency_24` | `wheel_event_count`, update/capture counters, and paint latency. |
| Command zoom | `zoom_wheel_bounce_latency_24` | Paint latency and zoom correctness verifier. Despite the historical scenario name, this uses `SCI_ZOOMIN` / `SCI_ZOOMOUT`, not Ctrl-wheel events, and must not be used to validate Ctrl-wheel event acceptance or `wheel_event_count`. Real Ctrl-wheel zoom coverage needs a future benchmark or smoke test scenario. |
| Scroll after content mutation | `scroll_after_edit_latency_32` | `full_update_count` should remain expected after edit; future reuse-reject reason should show mutation marker as the cause. |
| Selection drag and mouse overlay movement | `selection_drag_latency_48` | `update_requests`, overlay/full counts, paint latency. Add autoscroll source counters before changing mouse-move invalidation. |
| IME commit/preedit/read-only paths | Existing smoke IME malformed/read-only coverage | Correctness now. Add IME source counters and, if practical, a focused benchmark or smoke profiling hook before migrating IME scheduling. |
| Notifications | Existing smoke notification API tests | Correctness now. Add notification source counters before migrating notification scheduling. |
| Core fine timer/autoscroll | Existing source behavior only | Add timer/autoscroll source counters or focused tests before migrating core timer scheduling. |
| Caret blink fast path | Existing profiling counter, no active benchmark scenario | Add a focused blink scenario or smoke profiling test before changing blink scheduling. |
| `enableUpdate()` gating | Existing source behavior only | Add focused smoke/profiling coverage before moving suppression/resume state. |

## Comparison Strategy

Use the process in `performance_optimization_process.md`:

- capture at least five baseline runs and at least five post-change runs on the
  same machine;
- compare distributions, not one run;
- use the baseline range (`max - min`) as the minimum noise floor;
- inspect the hierarchical profile when a scalar or counter regresses.

For Phase 3A, the implementation target is behavior-equivalent extraction, not
optimization. The pass/fail bar is therefore stricter than for a deliberate
performance optimization.

Hard failures:

- any correctness test failure;
- any benchmark `correctness_failures` entry;
- any benchmark timeout count increase above the baseline maximum;
- `full_update_count` increases above the baseline maximum in caret movement,
  selection drag, direct-query, or scroll-reuse scenarios;
- `blink_only_update_count` disappears or blink updates start incrementing
  full/static capture counters in the future blink scenario;
- once instrumented, `vertical_scroll_reuse_count` drops below the baseline
  minimum for vertical scroll scenarios;
- once instrumented, property-sync count increases above the baseline maximum
  for caret movement or overlay-only scenarios;
- once instrumented, scheduled polish/update count increases above the
  baseline maximum without a matching and reviewed correctness reason.

Performance failures:

- for paint-latency scenarios that emit `mean_ms`, `median_ms`, and `p95_ms`,
  any of those values regresses by more than the larger of the measured
  baseline noise range and 5 percent for any primary Phase 3A scenario;
- for command-elapsed scenarios that emit only `elapsed_ms`, such as
  `caret_move_right_5000` and `insert_character_2000`, compare the distribution
  of `elapsed_ms` across baseline and post-change runs. A regression larger than
  the larger of the measured baseline noise range and 5 percent is a failure;
- `metrics.update_polish.total_ms`, `metrics.build_render_snapshot.total_ms`,
  or `metrics.update_paint_node.total_ms` regresses by more than the larger of
  the measured baseline noise range and 5 percent, unless the scalar benchmark
  remains within noise and reviewers accept the attribution;
- a hierarchical profile shows cost moved into a newly introduced invalidation
  helper in a frequent path and the movement is outside the measured noise
  floor.

Review-required but not automatic failure:

- small timing changes inside the baseline noise range;
- counter changes caused by intentionally added metrics-only instrumentation;
- platform-specific visual-baseline differences on a runner that is not the
  baseline owner;
- conservative extra invalidation in a path not exercised by existing tests,
  provided a focused test and benchmark are added before merging.

## Metrics-Only Instrumentation Package

If reviewers decide the missing metrics are required before Phase 3A source
migration, create a separate metrics-only package. That package must not change
invalidation behavior. It should land and be reviewed before any
`RenderInvalidationState` implementation.

Future write scope for that package:

| File or area | Allowed metrics-only changes |
| --- | --- |
| `src/public/scintillaquick_item.cpp` | Add profiling counters, JSON fields, source tags, capture-path counters, property-sync timing/counting, and request/coalescing counters. |
| `src/core/scintillaquick_core.cpp` / `.h` | Only if needed to tag core timer invalidation source before it calls back to the item. No Scintilla behavior changes. |
| `benchmarks/embedded_editor/main.cpp` | Add focused scenarios for caret blink, IME, direct callback mutation, timer/autoscroll, or update gating if counters cannot be covered by existing scenarios. |
| `tests/smoke/main.cpp` | Add focused correctness/profiling smoke coverage for metrics that require deterministic event paths. |
| `tests/support/` | Extend existing test helpers only to observe profiling output or paint boundaries. |
| `docs/render_invalidation_metrics_baseline.md` | Record implemented metrics and update the baseline instructions. |

Files that should remain out of scope for metrics-only instrumentation:

- `src/render/scintillaquick_scene_graph_renderer.cpp`;
- `src/render/render_frame.h`;
- `src/core/scintillaquick_dispatch_table.h`;
- installed public headers;
- vendored Scintilla.

Renderer profiling scopes or renderer-node metrics belong in a separate future
renderer metrics proposal, not in the Phase 3A invalidation metrics baseline.

Suggested metrics-only additions:

- `scheduled_update_count`;
- `coalesced_update_request_count`;
- `suppressed_update_request_count`;
- `caret_blink_schedule_count`;
- `vertical_scroll_reuse_count`;
- `vertical_scroll_reuse_rejected_content_modified_count`;
- `vertical_scroll_reuse_rejected_x_offset_count`;
- `vertical_scroll_reuse_rejected_range_count`;
- `static_mutation_mark_count`;
- `property_sync_count` and `property_sync` timing metric;
- source counters for dispatch, direct callbacks, notifications, wheel zoom,
  wheel scroll, explicit scroll, mouse autoscroll, IME, core timer, focus,
  key, and `enableUpdate()` resume.

Instrumentation constraints:

- use inactive-by-default counters behind the existing profiling session;
- avoid heap allocation in `request_scene_graph_update()` and caret blink paths;
- prefer integer counters and existing `Profiling_metric` style timing;
- keep JSON additive so existing benchmark consumers continue to work;
- do not expose new public API solely for Phase 3A metrics;
- do not combine metrics-only changes with invalidation state extraction.

## Baseline Artifact Checklist

Before starting Phase 3A implementation, reviewers should be able to inspect:

- commit SHA and build configuration;
- environment variables and platform plugin;
- CTest command and result;
- five or more benchmark JSON files for the selected scenarios;
- per-scenario table containing timing distribution and profiling counters;
- list of missing metrics accepted for deferral or scheduled for the
  metrics-only package;
- explicit pass/fail comparison thresholds for the next implementation slice.

The baseline artifact can live outside the repository if it is large. If a
summary is committed, it should be a small Markdown table or JSON summary, not
the full set of raw benchmark reports.

## Implementation Gate

Phase 3A implementation may begin only after one of these is true:

- reviewers accept that existing counters are sufficient for the first
  behavior-equivalent state extraction slice; or
- the metrics-only instrumentation package is implemented, reviewed, and has
  its own baseline captured.

The first implementation slice should be rejected if it changes capture-path
counts, property-sync frequency, update coalescing behavior, caret blink
behavior, or scroll-reuse eligibility without an explicit test and benchmark
reason.
