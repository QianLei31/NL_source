#include "core/channel_config_model.h"

namespace ccv2 {

namespace {
constexpr quint16 kRecDefault = 0x060;    // gain=60x, highpass=1Hz
constexpr quint16 kDacDefault = 0x8000;   // OFF_STIM disabled
constexpr quint16 kDacCtDefault = 0x0000;

QString toBin(unsigned value, int width) {
    QString s(width, QLatin1Char('0'));
    for (int i = 0; i < width; ++i) {
        if (value & (1u << i)) s[width - 1 - i] = QLatin1Char('1');
    }
    return s;
}

// Replace a bit field [pos, pos+bits) inside value with fieldValue.
void setField(quint16 &value, int pos, int bits, int fieldValue) {
    const quint16 mask = static_cast<quint16>(((1u << bits) - 1u) << pos);
    value = static_cast<quint16>((value & ~mask) |
                                 ((static_cast<unsigned>(fieldValue) << pos) & mask));
}

int getField(quint16 value, int pos, int bits) {
    return (value >> pos) & ((1u << bits) - 1u);
}

// Field -> {pos, bits}
struct FieldSpec { int pos; int bits; };

FieldSpec recSpec(ChannelConfigModel::RecField f) {
    switch (f) {
    case ChannelConfigModel::RecReference: return {0, 1};
    case ChannelConfigModel::RecLowLP:     return {1, 1};
    case ChannelConfigModel::RecHighPower: return {4, 1};
    case ChannelConfigModel::RecGain:      return {5, 1};
    case ChannelConfigModel::RecHighpass:  return {6, 1};
    case ChannelConfigModel::RecTrimImp:   return {7, 3};
    case ChannelConfigModel::RecOff:       return {10, 1};
    case ChannelConfigModel::RecRstN:      return {11, 1};
    }
    return {0, 0};
}

FieldSpec dacSpec(ChannelConfigModel::DacField f) {
    switch (f) {
    case ChannelConfigModel::DacAmplitude:  return {0, 9};
    case ChannelConfigModel::DacPolarity:   return {9, 2};
    case ChannelConfigModel::DacCompensate: return {11, 1};
    case ChannelConfigModel::DacStep:       return {12, 1};
    case ChannelConfigModel::DacElectrode:  return {13, 2};
    case ChannelConfigModel::DacOffStim:    return {15, 1};
    }
    return {0, 0};
}

FieldSpec ctSpec(ChannelConfigModel::CtField f) {
    switch (f) {
    case ChannelConfigModel::CtTimeNeg:    return {0, 4};
    case ChannelConfigModel::CtTimePos:    return {4, 4};
    case ChannelConfigModel::CtGlobalFreq: return {8, 3};
    case ChannelConfigModel::CtLocalFreq:  return {11, 2};
    case ChannelConfigModel::CtAmpX20:     return {13, 1};
    case ChannelConfigModel::CtPolFirst:   return {14, 1};
    case ChannelConfigModel::CtOnOff:      return {15, 1};
    }
    return {0, 0};
}
}  // namespace

void ChannelConfigModel::reset() {
    m_rec = QVector<quint16>(kChannels, kRecDefault);
    m_dac = QVector<quint16>(kChannels, kDacDefault);
    m_dacCt = QVector<quint16>(kChannels, kDacCtDefault);
}

void ChannelConfigModel::setRec(int idx, quint16 value) {
    if (idx >= 0 && idx < m_rec.size()) m_rec[idx] = value;
}

void ChannelConfigModel::setDac(int idx, quint16 value) {
    if (idx >= 0 && idx < m_dac.size()) m_dac[idx] = value;
}

void ChannelConfigModel::setDacCt(int idx, quint16 value) {
    if (idx >= 0 && idx < m_dacCt.size()) m_dacCt[idx] = value;
}

quint16 ChannelConfigModel::withRecField(quint16 reg, RecField f, int value) {
    const FieldSpec s = recSpec(f);
    setField(reg, s.pos, s.bits, value);
    return reg;
}

quint16 ChannelConfigModel::withDacField(quint16 reg, DacField f, int value) {
    const FieldSpec s = dacSpec(f);
    setField(reg, s.pos, s.bits, value);
    return reg;
}

quint16 ChannelConfigModel::withCtField(quint16 reg, CtField f, int value) {
    const FieldSpec s = ctSpec(f);
    setField(reg, s.pos, s.bits, value);
    return reg;
}

void ChannelConfigModel::setRecField(int idx, RecField f, int value) {
    if (idx < 0 || idx >= m_rec.size()) return;
    const FieldSpec s = recSpec(f);
    setField(m_rec[idx], s.pos, s.bits, value);
}

void ChannelConfigModel::setDacField(int idx, DacField f, int value) {
    if (idx < 0 || idx >= m_dac.size()) return;
    const FieldSpec s = dacSpec(f);
    setField(m_dac[idx], s.pos, s.bits, value);
}

void ChannelConfigModel::setCtField(int idx, CtField f, int value) {
    if (idx < 0 || idx >= m_dacCt.size()) return;
    const FieldSpec s = ctSpec(f);
    setField(m_dacCt[idx], s.pos, s.bits, value);
}

int ChannelConfigModel::recField(int idx, RecField f) const {
    const FieldSpec s = recSpec(f);
    return getField(rec(idx), s.pos, s.bits);
}

int ChannelConfigModel::dacField(int idx, DacField f) const {
    const FieldSpec s = dacSpec(f);
    return getField(dac(idx), s.pos, s.bits);
}

int ChannelConfigModel::ctField(int idx, CtField f) const {
    const FieldSpec s = ctSpec(f);
    return getField(dacCt(idx), s.pos, s.bits);
}

QString ChannelConfigModel::buildRecCommand(int block, int ch, quint16 rec) {
    // Write REC (0x04): opcode(6)+block(8)+ch(2)+data16, REC is 12 bits in [11:0].
    return toBin(0x04, 6) + toBin(block, 8) + toBin(ch, 2) + toBin(rec & 0x0FFF, 16);
}

QString ChannelConfigModel::buildDacCommand(int block, int ch, quint16 dac) {
    // Write STIM/DAC (0x06). The outer frame channel selects one ADC group;
    // DAC data[14:13] independently selects one of its four electrodes.
    return toBin(0x06, 6) + toBin(block, 8) + toBin(ch, 2) + toBin(dac, 16);
}

QString ChannelConfigModel::buildCtCommand(int block, int ch, quint16 dacCt) {
    // Write STIM_CT (0x1E).
    return toBin(0x1E, 6) + toBin(block, 8) + toBin(ch, 2) + toBin(dacCt, 16);
}

QString ChannelConfigModel::serialize(const QVector<quint16> &v) {
    QString s;
    s.reserve(v.size() * 4);
    for (quint16 x : v) {
        s += QStringLiteral("%1").arg(x, 4, 16, QLatin1Char('0'));
    }
    return s;
}

bool ChannelConfigModel::deserialize(const QString &hex, QVector<quint16> *out) {
    if (hex.size() != kChannels * 4 || !out) {
        return false;
    }
    QVector<quint16> v(kChannels);
    for (int i = 0; i < kChannels; ++i) {
        bool ok = false;
        v[i] = hex.mid(i * 4, 4).toUShort(&ok, 16);
        if (!ok) {
            return false;
        }
    }
    *out = v;
    return true;
}

bool ChannelConfigModel::restore(const QString &rec, const QString &dac, const QString &dacCt) {
    QVector<quint16> r, d, c;
    if (!deserialize(rec, &r) || !deserialize(dac, &d) || !deserialize(dacCt, &c)) {
        return false;
    }
    m_rec = r;
    m_dac = d;
    m_dacCt = c;
    return true;
}

}  // namespace ccv2
