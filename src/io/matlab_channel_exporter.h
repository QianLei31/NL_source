#pragma once

#include <QString>

#include <atomic>

namespace ccv2 {

enum class MatlabExportMode {
    RawAdc,    // low 12-bit ADC value in int32 little-endian files
    RawWords,  // original 32-bit word, including timestamp bits
    TdmDemux,
};

struct MatlabChannelExportOptions {
    MatlabExportMode mode{MatlabExportMode::RawAdc};
    // Legacy option retained for source compatibility. Four-phase TDM export
    // always writes all local electrodes 0..3, so the display pair is irrelevant.
    bool evenSampleIsFirstLocalEle{true};
    const std::atomic_bool *cancelFlag{nullptr};
};

struct MatlabChannelExportResult {
    bool ok{false};
    QString error;
    QString warning;
    QString outputDir;
    qint64 frameCount{0};
    qint64 leftoverBytes{0};
    int outputCount{0};
};

class MatlabChannelExporter {
public:
    static MatlabChannelExportResult exportAdcBin(const QString &adcFile,
                                                  const QString &outputDir,
                                                  double samplingRateHz);
    static MatlabChannelExportResult exportAdcBin(const QString &adcFile,
                                                  const QString &outputDir,
                                                  double samplingRateHz,
                                                  const MatlabChannelExportOptions &options);
};

}  // namespace ccv2
