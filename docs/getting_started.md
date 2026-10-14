# Getting Started

## Requirements

- CMake `3.25+`
- C++20 compiler
- Qt `6.12+`
- Qt modules: `Core`, `Gui`, `Qml`, `Quick`

## Build The Project

```bash
cmake -S . -B build
cmake --build build
```

Useful targets:

- `ScintillaQuick`

If `BUILD_TESTING` is enabled, test executables are built as well.
Examples and benchmarks are opt-in:

```bash
cmake -S . -B build -DSCINTILLAQUICK_BUILD_EXAMPLES=ON -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON
cmake --build build
```

## Run The Example

After building with `SCINTILLAQUICK_BUILD_EXAMPLES=ON`, run:

```bash
./build/scintillaquick_minimal_editor
```

On Windows with a multi-config generator, use the matching configuration path,
for example:

```bash
./build/Debug/scintillaquick_minimal_editor.exe
```

## Install And Consume

Install the package:

```bash
cmake --install build --prefix <install-prefix>
```

Set `CMAKE_PREFIX_PATH` to the installation prefix when configuring another
CMake project, then consume the package:

```cmake
find_package(ScintillaQuick CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ScintillaQuick::ScintillaQuick)
```

The installed package includes Scintilla's public headers. Scintilla internal
implementation headers are not part of the installed interface.

## Minimal Integration Pattern

Typical C++ integration looks like this:

1. Create a `QGuiApplication`.
2. Create a `QQuickWindow` or parent `QQuickItem`.
3. Instantiate `ScintillaQuick_item`.
4. Attach it to the Qt Quick scene.
5. Set geometry, font, and initial text.
   The first-party examples and tests use the shared helper in
   `examples/common/scintillaquick_font.h` to select a bundled fixed-font
   family. By default it picks `Cascadia Code`, and
   `SCINTILLAQUICK_FIXED_FONT_FAMILY` can switch it to `Cousine`.
6. Drive editor behavior with Scintilla messages through `send()`.

See [`examples/minimal_editor/main.cpp`](../examples/minimal_editor/main.cpp)
for a complete runnable example.

## QML Registration

Include `<scintillaquick/scintillaquick_item.h>` in the application's C++ startup
and call `register_scintilla_type()` before loading QML. This registers
`ScintillaQuick_item` in the `ScintillaQuick` module, version `1.0`:

```qml
import ScintillaQuick 1.0

ScintillaQuick_item {
    anchors.fill: parent
    focus: true
    text: "Hello from ScintillaQuick\n"
}
```

## Find and Replace

`ScintillaQuick_item` includes a bottom find panel. `Ctrl+F` opens its find row,
`Ctrl+H` opens both the find and replace rows, and `Escape` closes it. The panel
provides Find Previous, Find Next, Select All, Replace, Replace & Find, and
Replace All actions. Searches wrap at the beginning or end of the document.
Opening either panel seeds the search from one nonempty single-line selection
that is not rectangular. Otherwise, the previous search text is retained.

The panel can also be controlled directly from C++ or QML with
`showFind()`, `showFindReplace()`, and `hideFindPanel()`. Applications can bind to
the `findPanelVisible` and `findReplaceMode` properties. Search behavior is
configured with Scintilla's `SCFIND_*` flags through `findOptions` for
case-sensitive, whole-word, or regular-expression matching:

```cpp
editor.setFindText(QStringLiteral("needle"));
editor.setFindOptions(SCFIND_MATCHCASE | SCFIND_WHOLEWORD);
editor.showFind();
```

The caller controls the panel font and its semantic colors:

```cpp
editor.setFindPanelFont(QFont(QStringLiteral("Inter"), 10));
editor.setFindPanelBackgroundColor(QColor("#2d2d30"));
editor.setFindPanelForegroundColor(QColor("#e6e6e6"));
editor.setFindPanelFieldBackgroundColor(QColor("#252526"));
editor.setFindPanelFieldForegroundColor(QColor("#f0f0f0"));
editor.setFindPanelBorderColor(QColor("#3f3f46"));
editor.setFindPanelButtonHoverColor(QColor("#3e3e42"));
```

Selection and disabled-state colors are separately available through
`findPanelSelectionBackgroundColor`, `findPanelSelectionForegroundColor`, and
`findPanelDisabledForegroundColor`.

## Optional Lexilla Support

ScintillaQuick does not require Lexilla. Applications that use lexers can call
`scintillaquick_add_lexilla()` after adding the ScintillaQuick subproject or finding
its installed package, then link `Lexilla::Lexilla`. The helper reuses an existing
target. Otherwise, it builds a supplied source checkout; with no source path, it
tries an installed Lexilla package before fetching upstream `master`. Source
builds use ScintillaQuick's public Scintilla headers.

Set `FETCHCONTENT_SOURCE_DIR_LEXILLA` or pass `SOURCE_DIR <checkout>` to select a
local checkout. Invalid explicit paths fail configuration when the helper needs
to create the target.

The helper installs a separate `Lexilla` CMake package alongside ScintillaQuick
when it builds Lexilla; installed applications can use
`find_package(Lexilla CONFIG REQUIRED)` directly.

## Default Document Syntax

The optional `ScintillaQuick::Syntax` companion selects Lexilla lexers, keyword
sets and light/dark lexical colors for C/C++, QML/JavaScript, GLSL, JSON, Python,
CMake and Rust paths. Unknown paths use plain text. The core editor remains
independent of Lexilla.

Call `scintillaquick_add_syntax()` after adding the subproject or finding the
installed package, then link `ScintillaQuick::Syntax`. To build and install the
companion with ScintillaQuick itself, set `SCINTILLAQUICK_BUILD_SYNTAX=ON`.
An installed core-only package can build the companion through the same helper;
its source is included in the package. Repeated helper calls reuse the target.

Include `<scintillaquick/scintillaquick_syntax.h>` and call
`scintillaquick::configure_default_syntax(pane, path, dark_mode)` once with the
final language path and theme, then apply any application keyword, property or
color overrides. Configuration preserves the host's base font, backgrounds,
margins and predefined styles. Custom lexer owners can use
`scintillaquick::reset_syntax_styles(pane)` to clear lexical colors, emphasis and
keyword sets while retaining their lexer. Application-specific lexer policy
belongs to the host.

## Testing

Run the registered test suite with:

```bash
ctest --test-dir build --output-on-failure
```

Registered tests currently include smoke, frame-validation, and
visual-regression coverage. Benchmark-backed coverage is registered when
`SCINTILLAQUICK_BUILD_BENCHMARKS=ON`.

On Windows, visual tests require a desktop session because they create and show
a `QQuickWindow`.
