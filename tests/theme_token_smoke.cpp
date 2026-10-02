#include "theme/theme_manager.h"
#include <QApplication>
#include <QRegularExpression>
#include <iostream>
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    ccv2::ThemeManager theme;
    for(const auto &name: ccv2::ThemeManager::themeNames()) {
        theme.apply(name);
        const QString css=app.styleSheet();
        if(css.isEmpty() || css.contains(QRegularExpression("#[0-9A-Fa-f]{6}-[a-z]")) || css.contains("@primary")) {
            std::cerr<<"unexpanded color token in "<<name.toStdString()<<'\n';return 1;
        }
    }
    std::cout<<"theme_token_smoke OK\n";return 0;
}
