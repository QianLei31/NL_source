#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace ccv2 {

class LicenseDialog : public QDialog {
    Q_OBJECT

public:
    explicit LicenseDialog(QWidget *parent = nullptr);

private:
    void verifyAndAccept();
    void updateStatus(const QString &text, bool error);

    QLineEdit *m_keyEdit{nullptr};
    QLabel *m_statusLabel{nullptr};
    QPushButton *m_verifyButton{nullptr};
};

}  // namespace ccv2
