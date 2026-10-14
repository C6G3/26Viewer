// Blur's data tables ("Flask" packs; little-endian). From Blur.exe: header
// check 0x584700, loader 0x585660, record reader 0x584CD0, text unscrambling
// 0x5871B0 (the schema builder at 0x1F04BB0 sits in the protected section).
//
//   0x00 "KSLF", u32 version (1, or 2: names and texts scrambled)
//   0x08 eight sections, each u32 offset, u32 count:
//     types   x 12: u16 name, u16 fields, u16 bases, u16 kind (0 value, 1 enum,
//                   2 flags, 3 struct), u16 value type, u16 size
//     bases   x 4:  u16 type, u16 offset (the types each type builds on, in order)
//     fields  x 8:  u16 name, u16 type, u16 offset, u8 kind, u8 array
//     names:        `count` bytes of 0-terminated names
//     records x 8:  u32 id (sorted), u16 type, u16 block (0xFFFF: another table's)
//     blocks  x 16: u32 record, u32 layouts, u32 object bytes, u32 text bytes
//     layouts x 6:  u16 type, u16 count, u16 kind (0 objects, 1 pointers, 2 records)
//     data:         per block, its objects (layouts one after another, each
//                   starting at a multiple of 4) and then its texts
// Values take 4 bytes (8 for 64-bit ones); a text is an offset into the block's
// texts; an array is u16 layout, u16 first element, u32 count; a pointer is
// u16 layout, u16 element; a record reference is the record's index.

#include "FlaskTable.hpp"
#include "Localisation.hpp"

#include "viewer/core/TextEdit.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace viewer::games::blur {

namespace {

constexpr std::uint32_t kMagic = 0x464C534B; // "KSLF"
constexpr std::uint32_t kNoObject = 0xFFFFFFFF; ///< A null pointer or record reference.
constexpr int kMaxDepth = 48;

std::string nameAt(const std::string& names, std::size_t offset)
{
    if (offset >= names.size())
        return "?";
    const std::size_t end = names.find('\0', offset);
    return names.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
}

/// Calls `visit(field, offset)` for each field of a struct, its base types' first.
template <typename Visit>
void eachField(const FlaskPack& pack, std::size_t t, std::size_t offset, Visit&& visit, int depth = 0)
{
    if (depth > kMaxDepth)
        return;
    const FlaskPack::Type& ty = pack.types[t];
    for (std::size_t i = 0; i < ty.baseCount; ++i) {
        const FlaskPack::Base& base = pack.bases[ty.firstBase + i];
        eachField(pack, base.type, offset + base.offset, visit, depth + 1);
    }
    for (std::size_t i = 0; i < ty.fieldCount; ++i)
        visit(pack.fields[ty.firstField + i], offset);
}

/// The object's "name" text, if it has one.
std::optional<std::string> objectName(const FlaskPack& pack, const FlaskPack::Block& b, std::size_t t, std::size_t offset)
{
    std::optional<std::string> name;
    eachField(pack, t, offset, [&](const FlaskPack::Field& f, std::size_t at) {
        if (name || f.array || f.kind != 0 || pack.types[f.type].primitive != 12 || (f.name != "name" && f.name != "Name"))
            return;
        const std::size_t p = at + f.offset;
        if (p + 4 > b.objects.size())
            return;
        const std::uint32_t s = loadScalar<std::uint32_t>(b.objects, p);
        if (s < b.strings.size() && b.strings[s] != '\0')
            name = nameAt(b.strings, s);
    });
    return name;
}

/// `AmaxVehicle "Audi_S3_Stock"` for a record of this pack that has a name.
std::optional<std::string> namedLabel(const FlaskPack& pack, std::size_t index)
{
    const FlaskPack::Record& r = pack.records[index];
    if (r.block >= pack.blocks.size())
        return std::nullopt;
    const FlaskPack::Block& b = pack.blocks[r.block];
    if (auto name = objectName(pack, b, b.layouts[0].type, b.layouts[0].start))
        return std::format("{} {}", pack.types[r.type].name, quotedText(*name));
    return std::nullopt;
}

/// Where an editable value of the text lives in the pack.
struct Slot {
    std::size_t block = 0;
    std::size_t offset = 0; ///< In the block's objects.
    std::uint16_t type = 0;
    std::uint8_t kind = 0;  ///< 0 value, 1 enum, 2 flags, 4 object reference, 5 record reference
};

/// Where a list (array field) of the text lives: its header in a block.
struct ListSlot {
    std::size_t block = 0;
    std::size_t offset = 0; ///< Of the header (u16 layout, u16 first, u32 count) in the block's objects.
    std::size_t field = 0;  ///< Index into FlaskPack::fields.
};

/// Writes one block's objects as text, each editable value marked (EditableText).
class Printer {
public:
    /// `texts`: LocId -> English text; `elsewhere`: record id -> label, for
    /// the records of other tables.
    Printer(const FlaskPack& pack, const std::unordered_map<std::uint32_t, std::string>& texts,
            const std::unordered_map<std::uint32_t, std::string>& elsewhere, std::vector<Slot>& slots,
            std::vector<ListSlot>& lists)
        : m_pack(pack), m_texts(texts), m_elsewhere(elsewhere), m_slots(slots), m_lists(lists)
    {
    }

    std::string recordLabel(std::size_t index) const
    {
        if (auto label = namedLabel(m_pack, index))
            return *label;
        const FlaskPack::Record& r = m_pack.records[index];
        if (r.block == 0xFFFF)
            if (const auto it = m_elsewhere.find(r.id); it != m_elsewhere.end())
                return it->second;
        return std::format("{} #{:08x}", m_pack.types[r.type].name, r.id);
    }

    void block(std::size_t index, std::string& out)
    {
        const FlaskPack::Block& b = m_pack.blocks[index];
        m_blockIndex = index;
        m_block = &b;
        m_covered.clear();
        m_reached.clear();
        m_printed.clear();
        const FlaskPack::Layout& root = b.layouts[0];
        mark(root.type, root.start, 0);
        out += std::format("{} {{  // record 0x{:08x}\n", recordLabel(b.record), m_pack.records[b.record].id);
        fields(root.type, root.start, 1, out);
        out += "}\n";
    }

private:
    using Element = std::pair<std::size_t, std::size_t>; // layout, index

    const FlaskPack::Type& type(std::size_t t) const { return m_pack.types[t]; }

    bool fits(std::size_t offset, std::size_t size) const
    {
        return offset <= m_block->objects.size() && size <= m_block->objects.size() - offset;
    }
    std::uint32_t u32(std::size_t offset) const
    {
        return fits(offset, 4) ? loadScalar<std::uint32_t>(m_block->objects, offset) : 0;
    }

    /// Where element `index` of `layout` starts, if it exists.
    std::optional<std::size_t> element(std::size_t layout, std::size_t index) const
    {
        if (layout >= m_block->layouts.size())
            return std::nullopt;
        const FlaskPack::Layout& l = m_block->layouts[layout];
        if (index >= l.count)
            return std::nullopt;
        return l.start + index * l.elementSize(m_pack);
    }

    // ---- First pass: which objects arrays hold (those are written there) ----

    void markPointer(std::uint32_t value, int depth)
    {
        const Element e{value & 0xFFFF, value >> 16};
        const auto at = element(e.first, e.second);
        if (!at || !m_reached.insert(e).second)
            return;
        mark(m_block->layouts[e.first].type, *at, depth + 1);
    }

    void mark(std::size_t t, std::size_t offset, int depth)
    {
        if (depth > kMaxDepth || type(t).kind != 3)
            return;
        eachField(m_pack, t, offset, [&](const FlaskPack::Field& f, std::size_t at) {
            const std::size_t p = at + f.offset;
            if (f.array) {
                const std::uint32_t head = u32(p), count = u32(p + 4);
                const std::size_t layout = head & 0xFFFF, first = head >> 16;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto e = element(layout, first + i);
                    if (!e)
                        break;
                    const FlaskPack::Layout& l = m_block->layouts[layout];
                    if (l.kind == 0) {
                        m_covered.insert({layout, first + i});
                        if (f.kind == 3)
                            mark(l.type, *e, depth + 1);
                    } else if (l.kind == 1 && f.kind == 4) {
                        markPointer(u32(*e), depth);
                    }
                }
            } else if (f.kind == 3) {
                mark(f.type, p, depth + 1);
            } else if (f.kind == 4) {
                markPointer(u32(p), depth);
            }
        });
    }

    // ---- Second pass: the text ----

    static std::string indent(int depth) { return std::string(static_cast<std::size_t>(depth) * 4, ' '); }

    /// "name = value" for each field (values written for lines indented `depth` deep).
    std::vector<std::string> fieldTexts(std::size_t t, std::size_t offset, int depth)
    {
        std::vector<std::string> texts;
        eachField(m_pack, t, offset, [&](const FlaskPack::Field& f, std::size_t at) {
            std::string text = f.name + " = ";
            if (f.array)
                array(f, at + f.offset, depth, text);
            else
                value(f.kind, f.type, at + f.offset, depth, text);
            texts.push_back(std::move(text));
        });
        return texts;
    }

    void fields(std::size_t t, std::size_t offset, int depth, std::string& out)
    {
        for (const std::string& text : fieldTexts(t, offset, depth))
            out += indent(depth) + text + '\n';
    }

    void structValue(std::size_t t, std::size_t offset, int depth, std::string& out)
    {
        if (depth > kMaxDepth || !fits(offset, type(t).size)) {
            out += "{ /* damaged */ }";
            return;
        }
        const std::vector<std::string> texts = fieldTexts(t, offset, depth + 1);
        // Small structs of plain values (colours, positions) on one line.
        std::size_t length = type(t).name.size();
        bool oneLine = texts.size() <= 6;
        for (const std::string& text : texts) {
            length += EditableText::visibleLength(text) + 2;
            oneLine = oneLine && text.find('\n') == std::string::npos;
        }
        if (oneLine && length <= 72) {
            out += type(t).name + " {";
            for (std::size_t i = 0; i < texts.size(); ++i)
                out += (i ? ", " : " ") + texts[i];
            out += texts.empty() ? "}" : " }";
            return;
        }
        out += type(t).name + " {\n";
        for (const std::string& text : texts)
            out += indent(depth + 1) + text + '\n';
        out += indent(depth) + "}";
    }

    void array(const FlaskPack::Field& f, std::size_t p, int depth, std::string& out)
    {
        const std::uint32_t head = u32(p), count = u32(p + 4);
        const std::size_t layout = head & 0xFFFF, first = head >> 16;
        if (count != 0 && (!element(layout, first) || !element(layout, first + count - 1))) {
            out += std::format("[ /* {} element(s) out of range */ ]", count);
            return;
        }
        // A list that may get more or fewer items when edited.
        if (fits(p, 8)) {
            out += EditableText::markList(m_lists.size());
            m_lists.push_back({m_blockIndex, p, static_cast<std::size_t>(&f - m_pack.fields.data())});
        }
        if (count == 0) {
            out += "[]";
            return;
        }
        const FlaskPack::Layout& l = m_block->layouts[layout];
        std::vector<std::string> items;
        bool multiLine = false;
        std::size_t length = 0;
        for (std::size_t i = 0; i < count; ++i) {
            std::string item;
            const std::size_t at = *element(layout, first + i);
            if (l.kind == 0)
                value(f.kind, l.type, at, depth + 1, item);
            else
                value(l.kind == 1 ? 4 : 5, l.type, at, depth + 1, item);
            multiLine = multiLine || item.find('\n') != std::string::npos;
            length += EditableText::visibleLength(item) + 2;
            items.push_back(std::move(item));
        }
        if (!multiLine && length < 100) {
            out += '[';
            for (std::size_t i = 0; i < items.size(); ++i)
                out += (i ? ", " : "") + items[i];
            out += ']';
            return;
        }
        out += "[\n";
        for (const std::string& item : items)
            out += indent(depth + 1) + item + "\n";
        out += indent(depth) + "]";
    }

    void value(std::uint8_t kind, std::size_t t, std::size_t p, int depth, std::string& out)
    {
        if (t >= m_pack.types.size()) {
            out += "?";
            return;
        }
        const FlaskPack::Type& ty = type(t);
        // An editable value: marked, and remembered as a slot.
        const auto editable = [&](std::string_view text) {
            if (!fits(p, ty.kind == 0 && ty.size == 8 ? 8 : 4)) {
                out += text;
                return;
            }
            out += EditableText::mark(text, m_slots.size());
            m_slots.push_back({m_blockIndex, p, static_cast<std::uint16_t>(t), kind});
        };
        switch (kind) {
        case 0: {
            const bool wide = ty.size == 8;
            if (!fits(p, wide ? 8 : 4)) {
                out += "?";
                return;
            }
            const std::uint32_t v = u32(p);
            const std::uint64_t v64 = wide ? loadScalar<std::uint64_t>(m_block->objects, p) : v;
            switch (ty.primitive) {
            case 1: editable(v ? "true" : "false"); break;
            case 2:
            case 4: editable(std::to_string(static_cast<std::int32_t>(v))); break;
            case 5: editable(std::to_string(static_cast<std::int64_t>(v64))); break;
            case 10: editable(std::format("{}", std::bit_cast<float>(v))); break;
            case 11: editable(std::format("{}", std::bit_cast<double>(v64))); break;
            case 12:
                if (v < m_block->strings.size())
                    editable(quotedText(nameAt(m_block->strings, v)));
                else
                    out += "null";
                break;
            case 14:
                editable(v == 0 ? std::string("0") : std::format("0x{:08x}", v));
                if (const auto it = m_texts.find(v); v != 0 && it != m_texts.end())
                    out += std::format(" /* {} */", quotedText(it->second));
                break;
            default: editable(std::to_string(v64)); break;
            }
            return;
        }
        case 1: { // enum: the item's index
            const std::uint32_t v = u32(p);
            editable(v < ty.fieldCount ? m_pack.fields[ty.firstField + v].name : std::format("{}({})", ty.name, v));
            return;
        }
        case 2: { // flags: one bit per item
            const std::uint32_t v = u32(p);
            std::string names;
            for (std::size_t i = 0; i < ty.fieldCount && i < 32; ++i)
                if (v >> i & 1)
                    names += (names.empty() ? "" : " | ") + m_pack.fields[ty.firstField + i].name;
            const std::uint32_t unknown = ty.fieldCount >= 32 ? 0 : v >> ty.fieldCount;
            if (unknown)
                names += std::format("{}0x{:x}", names.empty() ? "" : " | ", unknown << ty.fieldCount);
            editable(names.empty() ? "0" : names);
            return;
        }
        case 3: structValue(t, p, depth, out); return;
        case 4: { // pointer to an object of this record
            const std::uint32_t v = u32(p);
            const Element e{v & 0xFFFF, v >> 16};
            const auto at = element(e.first, e.second);
            if (!at) {
                editable("null");
                return;
            }
            const std::size_t target = m_block->layouts[e.first].type;
            if (!m_covered.contains(e) && m_printed.insert(e).second) {
                structValue(target, *at, depth, out);
                return;
            }
            // A reference to an object shown elsewhere: which one can be changed.
            std::string ref = std::format("-> {}[{}]", type(target).name, e.second);
            if (auto name = objectName(m_pack, *m_block, target, *at))
                ref += " " + quotedText(*name);
            editable(ref);
            return;
        }
        case 5: { // another record
            const std::uint32_t v = u32(p);
            editable(v < m_pack.records.size() ? "-> " + recordLabel(v) : std::string("null"));
            return;
        }
        default: out += std::format("? /* kind {} */", kind); return;
        }
    }

    const FlaskPack& m_pack;
    const std::unordered_map<std::uint32_t, std::string>& m_texts;
    const std::unordered_map<std::uint32_t, std::string>& m_elsewhere;
    std::vector<Slot>& m_slots;
    std::vector<ListSlot>& m_lists;
    std::size_t m_blockIndex = 0;
    const FlaskPack::Block* m_block = nullptr;
    std::set<Element> m_covered; ///< Held by an array (written there).
    std::set<Element> m_reached; ///< Seen while marking.
    std::set<Element> m_printed; ///< Already written in full.
};

} // namespace

std::string unscrambleFlaskText(ByteSpan data)
{
    // Blur.exe 0x5871B0: every byte but the last, which becomes the terminating 0.
    std::string out(data.size(), '\0');
    std::uint8_t key = 0xCC;
    for (std::size_t i = 0; i + 1 < data.size(); ++i) {
        const auto c = static_cast<std::uint8_t>(key - (std::to_integer<std::uint8_t>(data[i]) ^ 0xB3));
        out[i] = static_cast<char>(c);
        key = static_cast<std::uint8_t>(key + c - static_cast<std::uint8_t>(i));
    }
    return out;
}

Result<FlaskPack> readFlask(ByteSpan d)
{
    if (d.size() < 0x48 || loadScalar<std::uint32_t>(d, 0) != kMagic)
        return fail(ErrorCode::InvalidFormat, "not a Blur data table");
    FlaskPack pack;
    pack.version = loadScalar<std::uint32_t>(d, 4);
    if (pack.version != 1 && pack.version != 2)
        return fail(ErrorCode::Unsupported, "data table version {} isn't supported", pack.version);

    constexpr std::size_t kSizes[8] = {12, 4, 8, 1, 8, 16, 6, 1};
    std::size_t offset[8], count[8];
    for (std::size_t s = 0; s < 8; ++s) {
        offset[s] = loadScalar<std::uint32_t>(d, 8 + 8 * s);
        count[s] = loadScalar<std::uint32_t>(d, 12 + 8 * s);
        if (offset[s] > d.size() || count[s] > (d.size() - offset[s]) / kSizes[s])
            return fail(ErrorCode::InvalidFormat, "data table section {} is past the end", s);
        pack.sectionOffset[s] = offset[s];
        pack.sectionCount[s] = count[s];
    }
    const auto damaged = [](std::string_view what) {
        return fail(ErrorCode::InvalidFormat, "data table is damaged ({})", what);
    };

    const ByteSpan rawNames = d.subspan(offset[3], count[3]);
    const std::string names = pack.version == 2 ? unscrambleFlaskText(rawNames)
                                                : std::string(reinterpret_cast<const char*>(rawNames.data()), rawNames.size());

    std::size_t fieldsUsed = 0, basesUsed = 0;
    for (std::size_t i = 0; i < count[0]; ++i) {
        const std::size_t p = offset[0] + 12 * i;
        FlaskPack::Type t;
        t.name = nameAt(names, loadScalar<std::uint16_t>(d, p));
        t.fieldCount = loadScalar<std::uint16_t>(d, p + 2);
        t.baseCount = loadScalar<std::uint16_t>(d, p + 4);
        t.kind = loadScalar<std::uint16_t>(d, p + 6);
        t.primitive = loadScalar<std::uint16_t>(d, p + 8);
        t.size = loadScalar<std::uint16_t>(d, p + 10);
        t.firstField = fieldsUsed;
        t.firstBase = basesUsed;
        fieldsUsed += t.fieldCount;
        basesUsed += t.baseCount;
        if (t.kind > 3)
            return damaged("type kind");
        pack.types.push_back(std::move(t));
    }
    if (fieldsUsed != count[2] || basesUsed != count[1])
        return damaged("field count");
    for (std::size_t i = 0; i < count[1]; ++i) {
        const std::size_t p = offset[1] + 4 * i;
        pack.bases.push_back({loadScalar<std::uint16_t>(d, p), loadScalar<std::uint16_t>(d, p + 2)});
        if (pack.bases.back().type >= pack.types.size())
            return damaged("base type");
    }
    for (std::size_t i = 0; i < count[2]; ++i) {
        const std::size_t p = offset[2] + 8 * i;
        FlaskPack::Field f;
        f.name = nameAt(names, loadScalar<std::uint16_t>(d, p));
        f.type = loadScalar<std::uint16_t>(d, p + 2);
        f.offset = loadScalar<std::uint16_t>(d, p + 4);
        f.kind = std::to_integer<std::uint8_t>(d[p + 6]);
        f.array = d[p + 7] != std::byte{0};
        if (f.kind > 6 || (f.kind != 6 && f.type >= pack.types.size()))
            return damaged("field type");
        pack.fields.push_back(std::move(f));
    }
    for (std::size_t i = 0; i < count[4]; ++i) {
        const std::size_t p = offset[4] + 8 * i;
        pack.records.push_back(
            {loadScalar<std::uint32_t>(d, p), loadScalar<std::uint16_t>(d, p + 4), loadScalar<std::uint16_t>(d, p + 6)});
        const FlaskPack::Record& r = pack.records.back();
        if (r.type >= pack.types.size() || (r.block != 0xFFFF && r.block >= count[5]))
            return damaged("record");
    }

    const ByteSpan data = d.subspan(offset[7], count[7]);
    std::size_t layoutsUsed = 0, dataUsed = 0;
    for (std::size_t i = 0; i < count[5]; ++i) {
        const std::size_t p = offset[5] + 16 * i;
        FlaskPack::Block b;
        b.record = loadScalar<std::uint32_t>(d, p);
        const std::size_t layouts = loadScalar<std::uint32_t>(d, p + 4);
        const std::size_t objectBytes = loadScalar<std::uint32_t>(d, p + 8), textBytes = loadScalar<std::uint32_t>(d, p + 12);
        if (b.record >= pack.records.size() || layouts == 0 || layouts > count[6] - layoutsUsed
            || objectBytes > data.size() - dataUsed || textBytes > data.size() - dataUsed - objectBytes)
            return damaged("block");
        b.objects = data.subspan(dataUsed, objectBytes);
        const ByteSpan text = data.subspan(dataUsed + objectBytes, textBytes);
        b.rawText = text;
        b.strings = pack.version == 2 ? unscrambleFlaskText(text)
                                      : std::string(reinterpret_cast<const char*>(text.data()), text.size());
        dataUsed += objectBytes + textBytes;
        std::size_t at = 0;
        for (std::size_t l = 0; l < layouts; ++l) {
            const std::size_t q = offset[6] + 6 * (layoutsUsed + l);
            FlaskPack::Layout layout{loadScalar<std::uint16_t>(d, q), loadScalar<std::uint16_t>(d, q + 2),
                                     loadScalar<std::uint16_t>(d, q + 4), 0};
            if (layout.type >= pack.types.size() || layout.kind > 2)
                return damaged("layout");
            at = (at + 3) & ~std::size_t{3};
            layout.start = at;
            at += std::size_t{layout.count} * layout.elementSize(pack);
            if (at > objectBytes)
                return damaged("layout size");
            b.layouts.push_back(layout);
        }
        layoutsUsed += layouts;
        if (pack.types[b.layouts[0].type].kind != 3 || b.layouts[0].count == 0)
            return damaged("record object");
        pack.blocks.push_back(std::move(b));
    }
    return pack;
}

bool FlaskDecompiler::canDecompile(std::string_view, ByteSpan data) const
{
    return data.size() >= 0x48 && loadScalar<std::uint32_t>(data, 0) == kMagic;
}


namespace {

/// A pack as text, with where each editable value lives.
struct Decompiled {
    EditableText text;
    std::vector<Slot> slots;
    std::vector<ListSlot> lists;
    std::vector<std::size_t> ids;    ///< Per value in `text`: its slot.
    std::vector<std::string> labels; ///< Per record, as references show it (when asked for).
};

Decompiled decompilePack(const FlaskPack& pack, const DecompileContext& context, bool withLabels)
{
    // Names from around the file: English texts for text ids, and the names of
    // records other tables define (the game's ids are shared by all tables).
    const bool usesTexts = std::ranges::any_of(pack.types, [](const FlaskPack::Type& t) {
        return t.kind == 0 && t.primitive == 14;
    });
    std::unordered_map<std::uint32_t, std::string> texts, elsewhere;
    if (context.readFolder && usesTexts)
        texts = englishTexts(context.readFolder("gamedata/xt/loc"));
    if (context.readFolder && pack.records.size() > pack.blocks.size()) {
        for (const DecompileContext::File& file : context.readFolder("gamedata/xt")) {
            auto other = readFlask(file.data);
            if (!other)
                continue;
            for (const FlaskPack::Block& b : other->blocks)
                if (auto label = namedLabel(*other, b.record))
                    elsewhere.emplace(other->records[b.record].id, std::move(*label));
        }
    }

    std::map<std::string, std::size_t> kinds;
    for (const FlaskPack::Block& b : pack.blocks)
        ++kinds[pack.types[pack.records[b.record].type].name];
    std::string summary;
    for (const auto& [name, n] : kinds)
        summary += std::format("{}{} x{}", summary.empty() ? "" : ", ", name, n);
    std::string out = std::format("// Blur data table: {} record(s){}{}\n", pack.blocks.size(),
                                  summary.empty() ? "" : ": ", summary);
    if (const std::size_t external = pack.records.size() - pack.blocks.size())
        out += std::format("// It also refers to {} record(s) defined in other tables{}.\n", external,
                           elsewhere.empty() ? " (shown as #id)" : " (named from gamedata/xt where they have a name)");
    if (!texts.empty())
        out += "// Text ids are followed by their English text.\n";

    Decompiled d;
    Printer printer(pack, texts, elsewhere, d.slots, d.lists);
    for (std::size_t b = 0; b < pack.blocks.size(); ++b) {
        out += '\n';
        printer.block(b, out);
    }
    d.text = EditableText::finish(out, &d.ids);
    if (withLabels)
        for (std::size_t r = 0; r < pack.records.size(); ++r)
            d.labels.push_back(printer.recordLabel(r));
    return d;
}

/// "Name" without the spaces around it.
std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

/// The bytes a block's texts are stored as.
ByteBuffer storedText(const FlaskPack& pack, const std::string& text)
{
    ByteBuffer out(text.size());
    if (pack.version != 2) {
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }
    std::uint8_t key = 0xCC;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<std::uint8_t>(text[i]);
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(key - c) ^ 0xB3);
        key = static_cast<std::uint8_t>(key + c - static_cast<std::uint8_t>(i));
    }
    return out;
}

} // namespace

Result<std::string> FlaskDecompiler::decompile(ByteSpan data) const
{
    return decompileWith(data, {});
}

Result<std::string> FlaskDecompiler::decompileWith(ByteSpan data, const DecompileContext& context) const
{
    auto pack = readFlask(data);
    if (!pack)
        return std::unexpected(pack.error());
    return std::move(decompilePack(*pack, context, false).text.text);
}

/// Writes the values changed in `text` into `original` (whose lists already
/// have the edited text's lengths).
static Result<ByteBuffer> applyValues(ByteSpan original, std::string_view text, const DecompileContext& context)
{
    auto pack = readFlask(original);
    if (!pack)
        return std::unexpected(pack.error());
    const Decompiled d = decompilePack(*pack, context, true);
    auto changes = changedValues(d.text, text);
    if (!changes)
        return std::unexpected(changes.error());
    if (changes->empty())
        return ByteBuffer(original.begin(), original.end());

    // Working copies of the blocks that change.
    std::vector<std::optional<ByteBuffer>> objects(pack->blocks.size());
    std::vector<std::optional<std::string>> texts(pack->blocks.size());
    std::unordered_map<std::string, std::size_t> byLabel;

    for (const ChangedValue& change : *changes) {
        const Slot& slot = d.slots[d.ids[change.index]];
        const FlaskPack::Block& block = pack->blocks[slot.block];
        const FlaskPack::Type& type = pack->types[slot.type];
        const std::string_view v = trimmed(change.text);
        const auto bad = [&](std::string_view why) {
            return fail(ErrorCode::InvalidFormat, "line {}: \"{}\" {}", change.line, v, why);
        };
        if (!objects[slot.block])
            objects[slot.block] = ByteBuffer(block.objects.begin(), block.objects.end());
        ByteBuffer& o = *objects[slot.block];
        const auto put = [&](auto value) { storeScalar(o, slot.offset, value); };

        if (slot.kind == 0) {
            switch (type.primitive) {
            case 1:
                if (v != "true" && v != "false")
                    return bad("isn't true or false");
                put(std::uint32_t{v == "true"});
                break;
            case 2:
            case 4: {
                const auto n = parseInteger(v);
                const long long lo = type.primitive == 2 ? -128 : std::numeric_limits<std::int32_t>::min(), hi = type.primitive == 2 ? 127 : std::numeric_limits<std::int32_t>::max();
                if (!n || *n < lo || *n > hi)
                    return bad(std::format("isn't a whole number from {} to {}", lo, hi));
                put(static_cast<std::int32_t>(*n));
                break;
            }
            case 5: {
                const auto n = parseInteger(v);
                if (!n)
                    return bad("isn't a whole number");
                put(static_cast<std::int64_t>(*n));
                break;
            }
            case 6:
            case 7:
            case 8:
            case 14: {
                const auto n = parseUnsigned(v);
                const unsigned long long hi = type.primitive == 6 ? 255 : type.primitive == 7 ? 65535 : 0xFFFFFFFFull;
                if (!n || *n > hi)
                    return bad(std::format("isn't a whole number from 0 to {}", hi));
                put(static_cast<std::uint32_t>(*n));
                break;
            }
            case 9: {
                const auto n = parseUnsigned(v);
                if (!n)
                    return bad("isn't a whole number from 0 up");
                put(static_cast<std::uint64_t>(*n));
                break;
            }
            case 10:
            case 11: {
                const auto n = parseReal(v);
                if (!n)
                    return bad("isn't a number (use a dot for decimals: 1.25)");
                if (type.primitive == 10)
                    put(static_cast<float>(*n));
                else
                    put(*n);
                break;
            }
            case 12: {
                const auto s = unquote(v);
                if (!s)
                    return bad("isn't a text in double quotes");
                if (s->find('\0') != std::string::npos)
                    return bad("can't hold a \\x00 character");
                // Texts are shared: reuse an equal one, else add it after the others.
                std::string& t = texts[slot.block] ? *texts[slot.block] : texts[slot.block].emplace(block.strings);
                std::size_t at = std::string::npos;
                for (std::size_t p = t.find(*s + '\0'); p != std::string::npos; p = t.find(*s + '\0', p + 1))
                    if (p == 0 || t[p - 1] == '\0') {
                        at = p;
                        break;
                    }
                if (at == std::string::npos) {
                    at = t.size();
                    t += *s;
                    t += '\0';
                    while (t.size() % 4)
                        t += '\0';
                }
                put(static_cast<std::uint32_t>(at));
                break;
            }
            default: {
                const auto n = parseUnsigned(v);
                if (!n || *n > 0xFFFFFFFFull)
                    return bad("isn't a whole number");
                put(static_cast<std::uint32_t>(*n));
                break;
            }
            }
        } else if (slot.kind == 1) { // enum: an item's name, Type(n) or n
            std::optional<std::uint32_t> index;
            for (std::size_t i = 0; i < type.fieldCount; ++i)
                if (pack->fields[type.firstField + i].name == v)
                    index = static_cast<std::uint32_t>(i);
            if (!index && v.starts_with(type.name + "(") && v.ends_with(')'))
                if (auto n = parseUnsigned(v.substr(type.name.size() + 1, v.size() - type.name.size() - 2)); n && *n <= 0xFFFFFFFFull)
                    index = static_cast<std::uint32_t>(*n);
            if (!index) {
                std::string choices;
                for (std::size_t i = 0; i < type.fieldCount; ++i)
                    choices += (i ? ", " : "") + pack->fields[type.firstField + i].name;
                return bad(std::format("isn't one of: {}", choices));
            }
            put(*index);
        } else if (slot.kind == 2) { // flags: names (and 0x… for unnamed bits) joined by |
            std::uint32_t bits = 0;
            std::string_view rest = v;
            while (!rest.empty()) {
                const std::size_t bar = rest.find('|');
                const std::string_view part = trimmed(rest.substr(0, bar));
                rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
                bool found = false;
                for (std::size_t i = 0; i < type.fieldCount && i < 32; ++i)
                    if (pack->fields[type.firstField + i].name == part) {
                        bits |= 1u << i;
                        found = true;
                    }
                if (!found) {
                    const auto n = parseUnsigned(part);
                    if (!n || *n > 0xFFFFFFFFull) {
                        std::string choices;
                        for (std::size_t i = 0; i < type.fieldCount; ++i)
                            choices += (i ? ", " : "") + pack->fields[type.firstField + i].name;
                        return bad(std::format("has \"{}\", which isn't one of: {} (join them with |, or write 0)",
                                               part, choices));
                    }
                    bits |= static_cast<std::uint32_t>(*n);
                }
            }
            put(bits);
        } else if (slot.kind == 4) { // an object of this record: "-> Type[n]" or null
            if (v == "null") {
                put(kNoObject);
                continue;
            }
            const std::string_view ref = trimmed(v.starts_with("->") ? v.substr(2) : v);
            const std::size_t open = ref.find('['), close = ref.find(']');
            const auto index = open != std::string_view::npos && close > open
                                   ? parseUnsigned(ref.substr(open + 1, close - open - 1))
                                   : std::nullopt;
            if (!index)
                return bad("isn't a reference like -> Type[3] (or null)");
            const std::string_view name = trimmed(ref.substr(0, open));
            // The layout of objects of that type: the one pointed into now, else the only one.
            const std::uint32_t now = loadScalar<std::uint32_t>(o, slot.offset);
            std::optional<std::size_t> layout;
            if (now != kNoObject && (now & 0xFFFF) < block.layouts.size()
                && pack->types[block.layouts[now & 0xFFFF].type].name == name)
                layout = now & 0xFFFF;
            for (std::size_t l = 0; l < block.layouts.size() && !layout; ++l)
                if (block.layouts[l].kind == 0 && pack->types[block.layouts[l].type].name == name) {
                    for (std::size_t m = l + 1; m < block.layouts.size(); ++m)
                        if (block.layouts[m].kind == 0 && pack->types[block.layouts[m].type].name == name)
                            return bad("is ambiguous: this record keeps several groups of that type");
                    layout = l;
                }
            if (!layout)
                return bad(std::format("names a type this record has no objects of ({})", name));
            if (*index >= block.layouts[*layout].count)
                return bad(std::format("is past the last {} of this record ({})", name, block.layouts[*layout].count - 1));
            put(static_cast<std::uint32_t>(*layout | *index << 16));
        } else { // another record
            if (v == "null") {
                put(std::uint32_t{0xFFFFFFFF});
                continue;
            }
            if (byLabel.empty())
                for (std::size_t r = 0; r < d.labels.size(); ++r) {
                    byLabel.emplace(d.labels[r], r);
                    byLabel.emplace(std::format("{} #{:08x}", pack->types[pack->records[r].type].name,
                                                pack->records[r].id), r);
                }
            std::string wanted(trimmed(v.starts_with("->") ? v.substr(2) : v));
            const auto it = byLabel.find(wanted);
            if (it == byLabel.end())
                return bad("isn't a record this table refers to. Write it like the others "
                           "(-> Type \"name\" or -> Type #id); a table can only point at records it already lists");
            put(static_cast<std::uint32_t>(it->second));
        }
    }

    // The data section again: blocks one after another, each its objects then its texts.
    ByteBuffer out(original.begin(), original.begin() + pack->sectionOffset[7]);
    for (std::size_t b = 0; b < pack->blocks.size(); ++b) {
        const FlaskPack::Block& block = pack->blocks[b];
        const ByteSpan obj = objects[b] ? ByteSpan(*objects[b]) : block.objects;
        out.insert(out.end(), obj.begin(), obj.end());
        if (texts[b]) {
            const ByteBuffer stored = storedText(*pack, *texts[b]);
            out.insert(out.end(), stored.begin(), stored.end());
            storeScalar(out, pack->sectionOffset[5] + 16 * b + 12, static_cast<std::uint32_t>(stored.size()));
        } else {
            out.insert(out.end(), block.rawText.begin(), block.rawText.end());
        }
    }
    // Anything after the last block (and after the data section) stays as it was.
    const std::size_t blocksEnd = pack->blocks.empty() ? pack->sectionOffset[7]
                                                       : static_cast<std::size_t>(pack->blocks.back().rawText.data() + pack->blocks.back().rawText.size() - original.data());
    out.insert(out.end(), original.begin() + static_cast<std::ptrdiff_t>(blocksEnd), original.end());
    const std::size_t grown = out.size() - original.size();
    if (grown && pack->sectionOffset[7] + pack->sectionCount[7] != original.size())
        return fail(ErrorCode::Unsupported, "this table keeps data after its records, so its texts can't grow");
    storeScalar(out, 8 + 8 * 7 + 4, static_cast<std::uint32_t>(pack->sectionCount[7] + grown));
    if (auto check = readFlask(out); !check)
        return fail(ErrorCode::InvalidFormat, "the rebuilt table doesn't read back: {}", check.error().message);
    return out;
}

namespace {

// ---- Lists that grow or shrink -----------------------------------------------------------

/// A block taken apart: each layout's elements, so elements can be added and removed.
struct EditBlock {
    std::vector<FlaskPack::Layout> layouts;
    std::vector<std::vector<ByteBuffer>> items;
    std::string strings;
    bool stringsChanged = false;

    EditBlock(const FlaskPack& pack, const FlaskPack::Block& b) : layouts(b.layouts), strings(b.strings)
    {
        for (const FlaskPack::Layout& l : layouts) {
            const std::size_t size = l.elementSize(pack);
            auto& list = items.emplace_back();
            for (std::size_t i = 0; i < l.count; ++i) {
                const ByteSpan e = b.objects.subspan(l.start + i * size, size);
                list.emplace_back(e.begin(), e.end());
            }
        }
    }

    /// The objects again: layouts one after another, each starting on a multiple of 4.
    [[nodiscard]] ByteBuffer objects() const
    {
        ByteBuffer out;
        for (const auto& list : items) {
            out.resize((out.size() + 3) & ~std::size_t{3});
            for (const ByteBuffer& e : list)
                out.insert(out.end(), e.begin(), e.end());
        }
        return out;
    }
};

/// An element's place: layout, element.
using Place = std::pair<std::size_t, std::size_t>;

constexpr std::uint32_t kNone = 0xFFFFFFFF;

/// Calls `visit(field, offset)` for every field of a struct that isn't an inline
/// struct itself (those are entered), including the base types' fields.
template <typename Visit>
void eachLeaf(const FlaskPack& pack, std::size_t t, std::size_t offset, Visit&& visit, int depth = 0)
{
    if (depth > kMaxDepth)
        return;
    eachField(pack, t, offset, [&](const FlaskPack::Field& f, std::size_t at) {
        if (!f.array && f.kind == 3 && f.type < pack.types.size() && pack.types[f.type].kind == 3)
            eachLeaf(pack, f.type, at + f.offset, visit, depth + 1);
        else
            visit(f, at + f.offset);
    });
}

class ListEditor {
public:
    ListEditor(const FlaskPack& pack, EditBlock& block) : m_pack(pack), m_b(block) {}

    /// Gives the list whose header is at `header` (layout, element, offset in the
    /// element) `wanted` items: removes from its end, or adds copies of its last
    /// item (with everything the item owns copied too).
    Result<void> resize(Place element, std::size_t offset, const FlaskPack::Field& field, std::size_t wanted)
    {
        std::uint16_t layout = u16(element, offset);
        const std::uint16_t first = u16(element, offset + 2);
        const std::uint32_t count = u32(element, offset + 4);
        if (wanted == count)
            return {};
        if (wanted > 65535)
            return fail(ErrorCode::InvalidFormat, "a list can't hold more than 65535 items");

        if (wanted < count) {
            const std::size_t at = first + wanted, removed = count - wanted;
            if (layout >= m_b.items.size() || at + removed > m_b.items[layout].size())
                return fail(ErrorCode::InvalidFormat, "the list's items are out of range");
            // What the removed items hold goes with them — unless something else
            // in the record points at it (then it can't go).
            findOwners();
            std::set<Place> held;
            for (std::size_t i = at; i < at + removed; ++i)
                collectHeld({layout, i}, held, 0);
            std::string pointedAt;
            eachReference([](Place, std::size_t) {},
                          [&](Place p, std::size_t o) {
                              const std::uint32_t v = u32(p, o);
                              const Place target{v & 0xFFFF, v >> 16};
                              if (pointedAt.empty() && v != kNone && held.contains(target) && !held.contains(p))
                                  pointedAt = std::format("{}[{}]", m_pack.types[m_b.layouts[target.first].type].name,
                                                          target.second);
                          });
            if (!pointedAt.empty())
                return fail(ErrorCode::Unsupported,
                            "this list can't get shorter: other parts of the record point at what its last "
                            "item(s) hold (-> {}), so those items have to stay",
                            pointedAt);
            set16(element, offset, wanted ? layout : std::uint16_t{0xFFFF});
            set16(element, offset + 2, wanted ? first : std::uint16_t{0});
            set32(element, offset + 4, static_cast<std::uint32_t>(wanted));
            auto done = remove(layout, at, removed);
            if (!done)
                return done;
            return {};
        }

        // Growing: where the new items go, and what they start as.
        const std::size_t added = wanted - count;
        ByteBuffer pattern;
        std::size_t at = 0;
        std::optional<std::size_t> templateIndex;
        if (count == 0) {
            const std::uint16_t kind = field.kind == 4 ? 1 : field.kind == 5 ? 2 : 0;
            if (kind == 1)
                return fail(ErrorCode::Unsupported, "an empty list of linked objects can't get its first item yet");
            layout = 0xFFFF;
            for (std::size_t l = 0; l < m_b.layouts.size(); ++l)
                if (m_b.layouts[l].type == field.type && m_b.layouts[l].kind == kind)
                    layout = static_cast<std::uint16_t>(l);
            if (layout == 0xFFFF) {
                if (m_b.layouts.size() >= 0xFFFF)
                    return fail(ErrorCode::InvalidFormat, "too many object kinds in this record");
                layout = static_cast<std::uint16_t>(m_b.layouts.size());
                m_b.layouts.push_back({field.type, 0, kind, 0});
                m_b.items.emplace_back();
            }
            at = m_b.items[layout].size();
            pattern = blank(field.type, kind);
        } else {
            if (layout >= m_b.items.size() || first + count > m_b.items[layout].size())
                return fail(ErrorCode::InvalidFormat, "the list's items are out of range");
            at = first + count;
            templateIndex = at - 1;
            pattern = m_b.items[layout][at - 1];
        }
        if (m_b.items[layout].size() + added > 65535)
            return fail(ErrorCode::InvalidFormat, "too many items of this kind in one record");

        m_b.items[layout].insert(m_b.items[layout].begin() + static_cast<std::ptrdiff_t>(at), added, pattern);
        m_b.layouts[layout].count = static_cast<std::uint16_t>(m_b.items[layout].size());
        shift(layout, at, static_cast<std::ptrdiff_t>(added));
        if (element.first == layout && element.second >= at)
            element.second += added;
        set16(element, offset, layout);
        set16(element, offset + 2, static_cast<std::uint16_t>(count ? first : at));
        set32(element, offset + 4, static_cast<std::uint32_t>(wanted));

        if (!templateIndex)
            return {};
        // Each new item gets its own copies of what the template item holds: its
        // lists' items, and the objects written out inside it in the text.
        findOwners();
        const Place original{layout, *templateIndex};
        for (std::size_t i = 0; i < added; ++i) {
            m_map.clear();
            m_map[original] = {layout, at + i};
            auto copied = m_b.layouts[layout].kind == 1 ? copyPointer({layout, at + i}, 0, original, 0)
                                                        : copyOwned({layout, at + i}, original, 0);
            if (!copied)
                return copied;
        }
        return {};
    }

private:
    std::uint16_t u16(Place p, std::size_t o) const { return loadScalar<std::uint16_t>(m_b.items[p.first][p.second], o); }
    std::uint32_t u32(Place p, std::size_t o) const { return loadScalar<std::uint32_t>(m_b.items[p.first][p.second], o); }
    void set16(Place p, std::size_t o, std::uint16_t v) { storeScalar(m_b.items[p.first][p.second], o, v); }
    void set32(Place p, std::size_t o, std::uint32_t v) { storeScalar(m_b.items[p.first][p.second], o, v); }

    bool isStruct(std::size_t layout) const
    {
        return m_b.layouts[layout].kind == 0 && m_pack.types[m_b.layouts[layout].type].kind == 3;
    }

    /// Every list header and pointer in the block: `onList(place, offset)`, `onPointer(place, offset)`.
    template <typename OnList, typename OnPointer>
    void eachReference(OnList&& onList, OnPointer&& onPointer) const
    {
        for (std::size_t l = 0; l < m_b.items.size(); ++l) {
            if (m_b.layouts[l].kind == 1) {
                for (std::size_t e = 0; e < m_b.items[l].size(); ++e)
                    onPointer(Place{l, e}, 0);
                continue;
            }
            if (!isStruct(l))
                continue;
            for (std::size_t e = 0; e < m_b.items[l].size(); ++e)
                eachLeaf(m_pack, m_b.layouts[l].type, 0, [&](const FlaskPack::Field& f, std::size_t at) {
                    if (f.array)
                        onList(Place{l, e}, at);
                    else if (f.kind == 4)
                        onPointer(Place{l, e}, at);
                });
        }
    }

    /// Objects held by lists (written where the list is).
    std::set<Place> coveredObjects() const
    {
        std::set<Place> covered;
        eachReference(
            [&](Place p, std::size_t o) {
                const std::size_t l = u16(p, o), first = u16(p, o + 2), count = u32(p, o + 4);
                if (l < m_b.layouts.size() && m_b.layouts[l].kind == 0 && count)
                    for (std::size_t i = first; i < first + count; ++i)
                        covered.insert({l, i});
            },
            [](Place, std::size_t) {});
        return covered;
    }

    /// For each object written out in full at a pointer (as the text does: the
    /// first pointer to it, in text order, unless a list holds it), that pointer.
    void findOwners()
    {
        m_owner.clear();
        m_covered = coveredObjects();
        std::set<Place> seen{{0, 0}};
        visitStruct({0, 0}, seen, 0);
    }

    bool exists(Place p) const { return p.first < m_b.items.size() && p.second < m_b.items[p.first].size(); }

    void visitStruct(Place p, std::set<Place>& seen, int depth)
    {
        if (depth > kMaxDepth || !exists(p) || !isStruct(p.first))
            return;
        eachLeaf(m_pack, m_b.layouts[p.first].type, 0, [&](const FlaskPack::Field& f, std::size_t at) {
            if (f.array) {
                const std::size_t l = u16(p, at), first = u16(p, at + 2), count = u32(p, at + 4);
                for (std::size_t i = 0; i < count && exists({l, first + i}); ++i) {
                    if (m_b.layouts[l].kind == 0)
                        visitStruct({l, first + i}, seen, depth + 1);
                    else if (m_b.layouts[l].kind == 1)
                        visitPointer({l, first + i}, 0, seen, depth);
                }
            } else if (f.kind == 4) {
                visitPointer(p, at, seen, depth);
            }
        });
    }

    void visitPointer(Place p, std::size_t o, std::set<Place>& seen, int depth)
    {
        const std::uint32_t v = u32(p, o);
        const Place target{v & 0xFFFF, v >> 16};
        if (v == kNone || !exists(target) || m_covered.contains(target) || !seen.insert(target).second)
            return;
        m_owner[target] = {p, o};
        visitStruct(target, seen, depth + 1);
    }

    /// Elements of `layout` from `at` moved by `delta` (inserted or removed):
    /// lists and pointers into them follow; pointers to removed ones become null.
    void shift(std::size_t layout, std::size_t at, std::ptrdiff_t delta)
    {
        const std::size_t removedEnd = delta < 0 ? at + static_cast<std::size_t>(-delta) : at;
        eachReference(
            [&](Place p, std::size_t o) {
                const std::size_t first = u16(p, o + 2);
                if (u16(p, o) == layout && u32(p, o + 4) != 0 && first >= removedEnd)
                    set16(p, o + 2, static_cast<std::uint16_t>(static_cast<std::ptrdiff_t>(first) + delta));
            },
            [&](Place p, std::size_t o) {
                const std::uint32_t v = u32(p, o);
                if (v == kNone || (v & 0xFFFF) != layout)
                    return;
                const std::size_t e = v >> 16;
                if (e >= removedEnd)
                    set32(p, o, (v & 0xFFFF) | static_cast<std::uint32_t>(static_cast<std::ptrdiff_t>(e) + delta) << 16);
                else if (e >= at) // removed
                    set32(p, o, kNone);
            });
    }

    Result<void> remove(std::size_t layout, std::size_t at, std::size_t n)
    {
        if (layout >= m_b.items.size() || at + n > m_b.items[layout].size())
            return fail(ErrorCode::InvalidFormat, "the list's items are out of range");
        auto& list = m_b.items[layout];
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(at), list.begin() + static_cast<std::ptrdiff_t>(at + n));
        m_b.layouts[layout].count = static_cast<std::uint16_t>(list.size());
        shift(layout, at, -static_cast<std::ptrdiff_t>(n));
        return {};
    }

    /// An offset into the block's texts holding "" (added if there is none).
    std::uint32_t emptyText()
    {
        const std::size_t zero = m_b.strings.find('\0');
        if (zero != std::string::npos)
            return static_cast<std::uint32_t>(zero);
        const std::size_t at = m_b.strings.size();
        m_b.strings.append(4, '\0');
        m_b.stringsChanged = true;
        return static_cast<std::uint32_t>(at);
    }

    /// A new item with nothing in it: zeros, empty lists, no links, empty texts.
    ByteBuffer blank(std::uint16_t type, std::uint16_t kind)
    {
        if (kind != 0)
            return ByteBuffer(4, std::byte{0xFF});
        const FlaskPack::Type& t = m_pack.types[type];
        ByteBuffer out(t.size);
        if (t.kind == 0 && t.primitive == 12 && out.size() >= 4)
            storeScalar(out, 0, emptyText());
        if (t.kind != 3)
            return out;
        eachLeaf(m_pack, type, 0, [&](const FlaskPack::Field& f, std::size_t at) {
            if (at + (f.array ? 8 : 4) > out.size())
                return;
            if (f.array) {
                storeScalar(out, at, std::uint16_t{0xFFFF});
                storeScalar(out, at + 2, std::uint16_t{0});
                storeScalar(out, at + 4, std::uint32_t{0});
            } else if (f.kind == 4 || f.kind == 5) {
                storeScalar(out, at, kNone);
            } else if (f.kind == 0 && f.type < m_pack.types.size() && m_pack.types[f.type].primitive == 12) {
                storeScalar(out, at, emptyText());
            }
        });
        return out;
    }

    /// Appends a copy of `from` to its layout; returns where.
    Result<Place> append(Place from)
    {
        auto& list = m_b.items[from.first];
        if (list.size() >= 65535)
            return fail(ErrorCode::InvalidFormat, "too many items of this kind in one record");
        list.push_back(list[from.second]); // copy first: push_back may move the list
        m_b.layouts[from.first].count = static_cast<std::uint16_t>(list.size());
        return Place{from.first, list.size() - 1};
    }

    /// `p` and everything written out inside it (its lists' items, the objects
    /// shown at its pointers), into `held`.
    void collectHeld(Place p, std::set<Place>& held, int depth)
    {
        if (depth > kMaxDepth || !exists(p) || !held.insert(p).second)
            return;
        const auto pointer = [&](Place at, std::size_t o) {
            const std::uint32_t v = u32(at, o);
            const Place target{v & 0xFFFF, v >> 16};
            if (const auto it = m_owner.find(target); v != kNone && it != m_owner.end() && it->second == std::pair{at, o})
                collectHeld(target, held, depth + 1);
        };
        if (m_b.layouts[p.first].kind == 1) {
            pointer(p, 0);
            return;
        }
        if (!isStruct(p.first))
            return;
        eachLeaf(m_pack, m_b.layouts[p.first].type, 0, [&](const FlaskPack::Field& f, std::size_t at) {
            if (f.array) {
                const std::size_t l = u16(p, at), first = u16(p, at + 2), count = u32(p, at + 4);
                for (std::size_t i = 0; i < count && exists({l, first + i}); ++i)
                    collectHeld({l, first + i}, held, depth + 1);
            } else if (f.kind == 4) {
                pointer(p, at);
            }
        });
    }

    /// The pointer at (p, o) of a copy of `original`: to its own copy of the
    /// object, if the original's pointer is where that object is written out.
    Result<void> copyPointer(Place p, std::size_t o, Place original, int depth)
    {
        const std::uint32_t v = u32(p, o);
        if (v == kNone)
            return {};
        const Place target{v & 0xFFFF, v >> 16};
        if (const auto it = m_map.find(target); it != m_map.end()) {
            set32(p, o, static_cast<std::uint32_t>(it->second.first | it->second.second << 16));
            return {};
        }
        const auto owner = m_owner.find(target);
        if (owner == m_owner.end() || owner->second != std::pair{original, o})
            return {}; // shown elsewhere: the copy points at the same object
        auto copy = append(target);
        if (!copy)
            return std::unexpected(copy.error());
        m_map[target] = *copy;
        set32(p, o, static_cast<std::uint32_t>(copy->first | copy->second << 16));
        return copyOwned(*copy, target, depth + 1);
    }

    /// Gives the new object at `p` (a copy of `original`) its own copies of its
    /// lists' items and of the objects written out inside it.
    Result<void> copyOwned(Place p, Place original, int depth)
    {
        if (depth > kMaxDepth)
            return fail(ErrorCode::InvalidFormat, "objects nested too deeply to copy");
        if (!isStruct(p.first))
            return {};
        std::vector<std::size_t> lists, pointers;
        eachLeaf(m_pack, m_b.layouts[p.first].type, 0, [&](const FlaskPack::Field& f, std::size_t at) {
            if (f.array)
                lists.push_back(at);
            else if (f.kind == 4)
                pointers.push_back(at);
        });
        for (const std::size_t o : lists) {
            const std::size_t l = u16(p, o), first = u16(p, o + 2), count = u32(p, o + 4);
            if (!count || l >= m_b.items.size() || first + count > m_b.items[l].size())
                continue;
            std::vector<Place> copies;
            for (std::size_t i = 0; i < count; ++i) {
                auto copy = append({l, first + i});
                if (!copy)
                    return std::unexpected(copy.error());
                m_map[{l, first + i}] = *copy;
                copies.push_back(*copy);
            }
            set16(p, o + 2, static_cast<std::uint16_t>(copies.front().second));
            for (std::size_t i = 0; i < copies.size(); ++i) {
                const Place from{l, first + i};
                auto done = m_b.layouts[l].kind == 1 ? copyPointer(copies[i], 0, from, depth + 1)
                                                     : copyOwned(copies[i], from, depth + 1);
                if (!done)
                    return done;
            }
        }
        for (const std::size_t o : pointers)
            if (auto done = copyPointer(p, o, original, depth); !done)
                return done;
        return {};
    }

    const FlaskPack& m_pack;
    EditBlock& m_b;
    std::map<Place, std::pair<Place, std::size_t>> m_owner; ///< Object -> the pointer it's written out at.
    std::set<Place> m_covered;
    std::map<Place, Place> m_map; ///< Original -> copy, while copying one item.
};

/// `file` with the blocks in `edits` replaced (their layouts may have changed):
/// every section written again in the original order, each on a multiple of 4.
Result<ByteBuffer> writePack(ByteSpan file, const FlaskPack& pack, const std::vector<std::optional<EditBlock>>& edits)
{
    constexpr std::size_t kSizes[8] = {12, 4, 8, 1, 8, 16, 6, 1};
    ByteBuffer blocks, layouts, data;
    std::size_t layoutCount = 0;
    for (std::size_t b = 0; b < pack.blocks.size(); ++b) {
        const FlaskPack::Block& block = pack.blocks[b];
        const auto& edit = edits[b];
        const std::vector<FlaskPack::Layout>& ls = edit ? edit->layouts : block.layouts;
        const ByteBuffer objects = edit ? edit->objects() : ByteBuffer(block.objects.begin(), block.objects.end());
        const ByteBuffer text = edit && edit->stringsChanged ? storedText(pack, edit->strings)
                                                             : ByteBuffer(block.rawText.begin(), block.rawText.end());
        const std::size_t at = blocks.size();
        blocks.resize(at + 16);
        storeScalar(blocks, at, block.record);
        storeScalar(blocks, at + 4, static_cast<std::uint32_t>(ls.size()));
        storeScalar(blocks, at + 8, static_cast<std::uint32_t>(objects.size()));
        storeScalar(blocks, at + 12, static_cast<std::uint32_t>(text.size()));
        for (const FlaskPack::Layout& l : ls) {
            const std::size_t p = layouts.size();
            layouts.resize(p + 6);
            storeScalar(layouts, p, l.type);
            storeScalar(layouts, p + 2, l.count);
            storeScalar(layouts, p + 4, l.kind);
        }
        layoutCount += ls.size();
        data.insert(data.end(), objects.begin(), objects.end());
        data.insert(data.end(), text.begin(), text.end());
    }
    // Data after the last block (if any) stays.
    std::size_t used = 0;
    for (const FlaskPack::Block& block : pack.blocks)
        used += block.objects.size() + block.rawText.size();
    if (used < pack.sectionCount[7])
        data.insert(data.end(), file.begin() + static_cast<std::ptrdiff_t>(pack.sectionOffset[7] + used),
                    file.begin() + static_cast<std::ptrdiff_t>(pack.sectionOffset[7] + pack.sectionCount[7]));

    std::array<std::size_t, 8> order{0, 1, 2, 3, 4, 5, 6, 7};
    std::ranges::stable_sort(order, {}, [&](std::size_t s) { return pack.sectionOffset[s]; });
    ByteBuffer out(file.begin(), file.begin() + 0x48);
    std::size_t end = 0x48;
    for (const std::size_t s : order) {
        end = std::max(end, pack.sectionOffset[s] + pack.sectionCount[s] * kSizes[s]);
        out.resize((out.size() + 3) & ~std::size_t{3});
        storeScalar(out, 8 + 8 * s, static_cast<std::uint32_t>(out.size()));
        std::size_t count = pack.sectionCount[s];
        if (s == 5) {
            out.insert(out.end(), blocks.begin(), blocks.end());
        } else if (s == 6) {
            out.insert(out.end(), layouts.begin(), layouts.end());
            count = layoutCount;
        } else if (s == 7) {
            out.insert(out.end(), data.begin(), data.end());
            count = data.size();
        } else {
            const auto from = file.begin() + static_cast<std::ptrdiff_t>(pack.sectionOffset[s]);
            out.insert(out.end(), from, from + static_cast<std::ptrdiff_t>(count * kSizes[s]));
        }
        storeScalar(out, 12 + 8 * s, static_cast<std::uint32_t>(count));
    }
    if (end < file.size()) // anything after the sections stays
        out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(end), file.end());
    if (auto check = readFlask(out); !check)
        return fail(ErrorCode::InvalidFormat, "the rebuilt table doesn't read back: {}", check.error().message);
    return out;
}

} // namespace

Result<ByteBuffer> FlaskDecompiler::recompile(ByteSpan original, std::string_view text,
                                              const DecompileContext& context) const
{
    // First the lists that got more or fewer items, outermost first (one per
    // record per round: changing a list moves the objects after it), then the values.
    ByteBuffer current(original.begin(), original.end());
    for (int round = 0;; ++round) {
        auto pack = readFlask(current);
        if (!pack)
            return std::unexpected(pack.error());
        const Decompiled d = decompilePack(*pack, context, false);
        auto resized = resizedLists(d.text, text);
        if (!resized)
            return std::unexpected(resized.error());
        if (resized->empty())
            break;
        if (round > 10000)
            return fail(ErrorCode::InvalidFormat, "too many lists changed at once");
        std::vector<std::optional<EditBlock>> edits(pack->blocks.size());
        for (const ResizedList& r : *resized) {
            const ListSlot& list = d.lists[r.id];
            if (edits[list.block])
                continue; // next round
            EditBlock& block = edits[list.block].emplace(*pack, pack->blocks[list.block]);
            // The header's place: which layout element holds it.
            std::optional<Place> place;
            std::size_t offset = 0;
            for (std::size_t l = 0; l < block.layouts.size() && !place; ++l) {
                const FlaskPack::Layout& layout = pack->blocks[list.block].layouts[l];
                const std::size_t size = layout.elementSize(*pack);
                if (size && list.offset >= layout.start && list.offset < layout.start + layout.count * size) {
                    place = Place{l, (list.offset - layout.start) / size};
                    offset = (list.offset - layout.start) % size;
                }
            }
            if (!place)
                return fail(ErrorCode::InvalidFormat, "line {}: can't find this list in the file", r.line);
            if (auto done = ListEditor(*pack, block).resize(*place, offset, pack->fields[list.field], r.items); !done)
                return fail(done.error().code, "line {}: {}", r.line, done.error().message);
        }
        auto rebuilt = writePack(current, *pack, edits);
        if (!rebuilt)
            return std::unexpected(rebuilt.error());
        current = std::move(*rebuilt);
    }
    return applyValues(current, text, context);
}

} // namespace viewer::games::blur
