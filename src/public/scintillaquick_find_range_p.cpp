// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_find_range_p.h"

#include <scintillaquick/scintillaquick_item.h>

namespace scintillaquick
{

match_range_t search_range(
    ScintillaQuick_item& editor,
    QByteArrayView needle,
    Scintilla::Position start,
    Scintilla::Position end,
    int flags)
{
    editor.send(SCI_SETTARGETRANGE, start, end);
    editor.send(SCI_SETSEARCHFLAGS, (Scintilla::uptr_t)flags);
    const auto found = (Scintilla::Position)editor.sends(
        SCI_SEARCHINTARGET, (Scintilla::uptr_t)needle.size(), needle.data());
    if (found < 0) {
        return {};
    }
    return {found, (Scintilla::Position)editor.send(SCI_GETTARGETEND)};
}

} // namespace scintillaquick
