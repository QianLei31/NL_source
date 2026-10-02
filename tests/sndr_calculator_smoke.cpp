#include "signal/sndr_calculator.h"

#include <QVector>

#include <algorithm>
#include <cmath>
#include <iostream>

int main() {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int n = 1024;
    constexpr double fs = 20000.0;
    constexpr int toneBin = 51;
    const double finExpected = static_cast<double>(toneBin) * fs / static_cast<double>(n);

    QVector<double> data;
    data.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double phase = 2.0 * kPi * static_cast<double>(toneBin) * static_cast<double>(i) / static_cast<double>(n);
        data.push_back(std::sin(phase));
    }

    const ccv2::SndrResult r = ccv2::calSndr(data, fs, fs / 2.0, QStringLiteral("hann"));
    if (!r.ok) {
        std::cerr << "calSndr failed: " << r.error.toStdString() << std::endl;
        return 1;
    }

    if (r.fftData.size() != n / 2 || r.fftFreq.size() != n / 2) {
        std::cerr << "Unexpected FFT size" << std::endl;
        return 2;
    }

    if (!std::isfinite(r.sndrDb) || !std::isfinite(r.enob) || !std::isfinite(r.fin)) {
        std::cerr << "Result contains non-finite values" << std::endl;
        return 3;
    }

    const double resolution = fs / static_cast<double>(n);
    if (std::abs(r.fin - finExpected) > resolution * 0.25) {
        std::cerr << "Detected fin mismatch: " << r.fin << " vs " << finExpected << std::endl;
        return 4;
    }

    int maxBin = 5;
    for (int i = maxBin + 1; i < r.fftData.size(); ++i) {
        if (r.fftData[i] > r.fftData[maxBin]) {
            maxBin = i;
        }
    }
    if (maxBin != toneBin) {
        std::cerr << "FFT peak bin mismatch: " << maxBin << " vs " << toneBin << std::endl;
        return 5;
    }
    if (std::abs(r.fftFreq[toneBin] - finExpected) > resolution * 0.25) {
        std::cerr << "FFT frequency axis mismatch: " << r.fftFreq[toneBin] << " vs " << finExpected << std::endl;
        return 6;
    }
    if (r.irn < 0.1) {
        std::cerr << "Reference-style IRN should include in-band tone power: " << r.irn << std::endl;
        return 7;
    }
    const double expectedIrnPowerDb = 10.0 * std::log10(std::max(r.irn * r.irn, 1e-30)) + 3.0;
    if (std::abs(r.irnPowerDb - expectedIrnPowerDb) > 1e-9) {
        std::cerr << "IRN power dB mismatch: " << r.irnPowerDb << " vs " << expectedIrnPowerDb << std::endl;
        return 7;
    }

    ccv2::FftAnalysisConfig invalidCfg;
    invalidCfg.window = QStringLiteral("hann");
    invalidCfg.dcBins = 8;
    invalidCfg.signalBins = 8;
    const ccv2::SndrResult tooNarrow =
        ccv2::calSndr(data, fs, fs / static_cast<double>(n) * 8.0, invalidCfg);
    if (tooNarrow.ok || tooNarrow.error.isEmpty()) {
        std::cerr << "Invalid overlapping FFT bands were accepted" << std::endl;
        return 8;
    }

    const ccv2::SndrResult beyondNyquist =
        ccv2::calSndr(data, fs, fs, invalidCfg);
    if (beyondNyquist.ok) {
        std::cerr << "Bandwidth beyond Nyquist was accepted" << std::endl;
        return 9;
    }

    return 0;
}
