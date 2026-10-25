// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_syntax.h>
#include <scintillaquick/scintillaquick_item.h>

#include <SciLexer.h>
#include <QByteArray>
#include <QGuiApplication>
#include <cstdio>
#include <cstdlib>

namespace
{

void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "Syntax contract failed: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

QByteArray lexer_name(ScintillaQuick_item& pane)
{
    const auto length = pane.send(SCI_GETLEXERLANGUAGE);
    QByteArray name(static_cast<qsizetype>(length) + 1, '\0');
    pane.send(SCI_GETLEXERLANGUAGE, 0, reinterpret_cast<sptr_t>(name.data()));
    name.resize(static_cast<qsizetype>(length));
    return name;
}

void require_plain_text_styles(ScintillaQuick_item& pane)
{
    const auto length     = pane.send(SCI_GETLENGTH);
    const auto foreground = pane.send(SCI_STYLEGETFORE, STYLE_DEFAULT);
    const auto bold       = pane.send(SCI_STYLEGETBOLD, STYLE_DEFAULT);
    const auto italic     = pane.send(SCI_STYLEGETITALIC, STYLE_DEFAULT);
    for (sptr_t position = 0; position < length; ++position) {
        const auto style = static_cast<uptr_t>(pane.send(SCI_GETSTYLEINDEXAT, position));
        require(pane.send(SCI_STYLEGETFORE, style) == foreground, "plain text uses the base foreground");
        require(pane.send(SCI_STYLEGETBOLD, style) == bold, "plain text uses the base weight");
        require(pane.send(SCI_STYLEGETITALIC, style) == italic, "plain text uses the base italic setting");
    }
}

}

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    ScintillaQuick_item pane;
    pane.send(SCI_STYLESETFORE, STYLE_DEFAULT, 0x345678);
    pane.send(SCI_STYLESETBACK, STYLE_DEFAULT, 0x123456);
    pane.send(SCI_STYLESETSIZE, STYLE_DEFAULT, 15);
    pane.send(SCI_STYLESETBACK, SCE_C_WORD, 0x654321);
    pane.send(SCI_SETMARGINWIDTHN, 0, 37);

    struct syntax_case_t {
        const char* path;
        const char* lexer;
    };
    const syntax_case_t cases[] = {
        {"source.cpp", "cpp"}, {"view.qml", "cpp"},
        {"settings.json", "json"}, {"module.py", "python"},
        {"CMakeLists.txt", "cmake"}, {"library.rs", "rust"}
    };
    for (const auto& entry : cases) {
        scintillaquick::configure_default_syntax(pane, QString::fromUtf8(entry.path), true);
        require(lexer_name(pane) == entry.lexer, "path selects its language");
    }

    pane.send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>("int value;"));
    scintillaquick::configure_default_syntax(pane, QStringLiteral("source.cpp"), true);
    require(pane.send(SCI_GETSTYLEAT, 0) == SCE_C_WORD, "C++ keywords are styled");
    const auto dark_keyword = pane.send(SCI_STYLEGETFORE, SCE_C_WORD);
    scintillaquick::configure_default_syntax(pane, QStringLiteral("source.cpp"), false);
    require(pane.send(SCI_STYLEGETFORE, SCE_C_WORD) != dark_keyword, "theme changes lexical colors");
    require(pane.send(SCI_STYLEGETFORE, STYLE_DEFAULT) == 0x345678, "base foreground is preserved");
    require(pane.send(SCI_STYLEGETBACK, STYLE_DEFAULT) == 0x123456, "base background is preserved");
    require(pane.send(SCI_STYLEGETSIZE, STYLE_DEFAULT) == 15, "host font size is preserved");
    require(pane.send(SCI_STYLEGETBACK, SCE_C_WORD) == 0x654321, "lexical background is preserved");
    require(pane.send(SCI_GETMARGINWIDTHN, 0) == 37, "host margins are preserved");

    pane.send(SCI_SETKEYWORDS, 0, reinterpret_cast<sptr_t>("customword"));
    pane.send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>("customword"));
    pane.send(SCI_COLOURISE, 0, -1);
    require(pane.send(SCI_GETSTYLEAT, 0) == SCE_C_WORD, "host keyword override takes effect");
    scintillaquick::reset_syntax_styles(pane);
    require(lexer_name(pane) == "cpp", "style reset preserves the selected lexer");
    pane.send(SCI_COLOURISE, 0, -1);
    require(pane.send(SCI_GETSTYLEAT, 0) == SCE_C_IDENTIFIER, "style reset clears keyword overrides");
    require(pane.send(SCI_STYLEGETFORE, SCE_C_WORD) == 0x345678, "style reset restores base foreground");

    pane.send(SCI_STYLESETFORE,   SCE_C_IDENTIFIER, 0x765432);
    pane.send(SCI_STYLESETBOLD,   SCE_C_IDENTIFIER, 1);
    pane.send(SCI_STYLESETITALIC, SCE_C_IDENTIFIER, 1);
    scintillaquick::configure_default_syntax(pane, QStringLiteral("notes.txt"), false);
    require_plain_text_styles(pane);
    require(pane.send(SCI_GETENDSTYLED) == pane.send(SCI_GETLENGTH),
        "switching to plain text completes styling");
    const QByteArray addition(" More plain text");
    pane.send(SCI_APPENDTEXT, static_cast<uptr_t>(addition.size()),
        reinterpret_cast<sptr_t>(addition.constData()));
    pane.send(SCI_COLOURISE, 0, -1);
    require(pane.send(SCI_GETENDSTYLED) == pane.send(SCI_GETLENGTH),
        "plain-text edits complete styling without a container callback");
    require_plain_text_styles(pane);
    pane.send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>("def function(): pass"));
    scintillaquick::configure_default_syntax(pane, QStringLiteral("module.py"), false);
    require(pane.send(SCI_GETSTYLEAT, 0) == SCE_P_WORD, "switching out of plain text restores keywords");
    return 0;
}
