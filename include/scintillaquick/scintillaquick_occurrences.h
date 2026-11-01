// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include "scintillaquick_item.h"

#include <QByteArray>
#include <QColor>
#include <QObject>

#include <memory>

/** One transient literal whole-word query for a shared native document.
 * The document owner and this object live on the same thread. Hosts construct
 * one instance per document and configure indicator 8 on each attached view.
 * Text edits end the query; closing an individual view does not affect it.
 */
class SCINTILLAQUICK_EXPORT ScintillaQuick_occurrences final : public QObject
{
    Q_OBJECT

public:
    explicit ScintillaQuick_occurrences(
        ScintillaQuick_item* document_owner, QObject* parent = nullptr);
    ~ScintillaQuick_occurrences() override;

    // Call after Scintilla's normal double-click selection. Punctuation,
    // whitespace and selections exceeding 4096 UTF-8 bytes end the query.
    void highlight_selected_word(ScintillaQuick_item* view);
    void clear();
    bool active() const;

    // At most 8192 normalized logical-line buckets, populated incrementally.
    // Bucket floor(line * size / line_count) is nonzero when a match exists.
    QByteArray occurrence_rows() const;
    static void configure_view(ScintillaQuick_item* view, const QColor& color);

signals:
    void occurrence_rows_changed();

private:
    void process_next_batch();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
