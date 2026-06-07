# Platform Window Ownership Contract

This note records the current Qt Quick platform-window ownership contract after
Package 1F hardening. It is a maintenance contract for future changes, not a
pre-implementation design gate.

Scintilla's vendored `Window` abstraction stores an opaque `WindowID`.
ScintillaQuick maps that value to a `QQuickItem*`, but ownership depends on how
the item was created.

## Pre-Hardening Risk

Before Package 1F, `Window::Destroy()` deleted whatever `wid` pointed to and
then cleared the handle. That conflicted with Scintilla's upstream non-owning
`Window` model and with `wMain`, which points at the public
`ScintillaQuick_item` owned by the Qt/QML object tree.

The hardened rule is:

> Delete only Qt Quick items that ScintillaQuick explicitly registered as
> platform-owned. Borrowed and stale handles must never be deleted through a
> generic `Window`.

## Implemented Policy

Platform-created call-tip and completion/list-box items are tracked in a private
Qt-side registry in `src/platform/scintillaquick_platqt.cpp`.

- `wMain` is borrowed. It is assigned from `ScintillaQuick_core` to the main
  `ScintillaQuick_item` and is deliberately not registered as owned.
- Call tips are owned. `ScintillaQuick_core::CreateCallTipWindow()` creates a
  `Call_tip_item` when needed and registers it with `register_owned_window()`.
- Completion/list-box UI is owned. `List_box_impl::Create()` creates a
  `Quick_list_box_item` and registers it with `register_owned_window()`.
- Popup menu data is separate. `Menu` owns its heap allocated menu container and
  releases it through `Menu::Destroy()`.
- `Surface_impl` owns only internally allocated pixmaps/devices. Devices passed
  through `Init(SurfaceID, WindowID)` are borrowed.

The owned-window registry stores:

- the raw `WindowID` key,
- a `QPointer<QQuickItem>` so external Qt deletion becomes null,
- the owning `Window*`,
- the owned-window kind, currently call tip or list box.

A companion owner index maps each owning `Window*` to the handle it currently
owns. That index makes the registry owner-aware, so an old `Window` cannot
delete or resolve another owner's item if a raw address is later reused.

## Resolver Rules

All platform methods that consume a `WindowID` must resolve it through the
tracking policy before dereferencing or deleting.

Use `resolve_window_item_for_owner()` for owner-sensitive `Window` and list-box
paths. Its contract is:

- Matching live owned handle for this owner: return the live `QQuickItem*`.
- Matching owned handle whose item was externally deleted or tombstoned: return
  null and make the caller a no-op.
- Registry entry owned by another `Window`: return null if this owner still
  tracks the handle; otherwise allow raw fallback for an untracked borrowed
  handle.
- Owner-index entry without a live registry record: return null.
- Untracked handle: treat as borrowed and return the raw item.

`resolve_window_item()` is available for non-owner-sensitive lookup by handle,
such as call-tip reuse after the owner handle has already been validated. It
returns a live registry item when present, otherwise raw fallback for borrowed
handles.

Current `Window` consumers routed through the owner-aware resolver include
`GetPosition()`, `SetPosition()`, `SetPositionRelative()`, `Show()`,
`InvalidateAll()`, `InvalidateRectangle()`, `SetCursor()`, `GetMonitorRect()`,
and `List_box_impl::GetWidget()`.

## Destroy And External Deletion

`Window::Destroy()` delegates to `destroy_or_clear_window()`.

- Null handle: detach any owner-index entry and return.
- Unregistered handle: clear this `Window` without deleting the item.
- Registered handle owned by another `Window`: clear this `Window` without
  deleting the item.
- Registered handle owned by this `Window`: remove the owner index, clear the
  `Window`, erase the registry record, and delete the live item if it still
  exists.

External Qt deletion is handled through the registered item's
`QObject::destroyed` signal.

- Call tip deletion clears the owning `Window` when it still points at the
  deleted handle, removes the owner index entry, and erases the registry record.
  A later `CreateCallTipWindow()` sees `ct.wCallTip` as not created and
  recreates the item.
- List-box deletion keeps a tombstone: the registry record remains with a null
  `QPointer`. This lets Scintilla's autocomplete cancel path still observe the
  list box as created long enough to clear its active state, while every item
  access resolves to null and no-ops safely. Normal destroy/cancel then clears
  the owner state, and later list-box creation registers a fresh item.

## Reuse And Recreation

`CreateCallTipWindow()` creates and registers a call-tip item only when
`ct.wCallTip` is not created. It then always resolves the current handle and, if
live, updates position, size, and content. Reused call tips are therefore
repositioned and refreshed instead of keeping stale geometry.

`List_box_impl::Create()` destroys or clears any previous owned item, resolves
the parent through the same ownership policy, creates a new list-box item, and
registers it. List-box methods that need the item go through `GetWidget()`, so
stale externally deleted items no-op until Scintilla drives cleanup or creates a
replacement.

## Maintenance Rules

- Do not edit vendored Scintilla `Window` for this ownership policy.
- Do not register `wMain` as owned.
- Do not add raw `window(wid)`, `static_cast<QQuickItem*>(wid)`, or direct
  `QQuickItem*` dereferences in platform-window paths unless the path is
  explicitly borrowed and documented.
- New platform-owned `QQuickItem` windows must be registered with
  `register_owned_window()` and must define their external-deletion behavior.
- Preserve the list-box tombstone behavior unless autocomplete cancel semantics
  are changed and tested with the new lifecycle.
- If `Window::Destroy()` or resolver behavior changes, update this document and
  add focused lifecycle coverage.

## Completed Hardening Checklist

Package 1F completed the targeted hardening:

1. Added private ownership registry and owner index in platform code.
2. Registered call-tip and list-box items at creation.
3. Kept `wMain` unregistered and borrowed.
4. Added `QPointer` tracking and external-deletion handling.
5. Routed `WindowID` consumers through resolver helpers.
6. Changed `Window::Destroy()` to delete only matching owned items.
7. Repositioned and refreshed existing call-tip items on reuse.
8. Added focused call-tip, list-box, stale deletion, recreate, and owner-mismatch
   lifecycle tests.

Broader wrapper-type refactoring remains a future Phase 3 option. It should not
be started as a cleanup-only change without a fresh plan and performance-neutral
validation.

## Testing Strategy

Focused tests should cover observable lifetime behavior rather than private
implementation details.

- Main editor safety: platform-window destroy/reset paths must not delete the
  `ScintillaQuick_item`.
- Call-tip ownership: create, destroy, externally delete, and recreate call tips
  without stale geometry or dangling access.
- Call-tip reuse: open two call tips at different positions/sizes and verify the
  existing item is repositioned, resized, updated, and rendered at the new
  location.
- List-box ownership: create and destroy autocomplete/list-box UI repeatedly.
- Stale Qt deletion: delete an owned call-tip or list-box item through Qt parent
  cleanup, then exercise show, position, invalidation, cursor, monitor-rect,
  destroy, and list-box access paths. They must no-op through null/tombstoned
  resolution or use a cleared `wid`, never a dangling pointer.
- Regression gate: run the CI-compatible correctness subset from
  `docs/review_remediation_plan.md`.

This ownership policy is not a renderer hot path. Renderer benchmarks are not
required unless a future change also touches render update scheduling, repeated
popup creation in benchmark scenarios, or scene-graph rendering.
