// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_syntax.h>
#include <scintillaquick/scintillaquick_item.h>

#include <SciLexer.h>
#include <QByteArray>
#include <QGuiApplication>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

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
        require(style == 0, "plain text clears every obsolete lexical style index");
        require(pane.send(SCI_STYLEGETFORE, style) == foreground, "plain text uses the base foreground");
        require(pane.send(SCI_STYLEGETBOLD, style) == bold, "plain text uses the base weight");
        require(pane.send(SCI_STYLEGETITALIC, style) == italic, "plain text uses the base italic setting");
    }
}

void test_plain_text_transition()
{
    enum class Text_load { Unchanged, Ordinary, Deferred };
    const QString original = QStringLiteral("{\"old\": 1, \"flag\": false}\n").repeated(12);
    const QString replacement = QStringLiteral("{\"new\": 23, \"flag\": true}\n").repeated(12);
    for (Text_load load : {Text_load::Unchanged, Text_load::Ordinary, Text_load::Deferred}) {
        ScintillaQuick_item pane;
        pane.send(SCI_STYLESETFORE, STYLE_DEFAULT, 0x345678);
        pane.send(SCI_STYLESETBACK, STYLE_DEFAULT, 0x123456);
        pane.send(SCI_STYLESETSIZE, STYLE_DEFAULT, 15);
        pane.send(SCI_STYLECLEARALL);
        pane.setProperty("text", original);
        scintillaquick::configure_default_syntax(pane, QStringLiteral("settings.json"), false);
        require(pane.send(SCI_GETSTYLEINDEXAT, 0) == SCE_JSON_OPERATOR,
            "the plain-text transition begins with nonzero JSON styles");
        if (load == Text_load::Ordinary) {
            pane.setProperty("text", replacement);
        }
        else if (load == Text_load::Deferred) {
            pane.set_text_deferred_styling(replacement);
        }

        const QString loaded = load == Text_load::Unchanged ? original : replacement;
        pane.sends(SCI_INSERTTEXT, pane.send(SCI_GETLENGTH), " ");
        pane.send(SCI_SETMARGINWIDTHN, 0, 37);
        pane.send(SCI_MARKERADD, 1, 3);
        pane.send(SCI_SETFOLDLEVEL, 1, (SC_FOLDLEVELBASE + 1) | SC_FOLDLEVELHEADERFLAG);
        pane.send(SCI_SETFOLDEXPANDED, 1, 0);
        pane.send(SCI_SETLINESTATE, 1, 0x5a);
        pane.send(SCI_SETSEL, 2, 9);
        pane.send(SCI_SETXOFFSET, 17);
        pane.send(SCI_SETFIRSTVISIBLELINE, 2);

        // Snapshot after loading: the text setter intentionally clears undo.
        // Syntax configuration itself must preserve the new edit and metadata.
        const QString text_before = pane.property("text").toString();
        const auto first_visible = pane.send(SCI_GETFIRSTVISIBLELINE);
        const auto x_offset = pane.send(SCI_GETXOFFSET);
        const auto marker = pane.send(SCI_MARKERGET, 1);
        const auto fold_level = pane.send(SCI_GETFOLDLEVEL, 1);
        const auto fold_expanded = pane.send(SCI_GETFOLDEXPANDED, 1);
        require(pane.send(SCI_CANUNDO) != 0, "the transition fixture must have an undoable edit");
        int text_changes = 0;
        const auto connection = QObject::connect(&pane, &ScintillaQuick_item::textChanged,
            [&] { ++text_changes; });

        scintillaquick::configure_default_syntax(pane, QStringLiteral("notes.txt"), false);
        require_plain_text_styles(pane);
        require(pane.send(SCI_GETENDSTYLED) == pane.send(SCI_GETLENGTH),
            "known-language transitions fully style plain text");
        require(pane.property("text").toString() == text_before && text_changes == 0,
            "plain syntax cleanup does not mutate or republish text");
        require(pane.send(SCI_GETANCHOR) == 2 && pane.send(SCI_GETCURRENTPOS) == 9 &&
                pane.send(SCI_GETFIRSTVISIBLELINE) == first_visible && pane.send(SCI_GETXOFFSET) == x_offset,
            "plain syntax cleanup preserves selection and scrolling");
        require(pane.send(SCI_GETMARGINWIDTHN, 0) == 37 && pane.send(SCI_MARKERGET, 1) == marker &&
                pane.send(SCI_GETFOLDLEVEL, 1) == fold_level &&
                pane.send(SCI_GETFOLDEXPANDED, 1) == fold_expanded && pane.send(SCI_GETLINESTATE, 1) == 0x5a,
            "plain syntax cleanup preserves margins, markers, folds and line state");
        require(pane.send(SCI_STYLEGETFORE, STYLE_DEFAULT) == 0x345678 &&
                pane.send(SCI_STYLEGETBACK, STYLE_DEFAULT) == 0x123456 &&
                pane.send(SCI_STYLEGETBACK, 0) == 0x123456 && pane.send(SCI_STYLEGETSIZE, 0) == 15,
            "plain text retains its base colours, background and font size");
        require(pane.send(SCI_CANUNDO) != 0, "plain syntax cleanup preserves the undoable edit");
        QObject::disconnect(connection);
        pane.send(SCI_UNDO);
        require(pane.property("text").toString() == loaded,
            "undo after a plain-text transition restores the loaded document");
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
    test_plain_text_transition();
    return 0;
}
