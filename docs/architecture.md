# Architecture

## Goal

`ScintillaQuick` should provide a Qt Quick-native integration of Scintilla rather
than hosting a `QWidget` editor behind a capture bridge.

## Direction

The intended architecture is:

- keep Scintilla core behavior
- implement a Qt Quick-facing platform layer
- handle input, focus, IME, and cursor geometry at the visible item boundary
- avoid hidden-widget snapshotting

## Initial Focus

The first design and implementation work should focus on:

- editor item ownership model
- rendering path
- IME and cursor rectangle behavior
- scrolling contract with Qt Quick applications
- public API shape for C++ and optional QML registration

