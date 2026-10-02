#include "ui/spi_control_panel.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTcpSocket>
#include <QThread>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

#include "core/constants.h"

namespace ccv2 {

namespace {

void setComboByValuePrefix(QComboBox *combo, const QString &storedText) {
    if (!combo) {
        return;
    }
    const QString wanted = storedText.trimmed().split(' ').value(0);
    if (wanted.isEmpty()) {
        return;
    }
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i).trimmed().split(' ').value(0) == wanted) {
            combo->setCurrentIndex(i);
            return;
        }
    }
}

QString comboValuePrefix(QComboBox *combo)
{
    return combo ? combo->currentText().trimmed().split(' ').value(0) : QString();
}

QStringList makeGlobalGainCommands(const QString &dataBits) {
    QStringList commands;
    commands.reserve(512);
    const QString dummy(32, QLatin1Char('0'));
    for (int block = 0; block < 64; ++block) {
        const QString blockBits = QStringLiteral("00") +
                                  QString::number(block, 2).rightJustified(6, QLatin1Char('0'));
        for (int channel = 0; channel < 4; ++channel) {
            const QString channelBits = QString::number(channel, 2).rightJustified(2, QLatin1Char('0'));
            commands.push_back(QStringLiteral("000100") + blockBits + channelBits + dataBits);
            commands.push_back(dummy);
        }
    }
    return commands;
}

struct SpiCommandResult {
    QString tx;
    QStringList rxLines;
    QString error;
    // True once the command bytes were written to the socket: a later reply
    // failure means "sent but unconfirmed", not "not sent" — the shadow
    // registers must still record it.
    bool sent{false};
};

QString normalizeSpiBitsForWorker(const QString &text, QString *error = nullptr)
{
    QString clean = text.trimmed();
    clean.remove('_');
    clean.remove(' ');
    if (clean.size() != 32) {
        if (error) {
            *error = QStringLiteral("命令长度错误: 当前 %1 bit，应为 32 bit").arg(clean.size());
        }
        return {};
    }
    for (const QChar ch : clean) {
        if (ch != QLatin1Char('0') && ch != QLatin1Char('1')) {
            if (error) {
                *error = QStringLiteral("命令无效: 每条命令必须是 32 bit，只能包含 0/1");
            }
            return {};
        }
    }
    return clean;
}

QByteArray recvTcpWorker(QTcpSocket &socket, int nbytes, int timeoutMs)
{
    QByteArray buf;
    qint64 remain = nbytes;
    QElapsedTimer timer;
    timer.start();

    while (remain > 0) {
        if (!socket.waitForReadyRead(10)) {
            if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
                break;
            }
            if (socket.state() != QAbstractSocket::ConnectedState) {
                break;
            }
            continue;
        }
        const QByteArray rx = socket.read(qMin<qint64>(remain, kTcpBuf));
        if (rx.isEmpty()) {
            break;
        }
        buf.append(rx);
        remain -= rx.size();
    }

    return buf;
}

bool writeSpiBitsWorker(QTcpSocket &socket, const QString &spiCmd, QString *wireMessage, QString *error)
{
    const QString clean = normalizeSpiBitsForWorker(spiCmd, error);
    if (clean.isEmpty()) {
        return false;
    }

    QByteArray raw;
    raw.reserve(clean.size() / 8);
    for (int i = 0; i < clean.size(); i += 8) {
        bool ok = false;
        const int v = clean.mid(i, 8).toInt(&ok, 2);
        if (!ok) {
            if (error) {
                *error = QStringLiteral("命令只能包含 0/1");
            }
            return false;
        }
        raw.push_back(static_cast<char>(v));
    }

    const QString msg = QStringLiteral("spi") + QString::fromLatin1(raw.toHex());
    socket.write(msg.toUtf8());
    if (!socket.waitForBytesWritten(1000)) {
        if (error) {
            *error = QStringLiteral("发送失败: ") + socket.errorString();
        }
        return false;
    }
    if (wireMessage) {
        *wireMessage = msg;
    }
    return true;
}

SpiCommandResult sendSpiCommandWorker(const QString &host,
                                      int port,
                                      const QString &command,
                                      bool showReply,
                                      int replyTimeoutMs)
{
    SpiCommandResult result;
    QTcpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(5000)) {
        result.error = QStringLiteral("连接错误: ") + socket.errorString();
        return result;
    }

    if (!writeSpiBitsWorker(socket, command, &result.tx, &result.error)) {
        socket.disconnectFromHost();
        return result;
    }
    result.sent = true;

    if (!showReply) {
        socket.disconnectFromHost();
        return result;
    }

    const QByteArray data = recvTcpWorker(socket, 12, replyTimeoutMs);
    if (data.size() < 12) {
        // A short read used to fall through silently: the parse loop below
        // never entered, no error was set, and the caller treated the command
        // as fully confirmed. Surface it — but note the write itself already
        // reached the device (see result.sent).
        result.error = QStringLiteral("命令已发出，但回包超时/不完整(仅收到 %1/12 字节)，芯片状态未确认")
                           .arg(data.size());
        socket.disconnectFromHost();
        return result;
    }
    for (int i = 8; i + 4 <= data.size(); i += 4) {
        QByteArray chunk = data.mid(i, 4);
        std::reverse(chunk.begin(), chunk.end());
        const QString bits = QString::number(chunk.toHex().toUInt(nullptr, 16), 2).rightJustified(32, QLatin1Char('0'));
        result.rxLines << QStringLiteral("RX <- Code:%1 Addr:%2 Data:%3")
                              .arg(bits.mid(0, 6), bits.mid(6, 10), bits.mid(16));
    }

    socket.disconnectFromHost();
    return result;
}

}  // namespace

SpiControlPanel::SpiControlPanel(ConfigManager *cfgMgr,
                                 NetworkState *networkState,
                                 ChannelAddressState *channelState,
                                 QWidget *parent)
    : QWidget(parent), m_cfgMgr(cfgMgr), m_networkState(networkState), m_channelState(channelState) {
    m_appCfg = m_cfgMgr->load();
    loadAttrs();

    m_stimSaveTimer.setSingleShot(true);
    m_stimSaveTimer.setInterval(500);
    connect(&m_stimSaveTimer, &QTimer::timeout, this, &SpiControlPanel::saveStimulatorConfig);

    buildUi();

    updateNetworkHint(m_networkState->host(), m_networkState->port());
    connect(m_networkState, &NetworkState::endpointChanged, this, &SpiControlPanel::updateNetworkHint);
    connect(m_networkState, &NetworkState::endpointChanged, this, [this]() {
        m_endpointGeneration.fetch_add(1);
        const int cancelled = m_spiQueue.size();
        m_spiQueue.clear();
        if (m_spiBusy || cancelled > 0) {
            log(QStringLiteral("控制地址已改变，取消旧设备的剩余 SPI 命令"), QStringLiteral("warn"));
        }
    });
}

void SpiControlPanel::scheduleSaveStimulatorConfig() {
    m_stimSaveTimer.start();
}

void SpiControlPanel::flushPendingConfig() {
    if (m_stimSaveTimer.isActive()) {
        m_stimSaveTimer.stop();
        saveStimulatorConfig();
    }
}

SpiControlPanel::~SpiControlPanel() {
    m_destroying.store(true);
    if (m_spiFuture.isRunning()) {
        m_spiFuture.waitForFinished();
    }
}

void SpiControlPanel::loadAttrs() {
    // Reserved for future field normalization.
}

void SpiControlPanel::log(const QString &msg, const QString &style) {
    const QString prefix = style.isEmpty() ? QString() : QStringLiteral("[") + style.toUpper() + QStringLiteral("] ");
    m_console->appendPlainText(prefix + msg);
}

void SpiControlPanel::enqueueExternalCommands(const QString &label, const QStringList &commands,
                                              int interCommandDelayUs) {
    if (commands.isEmpty()) return;
    const QString dummy(32, QLatin1Char('0'));

    // Validate first, then follow every real write with a dummy. This mirrors
    // the proven Python sequence, including the final write.
    QStringList valid;
    for (const QString &cmd : commands) {
        QString normalized;
        if (!normalizeSpiBits(cmd, &normalized)) {
            log(QStringLiteral("%1: 跳过无效命令(需 32bit): %2").arg(label, cmd),
                QStringLiteral("error"));
            continue;
        }
        valid.append(normalized);
    }
    if (valid.isEmpty()) return;

    SpiSendTask task;
    task.label = label;
    task.interCommandDelayUs = qMax(0, interCommandDelayUs);
    for (const QString &command : valid) {
        task.commands.append(command);
        task.showReplies.append(false);
        task.commands.append(dummy);
        task.showReplies.append(false);
    }
    log(QStringLiteral("%1: %2 条命令(含 dummy 间隔),命令间延时 %3 us")
            .arg(label).arg(task.commands.size()).arg(task.interCommandDelayUs));
    enqueueSpiTask(task);
}

void SpiControlPanel::singleTcp(const QString &spiCmd, bool showReply) {
    QString normalized;
    if (!normalizeSpiBits(spiCmd, &normalized)) {
        log(QStringLiteral("命令无效: 每条命令必须是 32 bit，只能包含 0/1"), QStringLiteral("error"));
        return;
    }

    SpiSendTask task;
    task.label = QStringLiteral("SPI命令");
    task.commands = {normalized};
    task.showReplies = {showReply};
    enqueueSpiTask(task);
}

void SpiControlPanel::sendSpiSequence(const QStringList &commands, const QString &label) {
    if (commands.isEmpty()) {
        return;
    }

    SpiSendTask task;
    task.label = label;
    task.commands.reserve(commands.size());
    task.showReplies.reserve(commands.size());
    // Large sequences (quick commands like 增益全高 = 512 entries) must not
    // wait for a 12-byte echo per command: a silent device would cost
    // 512 x replyTimeout with the SPI queue blocked throughout. Match the
    // batch path (enqueueExternalCommands), which never reads replies.
    // Dummy (all-zero) commands never get an echo either — stimSend and
    // enqueueExternalCommands both skip reading them.
    const bool showReply = commands.size() <= 8;
    const QString dummy(32, QLatin1Char('0'));
    for (const QString &command : commands) {
        QString normalized;
        if (!normalizeSpiBits(command, &normalized)) {
            log(QStringLiteral("%1: 存在无效命令，已取消发送").arg(label), QStringLiteral("error"));
            return;
        }
        task.commands.push_back(normalized);
        task.showReplies.push_back(showReply && normalized != dummy);
    }
    if (!showReply) {
        log(QStringLiteral("%1: 序列超过 8 条，不读取回包").arg(label));
    }
    enqueueSpiTask(task);
}

void SpiControlPanel::enqueueSpiTask(const SpiSendTask &task) {
    if (task.commands.isEmpty()) {
        return;
    }
    SpiSendTask boundTask = task;
    boundTask.host = m_networkState->host();
    boundTask.port = m_networkState->port();
    boundTask.endpointGeneration = m_endpointGeneration.load();
    m_spiQueue.enqueue(boundTask);
    log(QStringLiteral("%1: 已加入队列 (%2 条，队列剩余 %3)")
            .arg(task.label)
            .arg(task.commands.size())
            .arg(m_spiQueue.size()));
    startNextSpiTask();
}

void SpiControlPanel::startNextSpiTask() {
    if (m_spiBusy || m_spiQueue.isEmpty() || m_destroying.load()) {
        return;
    }

    m_spiBusy = true;
    emit busyChanged(true);
    emit controlLinkChecking();
    const SpiSendTask task = m_spiQueue.dequeue();
    const QString host = task.host;
    const int port = task.port;
    log(QStringLiteral("%1: 开始后台发送 %2 条 @%3:%4")
            .arg(task.label)
            .arg(task.commands.size())
            .arg(host)
            .arg(port));

    QPointer<SpiControlPanel> self(this);
    m_spiFuture = QtConcurrent::run([self, task, host, port]() {
        bool ok = true;
        const int total = task.commands.size();
        const bool verboseTx = total <= 8;
        for (int i = 0; i < total; ++i) {
            if (!self || self->m_destroying.load()) {
                return;
            }
            if (self->m_endpointGeneration.load() != task.endpointGeneration) {
                ok = false;
                break;
            }

            const bool showReply = i < task.showReplies.size() ? task.showReplies[i] : true;
            // Only short sequences (<=8, non-dummy) read replies at all; the
            // 800ms tier is defensive for any future large task that opts in.
            const int replyTimeoutMs = total > 8 ? 800 : 5000;
            const SpiCommandResult result = sendSpiCommandWorker(host, port, task.commands[i], showReply, replyTimeoutMs);
            if (!self || self->m_destroying.load()) {
                return;
            }

            QMetaObject::invokeMethod(self, [self, task, result, i, total, verboseTx]() {
                if (!self || self->m_endpointGeneration.load() != task.endpointGeneration) {
                    return;
                }
                if (!result.error.isEmpty()) {
                    if (result.sent) {
                        // The write reached the wire; keep the shadow
                        // registers in step with what the hardware received
                        // even though the echo was lost.
                        emit self->spiCommandApplied(task.commands[i]);
                        emit self->controlLinkResult(
                            true, true,
                            QStringLiteral("控制命令已发送，但回包异常：%1").arg(result.error));
                    } else {
                        emit self->controlLinkResult(false, false, result.error);
                    }
                    self->log(QStringLiteral("%1: 第 %2/%3 条失败: %4")
                                  .arg(task.label)
                                  .arg(i + 1)
                                  .arg(total)
                                  .arg(result.error),
                              QStringLiteral("error"));
                    return;
                }
                emit self->controlLinkResult(
                    true, false, QStringLiteral("SPI 控制命令发送成功"));
                if (verboseTx) {
                    self->log(QStringLiteral("TX -> %1").arg(result.tx));
                }
                emit self->spiCommandApplied(task.commands[i]);
                for (const QString &line : result.rxLines) {
                    self->log(line, QStringLiteral("success"));
                }
                if ((i + 1) % 32 == 0 || i + 1 == total) {
                    self->log(QStringLiteral("%1: 进度 %2/%3").arg(task.label).arg(i + 1).arg(total));
                }
            }, Qt::QueuedConnection);

            if (!result.error.isEmpty()) {
                ok = false;
                break;
            }
            if (task.pauseAfterFirstMs > 0 && i == 0) {
                QThread::msleep(static_cast<unsigned long>(task.pauseAfterFirstMs));
            }
            // Hardware inter-command spacing (applied between every command).
            if (task.interCommandDelayUs > 0 && i + 1 < total) {
                QThread::usleep(static_cast<unsigned long>(task.interCommandDelayUs));
            }
        }

        if (!self || self->m_destroying.load()) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, task, ok]() {
            if (self) {
                self->finishSpiTask(task.label, ok);
            }
        }, Qt::QueuedConnection);
    });
}

void SpiControlPanel::finishSpiTask(const QString &label, bool ok) {
    m_spiBusy = false;
    log(QStringLiteral("%1: %2").arg(label, ok ? QStringLiteral("完成") : QStringLiteral("中止")),
        ok ? QStringLiteral("success") : QStringLiteral("error"));
    if (m_spiQueue.isEmpty()) {
        emit busyChanged(false);
    }
    startNextSpiTask();
}

void SpiControlPanel::sendDirectSpi() {
    QString cmd = m_spiDirectEdit->text().trimmed();
    cmd.remove('_');
    cmd.remove(' ');
    if (cmd.size() != 32) {
        log(QStringLiteral("命令长度错误: 当前 %1 bit，应为 32 bit").arg(cmd.size()), QStringLiteral("error"));
        return;
    }
    saveSpiConfig();
    singleTcp(cmd);
}

QVector<SpiControlPanel::QuickCommand> SpiControlPanel::defaultQuickCommands() const {
    QVector<QuickCommand> commands;
    commands.push_back({QStringLiteral("增益全高"),
                        makeGlobalGainCommands(QStringLiteral("0000000001011110"))});
    commands.push_back({QStringLiteral("增益全低"),
                        makeGlobalGainCommands(QStringLiteral("0000000001111110"))});
    return commands;
}

bool SpiControlPanel::normalizeSpiBits(const QString &text, QString *normalized) const {
    QString clean = text.trimmed();
    clean.remove('_');
    clean.remove(' ');
    if (clean.size() != 32) {
        return false;
    }
    for (const QChar ch : clean) {
        if (ch != QLatin1Char('0') && ch != QLatin1Char('1')) {
            return false;
        }
    }
    if (normalized) {
        *normalized = clean;
    }
    return true;
}

bool SpiControlPanel::expandQuickCommandToken(const QString &token, QStringList *commands) const {
    const QString clean = token.trimmed().toLower();
    if (clean == QStringLiteral("@gain_all_high") || clean == QStringLiteral("@增益全高")) {
        if (commands) {
            commands->append(makeGlobalGainCommands(QStringLiteral("0000000001011110")));
        }
        return true;
    }
    if (clean == QStringLiteral("@gain_all_low") || clean == QStringLiteral("@增益全低")) {
        if (commands) {
            commands->append(makeGlobalGainCommands(QStringLiteral("0000000001111110")));
        }
        return true;
    }
    return false;
}

bool SpiControlPanel::parseQuickCommandsText(const QString &text,
                                             QVector<QuickCommand> *commands,
                                             QString *error) const {
    QVector<QuickCommand> parsed;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        QString line = lines[lineIndex].trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            if (error) {
                *error = QStringLiteral("第 %1 行缺少 '='，格式应为: 名称 = 32bit[,32bit...]").arg(lineIndex + 1);
            }
            return false;
        }

        const QString name = line.left(eq).trimmed();
        const QString rawCommands = line.mid(eq + 1).trimmed();
        if (name.isEmpty()) {
            if (error) {
                *error = QStringLiteral("第 %1 行缺少快速命令名称").arg(lineIndex + 1);
            }
            return false;
        }

        const QStringList tokens = rawCommands.split(QRegularExpression(QStringLiteral("[,;]+")),
                                                     Qt::SkipEmptyParts);
        QStringList normalizedCommands;
        for (const QString &token : tokens) {
            if (expandQuickCommandToken(token, &normalizedCommands)) {
                continue;
            }
            QString normalized;
            if (!normalizeSpiBits(token, &normalized)) {
                if (error) {
                    *error = QStringLiteral("第 %1 行命令无效: 每条命令必须是 32 bit，只能包含 0/1；或使用 @gain_all_high / @gain_all_low")
                                 .arg(lineIndex + 1);
                }
                return false;
            }
            normalizedCommands.push_back(normalized);
        }

        if (normalizedCommands.isEmpty()) {
            if (error) {
                *error = QStringLiteral("第 %1 行没有有效的 32-bit 命令").arg(lineIndex + 1);
            }
            return false;
        }

        parsed.push_back({name, normalizedCommands});
    }

    if (parsed.isEmpty()) {
        if (error) {
            *error = QStringLiteral("至少需要保留一个快速命令");
        }
        return false;
    }

    if (commands) {
        *commands = parsed;
    }
    return true;
}

QString SpiControlPanel::commandsToConfigText(const QuickCommand &command) const {
    const QStringList high = makeGlobalGainCommands(QStringLiteral("0000000001011110"));
    const QStringList low = makeGlobalGainCommands(QStringLiteral("0000000001111110"));
    if (command.commands == high) {
        return QStringLiteral("@gain_all_high");
    }
    if (command.commands == low) {
        return QStringLiteral("@gain_all_low");
    }
    return command.commands.join(QLatin1Char(','));
}

QVector<SpiControlPanel::QuickCommand> SpiControlPanel::loadQuickCommands() const {
    const QVector<QuickCommand> defaults = defaultQuickCommands();
    const ConfigSection sec = m_appCfg.value(QStringLiteral("SpiQuickCommands"));
    bool ok = false;
    const int count = sec.value(QStringLiteral("count"), QStringLiteral("0")).toInt(&ok);
    if (!ok || count <= 0) {
        return defaults;
    }

    QVector<QuickCommand> commands;
    for (int i = 0; i < count; ++i) {
        const QString name = sec.value(QStringLiteral("name_%1").arg(i)).trimmed();
        const QString raw = sec.value(QStringLiteral("commands_%1").arg(i)).trimmed();
        QVector<QuickCommand> parsedLine;
        QString error;
        if (parseQuickCommandsText(QStringLiteral("%1 = %2").arg(name, raw), &parsedLine, &error) &&
            !parsedLine.isEmpty()) {
            commands.push_back(parsedLine.first());
        }
    }

    if (commands.size() == 2 &&
        commands[0].name == QStringLiteral("增益全高") &&
        commands[0].commands == QStringList{QStringLiteral("00110100000000000000000000000001")} &&
        commands[1].name == QStringLiteral("增益全低") &&
        commands[1].commands == QStringList{QStringLiteral("00110100000000000000000000000000")}) {
        return defaults;
    }

    return commands.isEmpty() ? defaults : commands;
}

void SpiControlPanel::saveQuickCommands(const QVector<QuickCommand> &commands) {
    ConfigSection sec;
    sec[QStringLiteral("count")] = QString::number(commands.size());
    for (int i = 0; i < commands.size(); ++i) {
        sec[QStringLiteral("name_%1").arg(i)] = commands[i].name;
        sec[QStringLiteral("commands_%1").arg(i)] = commandsToConfigText(commands[i]);
    }
    m_appCfg = m_cfgMgr->load();
    m_appCfg[QStringLiteral("SpiQuickCommands")] = sec;
    if (!m_cfgMgr->save(m_appCfg)) {
        log(QStringLiteral("保存快速命令失败"), QStringLiteral("error"));
    }
}

QString SpiControlPanel::quickCommandsToEditorText(const QVector<QuickCommand> &commands) const {
    QStringList lines;
    for (const QuickCommand &command : commands) {
        lines << QStringLiteral("%1 = %2").arg(command.name, commandsToConfigText(command));
    }
    return lines.join(QLatin1Char('\n'));
}

void SpiControlPanel::rebuildQuickCommands() {
    if (!m_quickCommandsLayout) {
        return;
    }

    while (QLayoutItem *item = m_quickCommandsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            delete widget;
        }
        delete item;
    }

    for (int i = 0; i < m_quickCommands.size(); ++i) {
        const QuickCommand command = m_quickCommands[i];
        auto *btn = new QPushButton(command.name);
        btn->setProperty("variant", "secondary");
        btn->setToolTip(QStringLiteral("%1 条 32-bit SPI 命令").arg(command.commands.size()));
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(btn, &QPushButton::clicked, this, [this, command]() { runQuickCommand(command); });
        m_quickCommandsLayout->addWidget(btn, i / 2, i % 2);
    }
}

void SpiControlPanel::runQuickCommand(const QuickCommand &command) {
    sendSpiSequence(command.commands, QStringLiteral("快速命令: %1").arg(command.name));
}

void SpiControlPanel::editQuickCommands() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑快速命令"));
    dialog.resize(560, 420);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *hint = new QLabel(QStringLiteral("每行一个按钮: 名称 = 32bit[,32bit...]。可用 @gain_all_high / @gain_all_low；多条命令会按顺序发送。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *editor = new QPlainTextEdit(&dialog);
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setPlainText(quickCommandsToEditorText(m_quickCommands));
    layout->addWidget(editor, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *resetButton = buttons->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    connect(resetButton, &QPushButton::clicked, &dialog, [this, editor]() {
        editor->setPlainText(quickCommandsToEditorText(defaultQuickCommands()));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    while (dialog.exec() == QDialog::Accepted) {
        QVector<QuickCommand> commands;
        QString error;
        if (!parseQuickCommandsText(editor->toPlainText(), &commands, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("快速命令格式错误"), error);
            continue;
        }

        m_quickCommands = commands;
        saveQuickCommands(m_quickCommands);
        rebuildQuickCommands();
        log(QStringLiteral("快速命令已更新: %1 个按钮").arg(m_quickCommands.size()), QStringLiteral("success"));
        break;
    }
}

void SpiControlPanel::onGetChannelAddress() {
    const QVector<AddressRecord> records = m_channelState->selectedAddressRecords();
    if (records.isEmpty()) {
        if (m_addrPreview) m_addrPreview->setPlainText(QStringLiteral("未选择任何通道"));
        log(QStringLiteral("读取通道地址: 尚未选择通道"), QStringLiteral("error"));
        return;
    }

    QStringList lines;
    lines << QStringLiteral("Block  本地  全局  SPI(8b+2b)");
    for (const AddressRecord &record : records) {
        lines << QStringLiteral("%1     %2     %3     %4")
                     .arg(record.block, 2, 10, QLatin1Char('0'))
                     .arg(record.localBits)
                     .arg(record.globalChannel, 3, 10, QLatin1Char('0'))
                     .arg(record.addressBits);
    }
    if (m_addrPreview) m_addrPreview->setPlainText(lines.join('\n'));
    log(QStringLiteral("读取通道地址: %1 条").arg(records.size()));
}

void SpiControlPanel::updateNetworkHint(const QString &host, int port) {
    if (m_netHintLabel) {
        m_netHintLabel->setText(QStringLiteral("%1:%2").arg(host).arg(port));
    }
}

void SpiControlPanel::stimSend(const QString &offBit) {
    saveStimulatorConfig();
    const QString blk = m_stimBlock->text().trimmed().rightJustified(8, QLatin1Char('0'));
    const QString adr = m_stimAddr->currentText().split(' ').value(0);
    const QString dac = m_stimDac->currentText().split(' ').value(0);
    const QString stp = m_stimStep->currentText().split(' ').value(0);
    const QString cmp = m_stimComp->currentText().split(' ').value(0);
    const QString pol = m_stimPolarity->currentText().split(' ').value(0);
    const QString amp = m_stimAmp->text().trimmed().rightJustified(9, QLatin1Char('0'));
    const QString cmd = QStringLiteral("000110") + blk + adr + offBit + dac + stp + cmp + pol + amp;

    SpiSendTask task;
    task.label = offBit == QStringLiteral("0") ? QStringLiteral("刺激输出") : QStringLiteral("刺激关闭");
    task.commands = {QString(32, QLatin1Char('0')), cmd};
    task.showReplies = {false, true};
    task.pauseAfterFirstMs = 100;
    enqueueSpiTask(task);
}

void SpiControlPanel::saveSpiConfig()
{
    if (!m_cfgMgr || !m_spiDirectEdit) {
        return;
    }
    m_appCfg = m_cfgMgr->load();
    m_appCfg[QStringLiteral("Spi")][QStringLiteral("direct_command")] = m_spiDirectEdit->text().trimmed();
    if (!m_cfgMgr->save(m_appCfg)) {
        log(QStringLiteral("保存 SPI 设置失败"), QStringLiteral("error"));
    }
}

void SpiControlPanel::saveStimulatorConfig()
{
    if (!m_cfgMgr || !m_stimBlock || !m_stimAddr || !m_stimAmp) {
        return;
    }
    m_appCfg = m_cfgMgr->load();
    ConfigSection &stim = m_appCfg[QStringLiteral("Stimulator")];
    stim[QStringLiteral("block")] = m_stimBlock->text().trimmed();
    stim[QStringLiteral("addr_channel")] = comboValuePrefix(m_stimAddr);
    stim[QStringLiteral("amplitude")] = m_stimAmp->text().trimmed();
    stim[QStringLiteral("polarity")] = comboValuePrefix(m_stimPolarity);
    stim[QStringLiteral("compensate")] = comboValuePrefix(m_stimComp);
    stim[QStringLiteral("step")] = comboValuePrefix(m_stimStep);
    stim[QStringLiteral("dac_channel")] = comboValuePrefix(m_stimDac);
    if (!m_cfgMgr->save(m_appCfg)) {
        log(QStringLiteral("保存刺激器设置失败"), QStringLiteral("error"));
    }
}

void SpiControlPanel::buildUi() {
    setObjectName(QStringLiteral("spiControlPanel"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 4, 6, 6);
    root->setSpacing(6);

    auto *controlScroll = new QScrollArea;
    controlScroll->setWidgetResizable(true);
    auto *controlWidget = new QWidget;
    auto *controlLayout = new QVBoxLayout(controlWidget);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(5);

    auto *spiGrp = new QGroupBox(QStringLiteral("直接 32-bit SPI 命令"));
    auto *spiH = new QHBoxLayout(spiGrp);
    spiH->setContentsMargins(6, 10, 6, 6);
    spiH->setSpacing(5);
    m_spiDirectEdit = new QLineEdit;
    m_spiDirectEdit->setText(m_appCfg.value(QStringLiteral("Spi")).value(QStringLiteral("direct_command")));
    m_spiDirectEdit->setPlaceholderText(QStringLiteral("例如 00011100000000000000000000000000"));
    m_spiDirectEdit->setMinimumWidth(0);
    m_spiDirectEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *btnSend = new QPushButton(QStringLiteral("发送"));
    btnSend->setProperty("variant", "primary");
    btnSend->setMinimumWidth(58);
    btnSend->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(btnSend, &QPushButton::clicked, this, &SpiControlPanel::sendDirectSpi);
    connect(m_spiDirectEdit, &QLineEdit::editingFinished, this, &SpiControlPanel::saveSpiConfig);
    spiH->addWidget(m_spiDirectEdit, 1);
    spiH->addWidget(btnSend);
    controlLayout->addWidget(spiGrp);

    auto *globGrp = new QGroupBox(QStringLiteral("全局命令"));
    auto *gg = new QGridLayout(globGrp);
    gg->setContentsMargins(6, 10, 6, 6);
    gg->setHorizontalSpacing(5);
    gg->setVerticalSpacing(5);
    const QList<QPair<QString, QString>> cmds = {
        {QStringLiteral("模拟RST"), QStringLiteral("00011100000000000000000000000000")},
        {QStringLiteral("模拟解除RST"), QStringLiteral("00100000000000000000000000000000")},
        {QStringLiteral("DAC开"), QStringLiteral("00100100000000000000000000000000")},
        {QStringLiteral("DAC关"), QStringLiteral("00110100000000000000000000000000")},
        {QStringLiteral("CBOK低"), QStringLiteral("01001000000000000000000000000000")},
        {QStringLiteral("Dummy"), QStringLiteral("00000000000000000000000000000000")},
    };
    const QStringList cmdTooltips = {
        QStringLiteral("模拟复位"),
        QStringLiteral("模拟复位关闭"),
        QStringLiteral("全局 DAC 打开"),
        QStringLiteral("全局 DAC 关闭"),
        QStringLiteral("CBOK 置低"),
        QStringLiteral("Dummy"),
    };
    for (int i = 0; i < cmds.size(); ++i) {
        auto *btn = new QPushButton(cmds[i].first);
        btn->setProperty("variant", "secondary");
        btn->setToolTip(cmdTooltips.value(i, cmds[i].first));
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(btn, &QPushButton::clicked, this, [this, i, cmds]() { singleTcp(cmds[i].second, true); });
        gg->addWidget(btn, i / 2, i % 2);
    }
    controlLayout->addWidget(globGrp);

    auto *quickGrp = new QGroupBox(QStringLiteral("快速命令"));
    auto *quickLayout = new QVBoxLayout(quickGrp);
    quickLayout->setContentsMargins(6, 10, 6, 6);
    quickLayout->setSpacing(5);
    m_quickCommandsLayout = new QGridLayout;
    m_quickCommandsLayout->setContentsMargins(0, 0, 0, 0);
    m_quickCommandsLayout->setHorizontalSpacing(5);
    m_quickCommandsLayout->setVerticalSpacing(5);
    auto *btnEditQuick = new QPushButton(QStringLiteral("编辑快速命令"));
    btnEditQuick->setProperty("variant", "secondary");
    btnEditQuick->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(btnEditQuick, &QPushButton::clicked, this, &SpiControlPanel::editQuickCommands);
    quickLayout->addLayout(m_quickCommandsLayout);
    quickLayout->addWidget(btnEditQuick);
    m_quickCommands = loadQuickCommands();
    rebuildQuickCommands();
    controlLayout->addWidget(quickGrp);

    const ConfigSection stimCfg = m_appCfg.value(QStringLiteral("Stimulator"));
    auto *stimGrp = new QGroupBox(QStringLiteral("刺激器"));
    auto *sf = new QGridLayout(stimGrp);
    sf->setContentsMargins(6, 10, 6, 6);
    sf->setHorizontalSpacing(5);
    sf->setVerticalSpacing(5);

    m_stimBlock = new QLineEdit(stimCfg.value(QStringLiteral("block"), QStringLiteral("00000000")));
    m_stimAddr = new QComboBox;
    m_stimAddr->addItems({QStringLiteral("00"), QStringLiteral("01"), QStringLiteral("10"), QStringLiteral("11")});
    setComboByValuePrefix(m_stimAddr, stimCfg.value(QStringLiteral("addr_channel"), QStringLiteral("00")));
    m_stimAmp = new QLineEdit(stimCfg.value(QStringLiteral("amplitude"), QStringLiteral("000000000")));
    m_stimPolarity = new QComboBox;
    m_stimPolarity->addItems({QStringLiteral("00 (输出0)"), QStringLiteral("01 (负向)"), QStringLiteral("10 (正向)"), QStringLiteral("11 (无)")});
    setComboByValuePrefix(m_stimPolarity, stimCfg.value(QStringLiteral("polarity"), QStringLiteral("00 (Output 0)")));
    m_stimDac = new QComboBox;
    m_stimDac->addItems({QStringLiteral("00"), QStringLiteral("01"), QStringLiteral("10"), QStringLiteral("11")});
    setComboByValuePrefix(m_stimDac, stimCfg.value(QStringLiteral("dac_channel"), QStringLiteral("00")));
    m_stimComp = new QComboBox;
    m_stimComp->addItems({QStringLiteral("1 (使能)"), QStringLiteral("0 (关闭)")});
    setComboByValuePrefix(m_stimComp, stimCfg.value(QStringLiteral("compensate"), QStringLiteral("0 (Disable)")));
    m_stimStep = new QComboBox;
    m_stimStep->addItems({QStringLiteral("1 (200nA)"), QStringLiteral("0 (4nA)")});
    setComboByValuePrefix(m_stimStep, stimCfg.value(QStringLiteral("step"), QStringLiteral("0 (4nA)")));

    connect(m_stimBlock, &QLineEdit::editingFinished, this, &SpiControlPanel::scheduleSaveStimulatorConfig);
    connect(m_stimAmp, &QLineEdit::editingFinished, this, &SpiControlPanel::scheduleSaveStimulatorConfig);
    connect(m_stimAddr, &QComboBox::currentIndexChanged, this, [this](int) { scheduleSaveStimulatorConfig(); });
    connect(m_stimPolarity, &QComboBox::currentIndexChanged, this, [this](int) { scheduleSaveStimulatorConfig(); });
    connect(m_stimDac, &QComboBox::currentIndexChanged, this, [this](int) { scheduleSaveStimulatorConfig(); });
    connect(m_stimComp, &QComboBox::currentIndexChanged, this, [this](int) { scheduleSaveStimulatorConfig(); });
    connect(m_stimStep, &QComboBox::currentIndexChanged, this, [this](int) { scheduleSaveStimulatorConfig(); });

    sf->addWidget(new QLabel(QStringLiteral("block")), 0, 0);
    sf->addWidget(m_stimBlock, 0, 1);
    sf->addWidget(new QLabel(QStringLiteral("adder")), 0, 2);
    sf->addWidget(m_stimAddr, 0, 3);
    sf->addWidget(new QLabel(QStringLiteral("幅度")), 1, 0);
    sf->addWidget(m_stimAmp, 1, 1, 1, 3);
    sf->addWidget(new QLabel(QStringLiteral("极性")), 2, 0);
    sf->addWidget(m_stimPolarity, 2, 1);
    sf->addWidget(new QLabel(QStringLiteral("DAC")), 2, 2);
    sf->addWidget(m_stimDac, 2, 3);
    sf->addWidget(new QLabel(QStringLiteral("补偿")), 3, 0);
    sf->addWidget(m_stimComp, 3, 1);
    sf->addWidget(new QLabel(QStringLiteral("步进")), 3, 2);
    sf->addWidget(m_stimStep, 3, 3);

    auto *row = new QHBoxLayout;
    row->setSpacing(5);
    auto *btnOut = new QPushButton(QStringLiteral("输出"));
    auto *btnOff = new QPushButton(QStringLiteral("关闭"));
    btnOut->setProperty("variant", "primary");
    btnOff->setProperty("variant", "danger");
    connect(btnOut, &QPushButton::clicked, this, [this]() { stimSend(QStringLiteral("0")); });
    connect(btnOff, &QPushButton::clicked, this, [this]() { stimSend(QStringLiteral("1")); });
    row->addWidget(btnOut);
    row->addWidget(btnOff);
    sf->addLayout(row, 4, 0, 1, 4);
    controlLayout->addWidget(stimGrp);

    controlLayout->addStretch(1);
    controlScroll->setWidget(controlWidget);

    auto *consoleGrp = new QGroupBox(QStringLiteral("控制台输出"));
    auto *consoleLayout = new QVBoxLayout(consoleGrp);
    consoleLayout->setContentsMargins(6, 10, 6, 6);
    consoleLayout->setSpacing(5);
    m_console = new QPlainTextEdit;
    m_console->setReadOnly(true);
    m_console->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_console->setMaximumBlockCount(400);
    m_console->setMinimumHeight(260);
    auto *btnClear = new QPushButton(QStringLiteral("清空"));
    btnClear->setProperty("variant", "secondary");
    connect(btnClear, &QPushButton::clicked, m_console, &QPlainTextEdit::clear);
    consoleLayout->addWidget(m_console, 1);
    consoleLayout->addWidget(btnClear);

    root->addWidget(controlScroll, 4);
    root->addWidget(consoleGrp, 1);
}

}  // namespace ccv2
