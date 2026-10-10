// Blur "2KAP" package reader.
//
// Layout (little endian, verified against every .pak shipped with the PC game):
//
//   0x00  char[4]  magic "2KAP"
//   0x04  u32      version (1 or 2)
//   0x08  u32      alignment (0x800) — data offsets are stored in these units
//   0x10  u16      flags (v2)         0x24 in v1
//   0x38  u32      name table size; when 0 the size lives at `alignment`
//   ....  name table, XOR-obfuscated with a 14-byte key, NUL-separated strings
//   align file table: u32 count, u32 tableBytes, then `count` fixed-size
//         entries (stride = tableBytes / count: 32, 40 or 64 bytes), sorted
//         by name hash
//
// Entry: u32 nameHash, u32 nameOffset, u32 parentNameOffset, u32 size,
//        u64 FILETIME, i64 offset (alignment units, -1 = directory),
//        [v2] u32 contentHash, u32 storedSize, [stride 64] u32 flags + 20-byte
//        SHA-1 of the stored bytes
//
// Both hashes were recovered from Blur.exe (name lookup at 0xCB6720, archive
// self-check at 0xCB7340) and verified against every shipped entry.
//
// Data storage, one of:
//   * stored            storedSize == size
//   * zlib              RFC 1950 stream (gamedata*.pak)
//   * chunked deflate   u32 chunkSize, u32 zsize, raw deflate; then repeated
//                       u32 zsize + raw deflate until `size` bytes are produced
//                       (a chunk whose zsize equals its output size may still
//                       be compressed — try inflating before treating it as stored)
//   * v1                u32 zsize prefix followed by a zlib stream
//
// See docs/formats/blur.md for the full write-up.

#include "PakArchive.hpp"

#include "viewer/core/Compression.hpp"
#include "viewer/core/Hash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <fstream>

namespace viewer::games::blur {

namespace {

constexpr std::uint32_t kMagic = 0x50414B32;       // "2KAP" little endian
constexpr std::uint32_t kMagicXbox = 0x3250414B;   // "PAK2" (Xbox 360, big endian + XMemCompress)
constexpr std::array<std::uint8_t, 14> kNameKey{'V', 'X', 'o', '4', '0', 'j', '3', '@', '$', '%', '\\', '%', '`', 'x'};

std::string hex(std::uint64_t v, int width = 8)
{
    return std::format("0x{:0{}X}", v, width);
}

std::string u8name(const std::filesystem::path& p)
{
    auto s = p.filename().u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

} // namespace

// ---- PakFormat -------------------------------------------------------------------

bool PakFormat::probe(ByteSpan header, const std::filesystem::path&) const
{
    return header.size() >= 0x40 && loadScalar<std::uint32_t>(header, 0) == kMagic;
}

Result<std::unique_ptr<Archive>> PakFormat::open(const std::filesystem::path& path) const
{
    auto pak = PakArchive::open(path);
    if (!pak)
        return std::unexpected(pak.error());
    return std::unique_ptr<Archive>(std::move(*pak));
}

// ---- PakArchive ------------------------------------------------------------------

Result<std::unique_ptr<PakArchive>> PakArchive::open(const std::filesystem::path& path)
{
    auto fileResult = RandomAccessFile::open(path);
    if (!fileResult)
        return std::unexpected(fileResult.error());

    std::unique_ptr<PakArchive> pak(new PakArchive());
    pak->m_file = std::move(*fileResult);
    const RandomAccessFile& file = *pak->m_file;
    const std::string name = u8name(path);

    auto header = file.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(file.size(), 0x40)));
    if (!header || header->size() < 0x40)
        return fail(ErrorCode::InvalidFormat, "{}: file too small for a PAK header", name);
    const ByteSpan h = *header;

    const auto magic = loadScalar<std::uint32_t>(h, 0);
    if (magic == kMagicXbox)
        return fail(ErrorCode::Unsupported, "{}: Xbox 360 'PAK2' archives are not supported yet", name);
    if (magic != kMagic)
        return fail(ErrorCode::InvalidFormat, "{}: not a Blur PAK (bad magic)", name);

    pak->m_version = loadScalar<std::uint32_t>(h, 4);
    pak->m_alignment = loadScalar<std::uint32_t>(h, 8);
    if (pak->m_version < 1 || pak->m_version > 2)
        return fail(ErrorCode::Unsupported, "{}: unsupported PAK version {}", name, pak->m_version);
    if (!std::has_single_bit(pak->m_alignment) || pak->m_alignment < 16 || pak->m_alignment > (1u << 20))
        return fail(ErrorCode::InvalidFormat, "{}: invalid alignment {}", name, pak->m_alignment);
    pak->m_flags = loadScalar<std::uint16_t>(h, pak->m_version >= 2 ? 0x10 : 0x24);

    // --- Name table -------------------------------------------------------------
    std::uint64_t namesOffset = 0x3C;
    std::uint32_t namesSize = loadScalar<std::uint32_t>(h, 0x38);
    if (namesSize == 0) {
        auto sizeBytes = file.read(pak->m_alignment, 4);
        if (!sizeBytes)
            return std::unexpected(sizeBytes.error());
        namesSize = loadScalar<std::uint32_t>(*sizeBytes, 0);
        namesOffset = pak->m_alignment + 4ull;
    }
    if (namesSize == 0 || namesOffset + namesSize > file.size())
        return fail(ErrorCode::InvalidFormat, "{}: name table out of range", name);
    pak->m_nameTableSize = namesSize;

    auto names = file.read(namesOffset, namesSize);
    if (!names)
        return std::unexpected(names.error());
    for (std::size_t i = 0; i < names->size(); ++i)
        (*names)[i] ^= std::byte{kNameKey[i % kNameKey.size()]};

    // --- File table ---------------------------------------------------------------
    const std::uint64_t align = pak->m_alignment;
    const std::uint64_t tableOffset = (namesOffset + namesSize + align - 1) / align * align;
    auto tableHeader = file.read(tableOffset, 8);
    if (!tableHeader)
        return std::unexpected(tableHeader.error());
    const auto count = loadScalar<std::uint32_t>(*tableHeader, 0);
    const auto tableBytes = loadScalar<std::uint32_t>(*tableHeader, 4);
    if (count == 0)
        return pak; // empty archive
    const std::uint32_t stride = tableBytes / count;
    const bool strideOk = stride * count == tableBytes
                          && (pak->m_version == 1 ? stride >= 32 : (stride == 40 || stride == 64));
    if (!strideOk)
        return fail(ErrorCode::InvalidFormat, "{}: unexpected entry layout ({} bytes for {} entries)", name,
                    tableBytes, count);
    pak->m_entryStride = stride;

    const std::uint64_t entriesOffset = tableOffset + 8;
    auto table = file.read(entriesOffset, tableBytes);
    if (!table)
        return std::unexpected(table.error());

    pak->m_entries.reserve(count);
    pak->m_slots.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const ByteSpan e = ByteSpan(*table).subspan(static_cast<std::size_t>(i) * stride, stride);
        const auto nameOffset = loadScalar<std::uint32_t>(e, 4);
        const auto size = loadScalar<std::uint32_t>(e, 12);
        const auto fileTime = loadScalar<std::uint64_t>(e, 16);
        const auto offset = loadScalar<std::int64_t>(e, 24);

        if (offset == -1) { // directory record — the VFS derives folders from paths
            ++pak->m_directoryCount;
            continue;
        }
        if (offset < 0 || static_cast<std::uint64_t>(offset) * align >= file.size())
            return fail(ErrorCode::InvalidFormat, "{}: entry {} points outside the file", name, i);

        Slot slot;
        slot.entryOffset = entriesOffset + std::uint64_t{i} * stride;
        slot.dataOffset = static_cast<std::uint64_t>(offset) * align;
        slot.nameHash = loadScalar<std::uint32_t>(e, 0);
        std::uint64_t packed = 0;
        if (pak->m_version >= 2) {
            slot.hash = loadScalar<std::uint32_t>(e, 32);
            slot.storedSize = loadScalar<std::uint32_t>(e, 36);
            packed = slot.storedSize;
            if (stride == 64) {
                std::copy_n(e.data() + 44, slot.digest.size(), slot.digest.begin());
                slot.hasDigest = true;
            }
        } else {
            auto prefix = file.read(slot.dataOffset, 4);
            if (!prefix)
                return std::unexpected(prefix.error());
            slot.storedSize = loadScalar<std::uint32_t>(*prefix, 0);
            packed = slot.storedSize + 4ull;
        }
        if (slot.dataOffset + packed > file.size())
            return fail(ErrorCode::InvalidFormat, "{}: entry {} data runs past end of file", name, i);

        ArchiveEntry entry;
        entry.path = normalizeArchivePath(cstringAt(*names, nameOffset));
        entry.size = size;
        entry.packedSize = packed;
        entry.modified = fromFileTime(fileTime);
        if (entry.path.empty())
            entry.path = std::format("unnamed_{:05}", i);

        pak->m_entries.push_back(std::move(entry));
        pak->m_slots.push_back(slot);
    }
    return pak;
}

Result<PakArchive::Storage> PakArchive::detectStorage(const Slot& slot, std::uint64_t size) const
{
    if (m_version < 2)
        return Storage::Zlib;
    if (slot.storedSize == size)
        return Storage::Stored;
    auto head = m_file->read(slot.dataOffset, std::min<std::size_t>(slot.storedSize, 2));
    if (!head)
        return std::unexpected(head.error());
    return compression::looksLikeZlib(*head) ? Storage::Zlib : Storage::ChunkedDeflate;
}

Result<ByteBuffer> PakArchive::read(std::size_t index) const
{
    if (index >= m_entries.size())
        return fail(ErrorCode::NotFound, "entry {} out of range", index);
    const ArchiveEntry& entry = m_entries[index];
    const Slot& slot = m_slots[index];
    const auto size = static_cast<std::size_t>(entry.size);
    if (size == 0)
        return ByteBuffer{};

    if (m_version < 2) {
        auto raw = m_file->read(slot.dataOffset + 4, slot.storedSize);
        if (!raw)
            return std::unexpected(raw.error());
        if (slot.storedSize == size && !compression::looksLikeZlib(*raw))
            return raw;
        return compression::inflateZlib(*raw, size);
    }

    auto raw = m_file->read(slot.dataOffset, slot.storedSize);
    if (!raw)
        return std::unexpected(raw.error());
    if (slot.storedSize == size)
        return raw;
    if (compression::looksLikeZlib(*raw))
        return compression::inflateZlib(*raw, size);
    return readChunked(*raw, size);
}

Result<ByteBuffer> PakArchive::readChunked(ByteSpan raw, std::size_t size) const
{
    BinaryReader r(raw);
    auto chunkSize = r.read<std::uint32_t>();
    auto zsize = r.read<std::uint32_t>();
    if (!chunkSize || !zsize)
        return fail(ErrorCode::InvalidFormat, "truncated chunk header");
    if (*chunkSize == 0 || *chunkSize > (16u << 20))
        return fail(ErrorCode::InvalidFormat, "implausible chunk size {}", *chunkSize);

    ByteBuffer out;
    out.reserve(size);
    std::uint32_t z = *zsize;
    for (;;) {
        const std::size_t want = std::min<std::size_t>(*chunkSize, size - out.size());
        auto block = r.readBytes(z);
        if (!block)
            return std::unexpected(block.error());
        // A compressed chunk can coincidentally be exactly `want` bytes long,
        // so always try to inflate first; only fall back to a stored copy.
        if (auto ok = compression::inflateRawAppend(*block, want, out); !ok) {
            if (z != want)
                return std::unexpected(ok.error());
            out.insert(out.end(), block->begin(), block->end());
        }
        if (out.size() >= size)
            break;
        auto next = r.read<std::uint32_t>();
        if (!next)
            return std::unexpected(next.error());
        z = *next;
    }
    return out;
}

// ---- Writing ---------------------------------------------------------------------

namespace {

void appendU32(ByteBuffer& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>(v >> (8 * i)));
}

template <typename T>
bool writeAt(std::fstream& f, std::uint64_t offset, T value)
{
    std::array<char, sizeof(T)> bytes;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        bytes[i] = static_cast<char>(static_cast<std::uint64_t>(value) >> (8 * i));
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(bytes.data(), bytes.size());
    return static_cast<bool>(f);
}

} // namespace

std::uint32_t PakArchive::nameHash(std::string_view archivePath) noexcept
{
    std::string lower(archivePath);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        else if (c == '/')
            c = '\\';
    }
    return compression::crc32(asBytes(lower)) ^ 0xFFFFFFFFu;
}

std::uint32_t PakArchive::contentHash(ByteSpan data) noexcept
{
    constexpr std::size_t kBlock = 32 * 1024;
    std::uint32_t sum = 0;
    for (std::size_t pos = 0; pos < data.size(); pos += kBlock)
        sum += compression::crc32(data.subspan(pos, std::min(kBlock, data.size() - pos))) ^ 0xFFFFFFFFu;
    return sum;
}

Result<ByteBuffer> PakArchive::encodeLike(const Slot& slot, std::uint64_t originalSize, ByteSpan data,
                                          std::string& storageName) const
{
    if (m_version < 2) {
        auto z = compression::deflateZlib(data);
        if (!z)
            return std::unexpected(z.error());
        ByteBuffer out;
        appendU32(out, static_cast<std::uint32_t>(z->size()));
        out.insert(out.end(), z->begin(), z->end());
        storageName = "zlib (size-prefixed, v1)";
        return out;
    }

    auto storage = detectStorage(slot, originalSize);
    if (!storage)
        return std::unexpected(storage.error());
    switch (*storage) {
    case Storage::Stored:
        storageName = "stored";
        return ByteBuffer(data.begin(), data.end());
    case Storage::Zlib:
        storageName = "zlib";
        return compression::deflateZlib(data);
    case Storage::ChunkedDeflate: {
        // Keep the original chunk size (always 64 KiB in shipped archives).
        auto head = m_file->read(slot.dataOffset, 4);
        if (!head)
            return std::unexpected(head.error());
        const auto chunk = std::max<std::uint32_t>(1, loadScalar<std::uint32_t>(*head, 0));
        ByteBuffer out;
        appendU32(out, chunk);
        for (std::size_t pos = 0; pos < data.size(); pos += chunk) {
            const ByteSpan piece = data.subspan(pos, std::min<std::size_t>(chunk, data.size() - pos));
            const std::size_t sizeAt = out.size();
            appendU32(out, 0); // zsize, patched below
            // Always deflate (never store) so readers can't mistake the chunk type.
            if (auto ok = compression::deflateRawAppend(piece, out); !ok)
                return std::unexpected(ok.error());
            const auto z = static_cast<std::uint32_t>(out.size() - sizeAt - 4);
            for (int i = 0; i < 4; ++i)
                out[sizeAt + static_cast<std::size_t>(i)] = static_cast<std::byte>(z >> (8 * i));
        }
        storageName = "chunked raw deflate";
        return out;
    }
    }
    return fail(ErrorCode::Unsupported, "unknown storage");
}

Result<ReplaceReport> PakArchive::replace(std::size_t index, ByteSpan data) const
{
    if (index >= m_entries.size())
        return fail(ErrorCode::NotFound, "entry {} out of range", index);
    if (data.size() > UINT32_MAX)
        return fail(ErrorCode::Unsupported, "Blur archives cannot hold files of 4 GiB or more");

    const Slot& slot = m_slots[index];
    ReplaceReport report;
    auto blob = encodeLike(slot, m_entries[index].size, data, report.storage);
    if (!blob)
        return std::unexpected(blob.error());

    const auto& path = m_file->path();
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f)
        return fail(ErrorCode::Io, "cannot open '{}' for writing (read-only, or in use by the game?)",
                    u8name(path));

    // 1. Append the new data at the next alignment boundary (old data stays, unused).
    f.seekp(0, std::ios::end);
    const auto end = static_cast<std::uint64_t>(f.tellp());
    const std::uint64_t align = m_alignment;
    const std::uint64_t dataPos = (end + align - 1) / align * align;
    const std::uint64_t paddedEnd = (dataPos + blob->size() + align - 1) / align * align;
    const std::vector<char> zeros(static_cast<std::size_t>(dataPos - end), 0);
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    f.write(reinterpret_cast<const char*>(blob->data()), static_cast<std::streamsize>(blob->size()));
    const std::vector<char> tail(static_cast<std::size_t>(paddedEnd - dataPos - blob->size()), 0);
    f.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    if (!f)
        return fail(ErrorCode::Io, "write failed in '{}' (disk full?)", u8name(path));
    f.flush();

    // 2. Re-point this entry's record at the new data. Done last, so an
    //    interrupted write leaves the archive pointing at the old data.
    //    The name (and so the name hash and sort order) is unchanged.
    const std::uint64_t e = slot.entryOffset;
    bool ok = writeAt(f, e + 12, static_cast<std::uint32_t>(data.size()))
              && writeAt(f, e + 16, toFileTime(std::chrono::system_clock::now()))
              && writeAt(f, e + 24, static_cast<std::int64_t>(dataPos / align));
    if (m_version >= 2) {
        ok = ok && writeAt(f, e + 32, contentHash(data))
             && writeAt(f, e + 36, static_cast<std::uint32_t>(blob->size()));
        if (m_entryStride == 64) {
            const auto digest = hash::sha1(*blob); // SHA-1 of the stored bytes
            f.seekp(static_cast<std::streamoff>(e + 44));
            f.write(reinterpret_cast<const char*>(digest.data()), static_cast<std::streamsize>(digest.size()));
            ok = ok && static_cast<bool>(f);
        }
    }
    f.flush();
    if (!ok || !f)
        return fail(ErrorCode::Io, "could not update the file table of '{}'", u8name(path));

    report.storedSize = blob->size();
    auto sig = path;
    sig.replace_extension(".sig");
    std::error_code ec;
    if (std::filesystem::exists(sig, ec))
        report.warnings.push_back(std::format(
            "{} is signed ({}). The game may refuse modified signed archives unless signature checks are "
            "disabled (e.g. by a mod loader).",
            u8name(path), u8name(sig)));
    return report;
}

PropertyList PakArchive::describeEntry(std::size_t index) const
{
    if (index >= m_entries.size())
        return {};
    const Slot& slot = m_slots[index];
    PropertyList props;

    std::string storage = "unknown";
    if (auto s = detectStorage(slot, m_entries[index].size)) {
        switch (*s) {
        case Storage::Stored: storage = "Stored (uncompressed)"; break;
        case Storage::Zlib: storage = m_version < 2 ? "zlib (size-prefixed, v1)" : "zlib"; break;
        case Storage::ChunkedDeflate: storage = "Chunked raw deflate (64 KiB blocks)"; break;
        }
    }
    props.push_back({"Storage", storage});
    props.push_back({"Data offset", hex(slot.dataOffset, 10)});
    props.push_back({"Stored size", std::format("{} bytes", m_entries[index].packedSize)});
    if (m_version >= 2)
        props.push_back({"Hash", hex(slot.hash)});
    props.push_back({"Name hash", hex(slot.nameHash)});
    if (slot.hasDigest) {
        std::string digest;
        for (auto b : slot.digest)
            digest += std::format("{:02x}", std::to_integer<unsigned>(b));
        props.push_back({"Digest", digest});
    }
    return props;
}

PropertyList PakArchive::describe() const
{
    return {
        {"Format", "Blur PAK (2KAP)"},
        {"Version", std::to_string(m_version)},
        {"Alignment", hex(m_alignment, 4)},
        {"Flags", hex(m_flags, 4)},
        {"Entry size", std::format("{} bytes", m_entryStride)},
        {"Files", std::to_string(m_entries.size())},
        {"Directory records", std::to_string(m_directoryCount)},
        {"Name table", std::format("{} bytes (XOR obfuscated)", m_nameTableSize)},
    };
}

} // namespace viewer::games::blur
