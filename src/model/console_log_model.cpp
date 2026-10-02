#include "console_log_model.h"

namespace ccv2 {

ConsoleLogModel::ConsoleLogModel(QObject *parent)
    : QObject(parent)
{
}

void ConsoleLogModel::append(const ConsoleEntry &entry)
{
    m_entries.append(entry);
    while (m_entries.size() > kMaxEntries)
        m_entries.removeFirst();
    emit entryAppended(entry);
}

void ConsoleLogModel::clear()
{
    m_entries.clear();
    emit cleared();
}

QVector<ConsoleEntry> ConsoleLogModel::filtered(const QSet<ConsoleEntry::Level> &levels, const QString &search) const
{
    QVector<ConsoleEntry> result;
    for (const auto &e : m_entries) {
        if (!levels.isEmpty() && !levels.contains(e.level))
            continue;
        if (!search.isEmpty()) {
            if (!e.source.contains(search, Qt::CaseInsensitive) &&
                !e.message.contains(search, Qt::CaseInsensitive))
                continue;
        }
        result.append(e);
    }
    return result;
}

} // namespace ccv2
