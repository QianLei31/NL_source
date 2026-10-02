#include "hardware_control_page.h"
#include "src/ui/widgets/console_widget.h"
#include "src/model/console_log_model.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>

namespace ccv2 {

HardwareControlPage::HardwareControlPage(ConsoleLogModel *logModel, QWidget *parent)
    : PageBase(parent)
{
    setTitle(QStringLiteral("Hardware Control"));
    setSubtitle(QStringLiteral("Connection · Configuration · Global Commands · Quick Commands"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(12);

    // Header
    auto *titleLabel = new QLabel(QStringLiteral("HARDWARE CONTROL"), this);
    titleLabel->setObjectName(QStringLiteral("title"));
    layout->addWidget(titleLabel);

    // Body: Controls + Console
    auto *body = new QHBoxLayout;
    body->setSpacing(12);

    // Left: Control buttons
    auto *controlCard = new QGroupBox(QStringLiteral("COMMANDS"), this);
    auto *grid = new QGridLayout(controlCard);
    grid->setSpacing(8);

    auto makeBtn = [this](const QString &text, const QString &objName) {
        auto *btn = new QPushButton(text, this);
        btn->setObjectName(objName);
        btn->setMinimumHeight(36);
        return btn;
    };

    grid->addWidget(makeBtn(QStringLiteral("Load Config"), QStringLiteral("")), 0, 0);
    grid->addWidget(makeBtn(QStringLiteral("Save Config"), QStringLiteral("")), 0, 1);
    grid->addWidget(makeBtn(QStringLiteral("Apply Config"), QStringLiteral("primary")), 1, 0);
    grid->addWidget(makeBtn(QStringLiteral("Verify Link"), QStringLiteral("primary")), 1, 1);
    grid->addWidget(makeBtn(QStringLiteral("Analog Reset"), QStringLiteral("danger")), 2, 0);
    grid->addWidget(makeBtn(QStringLiteral("Remove Reset"), QStringLiteral("")), 2, 1);
    grid->addWidget(makeBtn(QStringLiteral("DAC On"), QStringLiteral("primary")), 3, 0);
    grid->addWidget(makeBtn(QStringLiteral("DAC Off"), QStringLiteral("danger")), 3, 1);

    auto *seqGroup = new QGroupBox(QStringLiteral("QUICK COMMANDS"), this);
    auto *seqLayout = new QVBoxLayout(seqGroup);
    seqLayout->addWidget(makeBtn(QStringLiteral("Gain All High"), QStringLiteral("")));
    seqLayout->addWidget(makeBtn(QStringLiteral("Gain All Low"), QStringLiteral("")));

    auto *leftPanel = new QVBoxLayout;
    leftPanel->addWidget(controlCard);
    leftPanel->addWidget(seqGroup);
    leftPanel->addStretch();
    body->addLayout(leftPanel, 0);

    // Right: Console
    m_console = new ConsoleWidget(logModel, this);
    body->addWidget(m_console, 1);

    layout->addLayout(body, 1);
}

} // namespace ccv2
