// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace Scintilla::Internal {

enum class Text_direction
{
    left_to_right,
    right_to_left,
    mixed,
};

struct Capture_text_run
{
    std::string text;
    double x                 = 0.0;
    double width             = 0.0;
    double top               = 0.0;
    double bottom            = 0.0;
    double blob_text_left    = 0.0;
    double blob_text_top     = 0.0;
    double blob_text_right   = 0.0;
    double blob_text_bottom  = 0.0;
    double blob_outer_left   = 0.0;
    double blob_outer_top    = 0.0;
    double blob_outer_right  = 0.0;
    double blob_outer_bottom = 0.0;
    double blob_inner_left   = 0.0;
    double blob_inner_top    = 0.0;
    double blob_inner_right  = 0.0;
    double blob_inner_bottom = 0.0;
    double baseline_y        = 0.0;
    int style_id             = 0;
    QColor foreground;
    QColor blob_outer;
    QColor blob_inner;
    Text_direction direction = Text_direction::left_to_right;
    bool is_represented_text = false;
    bool represented_as_blob = false;
};

struct Capture_visual_line
{
    int document_line = 0;
    int subline_index = 0;
    int visual_order  = 0;
    double left       = 0.0;
    double top        = 0.0;
    double right      = 0.0;
    double bottom     = 0.0;
    double baseline_y = 0.0;
    std::vector<Capture_text_run> text_runs;
};

struct Capture_selection_primitive
{
    double left        = 0.0;
    double top         = 0.0;
    double right       = 0.0;
    double bottom      = 0.0;
    std::uint32_t rgba = 0;
    bool is_main       = false;
};

struct Capture_caret_primitive
{
    double left        = 0.0;
    double top         = 0.0;
    double right       = 0.0;
    double bottom      = 0.0;
    std::uint32_t rgba = 0;
    bool is_main       = false;
};

struct Capture_indicator_primitive
{
    double left             = 0.0;
    double top              = 0.0;
    double right            = 0.0;
    double bottom           = 0.0;
    double line_top         = 0.0;
    double line_bottom      = 0.0;
    double character_left   = 0.0;
    double character_top    = 0.0;
    double character_right  = 0.0;
    double character_bottom = 0.0;
    double stroke_width     = 1.0;
    int fill_alpha          = 30;
    int outline_alpha       = 50;
    std::uint32_t rgba      = 0;
    int indicator_number    = 0;
    int indicator_style     = 0;
    bool under_text         = false;
    bool is_main            = false;
};

struct Capture_current_line_primitive
{
    double left        = 0.0;
    double top         = 0.0;
    double right       = 0.0;
    double bottom      = 0.0;
    std::uint32_t rgba = 0;
    bool framed        = false;
};

struct Capture_marker_primitive
{
    double left                      = 0.0;
    double top                       = 0.0;
    double right                     = 0.0;
    double bottom                    = 0.0;
    int marker_number                = 0;
    int marker_type                  = 0;
    std::uint32_t fore_rgba          = 0;
    std::uint32_t back_rgba          = 0;
    std::uint32_t back_rgba_selected = 0;
    int document_line                = 0;
    int fold_part = 0; // 0=undefined, 1=head, 2=body, 3=tail, 4=headWithTail
};

struct Capture_margin_text_primitive
{
    std::string text;
    double x          = 0.0;
    double y          = 0.0;
    double left       = 0.0;
    double top        = 0.0;
    double right      = 0.0;
    double bottom     = 0.0;
    double baseline_y = 0.0;
    int document_line = 0;
    int subline_index = 0;
    int style_id      = 0;
};

struct Capture_fold_display_text
{
    std::string text;
    double left             = 0.0;
    double top              = 0.0;
    double right            = 0.0;
    double bottom           = 0.0;
    double baseline_y       = 0.0;
    int style_id            = 0;
    std::uint32_t fore_rgba = 0;
    std::uint32_t back_rgba = 0;
    int document_line       = 0;
    bool boxed              = false;
};

struct Capture_eol_annotation
{
    std::string text;
    double left             = 0.0;
    double top              = 0.0;
    double right            = 0.0;
    double bottom           = 0.0;
    double text_left        = 0.0;
    double baseline_y       = 0.0;
    int style_id            = 0;
    std::uint32_t fore_rgba = 0;
    std::uint32_t back_rgba = 0;
    int document_line       = 0;
    int visible_style       = 0;
};

struct Capture_annotation
{
    std::string text;
    double left             = 0.0;
    double top              = 0.0;
    double right            = 0.0;
    double bottom           = 0.0;
    double text_left        = 0.0;
    double baseline_y       = 0.0;
    int style_id            = 0;
    std::uint32_t fore_rgba = 0;
    std::uint32_t back_rgba = 0;
    int document_line       = 0;
    int annotation_line     = 0;
    bool boxed              = false;
};

enum class Whitespace_mark_kind_t
{
    space_dot,
    tab_arrow,
};

struct Capture_whitespace_mark
{
    double left               = 0.0;
    double top                = 0.0;
    double right              = 0.0;
    double bottom             = 0.0;
    double mid_y              = 0.0;
    std::uint32_t rgba        = 0;
    Whitespace_mark_kind_t kind = Whitespace_mark_kind_t::space_dot;
};

enum class Decoration_kind_t
{
    hotspot,
    style_underline,
};

struct Capture_decoration_underline
{
    double left          = 0.0;
    double top           = 0.0;
    double right         = 0.0;
    double bottom        = 0.0;
    std::uint32_t rgba   = 0;
    Decoration_kind_t kind = Decoration_kind_t::style_underline;
};

struct Capture_indent_guide
{
    double x           = 0.0;
    double top         = 0.0;
    double bottom      = 0.0;
    std::uint32_t rgba = 0;
    bool highlight     = false;
};

struct Captured_frame
{
    double viewport_width  = 0.0;
    double viewport_height = 0.0;
    double text_left       = 0.0;
    double text_top        = 0.0;
    double text_width      = 0.0;
    double text_height     = 0.0;
    double margin_left     = 0.0;
    double margin_top      = 0.0;
    double margin_width    = 0.0;
    double margin_height   = 0.0;
    std::vector<Capture_visual_line> visual_lines;
    std::vector<Capture_selection_primitive> selection_primitives;
    std::vector<Capture_caret_primitive> caret_primitives;
    std::vector<Capture_indicator_primitive> indicator_primitives;
    std::vector<Capture_current_line_primitive> current_line_primitives;
    std::vector<Capture_marker_primitive> marker_primitives;
    std::vector<Capture_margin_text_primitive> margin_text_primitives;
    std::vector<Capture_fold_display_text> fold_display_texts;
    std::vector<Capture_eol_annotation> eol_annotations;
    std::vector<Capture_annotation> annotations;
    std::vector<Capture_whitespace_mark> whitespace_marks;
    std::vector<Capture_decoration_underline> decoration_underlines;
    std::vector<Capture_indent_guide> indent_guides;
};

struct Visual_line_key
{
    int document_line = 0;
    int subline_index = 0;

    friend bool operator==(const Visual_line_key &a, const Visual_line_key &b) noexcept
    {
        return a.document_line == b.document_line && a.subline_index == b.subline_index;
    }
    friend bool operator!=(const Visual_line_key &a, const Visual_line_key &b) noexcept
    {
        return !(a == b);
    }
};

struct Text_run
{
    QString text;
    QPointF position;
    qreal width  = 0.0;
    qreal top    = 0.0;
    qreal bottom = 0.0;
    QRectF blob_text_clip_rect;
    QRectF blob_outer_rect;
    QRectF blob_inner_rect;
    QColor foreground;
    QColor blob_outer;
    QColor blob_inner;
    QFont font;
    int style_id             = 0;
    Text_direction direction = Text_direction::left_to_right;
    bool is_represented_text = false;
    bool represented_as_blob = false;
};

struct Visual_line_frame
{
    Visual_line_key key;
    int visual_order = 0;
    QPointF origin;
    qreal baseline_y = 0.0;
    QRectF clip_rect;
    std::vector<Text_run> text_runs;
};

struct Selection_primitive
{
    QRectF rect;
    QColor color;
    bool is_main = false;
};

struct Caret_primitive
{
    QRectF rect;
    QColor color;
    bool is_main = false;
};

struct Indicator_primitive
{
    QRectF rect;
    QRectF line_rect;
    QRectF character_rect;
    QColor color;
    qreal stroke_width   = 1.0;
    int fill_alpha       = 30;
    int outline_alpha    = 50;
    int indicator_number = 0;
    int indicator_style  = 0;
    bool under_text      = false;
    bool is_main         = false;
};

struct Current_line_primitive
{
    QRectF rect;
    QColor color;
    bool framed = false;
};

struct Marker_primitive
{
    QRectF rect;
    int marker_number = 0;
    int marker_type   = 0;
    QColor foreground;
    QColor background;
    QColor background_selected;
    int document_line = 0;
    int fold_part     = 0;
};

struct Margin_text_primitive
{
    QString text;
    QPointF position;
    qreal baseline_y = 0.0;
    QColor foreground;
    QFont font;
    QRectF clip_rect;
    int document_line = 0;
    int subline_index = 0;
    int style_id      = 0;
};

struct Fold_display_text_primitive
{
    QString text;
    QPointF position;
    qreal baseline_y = 0.0;
    QRectF rect;
    QColor foreground;
    QColor background;
    QFont font;
    int document_line = 0;
    int style_id      = 0;
    bool boxed        = false;
};

struct Eol_annotation_primitive
{
    QString text;
    QPointF position;
    qreal baseline_y = 0.0;
    QRectF rect;
    QColor foreground;
    QColor background;
    QFont font;
    int document_line = 0;
    int style_id      = 0;
    int visible_style = 0;
};

struct Annotation_primitive
{
    QString text;
    QPointF position;
    qreal baseline_y = 0.0;
    QRectF rect;
    QColor foreground;
    QColor background;
    QFont font;
    int document_line   = 0;
    int annotation_line = 0;
    int style_id        = 0;
    bool boxed          = false;
};

struct Whitespace_mark_primitive
{
    QRectF rect;
    qreal mid_y = 0.0;
    QColor color;
    Whitespace_mark_kind_t kind = Whitespace_mark_kind_t::space_dot;
};

struct Decoration_underline_primitive
{
    QRectF rect;
    QColor color;
    Decoration_kind_t kind = Decoration_kind_t::style_underline;
};

struct Indent_guide_primitive
{
    qreal x      = 0.0;
    qreal top    = 0.0;
    qreal bottom = 0.0;
    QColor color;
    bool highlight = false;
};

struct Render_frame
{
    QRectF text_rect;
    QRectF margin_rect;
    std::vector<Visual_line_frame> visual_lines;
    std::vector<Selection_primitive> selection_primitives;
    std::vector<Caret_primitive> caret_primitives;
    std::vector<Indicator_primitive> indicator_primitives;
    std::vector<Current_line_primitive> current_line_primitives;
    std::vector<Marker_primitive> marker_primitives;
    std::vector<Margin_text_primitive> margin_text_primitives;
    std::vector<Fold_display_text_primitive> fold_display_texts;
    std::vector<Eol_annotation_primitive> eol_annotations;
    std::vector<Annotation_primitive> annotations;
    std::vector<Whitespace_mark_primitive> whitespace_marks;
    std::vector<Decoration_underline_primitive> decoration_underlines;
    std::vector<Indent_guide_primitive> indent_guides;
};

}
