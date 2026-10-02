#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <iostream>

#include "signal/spike_snippet_store.h"
#include "ui/widgets/spike_grid_view.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    ccv2::SpikeSnippetStore store;
    store.configure(1, 7, 10);
    const float samples[] = {0, 10e-6f, -70e-6f, -35e-6f, 40e-6f, 15e-6f, 0};
    store.addSnippet(0, samples);
    ccv2::SpikeGridView view;
    view.setStore(&store);
    view.setFocusLane(0);
    view.setFadeEnabled(false);
    view.resize(640, 420);
    view.show();
    app.processEvents();
    view.pullNewSnippets();

    // No new input arrives: changing theme while paused must reproduce the
    // same retained waveform layer as an explicit full rebuild.
    const QMap<QString, QString> palettes[] = {
        {{"plotBg", "#f9fbfd"}, {"grid", "#d5dce5"}, {"axis", "#32405b"},
         {"wave", "#176b86"}, {"border", "#a8b7c9"}},
        {{"plotBg", "#12171e"}, {"grid", "#323c48"}, {"axis", "#bac7d6"},
         {"wave", "#60d7c0"}, {"border", "#526073"}}
    };
    for (const auto &palette : palettes) {
        view.setThemePalette(palette);
        app.processEvents();
        const QImage actual = view.grab().toImage();
        view.rebuildAll();
        app.processEvents();
        const QImage rebuilt = view.grab().toImage();
        if (actual != rebuilt) {
            std::cerr << "FAIL: theme switch erased retained spike traces\n";
            return 1;
        }
        view.clearTraces();
        app.processEvents();
        if (view.grab().toImage() == rebuilt) {
            std::cerr << "FAIL: fixture has no visible waveform pixels\n";
            return 2;
        }
        view.rebuildAll();
    }
    std::cout << "spike_grid_theme_smoke OK\n";
    return 0;
}
