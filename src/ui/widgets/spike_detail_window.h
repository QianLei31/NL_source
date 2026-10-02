#pragma once

#include <QDialog>
#include <QMap>
#include <QString>
#include <QVector>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace ccv2 {

class SpikeGridView;
class SpikeSnippetStore;

// Non-modal, reusable single-channel spike inspector. It shares the panel's
// snippet store and therefore adds no receiver, decoder, or detection thread.
class SpikeDetailWindow : public QDialog {
    Q_OBJECT
public:
    explicit SpikeDetailWindow(QWidget *parent = nullptr);

    void setStore(SpikeSnippetStore *store);
    void setLane(int lane, const QString &name, bool overrideEnabled,
                 double overrideMagnitudeUv, bool negativePolarity);
    int lane() const { return m_lane; }

    void setTimebase(double sampleRate, int preSamples);
    void setDisplayOptions(double yFullScaleUv, bool autoScale, bool fade,
                           int overlayCount);
    void setWaveTheme(const QMap<QString, QString> &palette);
    void updateData(const QVector<qint64> &totals,
                    const QVector<double> &thresholdsV,
                    const QVector<double> &rmsV,
                    const QVector<double> &ratesHz);
    void pullNewSnippets();
    void clearTraces();

signals:
    void thresholdOverrideChanged(int lane, bool enabled, double magnitudeUv);
    void displayOptionsChanged(double yFullScaleUv, bool autoScale, bool fade,
                               int overlayCount);

private:
    void emitThresholdOverride();
    void updateThresholdEditingState();

    SpikeGridView *m_view = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_stats = nullptr;
    QLabel *m_hint = nullptr;
    QCheckBox *m_overrideCheck = nullptr;
    QDoubleSpinBox *m_thresholdSpin = nullptr;
    QDoubleSpinBox *m_yScaleSpin = nullptr;
    QCheckBox *m_autoScaleCheck = nullptr;
    QCheckBox *m_fadeCheck = nullptr;
    QSpinBox *m_overlaySpin = nullptr;

    int m_lane = -1;
    bool m_negativePolarity = true;
    bool m_updating = false;
    QVector<double> m_thresholds;
};

}  // namespace ccv2
