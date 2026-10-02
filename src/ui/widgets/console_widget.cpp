#include "console_widget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>

namespace ccv2 {

ConsoleWidget::ConsoleWidget(ConsoleLogModel *model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // Toolbar
    auto *toolbar = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("Search..."));
    toolbar->addWidget(m_searchEdit);

    m_clearBtn = new QPushButton(QStringLiteral("Clear"), this);
    m_clearBtn->setObjectName(QStringLiteral("ghost"));
    toolbar->addWidget(m_clearBtn);

    m_exportBtn = new QPushButton(QStringLiteral("Export"), this);
    m_exportBtn->setObjectName(QStringLiteral("ghost"));
    toolbar->addWidget(m_exportBtn);

    layout->addLayout(toolbar);

    // Text area
    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_textEdit->setMaximumBlockCount(5000);
    m_textEdit->setPlaceholderText(QStringLiteral("Console idle. Verify connection to start."));
    layout->addWidget(m_textEdit);

    // Connections
    connect(m_model, &ConsoleLogModel::entryAppended, this, &ConsoleWidget::onEntryAppended);
    connect(m_model, &ConsoleLogModel::cleared, m_textEdit, &QPlainTextEdit::clear);
    connect(m_clearBtn, &QPushButton::clicked, this, &ConsoleWidget::onClear);
    connect(m_exportBtn, &QPushButton::clicked, this, &ConsoleWidget::onExport);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ConsoleWidget::onSearchChanged);
}

void ConsoleWidget::onEntryAppended(const ConsoleEntry &entry)
{
    if (!m_searchFilter.isEmpty()) {
        if (!entry.source.contains(m_searchFilter, Qt::CaseInsensitive) &&
            !entry.message.contains(m_searchFilter, Qt::CaseInsensitive))
            return;
    }
    m_textEdit->appendPlainText(formatEntry(entry));
}

void ConsoleWidget::onClear()
{
    m_model->clear();
}

void ConsoleWidget::onExport()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Log"),
                                                QString(), QStringLiteral("Log files (*.log *.txt)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        for (const auto &e : m_model->entries()) {
            out << formatEntry(e) << "\n";
        }
    }
}

void ConsoleWidget::onSearchChanged(const QString &text)
{
    m_searchFilter = text;
    // Rebuild display
    m_textEdit->clear();
    QSet<ConsoleEntry::Level> allLevels;
    auto filtered = m_model->filtered(allLevels, text);
    for (const auto &e : filtered) {
        m_textEdit->appendPlainText(formatEntry(e));
    }
}

QString ConsoleWidget::formatEntry(const ConsoleEntry &entry) const
{
    static const char* levelNames[] = {"INFO", "TX  ", "RX  ", "WARN", "ERR "};
    int idx = static_cast<int>(entry.level);
    if (idx < 0 || idx > 4) idx = 0;
    return QStringLiteral("[%1] %2  [%3] %4")
        .arg(entry.ts.toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(QLatin1String(levelNames[idx]))
        .arg(entry.source)
        .arg(entry.message);
}

} // namespace ccv2
