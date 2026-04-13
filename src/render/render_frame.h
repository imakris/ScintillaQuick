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

enum class text_direction_t
{
    left_to_right,
    right_to_left,
    mixed,
};

struct capture_text_run_t
{
    std::string text;
    double x = 0.0;
    double width = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    double blob_text_left = 0.0;
    double blob_text_top = 0.0;
    double blob_text_right = 0.0;
    double blob_text_bottom = 0.0;
    double blob_outer_left = 0.0;
    double blob_outer_top = 0.0;
    double blob_outer_right = 0.0;
    double blob_outer_bottom = 0.0;
    double blob_inner_left = 0.0;
    double blob_inner_top = 0.0;
    double blob_inner_right = 0.0;
    double blob_inner_bottom = 0.0;
    double baseline_y = 0.0;
    int style_id = 0;
    QColor foreground;
    QColor blob_outer;
    QColor blob_inner;
    text_direction_t direction = text_direction_t::left_to_right;
    bool is_represented_text = false;
    bool represented_as_blob = false;
};

struct capture_visual_line_t
{
    int document_line = 0;
    int subline_index = 0;
    int visual_order = 0;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double baseline_y = 0.0;
    std::vector<capture_text_run_t> text_runs;
};

struct capture_selection_primitive_t
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    std::uint32_t rgba = 0;
    bool is_main = false;
};

struct capture_caret_primitive_t
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    std::uint32_t rgba = 0;
    bool is_main = false;
};

struct capture_indicator_primitive_t
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double line_top = 0.0;
    double line_bottom = 0.0;
    double character_left = 0.0;
    double character_top = 0.0;
    double character_right = 0.0;
    double character_bottom = 0.0;
    double stroke_width = 1.0;
    int fill_alpha = 30;
    int outline_alpha = 50;
    std::uint32_t rgba = 0;
    int indicator_number = 0;
    int indicator_style = 0;
    bool under_text = false;
    bool is_main = false;
};

struct capture_current_line_primitive_t
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    std::uint32_t rgba = 0;
    bool framed = false;
};

struct capture_marker_primitive_t
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    int marker_number = 0;
    int marker_type = 0;
    std::uint32_t fore_rgba = 0;
    std::uint32_t back_rgba = 0;
    std::uint32_t back_rgba_selected = 0;
    int document_line = 0;
    int fold_part = 0; // 0=undefined, 1=head, 2=body, 3=tail, 4=headWithTail
};

struct capture_margin_text_primitive_t
{
    std::string text;
    double x = 0.0;
    double y = 0.0;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double baseline_y = 0.0;
    int document_line = 0;
    int subline_index = 0;
    int style_id = 0;
};

struct capture_fold_display_text_t
{
    std::string text;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double baseline_y = 0.0;
    int style_id = 0;
    std::uint32_t fore_rgba = 0;
    std::uint32_t back_rgba = 0;
    int document_line = 0;
    bool boxed = false;
};

struct capture_eol_annotation_t
{
    std::string text;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double text_left = 0.0;
    double baseline_y = 0.0;
    int style_id = 0;
    std::uint32_t fore_rgba = 0;
    std::uint32_t back_rgba = 0;
    int document_line = 0;
    int visible_style = 0;
};

struct capture_annotation_t
{
    std::string text;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double text_left = 0.0;
    double baseline_y = 0.0;
    int style_id = 0;
    std::uint32_t fore_rgba = 0;
    std::uint32_t back_rgba = 0;
    int document_line = 0;
    int annotation_line = 0;
    bool boxed = false;
};

enum class whitespace_mark_kind_t
{
    space_dot,
    tab_arrow,
};

struct capture_whitespace_mark_t
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double mid_y = 0.0;
    std::uint32_t rgba = 0;
    whitespace_mark_kind_t kind = whitespace_mark_kind_t::space_dot;
};

enum class decoration_kind_t
{
    hotspot,
    style_underline,
};

struct capture_decoration_underline_t
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    std::uint32_t rgba = 0;
    decoration_kind_t kind = decoration_kind_t::style_underline;
};

struct capture_indent_guide_t
{
    double x = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    std::uint32_t rgba = 0;
    bool highlight = false;
};

struct Capture_frame
{
    double viewport_width = 0.0;
    double viewport_height = 0.0;
    double text_left = 0.0;
    double text_top = 0.0;
    double text_width = 0.0;
    double text_height = 0.0;
    double margin_left = 0.0;
    double margin_top = 0.0;
    double margin_width = 0.0;
    double margin_height = 0.0;
    std::vector<capture_visual_line_t> visual_lines;
    std::vector<capture_selection_primitive_t> selection_primitives;
    std::vector<capture_caret_primitive_t> caret_primitives;
    std::vector<capture_indicator_primitive_t> indicator_primitives;
    std::vector<capture_current_line_primitive_t> current_line_primitives;
    std::vector<capture_marker_primitive_t> marker_primitives;
    std::vector<capture_margin_text_primitive_t> margin_text_primitives;
    std::vector<capture_fold_display_text_t> fold_display_texts;
    std::vector<capture_eol_annotation_t> eol_annotations;
    std::vector<capture_annotation_t> annotations;
    std::vector<capture_whitespace_mark_t> whitespace_marks;
    std::vector<capture_decoration_underline_t> decoration_underlines;
    std::vector<capture_indent_guide_t> indent_guides;
};

struct visual_line_key_t
{
    int document_line = 0;
    int subline_index = 0;

    friend bool operator==(const visual_line_key_t &a, const visual_line_key_t &b) noexcept
    {
        return a.document_line == b.document_line && a.subline_index == b.subline_index;
    }
    friend bool operator!=(const visual_line_key_t &a, const visual_line_key_t &b) noexcept
    {
        return !(a == b);
    }
};

struct text_run_t
{
    QString text;
    QPointF position;
    qreal width = 0.0;
    qreal top = 0.0;
    qreal bottom = 0.0;
    QRectF blob_text_clip_rect;
    QRectF blob_outer_rect;
    QRectF blob_inner_rect;
    QColor foreground;
    QColor blob_outer;
    QColor blob_inner;
    QFont font;
    int style_id = 0;
    text_direction_t direction = text_direction_t::left_to_right;
    bool is_represented_text = false;
    bool represented_as_blob = false;
};

struct visual_line_frame_t
{
    visual_line_key_t key;
    int visual_order = 0;
    QPointF origin;
    qreal baseline_y = 0.0;
    QRectF clip_rect;
    std::vector<text_run_t> text_runs;
};

struct Selection_primitive
{
    QRectF rect;
    QColor color;
    bool is_main = false;
};

struct caret_primitive_t
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
    qreal stroke_width = 1.0;
    int fill_alpha = 30;
    int outline_alpha = 50;
    int indicator_number = 0;
    int indicator_style = 0;
    bool under_text = false;
    bool is_main = false;
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
    int marker_type = 0;
    QColor foreground;
    QColor background;
    QColor background_selected;
    int document_line = 0;
    int fold_part = 0;
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
    int style_id = 0;
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
    int style_id = 0;
    bool boxed = false;
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
    int style_id = 0;
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
    int document_line = 0;
    int annotation_line = 0;
    int style_id = 0;
    bool boxed = false;
};

struct Whitespace_mark_primitive
{
    QRectF rect;
    qreal mid_y = 0.0;
    QColor color;
    whitespace_mark_kind_t kind = whitespace_mark_kind_t::space_dot;
};

struct Decoration_underline_primitive
{
    QRectF rect;
    QColor color;
    decoration_kind_t kind = decoration_kind_t::style_underline;
};

struct Indent_guide_primitive
{
    qreal x = 0.0;
    qreal top = 0.0;
    qreal bottom = 0.0;
    QColor color;
    bool highlight = false;
};

struct Render_frame
{
    QRectF text_rect;
    QRectF margin_rect;
    std::vector<visual_line_frame_t> visual_lines;
    std::vector<Selection_primitive> selection_primitives;
    std::vector<caret_primitive_t> caret_primitives;
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
