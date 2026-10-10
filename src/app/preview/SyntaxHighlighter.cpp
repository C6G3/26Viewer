#include "SyntaxHighlighter.hpp"

#include <QGuiApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>

namespace viewer::app {

namespace {

QTextCharFormat colourFormat(const char* colour, bool bold = false, bool italic = false)
{
    QTextCharFormat f;
    f.setForeground(QColor(QString::fromLatin1(colour)));
    if (bold)
        f.setFontWeight(QFont::DemiBold);
    f.setFontItalic(italic);
    return f;
}

QSet<QString> words(std::initializer_list<const char*> list, bool lower = false)
{
    QSet<QString> set;
    for (const char* w : list)
        set.insert(lower ? QString::fromLatin1(w).toLower() : QString::fromLatin1(w));
    return set;
}

const QSet<QString>& keywordsFor(TextSyntax syntax)
{
    static const QSet<QString> lua = words({"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
                                            "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
                                            "then", "true", "until", "while"});
    static const QSet<QString> shader = words(
        {"if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default", "struct",
         "cbuffer", "tbuffer", "typedef", "const", "static", "uniform", "in", "out", "inout", "register",
         "technique", "technique10", "technique11", "pass", "compile", "discard", "true", "false", "extern",
         "shared", "volatile", "inline", "namespace", "class", "public", "private", "protected", "template",
         "typename", "sizeof", "nullptr", "this", "new", "delete", "enum", "union", "layout", "varying",
         "attribute", "precision", "highp", "mediump", "lowp", "groupshared", "nointerpolation", "linear",
         "centroid", "row_major", "column_major", "asm", "decl", "VertexShader", "PixelShader", "GeometryShader"});
    static const QSet<QString> json = words({"true", "false", "null"});
    // QuickBMS commands (case-insensitive).
    static const QSet<QString> bms = words(
        {"QuickBMSver", "FindLoc", "For", "Next", "Get", "GetDString", "GoTo", "IDString", "Log", "CLog", "Math",
         "XMath", "Open", "SavePos", "Set", "Do", "While", "String", "CleanExit", "If", "Else", "Elif", "EndIf",
         "GetCT", "ComType", "ReverseShort", "ReverseLong", "ReverseLongLong", "Endian", "FileXOR", "FileRot",
         "FileCrypt", "Strlen", "GetVarChr", "PutVarChr", "Debug", "Padding", "Append", "Encryption", "Print",
         "GetArray", "PutArray", "SortArray", "SearchArray", "CallFunction", "StartFunction", "EndFunction",
         "ScanDir", "CallDLL", "Put", "PutDString", "PutCT", "GetBits", "PutBits", "Include", "NameCRC",
         "Codepage", "SLog", "Reimport", "Label", "Break", "Continue", "Prev", "Arg", "Then", "To"},
        true);
    static const QSet<QString> none;
    switch (syntax) {
    case TextSyntax::Lua: return lua;
    case TextSyntax::Shader: return shader;
    case TextSyntax::Json: return json;
    case TextSyntax::QuickBms: return bms;
    default: return none;
    }
}

const QSet<QString>& typesFor(TextSyntax syntax)
{
    static const QSet<QString> shader = [] {
        QSet<QString> set = words({"void", "bool", "int", "uint", "dword", "half", "float", "double", "char",
                                   "short", "long", "unsigned", "signed", "auto", "matrix", "vector", "string",
                                   "sampler", "sampler1D", "sampler2D", "sampler3D", "samplerCUBE",
                                   "sampler_state", "SamplerState", "SamplerComparisonState", "texture",
                                   "texture1D", "texture2D", "texture3D", "textureCUBE", "Texture1D", "Texture2D",
                                   "Texture3D", "TextureCube", "Buffer", "StructuredBuffer", "RWTexture2D",
                                   "mat2", "mat3", "mat4", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
                                   "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "size_t"});
        for (const char* base : {"bool", "int", "uint", "half", "float", "double"})
            for (int i = 1; i <= 4; ++i) {
                set.insert(QStringLiteral("%1%2").arg(QLatin1String(base)).arg(i));
                for (int j = 1; j <= 4; ++j)
                    set.insert(QStringLiteral("%1%2x%3").arg(QLatin1String(base)).arg(i).arg(j));
            }
        return set;
    }();
    static const QSet<QString> bms =
        words({"long", "short", "byte", "threebyte", "longlong", "string", "asize", "float", "double", "longdouble",
               "filename", "basename", "extension", "unicode", "line", "bits", "variant", "time", "time64",
               "clsid", "ipv4", "ipv6", "assembly", "compressed"},
              true);
    static const QSet<QString> none;
    return syntax == TextSyntax::Shader ? shader : syntax == TextSyntax::QuickBms ? bms : none;
}

bool isIdentStart(QChar c)
{
    return c.isLetter() || c == QLatin1Char('_');
}

bool isIdent(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

/// Length of a number starting at `i` (decimal, hex, float with exponent and suffix), or 0.
qsizetype numberLength(const QString& text, qsizetype i)
{
    const qsizetype n = text.size();
    if (i > 0 && isIdent(text[i - 1]))
        return 0; // part of an identifier like "float4"
    qsizetype j = i;
    if (j < n && (text[j] == QLatin1Char('-') || text[j] == QLatin1Char('+')))
        ++j;
    if (j + 1 < n && text[j] == QLatin1Char('0') && (text[j + 1] == QLatin1Char('x') || text[j + 1] == QLatin1Char('X'))) {
        j += 2;
        while (j < n && (text[j].isDigit() || QStringLiteral("abcdefABCDEF").contains(text[j])))
            ++j;
        return j - i;
    }
    const qsizetype digits = j;
    while (j < n && text[j].isDigit())
        ++j;
    if (j < n && text[j] == QLatin1Char('.'))
        for (++j; j < n && text[j].isDigit();)
            ++j;
    if (j == digits || (j == digits + 1 && text[digits] == QLatin1Char('.')))
        return 0;
    if (j < n && (text[j] == QLatin1Char('e') || text[j] == QLatin1Char('E'))) {
        qsizetype k = j + 1;
        if (k < n && (text[k] == QLatin1Char('-') || text[k] == QLatin1Char('+')))
            ++k;
        if (k < n && text[k].isDigit()) {
            while (k < n && text[k].isDigit())
                ++k;
            j = k;
        }
    }
    while (j < n && QStringLiteral("fFhHuUlL").contains(text[j]))
        ++j;
    if (j < n && isIdent(text[j]))
        return 0;
    return j - i;
}

} // namespace

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* document) : QSyntaxHighlighter(document)
{
    refreshColours();
}

void SyntaxHighlighter::setSyntax(TextSyntax syntax)
{
    if (syntax == m_syntax)
        return;
    m_syntax = syntax;
    rehighlight();
}

void SyntaxHighlighter::refreshColours()
{
    const bool dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
    if (dark) { // in the spirit of VS Code "Dark+"
        m_keyword = colourFormat("#569CD6");
        m_type = colourFormat("#4EC9B0");
        m_string = colourFormat("#CE9178");
        m_number = colourFormat("#B5CEA8");
        m_comment = colourFormat("#6A9955", false, true);
        m_tag = colourFormat("#569CD6");
        m_attribute = colourFormat("#9CDCFE");
        m_value = colourFormat("#CE9178");
        m_entity = colourFormat("#D7BA7D");
        m_section = colourFormat("#4EC9B0", true);
        m_key = colourFormat("#9CDCFE");
        m_punctuation = colourFormat("#808080");
        m_preprocessor = colourFormat("#C586C0");
        const char* columns[] = {"#E06C75", "#61AFEF", "#98C379", "#E5C07B", "#C678DD", "#56B6C2"};
        for (std::size_t i = 0; i < m_columns.size(); ++i)
            m_columns[i] = colourFormat(columns[i]);
    } else { // in the spirit of Visual Studio "Light"
        m_keyword = colourFormat("#0000FF");
        m_type = colourFormat("#267F99");
        m_string = colourFormat("#A31515");
        m_number = colourFormat("#098658");
        m_comment = colourFormat("#008000", false, true);
        m_tag = colourFormat("#800000");
        m_attribute = colourFormat("#E50000");
        m_value = colourFormat("#0000FF");
        m_entity = colourFormat("#B0590B");
        m_section = colourFormat("#267F99", true);
        m_key = colourFormat("#001080");
        m_punctuation = colourFormat("#7A7A7A");
        m_preprocessor = colourFormat("#AF00DB");
        const char* columns[] = {"#B3261E", "#1F5FAA", "#2E7D32", "#8A5A00", "#7B2FA6", "#00727E"};
        for (std::size_t i = 0; i < m_columns.size(); ++i)
            m_columns[i] = colourFormat(columns[i]);
    }
    rehighlight();
}

void SyntaxHighlighter::highlightBlock(const QString& text)
{
    setCurrentBlockState(Normal);
    switch (m_syntax) {
    case TextSyntax::Plain: break;
    case TextSyntax::Xml: highlightXml(text); break;
    case TextSyntax::Ini: highlightIni(text); break;
    case TextSyntax::Csv: highlightCsv(text); break;
    case TextSyntax::Json:
    case TextSyntax::Lua:
    case TextSyntax::Shader:
    case TextSyntax::QuickBms: highlightCode(text); break;
    }
}

// ---- XML ------------------------------------------------------------------------------------

void SyntaxHighlighter::highlightXml(const QString& text)
{
    const qsizetype n = text.size();
    qsizetype i = 0;
    int state = previousBlockState();

    const auto finishUntil = [&](const QString& end, const QTextCharFormat& f, int stateIfOpen) {
        const qsizetype close = text.indexOf(end, i);
        const qsizetype stop = close < 0 ? n : close + end.size();
        setFormat(i, stop - i, f);
        i = stop;
        if (close < 0)
            setCurrentBlockState(stateIfOpen);
        return close >= 0;
    };

    while (i < n) {
        if (state == InComment) {
            if (!finishUntil(QStringLiteral("-->"), m_comment, InComment))
                return;
            state = Normal;
            continue;
        }
        if (state == InCData) {
            if (!finishUntil(QStringLiteral("]]>"), m_string, InCData))
                return;
            state = Normal;
            continue;
        }
        if (state == InTag) {
            // Inside <name ...>: attributes, values, and the closing > or />.
            const QChar c = text[i];
            if (c == QLatin1Char('>') || (c == QLatin1Char('/') && i + 1 < n && text[i + 1] == QLatin1Char('>'))
                || (c == QLatin1Char('?') && i + 1 < n && text[i + 1] == QLatin1Char('>'))) {
                const qsizetype len = c == QLatin1Char('>') ? 1 : 2;
                setFormat(i, len, m_tag);
                i += len;
                state = Normal;
            } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                const qsizetype close = text.indexOf(c, i + 1);
                const qsizetype stop = close < 0 ? n : close + 1;
                setFormat(i, stop - i, m_value);
                i = stop;
            } else if (isIdentStart(c) || c == QLatin1Char(':')) {
                qsizetype j = i;
                while (j < n && (isIdent(text[j]) || QStringLiteral(":-.").contains(text[j])))
                    ++j;
                setFormat(i, j - i, m_attribute);
                i = j;
            } else {
                if (c == QLatin1Char('='))
                    setFormat(i, 1, m_punctuation);
                ++i;
            }
            if (i >= n && state == InTag)
                setCurrentBlockState(InTag);
            continue;
        }

        // Normal text between tags.
        const QChar c = text[i];
        if (c == QLatin1Char('<')) {
            if (text.mid(i, 4) == QLatin1String("<!--")) {
                state = InComment;
                setFormat(i, 4, m_comment);
                i += 4;
            } else if (text.mid(i, 9) == QLatin1String("<![CDATA[")) {
                setFormat(i, 9, m_punctuation);
                i += 9;
                state = InCData;
            } else {
                // <name, </name, <?name, <!DOCTYPE
                qsizetype j = i + 1;
                if (j < n && QStringLiteral("/?!").contains(text[j]))
                    ++j;
                while (j < n && (isIdent(text[j]) || QStringLiteral(":-.").contains(text[j])))
                    ++j;
                setFormat(i, j - i, m_tag);
                i = j;
                state = InTag;
                if (i >= n)
                    setCurrentBlockState(InTag);
            }
        } else if (c == QLatin1Char('&')) {
            const qsizetype semi = text.indexOf(QLatin1Char(';'), i);
            if (semi > i && semi - i < 12) {
                setFormat(i, semi - i + 1, m_entity);
                i = semi + 1;
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
}

// ---- INI ------------------------------------------------------------------------------------

void SyntaxHighlighter::highlightIni(const QString& text)
{
    const qsizetype start = text.indexOf(QRegularExpression(QStringLiteral("\\S")));
    if (start < 0)
        return;
    const QChar first = text[start];
    if (first == QLatin1Char(';') || first == QLatin1Char('#')) {
        setFormat(start, text.size() - start, m_comment);
        return;
    }
    if (first == QLatin1Char('[')) {
        setFormat(start, text.size() - start, m_section);
        return;
    }
    const qsizetype eq = text.indexOf(QLatin1Char('='));
    if (eq < 0)
        return;
    setFormat(start, eq - start, m_key);
    setFormat(eq, 1, m_punctuation);
    // The value: quoted strings, numbers and on/off words stand out.
    qsizetype i = eq + 1;
    while (i < text.size() && text[i].isSpace())
        ++i;
    if (i >= text.size())
        return;
    const QString value = text.mid(i).trimmed();
    const QString lower = value.toLower();
    if (value.startsWith(QLatin1Char('"')))
        setFormat(i, value.size(), m_string);
    else if (numberLength(value, 0) == value.size())
        setFormat(i, value.size(), m_number);
    else if (lower == QLatin1String("true") || lower == QLatin1String("false") || lower == QLatin1String("yes")
             || lower == QLatin1String("no") || lower == QLatin1String("on") || lower == QLatin1String("off"))
        setFormat(i, value.size(), m_keyword);
}

// ---- CSV ------------------------------------------------------------------------------------

void SyntaxHighlighter::highlightCsv(const QString& text)
{
    // Tab-separated if the line has tabs and no commas.
    const QChar separator = !text.contains(QLatin1Char(',')) && text.contains(QLatin1Char('\t')) ? QLatin1Char('\t')
                                                                                               : QLatin1Char(',');
    const qsizetype n = text.size();
    std::size_t column = 0;
    qsizetype fieldStart = 0;
    bool quoted = previousBlockState() == InQuote; // a quoted field running over lines
    for (qsizetype i = 0; i <= n; ++i) {
        if (i < n && text[i] == QLatin1Char('"')) {
            if (quoted && i + 1 < n && text[i + 1] == QLatin1Char('"'))
                ++i; // "" escape
            else
                quoted = !quoted;
            continue;
        }
        if (i == n || (!quoted && text[i] == separator)) {
            setFormat(fieldStart, i - fieldStart, m_columns[column % m_columns.size()]);
            if (i < n && separator != QLatin1Char('\t'))
                setFormat(i, 1, m_punctuation);
            fieldStart = i + 1;
            ++column;
        }
    }
    if (quoted)
        setCurrentBlockState(InQuote);
}

// ---- Code: JSON, Lua, shaders/C, QuickBMS ----------------------------------------------------------

void SyntaxHighlighter::highlightCode(const QString& text)
{
    const qsizetype n = text.size();
    const auto& keywords = keywordsFor(m_syntax);
    const auto& types = typesFor(m_syntax);
    const bool caseless = m_syntax == TextSyntax::QuickBms;
    const bool cComments = m_syntax == TextSyntax::Shader || m_syntax == TextSyntax::Json;
    qsizetype i = 0;

    // Multi-line comments carried over from the previous line.
    if (previousBlockState() == InComment || previousBlockState() == InLongComment) {
        const QString end = previousBlockState() == InComment ? QStringLiteral("*/") : QStringLiteral("]]");
        const qsizetype close = text.indexOf(end);
        if (close < 0) {
            setFormat(0, n, m_comment);
            setCurrentBlockState(previousBlockState());
            return;
        }
        setFormat(0, close + end.size(), m_comment);
        i = close + end.size();
    }

    // Preprocessor lines (#include, #define...) in shaders.
    if (m_syntax == TextSyntax::Shader) {
        const QString trimmed = text.trimmed();
        if (trimmed.startsWith(QLatin1Char('#'))) {
            const qsizetype comment = text.indexOf(QStringLiteral("//"));
            setFormat(0, comment < 0 ? n : comment, m_preprocessor);
            if (comment >= 0)
                setFormat(comment, n - comment, m_comment);
            return;
        }
    }

    while (i < n) {
        const QChar c = text[i];
        const QStringView rest = QStringView(text).mid(i);

        // Comments
        if ((cComments && rest.startsWith(QLatin1String("//"))) || (m_syntax == TextSyntax::QuickBms && c == QLatin1Char('#'))
            || (m_syntax == TextSyntax::Lua && rest.startsWith(QLatin1String("--")) && !rest.startsWith(QLatin1String("--[[")))) {
            setFormat(i, n - i, m_comment);
            return;
        }
        if ((cComments && rest.startsWith(QLatin1String("/*"))) || (m_syntax == TextSyntax::Lua && rest.startsWith(QLatin1String("--[[")))) {
            const bool lua = m_syntax == TextSyntax::Lua;
            const QString end = lua ? QStringLiteral("]]") : QStringLiteral("*/");
            const qsizetype close = text.indexOf(end, i + (lua ? 4 : 2));
            if (close < 0) {
                setFormat(i, n - i, m_comment);
                setCurrentBlockState(lua ? InLongComment : InComment);
                return;
            }
            setFormat(i, close + end.size() - i, m_comment);
            i = close + end.size();
            continue;
        }

        // Strings
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            qsizetype j = i + 1;
            while (j < n && text[j] != c) {
                if (text[j] == QLatin1Char('\\'))
                    ++j;
                ++j;
            }
            const qsizetype stop = std::min(j + 1, n);
            // JSON: a string followed by ':' is a key.
            bool isKey = false;
            if (m_syntax == TextSyntax::Json) {
                qsizetype k = stop;
                while (k < n && text[k].isSpace())
                    ++k;
                isKey = k < n && text[k] == QLatin1Char(':');
            }
            setFormat(i, stop - i, isKey ? m_key : m_string);
            i = stop;
            continue;
        }

        // Numbers
        if (c.isDigit() || ((c == QLatin1Char('-') || c == QLatin1Char('.')) && i + 1 < n && text[i + 1].isDigit())) {
            if (const qsizetype len = numberLength(text, i)) {
                setFormat(i, len, m_number);
                i += len;
                continue;
            }
        }

        // Words
        if (isIdentStart(c)) {
            qsizetype j = i;
            while (j < n && isIdent(text[j]))
                ++j;
            QString word = text.mid(i, j - i);
            if (caseless)
                word = word.toLower();
            if (keywords.contains(word))
                setFormat(i, j - i, m_keyword);
            else if (types.contains(word))
                setFormat(i, j - i, m_type);
            i = j;
            continue;
        }
        ++i;
    }
}

} // namespace viewer::app
