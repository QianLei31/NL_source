#pragma once

#include <QFuture>
#include <QString>
#include <QWidget>

#include <atomic>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace ccv2 {

class RecordingPanel : public QWidget {
    Q_OBJECT
public:
    explicit RecordingPanel(QWidget *parent = nullptr);
    ~RecordingPanel() override;

    QString saveDir() const;
    QString sessionName() const;
    qint64 splitBytes() const;
    void setSaveDir(const QString &dir);
    void setSessionName(const QString &name);
    void setSplitBytes(qint64 bytes);
    void setRecordingStatus(bool recording, const QString &text);

    // Threshold-triggered recording settings.
    bool triggerEnabled() const;
    int triggerChannel() const;
    double triggerThresholdVolts() const;   // mV UI -> volts
    bool triggerRisingAbove() const;
    double triggerAutoStopSeconds() const;   // 0 = manual stop
    int triggerTdmSlot() const;              // -1 = both/off, 0/1 = electrode
    void setTriggerSettings(bool enabled, int channel, double thresholdMv,
                            bool risingAbove, double autoStopSeconds);
    void setTriggerTdmSlot(int slot);
    // Show/hide the TDM electrode selector to match the global TDM state.
    void setTdmMode(bool enabled, bool pair02 = true);

signals:
    void settingsChanged();
    void triggerSettingsChanged();

private:
    void refreshFreeSpace();
    void loadBinForExport();
    void exportLoadedBinToMatlab();

    QLineEdit *m_saveDirEdit{nullptr};
    QPushButton *m_browseDirBtn{nullptr};
    QLineEdit *m_sessionNameEdit{nullptr};
    QComboBox *m_splitCombo{nullptr};
    QLabel *m_freeSpaceLabel{nullptr};
    QLabel *m_recordStatusLabel{nullptr};

    QCheckBox *m_trigEnable{nullptr};
    QSpinBox *m_trigChannel{nullptr};
    QDoubleSpinBox *m_trigThreshold{nullptr};  // mV
    QComboBox *m_trigPolarity{nullptr};
    QDoubleSpinBox *m_trigAutoStop{nullptr};   // seconds
    QComboBox *m_trigTdmSlot{nullptr};         // TDM electrode select
    QWidget *m_trigTdmRow{nullptr};            // hidden unless TDM is active

    // Offline MATLAB export (moved here from the analyzer page).
    QPushButton *m_loadBinBtn{nullptr};
    QPushButton *m_exportBtn{nullptr};
    QDoubleSpinBox *m_exportFsSpin{nullptr};
    QCheckBox *m_exportTdmCheck{nullptr};
    QCheckBox *m_exportRaw32Check{nullptr};
    QComboBox *m_exportTdmPhase{nullptr};
    QLabel *m_exportFileLabel{nullptr};
    QLabel *m_exportStatusLabel{nullptr};
    QString m_loadedBin;
    std::atomic_bool m_exportBusy{false};
    std::atomic_bool m_exportCancel{false};
    std::atomic_bool m_shuttingDown{false};
    QFuture<void> m_exportFuture;
};

}  // namespace ccv2
