#pragma once

#include "viewer/core/Animation.hpp"
#include "viewer/core/Mesh.hpp"

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QElapsedTimer>
#include <QVector3D>
#include <QWidget>

#include <memory>
#include <vector>

class QCheckBox;
class QColor;
class QComboBox;
class QToolButton;
class QLabel;
class QSlider;
class QSplitter;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace viewer::app {

/// OpenGL canvas for a Mesh: orbit (left drag), pan (right/middle drag),
/// zoom (wheel), double-click to reset. Lit from the camera, so every side
/// you look at is readable; colours follow the palette (dark mode).
///
/// Draws one detail level (LOD) of the parts that are switched on, with
/// optional wireframe, a ground grid and a soft shadow on the ground. A
/// model's collision shapes are drawn see-through over it, outlined.
class ModelCanvas : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit ModelCanvas(QWidget* parent = nullptr);
    ~ModelCanvas() override;

    void setMesh(std::shared_ptr<const Mesh> mesh);
    void setLod(int lod);
    /// One flag per Mesh part.
    void setVisibleParts(std::vector<bool> visible);
    /// Parts drawn in the accent colour (selected in the parts list).
    void setHighlightedParts(std::vector<int> parts);
    void setShowSolid(bool on);
    void setShowWireframe(bool on);
    void setShowGrid(bool on);
    void setShowShadow(bool on);
    /// Colours parts by the game's effect (paint, glass, lights, chrome…) instead of plain grey.
    void setShowMaterials(bool on);
    /// Body colour for car paint (the game lets the player choose it).
    void setPaintColor(const QColor& color);
    void resetView();

    /// Up to four bones moving a vertex (index into the bone matrices; -1: none).
    struct Influence {
        std::array<int, 4> bone{-1, -1, -1, -1};
        std::array<float, 4> weight{0, 0, 0, 0};
    };
    /// Per Mesh vertex (empty: nothing moves).
    void setSkinning(std::vector<Influence> influences);
    /// Skinning matrices (Skeleton bones) the vertices are posed with.
    void setBoneMatrices(std::vector<Matrix43> matrices);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct EdgeRange {
        int first = 0;
        int count = 0;
    };

    void upload();
    /// The model's textures on the GPU (one per Mesh texture; 0 when it has no pixels).
    void uploadTextures();
    void releaseTextures();
    void buildGrid(const QVector3D& lo, const QVector3D& hi);
    [[nodiscard]] QMatrix4x4 viewMatrix() const;
    [[nodiscard]] bool partVisible(std::size_t part) const;
    void bindVertices(QOpenGLBuffer& buffer);
    /// Moves the vertices to the current bone matrices (from the rest data).
    void applyPose();

    std::shared_ptr<const Mesh> m_mesh;
    bool m_dirty = false;
    int m_lod = 0;
    std::vector<bool> m_visible;
    std::vector<bool> m_highlighted;
    std::vector<bool> m_helper; ///< Per part: Mesh::isHelper (worked out once per mesh).
    std::vector<int> m_partTexture; ///< Per part: the texture it's coloured with (Mesh::textures), or -1.
    std::vector<GLuint> m_glTextures;
    std::vector<bool> m_textureAlpha; ///< Per texture: has see-through pixels.
    std::vector<float> m_rest;            ///< Uploaded vertex data at rest (posing starts from it).
    std::vector<Influence> m_influences;
    std::vector<Matrix43> m_boneMatrices;
    bool m_poseDirty = false;
    bool m_solid = true;
    bool m_wireframe = false;
    bool m_grid = true;
    bool m_shadow = true;
    bool m_materials = true;
    QVector3D m_paint{0.55f, 0.07f, 0.06f};

    QOpenGLShaderProgram m_program;
    QOpenGLBuffer m_vertices{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_triangles{QOpenGLBuffer::IndexBuffer};
    QOpenGLBuffer m_edges{QOpenGLBuffer::IndexBuffer};
    QOpenGLBuffer m_gridLines{QOpenGLBuffer::VertexBuffer};
    std::vector<std::vector<EdgeRange>> m_edgeRanges; ///< [part][lod] into m_edges
    std::vector<float> m_gridData; ///< Minor lines first, then major (x, y, z, 0, 1, 0 per point).
    int m_gridMinorPoints = 0;
    int m_gridMajorPoints = 0;
    bool m_hasGeometry = false;
    bool m_glReady = false;

    // Camera (orbit around m_target)
    QVector3D m_center;
    float m_radius = 1.0f;
    float m_ground = 0.0f; ///< Lowest point (y): where the grid and shadow lie.
    QVector3D m_target;
    float m_distance = 3.0f;
    float m_yaw = -35.0f;
    float m_pitch = 20.0f;
    bool m_mirror = false; ///< Left-handed (Direct3D) model: flip x so it isn't shown mirrored.
    QPoint m_lastMouse;
};

/// Model preview: a parts tree (tick parts on/off) beside the 3D canvas, and
/// display options (solid, wireframe, grid, shadow, level of detail) on top.
class ModelView : public QWidget {
    Q_OBJECT
public:
    explicit ModelView(QWidget* parent = nullptr);
    void setMesh(std::shared_ptr<const Mesh> mesh);
    void clear();
    /// The skeleton the model is posed with (null: none; the animation bar hides).
    void setSkeleton(std::shared_ptr<const Skeleton> skeleton, const QString& source);
    /// The animation to play (null: rest pose). Doesn't start playing.
    void setAnimation(std::shared_ptr<const Animation> animation, const QString& source);
    [[nodiscard]] const std::shared_ptr<const Skeleton>& skeleton() const noexcept { return m_skeleton; }
    /// Moves the animation to `frame` (paused).
    void showAnimationFrame(float frame) { showFrame(frame); }

signals:
    /// "Animation…": choose an animation for the skeleton.
    void animationRequested();

private:
    void buildTree();
    void updateCounts();
    /// Parts at or under `item` in the tree.
    void collectParts(QTreeWidgetItem* item, std::vector<int>& out) const;
    void applyVisibility();
    void showPartsMenu(const QPoint& pos);
    void setAllParts(bool on, bool includeHelpers);
    /// Ticks or unticks every collision shape of the model (the "Collision" box).
    void setCollisionShown(bool on);
    void updatePaintButton();
    void buildInfluences();
    void updateAnimationBar();
    /// Poses the model at `frame` and moves the timeline there.
    void showFrame(float frame);
    void play(int direction);
    void step(int frames);

    std::shared_ptr<const Mesh> m_mesh;
    ModelCanvas* m_canvas;
    QSplitter* m_splitter;
    QTreeWidget* m_parts;
    QLabel* m_info;
    QCheckBox* m_solid;
    QCheckBox* m_wireframe;
    QCheckBox* m_grid;
    QCheckBox* m_shadow;
    QCheckBox* m_collision;
    QCheckBox* m_materials;
    QToolButton* m_paint;
    QComboBox* m_lod;
    std::vector<QTreeWidgetItem*> m_partItems; ///< Per Mesh part.
    bool m_updatingTree = false;

    // Animation
    std::shared_ptr<const Skeleton> m_skeleton;
    std::shared_ptr<const Animation> m_animation;
    QWidget* m_animationBar;
    QLabel* m_skeletonLabel;
    QToolButton* m_chooseAnimation;
    QToolButton* m_stepBack;
    QToolButton* m_playBack;
    QToolButton* m_pause;
    QToolButton* m_playForward;
    QToolButton* m_stepForward;
    QComboBox* m_speed;
    QSlider* m_timeline;
    QLabel* m_frameLabel;
    QTimer* m_timer;
    QElapsedTimer m_clock;
    float m_frame = 0;
    int m_direction = 0; ///< 1 playing forwards, -1 backwards, 0 paused.
};

} // namespace viewer::app
