# Dependency audit completion — 2026-09-26

All validated findings owned exclusively by ScintillaQuick have been addressed on `master` through `ebcb1e6`. The inventory covers **20 local IDs representing 19 distinct findings** because XC-07 duplicates SQ-10. Unsupported parts of the original claims are explicitly narrowed below; completion does not mean every original recommendation was implemented.

The source audit is `C:/plms/varinomics/_agent_reports/dependency_audit_2026-09-26.md`. **SQ-05, SQ-08 and XB-02 remain excluded** because they require other repositories, as do consumer-owned findings that merely mention ScintillaQuick. This work does not close those findings.

## Published disposition

| Finding IDs | Commits | Result and qualification |
|---|---|---|
| SQ-10, XC-07, G5-07 | `11749fc` | Consolidated equivalent node synchronization/conversion and removed unreachable geometry. Differing annotation fields and margin shaping semantics were retained. |
| SQ-11 | `94f8bb5` | Removed unreachable non-UTF8 platform paths. |
| G5-06 | `4a7cf86` | Reset IME composition position and kept query coordinates relative to the item. |
| SQ-09 | `a11ce3a` | Recorded verified fork provenance/local patches and corrected supported installed headers; no guessed upstream revision. |
| SQ-07 | `f39c254`, `4a4fbfa`, `eecc676` | Fixed effective-DPI cache invalidation, proportional-font kerning/caret mapping, and raw-versus-shaped fixed-font advances. Broad shaping-speed and consumer-probe claims were not established; see below. |
| G5-05 | `4590139` | Honored edit handlers for replacements and drag moves while preserving actual target semantics. |
| SQ-04 | `a02428d` | Preserved horizontal scroll during tracked-width recalculation; intentional document-switch reset remains. |
| SQ-01 | `6256c0f` | Delivered editor UI and captured-frame notifications. `painted()` denotes snapshot preparation, not GPU presentation. |
| SQ-12 | `9946c5d` | Captured call-tip images on the GUI thread. Autocomplete did not call Scintilla; the alleged QPixmap crash was not established. |
| G5-09 | `2cd4622` | Bounded automatic selection seeding and reused find-field layouts, with matched performance evidence. |
| SQ-02 | `7999671` | Established one caret blink timer and phase-independent captured geometry. Mouse dragging reproduced the divergence; the report's SCI_SETSEL example was incorrect. |
| SQ-06, G5-01, G5-02, G5-03, G5-04 | `8b6bc63` | Restored realized fonts, folding-margin colors, body clipping, indicator/marker semantics, backgrounds and layer ordering across software and RHI. |
| SQ-03 | `a9877c7` | Coalesced property synchronization while preserving synchronous getters and hidden/reentrant notification delivery, including nested event loops. |
| G5-08 | `ebcb1e6` | Consolidated solid underlines, cached guide images, shared body clipping and reused unchanged static nodes while refreshing overlays and inherited transform state. |

CI repair `54b49cc` corrected protected-enum qualification and used the public selection API in validation helpers. `eecc676` also isolated private platform tests from the shared-library ABI. The earlier Windows DPI assertion was caused by raw glyph advances differing from shaped cursor positions; the correction uses the shaped positions to populate the cache. The earlier red Windows workflow and unknown-cause diagnosis are superseded.

SQ-07 profiling found no UTF8 measurement calls in the normal measured scenarios. Margin scrolling spent approximately 1–2% in WidthText; repeated property synchronization during insertion belonged to SQ-03. These measurements did not justify a generalized WidthText fast path or replacement of the consumer's stronger font probe. Those unsupported recommendations were not adopted. SQ-06's original cache-size claim was also narrowed: the item queried two styles, not 257.

## Verification

The final product candidate `ebcb1e6` passed local Windows static and shared CTest suites, **12/12 each**, including **83 renderer cases in each of four native software/RHI × DPR 1/1.25 legs** and **33 unchanged visual-regression fixtures**. The installed shared-library consumer configured, built and ran successfully. The preceding renderer correctness gate also exercised 75 transform cases in each of four backend/DPR legs. Independent review accepted the production batches.

The renderer tests compare against upstream drawing/public API semantics rather than newly invented PNG goldens. Rendering consumes immutable captured values; const upstream drawing helpers operate on those values without live editor, ViewStyle or borrowed font state. Tests cover attachment, fractional coverage, raster coordinates/cropping, fonts, layer order and inherited transform/opacity, including fixed-revision overlay updates.

Local builds used VS 2026/MSVC v145 with the required queued-build discipline. Historical scratch libraries and executables remain tied to their matching source snapshots; they must not be mistaken for the final candidate.

Hosted Windows, Linux and macOS workflows passed for the preceding CI, renderer and SQ-03 publications and for the final product tip `ebcb1e6`:

| Platform | Final product-tip run | Result |
|---|---|---|
| Windows | [36260742640](https://github.com/imakris/ScintillaQuick/actions/runs/36260742640) | Passed |
| Linux | [36260742606](https://github.com/imakris/ScintillaQuick/actions/runs/36260742606) | Passed |
| macOS | [36260742608](https://github.com/imakris/ScintillaQuick/actions/runs/36260742608) | Passed |

## Performance evidence and limits

Comparisons used matched source/library pairs and native full-paint workloads. SQ-03 theme medians improved approximately **11–16% on software and 21–40% on RHI**; insertion improved **31–75%**, depending on the workload/backend. Software sampled frames matched exactly; RHI RGB differences were at most one channel value, matching baseline variability. One software latency statistic increased about 0.91 ms; these are workload-specific gains, not a universal latency claim.

G5-08 used 32 planned runs covering baseline/candidate, two repeats, two backends and four scenarios, with eight captures per scenario. The styled 500-line workload fell from about **55,400 nodes to 886–887**, while image nodes increased from 92 to 137. Median full-paint improvements were:

| Scenario | Software | RHI |
|---|---|---|
| Styled scrolling | 89–90% | 78–82% |
| Caret blinking | 92–93% | About 85% |
| Theme bursts | About 70% | 26–29% |

The 16 KiB horizontally scrolled indicator workload was effectively unchanged overall: software median comparisons ranged from 2.7% slower to 8.6% faster, and RHI from 0.3% to 3.8% faster. Its sampled frames were bit-exact on both backends. Other sampled G5-08 frames differed by at most one RGB channel value with exact alpha and no pixels exceeding the unchanged tolerance of three; baseline RHI repeats also varied by at most one.

The probe field called `p95_ms` is the **sampled maximum of eight captures**, not a robust tail-latency estimate. Some long-indicator maxima increased, and one RHI blink maximum rose from approximately 60 to 66 ms. No universal tail improvement is claimed. Texture-change diagnostics likewise do not establish universal texture-work savings.

## Retained evidence and continuation state

Original baseline reproductions, the narrowed inventory, patches and historical checkpoints remain under `C:/plms/_scratch/ScintillaQuick-audit-20260926`. Final resume evidence is under `C:/plms/_scratch/ScintillaQuick-audit-resume-20260926`; its `performance/` directory contains:

- `sq03-performance-summary.json`, `sq03-final-*-frame-parity.json` and matched run records;
- `g508-performance-summary.json`, `g508-*-frame-parity.json` and matched run records;
- `g508-static-final-ctest.log`, `g508-shared-ctest.log` and `g508-shared-consumer-*.log`.

The original local WIP `6f4001d` was reconciled by content against `ebcb1e6`. Its useful changes were adopted or corrected; remaining differences were superseded implementations, older tests/documentation or ordering/formatting. The original WIP worktree/branch and the landed CI, renderer and performance worktrees/branches have been removed after checking for unique state. No implementation continuation remains for this local audit. Unrelated worktrees belong to other sessions and were left untouched.
