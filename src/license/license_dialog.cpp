#include "license/license_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "license/device_fingerprint.h"
#include "license/license_client.h"

namespace ccv2 {

LicenseDialog::LicenseDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("NL CommandCenter V6 License"));
    setModal(true);
    resize(620, 280);

    auto *root = new QVBoxLayout(this);

    auto *title = new QLabel(QStringLiteral("请输入授权 License Key"));
    title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    root->addWidget(title);

    auto *hint = new QLabel(QStringLiteral("首次使用需要联网校验。授权成功后，服务器不可用时可离线宽限 %1 天。")
                                .arg(LicenseClient::offlineGraceDays()));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *form = new QFormLayout;
    m_keyEdit = new QLineEdit;
    m_keyEdit->setPlaceholderText(QStringLiteral("粘贴你收到的 license key"));
    m_keyEdit->setMinimumWidth(460);
    form->addRow(QStringLiteral("License Key"), m_keyEdit);
    root->addLayout(form);

    const QString deviceId = deviceFingerprint();
    auto *deviceRow = new QHBoxLayout;
    auto *deviceLabel = new QLabel(QStringLiteral("本机指纹: %1").arg(deviceId));
    deviceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *copyButton = new QPushButton(QStringLiteral("复制"));
    connect(copyButton, &QPushButton::clicked, this, [deviceId]() {
        QApplication::clipboard()->setText(deviceId);
    });
    deviceRow->addWidget(deviceLabel, 1);
    deviceRow->addWidget(copyButton);
    root->addLayout(deviceRow);

    m_statusLabel = new QLabel;
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    auto *buttons = new QDialogButtonBox;
    m_verifyButton = buttons->addButton(QStringLiteral("校验并进入"), QDialogButtonBox::AcceptRole);
    auto *cancelButton = buttons->addButton(QStringLiteral("退出"), QDialogButtonBox::RejectRole);
    connect(m_verifyButton, &QPushButton::clicked, this, &LicenseDialog::verifyAndAccept);
    connect(cancelButton, &QPushButton::clicked, this, &LicenseDialog::reject);
    root->addWidget(buttons);

    LicenseClient client;
    m_keyEdit->setText(client.cachedLicenseKey());
}

void LicenseDialog::verifyAndAccept() {
    m_verifyButton->setEnabled(false);
    updateStatus(QStringLiteral("正在校验..."), false);

    LicenseClient client(this);
    const LicenseResult result = client.verify(m_keyEdit->text(), this);
    if (result.allowed) {
        accept();
        return;
    }

    updateStatus(QStringLiteral("授权失败: %1").arg(result.reason), true);
    m_verifyButton->setEnabled(true);
}

void LicenseDialog::updateStatus(const QString &text, bool error) {
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QStringLiteral("color: #2e7d32;"));
}

}  // namespace ccv2
