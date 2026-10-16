// Blur's data tables ("Flask" packs: car handling, vehicles, events…) and text
// tables (.loc). Small ones are built here; with BLUR_DIR set, every table in
// gamedata.pak and gamedata_boot.pak is read too.

#include "TestUtil.hpp"

#include "FlaskTable.hpp"
#include "HashTool.hpp"
#include "Localisation.hpp"
#include "PakArchive.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

using namespace viewer;
using namespace viewer::games::blur;
using namespace std::string_literals;

namespace {

/// The inverse of unscrambleFlaskText (the game only reads).
ByteBuffer scramble(std::string_view text)
{
    ByteBuffer out;
    std::uint8_t key = 0xCC;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<std::uint8_t>(text[i]);
        out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(key - c) ^ 0xB3));
        key = static_cast<std::uint8_t>(key + c - static_cast<std::uint8_t>(i));
    }
    out.push_back(std::byte{0x55}); // the last byte is never read (it becomes the 0)
    return out;
}

/// A pack with one car: Car { name, mass, kind (enum), gears[] , tyre { grip } }.
ByteBuffer carPack(std::uint32_t version)
{
    const std::string names = "f32\0sz8\0Car\0name\0mass\0gears\0Drive\0Front\0Rear\0kind\0Tyre\0grip\0tyre\0"s;
    const auto at = [&](std::string_view n) { return static_cast<std::uint16_t>(names.find(std::string(n) + '\0')); };

    test::Writer types; // name, fields, bases, kind, value type, size
    const auto type = [&](std::string_view n, int fields, int kind, int primitive, int size) {
        for (int v : {static_cast<int>(at(n)), fields, 0, kind, primitive, size})
            types.put(static_cast<std::uint16_t>(v));
    };
    type("f32", 0, 0, 10, 4);  // 0
    type("sz8", 0, 0, 12, 4);  // 1
    type("Car", 5, 3, 0, 24);  // 2
    type("Drive", 2, 1, 0, 4); // 3
    type("Tyre", 1, 3, 0, 4);  // 4
    test::Writer fields; // name, type, offset, kind, array
    const auto field = [&](std::string_view n, int t, int offset, int kind, int array) {
        fields.put(at(n));
        fields.put(static_cast<std::uint16_t>(t));
        fields.put(static_cast<std::uint16_t>(offset));
        fields.put(static_cast<std::uint8_t>(kind));
        fields.put(static_cast<std::uint8_t>(array));
    };
    field("name", 1, 0, 0, 0);
    field("mass", 0, 4, 0, 0);
    field("gears", 0, 8, 0, 1);
    field("kind", 3, 16, 1, 0);
    field("tyre", 4, 20, 3, 0);
    field("Front", 0xFFFF, 0, 6, 0);
    field("Rear", 0xFFFF, 0, 6, 0);
    field("grip", 0, 0, 0, 0);

    test::Writer objects; // Car, then the gear ratios
    objects.put(std::uint32_t{0});                                  // name: text 0
    objects.put(std::bit_cast<std::uint32_t>(1200.0f));             // mass
    objects.put(std::uint16_t{1});                                  // gears: layout 1,
    objects.put(std::uint16_t{0});                                  //   from element 0,
    objects.put(std::uint32_t{3});                                  //   3 of them
    objects.put(std::uint32_t{1});                                  // kind: Rear
    objects.put(std::bit_cast<std::uint32_t>(1.25f));               // tyre.grip
    for (float g : {2.5f, 1.5f, 0.75f})
        objects.put(std::bit_cast<std::uint32_t>(g));
    const std::string carName = "Test_Car\0"s;
    test::Writer texts;
    if (version == 2)
        texts.put(scramble(carName.substr(0, 8)));
    else
        texts.put(carName);

    test::Writer pack;
    pack.put("KSLF");
    pack.put(version);
    const std::size_t sections = pack.bytes.size();
    for (int i = 0; i < 16; ++i)
        pack.put(std::uint32_t{0});
    std::size_t section = 0;
    const auto add = [&](ByteSpan bytes, std::size_t count) {
        pack.patch(sections + 8 * section, static_cast<std::uint32_t>(pack.bytes.size()));
        pack.patch(sections + 8 * section + 4, static_cast<std::uint32_t>(count));
        pack.put(bytes);
        ++section;
    };
    add(types.bytes, 5);
    add({}, 0); // bases
    add(fields.bytes, 8);
    const ByteBuffer nameBytes = version == 2 ? scramble(names.substr(0, names.size() - 1))
                                              : ByteBuffer(reinterpret_cast<const std::byte*>(names.data()),
                                                           reinterpret_cast<const std::byte*>(names.data()) + names.size());
    add(nameBytes, nameBytes.size());
    test::Writer records;
    records.put(std::uint32_t{0x12345678});
    records.put(std::uint16_t{2});
    records.put(std::uint16_t{0});
    add(records.bytes, 1);
    test::Writer blocks;
    for (std::uint32_t v : {0u, 2u, static_cast<std::uint32_t>(objects.bytes.size()), static_cast<std::uint32_t>(texts.bytes.size())})
        blocks.put(v);
    add(blocks.bytes, 1);
    test::Writer layouts;
    for (int v : {2, 1, 0, 0, 3, 0})
        layouts.put(static_cast<std::uint16_t>(v));
    add(layouts.bytes, 2);
    test::Writer data;
    data.put(objects.bytes);
    data.put(texts.bytes);
    add(data.bytes, data.bytes.size());
    return pack.bytes;
}

} // namespace

TEST_CASE("Blur data tables: unscrambling undoes the game's scrambling")
{
    const std::string text = "RP_CarDynamics\0mass\0gearRatios"s;
    const ByteBuffer scrambled = scramble(text);
    const std::string back = unscrambleFlaskText(scrambled);
    CHECK(back.substr(0, text.size()) == text);
    CHECK(back.back() == '\0');
}

TEST_CASE("Blur data tables: records, values, arrays, enums and structs as text")
{
    for (std::uint32_t version : {1u, 2u}) {
        INFO("version " << version);
        const ByteBuffer data = carPack(version);
        const FlaskDecompiler decompiler;
        REQUIRE(decompiler.canDecompile("bin", data));
        auto pack = readFlask(data);
        REQUIRE(pack);
        CHECK(pack->types.size() == 5);
        CHECK(pack->types[2].name == "Car");
        CHECK(pack->fields[2].name == "gears");
        REQUIRE(pack->blocks.size() == 1);
        CHECK(pack->blocks[0].layouts[1].start == 24);

        auto text = decompiler.decompile(data);
        REQUIRE(text);
        INFO(*text);
        CHECK(text->find("Car \"Test_Car\" {  // record 0x12345678") != std::string::npos);
        CHECK(text->find("    mass = 1200\n") != std::string::npos);
        CHECK(text->find("    gears = [2.5, 1.5, 0.75]\n") != std::string::npos);
        CHECK(text->find("    kind = Rear\n") != std::string::npos);
        CHECK(text->find("    tyre = Tyre { grip = 1.25 }\n") != std::string::npos);
    }
}

namespace {

std::string replaced(std::string text, std::string_view from, std::string_view to)
{
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

} // namespace

TEST_CASE("Blur data tables: edited values are written back")
{
    for (std::uint32_t version : {1u, 2u}) {
        INFO("version " << version);
        const ByteBuffer data = carPack(version);
        const FlaskDecompiler decompiler;
        REQUIRE(decompiler.canRecompile());
        const std::string text = decompiler.decompile(data).value();

        // Unchanged text: the very same file.
        auto same = decompiler.recompile(data, text, {});
        REQUIRE(same);
        CHECK(*same == data);
        // Comments and spacing don't matter either.
        same = decompiler.recompile(data, "// my notes\n" + replaced(text, "mass = 1200", "mass   =  1200   // heavy"), {});
        REQUIRE(same);
        CHECK(*same == data);

        std::string edited = replaced(text, "mass = 1200", "mass = 1350.5");
        edited = replaced(edited, "[2.5, 1.5, 0.75]", "[2.5, 1.25, 0.75]");
        edited = replaced(edited, "kind = Rear", "kind = Front");
        edited = replaced(edited, "name = \"Test_Car\"", "name = \"Tuned \\\"Car\\\"\"");
        edited = replaced(edited, "grip = 1.25", "grip = 2");
        auto rebuilt = decompiler.recompile(data, edited, {});
        REQUIRE(rebuilt);
        auto again = decompiler.decompile(*rebuilt);
        REQUIRE(again);
        INFO(*again);
        CHECK(again->find("Car \"Tuned \\\"Car\\\"\" {") != std::string::npos);
        CHECK(again->find("    mass = 1350.5\n") != std::string::npos);
        CHECK(again->find("    gears = [2.5, 1.25, 0.75]\n") != std::string::npos);
        CHECK(again->find("    kind = Front\n") != std::string::npos);
        CHECK(again->find("    tyre = Tyre { grip = 2 }\n") != std::string::npos);
        CHECK(*again == replaced(edited, "Car \"Test_Car\"", "Car \"Tuned \\\"Car\\\"\""));

        // Only values may change; mistakes name their line.
        auto wrong = decompiler.recompile(data, replaced(text, "mass = 1200", "mass = heavy"), {});
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().message.starts_with("line "));
        CHECK(wrong.error().message.find("isn't a number") != std::string::npos);
        wrong = decompiler.recompile(data, replaced(text, "kind = Rear", "kind = Middle"), {});
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().message.find("Front, Rear") != std::string::npos);
        wrong = decompiler.recompile(data, replaced(text, "tyre = Tyre { grip = 1.25 }", "tyre = Tyre { grip = 1.25, wear = 2 }"), {});
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().message.find("Only values can be changed") != std::string::npos);
        wrong = decompiler.recompile(data, replaced(text, "    mass = 1200\n", ""), {});
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().message.starts_with("line 5:"));
    }
}

TEST_CASE("Blur data tables: lists get more or fewer items")
{
    for (std::uint32_t version : {1u, 2u}) {
        INFO("version " << version);
        const ByteBuffer data = carPack(version);
        const FlaskDecompiler decompiler;
        const std::string text = decompiler.decompile(data).value();

        // Two more gears: copies of the last one, then given their own values.
        std::string edited = replaced(text, "[2.5, 1.5, 0.75]", "[2.5, 1.5, 0.75, 0.6, 0.5]");
        auto rebuilt = decompiler.recompile(data, edited, {});
        REQUIRE(rebuilt);
        CHECK(decompiler.decompile(*rebuilt).value() == edited);
        // Back to one; the texts and other values are untouched.
        edited = replaced(replaced(text, "[2.5, 1.5, 0.75]", "[3]"), "mass = 1200", "mass = 999");
        rebuilt = decompiler.recompile(data, edited, {});
        REQUIRE(rebuilt);
        CHECK(decompiler.decompile(*rebuilt).value() == edited);
        // None at all, and from none to some again.
        edited = replaced(text, "[2.5, 1.5, 0.75]", "[]");
        rebuilt = decompiler.recompile(data, edited, {});
        REQUIRE(rebuilt);
        const std::string empty = decompiler.decompile(*rebuilt).value();
        CHECK(empty == edited);
        rebuilt = decompiler.recompile(*rebuilt, text, {});
        REQUIRE(rebuilt);
        CHECK(decompiler.decompile(*rebuilt).value() == text);
    }
}

TEST_CASE("Blur data tables: damaged packs are refused")
{
    ByteBuffer data = carPack(1);
    data[0] = std::byte{'X'};
    CHECK_FALSE(readFlask(data));
    data = carPack(1);
    data.resize(0x60); // sections now point past the end
    CHECK_FALSE(readFlask(data));
    data = carPack(1);
    data[4] = std::byte{7}; // version
    CHECK_FALSE(readFlask(data));
}

TEST_CASE("Blur text tables: keys and translations; ids found by data tables")
{
    // Two languages, one text.
    test::Writer w;
    w.put("OLTX");
    w.put(std::uint32_t{1});
    w.put(std::uint32_t{16});
    w.put(std::uint32_t{0}); // patched below
    w.put(std::uint32_t{12});
    w.put(std::uint32_t{2});
    w.put(std::uint32_t{0x1C + 2 * 24 - 16});
    const std::size_t languages = w.bytes.size();
    for (std::string_view code : {"en", "fr"}) {
        w.put(code);
        for (std::size_t i = code.size(); i < 16; ++i)
            w.put(std::uint8_t{0});
        w.put(std::uint32_t{0});
        w.put(std::uint32_t{0});
    }
    w.put(std::uint32_t{1}); // keys
    w.put(std::uint32_t{8});
    w.put(std::uint32_t{0xABCD});
    w.put(std::uint32_t{16});
    w.put("CAR_NAME", true);
    w.padTo(4);
    for (int l = 0; l < 2; ++l) {
        const std::size_t start = w.bytes.size();
        w.patch(languages + 24 * l + 16, static_cast<std::uint32_t>(start));
        w.put(std::uint32_t{1});
        w.put(std::uint32_t{8});
        w.put(std::uint32_t{0xABCD});
        w.put(std::uint32_t{16});
        for (char16_t c : l == 0 ? std::u16string(u"Café") : std::u16string(u"Voiture"))
            w.put(static_cast<std::uint16_t>(c));
        w.put(std::uint16_t{0});
        w.patch(languages + 24 * l + 20, static_cast<std::uint32_t>(w.bytes.size() - start));
    }
    auto table = readLocTable(w.bytes);
    REQUIRE(table);
    REQUIRE(table->languages == std::vector<std::string>{"en", "fr"});
    REQUIRE(table->strings.size() == 1);
    CHECK(table->strings[0].key == "CAR_NAME");
    CHECK(table->strings[0].texts[0] == "Caf\xC3\xA9");
    CHECK(table->strings[0].texts[1] == "Voiture");
    auto text = LocTableDecompiler().decompile(w.bytes);
    REQUIRE(text);
    CHECK(text->find("CAR_NAME  // id 0x0000abcd\n    en \"Caf\xC3\xA9\"\n    fr \"Voiture\"") != std::string::npos);
    const auto english = englishTexts({{"cars.loc", w.bytes}});
    CHECK(english.at(0xABCD) == "Caf\xC3\xA9");

    // Edited texts are written back, longer or shorter.
    const LocTableDecompiler loc;
    REQUIRE(loc.canRecompile());
    std::string edited = *text;
    edited.replace(edited.find("\"Voiture\""), 9, "\"Une voiture tr\xC3\xA8s rapide \\\"GT\\\"\"");
    edited.replace(edited.find("\"Caf\xC3\xA9\""), 7, "\"\xF0\x9F\x9A\x97\"");
    auto rebuilt = loc.recompile(w.bytes, edited, {});
    REQUIRE(rebuilt);
    auto back = readLocTable(*rebuilt);
    REQUIRE(back);
    CHECK(back->strings[0].key == "CAR_NAME");
    CHECK(back->strings[0].texts[0] == "\xF0\x9F\x9A\x97"); // outside the BMP: a surrogate pair
    CHECK(back->strings[0].texts[1] == "Une voiture tr\xC3\xA8s rapide \"GT\"");
    // And back again: the very same bytes as at first.
    auto restored = loc.recompile(*rebuilt, *text, {});
    REQUIRE(restored);
    CHECK(*restored == w.bytes);

    // A new text: a name line without an id, then its texts. It goes in name
    // order (the game binary-searches names), with an id nothing else uses; a
    // language left out takes the first one's text.
    auto added = loc.recompile(w.bytes, *text + "\nAAA_NEW_CAR\n    en \"My Car\"\n", {});
    REQUIRE(added);
    auto withNew = readLocTable(*added);
    REQUIRE(withNew);
    REQUIRE(withNew->strings.size() == 2);
    const std::uint32_t newId = newTextId("AAA_NEW_CAR", {0xABCD});
    CHECK(newId != 0xABCD);
    CHECK(withNew->strings[0].key == "AAA_NEW_CAR"); // sorted before CAR_NAME
    CHECK(withNew->strings[0].id == newId);
    CHECK(withNew->strings[0].texts == std::vector<std::string>{"My Car", "My Car"});
    CHECK(withNew->strings[1].key == "CAR_NAME");
    CHECK(withNew->strings[1].texts[1] == "Voiture");
    // Its id shows in the listing, and editing that listing keeps it.
    const std::string newText = loc.decompile(*added).value();
    CHECK(newText.find(std::format("AAA_NEW_CAR  // id 0x{:08x}", newId)) != std::string::npos);
    CHECK(loc.recompile(*added, newText, {}).value() == *added);
    // Same name in another table: the same text, so the same id.
    DecompileContext elsewhere;
    elsewhere.readFolder = [&](std::string_view) {
        return std::vector<DecompileContext::File>{{"other.loc", *added}};
    };
    auto shared = loc.recompile(w.bytes, *text + "\nAAA_NEW_CAR\n    en \"Mine\"\n", elsewhere);
    REQUIRE(shared);
    CHECK(readLocTable(*shared)->strings[0].id == newId);

    // Refused: removing a text, renaming one, bad or repeated names, unknown languages.
    const auto refused = [&](const std::string& edit, std::string_view why) {
        INFO(edit);
        auto r = loc.recompile(w.bytes, edit, {});
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find(why) != std::string::npos);
    };
    refused("// Blur text table\n\nNEW_ONE\n    en \"x\"\n", "can't be removed");
    refused(*text + "\nBAD NAME\n    en \"x\"\n", "can't name a text");
    refused(*text + "\nCAR_NAME\n    en \"x\"\n", "already has a text called CAR_NAME");
    refused(*text + "\nOTHER\n    xx \"x\"\n", "no language \"xx\"");
    std::string renamed = *text + "\nOTHER\n    en \"x\"\n";
    renamed.replace(renamed.find("CAR_NAME"), 8, "CAR_TITLE");
    refused(renamed, "can't change");
}

TEST_CASE("Blur text tables: written the way the game stores them")
{
    LocTable table;
    table.languages = {"en", "fr"};
    table.strings = {{0x30, "ZED", {"z", "zz"}}, {0x10, "ALPHA", {"a", "aa"}}, {0x20, "", {"no name", "sans nom"}}};
    const ByteBuffer bytes = writeLocTable(table);
    auto back = readLocTable(bytes);
    REQUIRE(back);
    // The name table in name order, the language tables in id order.
    REQUIRE(back->strings.size() == 3);
    CHECK(back->strings[0].key == "ALPHA");
    CHECK(back->strings[1].key == "ZED");
    CHECK(back->strings[2].key.empty()); // only in the language tables
    CHECK(back->strings[2].texts[1] == "sans nom");
    CHECK(loadScalar<std::uint32_t>(bytes, 0x1C + 16) == 0x1C + 2 * 24 + 8 + 2 * 8 + 6 + 4);
    CHECK(isTextName("VEH_MOD_VIPERACR"));
    CHECK_FALSE(isTextName("Viper ACR"));
    CHECK_FALSE(isTextName(""));
}

TEST_CASE("Real Blur data tables (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    for (const char* name : {"gamedata.pak", "gamedata_boot.pak"}) {
        auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / name);
        REQUIRE(pak);
        const auto entries = (*pak)->entries();
        // The decompilers' view of the folders around a file: this archive.
        DecompileContext context;
        context.readFolder = [&](std::string_view folder) {
            std::vector<DecompileContext::File> files;
            const std::string prefix = std::string(folder) + "/";
            for (std::size_t i = 0; i < entries.size(); ++i)
                if (entries[i].path.starts_with(prefix) && entries[i].path.find('/', prefix.size()) == std::string::npos)
                    files.push_back({entries[i].path.substr(prefix.size()), (*pak)->read(i).value()});
            return files;
        };
        std::size_t tables = 0, texts = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const std::string& path = entries[i].path;
            const bool isLoc = path.ends_with(".loc");
            if (!isLoc && !path.ends_with(".bin") && !path.ends_with(".xt"))
                continue;
            INFO(name << "/" << path);
            const ByteBuffer data = (*pak)->read(i).value();
            if (isLoc) {
                ++texts;
                auto table = readLocTable(data);
                REQUIRE(table);
                CHECK(table->languages.size() == 5);
                CHECK(writeLocTable(*table) == data); // the game's own layout, byte for byte
                if (path.ends_with("vehicles.loc")) {
                    const LocTableDecompiler loc;
                    const std::string text = loc.decompile(data).value();
                    std::string edited = text;
                    const std::size_t at = edited.find("\"Legendary Camaro SS\"");
                    REQUIRE(at != std::string::npos);
                    edited.replace(at, 21, "\"My Camaro\"");
                    auto rebuilt = loc.recompile(data, edited, {});
                    REQUIRE(rebuilt);
                    CHECK(englishTexts({{"vehicles.loc", *rebuilt}}).at(33163972) == "My Camaro");
                    auto restored = loc.recompile(*rebuilt, text, {});
                    REQUIRE(restored);
                    CHECK(*restored == data);
                }
                continue;
            }
            ++tables;
            const FlaskDecompiler decompiler;
            REQUIRE(decompiler.canDecompile("bin", data));
            auto pack = readFlask(data);
            REQUIRE(pack);
            if (!path.ends_with("racingphysics.bin") && !path.ends_with("finalracebook.bin"))
                continue; // the big ones are slow in a debug build; they're decoded in the app and CLI
            auto text = decompiler.decompileWith(data, context);
            REQUIRE(text);
            CHECK(text->find("damaged") == std::string::npos);
            CHECK(text->find("out of range") == std::string::npos);
            if (path.ends_with("racingphysics.bin")) {
                CHECK(text->find("RP_CarDynamics \"Audi_S3_Stock\" {") != std::string::npos);
                CHECK(text->find("        mass = 1400\n") != std::string::npos);
                CHECK(text->find("gearRatios = [") != std::string::npos);
                CHECK(text->find("differentialType = Viscous") != std::string::npos);
            } else {
                CHECK(text->find("/* \"Carver's Viper GTSR\" */") != std::string::npos);
                CHECK(text->find("Vehicle = -> AmaxVehicle \"Dodge_ViperGTSR_Stock_Carver\"") != std::string::npos);
            }

            // Edit and write back: unchanged text gives the same file; edits read back.
            auto same = decompiler.recompile(data, *text, context);
            REQUIRE(same);
            CHECK(*same == data);
            std::string edited = *text;
            const auto edit = [&](std::string_view after, std::string_view from, std::string_view to) {
                const std::size_t at = edited.find(from, edited.find(after));
                REQUIRE(at != std::string::npos);
                edited.replace(at, from.size(), to);
            };
            std::vector<std::string> expected;
            if (path.ends_with("racingphysics.bin")) {
                edit("RP_CarDynamics \"Audi_S3_Stock\"", "mass = 1400", "mass = 1525.5");
                edit("RP_CarDynamics \"Audi_S3_Stock\"", "driveType = ", "driveType = FourWheel // was: ");
                expected = {"mass = 1525.5", "driveType = FourWheel"};
            } else {
                edit("RaceBookVehicle \"Carvers_Viper_GTSR\"", "Cost = 12000", "Cost = 500");
                edit("RaceBookVehicle \"Carvers_Viper_GTSR\"", "Name = \"Carvers_Viper_GTSR\"",
                     "Name = \"Carvers_Viper_GTSR_Tuned\"");
                edit("RaceBookVehicle \"Carvers_Viper_GTSR\"", "ForSale = false", "ForSale = true");
                edit("RaceBookVehicle \"Carvers_Viper_GTSR\"", "Vehicle = -> AmaxVehicle \"Dodge_ViperGTSR_Stock_Carver\"",
                     "Vehicle = -> AmaxVehicle \"Audi_S3_Stock\"");
                expected = {"RaceBookVehicle \"Carvers_Viper_GTSR_Tuned\" {", "Cost = 500", "ForSale = true",
                            "Vehicle = -> AmaxVehicle \"Audi_S3_Stock\""};
            }
            auto rebuilt = decompiler.recompile(data, edited, context);
            REQUIRE(rebuilt);
            auto again = decompiler.decompileWith(*rebuilt, context);
            REQUIRE(again);
            for (const std::string& e : expected) {
                INFO(e);
                CHECK(again->find(e) != std::string::npos);
            }
            CHECK(again->size() > text->size() - 64);
        }
        CHECK(tables >= 25);
        CHECK(texts >= 21);
        if (std::string_view(name) != "gamedata.pak")
            continue;

        // Text ids shown by name, and a name typed in place of an id.
        const FlaskDecompiler decompiler;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].path.ends_with("xt/finalracebook.bin"))
                continue;
            const ByteBuffer data = (*pak)->read(i).value();
            const std::string text = decompiler.decompileWith(data, context).value();
            const std::size_t at = text.find(" /* \"Carver's Viper GTSR\" */");
            REQUIRE(at != std::string::npos);
            const std::size_t equals = text.rfind(" = ", at);
            const std::string shown = text.substr(equals + 3, at - equals - 3);
            INFO(shown);
            CHECK(isTextName(shown));
            CHECK_FALSE(shown.starts_with("0x"));
            std::string edited = text;
            edited.replace(equals + 3, shown.size(), "VEH_MOD_VIPERACR");
            const std::string again = decompiler.decompileWith(decompiler.recompile(data, edited, context).value(),
                                                               context).value();
            CHECK(again.find("= VEH_MOD_VIPERACR /* \"Viper ACR\" */") != std::string::npos);
            edited.replace(equals + 3, 16, "NOT_A_TEXT_NAME");
            CHECK_FALSE(decompiler.recompile(data, edited, context));
        }

        // The hash tool: names <-> numbers, both ways.
        HashToolContext hashes;
        hashes.readFolder = context.readFolder;
        hashes.archivePaths = [&] {
            std::vector<std::string> paths;
            for (const ArchiveEntry& e : entries)
                paths.push_back(e.path);
            return paths;
        };
        const BlurHashTool tool;
        const auto has = [&](std::string_view query, std::string_view kind, std::string_view value) {
            INFO(query);
            const auto found = tool.lookup(query, hashes);
            CHECK(std::ranges::any_of(found, [&](const HashMatch& m) { return m.kind == kind && m.value == value; }));
        };
        has("VEH_MOD_VIPERACR", "Text id", "0xd2266d99");
        has("0xd2266d99", "Text", "VEH_MOD_VIPERACR");
        has("d2266d99", "Text", "VEH_MOD_VIPERACR");
        has("viper acr", "Text with this English", "0xd2266d99");
        has("gamedata/xt/amax.bin",
            "Archive path hash", std::format("0x{:08x}", PakArchive::nameHash("\\gamedata\\xt\\amax.bin")));
        has(std::format("0x{:08x}", PakArchive::nameHash("\\gamedata\\xt\\amax.bin")), "Archive path",
            "gamedata/xt/amax.bin");
        has("MY_BRAND_NEW_TEXT", "New text id", std::format("0x{:08x}", newTextId("MY_BRAND_NEW_TEXT", {})));
    }
}

namespace {

/// The multi-line lists of a decompiled table: where the "[" line starts, its
/// indentation, and each item's first line and the closing line.
struct TextList {
    std::size_t indent = 0;
    std::vector<std::size_t> items; ///< Offsets of each item's first line.
    std::size_t close = 0;          ///< Offset of the "]" line.
};

std::vector<TextList> multiLineLists(const std::string& text, std::size_t limit)
{
    std::vector<TextList> lists;
    for (std::size_t at = text.find("= [\n"); at != std::string::npos && lists.size() < limit;
         at = text.find("= [\n", at + 1)) {
        const std::size_t lineStart = text.rfind('\n', at) + 1;
        TextList list;
        list.indent = text.find_first_not_of(' ', lineStart) - lineStart;
        const std::string itemIndent(list.indent + 4, ' '), closing = std::string(list.indent, ' ') + "]";
        for (std::size_t line = at + 4; line < text.size(); line = text.find('\n', line) + 1) {
            if (text.compare(line, closing.size() + 1, closing + "\n") == 0) {
                list.close = line;
                break;
            }
            const char next = line + itemIndent.size() < text.size() ? text[line + itemIndent.size()] : ' ';
            if (text.compare(line, itemIndent.size(), itemIndent) == 0 && next != ' ' && next != '}' && next != ']')
                list.items.push_back(line);
        }
        if (list.close && !list.items.empty())
            lists.push_back(std::move(list));
    }
    return lists;
}

} // namespace

TEST_CASE("Real Blur data tables: lists edited (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "gamedata.pak");
    REQUIRE(pak);
    const auto entries = (*pak)->entries();
    for (const char* name : {"gamedata/xt/racingphysics.bin", "gamedata/xt/amax.bin", "gamedata/xt/pickups.bin"}) {
        INFO(name);
        const auto entry = std::ranges::find_if(entries, [&](const auto& e) { return e.path == name; });
        REQUIRE(entry != entries.end());
        const ByteBuffer data = (*pak)->read(static_cast<std::size_t>(entry - entries.begin())).value();
        const FlaskDecompiler decompiler;
        const std::string text = decompiler.decompile(data).value();

        // Lists from the end of the file: copy the last item of one, remove the
        // last item of the next, and so on. The rebuilt table reads back as
        // edited (nothing left to change), and the original text restores it.
        const std::vector<TextList> lists = multiLineLists(text, 4000);
        REQUIRE(lists.size() >= 4);
        std::size_t added = 0, removed = 0, kept = 0;
        std::size_t last = std::string::npos;
        std::string combined = text; // the successful edits together (far apart, from the end)
        for (std::size_t i = lists.size(); i-- > 0 && added + removed + kept < 6;) {
            const TextList& l = lists[i];
            if (last != std::string::npos && l.close + 20000 > last)
                continue;
            const bool add = (added + removed + kept) % 2 == 0;
            if (!add && l.items.size() < 2)
                continue;
            last = l.items.front();
            const std::size_t from = l.items.back();
            std::string edited = text;
            if (add)
                edited.insert(l.close, text.substr(from, l.close - from));
            else
                edited.erase(from, l.close - from);
            INFO((add ? "copying the last item of the list ending at " : "removing the last item of the list ending at ")
                 << l.close);
            auto rebuilt = decompiler.recompile(data, edited, {});
            if (!rebuilt) {
                // Only removing items others point at may be refused.
                INFO(rebuilt.error().message);
                CHECK_FALSE(add);
                CHECK(rebuilt.error().message.find("can't get shorter") != std::string::npos);
                ++kept;
                continue;
            }
            ++(add ? added : removed);
            auto settled = decompiler.recompile(*rebuilt, edited, {});
            REQUIRE(settled);
            CHECK(*settled == *rebuilt);
            if (add)
                combined.insert(l.close, text.substr(from, l.close - from));
            else
                combined.erase(from, l.close - from);
        }
        CHECK(added >= 2);
        // All of them at once (several rounds), and back again.
        auto rebuilt = decompiler.recompile(data, combined, {});
        if (!rebuilt)
            FAIL(rebuilt.error().message);
        auto settled = decompiler.recompile(*rebuilt, combined, {});
        REQUIRE(settled);
        CHECK(*settled == *rebuilt);
        auto restored = decompiler.recompile(*rebuilt, text, {});
        if (!restored)
            FAIL(restored.error().message);
        CHECK(decompiler.decompile(*restored).value() == text);
    }
}
