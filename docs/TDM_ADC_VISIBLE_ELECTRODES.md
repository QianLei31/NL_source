# TDM ADC Visible Electrode Map

The active V6.0.4/V7-preview implementation uses a **global** phase-pair selection.
The older block-dependent `block % 8` table in this document was historical and
is not the active routing rule. See `src/core/channel_routing.h`.

- Each of the 256 ADC channels carries four physical electrode phases
- Physical electrode ID = `4 * adc_channel + phase`, with zero-based IDs
- Phase = the corrected, unwrapped source-frame index modulo four
- Global pair **0/2** displays/analyzes 512 electrodes, phases 0 and 2 for every ADC
- Global pair **1/3** displays/analyzes 512 electrodes, phases 1 and 3 for every ADC
- One electrode's sample rate is the source-frame rate divided by four
- Raw recordings retain all four phases; the current neural detector analyzes only
  the selected pair. A 512-cell view does not establish coverage of all 1024 sites

Examples:

| ADC | Pair 0/2 | Pair 1/3 |
|---:|---|---|
| 0 | ELE0, ELE2 | ELE1, ELE3 |
| 1 | ELE4, ELE6 | ELE5, ELE7 |
| 16 | ELE64, ELE66 | ELE65, ELE67 |
| 255 | ELE1020, ELE1022 | ELE1021, ELE1023 |

Event exports include ADC channel, physical electrode, TDM phase, exact source
frame and waveform sample stride. Missing frames must preserve source phase;
invalid recorded padding must never be treated as an electrode measurement.
