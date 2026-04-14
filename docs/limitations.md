# Known Limitations

This document lists known limitations and platform-specific quirks of
`ScintillaQuick`. They are tracked here instead of in the issue tracker
so that consumers can find them without crawling history.

## Rendering

- **Complex-script / bidirectional text is only partially supported.**
  `QTextLayout` is configured with the document's paragraph direction
  and `ScintillaQuick` passes glyph runs through Qt's shaping pipeline,
  but there are no end-to-end visual-regression tests for Arabic or
  Hebrew editing. Caret placement, selection highlighting, and IME
  behaviour in RTL contexts may not exactly match upstream Scintilla.
  Do not depend on RTL or mixed-direction rendering without adding
  your own visual tests.

- **Indent guides and some marker styles.** `ScintillaQuick` renders
  markers and indent guides through its own scene-graph path rather
  than through Scintilla's rasterised reference output. The visual
  shape matches Scintilla closely but is not pixel-identical.

## Input method / IME

- **IME interaction is hard-wired to inline mode**
  (`imeInteraction = IMEInteraction::Inline` in
  `src/core/ScintillaQuickCore.cpp`). Windows- or macOS-style
  compositional IME modes are not exposed. If you need a different
  IME interaction mode, patch the core directly and add a visual
  regression covering the composition path.

## Tests and baselines

- **Visual regression baselines are Linux-only.** The PNGs under
  `tests/frame_visual_regression/baselines/` were generated on Linux
  with Qt's software scene graph and the shared test-font helper in
  `examples/common/ScintillaQuickFont.h`. Font-rendering differences
  between FreeType (Linux), ClearType (Windows), and Core Text
  (macOS) mean the visual tests will fail on Windows and macOS until
  per-platform baselines are added. The helper defaults to `Cousine`
  and can switch to `Cascadia Code` via `SCINTILLAQUICK_TEST_FONT_FAMILY`.
  The CI workflow skips `scintillaquick_visual_regression_test` on
  non-Linux hosts.

- **Windows visual tests require a desktop session.** The
  visual-regression runner uses the `windows` Qt platform plugin on
  Windows (the `offscreen` plugin does not cover all of the code paths
  the test exercises). Run the Windows tests in an interactive
  session or on a CI runner that provides one.

## Clipboard and drag-drop

- **`QDrag` uses `deleteLater()` after `exec()`** as a defensive
  measure. Earlier code held the `QDrag` alive until the owning
  `QQuickItem` died because deleting it immediately was reported to
  crash on Linux. Using `deleteLater()` lets Qt's platform drag
  helpers finish before the `QDrag` goes away, which is the shape
  Qt itself uses in its examples.

## Packaging

- **Static library is the default build.** Shared builds are
  available via `-DBUILD_SHARED_LIBS=ON` but are not exercised in CI
  and may need additional `SCINTILLAQUICK_EXPORT` annotations on
  internal symbols that are currently private to the static build.

- **Installed consumers must use the same Qt major.minor floor as
  the library build.** The floor is defined once at the top of
  `CMakeLists.txt` and is baked into the generated
  `ScintillaQuickConfig.cmake`. A mismatch between the configured
  floor in the config file and the Qt version in the consumer's
  environment surfaces as a cryptic `find_dependency(Qt6)` failure.

## Known internal invariants

- **`send()` on `ScintillaQuickItem` is declared `const`** because
  Qt `Q_PROPERTY` READ getters funnel through it. Mutating Scintilla
  messages routed through `send()` therefore cast `this` once, in a
  well-marked place at the top of the method. Do not copy that
  pattern blindly; the comment in the source explains the full
  contract.

- **The scene-graph dispatch table has a re-entry guard.** The
  fast-path allow-list in `src/core/scintillaquick_dispatch_table.h`
  is tested by `tests/dispatch_table/main.cpp`. If you add a new
  Scintilla query call from inside `syncQuickViewProperties()` or
  one of its helpers, you must also add the message to
  `scene_graph_message_is_known_read_only()` or the property-sync
  path will recurse.
