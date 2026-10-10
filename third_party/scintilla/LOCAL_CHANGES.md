# ScintillaQuick changes to Scintilla

This is a maintained fork. [VERSION](VERSION) records the imported upstream archive
and the Lexilla revision supplying lexer constants. Upstream refreshes must account for the
following changes alongside the ScintillaQuick platform implementation and
render-frame capture in `src/core`, `src/platform`, and `src/render`.

## Source patch ledger

Paths below are relative to this directory. The comparison baseline is
Scintilla 5.6.7; line-ending differences are excluded.

| Files | Local integration |
| --- | --- |
| `src/RenderCapture.h` | ScintillaQuick-owned capture records and `Render_collector` interface. Captures text, effective backgrounds, selections, carets, annotations, indicators, whitespace, indent guides, and margin primitives for Qt Quick. Layer and alpha values preserve the drawing order. Static-content filtering allows overlay capture to omit unchanged document content while refreshing effective backgrounds. |
| `src/EditView.h`, `src/EditView.cxx` | Optional collector arguments and calls throughout layout/painting, including direction metadata, visual-line geometry, background rectangles, current-line layers, and the primitives above. Indicator capture preserves its drawing, whole-line, and first-character rectangles for Scintilla's shape algorithms. Capture also works without raster indent-guide pixmaps. Review the layout and drawing changes together when updating this patch. |
| `src/MarginView.h`, `src/MarginView.cxx` | Collector arguments and capture of line numbers and marker symbols, including fold-part, stroke-width, and margin-style metadata. The paint loop skips negative display-line indices before visiting document lines. The core copies marker fonts into frame values for rendering. |
| `src/Platform.h` | `PainterID`, a Qt Quick surface-initialization overload, supporting includes, and an explicit `virtual` on the `ListBox` destructor. `PLAT_QT_QML` and `SCINTILLA_QT_QML` selection already exist in upstream 5.3.2. |
| `src/Editor.h`, `src/Editor.cxx` | Painter-aware measurement-surface creation and forwarding through `AutoSurface`, paired with `Surface_impl` in `src/platform/scintillaquick_platqt.cpp`. |
| `src/CellBuffer.cxx` | The Emscripten branch of `LineVector::InsertLines` always uses `InsertPartitionsWithCast`; other targets retain upstream's pointer-convertibility check. |
| `src/Indicator.cxx` | `DotBox` skips nonpositive image dimensions and explicitly visits the two edge coordinates. Upstream's edge loops incremented by width or height minus one, so a one-pixel dimension caused an infinite loop. The alternating edge alpha is preserved. |

`RenderCapture.h` is local project code, first recorded with the repository's
initial import by Ioannis Makris in 2026. It uses the project's
[BSD 2-Clause license](../../LICENSE). Upstream-derived files retain their
original notices and the [upstream license](LICENSE); their authorship must
not be replaced by the local project attribution.

## Retained upstream headers and packaging

`include/SciLexer.h` comes from Lexilla 5.5.4, at the revision recorded in
`VERSION`. It supplies lexer/style constants used by downstream consumers;
lexer implementations are supplied separately. Unreferenced `FontQuality.h`
and `IntegerRectangle.h` imports have been removed.

The installed Scintilla headers are the message, type, structure, position,
lexer, and loader interfaces, plus `SciLexer.h`. `ScintillaCall.h` describes a
wrapper whose implementation is not included in this library, and
`ScintillaWidget.h` describes the GTK widget; neither is part of the installed
ScintillaQuick interface. Scintilla implementation headers are not installed.

## Verification and refresh

The 2026-10-10 import replaces the retained source and interface files with
Scintilla 5.6.7 and adds its `RunThreads.h`, `UndoHistory.h`, and
`UndoHistory.cxx`. The nine modified upstream source files are those listed
in the source ledger, excluding the ScintillaQuick-owned `RenderCapture.h`.
`LICENSE` matches upstream `License.txt`. `SciLexer.h` matches the Lexilla
source recorded above. All retained files are accounted for by those sources,
the source patches, and local provenance documents.

The Qt Quick adapter follows the float line-layout storage and current rendering
signatures. It realizes font stretch and completion-image scaling through Qt,
and retains native document ownership through the public document interface.
New message IDs are classified in the dispatch table; document-changing line
indent, line dedent, and cut-allow-line messages also pass through the adopted
edit-handler boundary.

For a refresh, extract the recorded archive outside the source tree, compare
the retained paths, and inspect the complete diff of each modified file.
Compare `LICENSE` with upstream `License.txt`. Keep this ledger synchronized
with added or removed patches; it describes integration responsibilities,
while the actual source diff supplies the patch contents.
