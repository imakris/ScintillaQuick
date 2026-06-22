// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#ifndef TORTOISEDIFF_MERGE_MODEL_H
#define TORTOISEDIFF_MERGE_MODEL_H

#include <algorithm>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

enum class MergeSide
{
    Left,
    Right
};

enum class MergeCellKind
{
    Gap,
    Real
};

enum class MergeCellState
{
    Equal,
    Changed,
    Edited
};

struct MergeCell
{
    QString text;
    int sourceLine = -1;
    MergeCellKind kind = MergeCellKind::Gap;
    MergeCellState state = MergeCellState::Equal;
};

struct MergeRow
{
    int hunkId = 0;
    int changedGroupId = -1;
    MergeCell left;
    MergeCell right;
};

struct MergeModel
{
    std::vector<MergeRow> rows;
};

struct MergePosition
{
    int row = 0;
    qsizetype column = 0;
};

inline MergeCell real_merge_cell(QString text, int source_line, MergeCellState state = MergeCellState::Equal)
{
    return {std::move(text), source_line, MergeCellKind::Real, state};
}

inline MergeCell gap_merge_cell(MergeCellState state = MergeCellState::Equal)
{
    return {{}, -1, MergeCellKind::Gap, state};
}

inline MergeCell& merge_cell_for_side(MergeRow& row, MergeSide side)
{
    return side == MergeSide::Left ? row.left : row.right;
}

inline const MergeCell& merge_cell_for_side(const MergeRow& row, MergeSide side)
{
    return side == MergeSide::Left ? row.left : row.right;
}

inline bool merge_cell_is_gap_empty(const MergeCell& cell)
{
    return cell.kind == MergeCellKind::Gap && cell.text.isEmpty();
}

inline bool merge_cell_is_removable_empty(const MergeCell& cell)
{
    return merge_cell_is_gap_empty(cell);
}

inline void remove_empty_gap_rows(MergeModel& model)
{
    std::erase_if(model.rows, [](const MergeRow& row) {
        return merge_cell_is_removable_empty(row.left) && merge_cell_is_removable_empty(row.right);
    });
}

inline bool delete_selection(
    MergeModel& model, MergeSide target_side, MergePosition first, MergePosition last, bool compact_empty_rows = true)
{
    if (first.row > last.row || (first.row == last.row && first.column > last.column)) {
        std::swap(first, last);
    }
    if (first.row < 0 || last.row < 0 || first.row >= static_cast<int>(model.rows.size()) ||
        last.row >= static_cast<int>(model.rows.size()))
    {
        return false;
    }

    MergeCell& first_cell = merge_cell_for_side(model.rows[static_cast<size_t>(first.row)], target_side);
    MergeCell& last_cell = merge_cell_for_side(model.rows[static_cast<size_t>(last.row)], target_side);
    first.column = std::clamp(first.column, qsizetype{0}, first_cell.text.size());
    last.column = std::clamp(last.column, qsizetype{0}, last_cell.text.size());
    if (first.row == last.row && first.column == last.column) {
        return false;
    }

    if (first.row == last.row) {
        first_cell.text.remove(first.column, last.column - first.column);
        first_cell.kind = first_cell.text.isEmpty() ? first_cell.kind : MergeCellKind::Real;
        first_cell.state = MergeCellState::Edited;
        if (compact_empty_rows) {
            remove_empty_gap_rows(model);
        }
        return true;
    }

    first_cell.text = first_cell.text.left(first.column) + last_cell.text.mid(last.column);
    first_cell.kind = first_cell.text.isEmpty() && first_cell.sourceLine == -1 ? MergeCellKind::Gap : MergeCellKind::Real;
    first_cell.state = MergeCellState::Edited;

    for (int row = first.row + 1; row <= last.row; ++row) {
        merge_cell_for_side(model.rows[static_cast<size_t>(row)], target_side) =
            gap_merge_cell(MergeCellState::Edited);
    }

    if (compact_empty_rows) {
        remove_empty_gap_rows(model);
    }
    return true;
}

inline bool replace_selection(
    MergeModel& model, MergeSide target_side, MergePosition first, MergePosition last, QString replacement_text)
{
    if (first.row > last.row || (first.row == last.row && first.column > last.column)) {
        std::swap(first, last);
    }
    if (first.row < 0 || last.row < 0 || first.row >= static_cast<int>(model.rows.size()) ||
        last.row >= static_cast<int>(model.rows.size()))
    {
        return false;
    }
    first.column = std::clamp(first.column, qsizetype{0},
        merge_cell_for_side(model.rows[static_cast<size_t>(first.row)], target_side).text.size());
    last.column = std::clamp(last.column, qsizetype{0},
        merge_cell_for_side(model.rows[static_cast<size_t>(last.row)], target_side).text.size());
    const MergePosition insert_at = first;
    if (!delete_selection(model, target_side, first, last, false)) {
        return false;
    }

    replacement_text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    replacement_text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (replacement_text.isEmpty()) {
        remove_empty_gap_rows(model);
        return true;
    }

    const int insert_row = std::clamp(insert_at.row, 0, static_cast<int>(model.rows.size()));
    if (insert_row == static_cast<int>(model.rows.size())) {
        model.rows.push_back({0, -1, gap_merge_cell(), gap_merge_cell()});
    }

    MergeRow& row = model.rows[static_cast<size_t>(insert_row)];
    MergeCell& cell = merge_cell_for_side(row, target_side);
    if (cell.kind == MergeCellKind::Gap) {
        cell = real_merge_cell(QString(), -1, MergeCellState::Edited);
    }

    const qsizetype column = std::clamp(insert_at.column, qsizetype{0}, cell.text.size());
    const QString prefix = cell.text.left(column);
    const QString suffix = cell.text.mid(column);
    const QStringList lines = replacement_text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    if (lines.size() == 1) {
        cell.text = prefix + replacement_text + suffix;
        cell.kind = MergeCellKind::Real;
        cell.state = MergeCellState::Edited;
        return true;
    }

    cell.text = prefix + lines.first();
    cell.kind = MergeCellKind::Real;
    cell.state = MergeCellState::Edited;

    std::vector<MergeRow> inserted_rows;
    inserted_rows.reserve(static_cast<size_t>(lines.size() - 1));
    for (qsizetype line = 1; line < lines.size(); ++line) {
        const QString text = lines[line] + (line == lines.size() - 1 ? suffix : QString());
        MergeRow inserted_row{row.hunkId, row.changedGroupId, gap_merge_cell(), gap_merge_cell()};
        merge_cell_for_side(inserted_row, target_side) = real_merge_cell(text, -1, MergeCellState::Edited);
        inserted_rows.push_back(std::move(inserted_row));
    }
    model.rows.insert(model.rows.begin() + insert_row + 1, inserted_rows.begin(), inserted_rows.end());
    remove_empty_gap_rows(model);
    return true;
}

#endif
