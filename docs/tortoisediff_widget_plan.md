# TortoiseDiff-Style Widget Plan

## Goal

Build a TortoiseDiff-style two-pane text diff viewer on top of
`ScintillaQuick`.

The baseline is:

- keep `ScintillaQuick` as the editor/view primitive
- build the diff widget outside the library first, probably as an example or
  consuming component
- patch `ScintillaQuick` only after a concrete missing primitive is proven

## Non-Goals For The First Pass

- no three-way merge
- no full Git UI
- no binary/image diff
- no syntax-aware semantic diff
- no patch editing
- no custom editor renderer
- no ignore-whitespace mode

Add those only after the two-pane text diff works.

## Human-In-The-Loop Rule

Every step ends with a human checkpoint. Each checkpoint must name:

- fixture used
- demo or test command
- expected observable result
- acceptable deviations from TortoiseDiff
- artifact used for acceptance, such as a screenshot, captured frame, or test
  log

Do not start the next step until the current one is accepted.

## Fixture Set

Create the initial fixture set before coding diff behavior:

- identical files
- one inserted line
- one deleted line
- one changed line
- changed block where left and right line counts differ
- multiple hunks
- whitespace-only changes, treated as real changes
- long lines that require horizontal scrolling
- empty file versus non-empty file
- no trailing newline
- CRLF versus LF
- tabs and indentation-only changes
- adjacent hunks with minimal context

Parser-specific fixtures come later:

- new file
- deleted file
- path with spaces
- content line that starts with diff-looking text
- malformed hunk counts
- multi-file Git diff rejection
- binary diff rejection

Large-file fixtures are generated only for the performance step.

## Step 1: Two ScintillaQuick Panes

Build:

- a minimal `DiffView` surface with two `ScintillaQuick_item` instances side by
  side
- same font, same tab width, same wrap mode
- load pane text first, then set both panes readonly
- simple left/right sample text, no diff processing yet

Test:

- open the demo and verify both panes render
- smoke test constructs the component and loads different text into both panes
- verify readonly blocks edits but still allows selection and copy

Human checkpoint:

- accept when both panes render, focus, select, copy, and stay readonly

## Step 2: Editor Presentation Baseline

Build:

- line-number margins on both panes, initially display-buffer line numbers
- no wrapping for the first pass
- consistent colors, caret hidden or unobtrusive, readonly behavior confirmed
- center separator as plain Qt Quick UI, not inside Scintilla

Test:

- visual/manual check with long and short lines
- confirm selection and copy still work in each pane

Human checkpoint:

- compare the static two-pane view against TortoiseDiff with identical files
- accept only with an explicit note that line numbers are display-buffer
  numbers until Step 4 chooses the source-line strategy

## Step 3: Synchronized Scrolling

Build:

- connect left `verticalScrolled` to right `scrollVertical`
- connect right `verticalScrolled` to left `scrollVertical`
- connect horizontal scroll signals the same way
- use one small recursion guard while applying mirrored scroll

Test:

- programmatically set `SCI_SETFIRSTVISIBLELINE` on the left and assert the
  right pane follows
- call `scrollHorizontal` and assert the peer follows
- manually scroll from either pane

Human checkpoint:

- accept when both panes stay locked vertically and horizontally from either
  side

## Step 4: Display Row Model And Filler Policy

Build:

- a display-row model that carries source mapping, hunk identity, side-specific
  state, and optional changed-line grouping:

```cpp
enum class DiffSideState { Equal, Added, Deleted, Changed, Filler };

struct DiffRow {
    int hunkId;
    int changedGroupId;     // -1 when not a changed-line pair/group
    int leftSourceLine;     // -1 means filler
    int rightSourceLine;    // -1 means filler
    DiffSideState leftState;
    DiffSideState rightState;
};
```

- hard-code a few rows from fixtures
- render aligned left/right text by inserting actual blank buffer lines for
  filler rows
- lightly tint the demo rows with a Qt Quick overlay only if blank filler rows
  are otherwise too hard to verify visually; final highlighting still belongs
  to Steps 8 and 9
- document that normal copy and stock line-number margins are display-buffer
  based at this stage

Source-line numbers:

- if source-line numbers are required now, prototype `SC_MARGIN_TEXT` or a Qt
  Quick gutter before accepting this step
- otherwise keep stock numeric margins and fix source-line gutters later

Test:

- assert left and right display row counts are equal
- assert exact row sequences for 1:1, 1:N, N:1, and N:M changed blocks
- assert filler rows appear only where one side has no source line
- assert panes with one-sided filler rows render non-identical text
- assert the temporary overlay produces visible pixels in a captured frame
- verify and explicitly record the current copy and line-number behavior

Human checkpoint:

- accept only if blank-row alignment is correct and the temporary copy/gutter
  limitations are acceptable
- manual acceptance should confirm equal display row counts, but non-identical
  pane contents: added/deleted source rows appear opposite blank filler rows

## Step 5: Widget Input Contract

Build:

- widget input is `leftText`, `rightText`, and a caller-supplied row/hunk model
- no Git parsing in the widget
- no raw text diffing in the widget yet
- any temporary Qt Quick overlay state is derived from the row/hunk model, not
  accepted as separate widget input
- rendering and navigation tests use hand-authored `DiffRow` fixtures first

Test:

- assert the widget can render from explicit row/hunk fixtures without invoking
  any parser or diff algorithm
- malformed explicit models fail cleanly, for example mismatched row counts or
  invalid source-line references

Human checkpoint:

- accept when the widget contract is small, parser-independent, and does not
  expose the temporary overlay as API

## Step 6: Raw Text Line-Diff Adapter

Build:

- separate adapter that converts `leftText` and `rightText` into the accepted
  row/hunk model
- adapter consumes the raw widget texts and emits only model rows; it does not
  normalize text, build display buffers, or change the Step 5 widget API
- start with the smallest deterministic line-diff algorithm that passes the
  fixture set
- for repeated lines with multiple valid shortest scripts, prefer stable
  left-to-right anchors: earliest left source line first, then earliest matching
  right source line

Test:

- assert adapter output for the initial fixture set
- keep rendering/navigation tests on hand-authored `DiffRow` models; adapter
  tests are separate and assert row/hunk output rather than rendered panes

Human checkpoint:

- compare line-level hunk placement against TortoiseDiff on fixture files

## Step 7A: Stored Unified-Diff Fixture Adapter

Build:

- parse one in-memory unified diff for one text file into `DiffWidgetInput`
- no live Git, no filesystem input, and no multi-file support
- accept normal `diff --git` headers, `---`/`+++`, hunk headers, `/dev/null`
  creates/deletes, and no-newline markers
- reject multi-file, binary, combined, rename-only, and malformed diffs with a
  clear typed error

Test:

- assert parser output for parser-specific in-memory fixtures
- assert rejection cases produce clear errors
- integration test feeds parsed output into the widget contract from Step 5

Human checkpoint:

- accept when the widget renders from a parsed unified diff
- accept only when parser failures are separate from widget rendering failures

## Step 7B: Live Git Diff Command Adapter

Build:

- add a demo-grade live command adapter for one hardcoded text file
- run `git diff --no-color --unified=100000` and parse stdout into
  `DiffWidgetInput`
- keep live command execution outside the widget
- if git is unavailable, exits nonzero, or returns empty output, fall back to the
  stored fixture so the demo remains launchable

Future work:

- file selection
- real old/new full-file content handling
- multi-file rejection UX
- typed errors surfaced to UI

## Step 8: Line Highlight Primitive Spike

Build:

- add a tiny captured-frame fixture that only proves whether
  `SC_MARK_BACKGROUND` is currently usable for full-line row backgrounds
- if `SC_MARK_BACKGROUND` captures but is not visible, try a no-margin
  `SC_MARK_FULLRECT` marker on `SC_LAYER_UNDER_TEXT` before patching renderer
  code
- keep the Step 4 Qt Quick overlay in the prototype unless captured-frame proof
  shows that native marker primitive is visible and correctly framed
- treat renderer patching as a later explicit decision, not an automatic outcome
  of this spike

Test:

- prove whether `SC_MARK_BACKGROUND` or no-margin under-text
  `SC_MARK_FULLRECT` renders a usable full-line row background in a captured
  frame

Human checkpoint:

- accept the overlay, native primitive, or renderer-patch path before real row
  highlighting

## Step 9: Line Highlighting

Build:

- mark display rows by side state:
  - added: green-ish full-line background on the right
  - deleted: red-ish full-line background on the left
  - changed: yellow/orange full-line background on both sides
  - filler: neutral blank row
- use native `SC_MARK_FULLRECT` markers on `SC_LAYER_UNDER_TEXT`
  for row backgrounds
- zero margin masks for these marker numbers so they paint in the text area
- replace the Step 4 Qt Quick overlay with the native marker path in the
  prototype
- use hand-authored `DiffRow` fixtures first

Test:

- assert native marker bits follow the row side state
- pixel-check native `SC_MARK_FULLRECT` as solid rectangles; sparse/diagonal
  triangle-fan geometry is unacceptable
- rely on the Step 8 captured-frame proof for marker visibility

Human checkpoint:

- accept when visible row colors are readable and remain aligned through
  scroll/zoom
- document accepted deviations from TortoiseDiff

## Step 10: Scrollbar Prototype

Build:

- `ScintillaQuick` does not embed widget scrollbars
- replace the old arrow buttons with Qt Quick Controls `ScrollBar`s, not
  hand-painted scrollbars
- bind the vertical scrollbar to `scrollVertical(int)` and `verticalScrolled`
- bind the horizontal scrollbar to `scrollHorizontal(int)`,
  `horizontalScrolled`, and `horizontalRangeChanged`
- compute the horizontal range from the wider pane so one-sided long lines can
  still be scrolled
- show each scrollbar only when its range is needed
- mirror the existing synchronized-scroll guard

Test:

- manually scroll each bar from the thumb, track clicks, and each pane
- verify thumb positions follow both panes

Human checkpoint:

- accept when native-looking control behavior, thumb positions, horizontal
  drag/click sync, vertical drag/click sync, and both panes staying aligned are
  acceptable

## Step 11: Inline Changed Text Highlighting

Status:

- next implementation slice after Step 10 scrollbars; some notes may call this
  user-called 10.2, but keep it numbered Step 11 to avoid renumbering later
  plan entries

Build:

- simple changed-text highlighting pass for changed-line pairs/groups
- compute side-specific spans with common-prefix/common-suffix trimming
- convert spans to Scintilla document positions for each side
- render spans with native Scintilla indicators, not a Qt Quick overlay

Known limitations:

- ASCII/simple code-unit spans only; no Unicode grapheme correctness yet
- no token-aware, moved-text, or semantic matching
- repeated text may highlight a larger or less intuitive middle span
- no deletion ghosts or inserted placeholders on the opposite side
- line-count-mismatched changed groups may get only best-effort inline spans

Test:

- `abc def ghi` versus `abc xyz ghi` highlights only `def` and `xyz`
- insertion inside one line highlights only the inserted span
- repeated-token fixture documents the simple algorithm's behavior

Human checkpoint:

- compare changed-line detail against TortoiseDiff
- replace only the inline algorithm if the simple span finder is too noisy

## Step 12: Hunk Navigation

Build:

- first slice: previous/next hunk controls and shortcuts
- current hunk index as needed for navigation
- scroll both panes to the selected hunk
- Step 12.1 active hunk marker: draw TortoiseDiff-style black horizontal
  lines at the top and bottom of the selected hunk, updated on navigation,
  scroll, resize, and zoom before moving to Step 13 copy hygiene
- keep the active-hunk boundary thickness device-pixel-snapped: target two
  physical pixels and convert that to logical height from the window DPR

Test:

- assert navigation order over a multi-hunk hand-authored model
- assert `firstVisibleLine` moves to the selected hunk
- manual previous/next control and shortcut test

Human checkpoint:

- accept when navigation lands on the expected display row for every hunk

## Step 13: Copy And Selection Hygiene

Build:

- decide whether copied text should include filler rows
- if not, use source-line mapping to strip filler rows from copy output
- prototype the public `aboutToCopy(QMimeData*)` hook before changing
  `ScintillaQuick`

Test:

- select across an inserted/deleted block
- copy from each pane
- assert copied text matches the accepted source-side behavior

Human checkpoint:

- accept when copy behavior is explicit and matches the intended viewer UX

## Step 14: Optional Merge Actions

Build only if needed:

- copy selected hunk left-to-right
- copy selected hunk right-to-left
- expose the resulting edited buffer outside the widget

Test:

- apply one hunk and assert the output text changes exactly as expected
- reject conflicting or unsupported operations clearly

Human checkpoint:

- decide whether this is still a viewer or has become a merge tool

## Step 15: Large File Check

Build:

- no new features
- profile and fix only problems proven by the large fixture

Test:

- load and scroll a generated 10k to 50k line diff
- measure initial load time and scroll responsiveness
- verify memory does not grow while scrolling

Human checkpoint:

- accept if performance is good enough for real expected files
- if not, optimize the row model or rendering calls before adding features

## Step 16: Patch ScintillaQuick Only If Proven Necessary

Possible library changes, only after the external widget proves the need:

- render `SC_MARK_BACKGROUND` or another missing highlight primitive
- better support for custom gutter decorations
- stronger frame-validation coverage for background markers, text margins, or
  indicators
- copy-hook improvements for display buffers with filler rows
- scroll synchronization helpers if every consumer needs the same guard logic

Test:

- any `ScintillaQuick` change gets a focused frame-validation or
  visual-regression test
- no widget-specific behavior goes into the core library

Human checkpoint:

- accept a core change only if the diff widget cannot reasonably do it outside
  `ScintillaQuick`

## Milestones

Milestone 1 is Steps 1 through 3:

- two panes
- readonly presentation
- synchronized pane scrolling

Milestone 2 is Steps 4 and 5:

- accepted row/filler model
- parser-independent widget contract

That proves whether `ScintillaQuick` is the right visual baseline before owning
real diff parsing, inline highlighting, or merge behavior.
