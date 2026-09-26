# Dependency audit handover — 2026-09-26

## Stop state and scope

The user asked to validate and address **all valid findings exclusively about ScintillaQuick**, including maintenance and performance findings, in `C:\plms\varinomics\_agent_reports\dependency_audit_2026-09-26.md`. Later instructions authorized prompt commits and pushes of completed findings. The user then explicitly requested wrap-up and this handover. **The task is not complete; Windows and macOS CI are not green.** No further builds or tests were started after wrap-up.

The inventory contains **20 local finding IDs, representing 19 distinct findings** because XC-07 duplicates SQ-10. SQ-05, SQ-08 and XB-02 require other repositories and are excluded. Other report entries mentioning ScintillaQuick but owned by consumers are excluded. Detailed validation, narrowed claims, baseline evidence and ownership are preserved in `finding-inventory.md` under the scratch directory below.

| Location | Purpose |
|---|---|
| `C:\plms\bsd_licensed\ScintillaQuick` | Canonical `master`; published code is through `7999671`, followed by this handover-only commit. |
| `C:\plms\_worktrees\ScintillaQuick-audit-20260926` | Continuation worktree, branch `audit/scintillaquick-20260926`; unfinished changes are preserved in a **local-only WIP commit**, not published. See the WIP reference below. |
| `C:\plms\_scratch\ScintillaQuick-audit-20260926` | External reports, patches, immutable source copies, build trees, logs and performance probes. Paths below are relative to this directory unless stated otherwise. |

**WIP reference:** `6f4001d9e1efe8b807a5b250e534ec76bae9d818`; 24 durable files preserved, continuation worktree clean, commit **not pushed**. Do not push the task branch wholesale or treat its changes as publication-ready. The task branch deliberately differs from canonical `master`, including the handover commit. Scratch patches describe isolated changes already present in this WIP; inspect them or reconstruct specific gates, but **do not blindly apply them again to the WIP**. Other pre-existing worktrees belong to other sessions and must not be deleted or repurposed; inspect `git worktree list`. In particular, `C:\plms\varinomics\_worktrees\ScintillaQuick-renderer-fixes-20260926` is not this task's continuation worktree.

## Published findings: 13 IDs

These commits were pushed individually. Their existing local gate results do not override the subsequent CI failures below.

| Finding IDs | Commit | Change / validation qualification |
|---|---|---|
| SQ-10, XC-07, G5-07 | `11749fc` | Consolidated equivalent node synchronization/conversion and removed unreachable geometry. Annotation structures with different fields and margin shaping with different semantics were retained. |
| SQ-11 | `94f8bb5` | Removed unreachable non-UTF8 platform paths. |
| G5-06 | `4a7cf86` | Reset IME composition position and kept IME query coordinates relative to the item. |
| SQ-09 | `a11ce3a` | Recorded verified fork provenance/local patches and corrected supported installed headers. No guessed upstream revision. |
| SQ-07 | `f39c254` | Fixed font advance cache invalidation when effective DPI changes. Broad shaping-speed claims were narrowed after profiling; a supplemental kerning defect remains below. |
| G5-05 | `4590139` | Honored edit handlers for replacements and drag moves; restored actual target semantics. |
| SQ-04 | `a02428d` | Preserved horizontal scroll during tracked-width recalculation. |
| SQ-01 | `6256c0f` | Delivered editor UI and captured-frame notifications. `painted()` denotes a captured frame, not GPU presentation. |
| SQ-12 | `9946c5d` | Captured call-tip images on the GUI thread. Autocomplete did not call Scintilla; the alleged QPixmap crash was not established. |
| G5-09 | `2cd4622` | Bounded automatic selection seeding and reused find-field layouts. Corrected isolated performance comparisons preceded publication. |
| SQ-02 | `7999671` | One caret blink timer and phase-independent captured geometry. Mouse dragging reproduced the real divergence; the report's SCI_SETSEL example was incorrect. |

## First priority on resumption: repair published CI

Latest tested code is `7999671` in `imakris/ScintillaQuick`:

| CI run | Result and exact known cause |
|---|---|
| [Linux 36244136102](https://github.com/imakris/ScintillaQuick/actions/runs/36244136102) | Passed. |
| [macOS 36244136109](https://github.com/imakris/ScintillaQuick/actions/runs/36244136109) | Static and shared compilation fail: protected `Editor::TickReason` is named in `tests/support/scintillaquick_validation_access.h:31`. |
| [Windows 36244136081](https://github.com/imakris/ScintillaQuick/actions/runs/36244136081) | Shared tests reference unexported `Font::Allocate`, `Surface_impl` constructor/destructor/Init/MeasureWidths and `Editor::SetSelection`. Static smoke fails the DPI expectation at `tests/smoke/main.cpp:1127`. |

Evidence: `ci-macos-7999671.log`, `ci-windows-7999671.log`, `shared-diagnostic-build.log`. A fresh local shared build confirms the five platform-symbol link failures. The exact numeric cause of the DPI expectation failure on CI Qt 6.10.1 is **unknown**; local Qt 6.11.1 passed and the failure log prints no measured values.

No CI remediation code was written before stopping. Proposed next fixes: qualify the protected enum through the befriended `ScintillaQuick_core`; use public SCI_SETSEL for the drag helper; place private platform/conformance tests behind a private static test-library boundary while public API tests still exercise the shared DLL. Factor production source/configuration once and reuse upstream objects; do not export internals just for tests. For DPI, compare reused versus fresh font position vectors on the same image through 96→144→96 DPI, after confirming the failing values. Preserve the actual cache regression and avoid blessing an unexplained metric difference.

## Unpublished work and required gates

### Renderer and SQ-06: correctness before performance

Pending IDs: **G5-01, G5-02, G5-03, G5-04, G5-08 and SQ-06**. The worktree contains coupled changes in `src/render`, `src/public/scintillaquick_item.cpp`, the core collector, platform font helpers, vendored EditView/MarginView/RenderCapture/Indicator, documentation and tests.

The correctness design captures immutable values on the GUI thread, applies one shared body clip, and uses const upstream `Indicator::Draw` / `LineMarker::Draw` into cached image nodes on software and RHI. No live editor/ViewStyle/font pointer crosses to rendering. It also captures style/EOL backgrounds, body markers and selection layers. Marker font/stroke/margin metadata and realized editor fonts are retained. SQ-06 includes custom folding-margin checkerboards. Architecture/source review accepted this with the explicit pure drawing-helper exception; final runtime acceptance is pending.

Reusable patches: `renderer-core-collector.patch`, `platform-font-helpers-final.patch`, `sq-06-editor.patch`. Latest corrective deltas: `renderer-image-attachment.patch`, `renderer-caret-device-coverage.patch`, `renderer-original-shape-coordinates.patch`, `renderer-indicator-item-clip.patch`. Prefer these isolated deltas for the correctness checkpoint; copying the entire live renderer also brings unfinished G5-08 performance changes.

G5-08 in the WIP merges solid underline rectangles, caches guide images, removes redundant body clips, and skips static updates using revision/text-clip/DPR while refreshing overlays. **It has not been built or measured.** The completed original baseline `renderer-perf-baseline-software500-1.log` used 500 lines, 1100×760, eight captures per scenario, software at actual DPR 1.25: median scroll 1870 ms, blink 2381 ms, theme burst 2180 ms, approximately 109,000 nodes. These 24 captures establish an original baseline, not improvement. Larger timed-out trials are retained separately. `renderer-perf-probe/` now also contains an unmeasured 16 KiB horizontally scrolled indicator scenario; its latest source is newer than its executable. Reconfigure with `configure-renderer-perf.cmd <build> <matched-source> <matched-lib> OFF|ON` (OFF original timer, ON SQ-02 timer), using queue slots 1. Fix the probe's protected `Editor::TickReason` qualifier too. Preserve a correctness-only baseline before comparing G5-08.

New `tests/renderer_conformance/main.cpp` and `tests/CMakeLists.txt` contain **74 source-oracle cases** across software/RHI and DPR 1/1.25. They cover 21 indicator styles, translated/stroked shapes, 28 non-image marker families, character/number-margin metadata, clipping, thin DotBox rectangles, real-editor backgrounds, marker/selection layer order and custom fold-margin colors. The DotBox width-one upstream loop hang was separately reproduced and fixed with bounded endpoint iteration. There are no new subjective PNG goldens.

Checkpoint 8 compiled; smoke/edit-boundary/frame-validation passed. Software crashed; RHI 1 passed 70/74 and RHI 1.25 passed 64/74. The attachment fix then made software 1 complete 70/74 at checkpoint 9. **No final matrix pass exists.** Each remaining mismatch was investigated:

- Software attachment crash: attach the image node only after setting its texture; debugger evidence and isolated fix are saved.
- Fractional caret edge: raw QSG triangle coverage differs from upstream FillRectangleAligned/QPainter at a half-pixel boundary. Explicit device-edge rounding is an unbuilt correction.
- Eight folding styles: normalizing primitive coordinates changes a raster tie. Original coordinates plus painter translation match the full-frame oracle; the correction is unbuilt.
- FullBox/StraightBox/Box: clipping the raster allocation at the text-area edge changes one alpha corner. Allocate within the **item viewport**, retain the shared text-area clip, and skip invisible shapes. A focused crop probe supports the isolated correction; final gate pending.
- Character marker: the original oracle's non-premultiplied image selected grayscale antialiasing. Native premultiplied raster drawing matches the candidate exactly at both DPRs. The test oracle format was corrected with this evidence; pixel tolerance was not relaxed. Latest test source also calls the actual upstream caret fill helper.

Logs: `checkpoint-8-correctness.log`, `checkpoint-9-software-1.log`, `renderer-software-crash-gdb.log`, `oracle-probe-windows.log`, `oracle-probe-windows-1.25.log`, `oracle-probe-item-crop.log`. Baseline failures: `conformance-baseline-*.log`, `conformance-background-baseline-software-1.log`, `conformance-folding-baseline-software-1.log`; PNG pairs are in the corresponding `renderer_conformance_artifacts` directories.

### SQ-03 and supplemental SQ-07

- **SQ-03**: `sq-03-after-sq02.patch`, based on `7999671`, coalesces costly property/style synchronization while retaining synchronous getters and reentrant/windowless behavior. Source reviewed; matched performance gate **not run**. `sq03-theme-probe/` and `configure-sq03-theme-probe.cmd` preserve an uncompiled full-paint theme benchmark. `sq03-profile-main.cpp` preserves an uncompiled insert-2000 full-paint harness. Compare identical correctness-only source/libs differing solely by SQ-03 before enabling this change.
- **Supplemental SQ-07 kerning correction**: `sq07-kerning.patch` restricts raw glyph advances to fixed-pitch fonts and preserves the cursorToX path for proportional fonts; includes a public/captured caret regression. Source reviewed, **not gated**. `width-kerning-editor-baseline.log` and `width-kerning-baseline.log` reproduce Arial AV: Qt cursor 33 versus public position 34/caret-left 34, expected caret-left 32 after caret-width offset. Upstream Qt 5.3.2 uses cursorToX. The fail-first test-only checkpoint build was canceled before compilation.
- SQ-07 profiling (`width-profile-run1/2/3.*`) found zero UTF8 measurement calls in normal scenarios. Optional checkMonospaced uses a fresh empty font cache; no default caller enables it. Margin scrolling spent roughly 1–2% in WidthText; insertion repeated property queries dominate the call count and belong to SQ-03. No broad shaping fast path or general speedup is justified by these measurements.
- Pending documentation is in the worktree plus `C:\Users\imak\AppData\Local\Temp\scintillaquick-editor-docs-20260926.patch`; review statements against final accepted semantics before publishing.

## Preserve source/build pairing

`BUILD_GATES_HANDOVER.md` in scratch is the detailed build-worker record. Critical distinctions:

| Artifact | Exact purpose/state |
|---|---|
| `baseline-source`, `baseline-ninja/ScintillaQuick.lib` | Matching original `0778c97`. Use archived headers. **Do not rebuild baseline-ninja:** its CMake source points to advancing canonical. |
| `checkpoint-source`, `checkpoint-ninja` | Incremental static candidate. Library/conformance executable are checkpoint 9 (attachment fix only); other test executables are checkpoint 8. |
| `checkpoint-source` unbuilt edits | Caret rounding, original shape coordinates, and only the **test file** from the kerning patch. The production kerning guard is absent. |
| Not yet copied to checkpoint | Item-viewport indicator crop patch and latest corrected conformance oracle. G5-08 optimization is also absent from this correctness snapshot. |
| `shared-source`, `shared-diagnostic-ninja` | Immutable shared-link diagnostic; DLL/example/benchmark build, private smoke-test link fails. |
| `conformance-repro-source`, `conformance-repro-ninja` | Baseline-compatible 66-case harness linked to archived baseline headers/library. Final candidate suite is 74 cases. |

Before SQ-03/G5-08 comparisons, archive a matching source copy **and** built library for each side. Do not compare a moving source tree with an older library. Copy-Item can preserve old mtimes; set destination LastWriteTimeUtc to UtcNow so Ninja rebuilds copied sources. Never mutate a snapshot during a build or test.

## Resume gates and toolchain

1. Repair published cross-platform/shared-test CI issues and gate those corrections separately.
2. Run the saved kerning test without its production fix; then apply and verify the guard.
3. Integrate remaining renderer/SQ-06 correctness deltas and the corrected 74-case oracle. Run the complete native software/RHI DPR matrix and regression suite; resolve every failure.
4. Preserve correctness-only before/after artifacts for SQ-03, then G5-08. Run matched full-paint benchmarks, including a long horizontally scrolled indicator. Raw upstream drawing can still iterate offscreen geometry despite bounded image allocation.
5. Run full static/shared builds/tests and install/consumer gates; update documentation; independently review coherent commits and push completed findings promptly, as authorized. Reconcile all 20 IDs before declaring completion.

Every compile, link, configure/regeneration or packaging command must use `queued-build`; no bypass for queue delays. Existing helpers activate VS 2026 Community with `vcvarsall.bat x64 -vcvars_ver=14.51` in the same shell. Examples (substitute the scratch absolute path for `<scratch>`):

```powershell
queued-build --slots 1 -- cmd.exe /d /c <scratch>\configure.cmd <source> <fresh-ninja-build>
queued-build --slots 4 -- cmd.exe /d /c <scratch>\build.cmd <build> [targets]
queued-build --slots 1 -- cmd.exe /d /c <scratch>\build-serial.cmd <build> [targets]
```

CMake is `C:\Users\imak\AppData\Local\Programs\cmake-4.4.2-windows-x86_64\bin\cmake.exe`; Ninja is `C:\Qt\Tools\Ninja\ninja.exe`. Qt is `C:\Qt\6.11.1\msvc2022_64` (package name does not select the compiler). Require cached C/C++ compiler paths under `C:\Program Files\Microsoft Visual Studio\18\Community`; actual toolset was 14.51.36231. Use Embedded `/Z7`. `configure-shared.cmd` exists for a fresh shared tree. No FASTBuild was used for these gates.

Initial toolchain preflight: VS 18.9.3 complete and launchable; no VS/CBS/Windows Update reboot condition, only a Mozilla PendingFileRename cleanup. Recheck the required preflight in a new session; pending reboot alone is informational. Evidence includes `render-environment.json` and configure/build logs.

Runtime CTest may run directly, with `--parallel 1`; serialize visible desktop tests between workers. CTest environments supply Qt paths. Native RHI uses D3D11/threaded rendering; software uses the software backend. New names are `scintillaquick_renderer_{software,rhi}_{1,1_25}_test`, each with a 60-second bound. Direct probes need Qt bin/plugin paths and Windows QPA; offscreen QPA did not load the native font setup and is unsuitable for the font oracle.

At wrap-up, owned queued requests/builds/tests were stopped or complete. The last queued checkpoint-10 request was canceled **before compilation**; its log contains only queue waits. No owned active build/test process remains. Unrelated user/session processes and worktrees were left alone.
