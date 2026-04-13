# ScintillaQuick

`ScintillaQuick` is intended to be a permissively licensed Qt Quick integration of
the Scintilla editing engine.

The aim is to provide a real Qt Quick-native editor path instead of embedding a
hidden `QWidget` and showing captured frames of it. That means:

- Scintilla core stays intact.
- Qt Quick owns the visible item, input routing, focus, IME, and scrolling.
- The integration should be usable from C++ first, with optional QML registration
  layered on top.

## Status

The repository now contains a first working implementation:

- a standalone `ScintillaQuick` static library
- a minimal Qt Quick example application
- an embedded benchmark executable
- a vendored Scintilla source snapshot
- a Qt 6-native build surface without `Qt6::Core5Compat`

The main design note lives at
[`docs/scintillaquick_design.md`](docs/scintillaquick_design.md), and the code is
currently using a pragmatic first working path based on Scintilla's Qt Quick
platform layer while the architecture is tightened further.

The current text-encoding policy is deliberately UTF-8-first.

## Intended Scope

- Qt Quick-native Scintilla surface
- Full text input and cursor handling
- IME support
- Scroll integration suitable for Qt Quick applications
- Clean standalone library packaging

## Out of Scope

- Hidden-widget snapshot bridges
- Process-hosted editor transport
- QML-only design constraints

## Repository Layout

- [`include/`](include): public headers
- [`src/`](src): library implementation
- [`docs/`](docs): design notes and architecture documents
- [`examples/`](examples): runnable example applications
- [`benchmarks/`](benchmarks): embedded performance tooling
- [`third_party/`](third_party): vendored upstream code and licenses

## License

`ScintillaQuick` uses the BSD 2-Clause license. See [`LICENSE`](LICENSE).
