#pragma once

#include <QComboBox>
#include <QFuture>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QQueue>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QVector>

#include <atomic>

#include "config/config_manager.h"
#include "core/channel_address_state.h"
#include "core/network_state.h"

class QTcpSocket;
class QGridLayout;

namespace ccv2 {

class SpiControlPanel : public QWidget {
    Q_OBJECT

public:
    explicit SpiControlPanel(ConfigManager *cfgMgr,
                             NetworkState *networkState,
                             ChannelAddressState *channelState,
                             QWidget *parent = nullptr);
    ~SpiControlPanel() override;

    // Queue externally-built 32-bit SPI command strings (e.g. batch channel
    // config from the map page) through this panel's send pipeline + console.
    // A dummy (all-zero) command is sent after every command, and
    // interCommandDelayUs is applied between every command (hardware timing).
    void enqueueExternalCommands(const QString &label, const QStringList &commands,
                                 int interCommandDelayUs = 100);
    // Flush debounced config writes; called from the main window's closeEvent
    // (this panel outlives the ConfigManager during destruction, so flushing
    // in the destructor would be unsafe).
    void flushPendingConfig();

signals:
    void spiCommandApplied(const QString &commandBits);
    // Transport health is separate from acquisition: a command may reach the
    // control port even when no data session is running.
    void controlLinkChecking();
    void controlLinkResult(bool reachable, bool degraded, const QString &message);
    // True while the serial SPI task queue is executing; false once drained.
    // The map page disables its batch-write buttons during flight so a second
    // click can't snapshot half-updated shadow registers.
    void busyChanged(bool busy);

private slots:
    void sendDirectSpi();
    void onGetChannelAddress();
    void updateNetworkHint(const QString &host, int port);
    void editQuickCommands();

private:
    struct QuickCommand {
        QString name;
        QStringList commands;
    };
    struct SpiSendTask {
        QString host;
        int port{0};
        quint64 endpointGeneration{0};
        QString label;
        QStringList commands;
        QVector<bool> showReplies;
        int pauseAfterFirstMs{0};
        int interCommandDelayUs{0};  // delay inserted between consecutive commands
    };

    void loadAttrs();
    void buildUi();
    void log(const QString &msg, const QString &style = QString());

    void singleTcp(const QString &spiCmd, bool showReply = true);
    void sendSpiSequence(const QStringList &commands, const QString &label);
    void enqueueSpiTask(const SpiSendTask &task);
    void startNextSpiTask();
    void finishSpiTask(const QString &label, bool ok);
    QVector<QuickCommand> defaultQuickCommands() const;
    bool expandQuickCommandToken(const QString &token, QStringList *commands) const;
    QVector<QuickCommand> loadQuickCommands() const;
    void saveQuickCommands(const QVector<QuickCommand> &commands);
    void rebuildQuickCommands();
    void runQuickCommand(const QuickCommand &command);
    QString quickCommandsToEditorText(const QVector<QuickCommand> &commands) const;
    bool parseQuickCommandsText(const QString &text, QVector<QuickCommand> *commands, QString *error) const;
    bool normalizeSpiBits(const QString &text, QString *normalized) const;
    QString commandsToConfigText(const QuickCommand &command) const;
    void saveSpiConfig();
    void saveStimulatorConfig();
    void scheduleSaveStimulatorConfig();
    void stimSend(const QString &offBit);

    ConfigManager *m_cfgMgr;
    NetworkState *m_networkState;
    ChannelAddressState *m_channelState;
    ConfigMap m_appCfg;

    QLineEdit *m_spiDirectEdit{nullptr};
    QGridLayout *m_quickCommandsLayout{nullptr};
    QVector<QuickCommand> m_quickCommands;
    QQueue<SpiSendTask> m_spiQueue;
    bool m_spiBusy{false};
    QTimer m_stimSaveTimer;
    std::atomic_bool m_destroying{false};
    std::atomic<quint64> m_endpointGeneration{0};
    QFuture<void> m_spiFuture;

    QLineEdit *m_stimBlock{nullptr};
    QComboBox *m_stimAddr{nullptr};
    QLineEdit *m_stimAmp{nullptr};
    QComboBox *m_stimPolarity{nullptr};
    QComboBox *m_stimDac{nullptr};
    QComboBox *m_stimComp{nullptr};
    QComboBox *m_stimStep{nullptr};

    QLabel *m_netHintLabel{nullptr};
    QPlainTextEdit *m_addrPreview{nullptr};
    QPlainTextEdit *m_console{nullptr};
};

}  // namespace ccv2
