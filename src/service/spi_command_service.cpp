#include "spi_command_service.h"
#include <QDateTime>

namespace ccv2 {

SpiCommandService::SpiCommandService(ConsoleLogModel *log, QObject *parent)
    : QObject(parent)
    , m_log(log)
{
}

void SpiCommandService::sendGlobal(const QString &commandName, const QString &bits32)
{
    ConsoleEntry entry;
    entry.ts = QDateTime::currentDateTime();
    entry.level = ConsoleEntry::Level::Tx;
    entry.source = QStringLiteral("spi");
    entry.message = QStringLiteral("[%1] %2").arg(commandName, bits32);

    if (m_log) m_log->append(entry);
    emit commandLogged(entry);

    // TODO: actually send via AcquisitionService TCP socket (Phase 6 full impl)
}

void SpiCommandService::sendStimulation(const StimulationParameters &p)
{
    // Build 32-bit stim command (simplified — matches legacy spi_control_panel.cpp format)
    QString blockBits = QString::number(p.channelId / 4, 2).rightJustified(6, QLatin1Char('0'));
    QString addrBits = QString::number(p.channelId % 4, 2).rightJustified(4, QLatin1Char('0'));
    int ampInt = qBound(0, (int)(p.amplitudeUa / 2000.0 * 511), 511);
    QString ampBits = QString::number(ampInt, 2).rightJustified(9, QLatin1Char('0'));
    QString cmd = QStringLiteral("000110") + blockBits + addrBits + QStringLiteral("0") +
                  QStringLiteral("0") + QStringLiteral("0") + QStringLiteral("0") +
                  QStringLiteral("0") + QStringLiteral("1") + ampBits;
    // Pad/trim to 32 bits
    cmd = cmd.left(32).rightJustified(32, QLatin1Char('0'));

    sendGlobal(QStringLiteral("Stim CH%1").arg(p.channelId), cmd);
}

void SpiCommandService::sendSequence(const QStringList &bitLines)
{
    for (const QString &line : bitLines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        sendGlobal(QStringLiteral("Seq"), trimmed.left(32));
    }
}

void SpiCommandService::logTx(const QString &bits)
{
    ConsoleEntry entry;
    entry.ts = QDateTime::currentDateTime();
    entry.level = ConsoleEntry::Level::Tx;
    entry.source = QStringLiteral("spi");
    entry.message = bits;
    if (m_log) m_log->append(entry);
    emit commandLogged(entry);
}

} // namespace ccv2
