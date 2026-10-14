// Blur's vector UI layouts (.ipf; little-endian, packed). From Blur.exe's
// loader (0x773A60: header; groups 0x7737E0 / 0x7736A0; shapes 0x772B20):
//
//   u32 version (0x0010xxxx), u32 items, items x (u32 type 3, group)
//   group:  u32 ?, string name, u32 children, children x (u32 type: 3 group, 1 shape)
//   shape:  u32 id, string name,
//           u32 fill (0 none, 1 colour: f32 r g b, 2 gradient: 5 x u32),
//           f32 outline r g b, f32 outline width, u8 alpha, u8 ?,
//           f32[3] bounds min, f32[3] bounds max, f32[2], f32[2]
//           u32 fill lists, per list: u32 n, n x (f32 x, f32 y, f32 [4])
//           u32 index lists (one per fill list), per list: u32 n, n x u16 (triangles)
//           u32 outline lists, per list: u32 n, n x (f32 x, f32 y, f32 dx, f32 dy, f32 [4])
//   string: u32 length, characters
// Positions run 0..1 across each item (a child of a top-level layer: one icon
// of an icon sheet, one route of a minimap, one frame of the countdown), y as
// far down as the item reaches; an outline is a triangle strip whose points
// sit at (x, y) pushed sideways by (dx, dy) pixels (half the width). Items are
// laid out side by side here, each with its name under it.
// 71 of the 75 layouts in the game read to their last byte; the other 4 are
// console / older versions the PC game rejects too.

#include "UiLayout.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <optional>

namespace viewer::games::blur {

namespace {

constexpr std::uint32_t kVersion = 0x00100000;
constexpr float kCanvas = 1024.0f; ///< Pixels across: the outlines' sideways offsets are in pixels.

class Reader {
public:
    explicit Reader(ByteSpan d) : m_d(d) {}
    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t position() const noexcept { return m_p; }
    [[nodiscard]] bool atEnd() const noexcept { return m_p == m_d.size(); }

    bool need(std::size_t n)
    {
        if (m_ok && n <= m_d.size() - m_p)
            return true;
        m_ok = false;
        return false;
    }
    std::uint32_t u32()
    {
        if (!need(4))
            return 0;
        m_p += 4;
        return loadScalar<std::uint32_t>(m_d, m_p - 4);
    }
    std::uint16_t u16()
    {
        if (!need(2))
            return 0;
        m_p += 2;
        return loadScalar<std::uint16_t>(m_d, m_p - 2);
    }
    std::uint8_t u8()
    {
        if (!need(1))
            return 0;
        return std::to_integer<std::uint8_t>(m_d[m_p++]);
    }
    float f32()
    {
        const float v = std::bit_cast<float>(u32());
        return std::isfinite(v) ? v : 0.0f;
    }
    std::string text()
    {
        const std::uint32_t n = u32();
        if (n > 4096 || !need(n))
            return {};
        std::string s(reinterpret_cast<const char*>(m_d.data() + m_p), n);
        m_p += n;
        return s;
    }
    /// The name of the group or shape that starts here (u32, then the name), without moving on.
    std::string peekName()
    {
        const std::size_t at = m_p;
        u32();
        std::string name = text();
        m_p = at;
        return name;
    }
    /// A count that can't be more than the bytes left allow (each item at least `itemBytes`).
    std::uint32_t count(std::size_t itemBytes)
    {
        const std::uint32_t n = u32();
        if (itemBytes && n > (m_d.size() - m_p) / itemBytes)
            m_ok = false;
        return m_ok ? n : 0;
    }

private:
    ByteSpan m_d;
    std::size_t m_p = 0;
    bool m_ok = true;
};

void readShape(Reader& r, const std::string& parent, Drawing& out)
{
    r.u32(); // id
    const std::string name = r.text();
    const std::uint32_t fill = r.u32();
    std::optional<std::array<float, 4>> fillColour;
    if (fill == 1) {
        const float red = r.f32(), green = r.f32(), blue = r.f32();
        fillColour = std::array<float, 4>{red, green, blue, 1.0f};
    } else if (fill == 2) {
        for (int i = 0; i < 5; ++i)
            r.u32(); // gradient (no layout in the game uses one)
        fillColour = std::array<float, 4>{0.7f, 0.7f, 0.7f, 1.0f};
    }
    const float red = r.f32(), green = r.f32(), blue = r.f32();
    const float width = r.f32();
    const float alpha = static_cast<float>(r.u8()) / 255.0f;
    r.u8();
    for (int i = 0; i < 10; ++i)
        r.f32(); // bounds and placement
    if (fillColour)
        (*fillColour)[3] = alpha;

    // Fills: vertex lists, then one index list (triangles) per vertex list.
    std::vector<std::vector<std::array<float, 2>>> fills(r.count(4));
    for (auto& list : fills) {
        list.resize(r.count(24));
        for (auto& v : list) {
            v = {r.f32() * kCanvas, r.f32() * kCanvas};
            for (int i = 0; i < 4; ++i)
                r.f32();
        }
    }
    const std::uint32_t indexLists = r.count(4);
    Drawing::Shape filled{parent + name, fillColour.value_or(std::array<float, 4>{1, 1, 1, alpha}), false, {}};
    for (std::uint32_t l = 0; l < indexLists; ++l) {
        const std::uint32_t n = r.count(2);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint16_t index = r.u16();
            if (l < fills.size() && index < fills[l].size())
                filled.triangles.push_back(fills[l][index]);
        }
    }
    filled.triangles.resize(filled.triangles.size() / 3 * 3);
    if (!filled.triangles.empty())
        out.shapes.push_back(std::move(filled));

    // Outlines: triangle strips.
    Drawing::Shape outline{parent + name, {red, green, blue, alpha}, true, {}};
    const std::uint32_t outlines = r.count(4);
    for (std::uint32_t l = 0; l < outlines; ++l) {
        std::vector<std::array<float, 2>> strip(r.count(32));
        for (auto& v : strip) {
            const float x = r.f32(), y = r.f32(), dx = r.f32(), dy = r.f32();
            v = {x * kCanvas + dx, y * kCanvas + dy};
            for (int i = 0; i < 4; ++i)
                r.f32();
        }
        for (std::size_t i = 2; i < strip.size(); ++i)
            outline.triangles.insert(outline.triangles.end(), {strip[i - 2], strip[i - 1], strip[i]});
    }
    if (!outline.triangles.empty() && width > 0)
        out.shapes.push_back(std::move(outline));
}

bool readGroup(Reader& r, const std::string& parent, Drawing& out, int depth)
{
    if (depth > 32)
        return false;
    r.u32();
    const std::string name = r.text();
    const std::string path = parent + name + " / ";
    const std::uint32_t children = r.count(4);
    for (std::uint32_t i = 0; i < children && r.ok(); ++i) {
        const std::uint32_t type = r.u32();
        if (type == 3) {
            if (!readGroup(r, path, out, depth + 1))
                return false;
        } else if (type == 1) {
            readShape(r, path, out);
        } else {
            return false;
        }
    }
    return r.ok();
}

} // namespace

bool IpfDecoder::canDecode(std::string_view extension, ByteSpan data) const
{
    return extension == "ipf" && data.size() >= 8;
}

namespace {

/// Puts the items (shapes [first, end) each) side by side in rows, names under them.
void layOut(Drawing& drawing, const std::vector<std::pair<std::string, std::size_t>>& items)
{
    struct Box {
        std::string name;
        std::size_t first, end;
        float right = 0, bottom = 0;
    };
    std::vector<Box> boxes;
    for (std::size_t i = 0; i < items.size(); ++i) {
        Box b{items[i].first, items[i].second, i + 1 < items.size() ? items[i + 1].second : drawing.shapes.size()};
        for (std::size_t s = b.first; s < b.end; ++s)
            for (const auto& p : drawing.shapes[s].triangles) {
                b.right = std::max(b.right, p[0]);
                b.bottom = std::max(b.bottom, p[1]);
            }
        if (b.end > b.first)
            boxes.push_back(std::move(b));
    }
    const bool labelled = boxes.size() > 1;
    const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(boxes.size())))));
    const float gap = labelled ? kCanvas * 0.1f : 0.0f, caption = labelled ? kCanvas * 0.16f : 0.0f;
    float y = 0, width = 0;
    for (std::size_t row = 0; row * columns < boxes.size(); ++row) {
        float rowHeight = 0, x = 0;
        for (std::size_t c = 0; c < static_cast<std::size_t>(columns) && row * columns + c < boxes.size(); ++c) {
            Box& b = boxes[row * columns + c];
            const float cell = std::max(kCanvas, std::min(b.right, kCanvas * 4));
            for (std::size_t s = b.first; s < b.end; ++s)
                for (auto& p : drawing.shapes[s].triangles) {
                    p[0] += x;
                    p[1] += y;
                }
            if (labelled)
                drawing.labels.push_back({b.name, x, y + std::min(b.bottom, kCanvas * 4) + gap * 0.2f, cell});
            rowHeight = std::max(rowHeight, std::min(b.bottom, kCanvas * 4));
            x += cell + gap;
        }
        width = std::max(width, x - gap);
        y += rowHeight + caption + gap;
    }
    drawing.width = std::max(width, 1.0f);
    drawing.height = std::max(y - gap, 1.0f);
}

} // namespace

Result<Drawing> IpfDecoder::decode(ByteSpan data) const
{
    if (data.size() < 8)
        return fail(ErrorCode::InvalidFormat, "too small for a UI layout");
    const std::uint32_t version = loadScalar<std::uint32_t>(data, 0);
    if ((version & 0xFFFF0000) != kVersion)
        return fail(ErrorCode::Unsupported,
                    "UI layout version {:08X} isn't the PC one (a console or older layout the game doesn't load)",
                    version);
    Reader r(data);
    r.u32();
    Drawing drawing;
    std::vector<std::pair<std::string, std::size_t>> items; // name, first shape
    const std::uint32_t layers = r.count(4);
    for (std::uint32_t i = 0; i < layers; ++i) {
        // A top-level layer: each of its children is an item of its own.
        if (r.u32() != 3)
            return fail(ErrorCode::InvalidFormat, "UI layout damaged near byte {}", r.position());
        r.u32();
        const std::string layer = r.text() + " / ";
        const std::uint32_t children = r.count(4);
        for (std::uint32_t c = 0; c < children && r.ok(); ++c) {
            const std::uint32_t type = r.u32();
            const std::size_t first = drawing.shapes.size();
            std::string name;
            bool ok = true;
            if (type == 3) {
                name = r.peekName();
                ok = readGroup(r, layer, drawing, 1);
            } else if (type == 1) {
                name = r.peekName();
                readShape(r, layer, drawing);
            } else {
                ok = false;
            }
            if (!ok || !r.ok())
                return fail(ErrorCode::InvalidFormat, "UI layout damaged near byte {}", r.position());
            items.emplace_back(name.starts_with('<') ? std::string() : name, first);
        }
    }
    if (!r.ok())
        return fail(ErrorCode::InvalidFormat, "UI layout ends early (byte {})", r.position());
    if (!r.atEnd())
        drawing.notes.push_back(std::format("{} byte(s) at the end not read", data.size() - r.position()));
    layOut(drawing, items);
    return drawing;
}

} // namespace viewer::games::blur
