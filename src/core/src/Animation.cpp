#include "viewer/core/Animation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace viewer {

namespace {

bool sameName(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

int Skeleton::find(std::string_view name) const noexcept
{
    for (std::size_t i = 0; i < bones.size(); ++i)
        if (sameName(bones[i].name, name))
            return static_cast<int>(i);
    return -1;
}

namespace anim {

Matrix43 toMatrix(const Transform& t) noexcept
{
    const auto [x, y, z, w] = t.rotation;
    const auto& s = t.scale;
    // Rows are the rotated (and scaled) axes, for row vectors.
    return {(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + z * w) * s[0], 2 * (x * z - y * w) * s[0],
            2 * (x * y - z * w) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + x * w) * s[1],
            2 * (x * z + y * w) * s[2], 2 * (y * z - x * w) * s[2], (1 - 2 * (x * x + y * y)) * s[2],
            t.position[0], t.position[1], t.position[2]};
}

Matrix43 multiply(const Matrix43& a, const Matrix43& b) noexcept
{
    Matrix43 m{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            m[3 * r + c] = a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c] + (r == 3 ? b[9 + c] : 0.0f);
    return m;
}

std::array<float, 3> apply(const Matrix43& m, const std::array<float, 3>& p) noexcept
{
    return {p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9], p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
            p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]};
}

std::array<float, 3> rotate(const Matrix43& m, const std::array<float, 3>& d) noexcept
{
    return {d[0] * m[0] + d[1] * m[3] + d[2] * m[6], d[0] * m[1] + d[1] * m[4] + d[2] * m[7],
            d[0] * m[2] + d[1] * m[5] + d[2] * m[8]};
}

Matrix43 inverse(const Matrix43& m) noexcept
{
    const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12f)
        return {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const float k = 1.0f / det;
    Matrix43 r{(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k,
               (f * g - d * i) * k, (a * i - c * g) * k, (c * d - a * f) * k,
               (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k, 0, 0, 0};
    // p = (p' - t) R^-1  →  translation -t R^-1
    for (int col = 0; col < 3; ++col)
        r[9 + col] = -(m[9] * r[col] + m[10] * r[3 + col] + m[11] * r[6 + col]);
    return r;
}

Transform decompose(const Matrix43& m) noexcept
{
    // Inverse of toMatrix for a pure rotation (rows: m00 m01 m02 / m10 ...).
    Transform t;
    const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[3], m11 = m[4], m12 = m[5], m20 = m[6], m21 = m[7],
                m22 = m[8];
    const float trace = m00 + m11 + m22;
    float x, y, z, w;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2;
        w = 0.25f * s;
        x = (m12 - m21) / s;
        y = (m20 - m02) / s;
        z = (m01 - m10) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        w = (m12 - m21) / s;
        x = 0.25f * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        w = (m20 - m02) / s;
        x = (m01 + m10) / s;
        y = 0.25f * s;
        z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        w = (m01 - m10) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25f * s;
    }
    t.rotation = {x, y, z, w};
    t.position = {m[9], m[10], m[11]};
    return t;
}

Skeleton fromHierarchy(const std::vector<std::string>& names, const std::vector<int>& parents,
                       const std::vector<Matrix43>& transforms)
{
    Skeleton skeleton;
    for (std::size_t i = 0; i < names.size() && i < parents.size() && i < transforms.size(); ++i) {
        Skeleton::Bone bone;
        bone.name = names[i];
        const int parent = parents[i];
        bone.parent = parent >= 0 && static_cast<std::size_t>(parent) < i ? parent : -1;
        const Matrix43 local = bone.parent >= 0
                                   ? multiply(transforms[i], inverse(transforms[static_cast<std::size_t>(bone.parent)]))
                                   : transforms[i];
        bone.rest = decompose(local);
        bone.inverseBind = inverse(transforms[i]);
        skeleton.bones.push_back(std::move(bone));
    }
    return skeleton;
}

std::array<float, 4> slerp(const std::array<float, 4>& a, std::array<float, 4> b, float t) noexcept
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0) { // the short way round
        for (float& c : b)
            c = -c;
        dot = -dot;
    }
    float wa = 1 - t, wb = t;
    if (dot < 0.9995f) {
        const float angle = std::acos(std::min(dot, 1.0f));
        const float s = std::sin(angle);
        wa = std::sin((1 - t) * angle) / s;
        wb = std::sin(t * angle) / s;
    }
    std::array<float, 4> q{};
    float len = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = wa * a[i] + wb * b[i];
        len += q[i] * q[i];
    }
    len = std::sqrt(len);
    if (len > 0)
        for (float& c : q)
            c /= len;
    return q;
}

std::vector<Transform> sample(const Skeleton& skeleton, const Animation* animation, float frame)
{
    std::vector<Transform> locals;
    locals.reserve(skeleton.bones.size());
    for (const Skeleton::Bone& bone : skeleton.bones)
        locals.push_back(bone.rest);
    if (!animation)
        return locals;
    if (const float last = static_cast<float>(animation->frameCount) - 1; last > 0) {
        frame = std::fmod(frame, last);
        if (frame < 0)
            frame += last;
    } else {
        frame = 0;
    }
    for (const Animation::Channel& ch : animation->channels) {
        const int bone = skeleton.find(ch.bone);
        if (bone < 0 || ch.values.empty())
            continue;
        std::array<float, 4> value = ch.values.front();
        if (ch.values.size() > 1 && ch.frames.size() == ch.values.size()) {
            const auto next = std::ranges::upper_bound(ch.frames, frame);
            if (next == ch.frames.end()) {
                value = ch.values.back();
            } else if (next != ch.frames.begin()) {
                const auto i = static_cast<std::size_t>(next - ch.frames.begin());
                const float f0 = ch.frames[i - 1], f1 = ch.frames[i];
                const float t = f1 > f0 ? (frame - f0) / (f1 - f0) : 0.0f;
                if (ch.kind == Animation::Kind::Rotation)
                    value = slerp(ch.values[i - 1], ch.values[i], t);
                else
                    for (int k = 0; k < 4; ++k)
                        value[k] = ch.values[i - 1][k] + (ch.values[i][k] - ch.values[i - 1][k]) * t;
            }
        }
        Transform& local = locals[static_cast<std::size_t>(bone)];
        switch (ch.kind) {
        case Animation::Kind::Rotation:
            local.rotation = value;
            break;
        case Animation::Kind::Position:
            local.position = {value[0], value[1], value[2]};
            break;
        case Animation::Kind::Scale:
            local.scale = {value[0], value[1], value[2]};
            break;
        }
    }
    return locals;
}

std::vector<Matrix43> worldMatrices(const Skeleton& skeleton, const std::vector<Transform>& locals)
{
    std::vector<Matrix43> world(skeleton.bones.size());
    for (std::size_t i = 0; i < skeleton.bones.size() && i < locals.size(); ++i) {
        const Matrix43 local = toMatrix(locals[i]);
        const int parent = skeleton.bones[i].parent;
        world[i] = parent >= 0 && static_cast<std::size_t>(parent) < i
                       ? multiply(local, world[static_cast<std::size_t>(parent)])
                       : local;
    }
    return world;
}

std::vector<Matrix43> skinMatrices(const Skeleton& skeleton, const std::vector<Matrix43>& world)
{
    std::vector<Matrix43> skin(world.size());
    for (std::size_t i = 0; i < world.size() && i < skeleton.bones.size(); ++i)
        skin[i] = multiply(skeleton.bones[i].inverseBind, world[i]);
    return skin;
}

std::size_t matchCount(const Skeleton& skeleton, const std::vector<std::string>& names) noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(names, [&](const std::string& n) { return skeleton.find(n) >= 0; }));
}

bool fits(const Skeleton& skeleton, const Animation& animation) noexcept
{
    // Animations may also move helper bones a model doesn't have (a digger's
    // "AftkRoot" or collision helpers), so most of its bones, not all, must be there.
    std::size_t found = 0;
    for (const std::string& bone : animation.bones)
        found += skeleton.find(bone) >= 0;
    if (animation.bones.empty())
        for (const Animation::Channel& ch : animation.channels)
            found += skeleton.find(ch.bone) >= 0;
    const std::size_t total = animation.bones.empty() ? animation.channels.size() : animation.bones.size();
    return !animation.channels.empty() && found > 0 && 2 * found >= total;
}

} // namespace anim
} // namespace viewer
