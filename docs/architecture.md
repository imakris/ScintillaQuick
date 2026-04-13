# Architecture

## Overview

`ScintillaQuick` is a Qt Quick-native integration of Scintilla.

The visible editor is a real `QQuickItem`, not a hidden `QWidget` captured into
an image. Scintilla remains the source of truth for editing behavior, document
state, styling, wrapping, folding, and message handling, while Qt Quick owns
the visible item, event delivery, focus, input-method integration, and scene
graph.

## High-Level Shape

```text
application
    |
    v
ScintillaQuickItem (public QQuickItem)
    |
    +-- Qt events, focus, IME, drag/drop
    +-- properties, signals, send()/sends()
    +-- updatePaintNode()
    |
    v
ScintillaQuickCore (ScintillaBase + QObject)
    |
    +-- WndProc bridge
    +-- scroll, clipboard, timers, notifications
    +-- capture_current_frame()
    +-- current_render_frame()
    |
    v
Scintilla platform layer + vendored Scintilla
    |
    v
Render_frame -> Scene_graph_renderer -> QSGNode tree
```

## Main Components

### `ScintillaQuickItem`

[`ScintillaQuickItem`](../include/scintillaquick/ScintillaQuickItem.h) is the
public item exposed to applications.

It is responsible for:

- presenting the editor as a `QQuickItem`
- handling keyboard, mouse, wheel, touch, drag-and-drop, and IME events
- exposing a Qt property and signal surface
- forwarding Scintilla messages through `send()` and `sends()`
- driving scene-graph updates through `updatePaintNode()`

### `ScintillaQuickCore`

[`ScintillaQuickCore`](../src/core/ScintillaQuickCore.h) owns the Scintilla
engine integration.

It is responsible for:

- maintaining editor state and behavior through `WndProc`
- implementing platform hooks such as scrolling, clipboard, timers, and
  notifications
- capturing the visible editor state from Scintilla
- translating that capture into the internal render frame consumed by the Qt
  Quick renderer

### Scene-Graph Renderer

[`Scene_graph_renderer`](../src/render/scintillaquick_scene_graph_renderer.h)
consumes the current render frame and updates Qt Quick scene-graph nodes.

It is responsible for:

- body text rendering
- selections, carets, and current-line visuals
- margin text and fold visuals
- indicators, whitespace marks, annotations, and other captured primitives
- node reuse and pixel-aligned overlay geometry

### Vendored Scintilla

The vendored Scintilla tree under
[`third_party/scintilla/`](../third_party/scintilla) remains the authority for
editor semantics.

`ScintillaQuick` does not reimplement the editor model. It integrates Scintilla
with Qt Quick and translates Scintilla's visual output into a Qt Quick-friendly
representation.

## Rendering Flow

The visible path is:

1. A change arrives through Qt input handling or an explicit `send(SCI_*, ...)`
   call.
2. `ScintillaQuickCore::WndProc` applies the change to Scintilla's model.
3. `ScintillaQuickItem` decides whether the change needs property resync,
   snapshot invalidation, or a scene-graph update.
4. On the next frame, `build_render_snapshot()` asks the core for a fresh
   `Render_frame`.
5. The scene-graph renderer updates the `QSGNode` tree in `updatePaintNode()`.

This avoids the old captured-widget pattern:

- no hidden `QWidget`
- no full editor image upload as the primary display path
- no widget bridge just to make the editor visible inside Qt Quick

## Scene-Graph Update Dispatch

`send()` uses a small dispatch table in
[`src/core/scintillaquick_dispatch_table.h`](../src/core/scintillaquick_dispatch_table.h)
to classify Scintilla messages for rendering purposes.

The dispatch has three categories:

- known mutating messages: request the specific scene-graph update needed
- known read-only messages used internally by the library: take a fast path and
  do not trigger a resync
- unknown messages: fall back to a conservative full resync

That conservative default favors visual correctness over missing an update when
new Scintilla messages are introduced.

## Input And IME

Qt Quick is the event boundary.

`ScintillaQuickItem` receives:

- keyboard events
- mouse and wheel events
- touch events
- drag-and-drop events
- input method queries and composition events

Those are translated into the Scintilla-facing behavior implemented by
`ScintillaQuickCore` and the Qt platform layer. Pre-edit state and caret
geometry are reported back through Qt's input-method interfaces so the editor
participates correctly in Qt Quick IME workflows.

## Scrolling Model

Scrolling is designed for Qt Quick applications, where scrollbars and viewport
composition are often managed outside the editor item.

The editor exposes scroll-related properties and signals, while the visible item
remains a Qt Quick control that can participate cleanly in a larger scene.

## Lifetime And Ownership

- `ScintillaQuickItem` owns `ScintillaQuickCore`
- `ScintillaQuickItem` also owns item-level render/profiling state
- `Scene_graph_renderer` updates transient `QSGNode` content owned by the item's
  paint tree
- vendored Scintilla remains an implementation dependency, not a separate
  process or hosted widget

## Public Package Boundary

The public integration surface is C++-first:

- instantiate `ScintillaQuickItem`
- attach it to a `QQuickWindow` or another `QQuickItem`
- drive the editor through Qt properties, signals, and Scintilla messages

The library also provides `RegisterScintillaType()` for projects that want to
register `ScintillaQuickItem` as a QML type.

Installed packages expose:

- `include/scintillaquick/`
- the compatibility include path `include/scintilla_quick/`
- Scintilla's public headers from `third_party/scintilla/include/`

Installed packages intentionally do not expose Scintilla internal implementation
headers from `third_party/scintilla/src/`.

## Scope And Limits

This repository is focused on a faithful Qt Quick integration of Scintilla, not
on building a new editor engine.

What this architecture is intended to provide:

- Scintilla behavior on a Qt Quick-native surface
- accurate text, selection, margin, and fold rendering
- proper focus and IME participation in Qt Quick applications
- packaging as a reusable standalone library

What it is not intended to be:

- a hidden-widget compatibility layer
- a generic captured-widget bridge
- a brand-new editor implementation independent of Scintilla

## Documentation Boundary

This document is intended to stay stable and public.

Transient implementation notes, work packages, and profiling checklists age
quickly and are not part of the public documentation set.
