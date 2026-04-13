# Getting Started

## Requirements

- CMake `3.24+`
- C++20 compiler
- Qt `6.5+`
- Qt modules: `Core`, `Gui`, `Qml`, `Quick`

## Build The Project

```bash
cmake -S . -B build
cmake --build build
```

Useful targets:

- `ScintillaQuick`
- `scintillaquick_minimal_editor`
- `scintillaquick_embedded_benchmark`

If `BUILD_TESTING` is enabled, test executables are built as well.

## Run The Example

After building, run:

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

Then consume it from another CMake project:

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
3. Instantiate `ScintillaQuickItem`.
4. Attach it to the Qt Quick scene.
5. Set geometry, font, and initial text.
6. Drive editor behavior with Scintilla messages through `send()`.

See [`examples/minimal_editor/main.cpp`](../examples/minimal_editor/main.cpp)
for a complete runnable example.

## Testing

Run the registered test suite with:

```bash
ctest --test-dir build --output-on-failure
```

Registered tests currently include smoke, frame-validation, visual-regression,
and benchmark-backed coverage.

On Windows, visual tests require a desktop session because they create and show
a `QQuickWindow`.
