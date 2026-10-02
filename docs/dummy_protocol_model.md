# Built-in Dummy protocol model (SIMULATED)

The built-in loopback Dummy now stores the commands emitted by the real GUI,
so control validation can test the simulator's resulting state as well as the
outgoing bytes. It remains a software test source, not an analog board model.
The default sine/spike generator, 256 channels, 20 kHz frame rate, 12-bit ADC,
20-bit frame timestamp, seed, and single-/dual-port connection modes are retained.

## Source of the implemented contract

- `src/core/channel_config_model.h/.cpp`: `NL_SPI_REG v2chip` field layout,
  defaults and actual register command builders
- `src/ui/spi_control_panel.cpp`: `spi` plus eight hex digits, exact global
  commands, gain sequences and stimulation command packer
- `src/core/spi_protocol.h`: twelve-byte reply *layout only*
- `src/core/stim_protocol.h`: UI stimulation/safety parameters, not a board
  readback or pulse-generation specification
- `src/core/channel_routing.h` and `docs/TDM_ADC_VISIBLE_ELECTRODES.md`: four
  physical phases and electrode mapping

No separate board response-code, register-read opcode, reset side-effect,
filter transfer-function or stimulation timing specification is present in the
checked source. Those exact omissions bound the model below.

## Commands and state

A SPI request is eleven ASCII bytes: `spi` and eight hexadecimal digits for a
32-bit word. Case-insensitive transport names and hex, command fragmentation,
coalesced commands and whitespace separators are supported. Requests decode as
opcode[31:26], block[25:18], local channel[17:16], data[15:0]. The model accepts
only blocks 0–63 for channel writes; it never aliases higher blocks into the
256-channel array.

| Opcode | Simulated effect | Initial state |
|---|---|---|
| 0x04 | REC at `4*block+channel`, data masked to 12 bits | 0x060 |
| 0x06 | DAC at the addressed channel, all 16 bits | 0x8000 |
| 0x1e | DAC_CT at the addressed channel, all 16 bits | 0 |
| 0x00 | Exact all-zero no-op | No change |
| 0x07 | Exact global word: analog reset requested/asserted | Unknown |
| 0x08 | Exact global word: analog reset requested/released | Unknown |
| 0x09 | Exact global word: global DAC requested/on | Unknown |
| 0x0d | Exact global word: global DAC requested/off | Unknown |
| 0x12 | Exact global word: CBOK requested/low | Unknown |

Global commands require all lower 26 bits to be zero, matching the shipped GUI.
Unknown commands, undocumented nonzero global payloads, and invalid addresses
are counted as unsupported and cannot change register/global state. Malformed
transport requests are counted and disconnected without a fabricated error
frame; incomplete commands are counted once at disconnect. Buffers are bounded.

REC fields (reference, low-pass, high-power, gain, high-pass, trim, OFF, RST_N),
DAC fields (amplitude, polarity, compensation, step, electrode, OFF_STIM), and
all DAC_CT fields are retained independently per channel. DAC electrode bits
are independent of the outer channel address. No pulse waveform, current,
charge, neural response, filter response, or reset-induced register erasure is
inferred from these fields.

The GUI high/low gain sequences write 0x05e/0x07e to all 256 REC registers,
with an all-zero no-op after each write. Both are ordinary channel writes in
the simulator, not a special shortcut.

## Signal effects that are explicitly simulated

REC bit 5 selects 60x when set and 180x when clear. The existing generator is
calibrated at 60x. Its AC component is multiplied by `selected_gain / 60`
before integer quantization and clamping to [0,4095]. The baseline is unchanged.
The spike generator's 80–420 microvolt synthetic input spikes and 12 microvolt
input-noise standard deviation therefore retain their original 60x conversion
and scale consistently at 180x. Noise and spikes use the same gain.

As an explicit test convention, REC OFF bit 10 suppresses the synthetic AC
component, including noise, to the channel's existing baseline. This constant
baseline is not a prediction of an unpowered physical ADC. The generator keeps
advancing phases/RNG for disabled channels, so other channels are unaffected.
Other REC fields and global controls have stored state only. In particular,
RST_N is not used to mute the default stream: the shipped REC default has that
bit clear and no hardware reset/settling behavior is specified.

`ctre` restarts generator phase, timestamp and RNG under the existing lifecycle
rules; it does not erase programmed registers. `stop` preserves register state.
In dual-port mode it disarms data generation; in single-port mode it closes the
requesting stream connection. Use a separate TCP connection for SPI while a
single-port data stream is active, as the real GUI does, so control replies
cannot corrupt binary acquisition frames.

## Optional four-phase routing fixture

Default generation is unchanged. `--tdm`, or the in-process
`setTdmSimulation(true)`, explicitly enables a synthetic TDM test profile. It
uses the documented route `electrode = 4 * adc_channel + (source_frame % 4)`.
The four phases multiply the synthetic AC amplitude by 1, 1.25, 1.5 and 1.75.
This chosen stimulus makes phase routing testable; it does not pretend to be
four independent neurons or a measured analog response. REC gain/OFF is per ADC
group and applies to all four phases. All four raw phases remain present;
selection of visible pair 0/2 or 1/3 is a downstream application operation.

No TDM-enable SPI opcode is invented. The app's `TdmContext` changes software
interpretation; it is not evidence of a hardware register write. CLI startup
prints `model=SIMULATED`, its TDM fixture setting, and the reply placeholder
status so recordings/tests can label their source honestly.

## Replies, inspection and assertions

The previous Dummy returned twelve zero bytes for SPI. That response remains
for GUI transport compatibility after a complete valid hexadecimal SPI word,
including unsupported opcodes. **It is a legacy placeholder, not register
readback, not an echo, and not proof that a board accepted a command.** The
source documents the byte layout but not the board's command/response mapping;
implementing a meaningful response requires that missing specification.

Tests must inspect `DummyStreamServer::protocolState()` on its QObject thread
for simulator assertions. It returns a copy with REC/DAC/DAC_CT arrays,
optional global values, processed/supported/unsupported/malformed counters,
last SPI word, last supported status and a diagnostic. `channelGain`,
`channelSignalEnabled`, `tdmSimulation` and `simulatedElectrode` expose the
explicit signal/routing conventions. `protocolCommandProcessed(word,supported)`
allows event-driven observation. This is an in-process test API, not a new
network protocol or a production hardware readback API.

## Verification boundary

`dummy_protocol_model_smoke` exercises actual loopback TCP with every split of
a request, coalescing, malformed/unsupported inputs, both 512-command global
gain sequences, state isolation, all known global controls, 60x/180x signal and
noise scaling, ADC clipping, REC OFF, state persistence, phase-coded TDM routing
and dual-port stop/restart. The GUI-to-Dummy end-to-end test verifies actual
packers against this state, separately from TCP failure fixtures.

These are repeatable SIMULATED protocol/control/data-path checks. To extend
hardware-specific response behavior, provide the board read/write response
mapping (including header meaning and pipeline/dummy latency), reset/power-on
rules, TDM enable command, and analog/stimulation transfer/timing specifications.
Actual analog gain, filters, DAC current/charge and interlocks still require
instrumented physical-hardware acceptance testing; placeholder replies cannot
establish them.
