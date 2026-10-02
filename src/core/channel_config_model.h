#pragma once

#include <QString>
#include <QVector>

namespace ccv2 {

// Shadow-register model of the chip's per-channel configuration. The map page
// edits fields on a selected set of channels; each channel keeps its full REC /
// DAC / DAC_CT register value so a single-field change (e.g. "gain high") is a
// read-modify-write on the shadow copy that preserves the other bits. Command
// builders emit the exact 32-char binary SPI strings the left SPI panel sends.
//
// Bit layouts (from NL_SPI_REG v2chip):
//   REC[12]  : [0] ref(0=VSS/1=VREF) [1] EN_LOWLP(1=1kHz low) [4] high-power
//              [5] gain(1=60x/0=180x) [6] highpass(1=1Hz/0=200Hz)
//              [9:7] Trim_Imp [10] OFF [11] RST_N        (default 0x060)
//   DAC[16]  : [8:0] amplitude [10:9] polarity(00=0,01=neg,10=pos,11=none)
//              [11] compensate [12] step(1=200nA/0=4nA)
//              [14:13] DAC electrode select (00/01/10/11)
//              [15] OFF_STIM(1=disable)                  (default 0x8000)
//   DAC_CT16 : [3:0] time-neg [7:4] time-pos [10:8] global-freq
//              [12:11] local-freq [13] AMP_x20 [14] pol-first [15] CT_ON
class ChannelConfigModel {
public:
    static constexpr int kBlocks = 64;
    static constexpr int kChPerBlock = 4;
    static constexpr int kChannels = kBlocks * kChPerBlock;  // 256

    enum RecField { RecReference, RecLowLP, RecHighPower, RecGain, RecHighpass, RecTrimImp, RecOff, RecRstN };
    enum DacField { DacAmplitude, DacPolarity, DacCompensate, DacStep, DacElectrode, DacOffStim };
    enum CtField  { CtTimeNeg, CtTimePos, CtGlobalFreq, CtLocalFreq, CtAmpX20, CtPolFirst, CtOnOff };

    ChannelConfigModel() { reset(); }
    void reset();

    static int channelIndex(int block, int ch) { return block * kChPerBlock + ch; }

    quint16 rec(int idx) const { return valueAt(m_rec, idx); }
    quint16 dac(int idx) const { return valueAt(m_dac, idx); }
    quint16 dacCt(int idx) const { return valueAt(m_dacCt, idx); }

    void setRec(int idx, quint16 value);
    void setDac(int idx, quint16 value);
    void setDacCt(int idx, quint16 value);

    void setRecField(int idx, RecField f, int value);
    void setDacField(int idx, DacField f, int value);
    void setCtField(int idx, CtField f, int value);

    // Pure helpers: return reg with one field replaced (UI assembles a full
    // register from its controls without touching the shadow copies).
    static quint16 withRecField(quint16 reg, RecField f, int value);
    static quint16 withDacField(quint16 reg, DacField f, int value);
    static quint16 withCtField(quint16 reg, CtField f, int value);

    int recField(int idx, RecField f) const;
    int dacField(int idx, DacField f) const;
    int ctField(int idx, CtField f) const;

    // Static builders for arbitrary register values (command previews).
    // The frame channel and DAC electrode are independent: buildDacCommand
    // writes `ch` only into the outer 2-bit address and preserves data[14:13].
    static QString buildRecCommand(int block, int ch, quint16 rec);
    static QString buildDacCommand(int block, int ch, quint16 dac);
    static QString buildCtCommand(int block, int ch, quint16 dacCt);

    // Config persistence: hex string of kChannels 4-digit register values.
    // restore() is all-or-nothing so a corrupt entry can't mix restored and
    // default registers.
    QString serializeRec() const { return serialize(m_rec); }
    QString serializeDac() const { return serialize(m_dac); }
    QString serializeDacCt() const { return serialize(m_dacCt); }
    bool restore(const QString &rec, const QString &dac, const QString &dacCt);

private:
    static quint16 valueAt(const QVector<quint16> &v, int idx) {
        return (idx >= 0 && idx < v.size()) ? v[idx] : 0;
    }
    static QString serialize(const QVector<quint16> &v);
    static bool deserialize(const QString &hex, QVector<quint16> *out);
    QVector<quint16> m_rec, m_dac, m_dacCt;
};

}  // namespace ccv2
