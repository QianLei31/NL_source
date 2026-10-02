#pragma once
#include <QFrame>
#include <QLabel>

namespace ccv2 {

class MetricCard : public QFrame {
    Q_OBJECT
public:
    enum class Severity { Normal, Warning, Error };

    explicit MetricCard(const QString &label, const QString &unit, QWidget *parent = nullptr);

    void setValue(double v, int precision = 2);
    void setValueText(const QString &text);
    void setSeverity(Severity s);
    void setBadge(const QString &text);
    QString valueText() const;

private:
    QLabel *m_label = nullptr;
    QLabel *m_value = nullptr;
    QLabel *m_unit = nullptr;
    QLabel *m_badge = nullptr;
};

} // namespace ccv2
