#pragma once

#include "io/spike_event_archive.h"
#include <QColor>
#include <QMap>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
namespace ccv2 {

// Compact request-only controls, designed for the existing Spike page. No
// receiver, recorder, worker, timer, or implicit capture is owned here.
class SpikeAnalysisControls : public QWidget {
    Q_OBJECT
public:
    explicit SpikeAnalysisControls(QWidget *parent = nullptr);
    void setWaveTheme(const QMap<QString, QString> &palette);
    void setArchiveStatus(const SpikeArchiveStatus &status, const QString &directory = {});
    void setOfflineProgress(quint64 completed, quint64 total,
                            const QString &status, bool cancellable, bool finalizing = false);
    void setArchiveDestination(const QString &directory);
    void setOfflinePaths(const QString &recordingDirectory, const QString &outputDirectory);

signals:
    void startArchiveRequested(const QString &directory);
    void stopArchiveRequested();
    void openArchiveRequested(const QString &directory);
    void analyzeRecordingRequested(const QString &recordingDirectory, const QString &outputDirectory);
    void cancelAnalysisRequested();
    void ruleEditorRequested();

protected:
    void paintEvent(QPaintEvent *) override;
private:
    void updateAvailability();
    bool m_archiveRunning = false;
    bool m_archiveCanStop = false;
    bool m_offlineRunning = false;
    bool m_offlineCancellable = false;
    QString m_reportedArchiveDirectory;
    QLineEdit *m_archivePath = nullptr;
    QLineEdit *m_recordingPath = nullptr;
    QLineEdit *m_outputPath = nullptr;
    QPushButton *m_chooseDestination = nullptr;
    QPushButton *m_chooseRecording = nullptr;
    QPushButton *m_chooseOutput = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_analyze = nullptr;
    QPushButton *m_cancel = nullptr;
    QLabel *m_archiveStatus = nullptr;
    QLabel *m_offlineStatus = nullptr;
    QProgressBar *m_progress = nullptr;
    QColor m_windowBg{"#15171A"};
};
} // namespace ccv2
