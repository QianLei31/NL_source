#include "core/channel_config_model.h"

#include <iostream>

namespace {

QString compact(QString bits)
{
    bits.remove(QLatin1Char('_'));
    bits.remove(QLatin1Char(' '));
    return bits;
}

}  // namespace

int main()
{
    using Model = ccv2::ChannelConfigModel;

    quint16 dac = 0;
    dac = Model::withDacField(dac, Model::DacOffStim, 0);
    dac = Model::withDacField(dac, Model::DacElectrode, 1);
    dac = Model::withDacField(dac, Model::DacStep, 1);
    dac = Model::withDacField(dac, Model::DacCompensate, 0);
    dac = Model::withDacField(dac, Model::DacPolarity, 1);
    dac = Model::withDacField(dac, Model::DacAmplitude, 511);

    // Exact command taken from spi_mode.py Stim_ELE11:
    // 000110_00110000_01_0_01_1_0_01_111111111
    const QString expected = compact(
        QStringLiteral("000110_00110000_01_0_01_1_0_01_111111111"));
    const QString actual = Model::buildDacCommand(0x30, 1, dac);
    if (actual != expected) {
        std::cerr << "STIM/DAC protocol mismatch\nexpected="
                  << expected.toStdString() << "\nactual=" << actual.toStdString()
                  << std::endl;
        return 1;
    }

    // The outer frame channel must never overwrite DAC_DATA[14:13].
    for (int outerChannel = 0; outerChannel < 4; ++outerChannel) {
        const QString command = Model::buildDacCommand(7, outerChannel, dac);
        const int encodedOuter = command.mid(14, 2).toInt(nullptr, 2);
        const int encodedElectrode = command.mid(17, 2).toInt(nullptr, 2);
        if (encodedOuter != outerChannel || encodedElectrode != 1) {
            std::cerr << "outer channel contaminated DAC electrode bits" << std::endl;
            return 2;
        }
    }

    Model shadow;
    shadow.setDac(0, dac);
    if (shadow.dacField(0, Model::DacElectrode) != 1 ||
        shadow.dacField(0, Model::DacAmplitude) != 511) {
        std::cerr << "DAC shadow field readback failed" << std::endl;
        return 3;
    }

    std::cout << "channel_config_model_smoke ok" << std::endl;
    return 0;
}
