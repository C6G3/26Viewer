#include "McpWindow.hpp"

#include "QtConvert.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QTextOption>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace viewer::app {

namespace {

/// The name assistants know the server by.
constexpr auto kServerName = "26viewer";

#ifdef Q_OS_WIN
constexpr auto kCliName = "26viewer-cli.exe";
#else
constexpr auto kCliName = "26viewer-cli";
#endif

/// `text` in double quotes for a command line.
QString quoted(const QString& text)
{
    return QLatin1Char('"') + text + QLatin1Char('"');
}

/// `{ <section>: { "26viewer": { [type], command, args } } }`, indented.
QString config(const char* section, bool withType, const QString& program, const QStringList& args)
{
    QJsonObject server;
    if (withType)
        server.insert("type", "stdio");
    server.insert("command", program);
    server.insert("args", QJsonArray::fromStringList(args));
    QJsonObject servers;
    servers.insert(kServerName, server);
    QJsonObject root;
    root.insert(section, servers);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)).trimmed();
}

QLabel* wrappingLabel(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

McpWindow::McpWindow(QWidget* parent)
    : QDialog(parent), m_program(wrappingLabel(this)), m_target(wrappingLabel(this)),
      m_allowEdits(new QCheckBox(tr("Allow &edits"), this)), m_test(new QPushButton(tr("&Test Connection"), this)),
      m_status(wrappingLabel(this)), m_tools(new QTreeWidget(this)), m_timeout(new QTimer(this))
{
    setWindowTitle(tr("AI Assistant (MCP)"));
    m_status->setTextFormat(Qt::PlainText);

    auto* about = wrappingLabel(this);
    about->setText(tr("Lets an AI assistant — Claude, VS Code's Copilot, Cursor and others — use 26Viewer on "
                      "this game: browse it, read decompiled files, search them, look up names and hashes and "
                      "inspect models. Add the server to your assistant's settings once (below); the assistant "
                      "starts it by itself whenever it needs it, so 26Viewer doesn't have to be open."));

    m_allowEdits->setToolTip(tr("Also offer the tools that change game files. Even then the assistant can only "
                                "write to an archive after backing it up, like Edit Mode."));
    connect(m_allowEdits, &QCheckBox::toggled, this, &McpWindow::refresh);

    auto* details = new QFormLayout;
    details->addRow(tr("Server program:"), m_program);
    details->addRow(tr("Game:"), m_target);
    details->addRow(QString(), m_allowEdits);

    auto* tabs = new QTabWidget(this);
    m_code = addPage(tabs, tr("Claude Code"), m_codeHint);
    m_desktop = addPage(tabs, tr("Claude Desktop"), m_desktopHint);
    m_vscode = addPage(tabs, tr("VS Code"), m_vscodeHint);
    m_other = addPage(tabs, tr("Other Apps"), m_otherHint);
    m_codeHint->setText(tr("Run this once in a terminal (Command Prompt, PowerShell…). <i>--scope user</i> makes it "
                           "available in every folder. To change it later, run <i>claude mcp remove %1</i> first.")
                            .arg(kServerName));
    m_desktopHint->setText(tr("In Claude Desktop: <i>Settings ▸ Developer ▸ Edit Config</i> opens "
                              "<i>claude_desktop_config.json</i>. Put this in it (if it already has an "
                              "\"mcpServers\" section, add only the \"%1\" part to that), save, and restart "
                              "Claude Desktop.")
                               .arg(kServerName));
    m_vscodeHint->setText(tr("Save this as <i>.vscode/mcp.json</i> in your folder — or run <i>MCP: Open User "
                             "Configuration</i> from the Command Palette to have it everywhere — then use Copilot "
                             "Chat in <i>Agent</i> mode."));
    m_otherHint->setText(tr("Cursor (<i>.cursor/mcp.json</i>), Windsurf, Cline and most other apps take the same "
                            "\"mcpServers\" block as Claude Desktop. Apps that ask for a command line take this:"));

    m_test->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_test->setToolTip(tr("Start the server the way an assistant would and see what it offers"));
    connect(m_test, &QPushButton::clicked, this, &McpWindow::test);
    auto* testRow = new QHBoxLayout;
    testRow->addWidget(m_test);
    testRow->addWidget(m_status, 1);

    m_tools->setColumnCount(2);
    m_tools->setHeaderLabels({tr("Tool"), tr("What it does")});
    m_tools->setRootIsDecorated(false);
    m_tools->setSelectionMode(QAbstractItemView::NoSelection);
    m_tools->setFocusPolicy(Qt::NoFocus);
    m_tools->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tools->header()->setStretchLastSection(true);

    m_timeout->setSingleShot(true);
    m_timeout->setInterval(60'000);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (m_process)
            m_process->kill(); // finished() reports it
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(about);
    layout->addLayout(details);
    layout->addWidget(tabs, 3);
    layout->addLayout(testRow);
    layout->addWidget(m_tools, 2);
    layout->addWidget(buttons);
    resize(880, 820);
    setSession(nullptr);
}

McpWindow::~McpWindow()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

QPlainTextEdit* McpWindow::addPage(QTabWidget* tabs, const QString& title, QLabel*& hint)
{
    auto* page = new QWidget(tabs);
    hint = wrappingLabel(page);
    hint->setTextFormat(Qt::RichText);
    auto* text = new QPlainTextEdit(page);
    text->setReadOnly(true);
    text->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); // long paths stay in view
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto* copy = new QPushButton(tr("&Copy"), page);
    copy->setToolTip(tr("Copy to the clipboard"));
    connect(copy, &QPushButton::clicked, this, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(copy);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(hint);
    layout->addWidget(text, 1);
    layout->addLayout(row);
    tabs->addTab(page, title);
    return text;
}

QString McpWindow::program() const
{
    return QDir::fromNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(kCliName));
}

QString McpWindow::target() const
{
    return m_session ? QDir::fromNativeSeparators(toQString(m_session->source)) : QString();
}

QStringList McpWindow::arguments() const
{
    QStringList args{QStringLiteral("mcp"), target()};
    if (m_allowEdits->isChecked())
        args << QStringLiteral("--allow-edits");
    return args;
}

void McpWindow::setSession(std::shared_ptr<Session> session)
{
    m_session = std::move(session);
    refresh();
}

void McpWindow::refresh()
{
    const bool found = QFileInfo::exists(program());
    m_program->setText(found ? program()
                             : tr("%1 — not found. Build or install the command-line tool next to 26Viewer.")
                                   .arg(QDir::toNativeSeparators(program())));
    m_target->setText(m_session ? target() : tr("Open a game first."));
    const bool ready = found && m_session;
    for (QPlainTextEdit* text : {m_code, m_desktop, m_vscode, m_other})
        text->setEnabled(ready);
    m_test->setEnabled(ready && !m_process);
    if (!ready) {
        for (QPlainTextEdit* text : {m_code, m_desktop, m_vscode, m_other})
            text->clear();
        return;
    }
    QStringList line{quoted(program())};
    for (const QString& a : arguments())
        line << (a.startsWith(QLatin1String("--")) || a == QLatin1String("mcp") ? a : quoted(a));
    m_code->setPlainText(QStringLiteral("claude mcp add --scope user %1 -- %2").arg(kServerName, line.join(' ')));
    m_desktop->setPlainText(config("mcpServers", false, program(), arguments()));
    m_vscode->setPlainText(config("servers", true, program(), arguments()));
    m_other->setPlainText(line.join(' '));
}

void McpWindow::test()
{
    if (m_process || !m_session)
        return;
    m_tools->clear();
    m_status->setText(tr("Starting the server…"));
    m_process = new QProcess(this);
    m_process->setProgram(program());
    m_process->setArguments(arguments());
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::finished, this, &McpWindow::testFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            testFinished();
    });
    m_test->setEnabled(false);
    m_started.start();
    m_timeout->start();
    m_process->start();
    // What an assistant sends first; closing stdin then makes the server exit.
    const QByteArray requests =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18",)"
        R"("capabilities":{},"clientInfo":{"name":"26Viewer","version":"test"}}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
        "\n";
    m_process->write(requests);
    m_process->closeWriteChannel();
}

void McpWindow::testFinished()
{
    if (!m_process)
        return;
    m_timeout->stop();
    QProcess* process = m_process;
    m_process = nullptr;
    process->deleteLater();
    m_test->setEnabled(true);
    const double seconds = m_started.elapsed() / 1000.0;

    if (process->error() == QProcess::FailedToStart) {
        m_status->setText(tr("✗ The server program didn't start: %1").arg(process->errorString()));
        return;
    }
    QString name, version;
    QJsonArray tools;
    for (const QByteArray& line : process->readAllStandardOutput().split('\n')) {
        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        if (reply.value("id").toInt() == 1) {
            const QJsonObject info = reply.value("result").toObject().value("serverInfo").toObject();
            name = info.value("name").toString();
            version = info.value("version").toString();
        } else if (reply.value("id").toInt() == 2) {
            tools = reply.value("result").toObject().value("tools").toArray();
        }
    }
    if (name.isEmpty() || tools.isEmpty()) {
        QString why = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (why.isEmpty())
            why = process->exitStatus() == QProcess::CrashExit ? tr("it stopped unexpectedly")
                                                                : tr("it gave no answer");
        m_status->setText(tr("✗ The server didn't answer properly: %1").arg(why));
        return;
    }
    m_status->setText(tr("✓ Works — %1 %2 answered in %3 s and offers these %n tool(s). "
                         "Your assistant will see the same.",
                         nullptr, static_cast<int>(tools.size()))
                          .arg(name, version, QString::number(seconds, 'f', 1)));
    for (const QJsonValue& value : tools) {
        const QJsonObject tool = value.toObject();
        QString what = tool.value("description").toString();
        if (const qsizetype end = what.indexOf(QLatin1String(". ")); end > 0)
            what = what.left(end + 1); // the first sentence
        auto* item = new QTreeWidgetItem(m_tools, {tool.value("name").toString(), what});
        item->setToolTip(1, tool.value("description").toString());
        if (!tool.value("annotations").toObject().value("readOnlyHint").toBool(true))
            item->setText(0, item->text(0) + tr("  (edits)"));
    }
}

} // namespace viewer::app
