# Notification API Compatibility

This note started as the Package 2C design gate and now records the implemented
safe-notification compatibility contract. It covers notification signal
behavior, compatibility risks, and the review gates for future changes to this
public API.

## Source Facts Verified

- `ScintillaQuick_core::notifyParent(Scintilla::NotificationData)` is emitted by
  value from the core and connected to `ScintillaQuick_item::notifyParent(...)`.
  See `src/core/scintillaquick_core.h` and
  `src/public/scintillaquick_item.cpp`.
- `ScintillaQuick_item::notifyParent(NotificationData scn)` currently emits
  `notify(&scn)`. The pointer refers to the stack-local slot parameter and is
  valid only while the direct synchronous signal delivery is executing.
- The public header exposes `void notify(Scintilla::NotificationData* pscn)`.
  That signal is not safe for queued Qt connections, delayed use, or receivers
  that store the pointer.
- The first-wave fix changed `modified(...)` emission to build an owning
  `QByteArray` with `QByteArray(scn.text, scn.length)` before emitting the
  signal. If `modified(...)` is successfully delivered through a queued
  connection, its `QByteArray` text payload is owned rather than a
  `QByteArray::fromRawData(...)` wrapper.
- There is no repository-local `qRegisterMetaType` or `Q_DECLARE_METATYPE`
  coverage for `Scintilla::NotificationData` in the inspected files. A new
  queued-safe custom notification value type must include explicit metatype
  handling.

## Current Lifetime Contract

The existing `notify(NotificationData*)` signal is a direct callback hook, not an
owned notification object.

Required documentation wording for the existing signal:

```text
The pointer passed to notify(Scintilla::NotificationData*) is valid only during
direct synchronous signal delivery. Do not store it, use it from a queued
connection, or retain pointers inside the notification payload.
```

The pointer signal also exposes Scintilla payload fields such as `text` as raw
pointers. Copying only the `NotificationData` struct is not enough to make a
safe API, because the copied struct would still contain transient pointers.

## Compatibility Risk Classification

| Change | Compatibility Risk | Notes |
| --- | --- | --- |
| Remove `notify(NotificationData*)` | High | Source, binary, and QML-facing behavior can break existing users. Do not do this outside an explicit breaking-change release. |
| Change `notify(NotificationData*)` signature | High | Existing signal connections and generated meta-object expectations break. |
| Mark `notify(NotificationData*)` deprecated | Medium | Source remains compatible, but downstream builds may fail with warnings-as-errors. Delay until a replacement has shipped with migration docs. |
| Add a new signal with an owned snapshot type | Low to medium | Public API expands and meta-object data changes. Object layout should not change if no new data members are added, but implementation must follow the locked type, signal, and QML policy below. |
| Add a new public value type | Medium | Requires naming, exported headers, metatype registration, documentation, and ABI policy review. |
| Expose a generic `QVariantMap` notification signal for QML | Medium | QML-friendly, but weakly typed and easy to turn into an unstable contract. Use only if generic QML notification handling is a committed goal. |
| Copy notification text for every notification unconditionally | Medium performance risk | Large edits can produce large text payloads. Avoid extra copies beyond the bytes already required by typed signals. |

## Recommended Additive API Path

Preserve the existing pointer signal and add a new owned snapshot API using the
Package 2C naming and exposure policy below.

Final C++ API names:

- Snapshot value type: `ScintillaQuick_notification`
- Safe signal: `notificationReceived`

These names follow the existing exported type style in the public header, such
as `ScintillaQuick_item`, while keeping the signal in the existing lowerCamelCase
Qt signal style.

Final C++ signal shape:

```cpp
void notificationReceived(const ScintillaQuick_notification& notification);
```

Use the exact spelling above for Package 2C. Do not use the alternate
`ScintillaQuickNotification` name in this implementation package. The
replacement type must be copyable, registered with Qt's metatype system, and
must not expose raw pointers as stable data.

Recommended snapshot invariants:

- Every field in `Scintilla::NotificationData` and
  `Scintilla::NotifyHeader` must be accounted for by the snapshot design. The
  safe snapshot should copy every scalar notification value as a value, not only
  the fields used by the current typed Qt signals.
- Scalar coverage includes `nmhdr.idFrom`, `nmhdr.code`, `position`, `ch`,
  `modifiers`, `modificationType`, `length`, `linesAdded`, `message`,
  `wParam`, `line`, `foldLevelNow`, `foldLevelPrev`, `margin`, `listType`,
  `x`, `y`, `token`, `annotationLinesAdded`, `updated`,
  `listCompletionMethod`, and `characterSource`. `lParam` is exposed through
  `lParamKind` and `lParamValue`: `lParamValue` is meaningful only when
  `lParamKind` is `Numeric`, and that is set only when the notification or
  recorded-message contract defines `lParam` as numeric. Pointer-bearing
  contracts use owned payload fields such as `lParamText` instead; see the
  `SCN_MACRORECORD` rules below.
- For `SCN_AUTOCSELECTION`, `SCN_AUTOCCOMPLETED`,
  `SCN_AUTOCSELECTIONCHANGE`, and `SCN_USERLISTSELECTION`, Scintilla uses
  `lParam` as the numeric autocomplete/list start position. These notification
  codes should set `lParamKind` to `Numeric` and copy the value to
  `lParamValue`.
- Pointer payloads must not be stored as persistent raw pointers. `text` must be
  converted to owned storage according to the notification-code-specific rules
  below. `nmhdr.hwndFrom` must be omitted or represented only as an explicitly
  reviewed inert handle value that receivers must not dereference.
- For `SCN_MODIFIED`, copy exactly `scn.length` bytes from `scn.text` into
  owned storage when `scn.text` is non-null and `scn.length` is positive. Do not
  assume NUL termination, and preserve embedded NUL bytes. Store an explicit
  empty owned payload when the pointer is null or the length is zero.
- For string notifications such as `SCN_USERLISTSELECTION`,
  `SCN_AUTOCSELECTION`, `SCN_AUTOCCOMPLETED`,
  `SCN_AUTOCSELECTIONCHANGE`, and `SCN_URIDROPPED`, copy the
  NUL-terminated UTF-8 string with null guards into owned storage. The chosen
  snapshot design may expose that owned payload as a `QByteArray`, an owned
  `QString`, or both, but it must not retain the original raw pointer.
- For notification codes where `text` is not meaningful or is null, store an
  explicit empty or null owned payload. Do not infer a string length from
  unrelated scalar fields.
- `SCN_MACRORECORD` requires message-specific `lParam` handling because some
  recorded Scintilla messages carry transient pointer payloads through
  `lParam`. Decode text payloads according to the recorded message, not by one
  generic string rule:
  - For `SCI_ADDTEXT` and `SCI_APPENDTEXT`, `wParam` is the byte length and
    `lParam` is the text pointer. If `lParam` is non-null, copy exactly
    `wParam` bytes from it, preserving embedded NUL bytes. Apply sane
    bounds and overflow guards before converting `wParam` to the byte count
    used for allocation and copying.
  - For `SCI_REPLACESEL`, `SCI_INSERTTEXT`, `SCI_SEARCHNEXT`,
    `SCI_SEARCHPREV`, and editor-generated newline macro text, copy owned
    NUL-terminated bytes or an owned string with null guards.
  - For recorded messages whose contract defines `lParam` as numeric, set
    `lParamKind` to `Numeric` and copy the numeric value to `lParamValue`.
    Leave `lParamValue` zero and non-authoritative for all non-numeric
    `lParamKind` values.
  - For unknown or ambiguous pointer-like recorded-message payloads, omit
    decoded `lParam` from the queued-safe API. Do not claim queued safety for
    unclassified raw pointer values.
- The snapshot type should not contain `QQuickItem*`, Scintilla window handles,
  or raw Scintilla-owned pointers as persistent fields.
- If a view of `Scintilla::NotificationData` is needed for C++ migration, expose
  it as a temporary view method with documented lifetime, not as the stored data
  model.

Emission-order guidance:

- Preserve the existing behavior and ordering for all existing signals.
- Exact implementation algorithm for an additive safe signal:
  1. Build the owned snapshot from the original `scn` before emitting any
     signal, including copying any `text` payload and any classified
     `SCN_MACRORECORD` `lParam` payload with the notification-code-specific
     rules above.
  2. Emit the legacy `notify(&scn)` first, exactly where it is emitted today.
  3. Emit the new safe signal from the pre-copied snapshot.
  4. Run the existing typed-signal switch in today's order.
- If legacy direct receivers mutate the non-const `NotificationData*`, the
  existing typed signals should continue to observe the same behavior they
  observe today. The new safe signal must use the pre-copied snapshot and not
  data mutated during legacy `notify(...)` delivery.
- The existing `macroRecord(message, wParam, lParam)` signal remains a legacy
  direct callback for pointer-bearing macro messages. For messages whose
  `lParam` is a transient text pointer, queued consumers must use the owned
  snapshot's `lParamText` payload instead of retaining the raw legacy
  `lParam` value.
- Any intentional change to the typed-signal order or legacy mutation semantics
  requires a separate reviewed plan.

Performance guidance:

- Do not introduce a second large text allocation when `modified(...)` already
  needs owned bytes. Share the same `QByteArray` storage through Qt implicit
  sharing where possible.
- Avoid expensive snapshot text or macro payload copies when no safe snapshot
  receiver exists, unless the same bytes are already needed for a typed signal.
- Add a large-insert smoke or micro-benchmark check if implementation changes
  notification copy behavior for edit-heavy paths.

## QML-Facing Policy

The current generic pointer signal is not a suitable QML API. QML users should
prefer the existing typed signals such as `modified(...)`, `charAdded(...)`,
`updateUi(...)`, and `marginClicked(...)`.

Package 2C policy is C++ queued-safe API only. Because
`ScintillaQuick_item` is registered as a QML type, the new public Qt signal will
be present in the item's meta-object and may be visible to QML tooling. That
meta-object visibility is not a supported generic QML notification contract for
Package 2C.

Implementation constraints:

- Register `ScintillaQuick_notification` with Qt's metatype system for C++
  queued delivery, but do not register it as a QML value type.
- Do not add `Q_GADGET`, `QML_VALUE_TYPE`, `QML_ELEMENT`, QML properties, or a
  `QVariantMap` wrapper for `ScintillaQuick_notification` in Package 2C.
- Do not document or add examples that consume `notificationReceived(...)` from
  QML.
- Treat any QML access to the signal argument as unsupported and opaque. QML
  consumers have no stable property names, keys, byte/string conversion rules,
  or numeric-field semantics for this snapshot type.
- Keep typed signals as the supported QML notification surface.

Required QML gate for Package 2C implementation:

- Add or run a minimal QML smoke test proving `ScintillaQuick_item` still loads
  and existing typed signals remain usable after the meta-object change.
- Do not require a QML test that inspects `ScintillaQuick_notification`, because
  that is deliberately not a QML value type in Package 2C.

Any future generic QML notification API requires a separate reviewed design. If
that future design chooses `QVariantMap`, a QML value type, or additional string
views of byte payloads, it must document those keys/properties as stable API and
add QML tests for them.

## Implementation Phases

1. Documentation-only closure:
   - Document the direct-only lifetime of `notify(NotificationData*)`.
   - Keep the signal signature unchanged.
   - Confirm examples and docs do not imply pointer retention is safe.

2. API implementation review:
   - Confirm the implementation uses `ScintillaQuick_notification` and
     `notificationReceived(const ScintillaQuick_notification&)` exactly.
   - Confirm the initial replacement remains C++ queued-safe only, with no
     supported generic QML notification contract beyond unavoidable Qt
     meta-object visibility.
   - Confirm the public header location, export macro needs, metatype
     registration, and module versioning story.
   - Require any new signal to be appended after existing signals in the public
     header. Do not insert it before existing signals.
   - Verify or add metatype registration for every non-Qt parameter type used
     by queued `modified(...)` delivery, including Scintilla typedefs and enum
     classes such as `Scintilla::Position`,
     `Scintilla::ModificationFlags`, and `Scintilla::FoldLevel`.
   - Confirm no public class data members are added.
   - Include both static and shared library builds in validation after any
     public header or meta-object change.

3. Additive C++ implementation:
   - Add the copyable owned snapshot type.
   - Add the new signal without changing or removing the pointer signal.
   - Register the new snapshot type for queued connections and keep the
     existing `modified(...)` queued-delivery metatype requirements covered.
   - Build the owned snapshot once and share text or classified macro-record
     payload storage with existing typed signals where possible.

4. Tests:
   - Add a queued-connection test for `modified(...)` that fails visibly if any
     existing `modified(...)` parameter metatype is missing, then proves
     retained text is still valid after signal delivery.
   - Add direct and queued tests for the new safe notification signal with a
     text-bearing `SCN_MODIFIED` notification.
   - Add a queued safe-snapshot test proving `SCN_MODIFIED` preserves embedded
     NUL bytes by copying exactly `scn.length` bytes, and that retained bytes
     remain valid after the original notification scope ends.
   - Add a queued safe-snapshot test for at least one non-modified string
     notification, such as `SCN_AUTOCSELECTION` or `SCN_URIDROPPED`, proving the
     owned bytes or string remains valid after the original notification scope
     ends.
   - Add a queued safe-snapshot test for macro recording `SCI_REPLACESEL`,
     proving retained macro text copied from `SCN_MACRORECORD` `lParam` remains
     valid after the original notification scope ends.
   - Add a queued safe-snapshot test for macro recording `SCI_ADDTEXT` or
     `SCI_APPENDTEXT` with embedded NUL bytes, proving the implementation copies
     exactly the `wParam` byte count from `SCN_MACRORECORD` `lParam`.
   - Add a queued safe-snapshot test for macro recording `SCI_SEARCHNEXT` or
     `SCI_SEARCHPREV`, proving retained search text copied from
     `SCN_MACRORECORD` `lParam` remains valid after the original notification
     scope ends.
   - Add a non-text notification test to confirm scalar fields survive queued
     delivery.
   - Add metatype/queued-connection coverage so missing type registration fails
     visibly.
   - Add or run a minimal QML smoke test proving the item still loads and
     existing typed QML signals remain usable after the new public signal is
     appended.

5. Migration and deprecation:
   - Update docs and examples to prefer the safe API.
   - Keep `notify(NotificationData*)` available for at least one compatibility
     release after the safe API ships.
   - Consider deprecation only after downstream users have a documented
     migration path. Removal requires an explicit major-version or
     breaking-change decision.

## Go/No-Go Criteria Before Public Header Changes

Go only when all of these are true:

- The public API uses the final Package 2C names:
  `ScintillaQuick_notification` and
  `notificationReceived(const ScintillaQuick_notification&)`.
- The implementation follows the Package 2C QML policy: C++ queued-safe API
  only, with no QML value type, `QVariantMap`, or documented generic QML
  notification contract.
- The design preserves `notify(NotificationData*)` source compatibility.
- Any new signal is appended after existing public signals, not inserted before
  them, and no public class data members are added.
- The snapshot type owns all text bytes using notification-code-specific copy
  rules, classifies or omits pointer-like `SCN_MACRORECORD` `lParam` payloads,
  and contains no persistent raw Scintilla-owned pointers.
- Queued-connection tests are specified for both `modified(...)` and the new
  safe signal, including metatype coverage for all non-Qt parameters used by
  the existing `modified(...)` signal.
- The implementation plan avoids extra large text copies on edit-heavy paths or
  includes a benchmark gate for the added cost.
- Static and shared library builds are included in the validation plan.

No-go if any of these are true:

- The proposed replacement requires changing or removing the existing pointer
  signal in a non-breaking release.
- The safe value type cannot be registered for queued Qt delivery.
- The implementation registers or documents `ScintillaQuick_notification` as a
  QML value type, adds a `QVariantMap` generic notification signal, or otherwise
  exposes a generic QML notification contract without a separate approved QML
  API design and tests.
- The design stores raw `NotificationData::text` pointers or other transient
  Scintilla-owned pointers, including unclassified `SCN_MACRORECORD` `lParam`
  pointers, as persistent snapshot data.
- Reviewers cannot agree whether deprecation is appropriate for the current
  release.

## Open Questions

- What are the project's compatibility promises for public Qt signals between
  releases?
- Should the legacy pointer signal remain indefinitely as an expert C++ hook, or
  be deprecated after the safe API has shipped?
- Are there downstream users that intentionally mutate the
  `NotificationData*` from `notify(...)` before the typed signal switch runs?
