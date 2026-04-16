# ScintillaQuick

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

Current repository state:

- Version `0.1.0`
- Qt `6.5+`
- Static library build
- C++ integration first
- Optional QML type registration helper is available

## Requirements

- CMake `3.24+`
- A C++20 compiler
- Qt `6.5+` with:
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
- `scintillaquick_minimal_editor`
- `scintillaquick_embedded_benchmark`
- test executables when `BUILD_TESTING=ON`

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
- `scintillaquick_embedded_benchmark`
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

## Documentation

- [Getting Started](docs/getting_started.md)
- [Architecture](docs/architecture.md)
- [Known Limitations](docs/limitations.md)

## License

Project code is released under the BSD 2-Clause license. See
[`LICENSE`](LICENSE).

The repository also vendors Scintilla under its own license. See
[`third_party/scintilla/LICENSE`](third_party/scintilla/LICENSE).
