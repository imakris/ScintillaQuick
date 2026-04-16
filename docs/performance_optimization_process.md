# Iterative Performance Optimization Process

This document defines the repeatable process that the project (and any LLM
agent acting on it) should follow when asked to "make something faster".
The intent is to move optimization from ad-hoc speculation to a disciplined
loop that is guided by measurement, preserves correctness, and is executable
without human intuition in the loop.

The process is written generically. Section 2 describes the steps. Section 3
spells out the concrete adaptation for this codebase (ScintillaQuick) and the
tools that already exist to support it.

## 1. Preconditions

Before any optimization attempt is reasonable, the following must hold. If
any of them is missing, fixing that is step zero: you do not optimize on top
of a system you cannot trust to flag regressions.

1. **A correctness oracle.** A test suite (unit + integration) that, given a
   change, can tell you whether the observable behavior of the system is
   unchanged. The suite must be runnable in the environment where the
   optimization work happens, must return a non-zero exit code on failure,
   and must cover the code paths you intend to touch. For UI or
   render-oriented code, "correctness" includes visual-regression coverage
   (e.g. pixel/row snapshots for specific scenarios), because performance
   work in a renderer can silently break rendering without any logic error
   visible to a unit test.
2. **A repeatable benchmark.** A deterministic workload that exercises the
   code path under interest and emits *numerical* results. "It feels faster"
   is not an admissible signal. The benchmark must be rerunnable from a
   clean state and must report numbers that are stable enough that the
   expected improvement is larger than the noise floor (see section 2.2).
3. **Hierarchical profiling instrumentation.** The ability to attribute cost
   to nested scopes, so that "function F is 40% of the frame" does not hide
   the fact that 38% of that is actually a child function G. Flat
   profiling is acceptable as a starting point but must be deepenable
   locally in any scope that looks hot.
4. **Version control discipline.** Every optimization attempt happens on a
   branch, in a clean working tree, with the baseline numbers recorded
   before the change so the comparison is against a fixed reference rather
   than "whatever I remember".

The process below assumes all four preconditions are satisfied. If you are
an LLM and one of them is missing, your job is to add it *first* and to
stop optimizing until it is in place.

## 2. The Loop

The optimization loop has six phases. It terminates when the target metric
either reaches the goal or is no longer dominated by a hotspot that can be
attacked without an outsized change (in which case the outcome is not
"failure", it is "this area is already near its architectural floor").

### 2.1. Establish the Baseline

Run the benchmark against the current `HEAD` of the branch you will be
optimizing on. Capture:

- The scalar metric(s) you plan to improve (e.g. p95 paint latency,
  throughput, elapsed time for a scripted scenario).
- A full hierarchical profile produced *during* the benchmark run, not
  separately, so that the profile is attributable to the same workload as
  the scalar.
- The exact command line, the machine state assumptions (build type,
  platform plugin, env vars), and the git SHA you ran against.

Baselines must be produced in **Release** or equivalent optimized-build
mode. Profiling a debug build measures the debug build; it has almost no
predictive value for optimized code.

### 2.2. Characterize the Noise Floor

Run the same baseline benchmark **N** times (N >= 5 is a reasonable minimum,
more if the scenario is short). For each scalar metric, compute the
distribution across the N runs. Your optimization target must beat the
baseline by strictly more than the run-to-run variation of the baseline,
otherwise any reported improvement is statistically indistinguishable from
noise.

Pragmatically, a good threshold is:

    improvement required > max(baseline_values) - min(baseline_values)

If you cannot meet that, either the workload is too noisy (tighten it) or
the improvement is inside the noise and you should not claim it.

### 2.3. Identify the Dominant Hotspot by Hierarchical Drill-Down

This is the step that most ad-hoc "optimizations" skip, and it is the
step that makes the process repeatable.

Starting from the root of the hierarchical profile:

1. Sort the children of the current node by total time descending.
2. Pick the child that dominates the parent's time. "Dominates" is a
   judgement call, but a useful rule is: if one child accounts for >= 50%
   of the parent, drill into it; if the time is roughly evenly split
   across many children, the hotspot is in the parent's *fan-out*, not in
   any single child.
3. If the dominant child is already a named scope, recurse into it and
   repeat.
4. **If the dominant child is a leaf scope but you cannot tell what inside
   it is slow, you have hit the instrumentation floor.** Do not guess.
   Deepen the instrumentation by adding more nested profiling scopes
   inside that leaf, rebuild, rerun the benchmark, and repeat the
   drill-down. This is *hierarchical deepening*: the profile grows more
   detailed only where the cost justifies it.
5. Stop when the hotspot is a scope small and specific enough that an
   optimization would have a localized, testable effect. That is your
   target.

The output of this step is a single claim of the form: *"scope X accounts
for roughly Y% of scope Z's time, and inside X, operation Q is the
proximate cost."* Write that down. It is the hypothesis the next step will
attack.

### 2.4. Propose and Apply a Minimal Change

A valid optimization proposal has:

- **A mechanism**: *why* it will reduce the cost of the identified hotspot.
  Not "this should be faster", but "operation Q is redoing work W on every
  call; if we memoize W keyed by K, each repeat pays a lookup instead of
  a rebuild".
- **A predicted impact**: how much of the baseline you expect to reclaim,
  on which metric. The prediction does not have to be correct to the
  percent, but it has to be falsifiable.
- **A correctness argument**: why it does not change observable behavior.
  List the invariants you rely on, and note any that the existing tests
  do not cover (those are candidates for new tests).
- **Minimal scope**: touch only what is necessary to validate the
  hypothesis. Do not combine unrelated refactors with an optimization;
  that makes it impossible to attribute any regression to a cause.

Apply the change on your optimization branch as a single commit (or a
small number of commits with clear messages).

### 2.5. Re-Run the Benchmark and the Test Suite

Without any other changes, rerun:

1. The full correctness test suite. If anything regresses, stop; the
   change is unsafe and must be reverted or fixed before any performance
   claim is made. Do *not* tune the test to match the new behavior to
   "prove" the optimization is correct; that is cargo culting.
2. The benchmark, N times again, collecting the same scalar metrics and
   a fresh hierarchical profile.

Compare the new distribution to the baseline distribution from section 2.1 and
the noise bound from section 2.2. An optimization is only "real" when:

- The target metric improved by strictly more than the noise bound of
  the baseline, *and*
- The hierarchical profile shows the hotspot scope actually shrinking in
  proportion (not just globally getting smaller because of an unrelated
  fluke), *and*
- No other scope got meaningfully worse (no silent regression).

Record the result. If the improvement is real, the change is a keeper.

### 2.6. Decide Whether to Continue

After a successful iteration, the hotspot has either:

- **Been eliminated** - it no longer shows up in the profile in any
  meaningful way. The next iteration will find a *different* dominant
  hotspot, and the loop continues from section 2.3 with the new profile. This
  is the interesting case and is the reason the process is iterative:
  optimizing one hotspot simply promotes the next one, and you only
  know which one by re-profiling.
- **Been reduced but is still dominant** - maybe another change can
  bite more of it. Return to section 2.3 and re-run the drill-down on the
  updated profile; you may find that the hotspot is now inside a
  different child scope.
- **Become non-dominant but is still visible** - the effort budget for
  further attacks on this scope is now smaller, because the expected
  impact has shrunk. Consider whether the next marginal hour is better
  spent on the new top hotspot.

The loop terminates when the goal metric hits its target, when the
expected benefit of another iteration falls below the cost of the change,
or when every remaining iteration would require restructuring the system
in a way the task does not authorize.

## 3. Concrete Adaptation to ScintillaQuick

ScintillaQuick already ships most of the machinery this process assumes.
This section maps each generic step to the specific tool in this tree.

### 3.1. The Correctness Oracle

The project's correctness coverage comes from CTest:

- `scintillaquick_dispatch_table_test` - verifies the Scintilla message
  dispatch table. Pure logic test; cheap.
- `scintillaquick_smoke_test` - end-to-end smoke test of the editor item
  under the `offscreen` Qt platform plugin.
- `scintillaquick_frame_validation_test` - validates that the structured
  *Captured_frame* the core layer publishes is self-consistent for
  specific scenarios (scroll, wrap, selection, etc.).
- `scintillaquick_visual_regression_test` - uses Qt's software scene
  graph to render to a deterministic image, compared to stored
  references. This is the oracle that catches rendering regressions a
  unit test cannot see.
- `scintillaquick_embedded_benchmark` (run as a CTest entry, also
  launchable standalone) - doubles as a correctness check: the scenarios that attach verifiers (`verify_visible_rows`, zoom level verification, etc.), and the benchmark returns exit code `2` if any verifier fails.

Any optimization that touches rendering or layout should be considered
unsafe until all of the above pass. The benchmark's built-in
`correctness_failures` field is the first thing to check in its output.

### 3.2. The Repeatable Benchmark

`benchmarks/embedded_editor/main.cpp` runs a battery of named scenarios
against a *real* `ScintillaQuick_item` instance inside a headless
`QQuickWindow`. For each scenario it emits a JSON document with, per
scenario:

- A name and a measurement kind (`command_elapsed` or `paint_latency`).
- Scalar timing stats (`mean_ms`, `median_ms`, `p95_ms`, `max_ms`,
  `elapsed_ms`, timeout counts).
- A nested `profiling` block containing the hierarchical profile (see
  section 3.3) for the duration of that scenario.

The scenarios relevant to performance work on scrolling, typing, and
*zooming* include:

- `vertical_scroll_step_latency_64`
- `vertical_scroll_page_step_latency_16`
- `vertical_scroll_bounce_latency_48`
- `vertical_wheel_bounce_latency_48`
- `wrapped_wheel_bounce_latency_24`
- `scroll_after_edit_latency_32`
- `caret_step_right_latency_64` / `caret_step_left_latency_64`
- **`zoom_wheel_bounce_latency_24`** - the specific target scenario for
  zoom performance. Alternates zoom in/out for 24 paint cycles.

Run a single scenario with:

```sh
QT_QPA_PLATFORM=offscreen \
QT_FONT_DPI=96 QT_SCALE_FACTOR=1 QT_ENABLE_HIGHDPI_SCALING=0 \
./build/scintillaquick_embedded_benchmark \
    -s zoom_wheel_bounce_latency_24 \
    -o /tmp/bench.json
```

and inspect `/tmp/bench.json`. To characterize the noise floor, run the
same command 5+ times and compare the distribution. If benchmark
automation helpers are added later, this document should point to
them explicitly.

### 3.3. The Hierarchical Profiler

`src/core/scintillaquick_hierarchical_profiler.h` defines
`Hierarchical_profiler` and two helpers:

- `SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("dotted.name")` - instrument a
  scope. The name is a free-form dotted identifier; by convention the
  first segment names the layer (`renderer.`, `core.`, `platform.`,
  `item.`) and deeper segments drill into specific operations.
- `Active_hierarchical_profiler_binding` - binds a profiler to the
  current thread so that `SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE` works in
  helper worker threads too (used by Scintilla's parallel line-layout
  path in `EditView::LayoutLine`).

The benchmark harness turns the profiler on around each scenario's measurement window, so the profile attached to a scenario's JSON tracks the work the scenario performs during that measurement, including any wait-for-paint or verifier logic inside the measured path.

**Deepening the instrumentation.** When drill-down in section 2.3 hits a leaf
scope whose internal structure is opaque, add more
`SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE` calls inside that leaf, rebuild,
and re-run. The cost of an inactive instrumentation macro is a null
pointer check, so leaving new scopes in place is fine; they only cost
anything when a profile session is active. This is what section 2.3 calls
"hierarchical deepening".

Scopes should be named hierarchically so the profile is
self-documenting. Existing examples:

- `renderer.text_node.update_from_visual_line.layout_run.shape_text`
- `core.capture_current_frame.paint_text.layout_line.measure_segment.measure_widths`
- `platform.measure_widths.cursor_positions.simple_glyph_path`

Each dot is a drill-down step in the profile tree.

### 3.4. A Worked Example: Zoom Latency

See [`performance_zoom_bounce_case_study.md`](performance_zoom_bounce_case_study.md)
for a narrative walkthrough of one full pass through the loop, applied
to `zoom_wheel_bounce_latency_24`. Read that document alongside this one
if you need to see the process executed on a concrete scenario.

## 4. Rules for an Automated / LLM Agent Executing This Process

The process above is written for a human, but it is designed to be
mechanizable. An LLM acting on this repository should treat the
following as hard rules:

1. **Never edit production code before the baseline is captured.** The
   baseline must be reproduced from `HEAD`. If you cannot get a stable
   baseline, stop and report that instead of guessing.
2. **Never claim an improvement without comparing N >= 5 runs against
   the baseline distribution.** A single pre/post pair is anecdote.
3. **Never bypass the correctness oracle.** If the test suite cannot be
   run (toolchain missing, etc.), the task is blocked on environment
   setup, not on "let's ship the change anyway".
4. **Never combine a refactor with an optimization.** If you notice
   unrelated cleanup that would help, land it as a separate commit so
   any regression is attributable.
5. **Never silently widen a cache's memory footprint.** If the
   optimization is "cache more", bound it, document the bound, and
   verify the bound is not crossed under the benchmark.
6. **Never skip section 2.6.** After a successful iteration, re-profile and
   decide whether to continue based on the new profile, not the old
   one. Optimizing a no-longer-dominant scope is a time sink.
7. **Always write down the hypothesis before the change.** "I am
   optimizing X because profile shows Y, and I expect Z savings." If
   you cannot write that sentence, you do not yet know what you are
   doing and section 2.3 is not finished.
8. **Always report the actual numbers, not adjectives.** "p95 went from
   8.12ms to 4.9ms on median-of-5 runs" is useful. "Zoom is much
   faster now" is not.

If any rule cannot be followed because the environment does not allow
it, say so explicitly and stop; do not substitute qualitative
assertions for the missing measurement.

## 5. Anti-Patterns

These are the mistakes the process is designed to make impossible.
They are listed explicitly so that an agent can recognise them in its
own draft work before publishing:

- **Optimising in debug builds.** Nothing you measure there predicts
  the release build. Release-mode baseline or nothing.
- **Optimising on the leaf of the profile you happened to recognise.**
  The job of section 2.3 is to let the profile pick the target, not your
  intuition.
- **"Optimising" code that is not in any profile.** If a scope does not
  appear in the hierarchical profile for the benchmark you care about,
  it is not on your hot path; changing it is refactoring, not
  optimisation, and should be billed as such.
- **Introducing a cache without an invalidation story.** A cache that
  is not invalidated on the right event is a correctness bug in
  disguise.
- **Running the benchmark once, seeing a good number, and declaring
  victory.** One run is inside the noise. Always rerun.
- **Comparing against a stale baseline.** If the branch you are
  optimizing has moved since the baseline was taken, re-take the
  baseline on the new tip before you claim an improvement.
- **Removing profile scopes because they "add overhead".** An inactive
  `SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE` is a pointer check; its cost
  outside an active session is negligible. The scopes are what make
  future iterations of this process possible. Do not remove them
  unless one is provably *inside* the critical path of the new
  optimisation, in which case you should move it, not delete it.

## 6. Where to Look Next

- [`performance_zoom_bounce_case_study.md`](performance_zoom_bounce_case_study.md) -
  concrete walkthrough of the loop applied to zoom bouncing.
- [`architecture.md`](architecture.md) - the structural layering the
  profiler names refer to (`renderer.`, `core.`, `platform.`, `item.`).
- [`limitations.md`](limitations.md) - known performance-relevant
  constraints in the current implementation.

