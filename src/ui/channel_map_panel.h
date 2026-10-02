#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <QVariantMap>

#include "config/config_manager.h"
#include "core/channel_address_state.h"
#include "core/channel_config_model.h"
#include "core/network_state.h"

class QResizeEvent;
class QSplitter;
class QVBoxLayout;

namespace ccv2 {

class ElectrodeMapView;

class ChannelMapPanel : public QWidget {
    Q_OBJECT

public:
    explicit ChannelMapPanel(ChannelAddressState *channelState,
                             ConfigManager *cfgMgr = nullptr,
                             NetworkState *networkState = nullptr,
                             QWidget *parent = nullptr);
    void shutdown();
    void setMapTheme(const QMap<QString, QString> &palette);

public slots:
    void applySpiShadowCommand(const QString &commandBits);
    // Mirrors SpiControlPanel::busyChanged: batch-write buttons are disabled
    // while the serial SPI queue is executing.
    void setSpiBusy(bool busy);

protected:
    void resizeEvent(QResizeEvent *event) override;

signals:
    // Batch channel-config -> 32-bit SPI command strings for the SPI panel.
    void spiBatchRequested(const QString &label, const QStringList &commands, int delayUs);

private slots:
    void onZoomChanged(int zoomPercent);
    void onBatchModifySelect();
    void onBatchModifyUnselect();
    void onSelectAll();
    void onApplyGlobalChannels();
    void syncUiFromState();

private:
    QSet<int> parseIntRanges(const QString &text, int minimum, int maximum, bool *ok, QString *error) const;
    void onMapPairsChanged(const QSet<QPair<int, int>> &pairs);
    void onElectrodeClicked(const QVariantMap &info);
    void batchModify(bool makeSelected);
    void loadPanelConfig();
    void savePanelConfig() const;
    void scheduleSavePanelConfig();
    void buildBatchConfigGroup(QVBoxLayout *rightLayout);
    void applyRecSection();
    void applyDacSection();
    void applyCtSection();
    void applyAllSections();
    // Read-modify-write: start from the channel's own shadow register and apply
    // only the fields the UI shows a definite value for. A field left in the
    // "mixed" state (kMixedCode) keeps its per-channel value instead of being
    // overwritten with an invented one.
    quint16 recValueFor(int idx) const;
    quint16 dacValueFor(int idx) const;
    quint16 ctValueFor(int idx) const;
    int cfgDelayUs() const;
    void updateCfgPreviews();
    // Pull the selected channels' shadow registers back into the controls.
    void syncCfgUiFromShadow();
    // Coalesced variant for the per-command spiCommandApplied feedback path:
    // a 1536-command batch must not rebuild the controls 1536 times.
    void scheduleSyncCfgUiFromShadow();
    // Persist the shadow registers ("what this GUI last successfully sent")
    // so the next session's read-modify-write starts from the right baseline.
    void saveShadowConfig() const;
    void scheduleSaveShadowConfig();
    void refreshBatchSelectionLabel();
    // Append a "•" to a register tab whose fields disagree across the current
    // selection (some control shows the mixed sentinel).
    void updateCfgTabIndicators();

    ChannelAddressState *m_channelState;
    ChannelConfigModel m_config;
    QLabel *m_cfgSelLabel{nullptr};
    QSpinBox *m_cfgDelaySpin{nullptr};
    QTabWidget *m_cfgTabs{nullptr};
    QPushButton *m_btnWriteCurrent{nullptr};
    QPushButton *m_btnWriteAll{nullptr};
    // Controls the operator changed since the last write/selection change.
    // The async applied-command feedback must not drag these back to the
    // shadow value while the operator is still editing (see readBackCombo).
    QSet<const QObject *> m_cfgDirtyControls;

    // Batch-config controls. Combo item userData = register field code.
    QComboBox *m_recGain{nullptr};
    QComboBox *m_recHighpass{nullptr};
    QComboBox *m_recReference{nullptr};
    QComboBox *m_recLowLp{nullptr};
    QComboBox *m_recHighPower{nullptr};
    QComboBox *m_recTrimImp{nullptr};
    QComboBox *m_recOff{nullptr};
    QComboBox *m_recRstN{nullptr};
    QLabel *m_recPreview{nullptr};

    QSpinBox *m_dacAmpSpin{nullptr};
    QComboBox *m_dacPolarity{nullptr};
    QComboBox *m_dacStep{nullptr};
    QComboBox *m_dacComp{nullptr};
    QComboBox *m_dacOffStim{nullptr};
    QComboBox *m_dacElectrode{nullptr};
    QLabel *m_dacCurrentHint{nullptr};
    QLabel *m_dacPreview{nullptr};

    QComboBox *m_ctTimePos{nullptr};
    QComboBox *m_ctTimeNeg{nullptr};
    QComboBox *m_ctGlobalFreq{nullptr};
    QComboBox *m_ctLocalFreq{nullptr};
    QComboBox *m_ctPolFirst{nullptr};
    QComboBox *m_ctAmpX20{nullptr};
    QComboBox *m_ctOnOff{nullptr};
    QLabel *m_ctPreview{nullptr};
    ConfigManager *m_cfgMgr{nullptr};
    NetworkState *m_networkState{nullptr};
    bool m_loadingConfig{false};
    QTimer m_saveDebounceTimer;
    QTimer m_shadowSaveTimer;
    QTimer m_cfgSyncTimer;
    ElectrodeMapView *m_mapView{nullptr};
    QSplitter *m_splitter{nullptr};
    QCheckBox *m_routedOnlyCheck{nullptr};

    QLineEdit *m_blockRangeEdit{nullptr};
    QVector<QCheckBox *> m_localChecks;
    QLineEdit *m_globalEdit{nullptr};
    QLabel *m_zoomLabel{nullptr};
    QSlider *m_zoomSlider{nullptr};

    QLabel *m_blockLabel{nullptr};
    QLabel *m_localLabel{nullptr};
    QLabel *m_localEleLabel{nullptr};
    QLabel *m_globalLabel{nullptr};
    QLabel *m_electrodeLabel{nullptr};
    QLabel *m_spiBitsLabel{nullptr};
    QListWidget *m_historyList{nullptr};
};

}  // namespace ccv2
