#include "viewer/core/Text.hpp"

#include "viewer/core/FileTypes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace viewer {

namespace {

unsigned byteAt(ByteSpan data, std::size_t i)
{
    return std::to_integer<unsigned>(data[i]);
}

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

/// Reads one UTF-8 character at `i` (advancing it); nullopt on malformed input
/// (bad continuation bytes, overlong forms, surrogates, > U+10FFFF).
std::optional<char32_t> nextUtf8(std::string_view s, std::size_t& i)
{
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    int extra = 0;
    char32_t c = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0) {
        extra = 1, c = b0 & 0x1F, min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2, c = b0 & 0x0F, min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        extra = 3, c = b0 & 0x07, min = 0x10000;
    } else {
        return std::nullopt;
    }
    if (i + extra >= s.size())
        return std::nullopt;
    for (int k = 1; k <= extra; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80)
            return std::nullopt;
        c = (c << 6) | (b & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return std::nullopt;
    i += extra + 1;
    return c;
}

bool validUtf8(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();)
        if (!nextUtf8(s, i))
            return false;
    return true;
}

std::string decodeUtf16(ByteSpan data, bool bigEndian)
{
    std::string out;
    out.reserve(data.size());
    const auto unit = [&](std::size_t i) {
        return bigEndian ? (byteAt(data, i) << 8) | byteAt(data, i + 1) : byteAt(data, i) | (byteAt(data, i + 1) << 8);
    };
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        char32_t c = unit(i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < data.size()) {
            const char32_t low = unit(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (c >= 0xD800 && c <= 0xDFFF)
            c = 0xFFFD; // unpaired surrogate
        appendUtf8(out, c);
    }
    return out;
}

void putUtf16(ByteBuffer& out, char32_t unit, bool bigEndian)
{
    const auto hi = static_cast<std::byte>(unit >> 8);
    const auto lo = static_cast<std::byte>(unit & 0xFF);
    out.push_back(bigEndian ? hi : lo);
    out.push_back(bigEndian ? lo : hi);
}

bool startsWith(ByteSpan data, std::initializer_list<unsigned> prefix)
{
    if (data.size() < prefix.size())
        return false;
    std::size_t i = 0;
    for (unsigned b : prefix)
        if (byteAt(data, i++) != b)
            return false;
    return true;
}

} // namespace

std::string_view encodingName(TextEncoding encoding) noexcept
{
    switch (encoding) {
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::Utf8Bom: return "UTF-8 with BOM";
    case TextEncoding::Utf16LE: return "UTF-16 LE";
    case TextEncoding::Utf16BE: return "UTF-16 BE";
    case TextEncoding::Latin1: return "Latin-1";
    }
    return "?";
}

Result<TextDocument> decodeText(ByteSpan data)
{
    TextDocument doc;
    if (startsWith(data, {0xEF, 0xBB, 0xBF})) {
        doc.encoding = TextEncoding::Utf8Bom;
        const std::string_view body(reinterpret_cast<const char*>(data.data()) + 3, data.size() - 3);
        if (!validUtf8(body))
            return fail(ErrorCode::InvalidFormat, "text marked as UTF-8 contains invalid UTF-8");
        doc.text = body;
    } else if (startsWith(data, {0xFF, 0xFE}) || startsWith(data, {0xFE, 0xFF})) {
        const bool bigEndian = byteAt(data, 0) == 0xFE;
        doc.encoding = bigEndian ? TextEncoding::Utf16BE : TextEncoding::Utf16LE;
        if (data.size() % 2)
            return fail(ErrorCode::InvalidFormat, "UTF-16 text has an odd number of bytes");
        doc.text = decodeUtf16(data.subspan(2), bigEndian);
    } else {
        const std::string_view body(reinterpret_cast<const char*>(data.data()), data.size());
        if (validUtf8(body)) {
            doc.text = body;
        } else {
            doc.encoding = TextEncoding::Latin1;
            doc.text.reserve(body.size());
            for (unsigned char c : body)
                appendUtf8(doc.text, c);
        }
    }

    // Line endings: CRLF if most breaks are CRLF; those become "\n" here and
    // turn back into CRLF on save. (A file that is mostly LF keeps any stray
    // CRs untouched, so it round-trips byte for byte.)
    std::size_t crlf = 0, lf = 0;
    for (std::size_t i = 0; i < doc.text.size(); ++i)
        if (doc.text[i] == '\n')
            ++(i > 0 && doc.text[i - 1] == '\r' ? crlf : lf);
    if (crlf > 0 && crlf >= lf) {
        doc.lineEnding = LineEnding::CRLF;
        std::string text;
        text.reserve(doc.text.size());
        for (std::size_t i = 0; i < doc.text.size(); ++i)
            if (!(doc.text[i] == '\r' && i + 1 < doc.text.size() && doc.text[i + 1] == '\n'))
                text += doc.text[i];
        doc.text = std::move(text);
    }
    return doc;
}

Result<ByteBuffer> encodeText(const TextDocument& document)
{
    std::string text;
    if (document.lineEnding == LineEnding::CRLF) {
        text.reserve(document.text.size() + document.text.size() / 32);
        for (char c : document.text) {
            if (c == '\n')
                text += '\r';
            text += c;
        }
    } else {
        text = document.text;
    }

    ByteBuffer out;
    switch (document.encoding) {
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom:
        if (!validUtf8(text))
            return fail(ErrorCode::InvalidFormat, "the text isn't valid UTF-8");
        if (document.encoding == TextEncoding::Utf8Bom)
            out = {std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}};
        for (char c : text)
            out.push_back(static_cast<std::byte>(c));
        return out;
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
        const bool bigEndian = document.encoding == TextEncoding::Utf16BE;
        out.reserve(text.size() * 2 + 2);
        putUtf16(out, 0xFEFF, bigEndian);
        for (std::size_t i = 0; i < text.size();) {
            const auto c = nextUtf8(text, i);
            if (!c)
                return fail(ErrorCode::InvalidFormat, "the text isn't valid UTF-8");
            if (*c >= 0x10000) {
                putUtf16(out, 0xD800 + ((*c - 0x10000) >> 10), bigEndian);
                putUtf16(out, 0xDC00 + ((*c - 0x10000) & 0x3FF), bigEndian);
            } else {
                putUtf16(out, *c, bigEndian);
            }
        }
        return out;
    }
    case TextEncoding::Latin1:
        out.reserve(text.size());
        for (std::size_t i = 0, line = 1; i < text.size();) {
            const auto c = nextUtf8(text, i);
            if (!c)
                return fail(ErrorCode::InvalidFormat, "the text isn't valid UTF-8");
            if (*c > 0xFF)
                return fail(ErrorCode::Unsupported,
                            "line {} has a character (U+{:04X}) that Latin-1 text can't store", line,
                            static_cast<std::uint32_t>(*c));
            if (*c == '\n')
                ++line;
            out.push_back(static_cast<std::byte>(*c));
        }
        return out;
    }
    return fail(ErrorCode::Unsupported, "unknown text encoding");
}

std::string_view syntaxName(TextSyntax syntax) noexcept
{
    switch (syntax) {
    case TextSyntax::Plain: return "Plain text";
    case TextSyntax::Xml: return "XML";
    case TextSyntax::Ini: return "INI";
    case TextSyntax::Csv: return "CSV";
    case TextSyntax::Json: return "JSON";
    case TextSyntax::Lua: return "Lua";
    case TextSyntax::Shader: return "Shader / C";
    case TextSyntax::QuickBms: return "QuickBMS script";
    }
    return "?";
}

TextSyntax syntaxFor(const std::filesystem::path& name, std::string_view text)
{
    struct Rule {
        std::string_view extension;
        TextSyntax syntax;
    };
    static constexpr std::array rules{
        Rule{"xml", TextSyntax::Xml},     Rule{"html", TextSyntax::Xml},    Rule{"htm", TextSyntax::Xml},
        Rule{"xhtml", TextSyntax::Xml},   Rule{"svg", TextSyntax::Xml},     Rule{"xsd", TextSyntax::Xml},
        Rule{"xsl", TextSyntax::Xml},     Rule{"xslt", TextSyntax::Xml},    Rule{"plist", TextSyntax::Xml},
        Rule{"resx", TextSyntax::Xml},    Rule{"ini", TextSyntax::Ini},     Rule{"cfg", TextSyntax::Ini},
        Rule{"inf", TextSyntax::Ini},     Rule{"conf", TextSyntax::Ini},    Rule{"toml", TextSyntax::Ini},
        Rule{"csv", TextSyntax::Csv},     Rule{"tsv", TextSyntax::Csv},     Rule{"json", TextSyntax::Json},
        Rule{"lua", TextSyntax::Lua},     Rule{"fx", TextSyntax::Shader},   Rule{"fxh", TextSyntax::Shader},
        Rule{"hlsl", TextSyntax::Shader}, Rule{"hlsli", TextSyntax::Shader}, Rule{"glsl", TextSyntax::Shader},
        Rule{"vert", TextSyntax::Shader}, Rule{"frag", TextSyntax::Shader}, Rule{"vsh", TextSyntax::Shader},
        Rule{"psh", TextSyntax::Shader},  Rule{"shader", TextSyntax::Shader}, Rule{"c", TextSyntax::Shader},
        Rule{"h", TextSyntax::Shader},    Rule{"cpp", TextSyntax::Shader},  Rule{"hpp", TextSyntax::Shader},
        Rule{"bms", TextSyntax::QuickBms},
    };
    const std::string ext = extensionOf(name);
    for (const Rule& rule : rules)
        if (rule.extension == ext)
            return rule.syntax;

    // Unknown extension: look at the text itself.
    const std::size_t start = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (start == std::string_view::npos)
        return TextSyntax::Plain;
    const std::string_view head = text.substr(start, 4096);
    if (head.starts_with("<?xml") || head.starts_with("<!--") || (head[0] == '<' && head.find('>') != head.npos))
        return TextSyntax::Xml;
    if (head[0] == '{')
        return TextSyntax::Json;
    if (head[0] == '[' && head.find(']') < head.find('\n') && head.find('=') != head.npos)
        return TextSyntax::Ini;

    // Tables: every one of the first (complete) lines has commas between its fields.
    std::size_t lines = 0;
    for (std::size_t pos = 0; pos < head.size() && lines < 20;) {
        const std::size_t end = head.find('\n', pos);
        if (end == head.npos && lines > 0)
            break; // a cut-off last line doesn't count
        const std::string_view line = head.substr(pos, end == head.npos ? head.npos : end - pos);
        pos = end == head.npos ? head.size() : end + 1;
        if (line.find_first_not_of(" \t\r") == line.npos)
            continue;
        if (line.find(',') == line.npos)
            return TextSyntax::Plain;
        ++lines;
    }
    return lines >= 2 ? TextSyntax::Csv : TextSyntax::Plain;
}

} // namespace viewer
