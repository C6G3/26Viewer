// Blur's skeletons (.skeleton) and animation cycles (.cycle).
//
// .skeleton (version 7; little-endian):
//   u32 version, u32 bones (version 6 has one more byte before the count),
//   s16 parent[bones] (-1: root; parents come first),
//   bones x (quaternion x y z w, position x y z, scale x y z) — the rest pose
//   relative to the parent, then bones x 3x4 inverse bind matrix (row vectors:
//   model space → bone space), 14 bytes (u32 4, zeros), u32 count, then
//   count length-prefixed bone names.
//
// .cycle (version 15; version 14's header is 24 bytes shorter):
//   u32 version, u32 0, two transforms (10 floats each), u32 ?, u32 frames,
//   f32 last frame, f32 frames per second, u32 bones, one byte per bone, u32
//   count, count length-prefixed bone names. Then the channels: u16 constant
//   count, u16 animated count, nine u16 list sizes, the lists of channel ids
//   (bone * 64 + kind: 0 rotation, 1 position, 2 scale; 3, 9, 10 other values):
//     0 constant vectors (3 floats), 1 constant numbers (1 float), 2 constant
//     rotations (48-bit); animated: 3 vectors (3 floats), 4 rotations (32-bit),
//     5 numbers (1 float), 6 rotations (48-bit), 7 vectors (3 x u16 within a
//     range), 8 vectors (3 x u8 within a range).
//   u32 size of the constant values, which are: per list-7/8 channel its range
//   (minimum x y z, size x y z), then lists 0, 1 and 2.
//
//   Rotations keep the three smallest components of the quaternion: bit 0 is
//   set when the largest component is negative, bits 1-2 say which one it is,
//   and from bit 3 come the other three in order, each mapping 0..max to
//   -1/sqrt(2)..+1/sqrt(2). A 48-bit rotation is one number made of the three
//   u16 (the first one highest) with 15-bit components; a 32-bit one is a u32
//   with components of 9, 10 and 10 bits. (Checked against the skeletons' rest
//   rotations of props that don't move, and on spinning rotors and fans.)
//
//   The animated keys follow, in blocks of up to 255 frames: u32 blocks + 1,
//   u32 ?, u32 0, per block u16 frames, u16 ?, u16 last frame, u16 0; then
//   u16 0, u16 ?. Each block: every animated channel's value at the block's
//   first frame (lists 3..8 in order), six u16 key totals (one per list), per
//   channel u8 key count and that many u8 frames (from the block's start), the
//   key values channel by channel, and in the last block every channel's value
//   at the last frame.

#include "Animation.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace viewer::games::blur {

namespace {

constexpr std::uint32_t kSkeletonVersion = 7, kCycleVersion = 15, kOldCycleVersion = 14;

/// Bounds-checked little-endian access to a whole file.
struct Bytes {
    ByteSpan d;
    [[nodiscard]] bool has(std::size_t at, std::size_t n) const noexcept { return at <= d.size() && n <= d.size() - at; }
    [[nodiscard]] std::uint8_t u8(std::size_t at) const noexcept { return std::to_integer<std::uint8_t>(d[at]); }
    [[nodiscard]] std::uint16_t u16(std::size_t at) const noexcept { return loadScalar<std::uint16_t>(d, at); }
    [[nodiscard]] std::uint32_t u32(std::size_t at) const noexcept { return loadScalar<std::uint32_t>(d, at); }
    [[nodiscard]] float f32(std::size_t at) const noexcept { return loadScalar<float>(d, at); }
};

/// count length-prefixed names at `at` (moved past them); nullopt if they don't fit.
std::optional<std::vector<std::string>> readNames(const Bytes& b, std::size_t& at, std::uint32_t count)
{
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!b.has(at, 4))
            return std::nullopt;
        const std::uint32_t n = b.u32(at);
        if (n > 256 || !b.has(at + 4, n))
            return std::nullopt;
        names.emplace_back(reinterpret_cast<const char*>(b.d.data() + at + 4), n);
        at += 4 + n;
    }
    return names;
}

Transform readTransform(const Bytes& b, std::size_t at)
{
    Transform t;
    for (int k = 0; k < 4; ++k)
        t.rotation[k] = b.f32(at + 4 * k);
    for (int k = 0; k < 3; ++k) {
        t.position[k] = b.f32(at + 16 + 4 * k);
        t.scale[k] = b.f32(at + 28 + 4 * k);
    }
    return t;
}

/// The quaternion in `bits` (see the top of the file), components `widths` bits wide.
std::array<float, 4> smallestThree(std::uint64_t bits, const std::array<int, 3>& widths)
{
    const auto largest = static_cast<int>((bits >> 1) & 3);
    std::array<float, 3> small{};
    float sum = 0;
    int shift = 3;
    for (int k = 0; k < 3; ++k) {
        const std::uint64_t max = (std::uint64_t{1} << widths[k]) - 1;
        const auto v = static_cast<float>((bits >> shift) & max);
        small[k] = (v / static_cast<float>(max) * 2.0f - 1.0f) * 0.70710678f;
        sum += small[k] * small[k];
        shift += widths[k];
    }
    const float big = std::sqrt(std::max(0.0f, 1.0f - sum));
    std::array<float, 4> q{};
    for (int k = 0, s = 0; k < 4; ++k)
        q[k] = k == largest ? (bits & 1 ? -big : big) : small[s++];
    return q;
}

std::array<float, 4> rotation48(const Bytes& b, std::size_t at)
{
    return smallestThree(std::uint64_t{b.u16(at)} << 32 | std::uint64_t{b.u16(at + 2)} << 16 | b.u16(at + 4),
                         {15, 15, 15});
}

std::array<float, 4> rotation32(const Bytes& b, std::size_t at)
{
    return smallestThree(b.u32(at), {9, 10, 10});
}

/// One animated channel while its keys are read.
struct Track {
    unsigned list = 0;            ///< 3..8
    std::size_t size = 0;         ///< Bytes per value.
    std::array<float, 6> range{}; ///< Lists 7 and 8: minimum x y z, size x y z.
    std::optional<Animation::Channel> channel; ///< Null: not a bone transform (still read).

    [[nodiscard]] std::array<float, 4> value(const Bytes& b, std::size_t at) const
    {
        switch (list) {
        case 3:
            return {b.f32(at), b.f32(at + 4), b.f32(at + 8), 0};
        case 4:
            return rotation32(b, at);
        case 5:
            return {b.f32(at), 0, 0, 0};
        case 6:
            return rotation48(b, at);
        case 7:
            return {range[0] + range[3] * static_cast<float>(b.u16(at)) / 65535.0f,
                    range[1] + range[4] * static_cast<float>(b.u16(at + 2)) / 65535.0f,
                    range[2] + range[5] * static_cast<float>(b.u16(at + 4)) / 65535.0f, 0};
        default:
            return {range[0] + range[3] * static_cast<float>(b.u8(at)) / 255.0f,
                    range[1] + range[4] * static_cast<float>(b.u8(at + 1)) / 255.0f,
                    range[2] + range[5] * static_cast<float>(b.u8(at + 2)) / 255.0f, 0};
        }
    }
    void add(float frame, const std::array<float, 4>& v)
    {
        if (!channel)
            return;
        if (!channel->frames.empty() && channel->frames.back() >= frame) // a block's start repeats the frame before
            return;
        channel->frames.push_back(frame);
        channel->values.push_back(v);
    }
};

} // namespace

bool SkeletonFileDecoder::canDecode(std::string_view extension, ByteSpan data) const
{
    return extension == "skeleton" && data.size() >= 8 && loadScalar<std::uint32_t>(data, 0) == kSkeletonVersion;
}

Result<Skeleton> SkeletonFileDecoder::decode(ByteSpan data) const
{
    const Bytes b{data};
    if (!b.has(0, 8) || b.u32(0) != kSkeletonVersion)
        return fail(ErrorCode::Unsupported, "not a version {} skeleton", kSkeletonVersion);
    const std::uint32_t count = b.u32(4);
    const std::size_t transforms = 8 + 2 * std::size_t{count}, matrices = transforms + 40 * std::size_t{count};
    std::size_t names = matrices + 48 * std::size_t{count} + 14;
    if (count == 0 || count > 1024 || !b.has(names, 4) || b.u32(names) != count)
        return fail(ErrorCode::InvalidFormat, "skeleton layout doesn't fit ({} bones)", count);
    names += 4;
    auto boneNames = readNames(b, names, count);
    if (!boneNames)
        return fail(ErrorCode::InvalidFormat, "skeleton bone names cut off");
    Skeleton skeleton;
    for (std::uint32_t i = 0; i < count; ++i) {
        Skeleton::Bone bone;
        bone.name = (*boneNames)[i];
        const auto parent = static_cast<std::int16_t>(b.u16(8 + 2 * i));
        bone.parent = parent >= 0 && static_cast<std::uint32_t>(parent) < i ? parent : -1;
        bone.rest = readTransform(b, transforms + 40 * std::size_t{i});
        for (int k = 0; k < 12; ++k)
            bone.inverseBind[k] = b.f32(matrices + 48 * std::size_t{i} + 4 * k);
        skeleton.bones.push_back(std::move(bone));
    }
    return skeleton;
}

bool CycleDecoder::canDecode(std::string_view extension, ByteSpan data) const
{
    if (extension != "cycle" || data.size() < 0x70)
        return false;
    const auto version = loadScalar<std::uint32_t>(data, 0);
    return version == kCycleVersion || version == kOldCycleVersion;
}

Result<Animation> CycleDecoder::decode(ByteSpan data) const
{
    const Bytes b{data};
    if (!canDecode("cycle", data))
        return fail(ErrorCode::Unsupported, "not a version {} or {} animation cycle", kOldCycleVersion, kCycleVersion);
    // Version 14's header is 24 bytes shorter (a smaller transform); the rest matches.
    const std::size_t h = b.u32(0) == kCycleVersion ? 0x58 : 0x40;
    Animation anim;
    anim.frameCount = std::max<std::uint32_t>(1, b.u32(h + 4));
    anim.fps = b.f32(h + 12) > 0 ? b.f32(h + 12) : 30.0f;
    const std::uint32_t boneCount = b.u32(h + 16);
    std::size_t p = h + 20 + boneCount; // one byte per bone
    if (boneCount > 1024 || !b.has(p, 4) || b.u32(p) != boneCount)
        return fail(ErrorCode::InvalidFormat, "animation bone list doesn't fit ({} bones)", boneCount);
    p += 4;
    auto names = readNames(b, p, boneCount);
    if (!names)
        return fail(ErrorCode::InvalidFormat, "animation bone names cut off");
    anim.bones = std::move(*names);

    // Channel lists: u16 constant count, u16 animated count, nine u16 list sizes.
    if (!b.has(p, 22))
        return fail(ErrorCode::InvalidFormat, "animation channel header cut off");
    const std::uint16_t constant = b.u16(p), animated = b.u16(p + 2);
    std::array<std::uint16_t, 9> counts{};
    for (std::size_t l = 0; l < counts.size(); ++l)
        counts[l] = b.u16(p + 4 + 2 * l);
    if (counts[0] + counts[1] + counts[2] != constant
        || counts[3] + counts[4] + counts[5] + counts[6] + counts[7] + counts[8] != animated)
        return fail(ErrorCode::InvalidFormat, "animation channel counts don't add up");
    p += 22;
    std::array<std::vector<std::uint16_t>, 9> lists;
    for (std::size_t l = 0; l < lists.size(); ++l) {
        if (!b.has(p, 2 * std::size_t{counts[l]}))
            return fail(ErrorCode::InvalidFormat, "animation channel list {} cut off", l);
        for (std::uint32_t i = 0; i < counts[l]; ++i)
            lists[l].push_back(b.u16(p + 2 * i));
        p += 2 * std::size_t{counts[l]};
    }
    const auto channel = [&](std::uint16_t id) -> std::optional<Animation::Channel> {
        const std::size_t bone = id >> 6;
        const unsigned kind = id & 63;
        if (bone >= anim.bones.size() || kind > 2) // other kinds: camera values and the like
            return std::nullopt;
        Animation::Channel ch;
        ch.bone = anim.bones[bone];
        ch.kind = static_cast<Animation::Kind>(kind);
        return ch;
    };

    // Constant values: the ranges of lists 7 and 8, then lists 0, 1 and 2.
    if (!b.has(p, 4))
        return fail(ErrorCode::InvalidFormat, "animation values cut off");
    const std::uint32_t constSize = b.u32(p);
    p += 4;
    if (!b.has(p, constSize)
        || constSize != 12u * counts[0] + 4u * counts[1] + 6u * counts[2] + 24u * (counts[7] + counts[8]))
        return fail(ErrorCode::InvalidFormat, "animation constant values don't fit");
    std::vector<std::array<float, 6>> ranges;
    std::size_t q = p;
    for (std::size_t i = 0; i < std::size_t{counts[7]} + counts[8]; ++i, q += 24) {
        std::array<float, 6> r{};
        for (int k = 0; k < 6; ++k)
            r[k] = b.f32(q + 4 * k);
        ranges.push_back(r);
    }
    for (const std::uint16_t id : lists[0]) {
        if (auto ch = channel(id)) {
            ch->frames = {0.0f};
            ch->values = {{b.f32(q), b.f32(q + 4), b.f32(q + 8), 0.0f}};
            anim.channels.push_back(std::move(*ch));
        }
        q += 12;
    }
    q += 4 * std::size_t{counts[1]};
    for (const std::uint16_t id : lists[2]) {
        if (auto ch = channel(id)) {
            ch->frames = {0.0f};
            ch->values = {rotation48(b, q)};
            anim.channels.push_back(std::move(*ch));
        }
        q += 6;
    }
    p += constSize;
    if (animated == 0)
        return anim;

    // Animated channels, lists 3..8 in order.
    static constexpr std::array<std::size_t, 6> kValueSize{12, 4, 4, 6, 6, 3};
    std::vector<Track> tracks;
    std::size_t nextRange = 0;
    for (unsigned l = 3; l < 9; ++l)
        for (const std::uint16_t id : lists[l]) {
            Track t;
            t.list = l;
            t.size = kValueSize[l - 3];
            if (l >= 7 && nextRange < ranges.size())
                t.range = ranges[nextRange++];
            t.channel = channel(id);
            tracks.push_back(std::move(t));
        }
    std::size_t valueBytes = 0;
    for (const Track& t : tracks)
        valueBytes += t.size;

    const auto broken = [&](std::string_view what) {
        return fail(ErrorCode::InvalidFormat, "animation keys damaged ({} near byte {})", what, p);
    };
    if (!b.has(p, 12))
        return broken("block list");
    const std::uint32_t blocks = b.u32(p) > 0 ? b.u32(p) - 1 : 0;
    if (blocks == 0 || blocks > 4096 || !b.has(p + 12, 8 * std::size_t{blocks} + 4))
        return broken("block list");
    std::vector<std::uint16_t> lastFrames;
    for (std::uint32_t i = 0; i < blocks; ++i)
        lastFrames.push_back(b.u16(p + 12 + 8 * std::size_t{i} + 4));
    p += 12 + 8 * std::size_t{blocks} + 4;
    std::vector<std::uint8_t> keyCounts(tracks.size());
    for (std::uint32_t block = 0; block < blocks; ++block) {
        const float start = block == 0 ? 0.0f : static_cast<float>(lastFrames[block - 1]);
        if (!b.has(p, valueBytes + 12))
            return broken("block start");
        for (Track& t : tracks) {
            t.add(start, t.value(b, p));
            p += t.size;
        }
        p += 12; // key totals per list
        std::vector<std::vector<std::uint8_t>> times(tracks.size());
        for (std::size_t c = 0; c < tracks.size(); ++c) {
            if (!b.has(p, 1) || !b.has(p + 1, b.u8(p)))
                return broken("key times");
            for (std::uint8_t k = 0; k < b.u8(p); ++k)
                times[c].push_back(b.u8(p + 1 + k));
            p += 1 + times[c].size();
        }
        for (std::size_t c = 0; c < tracks.size(); ++c)
            for (const std::uint8_t t : times[c]) {
                if (!b.has(p, tracks[c].size))
                    return broken("key values");
                tracks[c].add(start + static_cast<float>(t), tracks[c].value(b, p));
                p += tracks[c].size;
            }
        if (block + 1 == blocks) {
            if (!b.has(p, valueBytes))
                return broken("last frame");
            for (Track& t : tracks) {
                t.add(static_cast<float>(lastFrames[block]), t.value(b, p));
                p += t.size;
            }
        }
    }
    for (Track& t : tracks)
        if (t.channel && !t.channel->values.empty())
            anim.channels.push_back(std::move(*t.channel));
    return anim;
}

} // namespace viewer::games::blur
