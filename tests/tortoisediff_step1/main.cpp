// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_item.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "Scintilla.h"
#include "scintillaquick_font.h"
#include "scintillaquick_test_macros.h"

namespace
{

int g_failures = 0;
constexpr int line_number_margin = 0;
constexpr int line_number_margin_width = 40;
constexpr int visible_row_tint_y = 48;
constexpr int visible_filler_y = 78;
constexpr int visible_tint_height = 18;
constexpr int visible_filler_line = 12;
constexpr int diff_marker_added = 0;
constexpr int diff_marker_deleted = 1;
constexpr int diff_marker_changed = 2;
constexpr int diff_marker_filler = 3;

enum class Scroll_axis
{
    Horizontal,
    Vertical
};

enum class DiffSideState
{
    Equal,
    Added,
    Deleted,
    Changed,
    Filler
};

enum class DiffSide
{
    Left,
    Right
};

struct DiffRow
{
    int hunkId;
    int changedGroupId;
    int leftSourceLine;
    int rightSourceLine;
    DiffSideState leftState;
    DiffSideState rightState;
};

struct DiffWidgetInput
{
    QString leftText;
    QString rightText;
    std::vector<DiffRow> rows;
};

struct GreenPixelCoverage
{
    int greenPixels = 0;
    int totalPixels = 0;
};

struct VerticalScrollbarModel
{
    int maxFirstVisibleLine = 0;
    double position = 0.0;
    double size = 1.0;
};

struct HorizontalScrollbarModel
{
    bool needed = false;
    int maxXOffset = 0;
    double position = 0.0;
    double size = 1.0;
};

struct HunkRowRange
{
    int startRow = -1;
    int endRow = -1;
};

struct ChangedTextSpan
{
    qsizetype start = 0;
    qsizetype length = 0;
};

struct ChangedTextSpans
{
    ChangedTextSpan left;
    ChangedTextSpan right;
};

enum class WidgetInputValidation
{
    Accepted,
    MismatchedDisplayRowCount,
    InvalidLeftSourceLine,
    InvalidRightSourceLine,
    SourceLineStateMismatch
};

bool operator==(const DiffRow& left, const DiffRow& right)
{
    return left.hunkId == right.hunkId && left.changedGroupId == right.changedGroupId &&
           left.leftSourceLine == right.leftSourceLine && left.rightSourceLine == right.rightSourceLine &&
           left.leftState == right.leftState && left.rightState == right.rightState;
}

void pump_events()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

QString sample_text(const QString& label, int blank_line = -1)
{
    QString text;
    text.reserve(8192);
    for (int line = 1; line <= 80; ++line) {
        if (line != blank_line) {
            text += QStringLiteral("%1 pane line %2: shared scrolling fixture text").arg(label, QString::number(line));
            if (line == 12) {
                text += QStringLiteral(" with a deliberately long unwrapped segment for horizontal scrolling ");
                text += QString(220, QLatin1Char('x'));
            }
        }
        text += QLatin1Char('\n');
    }
    return text;
}

std::vector<DiffRow> changed_block_rows(int hunk_id, int changed_group_id, std::initializer_list<int> left_source_lines,
    std::initializer_list<int> right_source_lines)
{
    const std::vector<int> left_lines(left_source_lines);
    const std::vector<int> right_lines(right_source_lines);
    const size_t row_count = std::max(left_lines.size(), right_lines.size());

    std::vector<DiffRow> rows;
    rows.reserve(row_count);
    for (size_t row = 0; row < row_count; ++row) {
        const bool has_left = row < left_lines.size();
        const bool has_right = row < right_lines.size();
        rows.push_back({
            hunk_id,
            changed_group_id,
            has_left ? left_lines[row] : -1,
            has_right ? right_lines[row] : -1,
            has_left ? (has_right ? DiffSideState::Changed : DiffSideState::Deleted) : DiffSideState::Filler,
            has_right ? (has_left ? DiffSideState::Changed : DiffSideState::Added) : DiffSideState::Filler,
        });
    }

    return rows;
}

QString source_line_text(DiffSide side, int source_line)
{
    return QStringLiteral("%1 source line %2")
        .arg(side == DiffSide::Left ? QStringLiteral("left") : QStringLiteral("right"), QString::number(source_line));
}

QString render_display_text(const std::vector<DiffRow>& rows, DiffSide side)
{
    QStringList display_lines;
    display_lines.reserve(static_cast<qsizetype>(rows.size()));

    for (const DiffRow& row : rows) {
        const int source_line = side == DiffSide::Left ? row.leftSourceLine : row.rightSourceLine;
        display_lines.push_back(source_line == -1 ? QString() : source_line_text(side, source_line));
    }

    return display_lines.join(QLatin1Char('\n'));
}

QStringList display_lines_from_text(const QString& text)
{
    return text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
}

int display_row_count(const QString& text)
{
    return text.count(QLatin1Char('\n')) + 1;
}

bool nearly_equal(double left, double right)
{
    return std::abs(left - right) <= 0.000001;
}

VerticalScrollbarModel vertical_scrollbar_model(int total_rows, int visible_rows, int first_visible_line)
{
    const int bounded_total_rows = std::max(0, total_rows);
    const int bounded_visible_rows = std::max(0, visible_rows);
    const int max_first_visible_line = std::max(0, bounded_total_rows - bounded_visible_rows);
    if (bounded_total_rows == 0 || max_first_visible_line == 0) {
        return {};
    }

    const double size = std::clamp(static_cast<double>(bounded_visible_rows) / bounded_total_rows, 0.0, 1.0);
    const int bounded_first_visible_line = std::clamp(first_visible_line, 0, max_first_visible_line);
    const double position =
        std::clamp(static_cast<double>(bounded_first_visible_line) / bounded_total_rows, 0.0, 1.0 - size);

    return {max_first_visible_line, position, size};
}

int first_visible_line_for_vertical_scrollbar_position(int total_rows, int visible_rows, double position)
{
    const VerticalScrollbarModel top_model = vertical_scrollbar_model(total_rows, visible_rows, 0);
    if (top_model.maxFirstVisibleLine == 0) {
        return 0;
    }

    const int bounded_total_rows = std::max(0, total_rows);
    const double bounded_position = std::clamp(position, 0.0, 1.0 - top_model.size);
    const int first_visible_line = static_cast<int>(std::llround(bounded_position * bounded_total_rows));
    return std::clamp(first_visible_line, 0, top_model.maxFirstVisibleLine);
}

HorizontalScrollbarModel horizontal_scrollbar_model(int content_width, int viewport_width, int x_offset)
{
    const int bounded_content_width = std::max(0, content_width);
    const int bounded_viewport_width = std::max(0, viewport_width);
    const int max_x_offset = std::max(0, bounded_content_width - bounded_viewport_width);
    if (bounded_content_width == 0 || max_x_offset == 0) {
        return {};
    }

    const double size = std::clamp(static_cast<double>(bounded_viewport_width) / bounded_content_width, 0.0, 1.0);
    const int bounded_x_offset = std::clamp(x_offset, 0, max_x_offset);
    const double position =
        std::clamp(static_cast<double>(bounded_x_offset) / bounded_content_width, 0.0, 1.0 - size);

    return {true, max_x_offset, position, size};
}

int x_offset_for_horizontal_scrollbar_position(int content_width, int viewport_width, double position)
{
    const HorizontalScrollbarModel start_model = horizontal_scrollbar_model(content_width, viewport_width, 0);
    if (!start_model.needed) {
        return 0;
    }

    const int bounded_content_width = std::max(0, content_width);
    const double bounded_position = std::clamp(position, 0.0, 1.0 - start_model.size);
    const int x_offset = static_cast<int>(std::llround(bounded_position * bounded_content_width));
    return std::clamp(x_offset, 0, start_model.maxXOffset);
}

HorizontalScrollbarModel horizontal_scrollbar_model_for_panes(
    int left_content_width, int right_content_width, int left_viewport_width, int right_viewport_width, int x_offset)
{
    return horizontal_scrollbar_model(
        std::max(left_content_width, right_content_width), std::min(left_viewport_width, right_viewport_width), x_offset);
}

std::vector<int> hunk_target_rows(const std::vector<DiffRow>& rows)
{
    std::vector<int> target_rows;
    std::vector<int> seen_hunk_ids;

    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const int hunk_id = rows[row_index].hunkId;
        if (hunk_id <= 0 ||
            std::find(seen_hunk_ids.begin(), seen_hunk_ids.end(), hunk_id) != seen_hunk_ids.end())
        {
            continue;
        }

        seen_hunk_ids.push_back(hunk_id);
        target_rows.push_back(static_cast<int>(row_index));
    }

    return target_rows;
}

int hunk_index_for_display_row(const std::vector<DiffRow>& rows, const std::vector<int>& target_rows, int display_row)
{
    if (display_row < 0 || display_row >= static_cast<int>(rows.size())) {
        return -1;
    }

    const int hunk_id = rows[static_cast<size_t>(display_row)].hunkId;
    if (hunk_id <= 0) {
        return -1;
    }

    for (int hunk_index = 0; hunk_index < static_cast<int>(target_rows.size()); ++hunk_index) {
        const int target_row = target_rows[static_cast<size_t>(hunk_index)];
        if (target_row >= 0 && target_row < static_cast<int>(rows.size()) &&
            rows[static_cast<size_t>(target_row)].hunkId == hunk_id)
        {
            return hunk_index;
        }
    }

    return -1;
}

int first_hunk_index_intersecting_display_row_range(
    const std::vector<DiffRow>& rows, const std::vector<int>& target_rows, int first_display_row, int end_display_row)
{
    if (rows.empty()) {
        return -1;
    }

    const int row_count = static_cast<int>(rows.size());
    const int first_row = std::clamp(std::min(first_display_row, end_display_row), 0, row_count);
    const int end_row = std::clamp(std::max(first_display_row, end_display_row), 0, row_count);
    for (int row = first_row; row < end_row; ++row) {
        const int hunk_index = hunk_index_for_display_row(rows, target_rows, row);
        if (hunk_index >= 0) {
            return hunk_index;
        }
    }

    return -1;
}

HunkRowRange hunk_row_range_for_target_index(
    const std::vector<DiffRow>& rows, const std::vector<int>& target_rows, int target_index)
{
    if (target_index < 0 || target_index >= static_cast<int>(target_rows.size())) {
        return {};
    }

    const int target_row = target_rows[static_cast<size_t>(target_index)];
    if (target_row < 0 || target_row >= static_cast<int>(rows.size())) {
        return {};
    }

    const int hunk_id = rows[static_cast<size_t>(target_row)].hunkId;
    if (hunk_id <= 0) {
        return {};
    }

    int start_row = target_row;
    while (start_row > 0 && rows[static_cast<size_t>(start_row - 1)].hunkId == hunk_id) {
        --start_row;
    }

    int end_row = target_row + 1;
    while (end_row < static_cast<int>(rows.size()) && rows[static_cast<size_t>(end_row)].hunkId == hunk_id) {
        ++end_row;
    }

    return {start_row, end_row};
}

HunkRowRange exact_display_row_range(int first_display_row, int end_display_row, int row_count)
{
    if (row_count <= 0) {
        return {};
    }

    int first_row = std::clamp(std::min(first_display_row, end_display_row), 0, row_count);
    int end_row = std::clamp(std::max(first_display_row, end_display_row), 0, row_count);
    if (first_row == end_row && first_row < row_count) {
        ++end_row;
    }
    return first_row < end_row ? HunkRowRange{first_row, end_row} : HunkRowRange{};
}

HunkRowRange changed_block_range_for_display_row(
    const std::vector<DiffRow>& rows, const std::vector<int>& target_rows, int display_row)
{
    const int hunk_index = hunk_index_for_display_row(rows, target_rows, display_row);
    return hunk_index >= 0 ? hunk_row_range_for_target_index(rows, target_rows, hunk_index) :
                             exact_display_row_range(display_row, display_row + 1, static_cast<int>(rows.size()));
}

bool display_row_in_range(int display_row, const HunkRowRange& range)
{
    return display_row >= range.startRow && display_row < range.endRow;
}

double active_hunk_boundary_logical_thickness(double device_pixel_ratio)
{
    constexpr double boundary_physical_pixels = 2.0;
    return boundary_physical_pixels / std::max(1.0, device_pixel_ratio);
}

int next_hunk_index(int current_index, int hunk_count)
{
    if (hunk_count <= 0) {
        return -1;
    }
    if (current_index >= hunk_count - 1) {
        return hunk_count - 1;
    }
    return std::max(0, current_index + 1);
}

int previous_hunk_index(int current_index, int hunk_count)
{
    if (hunk_count <= 0) {
        return -1;
    }
    if (current_index <= 0) {
        return 0;
    }
    return std::min(current_index - 1, hunk_count - 1);
}

int centered_hunk_first_visible_line(int hunk_row, int total_rows, int visible_rows)
{
    const int bounded_total_rows = std::max(1, total_rows);
    const int bounded_visible_rows = std::clamp(visible_rows, 1, bounded_total_rows);
    const int max_first_line = std::max(0, bounded_total_rows - bounded_visible_rows);
    return std::clamp(hunk_row - bounded_visible_rows / 2, 0, max_first_line);
}

ChangedTextSpans inline_changed_text_spans(const QString& left_text, const QString& right_text)
{
    qsizetype prefix = 0;
    const qsizetype max_prefix = std::min(left_text.size(), right_text.size());
    while (prefix < max_prefix && left_text[prefix] == right_text[prefix]) {
        ++prefix;
    }

    qsizetype suffix = 0;
    while (prefix + suffix < left_text.size() && prefix + suffix < right_text.size() &&
           left_text[left_text.size() - suffix - 1] == right_text[right_text.size() - suffix - 1])
    {
        ++suffix;
    }

    return {{prefix, left_text.size() - prefix - suffix}, {prefix, right_text.size() - prefix - suffix}};
}

std::vector<DiffRow> raw_text_diff_rows(const QString& left_text, const QString& right_text)
{
    const QStringList left_lines = display_lines_from_text(left_text);
    const QStringList right_lines = display_lines_from_text(right_text);
    const int left_count = static_cast<int>(left_lines.size());
    const int right_count = static_cast<int>(right_lines.size());

    std::vector<std::vector<int>> lcs(
        static_cast<size_t>(left_count + 1), std::vector<int>(static_cast<size_t>(right_count + 1), 0));
    for (int left = left_count - 1; left >= 0; --left) {
        for (int right = right_count - 1; right >= 0; --right) {
            if (left_lines[left] == right_lines[right]) {
                lcs[static_cast<size_t>(left)][static_cast<size_t>(right)] =
                    lcs[static_cast<size_t>(left + 1)][static_cast<size_t>(right + 1)] + 1;
            }
            else {
                lcs[static_cast<size_t>(left)][static_cast<size_t>(right)] =
                    std::max(lcs[static_cast<size_t>(left + 1)][static_cast<size_t>(right)],
                        lcs[static_cast<size_t>(left)][static_cast<size_t>(right + 1)]);
            }
        }
    }

    std::vector<DiffRow> rows;
    rows.reserve(static_cast<size_t>(left_count + right_count));
    int next_hunk_id = 1;

    auto append_block = [&](int left_begin, int left_end, int right_begin, int right_end) {
        if (left_begin == left_end && right_begin == right_end) {
            return;
        }

        const int hunk_id = next_hunk_id;
        const int changed_group_id = next_hunk_id;
        const int row_count = std::max(left_end - left_begin, right_end - right_begin);
        for (int row = 0; row < row_count; ++row) {
            const bool has_left = left_begin + row < left_end;
            const bool has_right = right_begin + row < right_end;
            rows.push_back({
                hunk_id,
                changed_group_id,
                has_left ? left_begin + row + 1 : -1,
                has_right ? right_begin + row + 1 : -1,
                has_left ? (has_right ? DiffSideState::Changed : DiffSideState::Deleted) : DiffSideState::Filler,
                has_right ? (has_left ? DiffSideState::Changed : DiffSideState::Added) : DiffSideState::Filler,
            });
        }
        ++next_hunk_id;
    };

    int left = 0;
    int right = 0;
    while (left < left_count && right < right_count) {
        if (left_lines[left] == right_lines[right]) {
            rows.push_back({0, -1, left + 1, right + 1, DiffSideState::Equal, DiffSideState::Equal});
            ++left;
            ++right;
            continue;
        }

        const int left_begin = left;
        const int right_begin = right;
        while (left < left_count && right < right_count && left_lines[left] != right_lines[right]) {
            if (lcs[static_cast<size_t>(left)][static_cast<size_t>(right + 1)] >=
                lcs[static_cast<size_t>(left + 1)][static_cast<size_t>(right)])
            {
                ++right;
            }
            else {
                ++left;
            }
        }
        if (left == left_count || right == right_count) {
            left = left_count;
            right = right_count;
        }
        append_block(left_begin, left, right_begin, right);
    }
    append_block(left, left_count, right, right_count);

    return rows;
}

QString render_display_text(const DiffWidgetInput& input, DiffSide side)
{
    const QStringList source_lines = display_lines_from_text(side == DiffSide::Left ? input.leftText : input.rightText);
    QStringList display_lines;
    display_lines.reserve(static_cast<qsizetype>(input.rows.size()));

    for (const DiffRow& row : input.rows) {
        const int source_line = side == DiffSide::Left ? row.leftSourceLine : row.rightSourceLine;
        display_lines.push_back(source_line == -1 ? QString() : source_lines[source_line - 1]);
    }

    return display_lines.join(QLatin1Char('\n'));
}

int source_line_for_side(const DiffRow& row, DiffSide side)
{
    return side == DiffSide::Left ? row.leftSourceLine : row.rightSourceLine;
}

QString source_text_for_side(const DiffWidgetInput& input, DiffSide side)
{
    return side == DiffSide::Left ? input.leftText : input.rightText;
}

QString source_text_with_display_row_range_applied(
    const DiffWidgetInput& input, int first_display_row, int end_display_row, DiffSide target_side)
{
    const DiffSide source_side = target_side == DiffSide::Left ? DiffSide::Right : DiffSide::Left;
    const HunkRowRange range = exact_display_row_range(
        first_display_row, end_display_row, static_cast<int>(input.rows.size()));
    if (range.startRow < 0 || range.endRow <= range.startRow) {
        return source_text_for_side(input, target_side);
    }

    const QStringList source_lines = display_lines_from_text(source_text_for_side(input, source_side));
    const QStringList target_lines = display_lines_from_text(source_text_for_side(input, target_side));
    QStringList replacement_lines;
    int target_start = target_lines.size();
    int target_end = target_lines.size();
    bool has_target_lines = false;

    for (int row_index = range.startRow; row_index < range.endRow; ++row_index) {
        const DiffRow& row = input.rows[static_cast<size_t>(row_index)];
        const int source_line = source_line_for_side(row, source_side);
        if (source_line != -1) {
            replacement_lines.append(source_lines[source_line - 1]);
        }

        const int target_line = source_line_for_side(row, target_side);
        if (target_line != -1) {
            if (!has_target_lines) {
                target_start = target_line - 1;
            }
            target_end = target_line;
            has_target_lines = true;
        }
    }

    if (!has_target_lines) {
        for (int row_index = range.endRow; row_index < static_cast<int>(input.rows.size()); ++row_index) {
            const int target_line = source_line_for_side(input.rows[static_cast<size_t>(row_index)], target_side);
            if (target_line != -1) {
                target_start = target_line - 1;
                break;
            }
        }
        target_end = target_start;
    }

    QStringList merged_lines;
    for (int line = 0; line < target_start; ++line) {
        merged_lines.append(target_lines[line]);
    }
    merged_lines.append(replacement_lines);
    for (int line = target_end; line < target_lines.size(); ++line) {
        merged_lines.append(target_lines[line]);
    }

    return merged_lines.join(QLatin1Char('\n'));
}

bool apply_active_hunk(DiffWidgetInput& input, int active_hunk_index, DiffSide target_side)
{
    const std::vector<int> target_rows = hunk_target_rows(input.rows);
    if (active_hunk_index < 0 || active_hunk_index >= static_cast<int>(target_rows.size())) {
        return false;
    }

    const HunkRowRange range = hunk_row_range_for_target_index(input.rows, target_rows, active_hunk_index);
    const QString merged_text =
        source_text_with_display_row_range_applied(input, range.startRow, range.endRow, target_side);
    if (target_side == DiffSide::Left) {
        input.leftText = merged_text;
    }
    else {
        input.rightText = merged_text;
    }
    input.rows = raw_text_diff_rows(input.leftText, input.rightText);
    return true;
}

bool apply_active_display_row_range(
    DiffWidgetInput& input, int first_display_row, int end_display_row, DiffSide target_side)
{
    const QString merged_text =
        source_text_with_display_row_range_applied(input, first_display_row, end_display_row, target_side);
    if (target_side == DiffSide::Left) {
        input.leftText = merged_text;
    }
    else {
        input.rightText = merged_text;
    }
    input.rows = raw_text_diff_rows(input.leftText, input.rightText);
    return true;
}

QString source_side_copy_text(
    const QString& display_copy_text, const std::vector<DiffRow>& rows, DiffSide side, int first_display_row)
{
    const QStringList copied_lines = display_lines_from_text(display_copy_text);
    QStringList source_lines;
    for (int index = 0; index < copied_lines.size(); ++index) {
        const int row_index = first_display_row + index;
        if (row_index < 0 || row_index >= static_cast<int>(rows.size())) {
            continue;
        }

        const DiffRow& row = rows[static_cast<size_t>(row_index)];
        const int source_line = side == DiffSide::Left ? row.leftSourceLine : row.rightSourceLine;
        if (source_line != -1) {
            source_lines.append(copied_lines[index]);
        }
    }
    return source_lines.join(QLatin1Char('\n'));
}

bool source_and_state_match(int source_line, DiffSideState state)
{
    return (source_line == -1) == (state == DiffSideState::Filler);
}

WidgetInputValidation validate_source_line_references(
    const std::vector<DiffRow>& rows, DiffSide side, int source_line_count)
{
    int expected_source_line = 1;
    for (const DiffRow& row : rows) {
        const int source_line = side == DiffSide::Left ? row.leftSourceLine : row.rightSourceLine;
        if (source_line == -1) {
            continue;
        }

        if (source_line != expected_source_line || source_line > source_line_count) {
            return side == DiffSide::Left ? WidgetInputValidation::InvalidLeftSourceLine
                                          : WidgetInputValidation::InvalidRightSourceLine;
        }
        ++expected_source_line;
    }

    if (expected_source_line - 1 != source_line_count) {
        return side == DiffSide::Left ? WidgetInputValidation::InvalidLeftSourceLine
                                      : WidgetInputValidation::InvalidRightSourceLine;
    }

    return WidgetInputValidation::Accepted;
}

WidgetInputValidation validate_diff_widget_input(const DiffWidgetInput& input)
{
    WidgetInputValidation source_validation =
        validate_source_line_references(input.rows, DiffSide::Left, display_row_count(input.leftText));
    if (source_validation != WidgetInputValidation::Accepted) {
        return source_validation;
    }

    source_validation =
        validate_source_line_references(input.rows, DiffSide::Right, display_row_count(input.rightText));
    if (source_validation != WidgetInputValidation::Accepted) {
        return source_validation;
    }

    for (const DiffRow& row : input.rows) {
        if (!source_and_state_match(row.leftSourceLine, row.leftState) ||
            !source_and_state_match(row.rightSourceLine, row.rightState))
        {
            return WidgetInputValidation::SourceLineStateMismatch;
        }
    }

    const int expected_rows = static_cast<int>(input.rows.size());
    if (display_row_count(render_display_text(input, DiffSide::Left)) != expected_rows ||
        display_row_count(render_display_text(input, DiffSide::Right)) != expected_rows)
    {
        return WidgetInputValidation::MismatchedDisplayRowCount;
    }

    return WidgetInputValidation::Accepted;
}

void expect_exact_rows(const std::vector<DiffRow>& actual, const std::vector<DiffRow>& expected)
{
    SQ_EXPECT(actual.size() == expected.size());
    if (actual.size() != expected.size()) {
        return;
    }

    for (size_t row = 0; row < expected.size(); ++row) {
        SQ_EXPECT(actual[row] == expected[row]);
    }
}

void expect_raw_text_diff_rows(
    const QString& left_text, const QString& right_text, const std::vector<DiffRow>& expected)
{
    const std::vector<DiffRow> rows = raw_text_diff_rows(left_text, right_text);
    expect_exact_rows(rows, expected);

    const DiffWidgetInput input{left_text, right_text, rows};
    SQ_EXPECT(validate_diff_widget_input(input) == WidgetInputValidation::Accepted);
    SQ_EXPECT(display_row_count(render_display_text(input, DiffSide::Left)) == static_cast<int>(rows.size()));
    SQ_EXPECT(display_row_count(render_display_text(input, DiffSide::Right)) == static_cast<int>(rows.size()));
}

bool read_hunk_number(const QString& line, qsizetype& offset)
{
    const qsizetype begin = offset;
    while (offset < line.size() && line[offset].isDigit()) {
        ++offset;
    }
    return offset != begin;
}

bool read_hunk_range(const QString& line, qsizetype& offset, QChar sign)
{
    if (offset >= line.size() || line[offset] != sign) {
        return false;
    }
    ++offset;

    if (!read_hunk_number(line, offset) || offset >= line.size() || line[offset] != QLatin1Char(',')) {
        return false;
    }
    ++offset;

    return read_hunk_number(line, offset);
}

bool is_unified_hunk_header(const QString& line)
{
    qsizetype offset = 3;
    if (!line.startsWith(QStringLiteral("@@ ")) || !read_hunk_range(line, offset, QLatin1Char('-')) ||
        offset >= line.size() || line[offset] != QLatin1Char(' '))
    {
        return false;
    }
    ++offset;

    return read_hunk_range(line, offset, QLatin1Char('+')) && line.mid(offset).startsWith(QStringLiteral(" @@"));
}

bool stored_unified_diff_input(const QString& diff_text, DiffWidgetInput& input)
{
    QStringList left_lines;
    QStringList right_lines;
    std::vector<DiffRow> rows;
    std::vector<int> deleted_lines;
    std::vector<int> added_lines;
    int hunk_id = 0;
    int next_changed_group_id = 1;
    bool in_hunk = false;
    bool saw_hunk = false;
    bool saw_diff_git = false;

    auto flush_changed_block = [&]() {
        if (deleted_lines.empty() && added_lines.empty()) {
            return;
        }

        const int changed_group_id = next_changed_group_id++;
        const size_t row_count = std::max(deleted_lines.size(), added_lines.size());
        for (size_t row = 0; row < row_count; ++row) {
            const bool has_left = row < deleted_lines.size();
            const bool has_right = row < added_lines.size();
            rows.push_back({
                hunk_id,
                changed_group_id,
                has_left ? deleted_lines[row] : -1,
                has_right ? added_lines[row] : -1,
                has_left ? (has_right ? DiffSideState::Changed : DiffSideState::Deleted) : DiffSideState::Filler,
                has_right ? (has_left ? DiffSideState::Changed : DiffSideState::Added) : DiffSideState::Filler,
            });
        }
        deleted_lines.clear();
        added_lines.clear();
    };

    const QStringList lines = diff_text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (qsizetype index = 0; index < lines.size(); ++index) {
        QString line = lines[index];
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (index == lines.size() - 1 && line.isEmpty()) {
            break;
        }

        if (line.startsWith(QStringLiteral("diff --git "))) {
            if (saw_diff_git || saw_hunk) {
                return false;
            }
            saw_diff_git = true;
            continue;
        }

        if (is_unified_hunk_header(line)) {
            flush_changed_block();
            in_hunk = true;
            saw_hunk = true;
            ++hunk_id;
            continue;
        }

        if (!in_hunk) {
            if (line.isEmpty() || line.startsWith(QStringLiteral("index ")) || line.startsWith(QStringLiteral("--- ")) ||
                line.startsWith(QStringLiteral("+++ ")))
            {
                continue;
            }
            return false;
        }

        if (line.startsWith(QStringLiteral("\\ No newline at end of file"))) {
            continue;
        }

        if (line.startsWith(QLatin1Char(' '))) {
            flush_changed_block();
            const QString text = line.mid(1);
            left_lines.push_back(text);
            right_lines.push_back(text);
            rows.push_back({0, -1, static_cast<int>(left_lines.size()), static_cast<int>(right_lines.size()),
                DiffSideState::Equal, DiffSideState::Equal});
            continue;
        }

        if (line.startsWith(QLatin1Char('-'))) {
            left_lines.push_back(line.mid(1));
            deleted_lines.push_back(static_cast<int>(left_lines.size()));
            continue;
        }

        if (line.startsWith(QLatin1Char('+'))) {
            right_lines.push_back(line.mid(1));
            added_lines.push_back(static_cast<int>(right_lines.size()));
            continue;
        }

        return false;
    }

    flush_changed_block();
    if (!saw_hunk) {
        return false;
    }

    input = {left_lines.join(QLatin1Char('\n')), right_lines.join(QLatin1Char('\n')), rows};
    return validate_diff_widget_input(input) == WidgetInputValidation::Accepted;
}

bool live_or_fallback_unified_diff_input(
    bool command_succeeded, const QString& stdout_text, const QString& fallback_diff_text, DiffWidgetInput& input)
{
    const QString& diff_text = command_succeeded && !stdout_text.isEmpty() ? stdout_text : fallback_diff_text;
    return stored_unified_diff_input(diff_text, input);
}

void test_stored_unified_diff_adapter()
{
    DiffWidgetInput input;
    SQ_EXPECT(stored_unified_diff_input(QStringLiteral("diff --git a/file.txt b/file.txt\n"
                                                       "index 1111111..2222222 100644\n"
                                                       "--- a/file.txt\n"
                                                       "+++ b/file.txt\n"
                                                       "@@ -1,5 +1,5 @@\n"
                                                       " alpha\n"
                                                       "-old value\n"
                                                       "+new value\n"
                                                       "+right only\n"
                                                       " shared\n"
                                                       "-left only\n"
                                                       "\\ No newline at end of file\n"
                                                       " tail\n"),
        input));

    SQ_EXPECT(input.leftText == QStringLiteral("alpha\nold value\nshared\nleft only\ntail"));
    SQ_EXPECT(input.rightText == QStringLiteral("alpha\nnew value\nright only\nshared\ntail"));
    expect_exact_rows(input.rows, {
                                      {0, -1, 1, 1, DiffSideState::Equal, DiffSideState::Equal},
                                      {1, 1, 2, 2, DiffSideState::Changed, DiffSideState::Changed},
                                      {1, 1, -1, 3, DiffSideState::Filler, DiffSideState::Added},
                                      {0, -1, 3, 4, DiffSideState::Equal, DiffSideState::Equal},
                                      {1, 2, 4, -1, DiffSideState::Deleted, DiffSideState::Filler},
                                      {0, -1, 5, 5, DiffSideState::Equal, DiffSideState::Equal},
                                  });
    SQ_EXPECT(validate_diff_widget_input(input) == WidgetInputValidation::Accepted);

    SQ_EXPECT(!stored_unified_diff_input(QStringLiteral("--- a/file.txt\n"
                                                        "+++ b/file.txt\n"
                                                        "-body before hunk\n"),
        input));
    SQ_EXPECT(!stored_unified_diff_input(QStringLiteral("diff --git a/one.txt b/one.txt\n"
                                                        "--- a/one.txt\n"
                                                        "+++ b/one.txt\n"
                                                        "@@ -1,1 +1,1 @@\n"
                                                        " one\n"
                                                        "diff --git a/two.txt b/two.txt\n"),
        input));
}

void test_live_command_diff_adapter_selection()
{
    const QString live_diff = QStringLiteral("diff --git a/file.txt b/file.txt\n"
                                             "--- a/file.txt\n"
                                             "+++ b/file.txt\n"
                                             "@@ -1,1 +1,1 @@\n"
                                             "-fallback loses\n"
                                             "+live wins\n");
    const QString fallback_diff = QStringLiteral("diff --git a/file.txt b/file.txt\n"
                                                 "--- a/file.txt\n"
                                                 "+++ b/file.txt\n"
                                                 "@@ -1,1 +1,1 @@\n"
                                                 "-old fallback\n"
                                                 "+fallback wins\n");

    DiffWidgetInput input;
    SQ_EXPECT(live_or_fallback_unified_diff_input(true, live_diff, fallback_diff, input));
    SQ_EXPECT(input.rightText == QStringLiteral("live wins"));

    SQ_EXPECT(live_or_fallback_unified_diff_input(true, QString(), fallback_diff, input));
    SQ_EXPECT(input.rightText == QStringLiteral("fallback wins"));

    SQ_EXPECT(live_or_fallback_unified_diff_input(false, live_diff, fallback_diff, input));
    SQ_EXPECT(input.rightText == QStringLiteral("fallback wins"));
}

void expect_side_has_blank_fillers_only(
    const std::vector<DiffRow>& rows, const QStringList& display_lines, DiffSide side)
{
    SQ_EXPECT(static_cast<size_t>(display_lines.size()) == rows.size());
    if (static_cast<size_t>(display_lines.size()) != rows.size()) {
        return;
    }

    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const DiffRow& row = rows[row_index];
        const int source_line = side == DiffSide::Left ? row.leftSourceLine : row.rightSourceLine;
        const DiffSideState state = side == DiffSide::Left ? row.leftState : row.rightState;
        const QString& display_line = display_lines[static_cast<qsizetype>(row_index)];

        SQ_EXPECT((source_line == -1) == (state == DiffSideState::Filler));
        SQ_EXPECT(display_line.isEmpty() == (source_line == -1));
        if (source_line != -1) {
            SQ_EXPECT(display_line == source_line_text(side, source_line));
        }
    }
}

void expect_rendered_text_matches_row_model(const std::vector<DiffRow>& rows)
{
    const QString left_text = render_display_text(rows, DiffSide::Left);
    const QString right_text = render_display_text(rows, DiffSide::Right);
    const QStringList left_lines = display_lines_from_text(left_text);
    const QStringList right_lines = display_lines_from_text(right_text);
    bool has_one_sided_filler = false;

    SQ_EXPECT(left_lines.size() == right_lines.size());
    SQ_EXPECT(static_cast<size_t>(left_lines.size()) == rows.size());
    SQ_EXPECT(static_cast<size_t>(right_lines.size()) == rows.size());
    expect_side_has_blank_fillers_only(rows, left_lines, DiffSide::Left);
    expect_side_has_blank_fillers_only(rows, right_lines, DiffSide::Right);

    if (static_cast<size_t>(left_lines.size()) != rows.size() || static_cast<size_t>(right_lines.size()) != rows.size())
    {
        return;
    }

    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const DiffRow& row = rows[row_index];
        const bool left_missing = row.leftSourceLine == -1;
        const bool right_missing = row.rightSourceLine == -1;

        if (left_missing == right_missing) {
            continue;
        }

        has_one_sided_filler = true;
        SQ_EXPECT(left_lines[static_cast<qsizetype>(row_index)].isEmpty() == left_missing);
        SQ_EXPECT(right_lines[static_cast<qsizetype>(row_index)].isEmpty() == right_missing);
        SQ_EXPECT(left_lines[static_cast<qsizetype>(row_index)].isEmpty() !=
                  right_lines[static_cast<qsizetype>(row_index)].isEmpty());
    }

    if (has_one_sided_filler) {
        SQ_EXPECT(left_text != right_text);
    }
}

DiffWidgetInput valid_widget_input_fixture()
{
    std::vector<DiffRow> rows{
        {1, 10, 1, 1, DiffSideState::Changed, DiffSideState::Changed},
        {1, 10, -1, 2, DiffSideState::Filler, DiffSideState::Added},
        {1, 10, 2, -1, DiffSideState::Deleted, DiffSideState::Filler},
    };

    return {QStringLiteral("left source line 1\nleft source line 2"),
        QStringLiteral("right source line 1\nright source line 2"), rows};
}

void test_widget_input_contract_validation()
{
    const DiffWidgetInput valid = valid_widget_input_fixture();
    SQ_EXPECT(validate_diff_widget_input(valid) == WidgetInputValidation::Accepted);
    SQ_EXPECT(render_display_text(valid, DiffSide::Left) == QStringLiteral("left source line 1\n\nleft source line 2"));
    SQ_EXPECT(
        render_display_text(valid, DiffSide::Right) == QStringLiteral("right source line 1\nright source line 2\n"));

    DiffWidgetInput short_left_text = valid;
    short_left_text.leftText = QStringLiteral("left source line 1");
    SQ_EXPECT(validate_diff_widget_input(short_left_text) == WidgetInputValidation::InvalidLeftSourceLine);

    DiffWidgetInput long_right_text = valid;
    long_right_text.rightText += QStringLiteral("\nright extra source line");
    SQ_EXPECT(validate_diff_widget_input(long_right_text) == WidgetInputValidation::InvalidRightSourceLine);

    DiffWidgetInput invalid_left_reference = valid;
    invalid_left_reference.rows[2].leftSourceLine = 4;
    SQ_EXPECT(validate_diff_widget_input(invalid_left_reference) == WidgetInputValidation::InvalidLeftSourceLine);

    DiffWidgetInput invalid_right_reference = valid;
    invalid_right_reference.rows[1].rightSourceLine = 3;
    SQ_EXPECT(validate_diff_widget_input(invalid_right_reference) == WidgetInputValidation::InvalidRightSourceLine);
}

void test_source_side_copy_strips_filler_rows()
{
    const DiffWidgetInput input = valid_widget_input_fixture();
    const QString left_display_copy = render_display_text(input, DiffSide::Left);
    const QString right_display_copy = render_display_text(input, DiffSide::Right);

    SQ_EXPECT(source_side_copy_text(left_display_copy, input.rows, DiffSide::Left, 0) ==
              QStringLiteral("left source line 1\nleft source line 2"));
    SQ_EXPECT(source_side_copy_text(right_display_copy, input.rows, DiffSide::Right, 0) ==
              QStringLiteral("right source line 1\nright source line 2"));
    SQ_EXPECT(source_side_copy_text(QString(), input.rows, DiffSide::Left, 1).isEmpty());
}

void test_apply_active_hunk_left_to_right_and_right_to_left()
{
    const DiffWidgetInput base{
        QStringLiteral("alpha\nleft-only\nomega"),
        QStringLiteral("alpha\nomega"),
        raw_text_diff_rows(QStringLiteral("alpha\nleft-only\nomega"), QStringLiteral("alpha\nomega")),
    };
    SQ_EXPECT(validate_diff_widget_input(base) == WidgetInputValidation::Accepted);
    SQ_EXPECT(hunk_target_rows(base.rows) == std::vector<int>({1}));

    DiffWidgetInput left_to_right = base;
    SQ_EXPECT(apply_active_hunk(left_to_right, 0, DiffSide::Right));
    SQ_EXPECT(left_to_right.leftText == base.leftText);
    SQ_EXPECT(left_to_right.rightText == base.leftText);
    SQ_EXPECT(validate_diff_widget_input(left_to_right) == WidgetInputValidation::Accepted);
    SQ_EXPECT(hunk_target_rows(left_to_right.rows).empty());
    SQ_EXPECT(render_display_text(left_to_right, DiffSide::Left) ==
              render_display_text(left_to_right, DiffSide::Right));

    DiffWidgetInput right_to_left = base;
    SQ_EXPECT(apply_active_hunk(right_to_left, 0, DiffSide::Left));
    SQ_EXPECT(right_to_left.leftText == base.rightText);
    SQ_EXPECT(right_to_left.rightText == base.rightText);
    SQ_EXPECT(validate_diff_widget_input(right_to_left) == WidgetInputValidation::Accepted);
    SQ_EXPECT(hunk_target_rows(right_to_left.rows).empty());
    SQ_EXPECT(render_display_text(right_to_left, DiffSide::Left) ==
              render_display_text(right_to_left, DiffSide::Right));
}

void test_apply_active_display_row_range()
{
    DiffWidgetInput input{
        QStringLiteral("alpha\nleft one\nleft two\nomega"),
        QStringLiteral("alpha\nright one\nright two\nomega"),
        raw_text_diff_rows(QStringLiteral("alpha\nleft one\nleft two\nomega"),
            QStringLiteral("alpha\nright one\nright two\nomega")),
    };
    SQ_EXPECT(validate_diff_widget_input(input) == WidgetInputValidation::Accepted);
    SQ_EXPECT(hunk_target_rows(input.rows) == std::vector<int>({1}));

    SQ_EXPECT(apply_active_display_row_range(input, 1, 2, DiffSide::Right));
    SQ_EXPECT(input.rightText == QStringLiteral("alpha\nleft one\nright two\nomega"));
    SQ_EXPECT(input.leftText == QStringLiteral("alpha\nleft one\nleft two\nomega"));
}

void test_raw_text_line_diff_adapter()
{
    expect_raw_text_diff_rows(QStringLiteral("alpha\nbeta\ngamma"), QStringLiteral("alpha\nbeta\ngamma"),
        {
            {0, -1, 1, 1, DiffSideState::Equal, DiffSideState::Equal},
            {0, -1, 2, 2, DiffSideState::Equal, DiffSideState::Equal},
            {0, -1, 3, 3, DiffSideState::Equal, DiffSideState::Equal},
        });

    expect_raw_text_diff_rows(QStringLiteral("alpha\ngamma"), QStringLiteral("alpha\nbeta\ngamma"),
        {
            {0, -1, 1, 1, DiffSideState::Equal, DiffSideState::Equal},
            {1, 1, -1, 2, DiffSideState::Filler, DiffSideState::Added},
            {0, -1, 2, 3, DiffSideState::Equal, DiffSideState::Equal},
        });

    expect_raw_text_diff_rows(QStringLiteral("alpha\nbeta\ngamma"), QStringLiteral("alpha\ngamma"),
        {
            {0, -1, 1, 1, DiffSideState::Equal, DiffSideState::Equal},
            {1, 1, 2, -1, DiffSideState::Deleted, DiffSideState::Filler},
            {0, -1, 3, 2, DiffSideState::Equal, DiffSideState::Equal},
        });

    expect_raw_text_diff_rows(QStringLiteral("old value"), QStringLiteral("new value"),
        {
            {1, 1, 1, 1, DiffSideState::Changed, DiffSideState::Changed},
        });

    expect_raw_text_diff_rows(QStringLiteral("old one\nold two"), QStringLiteral("new one\nnew two\nnew three"),
        {
            {1, 1, 1, 1, DiffSideState::Changed, DiffSideState::Changed},
            {1, 1, 2, 2, DiffSideState::Changed, DiffSideState::Changed},
            {1, 1, -1, 3, DiffSideState::Filler, DiffSideState::Added},
        });

    expect_raw_text_diff_rows(QStringLiteral("same\nleft-only"), QStringLiteral("right-only\nsame"),
        {
            {1, 1, -1, 1, DiffSideState::Filler, DiffSideState::Added},
            {0, -1, 1, 2, DiffSideState::Equal, DiffSideState::Equal},
            {2, 2, 2, -1, DiffSideState::Deleted, DiffSideState::Filler},
        });
}

void test_inline_changed_text_spans()
{
    const ChangedTextSpans changed_words =
        inline_changed_text_spans(QStringLiteral("abc def ghi"), QStringLiteral("abc xyz ghi"));
    SQ_EXPECT(changed_words.left.start == 4);
    SQ_EXPECT(changed_words.left.length == 3);
    SQ_EXPECT(changed_words.right.start == 4);
    SQ_EXPECT(changed_words.right.length == 3);
    SQ_EXPECT(QStringLiteral("abc def ghi").mid(changed_words.left.start, changed_words.left.length) ==
              QStringLiteral("def"));
    SQ_EXPECT(QStringLiteral("abc xyz ghi").mid(changed_words.right.start, changed_words.right.length) ==
              QStringLiteral("xyz"));

    const ChangedTextSpans inserted =
        inline_changed_text_spans(QStringLiteral("abcghi"), QStringLiteral("abcxyzghi"));
    SQ_EXPECT(inserted.left.start == 3);
    SQ_EXPECT(inserted.left.length == 0);
    SQ_EXPECT(inserted.right.start == 3);
    SQ_EXPECT(inserted.right.length == 3);
    SQ_EXPECT(QStringLiteral("abcghi").mid(inserted.left.start, inserted.left.length).isEmpty());
    SQ_EXPECT(QStringLiteral("abcxyzghi").mid(inserted.right.start, inserted.right.length) ==
              QStringLiteral("xyz"));

    const ChangedTextSpans repeated_token =
        inline_changed_text_spans(QStringLiteral("foo bar foo"), QStringLiteral("foo foo bar foo"));
    SQ_EXPECT(repeated_token.left.start == 4);
    SQ_EXPECT(repeated_token.left.length == 0);
    SQ_EXPECT(repeated_token.right.start == 4);
    SQ_EXPECT(repeated_token.right.length == 4);
    SQ_EXPECT(QStringLiteral("foo bar foo").mid(repeated_token.left.start, repeated_token.left.length).isEmpty());
    SQ_EXPECT(QStringLiteral("foo foo bar foo").mid(repeated_token.right.start, repeated_token.right.length) ==
              QStringLiteral("foo "));
}

void test_display_row_model_changed_blocks()
{
    const std::vector<DiffRow> one_to_one = changed_block_rows(1, 10, {3}, {7});
    expect_exact_rows(one_to_one, {
                                      {1, 10, 3, 7, DiffSideState::Changed, DiffSideState::Changed},
                                  });
    expect_rendered_text_matches_row_model(one_to_one);

    const std::vector<DiffRow> one_to_many = changed_block_rows(2, 20, {11}, {15, 16, 17});
    expect_exact_rows(one_to_many, {
                                       {2, 20, 11, 15, DiffSideState::Changed, DiffSideState::Changed},
                                       {2, 20, -1, 16, DiffSideState::Filler, DiffSideState::Added},
                                       {2, 20, -1, 17, DiffSideState::Filler, DiffSideState::Added},
                                   });
    expect_rendered_text_matches_row_model(one_to_many);

    const std::vector<DiffRow> many_to_one = changed_block_rows(3, 30, {21, 22, 23}, {31});
    expect_exact_rows(many_to_one, {
                                       {3, 30, 21, 31, DiffSideState::Changed, DiffSideState::Changed},
                                       {3, 30, 22, -1, DiffSideState::Deleted, DiffSideState::Filler},
                                       {3, 30, 23, -1, DiffSideState::Deleted, DiffSideState::Filler},
                                   });
    expect_rendered_text_matches_row_model(many_to_one);

    const std::vector<DiffRow> many_to_many = changed_block_rows(4, 40, {41, 42}, {51, 52, 53});
    expect_exact_rows(many_to_many, {
                                        {4, 40, 41, 51, DiffSideState::Changed, DiffSideState::Changed},
                                        {4, 40, 42, 52, DiffSideState::Changed, DiffSideState::Changed},
                                        {4, 40, -1, 53, DiffSideState::Filler, DiffSideState::Added},
                                    });
    expect_rendered_text_matches_row_model(many_to_many);
}

void test_hunk_navigation_model()
{
    const std::vector<DiffRow> rows{
        {0, -1, 1, 1, DiffSideState::Equal, DiffSideState::Equal},
        {10, 1, 2, 2, DiffSideState::Changed, DiffSideState::Changed},
        {10, 1, -1, 3, DiffSideState::Filler, DiffSideState::Added},
        {0, -1, 3, 4, DiffSideState::Equal, DiffSideState::Equal},
        {10, 2, 4, 5, DiffSideState::Changed, DiffSideState::Changed},
        {20, 3, 5, -1, DiffSideState::Deleted, DiffSideState::Filler},
        {0, -1, 6, 6, DiffSideState::Equal, DiffSideState::Equal},
        {30, 4, 7, 7, DiffSideState::Changed, DiffSideState::Changed},
    };
    const std::vector<int> targets = hunk_target_rows(rows);

    SQ_EXPECT(targets == std::vector<int>({1, 5, 7}));
    const HunkRowRange first_range = hunk_row_range_for_target_index(rows, targets, 0);
    SQ_EXPECT(first_range.startRow == 1);
    SQ_EXPECT(first_range.endRow == 3);
    const HunkRowRange second_range = hunk_row_range_for_target_index(rows, targets, 1);
    SQ_EXPECT(second_range.startRow == 5);
    SQ_EXPECT(second_range.endRow == 6);
    const HunkRowRange third_range = hunk_row_range_for_target_index(rows, targets, 2);
    SQ_EXPECT(third_range.startRow == 7);
    SQ_EXPECT(third_range.endRow == 8);
    const HunkRowRange invalid_range = hunk_row_range_for_target_index(rows, targets, -1);
    SQ_EXPECT(invalid_range.startRow == -1);
    SQ_EXPECT(invalid_range.endRow == -1);
    const HunkRowRange no_hunk_range = hunk_row_range_for_target_index(rows, {0}, 0);
    SQ_EXPECT(no_hunk_range.startRow == -1);
    SQ_EXPECT(no_hunk_range.endRow == -1);
    SQ_EXPECT(next_hunk_index(0, static_cast<int>(targets.size())) == 1);
    SQ_EXPECT(next_hunk_index(2, static_cast<int>(targets.size())) == 2);
    SQ_EXPECT(previous_hunk_index(2, static_cast<int>(targets.size())) == 1);
    SQ_EXPECT(previous_hunk_index(0, static_cast<int>(targets.size())) == 0);
    SQ_EXPECT(next_hunk_index(0, 0) == -1);
    SQ_EXPECT(previous_hunk_index(0, 0) == -1);
    SQ_EXPECT(centered_hunk_first_visible_line(30, 100, 20) == 20);
    SQ_EXPECT(centered_hunk_first_visible_line(3, 100, 20) == 0);
    SQ_EXPECT(centered_hunk_first_visible_line(95, 100, 20) == 80);
}

void test_active_hunk_from_display_rows()
{
    const std::vector<DiffRow> rows{
        {0, -1, 1, 1, DiffSideState::Equal, DiffSideState::Equal},
        {10, 1, 2, 2, DiffSideState::Changed, DiffSideState::Changed},
        {10, 1, -1, 3, DiffSideState::Filler, DiffSideState::Added},
        {0, -1, 3, 4, DiffSideState::Equal, DiffSideState::Equal},
        {20, 2, 4, -1, DiffSideState::Deleted, DiffSideState::Filler},
        {0, -1, 5, 5, DiffSideState::Equal, DiffSideState::Equal},
    };
    const std::vector<int> targets = hunk_target_rows(rows);

    SQ_EXPECT(targets == std::vector<int>({1, 4}));
    SQ_EXPECT(hunk_index_for_display_row(rows, targets, 0) == -1);
    SQ_EXPECT(hunk_index_for_display_row(rows, targets, 1) == 0);
    SQ_EXPECT(hunk_index_for_display_row(rows, targets, 2) == 0);
    SQ_EXPECT(hunk_index_for_display_row(rows, targets, 4) == 1);
    SQ_EXPECT(first_hunk_index_intersecting_display_row_range(rows, targets, 0, 1) == -1);
    SQ_EXPECT(first_hunk_index_intersecting_display_row_range(rows, targets, 0, 3) == 0);
    SQ_EXPECT(first_hunk_index_intersecting_display_row_range(rows, targets, 3, 5) == 1);
    SQ_EXPECT(first_hunk_index_intersecting_display_row_range(rows, targets, 5, 6) == -1);

    HunkRowRange exact = exact_display_row_range(2, 3, static_cast<int>(rows.size()));
    SQ_EXPECT(exact.startRow == 2);
    SQ_EXPECT(exact.endRow == 3);
    exact = exact_display_row_range(1, 4, static_cast<int>(rows.size()));
    SQ_EXPECT(exact.startRow == 1);
    SQ_EXPECT(exact.endRow == 4);
    HunkRowRange block = changed_block_range_for_display_row(rows, targets, 2);
    SQ_EXPECT(block.startRow == 1);
    SQ_EXPECT(block.endRow == 3);
    block = changed_block_range_for_display_row(rows, targets, 0);
    SQ_EXPECT(block.startRow == 0);
    SQ_EXPECT(block.endRow == 1);
    SQ_EXPECT(display_row_in_range(2, HunkRowRange{2, 3}));
    SQ_EXPECT(!display_row_in_range(1, HunkRowRange{2, 3}));
    SQ_EXPECT(!display_row_in_range(3, HunkRowRange{2, 3}));
}

void test_active_hunk_boundary_thickness_model()
{
    SQ_EXPECT(nearly_equal(active_hunk_boundary_logical_thickness(0.0), 2.0));
    SQ_EXPECT(nearly_equal(active_hunk_boundary_logical_thickness(1.0), 2.0));
    SQ_EXPECT(nearly_equal(active_hunk_boundary_logical_thickness(1.25), 1.6));
    SQ_EXPECT(nearly_equal(active_hunk_boundary_logical_thickness(2.0), 1.0));
    SQ_EXPECT(nearly_equal(active_hunk_boundary_logical_thickness(3.0), 2.0 / 3.0));
}

void test_vertical_scrollbar_model_mapping()
{
    constexpr int total_rows = 100;
    constexpr int visible_rows = 20;
    constexpr int max_first_visible_line = total_rows - visible_rows;

    const VerticalScrollbarModel equal_rows = vertical_scrollbar_model(visible_rows, visible_rows, 8);
    SQ_EXPECT(equal_rows.maxFirstVisibleLine == 0);
    SQ_EXPECT(nearly_equal(equal_rows.size, 1.0));
    SQ_EXPECT(nearly_equal(equal_rows.position, 0.0));

    const VerticalScrollbarModel fewer_rows = vertical_scrollbar_model(visible_rows - 1, visible_rows, 8);
    SQ_EXPECT(fewer_rows.maxFirstVisibleLine == 0);
    SQ_EXPECT(nearly_equal(fewer_rows.size, 1.0));
    SQ_EXPECT(nearly_equal(fewer_rows.position, 0.0));

    const VerticalScrollbarModel middle =
        vertical_scrollbar_model(total_rows, visible_rows, max_first_visible_line / 2);
    const double max_normalized_travel = 1.0 - middle.size;
    SQ_EXPECT(middle.maxFirstVisibleLine == max_first_visible_line);
    SQ_EXPECT(middle.position > 0.0);
    SQ_EXPECT(middle.position < max_normalized_travel);

    const VerticalScrollbarModel bottom = vertical_scrollbar_model(total_rows, visible_rows, max_first_visible_line);
    SQ_EXPECT(nearly_equal(bottom.position, 1.0 - bottom.size));

    SQ_EXPECT(first_visible_line_for_vertical_scrollbar_position(total_rows, visible_rows, 1.0 - bottom.size - 0.001) ==
              max_first_visible_line);
    SQ_EXPECT(first_visible_line_for_vertical_scrollbar_position(total_rows, visible_rows, 1.0) ==
              max_first_visible_line);
}

void test_horizontal_scrollbar_model_mapping()
{
    constexpr int viewport_width = 320;
    constexpr int content_width = 1280;
    constexpr int max_x_offset = content_width - viewport_width;

    const HorizontalScrollbarModel equal_width = horizontal_scrollbar_model(viewport_width, viewport_width, 80);
    SQ_EXPECT(!equal_width.needed);
    SQ_EXPECT(equal_width.maxXOffset == 0);
    SQ_EXPECT(nearly_equal(equal_width.size, 1.0));
    SQ_EXPECT(nearly_equal(equal_width.position, 0.0));

    const HorizontalScrollbarModel narrower_content =
        horizontal_scrollbar_model(viewport_width - 1, viewport_width, 80);
    SQ_EXPECT(!narrower_content.needed);
    SQ_EXPECT(narrower_content.maxXOffset == 0);
    SQ_EXPECT(nearly_equal(narrower_content.size, 1.0));
    SQ_EXPECT(nearly_equal(narrower_content.position, 0.0));

    const HorizontalScrollbarModel middle = horizontal_scrollbar_model(content_width, viewport_width, max_x_offset / 2);
    const double max_normalized_travel = 1.0 - middle.size;
    SQ_EXPECT(middle.needed);
    SQ_EXPECT(middle.maxXOffset == max_x_offset);
    SQ_EXPECT(middle.position > 0.0);
    SQ_EXPECT(middle.position < max_normalized_travel);

    const HorizontalScrollbarModel right_only_long =
        horizontal_scrollbar_model_for_panes(viewport_width - 1, content_width, viewport_width, viewport_width,
            max_x_offset / 2);
    SQ_EXPECT(right_only_long.needed);
    SQ_EXPECT(right_only_long.maxXOffset == max_x_offset);

    const HorizontalScrollbarModel end = horizontal_scrollbar_model(content_width, viewport_width, max_x_offset);
    SQ_EXPECT(end.needed);
    SQ_EXPECT(end.maxXOffset == max_x_offset);
    SQ_EXPECT(nearly_equal(end.position, 1.0 - end.size));

    SQ_EXPECT(x_offset_for_horizontal_scrollbar_position(content_width, viewport_width, 1.0 - end.size - 0.0001) ==
              max_x_offset);
    SQ_EXPECT(x_offset_for_horizontal_scrollbar_position(content_width, viewport_width, 1.0) == max_x_offset);
}

bool image_region_has_dark_pixels(const QImage& image, int x_start, int x_end)
{
    if (image.isNull()) {
        return false;
    }

    const QImage converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int dark_pixels = 0;
    for (int y = 0; y < converted.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(converted.constScanLine(y));
        for (int x = x_start; x < x_end; ++x) {
            const int alpha = qAlpha(line[x]);
            if (alpha == 0) {
                continue;
            }
            const int luma = (qRed(line[x]) * 299 + qGreen(line[x]) * 587 + qBlue(line[x]) * 114) / 1000;
            if (luma < 120) {
                ++dark_pixels;
                if (dark_pixels >= 32) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool pixel_is_green(QRgb pixel)
{
    return qAlpha(pixel) != 0 && qGreen(pixel) > 180 && qRed(pixel) < 120 && qBlue(pixel) < 120;
}

GreenPixelCoverage image_region_green_pixel_coverage(const QImage& image, int x_start, int x_end)
{
    if (image.isNull()) {
        return {};
    }

    const int start = std::max(0, x_start);
    const int end = std::min(image.width(), x_end);
    if (start >= end) {
        return {};
    }

    const QImage converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    GreenPixelCoverage coverage;
    coverage.totalPixels = (end - start) * converted.height();
    for (int y = 0; y < converted.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(converted.constScanLine(y));
        for (int x = start; x < end; ++x) {
            if (pixel_is_green(line[x])) {
                ++coverage.greenPixels;
            }
        }
    }
    return coverage;
}

bool image_region_has_green_pixels(const QImage& image, int x_start, int x_end)
{
    return image_region_green_pixel_coverage(image, x_start, x_end).greenPixels >= 64;
}

bool image_region_has_magenta_pixels(const QImage& image, int x_start, int x_end)
{
    if (image.isNull()) {
        return false;
    }

    const QImage converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int magenta_pixels = 0;
    for (int y = 0; y < converted.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(converted.constScanLine(y));
        for (int x = x_start; x < x_end; ++x) {
            if (qAlpha(line[x]) != 0 && qRed(line[x]) > 180 && qBlue(line[x]) > 180 && qGreen(line[x]) < 120) {
                ++magenta_pixels;
                if (magenta_pixels >= 64) {
                    return true;
                }
            }
        }
    }
    return false;
}

void configure_pane(ScintillaQuick_item& pane, const QString& text)
{
    pane.setProperty("font", scintillaquick::shared::deterministic_test_font(11));
    pane.send(SCI_SETTABWIDTH, 4);
    pane.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    pane.send(SCI_SETMARGINTYPEN, line_number_margin, SC_MARGIN_NUMBER);
    pane.send(SCI_SETMARGINWIDTHN, line_number_margin, line_number_margin_width);
    pane.setProperty("text", text);
    pane.setProperty("readonly", true);
}

void configure_native_diff_row_markers(ScintillaQuick_item& pane)
{
    const int margin_count = static_cast<int>(pane.send(SCI_GETMARGINS));
    for (int margin = 0; margin < margin_count; ++margin) {
        pane.send(SCI_SETMARGINMASKN, margin, 0);
    }

    auto configure_marker = [&](int marker_number, int color) {
        pane.send(SCI_MARKERDEFINE, marker_number, SC_MARK_FULLRECT);
        pane.send(SCI_MARKERSETBACK, marker_number, color);
        pane.send(SCI_MARKERSETLAYER, marker_number, SC_LAYER_UNDER_TEXT);
    };

    configure_marker(diff_marker_added, 0xCCFFCC);
    configure_marker(diff_marker_deleted, 0xCCCCFF);
    configure_marker(diff_marker_changed, 0xCCFFFF);
    configure_marker(diff_marker_filler, 0xEEEEEE);
}

int native_marker_for_state(DiffSideState state)
{
    switch (state) {
        case DiffSideState::Added:
            return diff_marker_added;
        case DiffSideState::Deleted:
            return diff_marker_deleted;
        case DiffSideState::Changed:
            return diff_marker_changed;
        case DiffSideState::Filler:
            return diff_marker_filler;
        case DiffSideState::Equal:
            return -1;
    }

    return -1;
}

void apply_native_diff_row_markers(ScintillaQuick_item& pane, const std::vector<DiffRow>& rows, DiffSide side)
{
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const DiffRow& row = rows[row_index];
        const DiffSideState state = side == DiffSide::Left ? row.leftState : row.rightState;
        const int marker = native_marker_for_state(state);
        if (marker != -1) {
            pane.send(SCI_MARKERADD, static_cast<int>(row_index), marker);
        }
    }
}

int native_marker_mask(int marker_number)
{
    return 1 << marker_number;
}

void expect_marker_bits(ScintillaQuick_item& pane, int display_row, int expected_mask)
{
    SQ_EXPECT(static_cast<int>(pane.send(SCI_MARKERGET, display_row)) == expected_mask);
}

void test_native_diff_row_markers_follow_side_state()
{
    const std::vector<DiffRow> rows{
        {0, -1, 1, 1, DiffSideState::Equal, DiffSideState::Equal},
        {1, 10, 2, -1, DiffSideState::Deleted, DiffSideState::Filler},
        {1, 10, 3, 2, DiffSideState::Changed, DiffSideState::Changed},
        {1, 10, -1, 3, DiffSideState::Filler, DiffSideState::Added},
    };

    ScintillaQuick_item left;
    ScintillaQuick_item right;
    left.setProperty("text", render_display_text(rows, DiffSide::Left));
    right.setProperty("text", render_display_text(rows, DiffSide::Right));

    configure_native_diff_row_markers(left);
    configure_native_diff_row_markers(right);
    apply_native_diff_row_markers(left, rows, DiffSide::Left);
    apply_native_diff_row_markers(right, rows, DiffSide::Right);

    expect_marker_bits(left, 0, 0);
    expect_marker_bits(left, 1, native_marker_mask(diff_marker_deleted));
    expect_marker_bits(left, 2, native_marker_mask(diff_marker_changed));
    expect_marker_bits(left, 3, native_marker_mask(diff_marker_filler));

    expect_marker_bits(right, 0, 0);
    expect_marker_bits(right, 1, native_marker_mask(diff_marker_filler));
    expect_marker_bits(right, 2, native_marker_mask(diff_marker_changed));
    expect_marker_bits(right, 3, native_marker_mask(diff_marker_added));
}

GreenPixelCoverage native_marker_line_green_pixel_coverage(int marker_symbol)
{
    QQuickWindow window;
    window.resize(360, 140);

    ScintillaQuick_item editor;
    editor.setParentItem(window.contentItem());
    editor.setPosition({0.0, 0.0});
    editor.setWidth(360);
    editor.setHeight(140);

    window.show();
    pump_events();

    editor.setProperty("font", scintillaquick::shared::deterministic_test_font(11));
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    editor.send(SCI_SETMARGINTYPEN, line_number_margin, SC_MARGIN_NUMBER);
    editor.send(SCI_SETMARGINWIDTHN, line_number_margin, line_number_margin_width);
    const int margin_count = static_cast<int>(editor.send(SCI_GETMARGINS));
    for (int margin = 0; margin < margin_count; ++margin) {
        editor.send(SCI_SETMARGINMASKN, margin, 0);
    }
    editor.send(SCI_STYLESETBACK, STYLE_DEFAULT, 0xFFFFFF);
    editor.send(SCI_STYLECLEARALL);
    editor.setProperty("text", QStringLiteral("before\nmarked line\nafter"));

    constexpr int marker_number = 0;
    constexpr int marked_line = 1;
    editor.send(SCI_MARKERDEFINE, marker_number, marker_symbol);
    editor.send(SCI_MARKERSETBACK, marker_number, 0x00FF00);
    editor.send(SCI_MARKERSETLAYER, marker_number, SC_LAYER_UNDER_TEXT);
    editor.send(SCI_MARKERADD, marked_line, marker_number);

    pump_events();
    QThread::msleep(20);
    pump_events();

    const QImage rendered = window.grabWindow();
    const int line_start = static_cast<int>(editor.send(SCI_POSITIONFROMLINE, marked_line));
    const int text_x =
        std::max(line_number_margin_width + 4, static_cast<int>(editor.send(SCI_POINTXFROMPOSITION, 0, line_start)));
    const int line_y = static_cast<int>(editor.send(SCI_POINTYFROMPOSITION, 0, line_start));
    const int line_height = static_cast<int>(editor.send(SCI_TEXTHEIGHT, marked_line));
    const int scan_width = rendered.width() - text_x - 4;

    constexpr int vertical_inset = 3;
    QImage marked_line_area;
    if (!rendered.isNull() && scan_width > 0 && line_height > vertical_inset * 2) {
        marked_line_area = rendered.copy(text_x, line_y + vertical_inset, scan_width, line_height - vertical_inset * 2);
    }

    SQ_EXPECT(!rendered.isNull());
    SQ_EXPECT(!marked_line_area.isNull());
    return image_region_green_pixel_coverage(marked_line_area, 0, marked_line_area.width());
}

bool native_marker_line_has_green_pixels(int marker_symbol)
{
    const GreenPixelCoverage coverage = native_marker_line_green_pixel_coverage(marker_symbol);
    return coverage.greenPixels >= 64;
}

bool native_marker_line_is_mostly_green(int marker_symbol)
{
    constexpr int minimum_green_percent = 80;
    const GreenPixelCoverage coverage = native_marker_line_green_pixel_coverage(marker_symbol);
    const bool mostly_green =
        coverage.totalPixels > 0 && coverage.greenPixels * 100 >= coverage.totalPixels * minimum_green_percent;
    if (!mostly_green) {
        std::fprintf(stderr, "native marker green coverage: %d/%d pixels\n", coverage.greenPixels, coverage.totalPixels);
    }
    return mostly_green;
}

void test_native_marker_line_highlight_candidates()
{
    // TortoiseDiff needs text-area line tints; background markers are captured but not rendered by Quick today.
    SQ_EXPECT(!native_marker_line_has_green_pixels(SC_MARK_BACKGROUND));
    SQ_EXPECT(native_marker_line_is_mostly_green(SC_MARK_FULLRECT));
}

class Tint_overlay final : public QQuickPaintedItem
{
  public:
    explicit Tint_overlay(bool filler, QQuickItem* parent = nullptr) : QQuickPaintedItem(parent), m_filler(filler)
    {
        setAcceptedMouseButtons(Qt::NoButton);
        setAntialiasing(false);
        setOpaquePainting(false);
        setZ(2.0);
    }

    void paint(QPainter* painter) override
    {
        painter->fillRect(QRectF(0, visible_row_tint_y, width(), visible_tint_height), QColor(0, 255, 0, 160));
        if (m_filler) {
            painter->fillRect(QRectF(0, visible_filler_y, width(), visible_tint_height), QColor(255, 0, 255, 160));
        }
    }

  private:
    bool m_filler = false;
};

void apply_synchronized_scroll(
    ScintillaQuick_item& left, ScintillaQuick_item& right, Scroll_axis axis, int value, bool& is_syncing)
{
    if (is_syncing) {
        return;
    }

    struct sync_guard
    {
        bool& value;

        explicit sync_guard(bool& guarded_value) : value(guarded_value)
        {
            value = true;
        }

        ~sync_guard()
        {
            value = false;
        }
    } guard(is_syncing);

    auto scroll_pane = [axis, value](ScintillaQuick_item& pane) {
        if (axis == Scroll_axis::Vertical) {
            if (pane.send(SCI_GETFIRSTVISIBLELINE) != value) {
                pane.scrollVertical(value);
            }
            return;
        }

        if (pane.send(SCI_GETXOFFSET) != value) {
            pane.scrollHorizontal(value);
        }
    };

    scroll_pane(left);
    scroll_pane(right);
}

void apply_synchronized_zoom(ScintillaQuick_item& target, int value, bool& is_syncing)
{
    if (is_syncing) {
        return;
    }

    struct sync_guard
    {
        bool& value;

        explicit sync_guard(bool& guarded_value) : value(guarded_value)
        {
            value = true;
        }

        ~sync_guard()
        {
            value = false;
        }
    } guard(is_syncing);

    if (target.send(SCI_GETZOOM) != value) {
        target.send(SCI_SETZOOM, value);
    }
}

void test_two_readonly_panes_in_one_window()
{
    QQuickWindow window;
    window.resize(800, 360);

    QQuickItem left_container;
    QQuickItem right_container;
    ScintillaQuick_item left;
    ScintillaQuick_item right;

    left_container.setParentItem(window.contentItem());
    right_container.setParentItem(window.contentItem());
    left.setParentItem(&left_container);
    right.setParentItem(&right_container);

    left_container.setPosition({0.0, 0.0});
    left_container.setWidth(400);
    left_container.setHeight(360);
    left.setPosition({0.0, 0.0});
    left.setWidth(400);
    left.setHeight(360);

    right_container.setPosition({400.0, 0.0});
    right_container.setWidth(400);
    right_container.setHeight(360);
    right.setPosition({0.0, 0.0});
    right.setWidth(400);
    right.setHeight(360);

    Tint_overlay left_overlay(true, &left_container);
    Tint_overlay right_overlay(false, &right_container);
    left_overlay.setPosition({0.0, 0.0});
    right_overlay.setPosition({0.0, 0.0});
    left_overlay.setSize(left_container.size());
    right_overlay.setSize(right_container.size());

    bool scroll_syncing = false;
    bool zoom_syncing = false;
    QObject::connect(&left, &ScintillaQuick_item::verticalScrolled, &right, [&](int value) {
        apply_synchronized_scroll(left, right, Scroll_axis::Vertical, value, scroll_syncing);
    });
    QObject::connect(&right, &ScintillaQuick_item::verticalScrolled, &left, [&](int value) {
        apply_synchronized_scroll(left, right, Scroll_axis::Vertical, value, scroll_syncing);
    });
    QObject::connect(&left, &ScintillaQuick_item::horizontalScrolled, &right, [&](int value) {
        apply_synchronized_scroll(left, right, Scroll_axis::Horizontal, value, scroll_syncing);
    });
    QObject::connect(&right, &ScintillaQuick_item::horizontalScrolled, &left, [&](int value) {
        apply_synchronized_scroll(left, right, Scroll_axis::Horizontal, value, scroll_syncing);
    });
    QObject::connect(&left, &ScintillaQuick_item::zoom, &right, [&](int value) {
        apply_synchronized_zoom(right, value, zoom_syncing);
    });
    QObject::connect(&right, &ScintillaQuick_item::zoom, &left, [&](int value) {
        apply_synchronized_zoom(left, value, zoom_syncing);
    });

    const QString left_text = sample_text(QStringLiteral("left"), visible_filler_line + 1);
    const QString right_text = sample_text(QStringLiteral("right"));

    window.show();
    pump_events();
    configure_pane(left, left_text);
    configure_pane(right, right_text);
    pump_events();
    QThread::msleep(20);
    pump_events();

    SQ_EXPECT(left.window() == &window);
    SQ_EXPECT(right.window() == &window);
    SQ_EXPECT(left.property("text").toString() == left_text);
    SQ_EXPECT(right.property("text").toString() == right_text);
    SQ_EXPECT(left.property("readonly").toBool());
    SQ_EXPECT(right.property("readonly").toBool());
    SQ_EXPECT(left.send(SCI_GETREADONLY) != 0);
    SQ_EXPECT(right.send(SCI_GETREADONLY) != 0);
    SQ_EXPECT(left.send(SCI_GETWRAPMODE) == SC_WRAP_NONE);
    SQ_EXPECT(right.send(SCI_GETWRAPMODE) == SC_WRAP_NONE);
    SQ_EXPECT(left.send(SCI_GETMARGINTYPEN, line_number_margin) == SC_MARGIN_NUMBER);
    SQ_EXPECT(right.send(SCI_GETMARGINTYPEN, line_number_margin) == SC_MARGIN_NUMBER);
    SQ_EXPECT(left.send(SCI_GETMARGINWIDTHN, line_number_margin) == line_number_margin_width);
    SQ_EXPECT(right.send(SCI_GETMARGINWIDTHN, line_number_margin) == line_number_margin_width);

    constexpr int vertical_target = 18;
    left.send(SCI_SETFIRSTVISIBLELINE, vertical_target);
    pump_events();
    const int left_first_visible_line = static_cast<int>(left.send(SCI_GETFIRSTVISIBLELINE));
    const int right_first_visible_line = static_cast<int>(right.send(SCI_GETFIRSTVISIBLELINE));
    SQ_EXPECT(left_first_visible_line == vertical_target);
    SQ_EXPECT(right_first_visible_line == vertical_target);

    constexpr int horizontal_target = 96;
    left.scrollHorizontal(horizontal_target);
    pump_events();
    const int left_x_offset = static_cast<int>(left.send(SCI_GETXOFFSET));
    const int right_x_offset = static_cast<int>(right.send(SCI_GETXOFFSET));
    SQ_EXPECT(left.send(SCI_GETSCROLLWIDTH) > left.width());
    SQ_EXPECT(left_x_offset == horizontal_target);
    SQ_EXPECT(right_x_offset == horizontal_target);

    constexpr int external_vertical_target = 7;
    constexpr int external_horizontal_target = 32;
    apply_synchronized_scroll(left, right, Scroll_axis::Vertical, external_vertical_target, scroll_syncing);
    apply_synchronized_scroll(left, right, Scroll_axis::Horizontal, external_horizontal_target, scroll_syncing);
    pump_events();
    SQ_EXPECT(left.send(SCI_GETFIRSTVISIBLELINE) == external_vertical_target);
    SQ_EXPECT(right.send(SCI_GETFIRSTVISIBLELINE) == external_vertical_target);
    SQ_EXPECT(left.send(SCI_GETXOFFSET) == external_horizontal_target);
    SQ_EXPECT(right.send(SCI_GETXOFFSET) == external_horizontal_target);

    left.send(SCI_SETZOOM, 2);
    pump_events();
    SQ_EXPECT(left.send(SCI_GETZOOM) == 2);
    SQ_EXPECT(right.send(SCI_GETZOOM) == 2);

    right.send(SCI_SETZOOM, -1);
    pump_events();
    SQ_EXPECT(left.send(SCI_GETZOOM) == -1);
    SQ_EXPECT(right.send(SCI_GETZOOM) == -1);

    const QImage rendered = window.grabWindow();
    SQ_EXPECT(image_region_has_dark_pixels(rendered, 0, rendered.width() / 2));
    SQ_EXPECT(image_region_has_dark_pixels(rendered, rendered.width() / 2, rendered.width()));
    SQ_EXPECT(image_region_has_green_pixels(rendered, 0, rendered.width() / 2));
    SQ_EXPECT(image_region_has_green_pixels(rendered, rendered.width() / 2, rendered.width()));
    SQ_EXPECT(image_region_has_magenta_pixels(rendered, 0, rendered.width() / 2));
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        std::fprintf(stderr, "%s\n", qPrintable(font_error));
        return 1;
    }

    test_display_row_model_changed_blocks();
    test_hunk_navigation_model();
    test_active_hunk_from_display_rows();
    test_active_hunk_boundary_thickness_model();
    test_raw_text_line_diff_adapter();
    test_inline_changed_text_spans();
    test_stored_unified_diff_adapter();
    test_live_command_diff_adapter_selection();
    test_widget_input_contract_validation();
    test_source_side_copy_strips_filler_rows();
    test_apply_active_hunk_left_to_right_and_right_to_left();
    test_apply_active_display_row_range();
    test_vertical_scrollbar_model_mapping();
    test_horizontal_scrollbar_model_mapping();
    test_native_marker_line_highlight_candidates();
    test_native_diff_row_markers_follow_side_state();
    test_two_readonly_panes_in_one_window();

    if (g_failures != 0) {
        std::fprintf(stderr, "scintillaquick_tortoisediff_step1_test: %d failure(s)\n", g_failures);
        return 1;
    }

    return 0;
}
