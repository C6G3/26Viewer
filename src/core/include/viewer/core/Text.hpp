#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace viewer {

/// How a text file's characters are stored on disk.
enum class TextEncoding {
    Utf8,    ///< UTF-8 (or plain ASCII) without a byte order mark.
    Utf8Bom, ///< UTF-8 starting with EF BB BF.
    Utf16LE, ///< UTF-16 little-endian with an FF FE mark (Windows "Unicode").
    Utf16BE, ///< UTF-16 big-endian with an FE FF mark.
    Latin1,  ///< Legacy 8-bit text that isn't valid UTF-8; every byte is one character.
};

enum class LineEnding { LF, CRLF };

[[nodiscard]] std::string_view encodingName(TextEncoding encoding) noexcept;

/// A decoded text file. `text` is UTF-8 with "\n" line breaks; `encoding` and
/// `lineEnding` remember the original layout so saving writes it back the same way.
struct TextDocument {
    std::string text;
    TextEncoding encoding = TextEncoding::Utf8;
    LineEnding lineEnding = LineEnding::LF;
};

/// Detects the encoding (byte order mark, else UTF-8, else Latin-1) and line
/// endings (CRLF when most breaks are CRLF), and decodes to UTF-8 with "\n".
[[nodiscard]] Result<TextDocument> decodeText(ByteSpan data);

/// The inverse of decodeText: converts the line breaks and characters back.
/// Fails if a character can't be stored in the encoding (e.g. "€" in Latin-1).
[[nodiscard]] Result<ByteBuffer> encodeText(const TextDocument& document);

/// Colour-coding schemes for the text viewer.
enum class TextSyntax {
    Plain,
    Xml,      ///< XML, HTML, SVG...
    Ini,      ///< [sections] and key=value
    Csv,      ///< comma-separated tables (columns coloured in turn)
    Json,
    Lua,
    Shader,   ///< C-like: HLSL, GLSL, C/C++
    QuickBms, ///< QuickBMS extraction scripts
};

[[nodiscard]] std::string_view syntaxName(TextSyntax syntax) noexcept;

/// Picks a scheme from the file's extension, falling back to a look at the
/// first characters (e.g. "<?xml") for unknown extensions.
[[nodiscard]] TextSyntax syntaxFor(const std::filesystem::path& name, std::string_view text = {});

} // namespace viewer
