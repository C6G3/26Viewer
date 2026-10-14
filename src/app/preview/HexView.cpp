#include "HexView.hpp"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>
#include <bit>
#include <cstring>

namespace viewer::app {

namespace {

constexpr int kBytesPerLine = 16;
constexpr int kMargin = 8;

QFont monospaceFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), font.family()});
    font.setStyleHint(QFont::Monospace);
    return font;
}

QColor blend(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

} // namespace

HexView::HexView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFont(monospaceFont());
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Base);
    viewport()->setCursor(Qt::IBeamCursor);
    updateMetrics();
}

void HexView::setData(QByteArray data)
{
    m_data = std::move(data);
    m_highlightStart = -1;
    m_cursor = 0;
    m_anchor = -1;
    m_offsetDigits = m_data.size() > 0xFFFFFFFFll ? 10 : 8;
    verticalScrollBar()->setValue(0);
    updateScrollBar();
    viewport()->update();
    emit cursorChanged();
}

void HexView::clear()
{
    setData({});
}

void HexView::highlight(qint64 offset, qint64 length)
{
    m_highlightStart = offset;
    m_highlightLength = length;
    ensureVisible(offset);
    viewport()->update();
}

void HexView::select(qint64 offset, qint64 length)
{
    if (m_data.isEmpty())
        return;
    offset = std::clamp<qint64>(offset, 0, m_data.size() - 1);
    m_anchor = length > 0 ? offset : -1;
    m_cursor = length > 0 ? std::min<qint64>(offset + length, m_data.size()) - 1 : offset;
    ensureVisible(offset);
    viewport()->update();
    emit cursorChanged();
}

qint64 HexView::selectionStart() const noexcept
{
    return m_anchor < 0 ? m_cursor : std::min(m_anchor, m_cursor);
}

qint64 HexView::selectionLength() const noexcept
{
    return m_anchor < 0 ? 0 : std::max(m_anchor, m_cursor) - std::min(m_anchor, m_cursor) + 1;
}

void HexView::copySelection(CopyFormat format) const
{
    const qint64 length = selectionLength();
    if (length == 0)
        return;
    const QByteArray bytes = m_data.mid(selectionStart(), length);
    QString out;
    switch (format) {
    case CopyFormat::Hex:
        out = QString::fromLatin1(bytes.toHex(' ').toUpper());
        break;
    case CopyFormat::CArray: {
        QStringList items;
        for (const char c : bytes)
            items << QStringLiteral("0x%1").arg(
                QString::number(static_cast<unsigned char>(c), 16).toUpper(), 2, QLatin1Char('0'));
        QStringList lines;
        for (qsizetype i = 0; i < items.size(); i += 12)
            lines << QStringLiteral("    ") + items.mid(i, 12).join(QStringLiteral(", "));
        out = QStringLiteral("const unsigned char data[%1] = {\n%2\n};").arg(bytes.size()).arg(lines.join(",\n"));
        break;
    }
    case CopyFormat::Text:
        for (const char c : bytes) {
            const auto b = static_cast<unsigned char>(c);
            out += b >= 0x20 && b < 0x7F ? QChar(QLatin1Char(c)) : QChar(QLatin1Char('.'));
        }
        break;
    }
    QGuiApplication::clipboard()->setText(out);
}

bool HexView::findNext(const QByteArray& needle)
{
    if (needle.isEmpty() || m_data.isEmpty())
        return false;
    const qint64 from = selectionLength() > 0 ? selectionStart() + 1 : m_cursor + 1;
    qint64 at = m_data.indexOf(needle, from);
    if (at < 0)
        at = m_data.indexOf(needle, 0); // wrap around
    if (at < 0)
        return false;
    select(at, needle.size());
    return true;
}

qint64 HexView::lineCount() const
{
    return (m_data.size() + kBytesPerLine - 1) / kBytesPerLine;
}

int HexView::visibleLines() const
{
    return std::max(1, viewport()->height() / m_lineHeight);
}

void HexView::ensureVisible(qint64 offset)
{
    const qint64 line = offset / kBytesPerLine;
    auto* bar = verticalScrollBar();
    if (line < bar->value())
        bar->setValue(static_cast<int>(line));
    else if (line >= bar->value() + visibleLines())
        bar->setValue(static_cast<int>(std::max<qint64>(0, line - visibleLines() + 1)));
}

void HexView::updateMetrics()
{
    const QFontMetrics fm(font());
    m_lineHeight = fm.height() + 2;
    m_charWidth = fm.horizontalAdvance(QLatin1Char('0'));
    updateScrollBar();
}

void HexView::updateScrollBar()
{
    // Scroll range is in lines; QScrollBar's int range covers ~34 GB at 16 B/line.
    const qint64 maxLine = std::max<qint64>(0, lineCount() - visibleLines());
    verticalScrollBar()->setRange(0, static_cast<int>(std::min<qint64>(maxLine, INT_MAX)));
    verticalScrollBar()->setPageStep(visibleLines());
    const int width = kMargin * 2 + (m_offsetDigits + 2 + kBytesPerLine * 3 + 2 + kBytesPerLine) * m_charWidth;
    horizontalScrollBar()->setRange(0, std::max(0, width - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
}

void HexView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBar();
}

void HexView::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange)
        updateMetrics();
    QAbstractScrollArea::changeEvent(event);
}

std::optional<qint64> HexView::byteAt(const QPoint& pos) const
{
    const int x0 = kMargin - horizontalScrollBar()->value();
    const int hexX = x0 + (m_offsetDigits + 2) * m_charWidth;
    const int asciiX = hexX + (kBytesPerLine * 3 + 2) * m_charWidth;
    const qint64 line = verticalScrollBar()->value() + std::max(0, pos.y()) / m_lineHeight;
    int column = -1;
    if (pos.x() >= asciiX - m_charWidth / 2) {
        column = (pos.x() - asciiX) / m_charWidth;
    } else if (pos.x() >= hexX - m_charWidth) {
        // Byte i sits at (i * 3 + (i >= 8)) characters, two wide plus a space.
        int chars = (pos.x() - hexX + m_charWidth / 2) / m_charWidth;
        if (chars >= 8 * 3 + 1)
            --chars;
        column = chars / 3;
    }
    if (column < 0)
        return std::nullopt;
    column = std::clamp(column, 0, kBytesPerLine - 1);
    const qint64 offset = line * kBytesPerLine + column;
    if (m_data.isEmpty())
        return std::nullopt;
    return std::min<qint64>(offset, m_data.size() - 1);
}

void HexView::moveCursor(qint64 offset, bool extend)
{
    if (m_data.isEmpty())
        return;
    offset = std::clamp<qint64>(offset, 0, m_data.size() - 1);
    if (extend) {
        if (m_anchor < 0)
            m_anchor = m_cursor;
    } else {
        m_anchor = -1;
    }
    m_cursor = offset;
    ensureVisible(offset);
    viewport()->update();
    emit cursorChanged();
}

void HexView::mousePressEvent(QMouseEvent* event)
{
    // Clicking clears a structure highlight.
    m_highlightStart = -1;
    if (event->button() == Qt::LeftButton)
        if (const auto at = byteAt(event->position().toPoint()))
            moveCursor(*at, event->modifiers() & Qt::ShiftModifier);
    viewport()->update();
    QAbstractScrollArea::mousePressEvent(event);
}

void HexView::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton)
        if (const auto at = byteAt(event->position().toPoint()))
            moveCursor(*at, true);
    QAbstractScrollArea::mouseMoveEvent(event);
}

void HexView::keyPressEvent(QKeyEvent* event)
{
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const qint64 page = static_cast<qint64>(visibleLines()) * kBytesPerLine;
    if (event->matches(QKeySequence::Copy)) {
        copySelection(CopyFormat::Hex);
    } else if (event->matches(QKeySequence::SelectAll)) {
        if (!m_data.isEmpty())
            select(0, m_data.size());
    } else if (event->matches(QKeySequence::Find)) {
        emit findRequested();
    } else if (event->matches(QKeySequence::FindNext)) {
        emit findNextRequested();
    } else if (ctrl && event->key() == Qt::Key_G) {
        emit goToRequested();
    } else {
        switch (event->key()) {
        case Qt::Key_Left: moveCursor(m_cursor - 1, shift); break;
        case Qt::Key_Right: moveCursor(m_cursor + 1, shift); break;
        case Qt::Key_Up: moveCursor(m_cursor - kBytesPerLine, shift); break;
        case Qt::Key_Down: moveCursor(m_cursor + kBytesPerLine, shift); break;
        case Qt::Key_PageUp: moveCursor(m_cursor - page, shift); break;
        case Qt::Key_PageDown: moveCursor(m_cursor + page, shift); break;
        case Qt::Key_Home: moveCursor(ctrl ? 0 : m_cursor - m_cursor % kBytesPerLine, shift); break;
        case Qt::Key_End:
            moveCursor(ctrl ? m_data.size() - 1 : m_cursor - m_cursor % kBytesPerLine + kBytesPerLine - 1, shift);
            break;
        default: QAbstractScrollArea::keyPressEvent(event); return;
        }
    }
    event->accept();
}

void HexView::contextMenuEvent(QContextMenuEvent* event)
{
    if (const auto at = byteAt(viewport()->mapFromGlobal(event->globalPos()));
        at && (selectionLength() == 0 || *at < selectionStart() || *at >= selectionStart() + selectionLength()))
        moveCursor(*at, false);
    QMenu menu(this);
    const bool any = selectionLength() > 0;
    menu.addAction(tr("Copy as &Hex"), QKeySequence::Copy, this, [this] { copySelection(CopyFormat::Hex); })
        ->setEnabled(any);
    menu.addAction(tr("Copy as &C Array"), this, [this] { copySelection(CopyFormat::CArray); })->setEnabled(any);
    menu.addAction(tr("Copy as &Text"), this, [this] { copySelection(CopyFormat::Text); })->setEnabled(any);
    menu.addSeparator();
    menu.addAction(tr("Select &All"), QKeySequence::SelectAll, this, [this] {
        if (!m_data.isEmpty())
            select(0, m_data.size());
    });
    menu.addAction(tr("&Go to Offset…"), QKeySequence(Qt::CTRL | Qt::Key_G), this, [this] { emit goToRequested(); });
    menu.addAction(tr("&Find…"), QKeySequence::Find, this, [this] { emit findRequested(); });
    menu.exec(event->globalPos());
}

void HexView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::Text);
    const QColor dim = blend(text, pal.color(QPalette::Base), 0.55);
    const QColor offsetColor = pal.color(QPalette::Link);
    const QColor highlightBg = blend(pal.color(QPalette::Highlight), pal.color(QPalette::Base), 0.55);
    const QColor selectionBg = pal.color(QPalette::Highlight);
    const QColor selectionText = pal.color(QPalette::HighlightedText);

    p.setFont(font());
    const int ascent = QFontMetrics(font()).ascent() + 1;
    const int x0 = kMargin - horizontalScrollBar()->value();
    const int hexX = x0 + (m_offsetDigits + 2) * m_charWidth;
    const int asciiX = hexX + (kBytesPerLine * 3 + 2) * m_charWidth;

    if (m_data.isEmpty()) {
        p.setPen(dim);
        p.drawText(viewport()->rect(), Qt::AlignCenter, tr("No data"));
        return;
    }

    const qint64 first = verticalScrollBar()->value();
    const qint64 last = std::min(lineCount(), first + visibleLines() + 1);
    const qint64 selStart = selectionStart(), selEnd = selStart + selectionLength();
    const auto* bytes = reinterpret_cast<const unsigned char*>(m_data.constData());
    static constexpr char kHex[] = "0123456789ABCDEF";

    for (qint64 line = first; line < last; ++line) {
        const int y = static_cast<int>(line - first) * m_lineHeight;
        const qint64 base = line * kBytesPerLine;

        p.setPen(offsetColor);
        p.drawText(x0, y + ascent, QStringLiteral("%1").arg(base, m_offsetDigits, 16, QLatin1Char('0')).toUpper());

        for (int i = 0; i < kBytesPerLine && base + i < m_data.size(); ++i) {
            const qint64 pos = base + i;
            const unsigned char b = bytes[pos];
            const int hx = hexX + (i * 3 + (i >= 8 ? 1 : 0)) * m_charWidth;
            const int ax = asciiX + i * m_charWidth;
            const bool selected = pos >= selStart && pos < selEnd;

            if (selected) {
                p.fillRect(hx - m_charWidth / 2, y, m_charWidth * 3, m_lineHeight, selectionBg);
                p.fillRect(ax, y, m_charWidth, m_lineHeight, selectionBg);
            } else if (m_highlightStart >= 0 && pos >= m_highlightStart
                       && pos < m_highlightStart + m_highlightLength) {
                p.fillRect(hx - m_charWidth / 2, y, m_charWidth * 3, m_lineHeight, highlightBg);
                p.fillRect(ax, y, m_charWidth, m_lineHeight, highlightBg);
            }
            if (pos == m_cursor && hasFocus()) { // the cursor: a frame in both columns
                p.setPen(selected ? selectionText : text);
                p.drawRect(hx - m_charWidth / 2, y, m_charWidth * 3 - 1, m_lineHeight - 1);
                p.drawRect(ax, y, m_charWidth - 1, m_lineHeight - 1);
            }

            p.setPen(selected ? selectionText : b == 0 ? dim : text);
            const QChar hexPair[2] = {QLatin1Char(kHex[b >> 4]), QLatin1Char(kHex[b & 0xF])};
            p.drawText(hx, y + ascent, QString(hexPair, 2));
            p.setPen(selected ? selectionText : b >= 0x20 && b < 0x7F ? text : dim);
            p.drawText(ax, y + ascent, QString(QLatin1Char(b >= 0x20 && b < 0x7F ? char(b) : '.')));
        }
    }
}

// ---- HexPanel ----------------------------------------------------------------------------------

HexPanel::HexPanel(QWidget* parent)
    : QWidget(parent), m_view(new HexView(this)), m_goTo(new QLineEdit(this)), m_find(new QLineEdit(this)),
      m_status(new QLabel(this))
{
    m_goTo->setPlaceholderText(tr("Go to offset (Ctrl+G)"));
    m_goTo->setToolTip(tr("Decimal (1024) or hex (0x400, 400h)"));
    m_goTo->setClearButtonEnabled(true);
    m_goTo->setMaximumWidth(200);
    m_find->setPlaceholderText(tr("Find bytes or \"text\" (Ctrl+F, F3 next)"));
    m_find->setToolTip(tr("Hex bytes (4B 41 50 32) or text in quotes (\"KAP2\")"));
    m_find->setClearButtonEnabled(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setFont(m_view->font());
    m_status->setContentsMargins(4, 0, 4, 2);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->addWidget(m_goTo);
    bar->addWidget(m_find, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addLayout(bar);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_status);

    connect(m_goTo, &QLineEdit::returnPressed, this, &HexPanel::goTo);
    connect(m_find, &QLineEdit::returnPressed, this, &HexPanel::find);
    connect(m_view, &HexView::cursorChanged, this, &HexPanel::updateStatus);
    connect(m_view, &HexView::goToRequested, this, [this] {
        m_goTo->setFocus();
        m_goTo->selectAll();
    });
    connect(m_view, &HexView::findRequested, this, [this] {
        m_find->setFocus();
        m_find->selectAll();
    });
    connect(m_view, &HexView::findNextRequested, this, &HexPanel::find);
    auto* findNext = new QShortcut(QKeySequence::FindNext, this, this, &HexPanel::find);
    findNext->setContext(Qt::WidgetWithChildrenShortcut);
    updateStatus();
}

QByteArray HexPanel::parseNeedle(const QString& text)
{
    const QString t = text.trimmed();
    if (t.size() >= 2 && t.startsWith(QLatin1Char('"')) && t.endsWith(QLatin1Char('"')))
        return t.mid(1, t.size() - 2).toUtf8();
    QString hex = t;
    hex.remove(QRegularExpression(QStringLiteral("[\\s,]|0x")));
    static const QRegularExpression isHex(QStringLiteral("^([0-9A-Fa-f]{2})+$"));
    if (isHex.match(hex).hasMatch())
        return QByteArray::fromHex(hex.toLatin1());
    return t.toUtf8();
}

std::optional<qint64> HexPanel::parseOffset(const QString& text)
{
    QString t = text.trimmed().toLower();
    int base = 10;
    if (t.startsWith(QLatin1String("0x"))) {
        t = t.mid(2);
        base = 16;
    } else if (t.startsWith(QLatin1Char('$'))) {
        t = t.mid(1);
        base = 16;
    } else if (t.endsWith(QLatin1Char('h'))) {
        t.chop(1);
        base = 16;
    } else if (t.contains(QRegularExpression(QStringLiteral("[a-f]")))) {
        base = 16;
    }
    bool ok = false;
    const qint64 value = t.toLongLong(&ok, base);
    if (!ok || value < 0)
        return std::nullopt;
    return value;
}

void HexPanel::goTo()
{
    const auto offset = parseOffset(m_goTo->text());
    if (!offset || *offset >= m_view->data().size()) {
        m_status->setText(tr("No offset %1 in this file (%2 bytes)").arg(m_goTo->text()).arg(m_view->data().size()));
        return;
    }
    m_view->select(*offset);
    m_view->setFocus();
}

void HexPanel::find()
{
    const QByteArray needle = parseNeedle(m_find->text());
    if (needle.isEmpty())
        return;
    if (!m_view->findNext(needle))
        m_status->setText(tr("Not found: %1").arg(QString::fromLatin1(needle.toHex(' ').toUpper())));
}

void HexPanel::updateStatus()
{
    const QByteArray& d = m_view->data();
    if (d.isEmpty()) {
        m_status->clear();
        return;
    }
    const qint64 at = m_view->selectionLength() > 0 ? m_view->selectionStart() : m_view->cursor();
    QString s = tr("Offset %1 (0x%2)").arg(at).arg(QString::number(at, 16).toUpper());
    if (const qint64 n = m_view->selectionLength(); n > 0)
        s += tr("  ·  %n byte(s) selected", nullptr, static_cast<int>(std::min<qint64>(n, INT_MAX)));
    // The value at the cursor, little-endian, as the game reads it.
    const auto read = [&](int size) -> std::optional<quint32> {
        if (at + size > d.size())
            return std::nullopt;
        quint32 v = 0;
        std::memcpy(&v, d.constData() + at, static_cast<std::size_t>(size));
        return v;
    };
    if (const auto u8 = read(1))
        s += QStringLiteral("  ·  u8 %1").arg(*u8);
    if (const auto u16 = read(2))
        s += QStringLiteral("  u16 %1").arg(*u16);
    if (const auto u32 = read(4)) {
        s += QStringLiteral("  u32 %1  i32 %2").arg(*u32).arg(static_cast<qint32>(*u32));
        s += QStringLiteral("  f32 %1").arg(static_cast<double>(std::bit_cast<float>(*u32)), 0, 'g', 7);
    }
    m_status->setText(s);
}

} // namespace viewer::app
