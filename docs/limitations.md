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
  `src/core/scintillaquick_core.cpp`). Windows- or macOS-style
  compositional IME modes are not exposed. If you need a different
  IME interaction mode, patch the core directly and add a visual
  regression covering the composition path.

## Tests and baselines

- **Visual regression baselines are a Windows native-QPA oracle.** The
  PNGs under `tests/frame_visual_regression/baselines/` were captured on
  Windows through the native `windows` Qt platform plugin, which
  rasterises glyphs with DirectWrite/GDI and LCD subpixel antialiasing.
  The capture used the bundled `Cascadia Code` family at 11pt (the
  default of the shared test-font helper in
  `examples/common/scintillaquick_font.h`, switchable to `Cousine` via
  `SCINTILLAQUICK_TEST_FONT_FAMILY`), `QT_FONT_DPI=96`,
  `QT_SCALE_FACTOR=1`, `QT_ENABLE_HIGHDPI_SCALING=0`, and Qt's software
  scene graph. A different rasteriser lays down different ink: under the
  `offscreen` plugin, which `tests/CMakeLists.txt` selects on every
  non-Windows host, glyphs go through FreeType with grayscale
  antialiasing and 32 of the 33 fixtures fail. That depends on the
  platform plugin rather than on the host, so `offscreen` fails on
  Windows too. Only the `windows` plugin reproduces the baselines, so
  `scintillaquick_visual_regression_test` is gated in the Windows CI
  workflow and excluded from the Linux and macOS ones, which run under
  `offscreen`. Gating another platform needs per-platform baselines that
  a human has inspected; regenerating them from a run only records
  whatever that renderer happened to produce.

- **The benchmark target is opt-in.**
  Build it with `-DSCINTILLAQUICK_BUILD_BENCHMARKS=ON`. Treat benchmark
  results as a local or dedicated-runner concern rather than a normal CI signal.

- **Windows visual tests require a desktop session.** The
  visual-regression runner uses the `windows` Qt platform plugin
  because that is the only rasteriser the baselines reproduce under,
  as described above. Run the Windows tests in an interactive
  session or on a CI runner that provides one. The hosted
  `windows-latest` image does: it renders with ClearType subpixel
  antialiasing (`FontSmoothing` 2, `FontSmoothingType` 2), and all 33
  fixtures reproduce there. That is a property of the runner image
  rather than a guarantee, which is why `ci-windows.yml` logs those
  settings on every run and uploads the `_actual`/`_expected`/`_diff`
  triples when a fixture fails.

## Clipboard and drag-drop

- **`QDrag` uses `deleteLater()` after `exec()`** as a defensive
  measure. Earlier code held the `QDrag` alive until the owning
  `QQuickItem` died because deleting it immediately was reported to
  crash on Linux. Using `deleteLater()` lets Qt's platform drag
  helpers finish before the `QDrag` goes away, which is the shape
  Qt itself uses in its examples.

## Packaging

- **Static library is the default build.** Shared builds are
  available via `-DBUILD_SHARED_LIBS=ON` and are exercised in CI, but
  Windows shared builds in particular are sensitive to DLL export and
  runtime-path issues because some in-tree tests reach internal
  symbols and the test executables must be able to locate
  `ScintillaQuick.dll` at runtime.

- **Installed consumers must use the same Qt major.minor floor as
  the library build.** The floor is defined once at the top of
  `CMakeLists.txt` and is baked into the generated
  `ScintillaQuickConfig.cmake`. A mismatch between the configured
  floor in the config file and the Qt version in the consumer's
  environment surfaces as a cryptic `find_dependency(Qt6)` failure.

## Known internal invariants

- **`send()` on `ScintillaQuick_item` is declared `const`** because
  Qt `Q_PROPERTY` READ getters funnel through it. Mutating Scintilla
  messages routed through `send()` therefore use a single private helper that
  casts `this` when it needs to schedule updates. Do not copy that pattern
  blindly.

- **The scene-graph dispatch table has a re-entry guard.** The
  fast-path allow-list in `src/core/scintillaquick_dispatch_table.h`
  is tested by `tests/dispatch_table/main.cpp`. If you add a new
  Scintilla query call from inside `syncQuickViewProperties()` or
  one of its helpers, you must also add the message to
  `scene_graph_message_is_known_read_only()` or the property-sync
  path will recurse.
