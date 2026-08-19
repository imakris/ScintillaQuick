// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_item.h>

#include <QFontMetricsF>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace
{

int scintilla_color(const QColor& color)
{
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

Scintilla::Position document_length(const ScintillaQuick_item& editor)
{
    return static_cast<Scintilla::Position>(editor.send(SCI_GETLENGTH));
}

Scintilla::Position search_range(
    ScintillaQuick_item& editor, const QByteArray& needle, Scintilla::Position start, Scintilla::Position end)
{
    editor.send(SCI_SETTARGETRANGE, start, end);
    editor.send(SCI_SETSEARCHFLAGS, static_cast<Scintilla::uptr_t>(editor.findOptions()));
    return static_cast<Scintilla::Position>(
        editor.sends(SCI_SEARCHINTARGET, static_cast<Scintilla::uptr_t>(needle.size()), needle.constData()));
}

std::vector<std::pair<Scintilla::Position, Scintilla::Position>> all_matches(
    ScintillaQuick_item& editor, const QByteArray& needle)
{
    std::vector<std::pair<Scintilla::Position, Scintilla::Position>> matches;
    Scintilla::Position search_start = 0;

    while (search_start <= document_length(editor)) {
        const Scintilla::Position match = search_range(editor, needle, search_start, document_length(editor));
        if (match < 0) {
            break;
        }

        const Scintilla::Position match_end = static_cast<Scintilla::Position>(editor.send(SCI_GETTARGETEND));
        matches.emplace_back(match, match_end);

        if (match_end > match) {
            search_start = match_end;
        }
        else {
            const Scintilla::Position next = static_cast<Scintilla::Position>(editor.send(SCI_POSITIONAFTER, match));
            if (next <= match) {
                break;
            }
            search_start = next;
        }
    }

    return matches;
}

bool selection_is_find_match(ScintillaQuick_item& editor, const QByteArray& needle, bool allow_empty)
{
    const Scintilla::Position selection_start = static_cast<Scintilla::Position>(editor.send(SCI_GETSELECTIONSTART));
    const Scintilla::Position selection_end = static_cast<Scintilla::Position>(editor.send(SCI_GETSELECTIONEND));
    if (selection_start == selection_end) {
        if (!allow_empty) {
            return false;
        }
        const Scintilla::Position match = search_range(editor, needle, selection_start, document_length(editor));
        return match == selection_start &&
               static_cast<Scintilla::Position>(editor.send(SCI_GETTARGETEND)) == selection_end;
    }

    const Scintilla::Position match = search_range(editor, needle, selection_start, selection_end);
    return match == selection_start && static_cast<Scintilla::Position>(editor.send(SCI_GETTARGETEND)) == selection_end;
}

} // namespace

class ScintillaQuick_item::Find_panel final : public QQuickItem
{
    struct Field_visual_state
    {
        QRectF rect;
        QString text;
        Scintilla::Position caret = 0;
        Scintilla::Position selection_start = 0;
        Scintilla::Position selection_end = 0;
        int x_offset = 0;
        bool active_focus = false;
    };

  public:
    explicit Find_panel(ScintillaQuick_item* owner)
        : QQuickItem(owner), m_owner(owner), m_find_field(new Find_field(this, false)),
          m_replace_field(new Find_field(this, true))
    {
        setAcceptedMouseButtons(Qt::LeftButton);
        setAcceptHoverEvents(true);
        setClip(true);
        setFlag(QQuickItem::ItemHasContents, true);
        setZ(2000.0);
        setVisible(false);
        setObjectName(QStringLiteral("scintillaquickFindPanel"));

        connect_field(m_find_field, false);
        connect_field(m_replace_field, true);
        QObject::connect(owner, &ScintillaQuick_item::readonlyChanged, this, [this]() {
            syncFromOwner();
        });
        syncFromOwner();
    }

    qreal preferredHeight() const
    {
        const QFontMetricsF metrics(m_panel_font);
        const qreal row_height = std::max<qreal>(28.0, std::ceil(metrics.height()) + 10.0);
        return row_height * (m_replace_mode ? 2.0 : 1.0);
    }

    void relayout()
    {
        const QFontMetricsF metrics(m_panel_font);
        m_row_height = std::max<qreal>(28.0, std::ceil(metrics.height()) + 10.0);
        const qreal label_width =
            std::max<qreal>(94.0, metrics.horizontalAdvance(QStringLiteral("Replace with:")) + 12.0);
        const qreal padding = 10.0;

        const std::array<QString, 6> labels = {
            QStringLiteral("Find Previous"),
            QStringLiteral("Find Next"),
            QStringLiteral("Select All"),
            QStringLiteral("Replace"),
            QStringLiteral("Replace & Find"),
            QStringLiteral("Replace All"),
        };
        std::array<qreal, 6> button_widths{};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            button_widths[index] = std::ceil(metrics.horizontalAdvance(labels[index])) + padding * 2.0;
        }

        const qreal find_button_width = button_widths[0] + button_widths[1] + button_widths[2];
        const qreal replace_button_width = button_widths[3] + button_widths[4] + button_widths[5];
        const qreal shared_button_width = std::max(find_button_width, replace_button_width);
        const qreal shared_button_x = width() - shared_button_width;

        auto layout_row = [this, &button_widths, shared_button_width, shared_button_x](
                              int row, int first_button, qreal natural_width) {
            const qreal extra_per_button = (shared_button_width - natural_width) / 3.0;
            qreal button_x = shared_button_x;
            for (int index = first_button; index <= first_button + 2; ++index) {
                const qreal button_width = index == first_button + 2
                                               ? width() - button_x
                                               : button_widths[static_cast<std::size_t>(index)] + extra_per_button;
                m_button_rects[static_cast<std::size_t>(index)] =
                    QRectF(button_x, row * m_row_height, button_width, m_row_height);
                button_x += button_width;
            }
        };

        layout_row(0, 0, find_button_width);
        layout_row(1, 3, replace_button_width);
        const qreal field_left = label_width + 1.0;
        const qreal field_right = std::max(field_left + 24.0, shared_button_x - 1.0);

        m_find_field->setPosition(QPointF(field_left + 1.0, 2.0));
        m_find_field->setSize(
            QSizeF(std::max<qreal>(1.0, field_right - field_left - 3.0), std::max<qreal>(1.0, m_row_height - 4.0)));
        m_replace_field->setPosition(QPointF(field_left + 1.0, m_row_height + 2.0));
        m_replace_field->setSize(
            QSizeF(std::max<qreal>(1.0, field_right - field_left - 3.0), std::max<qreal>(1.0, m_row_height - 4.0)));
        m_replace_field->setVisible(m_replace_mode);

        refresh_field_state(m_find_field, m_find_visual);
        refresh_field_state(m_replace_field, m_replace_visual);

        setHeight(preferredHeight());
        update();
    }

    void syncFromOwner()
    {
        m_replace_mode = m_owner->findReplaceMode();
        m_read_only = m_owner->send(SCI_GETREADONLY) != 0;
        m_panel_font = m_owner->findPanelFont();
        m_background_color = m_owner->findPanelBackgroundColor();
        m_foreground_color = m_owner->findPanelForegroundColor();
        m_field_background_color = m_owner->findPanelFieldBackgroundColor();
        m_field_foreground_color = m_owner->findPanelFieldForegroundColor();
        m_border_color = m_owner->findPanelBorderColor();
        m_button_hover_color = m_owner->findPanelButtonHoverColor();
        m_disabled_foreground_color = m_owner->findPanelDisabledForegroundColor();
        m_selection_background_color = m_owner->findPanelSelectionBackgroundColor();
        m_selection_foreground_color = m_owner->findPanelSelectionForegroundColor();

        sync_field_text(m_find_field, m_owner->findText());
        sync_field_text(m_replace_field, m_owner->replacementText());
        configure_field(m_find_field);
        configure_field(m_replace_field);
        m_replace_field->setVisible(m_replace_mode);
        refresh_field_state(m_find_field, m_find_visual);
        refresh_field_state(m_replace_field, m_replace_visual);
        relayout();
        update();
    }

    void focusFindField()
    {
        m_find_field->forceActiveFocus(Qt::ShortcutFocusReason);
        m_find_field->send(SCI_SELECTALL);
        refresh_field_state(m_find_field, m_find_visual);
        refresh_field_state(m_replace_field, m_replace_visual);
        update();
    }

    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override
    {
        delete old_node;
        m_text_layouts.clear();

        QQuickWindow* quick_window = window();
        if (!quick_window || width() <= 0.0 || height() <= 0.0) {
            return nullptr;
        }

        auto* root = new QSGNode();
        append_rectangle(root, boundingRect(), m_background_color);
        append_rectangle(root, QRectF(0.0, 0.0, width(), 1.0), m_border_color);
        if (m_replace_mode) {
            append_rectangle(root, QRectF(0.0, m_row_height - 1.0, width(), 1.0), m_border_color);
        }

        append_border(root, m_find_visual.rect.adjusted(-1.0, -1.0, 1.0, 1.0));
        append_field(root, m_find_visual);
        if (m_replace_mode) {
            append_border(root, m_replace_visual.rect.adjusted(-1.0, -1.0, 1.0, 1.0));
            append_field(root, m_replace_visual);
        }

        append_text(root, QStringLiteral("Find:"), QRectF(4.0, 0.0, m_find_visual.rect.left() - 8.0, m_row_height),
            m_foreground_color);
        if (m_replace_mode) {
            append_text(root, QStringLiteral("Replace with:"),
                QRectF(4.0, m_row_height, m_replace_visual.rect.left() - 8.0, m_row_height), m_foreground_color);
        }

        const std::array<QString, 6> labels = {
            QStringLiteral("Find Previous"),
            QStringLiteral("Find Next"),
            QStringLiteral("Select All"),
            QStringLiteral("Replace"),
            QStringLiteral("Replace & Find"),
            QStringLiteral("Replace All"),
        };
        const int button_count = m_replace_mode ? 6 : 3;
        for (int index = 0; index < button_count; ++index) {
            const bool enabled = buttonEnabled(index);
            if (index == m_hovered_button && enabled) {
                append_rectangle(root, m_button_rects[static_cast<std::size_t>(index)], m_button_hover_color);
            }
            append_text(root, labels[static_cast<std::size_t>(index)], m_button_rects[static_cast<std::size_t>(index)],
                enabled ? m_foreground_color : m_disabled_foreground_color, true);
        }

        return root;
    }

  protected:
    void hoverMoveEvent(QHoverEvent* event) override
    {
        const int hovered = buttonAt(event->position());
        if (hovered != m_hovered_button) {
            m_hovered_button = hovered;
            update();
        }
        event->accept();
    }

    void hoverLeaveEvent(QHoverEvent* event) override
    {
        if (m_hovered_button != -1) {
            m_hovered_button = -1;
            update();
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        const int button = buttonAt(event->position());
        if (button >= 0 && buttonEnabled(button)) {
            invokeButton(button);
            event->accept();
            return;
        }
        QQuickItem::mousePressEvent(event);
    }

  private:
    class Find_field final : public ScintillaQuick_item
    {
      public:
        Find_field(Find_panel* panel, bool replacement)
            : ScintillaQuick_item(panel), m_panel(panel), m_replacement(replacement)
        {
            setZ(1.0);
            // Keep Scintilla's input, clipboard, selection, and IME behavior,
            // but let the panel own the field visuals. Nested editor renderers
            // can otherwise interfere with one another in the same subtree.
            setOpacity(0.0);
            setObjectName(
                replacement ? QStringLiteral("scintillaquickReplaceField") : QStringLiteral("scintillaquickFindField"));
        }

      protected:
        void keyPressEvent(QKeyEvent* event) override
        {
            const bool control = event->modifiers().testFlag(Qt::ControlModifier);
            if (control && event->key() == Qt::Key_F) {
                m_panel->m_owner->showFind();
                event->accept();
                return;
            }
            if (control && event->key() == Qt::Key_H) {
                m_panel->m_owner->showFindReplace();
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_Escape) {
                m_panel->m_owner->hideFindPanel();
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
                if (m_replacement) {
                    m_panel->m_owner->replaceAndFind();
                }
                else if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                    m_panel->m_owner->findPrevious();
                }
                else {
                    m_panel->m_owner->findNext();
                }
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
                if (m_panel->m_owner->findReplaceMode()) {
                    Find_field* target = m_replacement ? m_panel->m_find_field : m_panel->m_replace_field;
                    target->forceActiveFocus(Qt::TabFocusReason);
                    target->send(SCI_SELECTALL);
                }
                event->accept();
                return;
            }
            ScintillaQuick_item::keyPressEvent(event);
        }

      private:
        Find_panel* m_panel;
        bool m_replacement;
    };

    void connect_field(Find_field* field, bool replacement)
    {
        QObject::connect(field, &ScintillaQuick_item::modified, this, [this, field, replacement]() {
            fieldChanged(field, replacement);
        });
        QObject::connect(field, &ScintillaQuick_item::textChanged, this, [this, field, replacement]() {
            fieldChanged(field, replacement);
        });
        QObject::connect(field, &QQuickItem::activeFocusChanged, this, [this, field, replacement]() {
            refresh_field_state(field, replacement ? m_replace_visual : m_find_visual);
            update();
        });
        QObject::connect(field, &ScintillaQuick_item::cursorPositionChanged, this, [this, field, replacement]() {
            refresh_field_state(field, replacement ? m_replace_visual : m_find_visual);
            update();
        });
        QObject::connect(field, &ScintillaQuick_item::horizontalScrolled, this, [this, field, replacement](int) {
            refresh_field_state(field, replacement ? m_replace_visual : m_find_visual);
            update();
        });
    }

    void refresh_field_state(Find_field* field, Field_visual_state& state)
    {
        state.rect = QRectF(field->x(), field->y(), field->width(), field->height());
        state.text = field->property("text").toString();
        state.caret = static_cast<Scintilla::Position>(field->send(SCI_GETCURRENTPOS));
        state.selection_start = static_cast<Scintilla::Position>(field->send(SCI_GETSELECTIONSTART));
        state.selection_end = static_cast<Scintilla::Position>(field->send(SCI_GETSELECTIONEND));
        state.x_offset = static_cast<int>(field->send(SCI_GETXOFFSET));
        state.active_focus = field->hasActiveFocus();
    }

    void fieldChanged(Find_field* field, bool replacement)
    {
        const QString value = field->property("text").toString();
        if (replacement) {
            m_owner->setReplacementText(value);
        }
        else {
            m_owner->setFindText(value);
        }
        refresh_field_state(field, replacement ? m_replace_visual : m_find_visual);
        update();
    }

    void sync_field_text(Find_field* field, const QString& text)
    {
        if (field->property("text").toString() != text) {
            field->setProperty("text", text);
        }
    }

    void configure_field(Find_field* field)
    {
        field->setProperty("font", m_owner->findPanelFont());
        field->send(SCI_SETWRAPMODE, SC_WRAP_NONE);
        field->send(SCI_SETHSCROLLBAR, 0);
        field->send(SCI_SETVSCROLLBAR, 0);
        field->send(SCI_SETSCROLLWIDTHTRACKING, 0);
        for (int margin = 0; margin <= SC_MAX_MARGIN; ++margin) {
            field->send(SCI_SETMARGINWIDTHN, margin, 0);
        }

        const int foreground = scintilla_color(m_owner->findPanelFieldForegroundColor());
        const int background = scintilla_color(m_owner->findPanelFieldBackgroundColor());
        field->send(SCI_STYLESETFORE, STYLE_DEFAULT, foreground);
        field->send(SCI_STYLESETBACK, STYLE_DEFAULT, background);
        field->send(SCI_STYLECLEARALL);
        field->send(SCI_SETCARETFORE, scintilla_color(m_owner->findPanelFieldForegroundColor()));
        field->send(SCI_SETSELFORE, 1, scintilla_color(m_owner->findPanelSelectionForegroundColor()));
        field->send(SCI_SETSELBACK, 1, scintilla_color(m_owner->findPanelSelectionBackgroundColor()));
        field->send(SCI_SETADDITIONALSELFORE, scintilla_color(m_owner->findPanelSelectionForegroundColor()));
        field->send(SCI_SETADDITIONALSELBACK, scintilla_color(m_owner->findPanelSelectionBackgroundColor()));
    }

    bool buttonEnabled(int button) const
    {
        const bool has_find_text = !m_find_visual.text.isEmpty();
        if (button < 3) {
            return has_find_text;
        }
        return has_find_text && !m_read_only;
    }

    int buttonAt(const QPointF& point) const
    {
        const int button_count = m_replace_mode ? 6 : 3;
        for (int index = 0; index < button_count; ++index) {
            if (m_button_rects[static_cast<std::size_t>(index)].contains(point)) {
                return index;
            }
        }
        return -1;
    }

    void invokeButton(int button)
    {
        switch (button) {
            case 0:
                m_owner->findPrevious();
                break;
            case 1:
                m_owner->findNext();
                break;
            case 2:
                m_owner->selectAllFindMatches();
                break;
            case 3:
                m_owner->replaceSelection();
                break;
            case 4:
                m_owner->replaceAndFind();
                break;
            case 5:
                m_owner->replaceAll();
                break;
            default:
                break;
        }
        update();
    }

    void append_rectangle(QSGNode* root, const QRectF& rect, const QColor& color)
    {
        if (rect.isEmpty() || color.alpha() == 0) {
            return;
        }
        if (auto* node = window()->createRectangleNode()) {
            node->setRect(rect);
            node->setColor(color);
            root->appendChildNode(node);
        }
    }

    void append_border(QSGNode* root, const QRectF& rect)
    {
        append_rectangle(root, QRectF(rect.left(), rect.top(), rect.width(), 1.0), m_border_color);
        append_rectangle(root, QRectF(rect.left(), rect.bottom() - 1.0, rect.width(), 1.0), m_border_color);
        append_rectangle(root, QRectF(rect.left(), rect.top(), 1.0, rect.height()), m_border_color);
        append_rectangle(root, QRectF(rect.right() - 1.0, rect.top(), 1.0, rect.height()), m_border_color);
    }

    void append_field(QSGNode* root, const Field_visual_state& state)
    {
        const QRectF field_rect = state.rect;
        const QString& text = state.text;
        append_rectangle(root, field_rect, m_field_background_color);
        if (field_rect.width() <= 4.0 || field_rect.height() <= 4.0) {
            return;
        }

        auto* clip = new QSGClipNode();
        clip->setIsRectangular(true);
        clip->setClipRect(field_rect.adjusted(2.0, 1.0, -2.0, -1.0));
        root->appendChildNode(clip);

        const QByteArray utf8 = text.toUtf8();
        auto utf16_index = [&utf8](Scintilla::Position byte_position) {
            const qsizetype bounded_position =
                std::clamp<qsizetype>(static_cast<qsizetype>(byte_position), 0, utf8.size());
            return QString::fromUtf8(utf8.constData(), bounded_position).size();
        };

        const int caret_index = utf16_index(state.caret);
        const int selection_start_index = utf16_index(state.selection_start);
        const int selection_end_index = utf16_index(state.selection_end);

        QTextLayout position_layout(text, m_panel_font);
        QTextOption position_option;
        position_option.setWrapMode(QTextOption::NoWrap);
        position_layout.setTextOption(position_option);
        position_layout.beginLayout();
        QTextLine position_line = position_layout.createLine();
        if (position_line.isValid()) {
            position_line.setLineWidth(1000000.0);
            position_line.setPosition(QPointF(0.0, 0.0));
        }
        position_layout.endLayout();
        auto cursor_x = [&position_line](int index) {
            return position_line.isValid() ? position_line.cursorToX(index) : 0.0;
        };

        const qreal text_left = field_rect.left() + 3.0;
        const qreal available_width = std::max<qreal>(1.0, field_rect.width() - 6.0);
        const qreal caret_advance = cursor_x(caret_index);
        const qreal text_advance = position_line.isValid() ? position_line.naturalTextWidth() : 0.0;
        const qreal origin_x = text_left - state.x_offset;

        if (selection_end_index > selection_start_index) {
            const qreal selection_x1 = origin_x + cursor_x(selection_start_index);
            const qreal selection_x2 = origin_x + cursor_x(selection_end_index);
            append_rectangle(clip,
                QRectF(std::min(selection_x1, selection_x2), field_rect.top() + 2.0,
                    std::max<qreal>(1.0, std::abs(selection_x2 - selection_x1)), field_rect.height() - 4.0),
                m_selection_background_color);
        }

        append_text(clip, text,
            QRectF(
                origin_x, field_rect.top(), std::max<qreal>(available_width, text_advance + 1.0), field_rect.height()),
            m_field_foreground_color, false, selection_start_index, selection_end_index - selection_start_index,
            m_selection_foreground_color);

        if (state.active_focus) {
            const qreal caret_x = origin_x + caret_advance;
            append_rectangle(clip,
                QRectF(caret_x, field_rect.top() + 3.0, 1.0, std::max<qreal>(1.0, field_rect.height() - 6.0)),
                m_field_foreground_color);
        }
    }

    void append_text(QSGNode* root, const QString& text, const QRectF& rect, const QColor& color, bool centered = false,
        int selection_start = -1, int selection_length = 0, const QColor& selection_color = QColor())
    {
        auto layout = std::make_unique<QTextLayout>(text, m_panel_font);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout->setTextOption(option);
        if (selection_start >= 0 && selection_length > 0) {
            QTextLayout::FormatRange selected_range;
            selected_range.start = selection_start;
            selected_range.length = selection_length;
            selected_range.format.setForeground(selection_color);
            layout->setFormats({selected_range});
        }
        layout->beginLayout();
        QTextLine line = layout->createLine();
        if (line.isValid()) {
            line.setLineWidth(std::max<qreal>(1.0, rect.width()));
            line.setPosition(QPointF(0.0, 0.0));
        }
        layout->endLayout();

        const QFontMetricsF metrics(m_panel_font);
        qreal x = rect.left();
        if (centered) {
            x += std::max<qreal>(0.0, (rect.width() - metrics.horizontalAdvance(text)) * 0.5);
        }
        const qreal y = rect.top() + std::max<qreal>(0.0, (rect.height() - metrics.height()) * 0.5);

        if (auto* node = window()->createTextNode()) {
            node->setColor(color);
            node->setViewport(boundingRect());
            node->clear();
            node->addTextLayout(QPointF(x, y), layout.get());
            root->appendChildNode(node);
        }
        m_text_layouts.push_back(std::move(layout));
    }

    ScintillaQuick_item* m_owner;
    Find_field* m_find_field;
    Find_field* m_replace_field;
    Field_visual_state m_find_visual;
    Field_visual_state m_replace_visual;
    bool m_replace_mode = false;
    bool m_read_only = false;
    QFont m_panel_font;
    QColor m_background_color;
    QColor m_foreground_color;
    QColor m_field_background_color;
    QColor m_field_foreground_color;
    QColor m_border_color;
    QColor m_button_hover_color;
    QColor m_disabled_foreground_color;
    QColor m_selection_background_color;
    QColor m_selection_foreground_color;
    qreal m_row_height = 28.0;
    std::array<QRectF, 6> m_button_rects{};
    int m_hovered_button = -1;
    std::vector<std::unique_ptr<QTextLayout>> m_text_layouts;
};

void ScintillaQuick_item::ensureFindPanel()
{
    if (!m_find_panel) {
        m_find_panel = new Find_panel(this);
    }
    m_find_panel->syncFromOwner();
    updateFindPanelGeometry();
}

void ScintillaQuick_item::updateFindPanelGeometry()
{
    if (!m_find_panel) {
        return;
    }
    m_find_panel->setWidth(width());
    m_find_panel->relayout();
    m_find_panel->setY(std::max<qreal>(0.0, height() - m_find_panel->height()));
}

bool ScintillaQuick_item::findPanelVisible() const
{
    return m_find_panel_visible;
}

void ScintillaQuick_item::setFindPanelVisible(bool visible)
{
    if (m_find_panel_visible == visible) {
        if (visible) {
            ensureFindPanel();
            m_find_panel->setVisible(true);
            m_find_panel->focusFindField();
        }
        return;
    }

    m_find_panel_visible = visible;
    if (visible) {
        ensureFindPanel();
        m_find_panel->setVisible(true);
        m_find_panel->focusFindField();
    }
    else if (m_find_panel) {
        m_find_panel->setVisible(false);
        forceActiveFocus(Qt::ShortcutFocusReason);
    }
    emit findPanelVisibleChanged();
}

bool ScintillaQuick_item::findReplaceMode() const
{
    return m_find_replace_mode;
}

void ScintillaQuick_item::setFindReplaceMode(bool replace_mode)
{
    if (m_find_replace_mode == replace_mode) {
        return;
    }
    m_find_replace_mode = replace_mode;
    if (m_find_panel) {
        m_find_panel->syncFromOwner();
        updateFindPanelGeometry();
    }
    emit findReplaceModeChanged();
}

QString ScintillaQuick_item::findText() const
{
    return m_find_text;
}

void ScintillaQuick_item::setFindText(const QString& text)
{
    if (m_find_text == text) {
        return;
    }
    m_find_text = text;
    m_last_find_start = -1;
    m_last_find_end = -1;
    m_last_find_text.clear();
    if (m_find_panel) {
        m_find_panel->syncFromOwner();
    }
    emit findTextChanged();
}

QString ScintillaQuick_item::replacementText() const
{
    return m_replacement_text;
}

void ScintillaQuick_item::setReplacementText(const QString& text)
{
    if (m_replacement_text == text) {
        return;
    }
    m_replacement_text = text;
    if (m_find_panel) {
        m_find_panel->syncFromOwner();
    }
    emit replacementTextChanged();
}

int ScintillaQuick_item::findOptions() const
{
    return m_find_options;
}

void ScintillaQuick_item::setFindOptions(int options)
{
    if (m_find_options == options) {
        return;
    }
    m_find_options = options;
    m_last_find_start = -1;
    m_last_find_end = -1;
    m_last_find_text.clear();
    emit findOptionsChanged();
}

QFont ScintillaQuick_item::findPanelFont() const
{
    return m_find_panel_font;
}

void ScintillaQuick_item::setFindPanelFont(const QFont& font)
{
    if (m_find_panel_font == font) {
        return;
    }
    m_find_panel_font = font;
    if (m_find_panel) {
        m_find_panel->syncFromOwner();
        updateFindPanelGeometry();
    }
    emit findPanelFontChanged();
}

#define SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(Getter, Setter, member, Signal)                                      \
    QColor ScintillaQuick_item::Getter() const                                                                         \
    {                                                                                                                  \
        return member;                                                                                                 \
    }                                                                                                                  \
    void ScintillaQuick_item::Setter(const QColor& color)                                                              \
    {                                                                                                                  \
        if (member == color)                                                                                           \
            return;                                                                                                    \
        member = color;                                                                                                \
        if (m_find_panel)                                                                                              \
            m_find_panel->syncFromOwner();                                                                             \
        emit Signal();                                                                                                 \
    }

SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelBackgroundColor, setFindPanelBackgroundColor,
    m_find_panel_background_color, findPanelBackgroundColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelForegroundColor, setFindPanelForegroundColor,
    m_find_panel_foreground_color, findPanelForegroundColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelFieldBackgroundColor, setFindPanelFieldBackgroundColor,
    m_find_panel_field_background_color, findPanelFieldBackgroundColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelFieldForegroundColor, setFindPanelFieldForegroundColor,
    m_find_panel_field_foreground_color, findPanelFieldForegroundColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(
    findPanelBorderColor, setFindPanelBorderColor, m_find_panel_border_color, findPanelBorderColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelButtonHoverColor, setFindPanelButtonHoverColor,
    m_find_panel_button_hover_color, findPanelButtonHoverColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelDisabledForegroundColor, setFindPanelDisabledForegroundColor,
    m_find_panel_disabled_foreground_color, findPanelDisabledForegroundColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelSelectionBackgroundColor, setFindPanelSelectionBackgroundColor,
    m_find_panel_selection_background_color, findPanelSelectionBackgroundColorChanged)
SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS(findPanelSelectionForegroundColor, setFindPanelSelectionForegroundColor,
    m_find_panel_selection_foreground_color, findPanelSelectionForegroundColorChanged)

#undef SCINTILLAQUICK_FIND_PANEL_COLOR_ACCESSORS

void ScintillaQuick_item::showFind()
{
    const Scintilla::Position selection_length = static_cast<Scintilla::Position>(send(SCI_GETSELTEXT, 0, 0));
    if (selection_length > 0) {
        QByteArray selected(static_cast<qsizetype>(selection_length + 1), Qt::Uninitialized);
        send(SCI_GETSELTEXT, 0, reinterpret_cast<Scintilla::sptr_t>(selected.data()));
        setFindText(QString::fromUtf8(selected.constData(), static_cast<qsizetype>(selection_length)));
    }
    setFindReplaceMode(false);
    setFindPanelVisible(true);
}

void ScintillaQuick_item::showFindReplace()
{
    const Scintilla::Position selection_length = static_cast<Scintilla::Position>(send(SCI_GETSELTEXT, 0, 0));
    if (selection_length > 0) {
        QByteArray selected(static_cast<qsizetype>(selection_length + 1), Qt::Uninitialized);
        send(SCI_GETSELTEXT, 0, reinterpret_cast<Scintilla::sptr_t>(selected.data()));
        setFindText(QString::fromUtf8(selected.constData(), static_cast<qsizetype>(selection_length)));
    }
    setFindReplaceMode(true);
    setFindPanelVisible(true);
}

void ScintillaQuick_item::hideFindPanel()
{
    setFindPanelVisible(false);
}

bool ScintillaQuick_item::findNext()
{
    const QByteArray needle = m_find_text.toUtf8();
    if (needle.isEmpty()) {
        return false;
    }

    const Scintilla::Position length = document_length(*this);
    const Scintilla::Position selection_start = static_cast<Scintilla::Position>(send(SCI_GETSELECTIONSTART));
    const Scintilla::Position selection_end = static_cast<Scintilla::Position>(send(SCI_GETSELECTIONEND));
    const bool repeated_zero_width = selection_start == selection_end && selection_start == m_last_find_start &&
                                     selection_end == m_last_find_end && m_find_text == m_last_find_text &&
                                     m_find_options == m_last_find_options;

    Scintilla::Position search_start = selection_end;
    Scintilla::Position match = -1;
    if (repeated_zero_width) {
        const Scintilla::Position advanced = static_cast<Scintilla::Position>(send(SCI_POSITIONAFTER, search_start));
        if (advanced > search_start) {
            match = search_range(*this, needle, advanced, length);
        }
    }
    else {
        match = search_range(*this, needle, search_start, length);
    }
    if (match < 0 && search_start > 0) {
        match = search_range(*this, needle, 0, search_start);
    }
    if (match < 0) {
        return false;
    }

    const Scintilla::Position target_start = static_cast<Scintilla::Position>(send(SCI_GETTARGETSTART));
    const Scintilla::Position target_end = static_cast<Scintilla::Position>(send(SCI_GETTARGETEND));
    if (repeated_zero_width && target_start == m_last_find_start && target_end == m_last_find_end) {
        return false;
    }
    m_last_find_start = target_start;
    m_last_find_end = target_end;
    m_last_find_text = m_find_text;
    m_last_find_options = m_find_options;
    send(SCI_SETSEL, target_start, target_end);
    send(SCI_SCROLLCARET);
    return true;
}

bool ScintillaQuick_item::findPrevious()
{
    const QByteArray needle = m_find_text.toUtf8();
    if (needle.isEmpty()) {
        return false;
    }

    const Scintilla::Position length = document_length(*this);
    const Scintilla::Position selection_start = static_cast<Scintilla::Position>(send(SCI_GETSELECTIONSTART));
    const Scintilla::Position selection_end = static_cast<Scintilla::Position>(send(SCI_GETSELECTIONEND));
    const bool repeated_zero_width = selection_start == selection_end && selection_start == m_last_find_start &&
                                     selection_end == m_last_find_end && m_find_text == m_last_find_text &&
                                     m_find_options == m_last_find_options;

    Scintilla::Position match = -1;
    if (repeated_zero_width) {
        const Scintilla::Position retreated =
            static_cast<Scintilla::Position>(send(SCI_POSITIONBEFORE, selection_start));
        if (retreated < selection_start) {
            match = search_range(*this, needle, retreated, 0);
        }
    }
    else {
        match = search_range(*this, needle, selection_start, 0);
    }
    if (match < 0 && selection_start < length) {
        match = search_range(*this, needle, length, selection_start);
    }
    if (match < 0) {
        return false;
    }

    const Scintilla::Position target_start = static_cast<Scintilla::Position>(send(SCI_GETTARGETSTART));
    const Scintilla::Position target_end = static_cast<Scintilla::Position>(send(SCI_GETTARGETEND));
    if (repeated_zero_width && target_start == m_last_find_start && target_end == m_last_find_end) {
        return false;
    }
    m_last_find_start = target_start;
    m_last_find_end = target_end;
    m_last_find_text = m_find_text;
    m_last_find_options = m_find_options;
    send(SCI_SETSEL, target_start, target_end);
    send(SCI_SCROLLCARET);
    return true;
}

int ScintillaQuick_item::selectAllFindMatches()
{
    const QByteArray needle = m_find_text.toUtf8();
    if (needle.isEmpty()) {
        return 0;
    }

    const auto matches = all_matches(*this, needle);
    if (matches.empty()) {
        return 0;
    }

    send(SCI_SETMULTIPLESELECTION, 1);
    send(SCI_CLEARSELECTIONS);
    send(SCI_SETSEL, matches.front().first, matches.front().second);
    for (std::size_t index = 1; index < matches.size(); ++index) {
        send(SCI_ADDSELECTION, matches[index].second, matches[index].first);
    }
    send(SCI_SETMAINSELECTION, 0);
    send(SCI_SCROLLCARET);
    return static_cast<int>(matches.size());
}

ScintillaQuick_item::Replace_result ScintillaQuick_item::replace_selection_outcome()
{
    const QByteArray needle = m_find_text.toUtf8();
    const Scintilla::Position selection_start = static_cast<Scintilla::Position>(send(SCI_GETSELECTIONSTART));
    const Scintilla::Position selection_end = static_cast<Scintilla::Position>(send(SCI_GETSELECTIONEND));
    const bool tracked_empty_match = selection_start == selection_end && selection_start == m_last_find_start &&
                                     selection_end == m_last_find_end && m_find_text == m_last_find_text &&
                                     m_find_options == m_last_find_options;
    if (needle.isEmpty() || send(SCI_GETREADONLY) != 0 || !selection_is_find_match(*this, needle, tracked_empty_match))
    {
        return Replace_result::NOT_A_MATCH;
    }

    const QByteArray replacement = m_replacement_text.toUtf8();
    const unsigned int replace_message = (m_find_options & SCFIND_REGEXP) ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;
    const Edit_dispatch_outcome outcome = send_with_outcome(
        replace_message,
        static_cast<Scintilla::uptr_t>(replacement.size()),
        reinterpret_cast<Scintilla::sptr_t>(replacement.constData()));
    if (outcome.disposition == ScintillaQuick_edit_disposition::REJECTED) {
        return Replace_result::REJECTED;
    }
    if (outcome.intercepted && !outcome.local_document_changed) {
        // The handler claims to have dealt with the replacement externally;
        // the local target/selection state is unchanged and must not be
        // trusted.
        return Replace_result::HANDLED_EXTERNAL;
    }
    send(SCI_SETSEL, send(SCI_GETTARGETSTART), send(SCI_GETTARGETEND));
    return Replace_result::CHANGED;
}

bool ScintillaQuick_item::replaceSelection()
{
    const Replace_result result = replace_selection_outcome();
    return result == Replace_result::CHANGED || result == Replace_result::HANDLED_EXTERNAL;
}

bool ScintillaQuick_item::replaceAndFind()
{
    const Replace_result result = replace_selection_outcome();
    if (result == Replace_result::REJECTED) {
        // Do not advance past the current match after a rejected replacement.
        return false;
    }
    const bool found = findNext();
    return result != Replace_result::NOT_A_MATCH || found;
}

int ScintillaQuick_item::replaceAll()
{
    const QByteArray needle = m_find_text.toUtf8();
    if (needle.isEmpty() || send(SCI_GETREADONLY) != 0) {
        return 0;
    }

    const QByteArray replacement = m_replacement_text.toUtf8();
    const unsigned int replace_message = (m_find_options & SCFIND_REGEXP) ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;
    Scintilla::Position search_start = 0;
    int replacements = 0;

    send(SCI_BEGINUNDOACTION);
    while (search_start <= document_length(*this)) {
        const Scintilla::Position match = search_range(*this, needle, search_start, document_length(*this));
        if (match < 0) {
            break;
        }
        const Scintilla::Position old_match_end = static_cast<Scintilla::Position>(send(SCI_GETTARGETEND));
        const Edit_dispatch_outcome outcome = send_with_outcome(
            replace_message,
            static_cast<Scintilla::uptr_t>(replacement.size()),
            reinterpret_cast<Scintilla::sptr_t>(replacement.constData()));
        if (outcome.disposition == ScintillaQuick_edit_disposition::REJECTED) {
            break;
        }
        ++replacements;
        if (outcome.intercepted && !outcome.local_document_changed) {
            // The handler claims the edit but the local document is
            // unchanged; the local target state is stale, so continuing the
            // loop would operate on phantom positions.
            break;
        }

        const Scintilla::Position replacement_end = static_cast<Scintilla::Position>(send(SCI_GETTARGETEND));
        if (old_match_end > match) {
            search_start = replacement_end;
        }
        else {
            const Scintilla::Position next = static_cast<Scintilla::Position>(send(SCI_POSITIONAFTER, replacement_end));
            if (next <= replacement_end) {
                break;
            }
            search_start = next;
        }
    }
    send(SCI_ENDUNDOACTION);
    return replacements;
}
