# Dispatch Audit Triage Baseline

This document records the current warning baseline from
`tools/dispatch/audit_dispatch_table.py --check`. It is a maintenance aid for
Scintilla upgrades and dispatch-table reviews; it is not a request to change
message classifications.

Stage 2 remains audit-only. No runtime dispatch behavior is changed by this
document, and Stage 3 generated dispatch output remains deferred.

## Baseline

Baseline date: 2026-06-07.

Command:

```powershell
python tools/dispatch/audit_dispatch_table.py --check
```

Inputs:

- `third_party/scintilla/include/Scintilla.iface`
- `src/core/scintillaquick_dispatch_table.h`

Current structural result: `PASS`.

Message inventory:

| Metric | Count |
| --- | ---: |
| Total iface messages | 788 |
| Minimum message id | 2001 |
| Maximum message id | 4033 |
| `get` entries | 253 |
| `set` entries | 222 |
| `fun` entries | 313 |
| Basics category | 779 |
| Deprecated category | 7 |
| Provisional category | 2 |

Manual rule-table coverage:

| Metric | Count |
| --- | ---: |
| Rule entries | 442 |
| Known getter entries | 206 |
| Scroll-width reset entries | 17 |
| `Read_only` rules | 323 |
| `Overlay` rules | 70 |
| `Static_content` rules | 1 |
| `Static_content_and_style` rules | 47 |
| `Scroll` rules | 1 |

## Hard Failures

The current baseline has no hard structural failures.

Future audit runs should continue to fail package validation on structural
issues, including:

- unreadable or unparseable `Scintilla.iface`;
- unreadable or unparseable `k_message_rules`;
- duplicate table entries by name or id;
- table rules missing from iface without an explicit compatibility exception;
- any `set` iface entry classified as `Read_only`;
- nondeterministic table ordering or audit output.

Hard failures must be fixed or explicitly documented before dispatch work
continues.

The current script reports duplicate iface names or ids as audit findings, not
hard structural failures. Treat those reports as upgrade-review input unless a
later script policy deliberately promotes them to package-validation failures.

## Warning-Only Categories

These warning categories are accepted for the current baseline. They should be
reviewed when counts or message names change, but they do not require immediate
classification changes.

| Warning category | Current count | Current interpretation |
| --- | ---: | --- |
| Unclassified `get` messages | 0 | No issue. Every iface `get` is classified through the rule table. |
| Unclassified `set` messages | 183 | Conservative fallback. Setters are not read-only; over-invalidation is accepted until a narrower rule is reviewed. |
| Unclassified `fun` messages | 163 | Conservative fallback. Many functions mutate document, view, popup, style, selection, search, or allocation state. |
| `get` entries not `Read_only` | 0 | No issue. |
| `set` entries classified `Read_only` | 0 | No issue. This must remain zero. |
| `fun` entries classified `Read_only` | 70 | Intentional semantic read-only queries that are not iface `get` entries. |
| Query-looking `fun` messages absent from table | 0 | No issue. |
| Stateful query-looking messages intentionally conservative | 13 | Accepted conservative fallback for commands that look query-like but can mutate or depend on state. |
| Possible scroll-width reset candidates | 107 | Heuristic candidates only. The list is not proof that reset behavior is missing. |

The audit script truncates long terminal sections to keep output readable. Use
the script output for stable counts, categories, and structural status. For
exhaustive message-name review, inspect `Scintilla.iface`, `k_message_rules`,
or the parsed data in the script directly; a future full-output mode would be
needed before terminal output can be used as the exhaustive name list.

## Triage Buckets

### Safe To Leave Conservative Fallback

These are accepted as full-resync fallback unless a later review proves a
narrower classification is correct and worthwhile:

- unclassified `set` messages;
- unclassified mutating `fun` messages;
- unknown future Scintilla messages;
- deprecated setters that are still present for compatibility;
- stateful query-looking messages listed below.

The accepted stateful query-looking fallback list is:

| Message | Reason to keep conservative |
| --- | --- |
| `SCI_FORMATRANGE` | Formatting path can depend on external drawing/layout state. |
| `SCI_FORMATRANGEFULL` | Formatting path can depend on external drawing/layout state. |
| `SCI_REPLACETARGET` | Mutates target/document content. |
| `SCI_REPLACETARGETRE` | Mutates target/document content. |
| `SCI_REPLACETARGETMINIMAL` | Mutates target/document content. |
| `SCI_SEARCHINTARGET` | Uses target/search state and should not be treated as a pure viewport query. |
| `SCI_SEARCHNEXT` | Advances search state/selection. |
| `SCI_SEARCHPREV` | Advances search state/selection. |
| `SCI_CREATEDOCUMENT` | Creates document state. |
| `SCI_CREATELOADER` | Creates loader/document import state. |
| `SCI_ALLOCATE` | Allocates document storage. |
| `SCI_ALLOCATESUBSTYLES` | Allocates style/lexer-related state. |
| `SCI_PRIVATELEXERCALL` | Lexer-specific behavior is not generally auditable by name. |

### Needs Semantic Review

These should not be changed mechanically. A reviewer must inspect Scintilla
semantics, ScintillaQuick invalidation needs, tests, and performance impact:

- any new iface `get` that is not classified `Read_only`;
- any new query-looking `fun` absent from the rule table;
- any `fun` currently classified `Read_only` if Scintilla changes its contract;
- any possible scroll-width reset candidate before adding
  `scroll_width_reset`;
- any message whose behavior depends on popup/list/calltip, IME, direct
  callbacks, target/search state, document allocation, or lexer-private data.

### Possible Future Rule

These may become narrow rules only after semantic review and tests:

- unclassified messages that are proven overlay-only;
- unclassified messages that are proven static-content-only or style-only;
- possible scroll-width reset candidates that affect measured text width,
  wrapping, margins, annotations, represented text, tab stops, lexer styling, or
  font metrics;
- read-only `fun` candidates added by a future Scintilla release;
- performance-sensitive public messages that currently fall back to full
  resync and appear in profiles.

The current scroll-width candidate count is 107. It is generated by name hints,
not by deep semantic proof. Hint buckets overlap, so their counts are not
additive:

| Hint | Current count |
| --- | ---: |
| `Text` | 23 |
| `Style` | 36 |
| `Font` | 2 |
| `Tab` | 8 |
| `Wrap` | 12 |
| `Margin` | 14 |
| `Annotation` | 11 |
| `Representation` | 6 |
| `Indent` | 7 |
| `Lexer` | 3 |
| `Property` | 4 |
| `CodePage` | 1 |

### Deprecated And Provisional Caveat

Deprecated messages should normally stay conservative unless compatibility
requires a known rule. Provisional messages should be reviewed on every
Scintilla upgrade because their semantics may change.

Current deprecated messages:

| Message | Kind | Current dispatch status |
| --- | --- | --- |
| `SCI_SETSTYLEBITS` | `set` | Conservative fallback |
| `SCI_GETSTYLEBITS` | `get` | `Read_only`, known getter |
| `SCI_GETTWOPHASEDRAW` | `get` | `Read_only`, known getter |
| `SCI_SETTWOPHASEDRAW` | `set` | Conservative fallback |
| `SCI_SETKEYSUNICODE` | `set` | Conservative fallback |
| `SCI_GETKEYSUNICODE` | `get` | `Read_only`, known getter |
| `SCI_GETSTYLEBITSNEEDED` | `get` | `Read_only`, known getter |

Current provisional messages:

| Message | Kind | Current dispatch status |
| --- | --- | --- |
| `SCI_GETBIDIRECTIONAL` | `get` | `Read_only`, known getter |
| `SCI_SETBIDIRECTIONAL` | `set` | `Static_content_and_style` |

## Scintilla Upgrade Procedure

When `third_party/scintilla` is upgraded:

1. Run `python tools/dispatch/audit_dispatch_table.py --check`.
2. Compare inventory counts, category counts, structural status, and warning
   counts against this baseline.
3. If structural failures appear, fix the parser/table issue or add an explicit
   compatibility exception before changing classifications.
4. Keep newly added unclassified messages on conservative fallback until a
   semantic review approves narrower behavior.
5. Re-review all deprecated and provisional entries whose status changed.
6. Re-review the stateful query-looking fallback list if any listed message is
   removed, renamed, renumbered, or has changed parameters.
7. Re-review possible scroll-width candidates if Scintilla added text, style,
   font, tab, wrap, margin, annotation, representation, indent, lexer,
   property, code-page, or document-pointer messages.
8. Update this document with the new date, counts, accepted warnings, and any
   changed caveats.
9. Keep Stage 3 generation deferred unless the generated-output proposal,
   reproducibility gates, and review plan are separately approved.

This document should only record the accepted baseline and triage decisions. It
should not be used to smuggle runtime dispatch classification changes into a
Scintilla upgrade.
