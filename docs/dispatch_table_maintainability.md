# Dispatch Table Maintainability

This document is the design and implementation record for Package 2F. Stage 1
is implemented: the runtime classifier now uses one checked-in rule table with
equivalent behavior. Stage 2 is implemented as an audit-only inventory check.
Stage 3 remains future work.

The dispatch table is correctness-critical because every public, convenience,
and direct Scintilla message path relies on it to decide whether a message is a
pure query, an overlay change, a static-content/style mutation, a scroll event,
or an unknown message that must receive a conservative full resync.

## Current State

The current classifier lives in `src/core/scintillaquick_dispatch_table.h` and
is intentionally header-only so `tests/dispatch_table/main.cpp` can test it as a
pure function.

Stage 1 centralized the classifier into one sorted `Message_rule` table. Each
rule records the message id, dispatch effect, scroll-width reset bit, and legacy
getter flag. Lookup uses binary search, and compile-time checks require the
table to stay sorted and unique. The existing helper APIs remain intact and now
read from the same rule source:

| Surface | Current shape | Purpose |
| --- | --- | --- |
| Getter allow-list | `scene_graph_message_is_known_getter()` over the rule table | Preserves legacy getter helper behavior. |
| Read-only allow-list | `scene_graph_message_is_known_read_only()` over the rule table | Fast-paths pure queries, including helpers that are not named `SCI_GET*`. |
| Update classifier | `scene_graph_update_request()` over the rule table | Gives narrow classifications for known mutating or visual messages, then falls back to conservative full resync. |
| Scroll-width reset | `tracked_scroll_width_should_reset()` over the rule table | Mirrors `scene_graph_update_request_info_t::scroll_width_reset` from the same entry. |

The important invariant is already documented and tested:

> Any message that is not explicitly known read-only and not explicitly
> classified more narrowly must default to conservative full resync.

That invariant must not be weakened. Unknown messages should over-invalidate
rather than risk stale rendering.

## Remaining Drift Risks

The previous switch-based design was safe at runtime but expensive to maintain:

- Message knowledge was split across multiple switches. Stage 1 removed that
  local drift source by putting dispatch effect and scroll-width behavior in
  one rule.
- Read-only classification is partly name-based and partly semantic. Scintilla
  has `get` iface entries, but also query-like `fun` entries such as geometry,
  marker, lexer metadata, and encoding conversion helpers.
- Some query-looking messages are stateful and must not be fast-pathed.
  Existing tests explicitly keep `SCI_SEARCHNEXT`, `SCI_SEARCHPREV`,
  `SCI_SEARCHINTARGET`, replacement, formatting, allocation, document creation,
  loader creation, and private lexer calls out of the read-only set.
- Scintilla updates can add new messages. The conservative fallback protects
  correctness, but new read-only messages may cause unnecessary full resyncs
  until audited.
- The dispatch tests sweep a generous numeric range, and Stage 2 now adds the
  exact message inventory from `Scintilla.iface` as audit output without
  changing runtime dispatch.

The drift to avoid is under-invalidation. Over-invalidation is allowed as a
temporary conservative default, but it should be visible in tests or audit
output so maintainers can decide whether to classify it.

## Options

### Option A: Manual Constexpr Rule Table

Stage 1 implemented this option with one checked-in table:

```cpp
enum class Dispatch_effect {
    Read_only,
    Overlay,
    Static_content,
    Static_content_and_style,
    Scroll,
};

struct Message_rule {
    unsigned int message;
    Dispatch_effect effect;
    bool scroll_width_reset;
};
```

`scene_graph_update_request()` uses a sorted constexpr table with binary search,
converts a found rule into `scene_graph_update_request_info_t`, and keeps the
same conservative fallback for messages not present in the table. A future
implementation may choose a different lookup shape only if it preserves
switch-equivalent hot-path performance; a linear scan would require benchmark
smoke even when the rule table is behavior-equivalent.

Benefits:

- One source of truth for `needed`, `static_content_dirty`,
  `needs_style_sync`, `scrolling`, and `scroll_width_reset`.
- No build-time generation complexity.
- Small, reviewable implementation.
- Equivalence tests cover the full swept message range and all caller-visible
  fields.

Costs:

- The table is still manually maintained.
- Scintilla iface drift is still discovered only by tests, benchmarks, or manual
  review unless an audit tool is added separately.
- Comments for non-obvious rules need a nearby mechanism, such as grouped table
  sections or explicit `//` comments before entries.

This was the safest first implementation step and is now the current runtime
shape.

### Option B: Generated Table From Scintilla Iface Plus Overrides

Parse `third_party/scintilla/include/Scintilla.iface` and combine it with a
ScintillaQuick override file to produce a generated dispatch table.

The iface can provide:

- Message names and numeric ids.
- Feature kind: `get`, `set`, or `fun`.
- Parameter and return types.

The override file would provide ScintillaQuick semantics:

```yaml
SCI_GETTEXT:
  effect: read_only
SCI_SETTEXT:
  effect: static_content_and_style
  scroll_width_reset: true
SCI_CHARRIGHT:
  effect: overlay
SCI_SETFIRSTVISIBLELINE:
  effect: scroll
SCI_SEARCHNEXT:
  effect: conservative_full_resync
  reason: stateful search command
```

Benefits:

- The exact upstream message inventory becomes visible.
- Scintilla upgrades can produce a clear diff of added, removed, or renumbered
  messages.
- Runtime table output can eventually be generated reproducibly from the iface
  and overrides.
- Missing override coverage can fail a generator check before it becomes a
  subtle performance issue.

Costs and risks:

- The iface feature kind is not enough to decide visual invalidation. Many
  `fun` entries are pure queries; many others mutate editor state.
- A generator bug can create broad dispatch regressions.
- Reviewing generated output can become noisy unless output ordering and
  formatting are stable.
- If generation is allowed to fast-path messages only because they are `get`,
  the project risks under-invalidating on a misunderstood Scintilla contract.

Generation is valuable, but only after the Stage 1 table stays stable and the
generator has an audit-only phase.

## Recommendation

Use a staged path:

1. Stage 1 is complete: the classifier is a manual constexpr rule table with no
   behavior changes.
2. Stage 2 is complete: the audit-only iface parser reports message inventory,
   drift, and candidate classifications without affecting runtime dispatch.
3. After the audit output stays stable through review cycles, optionally
   generate the rule table
   from checked-in overrides and require reproducible generated output.

Do not make generated logic influence invalidation until the audit-only phase is
stable and reviewed.

## Staged Implementation Plan

### Stage 1: Single Manual Rule Source

Status: complete and reviewed green locally.

Completed goal: remove parallel-switch drift while preserving the exact dispatch
result for every message in the swept range.

Completed write scope:

- `src/core/scintillaquick_dispatch_table.h`
- `tests/dispatch_table/main.cpp`
- No renderer, render-frame, public API, `send()`, `sends()`, direct callback,
  or invalidation-state changes.

Implemented constraints:

- Preserve `scene_graph_update_request_info_t` public shape unless a separate
  call-site review approves a change.
- Preserve `scene_graph_message_is_known_read_only()` as a public test/helper
  API if tests or call sites still need it.
- Preserve unknown-message fallback:
  `{needed=true, static_content_dirty=true, needs_style_sync=true,
  scrolling=false}`.
- Keep `scroll_width_reset` in the same rule as the message effect. If a
  compatibility helper named `tracked_scroll_width_should_reset()` remains, it
  must query the same rule table.
- Comments are required for narrow classifications that are not obvious from the
  message name, especially overlay-only caret movement and stateful
  query-looking messages that intentionally remain conservative.

Implemented tests:

- Existing dispatch-table test suite passes with equivalent assertions.
- Automated equivalence coverage sweeps every message in the full
  swept range before replacing the implementation. The sweep must compare every
  output field that affects callers: `needed`, `static_content_dirty`,
  `needs_style_sync`, `scrolling`, and `scroll_width_reset`. Mechanical review is
  still required, but it is not a substitute for this test coverage.
- Tests assert stateful query-like messages are not read-only.
- The mirror test proves `scene_graph_update_request(msg).scroll_width_reset`
  matches `tracked_scroll_width_should_reset(msg)` if that helper remains.
- The recursion-safety test remains for the exact `syncQuickViewProperties()`
  path.

Completed review gates:

- One dispatch/code reviewer verifies behavior equivalence and fallback shape.
- One test reviewer verifies tests fail for under-invalidation and accidental
  read-only classification.
- One performance reviewer checks that caret movement and public read-only query
  fast paths remain narrow and do not regress to full resync.

Completed validation gates:

- `scintillaquick_dispatch_table_test`.
- CI-compatible CTest subset.
- `git diff --check`.
- Benchmark smoke if classification changes in a hot path, or if the Stage 1
  implementation uses linear scan rather than sorted constexpr binary search or
  another switch-equivalent lookup. A behavior-equivalent sorted lookup should
  not require expensive benchmarks by default.

### Stage 2: Audit-Only Iface Inventory

Status: complete locally. `tools/dispatch/audit_dispatch_table.py` reads the
vendored iface inventory and the Stage 1 rule table, then reports deterministic
audit sections. It is also registered as `scintillaquick_dispatch_iface_audit_test`
when CMake finds a Python 3 interpreter.
The current warning baseline and accepted triage policy are recorded in
[`dispatch_audit_triage.md`](dispatch_audit_triage.md).

Goal: make Scintilla upgrade drift visible without changing runtime behavior.

Completed write scope:

- `tools/dispatch/audit_dispatch_table.py`.
- `tests/CMakeLists.txt`.
- `docs/dispatch_table_maintainability.md`.
- `docs/review_remediation_plan.md`.
- No runtime dispatch changes.

Usage:

```powershell
python tools/dispatch/audit_dispatch_table.py --check
ctest -R "scintillaquick_dispatch_iface_audit_test|scintillaquick_dispatch_table_test"
```

The audit parses:

- `third_party/scintilla/include/Scintilla.iface`
- The manual rule table in `src/core/scintillaquick_dispatch_table.h`

The audit reports:

- Message inventory counts by `get`, `set`, `fun`, category, deprecated
  category, min/max ids, duplicates, and parse gaps.
- Manual-table coverage by rule count, effect count, legacy getter count, and
  scroll-width reset count.
- Unclassified iface messages absent from the rule table, grouped by kind.
- Rule entries that no longer exist in iface, excluding explicitly documented
  local compatibility guards.
- `get` entries that are not classified read-only.
- `set` entries classified read-only.
- `fun` entries classified read-only, requiring explicit override comments.
- Heuristic override candidates such as query-looking `fun` entries, stateful
  query-looking messages that intentionally remain conservative, and possible
  scroll-width reset candidates.

The `--check` mode fails only on structural issues:

- Cannot parse the iface or rule table.
- Duplicate manual table entries.
- Rule entries missing from iface without an explicit compatibility
  explanation.
- `set` entries classified `Read_only`.
- Nondeterministic output ordering when detectable.

Unclassified iface messages and heuristic override candidates remain audit
warnings. They should not fail CI until reviewers intentionally promote a
specific finding to a hard rule.

Completed review gates:

- One dispatch reviewer checks the parser's interpretation of iface syntax.
- One Scintilla-upgrade reviewer checks that the audit helps identify upstream
  drift without encouraging unsafe fast paths.
- One test reviewer checks that the audit output is deterministic.

Validation gates:

- `python tools/dispatch/audit_dispatch_table.py --check`.
- `ctest -R "scintillaquick_dispatch_iface_audit_test|scintillaquick_dispatch_table_test"`.
- `git diff --check`.
- No build-system dependency on network or untracked local tools.

### Stage 3: Generated Rule Table From Overrides

Goal: optionally replace the manual table with generated output once the audit
has proven useful and stable.

Future write scope:

- Generator script under `tools/` or `scripts/`.
- Checked-in override file, for example `src/core/scintillaquick_dispatch_rules.yml`
  or `tools/dispatch/scintillaquick_dispatch_rules.yml`.
- Checked-in generated header, for example
  `src/core/scintillaquick_dispatch_table.generated.h`.
- Thin hand-written wrapper in `src/core/scintillaquick_dispatch_table.h`.
- `tests/dispatch_table/main.cpp`.
- Build-system target only if generation is integrated into CMake.

Generated-output reproducibility requirements:

- Generated output must be checked in.
- Output order must be deterministic, sorted by numeric message id and then name
  where needed.
- Generated files must contain no timestamps, absolute paths, machine names, or
  locale-dependent formatting.
- The generator must consume only checked-in inputs:
  `Scintilla.iface`, override file, and the generator script.
- CI must have a check that regenerates the output and fails if `git diff`
  becomes non-empty.
- Deprecated or optional Scintilla macros must be handled deterministically, with
  explicit guards when the public header requires them.
- Unknown or unclassified messages must still map to conservative full resync at
  runtime. Generation must never emit a default read-only rule.

Override-file requirements:

- Every read-only `fun` entry needs an explicit override and short reason.
- Every stateful query-looking message that remains conservative needs an
  explicit negative override or deny-list entry.
- Every narrow overlay-only or scroll-only mutator needs an explicit reason.
- Every scroll-width reset rule lives with the same message entry as its
  dispatch effect.
- New iface messages default to conservative runtime behavior until reviewed.

Review gates:

- Two dispatch/code reviewers inspect generated and source inputs.
- One test reviewer verifies generated-output reproducibility and failure modes.
- One performance reviewer checks hot-path classifications against current
  benchmark-sensitive behavior.

Validation gates:

- Regenerate output and assert clean `git diff`.
- `scintillaquick_dispatch_table_test`.
- CI-compatible CTest subset.
- Static and shared builds if the generated header changes include behavior or
  build graph.
- Benchmark smoke for caret movement and read-only query hot paths if any
  classification differs from Stage 1.

## Regression and Performance Considerations

Dispatch classification is an invalidation input. It is not a renderer rewrite
and should not be bundled with renderer, scene-graph node, render-frame, or
update-batching changes.

The main regression modes are:

- Under-invalidation: a mutating message is classified read-only and stale
  rendering survives until a later unrelated update.
- Over-invalidation: a hot read-only query or overlay-only operation falls back
  to full resync and hurts scrolling, caret movement, or property-sync paths.
- Recursion: an internal read-only query used by property sync is not
  fast-pathed, causing `send()` to schedule sync and re-enter itself.
- Scroll-width drift: a text/style/layout message updates visual width but the
  scroll-width reset bit is not updated with it.

Performance-sensitive classifications to preserve:

- Caret and selection movement commands should stay overlay-only.
- Public read-only queries should not mark static content dirty.
- Line/position lookup queries should stay read-only so vertical-scroll reuse is
  not accidentally defeated.
- Unknown messages should still full-resync; optimizing them requires an
  explicit rule and review.

No renderer or invalidation refactor should be included in a dispatch
maintainability implementation. If a future package needs to change both, split
it into two reviewed changes: dispatch equivalence first, invalidation or
renderer changes later with visual and benchmark gates.

## Future Package Checklist

Before any Stage 3 or follow-up implementation begins:

1. Decide whether the package is Stage 3 or a Stage 1/2 follow-up.
2. Confirm write scope matches that stage.
3. Confirm unknown-message fallback remains conservative.
4. Confirm automated equivalence tests cover every swept message and the
   `needed`, `static_content_dirty`, `needs_style_sync`, `scrolling`, and
   `scroll_width_reset` fields.
5. Confirm no renderer, scene-graph, render-frame, `send()`, `sends()`, or
   direct-callback behavior is changed without a separate package.

Recommended next step: keep Stage 3 generated output deferred. Use the Stage 2
audit during Scintilla upgrades and dispatch reviews until the output is stable
enough to justify a separate generation proposal.
