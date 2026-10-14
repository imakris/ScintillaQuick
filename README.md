# ScintillaQuick

[![CI Linux](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-linux.yml)
[![CI macOS](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-macos.yml)
[![CI Windows](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-windows.yml)

`ScintillaQuick` is a Qt Quick-native integration of the
[Scintilla](https://www.scintilla.org/) editing engine.

It is intended for applications that want Scintilla behavior inside a real
`QQuickItem`. Applications can integrate through C++ or the optional QML type
registration helper.

## What It Provides

- A `ScintillaQuick_item` that derives from `QQuickItem`
- Scintilla's editing model and message API on top of a Qt Quick surface
- Qt Quick-native focus, input, IME, and scene-graph rendering
- CMake package export for `find_package(ScintillaQuick)`
- A minimal example application, benchmark target, and validation tests

## Requirements

- CMake `3.25+`
- A C++20 compiler
- Qt `6.12+` with:
  - `Core`
  - `Gui`
  - `Qml`
  - `Quick`

Target platforms:

- Windows
- Linux
- macOS

## Build

```bash
cmake -S . -B build
cmake --build build
```

This builds:

- `ScintillaQuick` static library
- test executables when `BUILD_TESTING=ON`

Examples and benchmarks are opt-in:

```bash
cmake -S . -B build -DSCINTILLAQUICK_BUILD_EXAMPLES=ON -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON
```

To install the package:

```bash
cmake --install build --prefix <install-prefix>
```

Installed consumers can then use `find_package(ScintillaQuick CONFIG REQUIRED)`.

## Use From CMake

```cmake
find_package(ScintillaQuick CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE ScintillaQuick::ScintillaQuick)
```

Public headers:

- [`include/scintillaquick/scintillaquick_item.h`](include/scintillaquick/scintillaquick_item.h)

Installed packages expose Scintilla's public headers alongside the library.
Scintilla internal implementation headers are intentionally not installed.

## Minimal C++ Usage

```cpp
#include <QGuiApplication>
#include <QQuickWindow>
#include <scintillaquick/scintillaquick_item.h>

#include "Scintilla.h"

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);

    QQuickWindow window;
    window.resize(1100, 720);
    window.setColor(Qt::white);

    ScintillaQuick_item editor;
    editor.setParentItem(window.contentItem());
    editor.setWidth(window.width());
    editor.setHeight(window.height());
    editor.setProperty("text", "hello from ScintillaQuick\n");
    editor.send(SCI_STYLECLEARALL);

    window.show();
    editor.forceActiveFocus();
    return app.exec();
}
```

For a runnable example, see
[`examples/minimal_editor/main.cpp`](examples/minimal_editor/main.cpp).

## Find and Replace

`ScintillaQuick_item` includes a bottom find panel. `Ctrl+F` opens its find row,
`Ctrl+H` opens both the find and replace rows, and `Escape` closes it. The panel
provides Find Previous, Find Next, Select All, Replace, Replace & Find, and
Replace All actions. Searches wrap at the beginning or end of the document.
Opening either panel seeds the search from a nonempty single-line selection.
With an empty or multiline selection, the previous search text is retained.

The panel can also be controlled directly from C++ or QML with
`showFind()`, `showFindReplace()`, and `hideFindPanel()`. Search behavior is
configured with Scintilla's `SCFIND_*` flags through `findOptions`:

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

## QML Registration

The library exposes `register_scintilla_type()` in
[`scintillaquick_item.h`](include/scintillaquick/scintillaquick_item.h), which
registers:

- module: `ScintillaQuick`
- version: `1.0`
- type: `ScintillaQuick_item`

## Optional Lexilla Support

ScintillaQuick does not require Lexilla. Applications that use lexers can call
`scintillaquick_add_lexilla()` after adding the ScintillaQuick subproject or finding
its installed package, then link `Lexilla::Lexilla`. This uses an existing target
or installed Lexilla package first, otherwise fetches upstream `master` and builds
against ScintillaQuick's public Scintilla headers. Set
`FETCHCONTENT_SOURCE_DIR_LEXILLA` or pass `SOURCE_DIR <checkout>` to select a local
checkout. Invalid explicit paths fail configuration.

The helper installs a separate `Lexilla` CMake package alongside ScintillaQuick;
installed applications can use `find_package(Lexilla CONFIG REQUIRED)` directly.

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

Run the test suite with:

```bash
ctest --test-dir build --output-on-failure
```

On Windows, visual tests require a desktop session.
See [Getting Started](docs/getting_started.md#testing) for test setup and
[Maintenance Invariants](docs/maintenance_invariants.md#test-and-benchmark-playbook)
for maintainer validation guidance.

## Repository Layout

- [`include/`](include): public headers
- [`src/`](src): library implementation
- [`examples/`](examples): sample applications
- [`benchmarks/`](benchmarks): benchmark application
- [`tests/`](tests): validation and regression tests
- [`docs/`](docs): public project documentation
- [`third_party/`](third_party): vendored dependencies

[`third_party/scintilla/`](third_party/scintilla) is a maintained Scintilla fork
with capture and Qt Quick surface hooks. Its [provenance record](third_party/scintilla/VERSION)
and [patch ledger](third_party/scintilla/LOCAL_CHANGES.md) describe the upstream
sources and local integration changes to preserve when updating the dependency.

## Documentation

- [Getting Started](docs/getting_started.md)
- [Architecture](docs/architecture.md)
- [Known Limitations](docs/limitations.md)
- [Maintenance Invariants](docs/maintenance_invariants.md)

## License

Project code is released under the BSD 2-Clause license. See
[`LICENSE`](LICENSE).

The repository also vendors Scintilla under its own license. See
[`third_party/scintilla/LICENSE`](third_party/scintilla/LICENSE).
