#include "ModelView.hpp"

#include "../Icons.hpp"
#include "../QtConvert.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QFileInfo>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace viewer::app {

namespace {

// GLSL 1.10 style (attribute/varying) so it runs on any desktop GL context,
// including the software renderer used on machines without drivers.
constexpr const char* kVertexShader = R"(
attribute vec3 position;
attribute vec3 normal;
attribute vec2 uv;
uniform mat4 mvp;
uniform mat4 model;
uniform mat3 normalMatrix;
varying vec3 vNormal;
varying vec2 vUv;
void main() {
    vNormal = normalMatrix * normal;
    vUv = uv;
    gl_Position = mvp * model * vec4(position, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(
uniform vec3 color;
uniform float lit;
uniform float alpha;
uniform float specular;  // highlight strength (glossy paint, chrome, glass)
uniform float shininess; // how tight the highlights are
uniform float rim;       // how much of the sky the surface reflects (paint, chrome, glass)
uniform vec3 up;         // the world's up, in view space (sky above, ground below)
uniform sampler2D tex;
uniform float textured;  // 1: colour from the part's texture
uniform float alphaTest; // texture alpha below this is cut out
uniform float texAlpha;  // 1: the texture's alpha sets the coverage (glows)
varying vec3 vNormal;
varying vec2 vUv;
void main() {
    vec4 t = textured > 0.5 ? texture2D(tex, vUv) : vec4(1.0);
    if (t.a < alphaTest)
        discard;
    vec3 base = color * t.rgb;
    // A studio around the model, like the game's lighting: a key light from the
    // upper front, a soft fill from the other side, light from the sky above and
    // the ground below, highlights, and the sky reflected at grazing angles.
    // Both faces are lit (models are often single-sided).
    vec3 n = normalize(vNormal);
    if (n.z < 0.0)
        n = -n;
    vec3 v = vec3(0.0, 0.0, 1.0);
    vec3 key = normalize(vec3(-0.45, 0.65, 0.62));
    vec3 fill = normalize(vec3(0.7, 0.05, 0.7));
    float sky = dot(n, up) * 0.5 + 0.5;
    vec3 ambient = mix(vec3(0.20, 0.19, 0.18), vec3(0.42, 0.45, 0.50), sky);
    vec3 diffuse = base * (ambient + vec3(0.78) * max(dot(n, key), 0.0) + vec3(0.22) * max(dot(n, fill), 0.0));
    float highlight = pow(max(dot(n, normalize(key + v)), 0.0), shininess) + 0.35 * pow(max(dot(n, normalize(fill + v)), 0.0), shininess);
    float fresnel = 0.04 + 0.96 * pow(1.0 - n.z, 5.0);
    vec3 r = reflect(-v, n);
    vec3 reflected = mix(vec3(0.16, 0.15, 0.14), vec3(0.75, 0.80, 0.88), smoothstep(-0.25, 0.35, dot(r, up)));
    vec3 shaded = diffuse + vec3(specular * highlight) + reflected * rim * fresnel;
    gl_FragColor = vec4(mix(base, shaded, lit), alpha * mix(1.0, t.a, texAlpha));
}
)";

/// How a part looks with "Materials" on, from the game's effect name.
struct Look {
    QVector3D color;
    float specular = 0.0f;
    float rim = 0.0f;
    float alpha = 1.0f;
    float lit = 1.0f; ///< 1: shaded by the light; lower: glows (lights)
    bool known = false;
    float shininess = 24.0f; ///< Tight (chrome, glass) to broad (rubber) highlights.
};

Look lookFor(const std::string& shader, const QVector3D& paint)
{
    std::string s = shader;
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&](const char* what) { return s.find(what) != std::string::npos; };
    if (has("_paint")) // glossy clear coat over the colour
        return {paint, 0.6f, 0.55f, 1.0f, 1.0f, true, 90.0f};
    if (has("_glass"))
        return {QVector3D(0.10f, 0.13f, 0.16f), 1.0f, 0.9f, 0.45f, 1.0f, true, 160.0f};
    if (has("_light"))
        return {QVector3D(1.0f, 0.96f, 0.88f), 0.3f, 0.2f, 1.0f, 0.35f, true, 60.0f};
    if (has("_neon"))
        return {QVector3D(0.85f, 0.9f, 1.0f), 0.2f, 0.0f, 1.0f, 0.3f, true, 20.0f};
    if (has("_alloy") || has("_chrome"))
        return {QVector3D(0.62f, 0.63f, 0.66f), 0.9f, 0.85f, 1.0f, 1.0f, true, 120.0f};
    if (has("_matt")) // rubber, plastic trim
        return {QVector3D(0.16f, 0.16f, 0.17f), 0.06f, 0.05f, 1.0f, 1.0f, true, 8.0f};
    if (has("veh_default"))
        return {QVector3D(0.30f, 0.31f, 0.33f), 0.15f, 0.12f, 1.0f, 1.0f, true, 30.0f};
    return {};
}

/// The texture that gives a part its colour: the material's diffuse map
/// ("g_MeshTexture", "tAlbedo", "tColour", a light's "off" picture), not its
/// dent, scratch, bump or material maps. -1 when it has none.
int colourTexture(const Mesh::Part& part)
{
    const auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    static constexpr const char* kColour[] = {"albedo", "meshtexture", "colour", "diffuse", "sofftexture", "neon"};
    for (const char* want : kColour)
        for (const Mesh::TextureSlot& slot : part.textures) {
            const std::string name = lower(slot.name);
            if (slot.texture >= 0 && name.find(want) != std::string::npos && name.find("damage") == std::string::npos)
                return slot.texture;
        }
    // Otherwise the first plain picture ("tTexture" on effects): not a map of
    // normals, dents, scratches, distortion, masks or material settings.
    static constexpr const char* kNotColour[] = {"bump", "nrm", "normal", "dent", "scratch", "distort", "mask",
                                                 "material", "spec", "env", "damage"};
    for (const Mesh::TextureSlot& slot : part.textures) {
        const std::string name = lower(slot.name);
        if (slot.texture >= 0
            && std::ranges::none_of(kNotColour, [&](const char* no) { return name.find(no) != std::string::npos; }))
            return slot.texture;
    }
    return -1;
}

/// An effect's tint: its "g_vColour" (rgb times strength in the 4th value), or white.
/// Negative colours darken what's behind instead of lighting it.
QVector3D effectTint(const Mesh::Part& part)
{
    for (const Mesh::Constant& c : part.constants)
        if (c.name == "g_vColour" && c.values.size() >= 4)
            return QVector3D(c.values[0], c.values[1], c.values[2]) * c.values[3];
    return {1, 1, 1};
}

/// How a textured part uses its texture's alpha.
enum class AlphaUse { Opaque, CutOut, Blend, Glow };

AlphaUse alphaUse(const std::string& shader, bool textureHasAlpha)
{
    const QString s = QString::fromStdString(shader);
    if (s.contains("flare", Qt::CaseInsensitive))
        return AlphaUse::Glow; // light flares: added light
    if (!textureHasAlpha && !s.contains("effect", Qt::CaseInsensitive))
        return AlphaUse::Opaque;
    if (s.contains("effect", Qt::CaseInsensitive))
        return AlphaUse::Glow; // effects (pickups, sprites) add light, scaled by the texture's alpha
    // Cut-outs (fences, lamp glass, "veh_Alpha"); other car effects keep their
    // alpha for other uses (gloss), so they stay solid.
    if (s.contains("alpha", Qt::CaseInsensitive) || !s.startsWith("veh_", Qt::CaseInsensitive))
        return AlphaUse::CutOut;
    return AlphaUse::Opaque;
}

QVector3D rgb(const QColor& c)
{
    return {static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF())};
}

/// Squashes the model flat onto the ground (y = h) along a light from above:
/// the ground shadow.
QMatrix4x4 shadowMatrix(float h)
{
    const QVector3D light(0.55f, -1.0f, 0.35f); // direction the light travels
    const float sx = light.x() / light.y();
    const float sz = light.z() / light.y();
    return QMatrix4x4(1, -sx, 0, sx * h, //
                      0, 0, 0, h,        //
                      0, -sz, 1, sz * h, //
                      0, 0, 0, 1);
}

constexpr int kStride = 8 * sizeof(float); // position, normal, uv

} // namespace

// ---- ModelCanvas -------------------------------------------------------------------------

ModelCanvas::ModelCanvas(QWidget* parent) : QOpenGLWidget(parent)
{
    setMinimumSize(200, 150);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("Drag to orbit · right-drag to pan · wheel to zoom · double-click to reset"));
    QSurfaceFormat f = format();
    f.setDepthBufferSize(24);
    f.setStencilBufferSize(8); // the ground shadow darkens each pixel once
    f.setSamples(4);           // smooth wireframe and grid lines
    setFormat(f);
}

ModelCanvas::~ModelCanvas()
{
    makeCurrent();
    releaseTextures();
    m_vertices.destroy();
    m_triangles.destroy();
    m_edges.destroy();
    m_gridLines.destroy();
    doneCurrent();
}

void ModelCanvas::setMesh(std::shared_ptr<const Mesh> mesh)
{
    m_mesh = std::move(mesh);
    m_dirty = true;
    m_lod = 0;
    m_mirror = m_mesh && m_mesh->leftHanded;
    m_visible.assign(m_mesh ? m_mesh->parts.size() : 0, true);
    m_highlighted.assign(m_visible.size(), false);
    m_helper.assign(m_visible.size(), false);
    m_partTexture.assign(m_visible.size(), -1);
    for (std::size_t p = 0; p < m_helper.size(); ++p) {
        m_helper[p] = m_mesh->isHelper(m_mesh->parts[p]);
        m_partTexture[p] = colourTexture(m_mesh->parts[p]);
    }
    // Bounds of the real geometry (not shadow hulls or collision) → camera framing, grid and ground.
    if (m_mesh && m_mesh->vertexCount()) {
        QVector3D lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        const auto include = [&](std::uint32_t v) {
            const QVector3D p(m_mesh->positions[3 * v], m_mesh->positions[3 * v + 1], m_mesh->positions[3 * v + 2]);
            lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
            hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
        };
        for (std::size_t p = 0; p < m_mesh->parts.size(); ++p)
            if (!m_helper[p])
                for (const Mesh::Range& r : m_mesh->parts[p].lods)
                    for (std::uint32_t i = r.firstIndex; i < r.firstIndex + r.indexCount; ++i)
                        include(m_mesh->indices[i]);
        if (lo.x() > hi.x()) // only helpers: use everything
            for (std::uint32_t v = 0; v < m_mesh->vertexCount(); ++v)
                include(v);
        m_center = (lo + hi) * 0.5f;
        m_radius = std::max(0.001f, (hi - lo).length() * 0.5f);
        m_ground = lo.y();
        buildGrid(lo, hi);
    }
    resetView();
}

void ModelCanvas::buildGrid(const QVector3D& lo, const QVector3D& hi)
{
    // A "nice" spacing (…, 0.1, 1, 10, …) with every 5th line stronger.
    const float spacing = std::pow(10.0f, std::floor(std::log10(std::max(m_radius / 2.0f, 1e-4f))));
    const float margin = m_radius * 0.6f;
    const auto from = [&](float v) { return std::floor((v - margin) / spacing); };
    const auto to = [&](float v) { return std::ceil((v + margin) / spacing); };
    const int x0 = static_cast<int>(from(lo.x())), x1 = static_cast<int>(to(hi.x()));
    const int z0 = static_cast<int>(from(lo.z())), z1 = static_cast<int>(to(hi.z()));
    std::vector<float> minor, major;
    const auto line = [](std::vector<float>& out, float ax, float az, float bx, float bz, float y) {
        out.insert(out.end(), {ax, y, az, 0, 1, 0, 0, 0, bx, y, bz, 0, 1, 0, 0, 0});
    };
    for (int i = x0; i <= x1; ++i)
        line(i % 5 ? minor : major, i * spacing, z0 * spacing, i * spacing, z1 * spacing, m_ground);
    for (int k = z0; k <= z1; ++k)
        line(k % 5 ? minor : major, x0 * spacing, k * spacing, x1 * spacing, k * spacing, m_ground);
    m_gridMinorPoints = static_cast<int>(minor.size() / 8);
    m_gridMajorPoints = static_cast<int>(major.size() / 8);
    m_gridData = std::move(minor);
    m_gridData.insert(m_gridData.end(), major.begin(), major.end());
}

void ModelCanvas::setLod(int lod)
{
    m_lod = std::max(0, lod);
    update();
}

void ModelCanvas::setVisibleParts(std::vector<bool> visible)
{
    m_visible = std::move(visible);
    update();
}

void ModelCanvas::setHighlightedParts(std::vector<int> parts)
{
    m_highlighted.assign(m_visible.size(), false);
    for (int p : parts)
        if (p >= 0 && static_cast<std::size_t>(p) < m_highlighted.size())
            m_highlighted[static_cast<std::size_t>(p)] = true;
    update();
}

void ModelCanvas::setShowSolid(bool on)
{
    m_solid = on;
    update();
}

void ModelCanvas::setShowWireframe(bool on)
{
    m_wireframe = on;
    update();
}

void ModelCanvas::setShowGrid(bool on)
{
    m_grid = on;
    update();
}

void ModelCanvas::setShowShadow(bool on)
{
    m_shadow = on;
    update();
}

void ModelCanvas::setShowMaterials(bool on)
{
    m_materials = on;
    update();
}

void ModelCanvas::setPaintColor(const QColor& color)
{
    m_paint = rgb(color);
    update();
}

void ModelCanvas::resetView()
{
    m_target = m_center;
    m_distance = m_radius * 2.6f;
    m_yaw = -35.0f;
    m_pitch = 20.0f;
    update();
}

bool ModelCanvas::partVisible(std::size_t part) const
{
    return part < m_visible.size() && m_visible[part];
}

void ModelCanvas::initializeGL()
{
    initializeOpenGLFunctions();
    m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    m_program.bindAttributeLocation("position", 0);
    m_program.bindAttributeLocation("normal", 1);
    m_program.bindAttributeLocation("uv", 2);
    m_glReady = m_program.link();
    m_vertices.create();
    m_triangles.create();
    m_edges.create();
    m_gridLines.create();
    m_dirty = true;
}

void ModelCanvas::upload()
{
    m_dirty = false;
    m_hasGeometry = false;
    m_edgeRanges.clear();
    if (!m_mesh || m_mesh->indices.empty())
        return;
    const Mesh& mesh = *m_mesh;

    // Smooth normals (area weighted). Blur splits vertices at hard edges, so this matches the game closely.
    // Lower detail levels reuse some of the same vertices with coarser triangles, so a
    // vertex takes its normal from the most detailed level that uses it.
    const auto pos = [&](std::uint32_t i) {
        return QVector3D(mesh.positions[3 * i], mesh.positions[3 * i + 1], mesh.positions[3 * i + 2]);
    };
    std::vector<QVector3D> detailed(mesh.vertexCount()), coarse(mesh.vertexCount());
    for (const Mesh::Part& part : mesh.parts)
        for (std::size_t level = 0; level < part.lods.size(); ++level) {
            auto& normals = level == 0 ? detailed : coarse;
            const Mesh::Range& r = part.lods[level];
            for (std::uint32_t t = r.firstIndex; t + 2 < r.firstIndex + r.indexCount; t += 3) {
                const auto a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
                const QVector3D n = QVector3D::crossProduct(pos(b) - pos(a), pos(c) - pos(a));
                normals[a] += n;
                normals[b] += n;
                normals[c] += n;
            }
        }
    // The file's own normals win when it has them (they include the artists' smoothing).
    const bool fileNormals = mesh.normals.size() == mesh.positions.size();
    const bool uvs = mesh.uvs.size() == 2 * mesh.vertexCount();
    std::vector<float> interleaved;
    interleaved.reserve(mesh.vertexCount() * 8);
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
        QVector3D n = (detailed[v].isNull() ? coarse[v] : detailed[v]).normalized();
        if (fileNormals) {
            const QVector3D stored(mesh.normals[3 * v], mesh.normals[3 * v + 1], mesh.normals[3 * v + 2]);
            if (stored.lengthSquared() > 0.25f)
                n = stored.normalized();
        }
        interleaved.insert(interleaved.end(), {mesh.positions[3 * v], mesh.positions[3 * v + 1],
                                               mesh.positions[3 * v + 2], n.x(), n.y(), n.z(),
                                               uvs ? mesh.uvs[2 * v] : 0.0f, uvs ? mesh.uvs[2 * v + 1] : 0.0f});
    }
    uploadTextures();
    m_vertices.bind();
    m_vertices.allocate(interleaved.data(), static_cast<int>(interleaved.size() * sizeof(float)));
    m_rest = std::move(interleaved);
    m_poseDirty = !m_boneMatrices.empty();
    m_triangles.bind();
    m_triangles.allocate(mesh.indices.data(), static_cast<int>(mesh.indices.size() * sizeof(std::uint32_t)));

    // Unique edges for the wireframe, per part and level.
    std::vector<std::uint32_t> lines;
    for (const Mesh::Part& part : mesh.parts) {
        auto& ranges = m_edgeRanges.emplace_back();
        for (const Mesh::Range& r : part.lods) {
            std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
            for (std::uint32_t i = r.firstIndex; i + 2 < r.firstIndex + r.indexCount; i += 3)
                for (int e = 0; e < 3; ++e) {
                    const auto a = mesh.indices[i + e], b = mesh.indices[i + (e + 1) % 3];
                    edges.emplace(std::min(a, b), std::max(a, b));
                }
            ranges.push_back({static_cast<int>(lines.size()), static_cast<int>(edges.size() * 2)});
            for (const auto& [a, b] : edges)
                lines.insert(lines.end(), {a, b});
        }
    }
    m_edges.bind();
    m_edges.allocate(lines.data(), static_cast<int>(lines.size() * sizeof(std::uint32_t)));

    m_gridLines.bind();
    m_gridLines.allocate(m_gridData.data(), static_cast<int>(m_gridData.size() * sizeof(float)));
    m_hasGeometry = true;
}

void ModelCanvas::setSkinning(std::vector<Influence> influences)
{
    m_influences = std::move(influences);
    m_poseDirty = true;
    update();
}

void ModelCanvas::setBoneMatrices(std::vector<Matrix43> matrices)
{
    m_boneMatrices = std::move(matrices);
    m_poseDirty = true;
    update();
}

void ModelCanvas::applyPose()
{
    m_poseDirty = false;
    if (m_rest.empty())
        return;
    std::vector<float> posed = m_rest;
    const std::size_t count = std::min(m_influences.size(), posed.size() / 8);
    for (std::size_t v = 0; v < count; ++v) {
        const Influence& in = m_influences[v];
        if (in.bone[0] < 0)
            continue;
        float* out = &posed[8 * v];
        const std::array<float, 3> p{out[0], out[1], out[2]}, n{out[3], out[4], out[5]};
        std::array<float, 3> sp{}, sn{};
        float total = 0;
        for (int k = 0; k < 4; ++k) {
            const int b = in.bone[k];
            if (b < 0 || static_cast<std::size_t>(b) >= m_boneMatrices.size() || in.weight[k] <= 0)
                continue;
            const auto q = anim::apply(m_boneMatrices[static_cast<std::size_t>(b)], p);
            const auto r = anim::rotate(m_boneMatrices[static_cast<std::size_t>(b)], n);
            for (int c = 0; c < 3; ++c) {
                sp[c] += q[c] * in.weight[k];
                sn[c] += r[c] * in.weight[k];
            }
            total += in.weight[k];
        }
        if (total <= 0)
            continue;
        const float len = std::sqrt(sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2]);
        for (int c = 0; c < 3; ++c) {
            out[c] = sp[c] / total;
            out[3 + c] = len > 0 ? sn[c] / len : n[c];
        }
    }
    m_vertices.bind();
    m_vertices.write(0, posed.data(), static_cast<int>(posed.size() * sizeof(float)));
}

void ModelCanvas::releaseTextures()
{
    if (!m_glTextures.empty())
        glDeleteTextures(static_cast<GLsizei>(m_glTextures.size()), m_glTextures.data());
    m_glTextures.clear();
}

void ModelCanvas::uploadTextures()
{
    releaseTextures();
    if (!m_mesh)
        return;
    m_glTextures.assign(m_mesh->textures.size(), 0);
    m_textureAlpha.assign(m_mesh->textures.size(), false);
    for (std::size_t t = 0; t < m_mesh->textures.size(); ++t) {
        const Image& image = m_mesh->textures[t].image;
        if (image.width == 0 || image.height == 0 || image.rgba.size() < std::size_t{image.width} * image.height * 4)
            continue;
        for (std::size_t i = 3; i < image.rgba.size(); i += 4)
            if (image.rgba[i] < 250) {
                m_textureAlpha[t] = true;
                break;
            }
        glGenTextures(1, &m_glTextures[t]);
        glBindTexture(GL_TEXTURE_2D, m_glTextures[t]);
        // Rows top first, as the game's texture coordinates expect (v down).
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); // tyre treads and the like tile
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

QMatrix4x4 ModelCanvas::viewMatrix() const
{
    QMatrix4x4 view;
    view.translate(0, 0, -m_distance);
    view.rotate(m_pitch, 1, 0, 0);
    view.rotate(m_yaw, 0, 1, 0);
    if (m_mirror)
        view.scale(-1.0f, 1.0f, 1.0f);
    view.translate(-m_target);
    return view;
}

void ModelCanvas::bindVertices(QOpenGLBuffer& buffer)
{
    buffer.bind();
    m_program.setAttributeBuffer(0, GL_FLOAT, 0, 3, kStride);
    m_program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, kStride);
    m_program.setAttributeBuffer(2, GL_FLOAT, 6 * sizeof(float), 2, kStride);
}

void ModelCanvas::paintGL()
{
    const QColor background = palette().color(QPalette::Window);
    glClearColor(static_cast<float>(background.redF()), static_cast<float>(background.greenF()),
                 static_cast<float>(background.blueF()), 1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (!m_glReady)
        return;
    if (m_dirty)
        upload();
    if (!m_hasGeometry)
        return;
    if (m_poseDirty)
        applyPose();
    const Mesh& mesh = *m_mesh;
    const bool dark = background.lightnessF() < 0.5;

    glEnable(GL_DEPTH_TEST);
    // Near/far planes from the model size keep depth precision good for tiny and huge models.
    QMatrix4x4 projection;
    projection.perspective(45.0f, static_cast<float>(width()) / static_cast<float>(std::max(1, height())),
                           std::max(m_distance - m_radius * 4.0f, std::min(m_radius * 0.01f, m_distance * 0.05f)),
                           m_distance + m_radius * 4.0f);
    const QMatrix4x4 view = viewMatrix();
    m_program.bind();
    m_program.setUniformValue("mvp", projection * view);
    m_program.setUniformValue("model", QMatrix4x4());
    m_program.setUniformValue("normalMatrix", view.normalMatrix());
    m_program.setUniformValue("alpha", 1.0f);
    m_program.setUniformValue("specular", 0.0f);
    m_program.setUniformValue("shininess", 24.0f);
    m_program.setUniformValue("rim", 0.0f);
    m_program.setUniformValue("up", view.mapVector(QVector3D(0, 1, 0)).normalized());
    m_program.setUniformValue("tex", 0);
    m_program.setUniformValue("textured", 0.0f);
    m_program.setUniformValue("alphaTest", 0.0f);
    m_program.setUniformValue("texAlpha", 0.0f);
    m_program.enableAttributeArray(0);
    m_program.enableAttributeArray(1);
    m_program.enableAttributeArray(2);

    const auto lodRange = [&](std::size_t part) -> const Mesh::Range* {
        const auto& lods = mesh.parts[part].lods;
        return static_cast<std::size_t>(m_lod) < lods.size() && lods[m_lod].indexCount ? &lods[m_lod] : nullptr;
    };
    const auto drawTriangles = [&](const Mesh::Range& r) {
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(r.indexCount), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(static_cast<std::uintptr_t>(r.firstIndex) * sizeof(std::uint32_t)));
    };

    // Ground grid
    if (m_grid && (m_gridMinorPoints || m_gridMajorPoints)) {
        const QVector3D text = rgb(palette().color(QPalette::Text));
        const QVector3D back = rgb(background);
        m_program.setUniformValue("lit", 0.0f);
        bindVertices(m_gridLines);
        m_program.setUniformValue("color", back + (text - back) * 0.13f);
        glDrawArrays(GL_LINES, 0, m_gridMinorPoints);
        m_program.setUniformValue("color", back + (text - back) * 0.3f);
        glDrawArrays(GL_LINES, m_gridMinorPoints, m_gridMajorPoints);
    }

    bindVertices(m_vertices);
    m_triangles.bind();

    // Ground shadow: the visible parts squashed onto the ground, each pixel darkened once.
    if (m_shadow) {
        m_program.setUniformValue("model", shadowMatrix(m_ground + m_radius * 0.002f));
        m_program.setUniformValue("color", QVector3D(0, 0, 0));
        m_program.setUniformValue("lit", 0.0f);
        m_program.setUniformValue("alpha", dark ? 0.7f : 0.3f);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE); // keep the picture opaque
        glDepthMask(GL_FALSE);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        for (std::size_t p = 0; p < mesh.parts.size(); ++p)
            if (partVisible(p) && !m_helper[p])
                if (const Mesh::Range* r = lodRange(p))
                    drawTriangles(*r);
        glDisable(GL_STENCIL_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        m_program.setUniformValue("model", QMatrix4x4());
        m_program.setUniformValue("alpha", 1.0f);
    }

    const QVector3D base = dark ? QVector3D(0.78f, 0.80f, 0.84f) : QVector3D(0.70f, 0.72f, 0.76f);
    const QVector3D accent = rgb(palette().color(QPalette::Highlight));
    const QVector3D hull(0.62f, 0.45f, 0.78f); // shadow hulls, when shown
    const auto ownColor = [](std::uint32_t c) {
        return QVector3D(static_cast<float>((c >> 16) & 0xFF), static_cast<float>((c >> 8) & 0xFF),
                         static_cast<float>(c & 0xFF)) / 255.0f;
    };
    // A model's collision is drawn see-through over the model, and outlined.
    const auto overlay = [&](std::size_t p) { return mesh.parts[p].collision && m_helper[p]; };
    if (m_solid) {
        glEnable(GL_POLYGON_OFFSET_FILL); // keeps the wireframe overlay on top of the faces
        glPolygonOffset(1.0f, 1.0f);
        // Opaque parts first, then see-through ones (glass) blended over them.
        std::vector<std::pair<std::size_t, Look>> clear;
        const auto drawPart = [&](std::size_t p, const Look& look) {
            // With materials on, a part with a colour texture shows it (lit like the rest).
            const int t = m_partTexture[p];
            const bool textured = m_materials && t >= 0 && static_cast<std::size_t>(t) < m_glTextures.size()
                                  && m_glTextures[static_cast<std::size_t>(t)] != 0 && !overlay(p);
            const AlphaUse use0 = textured ? alphaUse(mesh.parts[p].shader, m_textureAlpha[static_cast<std::size_t>(t)])
                                           : AlphaUse::Opaque;
            QVector3D tint = use0 == AlphaUse::Glow ? effectTint(mesh.parts[p]) : QVector3D(1, 1, 1);
            const bool darken = tint.x() < 0 && tint.y() <= 0 && tint.z() <= 0;
            if (darken)
                tint = -tint;
            const QVector3D c = textured ? tint : look.color;
            if (textured)
                glBindTexture(GL_TEXTURE_2D, m_glTextures[static_cast<std::size_t>(t)]);
            m_program.setUniformValue("textured", textured ? 1.0f : 0.0f);
            const AlphaUse use = use0;
            // Cut-outs drop their see-through pixels with a clean edge; blended
            // effects fade with the texture's alpha (fully clear pixels dropped).
            m_program.setUniformValue("alphaTest", use == AlphaUse::CutOut ? 0.5f : use == AlphaUse::Blend ? 0.02f : 0.0f);
            m_program.setUniformValue("texAlpha", use == AlphaUse::Blend ? 1.0f : 0.0f);
            m_program.setUniformValue("lit", look.lit);
            m_program.setUniformValue("specular", look.specular);
            m_program.setUniformValue("shininess", look.shininess);
            m_program.setUniformValue("rim", look.rim);
            m_program.setUniformValue("alpha", look.alpha);
            m_program.setUniformValue("color", m_highlighted[p] ? c * 0.35f + accent * 0.65f : c);
            // Light flares are glows the game adds on top of the picture.
            const bool glow = use == AlphaUse::Glow;
            const bool wasBlending = glIsEnabled(GL_BLEND); // in the see-through pass
            if (glow) {
                m_program.setUniformValue("lit", 0.0f);
                m_program.setUniformValue("texAlpha", 1.0f);
                glEnable(GL_BLEND);
                // Added light on a dark background; on a light one that washes out, so it's laid over faintly.
                if (darken) { // a negative tint takes light away
                    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
                } else if (dark) {
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE); // the picture stays opaque
                } else {
                    m_program.setUniformValue("alpha", 0.35f);
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
                }
                glDepthMask(GL_FALSE);
            }
            if (const Mesh::Range* r = lodRange(p))
                drawTriangles(*r);
            m_program.setUniformValue("texAlpha", 0.0f);
            if (glow)
                glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            if (glow && wasBlending) { // back to the see-through pass's blending
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            } else if (glow) {
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }
        };
        for (std::size_t p = 0; p < mesh.parts.size(); ++p) {
            if (!partVisible(p) || !lodRange(p))
                continue;
            const Mesh::Part& part = mesh.parts[p];
            Look look = m_materials && !part.shadow && !part.collision ? lookFor(part.shader, m_paint) : Look{};
            if (!look.known)
                look.color = part.shadow ? hull : part.color ? ownColor(part.color) : base;
            if (overlay(p))
                look.alpha = 0.3f;
            if (const int t = m_partTexture[p]; m_materials && t >= 0 && static_cast<std::size_t>(t) < m_textureAlpha.size()) {
                const AlphaUse use = alphaUse(part.shader, m_textureAlpha[static_cast<std::size_t>(t)]);
                if (use == AlphaUse::Blend || use == AlphaUse::Glow)
                    look.alpha = std::min(look.alpha, 0.999f); // drawn after the solid parts
            }
            if (look.alpha < 1.0f && !m_highlighted[p]) {
                clear.emplace_back(p, look);
                continue;
            }
            look.alpha = 1.0f;
            drawPart(p, look);
        }
        if (!clear.empty()) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE); // keep the picture opaque
            glDepthMask(GL_FALSE);
            for (const auto& [p, look] : clear)
                drawPart(p, look);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
        m_program.setUniformValue("specular", 0.0f);
        m_program.setUniformValue("rim", 0.0f);
        m_program.setUniformValue("alpha", 1.0f);
        m_program.setUniformValue("textured", 0.0f);
        m_program.setUniformValue("alphaTest", 0.0f);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    {
        // Over faces: black, readable on every part; without faces: the text colour (black would vanish in dark mode).
        const QVector3D line = m_solid ? QVector3D(0, 0, 0) : rgb(palette().color(QPalette::Text));
        const bool allEdges = m_wireframe || !m_solid;
        m_program.setUniformValue("lit", 0.0f);
        m_edges.bind();
        for (std::size_t p = 0; p < mesh.parts.size() && p < m_edgeRanges.size(); ++p) {
            if (!partVisible(p) || !lodRange(p) || !(allEdges || overlay(p)))
                continue;
            const EdgeRange& e = m_edgeRanges[p][static_cast<std::size_t>(m_lod)];
            const QVector3D color = m_highlighted[p] ? accent
                                    : overlay(p)     ? ownColor(mesh.parts[p].color) * (dark ? 1.0f : 0.6f)
                                                     : line;
            m_program.setUniformValue("color", color);
            glDrawElements(GL_LINES, e.count, GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(static_cast<std::uintptr_t>(e.first) * sizeof(std::uint32_t)));
        }
    }
    m_program.disableAttributeArray(0);
    m_program.disableAttributeArray(1);
    m_program.disableAttributeArray(2);
    m_program.release();
}

void ModelCanvas::mousePressEvent(QMouseEvent* event)
{
    m_lastMouse = event->position().toPoint();
    setCursor(Qt::ClosedHandCursor);
}

void ModelCanvas::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    const QPoint delta = pos - m_lastMouse;
    m_lastMouse = pos;
    if (event->buttons() & Qt::LeftButton) {
        m_yaw += static_cast<float>(delta.x()) * 0.4f;
        m_pitch = std::clamp(m_pitch + static_cast<float>(delta.y()) * 0.4f, -89.0f, 89.0f);
    } else if (event->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        // Pan in the view plane, scaled so the model follows the cursor.
        const float scale = m_distance * 2.0f * std::tan(qDegreesToRadians(22.5f)) / static_cast<float>(height());
        const QMatrix4x4 inverse = viewMatrix().inverted();
        const QVector3D right = inverse.mapVector({1, 0, 0});
        const QVector3D up = inverse.mapVector({0, 1, 0});
        m_target -= right * static_cast<float>(delta.x()) * scale;
        m_target += up * static_cast<float>(delta.y()) * scale;
    } else {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    update();
}

void ModelCanvas::mouseDoubleClickEvent(QMouseEvent*)
{
    resetView();
}

void ModelCanvas::wheelEvent(QWheelEvent* event)
{
    const float steps = static_cast<float>(event->angleDelta().y()) / 120.0f;
    // Close enough to look at a corner of a level, far enough to see it all.
    m_distance = std::clamp(m_distance * std::pow(0.85f, steps), m_radius * 0.002f, m_radius * 50.0f);
    update();
}

// ---- ModelView ---------------------------------------------------------------------------

ModelView::ModelView(QWidget* parent)
    : QWidget(parent), m_canvas(new ModelCanvas(this)), m_splitter(new QSplitter(this)),
      m_parts(new QTreeWidget(this)), m_info(new QLabel(this)), m_solid(new QCheckBox(tr("Solid"), this)),
      m_wireframe(new QCheckBox(tr("Wireframe"), this)), m_grid(new QCheckBox(tr("Grid"), this)),
      m_shadow(new QCheckBox(tr("Shadow"), this)), m_collision(new QCheckBox(tr("Collision"), this)),
      m_materials(new QCheckBox(tr("Materials"), this)),
      m_paint(new QToolButton(this)), m_lod(new QComboBox(this)),
      m_animationBar(new QWidget(this)), m_skeletonLabel(new QLabel(this)), m_chooseAnimation(new QToolButton(this)),
      m_stepBack(new QToolButton(this)), m_playBack(new QToolButton(this)), m_pause(new QToolButton(this)),
      m_playForward(new QToolButton(this)), m_stepForward(new QToolButton(this)), m_speed(new QComboBox(this)),
      m_timeline(new QSlider(Qt::Horizontal, this)), m_frameLabel(new QLabel(this)), m_timer(new QTimer(this))
{
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // Display options (remembered between files and runs).
    QSettings settings;
    const auto option = [&](QCheckBox* box, const char* key, bool fallback, const QString& tip,
                            void (ModelCanvas::*apply)(bool)) {
        box->setChecked(settings.value(QStringLiteral("preview/model/%1").arg(QLatin1String(key)), fallback).toBool());
        box->setToolTip(tip);
        (m_canvas->*apply)(box->isChecked());
        connect(box, &QCheckBox::toggled, this, [this, key, apply](bool on) {
            QSettings().setValue(QStringLiteral("preview/model/%1").arg(QLatin1String(key)), on);
            (m_canvas->*apply)(on);
        });
    };
    option(m_solid, "solid", true, tr("Draw the faces"), &ModelCanvas::setShowSolid);
    option(m_wireframe, "wireframe", false, tr("Draw the triangle edges"), &ModelCanvas::setShowWireframe);
    option(m_grid, "grid", true, tr("Ground grid (lines every 1, 10, 100… units; every 5th stronger)"),
           &ModelCanvas::setShowGrid);
    option(m_shadow, "shadow", true, tr("Soft shadow on the ground"), &ModelCanvas::setShowShadow);
    option(m_materials, "materials", true,
           tr("Colour parts by the game's material: car paint, glass, lights, chrome… (textures aren't applied yet)"),
           &ModelCanvas::setShowMaterials);

    // Car paint colour: the game lets the player choose it, so it's ours to pick too.
    m_canvas->setPaintColor(settings.value("preview/model/paint", QColor(140, 18, 16)).value<QColor>());
    m_paint->setAutoRaise(true);
    m_paint->setToolTip(tr("Car paint colour"));
    updatePaintButton();
    connect(m_paint, &QToolButton::clicked, this, [this] {
        QSettings store;
        const QColor current = store.value("preview/model/paint", QColor(140, 18, 16)).value<QColor>();
        const QColor chosen = QColorDialog::getColor(current, this, tr("Car Paint Colour"));
        if (!chosen.isValid())
            return;
        store.setValue("preview/model/paint", chosen);
        m_canvas->setPaintColor(chosen);
        updatePaintButton();
    });
    // Collision shapes are parts of the model, so this box ticks them in the parts list.
    m_collision->setChecked(settings.value("preview/model/collision", false).toBool());
    m_collision->setToolTip(tr("Show the shapes the game's physics uses (drawn see-through over the model)"));
    m_collision->setVisible(false);
    connect(m_collision, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("preview/model/collision", on);
        setCollisionShown(on);
    });
    connect(m_materials, &QCheckBox::toggled, m_paint, &QWidget::setEnabled);
    m_paint->setEnabled(m_materials->isChecked());

    m_lod->setToolTip(tr("Level of detail: what the game draws close up (LOD 0) or far away"));
    connect(m_lod, &QComboBox::currentIndexChanged, this, [this](int lod) {
        m_canvas->setLod(std::max(0, lod));
        updateCounts();
    });

    auto* reset = new QToolButton(this);
    reset->setIcon(icons::theme(QIcon::ThemeIcon::ZoomFitBest, QStyle::SP_TitleBarMaxButton));
    reset->setToolTip(tr("Reset view (double-click the model)"));
    reset->setAutoRaise(true);
    connect(reset, &QToolButton::clicked, m_canvas, &ModelCanvas::resetView);

    // Parts list: tick to show/hide; select to highlight in the view.
    m_parts->setHeaderLabels({tr("Part"), tr("Triangles")});
    m_parts->setUniformRowHeights(true);
    m_parts->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_parts->setContextMenuPolicy(Qt::CustomContextMenu);
    m_parts->header()->setStretchLastSection(false);
    m_parts->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_parts->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(m_parts, &QTreeWidget::itemChanged, this, [this] {
        if (m_updatingTree)
            return;
        applyVisibility();
        // Keep the Collision box in step with ticks made in the list.
        bool anyShown = false;
        for (std::size_t i = 0; i < m_partItems.size(); ++i)
            if (m_partItems[i] && m_mesh->parts[i].collision && m_mesh->isHelper(m_mesh->parts[i])
                && m_partItems[i]->checkState(0) == Qt::Checked)
                anyShown = true;
        const QSignalBlocker block(m_collision);
        m_collision->setChecked(anyShown);
    });
    connect(m_parts, &QTreeWidget::itemSelectionChanged, this, [this] {
        std::vector<int> selected;
        for (QTreeWidgetItem* item : m_parts->selectedItems())
            collectParts(item, selected);
        m_canvas->setHighlightedParts(std::move(selected));
    });
    connect(m_parts, &QTreeWidget::customContextMenuRequested, this, &ModelView::showPartsMenu);

    // Display options: a column in the top right corner of the 3D view, under the
    // LOD picker. No background, so the model stays visible behind it.
    auto* options = new QWidget(this);
    auto* optionsColumn = new QVBoxLayout(options);
    optionsColumn->setContentsMargins(8, 6, 8, 6);
    optionsColumn->setSpacing(2);
    for (QCheckBox* box : {m_solid, m_wireframe, m_grid, m_shadow, m_collision})
        optionsColumn->addWidget(box);
    auto* materialsRow = new QHBoxLayout;
    materialsRow->setSpacing(2);
    materialsRow->addWidget(m_materials);
    materialsRow->addWidget(m_paint);
    materialsRow->addStretch(1);
    optionsColumn->addLayout(materialsRow);

    // The 3D view with the animation controls under it (the parts list keeps its full height).
    auto* viewArea = new QWidget(this);
    auto* viewLayout = new QGridLayout(viewArea);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    viewLayout->setSpacing(0);
    viewLayout->addWidget(m_canvas, 0, 0);
    viewLayout->addWidget(options, 0, 0, Qt::AlignTop | Qt::AlignRight);
    viewLayout->addWidget(m_animationBar, 1, 0);
    viewLayout->setRowStretch(0, 1);
    options->raise();
    m_splitter->addWidget(m_parts);
    m_splitter->addWidget(viewArea);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(true);
    if (!m_splitter->restoreState(settings.value("preview/model/splitter").toByteArray()))
        m_splitter->setSizes({260, 800});
    connect(m_splitter, &QSplitter::splitterMoved, this,
            [this] { QSettings().setValue("preview/model/splitter", m_splitter->saveState()); });

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(8, 4, 8, 4);
    bar->addWidget(m_info, 1);
    bar->addWidget(m_lod);
    bar->addWidget(reset);

    // Animation bar: shown when the model has a skeleton.
    m_skeletonLabel->setForegroundRole(QPalette::PlaceholderText);
    m_chooseAnimation->setText(tr("Animation…"));
    m_chooseAnimation->setIcon(icons::forCategory(FileCategory::Animation));
    m_chooseAnimation->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_chooseAnimation->setAutoRaise(true);
    m_chooseAnimation->setToolTip(tr("Choose one of the game's animations for this skeleton"));
    connect(m_chooseAnimation, &QToolButton::clicked, this, &ModelView::animationRequested);
    const QIcon playIcon = icons::theme(QIcon::ThemeIcon::MediaPlaybackStart, QStyle::SP_MediaPlay);
    const QPixmap playPixmap = playIcon.pixmap(QSize(32, 32));
    const auto button = [&](QToolButton* b, const QIcon& icon, const QString& tip) {
        b->setIcon(icon);
        b->setToolTip(tip);
        b->setAutoRaise(true);
    };
    button(m_stepBack, icons::theme(QIcon::ThemeIcon::MediaSkipBackward, QStyle::SP_MediaSkipBackward),
           tr("One frame back"));
    button(m_playBack, QIcon(playPixmap.transformed(QTransform().scale(-1, 1))), tr("Play backwards"));
    button(m_pause, icons::theme(QIcon::ThemeIcon::MediaPlaybackPause, QStyle::SP_MediaPause), tr("Pause"));
    button(m_playForward, playIcon, tr("Play"));
    button(m_stepForward, icons::theme(QIcon::ThemeIcon::MediaSkipForward, QStyle::SP_MediaSkipForward),
           tr("One frame forward"));
    for (auto* b : {m_playBack, m_pause, m_playForward})
        b->setCheckable(true);
    connect(m_stepBack, &QToolButton::clicked, this, [this] { step(-1); });
    connect(m_stepForward, &QToolButton::clicked, this, [this] { step(1); });
    connect(m_playBack, &QToolButton::clicked, this, [this] { play(-1); });
    connect(m_pause, &QToolButton::clicked, this, [this] { play(0); });
    connect(m_playForward, &QToolButton::clicked, this, [this] { play(1); });
    for (const double speed : {0.1, 0.25, 0.5, 1.0, 2.0, 4.0})
        m_speed->addItem(QStringLiteral("%1×").arg(QLocale().toString(speed)), speed);
    m_speed->setCurrentIndex(m_speed->findData(settings.value("preview/model/animationSpeed", 1.0).toDouble()));
    if (m_speed->currentIndex() < 0)
        m_speed->setCurrentIndex(3);
    m_speed->setToolTip(tr("Playback speed"));
    connect(m_speed, &QComboBox::currentIndexChanged, this,
            [this] { QSettings().setValue("preview/model/animationSpeed", m_speed->currentData()); });
    m_timeline->setToolTip(tr("Frame"));
    connect(m_timeline, &QSlider::valueChanged, this, [this](int frame) {
        if (m_direction == 0 || m_timeline->isSliderDown())
            showFrame(static_cast<float>(frame));
    });
    connect(m_timeline, &QSlider::sliderPressed, this, [this] { play(0); });
    m_frameLabel->setMinimumWidth(m_frameLabel->fontMetrics().horizontalAdvance(QStringLiteral("0000 / 0000")));
    m_frameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!m_animation || m_direction == 0)
            return;
        const float seconds = static_cast<float>(m_clock.restart()) / 1000.0f;
        showFrame(m_frame + static_cast<float>(m_direction) * m_speed->currentData().toFloat() * m_animation->fps
                                * seconds);
    });

    auto* animationRow = new QHBoxLayout(m_animationBar);
    animationRow->setContentsMargins(8, 4, 8, 4);
    animationRow->addWidget(m_skeletonLabel);
    animationRow->addWidget(m_chooseAnimation);
    animationRow->addSpacing(8);
    for (auto* b : {m_stepBack, m_playBack, m_pause, m_playForward, m_stepForward})
        animationRow->addWidget(b);
    animationRow->addWidget(m_speed);
    animationRow->addWidget(m_timeline, 1);
    animationRow->addWidget(m_frameLabel);
    m_animationBar->setVisible(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_splitter, 1);
}

void ModelView::setSkeleton(std::shared_ptr<const Skeleton> skeleton, const QString& source)
{
    play(0);
    m_skeleton = std::move(skeleton);
    m_animation.reset();
    m_frame = 0;
    m_skeletonLabel->setText(m_skeleton ? tr("Skeleton: %1").arg(QFileInfo(source).completeBaseName().isEmpty()
                                                                      ? source
                                                                      : QFileInfo(source).completeBaseName())
                                        : QString());
    m_skeletonLabel->setToolTip(m_skeleton ? tr("Posed with %1 (%n bone(s))", nullptr,
                                                static_cast<int>(m_skeleton->bones.size()))
                                                 .arg(source)
                                           : QString());
    buildInfluences();
    updateAnimationBar();
    showFrame(0);
}

void ModelView::setAnimation(std::shared_ptr<const Animation> animation, const QString& source)
{
    play(0);
    m_animation = std::move(animation);
    m_frame = 0;
    m_chooseAnimation->setText(m_animation ? QFileInfo(source).fileName() : tr("Animation…"));
    QString tip = m_animation ? tr("%1\n%n frame(s) at %2 per second", nullptr, static_cast<int>(m_animation->frameCount))
                                    .arg(source)
                                    .arg(QLocale().toString(m_animation->fps))
                              : tr("Choose one of the game's animations for this skeleton");
    if (m_animation)
        for (const std::string& note : m_animation->notes)
            tip += QStringLiteral("\n") + toQString(note);
    m_chooseAnimation->setToolTip(tip);
    updateAnimationBar();
    showFrame(0);
}

void ModelView::buildInfluences()
{
    if (!m_mesh || !m_skeleton) {
        m_canvas->setSkinning({});
        m_canvas->setBoneMatrices({});
        return;
    }
    const Mesh& mesh = *m_mesh;
    std::vector<ModelCanvas::Influence> influences(mesh.vertexCount());
    // A rigid part follows the bone of its node, or of the nearest node above it that has one.
    std::vector<int> nodeBone(mesh.nodes.size(), -1);
    for (std::size_t n = 0; n < mesh.nodes.size(); ++n) {
        int at = static_cast<int>(n);
        for (int guard = 0; at >= 0 && guard < 64 && nodeBone[n] < 0; ++guard) {
            nodeBone[n] = m_skeleton->find(mesh.nodes[static_cast<std::size_t>(at)].name);
            at = mesh.nodes[static_cast<std::size_t>(at)].parent;
        }
    }
    const bool skin = mesh.boneSlots.size() >= 4 * mesh.vertexCount() && mesh.boneWeights.size() == mesh.boneSlots.size();
    for (const Mesh::Part& part : mesh.parts) {
        std::vector<int> palette;
        for (const std::string& bone : part.bones)
            palette.push_back(m_skeleton->find(bone));
        const int rigid = part.node >= 0 && static_cast<std::size_t>(part.node) < nodeBone.size()
                              ? nodeBone[static_cast<std::size_t>(part.node)]
                              : -1;
        for (const Mesh::Range& range : part.lods)
            for (std::uint32_t i = range.firstIndex; i < range.firstIndex + range.indexCount && i < mesh.indices.size();
                 ++i) {
                const std::uint32_t v = mesh.indices[i];
                if (v >= influences.size())
                    continue;
                ModelCanvas::Influence& in = influences[v];
                if (!palette.empty() && skin) {
                    for (int k = 0; k < 4; ++k) {
                        const std::uint8_t slot = mesh.boneSlots[4 * v + k];
                        in.bone[k] = slot < palette.size() ? palette[slot] : -1;
                        in.weight[k] = mesh.boneWeights[4 * v + k];
                    }
                } else if (rigid >= 0) {
                    in = {{rigid, -1, -1, -1}, {1, 0, 0, 0}};
                }
            }
    }
    m_canvas->setSkinning(std::move(influences));
}

void ModelView::updateAnimationBar()
{
    m_animationBar->setVisible(m_skeleton != nullptr);
    const bool frames = m_animation && m_animation->frameCount > 1;
    for (QWidget* w : std::initializer_list<QWidget*>{m_stepBack, m_playBack, m_pause, m_playForward, m_stepForward,
                                                      m_speed, m_timeline})
        w->setEnabled(frames);
    const QSignalBlocker block(m_timeline);
    m_timeline->setRange(0, frames ? static_cast<int>(m_animation->frameCount) - 1 : 0);
}

void ModelView::showFrame(float frame)
{
    if (!m_skeleton)
        return;
    const float last = m_animation ? static_cast<float>(m_animation->frameCount) - 1 : 0;
    if (last > 0) {
        frame = std::fmod(frame, last + 1);
        if (frame < 0)
            frame += last + 1;
    } else {
        frame = 0;
    }
    m_frame = frame;
    const auto locals = anim::sample(*m_skeleton, m_animation.get(), frame);
    m_canvas->setBoneMatrices(anim::skinMatrices(*m_skeleton, anim::worldMatrices(*m_skeleton, locals)));
    {
        const QSignalBlocker block(m_timeline);
        m_timeline->setValue(static_cast<int>(frame));
    }
    m_frameLabel->setText(m_animation ? QStringLiteral("%1 / %2").arg(static_cast<int>(frame)).arg(static_cast<int>(last))
                                      : QString());
}

void ModelView::play(int direction)
{
    m_direction = m_animation && m_animation->frameCount > 1 ? direction : 0;
    m_playBack->setChecked(m_direction < 0);
    m_pause->setChecked(m_direction == 0);
    m_playForward->setChecked(m_direction > 0);
    if (m_direction != 0) {
        m_clock.start();
        m_timer->start();
    } else {
        m_timer->stop();
    }
}

void ModelView::step(int frames)
{
    play(0);
    showFrame(std::round(m_frame) + static_cast<float>(frames));
}

void ModelView::setMesh(std::shared_ptr<const Mesh> mesh)
{
    m_mesh = std::move(mesh);
    m_canvas->setMesh(m_mesh);
    setSkeleton(nullptr, {});
    {
        const QSignalBlocker block(m_lod);
        m_lod->clear();
        const int levels = m_mesh ? static_cast<int>(m_mesh->lodCount()) : 0;
        for (int i = 0; i < levels; ++i)
            m_lod->addItem(i == 0 && levels > 1   ? tr("LOD 0 (most detail)")
                           : i == levels - 1 && i ? tr("LOD %1 (least detail)").arg(i)
                                                  : tr("LOD %1").arg(i));
        m_lod->setEnabled(levels > 1);
    }
    m_collision->setVisible(m_mesh && std::ranges::any_of(m_mesh->parts, [this](const Mesh::Part& part) {
                                return part.collision && m_mesh->isHelper(part);
                            }));
    // Material looks and car paint only mean something for models drawn with the game's effects.
    const bool effects = m_mesh && std::ranges::any_of(m_mesh->parts, [](const Mesh::Part& part) {
                             return !part.shader.empty();
                         });
    m_materials->setVisible(effects);
    m_paint->setVisible(effects);
    buildTree();
    applyVisibility();
    updateCounts();
}

void ModelView::clear()
{
    setMesh(nullptr);
}

void ModelView::buildTree()
{
    m_updatingTree = true;
    m_parts->clear();
    m_partItems.assign(m_mesh ? m_mesh->parts.size() : 0, nullptr);
    if (!m_mesh) {
        m_updatingTree = false;
        return;
    }
    const auto folderItem = [](QTreeWidgetItem* item, Qt::CheckState state) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        item->setCheckState(0, state);
    };
    const bool collisionOn = m_collision->isChecked();
    const auto shownAtFirst = [&](const Mesh::Part& part) {
        return !part.startHidden && (!m_mesh->isHelper(part) || (part.collision && collisionOn));
    };
    const auto partItem = [&](QTreeWidgetItem* item, std::size_t part) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, shownAtFirst(m_mesh->parts[part]) ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, static_cast<int>(part));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(0, toQString(m_mesh->parts[part].material));
        m_partItems[part] = item;
    };

    const auto& nodes = m_mesh->nodes;
    std::vector<std::vector<std::size_t>> partsOf(nodes.size());
    std::vector<std::size_t> loose, shadows, collisions;
    for (std::size_t i = 0; i < m_mesh->parts.size(); ++i) {
        const Mesh::Part& part = m_mesh->parts[i];
        if (part.shadow)
            shadows.push_back(i);
        else if (part.collision && m_mesh->isHelper(part))
            collisions.push_back(i);
        else if (part.node >= 0 && static_cast<std::size_t>(part.node) < nodes.size())
            partsOf[static_cast<std::size_t>(part.node)].push_back(i);
        else
            loose.push_back(i);
    }

    if (!nodes.empty()) {
        // The model's own hierarchy, keeping only branches that draw something.
        std::vector<bool> used(nodes.size(), false), hasChildren(nodes.size(), false);
        for (std::size_t n = 0; n < nodes.size(); ++n)
            if (!partsOf[n].empty())
                for (int k = static_cast<int>(n), guard = 0; k >= 0 && guard < 256; ++guard) {
                    used[static_cast<std::size_t>(k)] = true;
                    k = nodes[static_cast<std::size_t>(k)].parent;
                }
        for (std::size_t n = 0; n < nodes.size(); ++n)
            if (used[n] && nodes[n].parent >= 0)
                hasChildren[static_cast<std::size_t>(nodes[n].parent)] = true;
        std::vector<QTreeWidgetItem*> items(nodes.size(), nullptr);
        std::function<QTreeWidgetItem*(std::size_t, int)> itemFor = [&](std::size_t n, int depth) -> QTreeWidgetItem* {
            if (items[n])
                return items[n];
            const int parent = nodes[n].parent;
            QTreeWidgetItem* up = parent >= 0 && depth < 256 ? itemFor(static_cast<std::size_t>(parent), depth + 1) : nullptr;
            auto* item = up ? new QTreeWidgetItem(up) : new QTreeWidgetItem(m_parts);
            item->setText(0, toQString(nodes[n].name));
            items[n] = item;
            return item;
        };
        for (std::size_t n = 0; n < nodes.size(); ++n) {
            if (!used[n])
                continue;
            QTreeWidgetItem* item = itemFor(n, 0);
            if (partsOf[n].size() == 1 && !hasChildren[n]) {
                partItem(item, partsOf[n].front()); // a single piece: the node is the part
                continue;
            }
            folderItem(item, Qt::Checked);
            for (std::size_t part : partsOf[n]) // several materials: one row each
                partItem(new QTreeWidgetItem(item, {toQString(m_mesh->parts[part].material)}), part);
        }
        if (!loose.empty()) {
            auto* other = new QTreeWidgetItem(m_parts, {tr("Other parts")});
            folderItem(other, Qt::Checked);
            for (std::size_t part : loose)
                partItem(new QTreeWidgetItem(other, {toQString(m_mesh->parts[part].name)}), part);
        }
    } else {
        // No hierarchy in the file: one folder per group, in the order the groups first appear.
        std::map<std::string, QTreeWidgetItem*> folders;
        for (std::size_t part : loose) {
            QTreeWidgetItem*& folder = folders[m_mesh->parts[part].group];
            if (!folder) {
                folder = new QTreeWidgetItem(m_parts, {toQString(m_mesh->parts[part].group)});
                folderItem(folder, Qt::Checked);
            }
            partItem(new QTreeWidgetItem(folder, {toQString(m_mesh->parts[part].name)}), part);
        }
    }
    if (!collisions.empty()) {
        auto* folder = new QTreeWidgetItem(m_parts, {tr("Collision")});
        folderItem(folder, collisionOn ? Qt::Checked : Qt::Unchecked);
        folder->setToolTip(0, tr("The shapes the game's physics uses for this model; the game never draws them"));
        for (std::size_t part : collisions) {
            // "e_door_left · Convex hull"; the tooltip adds the surface ("Convex hull · Plastic")
            const Mesh::Part& p = m_mesh->parts[part];
            const std::string kind = p.material.substr(0, p.material.find(" · "));
            partItem(new QTreeWidgetItem(folder, {QStringLiteral("%1 · %2").arg(toQString(p.name), toQString(kind))}), part);
        }
    }
    if (!shadows.empty()) {
        auto* folder = new QTreeWidgetItem(m_parts, {tr("Shadow hulls")});
        folderItem(folder, Qt::Unchecked);
        folder->setToolTip(0, tr("Simplified shapes the game uses to cast shadows"));
        for (std::size_t part : shadows)
            partItem(new QTreeWidgetItem(folder, {toQString(m_mesh->parts[part].name)}), part);
    }
    for (int i = 0; i < m_parts->topLevelItemCount(); ++i) // top level plus one more
        m_parts->topLevelItem(i)->setExpanded(true);
    if (m_parts->topLevelItemCount() == 1)
        for (int i = 0; i < m_parts->topLevelItem(0)->childCount(); ++i)
            m_parts->topLevelItem(0)->child(i)->setExpanded(true);
    m_updatingTree = false;
}

void ModelView::collectParts(QTreeWidgetItem* item, std::vector<int>& out) const
{
    if (const QVariant v = item->data(0, Qt::UserRole); v.isValid())
        out.push_back(v.toInt());
    for (int i = 0; i < item->childCount(); ++i)
        collectParts(item->child(i), out);
}

void ModelView::updateCounts()
{
    if (!m_mesh) {
        m_info->clear();
        return;
    }
    const QLocale locale;
    const auto lod = static_cast<std::size_t>(std::max(0, m_lod->currentIndex()));
    std::size_t shown = 0;
    m_updatingTree = true;
    for (std::size_t i = 0; i < m_partItems.size(); ++i) {
        if (!m_partItems[i])
            continue;
        const auto& lods = m_mesh->parts[i].lods;
        const std::uint32_t triangles = lod < lods.size() ? lods[lod].indexCount / 3 : 0;
        m_partItems[i]->setText(1, triangles ? locale.toString(triangles) : QStringLiteral("—"));
        m_partItems[i]->setToolTip(1, triangles ? QString() : tr("Not drawn at this level of detail"));
        if (m_partItems[i]->checkState(0) == Qt::Checked)
            shown += triangles;
    }
    // Folders: the total of the parts under them.
    std::function<std::size_t(QTreeWidgetItem*)> total = [&](QTreeWidgetItem* item) -> std::size_t {
        std::size_t sum = 0;
        if (const QVariant v = item->data(0, Qt::UserRole); v.isValid()) {
            const auto& lods = m_mesh->parts[static_cast<std::size_t>(v.toInt())].lods;
            sum += lod < lods.size() ? lods[lod].indexCount / 3 : 0;
        }
        for (int c = 0; c < item->childCount(); ++c)
            sum += total(item->child(c));
        if (item->childCount()) {
            item->setText(1, locale.toString(static_cast<qulonglong>(sum)));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
        return sum;
    };
    for (int f = 0; f < m_parts->topLevelItemCount(); ++f)
        total(m_parts->topLevelItem(f));
    m_updatingTree = false;
    m_info->setText(tr("%1 vertices  ·  %2 triangles shown  ·  %n part(s)", nullptr,
                       static_cast<int>(m_mesh->parts.size()))
                        .arg(locale.toString(static_cast<qulonglong>(m_mesh->vertexCount())),
                             locale.toString(static_cast<qulonglong>(shown))));
}

void ModelView::applyVisibility()
{
    std::vector<bool> visible(m_partItems.size());
    for (std::size_t i = 0; i < m_partItems.size(); ++i)
        visible[i] = m_partItems[i] && m_partItems[i]->checkState(0) == Qt::Checked;
    m_canvas->setVisibleParts(std::move(visible));
    updateCounts();
}

void ModelView::setAllParts(bool on, bool includeHelpers)
{
    m_updatingTree = true;
    for (std::size_t i = 0; i < m_partItems.size(); ++i)
        if (m_partItems[i])
            m_partItems[i]->setCheckState(0, on && (includeHelpers || !m_mesh->isHelper(m_mesh->parts[i]))
                                                 ? Qt::Checked
                                                 : Qt::Unchecked);
    m_updatingTree = false;
    applyVisibility();
}

void ModelView::setCollisionShown(bool on)
{
    if (!m_mesh)
        return;
    m_updatingTree = true;
    for (std::size_t i = 0; i < m_partItems.size(); ++i)
        if (m_partItems[i] && m_mesh->parts[i].collision && m_mesh->isHelper(m_mesh->parts[i]))
            m_partItems[i]->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
    m_updatingTree = false;
    applyVisibility();
}

void ModelView::updatePaintButton()
{
    const QColor color = QSettings().value("preview/model/paint", QColor(140, 18, 16)).value<QColor>();
    QPixmap swatch(16, 16);
    swatch.fill(color);
    m_paint->setIcon(QIcon(swatch));
}

void ModelView::showPartsMenu(const QPoint& pos)
{
    if (!m_mesh)
        return;
    QMenu menu(this);
    const QList<QTreeWidgetItem*> selected = m_parts->selectedItems();
    if (!selected.isEmpty()) {
        menu.addAction(tr("Show &Only Selected"), this, [this, selected] {
            std::vector<int> parts;
            for (QTreeWidgetItem* item : selected)
                collectParts(item, parts);
            const std::set<int> keep(parts.begin(), parts.end());
            m_updatingTree = true;
            for (std::size_t i = 0; i < m_partItems.size(); ++i)
                if (m_partItems[i])
                    m_partItems[i]->setCheckState(0, keep.contains(static_cast<int>(i)) ? Qt::Checked : Qt::Unchecked);
            m_updatingTree = false;
            applyVisibility();
        });
        menu.addSeparator();
    }
    menu.addAction(tr("Show &All Parts"), this, [this] { setAllParts(true, false); });
    menu.addAction(tr("Show All Including &Shadow Hulls and Collision"), this, [this] { setAllParts(true, true); });
    menu.addAction(tr("&Hide All"), this, [this] { setAllParts(false, true); });
    menu.addSeparator();
    menu.addAction(tr("&Expand All"), m_parts, &QTreeWidget::expandAll);
    menu.addAction(tr("&Collapse All"), m_parts, &QTreeWidget::collapseAll);
    menu.exec(m_parts->viewport()->mapToGlobal(pos));
}

} // namespace viewer::app
