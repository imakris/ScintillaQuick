// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QChar>
#include <QString>
#include <QStringList>
#include <QStringLiteral>

// Deterministic synthetic documents shared by tests and benchmarks. The
// per-line formatting (including the modulo-based value field) is part
// of the public contract of these builders: visual-regression baselines
// and benchmark scenarios depend on the exact byte layout.

inline QString build_large_document(int line_count)
{
    QStringList lines;
    lines.reserve(line_count);
    for (int i = 0; i < line_count; ++i) {
        lines << QStringLiteral("Line %1: The quick brown fox jumps over the lazy dog. value=%2")
                     .arg(i + 1, 6, 10, QChar('0'))
                     .arg((i * 17) % 9973);
    }
    return lines.join('\n');
}

inline QString build_wrapped_document(int line_count)
{
    QStringList lines;
    lines.reserve(line_count);
    for (int i = 0; i < line_count; ++i) {
        lines << QStringLiteral(
            "Wrapped line %1: The quick brown fox jumps over the lazy dog "
            "and keeps running until the text spans multiple wrapped sublines "
            "at the narrow visual-regression width %2.")
                     .arg(i + 1, 5, 10, QChar('0'))
                     .arg((i * 13) % 101);
    }
    return lines.join('\n');
}
