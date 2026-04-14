// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#define SCINTILLAQUICK_ENABLE_TEST_ACCESS
#include <scintillaquick/ScintillaQuickItem.h>
#undef SCINTILLAQUICK_ENABLE_TEST_ACCESS

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QThread>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <vector>

#include "ScintillaQuickFont.h"
#include "ScintillaQuickWindowBinding.h"
#include "Scintilla.h"

namespace Scintilla::Internal {

class ScintillaQuick_validation_access
{
public:
    static std::vector<Displayed_row_for_test> displayed_rows(::ScintillaQuick_item &item)
    {
        return item.displayed_rows_for_test();
    }
};

} // namespace Scintilla::Internal

namespace {

class Benchmark_editor : public ScintillaQuick_item
{
public:
    using ScintillaQuick_item::ScintillaQuick_item;

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

QString build_large_document(int line_count)
{
    QStringList lines;
    lines.reserve(line_count);
    for (int i = 0; i < line_count; ++i) {
        lines << QString("Line %1: The quick brown fox jumps over the lazy dog. value=%2")
                     .arg(i + 1, 6, 10, QChar('0'))
                     .arg((i * 17) % 9973);
    }
    return lines.join('\n');
}

QString profiling_session_directory(QStringView scenario_name)
{
    const QString safe_name = scenario_name.toString().replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_\\-]")), QStringLiteral("_"));
    return QDir(QDir::tempPath()).filePath(
        QStringLiteral("scintillaquick_bench_%1_%2")
            .arg(safe_name, QUuid::createUuid().toString(QUuid::WithoutBraces)));
}

void cleanup_profiling_session_directory(const QString &directory_path)
{
    if (directory_path.isEmpty()) {
        return;
    }

    const QString keep_reports = qEnvironmentVariable("SCINTILLAQUICK_KEEP_PROFILING_REPORTS").trimmed();
    if (!keep_reports.isEmpty() && keep_reports != QStringLiteral("0")) {
        return;
    }

    QDir directory(directory_path);
    if (directory.exists()) {
        directory.removeRecursively();
    }
}

QJsonObject load_profiling_report(const QString &directory_path)
{
    QElapsedTimer wait_timer;
    wait_timer.start();

    while (wait_timer.elapsed() < 500) {
        QDir directory(directory_path);
        const QFileInfoList reports = directory.entryInfoList(
            QStringList{QStringLiteral("*.json")},
            QDir::Files,
            QDir::Time);
        if (!reports.isEmpty()) {
            QFile file(reports.front().absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                if (doc.isObject()) {
                    return doc.object();
                }
            }
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(10);
    }

    return {};
}

template <typename Fn>
QJsonObject measure_scenario(Benchmark_editor &editor, QStringView name, Fn &&fn)
{
    const QString profiling_dir = profiling_session_directory(name);
    QDir().mkpath(profiling_dir);
    const bool profiling_started = editor.startProfilingSession(profiling_dir, 3600.0);

    QElapsedTimer timer;
    timer.start();
    fn();
    const double elapsed_ms = timer.nsecsElapsed() / 1'000'000.0;

    if (profiling_started && editor.profilingActive()) {
        editor.stopProfilingSession();
    }

    QJsonObject result{
        {QStringLiteral("name"), name.toString()},
        {QStringLiteral("measurement_kind"), QStringLiteral("command_elapsed")},
        {QStringLiteral("elapsed_ms"), elapsed_ms},
    };
    if (profiling_started) {
        const QJsonObject profiling = load_profiling_report(profiling_dir);
        if (!profiling.isEmpty()) {
            result.insert(QStringLiteral("profiling"), profiling);
        }
    }
    cleanup_profiling_session_directory(profiling_dir);
    return result;
}

void pump_gui(int iterations = 3)
{
    for (int i = 0; i < iterations; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
    }
}

struct latency_stats_t
{
    int steps = 0;
    int completed_steps = 0;
    int timeout_count = 0;
    double elapsed_ms = 0.0;
    double min_ms = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double max_ms = 0.0;
};

struct Correctness_issue
{
    int step = -1;
    QString message;
};

latency_stats_t summarize_latencies(const std::vector<double> &latencies_ms, int timeout_count, double elapsed_ms)
{
    latency_stats_t stats;
    stats.steps = static_cast<int>(latencies_ms.size()) + timeout_count;
    stats.completed_steps = static_cast<int>(latencies_ms.size());
    stats.timeout_count = timeout_count;
    stats.elapsed_ms = elapsed_ms;

    if (latencies_ms.empty()) {
        return stats;
    }

    std::vector<double> sorted = latencies_ms;
    std::sort(sorted.begin(), sorted.end());

    const auto percentile = [&](double p) {
        const size_t index = static_cast<size_t>(std::clamp(
            std::llround((static_cast<long long>(sorted.size()) - 1) * p),
            0ll,
            static_cast<long long>(sorted.size() - 1)));
        return sorted[index];
    };

    const double total_ms = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    stats.min_ms = sorted.front();
    stats.mean_ms = total_ms / static_cast<double>(sorted.size());
    stats.median_ms = percentile(0.5);
    stats.p95_ms = percentile(0.95);
    stats.max_ms = sorted.back();
    return stats;
}

QJsonObject latency_stats_to_json(QStringView name, const latency_stats_t &stats)
{
    return QJsonObject{
        {QStringLiteral("name"), name.toString()},
        {QStringLiteral("measurement_kind"), QStringLiteral("paint_latency")},
        {QStringLiteral("steps"), stats.steps},
        {QStringLiteral("completed_steps"), stats.completed_steps},
        {QStringLiteral("timeout_count"), stats.timeout_count},
        {QStringLiteral("elapsed_ms"), stats.elapsed_ms},
        {QStringLiteral("min_ms"), stats.min_ms},
        {QStringLiteral("mean_ms"), stats.mean_ms},
        {QStringLiteral("median_ms"), stats.median_ms},
        {QStringLiteral("p95_ms"), stats.p95_ms},
        {QStringLiteral("max_ms"), stats.max_ms},
    };
}

QString expected_document_line_text(int zero_based_line)
{
    return QString("Line %1: The quick brown fox jumps over the lazy dog. value=%2")
        .arg(zero_based_line + 1, 6, 10, QChar('0'))
        .arg((zero_based_line * 17) % 9973);
}

QString build_wrapped_document(int line_count)
{
    static const QString repeated_block =
        QStringLiteral("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau ");

    QStringList lines;
    lines.reserve(line_count);
    for (int i = 0; i < line_count; ++i) {
        QString line = QString("Wrap %1: ").arg(i + 1, 6, 10, QChar('0'));
        for (int repeat = 0; repeat < 6; ++repeat) {
            line += repeated_block;
        }
        line += QString("payload=%1").arg((i * 29) % 7919);
        lines << line;
    }
    return lines.join('\n');
}

std::vector<Scintilla::Internal::Displayed_row_for_test> visible_rows_for_viewport(Benchmark_editor &editor)
{
    const auto rows = ::Scintilla::Internal::ScintillaQuick_validation_access::displayed_rows(editor);
    std::vector<Scintilla::Internal::Displayed_row_for_test> visible_rows;
    visible_rows.reserve(rows.size());
    const qreal viewport_top = 0.0;
    const qreal viewport_bottom = static_cast<qreal>(editor.height());
    for (const auto &row : rows) {
        if (row.bottom > viewport_top && row.top < viewport_bottom) {
            visible_rows.push_back(row);
        }
    }

    std::sort(
        visible_rows.begin(),
        visible_rows.end(),
        [](const Scintilla::Internal::Displayed_row_for_test &lhs,
           const Scintilla::Internal::Displayed_row_for_test &rhs) {
            if (lhs.top != rhs.top) {
                return lhs.top < rhs.top;
            }
            if (lhs.document_line != rhs.document_line) {
                return lhs.document_line < rhs.document_line;
            }
            return lhs.subline_index < rhs.subline_index;
        });
    return visible_rows;
}

std::optional<Correctness_issue> verify_visible_rows(
    Benchmark_editor &editor,
    int step,
    bool expect_wrapped_rows)
{
    const int first_visible_line = static_cast<int>(editor.send(SCI_GETFIRSTVISIBLELINE));
    const auto rows = visible_rows_for_viewport(editor);
    if (rows.empty()) {
        return Correctness_issue{step, QStringLiteral("published row snapshot is empty")};
    }

    int wrapped_rows_seen = 0;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const int expected_display_line = first_visible_line + i;
        const int expected_document_line = static_cast<int>(editor.send(SCI_DOCLINEFROMVISIBLE, expected_display_line));
        const int visible_from_document_line = static_cast<int>(editor.send(SCI_VISIBLEFROMDOCLINE, expected_document_line));
        const int expected_subline_index = expected_display_line - visible_from_document_line;
        const int wrap_count = static_cast<int>(editor.send(SCI_WRAPCOUNT, expected_document_line));
        const Scintilla::Internal::Displayed_row_for_test &row = rows[static_cast<size_t>(i)];
        if (row.document_line != expected_document_line) {
            return Correctness_issue{
                step,
                QStringLiteral("display line %1 mismatch: expected document line %2 got %3")
                    .arg(expected_display_line)
                    .arg(expected_document_line)
                    .arg(row.document_line)};
        }

        if (wrap_count <= 0) {
            return Correctness_issue{
                step,
                QStringLiteral("document line %1 reported invalid wrap count %2")
                    .arg(expected_document_line)
                    .arg(wrap_count)};
        }

        if (row.subline_index != expected_subline_index) {
            return Correctness_issue{
                step,
                QStringLiteral("display line %1 mismatch: expected subline %2 got %3 for document line %4")
                    .arg(expected_display_line)
                    .arg(expected_subline_index)
                    .arg(row.subline_index)
                    .arg(expected_document_line)};
        }

        if (row.subline_index < 0 || row.subline_index >= wrap_count) {
            return Correctness_issue{
                step,
                QStringLiteral("document line %1 published out-of-range subline %2 with wrap count %3")
                    .arg(expected_document_line)
                    .arg(row.subline_index)
                    .arg(wrap_count)};
        }

        if (expect_wrapped_rows) {
            if (row.subline_index > 0) {
                ++wrapped_rows_seen;
            }
            if (row.text.isEmpty()) {
                return Correctness_issue{
                    step,
                    QStringLiteral("wrapped display line %1 produced empty row text for document line %2 subline %3")
                        .arg(expected_display_line)
                        .arg(expected_document_line)
                        .arg(row.subline_index)};
            }
        }
        else {
            if (row.subline_index != 0) {
                return Correctness_issue{
                    step,
                    QStringLiteral("unexpected wrapped subline %1 for document line %2")
                        .arg(row.subline_index)
                        .arg(row.document_line)};
            }
            const QString actual_text = row.text;
            const QString expected_text = expected_document_line_text(expected_document_line);
            if (actual_text != expected_text) {
                return Correctness_issue{
                    step,
                    QStringLiteral("document line %1 mismatch: expected '%2' got '%3'")
                        .arg(expected_document_line)
                        .arg(expected_text.left(40))
                        .arg(actual_text.left(40))};
            }
        }
    }

    if (expect_wrapped_rows && wrapped_rows_seen == 0) {
        return Correctness_issue{
            step,
            QStringLiteral("wrapped scroll scenario did not expose any wrapped sublines")};
    }

    return std::nullopt;
}

bool wait_for_next_paint(
    Benchmark_editor &editor,
    quint64 &paint_counter,
    quint64 previous_paint_counter,
    int timeout_ms = 25)
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

template <typename StepFn>
QJsonObject measure_paint_latency_scenario(
    QStringView name,
    int steps,
    Benchmark_editor &editor,
    quint64 &paint_counter,
    StepFn &&step_fn,
    std::function<std::optional<Correctness_issue>(Benchmark_editor &, int)> verify_step = {})
{
    const QString profiling_dir = profiling_session_directory(name);
    QDir().mkpath(profiling_dir);
    const bool profiling_started = editor.startProfilingSession(profiling_dir, 3600.0);

    std::vector<double> latencies_ms;
    latencies_ms.reserve(static_cast<size_t>(steps));
    int timeout_count = 0;
    std::vector<Correctness_issue> correctness_issues;

    QElapsedTimer total_timer;
    total_timer.start();

    for (int i = 0; i < steps; ++i) {
        const quint64 previous_paint_counter = paint_counter;
        QElapsedTimer step_timer;
        step_timer.start();

        step_fn(i);

        if (wait_for_next_paint(editor, paint_counter, previous_paint_counter)) {
            latencies_ms.push_back(step_timer.nsecsElapsed() / 1'000'000.0);
            if (verify_step) {
                if (const auto issue = verify_step(editor, i); issue.has_value()) {
                    correctness_issues.push_back(*issue);
                }
            }
        }
        else {
            ++timeout_count;
        }
    }

    const latency_stats_t stats = summarize_latencies(
        latencies_ms,
        timeout_count,
        total_timer.nsecsElapsed() / 1'000'000.0);
    if (profiling_started && editor.profilingActive()) {
        editor.stopProfilingSession();
    }

    QJsonObject result = latency_stats_to_json(name, stats);
    if (profiling_started) {
        const QJsonObject profiling = load_profiling_report(profiling_dir);
        if (!profiling.isEmpty()) {
            result.insert(QStringLiteral("profiling"), profiling);
        }
    }
    cleanup_profiling_session_directory(profiling_dir);
    if (!correctness_issues.empty()) {
        QJsonArray issues_json;
        for (const Correctness_issue &issue : correctness_issues) {
            QJsonObject issue_json;
            issue_json.insert(QStringLiteral("step"), issue.step);
            issue_json.insert(QStringLiteral("message"), issue.message);
            issues_json.append(issue_json);
        }
        result.insert(QStringLiteral("correctness_failures"), issues_json);
    }
    return result;
}

QPointF local_point_for_position(ScintillaQuick_item &editor, sptr_t position)
{
    const int x = static_cast<int>(editor.send(SCI_POINTXFROMPOSITION, 0, position));
    const int y = static_cast<int>(editor.send(SCI_POINTYFROMPOSITION, 0, position));
    const int line = static_cast<int>(editor.send(SCI_LINEFROMPOSITION, position));
    const int height = static_cast<int>(editor.send(SCI_TEXTHEIGHT, line));
    return QPointF(static_cast<qreal>(x), static_cast<qreal>(y + std::max(1, height / 2)));
}

void send_mouse_event(
    QQuickWindow &window,
    ScintillaQuick_item &editor,
    QEvent::Type type,
    QPointF local_pos,
    Qt::MouseButton button,
    Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF scene_pos = editor.mapToScene(local_pos);
    const QPoint global_pos = window.mapToGlobal(scene_pos.toPoint());
    QMouseEvent event(type, local_pos, scene_pos, global_pos, button, buttons, modifiers);
    QCoreApplication::sendEvent(&editor, &event);
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

void send_key_press_event(
    Benchmark_editor &editor,
    int key,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(&editor, &event);
}

}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        qFatal("%s", qPrintable(font_error));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ScintillaQuick embedded benchmark"));
    parser.addHelpOption();
    QCommandLineOption outputOption(QStringList{QStringLiteral("o"), QStringLiteral("output")},
                                    QStringLiteral("Write JSON results to <file>."),
                                    QStringLiteral("file"));
    QCommandLineOption scenarioOption(QStringList{QStringLiteral("s"), QStringLiteral("scenario")},
                                      QStringLiteral("Run only the named scenario(s)."),
                                      QStringLiteral("name"));
    parser.addOption(outputOption);
    parser.addOption(scenarioOption);
    parser.process(app);

    QQuickWindow window;
    window.setTitle(QStringLiteral("ScintillaQuick Benchmark"));
    window.resize(1600, 900);
    window.setColor(Qt::white);
    quint64 paint_counter = 0;

    Benchmark_editor editor;
    scintillaquick::examples::bindItemToWindow(editor, window);
    editor.on_paint_node_updated = [&]() {
        ++paint_counter;
    };
    editor.setProperty("font", scintillaquick::shared::deterministic_test_font(11));
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    editor.send(SCI_STYLECLEARALL);
    editor.send(SCI_SETCARETPERIOD, 0);

    window.show();
    pump_gui(5);
    editor.forceActiveFocus();

    const QString large_document   = build_large_document(25000);
    const QString wrapped_document = build_wrapped_document(4000);
    const QByteArray insert_text("x");
    const QStringList selected_scenarios = parser.values(scenarioOption);
    const auto should_run_scenario = [&](QStringView name) {
        return selected_scenarios.isEmpty() || selected_scenarios.contains(name.toString());
    };
    const auto ensure_large_document_loaded = [&]() {
        editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
        editor.setProperty("text", large_document);
        pump_gui(10);
        editor.send(SCI_SETFIRSTVISIBLELINE, 0);
        pump_gui(4);
    };
    const auto ensure_wrapped_document_loaded = [&]() {
        editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
        editor.setProperty("text", wrapped_document);
        pump_gui(10);
        editor.send(SCI_SETFIRSTVISIBLELINE, 0);
        pump_gui(4);
    };

    QJsonArray scenarios;
    if (should_run_scenario(QStringLiteral("load_large_document"))) {
        scenarios.append(measure_scenario(editor, QStringLiteral("load_large_document"), [&]() {
            editor.setProperty("text", large_document);
            pump_gui(10);
        }));
    }

    if (should_run_scenario(QStringLiteral("caret_move_right_5000"))) {
        scenarios.append(measure_scenario(editor, QStringLiteral("caret_move_right_5000"), [&]() {
            editor.send(SCI_GOTOPOS, 0);
            for (int i = 0; i < 5000; ++i) {
                editor.send(SCI_CHARRIGHT);
            }
            pump_gui(5);
        }));
    }

    if (should_run_scenario(QStringLiteral("insert_character_2000"))) {
        scenarios.append(measure_scenario(editor, QStringLiteral("insert_character_2000"), [&]() {
            editor.send(SCI_GOTOPOS, editor.send(SCI_GETTEXTLENGTH));
            for (int i = 0; i < 2000; ++i) {
                editor.send(SCI_ADDTEXT, 1, reinterpret_cast<sptr_t>(insert_text.constData()));
            }
            pump_gui(5);
        }));
    }

    if (should_run_scenario(QStringLiteral("page_down_250"))) {
        scenarios.append(measure_scenario(editor, QStringLiteral("page_down_250"), [&]() {
            editor.send(SCI_GOTOPOS, 0);
            for (int i = 0; i < 250; ++i) {
                editor.send(SCI_PAGEDOWN);
            }
            pump_gui(5);
        }));
    }

    if (should_run_scenario(QStringLiteral("resize_window"))) {
        scenarios.append(measure_scenario(editor, QStringLiteral("resize_window"), [&]() {
            window.resize(1200, 760);
            pump_gui(4);
            window.resize(1600, 900);
            pump_gui(4);
        }));
    }

    if (should_run_scenario(QStringLiteral("caret_step_right_latency_64"))) {
        ensure_large_document_loaded();
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("caret_step_right_latency_64"),
            64,
            editor,
            paint_counter,
            [&](int) {
                send_key_press_event(editor, Qt::Key_Right);
            }));
    }

    if (should_run_scenario(QStringLiteral("caret_step_left_latency_64"))) {
        ensure_large_document_loaded();
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("caret_step_left_latency_64"),
            64,
            editor,
            paint_counter,
            [&](int) {
                send_key_press_event(editor, Qt::Key_Left);
            }));
    }

    if (should_run_scenario(QStringLiteral("vertical_scroll_step_latency_64"))) {
        ensure_large_document_loaded();
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("vertical_scroll_step_latency_64"),
            64,
            editor,
            paint_counter,
            [&](int) {
                const int next_top_line = static_cast<int>(editor.send(SCI_GETFIRSTVISIBLELINE)) + 3;
                editor.scrollVertical(next_top_line);
            },
            [](Benchmark_editor &editor, int step) {
                return verify_visible_rows(editor, step, false);
            }));
    }

    if (should_run_scenario(QStringLiteral("vertical_scroll_page_step_latency_16"))) {
        ensure_large_document_loaded();
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("vertical_scroll_page_step_latency_16"),
            16,
            editor,
            paint_counter,
            [&](int) {
                const int next_top_line = static_cast<int>(editor.send(SCI_GETFIRSTVISIBLELINE)) + 30;
                editor.scrollVertical(next_top_line);
            },
            [](Benchmark_editor &editor, int step) {
                return verify_visible_rows(editor, step, false);
            }));
    }

    if (should_run_scenario(QStringLiteral("vertical_scroll_bounce_latency_48"))) {
        ensure_large_document_loaded();
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("vertical_scroll_bounce_latency_48"),
            48,
            editor,
            paint_counter,
            [&](int step) {
                const int current_top_line = static_cast<int>(editor.send(SCI_GETFIRSTVISIBLELINE));
                const int delta = (step % 2 == 0) ? 3 : -3;
                const int max_top_line = std::max(
                    0,
                    static_cast<int>(editor.send(SCI_GETLINECOUNT)) -
                        static_cast<int>(editor.send(SCI_LINESONSCREEN)));
                const int next_top_line = std::clamp(current_top_line + delta, 0, max_top_line);
                editor.scrollVertical(next_top_line);
            },
            [](Benchmark_editor &editor, int step) {
                return verify_visible_rows(editor, step, false);
            }));
    }

    if (should_run_scenario(QStringLiteral("vertical_wheel_bounce_latency_48"))) {
        ensure_large_document_loaded();
        editor.send(SCI_SETFIRSTVISIBLELINE, 120);
        pump_gui(4);
        const QPointF wheel_point(
            std::max<qreal>(editor.width() * 0.5, 32.0),
            std::max<qreal>(editor.height() * 0.5, 32.0));
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("vertical_wheel_bounce_latency_48"),
            48,
            editor,
            paint_counter,
            [&](int step) {
                const bool wheel_up = (step % 2) == 0;
                send_wheel_event(
                    window,
                    editor,
                    wheel_point,
                    QPoint(),
                    QPoint(0, wheel_up ? 120 : -120));
            },
            [](Benchmark_editor &editor, int step) {
                return verify_visible_rows(editor, step, false);
            }));
    }

    if (should_run_scenario(QStringLiteral("wrapped_wheel_bounce_latency_24"))) {
        ensure_wrapped_document_loaded();
        editor.send(SCI_SETFIRSTVISIBLELINE, 120);
        pump_gui(4);
        const QPointF wheel_point(
            std::max<qreal>(editor.width() * 0.5, 32.0),
            std::max<qreal>(editor.height() * 0.5, 32.0));
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("wrapped_wheel_bounce_latency_24"),
            24,
            editor,
            paint_counter,
            [&](int step) {
                const bool wheel_up = (step % 2) == 0;
                send_wheel_event(
                    window,
                    editor,
                    wheel_point,
                    QPoint(),
                    QPoint(0, wheel_up ? 120 : -120));
            },
            [](Benchmark_editor &editor, int step) {
                return verify_visible_rows(editor, step, true);
            }));
    }

    if (should_run_scenario(QStringLiteral("scroll_after_edit_latency_32"))) {
        ensure_large_document_loaded();
        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("scroll_after_edit_latency_32"),
            32,
            editor,
            paint_counter,
            [&](int) {
                const sptr_t target_line = std::min<sptr_t>(
                    editor.send(SCI_GETLINECOUNT) - 1,
                    editor.send(SCI_GETFIRSTVISIBLELINE) + 5);
                editor.send(SCI_GOTOPOS, editor.send(SCI_POSITIONFROMLINE, target_line));
                editor.send(SCI_ADDTEXT, 1, reinterpret_cast<sptr_t>(insert_text.constData()));
                const int next_top_line = static_cast<int>(editor.send(SCI_GETFIRSTVISIBLELINE)) + 3;
                editor.scrollVertical(next_top_line);
            }));
    }

    if (should_run_scenario(QStringLiteral("selection_drag_latency_48"))) {
        ensure_large_document_loaded();
        editor.send(SCI_GOTOPOS, 0);
        pump_gui(6);

        const sptr_t drag_start = editor.send(SCI_POSITIONFROMLINE, 0) + 5;
        const QPointF drag_start_point = local_point_for_position(editor, drag_start);
        const quint64 drag_press_paint_counter = paint_counter;
        send_mouse_event(
            window,
            editor,
            QEvent::MouseButtonPress,
            drag_start_point,
            Qt::LeftButton,
            Qt::LeftButton);
        wait_for_next_paint(editor, paint_counter, drag_press_paint_counter);

        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("selection_drag_latency_48"),
            48,
            editor,
            paint_counter,
            [&](int index) {
                const sptr_t drag_position = drag_start + 1 + index;
                const QPointF drag_point = local_point_for_position(editor, drag_position);
                send_mouse_event(
                    window,
                    editor,
                    QEvent::MouseMove,
                    drag_point,
                    Qt::NoButton,
                    Qt::LeftButton);
            }));
        const QPointF drag_end_point = local_point_for_position(editor, drag_start + 48);
        send_mouse_event(
            window,
            editor,
            QEvent::MouseButtonRelease,
            drag_end_point,
            Qt::LeftButton,
            Qt::NoButton);
        pump_gui(4);
    }

    if (should_run_scenario(QStringLiteral("zoom_wheel_bounce_latency_24"))) {
        ensure_large_document_loaded();
        editor.send(SCI_SETZOOM, 0);
        pump_gui(6);
        int expected_zoom = static_cast<int>(editor.send(SCI_GETZOOM));

        scenarios.append(measure_paint_latency_scenario(
            QStringLiteral("zoom_wheel_bounce_latency_24"),
            24,
            editor,
            paint_counter,
            [&](int step) {
                const bool zoom_in = (step % 2) == 0;
                expected_zoom += zoom_in ? 1 : -1;
                editor.send(zoom_in ? SCI_ZOOMIN : SCI_ZOOMOUT);
            },
            [&](Benchmark_editor &editor, int step) -> std::optional<Correctness_issue> {
                const int actual_zoom = static_cast<int>(editor.send(SCI_GETZOOM));
                if (actual_zoom != expected_zoom) {
                    return Correctness_issue{
                        step,
                        QStringLiteral("zoom mismatch: expected %1 got %2")
                            .arg(expected_zoom)
                            .arg(actual_zoom)};
                }
                return std::nullopt;
            }));
    }

    QJsonObject summary{
        {QStringLiteral("viewport_width"), window.width()},
        {QStringLiteral("viewport_height"), window.height()},
        {QStringLiteral("document_length"), static_cast<qint64>(editor.send(SCI_GETTEXTLENGTH))},
        {QStringLiteral("line_count"), static_cast<qint64>(editor.send(SCI_GETLINECOUNT))},
        {QStringLiteral("first_visible_line"), static_cast<qint64>(editor.send(SCI_GETFIRSTVISIBLELINE))},
        {QStringLiteral("x_offset"), static_cast<qint64>(editor.send(SCI_GETXOFFSET))},
    };

    QJsonObject result{
        {QStringLiteral("benchmark"), QStringLiteral("ScintillaQuick")},
        {QStringLiteral("scenarios"), scenarios},
        {QStringLiteral("summary"), summary},
    };

    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Indented);
    QTextStream(stdout) << json;

    bool has_correctness_failures = false;
    for (const QJsonValue &scenario_value : scenarios) {
        const QJsonObject scenario = scenario_value.toObject();
        if (scenario.contains(QStringLiteral("correctness_failures"))) {
            has_correctness_failures = true;
            break;
        }
    }

    if (parser.isSet(outputOption)) {
        QFile file(parser.value(outputOption));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning().noquote() << "Failed to open benchmark output file:" << file.fileName();
            return 1;
        }
        file.write(json);
    }

    return has_correctness_failures ? 2 : 0;
}
