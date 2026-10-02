#include "system_status_model.h"
#include <QMutexLocker>

namespace ccv2 {

SystemStatusModel::SystemStatusModel(QObject *parent)
    : QObject(parent)
{
    m_throttle = new QTimer(this);
    m_throttle->setSingleShot(true);
    m_throttle->setTimerType(Qt::PreciseTimer);
    m_throttle->setInterval(150);
    connect(m_throttle, &QTimer::timeout, this, &SystemStatusModel::flush);
}

SystemStatus SystemStatusModel::snapshot() const
{
    QMutexLocker lock(&m_mutex);
    return m_status;
}

void SystemStatusModel::setControlLink(ControlLinkState state, const QString &detail)
{
    QMutexLocker lock(&m_mutex);
    if (m_status.controlLink != state || m_status.controlDetail != detail) {
        m_status.controlLink = state;
        m_status.controlDetail = detail;
        lock.unlock();
        emit controlLinkChanged(state);
        markDirty();
    }
}

void SystemStatusModel::setAcquisition(AcquisitionState state, const QString &detail)
{
    QMutexLocker lock(&m_mutex);
    if (m_status.acquisition != state || m_status.acquisitionDetail != detail) {
        m_status.acquisition = state;
        m_status.acquisitionDetail = detail;
        lock.unlock();
        emit acquisitionChanged(state);
        markDirty();
    }
}

void SystemStatusModel::setHost(const QString &host)
{
    QMutexLocker lock(&m_mutex);
    m_status.host = host;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setPort(int port)
{
    QMutexLocker lock(&m_mutex);
    m_status.port = port;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setDataPort(int dataPort)
{
    QMutexLocker lock(&m_mutex);
    m_status.dataPort = dataPort;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setSamplingRate(double fs)
{
    QMutexLocker lock(&m_mutex);
    m_status.samplingRate = fs;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setActiveChannels(int n)
{
    QMutexLocker lock(&m_mutex);
    m_status.activeChannels = n;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setPacketLossRate(double rate)
{
    QMutexLocker lock(&m_mutex);
    m_status.packetLossRate = rate;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setRecording(bool rec)
{
    QMutexLocker lock(&m_mutex);
    if (m_status.recording != rec) {
        m_status.recording = rec;
        lock.unlock();
        emit recordingChanged(rec);
        markDirty();
    }
}

void SystemStatusModel::setFpgaOk(bool ok)
{
    QMutexLocker lock(&m_mutex);
    m_status.fpgaOk = ok;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setStimEnabled(bool enabled)
{
    QMutexLocker lock(&m_mutex);
    m_status.stimEnabled = enabled;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setCompressionRatio(double ratio)
{
    QMutexLocker lock(&m_mutex);
    m_status.compressionRatio = ratio;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::setAdvancedDebugUnlocked(bool unlocked)
{
    QMutexLocker lock(&m_mutex);
    m_status.advancedDebugUnlocked = unlocked;
    lock.unlock();
    markDirty();
}

void SystemStatusModel::markDirty()
{
    m_dirty = true;
    if (!m_throttle->isActive()) {
        m_throttle->start();
    }
}

void SystemStatusModel::flush()
{
    if (m_dirty) {
        m_dirty = false;
        QMutexLocker lock(&m_mutex);
        SystemStatus copy = m_status;
        lock.unlock();
        emit statusChanged(copy);
    }
}

} // namespace ccv2
