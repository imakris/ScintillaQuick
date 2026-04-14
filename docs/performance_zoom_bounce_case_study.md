# Case Study: Optimizing `zoom_wheel_bounce_latency_24`

This document is a worked example of the process defined in
[`performance_optimization_process.md`](performance_optimization_process.md).
It exists so that anyone (human or agent) trying to reproduce the
optimization loop on this codebase can see each phase play out once on a
real scenario, with actual numbers and an actual code change.

The target was the `zoom_wheel_bounce_latency_24` benchmark scenario,
which sends 24 alternating zoom-in / zoom-out wheel events and records
the per-paint latency for each.

## 1. Preconditions (check)

- **Correctness oracle.** The CTest suite (smoke, dispatch-table,
  frame-validation, embedded-benchmark with its per-step verifier) runs
  under `QT_QPA_PLATFORM=offscreen`, and the benchmark's scenario
  verifier (`verify_visible_rows`, and the zoom scenario's
  zoom-level-match verifier) returns exit code `2` on any correctness
  drift.
- **Repeatable benchmark.** `scintillaquick_embedded_benchmark -s zoom_wheel_bounce_latency_24`
  produces a JSON document with `median_ms`, `p95_ms`, `mean_ms`,
  `max_ms`, `elapsed_ms`, `timeout_count`, and a full hierarchical
  profile attached to the scenario.
- **Hierarchical profiling.** `SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE` is
  already instrumented through the renderer, core, platform, and
  Scintilla layout paths, so a scenario's profile tree can be drilled
  down without adding new instrumentation up-front.
- **Branch discipline.** Work happened on
  `claude/optimize-library-performance-3mvdW`.

## 2. Baseline and Noise Floor (`HEAD` before the change)

Seven runs of the full benchmark under the same environment
(`QT_QPA_PLATFORM=offscreen`, 11pt deterministic bundled font,
1600x900 window, 25000-line document):

| metric       |   median |    range (max-min) |
|--------------|---------:|------------------:|
| `median_ms`  |   7.456  |           0.718   |
| `p95_ms`     |   8.560  |           1.042   |
| `mean_ms`    |   7.717  |           0.677   |
| `max_ms`     |  12.639  |           2.268   |
| `elapsed_ms` | 185.252  |          16.255   |

The baseline noise floor for this scenario (the range across 7 runs) is
roughly 0.7 ms on `median_ms` / `mean_ms`, 1.0 ms on `p95_ms`, and ~16
ms on total `elapsed_ms`. An improvement had to be larger than that to
count as real.

## 3. Drill-Down to the Hotspot

Taking the hierarchical profile from run #1 of the baseline, walking
from root to leaf and picking the dominant child at each level:

```
[root]  0.000 ms
  item.update_paint_node                                                 24  total= 118.517 ms  avg= 4.938
    renderer.update                                                       24  total= 118.484 ms
      renderer.update_from_frame                                          24  total= 118.470 ms
        renderer.update_from_frame.text                                   24  total= 116.879 ms
          renderer.text_node.update_from_visual_line                    1680  total= 113.739 ms
            .layout_run                                                 1680  total= 106.557 ms
              .attach_node                                              1680  total=  56.046 ms
                .attach_node.add_text_layout                            1680  total=  55.312 ms
              .shape_text                                               1680  total=  45.037 ms
              .create_layout                                            1680  total=   4.987 ms
            .rebuild                                                    1680  total=   6.349 ms
            .reuse_check                                                1680  total=   0.267 ms
  item.update_polish                                                      48  total=  17.998 ms
    item.build_render_snapshot                                            24  total=  17.933 ms
      core.current_render_frame                                           24  total=  17.626 ms
        core.capture_current_frame                                        24  total=  17.046 ms
          core.capture_current_frame.paint_text                           24  total=  16.588 ms
            core.capture_current_frame.paint_text.layout_line           1680  total=  13.492 ms
              ...measure_segment                                        1680  total=   3.572 ms
                ...measure_widths                                       1680  total=   3.310 ms
```

Read it aloud:

- `item.update_paint_node` is 118 ms of the scenario, which is ~87% of
  the total measured scope time.
- Inside `update_paint_node`, practically all of it (99%) is in
  `renderer.update_from_frame.text`.
- That, in turn, is all in `update_from_visual_line`, 1680 calls -
  70 per paint × 24 paints. That lines up with "70 visible lines".
- And within each visual line update, the cost is split between
  `attach_node.add_text_layout` (55.3 ms, 47% of `layout_run`),
  `shape_text` (45.0 ms, 38% of `layout_run`), and the smaller
  `create_layout` / `rebuild` / `reuse_check` siblings.
- The Scintilla-side `capture_current_frame.paint_text` is only 17 ms,
  about 13% of the total. The renderer pipeline, not the line-layout
  pipeline, is the dominant cost.

**Hypothesis (§2.3 output):** on every zoom step, every visible line's
`Scene_graph_frame_text_node` falls off the existing reuse fast paths
(because `cached_run.font != run.font` when the font size changes), so
each one goes through the full `rebuild` path: it builds a fresh
`QTextLayout`, shapes it, and calls `QSGTextNode::addTextLayout` to
push new glyph runs into the scene graph. Since the benchmark bounces
between exactly two zoom levels, the work being done on every other
frame is the work that was *already done* on the frame before the
previous one. If the node could remember one prior (`content`,
`QSGTextNode`) snapshot and restore it on a match, every alternate
paint would skip the shape + attach path entirely.

## 4. The Change

A two-slot LRU was added inside `Scene_graph_frame_text_node`
(see `src/render/scintillaquick_scene_graph_renderer.cpp`). The class
now keeps, in addition to its "active" cached state, a single
`m_backup` slot holding:

- the previously-active `QSGTextNode *` (detached from the scene graph
  but still owning its shaped glyph runs),
- the matching `QTextLayout` instances,
- the matching `Text_run` list (for content comparison),
- the layout positions and translation from when it was populated,
- its `Visual_line_key`.

The `update_from_visual_line` path gains three new helpers (plus a
small refactor: `layouts_match_content` is routed through a static
`runs_match_content(cached_runs, vl)` so the backup slot can share
the same comparison logic):

- `backup_layouts_match_content(vl)` - run-for-run comparison against
  `m_backup.cached_runs`, using the same fields as
  `layouts_match_content` (`style_id`, `direction`, `width`, `text`,
  `foreground`, **`font`**, ...).
- `swap_active_with_backup()` - swaps every cached-state member between
  the active slot and the backup, and, crucially, swaps which
  `QSGTextNode` is attached to `m_transform_node`. The previously
  backup node, whose glyph runs were baked at the requested font, is
  now the visible active node.
- `evict_active_to_backup(window)` - called right before a rebuild, so
  that the state being about to be overwritten is preserved as the
  backup for future restoration. The previously backup `QSGTextNode`
  (if any) is reused as the new active node, so the scene graph does
  not accumulate nodes across evictions.

The sequence inside `update_from_visual_line` becomes:

1. `reuse_check` against the active slot (unchanged fast path).
2. *New:* if the backup slot is populated, has the same key, and
   matches the new runs, `swap_active_with_backup()` and apply the
   uniform translation delta. Return without re-shaping or
   re-attaching.
3. *New:* otherwise, `evict_active_to_backup(window)` so the current
   state is preserved before it is overwritten.
4. Rebuild into the active slot (unchanged slow path).

The destructor was updated to `delete m_backup.text_node` explicitly,
because the backup's `QSGTextNode` is detached from the scene-graph
parent chain while it is inactive and is therefore not owned by any
parent `QSGNode`.

The margin path (`update_from_margin_text`) and the position-matching
helpers (`positions_match`, `uniform_translation_delta`) are
unchanged. `layouts_match_content` was refactored to route through a
static `runs_match_content` helper so the same comparison logic is
reused against both the active slot and the backup.

Correctness notes:

- The backup is only considered when `backup_layouts_match_content`
  returns true, which requires every translation-invariant field
  (including the font) to match exactly. A stale backup can never be
  served: if it does not match byte-for-byte, it is evicted by the
  next rebuild.
- The glyph positions *inside* a `QSGTextNode` are committed at
  `addTextLayout` time and cannot be shifted per-run after the fact,
  only uniformly by the transform node. Before swapping backup and
  active, the restore path therefore computes whether the new
  request's per-run positions are a uniform translation of the
  backup's cached positions. If they are not, the restore is *not*
  attempted and the update falls through to the normal rebuild path -
  no stale glyph positions can escape.
- For the zoom-bounce scenario in practice the per-run positions at
  a given zoom level are stable: Scintilla produces the same run
  offsets for the same (text, style, font) tuple, and the vertical
  position of a visible line is a function of `topLine * lineHeight`
  which recovers exactly when the zoom returns to its previous value.
  So the restore path is taken on every alternate paint.
- The destructor-owned backup node is only ever detached from the
  scene graph chain when `m_backup.text_node != nullptr`, and is
  always either `delete`d by the destructor or reused as the new
  active node by `evict_active_to_backup` - never leaked.
- No change to Scintilla itself, no change to the Scintilla platform
  layer, no change to how `Captured_frame` or `Render_frame` is
  populated, so the structured frame the core layer publishes is
  identical before and after the change.

## 5. Measurement

Seven runs of the optimized build, same environment as the baseline:

| metric       |  baseline median | optimized median |   delta |       verdict |
|--------------|-----------------:|-----------------:|--------:|--------------:|
| `median_ms`  |           7.456  |           6.051  |  -18.8% |  **IMPROVED** |
| `p95_ms`     |           8.560  |           7.747  |   -9.5% |  within noise |
| `mean_ms`    |           7.717  |           6.394  |  -17.1% |  **IMPROVED** |
| `max_ms`     |          12.639  |          12.480  |   -1.3% |  within noise |
| `elapsed_ms` |         185.252  |         153.489  |  -17.1% |  **IMPROVED** |

The marker "IMPROVED" is applied only when the delta is strictly
larger than the baseline noise range on that metric, per the rule in
`performance_optimization_process.md` §2.2. Deltas smaller than the
noise range are reported but not claimed as wins.

Cross-checks on *other* scenarios (same 7 baseline runs vs. 7
optimized runs), to confirm there is no silent regression:

- `caret_step_{left,right}_latency_64`: within noise, slight
  improvement in `p95_ms` (about -2 to -4%).
- `vertical_scroll_*` and `vertical_wheel_bounce_*`: all metrics
  within noise (deltas less than the baseline range).
- `wrapped_wheel_bounce_latency_24`: within noise.
- `scroll_after_edit_latency_32`: within noise.
- `selection_drag_latency_48`: within noise.
- Command-elapsed scenarios (`load_large_document`,
  `caret_move_right_5000`, `insert_character_2000`, `page_down_250`,
  `resize_window`): `elapsed_ms` within noise or small improvement.

No scenario regressed beyond its noise range.

The optimized profile also shrank the hotspot the way the hypothesis
predicted:

```
[root]  0.000 ms
  item.update_paint_node                                                   24  total=  22.829 ms  (was 118.517)
    renderer.update_from_frame.text                                        24  total=  21.017 ms  (was 116.879)
      renderer.text_node.update_from_visual_line                         1680  total=  17.732 ms  (was 113.739)
        .restore_from_backup                                             1564  total=   4.865 ms  (new fast path)
        .layout_run                                                       116  total=  11.635 ms  (was 106.557)
          .attach_node.add_text_layout                                    116  total=   6.982 ms  (was  55.312)
          .shape_text                                                     116  total=   3.901 ms  (was  45.037)
```

Reading this:

- `layout_run` is now entered **116** times across the whole scenario
  instead of **1680**. The other 1564 visual-line updates hit the
  new `restore_from_backup` fast path (swap instead of rebuild).
- The dominant leaves (`add_text_layout`, `shape_text`) dropped by
  roughly the same 14× factor (55 ms → 7 ms, 45 ms → 4 ms), which is
  the ratio the 2-slot cache was predicted to deliver for a pure
  two-state bounce.
- The Scintilla side (`capture_current_frame.paint_text`) is
  essentially unchanged (17 ms total, was 17 ms total) - the change
  is strictly in the renderer pipeline, as expected.

The hotspot that dominated the zoom bounce scenario has been
eliminated.

## 6. Correctness Verification

CTest suite under `QT_QPA_PLATFORM=offscreen`:

- `scintillaquick_smoke_test`: PASS
- `scintillaquick_dispatch_table_test`: PASS
- `scintillaquick_frame_validation_test`: PASS
- `scintillaquick_embedded_benchmark` (runs every scenario including
  the `zoom_wheel_bounce_latency_24` zoom-level verifier and the
  `verify_visible_rows` row-content verifier on scroll scenarios):
  PASS, exit code 0, `correctness_failures` empty for every scenario.

The `scintillaquick_visual_regression_test` target fails in this build
environment both before and after the change, with **byte-identical
`*_actual.png` artifacts in both runs** (verified by `cmp`). Those
failures are pre-existing environmental drift (font rasterization /
hinting differences vs. the stored baselines) and are not attributable
to this change: the optimization produces pixel-identical output
compared to the pre-change build on every visual scenario.

## 7. Next Iteration Pointer

Per `performance_optimization_process.md` §2.6, after a successful
iteration the next step is to re-profile and see where the new hot
spot is. Looking at the optimized profile:

- `item.update_paint_node` is now 22.8 ms total, roughly matching
  `item.update_polish` at 18.9 ms. Neither dominates the other.
- Inside `update_paint_node`, `restore_from_backup` now accounts for
  4.9 ms and `rebuild` for 11.6 ms. Further attacks on the renderer
  here would need to eliminate the residual 116 rebuilds. Most of
  those are the unavoidable "first visit at this zoom level" cost: on
  the initial zoom and on any level the backup has been evicted out
  of. A natural next step is to raise the cache from 2 slots to *N*,
  keyed by a hashable font signature, so deeper bounces (zoom=0 ↔
  zoom=1 ↔ zoom=2) also fit.
- Inside `update_polish` / `capture_current_frame.paint_text`,
  `layout_line` is 14 ms of 1680 calls. The `measure_segment` leaf
  at 3.7 ms is already small; deepening it further would cross into
  Scintilla-level changes. That is worth a separate iteration with
  its own hypothesis, not something to fold into this one.

The loop terminates here for this task: the zoom-bounce hot path has
been measurably fixed by a localized, reviewable change, and the next
slowest thing is a non-dominant scope that deserves its own pass
through the process.
