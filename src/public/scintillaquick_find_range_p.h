// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QByteArrayView>

#include <cstdint>
#include <ScintillaTypes.h>

class ScintillaQuick_item;

namespace scintillaquick
{

struct match_range_t
{
    Scintilla::Position start = -1;
    Scintilla::Position end   = -1;
};

// Uses native target/search state, leaving the matched target available for
// replacement. The caller owns range bounds and search policy independently
// of any find-panel options or selection state.
match_range_t search_range(
    ScintillaQuick_item& editor,
    QByteArrayView needle,
    Scintilla::Position start,
    Scintilla::Position end,
    int flags);

} // namespace scintillaquick
