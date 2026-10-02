#pragma once
#include <QWidget>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include "src/model/console_log_model.h"

namespace ccv2 {

class ConsoleWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConsoleWidget(ConsoleLogModel *model, QWidget *parent = nullptr);

private slots:
    void onEntryAppended(const ConsoleEntry &entry);
    void onClear();
    void onExport();
    void onSearchChanged(const QString &text);

private:
    QString formatEntry(const ConsoleEntry &entry) const;

    ConsoleLogModel *m_model = nullptr;
    QPlainTextEdit *m_textEdit = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QPushButton *m_exportBtn = nullptr;
    QString m_searchFilter;
};

} // namespace ccv2
