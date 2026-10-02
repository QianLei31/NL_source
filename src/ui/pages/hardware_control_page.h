#pragma once
#include "page_base.h"

namespace ccv2 {

class ConsoleWidget;
class ConsoleLogModel;

class HardwareControlPage : public PageBase {
    Q_OBJECT
public:
    explicit HardwareControlPage(ConsoleLogModel *logModel, QWidget *parent = nullptr);

private:
    ConsoleWidget *m_console = nullptr;
};

} // namespace ccv2
