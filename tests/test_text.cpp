// Text decoding/encoding (round trips) and colour-scheme detection.

#include "viewer/core/Text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace viewer;

namespace {

ByteBuffer bytes(std::string_view s)
{
    const auto span = asBytes(s);
    return ByteBuffer(span.begin(), span.end());
}

/// decode → encode gives back exactly the original bytes.
void checkRoundTrip(const ByteBuffer& original)
{
    auto doc = decodeText(original);
    REQUIRE(doc);
    auto again = encodeText(*doc);
    REQUIRE(again);
    CHECK(*again == original);
}

} // namespace

TEST_CASE("UTF-8 text with LF or CRLF line endings")
{
    auto lf = decodeText(bytes("a\nb\n"));
    REQUIRE(lf);
    CHECK(lf->encoding == TextEncoding::Utf8);
    CHECK(lf->lineEnding == LineEnding::LF);
    CHECK(lf->text == "a\nb\n");

    auto crlf = decodeText(bytes("[s]\r\nk=1\r\n"));
    REQUIRE(crlf);
    CHECK(crlf->lineEnding == LineEnding::CRLF);
    CHECK(crlf->text == "[s]\nk=1\n"); // the editor sees plain \n

    checkRoundTrip(bytes("a\nb\n"));
    checkRoundTrip(bytes("[s]\r\nk=1\r\n"));
    checkRoundTrip(bytes("caf\xC3\xA9 \xE2\x82\xAC\n")); // é €
    checkRoundTrip(bytes("\xEF\xBB\xBFwith BOM\r\n"));
    checkRoundTrip(bytes("mostly LF\nstray\r\nCR\n")); // the CR stays untouched
    checkRoundTrip({});
}

TEST_CASE("UTF-16 text (Blur's legal.txt style) round-trips")
{
    // FF FE, "Hi €", CRLF, and a character outside the BMP (surrogate pair).
    const ByteBuffer original = bytes(std::string("\xFF\xFE" "H\0i\0 \0\xAC\x20\r\0\n\0\x3D\xD8\x00\xDE", 18));
    auto doc = decodeText(original);
    REQUIRE(doc);
    CHECK(doc->encoding == TextEncoding::Utf16LE);
    CHECK(doc->lineEnding == LineEnding::CRLF);
    CHECK(doc->text == "Hi \xE2\x82\xAC\n\xF0\x9F\x98\x80");
    checkRoundTrip(original);

    const ByteBuffer bigEndian = bytes(std::string("\xFE\xFF\0A\0\n", 6));
    auto be = decodeText(bigEndian);
    REQUIRE(be);
    CHECK(be->encoding == TextEncoding::Utf16BE);
    CHECK(be->text == "A\n");
    checkRoundTrip(bigEndian);

    CHECK_FALSE(decodeText(bytes(std::string("\xFF\xFE" "A", 3)))); // odd length
}

TEST_CASE("Legacy 8-bit text falls back to Latin-1")
{
    const ByteBuffer original = bytes("na\xEFve \xA0 caf\xE9\r\n"); // not valid UTF-8
    auto doc = decodeText(original);
    REQUIRE(doc);
    CHECK(doc->encoding == TextEncoding::Latin1);
    CHECK(doc->text == "na\xC3\xAFve \xC2\xA0 caf\xC3\xA9\n");
    checkRoundTrip(original);

    // A character Latin-1 can't hold is refused, not silently dropped.
    doc->text += "\xE2\x82\xAC";
    CHECK_FALSE(encodeText(*doc));
}

TEST_CASE("Text can be re-saved in another encoding")
{
    TextDocument doc{"x\ny", TextEncoding::Utf16LE, LineEnding::CRLF};
    auto out = encodeText(doc);
    REQUIRE(out);
    CHECK(*out == bytes(std::string("\xFF\xFEx\0\r\0\n\0y\0", 10)));
    doc.encoding = TextEncoding::Utf8;
    doc.lineEnding = LineEnding::LF;
    CHECK(encodeText(doc).value() == bytes("x\ny"));
}

TEST_CASE("Colour schemes are picked by extension, then by content")
{
    CHECK(syntaxFor("data/ui.xml") == TextSyntax::Xml);
    CHECK(syntaxFor("Blur/Settings.INI") == TextSyntax::Ini);
    CHECK(syntaxFor("gamedata/events/am1_hard.csv") == TextSyntax::Csv);
    CHECK(syntaxFor("shaders/car.fx") == TextSyntax::Shader);
    CHECK(syntaxFor("blur.bms") == TextSyntax::QuickBms);
    CHECK(syntaxFor("readme.txt", "plain words") == TextSyntax::Plain);

    CHECK(syntaxFor("layout.dat", "<?xml version=\"1.0\"?>\n<a/>") == TextSyntax::Xml);
    CHECK(syntaxFor("x.cfg2", "[Video]\nWidth=1280\n") == TextSyntax::Ini);
    CHECK(syntaxFor("a.what", "{\"a\": 1}") == TextSyntax::Json);
    // Blur's datbanklists.wbl: comma lists with varying field counts.
    CHECK(syntaxFor("audio/datbanklists.wbl", "A.dat,nil,;\r\nB.dat,x.baf,y.baf,;\r\nC.dat,nil,;\r\n")
          == TextSyntax::Csv);
    CHECK(syntaxFor("notes.what", "one line, with a comma\nand one without\n") == TextSyntax::Plain);
}
