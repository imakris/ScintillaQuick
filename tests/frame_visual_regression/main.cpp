// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/ScintillaQuickItem.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QThread>

#include <cstring>
#include <functional>

#include "Scintilla.h"

// This test exercises Scintilla::Internal render data directly, so keeping the
// internal namespace open here keeps the assertions readable.
using namespace Scintilla::Internal;

// ---------------------------------------------------------------------------
// Configuration -- injected by CMake at compile time
// ---------------------------------------------------------------------------

#ifndef BASELINE_DIR
#define BASELINE_DIR "."
#endif

#ifndef ARTIFACT_DIR
#define ARTIFACT_DIR "."
#endif

// Per-channel tolerance for pixel comparison.  A small value absorbs
// sub-pixel anti-aliasing jitter that can appear between otherwise
// identical runs on the same machine.
static constexpr int k_channel_tolerance = 2;
// A single stray pixel can still appear nondeterministically on the
// same machine after all other rendering inputs are pinned.
static constexpr int k_max_differing_pixels = 1;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

int g_pass_count  = 0;
int g_fail_count  = 0;
int g_gen_count   = 0;

bool trace_enabled();
void trace_line(const QString &line);

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
        window.setColor(Qt::white);
        window.resize(640, 480);

        editor.setParentItem(window.contentItem());
        editor.setWidth(640);
        editor.setHeight(480);
        editor.setProperty("font", QFont(QStringLiteral("Consolas"), 11));
        editor.send(SCI_SETCARETPERIOD, 0);

        // Show the window so the software scene graph gets initialised.
        window.show();
        window.raise();
        window.requestActivate();
        editor.on_paint_node_updated = [&]() { ++paint_counter; };
        pump();
    }

    ~Fixture_editor()
    {
        editor.send(SCI_SETFOCUS, 0);
        editor.setParentItem(nullptr);
        window.close();
        pump();
    }

    void set_text(const char *text)
    {
        editor.send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(text));
        pump();
    }

    void set_text(const QString &text)
    {
        const QByteArray utf8 = text.toUtf8();
        set_text(utf8.constData());
    }

    void pump()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
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

    QString window_state_text(const char *fixture_name, const char *stage) const
    {
        const QSize window_size = window.size();
        const QSize content_size = window.contentItem()
            ? window.contentItem()->size().toSize()
            : QSize();

        QString state;
        state += QStringLiteral("fixture=%1\n").arg(QString::fromUtf8(fixture_name));
        state += QStringLiteral("stage=%1\n").arg(QString::fromUtf8(stage));
        state += QStringLiteral("window_visible=%1\n").arg(window.isVisible());
        state += QStringLiteral("window_exposed=%1\n").arg(window.isExposed());
        state += QStringLiteral("window_active=%1\n").arg(window.isActive());
        state += QStringLiteral("window_size=%1x%2\n")
                     .arg(window_size.width())
                     .arg(window_size.height());
        state += QStringLiteral("content_size=%1x%2\n")
                     .arg(content_size.width())
                     .arg(content_size.height());
        state += QStringLiteral("editor_active_focus=%1\n")
                     .arg(editor.hasActiveFocus());
        state += QStringLiteral("editor_size=%1x%2\n")
                     .arg(static_cast<int>(editor.width()))
                     .arg(static_cast<int>(editor.height()));
        return state;
    }

    void dump_window_state(const char *fixture_name, const char *stage) const
    {
        const QString state = window_state_text(fixture_name, stage);
        qWarning().noquote() << state.trimmed();

        QDir().mkpath(QStringLiteral(ARTIFACT_DIR));
        const QString path =
            QStringLiteral(ARTIFACT_DIR) + QDir::separator()
            + QString::fromUtf8(fixture_name)
            + QStringLiteral("_state.txt");
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            file.write(state.toUtf8());
        }
    }

    bool wait_for_ready(const char *fixture_name, int timeout_ms = 2000)
    {
        QElapsedTimer timer;
        timer.start();

        while (timer.elapsed() < timeout_ms) {
            pump();
            if (window.isExposed() && editor.hasActiveFocus()) {
                return true;
            }

            window.raise();
            window.requestActivate();
            editor.forceActiveFocus();
            editor.send(SCI_SETFOCUS, 1);
        }

        dump_window_state(fixture_name, "ready-timeout");
        return false;
    }

    QImage capture_image(const char *fixture_name)
    {
        pump();
        trace_line(QStringLiteral("capture[%1]: begin").arg(QString::fromUtf8(fixture_name)));

        if (!wait_for_ready(fixture_name)) {
            qWarning("capture[%s]: window did not become exposed/focused",
                     fixture_name);
            return QImage();
        }

        // Resize window to match editor so the grab is pixel-exact.
        const int ew = static_cast<int>(editor.width());
        const int eh = static_cast<int>(editor.height());
        window.resize(ew, eh);
        window.raise();
        window.requestActivate();
        editor.forceActiveFocus();
        editor.send(SCI_SETFOCUS, 1);
        pump();

        if (!wait_for_ready(fixture_name)) {
            qWarning("capture[%s]: window became unavailable after resize",
                     fixture_name);
            return QImage();
        }

        // grabWindow() forces a synchronous render with the software
        // scene graph and returns the result directly.
        QImage result;
        for (int attempt = 0; attempt < 3; ++attempt) {
            result = window.grabWindow();
            if (!result.isNull()) {
                break;
            }
            pump();
        }

        trace_line(QStringLiteral("capture[%1]: rendered %2x%3")
                       .arg(QString::fromUtf8(fixture_name))
                       .arg(result.width())
                       .arg(result.height()));

        if (result.isNull()) {
            dump_window_state(fixture_name, "grab-null");
            qWarning("capture[%s]: grabWindow() returned null", fixture_name);
        }

        return result;
    }

    QImage capture_ready_window(const char *fixture_name)
    {
        pump();
        trace_line(QStringLiteral("capture[%1]: begin").arg(QString::fromUtf8(fixture_name)));

        QImage result;
        for (int attempt = 0; attempt < 3; ++attempt) {
            result = window.grabWindow();
            if (!result.isNull()) {
                break;
            }
            pump();
        }

        trace_line(QStringLiteral("capture[%1]: rendered %2x%3")
                       .arg(QString::fromUtf8(fixture_name))
                       .arg(result.width())
                       .arg(result.height()));

        if (result.isNull()) {
            dump_window_state(fixture_name, "grab-null");
            qWarning("capture[%s]: grabWindow() returned null", fixture_name);
        }

        return result;
    }
};

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

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

bool image_has_visible_content(const QImage &image, const QColor &background)
{
    if (image.isNull()) {
        return false;
    }

    const QColor expected = background.isValid() ? background : QColor(Qt::white);
    const QRgb bg = expected.rgba();
    QImage converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < converted.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(converted.constScanLine(y));
        for (int x = 0; x < converted.width(); ++x) {
            if (line[x] != bg) {
                return true;
            }
        }
    }

    return false;
}

bool image_has_dark_text_pixels(
    const QImage &image,
    int max_luma = 160,
    int min_pixels = 64)
{
    if (image.isNull()) {
        return false;
    }

    QImage converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int dark_pixels = 0;
    for (int y = 0; y < converted.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(converted.constScanLine(y));
        for (int x = 0; x < converted.width(); ++x) {
            const int alpha = qAlpha(line[x]);
            if (alpha == 0) {
                continue;
            }

            const int luma =
                (qRed(line[x]) * 299 +
                 qGreen(line[x]) * 587 +
                 qBlue(line[x]) * 114) / 1000;
            if (luma <= max_luma) {
                ++dark_pixels;
                if (dark_pixels >= min_pixels) {
                    return true;
                }
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Image comparison
// ---------------------------------------------------------------------------

struct Comparison_result
{
    bool    passed           = false;
    int     differing_pixels = 0;
    double  diff_ratio       = 0.0;
    QImage  diff_image;
};

Comparison_result compare_images(const QImage &actual, const QImage &expected)
{
    Comparison_result result;

    QImage a = actual.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage e = expected.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if (a.size() != e.size()) {
        qWarning("    size mismatch: actual %dx%d vs expected %dx%d",
                 a.width(), a.height(), e.width(), e.height());
        result.diff_image = a;
        return result;
    }

    int w = a.width();
    int h = a.height();
    QImage diff(w, h, QImage::Format_ARGB32_Premultiplied);
    diff.fill(Qt::black);

    int diffs = 0;
    for (int y = 0; y < h; ++y) {
        const QRgb *al = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *el = reinterpret_cast<const QRgb *>(e.constScanLine(y));
        QRgb       *dl = reinterpret_cast<QRgb *>(diff.scanLine(y));

        for (int x = 0; x < w; ++x) {
            int dr = std::abs(qRed(al[x])   - qRed(el[x]));
            int dg = std::abs(qGreen(al[x]) - qGreen(el[x]));
            int db = std::abs(qBlue(al[x])  - qBlue(el[x]));
            int da = std::abs(qAlpha(al[x]) - qAlpha(el[x]));

            if (dr > k_channel_tolerance || dg > k_channel_tolerance ||
                db > k_channel_tolerance || da > k_channel_tolerance) {
                ++diffs;
                dl[x] = qRgb(255, 0, 0);
            }
            else {
                dl[x] = qRgb(qRed(al[x]) / 3,
                              qGreen(al[x]) / 3,
                              qBlue(al[x]) / 3);
            }
        }
    }

    result.differing_pixels = diffs;
    result.diff_ratio = (w * h > 0)
        ? static_cast<double>(diffs) / (w * h)
        : 0.0;
    result.passed = (diffs <= k_max_differing_pixels);
    result.diff_image = diff;
    return result;
}

// ---------------------------------------------------------------------------
// Baseline helpers
// ---------------------------------------------------------------------------

static QString baseline_path(const char *name)
{
    return QStringLiteral(BASELINE_DIR) + QDir::separator() + QString::fromUtf8(name)
         + QStringLiteral(".png");
}

static QString artifact_path(const char *name, const char *suffix)
{
    return QStringLiteral(ARTIFACT_DIR) + QDir::separator() + QString::fromUtf8(name)
         + QStringLiteral("_") + QString::fromUtf8(suffix) + QStringLiteral(".png");
}

enum class Fixture_outcome { pass, fail, generated };

bool regenerate_baselines()
{
    const QByteArray value = qgetenv("SCINTILLAQUICK_VR_GENERATE_BASELINES");
    return !value.isEmpty() && value != "0";
}

bool trace_enabled()
{
    const QByteArray value = qgetenv("SCINTILLAQUICK_VR_TRACE");
    return !value.isEmpty() && value != "0";
}

void trace_line(const QString &line)
{
    if (!trace_enabled()) {
        return;
    }

    QDir().mkpath(QStringLiteral(ARTIFACT_DIR));
    QFile file(QStringLiteral(ARTIFACT_DIR) + QDir::separator()
               + QStringLiteral("trace.log"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(line.toUtf8());
        file.write("\n");
    }
}

Fixture_outcome run_visual_fixture(const char *name, const QImage &actual)
{
    trace_line(QStringLiteral("fixture %1: actual=%2x%3")
                   .arg(QString::fromUtf8(name))
                   .arg(actual.width())
                   .arg(actual.height()));

    if (actual.isNull()) {
        qWarning("  [%s] FAIL: captured image is null", name);
        return Fixture_outcome::fail;
    }

    if (!image_has_visible_content(actual, QColor(Qt::white))) {
        QDir().mkpath(QStringLiteral(ARTIFACT_DIR));
        actual.save(artifact_path(name, "blank"));
        qWarning("  [%s] FAIL: captured image appears blank", name);
        return Fixture_outcome::fail;
    }

    QString bp = baseline_path(name);
    const bool regenerate = regenerate_baselines();
    QImage expected;

    if (!QFile::exists(bp)) {
        if (!regenerate) {
            qWarning("  [%s] FAIL: missing baseline %s", name, qPrintable(bp));
            return Fixture_outcome::fail;
        }
    }
    else
    if (!regenerate) {
        expected.load(bp);
        if (expected.isNull()) {
            qWarning("  [%s] FAIL: could not load baseline %s", name, qPrintable(bp));
            return Fixture_outcome::fail;
        }
    }

    if (regenerate) {
        QDir().mkpath(QStringLiteral(BASELINE_DIR));
        if (!actual.save(bp)) {
            qWarning("  [%s] FAIL: could not save baseline to %s",
                     name, qPrintable(bp));
            return Fixture_outcome::fail;
        }
        qDebug("  [%s] GENERATED baseline: %s", name, qPrintable(bp));
        return Fixture_outcome::generated;
    }

    Comparison_result cmp = compare_images(actual, expected);
    if (cmp.passed) {
        return Fixture_outcome::pass;
    }

    // Mismatch -- save artifacts.
    QDir().mkpath(QStringLiteral(ARTIFACT_DIR));
    actual.save(artifact_path(name, "actual"));
    expected.save(artifact_path(name, "expected"));
    cmp.diff_image.save(artifact_path(name, "diff"));

    qWarning("  [%s] FAIL: %d differing pixels (%.2f%%), artifacts saved to %s",
             name, cmp.differing_pixels, cmp.diff_ratio * 100.0,
             qPrintable(QStringLiteral(ARTIFACT_DIR)));
    return Fixture_outcome::fail;
}

QImage capture_stable_image(Fixture_editor &editor, const QByteArray &fixture_name, int attempts = 6)
{
    QImage previous = editor.capture_ready_window(fixture_name.constData());
    for (int attempt = 1; attempt < attempts; ++attempt) {
        editor.pump();
        QThread::msleep(10);
        editor.pump();
        QImage current = editor.capture_ready_window(fixture_name.constData());
        if (compare_images(current, previous).passed) {
            return current;
        }
        previous = current;
    }
    return previous;
}

Fixture_outcome run_scroll_probe_fixture(
    const QString &scenario_name,
    Fixture_editor &editor,
    QQuickWindow &window,
    const QPointF &wheel_point,
    int steps,
    bool wrapped)
{
    bool generated_any = false;
    for (int step = 0; step < steps; ++step) {
        const int previous_first_visible_line = static_cast<int>(editor.editor.send(SCI_GETFIRSTVISIBLELINE));
        const quint64 previous_paint_counter = editor.paint_counter;
        const bool wheel_up = (step % 2) == 0;
        send_wheel_event(
            window,
            editor.editor,
            wheel_point,
            QPoint(),
            QPoint(0, wheel_up ? 120 : -120));
        editor.pump();
        QElapsedTimer scroll_timer;
        scroll_timer.start();
        int current_first_visible_line = previous_first_visible_line;
        while (scroll_timer.elapsed() < 100) {
            editor.pump();
            current_first_visible_line = static_cast<int>(editor.editor.send(SCI_GETFIRSTVISIBLELINE));
            if (current_first_visible_line != previous_first_visible_line) {
                break;
            }
        }
        if (!editor.wait_for_next_paint(previous_paint_counter)) {
            qWarning("  [%s] FAIL: timed out waiting for repaint after wheel step %d",
                     scenario_name.toUtf8().constData(),
                     step);
            return Fixture_outcome::fail;
        }
        if (current_first_visible_line == previous_first_visible_line) {
            qWarning("  [%s] FAIL: wheel step %d did not change first visible line",
                     scenario_name.toUtf8().constData(),
                     step);
            return Fixture_outcome::fail;
        }

        const QString step_name = QStringLiteral("%1_step_%2")
                                      .arg(scenario_name)
                                      .arg(step, 2, 10, QChar('0'));
        trace_line(QStringLiteral("scroll-probe %1: step=%2 wrapped=%3")
                       .arg(scenario_name)
                       .arg(step)
                       .arg(wrapped));

        const QByteArray step_name_bytes = step_name.toUtf8();
        const QImage captured = capture_stable_image(editor, step_name_bytes);
        if (!image_has_dark_text_pixels(captured)) {
            QDir().mkpath(QStringLiteral(ARTIFACT_DIR));
            captured.save(artifact_path(step_name_bytes.constData(), "missing_text"));
            qWarning("  [%s] FAIL: wheel step %d produced a frame without visible text",
                     scenario_name.toUtf8().constData(),
                     step);
            return Fixture_outcome::fail;
        }
        Fixture_outcome outcome = run_visual_fixture(step_name_bytes.constData(), captured);
        if (outcome == Fixture_outcome::fail) {
            return outcome;
        }
        if (outcome == Fixture_outcome::generated) {
            generated_any = true;
        }
        else
        if (outcome != Fixture_outcome::pass) {
            return outcome;
        }
    }

    return generated_any ? Fixture_outcome::generated : Fixture_outcome::pass;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture functions
// ---------------------------------------------------------------------------

static Fixture_outcome vr_plain_ascii_short()
{
    Fixture_editor f;
    f.set_text("alpha beta gamma");
    return run_visual_fixture("plain_ascii_short",
                              f.capture_image("plain_ascii_short"));
}

static Fixture_outcome vr_plain_ascii_long_wrap()
{
    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    f.set_text(
        "The quick brown fox jumps over the lazy dog and then runs around "
        "the garden several more times until the text is long enough to "
        "definitely wrap into multiple visual sublines at a narrow width.");
    return run_visual_fixture("plain_ascii_long_wrap",
                              f.capture_image("plain_ascii_long_wrap"));
}

static Fixture_outcome vr_mixed_styles_wrap()
{
    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    f.editor.send(SCI_STYLESETFONT,
                  STYLE_DEFAULT,
                  reinterpret_cast<sptr_t>("Consolas"));
    f.editor.send(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    f.editor.send(SCI_STYLECLEARALL);
    f.editor.send(SCI_STYLESETFORE, 1, 0x000000);
    f.editor.send(SCI_STYLESETBOLD, 1, 1);
    f.editor.send(SCI_STYLESETFORE, 2, 0x0000FF);
    f.editor.send(SCI_STYLESETITALIC, 2, 1);
    f.editor.send(SCI_STYLESETFORE, 3, 0xFF0000);

    const char *text =
        "keyword identifier literal keyword identifier literal "
        "keyword identifier literal keyword identifier literal end";
    f.set_text(text);

    int styles[] = {1, 2, 3};
    int pos = 0;
    const int text_len = static_cast<int>(std::strlen(text));
    int word_index = 0;
    while (pos < text_len) {
        if (text[pos] == ' ') { ++pos; continue; }
        int word_len = 0;
        while (pos + word_len < text_len && text[pos + word_len] != ' ')
            ++word_len;
        f.editor.send(SCI_STARTSTYLING, pos);
        f.editor.send(SCI_SETSTYLING, word_len, styles[word_index % 3]);
        pos += word_len;
        ++word_index;
    }

    return run_visual_fixture("mixed_styles_wrap",
                              f.capture_image("mixed_styles_wrap"));
}

static Fixture_outcome vr_selection_single_line()
{
    Fixture_editor f;
    f.set_text("select this word here");
    f.editor.send(SCI_SETSEL, 7, 11);
    return run_visual_fixture("selection_single_line",
                              f.capture_image("selection_single_line"));
}

static Fixture_outcome vr_selection_wrap_boundary()
{
    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    const char *text =
        "The quick brown fox jumps over the lazy dog and then continues "
        "running around the field to ensure we get enough wrapping here.";
    f.set_text(text);

    int text_len = static_cast<int>(std::strlen(text));
    f.editor.send(SCI_SETSEL, text_len / 3, 2 * text_len / 3);
    return run_visual_fixture("selection_wrap_boundary",
                              f.capture_image("selection_wrap_boundary"));
}

static Fixture_outcome vr_caret_mid_line()
{
    Fixture_editor f;
    f.set_text("caret goes here in the middle of styled text");
    f.editor.send(SCI_GOTOPOS, 16);
    return run_visual_fixture("caret_mid_line",
                              f.capture_image("caret_mid_line"));
}

static Fixture_outcome vr_caret_wrap_continuation()
{
    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    const char *text =
        "The quick brown fox jumps over the lazy dog and then continues "
        "running around the field to test caret on a wrapped continuation.";
    f.set_text(text);
    f.editor.send(SCI_GOTOPOS, static_cast<int>(std::strlen(text)) - 10);
    return run_visual_fixture("caret_wrap_continuation",
                              f.capture_image("caret_wrap_continuation"));
}

static Fixture_outcome vr_current_line_basic()
{
    Fixture_editor f;
    f.editor.send(SCI_SETCARETLINEVISIBLE, 1);
    f.editor.send(SCI_SETCARETLINEBACK, 0xFFFFE0);
    f.set_text("first line\nsecond line\nthird line");
    f.editor.send(SCI_GOTOPOS, 5);
    return run_visual_fixture("current_line_basic",
                              f.capture_image("current_line_basic"));
}

static Fixture_outcome vr_current_line_wrap()
{
    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    f.editor.send(SCI_SETCARETLINEVISIBLE, 1);
    f.editor.send(SCI_SETCARETLINEBACK, 0xFFFFE0);

    const char *text =
        "The quick brown fox jumps over the lazy dog and then continues "
        "running around the field.";
    f.set_text(text);
    f.editor.send(SCI_GOTOPOS, static_cast<int>(std::strlen(text)) - 5);
    return run_visual_fixture("current_line_wrap",
                              f.capture_image("current_line_wrap"));
}

static Fixture_outcome vr_margin_numbers_basic()
{
    Fixture_editor f;
    f.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    f.editor.send(SCI_SETMARGINWIDTHN, 0, 40);
    f.set_text("line one\nline two\nline three\nline four\nline five");
    return run_visual_fixture("margin_numbers_basic",
                              f.capture_image("margin_numbers_basic"));
}

static Fixture_outcome vr_margin_numbers_wrap()
{
    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    f.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    f.editor.send(SCI_SETMARGINWIDTHN, 0, 40);

    f.set_text(
        "This is a very long first line that should definitely wrap into "
        "multiple visual sublines when shown at narrow width.\n"
        "Short second line.\n"
        "Third line here.");
    return run_visual_fixture("margin_numbers_wrap",
                              f.capture_image("margin_numbers_wrap"));
}

static Fixture_outcome vr_plain_indicator_basic()
{
    Fixture_editor f;
    f.set_text("indicator on this text here");

    f.editor.send(SCI_INDICSETSTYLE, 0, INDIC_PLAIN);
    f.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
    f.editor.send(SCI_INDICSETUNDER, 0, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 0);
    f.editor.send(SCI_INDICATORFILLRANGE, 13, 9);

    return run_visual_fixture("plain_indicator_basic",
                              f.capture_image("plain_indicator_basic"));
}

static Fixture_outcome vr_control_repr_simple()
{
    Fixture_editor f;
    // Text with control characters that trigger representation rendering.
    f.set_text("hello\x01world\x02end");
    return run_visual_fixture("control_repr_simple",
                              f.capture_image("control_repr_simple"));
}

static Fixture_outcome vr_multi_selection()
{
    Fixture_editor f;
    f.set_text("aaa bbb ccc ddd eee fff");
    f.editor.send(SCI_SETSELECTION, 0, 3);
    f.editor.send(SCI_ADDSELECTION, 4, 7);
    f.editor.send(SCI_ADDSELECTION, 8, 11);
    return run_visual_fixture("multi_selection",
                              f.capture_image("multi_selection"));
}

static Fixture_outcome vr_rectangular_selection()
{
    Fixture_editor f;
    f.set_text("line one\nline two\nline three");
    f.editor.send(SCI_SETRECTANGULARSELECTIONANCHOR, 0);
    f.editor.send(SCI_SETRECTANGULARSELECTIONCARET, 22);
    f.editor.send(SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE, 0);
    f.editor.send(SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE, 0);
    return run_visual_fixture("rectangular_selection",
                              f.capture_image("rectangular_selection"));
}

static Fixture_outcome vr_current_line_frame()
{
    Fixture_editor f;
    f.editor.send(SCI_SETCARETLINEVISIBLE, 1);
    f.editor.send(SCI_SETCARETLINEBACK, 0xFFFFE0);
    f.editor.send(SCI_SETCARETLINEFRAME, 1);
    f.set_text("first line\nsecond line\nthird line");
    f.editor.send(SCI_GOTOPOS, 5);
    return run_visual_fixture("current_line_frame",
                              f.capture_image("current_line_frame"));
}

static Fixture_outcome vr_squiggle_indicator()
{
    Fixture_editor f;
    f.editor.setWidth(900);
    f.editor.setHeight(220);
    f.set_text("error on this word here");
    f.editor.send(SCI_INDICSETSTYLE, 0, INDIC_SQUIGGLE);
    f.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
    f.editor.send(SCI_INDICSETUNDER, 0, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 0);
    f.editor.send(SCI_INDICATORFILLRANGE, 9, 4);
    return run_visual_fixture("squiggle_indicator",
                              f.capture_image("squiggle_indicator"));
}

static Fixture_outcome vr_box_indicator()
{
    Fixture_editor f;
    f.editor.setWidth(900);
    f.editor.setHeight(220);
    f.set_text("box around this word");
    f.editor.send(SCI_INDICSETSTYLE, 1, INDIC_BOX);
    f.editor.send(SCI_INDICSETFORE, 1, 0xFF0000);
    f.editor.send(SCI_INDICSETUNDER, 1, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 1);
    f.editor.send(SCI_INDICATORFILLRANGE, 4, 6);
    return run_visual_fixture("box_indicator",
                              f.capture_image("box_indicator"));
}

static Fixture_outcome vr_marker_symbol()
{
    Fixture_editor f;
    f.editor.send(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
    f.editor.send(SCI_SETMARGINWIDTHN, 1, 16);
    f.editor.send(SCI_SETMARGINMASKN, 1, 0x01);
    f.editor.send(SCI_MARKERDEFINE, 0, SC_MARK_CIRCLE);
    f.editor.send(SCI_MARKERSETFORE, 0, 0x000000);
    f.editor.send(SCI_MARKERSETBACK, 0, 0xFF0000);
    f.set_text("marked line\nunmarked line\nthird line");
    f.editor.send(SCI_MARKERADD, 0, 0);
    return run_visual_fixture("marker_symbol",
                              f.capture_image("marker_symbol"));
}

static Fixture_outcome vr_multi_caret()
{
    Fixture_editor f;
    f.set_text("abc def ghi jkl");
    f.editor.send(SCI_SETSELECTION, 4, 4);
    f.editor.send(SCI_ADDSELECTION, 8, 8);
    f.editor.send(SCI_ADDSELECTION, 12, 12);
    return run_visual_fixture("multi_caret",
                              f.capture_image("multi_caret"));
}

static Fixture_outcome vr_whitespace_visible()
{
    Fixture_editor f;
    f.editor.send(SCI_SETVIEWWS, SCWS_VISIBLEALWAYS);
    f.set_text("a b\tc");
    return run_visual_fixture("whitespace_visible",
                              f.capture_image("whitespace_visible"));
}

static Fixture_outcome vr_eol_annotation()
{
    Fixture_editor f;
    f.set_text("line one\nline two\n");
    f.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STANDARD);
    const char *annot_text = "this is an eol annotation";
    f.editor.send(SCI_EOLANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
    f.pump();
    return run_visual_fixture("eol_annotation",
                              f.capture_image("eol_annotation"));
}

static Fixture_outcome vr_annotation()
{
    Fixture_editor f;
    f.set_text("line one\nline two\nline three");
    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_STANDARD);
    const char *annot_text = "annotation text here";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
    f.pump();
    return run_visual_fixture("annotation",
                              f.capture_image("annotation"));
}

static Fixture_outcome vr_indent_guide()
{
    Fixture_editor f;
    f.editor.send(SCI_SETINDENTATIONGUIDES, SC_IV_REAL);
    f.editor.send(SCI_SETTABWIDTH, 4);
    f.editor.send(SCI_SETINDENT, 4);
    f.set_text("if (x) {\n    if (y) {\n        z = 1;\n    }\n}");
    return run_visual_fixture("indent_guide",
                              f.capture_image("indent_guide"));
}

static Fixture_outcome vr_style_underline()
{
    Fixture_editor f;
    f.editor.setWidth(900);
    f.editor.setHeight(220);
    f.editor.send(SCI_STYLESETUNDERLINE, STYLE_DEFAULT, 1);
    f.set_text("style underline here");
    f.editor.send(SCI_STARTSTYLING, 0);
    f.editor.send(SCI_SETSTYLING, 18, STYLE_DEFAULT);
    f.pump();
    return run_visual_fixture("style_underline",
                              f.capture_image("style_underline"));
}

static Fixture_outcome vr_scroll_wheel_bounce_unwrapped()
{
    Fixture_editor f;
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    f.set_text(build_large_document(25000));
    f.editor.send(SCI_SETFIRSTVISIBLELINE, 120);
    f.pump();

    const QPointF wheel_point(
        std::max<qreal>(f.editor.width() * 0.5, 32.0),
        std::max<qreal>(f.editor.height() * 0.5, 32.0));

    return run_scroll_probe_fixture(
        QStringLiteral("scroll_wheel_bounce_unwrapped"),
        f,
        f.window,
        wheel_point,
        48,
        false);
}

static Fixture_outcome vr_scroll_wheel_bounce_wrapped()
{
    Fixture_editor f;
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    f.set_text(build_wrapped_document(4000));
    f.editor.send(SCI_SETFIRSTVISIBLELINE, 120);
    f.pump();

    const QPointF wheel_point(
        std::max<qreal>(f.editor.width() * 0.5, 32.0),
        std::max<qreal>(f.editor.height() * 0.5, 32.0));

    return run_scroll_probe_fixture(
        QStringLiteral("scroll_wheel_bounce_wrapped"),
        f,
        f.window,
        wheel_point,
        24,
        true);
}

// ---------------------------------------------------------------------------
// Phase 8 VR fixtures: fold / marker / annotation fidelity
// ---------------------------------------------------------------------------

static Fixture_outcome vr_fold_display_text()
{
    Fixture_editor f;
    f.editor.send(SCI_SETPROPERTY,
        reinterpret_cast<uptr_t>("fold"), reinterpret_cast<sptr_t>("1"));
    f.editor.send(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
    f.editor.send(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
    f.editor.send(SCI_SETMARGINWIDTHN, 2, 16);
    f.editor.send(SCI_SETAUTOMATICFOLD,
        SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CHANGE, 0);
    f.editor.send(SCI_SETDEFAULTFOLDDISPLAYTEXT, 0,
        reinterpret_cast<sptr_t>("..."));
    f.editor.send(SCI_FOLDDISPLAYTEXTSETSTYLE, SC_FOLDDISPLAYTEXT_BOXED, 0);

    f.set_text("if (true) {\n    body line 1\n    body line 2\n}");
    f.editor.send(SCI_SETFOLDLEVEL, 0,
        SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
    f.editor.send(SCI_SETFOLDLEVEL, 1, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 2, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 3, SC_FOLDLEVELBASE);
    f.pump();

    f.editor.send(SCI_FOLDLINE, 0, SC_FOLDACTION_CONTRACT);
    f.pump();
    return run_visual_fixture("fold_display_text",
                              f.capture_image("fold_display_text"));
}

static Fixture_outcome vr_fold_markers()
{
    Fixture_editor f;
    f.editor.send(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
    f.editor.send(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
    f.editor.send(SCI_SETMARGINWIDTHN, 2, 16);

    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND,
        SC_MARK_BOXPLUSCONNECTED);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID,
        SC_MARK_BOXMINUSCONNECTED);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

    f.set_text("function a() {\n    line1\n    line2\n}");
    f.editor.send(SCI_SETFOLDLEVEL, 0,
        SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
    f.editor.send(SCI_SETFOLDLEVEL, 1, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 2, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 3, SC_FOLDLEVELBASE);
    f.pump();
    return run_visual_fixture("fold_markers",
                              f.capture_image("fold_markers"));
}

static Fixture_outcome vr_annotation_boxed()
{
    Fixture_editor f;
    f.set_text("line one\nline two\nline three");
    const sptr_t annotation_style_offset =
        f.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
    f.editor.send(SCI_ANNOTATIONSETSTYLEOFFSET, annotation_style_offset);
    f.editor.send(SCI_STYLESETFORE, annotation_style_offset, 0x000000);
    f.editor.send(SCI_STYLESETBACK, annotation_style_offset, 0xC2F4FF);
    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_BOXED, 0);
    f.editor.send(SCI_ANNOTATIONSETSTYLE, 0, 0);
    const char *annot_text = "boxed annotation text";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 0,
        reinterpret_cast<sptr_t>(annot_text));
    f.pump();
    return run_visual_fixture("annotation_boxed",
                              f.capture_image("annotation_boxed"));
}

// ---------------------------------------------------------------------------
// Phase 9: EOL annotation boxed
// ---------------------------------------------------------------------------

static Fixture_outcome vr_eol_annotation_boxed()
{
    Fixture_editor f;
    f.set_text("line one\nline two\n");
    const sptr_t eol_annotation_style_offset =
        f.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
    f.editor.send(SCI_EOLANNOTATIONSETSTYLEOFFSET, eol_annotation_style_offset);
    f.editor.send(SCI_STYLESETFORE, eol_annotation_style_offset, 0x000000);
    f.editor.send(SCI_STYLESETBACK, eol_annotation_style_offset, 0xC2F4FF);
    f.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_BOXED);
    f.editor.send(SCI_EOLANNOTATIONSETSTYLE, 0, 0);
    const char *annot_text = "boxed eol annotation";
    f.editor.send(SCI_EOLANNOTATIONSETTEXT, 0,
                  reinterpret_cast<sptr_t>(annot_text));
    f.pump();
    return run_visual_fixture("eol_annotation_boxed",
                              f.capture_image("eol_annotation_boxed"));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    // Truncate the trace log from prior runs.
    if (trace_enabled()) {
        QDir().mkpath(QStringLiteral(ARTIFACT_DIR));
        QFile::remove(QStringLiteral(ARTIFACT_DIR) + QDir::separator()
                      + QStringLiteral("trace.log"));
    }

    trace_line(QStringLiteral("main: start"));
    // Pin DPI and scale factor for deterministic rendering.
    qputenv("QT_FONT_DPI", "96");
    qputenv("QT_SCALE_FACTOR", "1");
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);

    QGuiApplication app(argc, argv);
    trace_line(QStringLiteral("main: app created"));

    QString selected_fixture_name;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--fixture") && i + 1 < argc) {
            selected_fixture_name = QString::fromLocal8Bit(argv[++i]);
        }
        else
        if (arg.startsWith(QStringLiteral("--fixture="))) {
            selected_fixture_name = arg.mid(QStringLiteral("--fixture=").size());
        }
    }

    // Verify the test font is available.
    {
        QFont probe(QStringLiteral("Consolas"), 11);
        QFontInfo info(probe);
        if (info.family().compare(QStringLiteral("Consolas"),
                                  Qt::CaseInsensitive) != 0) {
            qCritical("FAIL: Consolas font not available (resolved to '%s'). "
                      "Visual regression baselines require Consolas.",
                      qPrintable(info.family()));
            return 1;
        }
    }

    qDebug("=== Visual Regression Tests ===");
    qDebug("  baseline dir : %s", BASELINE_DIR);
    qDebug("  artifact dir : %s", ARTIFACT_DIR);
    qDebug("  regenerate baselines : %s",
           regenerate_baselines() ? "yes" : "no");
    qDebug("");
    trace_line(QStringLiteral("main: fixtures starting"));

    struct { const char *name; Fixture_outcome (*fn)(); } fixtures[] = {
        {"plain_ascii_short",         vr_plain_ascii_short},
        {"plain_ascii_long_wrap",     vr_plain_ascii_long_wrap},
        {"mixed_styles_wrap",         vr_mixed_styles_wrap},
        {"selection_single_line",     vr_selection_single_line},
        {"selection_wrap_boundary",   vr_selection_wrap_boundary},
        {"caret_mid_line",            vr_caret_mid_line},
        {"caret_wrap_continuation",   vr_caret_wrap_continuation},
        {"current_line_basic",        vr_current_line_basic},
        {"current_line_wrap",         vr_current_line_wrap},
        {"margin_numbers_basic",      vr_margin_numbers_basic},
        {"margin_numbers_wrap",       vr_margin_numbers_wrap},
        {"plain_indicator_basic",     vr_plain_indicator_basic},
        {"control_repr_simple",       vr_control_repr_simple},
        {"multi_selection",           vr_multi_selection},
        {"rectangular_selection",     vr_rectangular_selection},
        {"current_line_frame",        vr_current_line_frame},
        {"squiggle_indicator",        vr_squiggle_indicator},
        {"box_indicator",             vr_box_indicator},
        {"marker_symbol",             vr_marker_symbol},
        {"multi_caret",               vr_multi_caret},
        {"whitespace_visible",        vr_whitespace_visible},
        {"eol_annotation",            vr_eol_annotation},
        {"annotation",                vr_annotation},
        {"indent_guide",              vr_indent_guide},
        {"style_underline",           vr_style_underline},
        // Phase 8: fold / marker / annotation fidelity
        {"fold_display_text",         vr_fold_display_text},
        {"fold_markers",              vr_fold_markers},
        {"annotation_boxed",          vr_annotation_boxed},
        // Phase 9: EOL annotation boxed fidelity
        {"eol_annotation_boxed",      vr_eol_annotation_boxed},
        // Scroll-specific regression probes
        {"scroll_wheel_bounce_unwrapped", vr_scroll_wheel_bounce_unwrapped},
        {"scroll_wheel_bounce_wrapped",   vr_scroll_wheel_bounce_wrapped},
    };

    bool ran_any_fixture = false;
    for (const auto &f : fixtures) {
        if (!selected_fixture_name.isEmpty() &&
            selected_fixture_name != QString::fromLatin1(f.name)) {
            continue;
        }
        ran_any_fixture = true;
        qDebug("--- %s", f.name);
        Fixture_outcome outcome = f.fn();
        switch (outcome) {
            case Fixture_outcome::pass:
                qDebug("  PASS [%s]\n", f.name);
                ++g_pass_count;
                break;
            case Fixture_outcome::generated:
                qDebug("  GENERATED [%s] (needs review before commit)\n", f.name);
                ++g_gen_count;
                break;
            case Fixture_outcome::fail:
                qWarning("  FIXTURE FAILED [%s]\n", f.name);
                ++g_fail_count;
                break;
        }
    }

    if (!selected_fixture_name.isEmpty() && !ran_any_fixture) {
        qCritical("FAIL: requested fixture '%s' was not found",
                  qPrintable(selected_fixture_name));
        return 2;
    }

    int total = g_pass_count + g_fail_count + g_gen_count;
    qDebug("=== Results: %d passed, %d generated, %d failed (of %d) ===",
           g_pass_count, g_gen_count, g_fail_count, total);

    if (g_gen_count > 0) {
        qDebug("NOTE: %d baseline(s) were generated for the first time.", g_gen_count);
        qDebug("      Inspect them in %s and commit if correct.", BASELINE_DIR);
    }

    if (g_fail_count > 0) return 1;
    // When baselines were generated but nothing was compared, report SKIP
    // so CTest / CI does not treat an unchecked generation run as a pass.
    if (g_gen_count > 0) return 77;
    return 0;
}
