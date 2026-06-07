# Test-Access API Boundary

This document covers Package 2E: cleanup of test-only declarations that currently
leak through the installed `scintillaquick_item.h` header.

## Current State

The installed public header `include/scintillaquick/scintillaquick_item.h`
contains test-support surface under `Scintilla::Internal`:

- `Displayed_row_for_test` is declared unconditionally.
- `ScintillaQuick_validation_access` is forward-declared when
  `SCINTILLAQUICK_ENABLE_TEST_ACCESS` is defined.
- `ScintillaQuick_item` grants friendship to
  `Scintilla::Internal::ScintillaQuick_validation_access` when the same macro is
  defined.
- Private methods `displayed_rows_for_test()` and
  `rendered_frame_for_test()` are declared in `ScintillaQuick_item`.

In-tree validation tests include `tests/support/scintillaquick_validation_access.h`,
which defines `SCINTILLAQUICK_ENABLE_TEST_ACCESS` before including the public
header, then accesses private editor state through the friend hook. The frame
validation and review-capture tests use that support header. The embedded
benchmark currently defines a local `ScintillaQuick_validation_access` class and
calls `displayed_rows_for_test()` directly.

The public install step copies `include/` as installed headers. It deliberately
does not install `third_party/scintilla/src`, `src/core`, `src/render`, or
`tests/support`, so downstream consumers see the test-oriented declarations but
cannot use the in-tree validation helper.

## Risks

This is not an urgent runtime correctness issue, but it is an API-boundary
problem:

- Installed consumers can discover and depend on `Displayed_row_for_test`, even
  though it is explicitly not intended as long-term public API.
- Private implementation concepts (`Render_frame`, displayed rows, validation
  capture) are visible in the public class declaration and namespace.
- Removing or moving these names can break source builds for any downstream code
  that already used the unsupported hooks.
- Gating class members differently for the library build and consumer builds can
  create subtle C++ One Definition Rule risk. Friend-only test hooks are already
  macro-gated; adding more macro-dependent member declarations would increase
  that risk.
- The current benchmark has a second local validation-access helper instead of
  using the shared test-support helper, so cleanup must account for tests and
  benchmarks.

Because Package 2C already changed the public header and Qt meta-object surface,
this package should not immediately add another public-header cleanup unless the
maintainer explicitly accepts a source-compatibility cleanup window.

## Options

### Option A: Keep Current Surface For Now

Leave `Displayed_row_for_test` and the private test methods in the public header,
but keep all usage in in-tree tests and benchmarks.

Pros:
- No compatibility risk.
- No test-build churn.
- No chance of accidentally breaking frame validation or benchmarks.

Cons:
- The installed header continues to expose unsupported test concepts.
- Future consumers may keep discovering the unsupported surface.

### Option B: Macro-Gate More Test Declarations

Move `Displayed_row_for_test` and test-only private methods behind
`SCINTILLAQUICK_ENABLE_TEST_ACCESS` or a stronger internal macro.

Pros:
- Reduces accidental discovery for ordinary consumers.
- Small apparent code change.

Cons:
- Risky if the library target and consumer targets preprocess different
  definitions of `ScintillaQuick_item`.
- Consumers can still define the macro manually.
- Does not fully solve the installed-header boundary.

This option is not recommended as a standalone implementation.

### Option C: Move Test Row Model To Test Support

Move the displayed-row DTO and row reconstruction into
`tests/support/scintillaquick_validation_access.h` or a new non-installed test
support header. Tests and benchmarks would access cached `Render_frame` through
one shared helper, then derive displayed rows outside the installed public API.

Pros:
- Removes the most visible test DTO from the installed public header.
- Centralizes tests and benchmarks on one validation-access helper.
- Lower risk than a full private-header split.

Cons:
- Still leaves some friend/test access mechanism in the public class until a
  larger private-header cleanup exists.
- Requires benchmark migration and build validation.
- Source-breaking for consumers using `Displayed_row_for_test`.

This is the best first cleanup if the project accepts a pre-1.0 source cleanup.

### Option D: Full Private Test-Access Header

Create a non-installed private test-access boundary that exposes render snapshots
and raster oracle helpers without any test names in the installed public header.

Pros:
- Cleanest long-term public API.
- Tests access internals through an intentional non-installed boundary.

Cons:
- Requires a broader private-header/PIMPL-style split or another careful access
  mechanism.
- Higher risk than Package 2E should take immediately.
- Should be coordinated with any future PIMPL/public-header cleanup.

This is a long-term target, not the next implementation step.

## Recommendation

Defer Package 2E implementation for now. The current leakage is documented and
unsupported, but it is not causing a correctness or performance failure. The
safe path is to finish design review, then schedule code cleanup in a deliberate
API-boundary window.

When implementation is scheduled, use Option C as the first code step:

1. Centralize benchmark and validation-test access through one non-installed
   support helper.
2. Move the displayed-row test DTO out of the installed public header.
3. Keep legacy private snapshot hooks only as long as needed by the in-tree
   validation helper.
4. Defer full removal of all test hooks from `scintillaquick_item.h` until a
   larger private-header or PIMPL cleanup.

Do not implement Option B alone. Macro-gating more class members without a
broader header strategy is more likely to create confusing build behavior than a
clean API boundary.

## Future Implementation Scope

Expected write scope for the first implementation package:

- `include/scintillaquick/scintillaquick_item.h`
- `tests/support/scintillaquick_validation_access.h`
- `benchmarks/embedded_editor/main.cpp`
- `tests/CMakeLists.txt` if include paths or helper ownership change
- Top-level `CMakeLists.txt` only if benchmark include paths or install behavior
  must change
- `docs/test_access_api_boundary.md` and possibly release notes/docs describing
  the unsupported test API cleanup

Avoid touching renderer, invalidation, notification API, platform ownership, or
Scintilla message dispatch in the same package.

## Migration Steps

1. Add or extend a non-installed validation-access helper that owns all
   displayed-row support needed by tests and benchmarks.
2. Change `benchmarks/embedded_editor/main.cpp` to use the shared helper instead
   of defining a local `ScintillaQuick_validation_access` class.
3. Reconstruct visible rows from `Render_frame` in test support, then remove
   direct benchmark dependency on `displayed_rows_for_test()`.
4. Remove `Displayed_row_for_test` from the installed public header only after
   reviewers accept the source-compatibility impact.
5. If the private `displayed_rows_for_test()` member becomes unused, remove its
   declaration and implementation in the same package.
6. Keep `rendered_frame_for_test()` until a later private-header strategy can
   replace it safely.
7. Document the cleanup in release notes as removal of unsupported test-only
   surface, not as a public API feature change.

## Test Access After Cleanup

In-tree tests should include only non-installed support headers such as
`tests/support/scintillaquick_validation_access.h`. They may include internal
headers through build-interface include directories, because those headers are
not part of the installed consumer surface.

Benchmarks should follow the same rule. They should not define their own
validation-access class in the benchmark source file.

Installed consumers should use public Scintilla messages, QML properties,
signals, and documented API only. They should not rely on render-frame or
displayed-row internals.

## Install And Consumer-Build Considerations

Before any implementation removes or hides names from `scintillaquick_item.h`,
run an install/consumer smoke test:

1. Configure and install ScintillaQuick to a temporary prefix.
2. Configure a separate minimal consumer project with
   `find_package(ScintillaQuick CONFIG REQUIRED)`.
3. Include `<scintillaquick/scintillaquick_item.h>`.
4. Instantiate `ScintillaQuick_item`, call a public `send()` query, and link
   against `ScintillaQuick::ScintillaQuick`.
5. Verify the consumer does not need `tests/support`, `src/core`, `src/render`,
   or `third_party/scintilla/src`.

Also verify that the installed header no longer exposes any new replacement
test DTO as public API.

## Review Gates

Future implementation needs at least two green reviews:

- API/build reviewer: checks public-header compatibility, install behavior, and
  absence of accidental macro-dependent class member layouts.
- Test/benchmark reviewer: checks that frame validation, review capture, and
  embedded benchmark still access internals through the shared non-installed
  helper.

A third review is recommended if implementation touches CMake install/export
logic.

## Validation Gates

Required validation for future implementation:

- Static MinGW configure/build.
- Shared MinGW configure/build.
- CI-compatible CTest subset excluding visual regression and embedded benchmark
  only when appropriate for the local runner.
- Targeted frame-validation test.
- Embedded benchmark build, plus a small benchmark smoke if benchmark access
  code changes.
- Install/consumer smoke test from a temporary install prefix.
- `git diff --check`.
- Local markdown link and trailing-whitespace checks for changed docs.

Linux visual baselines are not required for this API-boundary cleanup unless
render-frame capture behavior changes. They remain required for renderer or
visual-output changes.

## Non-Goals

This package should not:

- Introduce PIMPL by itself.
- Rework `Render_frame`.
- Change validation fixture semantics.
- Change renderer, invalidation, or scrolling behavior.
- Make test-access helpers part of the installed package.
- Add a supported public displayed-row or render-frame API.
