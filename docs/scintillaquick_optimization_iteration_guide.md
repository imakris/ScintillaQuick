# ScintillaQuick Optimization Findings And Iteration Guide

## Current Position

ScintillaQuick is now functionally usable and materially faster than earlier revisions, but it is still not as snappy as native editors such as Notepad++ in some interaction paths.

The current priority is not broad correctness work. The current priority is targeted performance work, especially around zoom and any other interaction path that still feels noticeably slower than native Scintilla-based editors.

The active reference benchmark for zoom work is:

- `zoom_wheel_bounce_latency_24`

The current benchmark output shows the zoom path is still dominated by two coarse phases:

- `item.build_render_snapshot`
- `item.update_paint_node`

The hierarchical profiler narrowed that further:

- `item.build_render_snapshot`
  - `core.current_render_frame`
    - `core.capture_current_frame`
      - `core.capture_current_frame.paint_text`
- `item.update_paint_node`
  - `renderer.update_from_frame`
    - `renderer.update_from_frame.text`
      - `renderer.text_node.update_from_visual_line`

The bridge is not a meaningful hotspot:

- `core.render_frame_from_capture` is small relative to capture and renderer text work

## Confirmed Findings

### 1. Zoom still forces a full visible-frame rebuild

Every zoom tick still forces:

- a full Scintilla capture pass over the visible range
- a full scene-graph text update over the visible range

That is why zoom is still far from native-editor responsiveness.

### 2. Capture-side text work is a real hotspot

The deepest useful profiling result so far points into vendored Scintilla:

- `EditView.cxx`
- `LayoutSegments()`
- width measurement under `core.capture_current_frame.paint_text.layout_line.measure_segment`

This is the most concrete remaining capture-side zoom hotspot.

### 3. Renderer-side text work is still expensive, but no longer the first unknown

Renderer text updates are still large, but the profiling is already good enough to say:

- the renderer is not the only bottleneck
- further renderer-only tweaks are unlikely to unlock a large zoom win by themselves

### 4. Broad fast-path shortcuts are risky

A broad monospaced/fixed-pitch shortcut was explored and showed that substantial speed may be available in the text-measurement path, but that shortcut broke validation and was rejected.

Current rule:

- do not use broad font-classification shortcuts as a primary optimization strategy

### 5. The benchmark must stay correctness-aware

Performance work in retained/update-reuse paths can easily introduce stale-content bugs. The benchmark now needs to remain a correctness gate, not just a timing tool.

## Rejected Or Closed Directions

These are either already tried or already understood well enough that they should not be the next blind experiment:

- broad renderer-only reuse experiments without profiling evidence
- broad monospaced/fixed-pitch measurement shortcuts
- more bridge-focused optimization before capture and renderer text costs are reduced
- removing correctness checks from the benchmark in order to chase lower numbers

## Recommended Next Technical Targets

The next work should stay narrow and benchmark-driven.

### Target A: Deeper measurement-path optimization in vendored Scintilla

Focus area:

- `third_party/scintilla/src/EditView.cxx`
- `LayoutSegments()`
- any local measurement/cache layer it relies on

Likely safe directions:

- narrower width-measurement caching inside the measurement path
- avoiding repeated measurement within one capture step where the inputs are unchanged
- optimizing `PositionCache::MeasureWidths()` usage or equivalent local call structure

Avoid:

- shortcuts based only on declaring a font "monospaced"
- changes that skip validation coverage for wrapped lines, tabs, indicators, or annotations

### Target B: Deeper renderer text-path profiling only if the hotspot moves back there

Only do this if Target A materially reduces capture cost and the dominant time clearly shifts into:

- `renderer.update_from_frame.text`
- `renderer.text_node.update_from_visual_line`

If that happens, profile and optimize inside the renderer at a finer granularity again.

## Agent Workflow For ScintillaQuick Optimization

This is the default iterative workflow an agent should follow when optimizing ScintillaQuick.

### 1. Anchor every optimization to a fixed benchmark

Do not optimize against vague impressions alone.

Pick a concrete scenario and keep using it until the hotspot stops moving. For zoom work, the default reference is:

- `zoom_wheel_bounce_latency_24`

### 2. Start with profiling, not with speculative edits

Before changing code:

- run the reference benchmark
- read the flat metrics
- read the hierarchical `scope_tree`
- identify the current dominant scope

If the hotspot is still too coarse to act on, add deeper profiling only under the dominant scope and rerun.

### 3. Push profiling deeper until the hotspot stops moving

It is normal for the hotspot to move several times. Keep iterating:

1. instrument the dominant scope
2. rerun the benchmark
3. inspect the new dominant child scope
4. repeat until the next optimization target is concrete

Stop instrumenting when one of these is true:

- the same narrow hotspot stays dominant across reruns
- the next optimization slice is clear and small
- more profiling would only restate the same result

### 4. Keep optimization slices narrow

Each optimization attempt should target one concrete hotspot class only.

Examples:

- one capture-side measurement cache refinement
- one renderer text-node reuse refinement
- one scheduling/invalidation simplification

Do not mix broad capture, renderer, and benchmark rewrites in one slice unless the benchmark proves that all three are required.

### 5. Treat correctness as a hard gate

Every optimization slice must preserve:

- `frame_validation`
- `visual_regression`
- benchmark correctness checks

If a change is fast but makes validation fail, it is not accepted.

### 6. Keep failed experiments documented

If an experiment is promising but unsafe:

- record what was tried
- record what benchmark delta it showed
- record why it was rejected

Do not silently discard useful negative results.

### 7. Prefer evidence-driven handoff notes

If an agent stops after a useful finding, the handoff should say:

- which benchmark was used
- what the dominant scope was
- what was tried
- what moved
- what did not move
- what the next safe slice is

## Acceptance Bar For The Next Zoom Tranche

The next accepted zoom optimization should:

- materially improve `zoom_wheel_bounce_latency_24`
- keep `frame_validation` green
- keep `visual_regression` green
- keep benchmark correctness checks green
- explain which specific scope moved and by how much

The expected next focus is:

- capture-time text measurement inside `LayoutSegments()`

That is the current best evidence-backed direction.
