// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_validation_access.h"

#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGNode>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

#include "Scintilla.h"
#include "ScintillaQuickFont.h"

// This test exercises Scintilla::Internal render data directly, so keeping the
// internal namespace open here keeps the assertions readable.
using namespace Scintilla::Internal;

namespace {

QString build_large_document(int line_count)
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

QString build_wrapped_document(int line_count)
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

void send_wheel_event(
    QQuickWindow &window,
    ScintillaQuick_item &editor,
    QPointF local_pos,
    QPoint pixel_delta,
    QPoint angle_delta,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF scene_pos = editor.mapToScene(local_pos);
    const QPointF global_pos = window.mapToGlobal(scene_pos.toPoint());
    QWheelEvent event(
        local_pos,
        global_pos,
        pixel_delta,
        angle_delta,
        Qt::NoButton,
        modifiers,
        Qt::NoScrollPhase,
        false);
    QCoreApplication::sendEvent(&editor, &event);
}

struct Comparison_result
{
    int differing_pixels = 0;
    double diff_ratio = 0.0;
    QImage diff_image;
};

Comparison_result compare_images(const QImage &actual, const QImage &expected)
{
    Comparison_result result;

    QImage a = actual.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage e = expected.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if (a.size() != e.size()) {
        result.diff_image = a;
        result.differing_pixels = std::max(a.width() * a.height(), e.width() * e.height());
        result.diff_ratio = 1.0;
        return result;
    }

    const int width = a.width();
    const int height = a.height();
    QImage diff(width, height, QImage::Format_ARGB32_Premultiplied);
    diff.fill(Qt::black);

    int diffs = 0;
    for (int y = 0; y < height; ++y) {
        const QRgb *actual_line = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *expected_line = reinterpret_cast<const QRgb *>(e.constScanLine(y));
        QRgb *diff_line = reinterpret_cast<QRgb *>(diff.scanLine(y));

        for (int x = 0; x < width; ++x) {
            const int dr = std::abs(qRed(actual_line[x]) - qRed(expected_line[x]));
            const int dg = std::abs(qGreen(actual_line[x]) - qGreen(expected_line[x]));
            const int db = std::abs(qBlue(actual_line[x]) - qBlue(expected_line[x]));
            const int da = std::abs(qAlpha(actual_line[x]) - qAlpha(expected_line[x]));

            if (dr > 0 || dg > 0 || db > 0 || da > 0) {
                ++diffs;
                diff_line[x] = qRgb(255, 0, 0);
            }
            else {
                diff_line[x] = qRgb(
                    qRed(actual_line[x]) / 3,
                    qGreen(actual_line[x]) / 3,
                    qBlue(actual_line[x]) / 3);
            }
        }
    }

    result.differing_pixels = diffs;
    result.diff_ratio = (width * height > 0)
        ? static_cast<double>(diffs) / (width * height)
        : 0.0;
    result.diff_image = std::move(diff);
    return result;
}

struct Review_fixture
{
    QString name;
    std::function<void(class Fixture_editor &)> setup;
};

struct Fixture_editor
{
    struct Test_editor : ScintillaQuick_item
    {
        std::function<void()> on_paint_node_updated;

    protected:
        QSGNode *updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *update_paint_node_data) override
        {
            QSGNode *node = ScintillaQuick_item::updatePaintNode(old_node, update_paint_node_data);
            if (on_paint_node_updated) {
                on_paint_node_updated();
            }
            return node;
        }
    };

    QQuickWindow window;
    Test_editor editor;
    quint64 paint_counter = 0;

    Fixture_editor()
    {
        window.resize(640, 480);
        window.setColor(Qt::white);

        editor.setParentItem(window.contentItem());
        editor.setWidth(window.width());
        editor.setHeight(window.height());
        editor.setProperty("font", scintillaquick::shared::deterministic_test_font(11));
        editor.send(SCI_SETCARETPERIOD, 0);
        editor.on_paint_node_updated = [&]() { ++paint_counter; };

        window.show();
        window.raise();
        window.requestActivate();
        editor.forceActiveFocus();
        editor.send(SCI_SETFOCUS, 1);
        pump();
    }

    void set_text(const char *text)
    {
        editor.setProperty("text", QString::fromUtf8(text));
        pump();
    }

    void set_text(const QString &text)
    {
        editor.setProperty("text", text);
        pump();
    }

    void pump()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    bool wait_for_ready(int timeout_ms = 1000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeout_ms) {
            pump();
            if (window.isExposed() &&
                window.visibility() != QWindow::Hidden &&
                editor.window() == &window) {
                return true;
            }
            QThread::msleep(10);
        }
        return false;
    }

    bool wait_for_next_paint(quint64 previous_paint_counter, int timeout_ms = 50)
    {
        if (paint_counter > previous_paint_counter) {
            return true;
        }

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);

        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        const auto previous_callback = editor.on_paint_node_updated;
        editor.on_paint_node_updated = [&]() {
            if (previous_callback) {
                previous_callback();
            }
            if (paint_counter > previous_paint_counter) {
                loop.quit();
            }
        };

        timeout.start(timeout_ms);
        loop.exec();
        editor.on_paint_node_updated = previous_callback;
        return paint_counter > previous_paint_counter;
    }

    QImage capture_quick_image()
    {
        pump();
        if (!wait_for_ready()) {
            return {};
        }

        const int editor_width = std::max(1, static_cast<int>(std::ceil(editor.width())));
        const int editor_height = std::max(1, static_cast<int>(std::ceil(editor.height())));
        window.resize(editor_width, editor_height);
        window.raise();
        window.requestActivate();
        editor.forceActiveFocus();
        editor.send(SCI_SETFOCUS, 1);
        pump();

        if (!wait_for_ready()) {
            return {};
        }

        QImage image;
        for (int attempt = 0; attempt < 3; ++attempt) {
            image = window.grabWindow();
            if (!image.isNull()) {
                return image;
            }
            pump();
        }
        return image;
    }

    QImage capture_reference_image()
    {
        pump();
        return ScintillaQuick_validation_access::capture_raster_reference(editor);
    }
};

bool write_image(const QString &path, const QImage &image)
{
    if (image.isNull()) {
        return false;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    return image.save(path);
}

bool apply_wheel_step_fixture(
    Fixture_editor &fixture,
    bool wrapped,
    int steps,
    QString *error_message)
{
    fixture.editor.send(SCI_SETWRAPMODE, wrapped ? SC_WRAP_WORD : SC_WRAP_NONE);
    fixture.set_text(wrapped ? build_wrapped_document(4000) : build_large_document(25000));
    fixture.editor.send(SCI_SETFIRSTVISIBLELINE, 120);
    fixture.pump();

    const QPointF wheel_point(
        std::max<qreal>(fixture.editor.width() * 0.5, 32.0),
        std::max<qreal>(fixture.editor.height() * 0.5, 32.0));

    for (int step = 0; step < steps; ++step) {
        const int previous_first_visible_line =
            static_cast<int>(fixture.editor.send(SCI_GETFIRSTVISIBLELINE));
        const quint64 previous_paint_counter = fixture.paint_counter;
        const bool wheel_up = (step % 2) == 0;

        send_wheel_event(
            fixture.window,
            fixture.editor,
            wheel_point,
            QPoint(),
            QPoint(0, wheel_up ? 120 : -120));

        fixture.pump();

        QElapsedTimer scroll_timer;
        scroll_timer.start();
        int current_first_visible_line = previous_first_visible_line;
        while (scroll_timer.elapsed() < 100) {
            fixture.pump();
            current_first_visible_line =
                static_cast<int>(fixture.editor.send(SCI_GETFIRSTVISIBLELINE));
            if (current_first_visible_line != previous_first_visible_line) {
                break;
            }
        }

        if (!fixture.wait_for_next_paint(previous_paint_counter)) {
            if (error_message) {
                *error_message = QStringLiteral("Timed out waiting for repaint after wheel step %1").arg(step);
            }
            return false;
        }

        if (current_first_visible_line == previous_first_visible_line) {
            if (error_message) {
                *error_message = QStringLiteral("Wheel step %1 did not change first visible line").arg(step);
            }
            return false;
        }
    }

    return true;
}

Review_fixture make_wheel_review_fixture(
    const QString &name,
    bool wrapped,
    int steps)
{
    return {
        name,
        [wrapped, steps](Fixture_editor &fixture) {
            QString error_message;
            if (!apply_wheel_step_fixture(fixture, wrapped, steps, &error_message)) {
                qFatal("%s", qPrintable(error_message));
            }
        }
    };
}

std::vector<Review_fixture> review_fixtures()
{
    std::vector<Review_fixture> fixtures = {
        {
            QStringLiteral("plain_ascii_short"),
            [](Fixture_editor &fixture) {
                fixture.set_text("alpha beta gamma");
            }
        },
        {
            QStringLiteral("plain_ascii_long_wrap"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(200);
                fixture.editor.setHeight(480);
                fixture.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
                fixture.set_text(
                    "The quick brown fox jumps over the lazy dog and then runs around "
                    "the garden several more times until the text is long enough to "
                    "definitely wrap into multiple visual sublines at a narrow width.");
            }
        },
        {
            QStringLiteral("mixed_styles_wrap"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(200);
                fixture.editor.setHeight(480);
                fixture.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

                const QByteArray font_family = scintillaquick::shared::deterministic_test_font_family_utf8();
                fixture.editor.send(SCI_STYLESETFONT,
                                    STYLE_DEFAULT,
                                    reinterpret_cast<sptr_t>(font_family.constData()));
                fixture.editor.send(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
                fixture.editor.send(SCI_STYLECLEARALL);
                fixture.editor.send(SCI_STYLESETFORE, 1, 0x000000);
                fixture.editor.send(SCI_STYLESETBOLD, 1, 1);
                fixture.editor.send(SCI_STYLESETFORE, 2, 0x0000FF);
                fixture.editor.send(SCI_STYLESETITALIC, 2, 1);
                fixture.editor.send(SCI_STYLESETFORE, 3, 0xFF0000);

                const char *text =
                    "keyword identifier literal keyword identifier literal "
                    "keyword identifier literal keyword identifier literal end";
                fixture.set_text(text);

                int styles[] = {1, 2, 3};
                int pos = 0;
                const int text_len = static_cast<int>(std::strlen(text));
                int word_index = 0;
                while (pos < text_len) {
                    if (text[pos] == ' ') {
                        ++pos;
                        continue;
                    }
                    int word_len = 0;
                    while (pos + word_len < text_len && text[pos + word_len] != ' ') {
                        ++word_len;
                    }
                    fixture.editor.send(SCI_STARTSTYLING, pos);
                    fixture.editor.send(SCI_SETSTYLING, word_len, styles[word_index % 3]);
                    pos += word_len;
                    ++word_index;
                }
            }
        },
        {
            QStringLiteral("selection_single_line"),
            [](Fixture_editor &fixture) {
                fixture.set_text("select this word here");
                fixture.editor.send(SCI_SETSEL, 7, 11);
            }
        },
        {
            QStringLiteral("selection_wrap_boundary"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(200);
                fixture.editor.setHeight(480);
                fixture.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

                const char *text =
                    "The quick brown fox jumps over the lazy dog and then continues "
                    "running around the field to ensure we get enough wrapping here.";
                fixture.set_text(text);

                const int text_len = static_cast<int>(std::strlen(text));
                fixture.editor.send(SCI_SETSEL, text_len / 3, 2 * text_len / 3);
            }
        },
        {
            QStringLiteral("caret_mid_line"),
            [](Fixture_editor &fixture) {
                fixture.set_text("caret goes here in the middle of styled text");
                fixture.editor.send(SCI_GOTOPOS, 16);
            }
        },
        {
            QStringLiteral("caret_wrap_continuation"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(200);
                fixture.editor.setHeight(480);
                fixture.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
                const char *text =
                    "The quick brown fox jumps over the lazy dog and then continues "
                    "running around the field to test caret on a wrapped continuation.";
                fixture.set_text(text);
                fixture.editor.send(SCI_GOTOPOS, static_cast<int>(std::strlen(text)) - 10);
            }
        },
        {
            QStringLiteral("current_line_basic"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETCARETLINEVISIBLE, 1);
                fixture.editor.send(SCI_SETCARETLINEBACK, 0xFFFFE0);
                fixture.set_text("first line\nsecond line\nthird line");
                fixture.editor.send(SCI_GOTOPOS, 5);
            }
        },
        {
            QStringLiteral("current_line_wrap"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(200);
                fixture.editor.setHeight(480);
                fixture.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
                fixture.editor.send(SCI_SETCARETLINEVISIBLE, 1);
                fixture.editor.send(SCI_SETCARETLINEBACK, 0xFFFFE0);
                const char *text =
                    "The quick brown fox jumps over the lazy dog and then continues "
                    "running around the field.";
                fixture.set_text(text);
                fixture.editor.send(SCI_GOTOPOS, static_cast<int>(std::strlen(text)) - 5);
            }
        },
        {
            QStringLiteral("margin_numbers_basic"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
                fixture.editor.send(SCI_SETMARGINWIDTHN, 0, 40);
                fixture.set_text("line one\nline two\nline three\nline four\nline five");
            }
        },
        {
            QStringLiteral("margin_numbers_wrap"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(200);
                fixture.editor.setHeight(480);
                fixture.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
                fixture.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
                fixture.editor.send(SCI_SETMARGINWIDTHN, 0, 40);
                fixture.set_text(
                    "This is a very long first line that should definitely wrap into "
                    "multiple visual sublines when shown at narrow width.\n"
                    "Short second line.\n"
                    "Third line here.");
            }
        },
        {
            QStringLiteral("plain_indicator_basic"),
            [](Fixture_editor &fixture) {
                fixture.set_text("indicator on this text here");
                fixture.editor.send(SCI_INDICSETSTYLE, 0, INDIC_PLAIN);
                fixture.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
                fixture.editor.send(SCI_INDICSETUNDER, 0, 1);
                fixture.editor.send(SCI_SETINDICATORCURRENT, 0);
                fixture.editor.send(SCI_INDICATORFILLRANGE, 13, 9);
            }
        },
        {
            QStringLiteral("control_repr_simple"),
            [](Fixture_editor &fixture) {
                fixture.set_text("hello\x01world\x02end");
            }
        },
        {
            QStringLiteral("multi_selection"),
            [](Fixture_editor &fixture) {
                fixture.set_text("aaa bbb ccc ddd eee fff");
                fixture.editor.send(SCI_SETSELECTION, 0, 3);
                fixture.editor.send(SCI_ADDSELECTION, 4, 7);
                fixture.editor.send(SCI_ADDSELECTION, 8, 11);
            }
        },
        {
            QStringLiteral("rectangular_selection"),
            [](Fixture_editor &fixture) {
                fixture.set_text("line one\nline two\nline three");
                fixture.editor.send(SCI_SETRECTANGULARSELECTIONANCHOR, 0);
                fixture.editor.send(SCI_SETRECTANGULARSELECTIONCARET, 22);
                fixture.editor.send(SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE, 0);
                fixture.editor.send(SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE, 0);
            }
        },
        {
            QStringLiteral("current_line_frame"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETCARETLINEVISIBLE, 1);
                fixture.editor.send(SCI_SETCARETLINEBACK, 0xFFFFE0);
                fixture.editor.send(SCI_SETCARETLINEFRAME, 1);
                fixture.set_text("first line\nsecond line\nthird line");
                fixture.editor.send(SCI_GOTOPOS, 5);
            }
        },
        {
            QStringLiteral("squiggle_indicator"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(900);
                fixture.editor.setHeight(220);
                fixture.set_text("error on this word here");
                fixture.editor.send(SCI_INDICSETSTYLE, 0, INDIC_SQUIGGLE);
                fixture.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
                fixture.editor.send(SCI_INDICSETUNDER, 0, 1);
                fixture.editor.send(SCI_SETINDICATORCURRENT, 0);
                fixture.editor.send(SCI_INDICATORFILLRANGE, 9, 4);
            }
        },
        {
            QStringLiteral("box_indicator"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(900);
                fixture.editor.setHeight(220);
                fixture.set_text("box around this word");
                fixture.editor.send(SCI_INDICSETSTYLE, 1, INDIC_BOX);
                fixture.editor.send(SCI_INDICSETFORE, 1, 0xFF0000);
                fixture.editor.send(SCI_INDICSETUNDER, 1, 1);
                fixture.editor.send(SCI_SETINDICATORCURRENT, 1);
                fixture.editor.send(SCI_INDICATORFILLRANGE, 4, 6);
            }
        },
        {
            QStringLiteral("marker_symbol"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
                fixture.editor.send(SCI_SETMARGINWIDTHN, 1, 16);
                fixture.editor.send(SCI_SETMARGINMASKN, 1, 0x01);
                fixture.editor.send(SCI_MARKERDEFINE, 0, SC_MARK_CIRCLE);
                fixture.editor.send(SCI_MARKERSETFORE, 0, 0x000000);
                fixture.editor.send(SCI_MARKERSETBACK, 0, 0xFF0000);
                fixture.set_text("marked line\nunmarked line\nthird line");
                fixture.editor.send(SCI_MARKERADD, 0, 0);
            }
        },
        {
            QStringLiteral("multi_caret"),
            [](Fixture_editor &fixture) {
                fixture.set_text("abc def ghi jkl");
                fixture.editor.send(SCI_SETSELECTION, 4, 4);
                fixture.editor.send(SCI_ADDSELECTION, 8, 8);
                fixture.editor.send(SCI_ADDSELECTION, 12, 12);
            }
        },
        {
            QStringLiteral("whitespace_visible"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETVIEWWS, SCWS_VISIBLEALWAYS);
                fixture.set_text("a b\tc");
            }
        },
        {
            QStringLiteral("eol_annotation"),
            [](Fixture_editor &fixture) {
                fixture.set_text("line one\nline two\n");
                fixture.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STANDARD);
                const char *annot_text = "this is an eol annotation";
                fixture.editor.send(SCI_EOLANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
                fixture.pump();
            }
        },
        {
            QStringLiteral("annotation"),
            [](Fixture_editor &fixture) {
                fixture.set_text("line one\nline two\nline three");
                fixture.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_STANDARD);
                const char *annot_text = "annotation text here";
                fixture.editor.send(SCI_ANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
                fixture.pump();
            }
        },
        {
            QStringLiteral("indent_guide"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETINDENTATIONGUIDES, SC_IV_REAL);
                fixture.editor.send(SCI_SETTABWIDTH, 4);
                fixture.editor.send(SCI_SETINDENT, 4);
                fixture.set_text("if (x) {\n    if (y) {\n        z = 1;\n    }\n}");
            }
        },
        {
            QStringLiteral("style_underline"),
            [](Fixture_editor &fixture) {
                fixture.editor.setWidth(900);
                fixture.editor.setHeight(220);
                fixture.set_text("style underline here");
                fixture.editor.send(SCI_STYLESETUNDERLINE, STYLE_DEFAULT, 1);
                fixture.editor.send(SCI_STARTSTYLING, 0);
                fixture.editor.send(SCI_SETSTYLING, 18, STYLE_DEFAULT);
            }
        },
        {
            QStringLiteral("fold_display_text"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETPROPERTY,
                    reinterpret_cast<uptr_t>("fold"), reinterpret_cast<sptr_t>("1"));
                fixture.editor.send(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
                fixture.editor.send(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
                fixture.editor.send(SCI_SETMARGINWIDTHN, 2, 16);
                fixture.editor.send(SCI_SETAUTOMATICFOLD,
                    SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CHANGE, 0);
                fixture.editor.send(SCI_SETDEFAULTFOLDDISPLAYTEXT, 0,
                    reinterpret_cast<sptr_t>("..."));
                fixture.editor.send(SCI_FOLDDISPLAYTEXTSETSTYLE, SC_FOLDDISPLAYTEXT_BOXED, 0);

                fixture.set_text("if (true) {\n    body line 1\n    body line 2\n}");
                fixture.editor.send(SCI_SETFOLDLEVEL, 0,
                    SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
                fixture.editor.send(SCI_SETFOLDLEVEL, 1, SC_FOLDLEVELBASE + 1);
                fixture.editor.send(SCI_SETFOLDLEVEL, 2, SC_FOLDLEVELBASE + 1);
                fixture.editor.send(SCI_SETFOLDLEVEL, 3, SC_FOLDLEVELBASE);
                fixture.pump();

                fixture.editor.send(SCI_FOLDLINE, 0, SC_FOLDACTION_CONTRACT);
                fixture.pump();
            }
        },
        {
            QStringLiteral("fold_markers"),
            [](Fixture_editor &fixture) {
                fixture.editor.send(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
                fixture.editor.send(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
                fixture.editor.send(SCI_SETMARGINWIDTHN, 2, 16);

                fixture.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
                fixture.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
                fixture.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
                fixture.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
                fixture.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
                fixture.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
                fixture.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

                fixture.set_text("function a() {\n    line1\n    line2\n}");
                fixture.editor.send(SCI_SETFOLDLEVEL, 0,
                    SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
                fixture.editor.send(SCI_SETFOLDLEVEL, 1, SC_FOLDLEVELBASE + 1);
                fixture.editor.send(SCI_SETFOLDLEVEL, 2, SC_FOLDLEVELBASE + 1);
                fixture.editor.send(SCI_SETFOLDLEVEL, 3, SC_FOLDLEVELBASE);
                fixture.pump();
            }
        },
        {
            QStringLiteral("annotation_boxed"),
            [](Fixture_editor &fixture) {
                fixture.set_text("line one\nline two\nline three");
                const sptr_t annotation_style_offset =
                    fixture.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
                fixture.editor.send(SCI_ANNOTATIONSETSTYLEOFFSET, annotation_style_offset);
                fixture.editor.send(SCI_STYLESETFORE, annotation_style_offset, 0x000000);
                fixture.editor.send(SCI_STYLESETBACK, annotation_style_offset, 0xC2F4FF);
                fixture.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_BOXED, 0);
                fixture.editor.send(SCI_ANNOTATIONSETSTYLE, 0, 0);
                const char *annot_text = "boxed annotation text";
                fixture.editor.send(SCI_ANNOTATIONSETTEXT, 0,
                    reinterpret_cast<sptr_t>(annot_text));
                fixture.pump();
            }
        },
        {
            QStringLiteral("eol_annotation_boxed"),
            [](Fixture_editor &fixture) {
                fixture.set_text("line one\nline two\n");
                const sptr_t eol_annotation_style_offset =
                    fixture.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
                fixture.editor.send(SCI_EOLANNOTATIONSETSTYLEOFFSET, eol_annotation_style_offset);
                fixture.editor.send(SCI_STYLESETFORE, eol_annotation_style_offset, 0x000000);
                fixture.editor.send(SCI_STYLESETBACK, eol_annotation_style_offset, 0xC2F4FF);
                fixture.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_BOXED);
                fixture.editor.send(SCI_EOLANNOTATIONSETSTYLE, 0, 0);
                const char *annot_text = "boxed eol annotation";
                fixture.editor.send(SCI_EOLANNOTATIONSETTEXT, 0,
                    reinterpret_cast<sptr_t>(annot_text));
                fixture.pump();
            }
        },
    };

    for (int step = 0; step < 48; ++step) {
        fixtures.push_back(make_wheel_review_fixture(
            QStringLiteral("scroll_wheel_bounce_unwrapped_step_%1").arg(step, 2, 10, QChar('0')),
            false,
            step + 1));
    }

    for (int step = 0; step < 24; ++step) {
        fixtures.push_back(make_wheel_review_fixture(
            QStringLiteral("scroll_wheel_bounce_wrapped_step_%1").arg(step, 2, 10, QChar('0')),
            true,
            step + 1));
    }

    return fixtures;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_FONT_DPI", "96");
    qputenv("QT_SCALE_FACTOR", "1");
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);

    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ScintillaQuick review capture"));
    parser.addHelpOption();

    QCommandLineOption output_option(
        QStringList{QStringLiteral("o"), QStringLiteral("output-dir")},
        QStringLiteral("Write captures to <dir>."),
        QStringLiteral("dir"),
        QStringLiteral("review_capture_output"));
    QCommandLineOption fixture_option(
        QStringList{QStringLiteral("f"), QStringLiteral("fixture")},
        QStringLiteral("Generate only the named fixture(s)."),
        QStringLiteral("name"));

    parser.addOption(output_option);
    parser.addOption(fixture_option);
    parser.process(app);

    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        qCritical("%s", qPrintable(font_error));
        return 1;
    }

    {
        const QString family = scintillaquick::shared::deterministic_test_font_family();
        QFont probe(family, 11);
        QFontInfo info(probe);
        if (info.family().compare(family, Qt::CaseInsensitive) != 0) {
            qCritical("FAIL: bundled test font not available (resolved to '%s').",
                      qPrintable(info.family()));
            return 1;
        }
    }

    const QString output_dir = QFileInfo(parser.value(output_option)).absoluteFilePath();
    const QStringList selected_fixtures = parser.values(fixture_option);
    const auto should_run_fixture = [&](const QString &name) {
        return selected_fixtures.isEmpty() || selected_fixtures.contains(name);
    };

    int generated_count = 0;
    for (const Review_fixture &fixture_def : review_fixtures()) {
        if (!should_run_fixture(fixture_def.name)) {
            continue;
        }

        Fixture_editor fixture;
        fixture_def.setup(fixture);

        const QImage reference = fixture.capture_reference_image();
        const QImage quick = fixture.capture_quick_image();
        if (reference.isNull() || quick.isNull()) {
            qCritical("FAIL: could not capture fixture '%s'", qPrintable(fixture_def.name));
            return 1;
        }
        if (reference.size() != quick.size()) {
            qCritical(
                "FAIL: capture size mismatch for fixture '%s' (reference %dx%d, quick %dx%d, scale %.3fx%.3f)",
                qPrintable(fixture_def.name),
                reference.width(),
                reference.height(),
                quick.width(),
                quick.height(),
                reference.width() > 0
                    ? static_cast<double>(quick.width()) / static_cast<double>(reference.width())
                    : 0.0,
                reference.height() > 0
                    ? static_cast<double>(quick.height()) / static_cast<double>(reference.height())
                    : 0.0);
            return 1;
        }

        const Comparison_result comparison = compare_images(quick, reference);
        const QString fixture_dir = QDir(output_dir).filePath(fixture_def.name);

        if (!write_image(QDir(fixture_dir).filePath(QStringLiteral("scintilla_reference.png")), reference) ||
            !write_image(QDir(fixture_dir).filePath(QStringLiteral("scintillaquick.png")), quick) ||
            !write_image(QDir(fixture_dir).filePath(QStringLiteral("diff.png")), comparison.diff_image)) {
            qCritical("FAIL: could not write output for fixture '%s'", qPrintable(fixture_def.name));
            return 1;
        }

        qDebug().noquote()
            << QStringLiteral("%1: wrote review set to %2 (diff pixels=%3, %4%%)")
                  .arg(fixture_def.name)
                  .arg(fixture_dir)
                  .arg(comparison.differing_pixels)
                  .arg(comparison.diff_ratio * 100.0, 0, 'f', 2);
        ++generated_count;
    }

    if (generated_count == 0) {
        qCritical("FAIL: no fixtures selected");
        return 1;
    }

    qDebug("Generated %d review fixture set(s).", generated_count);
    return 0;
}
