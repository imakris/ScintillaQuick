// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QByteArrayView>
#include <QStringDecoder>

namespace scintillaquick
{

// Scintilla supplies document text, never a file signature. Preserve U+FEFF
// at the start of a body, selection, or rendered text segment.
inline QString decode_utf8_body(QByteArrayView bytes)
{
    QStringDecoder decoder(
        QStringDecoder::Utf8,
        QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    return decoder(bytes);
}

} // namespace scintillaquick
