# Native-Text Renderer Phase 0 Work Package

## Purpose

This document converts
`native_text_scene_graph_renderer_plan.md`
into an implementation-ready Phase 0 package.

Phase 0 is not the renderer rewrite itself. It is the feasibility gate that
proves the proposed capture seam, the neutral collector boundary, and the first
native scene-graph translation path before broader Scintilla refactoring
begins.

The output of Phase 0 should be:

- a narrow prototype implementation
- benchmark and validation evidence
- a short feasibility note
- a go / revise decision for the larger rewrite

If Phase 0 fails, the project should revise the seam or fragment granularity
before touching more of the renderer.

## Scope

Phase 0 should prove only these things:

1. Scintilla's existing paint traversal exposes an authoritative shared seam
   that can feed capture without duplicating traversal logic.
2. Wrapped visual fragments can be captured from Scintilla internals and
   rendered as native Quick text.
3. One selection case, one indicator case, and one margin primitive can be
   captured and rendered from the same authority.
4. The neutral vendored-side capture model is practical.
5. Shaping and geometry drift between Scintilla and `QSGTextNode` is
   measurable and acceptable for the initial path.

Phase 0 should not attempt:

- full margin support
- full indicator family support
- full multi-caret support
- retained update optimization
- complete popup redesign
- complete IME or BiDi hardening

## Deliverables

Phase 0 is complete only when all of these exist:

1. a compiling narrow capture path in vendored Scintilla
2. a compiling Qt-facing bridge in `ScintillaQuickCore`
3. a compiling narrow scene-graph render path that consumes the new frame data
4. a benchmark result set for the Phase 0 corpus
5. a feasibility note in `docs/`

Suggested feasibility-note filename:

- `docs/native_text_scene_graph_phase0_findings.md`

## Primary seam candidate

The primary seam should be the existing `EditView` paint traversal, not a new
independent traversal.

Recommended capture points:

1. `third_party/scintilla/src/EditView.cxx`
   `EditView::DrawLine(...)`
   Use this as the phase coordinator.

   Why:

   - it already orchestrates per-subline rendering
   - it knows the current `DrawPhase`
   - it already sequences background, foreground, fold display text,
     annotation text, indicators, and other overlays

2. `third_party/scintilla/src/EditView.cxx`
   `EditView::DrawForeground(...)`
   Use this as the first text-fragment seam.

   Why:

   - it iterates authoritative `TextSegment`s through `BreakFinder`
   - style, selection, representation, and screen geometry decisions are local
   - segment rectangles and visual ordering are already available here

3. `third_party/scintilla/src/MarginView.cxx`
   `MarginView::PaintOneMargin(...)`
   Use this as the first margin seam.

   Why:

   - it already owns the margin painting surface
   - it is the natural place to prove one margin primitive without synthesizing
     gutter state outside Scintilla

4. `third_party/scintilla/src/EditView.cxx`
   `EditView::LayoutLine(...)`
   Read-only dependency for geometry source validation.

   Why:

   - it already computes wrapped sublines
   - it already computes tab-expanded positions
   - it already populates `LineLayout` state used by the paint path

## Recommended first supported primitive subset

Phase 0 should stay intentionally narrow.

Implement exactly these first:

- wrapped text fragments from `DrawForeground(...)`
- one selection-background rectangle case
- one underline-style indicator case
- one line-number margin text primitive

That subset is enough to prove:

- shared traversal
- authoritative wrap geometry
- neutral capture records
- Qt-facing frame translation
- one non-text geometry family
- one margin-origin primitive family

## Minimal vendored-side collector API

The collector should be optional and narrow.

Recommended first interface:

```cpp
class Render_collector
{
public:
    virtual ~Render_collector() = default;

    virtual void add_text_segment(
        const Captured_text_segment& segment) = 0;

    virtual void add_selection_rect(
        const Captured_rect& rect) = 0;

    virtual void add_indicator_primitive(
        const Captured_indicator& indicator) = 0;

    virtual void add_margin_text(
        const Captured_margin_text& text) = 0;
};
```

Phase 0 should not expose every eventual primitive family yet. It only needs
enough surface to prove the seam and the bridge.

## Minimal vendored-side capture types

Suggested first-phase types:

```cpp
enum class Capture_direction : uint8_t
{
    LEFT_TO_RIGHT,
    RIGHT_TO_LEFT,
    MIXED,
};

struct Captured_text_segment
{
    float x;
    float y;
    float width;
    float height;
    float baseline_y;
    int visual_line_index;
    int document_line;
    int style_id;
    Capture_direction direction;
    bool is_represented_text;
    bool is_margin_text;
    std::string utf8_text;
};

struct Captured_rect
{
    float x;
    float y;
    float width;
    float height;
    uint32_t rgba;
};

struct Captured_indicator
{
    float x;
    float y;
    float width;
    float height;
    uint32_t rgba;
    int indicator_style;
};

struct Captured_margin_text
{
    float x;
    float y;
    float width;
    float height;
    float baseline_y;
    int document_line;
    std::string utf8_text;
    int style_id;
};
```

Phase 0 should keep these types in vendored code or in a small shared neutral
header used by vendored code, but they must remain Qt-free.

## Exact implementation strategy

### Step 1: introduce a narrow neutral header

Add one small header for Phase 0 capture types and the collector interface.

Recommended location:

- `third_party/scintilla/src/RenderCapture.h`

Contents:

- `Capture_direction`
- `Captured_text_segment`
- `Captured_rect`
- `Captured_indicator`
- `Captured_margin_text`
- `Render_collector`

Keep the header narrow. Do not add Qt includes or renderer-owned concepts.

### Step 2: thread the optional collector through the first paint seam

Recommended first change set:

- extend the narrow paint-path entrypoints with an optional
  `Render_collector* collector`
- default it to `nullptr`
- call the collector only when it is non-null

Recommended first functions:

- `EditView::DrawLine(...)`
- `EditView::DrawForeground(...)`
- `MarginView::PaintOneMargin(...)`

Do not create a parallel traversal. Reuse the existing traversal and insert
collector calls at authoritative points only.

### Step 3: capture wrapped text segments in DrawForeground

Inside `DrawForeground(...)`, emit one `Captured_text_segment` per accepted
Phase 0 fragment.

Rules:

- capture from the same segment geometry the painter path is using
- use existing visual ordering
- keep the first version conservative
- avoid coalescing across uncertain boundaries in Phase 0

If the first implementation needs one text segment per `BreakFinder` result,
that is acceptable. The question is feasibility, not optimization.

### Step 4: capture one selection case

Inside the existing selection/background handling path, emit one
`Captured_rect` per visible selection rectangle for the first supported case.

Phase 0 target:

- normal linear selection on a wrapped line

Do not implement rectangular selection yet.

### Step 5: capture one indicator case

Choose one indicator family that is not trivially reducible to a plain
background rectangle.

Recommended first case:

- underline-like indicator

Why:

- it exercises a non-text, non-background primitive family
- it is cheaper than starting with squiggles
- it is common enough to be representative

### Step 6: capture one margin primitive

Inside `MarginView::PaintOneMargin(...)`, capture one simple authoritative
primitive.

Recommended first case:

- line-number text

Why:

- it proves the margin seam without forcing icon or fold-geometry work early
- it is easy to validate visually

### Step 7: bridge captured data in ScintillaQuickCore

`ScintillaQuickCore` should request the narrow captured frame and translate it
into a Qt-facing Phase 0 frame object.

Recommended new files:

- `src/render/render_frame.h`
- `src/render/render_frame_builder.h`

Recommended first Qt-facing types:

- `Render_frame`
- `Text_fragment`
- `Selection_primitive`
- `Indicator_primitive`
- `Margin_text_primitive`

This bridge is where:

- UTF-8 becomes `QString`
- style ids map to Qt font and color data
- neutral geometry becomes `QRectF` / `QPointF`

### Step 8: render the Phase 0 frame in the scene graph

The renderer should consume only the new frame object for the Phase 0 path.

Recommended first rendering behavior:

- text fragments through `QSGTextNode`
- selection rectangles through geometry nodes
- underline indicator through geometry nodes
- line numbers through `QSGTextNode`

The Phase 0 path can live behind a temporary development flag if needed, but it
must remain in the real renderer stack, not in a disconnected toy example.

### Step 9: measure shaping divergence

The feasibility gate is incomplete unless it measures geometry agreement.

For a representative Phase 0 corpus, record:

- Scintilla fragment bounds
- Scintilla caret / selection geometry
- the final rendered fragment positions used by the scene graph

The output does not need to be pixel-perfect screenshots only. Numeric logging
or assertion-friendly traces are acceptable for the first gate.

## Concrete file touch plan

### Vendored Scintilla

Expected first edits:

- `third_party/scintilla/src/EditView.h`
- `third_party/scintilla/src/EditView.cxx`
- `third_party/scintilla/src/MarginView.h`
- `third_party/scintilla/src/MarginView.cxx`
- new `third_party/scintilla/src/RenderCapture.h`

Possible additional edit:

- `third_party/scintilla/src/Editor.cxx`

Only if needed to thread the collector request through the existing boundary.

### ScintillaQuick core / render bridge

Expected first edits:

- `src/core/ScintillaQuickCore.h`
- `src/core/ScintillaQuickCore.cpp`
- `src/render/scintillaquick_scene_graph_renderer.h`
- `src/render/scintillaquick_scene_graph_renderer.cpp`
- new `src/render/render_frame.h`
- optional `src/render/render_frame_builder.h`

### Public item

Keep changes small in Phase 0.

Expected touch only if needed:

- `src/public/ScintillaQuickItem.cpp`

Purpose:

- request the Phase 0 frame
- hand it to the renderer
- avoid broad public-item refactors during the feasibility gate

## Constraints for the implementation

Phase 0 should follow these constraints rigidly:

- no `QImage` fallback
- no new independent traversal of visible lines
- no direct Qt types in vendored Scintilla
- no attempt to solve all primitive families at once
- no retained-node optimization work yet
- no cleanup refactors outside the touched seam unless required by the design

## Success checklist

Phase 0 succeeds only if every item below is true:

- wrapped text is captured from Scintilla's paint path, not reconstructed from
  `SCI_*` messages
- the captured text renders through `QSGTextNode`
- one wrapped-selection case is captured and rendered correctly
- one underline-style indicator case is captured and rendered correctly
- one line-number margin primitive is captured and rendered correctly
- the collector seam is optional and does not require a parallel paint pass
- the neutral vendored-side types remain Qt-free
- fragment count and node count are measured on a representative wrapped file
- shaping divergence between Scintilla geometry and rendered text is measured
- the implementation can name the exact seam it would scale up for later phases

## Failure checklist

Phase 0 should be considered failed, or at least blocked for redesign, if any
of these happen:

- authoritative wrap fragments cannot be captured without duplicating traversal
- `QSGTextNode` rendering drifts materially from Scintilla geometry in the
  common corpus
- fragment count explodes beyond an operationally acceptable first version
- the collector seam requires invasive changes across unrelated Scintilla paths
- the margin path cannot expose even a simple authoritative primitive cleanly

If any of those fail, stop and write the findings before pushing further into
the rewrite.

## Benchmark and validation corpus

Phase 0 should use a deliberately small but revealing corpus.

Minimum manual corpus:

- wrapped long ASCII line with multiple style runs
- line with tabs at non-default width
- line with visible selection spanning a wrap break
- line with one underline indicator
- several lines with line numbers visible in the margin
- one file that produces a representative visible fragment count

Minimum extended corpus if available:

- one BiDi sample
- one control-character representation sample
- one visible-whitespace sample

## Feasibility-note template

The required findings note should answer:

1. Which exact functions formed the shared capture seam?
2. Which primitive families were implemented?
3. Did wrapped text capture work without parallel traversal?
4. What were the measured fragment counts and node counts?
5. Did text shaping drift materially from Scintilla geometry?
6. Which visual families remain clearly unresolved?
7. Is the project cleared to begin Phase 1 and Phase 2?

## Recommended next action after this document

The next coding step should be:

1. add `RenderCapture.h`
2. thread `Render_collector*` through the narrow seam
3. emit text-segment capture from `DrawForeground(...)`
4. bridge one captured frame through `ScintillaQuickCore`
5. render that frame through the existing scene-graph path

Do not start broader cleanup or broad renderer decomposition before that gate is
real.

If the gate passes, the next step is
`docs/native_text_scene_graph_phase1_work_package.md`.
