#pragma once

#include "io/spike_event_archive.h"
#include <QColor>
#include <QMap>
#include <QWidget>
#include <memory>

class QCheckBox;
class QCloseEvent;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace ccv2 {
class SpikeArchiveWaveform;

// Modeless, cursor-paged sidecar reader. Never consults a display ring or starts
// acquisition. Only a bounded page and one selected waveform are held in RAM.
class SpikeArchiveBrowser : public QWidget {
    Q_OBJECT
public:
    explicit SpikeArchiveBrowser(QWidget *parent = nullptr);
    ~SpikeArchiveBrowser() override;
    // True means the asynchronous request was accepted, not yet verified.
    bool openArchive(const QString &directory);
    QString archiveDirectory() const { return m_reader.directory(); }
    void setWaveTheme(const QMap<QString, QString> &palette);
    int visibleEventCount() const { return int(m_page.events.size()); }
    quint64 selectedEventId() const;
    bool appendSelectedLabel(int unitId, const QString &note = {});
    bool busy() const;
    void cancelPending();

signals:
    void archiveOpened(bool success);
    void annotationFinished(bool success);
    void pageReady();
    void busyChanged(bool busy);

protected:
    void paintEvent(QPaintEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    friend class SpikeArchiveWaveform;
    quint64 beginOperation(const QString &status);
    void setBusy(bool busy);
    void applyPage(SpikeArchiveCursor cursor, SpikeArchivePage page);
    void displayAnnotations(const SpikeArchiveAnnotationPage &page);
    void resetQuery();
    void showPage(SpikeArchiveCursor cursor);
    void selectEvent();
    void refreshStatus();
    void showAnnotations(qint64 offset = 32);
    SpikeArchiveQuery query() const;
    struct Async;
    std::unique_ptr<Async> m_async;
    SpikeEventArchiveReader m_reader;
    SpikeArchivePage m_page;
    SpikeArchiveQuery m_activeQuery;
    SpikeArchiveCursor m_cursor;
    QVector<SpikeArchiveCursor> m_history;
    qint64 m_annotationNext = 32;
    bool m_open = false;
    QLineEdit *m_path = nullptr;
    QSpinBox *m_electrode = nullptr;
    QCheckBox *m_epochEnabled = nullptr;
    QLineEdit *m_epoch = nullptr;
    QDoubleSpinBox *m_from = nullptr;
    QDoubleSpinBox *m_to = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_paging = nullptr;
    QLabel *m_annotation = nullptr;
    QLabel *m_feedback = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_moreAnnotations = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_label = nullptr;
    QPushButton *m_unlabel = nullptr;
    QSpinBox *m_unit = nullptr;
    QLineEdit *m_note = nullptr;
    SpikeArchiveWaveform *m_waveform = nullptr;
    QColor m_windowBg{"#15171A"}, m_plotBg{"#131518"}, m_border{"#31363D"};
    QColor m_text{"#E8EAED"}, m_secondary{"#A8B0BA"}, m_axis{"#8A929C"};
    QColor m_wave{"#4FB6C4"}, m_accent{"#4D8FE8"};
};
} // namespace ccv2
