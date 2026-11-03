# Source-preview benchmark

This opt-in executable measures the actual `ScintillaQuick_source_preview`
capture, worker raster, GUI publication and software scene-graph path. It uses
the same document messages and source-preview class as an application host.

## Build and run

Use a fresh Release build with Qt 6.12 or newer:

```sh
cmake -S . -B build-preview -DCMAKE_BUILD_TYPE=Release \
    -DSCINTILLAQUICK_BUILD_BENCHMARKS=ON
cmake --build build-preview --target scintillaquick_source_preview_benchmark --parallel 4

# Run from the repository so its bundled test fonts can be located.
QT_QPA_PLATFORM=offscreen build-preview/scintillaquick_source_preview_benchmark \
    --lines 100000 --iterations 3 --corpus unique --scenario all --output preview-100k.json
```

Qt's runtime libraries and plugins must be available as for the existing
embedded-editor benchmark. The executable defaults to the offscreen platform,
96 font DPI and scale factor 1 when those environment variables are unset. It
selects and verifies the Qt Quick software backend, and it never shows a native
window. `QT_SCALE_FACTOR=2` exercises a different device pixel ratio. The JSON
records the effective ratio, font, platform, Qt version, document hash, byte and
line counts, and preview size.

The benchmark does not run as part of default CTest timing gates. It checks
behavior and reports timings without pass/fail speed thresholds.

## Scenarios and inputs

`--scenario` accepts a comma-separated list, or `all`:

| Scenario | Work inside the measured boundary |
| --- | --- |
| `initial` | Attach a previously loaded and styled native document to a new preview; wait for its first publication and render one preview frame. |
| `refresh` | Invalidate the completed preview's style and wait for a full refresh. |
| `edit` | Insert a short prefix through the native editor API, preserving line count, and wait for the new preview. |
| `scroll` | Move the native editor's first visible line and render the preview handle for each step. Assert that the handle moves and the source image is reused, including a subsequent debounce interval. |
| `edit-burst` | Request a refresh, then make six native same-line edits 10 ms apart. Wait for the final raster and compare it pixel-for-pixel with a fresh refresh of the same final document. |

Every independent preview instance first publishes an `initial` image, which is
reported even when only later scenarios were selected. `--iterations` defaults
to 3. Font and process caches are shared across those iterations; the first
iteration can have a cold-font cost. Use fresh process runs to measure that
separately. `--timeout-ms` is a completion safeguard, not a speed assertion.

Generated inputs have exactly `--lines` logical lines (default 10,000), with a
deterministic small token classifier assigning mixed colors and bold/italic
styles before measurement:

| `--corpus` | Purpose |
| --- | --- |
| `unique` (default) | C++-like source with varying identifiers and values inside the visible prefix; avoids measuring only repeated-line cache hits. |
| `repeated` | Identical styled source lines, to expose reusable-work opportunities explicitly. |
| `unicode` | Unique identifiers plus Greek, Cyrillic, Arabic, CJK, combining marks and emoji. Font fallback depends on the reported runtime. |
| `sparse` | One short code line every 997 rows, with blank rows in between. |
| `long` | A unique source prefix followed by 4 KiB of text on every line, exercising the bounded capture horizon. |

`--input-file PATH` reads an actual UTF-8 source file exactly; it ignores
`--lines`. The same deterministic style classifier is applied, so the benchmark
does not include a lexer workload. Inputs must be nonempty and contain no NUL
bytes. Generated and file inputs have a 512 MiB setup bound. Keep long-line
cases small enough to avoid turning this into a document-memory benchmark.

Useful runs include:

```sh
build-preview/scintillaquick_source_preview_benchmark --lines 10000 --scenario all
build-preview/scintillaquick_source_preview_benchmark --lines 1000000 --scenario initial,edit
build-preview/scintillaquick_source_preview_benchmark --lines 100000 --corpus repeated
build-preview/scintillaquick_source_preview_benchmark --lines 10000 --corpus unicode
build-preview/scintillaquick_source_preview_benchmark --lines 1000 --corpus long
build-preview/scintillaquick_source_preview_benchmark \
    --input-file third_party/scintilla/src/Editor.cxx --scenario all
```

## Interpretation

`publication_ms` starts immediately before the action and ends when a 1 ms GUI
timer observes a different nonzero publication serial. It includes the normal
20 ms debounce, native capture, worker rasterization, and queued handoffs.
`trigger_ms` identifies the synchronous action portion. `software_frame_ms`
then measures one actual software preview frame. `rendered_ms` covers the
complete action-to-frame boundary. The source preview alone is drawn; document
generation, loading/styling, host editor painting, OS input and physical display
presentation are outside these timings. `setup_ms` reports native document
loading/styling separately.

`process_cpu_ms_to_publication` uses the C runtime process CPU clock. On Linux
it includes the main and raster worker threads; platform clock semantics should
be checked before interpreting it elsewhere. `gui_1ms_timer_gaps` observes event
loop availability. Its gaps include timer resolution, scheduler effects and
benchmark overhead, so they are not isolated native-capture durations.

For `edit-burst`, `last_edit_to_rendered_ms` isolates the settle latency after
typing stops. Very small documents can publish the first refresh before editing
begins; the report records that fact rather than claiming an in-flight
cancellation in every run. The independent pixel oracle is outside the reported
settle boundary; its publication duration is recorded separately.

Compare several baseline and candidate runs on the same machine, with the same
font/backend/geometry and no concurrent builds or benchmarks. Preserve raw JSON,
compare image hashes for identical inputs and actions, and summarize a
distribution. A software offscreen improvement does not establish native
Windows/Direct3D, macOS/Metal or application-wide responsiveness.
