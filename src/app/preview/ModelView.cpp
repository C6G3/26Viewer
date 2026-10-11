#include "ModelView.hpp"

#include "ImageView.hpp"

#include "../Icons.hpp"
#include "../QtConvert.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QSettings>
#include <QSplitter>
#include <QToolButton>
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
uniform mat4 mvp;
uniform mat4 model;
uniform mat3 normalMatrix;
varying vec3 vNormal;
void main() {
    vNormal = normalMatrix * normal;
    gl_Position = mvp * model * vec4(position, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(
uniform vec3 color;
uniform float lit;
uniform float alpha;
uniform float specular; // highlight strength (glossy paint, chrome, glass)
uniform float rim;      // brightening towards the edges (reflections on paint)
varying vec3 vNormal;
void main() {
    // Headlight: light comes from the camera; both faces lit (models are often single-sided).
    float d = abs(normalize(vNormal).z);
    vec3 shaded = color * (0.28 + 0.72 * d) + vec3(specular * pow(d, 48.0)) + vec3(rim * 0.6 * pow(1.0 - d, 3.0));
    gl_FragColor = vec4(mix(color, shaded, lit), alpha);
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
};

Look lookFor(const std::string& shader, const QVector3D& paint)
{
    std::string s = shader;
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&](const char* what) { return s.find(what) != std::string::npos; };
    if (has("_paint"))
        return {paint, 0.55f, 0.6f, 1.0f, 1.0f, true};
    if (has("_glass"))
        return {QVector3D(0.10f, 0.13f, 0.16f), 0.9f, 0.4f, 0.45f, 1.0f, true};
    if (has("_light"))
        return {QVector3D(1.0f, 0.96f, 0.88f), 0.3f, 0.0f, 1.0f, 0.35f, true};
    if (has("_neon"))
        return {QVector3D(0.85f, 0.9f, 1.0f), 0.2f, 0.0f, 1.0f, 0.3f, true};
    if (has("_alloy") || has("_chrome"))
        return {QVector3D(0.78f, 0.79f, 0.81f), 0.8f, 0.5f, 1.0f, 1.0f, true};
    if (has("_matt"))
        return {QVector3D(0.16f, 0.16f, 0.17f), 0.05f, 0.0f, 1.0f, 1.0f, true};
    if (has("veh_default"))
        return {QVector3D(0.30f, 0.31f, 0.33f), 0.15f, 0.0f, 1.0f, 1.0f, true};
    return {};
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

constexpr int kStride = 6 * sizeof(float);

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
    // Bounds of the real geometry (not shadow hulls) → camera framing, grid and ground.
    if (m_mesh && m_mesh->vertexCount()) {
        QVector3D lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        const auto include = [&](std::uint32_t v) {
            const QVector3D p(m_mesh->positions[3 * v], m_mesh->positions[3 * v + 1], m_mesh->positions[3 * v + 2]);
            lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
            hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
        };
        for (const Mesh::Part& part : m_mesh->parts)
            if (!part.shadow)
                for (const Mesh::Range& r : part.lods)
                    for (std::uint32_t i = r.firstIndex; i < r.firstIndex + r.indexCount; ++i)
                        include(m_mesh->indices[i]);
        if (lo.x() > hi.x()) // only shadow hulls: use everything
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
        out.insert(out.end(), {ax, y, az, 0, 1, 0, bx, y, bz, 0, 1, 0});
    };
    for (int i = x0; i <= x1; ++i)
        line(i % 5 ? minor : major, i * spacing, z0 * spacing, i * spacing, z1 * spacing, m_ground);
    for (int k = z0; k <= z1; ++k)
        line(k % 5 ? minor : major, x0 * spacing, k * spacing, x1 * spacing, k * spacing, m_ground);
    m_gridMinorPoints = static_cast<int>(minor.size() / 6);
    m_gridMajorPoints = static_cast<int>(major.size() / 6);
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
    std::vector<float> interleaved;
    interleaved.reserve(mesh.vertexCount() * 6);
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
        QVector3D n = (detailed[v].isNull() ? coarse[v] : detailed[v]).normalized();
        if (fileNormals) {
            const QVector3D stored(mesh.normals[3 * v], mesh.normals[3 * v + 1], mesh.normals[3 * v + 2]);
            if (stored.lengthSquared() > 0.25f)
                n = stored.normalized();
        }
        interleaved.insert(interleaved.end(), {mesh.positions[3 * v], mesh.positions[3 * v + 1],
                                               mesh.positions[3 * v + 2], n.x(), n.y(), n.z()});
    }
    m_vertices.bind();
    m_vertices.allocate(interleaved.data(), static_cast<int>(interleaved.size() * sizeof(float)));
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
    const Mesh& mesh = *m_mesh;
    const bool dark = background.lightnessF() < 0.5;

    glEnable(GL_DEPTH_TEST);
    // Near/far planes from the model size keep depth precision good for tiny and huge models.
    QMatrix4x4 projection;
    projection.perspective(45.0f, static_cast<float>(width()) / static_cast<float>(std::max(1, height())),
                           std::max(m_distance - m_radius * 4.0f, m_radius * 0.01f), m_distance + m_radius * 4.0f);
    const QMatrix4x4 view = viewMatrix();
    m_program.bind();
    m_program.setUniformValue("mvp", projection * view);
    m_program.setUniformValue("model", QMatrix4x4());
    m_program.setUniformValue("normalMatrix", view.normalMatrix());
    m_program.setUniformValue("alpha", 1.0f);
    m_program.setUniformValue("specular", 0.0f);
    m_program.setUniformValue("rim", 0.0f);
    m_program.enableAttributeArray(0);
    m_program.enableAttributeArray(1);

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
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        for (std::size_t p = 0; p < mesh.parts.size(); ++p)
            if (partVisible(p) && !mesh.parts[p].shadow)
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
    if (m_solid) {
        glEnable(GL_POLYGON_OFFSET_FILL); // keeps the wireframe overlay on top of the faces
        glPolygonOffset(1.0f, 1.0f);
        // Opaque parts first, then see-through ones (glass) blended over them.
        std::vector<std::pair<std::size_t, Look>> clear;
        const auto drawPart = [&](std::size_t p, const Look& look) {
            const QVector3D c = look.color;
            m_program.setUniformValue("lit", look.lit);
            m_program.setUniformValue("specular", look.specular);
            m_program.setUniformValue("rim", look.rim);
            m_program.setUniformValue("alpha", look.alpha);
            m_program.setUniformValue("color", m_highlighted[p] ? c * 0.35f + accent * 0.65f : c);
            drawTriangles(*lodRange(p));
        };
        for (std::size_t p = 0; p < mesh.parts.size(); ++p) {
            if (!partVisible(p) || !lodRange(p))
                continue;
            Look look = m_materials && !mesh.parts[p].shadow ? lookFor(mesh.parts[p].shader, m_paint) : Look{};
            if (!look.known)
                look.color = mesh.parts[p].shadow ? hull : base;
            if (look.alpha < 1.0f && !m_highlighted[p]) {
                clear.emplace_back(p, look);
                continue;
            }
            look.alpha = 1.0f;
            drawPart(p, look);
        }
        if (!clear.empty()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            for (const auto& [p, look] : clear)
                drawPart(p, look);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
        m_program.setUniformValue("specular", 0.0f);
        m_program.setUniformValue("rim", 0.0f);
        m_program.setUniformValue("alpha", 1.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (m_wireframe || !m_solid) {
        // Over faces: black, readable on every part; without faces: the text colour (black would vanish in dark mode).
        const QVector3D line = m_solid ? QVector3D(0, 0, 0) : rgb(palette().color(QPalette::Text));
        m_program.setUniformValue("lit", 0.0f);
        m_edges.bind();
        for (std::size_t p = 0; p < mesh.parts.size() && p < m_edgeRanges.size(); ++p) {
            if (!partVisible(p) || !lodRange(p))
                continue;
            const EdgeRange& e = m_edgeRanges[p][static_cast<std::size_t>(m_lod)];
            m_program.setUniformValue("color", m_highlighted[p] ? accent : line);
            glDrawElements(GL_LINES, e.count, GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(static_cast<std::uintptr_t>(e.first) * sizeof(std::uint32_t)));
        }
    }
    m_program.disableAttributeArray(0);
    m_program.disableAttributeArray(1);
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
    m_distance = std::clamp(m_distance * std::pow(0.85f, steps), m_radius * 0.05f, m_radius * 50.0f);
    update();
}

// ---- ModelView ---------------------------------------------------------------------------

ModelView::ModelView(QWidget* parent)
    : QWidget(parent), m_canvas(new ModelCanvas(this)), m_splitter(new QSplitter(this)),
      m_parts(new QTreeWidget(this)), m_info(new QLabel(this)), m_solid(new QCheckBox(tr("Solid"), this)),
      m_wireframe(new QCheckBox(tr("Wireframe"), this)), m_grid(new QCheckBox(tr("Grid"), this)),
      m_shadow(new QCheckBox(tr("Shadow"), this)), m_materials(new QCheckBox(tr("Materials"), this)),
      m_paint(new QToolButton(this)), m_lod(new QComboBox(this))
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
        if (!m_updatingTree)
            applyVisibility();
    });
    connect(m_parts, &QTreeWidget::itemSelectionChanged, this, [this] {
        std::vector<int> selected;
        for (QTreeWidgetItem* item : m_parts->selectedItems())
            collectParts(item, selected);
        m_canvas->setHighlightedParts(std::move(selected));
    });
    connect(m_parts, &QTreeWidget::customContextMenuRequested, this, &ModelView::showPartsMenu);
    connect(m_parts, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (const QVariant t = item->data(0, Qt::UserRole + 1); t.isValid())
            showTexture(t.toInt());
    });

    m_splitter->addWidget(m_parts);
    m_splitter->addWidget(m_canvas);
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
    bar->addWidget(m_solid);
    bar->addWidget(m_wireframe);
    bar->addWidget(m_grid);
    bar->addWidget(m_shadow);
    bar->addWidget(m_materials);
    bar->addWidget(m_paint);
    bar->addSpacing(8);
    bar->addWidget(m_lod);
    bar->addWidget(reset);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_splitter, 1);
}

void ModelView::setMesh(std::shared_ptr<const Mesh> mesh)
{
    m_mesh = std::move(mesh);
    m_canvas->setMesh(m_mesh);
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
    const auto partItem = [&](QTreeWidgetItem* item, std::size_t part) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_mesh->parts[part].shadow ? Qt::Unchecked : Qt::Checked);
        item->setData(0, Qt::UserRole, static_cast<int>(part));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(0, toQString(m_mesh->parts[part].material));
        m_partItems[part] = item;
    };

    const auto& nodes = m_mesh->nodes;
    std::vector<std::vector<std::size_t>> partsOf(nodes.size());
    std::vector<std::size_t> loose, shadows;
    for (std::size_t i = 0; i < m_mesh->parts.size(); ++i) {
        const Mesh::Part& part = m_mesh->parts[i];
        if (part.shadow)
            shadows.push_back(i);
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
    if (!shadows.empty()) {
        auto* folder = new QTreeWidgetItem(m_parts, {tr("Shadow hulls")});
        folderItem(folder, Qt::Unchecked);
        folder->setToolTip(0, tr("Simplified shapes the game uses to cast shadows"));
        for (std::size_t part : shadows)
            partItem(new QTreeWidgetItem(folder, {toQString(m_mesh->parts[part].name)}), part);
    }
    if (!m_mesh->textures.empty()) {
        // Textures stored in the model: double-click to look at one.
        auto* folder = new QTreeWidgetItem(m_parts, {tr("Textures")});
        folder->setData(0, Qt::UserRole + 2, true); // not parts: no counts, starts collapsed
        folder->setToolTip(0, tr("Textures stored in the model file. Double-click one to see it."));
        for (std::size_t t = 0; t < m_mesh->textures.size(); ++t) {
            const Mesh::Texture& texture = m_mesh->textures[t];
            auto* item = new QTreeWidgetItem(folder, {toQString(texture.name),
                                                      QStringLiteral("%1×%2").arg(texture.image.width).arg(texture.image.height)});
            item->setData(0, Qt::UserRole + 1, static_cast<int>(t));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            item->setToolTip(0, tr("%1 · double-click to view").arg(toQString(texture.image.format)));
        }
        folder->setExpanded(false);
    }
    for (int i = 0; i < m_parts->topLevelItemCount(); ++i) // top level plus one more
        if (!m_parts->topLevelItem(i)->data(0, Qt::UserRole + 2).toBool())
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
        if (!m_parts->topLevelItem(f)->data(0, Qt::UserRole + 2).toBool())
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

void ModelView::setAllParts(bool on, bool includeShadows)
{
    m_updatingTree = true;
    for (std::size_t i = 0; i < m_partItems.size(); ++i)
        if (m_partItems[i])
            m_partItems[i]->setCheckState(0, on && (includeShadows || !m_mesh->parts[i].shadow) ? Qt::Checked
                                                                                                : Qt::Unchecked);
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

void ModelView::showTexture(int index)
{
    if (!m_mesh || index < 0 || static_cast<std::size_t>(index) >= m_mesh->textures.size())
        return;
    const Mesh::Texture& texture = m_mesh->textures[static_cast<std::size_t>(index)];
    const QImage image(texture.image.rgba.data(), static_cast<int>(texture.image.width),
                       static_cast<int>(texture.image.height), static_cast<qsizetype>(texture.image.width) * 4,
                       QImage::Format_RGBA8888);
    auto* window = new QDialog(this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(toQString(texture.name));
    auto* view = new ImageView(window);
    view->setImage(image.copy(), QStringLiteral("%1 × %2  ·  %3")
                                     .arg(texture.image.width)
                                     .arg(texture.image.height)
                                     .arg(toQString(texture.image.format)));
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);
    window->resize(640, 680);
    window->show();
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
    menu.addAction(tr("Show All Including &Shadow Hulls"), this, [this] { setAllParts(true, true); });
    menu.addAction(tr("&Hide All"), this, [this] { setAllParts(false, true); });
    menu.addSeparator();
    menu.addAction(tr("&Expand All"), m_parts, &QTreeWidget::expandAll);
    menu.addAction(tr("&Collapse All"), m_parts, &QTreeWidget::collapseAll);
    menu.exec(m_parts->viewport()->mapToGlobal(pos));
}

} // namespace viewer::app
