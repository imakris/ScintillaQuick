#pragma once

// This header exists only for backward source compatibility with code
// written against the pre-0.2 layout, which put the version constants
// in `namespace scintilla_quick` under `include/scintilla_quick/`.
//
// The canonical location is now `namespace scintillaquick` in
// <scintillaquick/ScintillaQuickItem.h>. New code should include that
// header directly and use the new namespace name.
//
// This shim delegates to the new constants instead of duplicating them,
// so the two namespaces can never drift apart.

#include <scintillaquick/ScintillaQuickItem.h>

namespace scintilla_quick {

// Deprecated: use `scintillaquick::version_*` instead.
inline constexpr int version_major = scintillaquick::version_major;
inline constexpr int version_minor = scintillaquick::version_minor;
inline constexpr int version_patch = scintillaquick::version_patch;

} // namespace scintilla_quick
