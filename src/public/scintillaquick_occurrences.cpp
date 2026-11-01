// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_occurrences.h>

#include "scintillaquick_find_range_p.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace
{

constexpr int k_occurrence_indicator = 8;
constexpr Scintilla::Position k_maximum_word_bytes = 4096;
constexpr Scintilla::Position k_search_window_bytes = 4096;
constexpr Scintilla::Position k_maximum_row_buckets = 8192;
constexpr int k_batch_operations = 64;
constexpr qint64 k_batch_nanoseconds = 2'000'000;

void set_indicator_range(
    ScintillaQuick_item& editor,
    Scintilla::Position start,
    Scintilla::Position end,
    bool filled)
{
    // The current indicator/value belong to the document, so even a hidden
    // owner must restore them for other consumers, including inline IME.
    const auto previous_indicator = editor.send(SCI_GETINDICATORCURRENT);
    const auto previous_value     = editor.send(SCI_GETINDICATORVALUE);
    const auto binding_revision   = editor.native_binding_revision();
    const QPointer<ScintillaQuick_item> alive(&editor);
    editor.send(SCI_SETINDICATORCURRENT, k_occurrence_indicator);
    editor.send(filled ? SCI_INDICATORFILLRANGE : SCI_INDICATORCLEARRANGE, start, end - start);
    if (alive && alive->native_binding_revision() == binding_revision) {
        alive->send(SCI_SETINDICATORCURRENT, previous_indicator);
        alive->send(SCI_SETINDICATORVALUE, previous_value);
    }
}

QByteArray selected_native_word(ScintillaQuick_item& view)
{
    const auto start  = (Scintilla::Position)view.send(SCI_GETSELECTIONSTART);
    const auto end    = (Scintilla::Position)view.send(SCI_GETSELECTIONEND);
    const auto length = (Scintilla::Position)view.send(SCI_GETLENGTH);
    if (start < 0 || end <= start || end > length || end - start > k_maximum_word_bytes ||
        view.send(SCI_GETSELECTIONS) != 1 || view.send(SCI_SELECTIONISRECTANGLE))
    {
        return {};
    }

    // Validate both native edges before extending a word: an arbitrary short
    // selection inside a giant word must not cause an unbounded native walk.
    // IsRangeWord also admits punctuation; WordEndPosition(true) excludes it.
    if (!view.send(SCI_ISRANGEWORD, start, end) ||
        view.send(SCI_WORDENDPOSITION, start, true) != end)
    {
        return {};
    }

    QByteArray word((qsizetype)(end - start + 1), Qt::Uninitialized);
    Scintilla::TextRangeFull range{{start, end}, word.data()};
    view.send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<Scintilla::sptr_t>(&range));
    word.chop(1);
    return word;
}

} // namespace

struct ScintillaQuick_occurrences::Impl
{
    explicit Impl(ScintillaQuick_item* document_owner)
    :
        m_owner(document_owner),
        m_binding_revision(document_owner->native_binding_revision())
    {
        m_timer.setSingleShot(true);
    }

    QPointer<ScintillaQuick_item> m_owner;
    QTimer m_timer;
    QByteArray m_word;
    QByteArray m_rows;
    std::uint64_t m_binding_revision;
    std::uint64_t m_content_revision = 0;
    std::uint64_t m_generation = 0;
    Scintilla::Position m_length        = 0;
    Scintilla::Position m_line_count    = 0;
    Scintilla::Position m_next_search   = 0;
    Scintilla::Position m_candidate_end = 0;
    Scintilla::Position m_search_end    = 0;
    Scintilla::Position m_next_clear    = 0;
    bool m_has_indicators = false;
    bool m_clearing       = false;
};

ScintillaQuick_occurrences::ScintillaQuick_occurrences(
    ScintillaQuick_item* document_owner, QObject* parent)
:
    QObject(parent),
    m_impl(std::make_unique<Impl>(document_owner))
{
    connect(&m_impl->m_timer, &QTimer::timeout, this, &ScintillaQuick_occurrences::process_next_batch);
    connect(document_owner, &ScintillaQuick_item::textChanged, this, &ScintillaQuick_occurrences::clear);
    connect(document_owner, &QObject::destroyed, this, &ScintillaQuick_occurrences::clear);
}

ScintillaQuick_occurrences::~ScintillaQuick_occurrences() = default;

void ScintillaQuick_occurrences::highlight_selected_word(ScintillaQuick_item* view)
{
    auto& state = *m_impl;
    if (!state.m_owner || !view ||
        view->send(SCI_GETDOCPOINTER) != state.m_owner->send(SCI_GETDOCPOINTER))
    {
        return;
    }

    QByteArray word = selected_native_word(*view);
    if (word.isEmpty()) {
        clear();
        return;
    }

    const auto binding_revision = state.m_owner->native_binding_revision();
    if (state.m_binding_revision != binding_revision) {
        state.m_has_indicators = false;
    }
    ++state.m_generation;
    state.m_binding_revision = binding_revision;
    state.m_content_revision = state.m_owner->native_content_revision();
    state.m_length           = (Scintilla::Position)state.m_owner->send(SCI_GETLENGTH);
    state.m_line_count       = (Scintilla::Position)state.m_owner->send(SCI_GETLINECOUNT);
    state.m_next_search      = 0;
    state.m_candidate_end    = 0;
    state.m_next_clear       = 0;
    state.m_clearing         = state.m_has_indicators;
    state.m_word             = std::move(word);
    state.m_rows             = QByteArray((qsizetype)std::min(state.m_line_count, k_maximum_row_buckets), '\0');
    state.m_timer.start(0);
    emit occurrence_rows_changed();
}

void ScintillaQuick_occurrences::clear()
{
    auto& state = *m_impl;
    ++state.m_generation;
    state.m_timer.stop();
    state.m_word.clear();
    const bool rows_changed = !state.m_rows.isEmpty();
    state.m_rows.clear();
    state.m_next_clear = 0;
    state.m_clearing   = false;

    if (state.m_owner && state.m_owner->native_binding_revision() == state.m_binding_revision) {
        state.m_length           = (Scintilla::Position)state.m_owner->send(SCI_GETLENGTH);
        state.m_content_revision = state.m_owner->native_content_revision();
        state.m_clearing         = state.m_has_indicators;
        if (state.m_clearing) {
            state.m_timer.start(0);
        }
    }
    else {
        state.m_has_indicators = false;
    }
    if (rows_changed) {
        emit occurrence_rows_changed();
    }
}

bool ScintillaQuick_occurrences::active() const
{
    return !m_impl->m_word.isEmpty();
}

QByteArray ScintillaQuick_occurrences::occurrence_rows() const
{
    return m_impl->m_rows;
}

void ScintillaQuick_occurrences::configure_view(ScintillaQuick_item* view, const QColor& color)
{
    const int native_color = color.red() | (color.green() << 8) | (color.blue() << 16);
    view->send(SCI_INDICSETSTYLE,        k_occurrence_indicator, INDIC_ROUNDBOX);
    view->send(SCI_INDICSETFORE,         k_occurrence_indicator, native_color);
    view->send(SCI_INDICSETUNDER,        k_occurrence_indicator, true);
    view->send(SCI_INDICSETALPHA,        k_occurrence_indicator, 48);
    view->send(SCI_INDICSETOUTLINEALPHA, k_occurrence_indicator, 160);
}

void ScintillaQuick_occurrences::process_next_batch()
{
    auto& state = *m_impl;
    if (!state.m_owner || state.m_owner->native_binding_revision() != state.m_binding_revision ||
        state.m_owner->native_content_revision() != state.m_content_revision)
    {
        clear();
        return;
    }

    const QPointer<ScintillaQuick_occurrences> alive(this);
    const auto generation = state.m_generation;
    QElapsedTimer elapsed;
    elapsed.start();
    bool rows_changed = false;

    for (int operation = 0; operation < k_batch_operations; ++operation) {
        auto& owner = *state.m_owner;
        if (state.m_clearing) {
            // Clear one native run per operation, skipping zero runs by their
            // native boundary. A sparse query must not walk every document byte,
            // and clearing a dense query must not remove all runs in one call.
            const auto end = std::min(state.m_length,
                (Scintilla::Position)owner.send(SCI_INDICATOREND,
                    k_occurrence_indicator, state.m_next_clear));
            if (end <= state.m_next_clear) {
                state.m_clearing       = false;
                state.m_has_indicators = false;
            }
            else {
                const auto start = state.m_next_clear;
                state.m_next_clear = end;
                if (owner.send(SCI_INDICATORVALUEAT, k_occurrence_indicator, start)) {
                    set_indicator_range(owner, start, end, false);
                }
            }
        }
        else
        if (!state.m_word.isEmpty() && state.m_next_search < state.m_length) {
            if (state.m_next_search >= state.m_candidate_end) {
                // Ownership is by match start. The following window covers
                // starts at this boundary; overlap admits words crossing it.
                const auto candidate_end = state.m_next_search +
                    std::min(k_search_window_bytes, state.m_length - state.m_next_search);
                state.m_candidate_end = (Scintilla::Position)owner.send(
                    SCI_POSITIONAFTER, candidate_end - 1);
                state.m_search_end = state.m_candidate_end + std::min(
                    (Scintilla::Position)state.m_word.size() - 1,
                    state.m_length - state.m_candidate_end);
            }

            const auto match = scintillaquick::search_range(
                owner, state.m_word, state.m_next_search, state.m_search_end,
                SCFIND_MATCHCASE | SCFIND_WHOLEWORD);
            if (match.start < state.m_next_search || match.start >= state.m_candidate_end ||
                match.end <= match.start || match.end > state.m_length)
            {
                state.m_next_search = state.m_candidate_end;
            }
            else {
                state.m_next_search    = match.end;
                state.m_has_indicators = true;
                set_indicator_range(owner, match.start, match.end, true);
                if (!alive) {
                    return;
                }
                if (state.m_generation != generation) {
                    return;
                }
                if (!state.m_owner || owner.native_content_revision() != state.m_content_revision ||
                    owner.native_binding_revision() != state.m_binding_revision)
                {
                    clear();
                    return;
                }

                const auto line = (std::uint64_t)owner.send(SCI_LINEFROMPOSITION, match.start);
                const auto bucket = (qsizetype)(line * (std::uint64_t)state.m_rows.size() /
                    (std::uint64_t)state.m_line_count);
                if (state.m_rows[bucket] == '\0') {
                    state.m_rows[bucket] = 1;
                    rows_changed = true;
                }
            }
        }
        else {
            break;
        }

        if (!alive) {
            return;
        }
        if (state.m_generation != generation) {
            return;
        }
        if (!state.m_owner || owner.native_content_revision() != state.m_content_revision ||
            owner.native_binding_revision() != state.m_binding_revision)
        {
            clear();
            return;
        }
        if (elapsed.nsecsElapsed() >= k_batch_nanoseconds) {
            break;
        }
    }

    if (state.m_clearing || (!state.m_word.isEmpty() && state.m_next_search < state.m_length)) {
        state.m_timer.start(0);
    }
    if (rows_changed) {
        emit occurrence_rows_changed();
    }
}
