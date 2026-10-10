#include "ChunkFile.hpp"

#include <algorithm>
#include <format>

namespace viewer::games::blur {

namespace {

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxNameLength = 63;

/// Parses consecutive chunks in [begin, end). Returns how far parsing got.
std::size_t parseSequence(ByteSpan data, std::size_t begin, std::size_t end, std::vector<Chunk>& out, int depth);

void parseChildren(ByteSpan data, Chunk& chunk, int depth)
{
    if (depth >= kMaxDepth || chunk.payloadSize() < 16)
        return;
    std::vector<Chunk> kids;
    const std::size_t end = chunk.offset + chunk.size;
    const std::size_t reached = parseSequence(data, chunk.payloadOffset(), end, kids, depth + 1);
    // Containers keep whatever parsed; data chunks only count as nested when
    // the children cover the payload exactly (otherwise it's just raw bytes).
    if (!kids.empty() && (chunk.kind == 'C' || reached == end))
        chunk.children = std::move(kids);
}

std::size_t parseSequence(ByteSpan data, std::size_t begin, std::size_t end, std::vector<Chunk>& out, int depth)
{
    const ByteSpan bounded = data.first(end);
    std::size_t pos = begin;
    while (pos < end) {
        auto chunk = readChunkHeader(bounded, pos);
        if (!chunk)
            break;
        parseChildren(data, *chunk, depth);
        pos += chunk->size;
        out.push_back(std::move(*chunk));
    }
    return pos;
}

StructureNode toNode(const Chunk& c)
{
    StructureNode node;
    node.label = c.name;
    node.offset = c.offset;
    node.size = c.size;
    if (c.kind == 'C' || !c.children.empty())
        node.detail = std::format("container · {} children · {} bytes", c.children.size(), c.payloadSize());
    else
        node.detail = std::format("data · {} bytes", c.payloadSize());

    std::size_t covered = c.payloadOffset();
    for (const auto& child : c.children) {
        node.children.push_back(toNode(child));
        covered = child.offset + child.size;
    }
    if (!c.children.empty() && covered < c.offset + c.size) {
        node.children.push_back({"(unparsed)", std::format("{} bytes", c.offset + c.size - covered), covered,
                                 c.offset + c.size - covered, {}});
    }
    return node;
}

} // namespace

const Chunk* Chunk::child(std::string_view childName) const noexcept
{
    for (const auto& c : children)
        if (c.name == childName)
            return &c;
    return nullptr;
}

std::optional<Chunk> readChunkHeader(ByteSpan data, std::size_t offset)
{
    if (offset >= data.size())
        return std::nullopt;
    const auto name = cstringAt(data, offset);
    if (name.empty() || name.size() > kMaxNameLength || offset + name.size() >= data.size())
        return std::nullopt;
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
            return std::nullopt;

    const std::size_t padded = (name.size() + 1 + 3) & ~std::size_t{3};
    const std::size_t headerSize = padded + 8;
    if (data.size() - offset < headerSize)
        return std::nullopt;
    for (std::size_t i = offset + name.size(); i < offset + padded; ++i)
        if (data[i] != std::byte{0})
            return std::nullopt;

    const auto size = loadScalar<std::uint32_t>(data, offset + padded);
    const auto count = loadScalar<std::uint16_t>(data, offset + padded + 4);
    const auto kind = loadScalar<std::uint16_t>(data, offset + padded + 6);
    if ((kind != 'A' && kind != 'C') || size < headerSize || size > data.size() - offset)
        return std::nullopt;

    Chunk chunk;
    chunk.name = std::string(name);
    chunk.offset = offset;
    chunk.headerSize = headerSize;
    chunk.size = size;
    chunk.count = count;
    chunk.kind = static_cast<char>(kind);
    return chunk;
}

Result<Chunk> parseChunkFile(ByteSpan data)
{
    auto root = readChunkHeader(data, 0);
    if (!root)
        return fail(ErrorCode::InvalidFormat, "not a chunk file");
    parseChildren(data, *root, 0);
    return *root;
}

// ---- Structure parser ---------------------------------------------------------------

bool ChunkStructureParser::canParse(std::string_view, ByteSpan data) const
{
    return readChunkHeader(data, 0).has_value();
}

Result<StructureNode> ChunkStructureParser::parse(ByteSpan data) const
{
    std::vector<Chunk> roots;
    const std::size_t reached = parseSequence(data, 0, data.size(), roots, 0);
    if (roots.empty())
        return fail(ErrorCode::InvalidFormat, "not a chunk file");

    StructureNode file{"File", std::format("{} top-level chunk(s)", roots.size()), 0, data.size(), {}};
    for (const auto& r : roots)
        file.children.push_back(toNode(r));
    if (reached < data.size())
        file.children.push_back(
            {"(trailing data)", std::format("{} bytes", data.size() - reached), reached, data.size() - reached, {}});
    return file;
}

// ---- Texture decoder ------------------------------------------------------------------

bool TextureDecoder::canDecode(std::string_view extension, ByteSpan data) const
{
    if (extension == "texture")
        return true;
    auto chunk = readChunkHeader(data, 0);
    return chunk && chunk->name == "Texture";
}

Result<Image> TextureDecoder::decode(ByteSpan data) const
{
    auto root = parseChunkFile(data);
    if (!root)
        return std::unexpected(root.error());
    const Chunk* dataChunk = root->child("Data");
    if (root->name != "Texture" || !dataChunk)
        return fail(ErrorCode::InvalidFormat, "texture has no Data chunk");

    // Data payload: u32 width, height, depth, mipCount, format (FourCC or
    // D3DFORMAT), u32 unknown, u32 unknown, then every mip level in order.
    constexpr std::size_t kDataHeader = 28;
    if (dataChunk->payloadSize() < kDataHeader)
        return fail(ErrorCode::InvalidFormat, "texture Data chunk too small");
    const ByteSpan payload = data.subspan(dataChunk->payloadOffset(), dataChunk->payloadSize());
    const auto width = loadScalar<std::uint32_t>(payload, 0);
    const auto height = loadScalar<std::uint32_t>(payload, 4);
    const auto depth = loadScalar<std::uint32_t>(payload, 8);
    const auto mips = loadScalar<std::uint32_t>(payload, 12);
    const auto formatCode = loadScalar<std::uint32_t>(payload, 16);

    const auto format = image::fromD3DFormat(formatCode);
    if (!format)
        return fail(ErrorCode::Unsupported, "unsupported texture format {:#x}", formatCode);

    auto img = image::decodeSurface(payload.subspan(kDataHeader), *format, width, height);
    if (img) {
        img->mipCount = std::max(1u, mips);
        img->layers = std::max(1u, depth);
    }
    return img;
}

} // namespace viewer::games::blur
