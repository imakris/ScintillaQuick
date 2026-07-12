# ScintillaQuick

[![CI Linux](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-linux.yml)
[![CI macOS](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-macos.yml)
[![CI Windows](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-windows.yml)

`ScintillaQuick` is a Qt Quick-native integration of the
[Scintilla](https://www.scintilla.org/) editing engine.

It is intended for applications that want Scintilla behavior inside a real
`QQuickItem`.

## What It Provides

- A `ScintillaQuick_item` that derives from `QQuickItem`
- Scintilla's editing model and message API on top of a Qt Quick surface
- Qt Quick-native focus, input, IME, and scene-graph rendering
- CMake package export for `find_package(ScintillaQuick)`
- A minimal example application, benchmark target, and validation tests

## Status

`ScintillaQuick` is usable today, but it is still an early-stage library.

## CI Status

| Platform | Static | Shared | CI test run | Consumer install smoke | Notes |
| :-- | :--: | :--: | :--: | :--: | :-- |
| Linux | 🟢 | 🟢 | 🟢 | 🟢 | GitHub Actions runs dispatch-table, smoke, and frame-validation tests. |
| macOS | 🟢 | 🟢 | 🟢 | 🟢 | GitHub Actions runs dispatch-table, smoke, and frame-validation tests. |
| Windows | 🟢 | 🟢 | 🟢 | 🟢 | Shared-build test jobs add `build/Release` to `PATH` so `ScintillaQuick.dll` is found at runtime. |

Current `master` status: all platform GitHub Actions jobs pass on Qt `6.7.2`.

Current repository state:

- Version `0.1.0`
- Qt `6.7+`
- Static library build
- C++ integration first
- Optional QML type registration helper is available

## Requirements

- CMake `3.24+`
- A C++20 compiler
- Qt `6.7+` with:
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

## Testing

CTest currently registers:

- `scintillaquick_smoke_test`
- `scintillaquick_dispatch_table_test`
- `scintillaquick_embedded_benchmark` when `SCINTILLAQUICK_BUILD_BENCHMARKS=ON`
- `scintillaquick_frame_validation_test`
- `scintillaquick_visual_regression_test`

Run them with:

```bash
ctest --test-dir build --output-on-failure
```

Notes:

- Visual-regression coverage uses Qt's software scene graph for deterministic
  output.
- On Windows, the visual tests use the normal `windows` Qt platform plugin, so
  they require a desktop session rather than a truly headless environment.

## Repository Layout

- [`include/`](include): public headers
- [`src/`](src): library implementation
- [`examples/`](examples): sample applications
- [`benchmarks/`](benchmarks): benchmark application
- [`tests/`](tests): validation and regression tests
- [`docs/`](docs): public project documentation
- [`third_party/`](third_party): vendored dependencies

ScintillaQuick-specific work should normally stay outside
[`third_party/scintilla/`](third_party/scintilla). That tree is vendored
upstream Scintilla code and should be changed only when intentionally updating
or patching the dependency.

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
