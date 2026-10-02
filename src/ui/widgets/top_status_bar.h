#pragma once
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include "src/core/system_status.h"
#include "network/timestamp_checker.h"

class QPropertyAnimation;

namespace ccv2 {

class TopStatusBar : public QWidget {
    Q_OBJECT
public:
    explicit TopStatusBar(QWidget *parent = nullptr);

    void updateFromStatus(const SystemStatus &status);
    void setRecordingState(bool recording, const QString &text);
    // Always-on timestamp continuity health indicator. When tdmActive is set,
    // an odd upstream missing-frame count is flagged specially: it swaps the
    // two TDM electrodes' identities from that point on.
    void setTimestampHealth(const TimestampContinuityStats &stats,
                            bool sessionActive,
                            bool tdmActive = false);

signals:
    void settingsRequested();

private:
    QLabel *m_titleLabel = nullptr;
    QLabel *m_connBadge = nullptr;
    QLabel *m_endpoint = nullptr;
    QLabel *m_tsChip = nullptr;  // timestamp continuity health
    QWidget *m_recChip = nullptr;      // recording indicator pill (dot + time)
    QLabel *m_recDot = nullptr;        // pulsing red dot
    QLabel *m_recText = nullptr;       // "REC mm:ss"
    QPropertyAnimation *m_recPulse = nullptr;  // breathing pulse on the dot
    QPushButton *m_settingsBtn = nullptr;
};

} // namespace ccv2
