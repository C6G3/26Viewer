#include "AudioEntity.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace viewer::games::blur {

namespace {

/// A repeated list in a record: a u16 count at `countAt` (from the record's
/// name field), then `count` items of `itemSize` bytes laid out as `item`.
struct ListSpec {
    std::uint8_t countAt = 0;
    std::uint8_t itemSize = 0;
    const char* item = nullptr;
    const char* label = nullptr;
};

/// A component type: its record is `head` bytes from the name field on (the
/// name included), then its lists. `layout` describes the bytes after the
/// name, 4 at a time (S = 8):
///   L link "#component:port"   S parameter: link + constant   K component (u32)
///   f float   i integer
///   h two u16   n count (u16)   c two counts   b sound bank   w wave name   u u16 (list items)
/// Field layouts were inferred from every entity in the game; payload sizes
/// come from the game's own load functions.
struct TypeSpec {
    std::uint8_t id = 0;
    const char* name = nullptr;
    std::uint16_t head = 0;
    const char* layout = nullptr;
    ListSpec lists[2];
};

constexpr TypeSpec kTypes[] = {
    {0x00, "ParameterIn", 0x08, "f", {{}, {}}},
    {0x01, "ParameterOut", 0x0c, "S", {{}, {}}},
    {0x02, "ActionIn", 0x08, "K", {{}, {}}},
    {0x03, "ActionOut", 0x04, "", {{}, {}}},
    {0x04, "VectorIn", 0x10, "fff", {{}, {}}},
    {0x06, "ControlInput", 0x04, "", {{}, {}}},
    {0x07, "ControlOutput", 0x0c, "S", {{}, {}}},
    {0x08, "EventInput", 0x08, "K", {{}, {}}},
    {0x09, "EventOutput", 0x04, "", {{}, {}}},
    {0x0a, "CoordinateInput", 0x04, "", {{}, {}}},
    {0x0b, "CoordinateOutput", 0x08, "L", {{}, {}}},
    {0x0c, "MonoAudioInput", 0x04, "", {{}, {}}},
    {0x0d, "MonoAudioOutput", 0x08, "L", {{}, {}}},
    {0x0e, "StereoAudioInput", 0x04, "", {{}, {}}},
    {0x0f, "StereoAudioOutput", 0x08, "L", {{}, {}}},
    {0x10, "SurroundAudioInput", 0x04, "", {{}, {}}},
    {0x11, "SurroundAudioOutput", 0x08, "L", {{}, {}}},
    {0x12, "SurroundLFEAudioInput", 0x04, "", {{}, {}}},
    {0x13, "SurroundLFEAudioOutput", 0x08, "L", {{}, {}}},
    {0x14, "Variable", 0x08, "f", {{}, {}}},
    {0x15, "PiecewiseLinearFunction", 0x14, "LSn", {{0x10, 12, "fS", "points"}, {}}},
    {0x17, "Source", 0xa8, "Siiiiifiiiiiiiififiiiiiiiiififiiiihhhhhh", {{}, {}}},
    {0x18, "Rolloff", 0x30, "LSSSSS", {{}, {}}},
    {0x19, "Cone", 0x40, "LSSSSSSS", {{}, {}}},
    {0x1a, "LowPass", 0x10, "KS", {{}, {}}},
    {0x1c, "Timer", 0x20, "SSSL", {{}, {}}},
    {0x1d, "Metronome", 0x28, "", {{}, {}}},
    {0x20, "Random", 0x14, "SS", {{}, {}}},
    {0x21, "Selector", 0x10, "Sn", {{0x0c, 32, "SSSS", "items"}, {}}},
    {0x22, "StateMachine", 0x10, "", {{0x0c, 8, "ii", "items"}, {}}},
    {0x23, "Quantize", 0x18, "", {{}, {}}},
    {0x24, "Sum", 0x08, "n", {{0x04, 8, "S", "items"}, {}}},
    {0x25, "Product", 0x08, "n", {{0x04, 8, "S", "items"}, {}}},
    {0x28, "Exponent", 0x10, "KS", {{}, {}}},
    {0x29, "Max", 0x08, "n", {{0x04, 8, "S", "items"}, {}}},
    {0x2a, "Min", 0x08, "n", {{0x04, 8, "S", "items"}, {}}},
    {0x2b, "Invert", 0x08, "", {{}, {}}},
    {0x2c, "Gradient", 0x08, "L", {{}, {}}},
    {0x2d, "Spring", 0x18, "KSS", {{}, {}}},
    {0x2f, "Sine", 0x08, "", {{}, {}}},
    {0x33, "SampleAndHold", 0x08, "K", {{}, {}}},
    {0x34, "Envelope", 0x38, "KSSSSLLLL", {{}, {}}},
    {0x36, "GateSwitch", 0x30, "LSSSSLL", {{}, {}}},
    {0x37, "Array", 0x14, "KSn", {{0x10, 8, "S", "items"}, {}}},
    {0x38, "Fraction", 0x08, "", {{}, {}}},
    {0x3b, "MonoOneShotSound", 0x34, "bwSSSSLn", {{0x30, 4, "i", "items"}, {}}},
    {0x3c, "StereoOneShotSound", 0x34, "bwSSSSLn", {{0x30, 4, "i", "items"}, {}}},
    {0x3d, "MonoOneShotSoundTable", 0x3c, "bKiSSSSLnn", {{0x34, 4, "i", "items"}, {0x38, 4, "w", "waves"}}},
    {0x3e, "StereoOneShotSoundTable", 0x3c, "bKiSSSSLnn", {{0x34, 4, "i", "items"}, {0x38, 4, "w", "waves"}}},
    {0x3f, "MonoLoopingSound", 0x3c, "bwSSSSSLn", {{0x38, 2, "u", "items"}, {}}},
    {0x40, "StereoLoopingSound", 0x3c, "bwSSSSSLn", {{0x38, 4, "i", "items"}, {}}},
    {0x44, "MonoToMonoMixer", 0x18, "SSc", {{0x14, 4, "i", "inputs"}, {0x16, 20, "SSL", "channels"}}},
    {0x45, "MonoToStereoMixer", 0x18, "SSc", {{0x14, 4, "i", "inputs"}, {0x16, 36, "SSSSL", "channels"}}},
    {0x46, "MonoToSurroundMixer", 0x3c, "iSSSSSSc", {{0x38, 4, "i", "inputs"}, {0x3a, 20, "SSL", "channels"}}},
    {0x47, "MonoToSurroundLFEMixer", 0x4c, "iSSSSSSSSc", {{0x48, 4, "i", "inputs"}, {0x4a, 20, "SSf", "channels"}}},
    {0x48, "StereoToMonoMixer", 0x18, "SSc", {{0x14, 4, "i", "inputs"}, {0x16, 36, "SSSSL", "channels"}}},
    {0x49, "StereoToStereoMixer", 0x18, "SSc", {{0x14, 4, "i", "inputs"}, {0x16, 68, "SSLhSLhSSSL", "channels"}}},
    {0x4a, "StereoToQuadMixer", 0x18, "Sifc", {{0x14, 4, "i", "inputs"}, {0x16, 132, "SSLhSSifSifLhSSSSifSiff", "channels"}}},
    {0x4b, "StereoToSurroundMixer", 0x5c, "iSSSSSSSSSSc", {{0x58, 4, "i", "inputs"}, {0x5a, 36, "SSSSL", "channels"}}},
    {0x4c, "QuadToSurroundMixer", 0x9c, "iSififSSSifSSSifSSSifSSSc", {{0x98, 4, "i", "inputs"}, {0x9a, 68, "SSSSSSSSi", "channels"}}},
    {0x4f, "SurroundToSurroundMixer", 0x18, "SSc", {{0x14, 4, "i", "inputs"}, {0x16, 20, "SSL", "channels"}}},
    {0x50, "SurroundToSurroundLFEMixer", 0x28, "SSSifc", {{0x24, 4, "i", "inputs"}, {0x26, 20, "SSL", "channels"}}},
    {0x51, "SurroundLFEToSurroundLFEMixer", 0x18, "Sifc", {{0x14, 4, "i", "inputs"}, {0x16, 20, "SSL", "channels"}}},
    {0x70, "MonoDelay", 0x24, "ffifSS", {{}, {}}},
    {0x76, "MonoRMSLevel", 0x0c, "", {{}, {}}},
    {0x77, "StereoRMSLevel", 0x0c, "", {{}, {}}},
    {0x78, "SurroundRMSLevel", 0x0c, "S", {{}, {}}},
    {0x79, "MonoSaturator", 0x24, "SSSS", {{}, {}}},
    {0x7a, "StereoSaturator", 0x24, "iiSSif", {{}, {}}},
    {0x7b, "SurroundSaturator", 0x24, "SSSS", {{}, {}}},
    {0x82, "MonoConvolution", 0x24, "", {{}, {}}},
    {0x8e, "MonoLowPass", 0x2c, "SSSSS", {{}, {}}},
    {0x8f, "StereoLowPass", 0x2c, "SSifSS", {{}, {}}},
    {0x90, "SurroundLowPass", 0x2c, "SSSSS", {{}, {}}},
    {0x91, "MonoHighPass", 0x2c, "SSSSS", {{}, {}}},
    {0x92, "StereoHighPass", 0x2c, "SSSSS", {{}, {}}},
    {0x93, "SurroundHighPass", 0x2c, "SSifSS", {{}, {}}},
    {0x94, "MonoResonantLowPass", 0x3c, "", {{}, {}}},
    {0x95, "StereoResonantLowPass", 0x3c, "", {{}, {}}},
    {0x96, "SurroundResonantLowPass", 0x3c, "SSSSSSS", {{}, {}}},
    {0x9a, "Mono24dBResonantLowPass", 0x3c, "", {{}, {}}},
    {0x9c, "Surround24dBResonantLowPass", 0x3c, "", {{}, {}}},
    {0xa3, "MonoPeakEQ", 0x3c, "SSSSSSS", {{}, {}}},
    {0xa4, "StereoPeakEQ", 0x3c, "", {{}, {}}},
    {0xa5, "SurroundPeakEQ", 0x3c, "", {{}, {}}},
    {0xa6, "MonoCompressor", 0x40, "LSSSSSSS", {{}, {}}},
    {0xa7, "StereoCompressor", 0x40, "LSSSSSSS", {{}, {}}},
    {0xa8, "SurroundCompressor", 0x40, "LSSSSSSS", {{}, {}}},
    {0xaa, "StereoPitchShift", 0x24, "", {{}, {}}},
    {0xbd, "Script", 0x08, "n", {{0x04, 4, "h", "items"}, {}}},
    {0xbe, "Vector", 0x20, "SSSh", {{}, {}}},
    {0xbf, "VectorNearestPointOnLine", 0x0c, "LL", {{}, {}}},
    {0xc0, "DopplerShift", 0x14, "SLL", {{}, {}}},
    {0xc1, "DebugPrint", 0x0c, "", {{}, {}}},
    {0xc2, "Sequence", 0x0c, "", {{0x08, 16, "iiii", "items"}, {}}},
    {0xc3, "MonoSpectralReverb", 0x84, "SSSSSSifiiSSifififififif", {{}, {}}},
    {0xc4, "MonoToAmbisonicsMixer", 0x28, "", {{0x24, 4, "i", "inputs"}, {0x26, 20, "iiiii", "channels"}}},
    {0xc5, "AmbisonicsToSurroundMixer", 0x20, "", {{0x1c, 4, "i", "inputs"}, {0x1e, 20, "iiiii", "channels"}}},
    {0xc6, "ImpactFilter", 0x1c, "KSSi", {{}, {}}},
    {0xc7, "MessageDelay", 0x10, "SK", {{}, {}}},
    {0xca, "MonoFlanger", 0x38, "", {{}, {}}},
    {0xcc, "SurroundFlanger", 0x38, "fSSSSifS", {{}, {}}},
    {0xcd, "MonoBitCrusher", 0x1c, "", {{}, {}}},
    {0xcf, "SurroundBitCrusher", 0x1c, "ifSS", {{}, {}}},
    {0xd0, "MonoRingModulator", 0x1c, "SSS", {{}, {}}},
    {0xd2, "SurroundRingModulator", 0x1c, "ifSS", {{}, {}}},
    {0xd6, "MonoSend", 0x18, "SSi", {{}, {}}},
    {0xd7, "StereoSend", 0x18, "SSi", {{}, {}}},
    {0xd9, "MonoReturn", 0x18, "SSn", {{0x14, 4, "i", "items"}, {}}},
    {0xda, "StereoReturn", 0x18, "SSn", {{0x14, 4, "i", "items"}, {}}},
};

const TypeSpec* typeSpec(std::uint32_t id) noexcept
{
    for (const TypeSpec& t : kTypes)
        if (t.id == id)
            return &t;
    return nullptr;
}

std::uint16_t u16(ByteSpan d, std::size_t o) noexcept
{
    return loadScalar<std::uint16_t>(d, o);
}

std::uint32_t u32(ByteSpan d, std::size_t o) noexcept
{
    return loadScalar<std::uint32_t>(d, o);
}

std::size_t layoutBytes(const char* layout) noexcept
{
    std::size_t n = 0;
    for (const char* c = layout; *c; ++c)
        n += *c == 'S' ? 8 : *c == 'u' ? 2 : 4;
    return n;
}

constexpr std::uint32_t kMagic = 0x414f424a; // "JBOA"
constexpr std::size_t kHeader = 0x18;

} // namespace

bool isAudioEntity(ByteSpan data) noexcept
{
    return data.size() >= kHeader && u32(data, 0) == kMagic;
}

Result<AudioEntity> parseAudioEntity(ByteSpan data)
{
    if (!isAudioEntity(data))
        return fail(ErrorCode::InvalidFormat, "not a Blur audio entity (JBOA)");
    if (u16(data, 4) != 11 || u16(data, 6) != 2)
        return fail(ErrorCode::Unsupported, "audio entity version {}.{} (expected 11.2)", u16(data, 4), u16(data, 6));
    AudioEntity e;
    e.poolOffset = u32(data, 8);
    if (e.poolOffset < kHeader || e.poolOffset > data.size())
        return fail(ErrorCode::InvalidFormat, "string pool offset {:#x} is outside the file", e.poolOffset);
    const ByteSpan pool = data.subspan(e.poolOffset);
    auto text = [&](std::uint32_t offset) -> std::string {
        return offset < pool.size() ? std::string(cstringAt(pool, offset)) : std::string("?");
    };
    e.name = text(u32(data, 0x0c));
    const std::uint32_t count = u32(data, 0x10);
    e.updateRate = u32(data, 0x14);
    if (count > (e.poolOffset - kHeader) / 8)
        return fail(ErrorCode::InvalidFormat, "{} components can't fit before the string pool", count);

    std::size_t pos = kHeader;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos + 8 > e.poolOffset)
            return fail(ErrorCode::OutOfBounds, "component {} runs past the string pool", i);
        AudioEntity::Component c;
        c.type = u32(data, pos);
        c.offset = pos;
        c.name = text(u32(data, pos + 4));
        const TypeSpec* spec = typeSpec(c.type);
        if (!spec)
            return fail(ErrorCode::Unsupported, "component {} (\"{}\") has unknown type {:#x}", i, c.name, c.type);
        c.typeName = spec->name;
        const std::size_t rec = pos + 4; // the name field: sizes and count offsets count from here
        std::size_t size = spec->head;
        for (const ListSpec& list : spec->lists) {
            if (!list.item)
                continue;
            if (rec + list.countAt + 2 > e.poolOffset)
                return fail(ErrorCode::OutOfBounds, "component {} is cut short", i);
            size += std::size_t{u16(data, rec + list.countAt)} * list.itemSize;
        }
        c.size = 4 + size;
        if (rec + size > e.poolOffset)
            return fail(ErrorCode::OutOfBounds, "component {} (\"{}\") runs past the string pool", i, c.name);
        pos = rec + size;
        e.components.push_back(std::move(c));
    }
    return e;
}

Result<std::string> decompileAudioEntity(ByteSpan data)
{
    auto parsed = parseAudioEntity(data);
    if (!parsed)
        return std::unexpected(parsed.error());
    const AudioEntity& e = *parsed;
    const ByteSpan pool = data.subspan(e.poolOffset);
    auto text = [&](std::uint32_t offset) -> std::string {
        return offset < pool.size() ? std::format("\"{}\"", cstringAt(pool, offset)) : std::format("?{:#x}", offset);
    };
    auto link = [&](std::uint16_t component, std::uint16_t port) -> std::string {
        if (component == 0xffff)
            return "-";
        if (component < e.components.size() && port > 0xff) // audio links sometimes leave junk in the port half
            return std::format("#{} {}", component, e.components[component].name);
        if (component < e.components.size())
            return std::format("#{}:{} {}", component, port, e.components[component].name);
        return std::format("#{}:{}", component, port);
    };
    auto number = [](float f) { return std::format("{}", f); };

    std::string out;
    out += std::format("// {} — Blur audio entity, decompiled by 26Viewer (read-only).\n", e.name);
    out += "// Components are numbered #0, #1…; \"#14:2 Name\" is output 2 of component #14.\n";
    out += "// A parameter is a constant (= value) or fed by a link (<- #n:port, default value).\n";
    out += "// Field names aren't stored in the game; they're numbered by kind.\n\n";
    out += std::format("entity \"{}\"\n", e.name);
    out += std::format("update_rate {}\n", e.updateRate);
    out += std::format("components {}\n", e.components.size());

    for (std::size_t i = 0; i < e.components.size(); ++i) {
        const AudioEntity::Component& c = e.components[i];
        const TypeSpec* found = typeSpec(c.type);
        if (!found)
            continue; // can't happen: parseAudioEntity rejects unknown types
        const TypeSpec& spec = *found;
        out += std::format("\n#{} {} \"{}\"\n", i, spec.name, c.name);

        // Writes fields laid out as `layout` from `pos` (within `end`); `numbers`
        // keeps per-kind counters so labels read input 1, param 2…
        struct Counters {
            int input = 0, param = 0, value = 0, integer = 0, pair = 0, uses = 0;
        };
        auto fields = [&](const char* layout, std::size_t pos, std::size_t end, Counters& n, const std::string& indent) {
            for (const char* k = layout; *k; ++k) {
                const std::size_t bytes = *k == 'S' ? 8 : *k == 'u' ? 2 : 4;
                if (pos + bytes > end)
                    break;
                switch (*k) {
                case 'L':
                    out += std::format("{}input {:<3} {}\n", indent, ++n.input, link(u16(data, pos), u16(data, pos + 2)));
                    break;
                case 'S': {
                    const std::uint16_t from = u16(data, pos);
                    const float value = loadScalar<float>(data, pos + 4);
                    if (from == 0xffff)
                        out += std::format("{}param {:<3} = {}\n", indent, ++n.param, number(value));
                    else
                        out += std::format("{}param {:<3} <- {} (default {})\n", indent, ++n.param,
                                           link(from, u16(data, pos + 2)), number(value));
                    break;
                }
                case 'K': {
                    const std::uint32_t target = u32(data, pos);
                    out += std::format("{}uses {:<4} {}\n", indent, ++n.uses,
                                       target < e.components.size()
                                           ? std::format("#{} {}", target, e.components[target].name)
                                           : std::to_string(target));
                    break;
                }
                case 'f':
                    out += std::format("{}value {:<3} {}\n", indent, ++n.value, number(loadScalar<float>(data, pos)));
                    break;
                case 'i':
                    out += std::format("{}int {:<5} {}\n", indent, ++n.integer, static_cast<std::int32_t>(u32(data, pos)));
                    break;
                case 'h': {
                    auto half = [](std::uint16_t v) { return v == 0xffff ? std::string("-") : std::to_string(v); };
                    out += std::format("{}pair {:<4} {}, {}\n", indent, ++n.pair, half(u16(data, pos)),
                                       half(u16(data, pos + 2)));
                    break;
                }
                case 'u':
                    out += std::format("{}{}\n", indent, u16(data, pos));
                    break;
                case 'b':
                    out += std::format("{}bank      {}\n", indent, text(u32(data, pos)));
                    break;
                case 'w':
                    out += std::format("{}wave      {}\n", indent, text(u32(data, pos)));
                    break;
                case 'n':
                case 'c':
                    break; // list counts: shown with their lists
                }
                pos += bytes;
            }
            return pos;
        };

        const std::size_t rec = c.offset + 4;
        const std::size_t headEnd = rec + spec.head;
        Counters counters;
        std::size_t pos = fields(spec.layout, rec + 4, headEnd, counters, "    ");
        for (; pos + 4 <= headEnd; pos += 4) // bytes no layout covers (types the game never uses)
            out += std::format("    raw       {:08x}\n", u32(data, pos));

        std::size_t listPos = headEnd;
        for (const ListSpec& list : spec.lists) {
            if (!list.item)
                continue;
            const std::uint16_t count = u16(data, rec + list.countAt);
            out += std::format("    {} {}\n", list.label, count);
            for (std::uint16_t k = 0; k < count; ++k) {
                if (layoutBytes(list.item) == 2 && list.itemSize == 2) {
                    out += std::format("      [{}] {}\n", k, u16(data, listPos));
                } else {
                    out += std::format("      [{}]\n", k);
                    Counters itemCounters;
                    fields(list.item, listPos, listPos + list.itemSize, itemCounters, "        ");
                }
                listPos += list.itemSize;
            }
        }
    }
    return out;
}

bool AudioEntityDecompiler::canDecompile(std::string_view, ByteSpan data) const
{
    return isAudioEntity(data);
}

bool AudioEntityStructureParser::canParse(std::string_view, ByteSpan data) const
{
    return isAudioEntity(data);
}

Result<StructureNode> AudioEntityStructureParser::parse(ByteSpan data) const
{
    auto e = parseAudioEntity(data);
    if (!e)
        return std::unexpected(e.error());
    StructureNode root{"Audio entity", std::format("\"{}\" · {} components", e->name, e->components.size()), 0,
                       data.size(), {}};
    root.children.push_back({"Header", std::format("JBOA v11 · update rate {}", e->updateRate), 0, 0x18, {}});
    StructureNode list{"Components", std::format("{}", e->components.size()), 0x18, e->poolOffset - 0x18, {}};
    for (std::size_t i = 0; i < e->components.size(); ++i) {
        const auto& c = e->components[i];
        list.children.push_back({std::format("#{} {}", i, c.typeName), c.name, c.offset, c.size, {}});
    }
    root.children.push_back(std::move(list));
    root.children.push_back(
        {"Strings", "names of the entity and its components", e->poolOffset, data.size() - e->poolOffset, {}});
    return root;
}

} // namespace viewer::games::blur
