#pragma once
#include <QObject>
#include <QMutex>
#include <QTimer>
#include "src/core/system_status.h"

namespace ccv2 {

class SystemStatusModel : public QObject {
    Q_OBJECT
public:
    explicit SystemStatusModel(QObject *parent = nullptr);

    SystemStatus snapshot() const;

    // Setters — each marks dirty and schedules throttled emit
    void setControlLink(ControlLinkState state, const QString &detail = QString());
    void setAcquisition(AcquisitionState state, const QString &detail = QString());
    void setHost(const QString &host);
    void setPort(int port);
    void setDataPort(int dataPort);
    void setSamplingRate(double fs);
    void setActiveChannels(int n);
    void setPacketLossRate(double rate);
    void setRecording(bool rec);
    void setFpgaOk(bool ok);
    void setStimEnabled(bool enabled);
    void setCompressionRatio(double ratio);
    void setAdvancedDebugUnlocked(bool unlocked);

signals:
    void statusChanged(const ccv2::SystemStatus &status);
    void controlLinkChanged(ccv2::ControlLinkState state);
    void acquisitionChanged(ccv2::AcquisitionState state);
    void recordingChanged(bool recording);

private:
    void markDirty();
    void flush();

    mutable QMutex m_mutex;
    SystemStatus m_status;
    bool m_dirty = false;
    QTimer *m_throttle = nullptr;
};

} // namespace ccv2
