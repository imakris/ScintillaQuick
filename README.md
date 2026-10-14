# ScintillaQuick

[![CI Linux](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-linux.yml)
[![CI macOS](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-macos.yml)
[![CI Windows](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/imakris/ScintillaQuick/actions/workflows/ci-windows.yml)

ScintillaQuick brings the [Scintilla](https://www.scintilla.org/) editing engine
to Qt Quick as a `QQuickItem`. Embed it from C++ or QML and use Scintilla's message
API through `send()` to configure editor behavior.

It supports Qt Quick focus and input, IME, and scene-graph rendering, with a
built-in find/replace panel. Optional Lexilla support and a default syntax
companion provide syntax highlighting; see the
[integration guide](docs/getting_started.md#optional-lexilla-support).

## Requirements

- CMake `3.25+`, a C++20 compiler, and Qt `6.12+` (`Core`, `Gui`, `Qml`, `Quick`)
- Windows, Linux, or macOS

## Build and Integrate

```bash
cmake -S . -B build
cmake --build build
cmake --install build --prefix <install-prefix>
```

Set `CMAKE_PREFIX_PATH` to the installation prefix in your application, then
link the exported target:

```cmake
find_package(ScintillaQuick CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ScintillaQuick::ScintillaQuick)
```

## First Editor

For QML, include `<scintillaquick/scintillaquick_item.h>` in your C++ startup and
call `register_scintilla_type()` before loading QML. Then create the editor:

```qml
import QtQuick
import QtQuick.Window
import ScintillaQuick 1.0

Window {
    width: 800
    height: 500
    visible: true

    ScintillaQuick_item {
        anchors.fill: parent
        focus: true
        text: "Hello from ScintillaQuick\n"
    }
}
```

For C++, see the runnable
[minimal editor](examples/minimal_editor/main.cpp) and its
[build instructions](docs/getting_started.md#run-the-example).

## Documentation

- [Getting Started](docs/getting_started.md): build options, C++/QML integration,
  find/replace customization, and syntax highlighting
- [Public API](include/scintillaquick/scintillaquick_item.h) and
  [Known Limitations](docs/limitations.md)
- [Architecture](docs/architecture.md) and
  [Maintenance Invariants](docs/maintenance_invariants.md): source map, vendored
  changes, and validation guidance

## License

Project code is released under the BSD 2-Clause license. See
[`LICENSE`](LICENSE).

The repository also vendors Scintilla under its own license. See
[`third_party/scintilla/LICENSE`](third_party/scintilla/LICENSE).
