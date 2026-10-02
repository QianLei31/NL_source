#include "settings_panel.h"

#include <QDesktopServices>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStorageInfo>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "core/app_version.h"
#include "service/update_client.h"
#include "theme/theme_manager.h"

namespace ccv2 {

namespace {

struct ThemeOption {
    QString key;
    QString label;
    QString color;
};

const QVector<ThemeOption> &themeOptions()
{
    static const QVector<ThemeOption> options{
        {QStringLiteral("dark"), QStringLiteral("经典暗色"), QStringLiteral("#1B1E22")},
        {QStringLiteral("soft-dark"), QStringLiteral("柔和暗色"), QStringLiteral("#242931")},
        {QStringLiteral("light"), QStringLiteral("浅色主题"), QStringLiteral("#F8FAFC")},
        {QStringLiteral("midnight-indigo"), QStringLiteral("靛蓝夜色"), QStringLiteral("#141B33")},
    };
    return options;
}

void addThemeOption(QComboBox *combo, const ThemeOption &option)
{
    combo->addItem(option.label, option.key);
}

QWidget *makeSectionHeader(const QString &iconText, const QString &text, QWidget *rightWidget, QWidget *parent)
{
    auto *wrap = new QWidget(parent);
    wrap->setObjectName(QStringLiteral("settingsSectionHeader"));
    auto *row = new QHBoxLayout(wrap);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(7);

    auto *icon = new QLabel(iconText, wrap);
    icon->setObjectName(QStringLiteral("settingsSectionIcon"));
    icon->setAlignment(Qt::AlignCenter);
    row->addWidget(icon);

    auto *label = new QLabel(text, wrap);
    label->setObjectName(QStringLiteral("settingsSectionLabel"));
    row->addWidget(label);

    auto *line = new QFrame(wrap);
    line->setObjectName(QStringLiteral("settingsSectionLine"));
    line->setFrameShape(QFrame::HLine);
    row->addWidget(line, 1);
    if (rightWidget) {
        row->addWidget(rightWidget);
    }
    return wrap;
}

QFrame *makeCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    return card;
}

QWidget *makeLabeledField(const QString &labelText, QWidget *editor, QWidget *parent)
{
    auto *wrap = new QWidget(parent);
    auto *layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    auto *label = new QLabel(labelText);
    label->setObjectName(QStringLiteral("settingsFieldLabel"));
    label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    editor->setObjectName(editor->objectName().isEmpty()
                              ? QStringLiteral("settingsEditor")
                              : editor->objectName());
    editor->setMinimumHeight(24);

    layout->addWidget(label);
    layout->addWidget(editor);
    return wrap;
}

void polishWidget(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

}  // namespace

SettingsPanel::SettingsPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("settingsPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setFixedWidth(360);

    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 10);
    shadow->setColor(QColor(0, 0, 0, 185));
    setGraphicsEffect(shadow);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(7);

    auto *topAccent = new QFrame(this);
    topAccent->setObjectName(QStringLiteral("settingsTopAccent"));
    topAccent->setFixedHeight(2);
    layout->addWidget(topAccent);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    auto *titleColumn = new QVBoxLayout;
    titleColumn->setContentsMargins(0, 0, 0, 0);
    titleColumn->setSpacing(1);

    auto *title = new QLabel(QStringLiteral("设置"), this);
    title->setObjectName(QStringLiteral("settingsPanelTitle"));
    titleColumn->addWidget(title);

    auto *subtitle = new QLabel(QStringLiteral("连接与外观"), this);
    subtitle->setObjectName(QStringLiteral("settingsPanelSubtitle"));
    titleColumn->addWidget(subtitle);

    header->addLayout(titleColumn);
    header->addStretch();
    layout->addLayout(header);

    m_localModeBadge = new QLabel(QStringLiteral("硬件"), this);
    m_localModeBadge->setObjectName(QStringLiteral("settingsModeBadge"));
    m_localModeBadge->setAlignment(Qt::AlignCenter);
    layout->addWidget(makeSectionHeader(QStringLiteral("⌘"), QStringLiteral("网络配置"), m_localModeBadge, this));

    auto *netCard = makeCard(this);
    auto *netGrid = new QGridLayout(netCard);
    netGrid->setContentsMargins(10, 9, 10, 9);
    netGrid->setHorizontalSpacing(7);
    netGrid->setVerticalSpacing(7);
    netGrid->setColumnStretch(0, 1);
    netGrid->setColumnStretch(1, 1);

    m_localTestCheck = new QCheckBox(QStringLiteral("本地测试模式"), this);
    m_localTestCheck->setObjectName(QStringLiteral("settingsLocalToggle"));
    m_testSourceCombo = new QComboBox(this);
    m_testSourceCombo->setObjectName(QStringLiteral("settingsTestSourceCombo"));
    m_testSourceCombo->addItem(QStringLiteral("外部 Dummy Server"), 0);
    m_testSourceCombo->addItem(QStringLiteral("程序托管 Dummy (正弦)"), 1);
    m_testSourceCombo->addItem(QStringLiteral("程序托管 Dummy (Spike)"), 2);
    m_testSourceCombo->setEnabled(false);

    m_hostEdit = new QLineEdit(QStringLiteral("192.168.3.20"), this);
    m_hostEdit->setObjectName(QStringLiteral("settingsHostEdit"));

    m_spiPortSpin = new QSpinBox(this);
    m_spiPortSpin->setRange(1, 65535);
    m_spiPortSpin->setValue(7);

    m_dataPortSpin = new QSpinBox(this);
    m_dataPortSpin->setRange(1, 65535);
    m_dataPortSpin->setValue(5001);

    m_dummySpiPortSpin = new QSpinBox(this);
    m_dummySpiPortSpin->setRange(1, 65535);
    m_dummySpiPortSpin->setValue(10086);
    m_dummySpiPortSpin->setEnabled(false);

    m_dummyDataPortSpin = new QSpinBox(this);
    m_dummyDataPortSpin->setRange(1, 65535);
    m_dummyDataPortSpin->setValue(10086);
    m_dummyDataPortSpin->setEnabled(false);

    netGrid->addWidget(m_localTestCheck, 0, 0);
    netGrid->addWidget(m_testSourceCombo, 0, 1);
    netGrid->addWidget(makeLabeledField(QStringLiteral("主机地址"), m_hostEdit, netCard), 1, 0, 1, 2);
    netGrid->addWidget(makeLabeledField(QStringLiteral("控制口"), m_spiPortSpin, netCard), 2, 0);
    netGrid->addWidget(makeLabeledField(QStringLiteral("数据口"), m_dataPortSpin, netCard), 2, 1);
    netGrid->addWidget(makeLabeledField(QStringLiteral("测试控制"), m_dummySpiPortSpin, netCard), 3, 0);
    netGrid->addWidget(makeLabeledField(QStringLiteral("测试数据"), m_dummyDataPortSpin, netCard), 3, 1);

    layout->addWidget(netCard);

    layout->addWidget(makeSectionHeader(QStringLiteral("◐"), QStringLiteral("外观主题"), nullptr, this));

    auto *themeCard = makeCard(this);
    auto *themeLayout = new QVBoxLayout(themeCard);
    themeLayout->setContentsMargins(10, 9, 10, 9);
    themeLayout->setSpacing(6);

    m_themeCombo = new QComboBox(this);
    m_themeCombo->setObjectName(QStringLiteral("settingsThemeCombo"));
    m_themeCombo->hide();
    auto *swatchRow = new QHBoxLayout;
    swatchRow->setContentsMargins(0, 0, 0, 0);
    swatchRow->setSpacing(8);
    for (const ThemeOption &option : themeOptions()) {
        addThemeOption(m_themeCombo, option);
        auto *button = new QPushButton(this);
        button->setObjectName(QStringLiteral("settingsThemeSwatch"));
        button->setProperty("themeKey", option.key);
        button->setProperty("swatchColor", option.color);
        button->setToolTip(option.label);
        button->setFixedSize(28, 28);
        button->setCheckable(true);
        button->setStyleSheet(QStringLiteral("QPushButton { background: %1; }").arg(option.color));
        connect(button, &QPushButton::clicked, this, [this, key = option.key]() {
            const int index = m_themeCombo->findData(key);
            if (index >= 0) {
                m_themeCombo->setCurrentIndex(index);
            }
            refreshThemeButtons();
            emit themeSelected(key);
        });
        m_themeButtons.push_back(button);
        swatchRow->addWidget(button);
    }
    swatchRow->addStretch(1);
    themeLayout->addLayout(swatchRow);
    auto *themeHint = new QLabel(QStringLiteral("经典暗色 · 柔和暗色 · 浅色 · 靛蓝"), this);
    themeHint->setObjectName(QStringLiteral("settingsThemeHint"));
    themeLayout->addWidget(themeHint);
    layout->addWidget(themeCard);

    layout->addWidget(makeSectionHeader(QStringLiteral("▲"), QStringLiteral("版本"), nullptr, this));

    auto *versionCard = makeCard(this);
    auto *versionLayout = new QVBoxLayout(versionCard);
    versionLayout->setContentsMargins(8, 9, 8, 7);
    versionLayout->setSpacing(6);

    auto *versionRow = new QHBoxLayout;
    versionRow->setContentsMargins(0, 0, 0, 0);
    versionRow->setSpacing(8);
    auto *versionCaption = new QLabel(QStringLiteral("当前版本"), this);
    versionCaption->setObjectName(QStringLiteral("settingsFieldLabel"));
    m_versionLabel = new QLabel(appVersionDisplay(), this);
    m_versionLabel->setObjectName(QStringLiteral("settingsVersionLabel"));
    versionRow->addWidget(versionCaption);
    versionRow->addStretch();
    versionRow->addWidget(m_versionLabel);
    versionLayout->addLayout(versionRow);

    m_updateStatusLabel = new QLabel(QStringLiteral("点击检查更新"), this);
    m_updateStatusLabel->setObjectName(QStringLiteral("settingsUpdateStatus"));
    m_updateStatusLabel->setProperty("state", "info");
    m_updateStatusLabel->setWordWrap(true);
    versionLayout->addWidget(m_updateStatusLabel);
    m_updateUrl = QString::fromLatin1(kReleasePageUrl);

    auto *updateButtons = new QHBoxLayout;
    updateButtons->setContentsMargins(0, 0, 0, 0);
    updateButtons->setSpacing(6);
    m_checkUpdateBtn = new QPushButton(QStringLiteral("检查更新"), this);
    m_checkUpdateBtn->setObjectName(QStringLiteral("settingsCheckUpdateButton"));
    m_checkUpdateBtn->setProperty("variant", "secondary");
    updateButtons->addWidget(m_checkUpdateBtn);
    m_openUpdateBtn = new QPushButton(QStringLiteral("打开下载页"), this);
    m_openUpdateBtn->setObjectName(QStringLiteral("settingsOpenUpdateButton"));
    m_openUpdateBtn->setProperty("variant", "primary");
    m_openUpdateBtn->setEnabled(true);
    updateButtons->addWidget(m_openUpdateBtn);
    versionLayout->addLayout(updateButtons);

    layout->addWidget(versionCard);

    auto *statusFrame = new QFrame(this);
    statusFrame->setObjectName(QStringLiteral("settingsEndpointFrame"));
    auto *statusLayout = new QHBoxLayout(statusFrame);
    statusLayout->setContentsMargins(8, 5, 8, 5);
    statusLayout->setSpacing(6);
    auto *statusDot = new QLabel(QStringLiteral("●"), this);
    statusDot->setObjectName(QStringLiteral("settingsEndpointDot"));
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("settingsEndpointHint"));
    m_statusLabel->setProperty("state", "info");
    statusLayout->addWidget(statusDot);
    statusLayout->addWidget(m_statusLabel, 1);
    layout->addWidget(statusFrame);

    auto *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(6);

    m_closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    m_closeBtn->setObjectName(QStringLiteral("settingsCloseButton"));
    m_closeBtn->setProperty("variant", "secondary");
    btnRow->addWidget(m_closeBtn);

    btnRow->addStretch();

    m_applyBtn = new QPushButton(QStringLiteral("保存"), this);
    m_applyBtn->setObjectName(QStringLiteral("settingsSaveButton"));
    m_applyBtn->setProperty("variant", "secondary");
    btnRow->addWidget(m_applyBtn);

    m_connectBtn = new QPushButton(QStringLiteral("连接"), this);
    m_connectBtn->setObjectName(QStringLiteral("settingsConnectButton"));
    m_connectBtn->setProperty("variant", "primary");
    btnRow->addWidget(m_connectBtn);

    layout->addLayout(btnRow);

    connect(m_localTestCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_hostEdit->setEnabled(!checked);
        m_spiPortSpin->setEnabled(!checked);
        m_dataPortSpin->setEnabled(!checked);
        m_dummySpiPortSpin->setEnabled(checked);
        m_dummyDataPortSpin->setEnabled(checked);
        m_testSourceCombo->setEnabled(checked);
        refreshEndpointHint();
    });
    connect(m_testSourceCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshEndpointHint();
    });
    connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshThemeButtons();
    });
    connect(m_dummySpiPortSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        refreshEndpointHint();
    });
    connect(m_dummyDataPortSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        refreshEndpointHint();
    });
    connect(m_hostEdit, &QLineEdit::textChanged, this, [this]() { refreshEndpointHint(); });
    connect(m_spiPortSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { refreshEndpointHint(); });
    connect(m_dataPortSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { refreshEndpointHint(); });

    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::hide);
    connect(m_checkUpdateBtn, &QPushButton::clicked, this, &SettingsPanel::updateCheckRequested);
    connect(m_openUpdateBtn, &QPushButton::clicked, this, [this]() {
        const QString target = m_updateUrl.isEmpty()
                                   ? QString::fromLatin1(kReleaseRepositoryUrl)
                                   : m_updateUrl;
        QDesktopServices::openUrl(QUrl(target));
    });

    connect(m_connectBtn, &QPushButton::clicked, this, [this]() {
        const QString host = effectiveHost();
        const int spiPort = effectiveSpiPort();
        const int dataPort = effectiveDataPort();
        emit localTestToggled(m_localTestCheck->isChecked(),
                              m_dummySpiPortSpin->value(),
                              m_dummyDataPortSpin->value(),
                              managedTestSource());
        m_statusLabel->setText(QStringLiteral("正在连接 %1:%2").arg(host).arg(spiPort));
        m_statusLabel->setProperty("state", "warn");
        polishWidget(m_statusLabel);
        emit connectRequested(host, spiPort, dataPort);
    });

    connect(m_applyBtn, &QPushButton::clicked, this, [this]() {
        // Switching out of local-test mode synchronously restores the last
        // hardware endpoint into these editors. Preserve the operator's new
        // edits before that callback, just as the Connect action does.
        const QString requestedHost = effectiveHost();
        const int requestedSpiPort = effectiveSpiPort();
        const int requestedDataPort = effectiveDataPort();
        const QString requestedTheme = theme();
        emit localTestToggled(m_localTestCheck->isChecked(),
                              m_dummySpiPortSpin->value(),
                              m_dummyDataPortSpin->value(),
                              managedTestSource());
        emit applied(requestedHost, requestedSpiPort, requestedDataPort, requestedTheme);
        hide();
    });

    refreshEndpointHint();
    refreshThemeButtons();
}

QString SettingsPanel::host() const { return effectiveHost(); }
int SettingsPanel::spiPort() const { return effectiveSpiPort(); }
int SettingsPanel::dataPort() const { return effectiveDataPort(); }
QString SettingsPanel::theme() const
{
    const QString key = m_themeCombo->currentData().toString();
    return key.isEmpty() ? ThemeManager::normalizeThemeName(m_themeCombo->currentText()) : key;
}

bool SettingsPanel::managedTestSource() const
{
    return m_testSourceCombo && m_testSourceCombo->currentData().toInt() >= 1;
}

bool SettingsPanel::spikeWaveform() const
{
    return m_testSourceCombo && m_testSourceCombo->currentData().toInt() == 2;
}

QString SettingsPanel::effectiveHost() const
{
    return (m_localTestCheck && m_localTestCheck->isChecked())
               ? QStringLiteral("127.0.0.1")
               : m_hostEdit->text().trimmed();
}

int SettingsPanel::effectiveSpiPort() const
{
    return (m_localTestCheck && m_localTestCheck->isChecked())
               ? m_dummySpiPortSpin->value()
               : m_spiPortSpin->value();
}

int SettingsPanel::effectiveDataPort() const
{
    return (m_localTestCheck && m_localTestCheck->isChecked())
               ? m_dummyDataPortSpin->value()
               : m_dataPortSpin->value();
}

void SettingsPanel::refreshEndpointHint()
{
    if (!m_statusLabel) {
        return;
    }

    const bool local = m_localTestCheck && m_localTestCheck->isChecked();
    if (m_localModeBadge) {
        m_localModeBadge->setText(local ? QStringLiteral("本地测试") : QStringLiteral("硬件"));
        m_localModeBadge->setProperty("mode", local ? QStringLiteral("local") : QStringLiteral("hardware"));
        polishWidget(m_localModeBadge);
    }
    QString controlText;
    const char *state = "info";
    switch (m_controlLink) {
    case ControlLinkState::Checking:
        controlText = QStringLiteral("控制检测中");
        state = "warn";
        break;
    case ControlLinkState::Reachable:
        controlText = QStringLiteral("控制可用");
        state = "ok";
        break;
    case ControlLinkState::Degraded:
        controlText = QStringLiteral("控制可用（回包异常）");
        state = "warn";
        break;
    case ControlLinkState::Error:
        controlText = QStringLiteral("控制异常");
        state = "error";
        break;
    default:
        controlText = QStringLiteral("控制未检测");
        break;
    }

    QString acquisitionText;
    switch (m_acquisition) {
    case AcquisitionState::Connecting:
        acquisitionText = QStringLiteral("数据连接中");
        if (qstrcmp(state, "error") != 0) state = "warn";
        break;
    case AcquisitionState::Streaming:
        acquisitionText = QStringLiteral("数据接收中");
        break;
    case AcquisitionState::Stalled:
        acquisitionText = QStringLiteral("数据停滞");
        state = "error";
        break;
    case AcquisitionState::Error:
        acquisitionText = QStringLiteral("采集异常");
        state = "error";
        break;
    default:
        acquisitionText = QStringLiteral("采集待机");
        break;
    }

    m_statusLabel->setText(QStringLiteral("%1 · %2").arg(controlText, acquisitionText));
    const QString endpoint =
        QStringLiteral("%1 · %2 · 控制 %3 · 数据 %4\n%5\n%6")
            .arg(local
                     ? (managedTestSource() ? QStringLiteral("程序托管 Dummy")
                                            : QStringLiteral("外部 Dummy"))
                     : QStringLiteral("硬件"))
            .arg(effectiveHost().isEmpty() ? QStringLiteral("127.0.0.1") : effectiveHost())
            .arg(effectiveSpiPort())
            .arg(effectiveDataPort())
            .arg(m_controlDetail.isEmpty() ? controlText : m_controlDetail)
            .arg(m_acquisitionDetail.isEmpty() ? acquisitionText : m_acquisitionDetail);
    m_statusLabel->setToolTip(endpoint);
    m_statusLabel->setProperty("state", state);
    polishWidget(m_statusLabel);
}

void SettingsPanel::setLinkStates(ControlLinkState control,
                                  AcquisitionState acquisition,
                                  const QString &controlDetail,
                                  const QString &acquisitionDetail)
{
    m_controlLink = control;
    m_acquisition = acquisition;
    m_controlDetail = controlDetail;
    m_acquisitionDetail = acquisitionDetail;
    if (m_connectBtn) {
        m_connectBtn->setEnabled(acquisition != AcquisitionState::Connecting);
    }
    refreshEndpointHint();
}

void SettingsPanel::refreshThemeButtons()
{
    if (!m_themeCombo) {
        return;
    }
    const QString current = m_themeCombo->currentData().toString();
    for (QPushButton *button : m_themeButtons) {
        const bool selected = button && button->property("themeKey").toString() == current;
        button->setChecked(selected);
        button->setProperty("selected", selected);
        if (button) {
            const QString color = button->property("swatchColor").toString();
            button->setStyleSheet(QStringLiteral(
                "QPushButton { background: %1; border: %2px solid %3; border-radius: 6px; }"
                "QPushButton:hover { border-color: #8fb8ff; }")
                                      .arg(color)
                                      .arg(selected ? 2 : 1)
                                      .arg(selected ? QStringLiteral("#ffffff") : QStringLiteral("#334155")));
            polishWidget(button);
        }
    }
}

void SettingsPanel::setEndpoint(const QString &host, int spiPort, int dataPort)
{
    if (m_localTestCheck && m_localTestCheck->isChecked() && host == QStringLiteral("127.0.0.1")) {
        const QSignalBlocker spiBlocker(m_dummySpiPortSpin);
        const QSignalBlocker dataBlocker(m_dummyDataPortSpin);
        m_dummySpiPortSpin->setValue(spiPort);
        m_dummyDataPortSpin->setValue(dataPort);
    } else {
        const QSignalBlocker hostBlocker(m_hostEdit);
        const QSignalBlocker spiBlocker(m_spiPortSpin);
        const QSignalBlocker dataBlocker(m_dataPortSpin);
        m_hostEdit->setText(host);
        m_spiPortSpin->setValue(spiPort);
        m_dataPortSpin->setValue(dataPort);
    }
    refreshEndpointHint();
}

void SettingsPanel::setConnectionChecking()
{
    if (m_connectBtn) m_connectBtn->setEnabled(false);
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("正在建立数据连接…"));
        m_statusLabel->setProperty("state", "warn");
        polishWidget(m_statusLabel);
    }
}

void SettingsPanel::setConnectionProbeResult(bool reachable, const QString &message)
{
    if (m_connectBtn) m_connectBtn->setEnabled(true);
    if (m_statusLabel) {
        m_statusLabel->setText(message);
        m_statusLabel->setProperty("state", reachable ? "ok" : "error");
        polishWidget(m_statusLabel);
    }
}

void SettingsPanel::setTheme(const QString &theme)
{
    const int index = m_themeCombo->findData(ThemeManager::normalizeThemeName(theme));
    if (index >= 0) {
        m_themeCombo->setCurrentIndex(index);
    }
    refreshThemeButtons();
}

void SettingsPanel::setLocalTestEnabled(bool enabled)
{
    if (m_localTestCheck && m_localTestCheck->isChecked() != enabled) {
        m_localTestCheck->setChecked(enabled);
    }
}

void SettingsPanel::setManagedTestSource(bool managed)
{
    if (!m_testSourceCombo) {
        return;
    }
    // Preserve an existing spike selection when re-asserting managed mode.
    if (managed && m_testSourceCombo->currentData().toInt() == 2) {
        return;
    }
    const int index = m_testSourceCombo->findData(managed ? 1 : 0);
    if (index >= 0) {
        m_testSourceCombo->setCurrentIndex(index);
    }
}

void SettingsPanel::setSpikeWaveform(bool spike)
{
    if (!m_testSourceCombo) {
        return;
    }
    const int cur = m_testSourceCombo->currentData().toInt();
    if (spike) {
        const int index = m_testSourceCombo->findData(2);
        if (index >= 0) m_testSourceCombo->setCurrentIndex(index);
    } else if (cur == 2) {
        const int index = m_testSourceCombo->findData(1);
        if (index >= 0) m_testSourceCombo->setCurrentIndex(index);
    }
}

void SettingsPanel::setLocalTestPorts(int controlPort, int dataPort)
{
    const QSignalBlocker controlBlocker(m_dummySpiPortSpin);
    const QSignalBlocker dataBlocker(m_dummyDataPortSpin);
    m_dummySpiPortSpin->setValue(controlPort);
    m_dummyDataPortSpin->setValue(dataPort);
    refreshEndpointHint();
}

void SettingsPanel::setUpdateChecking()
{
    if (m_checkUpdateBtn) {
        m_checkUpdateBtn->setEnabled(false);
    }
    if (m_updateStatusLabel) {
        m_updateStatusLabel->setText(QStringLiteral("正在检查更新..."));
        m_updateStatusLabel->setProperty("state", "warn");
        polishWidget(m_updateStatusLabel);
    }
}

void SettingsPanel::setUpdateResult(const UpdateCheckResult &result)
{
    if (m_checkUpdateBtn) {
        m_checkUpdateBtn->setEnabled(true);
    }

    m_updateUrl = result.downloadUrl.isEmpty()
                      ? QString::fromLatin1(kReleaseRepositoryUrl)
                      : result.downloadUrl;

    if (m_openUpdateBtn) {
        m_openUpdateBtn->setEnabled(true);
    }

    if (!m_updateStatusLabel) {
        return;
    }

    if (!result.error.isEmpty()) {
        m_updateStatusLabel->setText(QStringLiteral("更新检查失败：%1").arg(result.error));
        m_updateStatusLabel->setProperty("state", "warn");
    } else if (result.updateAvailable) {
        const QString suffix = result.message.isEmpty() ? QString() : QStringLiteral(" · %1").arg(result.message);
        m_updateStatusLabel->setText(QStringLiteral("发现新版本 %1，可前往下载%2")
                                         .arg(result.latestVersion, suffix));
        m_updateStatusLabel->setProperty("state", "ok");
    } else if (!result.message.isEmpty()) {
        m_updateStatusLabel->setText(result.message);
        m_updateStatusLabel->setProperty("state", "info");
    } else {
        m_updateStatusLabel->setText(QStringLiteral("当前已是最新版本 %1").arg(result.currentVersion));
        m_updateStatusLabel->setProperty("state", "info");
    }
    polishWidget(m_updateStatusLabel);
}

QString SettingsPanel::saveDir() const {
    return m_saveDirEdit ? m_saveDirEdit->text().trimmed() : QString();
}

QString SettingsPanel::sessionName() const {
    const QString s = m_sessionNameEdit ? m_sessionNameEdit->text().trimmed() : QString();
    if (!s.isEmpty()) return s;
    return QStringLiteral("v6_%1").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
}

qint64 SettingsPanel::splitBytes() const {
    return m_splitCombo ? m_splitCombo->currentData().toLongLong() : 0;
}

void SettingsPanel::setSaveDir(const QString &dir) {
    if (m_saveDirEdit && !dir.isEmpty()) {
        m_saveDirEdit->setText(dir);
        refreshFreeSpace();
    }
}

void SettingsPanel::setRecordingStatus(bool recording, const QString &text) {
    m_recording = recording;
    if (m_recordBtn) {
        m_recordBtn->setText(recording ? QStringLiteral("■ 停止录制") : QStringLiteral("● 开始录制"));
        m_recordBtn->setProperty("variant", recording ? "danger" : "primary");
        polishWidget(m_recordBtn);
    }
    if (m_recordStatusLabel) {
        m_recordStatusLabel->setText(text.isEmpty() ? (recording ? QStringLiteral("录制中") : QStringLiteral("未录制")) : text);
    }
    if (m_saveDirEdit) m_saveDirEdit->setEnabled(!recording);
    if (m_browseDirBtn) m_browseDirBtn->setEnabled(!recording);
    if (m_sessionNameEdit) m_sessionNameEdit->setEnabled(!recording);
    if (m_splitCombo) m_splitCombo->setEnabled(!recording);
}

void SettingsPanel::refreshFreeSpace() {
    if (!m_freeSpaceLabel || !m_saveDirEdit) return;
    QStorageInfo info(m_saveDirEdit->text());
    if (!info.isValid() || !info.isReady()) {
        info = QStorageInfo(QDir(m_saveDirEdit->text()).absolutePath());
    }
    if (info.isValid() && info.isReady()) {
        const double gb = info.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
        m_freeSpaceLabel->setText(QStringLiteral("剩余空间: %1 GB").arg(gb, 0, 'f', 1));
    } else {
        m_freeSpaceLabel->setText(QStringLiteral("剩余空间: --"));
    }
}

} // namespace ccv2
