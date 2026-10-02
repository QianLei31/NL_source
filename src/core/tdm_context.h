#pragma once

#include <QObject>

#include <atomic>

namespace ccv2 {

// Application-wide source of truth for the hardware TDM state. A TDM ADC
// stream contains four phases (local ELE 0..3); pages display either pair 0/2
// or pair 1/3. Pages bind their controls here instead of keeping
// private copies, so displays, trigger logic and export defaults can never
// disagree about electrode identity. Getters are lock-free for the
// distributor/sorter threads; setters must run on the GUI thread.
class TdmContext : public QObject {
    Q_OBJECT

public:
    explicit TdmContext(QObject *parent = nullptr);

    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
    bool pair02() const { return m_pair02.load(std::memory_order_relaxed); }
    bool evenFirst() const { return pair02(); } // legacy manifest/API alias

public slots:
    void setEnabled(bool enabled);
    void setPair02(bool pair02);
    void setEvenFirst(bool evenFirst);
    void setState(bool enabled, bool pair02);

signals:
    void changed(bool enabled, bool pair02);

private:
    std::atomic_bool m_enabled{false};
    std::atomic_bool m_pair02{true};
};

}  // namespace ccv2
