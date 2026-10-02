#pragma once
#include <QWidget>

#include "core/system_status.h"
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QVector>

namespace ccv2 {

struct UpdateCheckResult;

class SettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget *parent = nullptr);

    QString host() const;
    int spiPort() const;
    int dataPort() const;
    QString theme() const;
    bool managedTestSource() const;
    bool spikeWaveform() const;
    void setEndpoint(const QString &host, int spiPort, int dataPort);
    void setTheme(const QString &theme);
    void setLocalTestEnabled(bool enabled);
    void setManagedTestSource(bool managed);
    void setSpikeWaveform(bool spike);
    void setLocalTestPorts(int controlPort, int dataPort);
    void setUpdateChecking();
    void setUpdateResult(const UpdateCheckResult &result);
    void setConnectionChecking();
    void setConnectionProbeResult(bool reachable, const QString &message);
    void setLinkStates(ControlLinkState control,
                       AcquisitionState acquisition,
                       const QString &controlDetail = QString(),
                       const QString &acquisitionDetail = QString());

    // Recording / storage
    QString saveDir() const;
    QString sessionName() const;
    qint64 splitBytes() const;
    void setSaveDir(const QString &dir);
    void setRecordingStatus(bool recording, const QString &text);

signals:
    void applied(const QString &host, int spiPort, int dataPort, const QString &theme);
    void connectRequested(const QString &host, int spiPort, int dataPort);
    void localTestToggled(bool enabled, int spiPort, int dataPort, bool managedSource);
    void themeSelected(const QString &theme);
    void updateCheckRequested();

private:
    QString effectiveHost() const;
    int effectiveSpiPort() const;
    int effectiveDataPort() const;
    void refreshEndpointHint();
    void refreshThemeButtons();

    QCheckBox *m_localTestCheck = nullptr;
    QComboBox *m_testSourceCombo = nullptr;
    QLabel *m_localModeBadge = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_spiPortSpin = nullptr;
    QSpinBox *m_dataPortSpin = nullptr;
    QSpinBox *m_dummySpiPortSpin = nullptr;
    QSpinBox *m_dummyDataPortSpin = nullptr;
    QComboBox *m_themeCombo = nullptr;
    QVector<QPushButton *> m_themeButtons;
    QPushButton *m_applyBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    ControlLinkState m_controlLink{ControlLinkState::Unknown};
    AcquisitionState m_acquisition{AcquisitionState::Idle};
    QString m_controlDetail;
    QString m_acquisitionDetail;
    QLabel *m_versionLabel = nullptr;
    QLabel *m_updateStatusLabel = nullptr;
    QPushButton *m_checkUpdateBtn = nullptr;
    QPushButton *m_openUpdateBtn = nullptr;
    QString m_updateUrl;

    // Recording / storage
    QLineEdit *m_saveDirEdit = nullptr;
    QPushButton *m_browseDirBtn = nullptr;
    QLineEdit *m_sessionNameEdit = nullptr;
    QComboBox *m_splitCombo = nullptr;
    QLabel *m_freeSpaceLabel = nullptr;
    QPushButton *m_recordBtn = nullptr;
    QLabel *m_recordStatusLabel = nullptr;
    bool m_recording = false;
    void refreshFreeSpace();
};

} // namespace ccv2
