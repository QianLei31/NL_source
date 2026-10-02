#pragma once

#include <QString>
#include <QVector>

namespace ccv2 {

struct FftAnalysisConfig {
    QString window{QStringLiteral("hann")};
    int dcBins{5};
    int signalBins{5};
};

struct SndrResult {
    double sndrDb{0.0};
    double enob{0.0};
    double irn{0.0};
    double fin{0.0};
    QVector<double> fftData;
    QVector<double> fftFreq;
    double irnPowerDb{0.0};
    double thdDb{0.0};
    double thdOddDb{0.0};
    double thdEvenDb{0.0};
    double snrDb{0.0};
    double sfdrDb{0.0};
    double fomDb{0.0};
    bool ok{false};
    QString error;
};

SndrResult calSndr(const QVector<double> &data, double fs, double fb, const FftAnalysisConfig &cfg);
SndrResult calSndr(const QVector<double> &data, double fs, double fb, const QString &winType);

}  // namespace ccv2
