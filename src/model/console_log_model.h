#pragma once
#include <QObject>
#include <QVector>
#include <QDateTime>
#include <QString>

namespace ccv2 {

struct ConsoleEntry {
    QDateTime ts;
    enum class Level { Info, Tx, Rx, Warn, Error } level = Level::Info;
    QString source;
    QString message;
};

class ConsoleLogModel : public QObject {
    Q_OBJECT
public:
    explicit ConsoleLogModel(QObject *parent = nullptr);

    void append(const ConsoleEntry &entry);
    void clear();
    QVector<ConsoleEntry> entries() const { return m_entries; }
    QVector<ConsoleEntry> filtered(const QSet<ConsoleEntry::Level> &levels, const QString &search) const;
    int count() const { return m_entries.size(); }

signals:
    void entryAppended(const ConsoleEntry &entry);
    void cleared();

private:
    static constexpr int kMaxEntries = 5000;
    QVector<ConsoleEntry> m_entries;
};

} // namespace ccv2
