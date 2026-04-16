// Scintilla source code edit control
/** @file RenderCapture.h
 ** Phase 1 capture types and collector interface.
 **/
// Copyright 2026 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#ifndef RENDERCAPTURE_H
#define RENDERCAPTURE_H

#include <cstdint>
#include <string>

namespace Scintilla::Internal {

enum class Capture_text_direction {
	left_to_right,
	right_to_left,
	mixed
};

struct Captured_visual_line {
	int document_line = 0;
	int subline_index = 0;
	int visual_order = 0;
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	float baseline_y = 0.0f;
};

struct Captured_text_run {
	std::string utf8_text;
	int style_id = 0;
	std::uint32_t foreground_rgba = 0;
	std::uint32_t blob_outer_rgba = 0;
	std::uint32_t blob_inner_rgba = 0;
	float x = 0.0f;
	float width = 0.0f;
	float top = 0.0f;
	float bottom = 0.0f;
	float blob_text_left = 0.0f;
	float blob_text_top = 0.0f;
	float blob_text_right = 0.0f;
	float blob_text_bottom = 0.0f;
	float blob_outer_left = 0.0f;
	float blob_outer_top = 0.0f;
	float blob_outer_right = 0.0f;
	float blob_outer_bottom = 0.0f;
	float blob_inner_left = 0.0f;
	float blob_inner_top = 0.0f;
	float blob_inner_right = 0.0f;
	float blob_inner_bottom = 0.0f;
	float baseline_y = 0.0f;
	Capture_text_direction direction = Capture_text_direction::left_to_right;
	bool is_represented_text = false;
	bool represented_as_blob = false;
};

struct Captured_selection_rect {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	std::uint32_t rgba = 0;
	bool is_main = false;
};

struct Captured_caret_rect {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	std::uint32_t rgba = 0;
	bool is_main = false;
};

struct Captured_indicator {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	float line_top = 0.0f;
	float line_bottom = 0.0f;
	float character_left = 0.0f;
	float character_top = 0.0f;
	float character_right = 0.0f;
	float character_bottom = 0.0f;
	float stroke_width = 1.0f;
	int fill_alpha = 30;
	int outline_alpha = 50;
	std::uint32_t rgba = 0;
	int indicator_number = 0;
	int indicator_style = 0;
	bool under_text = false;
	bool is_main = false;
};

struct Captured_current_line_highlight {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	std::uint32_t rgba = 0;
	bool framed = false;
};

// Fold-part semantics from LineMarker::FoldPart, encoded as int.
// 0=undefined, 1=head, 2=body, 3=tail, 4=headWithTail.
struct Captured_marker_symbol {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	int marker_number = 0;
	int marker_type = 0;
	std::uint32_t fore_rgba = 0;
	std::uint32_t back_rgba = 0;
	std::uint32_t back_rgba_selected = 0;
	int document_line = 0;
	int fold_part = 0;
};

struct Captured_margin_text {
	float x = 0.0f;
	float y = 0.0f;
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	float baseline_y = 0.0f;
	int document_line = 0;
	int subline_index = 0;
	std::string utf8_text;
	int style_id = 0;
};

struct Captured_fold_display_text {
	std::string utf8_text;
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	float baseline_y = 0.0f;
	int style_id = 0;
	std::uint32_t fore_rgba = 0;
	std::uint32_t back_rgba = 0;
	int document_line = 0;
	bool boxed = false;
};

struct Captured_eol_annotation {
	std::string utf8_text;
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	float text_left = 0.0f;
	float baseline_y = 0.0f;
	int style_id = 0;
	std::uint32_t fore_rgba = 0;
	std::uint32_t back_rgba = 0;
	int document_line = 0;
	int visible_style = 0;
};

struct Captured_annotation {
	std::string utf8_text;
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	float text_left = 0.0f;
	float baseline_y = 0.0f;
	int style_id = 0;
	std::uint32_t fore_rgba = 0;
	std::uint32_t back_rgba = 0;
	int document_line = 0;
	int annotation_line = 0;
	bool boxed = false;
};

enum class Whitespace_mark_kind {
	space_dot,
	tab_arrow,
};

struct Captured_whitespace_mark {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	float mid_y = 0.0f;
	std::uint32_t rgba = 0;
	Whitespace_mark_kind kind = Whitespace_mark_kind::space_dot;
};

enum class Decoration_kind {
	hotspot,
	style_underline,
};

struct Captured_decoration_underline {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	std::uint32_t rgba = 0;
	Decoration_kind kind = Decoration_kind::style_underline;
};

struct Captured_indent_guide {
	float x = 0.0f;
	float top = 0.0f;
	float bottom = 0.0f;
	std::uint32_t rgba = 0;
	bool highlight = false;
};

class Render_collector {
public:
	virtual ~Render_collector() = default;
	virtual bool wants_static_content() const { return true; }

	virtual void begin_visual_line(const Captured_visual_line &line) = 0;
	virtual void add_text_run(const Captured_text_run &run) = 0;
	virtual void add_selection_rect(const Captured_selection_rect &rect) = 0;
	virtual void add_caret_rect(const Captured_caret_rect &rect) = 0;
	virtual void add_indicator_primitive(const Captured_indicator &indicator) = 0;
	virtual void add_current_line_highlight(const Captured_current_line_highlight &highlight) = 0;
	virtual void add_marker_symbol(const Captured_marker_symbol &marker) = 0;
	virtual void add_margin_text(const Captured_margin_text &text) = 0;
	virtual void add_fold_display_text(const Captured_fold_display_text &text) = 0;
	virtual void add_eol_annotation(const Captured_eol_annotation &annotation) = 0;
	virtual void add_annotation(const Captured_annotation &annotation) = 0;
	virtual void add_whitespace_mark(const Captured_whitespace_mark &mark) = 0;
	virtual void add_decoration_underline(const Captured_decoration_underline &underline) = 0;
	virtual void add_indent_guide(const Captured_indent_guide &guide) = 0;
	virtual void end_visual_line() = 0;
};

}

#endif
