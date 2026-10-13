#include "AnimationSupport.hpp"

#include "../QtConvert.hpp"
#include "../VfsModel.hpp"

#include "viewer/core/FileTypes.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QTreeView>
#include <QMessageBox>
#include <QHash>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <set>
#include <unordered_set>

namespace viewer::app {

namespace {

/// Every file node of the session (main mount and extra ones) whose type is an animation file.
std::vector<const VfsNode*> animationCategoryFiles(const Session& session)
{
    std::vector<const VfsNode*> out;
    const auto scan = [&](const VfsNode& root) {
        for (const VfsNode* node : Vfs::collectFiles(root))
            if (const FileTypeInfo* type = session.content.fileTypes().find(extensionOf(node->name()));
                type && type->category == FileCategory::Animation)
                out.push_back(node);
    };
    scan(session.vfs.root());
    for (const auto& extra : session.extras)
        if (extra && extra->vfs)
            scan(extra->vfs->root());
    return out;
}

std::string stem(const std::string& name)
{
    const auto dot = name.rfind('.');
    return toLower(dot == std::string::npos ? name : name.substr(0, dot));
}

/// Whether `node`'s file decodes as an animation that fits `skeleton`.
std::shared_ptr<const Animation> loadAnimation(const Session& session, const VfsNode& node)
{
    auto data = session.vfs.read(node);
    if (!data)
        return nullptr;
    const std::string ext = extensionOf(node.name());
    const AnimationDecoder* decoder = session.content.findAnimationDecoder(ext, *data);
    if (!decoder)
        return nullptr;
    auto animation = decoder->decode(*data);
    return animation ? std::make_shared<const Animation>(std::move(*animation)) : nullptr;
}

/// Which animation files fit, per set of bone names (one bone name per line), for one session.
struct FitCache {
    std::weak_ptr<Session> session;
    QHash<QString, std::vector<int>> fits;
};

FitCache& fitCache()
{
    static FitCache cache;
    return cache;
}

} // namespace

std::vector<const VfsNode*> animationFiles(const Session& session)
{
    return animationCategoryFiles(session);
}

std::shared_ptr<const Animation> loadAnimationAt(const Session& session, const QString& virtualPath)
{
    const std::string wanted = toLower(virtualPath.toStdString());
    for (const VfsNode* node : animationCategoryFiles(session))
        if (toLower(node->virtualPath()) == wanted)
            return loadAnimation(session, *node);
    return nullptr;
}

SkeletonMatch findSkeleton(const Session& session, const VfsNode& model, const Mesh& mesh)
{
    SkeletonMatch match;
    std::set<std::string> skinned; // bones the model is skinned to (all must be there)
    for (const Mesh::Part& part : mesh.parts)
        for (const std::string& bone : part.bones)
            if (!bone.empty())
                skinned.insert(bone);
    std::vector<std::string> wanted(skinned.begin(), skinned.end());
    for (const Mesh::Node& node : mesh.nodes)
        wanted.push_back(node.name);
    const std::vector<std::string> skinnedNames(skinned.begin(), skinned.end());
    const std::string modelStem = stem(model.name());

    std::size_t bestScore = 0;
    const std::vector<const VfsNode*> files = animationCategoryFiles(session);
    for (const VfsNode* node : files) {
        auto data = session.vfs.read(*node);
        if (!data)
            continue;
        const SkeletonDecoder* decoder = session.content.findSkeletonDecoder(extensionOf(node->name()), *data);
        if (!decoder)
            continue;
        auto skeleton = decoder->decode(*data);
        if (!skeleton)
            continue;
        if (!skinnedNames.empty() && anim::matchCount(*skeleton, skinnedNames) < skinnedNames.size())
            continue;
        const std::size_t found = anim::matchCount(*skeleton, wanted);
        const bool sameName = stem(node->name()) == modelStem;
        // A rigid model needs a skeleton of its own name, or one sharing several part names.
        if (skinnedNames.empty() && !sameName && found < 2)
            continue;
        const std::size_t score = found + (sameName ? 1000 : 0);
        if (score > bestScore) {
            bestScore = score;
            match.skeleton = std::make_shared<const Skeleton>(std::move(*skeleton));
            match.source = toQString(node->virtualPath());
        }
    }
    if (!match.skeleton && skinned.empty() && mesh.nodes.size() > 1) {
        std::vector<std::string> names;
        std::vector<int> parents;
        std::vector<Matrix43> transforms;
        for (const Mesh::Node& node : mesh.nodes) {
            names.push_back(node.name);
            parents.push_back(node.parent);
            transforms.push_back(node.transform);
        }
        match.skeleton = std::make_shared<const Skeleton>(anim::fromHierarchy(names, parents, transforms));
        match.source = QObject::tr("the model's own parts");
    }
    if (!match.skeleton)
        return match;

    // An animation named like the model (fanblades.model → fanblades.cycle).
    for (const VfsNode* node : files)
        if (stem(node->name()) == modelStem && node != &model) {
            if (auto animation = loadAnimation(session, *node); animation && anim::fits(*match.skeleton, *animation)) {
                match.animation = std::move(animation);
                match.animationSource = toQString(node->virtualPath());
                break;
            }
        }
    return match;
}


// ---- AnimationPicker ----------------------------------------------------------------------

/// The main window's tree, cut down to animation files and the folders above them.
class AnimationFilterModel : public VfsFilterModel {
public:
    using VfsFilterModel::VfsFilterModel;

    /// The files to offer (fits per file: -1 not checked yet, 0 no, 1 yes; may be empty).
    void setFiles(const std::vector<const VfsNode*>& files, const std::vector<int>& fits, const QString& search,
                  bool showAll)
    {
        beginFilterChange();
        m_shown.clear();
        m_unfit.clear();
        for (std::size_t i = 0; i < files.size(); ++i) {
            const int fit = i < fits.size() ? fits[i] : -1;
            if (fit == 0)
                m_unfit.insert(files[i]);
            if ((fit == 0 && !showAll)
                || (!search.isEmpty() && !toQString(files[i]->name()).contains(search, Qt::CaseInsensitive)))
                continue;
            for (const VfsNode* n = files[i]; n && m_shown.insert(n).second;)
                n = n->parent();
        }
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        Qt::ItemFlags f = VfsFilterModel::flags(index) & ~Qt::ItemIsDragEnabled;
        if (m_unfit.contains(VfsModel::nodeFromIndex(mapToSource(index))))
            f &= ~Qt::ItemIsEnabled;
        return f;
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        return m_shown.contains(VfsModel::nodeFromIndex(sourceModel()->index(sourceRow, VfsModel::Name, sourceParent)));
    }

private:
    std::unordered_set<const VfsNode*> m_shown;
    std::unordered_set<const VfsNode*> m_unfit;
};

AnimationPicker::AnimationPicker(std::shared_ptr<Session> session, std::shared_ptr<const Skeleton> skeleton,
                                 AnimationPickerState* state, QWidget* parent)
    : QDialog(parent), m_session(std::move(session)), m_skeleton(std::move(skeleton)), m_state(state), m_model(new VfsModel(this)),
      m_filter(new AnimationFilterModel(this)), m_search(new QLineEdit(this)),
      m_showAll(new QCheckBox(tr("Show animations made for other skeletons"), this)), m_tree(new QTreeView(this)),
      m_status(new QLabel(this))
{
    setWindowTitle(tr("Choose Animation"));
    resize(520, 640);
    m_search->setPlaceholderText(tr("Search animations"));
    m_search->setClearButtonEnabled(true);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setWordWrap(true);

    std::vector<VfsModel::Root> roots{mainRoot(*m_session)};
    for (const auto& extra : m_session->extras)
        if (extra && extra->top)
            roots.push_back(extraRoot(*extra));
    m_model->setRoots(std::move(roots), &m_session->content);
    m_filter->setSourceModel(m_model);
    m_tree->setModel(m_filter);
    m_tree->setHeaderHidden(true);
    for (int column = VfsModel::Type; column < VfsModel::ColumnCount; ++column)
        m_tree->hideColumn(column);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(VfsModel::Name, Qt::AscendingOrder);
    m_tree->setUniformRowHeights(true);

    m_files = animationFiles(*m_session);
    m_filter->setFiles({}, {}, {}, false); // empty until the fit check is done

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Load"));
    connect(buttons, &QDialogButtonBox::accepted, this, &AnimationPicker::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_tree, &QTreeView::activated, this, [this] {
        if (const VfsNode* node = currentNode(); node && node->isFile())
            accept();
    });
    connect(m_search, &QLineEdit::textChanged, this, &AnimationPicker::refilter);
    connect(m_showAll, &QCheckBox::toggled, this, &AnimationPicker::refilter);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_showAll);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    restoreGeometry(QSettings().value("animationPicker/geometry").toByteArray());

    // Which animations fit only depends on the bone names: reuse an earlier check of these bones.
    QString bones;
    if (m_skeleton)
        for (const Skeleton::Bone& bone : m_skeleton->bones)
            bones += toQString(bone.name) + QLatin1Char('\n');
    if (FitCache& cache = fitCache(); cache.session.lock() == m_session && cache.fits.contains(bones)) {
        showFits(cache.fits.value(bones));
        return;
    }

    // Otherwise check in the background; the tree is filled when that's done.
    m_status->setText(tr("Checking which of %n animation(s) fit this model…", nullptr, static_cast<int>(m_files.size())));
    connect(&m_check, &QFutureWatcher<std::vector<int>>::finished, this, [this, bones] {
        FitCache& cache = fitCache();
        if (cache.session.lock() != m_session) {
            cache.session = m_session;
            cache.fits.clear();
        }
        cache.fits.insert(bones, m_check.result());
        showFits(m_check.result());
    });
    m_check.setFuture(QtConcurrent::run([session = m_session, skeleton = m_skeleton, files = m_files] {
        std::vector<int> fits;
        fits.reserve(files.size());
        for (const VfsNode* node : files) {
            const auto animation = loadAnimation(*session, *node);
            fits.push_back(animation && skeleton && anim::fits(*skeleton, *animation) ? 1 : 0);
        }
        return fits;
    }));
}

AnimationPicker::~AnimationPicker()
{
    m_check.waitForFinished();
}

const VfsNode* AnimationPicker::currentNode() const
{
    return VfsModel::nodeFromIndex(m_filter->mapToSource(m_tree->currentIndex()));
}

void AnimationPicker::showFits(std::vector<int> fits)
{
    m_fits = std::move(fits);
    m_ready = true;
    const auto fitting = std::ranges::count(m_fits, 1);
    m_status->setText(fitting ? tr("%n animation(s) fit this model.", nullptr, static_cast<int>(fitting))
                              : tr("No animation in this game fits this model's skeleton."));
    m_filter->setFiles(m_files, m_fits, m_search->text().trimmed(), m_showAll->isChecked());
    if (!m_search->text().trimmed().isEmpty()) {
        m_tree->expandAll();
        return;
    }
    if (!m_state) {
        restoreView({}, {});
        return;
    }
    QString current = m_state->current;
    if (m_state->expanded.isEmpty() && current.isEmpty() && !m_state->startFile.isEmpty()) {
        // First time for this model: start at its own animation.
        const std::string wanted = m_state->startFile.toStdString();
        const auto it = std::ranges::find_if(m_files, [&](const VfsNode* n) { return n->virtualPath() == wanted; });
        if (it != m_files.end())
            current = pathOf(m_filter->mapFromSource(m_model->indexForNode(*it)));
    }
    restoreView(m_state->expanded, current);
}

void AnimationPicker::refilter()
{
    if (!m_ready)
        return;
    const QString search = m_search->text().trimmed();
    const QString current = pathOf(m_tree->currentIndex());
    if (!search.isEmpty() && !m_searching)
        m_beforeSearch = expandedFolders();
    const QStringList expanded = m_searching ? m_beforeSearch : expandedFolders();
    m_searching = !search.isEmpty();
    m_tree->setUpdatesEnabled(false);
    m_filter->setFiles(m_files, m_fits, search, m_showAll->isChecked());
    if (search.isEmpty()) {
        m_tree->collapseAll();
        restoreView(expanded, current);
    } else {
        m_tree->expandAll();
    }
    m_tree->setUpdatesEnabled(true);
}

QString AnimationPicker::pathOf(const QModelIndex& index)
{
    QStringList names;
    for (QModelIndex i = index.siblingAtColumn(VfsModel::Name); i.isValid(); i = i.parent())
        names.prepend(i.data().toString());
    return names.join(QLatin1Char('/'));
}

QModelIndex AnimationPicker::indexAtPath(const QString& path) const
{
    QModelIndex at;
    for (const QString& name : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        QModelIndex next;
        for (int row = 0; row < m_filter->rowCount(at) && !next.isValid(); ++row)
            if (const QModelIndex child = m_filter->index(row, VfsModel::Name, at); child.data().toString() == name)
                next = child;
        if (!next.isValid())
            break;
        at = next;
    }
    return at;
}

QStringList AnimationPicker::expandedFolders() const
{
    QStringList out;
    std::vector<QModelIndex> todo{QModelIndex()};
    while (!todo.empty()) {
        const QModelIndex parent = todo.back();
        todo.pop_back();
        for (int row = 0; row < m_filter->rowCount(parent); ++row)
            if (const QModelIndex child = m_filter->index(row, VfsModel::Name, parent); m_tree->isExpanded(child)) {
                out << pathOf(child);
                todo.push_back(child);
            }
    }
    return out;
}

void AnimationPicker::restoreView(const QStringList& expanded, const QString& current)
{
    if (expanded.isEmpty() && current.isEmpty()) {
        m_tree->expandRecursively(QModelIndex(), 4); // first time: down to the animation folders
        return;
    }
    for (const QString& path : expanded)
        if (const QModelIndex index = indexAtPath(path); index.isValid() && pathOf(index) == path)
            m_tree->expand(index);
    // The file chosen last time, or (made for another skeleton) the folder it is in.
    const QModelIndex index = indexAtPath(current);
    if (!index.isValid())
        return;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent())
        m_tree->expand(parent);
    m_tree->setCurrentIndex(index);
    // Scroll once the dialog is laid out.
    QTimer::singleShot(0, m_tree, [tree = m_tree, index = QPersistentModelIndex(index)] {
        if (index.isValid())
            tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
    });
}

void AnimationPicker::done(int result)
{
    QSettings().setValue("animationPicker/geometry", saveGeometry());
    if (m_ready && m_state) {
        m_state->expanded = m_searching ? m_beforeSearch : expandedFolders();
        m_state->current = pathOf(m_tree->currentIndex());
    }
    QDialog::done(result);
}

void AnimationPicker::accept()
{
    const VfsNode* node = currentNode();
    if (!node || !node->isFile())
        return;
    auto animation = loadAnimation(*m_session, *node);
    if (!animation || !m_skeleton || !anim::fits(*m_skeleton, *animation)) {
        QMessageBox::information(this, windowTitle(),
                                 tr("%1 doesn't fit this model's skeleton.").arg(toQString(node->name())));
        return;
    }
    m_selected = node;
    m_animation = std::move(animation);
    QDialog::accept();
}

} // namespace viewer::app
