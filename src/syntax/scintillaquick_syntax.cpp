#include <scintillaquick/scintillaquick_syntax.h>

#include <scintillaquick/scintillaquick_item.h>
#include <Scintilla.h>
#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>
#include <QByteArray>
#include <QColor>
#include <QFileInfo>
#include <QtLogging>
#include <array>
#include <initializer_list>

namespace
{

enum class Syntax_family
{
    PlainText,
    Cpp,
    Json,
    Python,
    CMake,
    Rust
};

struct Lexer_configuration
{
    QByteArray lexer_name;
    Syntax_family family = Syntax_family::PlainText;
    std::array<QByteArray, 8> keyword_sets {};
};

struct Syntax_palette
{
    QColor comment;
    QColor number;
    QColor string;
    QColor keyword;
    QColor type;
    QColor preprocessor;
    QColor operator_color;
    QColor function;
    QColor property;
    QColor decorator;
    QColor macro;
    QColor error;
};

QByteArray join_keywords(std::initializer_list<const char*> keywords)
{
    QByteArray joined;
    bool first = true;
    for (const char* keyword : keywords) {
        if (!first) {
            joined += ' ';
        }
        joined += keyword;
        first = false;
    }
    return joined;
}

Lexer_configuration lexer_configuration_for_path(const QString& path)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    const QString file_name = info.fileName().toLower();

    if (file_name == QStringLiteral("cmakelists.txt") || suffix == QStringLiteral("cmake")) {
        Lexer_configuration config;
        config.lexer_name = "cmake";
        config.family = Syntax_family::CMake;
        config.keyword_sets[0] = join_keywords({
            "if", "elseif", "else", "endif",
            "foreach", "endforeach", "while", "endwhile",
            "function", "endfunction", "macro", "endmacro",
            "set", "unset", "option", "project", "add_executable",
            "add_library", "target_link_libraries", "target_include_directories",
            "target_compile_definitions", "target_compile_options",
            "find_package", "include", "message", "install",
            "file", "list", "string", "math", "cmake_minimum_required"
        });
        return config;
    }

    if (suffix == QStringLiteral("json") || suffix == QStringLiteral("jsonc") || suffix == QStringLiteral("jsonld")) {
        Lexer_configuration config;
        config.lexer_name = "json";
        config.family = Syntax_family::Json;
        config.keyword_sets[0] = join_keywords({"true", "false", "null"});
        return config;
    }

    if (suffix == QStringLiteral("py") || suffix == QStringLiteral("pyw")) {
        Lexer_configuration config;
        config.lexer_name = "python";
        config.family = Syntax_family::Python;
        config.keyword_sets[0] = join_keywords({
            "False", "None", "True", "and", "as", "assert", "async", "await",
            "break", "class", "continue", "def", "del", "elif", "else", "except",
            "finally", "for", "from", "global", "if", "import", "in", "is",
            "lambda", "nonlocal", "not", "or", "pass", "raise", "return",
            "try", "while", "with", "yield", "match", "case"
        });
        config.keyword_sets[1] = join_keywords({
            "self", "cls", "int", "float", "str", "bool", "list", "dict",
            "tuple", "set", "object", "classmethod", "staticmethod", "property"
        });
        return config;
    }

    if (suffix == QStringLiteral("rs")) {
        Lexer_configuration config;
        config.lexer_name = "rust";
        config.family = Syntax_family::Rust;
        config.keyword_sets[0] = join_keywords({
            "as", "async", "await", "break", "const", "continue", "crate", "dyn",
            "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in",
            "let", "loop", "match", "mod", "move", "mut", "pub", "ref",
            "return", "self", "Self", "static", "struct", "super", "trait",
            "true", "type", "unsafe", "use", "where", "while"
        });
        config.keyword_sets[1] = join_keywords({
            "i8", "i16", "i32", "i64", "i128", "isize",
            "u8", "u16", "u32", "u64", "u128", "usize",
            "f32", "f64", "bool", "char", "str", "Option", "Result", "String", "Vec"
        });
        return config;
    }

    const bool is_cpp_family =
        suffix == QStringLiteral("c") ||
        suffix == QStringLiteral("cc") ||
        suffix == QStringLiteral("cpp") ||
        suffix == QStringLiteral("cxx") ||
        suffix == QStringLiteral("c++") ||
        suffix == QStringLiteral("h") ||
        suffix == QStringLiteral("hh") ||
        suffix == QStringLiteral("hpp") ||
        suffix == QStringLiteral("hxx") ||
        suffix == QStringLiteral("h++") ||
        suffix == QStringLiteral("tpp") ||
        suffix == QStringLiteral("ixx") ||
        suffix == QStringLiteral("cppm") ||
        suffix == QStringLiteral("inl") ||
        suffix == QStringLiteral("ipp") ||
        suffix == QStringLiteral("qml") ||
        suffix == QStringLiteral("js") ||
        suffix == QStringLiteral("mjs") ||
        suffix == QStringLiteral("cjs") ||
        suffix == QStringLiteral("qs") ||
        suffix == QStringLiteral("qmltypes") ||
        suffix == QStringLiteral("vert") ||
        suffix == QStringLiteral("frag") ||
        suffix == QStringLiteral("geom") ||
        suffix == QStringLiteral("glsl") ||
        suffix == QStringLiteral("comp") ||
        suffix == QStringLiteral("tesc") ||
        suffix == QStringLiteral("tese") ||
        suffix == QStringLiteral("cg") ||
        suffix == QStringLiteral("fx") ||
        suffix == QStringLiteral("pro") ||
        suffix == QStringLiteral("pri") ||
        suffix == QStringLiteral("prf");

    if (is_cpp_family) {
        Lexer_configuration config;
        config.lexer_name = "cpp";
        config.family = Syntax_family::Cpp;
        config.keyword_sets[0] = join_keywords({
            "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch",
            "char", "class", "const", "constexpr", "consteval", "constinit",
            "continue", "co_await", "co_return", "co_yield", "decltype", "default",
            "delete", "do", "double", "else", "enum", "explicit", "export",
            "extern", "false", "float", "for", "friend", "goto", "if", "inline",
            "int", "long", "mutable", "namespace", "new", "noexcept", "nullptr",
            "operator", "private", "protected", "public", "register", "reinterpret_cast",
            "return", "short", "signed", "sizeof", "static", "static_assert",
            "struct", "switch", "template", "this", "thread_local", "throw",
            "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
            "using", "virtual", "void", "volatile", "while"
        });
        config.keyword_sets[1] = join_keywords({
            "QString", "QByteArray", "QVariant", "QObject", "QWidget", "QQuickItem",
            "QColor", "QFont", "QFileInfo", "vector", "string", "string_view",
            "optional", "unique_ptr", "shared_ptr", "std", "signals", "slots", "emit",
            "property", "function", "var", "import", "readonly", "Item", "Rectangle",
            "Text", "MouseArea", "anchors", "Component", "pragma", "signal",
            "uniform", "in", "out", "inout", "vec2", "vec3", "vec4", "mat3", "mat4",
            "sampler2D", "samplerCube", "float2", "float3", "float4", "half", "fixed",
            "CONFIG", "DEFINES", "SOURCES", "HEADERS", "INCLUDEPATH", "QT", "TARGET", "TEMPLATE"
        });
        return config;
    }

    return {};
}

Syntax_palette syntax_palette(bool dark_mode)
{
    if (dark_mode) {
        return {
            QColor(QStringLiteral("#6a9955")),
            QColor(QStringLiteral("#b5cea8")),
            QColor(QStringLiteral("#ce9178")),
            QColor(QStringLiteral("#569cd6")),
            QColor(QStringLiteral("#4ec9b0")),
            QColor(QStringLiteral("#c586c0")),
            QColor(QStringLiteral("#d4d4d4")),
            QColor(QStringLiteral("#dcdcaa")),
            QColor(QStringLiteral("#9cdcfe")),
            QColor(QStringLiteral("#c586c0")),
            QColor(QStringLiteral("#d7ba7d")),
            QColor(QStringLiteral("#f44747"))
        };
    }

    return {
        QColor(QStringLiteral("#008000")),
        QColor(QStringLiteral("#098658")),
        QColor(QStringLiteral("#a31515")),
        QColor(QStringLiteral("#0000ff")),
        QColor(QStringLiteral("#267f99")),
        QColor(QStringLiteral("#af00db")),
        QColor(QStringLiteral("#202020")),
        QColor(QStringLiteral("#795e26")),
        QColor(QStringLiteral("#0451a5")),
        QColor(QStringLiteral("#8b5fbf")),
        QColor(QStringLiteral("#b07d00")),
        QColor(QStringLiteral("#e51400"))
    };
}


}

void scintillaquick::reset_syntax_styles(ScintillaQuick_item& pane)
{
    // Reset only lexical styles. Keep the host's font, margins and backgrounds.
    const auto foreground = pane.send(SCI_STYLEGETFORE, STYLE_DEFAULT);
    const auto bold       = pane.send(SCI_STYLEGETBOLD, STYLE_DEFAULT);
    const auto italic     = pane.send(SCI_STYLEGETITALIC, STYLE_DEFAULT);
    for (int style = 0; style <= STYLE_MAX; ++style) {
        if (style >= STYLE_DEFAULT && style <= STYLE_LASTPREDEFINED) {
            continue;
        }
        pane.send(SCI_STYLESETFORE, style, foreground);
        pane.send(SCI_STYLESETBOLD, style, bold);
        pane.send(SCI_STYLESETITALIC, style, italic);
    }

    for (int index = 0; index < 8; ++index) {
        pane.send(SCI_SETKEYWORDS, static_cast<uptr_t>(index), reinterpret_cast<sptr_t>(""));
    }
}

void scintillaquick::configure_default_syntax(ScintillaQuick_item& pane, const QString& path, bool dark_mode)
{
    const Lexer_configuration config = lexer_configuration_for_path(path);

    const auto name_length = pane.send(SCI_GETLEXERLANGUAGE);
    QByteArray active_name(static_cast<qsizetype>(name_length) + 1, '\0');
    pane.send(SCI_GETLEXERLANGUAGE, 0, reinterpret_cast<sptr_t>(active_name.data()));
    active_name.resize(static_cast<qsizetype>(name_length));
    if (active_name != config.lexer_name) {
        Scintilla::ILexer5* lexer = nullptr;
        if (!config.lexer_name.isEmpty()) {
            lexer = ::CreateLexer(config.lexer_name.constData());
            if (!lexer) {
                qFatal("Could not create the requested document lexer: %s", config.lexer_name.constData());
            }
        }
        pane.send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(lexer));
    }

    reset_syntax_styles(pane);

    for (int index = 0; index < static_cast<int>(config.keyword_sets.size()); ++index) {
        pane.send(SCI_SETKEYWORDS,
            static_cast<uptr_t>(index),
            reinterpret_cast<sptr_t>(const_cast<char*>(config.keyword_sets[index].constData())));
    }

    const Syntax_palette palette = syntax_palette(dark_mode);
    const auto set_fore = [&pane](int style, const QColor& color) {
        pane.send(SCI_STYLESETFORE, static_cast<uptr_t>(style), scintillaquick::rgb_from_color(color));
    };
    const auto set_bold = [&pane](int style, bool enabled) {
        pane.send(SCI_STYLESETBOLD, static_cast<uptr_t>(style), enabled ? 1 : 0);
    };

    switch (config.family) {
    case Syntax_family::Json:
        set_fore(SCE_JSON_NUMBER, palette.number);
        set_fore(SCE_JSON_STRING, palette.string);
        set_fore(SCE_JSON_STRINGEOL, palette.error);
        set_fore(SCE_JSON_PROPERTYNAME, palette.property);
        set_fore(SCE_JSON_ESCAPESEQUENCE, palette.preprocessor);
        set_fore(SCE_JSON_LINECOMMENT, palette.comment);
        set_fore(SCE_JSON_BLOCKCOMMENT, palette.comment);
        set_fore(SCE_JSON_OPERATOR, palette.operator_color);
        set_fore(SCE_JSON_KEYWORD, palette.keyword);
        set_fore(SCE_JSON_LDKEYWORD, palette.keyword);
        set_fore(SCE_JSON_ERROR, palette.error);
        set_bold(SCE_JSON_PROPERTYNAME, true);
        break;

    case Syntax_family::Python:
        set_fore(SCE_P_COMMENTLINE, palette.comment);
        set_fore(SCE_P_COMMENTBLOCK, palette.comment);
        set_fore(SCE_P_NUMBER, palette.number);
        set_fore(SCE_P_STRING, palette.string);
        set_fore(SCE_P_CHARACTER, palette.string);
        set_fore(SCE_P_TRIPLE, palette.string);
        set_fore(SCE_P_TRIPLEDOUBLE, palette.string);
        set_fore(SCE_P_FSTRING, palette.string);
        set_fore(SCE_P_FCHARACTER, palette.string);
        set_fore(SCE_P_FTRIPLE, palette.string);
        set_fore(SCE_P_FTRIPLEDOUBLE, palette.string);
        set_fore(SCE_P_WORD, palette.keyword);
        set_fore(SCE_P_WORD2, palette.type);
        set_fore(SCE_P_CLASSNAME, palette.type);
        set_fore(SCE_P_DEFNAME, palette.function);
        set_fore(SCE_P_DECORATOR, palette.decorator);
        set_fore(SCE_P_OPERATOR, palette.operator_color);
        set_fore(SCE_P_STRINGEOL, palette.error);
        set_bold(SCE_P_WORD, true);
        set_bold(SCE_P_WORD2, true);
        set_bold(SCE_P_CLASSNAME, true);
        break;

    case Syntax_family::CMake:
        set_fore(SCE_CMAKE_COMMENT, palette.comment);
        set_fore(SCE_CMAKE_STRINGDQ, palette.string);
        set_fore(SCE_CMAKE_STRINGLQ, palette.string);
        set_fore(SCE_CMAKE_STRINGRQ, palette.string);
        set_fore(SCE_CMAKE_COMMANDS, palette.keyword);
        set_fore(SCE_CMAKE_PARAMETERS, palette.operator_color);
        set_fore(SCE_CMAKE_VARIABLE, palette.property);
        set_fore(SCE_CMAKE_USERDEFINED, palette.function);
        set_fore(SCE_CMAKE_WHILEDEF, palette.keyword);
        set_fore(SCE_CMAKE_FOREACHDEF, palette.keyword);
        set_fore(SCE_CMAKE_IFDEFINEDEF, palette.keyword);
        set_fore(SCE_CMAKE_MACRODEF, palette.function);
        set_fore(SCE_CMAKE_STRINGVAR, palette.property);
        set_fore(SCE_CMAKE_NUMBER, palette.number);
        set_bold(SCE_CMAKE_COMMANDS, true);
        set_bold(SCE_CMAKE_MACRODEF, true);
        break;

    case Syntax_family::Rust:
        set_fore(SCE_RUST_COMMENTBLOCK, palette.comment);
        set_fore(SCE_RUST_COMMENTLINE, palette.comment);
        set_fore(SCE_RUST_COMMENTBLOCKDOC, palette.comment);
        set_fore(SCE_RUST_COMMENTLINEDOC, palette.comment);
        set_fore(SCE_RUST_NUMBER, palette.number);
        set_fore(SCE_RUST_WORD, palette.keyword);
        set_fore(SCE_RUST_WORD2, palette.type);
        set_fore(SCE_RUST_WORD3, palette.type);
        set_fore(SCE_RUST_WORD4, palette.type);
        set_fore(SCE_RUST_WORD5, palette.type);
        set_fore(SCE_RUST_WORD6, palette.type);
        set_fore(SCE_RUST_WORD7, palette.type);
        set_fore(SCE_RUST_STRING, palette.string);
        set_fore(SCE_RUST_STRINGR, palette.string);
        set_fore(SCE_RUST_CHARACTER, palette.string);
        set_fore(SCE_RUST_OPERATOR, palette.operator_color);
        set_fore(SCE_RUST_LIFETIME, palette.property);
        set_fore(SCE_RUST_MACRO, palette.macro);
        set_fore(SCE_RUST_LEXERROR, palette.error);
        set_fore(SCE_RUST_BYTESTRING, palette.string);
        set_fore(SCE_RUST_BYTESTRINGR, palette.string);
        set_fore(SCE_RUST_BYTECHARACTER, palette.string);
        set_bold(SCE_RUST_WORD, true);
        set_bold(SCE_RUST_WORD2, true);
        break;

    case Syntax_family::Cpp:
        set_fore(SCE_C_COMMENT, palette.comment);
        set_fore(SCE_C_COMMENTLINE, palette.comment);
        set_fore(SCE_C_COMMENTDOC, palette.comment);
        set_fore(SCE_C_COMMENTLINEDOC, palette.comment);
        set_fore(SCE_C_COMMENTDOCKEYWORD, palette.decorator);
        set_fore(SCE_C_COMMENTDOCKEYWORDERROR, palette.error);
        set_fore(SCE_C_PREPROCESSORCOMMENT, palette.comment);
        set_fore(SCE_C_PREPROCESSORCOMMENTDOC, palette.comment);
        set_fore(SCE_C_NUMBER, palette.number);
        set_fore(SCE_C_WORD, palette.keyword);
        set_fore(SCE_C_WORD2, palette.type);
        set_fore(SCE_C_GLOBALCLASS, palette.type);
        set_fore(SCE_C_STRING, palette.string);
        set_fore(SCE_C_CHARACTER, palette.string);
        set_fore(SCE_C_STRINGRAW, palette.string);
        set_fore(SCE_C_TRIPLEVERBATIM, palette.string);
        set_fore(SCE_C_HASHQUOTEDSTRING, palette.string);
        set_fore(SCE_C_STRINGEOL, palette.error);
        set_fore(SCE_C_PREPROCESSOR, palette.preprocessor);
        set_fore(SCE_C_OPERATOR, palette.operator_color);
        set_fore(SCE_C_REGEX, palette.string);
        set_fore(SCE_C_UUID, palette.number);
        set_fore(SCE_C_USERLITERAL, palette.macro);
        set_fore(SCE_C_TASKMARKER, palette.decorator);
        set_fore(SCE_C_ESCAPESEQUENCE, palette.preprocessor);
        set_bold(SCE_C_WORD, true);
        set_bold(SCE_C_WORD2, true);
        set_bold(SCE_C_PREPROCESSOR, true);
        break;

    case Syntax_family::PlainText:
        // Removing a lexer leaves the document's existing style bytes intact.
        pane.send(SCI_STARTSTYLING, 0);
        pane.send(SCI_SETSTYLING, static_cast<uptr_t>(pane.send(SCI_GETLENGTH)), 0);
        break;
    }

    pane.send(SCI_COLOURISE, 0, -1);
}
