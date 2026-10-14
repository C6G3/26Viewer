// Builds synthetic Blur PAK files in memory and reads them back, so the
// format logic is tested without needing the game installed. When the
// BLUR_DIR environment variable points at an install, real archives are
// checked too.

#include "TestUtil.hpp"

#include "PakArchive.hpp"
#include "viewer/core/Compression.hpp"
#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/RandomAccessFile.hpp"
#include "viewer/core/Vfs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>

using namespace viewer;
using viewer::games::blur::PakArchive;

namespace {

enum class Storage { Stored, Zlib, Chunked, ChunkedWithStoredTail };

struct FileSpec {
    std::string name;
    ByteBuffer data;
    Storage storage;
};

constexpr std::array<std::uint8_t, 14> kKey{'V', 'X', 'o', '4', '0', 'j', '3', '@', '$', '%', '\\', '%', '`', 'x'};
constexpr std::uint32_t kAlign = 0x100;
constexpr std::uint32_t kChunk = 64; // small chunks so tests exercise several

ByteBuffer encode(const FileSpec& f, int version)
{
    test::Writer w;
    if (version == 1) {
        const auto z = test::deflate(f.data, true);
        w.put<std::uint32_t>(static_cast<std::uint32_t>(z.size()));
        w.put(z);
        return w.bytes;
    }
    switch (f.storage) {
    case Storage::Stored: return f.data;
    case Storage::Zlib: return test::deflate(f.data, true);
    case Storage::Chunked:
    case Storage::ChunkedWithStoredTail: {
        w.put<std::uint32_t>(kChunk);
        std::size_t pos = 0;
        while (pos < f.data.size()) {
            const std::size_t n = std::min<std::size_t>(kChunk, f.data.size() - pos);
            const ByteSpan piece(f.data.data() + pos, n);
            const bool last = pos + n == f.data.size();
            if (last && f.storage == Storage::ChunkedWithStoredTail) {
                w.put<std::uint32_t>(static_cast<std::uint32_t>(n)); // stored: zsize == size
                w.put(piece);
            } else {
                const auto z = test::deflate(piece, false);
                w.put<std::uint32_t>(static_cast<std::uint32_t>(z.size()));
                w.put(z);
            }
            pos += n;
        }
        return w.bytes;
    }
    }
    return {};
}

ByteBuffer buildPak(const std::vector<FileSpec>& files, int version, std::uint32_t stride)
{
    // Name table: "" then "\dir" then each "\dir\name".
    test::Writer names;
    names.put("", true);
    const auto dirOffset = static_cast<std::uint32_t>(names.bytes.size());
    names.put("\\dir", true);
    std::vector<std::uint32_t> nameOffsets;
    for (const auto& f : files) {
        nameOffsets.push_back(static_cast<std::uint32_t>(names.bytes.size()));
        names.put("\\dir\\" + f.name, true);
    }
    for (std::size_t i = 0; i < names.bytes.size(); ++i)
        names.bytes[i] ^= std::byte{kKey[i % kKey.size()]};

    test::Writer pak;
    pak.put("2KAP");
    pak.put<std::uint32_t>(static_cast<std::uint32_t>(version));
    pak.put<std::uint32_t>(kAlign);
    pak.put<std::uint32_t>(version == 1 ? 0x28 : 0x20);
    pak.padTo(0x40);
    pak.patch<std::uint16_t>(version == 1 ? 0x24 : 0x10, 0x0004);
    pak.patch<std::uint32_t>(0x38, static_cast<std::uint32_t>(names.bytes.size()));
    pak.bytes.resize(0x3C);
    pak.put(names.bytes);
    pak.padTo(kAlign);

    const auto count = static_cast<std::uint32_t>(files.size() + 1); // + directory record
    const std::uint64_t tableOffset = pak.bytes.size();
    std::vector<ByteBuffer> blobs;
    for (const auto& f : files)
        blobs.push_back(encode(f, version));

    // Data offsets (in alignment units) after the table.
    std::uint64_t cursor = (tableOffset + 8 + std::uint64_t{count} * stride + kAlign - 1) / kAlign * kAlign;
    std::vector<std::uint64_t> offsets;
    for (const auto& b : blobs) {
        offsets.push_back(cursor / kAlign);
        cursor = (cursor + b.size() + kAlign - 1) / kAlign * kAlign;
    }

    pak.put<std::uint32_t>(count);
    pak.put<std::uint32_t>(count * stride);
    auto entry = [&](std::uint32_t nameOff, std::uint32_t size, std::int64_t offset, std::uint32_t stored) {
        const std::size_t start = pak.bytes.size();
        pak.put<std::uint32_t>(nameOff * 2654435761u); // fake name hash (the reader doesn't check it)
        pak.put<std::uint32_t>(nameOff);
        pak.put<std::uint32_t>(dirOffset);
        pak.put<std::uint32_t>(size);
        pak.put<std::uint64_t>(129169728000000000ULL);
        pak.put<std::int64_t>(offset);
        if (version >= 2) {
            pak.put<std::uint32_t>(0xDEADBEEF);
            pak.put<std::uint32_t>(stored);
        }
        while (pak.bytes.size() < start + stride)
            pak.bytes.push_back(std::byte{0});
    };
    entry(dirOffset, 0, -1, 0);
    for (std::size_t i = 0; i < files.size(); ++i)
        entry(nameOffsets[i], static_cast<std::uint32_t>(files[i].data.size()), static_cast<std::int64_t>(offsets[i]),
              static_cast<std::uint32_t>(blobs[i].size()));

    for (std::size_t i = 0; i < blobs.size(); ++i) {
        pak.bytes.resize(offsets[i] * kAlign);
        pak.put(blobs[i]);
    }
    pak.padTo(kAlign);
    return pak.bytes;
}

std::vector<FileSpec> sampleFiles()
{
    std::vector<FileSpec> files{
        {"stored.bin", test::sampleData(300, 1), Storage::Stored},
        {"zlib.txt", test::sampleData(5000, 2), Storage::Zlib},
        {"chunked.model", test::sampleData(1000, 3), Storage::Chunked},
        {"tail.texture", test::sampleData(64 + 4, 4), Storage::ChunkedWithStoredTail},
    };
    // A last chunk of bytes that are not valid deflate, stored verbatim.
    auto& tail = files.back().data;
    std::fill(tail.end() - 4, tail.end(), std::byte{0xFF});
    return files;
}

std::string property(const PropertyList& props, std::string_view name)
{
    for (const auto& p : props)
        if (p.name == name)
            return p.value;
    return {};
}

std::string hex32(std::uint32_t v)
{
    return std::format("0x{:08X}", v);
}

void checkArchive(const ByteBuffer& bytes, const std::vector<FileSpec>& files, const std::string& name)
{
    test::TempFile file(name, bytes);
    auto pak = PakArchive::open(file.path);
    REQUIRE(pak);
    const auto entries = (*pak)->entries();
    REQUIRE(entries.size() == files.size()); // directory record skipped
    for (std::size_t i = 0; i < files.size(); ++i) {
        INFO(files[i].name);
        CHECK(entries[i].path == "dir/" + files[i].name);
        CHECK(entries[i].size == files[i].data.size());
        CHECK(entries[i].modified.has_value());
        auto data = (*pak)->read(i);
        REQUIRE(data);
        CHECK(*data == files[i].data);
    }
}

} // namespace

TEST_CASE("Blur PAK v2 with 40-byte entries: stored, zlib and chunked data")
{
    const auto files = sampleFiles();
    checkArchive(buildPak(files, 2, 40), files, "v2_40.pak");
}

TEST_CASE("Blur PAK v2 with 64-byte entries")
{
    const auto files = sampleFiles();
    checkArchive(buildPak(files, 2, 64), files, "v2_64.pak");
}

TEST_CASE("Blur PAK v1 (size-prefixed zlib)")
{
    std::vector<FileSpec> files{{"shader.fxb", test::sampleData(4000, 9), Storage::Zlib}};
    checkArchive(buildPak(files, 1, 32), files, "v1.pak");
}

TEST_CASE("Replacing entries keeps names, encodings and the other files intact")
{
    for (const std::uint32_t stride : {40u, 64u}) {
        DYNAMIC_SECTION("stride " << stride)
        {
            auto files = sampleFiles();
            test::TempFile file("replace_" + std::to_string(stride) + ".pak", buildPak(files, 2, stride));

            for (std::size_t i = 0; i < files.size(); ++i) {
                auto pak = PakArchive::open(file.path);
                REQUIRE(pak);
                const auto replacement = test::sampleData(200 + 700 * i, static_cast<unsigned>(50 + i));
                auto report = (*pak)->replace(i, replacement);
                REQUIRE(report);
                files[i].data = replacement;
            }

            auto pak = PakArchive::open(file.path);
            REQUIRE(pak);
            REQUIRE((*pak)->entries().size() == files.size());
            for (std::size_t i = 0; i < files.size(); ++i) {
                INFO(files[i].name);
                CHECK((*pak)->entries()[i].path == "dir/" + files[i].name);
                auto data = (*pak)->read(i);
                REQUIRE(data);
                CHECK(*data == files[i].data);
            }
            // Original encodings are preserved.
            CHECK((*pak)->describeEntry(0)[0].value.starts_with("Stored"));
            CHECK((*pak)->describeEntry(1)[0].value.starts_with("zlib"));
            CHECK((*pak)->describeEntry(2)[0].value.starts_with("Chunked"));
        }
    }
}

TEST_CASE("One file of a changed archive is restored from the archive's backup")
{
    namespace fs = std::filesystem;
    const fs::path game = fs::temp_directory_path() / "26viewer_test_restore_one";
    std::error_code ec;
    fs::remove_all(game, ec);
    const fs::path archive = game / "cache" / "data.pak";
    auto files = sampleFiles();
    REQUIRE(writeFile(archive, buildPak(files, 2, 40)));

    ContentRegistry content;
    content.addArchiveFormat(std::make_shared<viewer::games::blur::PakFormat>());
    // The archive's files in the order of `files` (the tree sorts them by name).
    const auto mounted = [&](Vfs& vfs) -> std::vector<const VfsNode*> {
        vfs.mountDirectory(game, content);
        const auto all = Vfs::collectFiles(vfs.root());
        std::vector<const VfsNode*> out;
        for (const FileSpec& f : files)
            if (const auto it = std::ranges::find_if(all, [&](const VfsNode* n) { return n->name() == f.name; });
                it != all.end())
                out.push_back(*it);
        CHECK(all.size() == files.size()); // the backup folder isn't mounted
        return out;
    };

    // Back up into "<game>/backup/cache/data.pak", then change two files.
    {
        Vfs vfs;
        const auto nodes = mounted(vfs);
        REQUIRE(nodes.size() == files.size());
        REQUIRE(ensureBackup(archive, vfs.backupRoot()) == game / "backup" / "cache" / "data.pak");
        REQUIRE(vfs.replace(*nodes[0], test::sampleData(300, 5)));
    }
    {
        Vfs vfs;
        const auto nodes = mounted(vfs);
        REQUIRE(nodes.size() == files.size());
        REQUIRE(vfs.replace(*nodes[1], test::sampleData(900, 6)));
    }
    // Put back only the second one, as it is in the backup.
    {
        Vfs vfs;
        const auto nodes = mounted(vfs);
        auto original = vfs.readBackup(*nodes[1], content);
        REQUIRE(original);
        CHECK(*original == files[1].data);
        REQUIRE(vfs.replace(*nodes[1], *original));
    }
    Vfs vfs;
    const auto nodes = mounted(vfs);
    CHECK(vfs.read(*nodes[1]).value() == files[1].data);  // restored
    CHECK(vfs.read(*nodes[0]).value() == test::sampleData(300, 5)); // keeps its change
    fs::remove_all(game, ec);
}

TEST_CASE("Blur name and content hashes")
{
    // Values taken from the shipped lut_rampak.pak.
    CHECK(PakArchive::nameHash("\\models\\objects\\shared\\construction") == 0x00D10931u);
    CHECK(PakArchive::nameHash("\\Models/Objects/Shared/Construction") == 0x00D10931u);
    CHECK(PakArchive::nameHash("\\ui") == 0x033EA576u);

    CHECK(PakArchive::contentHash({}) == 0u);
    const auto small = test::sampleData(1000, 1);
    CHECK(PakArchive::contentHash(small) == (compression::crc32(small) ^ 0xFFFFFFFFu));
    // Larger data: one ~CRC-32 per 32 KiB block, added together.
    const auto large = test::sampleData(80000, 2);
    const ByteSpan l(large);
    const std::uint32_t expected = (compression::crc32(l.subspan(0, 32768)) ^ 0xFFFFFFFFu)
                                   + (compression::crc32(l.subspan(32768, 32768)) ^ 0xFFFFFFFFu)
                                   + (compression::crc32(l.subspan(65536)) ^ 0xFFFFFFFFu);
    CHECK(PakArchive::contentHash(large) == expected);
}

TEST_CASE("Replacing writes Blur's content hash, also for files over 32 KiB")
{
    auto files = sampleFiles();
    test::TempFile file("replace_hash.pak", buildPak(files, 2, 40));
    const auto replacement = test::sampleData(100000, 77);
    std::string nameHashBefore;
    {
        auto pak = PakArchive::open(file.path);
        REQUIRE(pak);
        nameHashBefore = property((*pak)->describeEntry(2), "Name hash");
        auto report = (*pak)->replace(2, replacement);
        REQUIRE(report);
        CHECK(report->warnings.empty());
    }
    auto pak = PakArchive::open(file.path);
    REQUIRE(pak);
    CHECK((*pak)->read(2).value() == replacement);
    CHECK(property((*pak)->describeEntry(2), "Hash") == hex32(PakArchive::contentHash(replacement)));
    CHECK(property((*pak)->describeEntry(2), "Name hash") == nameHashBefore); // name unchanged
}

TEST_CASE("Replacing in a v1 archive")
{
    std::vector<FileSpec> files{{"a.fxb", test::sampleData(4000, 9), Storage::Zlib},
                                {"b.fxb", test::sampleData(100, 10), Storage::Zlib}};
    test::TempFile file("replace_v1.pak", buildPak(files, 1, 32));
    {
        auto pak = PakArchive::open(file.path);
        REQUIRE(pak);
        REQUIRE((*pak)->replace(0, test::sampleData(9000, 11)));
    }
    auto pak = PakArchive::open(file.path);
    REQUIRE(pak);
    CHECK((*pak)->read(0).value() == test::sampleData(9000, 11));
    CHECK((*pak)->read(1).value() == files[1].data);
}

TEST_CASE("Corrupt PAK files are rejected with an error")
{
    auto bytes = buildPak(sampleFiles(), 2, 40);
    SECTION("bad magic")
    {
        bytes[0] = std::byte{'X'};
        test::TempFile file("bad_magic.pak", bytes);
        CHECK_FALSE(PakArchive::open(file.path));
    }
    SECTION("truncated file")
    {
        bytes.resize(0x30);
        test::TempFile file("truncated.pak", bytes);
        CHECK_FALSE(PakArchive::open(file.path));
    }
}

TEST_CASE("Real Blur archives (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "gamedata_boot.pak");
    REQUIRE(pak);
    CHECK((*pak)->entries().size() == 48);
    for (std::size_t i = 0; i < (*pak)->entries().size(); ++i) {
        const auto& entry = (*pak)->entries()[i];
        INFO(entry.path);
        auto data = (*pak)->read(i);
        REQUIRE(data);
        CHECK(data->size() == entry.size);
        const auto props = (*pak)->describeEntry(i);
        CHECK(property(props, "Hash") == hex32(PakArchive::contentHash(*data)));
        CHECK(property(props, "Name hash") == hex32(PakArchive::nameHash("\\" + entry.path)));
    }
}
