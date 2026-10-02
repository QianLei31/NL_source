#pragma once

#include <QtGlobal>

namespace ccv2 {

// One threshold crossing and its retained, spike-band, input-referred waveform.
// sourceFrame is the unwrapped source-frame coordinate carried by StreamBlockInfo,
// not a GUI counter or a wall-clock timestamp. A TDM lane advances by four source
// frames per waveform sample. Epochs separate seeks/restarts: never compare times
// across epochs without the session's explicit timeline mapping.
struct SpikeEvent {
    quint64 epoch = 0;
    quint64 continuitySegment = 0; // increments at a gap/reset within an epoch
    quint64 sequence = 0;       // per-lane store sequence; assigned atomically
    qint64 sourceFrame = -1;    // -1 for legacy waveform-only producers
    double sourceSampleRate = 0.0; // frames/s, before TDM demultiplexing
    double inputGain = 1.0;
    int lane = -1;
    int adcChannel = -1;
    int electrode = -1;         // ADC index in ADC mode; ADC*4+phase in TDM
    int tdmPhase = -1;          // -1 = ADC mode; otherwise physical phase 0..3
    int sampleStride = 1;      // source frames per waveform sample
    int preSamples = 0;        // crossing index within the stored waveform
    int unitId = -1;           // -1 = unsorted; >=0 manual candidate label, not biological isolation

    bool hasSourceTime() const {
        return sourceFrame >= 0 && sourceSampleRate > 0.0;
    }
    double sourceTimeSeconds() const {
        return hasSourceTime() ? static_cast<double>(sourceFrame) / sourceSampleRate : -1.0;
    }
};

} // namespace ccv2
