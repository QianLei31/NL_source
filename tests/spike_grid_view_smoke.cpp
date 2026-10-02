#include "signal/spike_snippet_store.h"
#include "ui/widgets/spike_grid_view.h"

#include <QApplication>
#include <QMouseEvent>

#include <cmath>
#include <iostream>

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    ccv2::SpikeSnippetStore store;
    store.configure(1, 33, 20);

    ccv2::SpikeGridView view;
    view.resize(800, 500);
    view.setStore(&store);
    view.setFocusLane(0);
    view.setYFullScaleMicrovolts(200.0);
    view.setThresholds(QVector<double>{-50e-6});
    view.setThresholdEditingEnabled(true);
    view.show();
    app.processEvents();

    int dragSignals = 0;
    double lastThreshold = 0.0;
    QObject::connect(&view, &ccv2::SpikeGridView::thresholdDragged,
                     [&](double threshold) {
        ++dragSignals;
        lastThreshold = threshold;
    });

    const QPointF start(400.0, 180.0);
    const QPointF end(400.0, 360.0);
    QMouseEvent press(QEvent::MouseButtonPress, start, start,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    QMouseEvent move(QEvent::MouseMove, end, end,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &release);

    if (dragSignals < 2 || !std::isfinite(lastThreshold) ||
        lastThreshold >= 0.0) {
        std::cerr << "focused threshold drag was not delivered" << std::endl;
        return 1;
    }
    return 0;
}
