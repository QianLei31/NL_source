#pragma once
#include <QObject>
#include "src/core/spi_protocol.h"
#include "src/core/stim_protocol.h"
#include "src/model/console_log_model.h"

namespace ccv2 {

class SpiCommandService : public QObject {
    Q_OBJECT
public:
    explicit SpiCommandService(ConsoleLogModel *log, QObject *parent = nullptr);

    // High-level productized commands
    void sendGlobal(const QString &commandName, const QString &bits32);
    void sendStimulation(const StimulationParameters &p);
    void sendSequence(const QStringList &bitLines);

signals:
    void commandLogged(const ConsoleEntry &entry);

private:
    void logTx(const QString &bits);
    ConsoleLogModel *m_log = nullptr;
};

} // namespace ccv2
