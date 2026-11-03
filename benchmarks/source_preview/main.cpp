// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#define SCINTILLAQUICK_ENABLE_TEST_ACCESS
#include <scintillaquick/scintillaquick_source_preview.h>
#undef SCINTILLAQUICK_ENABLE_TEST_ACCESS

#include "scintillaquick_font.h"

#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSysInfo>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace Scintilla::Internal {

class ScintillaQuick_validation_access
{
public:
    static quint64 preview_image_serial(const ScintillaQuick_source_preview& item)
    {
        return item.published_image_serial_for_test();
    }
};

}

namespace {

using Access = Scintilla::Internal::ScintillaQuick_validation_access;

void require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

double milliseconds(const QElapsedTimer& timer)
{
    return timer.nsecsElapsed() / 1'000'000.0;
}

QJsonObject distribution(std::vector<double> values)
{
    if (values.empty()) { return {}; }
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) {
        return values[static_cast<size_t>((values.size() - 1) * p)];
    };
    return {
        {QStringLiteral("count"), static_cast<int>(values.size())},
        {QStringLiteral("min_ms"), values.front()},
        {QStringLiteral("median_ms"), percentile(0.5)},
        {QStringLiteral("p95_ms"), percentile(0.95)},
        {QStringLiteral("max_ms"), values.back()},
        {QStringLiteral("mean_ms"), std::accumulate(values.begin(), values.end(), 0.0) / values.size()}
    };
}

QByteArray generated_source(const QString& corpus, int lines)
{
    // Unique identifiers occur near the start of the line, inside the capture
    // horizon. A whole-line cache must not turn this into a repeated-line test.
    const qsizetype estimate = corpus == QLatin1String("long") ? 4352 : 160;
    require(estimate * lines <= 512 * 1024 * 1024, "generated corpus exceeds the 512 MiB setup bound");
    QByteArray text;
    text.reserve(estimate * lines);
    for (int i = 0; i < lines; ++i) {
        if (i) { text += '\n'; }
        if (corpus == QLatin1String("sparse") && i % 997 != 0) { continue; }
        if (corpus == QLatin1String("repeated")) {
            text += "    const auto value = lookup(item, 42); // retain a styled source overview";
        }
        else if (corpus == QLatin1String("unicode")) {
            text += "\tconst auto value_" + QByteArray::number(i) + " = \"";
            text += QStringLiteral("Ελληνικά Кириллица العربية 漢字 e\u0301 😀").toUtf8();
            text += "\"; // shaped text, fallback and combining marks";
        }
        else {
            switch (i % 5) {
                case 0: text += "    const auto item_" + QByteArray::number(i) + " = entries.find(key_" + QByteArray::number(i * 17) + ");"; break;
                case 1: text += "    if (item_" + QByteArray::number(i) + " != entries.end()) { return item_" + QByteArray::number(i) + "->second; }"; break;
                case 2: text += "    // row " + QByteArray::number(i) + ": preserve every logical source line in the overview"; break;
                case 3: text += "    QString label_" + QByteArray::number(i) + " = QStringLiteral(\"entry " + QByteArray::number(i * 23) + "\");"; break;
                default: text += "    result_" + QByteArray::number(i) + ".push_back({index_" + QByteArray::number(i) + ", value, true});"; break;
            }
            if (corpus == QLatin1String("long")) {
                text += QByteArray(4096, 'x');
            }
        }
    }
    return text;
}

QByteArray source_styles(const QByteArray& text)
{
    // A deterministic, deliberately small token classifier supplies mixed
    // fonts/colors without requiring Lexilla. It is setup, not measured work.
    QByteArray styles(text.size(), Qt::Uninitialized);
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.constData());
    char* result = styles.data();
    bool quoted = false;
    bool comment = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const unsigned char c = bytes[i];
        if (c == '\n') { quoted = comment = false; }
        if (!quoted && c == '/' && i + 1 < text.size() && bytes[i + 1] == '/') { comment = true; }
        if (!comment && c == '"' && (i == 0 || bytes[i - 1] != '\\')) { quoted = !quoted; }
        result[i] = comment ? 4 : quoted || c == '"' ? 3 : c >= '0' && c <= '9' ? 2
            : c >= 0x80 ? 5 : c == '(' || c == ')' || c == '{' || c == '}' || c == ';' ? 1 : 0;
    }
    return styles;
}

QString image_hash(const QImage& image)
{
    const QImage canonical = image.convertToFormat(QImage::Format_ARGB32);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < canonical.height(); ++y) {
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(canonical.constScanLine(y)), canonical.width() * 4));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool has_glyph_coverage(const QImage& image, qreal dpr)
{
    const QRgb background = qRgb(0x20, 0x21, 0x24);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = static_cast<int>(4 * dpr); x < image.width() - 4 * dpr; ++x) {
            if (image.pixel(x, y) != background) { return true; }
        }
    }
    return false;
}

class Scene
{
public:
    Scene(const QByteArray& text, const QByteArray& styles, int width, int height)
        : window(&control), preview(window.contentItem())
    {
        window.setGeometry(0, 0, width, height);
        window.contentItem()->setSize(QSizeF(width, height));
        require(window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software,
            "source-preview benchmark requires the software Qt Quick backend");
        require(!window.isVisible() && !window.isExposed(), "fixture must remain offscreen");
        preview.setSize(QSizeF(width, height));
        preview.set_viewport_color(Qt::transparent);
        // The native editor has a viewport, but only its preview is drawn.
        // Editor rendering and physical presentation are outside this boundary.
        editor.setSize(QSizeF(900, height));
        const QFont font = scintillaquick::shared::deterministic_test_font(11);
        editor.setProperty("font", font);
        editor.send(SCI_STYLESETBACK, STYLE_DEFAULT, 0x242120);
        editor.send(SCI_STYLESETFORE, STYLE_DEFAULT, 0xdddddd);
        editor.send(SCI_STYLECLEARALL);
        editor.send(SCI_STYLESETFORE, 1, 0xe8b18a);
        editor.send(SCI_STYLESETFORE, 2, 0xe6b2d1);
        editor.send(SCI_STYLESETFORE, 3, 0xa4d6a0);
        editor.send(SCI_STYLESETFORE, 4, 0xa89880);
        editor.send(SCI_STYLESETITALIC, 4, 1);
        editor.send(SCI_STYLESETBOLD, 1, 1);
        editor.send(SCI_STYLESETFORE, 5, 0xe0d080);
        editor.send(SCI_SETTABWIDTH, 4);
        editor.send(SCI_SETUNDOCOLLECTION, 0);
        QElapsedTimer load;
        load.start();
        editor.sends(SCI_SETTEXT, 0, text.constData());
        editor.send(SCI_STARTSTYLING, 0);
        editor.send(SCI_SETSTYLINGEX, styles.size(), reinterpret_cast<sptr_t>(styles.constData()));
        setup_ms = milliseconds(load);
        require(editor.send(SCI_GETLENGTH) == text.size(), "native document byte count differs from input");
        line_count = editor.send(SCI_GETLINECOUNT);
        // Drain pre-existing setup events before attaching the source preview.
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }

    ~Scene()
    {
        preview.set_editor(nullptr);
        window.setRenderTarget(QQuickRenderTarget());
        control.invalidate();
    }

    QImage frame()
    {
        const QImage image = window.grabWindow();
        require(!image.isNull(), "offscreen software frame was not rendered");
        const qreal dpr = window.effectiveDevicePixelRatio();
        require(image.size() == QSize(qRound(window.width() * dpr), qRound(window.height() * dpr)),
            "offscreen frame dimensions do not match requested preview geometry");
        return image;
    }

    QJsonObject publication(const QString& name, int timeout_ms, const std::function<void()>& action,
        QImage* rendered = nullptr)
    {
        const quint64 previous_serial = Access::preview_image_serial(preview);
        QElapsedTimer elapsed;
        QEventLoop loop;
        QTimer poll;
        QTimer timeout;
        poll.setTimerType(Qt::PreciseTimer);
        poll.setInterval(1);
        timeout.setSingleShot(true);
        std::vector<double> gaps;
        double previous_tick = 0;
        bool completed = false;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            const double now = milliseconds(elapsed);
            gaps.push_back(now - previous_tick);
            previous_tick = now;
            const auto serial = Access::preview_image_serial(preview);
            if (serial != 0 && serial != previous_serial) {
                completed = true;
                loop.quit();
            }
        });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        elapsed.start();
        const std::clock_t cpu_start = std::clock();
        poll.start();
        timeout.start(timeout_ms);
        action();
        const double action_ms = milliseconds(elapsed);
        loop.exec();
        poll.stop();
        timeout.stop();
        const double publication_ms = milliseconds(elapsed);
        const double cpu_ms = 1000.0 * (std::clock() - cpu_start) / CLOCKS_PER_SEC;
        require(completed, "timed out waiting for a new preview publication");
        QElapsedTimer frame_time;
        frame_time.start();
        const QImage image = frame();
        const double frame_ms = milliseconds(frame_time);
        const double rendered_ms = milliseconds(elapsed);
        require(has_glyph_coverage(image, window.effectiveDevicePixelRatio()), "published preview has no glyph coverage");
        if (rendered) { *rendered = image; }
        return {
            {QStringLiteral("scenario"), name},
            {QStringLiteral("trigger_ms"), action_ms},
            {QStringLiteral("publication_ms"), publication_ms},
            {QStringLiteral("process_cpu_ms_to_publication"), cpu_ms},
            {QStringLiteral("software_frame_ms"), frame_ms},
            {QStringLiteral("rendered_ms"), rendered_ms},
            {QStringLiteral("gui_1ms_timer_gaps"), distribution(std::move(gaps))},
            {QStringLiteral("image_sha256"), image_hash(image)},
            {QStringLiteral("image_width"), image.width()},
            {QStringLiteral("image_height"), image.height()}
        };
    }

    QJsonObject scroll(int steps)
    {
        const auto serial = Access::preview_image_serial(preview);
        require(serial != 0, "scroll scenario needs a completed source image");
        preview.set_viewport_color(QColor(255, 255, 255, 120));
        editor.send(SCI_SETFIRSTVISIBLELINE, 0);
        const QImage first = frame();
        std::vector<double> latencies;
        bool moved = false;
        QElapsedTimer total;
        total.start();
        for (int i = 0; i < steps; ++i) {
            const int target = static_cast<int>((static_cast<qint64>(i + 1) * (line_count - 1)) / steps);
            QElapsedTimer step;
            step.start();
            editor.send(SCI_SETFIRSTVISIBLELINE, target);
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            const QImage image = frame();
            latencies.push_back(milliseconds(step));
            moved |= image != first;
            require(Access::preview_image_serial(preview) == serial, "scrolling rerasterized the source image");
        }
        const double elapsed_ms = milliseconds(total);
        require(moved, "scroll scenario did not move the rendered viewport handle");
        // Observe a full debounce interval as well: a delayed capture must not
        // be mistaken for source reuse just because the scroll loop was fast.
        QEventLoop settle;
        QTimer::singleShot(50, &settle, &QEventLoop::quit);
        settle.exec();
        require(Access::preview_image_serial(preview) == serial, "scrolling scheduled a delayed source reraster");
        preview.set_viewport_color(Qt::transparent);
        editor.send(SCI_SETFIRSTVISIBLELINE, 0);
        return {
            {QStringLiteral("scenario"), QStringLiteral("scroll")},
            {QStringLiteral("native_scroll_and_software_frame"), distribution(std::move(latencies))},
            {QStringLiteral("elapsed_ms"), elapsed_ms},
            {QStringLiteral("source_reused"), true},
            {QStringLiteral("handle_moved"), moved}
        };
    }

    QJsonObject edit_burst(int timeout_ms)
    {
        // Begin a refresh and edit at 30,40,...,80 ms. These are native edits,
        // not OS keyboard events. A fresh rebuild below is the pixel oracle.
        QTimer edits;
        edits.setTimerType(Qt::PreciseTimer);
        edits.setInterval(10);
        int edit_count = 0;
        bool prior_refresh_published = false;
        const auto before = Access::preview_image_serial(preview);
        QElapsedTimer burst;
        double last_edit_ms = 0;
        QObject::connect(&edits, &QTimer::timeout, &edits, [&] {
            prior_refresh_published |= Access::preview_image_serial(preview) != before;
            editor.sends(SCI_INSERTTEXT, 0, "x");
            last_edit_ms = milliseconds(burst);
            if (++edit_count == 6) { edits.stop(); }
        });
        QTimer start_edits;
        start_edits.setSingleShot(true);
        QObject::connect(&start_edits, &QTimer::timeout, &edits, [&] { edits.start(); });
        QImage settled;
        burst.start();
        double observation_start_ms = milliseconds(burst);
        auto result = publication(QStringLiteral("edit-burst"), timeout_ms, [&] {
            preview.refresh_style();
            start_edits.start(20);
        }, &settled);
        // Small inputs can finish the initial refresh before the first edit.
        // Complete all edits before waiting for their final publication.
        if (edit_count != 6) {
            QEventLoop finish_edits;
            QObject::connect(&edits, &QTimer::timeout, &finish_edits, [&] {
                if (edit_count == 6) { finish_edits.quit(); }
            });
            finish_edits.exec();
            observation_start_ms = milliseconds(burst);
            result = publication(QStringLiteral("edit-burst"), timeout_ms, [] {}, &settled);
        }
        // publication() hashes its completed frame after stopping the render
        // clock. Keep that verification work outside the burst timings too.
        const double rendered_at_ms = observation_start_ms + result.value(QStringLiteral("rendered_ms")).toDouble();
        result.insert(QStringLiteral("burst_to_rendered_ms"), rendered_at_ms);
        result.insert(QStringLiteral("last_edit_to_rendered_ms"), rendered_at_ms - last_edit_ms);
        result.insert(QStringLiteral("edits"), edit_count);
        result.insert(QStringLiteral("prior_refresh_published_before_edits_finished"), prior_refresh_published);
        QImage fresh;
        const auto oracle = publication(QStringLiteral("oracle"), timeout_ms,
            [&] { preview.refresh_style(); }, &fresh);
        require(settled == fresh, "edit-burst final raster differs from a clean refresh of the same document");
        result.insert(QStringLiteral("matches_clean_refresh"), true);
        result.insert(QStringLiteral("oracle_publication_ms"), oracle.value(QStringLiteral("publication_ms")));
        return result;
    }

    QQuickRenderControl control;
    QQuickWindow window;
    ScintillaQuick_item editor;
    ScintillaQuick_source_preview preview;
    double setup_ms = 0;
    int line_count = 0;
};

int integer_option(const QCommandLineParser& parser, const QString& name, int low, int high)
{
    bool valid = false;
    const int value = parser.value(name).toInt(&valid);
    if (!valid || value < low || value > high) {
        throw std::runtime_error((QStringLiteral("invalid --") + name).toStdString());
    }
    return value;
}

}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) { qputenv("QT_QPA_PLATFORM", "offscreen"); }
    if (qEnvironmentVariableIsEmpty("QT_FONT_DPI")) { qputenv("QT_FONT_DPI", "96"); }
    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) { qputenv("QT_SCALE_FACTOR", "1"); }
    qputenv("QT_QUICK_BACKEND", "software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Production source-preview capture/raster/publication benchmark"));
    parser.addHelpOption();
    parser.addOptions({
        {{QStringLiteral("lines")}, QStringLiteral("Generated logical source lines"), QStringLiteral("count"), QStringLiteral("10000")},
        {{QStringLiteral("iterations")}, QStringLiteral("Independent preview instances in this process"), QStringLiteral("count"), QStringLiteral("3")},
        {{QStringLiteral("corpus")}, QStringLiteral("unique, repeated, unicode, sparse or long"), QStringLiteral("name"), QStringLiteral("unique")},
        {{QStringLiteral("input-file")}, QStringLiteral("Use this UTF-8 source file exactly; --lines is ignored"), QStringLiteral("path")},
        {{QStringLiteral("scenario")}, QStringLiteral("Comma-separated initial,refresh,edit,scroll,edit-burst or all"), QStringLiteral("names"), QStringLiteral("initial")},
        {{QStringLiteral("width")}, QStringLiteral("Logical preview width"), QStringLiteral("pixels"), QStringLiteral("96")},
        {{QStringLiteral("height")}, QStringLiteral("Logical preview height"), QStringLiteral("pixels"), QStringLiteral("720")},
        {{QStringLiteral("timeout-ms")}, QStringLiteral("Per-publication timeout, not a performance assertion"), QStringLiteral("milliseconds"), QStringLiteral("120000")},
        {{QStringLiteral("scroll-steps")}, QStringLiteral("Native scroll commands with complete preview frames"), QStringLiteral("count"), QStringLiteral("120")},
        {{QStringLiteral("output")}, QStringLiteral("Also write the JSON report here"), QStringLiteral("path")}
    });
    parser.process(app);
    QJsonObject report;
    QJsonArray runs;
    int exit_code = 0;
    try {
        const int lines = integer_option(parser, QStringLiteral("lines"), 1, 2'000'000);
        const int iterations = integer_option(parser, QStringLiteral("iterations"), 1, 100);
        const int width = integer_option(parser, QStringLiteral("width"), 8, 1024);
        const int height = integer_option(parser, QStringLiteral("height"), 32, 8192);
        const int timeout = integer_option(parser, QStringLiteral("timeout-ms"), 100, 600'000);
        const int scroll_steps = integer_option(parser, QStringLiteral("scroll-steps"), 2, 10000);
        const QStringList corpora{QStringLiteral("unique"), QStringLiteral("repeated"), QStringLiteral("unicode"), QStringLiteral("sparse"), QStringLiteral("long")};
        const QString corpus = parser.isSet(QStringLiteral("input-file")) ? QStringLiteral("file") : parser.value(QStringLiteral("corpus"));
        require(corpus == QLatin1String("file") || corpora.contains(corpus), "unknown corpus");
        const QStringList allowed{QStringLiteral("initial"), QStringLiteral("refresh"), QStringLiteral("edit"), QStringLiteral("scroll"), QStringLiteral("edit-burst")};
        const auto scenarios = parser.value(QStringLiteral("scenario")) == QLatin1String("all")
            ? allowed : parser.value(QStringLiteral("scenario")).split(QLatin1Char(','));
        for (const auto& scenario : scenarios) { require(allowed.contains(scenario), "unknown scenario"); }
        QString font_error;
        require(scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error), qPrintable(font_error));
        QByteArray text;
        if (corpus == QLatin1String("file")) {
            QFile input(parser.value(QStringLiteral("input-file")));
            require(input.open(QIODevice::ReadOnly), "cannot read input file");
            require(input.size() <= 512 * 1024 * 1024, "input file exceeds the 512 MiB setup bound");
            text = input.readAll();
            require(!text.isEmpty() && !text.contains('\0'), "input must be nonempty UTF-8 source without NUL bytes");
        }
        else { text = generated_source(corpus, lines); }
        const QByteArray styles = source_styles(text);
        report = {
            {QStringLiteral("benchmark"), QStringLiteral("ScintillaQuick source preview")},
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("os"), QSysInfo::prettyProductName()},
            {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("renderer"), QStringLiteral("Qt Quick software, offscreen render control")},
            {QStringLiteral("font"), QFontInfo(scintillaquick::shared::deterministic_test_font(11)).family()},
            {QStringLiteral("corpus"), corpus},
            {QStringLiteral("input_file"), parser.value(QStringLiteral("input-file"))},
            {QStringLiteral("input_sha256"), QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("input_bytes"), text.size()},
            {QStringLiteral("width"), width},
            {QStringLiteral("height"), height},
            {QStringLiteral("boundary"), QStringLiteral("Trigger through current GUI publication, then one software preview frame; includes debounce, worker raster and queued GUI capture; excludes corpus generation, document loading/styling, editor painting and physical display")},
            {QStringLiteral("timer_probe"), QStringLiteral("1 ms precise GUI timer; gaps include timer resolution, scheduling and benchmark overhead, not isolated capture time")}
        };
        for (int iteration = 0; iteration < iterations; ++iteration) {
            Scene scene(text, styles, width, height);
            report.insert(QStringLiteral("lines"), scene.line_count);
            report.insert(QStringLiteral("device_pixel_ratio"), scene.window.effectiveDevicePixelRatio());
            const auto append = [&](QJsonObject result) {
                result.insert(QStringLiteral("iteration"), iteration);
                result.insert(QStringLiteral("setup_ms"), scene.setup_ms);
                runs.append(result);
            };
            auto initial = scene.publication(QStringLiteral("initial"), timeout,
                [&] { scene.preview.set_editor(&scene.editor); });
            // Every scenario needs a current initial image; report that cost
            // even when the selected scenario only exercises later updates.
            append(std::move(initial));
            if (scenarios.contains(QStringLiteral("refresh"))) {
                append(scene.publication(QStringLiteral("refresh"), timeout,
                    [&] { scene.preview.refresh_style(); }));
            }
            if (scenarios.contains(QStringLiteral("edit"))) {
                append(scene.publication(QStringLiteral("edit"), timeout,
                    [&] { scene.editor.sends(SCI_INSERTTEXT, 0, "edited_"); }));
                require(scene.editor.send(SCI_GETLINECOUNT) == scene.line_count, "same-line edit changed topology");
            }
            if (scenarios.contains(QStringLiteral("scroll"))) { append(scene.scroll(scroll_steps)); }
            if (scenarios.contains(QStringLiteral("edit-burst"))) { append(scene.edit_burst(timeout)); }
        }
        report.insert(QStringLiteral("ok"), true);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "source-preview benchmark: %s\n", error.what());
        report.insert(QStringLiteral("ok"), false);
        report.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
        exit_code = 1;
    }
    report.insert(QStringLiteral("runs"), runs);
    const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (parser.isSet(QStringLiteral("output"))) {
        QFile output(parser.value(QStringLiteral("output")));
        if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size()) {
            std::fprintf(stderr, "source-preview benchmark: cannot write JSON report\n");
            exit_code = 1;
        }
    }
    std::fwrite(json.constData(), 1, static_cast<size_t>(json.size()), stdout);
    return exit_code;
}
